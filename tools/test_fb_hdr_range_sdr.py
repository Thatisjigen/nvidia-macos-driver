#!/usr/bin/env python3
"""The timing range that offers HDR10 still lets macOS make SDR timings for an HDMI monitor.

With HDR10 in the range macOS makes each timing as wire formats and declines, before it asks the driver, every format
whose encoding, depth, colorimetry or dynamic range the range does not list. On an HDMI monitor its SDR formats are BT.709
only (SDR_RGB_8_BT709, SDR_RGB_10_BT709, ...): a range listing native RGB, sRGB, BT.2020 and BT.2100 declined all of them,
so the monitor got HDR10 timings and scaled modes, no default mode, and no place in System Settings. Here macOS's check is
modelled from those formats, and run against the range bits publishTimingCaps() really sets, read from nvrm-fb.cpp; the
control is the old range, the same lines without BT.709. Without HDR10 the range must not change.

An HDMI sink that takes YCbCr 4:2:2 also gets 4:2:2 at 12 bits in the range, for the HDR10 timings RGB at 10 bits does not fit
(3840x2160 at 60 Hz on an HDMI 2.0 sink, which macOS declines in RGB for the sink's clock). The driver's refusals are modelled
next to macOS's: SDR above 8 bits, and 4:2:2 anywhere RGB at 10 bits fits, so every other timing is installed as before."""
import pathlib, re
root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
start = src.index('void NVRMNVDAFramebuffer::publishTimingCaps()')
body = src[start:src.index('\n}\n', start)]
# IOGraphicsTypes.h range bits
BITS = {'kIORangeColorimetryNativeRGB': 0x1, 'kIORangeColorimetrysRGB': 0x2, 'kIORangeColorimetryBT709': 0x80,
        'kIORangeColorimetryBT2020': 0x100, 'kIORangeColorimetryBT2100': 0x200, 'kIORangeDynamicRangeSDR': 0x1,
        'kIORangeDynamicRangeHDR10': 0x2, 'kIORangeBitsPerColorComponent8': 0x2, 'kIORangeBitsPerColorComponent10': 0x4,
        'kIORangePixelEncodingRGB444': 0x1, 'kIORangePixelEncodingYCbCr422': 0x4, 'kIORangeBitsPerColorComponent12': 0x8}
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
BITS_OF = {0x2: 8, 0x4: 10, 0x8: 12}
SINK_MHZ = 600                       # the LG's HDMI Forum block: HDMI 2.0
def fits_sink(fmt, pclk_mhz):        # macOS's own check ("Display_ClockLimit"): TMDS characters per second
    enc, bpc = fmt[0], fmt[1]
    if enc == 0x4: return pclk_mhz <= SINK_MHZ                                  # 4:2:2: 12 bits in the 8-bit clock
    if enc == 0x8: return pclk_mhz / 2 * BITS_OF[bpc] / 8 <= SINK_MHZ
    return pclk_mhz * BITS_OF[bpc] / 8 <= SINK_MHZ
validate = src[src.index('IOReturn NVRMNVDAFramebuffer::validateDetailedTiming('):]
validate = validate[:validate.index('\n}\n')]
assert 'nvrmTimingDepthOk(d->bitsPerColorComponent, d->dynamicRange == kIODynamicRangeHDR10)' in validate
assert 'd->pixelEncoding == kIOPixelEncodingYCbCr422 &&' in validate and '!(d->dynamicRange == kIODynamicRangeHDR10 && gHdrOn' in validate
assert 'nvrmHdr422Needed(d->pixelClock, fHdr422TmdsMHz)' in validate
def driver(fmt, pclk_mhz):           # validateDetailedTiming, as the two asserts above read it
    enc, bpc, clr, dyn = fmt
    if dyn == 0x1 and bpc > 0x2: return False
    if enc == 0x4:
        return dyn == 0x2 and pclk_mhz * 5 / 4 > SINK_MHZ and pclk_mhz <= SINK_MHZ
    return True
