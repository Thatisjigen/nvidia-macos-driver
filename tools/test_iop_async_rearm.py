#!/usr/bin/env python3
"""One stop must not leave the swap synchronous until the next boot.

Three refused flips in a row turn the asynchronous swap off, because the synchronous one copies a refused frame. A display
going to sleep or waking refuses every flip for a moment (commit rejected, flipResult 1, on every stop of an RTX 5050 here), so it
went off at the first stop that had three in a row. It must come back once flips are accepted again, and must stay off while they
are not. The decision is lifted from nvrm-accel.cpp, the negative control is the old policy that never comes back. It says
nothing about the kernel: that is debug.nvaccel_iop_async after a stop, on this kext."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
accel = (root / 'kexts/NVRM/accel/nvrm-accel.cpp').read_text()

def lift(name):
    i = accel.index('static inline bool %s(' % name)
    return accel[i:accel.index('\n', i) + 1]

def body(signature):
    i = accel.index(signature + '\n{')
    return accel[i:accel.index('\n}\n', i)]

flip = body('static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe)')
refused = flip.index('if (rr != kIOReturnSuccess) {')
assert 'gIopAsyncGoodRun = 0;' in flip[refused:refused + 80], 'a refusal must restart the count of accepted flips'
assert refused < flip.index('nvAccelIopAsyncComesBack(gIopAsyncAutoOff, gIopAsyncRearm, ++gIopAsyncGoodRun)'), 'only an accepted flip may count'
assert flip.index('nvAccelFlipOutcome(rr) == kNvFlipKept') < refused, 'a flip kept late is neither accepted nor refused'
run = body('static void nvAccelIopAsyncRun(void *p0, void *)')
assert 'gIopAsyncAutoOff = true; gIopAsyncGoodRun = 0;' in run, 'turning it off for refusals must be what allows it back'
install = accel[accel.index('static void nvaccelInstallSysctl(void)'):]
assert 'sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_rearm)' in install, 'the sysctl is not registered'

negative = 'static inline bool nvAccelIopAsyncComesBack(bool, int, int) { return false; }\n'

fixture = r'''
#include <cstdio>
typedef int SInt32;
%s
// the state of nvAccelIopAsyncRun and nvAccelIopFlip
static int async = 1, refuseRun, goodRun, rearm = 120; static bool autoOff;
static void async_flip(bool accepted) {
    if (accepted) { refuseRun = 0; return; }
    if (++refuseRun >= 3 && async) { async = 0; autoOff = true; goodRun = 0; }
}
static void sync_flip(bool accepted) {
    if (!accepted) { goodRun = 0; return; }
    if (nvAccelIopAsyncComesBack(autoOff, rearm, ++goodRun)) { autoOff = false; refuseRun = 0; async = 1; }
}
static void flip(bool accepted) { if (async) async_flip(accepted); else sync_flip(accepted); }
int main() {
    int bad = 0;
    for (int i = 0; i < 1000; i++) flip(true);
    flip(false); flip(false); flip(true); flip(false);
    if (!async) { puts("FAIL refusals with an accepted flip between them turned it off"); bad++; }
    flip(false); flip(false); flip(false);
    if (async) { puts("FAIL three refusals in a row did not turn it off"); bad++; }
    for (int i = 0; i < 119; i++) flip(true);
    if (async) { puts("FAIL it came back before the run of accepted flips"); bad++; }
    flip(false);
    for (int i = 0; i < 119; i++) flip(true);
    if (async) { puts("FAIL a refusal did not restart the count"); bad++; }
    flip(true);
    if (!async) { puts("FAIL it did not come back after the run of accepted flips"); bad++; }
    if (refuseRun) { puts("FAIL it came back with the old refusals still counted"); bad++; }
    // flips that go on being refused stay on the copying path
    for (int i = 0; i < 5; i++) flip(false);
    for (int i = 0; i < 1000; i++) flip(false);
    if (async) { puts("FAIL it came back while flips were refused"); bad++; }
    // rearm 0, and an asynchronous swap turned off by hand
    autoOff = true; async = 0; rearm = 0; goodRun = 0;
    for (int i = 0; i < 1000; i++) flip(true);
    if (async) { puts("FAIL debug.nvaccel_iop_async_rearm=0 still came back"); bad++; }
    autoOff = false; rearm = 120; goodRun = 0;
    for (int i = 0; i < 1000; i++) flip(true);
    if (async) { puts("FAIL a swap turned off by hand came back"); bad++; }
    return bad;
}
'''

def run_fixture(code, label):
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / 'rearm.cpp'
        src.write_text(fixture % code)
        exe = pathlib.Path(d) / 'rearm'
        subprocess.run(['clang++', '-std=c++17', '-o', str(exe), str(src)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print('%s: rc %d%s' % (label, r.returncode, (' ' + r.stdout.strip().replace('\n', ' | ')) if r.stdout.strip() else ''))
        return r.returncode

ok = run_fixture(lift('nvAccelIopAsyncComesBack'), 'shipped decision')
assert ok == 0, 'the shipped decision failed an arm'
bad = run_fixture(negative, 'control: it never comes back')
assert bad > 0, 'the control did not fire: the test cannot see the difference the fix makes'
print('ok: the swap comes back after a run of accepted flips and stays off while they are refused (%d arm(s) fired)' % bad)
