#!/usr/bin/env python3
"""At WindowServer's commit a surface another accelerator owns is taken from it, and marked again.

Intel writes an IOSurface and the RTX writes it: set_current_device_cache refuses. A refused mark used to leave the pixels for the
completion thread: a blocking mark and a CPU lock, on the one thread that releases the marks every page-off waits for, and a write
lock with no owner left waits for this accelerator's lock too (synchronize_device_caches, accelRelease on every cache). Here the
production code from plugin/nvmtl_vk.c runs against a small model of the kernel's answers: the surface's owner, the refusal, and a
read-only CPU lock that takes the surface from its owner before it pages it off, as in the disassembly of IOSurface 393.5.11. It says
when the lock happens, which client each mark takes and what a hung owner costs; it says nothing about the kernel or the GPUs: that
is xgpuprobe and debug.nvaccel_sv_other_owner, on this plugin. The negative control is the same code without the second mark, which
must leave the surface with its old owner."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
vk = (root / 'plugin/nvmtl_vk.c').read_text()
abi = (root / 'kexts/NVRM/nvrm_surfshare_abi.h').read_text()
events = (root / 'plugin/NVMTLEventSync.m').read_text()
objects = (root / 'plugin/NVMTLObjects.m').read_text()

def define(text, name):
    return int(re.search(r'#define %s\s+(0x[0-9A-Fa-f]+)u' % name, text).group(1), 0)
for name in ('OFF', 'BUSY', 'OTHER_OWNER', 'PLANE_TRYLOCK'):
    assert define(vk, 'NVMTL_SS_' + name) == define(abi, 'NVRM_SS_' + name), 'status %s differs from the ABI header' % name

start = vk.index("// WindowServer's marks have NVRM clients of their own.")
code = vk[start:vk.index("// 0 = the kernel took it. NVK's surface escape only if the client could not be opened.")]

pend = events[events.index('static void nvmtl_ws_mark_pending('):]
pend = pend[:pend.index('\n}\n')]
assert pend.index('nvmtl_vk_surface_dirty_commit(') < pend.index('nvmtl_ws_lock()'), 'the commit can wait for the other GPU: never under the marks\' lock'
settled = events[events.index('static BOOL nvmtl_ws_publish_settled('):]
settled = settled[:settled.index('\n}\n')]
assert '[d->texture nvmtlSurfaceOutRefused]' in settled and '[d->texture nvmtlSurfaceOut]' not in settled, \
    'a publication after a refused mark runs on the thread that releases the marks: no blocking mark there'
start_commit = events.index('- (void)nvmtlSubmitWithEvents')
commit = events[start_commit:events.index('\n}\n', start_commit)]
first_lock_end = commit.index('\n    }\n')
assert first_lock_end < commit.index('nvmtl_ws_mark_pending(self);') < commit.index('@synchronized ([NVMTLCommandBuffer class])', first_lock_end), \
    'the marks, and with them the lock, stay outside the class lock'
refused = objects[objects.index('- (BOOL)nvmtlSurfaceOutRefused {'):]
refused = refused[:refused.index('\n}\n')]
for waits in ('IOSurfaceLock(', 'nvmtl_vk_surface_dirty(', 'nvmtl_vk_image_read', 'lets_go'):
    assert waits not in refused, 'nothing that can wait on the completion thread, and nothing that takes the surface later: ' + waits
assert refused.index('_vramOn') < refused.index('nvmtl_vk_surface_dirty_try('), 'a private copy has no mark to try'
assert re.search(r'copy = marked == -1;', refused), 'only a refusal that has nothing to do with an owner goes to the copy'
assert 'IOSurfaceBindAccelOnPlane' not in vk, 'the lock runs the bind\'s code with no private call'

fixture = r'''
#include <assert.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef int IOReturn; typedef unsigned io_connect_t, io_service_t; typedef void *VkDevice, *IOSurfaceRef;
#define kIOReturnSuccess 0
#define kIOReturnBusy 2
#define kIOReturnNoMemory 3
#define kIOReturnTimeout 4
#define kIOSurfaceLockReadOnly 1
static _Atomic int retains;
#define CFRetain(x) atomic_fetch_add(&retains, 1)
#define CFRelease(x) atomic_fetch_sub(&retains, 1)
#define KERN_SUCCESS 0
#define kIOMainPortDefault 0
#define RTX 1u
#define INTEL 2u
static const char *progname = "WindowServer";
#define getprogname() progname
#define nvlog(...) ((void)0)
#define IOServiceMatching(name) 0
#define mach_task_self() 0
static unsigned opens, opensFail;
static io_service_t IOServiceGetMatchingService(int port, int matching) { return 7; }
static void IOObjectRelease(io_service_t s) {}
static int IOServiceOpen(io_service_t s, int task, unsigned type, io_connect_t *client) {
    assert(type == 0x4E560000u);
    if (opensFail) return 1;
    *client = ++opens + 100;
    return KERN_SUCCESS;
}
// the kernel's answers, as nvAccelSurfVram gives them
static _Atomic unsigned owner;         // the surface's owner: 0, RTX or INTEL
static unsigned takeAfterLock;         // the Intel GPU takes the surface back between the lock and the second mark
static io_connect_t lastClient; static unsigned lastPlane, marksSent, viaNVK;
static uint32_t nvkAnswer;
static int IOConnectCallMethod(io_connect_t c, unsigned sel, const uint64_t *in, unsigned nin, void *data, size_t size, void *o1, void *o2, void *out, size_t *outSize) {
    assert(sel == 1 && nin == 1 && in[0] == 0xE3 && size == 40 && *outSize == 40 && out == data);
    uint32_t *e = data;   // version, surfaceID, plane, hClient, hSrcClient, hSrcMemory, status, pad
    assert(e[0] == 2 && e[1] == 44);
    lastClient = c; lastPlane = e[2]; marksSent++;
    if (atomic_load(&owner) == 0 || atomic_load(&owner) == RTX) { atomic_store(&owner, RTX); e[6] = 0; return KERN_SUCCESS; }
    e[6] = 0x80000008u;
    return KERN_SUCCESS;
}
static uint32_t (*pnvk_surface_dirty)(VkDevice, uint32_t, uint32_t);
static uint32_t nvk_dirty(VkDevice d, uint32_t s, uint32_t p) { viaNVK++; lastPlane = p; return nvkAnswer; }
static VkDevice g_dev = (VkDevice)1;
static int nvmtl_gpu_headless(void) { return 0; }
#define NVMTL_MARK_OTHER_OWNER (-2)
// synchronize_device_caches: the owner is taken away first, then its page-off runs, and that is what can hang
static dispatch_semaphore_t hung; static _Atomic int hungNow;
static _Atomic unsigned locks; static IOReturn lockAnswer;
static IOReturn IOSurfaceLock(IOSurfaceRef s, unsigned o, void *seed) {
    assert(s == (void *)0x44 && o == kIOSurfaceLockReadOnly && !seed);
    atomic_fetch_add(&locks, 1);
    if (lockAnswer != kIOReturnSuccess) return lockAnswer;
    atomic_store(&owner, 0);
    if (atomic_load(&hungNow)) dispatch_semaphore_wait(hung, DISPATCH_TIME_FOREVER);
    if (takeAfterLock) atomic_store(&owner, INTEL);
    return kIOReturnSuccess;
}
static _Atomic unsigned unlocks;
static IOReturn IOSurfaceUnlock(IOSurfaceRef s, unsigned o, void *seed) { assert(o == kIOSurfaceLockReadOnly); atomic_fetch_add(&unlocks, 1); return kIOReturnSuccess; }
'''

checks = r'''
static void drain(void) { for (int i = 0; i < 2000 && atomic_load(&g_owner_jobs); i++) usleep(1000); assert(!atomic_load(&g_owner_jobs) && !atomic_load(&retains)); }
static void reset(unsigned o) { drain(); atomic_store(&owner, o); takeAfterLock = 0; atomic_store(&locks, 0); atomic_store(&unlocks, 0); lockAnswer = kIOReturnSuccess; marksSent = 0; viaNVK = 0; nvkAnswer = 0; }
static int commit(void) { return nvmtl_vk_surface_dirty_commit((void *)0x44, 44, 0); }
static double ms(uint64_t t0) { return (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6; }
int main(void) {
    pnvk_surface_dirty = nvk_dirty;
    // nobody's, or already this GPU's: one mark, no lock
    reset(0); assert(commit() == 0 && !atomic_load(&locks) && marksSent == 1 && atomic_load(&owner) == RTX);
    const io_connect_t waitClient = lastClient;
    assert(lastPlane == 0);
    reset(RTX); assert(commit() == 0 && !atomic_load(&locks) && marksSent == 1);
    // the Intel's: refused, the CPU lock takes it from the Intel, marked again
    reset(INTEL); assert(commit() == 0 && atomic_load(&locks) == 1 && atomic_load(&unlocks) == 1 && marksSent == 2 && atomic_load(&owner) == RTX);
    assert(lastClient == waitClient);
#ifndef CONTROL
    // the lock failed: no second mark
    reset(INTEL); lockAnswer = 5; assert(commit() == NVMTL_MARK_OTHER_OWNER && atomic_load(&locks) == 1 && !atomic_load(&unlocks) && marksSent == 1 && atomic_load(&owner) == INTEL);
    // the Intel took it back meanwhile: refused again, and no loop on the compositor's thread
    reset(INTEL); takeAfterLock = 1; assert(commit() == NVMTL_MARK_OTHER_OWNER && atomic_load(&locks) == 1 && marksSent == 2 && atomic_load(&owner) == INTEL);
    // a try goes on the other client, with the try bit, and a refused one takes nothing
    reset(INTEL); assert(nvmtl_vk_surface_dirty_try(44, 0) == NVMTL_MARK_OTHER_OWNER && !atomic_load(&locks) && marksSent == 1);
    assert(lastClient != waitClient && lastPlane == 0x40000000u);
    const io_connect_t marksClient = lastClient;
    reset(0); assert(nvmtl_vk_surface_dirty_try(44, 0) == 0 && lastClient == marksClient);
    reset(0); assert(nvmtl_vk_surface_dirty(44, 0) == 0 && lastClient == waitClient);
    assert(opens == 2);
    // the owner hangs: the commit goes on after the limit, a few times, then at once, and it all comes back
    hung = dispatch_semaphore_create(0); atomic_store(&hungNow, 1);
    reset(INTEL);
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    assert(commit() == 0 && marksSent == 2 && atomic_load(&owner) == RTX);   // the lock that hangs has taken the surface from its owner already
    const double first = ms(t0);
    assert(first >= 40 && first < 500);   // the limit is 50 ms
    atomic_store(&owner, INTEL);          // behind it nothing has started: the owner stays and the mark stays refused
    t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    for (int i = 0; i < 3; i++) assert(commit() == NVMTL_MARK_OTHER_OWNER);
    assert(ms(t0) < 1000 && atomic_load(&owner) == INTEL);
    t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    for (int i = 0; i < 100; i++) assert(commit() == NVMTL_MARK_OTHER_OWNER);
    assert(ms(t0) < 20);                  // nobody waits any more, and nothing more goes in line
    atomic_store(&hungNow, 0);
    for (int i = 0; i < 4; i++) dispatch_semaphore_signal(hung);
    reset(INTEL); assert(commit() == 0 && atomic_load(&owner) == RTX && marksSent == 2);
    puts("PASS: refused at commit, a CPU lock takes the surface from its owner and the mark goes in again; a failed lock or an Intel write in between stay refused; tries and waiting marks take different clients; an owner that hangs costs 50 ms a few times, then nothing, and it comes back");
#else
    puts("control: the surface was left with another owner, and this line must not be reached");
#endif
    return 0;
}
'''

# In another process no client of its own is opened, and with no client NVK's escape carries the mark. Each needs its own program:
# the choice is read once per process.
others = r'''
int main(void) {
    pnvk_surface_dirty = nvk_dirty;
    WHO;
    atomic_store(&owner, RTX);
    nvkAnswer = 0;
    assert(nvmtl_vk_surface_dirty_commit((void *)0x44, 44, 0) == 0 && viaNVK == 1 && !marksSent && !atomic_load(&locks));
    assert(nvmtl_vk_surface_dirty_try(44, 0) == 0 && viaNVK == 2 && lastPlane == 0x40000000u && !marksSent);
    // the status comes through NVK's client too: the owner still lets go, and the mark goes in again
    nvkAnswer = 0x80000008u;
    assert(nvmtl_vk_surface_dirty_commit((void *)0x44, 44, 0) == NVMTL_MARK_OTHER_OWNER && atomic_load(&locks) == 1 && viaNVK == 4);
    puts("PASS: WHAT");
    return 0;
}
'''

def build_and_run(d, name, source, flags=()):
    src = pathlib.Path(d) / (name + '.m')
    src.write_text(source)
    exe = pathlib.Path(d) / name
    subprocess.run(['clang', '-w', '-fobjc-arc', *flags, '-o', str(exe), str(src)], check=True)
    return subprocess.run([str(exe)], capture_output=True, text=True)

with tempfile.TemporaryDirectory() as d:
    r = build_and_run(d, 'take', fixture + code + checks)
    print(r.stdout.strip(), r.stderr.strip())
    assert r.returncode == 0, 'the commit must take the surface and mark again'
    for name, who, what in (('other', 'progname = "Safari"', 'another process keeps NVK\'s client'),
                            ('noclient', 'opensFail = 1', 'without a client of its own the mark goes through NVK\'s')):
        r = build_and_run(d, name, fixture + code + others.replace('WHO', who).replace('WHAT', what))
        print(r.stdout.strip(), r.stderr.strip())
        assert r.returncode == 0, name
    # negative control: the same code without the second mark
    once = 'if (let_go == kIOReturnSuccess || let_go == kIOReturnTimeout) marked = nvmtl_vk_surface_mark_newest(surfaceID, plane, 0);'
    assert once in code, 'the second mark moved: the control needs it'
    r = build_and_run(d, 'control', fixture + code.replace(once, '') + checks, ('-DCONTROL',))
    assert r.returncode != 0 and 'must not be reached' not in r.stdout, 'the control must fail: without the second mark the surface stays the Intel\'s'
print('ok: the negative control fails as it should')
