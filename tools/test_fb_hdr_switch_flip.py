#!/usr/bin/env python3
"""HDR on and off on the same timing without a mode set.

macOS lists an SDR and an HDR10 copy of each timing and goes from one to the other several times while a display comes up.
NVKMS's flip path sends the HDR infoframe, but KAPI made any request that changed it a mode set wherever the display hardware
cannot tie infoframes to a flip (Blackwell GB20x), and a mode set takes the head down and brings it back. Checked here:
620-hdr-infoframe-flip.patch takes the HDR infoframe, and only it, out of that list and adds the call NVRM answers
NVRM_KAPI_CAPS_FN with; NVRMFB asks once at start, and the production hdrSwitchFits and switchHdr, compiled, go the flip
way only for two copies that leave the same signal on the cable, with a request that names nothing but the infoframe and the
output LUT, tested before it is committed, and with nothing changed when NVKMS refuses it. The controls: an NVRM that does not
answer (the full mode set stays), and a request that also flags the mode."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
fb = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
nvrm = (root / 'kexts/NVRM/NVRM.cpp').read_text()
patch = (root / 'kexts/NVRM/patches/620-hdr-infoframe-flip.patch').read_text()
abi = (root / 'kexts/NVRM/nvrm_gpuva_abi.h').read_text()
assert abi == (root / 'kexts/NVRMFB/nvrm_gpuva_abi.h').read_text(), 'the two copies of the display ABI header differ'
assert '#define NVRM_KAPI_CAPS_FN "nvKapiCaps"' in abi and '#define NVRM_KAPI_CAP_HDR_INFOFRAME_FLIP 0x1u' in abi

# The patch: the HDR infoframe leaves the list of changes that need a mode set without flip-synchronized infoframes; the
# colorimetry and the rest stay in it. NVRM links the query, so an NVKMS library built without the patch does not link.
removed = [l[1:].strip() for l in patch.splitlines() if l.startswith('-') and not l.startswith('---')]
added = [l[1:].strip() for l in patch.splitlines() if l.startswith('+') and not l.startswith('+++')]
assert removed == ['headRequestedConfig->flags.hdrInfoFrameChanged ||'], removed
ctx = patch[patch.index('if (!device->caps.supportsFlipSynchronizedInfoframes) {'):]
assert 'headRequestedConfig->flags.colorimetryChanged' in ctx[:400], 'the colorimetry no longer needs a mode set'
assert 'NvBool nvKmsKapiHdrInfoFrameFlips(void)' in added and 'return NV_TRUE;' in added
assert 'extern "C" NvBool nvKmsKapiHdrInfoFrameFlips(void);' in nvrm
caps = nvrm[nvrm.index('isEqualTo(NVRM_KAPI_CAPS_FN)'):]
caps = caps[:caps.index('\n    }\n')]
assert 'nvKmsKapiHdrInfoFrameFlips() ? NVRM_KAPI_CAP_HDR_INFOFRAME_FLIP : 0u' in caps and 'return kIOReturnSuccess;' in caps

def body(signature):
    s = fb.index(signature)
    return fb[s:fb.index('\n}\n', s) + 3]
start = body('bool NVRMNVDAFramebuffer::start(IOService *provider)')
ask = start[start.index('OSSymbol::withCString(NVRM_KAPI_CAPS_FN)'):]
assert '!= kIOReturnSuccess) caps = 0;' in ask[:400] and 'fHdrFlip = (caps & NVRM_KAPI_CAP_HDR_INFOFRAME_FLIP) != 0;' in ask[:500]
mode = body('IOReturn NVRMNVDAFramebuffer::setDisplayMode(')
fast = 'result = hdrSwitchFits(dt, m, w, h) ? switchHdr(mode) : kIOReturnUnsupportedMode;'
full = 'if (result != kIOReturnSuccess) result = switchMode(mode, m, w, h, dt.scalerFlags & kIOScaleRotateFlags);'
assert fast in mode and full in mode and mode.index(fast) < mode.index(full) < mode.index('fCurDT = dt;')

same = body('static bool nvrmSameTimings(')
fits = body('bool NVRMNVDAFramebuffer::hdrSwitchFits(')
switch = body('IOReturn NVRMNVDAFramebuffer::switchHdr(')
olut = body('static void nvrmHeadOlut(')
control = switch.replace('hr->flags.hdrInfoFrameChanged = NV_TRUE;', 'hr->flags.hdrInfoFrameChanged = NV_TRUE; hr->flags.modeChanged = NV_TRUE;')
assert control != switch
control = control.replace('::switchHdr(', '::switchHdrControl(')

fixture = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "nvrm-fb-safety.h"
typedef unsigned NvU32; typedef unsigned char NvBool; typedef unsigned short NvU16; typedef int IOReturn; typedef unsigned UInt32;
typedef int IODisplayModeID;
enum { NV_FALSE = 0, NV_TRUE = 1 };
enum { kIOReturnSuccess = 0, kIOReturnNoMemory = 1, kIOReturnNotReady = 2, kIOReturnUnsupportedMode = 3 };
enum { kIOScaleRotateFlags = 0x7, NVKMS_OLUT_FP_NORM_SCALE_DEFAULT = 7, NVKMS_LUT_ARRAY_SIZE = 1024, NV_KMS_FLIP_RESULT_SUCCESS = 0 };
static const NvU32 kNvrmColorFormatAuto = 3;
static bool gHdrOn = true, gTenBit = true, gHdr422 = false; static int gHdrBt2020 = 0, gColorBpc = 0, gColorFormat = 3;
#define FBLOG(...) ((void)0)
struct IODetailedTimingInformationV2 { UInt32 scalerFlags; };
struct NvKmsKapiDisplayModeTimings {
    NvU32 refreshRate, pixelClockHz, hVisible, hSyncStart, hSyncEnd, hTotal, hSkew, vVisible, vSyncStart, vSyncEnd, vTotal;
    struct { NvU32 interlaced : 1, doubleScan : 1, hSyncPos : 1, hSyncNeg : 1, vSyncPos : 1, vSyncNeg : 1; } flags;
    NvU32 widthMM, heightMM;
};
struct NvKmsKapiDisplayMode { NvKmsKapiDisplayModeTimings timings; char name[32]; };
struct NvKmsKapiSurface { int id; };
struct NvKmsKapiHeadModeSetConfig {
    NvBool bActive; NvU32 numDisplays; NvU32 displays[4]; NvKmsKapiDisplayMode mode; int olutFpNormScale;
    struct { NvBool enabled; NvKmsKapiSurface *lutSurface; NvU32 offset, vssSegments, lutEntries; } olut;
};
struct NvKmsKapiLayerRequestedConfig { struct { NvBool surfaceChanged, srcXYChanged, cscChanged, matrixOverridesChanged; } flags; };
struct NvKmsKapiHeadRequestedConfig {
    NvKmsKapiHeadModeSetConfig modeSetConfig;
    struct { NvBool activeChanged, displaysChanged, modeChanged, hdrInfoFrameChanged, colorimetryChanged, olutChanged; } flags;
    NvKmsKapiLayerRequestedConfig layerRequestedConfig[8];
};
struct NvKmsKapiRequestedModeSetConfig { NvU32 headsMask; NvKmsKapiHeadRequestedConfig headRequestedConfig[8]; };
struct NvKmsKapiModeSetReplyConfig { int flipResult; };
static void nvu_zero(void *p, unsigned long n) { memset(p, 0, n); }
static void *IOMalloc(unsigned long n) { return malloc(n); }
static void IOFree(void *p, unsigned long) { free(p); }
static int OSAddAtomic(int, volatile int *v) { return *v; }
struct NVRMDisplayGate { bool valid() const { return true; } };
struct NVRMHeadGate { NVRMHeadGate(int, int, bool) {} bool valid() const { return true; } };
struct Call { bool commit, cursorOnly, hdrActive; NvKmsKapiHeadRequestedConfig hr; NvU32 headsMask; };
struct NVRMNVDAFramebuffer {
    bool fHdrFlip = true, fHdrCapable = true, fHdrWant = true, fHdrActive = false, fModeSet = true, fSurActive = false,
         fHeadSlept = false, fConsoleAperture = false, fSubmissionFaulted = false, fScan30 = true, fYccWant = false, fYccActive = false,
         fGammaOn = true;
    int fSurfStore = 1; int *fSurf = &fSurfStore; int fGammaStore = 1; int *fGamma = &fGammaStore;
    NvU32 fW = 1920, fH = 1080, fCurTransform = 0, fHdmiTmdsMHz = 600, fHead = 1, fDisplay = 0x100, fIndex = 0; IODisplayModeID fCurId = 0x80002000;
    NvKmsKapiDisplayMode fMode = {};
    int fPrimaryGate = 0; volatile int fStopping = 0; int fKmsStore = 1, *fKms = &fKmsStore, *fDev = &fKmsStore;
    NvKmsKapiSurface fLut = { 7 }; unsigned olutCalls = 0, published = 0, calls = 0; int refuseAt = -1; Call log[4];
    int startLock() { return 0; }
    bool rmGpuReady() const { return true; }
    NvKmsKapiSurface *olutFromGamma() { olutCalls++; return &fLut; }
    void publishStartupTiming() { published++; }
    NvBool submitConfig(NvKmsKapiRequestedModeSetConfig *cfg, NvKmsKapiModeSetReplyConfig *rep, NvBool commit, bool, bool cursorOnly) {
        Call &c = log[calls]; c.commit = commit; c.cursorOnly = cursorOnly; c.hdrActive = fHdrActive; c.hr = cfg->headRequestedConfig[fHead];
        c.headsMask = cfg->headsMask; rep->flipResult = 0;
        return (int)calls++ == refuseAt ? NV_FALSE : NV_TRUE;
    }
    bool hdrSwitchFits(const IODetailedTimingInformationV2 &d, const NvKmsKapiDisplayMode &m, NvU32 w, NvU32 h) const;
    IOReturn switchHdr(IODisplayModeID id);
    IOReturn switchHdrControl(IODisplayModeID id);
};
'''
checks = r'''
// a request that changes only the HDR infoframe and the output LUT, on the head, and names no layer
static bool headOnly(const Call &c) {
    const NvKmsKapiHeadRequestedConfig &h = c.hr;
    if (h.flags.modeChanged || h.flags.activeChanged || h.flags.displaysChanged || h.flags.colorimetryChanged) return false;
    for (int l = 0; l < 8; l++) {
        const auto &f = h.layerRequestedConfig[l].flags;
        if (f.surfaceChanged || f.srcXYChanged || f.cscChanged || f.matrixOverridesChanged) return false;
    }
    return h.flags.hdrInfoFrameChanged && h.flags.olutChanged && c.cursorOnly && h.modeSetConfig.bActive && h.modeSetConfig.numDisplays == 1;
}
static NvKmsKapiDisplayMode lg144() {   // 1920x1080 at 144 Hz, 339.95 MHz: an HDMI monitor's own timing
    NvKmsKapiDisplayMode m = {};
    m.timings = { 143998, 339950000, 1920, 1968, 2000, 2080, 0, 1080, 1083, 1088, 1135, {0, 0, 1, 0, 0, 1}, 0, 0 };
    return m;
}
int main() {
    const IODetailedTimingInformationV2 d = { 0 };
    const NvKmsKapiDisplayMode m = lg144();
    auto fresh = [&](bool hdrOn) { NVRMNVDAFramebuffer f; f.fMode = m; f.fHdrActive = hdrOn; f.fHdrWant = !hdrOn; return f; };
    { NVRMNVDAFramebuffer f = fresh(false); assert(f.hdrSwitchFits(d, m, 1920, 1080)); }          // SDR -> HDR10
    { NVRMNVDAFramebuffer f = fresh(true);  assert(f.hdrSwitchFits(d, m, 1920, 1080)); }          // HDR10 -> SDR
    { NVRMNVDAFramebuffer f = fresh(false); f.fHdrFlip = false; assert(!f.hdrSwitchFits(d, m, 1920, 1080)); }   // control: an NVRM that does not answer
    { NVRMNVDAFramebuffer f = fresh(false); f.fHdrWant = false; assert(!f.hdrSwitchFits(d, m, 1920, 1080)); }   // the same range: not this path
    { NVRMNVDAFramebuffer f = fresh(false); NvKmsKapiDisplayMode o = m; o.timings.hTotal = 2200; assert(!f.hdrSwitchFits(d, o, 1920, 1080)); }
    { NVRMNVDAFramebuffer f = fresh(false); assert(!f.hdrSwitchFits(d, m, 2560, 1440)); }         // another desktop
    { NVRMNVDAFramebuffer f = fresh(false); IODetailedTimingInformationV2 r = { 0x1 }; assert(!f.hdrSwitchFits(r, m, 1920, 1080)); }   // rotated
    { NVRMNVDAFramebuffer f = fresh(false); NvKmsKapiDisplayMode o = m; f.fMode.timings.pixelClockHz = o.timings.pixelClockHz = 148500000;
      assert(!f.hdrSwitchFits(d, o, 1920, 1080)); }                                               // under 324 MHz HDR10 goes at 10 bits
    { NVRMNVDAFramebuffer f = fresh(false); f.fHdmiTmdsMHz = 0; assert(!f.hdrSwitchFits(d, m, 1920, 1080)); }   // not HDMI
    { NVRMNVDAFramebuffer f = fresh(false); f.fScan30 = false; gTenBit = false; assert(!f.hdrSwitchFits(d, m, 1920, 1080)); gTenBit = true; }   // 8-bit SDR scanout
    { NVRMNVDAFramebuffer f = fresh(false); f.fYccWant = true; assert(!f.hdrSwitchFits(d, m, 1920, 1080)); }
    { NVRMNVDAFramebuffer f = fresh(false); gHdrBt2020 = 1; assert(!f.hdrSwitchFits(d, m, 1920, 1080)); gHdrBt2020 = 0; }
    { NVRMNVDAFramebuffer f = fresh(false); gColorBpc = 8; assert(!f.hdrSwitchFits(d, m, 1920, 1080)); gColorBpc = 0; }
    { NVRMNVDAFramebuffer f = fresh(false); gColorFormat = 0; assert(!f.hdrSwitchFits(d, m, 1920, 1080)); gColorFormat = 3; }
    { NVRMNVDAFramebuffer f = fresh(false); f.fHeadSlept = true; assert(!f.hdrSwitchFits(d, m, 1920, 1080)); }
    { NVRMNVDAFramebuffer f = fresh(false); f.fSurActive = true; assert(!f.hdrSwitchFits(d, m, 1920, 1080)); }

    {   // HDR on: test, then commit, of the infoframe with the LUT off; then the mode is the new one
        NVRMNVDAFramebuffer f = fresh(false);
        assert(f.switchHdr(0x80002001) == kIOReturnSuccess && f.calls == 2 && !f.log[0].commit && f.log[1].commit);
        assert(headOnly(f.log[0]) && headOnly(f.log[1]) && f.log[1].hdrActive && f.log[1].headsMask == 1u << f.fHead);
        assert(!f.log[1].hr.modeSetConfig.olut.enabled && f.olutCalls == 0 && f.fHdrActive && f.fCurId == 0x80002001 && f.published == 1);
        assert(f.log[1].hr.modeSetConfig.mode.timings.pixelClockHz == 339950000);
    }
    {   // HDR off: macOS's gamma table goes back on, built once for both
        NVRMNVDAFramebuffer f = fresh(true);
        assert(f.switchHdr(0x80002000) == kIOReturnSuccess && f.calls == 2 && headOnly(f.log[1]) && !f.log[1].hdrActive);
        assert(f.log[1].hr.modeSetConfig.olut.enabled && f.log[1].hr.modeSetConfig.olut.lutSurface == &f.fLut && f.olutCalls == 1);
        assert(!f.fHdrActive && f.fCurId == 0x80002000);
    }
    for (int at = 0; at < 2; at++) {   // refused at the test or at the commit: nothing changed, the caller does the mode set
        NVRMNVDAFramebuffer f = fresh(false); f.refuseAt = at;
        assert(f.switchHdr(0x80002001) == kIOReturnUnsupportedMode && f.calls == (unsigned)at + 1);
        assert(!f.fHdrActive && f.fCurId == 0x80002000 && f.published == 0);
    }
    {   // control: a request that also flags the mode is not head-only
        NVRMNVDAFramebuffer f = fresh(false);
        assert(f.switchHdrControl(0x80002001) == kIOReturnSuccess && !headOnly(f.log[1]));
    }
    puts("PASS: HDR on and off on the same timing in a flip, only where the cable carries the same signal; refused: nothing changed");
}
'''
src = fixture + olut + same + fits + switch + control + checks
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d); (d / 't.cpp').write_text(src)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Werror', '-Wno-unused-function', f'-I{root}/kexts/NVRMFB/fb', str(d / 't.cpp'), '-o', str(d / 't')], check=True)
    subprocess.run([str(d / 't')], check=True)
print('PASS: KAPI sends the HDR infoframe alone in a flip; NVRM says so; NVRMFB asks, and keeps the full mode set otherwise')