# macOS's own check of the timing against the range ("fbRange_a"): with link limits published it declined every timing above
# a quarter of the link-0 maximum, whatever the format (seen with 1200000 kHz: 297 MHz kept, 339.95 and 594 declined)
link = re.search(r'r\.maxNumLinks = (\d+); r\.minLink0PixelClock = (\d+); r\.maxLink0PixelClock = (\d+);', body)
assert link, 'the link fields of the range are not where the test reads them'
LINK0_KHZ = int(link.group(3)) if int(link.group(1)) else 0
def fits_range(pclk_mhz, link0_khz):
    return not link0_khz or pclk_mhz * 1000 * 4 <= link0_khz
def installed(r, tries, pclk_mhz, link0_khz=None):   # the first wire format the range lists, the sink takes and the driver accepts
    link0_khz = LINK0_KHZ if link0_khz is None else link0_khz
    for f in tries:
        if allowed(r, f) and fits_sink(f, pclk_mhz) and fits_range(pclk_mhz, link0_khz) and driver(f, pclk_mhz): return f
    return None
RGB8, RGB10, Y422 = (0x1, 0x2, 0x80, 0x1), (0x1, 0x4, 0x100, 0x2), (0x4, 0x8, 0x100, 0x2)
now = rng(base, hdr, True)
assert [f for f in SDR if allowed(now, f)] == [(0x4, 0x8, 0x80, 0x1), (0x1, 0x4, 0x80, 0x1), (0x1, 0x2, 0x80, 0x1)], \
    'with HDR10 offered, macOS cannot make the SDR timings of an HDMI monitor in RGB'
assert [f for f in HDR if allowed(now, f)] == [Y422, RGB10], 'HDR10 is not offered in RGB at 10 bits and in 4:2:2'
# 3840x2160 at 60 Hz (594 MHz), 1920x1080 at 144 (339.95), 3840x2160 at 30 (297): SDR stays RGB at 8 bits everywhere, HDR10 is
# RGB at 10 bits wherever that fits and 4:2:2 at 4K 60 Hz only
for pclk in (594, 339.95, 297):
    assert installed(now, SDR, pclk) == RGB8, pclk
assert installed(now, HDR, 594) == Y422
assert installed(now, HDR, 339.95) == RGB10 and installed(now, HDR, 297) == RGB10
assert LINK0_KHZ == 0, 'the range limits link 0 again'
# control: with the link limit of before, macOS kept none of the LG's timings above 300 MHz, SDR included
assert installed(now, HDR, 594, 1200000) is None and installed(now, SDR, 594, 1200000) is None
assert installed(now, SDR, 339.95, 1200000) is None and installed(now, HDR, 297, 1200000) == RGB10
# control: the range of before (RGB only) had no HDR10 timing at all at 4K 60 Hz, which is what macOS showed
rgb_only = rng(base, hdr[:hdr.index('if (fHdr422TmdsMHz) {')], True)
assert installed(rgb_only, HDR, 594) is None and installed(rgb_only, HDR, 297) == RGB10
# control: the range without BT.709 declined every SDR format
old = rng(base, hdr.replace('kIORangeColorimetryBT709 | ', ''), True)
assert not [f for f in SDR if allowed(old, f)] and [f for f in HDR if allowed(old, f)]
# without HDR10 the range is the one of before: native RGB and sRGB, SDR, 8 bits
plain = rng(base, hdr, False)
assert plain == {'supportedColorimetryModes': 0x3, 'supportedDynamicRangeModes': 0x1,
                 'supportedBitsPerColorComponent': 0x2, 'supportedPixelEncoding': 0x1}, plain
# 4:2:0 is never listed: NVKMS refuses it with BT.2100 colorimetry (nvChooseCurrentColorFormatAndRangeEvo)
assert 'kIORangePixelEncodingYCbCr420' not in hdr
print('PASS: with HDR10 offered an HDMI monitor keeps its SDR formats (RGB, BT.709); HDR10 is RGB at 10 bits, 4:2:2 only where that does not fit')
