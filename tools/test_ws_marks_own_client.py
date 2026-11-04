#!/usr/bin/env python3
"""WindowServer's completion marks, and the tries on the same thread, must not wait behind a dirty mark.

A dirty mark holds its NVRM client's gate while it waits for the accelerator lock, and that lock can be held by something
waiting for a mark. On one client the mark queues behind the gate until the kernel's 100 ms limit. Measured on an RTX 5050 with
a process named WindowServer: 12 rounds of 12 ran out with a commit from the same process, 0 of 6 with the same commits from a
second process, whose gate is another. Here three threads play the same parts with real locks, and the source is checked for
what makes that so: the marks' own client, and an escape that matches the kernel's. It says nothing about the kernel: that is
gatecycle, with this plugin.

A try never waits for the accelerator lock, but it waits for its client's gate. With two command buffers in flight the page-off
waits for the second one's mark, and the completion thread reaches that mark only after the first one's try: that is the same
cycle with one more step. The third thread plays it too."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
vk = (root / 'plugin/nvmtl_vk.c').read_text()
abi = (root / 'kexts/NVRM/nvrm_surfshare_abi.h').read_text()
nvrm = (root / 'kexts/NVRM/NVRM.cpp').read_text()
accel = (root / 'kexts/NVRM/accel/nvrm-accel.cpp').read_text()
patch = (root / 'nvk/nvk-macos.patch').read_text()

def define(text, name):
    return int(re.search(r'#define %s\s+(0x[0-9A-Fa-f]+|\d+)u' % name, text).group(1), 0)

i = vk.index('uint32_t nvmtl_vk_surface_gpu_mark(uint32_t surfaceID, int pending)')
mark = vk[i:vk.index('\n}\n', i)]
assert mark.index('IOConnectCallMethod(g_marks_client') < mark.index('pnvk_surface_dirty(g_dev'), \
    'the marks must try their own client first; NVK\'s escape is the ctl client the dirty marks hold'

i = vk.index('static uint32_t nvmtl_ws_dirty_esc(')
dirty = vk[i:vk.index('\n}\n', i)]
pick = re.search(r'client = \(flags & NVMTL_SS_PLANE_TRYLOCK\) \? (\w+) : (\w+);', dirty)
assert pick and pick.groups() == ('g_marks_client', 'g_wait_client'), \
    'a try goes with the completion marks, on the client that never waits for the accelerator lock; a mark that can wait on its own'
assert dirty.index('pthread_once(&g_wait_client_once') < dirty.index('IOConnectCallMethod(client'), 'the client is open before it is used'
assert 'strcmp(getprogname(), "WindowServer")' in dirty, 'other processes have no marks to release: they keep NVK\'s client'
i = vk.index('static int nvmtl_vk_surface_mark_newest(')
newest = vk[i:vk.index('\n}\n', i)]
assert newest.index('nvmtl_ws_dirty_esc(') < newest.index('pnvk_surface_dirty(g_dev'), 'NVK\'s ctl client only when the own one is not there'
assert int(re.search(r'#define NVMTL_SS_PLANE_TRYLOCK\s+(0x[0-9A-Fa-f]+)u', vk).group(1), 0) == define(abi, 'NVRM_SS_PLANE_TRYLOCK'), 'the try bit'

# the escape must be the kernel's
assert define(abi, 'NVRM_ESC_SURF_DIRTY') == int(re.search(r'NVMTL_ESC_SURF_DIRTY = (0x[0-9A-Fa-f]+)', vk).group(1), 0), 'escape number'
assert define(abi, 'NVRM_SURFSHARE_VERSION') == int(re.search(r'NVMTL_SS_VERSION = (\d+)', vk).group(1)), 'escape version'
assert define(abi, 'NVRM_SS_PLANE_GPU_MARK') == 0x80000000, 'the mark bit'
fields = re.search(r'struct NVRMSurfShareEsc \{(.*?)\};', abi, re.S).group(1)
assert re.findall(r'(\w+);', fields) == ['version', 'surfaceID', 'plane', 'hClient', 'hSrcClient', 'hSrcMemory', 'status', 'pad', 'size'], 'the layout changed'
assert 'version, surfaceID, plane, hClient, hSrcClient, hSrcMemory, status, pad; uint64_t size' in vk, 'the plugin\'s copy of the layout changed'
assert re.search(r'SEL_ESC = 1\b', patch) and re.search(r'NVMTL_SEL_ESC = 1\b', vk), 'the escape selector'
assert re.search(r'\(type & 0xFFFF0000u\) != 0x4E560000u', accel) and 'NVMTL_RM_CLIENT_CTL = 0x4E560000' in vk, 'the client type NVAccel hands to NVRM'
assert 'if (type > 1) return false;' in nvrm and 'fIsCtl = (type == 0);' in nvrm, 'type 0 is the ctl client'
vram = accel[accel.index('static IOReturn nvAccelSurfVram('):]
early = vram[:vram.index('nvFamLock(a, tryOnly)')]
assert 'nvSurfGpuEscape(p)' in early, 'the kernel must answer a mark before it takes the accelerator lock'

fixture = r'''
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
using ms = std::chrono::milliseconds;
static std::mutex accelerator, dirty_gate, own_gate, marks;
static std::condition_variable marked;
static bool pending;
static int timeouts;
static const auto limit = ms(100);
static void wait_ready() {
    std::unique_lock<std::mutex> l(marks);
    if (!marked.wait_for(l, limit, [] { return !pending; })) { pending = false; timeouts++; }
}
static int round(std::mutex &mark_gate) {
    timeouts = 0; pending = true;
    std::thread pageoff([] { std::lock_guard<std::mutex> family(accelerator); wait_ready(); });
    std::thread commit([] {   // a dirty mark on another surface: its client's gate, then the accelerator lock
        std::this_thread::sleep_for(ms(5));
        std::lock_guard<std::mutex> g(dirty_gate);
        std::lock_guard<std::mutex> family(accelerator);
    });
    std::thread completion([&mark_gate] {   // the GPU has finished
        std::this_thread::sleep_for(ms(20));
        std::lock_guard<std::mutex> g(mark_gate);
        { std::lock_guard<std::mutex> l(marks); pending = false; }
        marked.notify_all();
    });
    pageoff.join(); commit.join(); completion.join();
    return timeouts;
}
// two command buffers in flight: the page-off waits for the second one's mark, and the completion thread reaches it after the
// first one's try, which only looks at the accelerator lock but goes through its client's gate
static int round_try(std::mutex &try_gate) {
    timeouts = 0; pending = true;
    std::thread pageoff([] { std::lock_guard<std::mutex> family(accelerator); wait_ready(); });
    std::thread commit([] {
        std::this_thread::sleep_for(ms(5));
        std::lock_guard<std::mutex> g(dirty_gate);
        std::lock_guard<std::mutex> family(accelerator);
    });
    std::thread completion([&try_gate] {
        std::this_thread::sleep_for(ms(20));
        { std::lock_guard<std::mutex> g(try_gate); if (accelerator.try_lock()) accelerator.unlock(); }
        std::lock_guard<std::mutex> g(own_gate);
        { std::lock_guard<std::mutex> l(marks); pending = false; }
        marked.notify_all();
    });
    pageoff.join(); commit.join(); completion.join();
    return timeouts;
}
int main() {
    int own = 0, shared = 0, tryOwn = 0, tryShared = 0;
    for (int i = 0; i < 5; i++) {
        own += round(own_gate); shared += round(dirty_gate);
        tryOwn += round_try(own_gate); tryShared += round_try(dirty_gate);
    }
    printf("mark on its own client: %d of 5 ran out; on the dirty marks': %d of 5\n", own, shared);
    printf("try on the marks' client: %d of 5 ran out; on the dirty marks': %d of 5\n", tryOwn, tryShared);
    return own || tryOwn ? 1 : shared == 5 && tryShared == 5 ? 0 : 2;
}
'''

with tempfile.TemporaryDirectory() as d:
    src = pathlib.Path(d) / 'marks.cpp'
    src.write_text(fixture)
    exe = pathlib.Path(d) / 'marks'
    subprocess.run(['clang++', '-std=c++17', '-pthread', '-o', str(exe), str(src)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    print(r.stdout.strip())
    assert r.returncode == 0, 'a mark or a try on the client that never waits must get through, and on the dirty marks\' must not: the model cannot see the difference otherwise'
print('ok: the completion marks and the tries have a client that never waits, with the kernel\'s escape')
