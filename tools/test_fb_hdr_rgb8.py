#!/usr/bin/env python3
"""HDR10 above the 10-bit HDMI RGB limit goes out in RGB at 8 bits, not in NVKMS's YCbCr 4:2:2.

From Ampere on NVKMS sends RGB at 10 bits over HDMI only up to a 324 MHz pixel clock. An HDMI monitor at 1080p 144 Hz
(339.95 MHz) in HDR10 got 4:2:2 at 10 bits from NVKMS's own fallback, soft text and dull colours; RGB at 8 bits with PQ
looked right. The rule: RGB at 8 bits when RGB at 10 does not fit (the GPU's limit, or 1.25 times the clock over the sink's)
and RGB at 8 does; 10 bits everywhere else. It needs the default colorimetry (BT.2100 asks for at least 10 bits), and
nvfbhdr422=1 or a depth set by hand keeps the old way. The control is each case that stays at 10 bits or stays 4:2:2."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
fb = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
for need in ['const bool rgb8 = !gHdr422 && !gHdrBt2020 && nvrmHdrRgb8(mc.mode.timings.pixelClockHz, fHdmiTmdsMHz);',
             'if (!gColorBpc && mc.colorBpc == 0) mc.colorBpc = rgb8 ? 8 : 10;',
             'if (fYccActive && !rgb8 && (NvU32)gColorFormat == kNvrmColorFormatAuto)',
             'gHdr422 = PE_parse_boot_argn("nvfbhdr422", &v, sizeof v) && v;',
             'fHdmiTmdsMHz = nvrmEdidHdmiTmdsMHz(fEdid, fEdidSize, false);']:
    assert need in fb, 'missing: ' + need
fixture = r'''
#include <cassert>
#include <cstdio>
#include "nvrm-fb-safety.h"
int main() {
    const unsigned lg = 600;                                       // HDMI 2.0 sink
    assert(nvrmHdrRgb8(339950000ull, lg));                         // 1080p 144 Hz: over 324, RGB 8 fits
    assert(nvrmHdrRgb8(594000000ull, lg));                         // 4K 60 Hz: RGB 8 at 594 fits 600
    assert(!nvrmHdrRgb8(296700000ull, lg) && !nvrmHdrRgb8(324000000ull, lg));   // 120 Hz and the edge: RGB at 10 bits
    assert(nvrmHdrRgb8(324000001ull, lg));
    assert(!nvrmHdrRgb8(1188000000ull, lg));                       // 4K 120 Hz: RGB 8 does not fit either
    assert(nvrmHdrRgb8(297000000ull, 340) && !nvrmHdrRgb8(272000000ull, 340));   // HDMI 1.4: 10 bits stop at 340 * 4 / 5
    assert(!nvrmHdrRgb8(339950000ull, 0) && !nvrmHdrRgb8(0, lg));  // not HDMI, no clock: unchanged
    puts("PASS: HDR10 over the 10-bit HDMI limit in RGB at 8 bits where that fits; 10 bits below it");
}
'''
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d); (d / 't.cpp').write_text(fixture)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Werror', f'-I{root}/kexts/NVRMFB/fb', str(d / 't.cpp'), '-o', str(d / 't')], check=True)
    subprocess.run([str(d / 't')], check=True)
