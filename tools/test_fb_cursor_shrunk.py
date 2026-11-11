#!/usr/bin/env python3
"""On a scaled desktop larger than the scanned surface the cursor plane gets the cursor image shrunk to the raster.

macOS's HiDPI "looks like 1920x1080" on a 1920x1080 monitor is a 3840x2160 desktop the head shrinks to the raster. The
cursor image comes at the desktop's scale and the cursor plane drew it on the raster unscaled, so the arrow showed at twice
its size. The real nvrmShrinkCursor and nvrmDesktopShrunk are compiled from nvrm-fb-safety.h; the call sites are checked in
nvrm-fb.cpp: the image is built from macOS's copy, again after a mode change, and the plane's position is offset by the
hotspot on the raster. The hardware cursor stays reported: macOS kept sending cursor states to the plane when it was not,
and the arrow disappeared. The control is the old path, the image packed as it came."""
import pathlib, re, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
def body(sig):
    s = src.index(sig); return src[s:src.index('\n}\n', s)]
build = body('bool NVRMNVDAFramebuffer::buildCursor()')
assert 'if (desktopShrunk()) {' in build and 'nvrmShrinkCursor(fCurSrc, w, h, dw, dh, tw, th, small, &ow, &oh)' in build, \
    'the cursor image is not shrunk on a shrunk desktop'
assert 'hx = (SInt32)(((SInt64)hx * tw) / dw); hy = (SInt32)(((SInt64)hy * th) / dh);' in build, 'the hotspot is not shrunk with the image'
assert 'fCurHotX = fCurSrcHotX; fCurHotY = fCurSrcHotY; fCurTurnedHotX = thx; fCurTurnedHotY = thy;' in build
assert 'packed = buildCursor();' in body('IOReturn NVRMNVDAFramebuffer::setCursorImageLocked(void *cursorImage)'), \
    'a new cursor image does not go through buildCursor'
assert 'if (OSCompareAndSwap(1, 0, &fCurRebuild) && fCurHaveImage && buildCursor()) {' in body('bool NVRMNVDAFramebuffer::cursorApply(bool imageChanged)'), \
    'a mode change does not rebuild the cursor image'
assert 'OSCompareAndSwap(0, 1, &fCurRebuild);' in body('IOReturn NVRMNVDAFramebuffer::setDisplayMode(IODisplayModeID mode, IOIndex depth)')
assert '} else { x = px - fCurTurnedHotX; y = py - fCurTurnedHotY; }' in body('IOReturn NVRMNVDAFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible)'), \
    'the plane is offset by the desktop hotspot, not the raster one'
assert 'uintptr_t v = (fCursorEnabled && fCurSurf && fCurKva && !fSurActive) ? 1 : 0;' in src, 'the hardware cursor is no longer reported'
fixture = r'''
#include <cassert>
#include <cstdio>
#include "nvrm-fb-safety.h"
int main() {
    assert(nvrmDesktopShrunk(3840, 2160, 1920, 1080) && nvrmDesktopShrunk(2560, 1440, 1920, 1080));
    assert(!nvrmDesktopShrunk(1920, 1080, 1920, 1080) && !nvrmDesktopShrunk(1280, 720, 1920, 1080) && !nvrmDesktopShrunk(0, 0, 1920, 1080));
    // a 32x64 arrow (what macOS sends at 2x) on the 3840x2160 desktop of a 1920x1080 raster: 16x32 on the plane
    static unsigned src[32 * 64], dst[256 * 256];
    for (unsigned i = 0; i < 32 * 64; i++) src[i] = 0xffffffffu;
    unsigned ow = 0, oh = 0;
    assert(nvrmShrinkCursor(src, 32, 64, 3840, 2160, 1920, 1080, dst, &ow, &oh) && ow == 16 && oh == 32);
    assert(dst[0] == 0xffffffffu && dst[16 * 32 - 1] == 0xffffffffu);
    // one opaque red pixel in a 2x2 block, three transparent: a quarter of the alpha, the colour stays red (no dark fringe)
    unsigned blk[4] = { 0xffff0000u, 0, 0, 0 }, one[1];
    assert(nvrmShrinkCursor(blk, 2, 2, 3840, 2160, 1920, 1080, one, &ow, &oh) && ow == 1 && oh == 1);
    assert((one[0] >> 24) == 64 && ((one[0] >> 16) & 255) == 255 && (one[0] & 0xffff) == 0);
    // a 2560x1440 desktop on 1920x1080: three quarters of the size, rounded up
    assert(nvrmShrinkCursor(src, 32, 64, 2560, 1440, 1920, 1080, dst, &ow, &oh) && ow == 24 && oh == 48);
    // OLD PATH (control): the image went to the plane as it came, 32x64 on the raster, twice the desktop's arrow
    const unsigned oldW = 32, oldH = 64;
    assert(oldW == 2 * 16 && oldH == 2 * 32);
    puts("PASS: on a desktop larger than the raster the cursor image and its hotspot are shrunk by the head's ratio");
}
'''
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d); (d/'t.cpp').write_text(fixture)
    subprocess.run(['clang++','-std=c++17','-Wall','-Werror',f'-I{root}/kexts/NVRMFB/fb',str(d/'t.cpp'),'-o',str(d/'t')],check=True)
    subprocess.run([str(d/'t')],check=True)
