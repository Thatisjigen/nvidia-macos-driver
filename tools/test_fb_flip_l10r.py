#!/usr/bin/env python3
"""30-bit depth and direct scan-out of macOS's 10-bit surfaces.

With only 32-bit depth offered, WindowServer composed every display of NVRMFB in 8-bit BGRA, HDR10 included; the 10-bit
l10r surfaces it made at times went to the CPU copy. NVRMFB now offers 30-bit as depth 1 (IO30BitDirectPixels, red high,
the layout of l10r), and an l10r surface is flipped as NVKMS's X2B10G10R10 with the layer's FMT matrix swapping red and blue
back. The swap is checked on real pixels: an l10r word decoded the way the window reads X2B10G10R10, then multiplied by the
matrix, gives the colour macOS wrote; the control is the same decode without the matrix, red and blue exchanged. The call
sites: depth 1 offered, accepted and answered back; every flip names the override, on for l10r and off otherwise; NVAccel
flips l10r only where the framebuffer says it can, with its own switch; the flip ABI is the same on both sides."""
import filecmp, pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
fb = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
acc = (root / 'kexts/NVRM/accel/nvrm-accel.cpp').read_text()
def body(text, signature):
    s = text.index(signature)
    return text[s:text.index('\n}\n', s)]
assert 'static const char kPixelFormats30[] = IO32BitDirectPixels "\\0" IO30BitDirectPixels "\\0";' in fb
assert 'return !fProxy && g30Bit ? kPixelFormats30 : kPixelFormats;' in fb
assert 'if (PE_parse_boot_argn("nvfb30", &v, sizeof v) && !v)' in fb, 'no way to turn the 30-bit depth off'
pix = body(fb, 'IOReturn NVRMNVDAFramebuffer::getPixelInformation(')
assert "pi->componentMasks[0] = 0x3ff00000; pi->componentMasks[1] = 0x000ffc00; pi->componentMasks[2] = 0x000003ff;" in pix
assert 'strlcpy(pi->pixelFormat, IO30BitDirectPixels, sizeof pi->pixelFormat);' in pix
setmode = body(fb, 'IOReturn NVRMNVDAFramebuffer::setDisplayMode(')
assert 'if (depth < 0 || depth > (g30Bit && !fProxy ? 1 : 0)) return kIOReturnUnsupportedMode;' in setmode
assert '{ fDepth = depth; refreshDisplayAudioProperties(); }' in setmode
assert 'if (depth) *depth = fDepth;' in body(fb, 'IOReturn NVRMNVDAFramebuffer::getCurrentDisplayMode(')
flip = body(fb, 'bool NVRMNVDAFramebuffer::flipToMemory(')
for need in ['if (!mem || !g30Bit) l10r = false;', 'fFlipCache[i].l10r == l10r',
             'sp.format = l10r ? NvKmsSurfaceMemoryFormatX2B10G10R10 : NvKmsSurfaceMemoryFormatX8R8G8B8;',
             'nvrmLayerL10rSwap(lr, l10r);', '{ fFrontSurf = surf; fFrontL10r = l10r; }']:
    assert need in flip, 'flipToMemory: ' + need
helper = body(fb, 'static void nvrmLayerL10rSwap(')
for need in ['nvrmL10rSwapMatrix(lr->config.matrixOverrides.fmtCtm.m);',
             'lr->config.matrixOverrides.enabled.fmtCtm = l10r ? NV_TRUE : NV_FALSE;', 'lr->flags.matrixOverridesChanged = NV_TRUE;']:
    assert need in helper, 'nvrmLayerL10rSwap: ' + need
# a mode set takes the override from the request: one that puts an l10r surface back on the head keeps the swap
assert 'nvrmLayerL10rSwap(lr, front == fFrontSurf && fFrontL10r);' in body(fb, 'bool NVRMNVDAFramebuffer::reflip()')
sw = body(fb, 'IOReturn NVRMNVDAFramebuffer::switchMode(')
assert 'nvrmLayerL10rSwap(lr, keepFront && fFrontL10r);' in sw and 'fFrontL10r = keepFront && fFrontL10r;' in sw
assert fb.count('(f->flags & NVRM_FLIP_FLAG_L10R) != 0') == 2, 'one of the two flip entry points drops the l10r flag'
assert 'setProperty("NVRMFBFlipL10R", g30Bit && !fProxy ? kOSBooleanTrue : kOSBooleanFalse);' in fb
iop = body(acc, 'static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe)\n{')
assert 'l10r && gIopFlip30 && fb->getProperty("NVRMFBFlipL10R") == kOSBooleanTrue' in iop
assert iop.index('f.flags |= NVRM_FLIP_FLAG_L10R') < iop.index('nvAccelIopAsyncStart('), 'the async flip goes out without the flag'
assert 'sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip30);' in acc
assert filecmp.cmp(root / 'kexts/NVRMFB/nvrm_gpuva_abi.h', root / 'kexts/NVRM/nvrm_gpuva_abi.h', shallow=False), 'flip ABI differs'
fixture = r'''
#include <cassert>
#include <cstdio>
#include "nvrm-fb-safety.h"
// what the window reads from X2B10G10R10: red from bits 0-9, green 10-19, blue 20-29
static void readX2B10G10R10(unsigned v, int in[3]) { in[0] = v & 0x3ff; in[1] = (v >> 10) & 0x3ff; in[2] = (v >> 20) & 0x3ff; }
static void apply(const int m[3][4], const int in[3], int out[3]) {
    for (int r = 0; r < 3; r++) {
        long long acc = (long long)m[r][3];
        for (int c = 0; c < 3; c++) acc += (long long)m[r][c] * in[c];
        out[r] = (int)(acc >> 16);
    }
}
static unsigned l10r(unsigned r, unsigned g, unsigned b) { return 0xC0000000u | (r << 20) | (g << 10) | b; }   // red high
int main() {
    int m[3][4]; for (auto &row : m) for (int &x : row) x = 7;   // the function fills every coefficient
    nvrmL10rSwapMatrix(m);
    const unsigned px[][3] = { { 1023, 0, 0 }, { 0, 1023, 0 }, { 0, 0, 1023 }, { 940, 512, 64 }, { 1, 2, 3 }, { 0, 0, 0 } };
    for (auto &p : px) {
        int in[3], out[3];
        readX2B10G10R10(l10r(p[0], p[1], p[2]), in);
        apply(m, in, out);
        assert(out[0] == (int)p[0] && out[1] == (int)p[1] && out[2] == (int)p[2]);
    }
    int in[3]; readX2B10G10R10(l10r(1023, 0, 0), in);
    assert(in[0] == 0 && in[2] == 1023);                         // control: without the matrix pure red reads as pure blue
    for (int r = 0; r < 3; r++) assert(m[r][3] == 0);            // no offset
    puts("PASS: l10r scanned out as X2B10G10R10 with the FMT swap keeps every colour; without it red and blue trade places");
}
'''
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d); (d / 't.cpp').write_text(fixture)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Werror', f'-I{root}/kexts/NVRMFB/fb', str(d / 't.cpp'), '-o', str(d / 't')], check=True)
    subprocess.run([str(d / 't')], check=True)
print('PASS: 30-bit depth offered and answered, every flip names the FMT override, NVAccel flips l10r only where announced')
