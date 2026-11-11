#!/usr/bin/env python3
"""HDR10 and 30-bit scanout, end to end across NVRMFB and NVAccel, off unless nvfbhdr=1 / nvfb10bit=1.

Pixel conversions are tested in tools/test_iop_10bit.c; here: the display's HDR block is read from a real CTA-861 layout, the
scanout reply flag is the same bit on both sides, and the call sites are present: HDR timings advertised only for PQ
displays, an HDR10 timing is a different mode from the SDR timing of the same raster, a 30-bit surface for the mode set,
the infoframe and colorimetry on every config with the gamma ramp off and no colour passthrough, and the accelerator's converter keyed on the
reply flag. The switch is read before the range is published, and the range that offers HDR10 lists the 10 bits macOS makes
HDR10 timings at while SDR timings stay at 8. The control is the old driver: SDR only, an 8-bit surface, one copy loop."""
import filecmp, pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
fb = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
acc = (root / 'kexts/NVRM/accel/nvrm-accel.cpp').read_text()
for need in ['r.supportedDynamicRangeModes |= kIORangeDynamicRangeHDR10', 'if (gHdrOn && fHdrCapable) {',
             '(d.dynamicRange == kIODynamicRangeHDR10) == fHdrActive', 'NvKmsSurfaceMemoryFormatX2B10G10R10',
             'r->flags = fScan30 ? NVRM_VRAM_SCANOUT_30BIT : 0;', 'mc.hdrInfoFrame.eotf = NVKMS_INFOFRAME_EOTF_ST2084',
             'mc.colorimetry = NVKMS_OUTPUT_COLORIMETRY_BT2100', 'mc.olut.enabled = NV_FALSE',
             'attribute == kIOFBHDRMetaDataAttribute', 'PE_parse_boot_argn("nvfbhdr"', 'PE_parse_boot_argn("nvfb10bit"']:
    assert need in fb, 'missing in NVRMFB: ' + need
# NVKMS 620 refuses any request with colour passthrough unless the display HAL has supportsColorPassthrough, which only
# one class has; Blackwell's do not, and every HDR10 mode set that asked for it was refused.
assert 'mc.postcompColorPassthrough = NV_TRUE' not in fb, 'HDR10 asks NVKMS for colour passthrough again'
# The timing range is published from kapiInit() and offers HDR10 only if gHdrOn is set by then: start() has to read the
# switch first, and under the start lock, or the first display to start never gets HDR10 in its range.
def body(signature):
    s = fb.index(signature)
    return fb[s:fb.index('\n}\n', s)]
start = body('bool NVRMNVDAFramebuffer::start(IOService *provider)')
assert 'publishTimingCaps();' in body('bool NVRMNVDAFramebuffer::kapiInit()') and 'publishTimingCaps();' not in start
assert start.index('IORecursiveLockLock(sl);') < start.index('gHdrOn = PE_parse_boot_argn("nvfbhdr"') < start.index('bool kapiOk = kapiInit();'), \
    'nvfbhdr is read after kapiInit() published the timing range'
# The range that offers HDR10 lists 10 bits per component, and the validation keeps SDR timings at 8 (the fixture says why)
caps = body('void NVRMNVDAFramebuffer::publishTimingCaps()')
hdr_range = caps[caps.index('if (gHdrOn && fHdrCapable) {'):]
assert 'r.supportedBitsPerColorComponent |= kIORangeBitsPerColorComponent10;' in hdr_range[:hdr_range.index('\n    }')], 'HDR10 is offered at 8 bits per component only'
assert 'r.supportedBitsPerColorComponent = kIORangeBitsPerColorComponent8;' in caps[:caps.index('if (gHdrOn && fHdrCapable) {')]
assert 'nvrmTimingDepthOk(d->bitsPerColorComponent, d->dynamicRange == kIODynamicRangeHDR10)' in body('IOReturn NVRMNVDAFramebuffer::validateDetailedTiming(')
for need in ['q.flags & NVRM_VRAM_SCANOUT_30BIT', 'nv_iop_convert_row(dp, sp, w, srcFmt, dst30)', 'fmt != NV_IOP_FMT_RGHA']:
    assert need in acc, 'missing in NVAccel: ' + need
