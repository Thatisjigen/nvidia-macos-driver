#!/usr/bin/env python3
"""The refresh rates NVRMFB adds from NVKMS stay SDR when macOS lists HDR10 timings (nvfbhdr=1).

With HDR10 in the range macOS lists HDR10 timings for a PQ monitor, and on an HDMI monitor whose 144 Hz rate exists only in
NVKMS's list, the unscaled timings macOS kept for the native raster were all HDR10. The 144 Hz copy took its fields from the
first of them, so it was an HDR10 timing: never taken for the boot raster, and the mode set macOS asked for was an HDR10 one
at 144 Hz that the head refused, leaving the monitor on the driver's stand-in mode. The real nvrmRateSeed is compiled from
nvrm-fb-safety.h, and again without its HDR10 line (the control, the old loop); the call sites are checked in nvrm-fb.cpp."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
hdr = (root / 'kexts/NVRMFB/fb/nvrm-fb-safety.h').read_text()
src = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
assert 'const NvrmRateSeed seed = nvrmRateSeed(fDT, n, fromMac, k.hVisible, k.vVisible, hz);' in src, \
    'addKmsRefreshModes no longer asks nvrmRateSeed which raster, rate and template macOS lists'
assert 'if (d.dynamicRange == kIODynamicRangeHDR10) continue;   // an SDR copy, from an SDR timing' in src, \
    'the derived half and quarter rates can be copied from an HDR10 timing again'
assert '(unsigned)kNvrmDynamicRangeHDR10 == (unsigned)kIODynamicRangeHDR10' in src, 'the HDR10 bit is not checked against IOGraphics'
start = hdr.index('template <class T>\nstatic inline NvrmRateSeed nvrmRateSeed(')
helper = hdr[start:hdr.index('\n}\n', start) + 3]
line = '        if (d.dynamicRange == kNvrmDynamicRangeHDR10) continue;\n'
assert line in helper, 'nvrmRateSeed counts HDR10 timings again'
before = helper.replace(line, '').replace('nvrmRateSeed(', 'before(')
fixture = r'''
#include <cassert>
#include <cstdio>
#include "nvrm-fb-safety.h"
struct T { unsigned horizontalActive, verticalActive, horizontalBlanking, verticalBlanking, horizontalScaled, verticalScaled;
           unsigned long long pixelClock; unsigned short dynamicRange; };
''' + before + r'''
int main() {
    // macOS's list for the LG on the HDMI with nvfbhdr=1: HDR10 at 60 and 120 Hz, an HDR10 and a plain scaled desktop at 144 Hz
    const T hdr60 = { 1920, 1080, 280, 45, 0, 0, 148500000, kNvrmDynamicRangeHDR10 };
    const T hdr120 = { 1920, 1080, 280, 45, 0, 0, 297000000, kNvrmDynamicRangeHDR10 };
    const T hdrDesk = { 1920, 1080, 160, 55, 2560, 1440, 339950000, kNvrmDynamicRangeHDR10 };
    const T desk = { 1920, 1080, 160, 55, 1280, 720, 339950000, 0 };
    const T mac[4] = { hdr60, hdr120, hdrDesk, desk };
    // NVKMS's 144 Hz: the raster is listed (the plain scaled desktop), the rate is not, and there is no SDR timing to copy
    NvrmRateSeed s = nvrmRateSeed(mac, 4, 4, 1920, 1080, 144);
    assert(s.raster && !s.have && s.tmpl < 0);                  // the copy starts empty: an SDR timing, never HDR10
    NvrmRateSeed o = before(mac, 4, 4, 1920, 1080, 144);
    assert(o.raster && !o.have && o.tmpl == 0);                 // control: copied from the HDR10 60 Hz timing
    // NVKMS's 60 Hz: macOS lists it in HDR10 only, so the SDR rate is added
    s = nvrmRateSeed(mac, 4, 4, 1920, 1080, 60);
    assert(!s.have);
    o = before(mac, 4, 4, 1920, 1080, 60);
    assert(o.have);                                             // control: the HDR10 timing hid the SDR rate
    // A raster macOS lists in HDR10 only (3840x2160 at 30 Hz on that monitor) is not listed for an SDR rate
    const T uhd30 = { 3840, 2160, 560, 90, 0, 0, 297000000, kNvrmDynamicRangeHDR10 };
    assert(!nvrmRateSeed(&uhd30, 1, 1, 3840, 2160, 60).raster);
    assert(before(&uhd30, 1, 1, 3840, 2160, 60).raster);       // control: NVKMS's 4K rates were added for it
    // With an SDR timing in the list, that one is the template, wherever the HDR10 twin is
    const T sdr60 = { 1920, 1080, 280, 45, 0, 0, 148500000, 1 };
    const T both[2] = { hdr60, sdr60 };
    s = nvrmRateSeed(both, 2, 2, 1920, 1080, 144);
    assert(s.raster && !s.have && s.tmpl == 1);
    assert(before(both, 2, 2, 1920, 1080, 144).tmpl == 0);
    // Without HDR10 nothing changes: the old and the new rule agree on an SDR list
    const T sdr[3] = { sdr60, { 1920, 1080, 280, 45, 0, 0, 297000000, 1 }, desk };
    const unsigned rates[5] = { 50, 60, 100, 120, 144 };
    for (unsigned hz : rates) {
        const NvrmRateSeed a = nvrmRateSeed(sdr, 3, 3, 1920, 1080, hz), b = before(sdr, 3, 3, 1920, 1080, hz);
        assert(a.raster == b.raster && a.have == b.have && a.tmpl == b.tmpl);
    }
    // An entry the driver added (index >= fromMac) lists the rate but is neither the raster from macOS nor a template
    const T ours[2] = { desk, { 1920, 1080, 160, 55, 0, 0, 339950000, 0 } };
    s = nvrmRateSeed(ours, 2, 1, 1920, 1080, 144);
    assert(s.raster && s.have);
    s = nvrmRateSeed(ours, 2, 1, 1920, 1080, 60);
    assert(s.raster && !s.have && s.tmpl < 0);
    puts("PASS: rates added from NVKMS copy SDR timings only; HDR10 timings neither list a raster or rate nor seed a copy");
}
'''
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d); (d/'t.cpp').write_text(fixture)
    subprocess.run(['clang++','-std=c++17','-Wall','-Werror',f'-I{root}/kexts/NVRMFB/fb',str(d/'t.cpp'),'-o',str(d/'t')],check=True)
    subprocess.run([str(d/'t')],check=True)
