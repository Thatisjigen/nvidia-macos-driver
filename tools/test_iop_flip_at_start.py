#!/usr/bin/env python3
"""Zero-copy flips must not depend on who gets there first at boot.

NVRM's auto-go kept the copy path when another GPU's accelerator was already published, and IntelAccelerator is published within a second of that
look. On an RTX 5050 with an Iris Xe the same setup came up with flips on when auto-go looked 2 ms before the publication, and off when it looked
150 ms after. So auto-go no longer looks, and what it logs is what happens. -nvrmnoflip keeps the copy path, an unplaced BAR1 below 512 MB keeps it
too, and frame by frame VRAM the plugin did not write is copied, which is what the look was there for.

auto-go's decision is lifted from NVRM.cpp and run on a fake device. The negative controls are a decision that still stays off for an accelerator,
and one with no BAR1 check. It says nothing about the kernel: that is debug.nvaccel_iop_flip and auto-go's log line after a boot, on this kext."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
nvrm = (root / 'kexts/NVRM/NVRM.cpp').read_text()
accel = (root / 'kexts/NVRM/accel/nvrm-accel.cpp').read_text()

gate = 'if (armed == 1 && iop == 1 && !PE_parse_boot_argn("-nvrmnoflip", nb, sizeof(nb))) {'
assert nvrm.count(gate) == 1, 'flips must still wait for the armed stack, and -nvrmnoflip must still keep the copy path'
go = nvrm[nvrm.index(gate):]
decision = go[go.index('const char *why = nullptr;'):go.index('if (why) {')]
after = go[go.index('if (why) {'):]
after = after[:after.index('thread_terminate(current_thread());\n}')]

whole = go[:go.index('thread_terminate(current_thread());\n}')]
assert 'IOAccelerator' not in whole and 'getMatchingServices' not in whole, 'auto-go looks for another accelerator again: the race decides again'
assert len(re.findall(r'\bwhy\s*=[^=]', decision)) == 2, 'its initial value and the BAR1 case: "NOT started" must be logged for that alone'
assert after.index('zero-copy flip NOT started') < after.index('nvrmSysW("debug.nvaccel_iop_flip", 1)'), 'the write must come after the only way out'
assert 'nvrm-flip", f == 1 ? "on" : "FAILED"' in after, 'the property must say what the sysctl says'
assert not re.search(r'nvrmSysW\("debug\.nvaccel_iop_flip",\s*0', nvrm), 'auto-go must never turn flips off'

# What stands in for the look, and the way back.
flip = accel[accel.index('static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe)\n{'):]
flip = flip[:flip.index('\n}\n')]
assert 'if (!gIopFlip ' in flip and '!nvAccelIopVramFlippable(' in flip, "the sysctl and the frame-by-frame check are what stands in for the look"
assert 'CTLFLAG_RW' in accel[accel.index('SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_flip,'):][:120], 'it must stay writable: the way back with no reboot'
assert len(re.findall(r'\bgIopFlip\s*=[^=]', accel)) == 1, 'NVAccel only starts it at 0: auto-go and the sysctl turn flips on and off'

checks = r'''
#include <cstdint>
#include <cstdio>
#define NV_GPU_BAR_INDEX_FB 1
#define LOG(...) do { } while (0)
struct S { bool fBar1Placed; struct { struct { uint64_t size; } bars[4]; } fNv; };
static const uint64_t MB = 1ull << 20;
static bool keepsCopy(bool placed, uint64_t bytes) {
    S device = { placed, { { { 0 }, { bytes } } } }; S *s = &device;
    //LIFTED
    return why != nullptr;
}
#define NEED(what, ok) do { if (!(ok)) { puts("FAIL: " what); return 1; } } while (0)
int main() {
    NEED("a placed 8 GB BAR1 stays on the copy path", !keepsCopy(true, 8192 * MB));
    NEED("an unplaced 256 MB window flips", keepsCopy(false, 256 * MB));
    NEED("the limit for an unplaced window is 512 MB", !keepsCopy(false, 512 * MB) && keepsCopy(false, 511 * MB));
    NEED("a placed small window must flip as before, -nvrmsmallbar's case", !keepsCopy(true, 256 * MB));
    puts("flips start unless an unplaced BAR1 is below 512 MB; another GPU's accelerator plays no part");
    return 0;
}
'''

def run(d, name, decide):
    src = pathlib.Path(d) / (name + '.cpp')
    src.write_text(checks.replace('//LIFTED', decide))
    exe = pathlib.Path(d) / name
    subprocess.run(['clang++', '-std=c++17', '-o', str(exe), str(src)], check=True)
    return subprocess.run([str(exe)], capture_output=True, text=True)

small = re.search(r'if \(!s->fBar1Placed && bar1MB < 512\) why = "[^"]*";\n\s*else ', decision)
assert small, 'the BAR1 case is not where this test expects it'
controls = {
    'stays off for an accelerator': decision + 'if (!why) why = "another GPU\'s accelerator is running";\n',
    'no BAR1 check': decision.replace(small.group(0), ''),
}
with tempfile.TemporaryDirectory() as d:
    r = run(d, 'shipped', decision)
    print(r.stdout.strip())
    assert r.returncode == 0
    for i, (name, decide) in enumerate(controls.items()):
        c = run(d, 'control%d' % i, decide)
        print('control, %s: %s' % (name, c.stdout.strip()))
        assert c.returncode != 0, 'control "%s" must fail: the test could not see the difference otherwise' % name
print('ok')