for a in ['NVRMFB', 'NVRM', 'NMIntelFB']:
    assert 'NVRM_VRAM_SCANOUT_30BIT 0x1u' in (root / f'kexts/{a}/nvrm_vram_abi.h').read_text(), a
assert filecmp.cmp(root / 'kexts/NVRMFB/nvrm_vram_abi.h', root / 'kexts/NVRM/nvrm_vram_abi.h', shallow=False)
fixture = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "nvrm-fb-safety.h"
// EDID with a CTA-861 extension carrying: a video data block, then the HDR static metadata block
static void edid(unsigned char *e, bool pq) {
    memset(e, 0, 256); e[126] = 1;
    unsigned char *x = e + 128; x[0] = 0x02; x[1] = 3;
    unsigned i = 4;
    x[i++] = (2 << 5) | 2; x[i++] = 0x10; x[i++] = 0x1f;                    // video data block, 2 bytes
    x[i++] = (7 << 5) | 6; x[i++] = 6; x[i++] = pq ? 0x05 : 0x01; x[i++] = 0x01; x[i++] = 0x6b; x[i++] = 0x5b; x[i++] = 0x20;
    x[2] = i;                                                                // where the detailed timings start
}
// The depth macOS installs one timing at, as CoreDisplay logs its tries: SDR at 12, 10, then 8 bits, HDR10 at 12, then 10; a
// depth the framebuffer's range does not list is dropped before the driver is asked; the first one accepted is installed.
static unsigned installed(unsigned rangeDepths, bool hdr10, bool (*driver)(unsigned, bool)) {
    static const unsigned sdr[] = { kNvrmDepth12, kNvrmDepth10, kNvrmDepth8 }, hdr[] = { kNvrmDepth12, kNvrmDepth10 };
    const unsigned *tries = hdr10 ? hdr : sdr, n = hdr10 ? 2 : 3;
    for (unsigned i = 0; i < n; i++) if ((rangeDepths & tries[i]) && driver(tries[i], hdr10)) return tries[i];
    return 0;
}
static bool anyDepth(unsigned, bool) { return true; }                         // a validation that does not look at the depth
int main() {
    const unsigned was = kNvrmDepth8, now = kNvrmDepth8 | kNvrmDepth10;       // the range of a display that is offered HDR10
    assert(installed(was, true, anyDepth) == 0);                              // control: 8 bits listed, no HDR10 timing at all
    assert(installed(now, true, nvrmTimingDepthOk) == kNvrmDepth10);
    assert(installed(now, false, nvrmTimingDepthOk) == kNvrmDepth8);          // SDR stays at 8 bits,
    assert(installed(now, false, anyDepth) == kNvrmDepth10);                  // which it would not with 10 listed and no refusal
    assert(installed(was, false, nvrmTimingDepthOk) == kNvrmDepth8);          // a display that is not offered HDR10: as before
    assert(nvrmTimingDepthOk(0, false) && nvrmTimingDepthOk(kNvrmDepth12, true) && !nvrmTimingDepthOk(kNvrmDepth12, false));   // no depth named: passes
    unsigned char e[256]; unsigned code = 0;
    edid(e, true);  assert(nvrmEdidHdrPq(e, 256, &code) && code == 0x6b);
    edid(e, false); assert(!nvrmEdidHdrPq(e, 256, &code));                    // HDR block without PQ: no HDR10
    memset(e, 0, 256); assert(!nvrmEdidHdrPq(e, 256, &code));                 // no extension
    edid(e, true); assert(!nvrmEdidHdrPq(e, 128, &code) && !nvrmEdidHdrPq(nullptr, 256, &code));
    edid(e, true); e[128 + 4 + 3 + 1] = 5;                                    // a different extended tag
    assert(!nvrmEdidHdrPq(e, 256, &code));
    assert(nvrmHdrLuminanceFromCode(0) == 0 && nvrmHdrLuminanceFromCode(32) == 100 && nvrmHdrLuminanceFromCode(64) == 200);
    assert(nvrmHdrLuminanceFromCode(0x6b) > 500 && nvrmHdrLuminanceFromCode(0x6b) < 1000);                   // 0x6b = 107: about 50 * 2^(107/32)
    assert(nvrmHdrLuminanceFromCode(255) > 12000 && nvrmHdrLuminanceFromCode(255) < 13000);   // 50 * 2^(255/32)
    puts("PASS: HDR block parsed from a CTA-861 extension, PQ required, luminance code decoded, HDR10 at 10 bits with SDR kept at 8");
}
'''
# The boot raster is lit in SDR. macOS lists an HDR10 timing of the same raster and rate next to the SDR one, and
# setDetailedTimings makes the first timing that is the boot raster the current mode, without a mode set: it has to be the SDR
# one. The real dtIsBootRaster is compiled twice, as it is and without its HDR10 line (the control).
boot = body('bool NVRMNVDAFramebuffer::dtIsBootRaster(') + '\n}\n'
hdr_line = '    if (d.dynamicRange == kIODynamicRangeHDR10) return false;'
assert hdr_line in boot, 'an HDR10 timing can be taken for the boot raster'
before = '\n'.join(l for l in boot.splitlines() if not l.startswith(hdr_line)).replace('::dtIsBootRaster(', '::before(') + '\n'
boot_fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
typedef uint64_t UInt64; typedef uint32_t NvU32;
enum { kIODynamicRangeSDR = 1, kIODynamicRangeHDR10 = 2 };
struct IODetailedTimingInformationV2 { unsigned horizontalScaled, verticalScaled, horizontalActive, verticalActive, horizontalBlanking, verticalBlanking, dynamicRange; UInt64 pixelClock; };
struct FB {
    unsigned fBootW = 1920, fBootH = 1080; struct { struct { unsigned refreshRate; } timings; } fBootMode = { { 143998 } };
    bool dtIsBootRaster(const IODetailedTimingInformationV2 &d) const; bool before(const IODetailedTimingInformationV2 &d) const;
};
'''
boot_checks = r'''
int main() {
    FB fb;
    IODetailedTimingInformationV2 sdr = { 0, 0, 1920, 1080, 160, 55, kIODynamicRangeSDR, 339950000 }, hdr = sdr, slow = sdr;
    hdr.dynamicRange = kIODynamicRangeHDR10;
    slow.horizontalBlanking = 280; slow.verticalBlanking = 45; slow.pixelClock = 148350000;      // the same raster at 60 Hz
    const IODetailedTimingInformationV2 list[3] = { slow, hdr, sdr };                             // the HDR10 twin comes first
    int now = -1, was = -1;
    for (int i = 2; i >= 0; i--) { if (fb.dtIsBootRaster(list[i])) now = i; if (fb.before(list[i])) was = i; }
    assert(now == 2);                                  // the SDR timing, wherever it is in the list
    assert(was == 1);                                  // control: the HDR10 timing was taken for the boot raster
    puts("PASS: the boot raster is its SDR timing, not the HDR10 timing of the same raster and rate");
}
'''
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d); (d/'t.cpp').write_text(fixture)
    subprocess.run(['clang++','-std=c++17','-Wall','-Werror',f'-I{root}/kexts/NVRMFB/fb',str(d/'t.cpp'),'-o',str(d/'t')],check=True)
    subprocess.run([str(d/'t')],check=True)
    (d/'b.cpp').write_text(boot_fixture + (boot + before).replace('NVRMNVDAFramebuffer::', 'FB::') + boot_checks)
    subprocess.run(['clang++','-std=c++17','-Wall','-Werror',str(d/'b.cpp'),'-o',str(d/'b')],check=True)
    subprocess.run([str(d/'b')],check=True)
