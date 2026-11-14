#!/usr/bin/env python3
"""HDR10 in YCbCr 4:2:2 over HDMI, for the timings RGB at 10 bits does not fit.

An HDMI 2.0 sink takes 3840x2160 at 60 Hz (594 MHz) in RGB at 8 bits, but RGB at 10 bits needs 742.5 MHz of its 600, so
macOS declined every HDR10 timing at 4K 50 and 60 Hz and HDR could not be turned on there. 4:2:2 is carried at 12 bits in
the 8-bit clock. The sink's limit and its 4:2:2 support are read from the EDID the way NVKMS reads them (EDID 1.3: the CTA
flag; 1.4: the feature byte and the depth; HDMI block with 10-bit deep colour; HDMI Forum block or SCDB for the clock), and
4:2:2 is taken only where RGB at 10 bits does not fit and 4:2:2 does. The control is every EDID that lacks one piece, and
the timings RGB fits. The call sites: the range lists 4:2:2 at 12 bits only for such a sink, the mode set tells NVKMS 4:2:2
for those timings unless a colour format was set by hand, and the state is put back when the mode set fails."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
fb = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
def body(signature):
    s = fb.index(signature)
    return fb[s:fb.index('\n}\n', s)]
caps = body('void NVRMNVDAFramebuffer::publishTimingCaps()')
hdr_range = caps[caps.index('if (gHdrOn && fHdrCapable) {'):]
assert 'if (fHdr422TmdsMHz) {' in hdr_range and 'r.supportedPixelEncoding |= kIORangePixelEncodingYCbCr422;' in hdr_range
assert 'kIORangePixelEncodingYCbCr422' not in caps[:caps.index('if (gHdrOn && fHdrCapable) {')], '4:2:2 offered without HDR10'
assert 'fHdr422TmdsMHz = nvrmEdidHdmi422TmdsMHz(fEdid, fEdidSize);' in body('void NVRMNVDAFramebuffer::takeEdid(')
assert 'fYccWant = fHdrWant && nvrmHdr422Needed(dt.pixelClock, fHdr422TmdsMHz);' in fb
assert 'if (fYccActive && (NvU32)gColorFormat == kNvrmColorFormatAuto) nvrmSetColorFormat(&mc, kNvrmColorFormatYCbCr422, 0);' in fb
switch = body('IOReturn NVRMNVDAFramebuffer::switchMode(')
assert 'fHdrActive = fHdrWant; fYccActive = fYccWant;' in switch
assert switch.count('fYccActive = yccBefore;') == 2, 'a failed mode set leaves the head marked 4:2:2'
fixture = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "nvrm-fb-safety.h"
// An HDMI monitor's EDID with no identity: base block 1.3 (or 1.4), one CTA-861 extension with the blocks given
static void edid(unsigned char *e, unsigned minor, bool ycc422, bool vsdb, bool dc30, int hf, bool scdb) {
    memset(e, 0, 256);
    static const unsigned char head[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    memcpy(e, head, 8); e[18] = 1; e[19] = (unsigned char)minor; e[20] = 0x80; e[126] = 1;
    unsigned char *x = e + 128; x[0] = 0x02; x[1] = 3; x[3] = (unsigned char)(0x60 | (ycc422 ? 0x10 : 0));
    unsigned i = 4;
    x[i++] = (2 << 5) | 2; x[i++] = 16; x[i++] = 97;                                   // video: 1080p60, 2160p60
    if (vsdb) { x[i++] = (3 << 5) | 7; x[i++] = 0x03; x[i++] = 0x0c; x[i++] = 0x00; x[i++] = 0x10; x[i++] = 0x00;
                x[i++] = (unsigned char)(0x80 | (dc30 ? 0x30 : 0)); x[i++] = 60; }      // HDMI 1.4 block: AI, DC36/DC30, 300 MHz
    if (hf >= 0 && !scdb) { x[i++] = (3 << 5) | 7; x[i++] = 0xd8; x[i++] = 0x5d; x[i++] = 0xc4; x[i++] = 1;
                            x[i++] = (unsigned char)(hf / 5); x[i++] = 0x80; x[i++] = 0x03; }
    if (hf >= 0 && scdb) { x[i++] = (7 << 5) | 7; x[i++] = 0x79; x[i++] = 0; x[i++] = 0; x[i++] = 1;
                           x[i++] = (unsigned char)(hf / 5); x[i++] = 0x80; x[i++] = 0x03; }
    x[i++] = (7 << 5) | 3; x[i++] = 6; x[i++] = 0x05; x[i++] = 0x01;                 // HDR static metadata: PQ
    x[2] = (unsigned char)i;
}
int main() {
    unsigned char e[256];
    edid(e, 3, true, true, true, 600, false); assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 600);   // the LG's layout
    assert(nvrmEdidHdrPq(e, 256, nullptr));
    edid(e, 3, true, true, true, 600, true);  assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 600);   // the clock from an SCDB
    edid(e, 3, true, true, true, -1, false);  assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 340);   // HDMI 1.4 only
    edid(e, 3, true, true, true, 300, false); assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 340);   // never below 340, as NVKMS
    edid(e, 3, true, true, true, 1275, false); assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 600);  // TMDS stops at HDMI 2.0's 600
    edid(e, 3, false, true, true, 600, false); assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 0);    // no 4:2:2
    edid(e, 3, true, true, false, 600, false); assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 0);    // no 10-bit deep colour
    edid(e, 3, true, false, true, 600, false); assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 0);    // not HDMI
    edid(e, 3, true, true, true, 600, false);
    assert(!nvrmEdidHdmi422TmdsMHz(e, 128) && !nvrmEdidHdmi422TmdsMHz(nullptr, 256));
    // EDID 1.4: the base block says 4:2:2 and the depth; the CTA flag no longer counts (NVKMS's GetYuv422MaxBpc)
    edid(e, 4, true, true, true, 600, false); e[20] = 0x80 | (3 << 4) | 5; e[24] = 0x10;        // 10 bits, 4:2:2
    assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 600);
    e[20] = 0x80 | (2 << 4) | 5; assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 0);                  // 8 bits
    e[20] = 0x80 | (3 << 4) | 5; e[24] = 0x08; assert(nvrmEdidHdmi422TmdsMHz(e, 256) == 0);    // 4:4:4, no 4:2:2
    // which HDR10 timings go out in 4:2:2 on a 600 MHz sink
    assert(nvrmHdr422Needed(594000000ull, 600));             // 3840x2160 at 60 Hz: RGB 10 bits is 742.5 MHz
    assert(nvrmHdr422Needed(594000000ull, 600) && nvrmHdr422Needed(593406000ull, 600));        // 50 Hz and 59.94 Hz too
    assert(!nvrmHdr422Needed(297000000ull, 600));            // 4K at 30 Hz: RGB at 10 bits fits, stays RGB
    assert(!nvrmHdr422Needed(339950000ull, 600));            // 1080p at 144 Hz: 425 MHz, stays RGB
    assert(!nvrmHdr422Needed(480000000ull, 600) && nvrmHdr422Needed(480000001ull, 600));       // the edge: 480 MHz x 1.25 = 600
    assert(!nvrmHdr422Needed(1188000000ull, 600));           // 4K at 120 Hz: 4:2:2 does not fit either
    assert(!nvrmHdr422Needed(594000000ull, 340));            // an HDMI 1.4 sink: nothing at 594 MHz
    assert(!nvrmHdr422Needed(594000000ull, 0));              // not a 4:2:2 sink: as before
    puts("PASS: HDMI 4:2:2 sink and its TMDS limit read from the EDID as NVKMS reads them; 4:2:2 only where RGB at 10 bits does not fit");
}
'''
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d); (d / 't.cpp').write_text(fixture)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Werror', f'-I{root}/kexts/NVRMFB/fb', str(d / 't.cpp'), '-o', str(d / 't')], check=True)
    subprocess.run([str(d / 't')], check=True)
