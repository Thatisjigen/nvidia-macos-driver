#!/usr/bin/env python3
"""A monitor plugged in while the GPU had no power is looked for when the power is back.

NVRMFB hears of a display through NVKMS's event (hotplugEvent -> hotplugKick -> hotplugFire). A GPU in standby or
without power sends none, and NVKMS's resume brings back the heads it had without asking any connector. So a cable
plugged into an output that has no display, while the GPU was off, was not noticed when the GPU came back: the
"NVRMGpuPower" handler lit the heads that had a mode again (gpuWakeFire) and looked at nothing.

The handler now runs the event's scan for an output with no display. An output that has its display is left alone:
its head is lit again by gpuWakeFire, and a scan there could at most report a monitor that is slow to wake as gone.

This compiles the handler itself, taken from the source, and counts what it starts for each state NVRM sends."""
import pathlib, re, subprocess, sys, tempfile

root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()

# the handler: from its test of the function name to the return that ends it
a = src.index('if (functionName && functionName->isEqualTo("NVRMGpuPower")) {')
b = src.index('return kIOReturnSuccess;', a)
block = src[a:src.index('}', b) + 1]
assert block.count('{') == block.count('}'), 'the handler was not taken whole'

FIXTURE = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
typedef int IOReturn; enum { kIOReturnSuccess = 0, kIOReturnUnsupported = -1 };
#define FBLOG(...) do { } while (0)
struct OSSymbol { const char *s; bool isEqualTo(const char *o) const { return strcmp(s, o) == 0; } };
struct FB {
    bool fModeSet = false, fHeadSlept = false, fSysSlept = false, fDpyLost = false;
    void *fSurf = nullptr, *fGpuWake = (void *)1;
    unsigned fHead = 0, fDisplay = 0x100, fIndex = 0;
    int scans = 0, relights = 0;
    void scheduleCall(void *, uint64_t) { relights++; }
    void hotplugKick() { scans++; }
    IOReturn call(const OSSymbol *functionName, void *param1) {
        %BLOCK%
        return kIOReturnUnsupported;
    }
};
static void run(const char *what, bool lost, bool modeSet, uintptr_t state)
{
    FB fb; fb.fDpyLost = lost; fb.fModeSet = modeSet; fb.fSurf = modeSet ? (void *)1 : nullptr;
    const OSSymbol fn = { "NVRMGpuPower" };
    const IOReturn r = fb.call(&fn, (void *)state);
    printf("%s state=%lu rc=%d scans=%d relights=%d headSlept=%d\n", what, (unsigned long)state, r, fb.scans, fb.relights, (int)fb.fHeadSlept);
}
int main()
{
    run("EMPTY", true, false, 1);     // an output that started with no display, GPU back
    run("PULLED", true, true, 1);     // a display that went away, GPU back
    run("LIT", false, true, 1);       // an output with its display, GPU back
    run("EMPTY", true, false, 0);     // on the way to standby
    run("LIT", false, true, 0);
    run("EMPTY", true, false, 2);     // GPU lost
    return 0;
}
'''
WANT = '''EMPTY state=1 rc=0 scans=1 relights=1 headSlept=0
PULLED state=1 rc=0 scans=1 relights=1 headSlept=0
LIT state=1 rc=0 scans=0 relights=1 headSlept=0
EMPTY state=0 rc=0 scans=0 relights=0 headSlept=0
LIT state=0 rc=0 scans=0 relights=0 headSlept=1
EMPTY state=2 rc=0 scans=0 relights=0 headSlept=0
'''
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    (d / 'test.cpp').write_text(FIXTURE.replace('%BLOCK%', block))
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wno-unused-variable', str(d / 'test.cpp'), '-o', str(d / 'test')], check=True)
    got = subprocess.run([str(d / 'test')], check=True, capture_output=True, text=True).stdout
if got != WANT:
    sys.exit('the GPU power handler does not scan as expected:\n' + got + 'wanted:\n' + WANT)

# The scan is the one the event runs: delayed, on the shared call, and it relights an output whose display is back.
kick = src[src.index('void NVRMNVDAFramebuffer::hotplugKick()'):]
kick = kick[:kick.index('\n}')]
assert 'thread_call_enter_delayed(gHotplug' in kick, 'hotplugKick queues the scan'
assert re.search(r'NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED\)\s*\n\s*hotplugKick\(\);', src), 'the NVKMS event runs the same scan'
fire = src[src.index('void NVRMNVDAFramebuffer::hotplugFire('):]
fire = fire[:fire.index('\nstatic char gTrace')]
assert 'fb->fDpyLost = false;' in fire and 'relight[nrelight++] = fb' in fire, 'the scan takes a display that is back'
print('ok: GPU back scans an output with no display, and only that')
