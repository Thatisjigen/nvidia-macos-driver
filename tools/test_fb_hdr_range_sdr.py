#!/usr/bin/env python3
"""The timing range that offers HDR10 still lets macOS make SDR timings for an HDMI monitor.

With HDR10 in the range macOS makes each timing as wire formats and declines, before it asks the driver, every format
whose encoding, depth, colorimetry or dynamic range the range does not list. On an HDMI monitor its SDR formats are BT.709
only (SDR_RGB_8_BT709, SDR_RGB_10_BT709, ...): a range listing native RGB, sRGB, BT.2020 and BT.2100 declined all of them,
so the monitor got HDR10 timings and scaled modes, no default mode, and no place in System Settings. Here macOS's check is
modelled from those formats, and run against the range bits publishTimingCaps() really sets, read from nvrm-fb.cpp; the
control is the old range, the same lines without BT.709. Without HDR10 the range must not change."""
import pathlib, re
root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
start = src.index('void NVRMNVDAFramebuffer::publishTimingCaps()')
body = src[start:src.index('\n}\n', start)]
# IOGraphicsTypes.h range bits
BITS = {'kIORangeColorimetryNativeRGB': 0x1, 'kIORangeColorimetrysRGB': 0x2, 'kIORangeColorimetryBT709': 0x80,
        'kIORangeColorimetryBT2020': 0x100, 'kIORangeColorimetryBT2100': 0x200, 'kIORangeDynamicRangeSDR': 0x1,
        'kIORangeDynamicRangeHDR10': 0x2, 'kIORangeBitsPerColorComponent8': 0x2, 'kIORangeBitsPerColorComponent10': 0x4,
        'kIORangePixelEncodingRGB444': 0x1}
def value(expr):
    v = 0
    for name in re.findall(r'kIORange\w+', expr):
        assert name in BITS, 'unknown range bit ' + name
        v |= BITS[name]
    return v
def field(text, name, op):
    m = re.findall(r'r\.' + name + r' ' + re.escape(op) + r' ([^;]+);', text)
    return value(' '.join(m))
hdr_at = body.index('if (gHdrOn && fHdrCapable) {')
base, hdr = body[:hdr_at], body[hdr_at:]
def rng(text_base, text_hdr, with_hdr):
    r = {f: field(text_base, f, '=') for f in ('supportedColorimetryModes', 'supportedDynamicRangeModes',
                                                 'supportedBitsPerColorComponent', 'supportedPixelEncoding')}
    if with_hdr:
        for f in r: r[f] |= field(text_hdr, f, '|=')
    return r
# The wire formats macOS tried for the LG on the HDMI, in its order (CoreDisplay's log): encoding, depth, colorimetry, range
SDR = [(0x8, 0x4, 0x80, 0x1), (0x4, 0x8, 0x80, 0x1), (0x2, 0x4, 0x80, 0x1), (0x2, 0x2, 0x80, 0x1), (0x1, 0x4, 0x80, 0x1),
       (0x1, 0x2, 0x80, 0x1), (0x8, 0x2, 0x80, 0x1)]
HDR = [(0x8, 0x4, 0x100, 0x2), (0x4, 0x8, 0x100, 0x2), (0x2, 0x4, 0x100, 0x2), (0x1, 0x4, 0x100, 0x2)]
def allowed(r, fmt):
    enc, bpc, clr, dyn = fmt
    return bool(r['supportedPixelEncoding'] & enc and r['supportedBitsPerColorComponent'] & bpc and
                r['supportedColorimetryModes'] & clr and r['supportedDynamicRangeModes'] & dyn)
now = rng(base, hdr, True)
assert [f for f in SDR if allowed(now, f)] == [(0x1, 0x4, 0x80, 0x1), (0x1, 0x2, 0x80, 0x1)], \
    'with HDR10 offered, macOS cannot make the SDR timings of an HDMI monitor in RGB'
assert [f for f in HDR if allowed(now, f)] == [(0x1, 0x4, 0x100, 0x2)], 'HDR10 RGB at 10 bits is no longer offered'
# control: the range without BT.709 declined every SDR format
old = rng(base, hdr.replace('kIORangeColorimetryBT709 | ', ''), True)
assert not [f for f in SDR if allowed(old, f)] and [f for f in HDR if allowed(old, f)]
# without HDR10 the range is the one of before: native RGB and sRGB, SDR, 8 bits
plain = rng(base, hdr, False)
assert plain == {'supportedColorimetryModes': 0x3, 'supportedDynamicRangeModes': 0x1,
                 'supportedBitsPerColorComponent': 0x2, 'supportedPixelEncoding': 0x1}, plain
print('PASS: with HDR10 offered an HDMI monitor keeps its SDR formats (RGB, BT.709) next to HDR10; the old range declined them all')
