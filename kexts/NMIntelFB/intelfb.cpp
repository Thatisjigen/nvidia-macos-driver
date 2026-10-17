// NMIntelFB: framebuffer for the built-in panel of laptops whose panel is wired to an Intel GPU macOS has no driver for
// (display versions 12-20: Tiger Lake, Rocket Lake, Alder Lake, Raptor Lake, Meteor Lake, Arrow Lake, Lunar Lake).
//
// Phase 1 takes over the mode the firmware already lit: it finds the pipe whose primary plane is scanning out, checks
// that the plane is linear 32-bit XRGB, and publishes that surface (through the graphics aperture, BAR2 + the plane's GGTT
// offset) as an IOFramebuffer with the panel's real size and refresh. Nothing is written to the display engine, so a
// laptop it does not understand keeps exactly the screen it had: start() fails, every reason is logged, and the firmware
// framebuffer (IONDRVFramebuffer) takes the device as before.
//
// Register layout: Linux i915/xe display headers (MIT). Plane 1 of pipe P: PLANE_CTL 0x70180 + P*0x1000, STRIDE +0x08,
// SIZE +0x10, SURF +0x1C. Transcoder T timings at 0x60000 + T*0x1000, TRANSCONF at 0x70008 + P*0x1000, PIPESRC 0x6001C.

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/OSAtomic.h>
#include "nvrm_vram_abi.h"
// intelfb-0011: NVKMS's kernel API (open-gpu-kernel-modules 610.57.04 headers, fetched by tools/intelfb/build.sh) and
// the RM-handles call NVAccel makes to its primary framebuffer (same ABI as kexts/NVRM/nvrm_gpuva_abi.h)
extern "C" {
#include "nvkms-kapi.h"
}
#include "AppleGraphicsDeviceControl_re.h"   // nullmoth's reconstructed header (kexts/NVRMAGDC), intelfb-0014
#include "nvrm-agdc-k5.h"                     // nullmoth's AGDC link/plane-scaler answers (kexts/NVRMAGDC), intelfb-0015
#define NVRM_GPUVA_ABI_VERSION 1u
#define NVRM_GPUVA_FN_HANDLES  "nvRmHandles"
struct NVRMRmHandles { unsigned int version; void *kapiDevice; unsigned int hClient, hDevice, hSubDevice; };
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread_call.h>
#include <kern/sched_prim.h>
#include <sys/sysctl.h>
#include <libkern/version.h>

#define IFBLOG(fmt, ...) IOLog("NMIntelFB: " fmt "\n", ##__VA_ARGS__)

// nmx-intel: a trail of the steps in NVRAM ("nullmoth-intelfb-trail"), written and synced at each step, so a boot that
// freezes before the log is saved still shows the last step reached (`nvram nullmoth-intelfb-trail`). And a boot guard:
// "nullmoth-intelfb-attaching" is set while start() runs and removed when it returns; found at probe, the last start
// never returned, so this boot leaves the panel to the firmware framebuffer. To try again: sudo nvram -d
// nullmoth-intelfb-attaching
#include <IOKit/IONVRAM.h>
static IODTNVRAM *ifb_nvram()
{
    IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
    IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e);
    if (n == nullptr && e != nullptr) e->release();
    return n;
}
static void ifb_nv_set(const char *key, const char *val)
{
    IODTNVRAM *n = ifb_nvram();
    if (!n) return;
    const OSSymbol *k = OSSymbol::withCString(key);
    if (k) {
        if (val) { if (OSData *d = OSData::withBytes(val, (unsigned)strlen(val))) { n->setProperty(k, d); d->release(); } }
        else n->removeProperty(k);
        k->release();
    }
    n->sync();
    n->release();
}
static bool ifb_nv_has(const char *key)
{
    IODTNVRAM *n = ifb_nvram();
    if (!n) return false;
    OSObject *o = n->copyProperty(key);
    bool has = o != nullptr;
    if (o) o->release();
    n->release();
    return has;
}
static char gIfbTrail[900];   // intelfb-0004: room for the first-call tokens
static void ifb_trail(const char *step)
{
    if (strlen(gIfbTrail) + strlen(step) + 2 >= sizeof(gIfbTrail)) return;
    if (gIfbTrail[0]) strlcat(gIfbTrail, " ", sizeof(gIfbTrail));
    strlcat(gIfbTrail, step, sizeof(gIfbTrail));
    IFBLOG("trail: %s", step);
    ifb_nv_set("nullmoth-intelfb-trail", gIfbTrail);
}
static uint32_t ifb_knob(const char *key, uint32_t def)
{
    IODTNVRAM *n = ifb_nvram();
    if (!n) return def;
    uint32_t v = def;
    if (OSData *d = OSDynamicCast(OSData, n->copyProperty(key))) {
        char b[12] = {}; unsigned len = d->getLength() < 11 ? d->getLength() : 11;
        memcpy(b, d->getBytesNoCopy(), len);
        if (b[0] >= '0' && b[0] <= '9') { v = 0; for (unsigned i = 0; i < len && b[i] >= '0' && b[i] <= '9'; i++) v = v * 10 + (uint32_t)(b[i] - '0'); }
        d->release();
    }
    n->release();
    return v;
}

// kmutil places a kext in a kernel collection only with a _kmod_info symbol (Xcode generates it; this build does not)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#ifdef NMINTELFB_VIRTUAL
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMIntelFBVirtual, "0.1", _start, _stop) }
#else
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMIntelFB, "0.1", _start, _stop) }
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

// nmx-intel (intelfb-0004): the first call of each IOFramebuffer method, and the first of each attribute, in the trail,
// so a freeze after start() shows the last call macOS made into this framebuffer
// intelfb-0006: every call (not only the first) leaves its tag in gIfbLast for the heartbeat
static const char *volatile gIfbLast = "none";
static volatile SInt32 gIfbCalls, gIfbVbl;
static char gIfbLastAttr[24];
#define IFB_ONCE(tag) do { gIfbLast = tag; OSIncrementAtomic(&gIfbCalls); static bool _ifbDone; if (!_ifbDone) { _ifbDone = true; ifb_trail(tag); } } while (0)

// intelfb-0006: heartbeat every second for three minutes after start(): "nullmoth-intelfb-beat" = ms since start, VBL
// calls, framebuffer calls and the last call made. A freeze leaves the last beat: whether VBLs still ran, and the call
// intelfb-0007: a kernel thread of its own. As a thread_call it shared the HIGH group (one thread) with the VBL timer
// and stopped when the VBL callback blocked (RTX-5: ms=21450 vbl=2). At two minutes it also disarms the boot guard.
static uint64_t gIfbT0;
static void ifb_beat(void *, wait_result_t)
{
    for (;;) {
        IOSleep(1000);
        uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - gIfbT0, &ns);
        char b[96];
        snprintf(b, sizeof b, "ms=%llu vbl=%d calls=%d last=%s", ns / 1000000ull, (int)gIfbVbl, (int)gIfbCalls, (const char *)gIfbLast);
        ifb_nv_set("nullmoth-intelfb-beat", b);
        static bool alive;
        if (!alive && ns >= 120ull * 1000000000ull) {
            alive = true;
            ifb_trail("alive");
            ifb_nv_set("nullmoth-intelfb-attaching", nullptr);
        }
        if (ns >= 180ull * 1000000000ull) break;
    }
    thread_terminate(current_thread());
}
static void ifb_attr(const char *pfx, uint32_t a)
{
    OSIncrementAtomic(&gIfbCalls);
    snprintf(gIfbLastAttr, sizeof gIfbLastAttr, "%s:%x", pfx, a);
    gIfbLast = gIfbLastAttr;
    static uint32_t seen[40]; static unsigned n;
    uint32_t key = a ^ ((uint32_t)(uint8_t)pfx[0] << 1) ^ ((uint32_t)(uint8_t)pfx[1] << 9);
    for (unsigned i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 40) seen[n++] = key; else return;
    char c[5] = { (char)(a >> 24), (char)(a >> 16), (char)(a >> 8), (char)a, 0 }, t[24];
    bool printable = true;
    for (int i = 0; i < 4; i++) if (c[i] < 0x20 || c[i] > 0x7e || c[i] == ' ') printable = false;
    if (printable) snprintf(t, sizeof t, "%s:%s", pfx, c); else snprintf(t, sizeof t, "%s:%x", pfx, a);
    ifb_trail(t);
}

namespace {
// display version by PCI device ID, from Linux include/drm/intel/pciids.h (MIT); 0 = not ours
struct Id { uint16_t id; uint8_t ver; };
const Id kIds[] = {
#include "display_ids.inc"
};

uint8_t displayVersion(uint16_t dev)
{
    for (const Id &e : kIds) if (e.id == dev) return e.ver;
    return 0;
}

constexpr uint32_t PLANE_CTL(int p)    { return 0x70180u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_STRIDE(int p) { return 0x70188u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_SIZE(int p)   { return 0x70190u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_SURF(int p)   { return 0x7019Cu + (uint32_t)p * 0x1000u; }
constexpr uint32_t TRANSCONF(int p)    { return 0x70008u + (uint32_t)p * 0x1000u; }
constexpr uint32_t HTOTAL(int t)       { return 0x60000u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VTOTAL(int t)       { return 0x6000Cu + (uint32_t)t * 0x1000u; }
constexpr uint32_t HBLANK(int t)       { return 0x60004u + (uint32_t)t * 0x1000u; }
constexpr uint32_t HSYNC(int t)        { return 0x60008u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VBLANK(int t)       { return 0x60010u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VSYNC(int t)        { return 0x60014u + (uint32_t)t * 0x1000u; }
// one count per vblank (Linux PIPE_FRMCOUNT_G4X, 0x70040 + pipe): counting it is how the real refresh is known without
// reading the PLL and M/N values back
constexpr uint32_t FRMCOUNT(int p)     { return 0x70040u + (uint32_t)p * 0x1000u; }

// PLANE_CTL (display 11+): enable bit 31, format bits 27:23 (8 = XRGB 8:8:8:8, Linux PLANE_CTL_FORMAT_XRGB_8888 on
// ICL+), tiling bits 12:10 (0 = linear)
constexpr uint32_t kEnable = 1u << 31;
// the two power states NVRMFB registers: off, and on + usable
IOPMPowerState kPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};
constexpr uint32_t fmtOf(uint32_t ctl)  { return (ctl >> 23) & 0x1F; }
constexpr uint32_t tileOf(uint32_t ctl) { return (ctl >> 10) & 0x7; }
}

class NMIntelFB : public IOFramebuffer {
    OSDeclareDefaultStructors(NMIntelFB)
    IOPCIDevice *fPCI = nullptr;
    IOMemoryMap *fMMIO = nullptr;
    IOMemoryMap *fSurfMap = nullptr;   // write-combined CPU view of the scan-out surface (NVIDIA frames land here)
    // WindowServer paces an accelerated display on its VBL interrupt: the NVIDIA display pipe completes each frame on it.
    // Without one the first frame never completed and WindowServer stalled every display (in testing, the
    // virtual panel froze the Mac). A 60 Hz timer is the VBL, as NVRMFB's fallback is.
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; };
    Irq fVbl, fConnect;
    // intelfb-0007: the VBL runs on a kernel thread of its own, and only while IOFramebuffer has the VBL interrupt
    // enabled (setInterruptState). As a thread_call it ran in the HIGH group, which has one thread: the second VBL
    // callback blocked there (called before open() enabled it) and every HIGH thread_call in the kernel stopped behind
    // it (RTX-5: vbl=2, the heartbeat stopped at 21 s, then the Mac froze)
    uint64_t fVblNext = 0, fVblCalls = 0;
    volatile bool fStopping = false, fVblOn = false, fVblRunning = false;
    bool fOwnGroup = false;   // intelfb-0009: own IOFBDependentID, index 4 for NVAccel until open(), 0 for IOGraphics after
    // intelfb-0012: the VBL fires whenever it is registered, as NVRMFB's does (its setInterruptState is a no-op).
    // IOGraphics treats a registered VBL as enabled and never switched it on, and NVAccel's registration switched it
    // off (RTX-10 trail "is vbl-off"): vbl=0 in every test, and the accelerated display pipe got no vsync.
    bool fVblAlways = true;
    // intelfb-0011: as head 0 this panel is NVAccel's primary framebuffer, which NVAccel asks for RM handles and VRAM
    // (NVRMFB answers on a machine with an NVIDIA display; here no NVRMFB starts). An NVKMS device of our own answers.
    struct NvKmsKapiFunctionsTable *fKms = nullptr;
    struct NvKmsKapiDevice *fDev = nullptr;
    NvU64 (*fPhysForVa)(const void *) = nullptr;
    uint64_t fVramBudget = 0, fVramBytes = 0, fVramCookie = 0;
    unsigned fVramLogged = 0;
    IOLock *fVramLock = nullptr;
    NVRMVramRequest fScanRes = {};   // intelfb-0016: the VRAM block that stands for this panel's scanout
    void kapiInit();
    void kapiFree();
    IOReturn vramGrant(NVRMVramRequest *r);
    IOReturn vramRelease(NVRMVramRequest *r);
    static void vblThread(void *p0, wait_result_t) {
        NMIntelFB *me = (NMIntelFB *)p0;
        uint64_t period; nanoseconds_to_absolutetime(me->fFrameNs, &period);
        uint64_t next = mach_absolute_time() + period;
        while (!me->fStopping) {
            assert_wait_deadline((event_t)&me->fVblNext, THREAD_UNINT, next);
            thread_block(THREAD_CONTINUE_NULL);
            if (me->fStopping) break;
            if (me->fVblOn && me->fVbl.proc) {
                OSIncrementAtomic(&gIfbVbl);
                static bool _v1; if (!_v1) { _v1 = true; ifb_trail("vbl1"); }
                me->fVbl.proc(me->fVbl.target, me->fVbl.ref); me->fVblCalls++;
            }
            uint64_t now = mach_absolute_time();
            next += period; if (next <= now) next = now + period;
        }
        me->fVblRunning = false;
        thread_terminate(current_thread());
    }
    void armVbl() {
        thread_t t = nullptr;
        fVblRunning = true;
        if (kernel_thread_start(vblThread, this, &t) == KERN_SUCCESS) thread_deallocate(t); else fVblRunning = false;
        IFBLOG("vbl thread %s", fVblRunning ? "started (runs while the VBL interrupt is enabled)" : "FAILED");
    }
#ifdef NMINTELFB_VIRTUAL
    IOBufferMemoryDescriptor *fVBuf = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    unsigned fSamples = 0;
    // 64 pixels across the buffer: how many are non-black and how many differ from the first, published in the registry
public:
    // 64 pixels across the buffer, read on demand through sysctl debug.nmintelfb_sample (the registry property the
    // timer set never showed up)
    void sampleInto(char *b, size_t n) {
        if (!fSurfMap) { snprintf(b, n, "no surface"); return; }
        const volatile uint32_t *px = (const volatile uint32_t *)fSurfMap->getVirtualAddress();
        unsigned nz = 0, varied = 0; uint32_t first = px[0] & 0xFFFFFF;
        for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
            uint32_t v = px[(fHeight * (2 * i + 1) / 16) * (fRowBytes / 4) + fWidth * (2 * j + 1) / 16] & 0xFFFFFF;
            if (v) nz++; if (v != first) varied++; }
        snprintf(b, n, "sample %u: nonzero %u/64 varied %u/64 first 0x%06x", ++fSamples, nz, varied, first);
    }
    void sample() { char b[96]; sampleInto(b, sizeof b); setProperty("NMIntelFBSample", b); }
private:
#endif   // write-combined CPU view of the scan-out surface (NVIDIA frames land here)
    volatile uint8_t *fRegs = nullptr;
    IOPhysicalAddress fApertureBase = 0;
    IOPhysicalLength fApertureLen = 0;
    uint32_t fWidth = 0, fHeight = 0, fRowBytes = 0, fSurf = 0, fRefresh16 = 60 << 16;
    uint64_t fFrameNs = 16666667ull;   // vblank period; measured from the pipe's frame counter on real hardware
    // transcoder timing (active, total, sync start/end) for getTimingInfoForDisplayMode
    uint32_t fHT = 0, fVT = 0, fHSS = 0, fHSE = 0, fVSS = 0, fVSE = 0;
    uint64_t fPclk = 0;
    int fPipe = -1;
    uint8_t fVer = 0;

    uint32_t rd(uint32_t off) const { return *(volatile uint32_t *)(fRegs + off); }
    bool adoptFirmwareMode();

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    bool startReal(IOService *provider);
    void stop(IOService *provider) override;

    IOReturn enableController() override { IFB_ONCE("ec"); return kIOReturnSuccess; }
    // intelfb-0009: IOGraphics reads IOFBDependentIndex only in open() (controller->fFbs[index] = this) and walks fFbs[]
    // from 0 up to the first NULL; kIOFBWSStartAttribute dereferences fFbs[0]. Alone in its group at index 4, fFbs[0] is
    // NULL (the RTX-2..7 freezes). NVAccel took its head number from the same property at publish time, which is over by
    // now, so the panel becomes index 0 of its own controller here.
    IOReturn open() override {
        if (fOwnGroup) { setProperty("IOFBDependentIndex", 0ull, 32); IFB_ONCE("open-idx0"); }
        return IOFramebuffer::open();
    }
    bool isConsoleDevice() override { IFB_ONCE("con"); return true; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    IOItemCount getDisplayModeCount() override { IFB_ONCE("dmc"); return 1; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override { IFB_ONCE("dms"); modes[0] = 1; return kIOReturnSuccess; }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { IFB_ONCE("cdm"); *mode = 1; *depth = 0; return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { IFB_ONCE("sdm"); return getCurrentDisplayMode(mode, depth); }
    // WindowServer showed the panel at 0.00 Hz without this (virtual panel)
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        IFB_ONCE("tim");
        if (mode != 1 || !info) return kIOReturnUnsupportedMode;
        bzero(info, sizeof(*info));
        info->appleTimingID = kIOTimingIDInvalid;
        if (!fPclk || fHT <= fWidth || fVT <= fHeight) return kIOReturnSuccess;
        IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
        d.pixelClock = d.minPixelClock = d.maxPixelClock = fPclk;
        d.horizontalActive = fWidth; d.horizontalBlanking = fHT - fWidth;
        d.horizontalSyncOffset = fHSS > fWidth ? fHSS - fWidth : 0; d.horizontalSyncPulseWidth = fHSE > fHSS ? fHSE - fHSS : 0;
        d.verticalActive = fHeight; d.verticalBlanking = fVT - fHeight;
        d.verticalSyncOffset = fVSS > fHeight ? fVSS - fHeight : 0; d.verticalSyncPulseWidth = fVSE > fVSS ? fVSE - fVSS : 0;
        info->flags = kIODetailedTimingValid;
        return kIOReturnSuccess;
    }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override { IFB_ONCE("setdm"); return (mode == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported; }
    IOItemCount getConnectionCount() override { IFB_ONCE("cc"); return 1; }
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override;
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
        ifb_attr("ri", (uint32_t)type);
        if (type == kIOFBVBLInterruptType) { fVbl = { proc, target, ref }; __atomic_thread_fence(__ATOMIC_RELEASE); fVblOn = fVblOn || fVblAlways; if (iref) *iref = &fVbl; return kIOReturnSuccess; }
        if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    }
    IOReturn unregisterInterrupt(void *) override { return kIOReturnSuccess; }
    IOReturn setInterruptState(void *ref, UInt32 state) override {
        IFB_ONCE("is");
        if (ref == &fVbl) { fVblOn = fVblAlways || state != 0; static int n; if (n++ < 4) ifb_trail(state ? "vbl-on" : "vbl-off"); }
        return kIOReturnSuccess;
    }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { IFB_ONCE("gam"); return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { IFB_ONCE("gam2"); return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { IFB_ONCE("clut"); return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { IFB_ONCE("ape"); return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    // Power and connection handling copied from NVRMFB, which shares this controller (same IOFBDependentID). Left to
    // IOFramebuffer's base class, a power change queues on the shared controller thread and waits for an acknowledge
    // (IOFramebuffer::setPowerState returns a 45 s timeout, checkPowerWork -> setAttribute(kIOPowerStateAttribute));
    // NVRMFB acknowledges at once and answers the power attributes itself. With NMIntelFB under NVRM the
    // first WindowServer restart froze every display.
    IOReturn setPowerState(unsigned long ordinal, IOService *) override {
        ifb_trail(ordinal ? "sp1" : "sp0");
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        ifb_trail("sp-done");
        return kIOPMAckImplied;
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        ifb_attr("sa", (uint32_t)attribute);
        if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
        if (attribute == kIOPowerAttribute) {
            if (value) { IOReturn r = IOFramebuffer::setAttribute(attribute, value); handleEvent(kIOFBNotifyDidPowerOn);
                         return r == kIOReturnUnsupported ? kIOReturnSuccess : r; }
            handleEvent(kIOFBNotifyWillPowerOff);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }
    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
        ifb_attr("sc", (uint32_t)attribute);
        switch (attribute) {
        case kConnectionPower: case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
        case kConnectionDisplayFlags: case kConnectionFlags:
            return kIOReturnSuccess;
        default: return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }
    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override {
        IFB_ONCE("cf");
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }
    IODeviceMemory *getVRAMRange() override { IFB_ONCE("vram"); return getApertureRange(kIOFBSystemAperture); }
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
        ifb_attr("ga", (uint32_t)attribute);
        if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
        return IOFramebuffer::getAttribute(attribute, value);
    }
    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override;
};

OSDefineMetaClassAndStructors(NMIntelFB, IOFramebuffer)
#ifdef NMINTELFB_VIRTUAL
static NMIntelFB *gVirt;
static int nmintelfb_sample_sysctl SYSCTL_HANDLER_ARGS
{
    char b[128] = "no panel";
    if (gVirt) gVirt->sampleInto(b, sizeof b);
    return sysctl_handle_string(oidp, b, sizeof b, req);
}
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_sample, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
            nmintelfb_sample_sysctl, "A", "64 pixels of the virtual panel: non-black and varied counts");
#endif

// intelfb-0013: the Intel GPU whose panel this driver shows, kept by NMIntelFBClaim when the framebuffer sits under NVRM
static IOPCIDevice *gIfbPCI = nullptr;
static IOService *gIfbNubFB = nullptr;   // intelfb-0014: the framebuffer on the nub, which the AGDC reports
static uint32_t gIfbK5[NVK5_TIMING_WORDS];   // intelfb-0015: the panel's timing in NVRMFB's NVRMTiming layout
static void ifb_agdc_register(bool on);
static bool ifb_pci_ok(IOPCIDevice *pci, uint8_t *ver);

IOService *NMIntelFB::probe(IOService *provider, SInt32 *score)
{
    if (version_major != 24 && version_major != 25) return nullptr;   /* the 2.x line supports macOS 15 (Darwin 24) and macOS 26 (Darwin 25) only */
#ifdef NMINTELFB_VIRTUAL
    if (!provider->getProperty("nm-intel-panel")) return nullptr;   // only the nub the loader published
    // test build: a virtual 1920x1080 "panel" in system memory, so the NVIDIA -> Intel frame path can be proven on a
    // machine with no Intel GPU (frames composited on the NVIDIA card must arrive in this buffer)
    fVer = 0xFF; return IOFramebuffer::probe(provider, score);
#endif
    // intelfb-0013: the Intel-panel nub NMIntelFBClaim publishes under NVRM (the PCI checks ran in the claim's probe)
    if (provider->getProperty("nm-intel-panel") && gIfbPCI) {
        fVer = displayVersion(gIfbPCI->configRead16(kIOPCIConfigDeviceID));
        return fVer ? IOFramebuffer::probe(provider, score) : nullptr;
    }
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;
    return ifb_pci_ok(pci, &fVer) ? IOFramebuffer::probe(provider, score) : nullptr;
}

// The checks that decide whether this driver takes the Intel panel at all (NMIntelFB's own probe before intelfb-0013)
static bool ifb_pci_ok(IOPCIDevice *pci, uint8_t *ver)
{
    uint8_t &fVer = *ver;
    if (PE_parse_boot_argn("-nmintelfboff", nullptr, 0)) { IFBLOG("off by boot-arg"); return false; }
    // nmx-intel: "Start once without NVIDIA driver" (-nvoff) also leaves the panel to the firmware framebuffer, so a
    // hang never needs the Recovery; and with the AMD/Intel test driver on (nmxintel=1) i915 owns this GPU's display
    if (PE_parse_boot_argn("-nvoff", nullptr, 0)) { IFBLOG("off: -nvoff"); return false; }
    { uint32_t nmx = 0; if (PE_parse_boot_argn("nmxintel", &nmx, sizeof nmx) && nmx) { IFBLOG("off: nmxintel=1 (NMXGPU drives this GPU)"); return false; } }
    uint16_t dev = pci->configRead16(kIOPCIConfigDeviceID);
    fVer = displayVersion(dev);
    if (!fVer) return false;   // an Intel GPU this driver does not know (older ones have Apple's own driver)
    if (ifb_nv_has("nullmoth-intelfb-attaching")) {
        IFBLOG("the last start never returned (boot guard) - leaving the firmware screen; sudo nvram -d nullmoth-intelfb-attaching");
        ifb_nv_set("nullmoth-intelfb-trail", "guard: the last start never returned");
        return false;
    }
    if (ifb_knob("nullmoth-intelfb", 1) == 0) { IFBLOG("off: nullmoth-intelfb=0"); return false; }
    IFBLOG("8086:%04x display version %u", dev, fVer);
    return true;
}

bool NMIntelFB::adoptFirmwareMode()
{
    // the panel is on whichever pipe the firmware used; take the first pipe whose transcoder and plane 1 are both on
    for (int p = 0; p < 4; p++) {
        uint32_t conf = rd(TRANSCONF(p)), ctl = rd(PLANE_CTL(p));
        if (!(conf & kEnable) || !(ctl & kEnable)) continue;
        if (fmtOf(ctl) != 8) { IFBLOG("pipe %c: plane format %u is not XRGB8888 - leaving the firmware screen", 'A' + p, fmtOf(ctl)); return false; }
        if (tileOf(ctl) != 0) { IFBLOG("pipe %c: plane is tiled (%u) - leaving the firmware screen", 'A' + p, tileOf(ctl)); return false; }
        uint32_t size = rd(PLANE_SIZE(p)), stride = rd(PLANE_STRIDE(p)) & 0x7FF;
        fWidth = (size & 0xFFFF) + 1; fHeight = ((size >> 16) & 0xFFFF) + 1;
        fRowBytes = stride * 64;                     // linear planes count the stride in 64-byte units
        fSurf = rd(PLANE_SURF(p)) & ~0xFFFu;         // GGTT offset of the surface, 4 KiB aligned
        if (fWidth < 320 || fHeight < 200 || fRowBytes < fWidth * 4) {
            IFBLOG("pipe %c: plane %ux%u stride %u is not a usable surface", 'A' + p, fWidth, fHeight, fRowBytes); return false;
        }
        if ((uint64_t)fSurf + (uint64_t)fRowBytes * fHeight > fApertureLen) {
            IFBLOG("pipe %c: surface at 0x%x is past the %llu MiB aperture", 'A' + p, fSurf, (unsigned long long)(fApertureLen >> 20)); return false;
        }
        // the transcoder that feeds pipe p is p on these display versions (eDP on transcoder A in every recording)
        fHT = (rd(HTOTAL(p)) >> 16) + 1; fVT = (rd(VTOTAL(p)) >> 16) + 1;
        fHSS = (rd(HSYNC(p)) & 0xFFFF) + 1; fHSE = (rd(HSYNC(p)) >> 16) + 1;
        fVSS = (rd(VSYNC(p)) & 0xFFFF) + 1; fVSE = (rd(VSYNC(p)) >> 16) + 1;
        // refresh: count vblanks for 250 ms (a 30-240 Hz panel gives 7-60 counts); fall back to 60 Hz if it does not move
        uint32_t f0 = rd(FRMCOUNT(p)); uint64_t t0 = mach_absolute_time();
        IOSleep(250);
        uint32_t f1 = rd(FRMCOUNT(p)); uint64_t t1 = mach_absolute_time(), ns = 0;
        absolutetime_to_nanoseconds(t1 - t0, &ns);
        uint32_t frames = f1 - f0;
        if (frames >= 5 && frames <= 100 && ns) {
            fFrameNs = ns / frames;
            uint64_t mhz = (uint64_t)frames * 1000000000000ull / ns;   // refresh in milli-Hz
            fRefresh16 = (uint32_t)((mhz << 16) / 1000);
            fPclk = (uint64_t)fHT * fVT * mhz / 1000;
        } else fPclk = (uint64_t)fHT * fVT * 60;
        fPipe = p;
        IFBLOG("pipe %c: %ux%u stride %u surface 0x%x totals %ux%u, %u vblanks in %llu ms -> %u.%02u Hz, pclk %llu", 'A' + p,
               fWidth, fHeight, fRowBytes, fSurf, fHT, fVT, frames, (unsigned long long)(ns / 1000000),
               fRefresh16 >> 16, ((fRefresh16 & 0xFFFF) * 100) >> 16, (unsigned long long)fPclk);
        return true;
    }
    IFBLOG("no pipe is scanning out - the firmware left the panel off; leaving it to the firmware framebuffer");
    return false;
}

bool NMIntelFB::start(IOService *provider)
{
#ifdef NMINTELFB_VIRTUAL
    fWidth = 1920; fHeight = 1080; fRowBytes = 7680; fSurf = 0; fPipe = 0;
    fHT = 2200; fVT = 1125; fHSS = 2008; fHSE = 2052; fVSS = 1084; fVSE = 1089; fPclk = 148500000ull;   // CEA 1080p60
    fVBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                             (mach_vm_size_t)fRowBytes * fHeight, 0xFFFFFFFFFFFFF000ull);
    if (!fVBuf || fVBuf->prepare() != kIOReturnSuccess) { IFBLOG("virtual: no contiguous buffer"); OSSafeReleaseNULL(fVBuf); return false; }
    bzero(fVBuf->getBytesNoCopy(), fVBuf->getLength());
    fApertureBase = fVBuf->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone); fApertureLen = fVBuf->getLength();
    if (!IOFramebuffer::start(provider)) { fVBuf->complete(); OSSafeReleaseNULL(fVBuf); return false; }
    fSurfMap = fVBuf->map();
    setProperty("IOFBDependentID", 0x4E56524D00000001ull, 64);
    setProperty("IOFBDependentIndex", 4ull, 32);
    if (IOWorkLoop *wl = getWorkLoop()) {
        fTimer = IOTimerEventSource::timerEventSource(this, [](OSObject *o, IOTimerEventSource *t) {
            ((NMIntelFB *)o)->sample(); t->setTimeoutMS(2000); });
        if (fTimer && wl->addEventSource(fTimer) == kIOReturnSuccess) fTimer->setTimeoutMS(2000);
    }
    armVbl();
    registerPowerDriver(this, kPowerStates, 2);
    gVirt = this; sysctl_register_oid(&sysctl__debug_nmintelfb_sample);
    IFBLOG("virtual panel published (1920x1080 at phys 0x%llx)", (unsigned long long)fApertureBase);
    return true;
#endif
    gIfbTrail[0] = 0;
    ifb_nv_set("nullmoth-intelfb-attaching", "1");
    ifb_trail("start");
    // intelfb-0008: nullmoth-intelfb-delay=<s> waits before taking the panel, so the boot log reaches the disk and the
    // NVIDIA stack (NVRM auto-go, NVAccel) has started before this framebuffer exists. The firmware screen stays meanwhile
    if (uint32_t w = ifb_knob("nullmoth-intelfb-delay", 0)) {
        if (w > 120) w = 120;
        IFBLOG("waiting %u s before taking the panel (nullmoth-intelfb-delay)", w);
        ifb_trail("wait");
        IOSleep(w * 1000);
        ifb_trail("go");
    }
    bool ok = startReal(provider);
    ifb_trail(ok ? "ok" : "declined");
    // intelfb-0005: RTX-3 froze after start() returned, so the boot guard stays armed until the Mac has lived two
    // minutes with this framebuffer; a freeze before that leaves the panel to the firmware on the next boot
    if (ok) {
        gIfbT0 = mach_absolute_time();
        thread_t bt = nullptr;
        if (kernel_thread_start(ifb_beat, nullptr, &bt) == KERN_SUCCESS) thread_deallocate(bt);
        else ifb_nv_set("nullmoth-intelfb-attaching", nullptr);
    } else ifb_nv_set("nullmoth-intelfb-attaching", nullptr);
    return ok;
}

bool NMIntelFB::startReal(IOService *provider)
{
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) { fPCI = gIfbPCI; gIfbNubFB = this; }   // intelfb-0013: under NVRM, the Intel GPU the claim holds
    if (!fPCI) return false;
    fPCI->setMemoryEnable(true);
    fMMIO = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IODeviceMemory *ap = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!fMMIO || !ap) { IFBLOG("BAR0 or BAR2 is not mapped - leaving the firmware screen"); OSSafeReleaseNULL(fMMIO); return false; }
    fRegs = (volatile uint8_t *)fMMIO->getVirtualAddress();
    ifb_trail("mmio");
    fApertureBase = ap->getPhysicalAddress(); fApertureLen = ap->getLength();
    if (!adoptFirmwareMode()) { OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false; }
    { char t[64]; snprintf(t, sizeof t, "mode-%c-%ux%u-s%x", 'A' + fPipe, fWidth, fHeight, fSurf); ifb_trail(t); }
    // Head 4 of the NVIDIA accelerator (NVAccel): WindowServer composites this panel on the NVIDIA card and the
    // display pipe copies each frame into fSurfMap. Without NVAccel the panel stays a plain framebuffer.
    if (IODeviceMemory *sm = IODeviceMemory::withRange(fApertureBase + fSurf, (IOPhysicalLength)fRowBytes * fHeight)) {
        fSurfMap = sm->map(kIOMapWriteCombineCache); sm->release();
    }
    ifb_trail(fSurfMap ? "surf" : "surf-fail");
    // nmx-intel (RTX-2 hung in registerPowerDriver with these set after IOFramebuffer::start): the dependent ID decides
    // the controller IOFramebuffer::start builds, so it goes in before, as NVRMFB does. nullmoth-intelfb-dep=0
    // publishes a plain framebuffer, without joining NVAccel as head 4.
    // intelfb-0009: on a laptop with nothing on the NVIDIA ports no NVRMFB head ever starts (NVRM "heads 0x0"), so in
    // NVRM's group this panel sat alone at index 4 with fFbs[0] NULL. Default (dep=1): a controller of its own (see
    // open()); dep=2 keeps NVRM's group as before. Index 4 at publish is what makes NVAccel treat it as head 4.
    uint32_t dep = fSurfMap ? ifb_knob("nullmoth-intelfb-dep", 1) : 0;
    if (dep) {
        fOwnGroup = dep != 2;
        setProperty("IOFBDependentID", fOwnGroup ? 0x4E4D494E00000001ull : 0x4E56524D00000001ull, 64);
        // intelfb-0010: NVAccel makes only head 0 its primary framebuffer (fFB), and registers pipes (debug.nvaccelfb)
        // only once fFB is set; NVRMFB head 0 never starts here, so as head 4 the panel was never registered and the
        // 60 s headless check gave WindowServer up (RTX-8). In its own group the panel publishes as head 0
        // (nullmoth-intelfb-head=4: head 4, as in RTX-8).
        uint32_t head = fOwnGroup ? ifb_knob("nullmoth-intelfb-head", 0) : 4;
        setProperty("IOFBDependentIndex", head == 4 ? 4ull : 0ull, 32);
        ifb_trail(!fOwnGroup ? "dep-nvrm" : head == 4 ? "dep-own-h4" : "dep-own-h0");
    } else IFBLOG("the surface would not map write-combined, or nullmoth-intelfb-dep=0: NVIDIA rendering stays off for this panel");
    if (!IOFramebuffer::start(provider)) { ifb_trail("fb-start-failed"); OSSafeReleaseNULL(fSurfMap); OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false; }
    ifb_trail("fb-started");
    setProperty("NMIntelFBPipe", fPipe, 32);
    setProperty("NMIntelFBDisplayVersion", fVer, 32);
    setProperty("built-in", kOSBooleanTrue);   // the panel is the laptop's own (kConnectionFlags says so too)
    fVblAlways = ifb_knob("nullmoth-intelfb-vbl", 1) != 0;
    armVbl();
    ifb_trail(fVblAlways ? "vbl-always" : "vbl");
    if (fOwnGroup && ifb_knob("nullmoth-intelfb-head", 0) != 4) kapiInit();
    // nmx-intel: nullmoth-intelfb-pm=0 leaves power management out (RTX-2 hung inside this call)
    if (ifb_knob("nullmoth-intelfb-pm", 1)) {
        ifb_trail("pm");
        registerPowerDriver(this, kPowerStates, 2);
        ifb_trail("power");
    } else ifb_trail("no-pm");
    if (gIfbNubFB == this) {
        // intelfb-0015: the timing NVRMFB publishes as NVRMTiming (refresh in mHz, positive syncs as eDP panels use)
        const uint32_t k5[NVK5_TIMING_WORDS] = { NVK5_TIMING_TAG, (uint32_t)fPclk, fWidth, fHSS, fHSE, fHT, fHeight, fVSS, fVSE, fVT,
                                                 (uint32_t)(((uint64_t)fRefresh16 * 1000) >> 16), 1, 1 };
        memcpy(gIfbK5, k5, sizeof k5);
        if (OSData *d = OSData::withBytes(k5, sizeof k5)) { setProperty("NVRMTiming", d); d->release(); }
        int bad = nvk5_timing_bad(gIfbK5, sizeof gIfbK5);
        IFBLOG("K5 timing: pclk %u %ux%u h %u-%u/%u v %u-%u/%u %u mHz%s", k5[1], fWidth, fHeight, fHSS, fHSE, fHT, fVSS, fVSE, fVT,
               k5[10], bad ? " - REJECTED by nvk5_timing_bad" : "");
        ifb_agdc_register(true);
    }
    IFBLOG("built-in panel published (%ux%u)", fWidth, fHeight);
    return true;
}

void NMIntelFB::stop(IOService *provider)
{
    fStopping = true;
#ifdef NMINTELFB_VIRTUAL
    if (gVirt == this) { sysctl_unregister_oid(&sysctl__debug_nmintelfb_sample); gVirt = nullptr; }
#endif
    // intelfb-0007: wake the VBL thread and wait until it has left
    fVblOn = false;
    thread_wakeup((event_t)&fVblNext);
    while (fVblRunning) IOSleep(5);
    if (gIfbNubFB == this) { ifb_agdc_register(false); gIfbNubFB = nullptr; }
    kapiFree();
    OSSafeReleaseNULL(fSurfMap); OSSafeReleaseNULL(fMMIO); fRegs = nullptr;
    IOFramebuffer::stop(provider);
}

// intelfb-0011. The table of VRAM blocks handed to NVAccel (one NMIntelFB per machine).
struct IfbVram { struct NvKmsKapiMemory *m; void *k; uint64_t n, cookie; };
static IfbVram gIfbVram[2048];

static void ifb_kapi_event(const struct NvKmsKapiEvent *) {}

// The NVRMDisplay nubs NVRM publishes carry the NVKMS function table, the GPU id and RM's VA-to-physical lookup
// (what NVRMFB starts from). Ownership is not grabbed: it is for mode sets, and NVRMFB may still hold it.
void NMIntelFB::kapiInit()
{
    if (!ifb_knob("nullmoth-intelfb-kapi", 1)) { ifb_trail("kapi-off"); return; }
    fVramLock = IOLockAlloc();
    OSDictionary *m = IOService::serviceMatching("NVRMDisplay");
    IOService *nub = m ? IOService::waitForMatchingService(m, 20ull * NSEC_PER_SEC) : nullptr;
    OSSafeReleaseNULL(m);
    if (!nub || !fVramLock) { ifb_trail("kapi-none"); IFBLOG("kapi: no NVRMDisplay nub - NVAccel gets no VRAM from this panel"); OSSafeReleaseNULL(nub); return; }
    OSNumber *k = OSDynamicCast(OSNumber, nub->getProperty("nvkms-kapi"));
    OSNumber *g = OSDynamicCast(OSNumber, nub->getProperty("gpu-id"));
    OSNumber *pv = OSDynamicCast(OSNumber, nub->getProperty("phys-for-va"));
    fKms = k ? (struct NvKmsKapiFunctionsTable *)(uintptr_t)k->unsigned64BitValue() : nullptr;
    fPhysForVa = pv ? (NvU64 (*)(const void *))(uintptr_t)pv->unsigned64BitValue() : nullptr;
    uint64_t bar = 0;
    IOService *rm = nub->getProvider();
    // intelfb-0012: the first range of at least 256 MB, as NVRMFB's surveyBar1 finds BAR1 (NVRM replaces the device
    // memory list when it places BAR1; the register lookup gave 64 MB of an 8 GB BAR1 in RTX-10)
    if (IOPCIDevice *pci = rm ? OSDynamicCast(IOPCIDevice, rm->getProvider()) : nullptr)
        for (IOItemCount i = 0; i < pci->getDeviceMemoryCount() && !bar; i++)
            if (IODeviceMemory *d = pci->getDeviceMemoryWithIndex(i)) if (d->getLength() >= (256ull << 20)) bar = d->getLength();
    // NVRMFB's budget (nvrmVramBudget): half of a resizable BAR1, three quarters of a smaller one
    fVramBudget = bar >= (4ull << 30) ? bar / 2 : bar / 4 * 3;
    if (!fKms || !g || !fPhysForVa || !fKms->allocateDevice || !fVramBudget) {
        ifb_trail("kapi-none"); IFBLOG("kapi: nub lacks nvkms-kapi/gpu-id/phys-for-va or BAR1 (%llu)", bar); nub->release(); return;
    }
    struct NvKmsKapiAllocateDeviceParams ap = {};
    ap.gpuId = g->unsigned32BitValue(); ap.privateData = this; ap.eventCallback = ifb_kapi_event;
    nub->release();
    fDev = fKms->allocateDevice(&ap);
    ifb_trail(fDev ? "kapi-dev" : "kapi-fail");
    IFBLOG("kapi: allocateDevice(gpu 0x%x) -> %p, VRAM budget %llu MB of BAR1 %llu MB", ap.gpuId, fDev,
           fVramBudget >> 20, bar >> 20);
}

void NMIntelFB::kapiFree()
{
    if (!fKms || !fDev) return;
    IOLockLock(fVramLock);
    for (IfbVram &e : gIfbVram) {
        if (!e.m) continue;
        fKms->unmapMemory(fDev, e.m, NVKMS_KAPI_MAPPING_TYPE_KERNEL, e.k);
        fKms->freeMemory(fDev, e.m);
        e = {};
    }
    fKms->freeDevice(fDev); fDev = nullptr; fVramBytes = 0;
    IOLockUnlock(fVramLock);
}

// NVRMFB's vramGrant without its NVIDIA-display parts (no NVIDIA scanout or console to keep clear of here)
IOReturn NMIntelFB::vramGrant(NVRMVramRequest *r)
{
    r->phys = 0; r->kva = nullptr; r->handle = nullptr; r->actualSize = 0; r->mappedTotal = 0;
    if (!fKms || !fDev) return kIOReturnNotReady;
    const NvU64 want = (r->size + 0xFFFFull) & ~0xFFFFull;
    if (!want) return kIOReturnBadArgument;
    IOLockLock(fVramLock);
    IfbVram *slot = nullptr;
    for (IfbVram &e : gIfbVram) if (!e.m) { slot = &e; break; }
    if (!slot || fVramBytes + want > fVramBudget) {
        IOLockUnlock(fVramLock);
        IFBLOG("nvAllocVram(%llu): refused (%s, %llu of %llu bytes)", want, slot ? "budget" : "table full", fVramBytes, fVramBudget);
        return kIOReturnNoMemory;
    }
    struct NvKmsKapiAllocateMemoryParams mp = {};
    NvU8 compressible = 0;
    mp.layout = NvKmsSurfaceMemoryLayoutPitch; mp.type = NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT;
    mp.size = want; mp.useVideoMemory = NV_TRUE; mp.compressible = &compressible;
    struct NvKmsKapiMemory *m = fKms->allocateMemory(fDev, &mp);
    void *k = nullptr;
    NvU64 p = 0;
    if (m && fKms->mapMemory(fDev, m, NVKMS_KAPI_MAPPING_TYPE_KERNEL, &k) && k) p = fPhysForVa(k);
    bool live = false;
    if (p) {
        bzero(k, (size_t)want);
        volatile NvU64 *q = (volatile NvU64 *)k;
        q[0] = 0x5A5AA5A5C0FFEE01ull; live = q[0] == 0x5A5AA5A5C0FFEE01ull; q[0] = 0;
    }
    if (!live) {
        if (k) fKms->unmapMemory(fDev, m, NVKMS_KAPI_MAPPING_TYPE_KERNEL, k);
        if (m) fKms->freeMemory(fDev, m);
        IOLockUnlock(fVramLock);
        IFBLOG("nvAllocVram(%llu): %s", want, !m ? "allocateMemory failed" : !k ? "mapMemory failed" : !p ? "no physical address" : "the mapping is not live");
        return kIOReturnNoMemory;
    }
    *slot = { m, k, want, ++fVramCookie };
    fVramBytes += want;
    r->handle = m; r->kva = k; r->phys = p; r->actualSize = want; r->mappedTotal = fVramBytes;
    r->allocationCookie = slot->cookie;
    IOLockUnlock(fVramLock);
    if (fVramLogged < 16) { fVramLogged++; IFBLOG("nvAllocVram: %llu bytes VRAM, phys 0x%llx (%llu MB in use)", want, p, fVramBytes >> 20); }
    if (fVramLogged == 1) ifb_trail("vram1");
    return kIOReturnSuccess;
}

IOReturn NMIntelFB::vramRelease(NVRMVramRequest *r)
{
    if (!fKms || !fDev || !r->handle) return kIOReturnBadArgument;
    IOLockLock(fVramLock);
    for (IfbVram &e : gIfbVram) {
        if (e.m != r->handle || e.cookie != r->allocationCookie) continue;
        fKms->unmapMemory(fDev, e.m, NVKMS_KAPI_MAPPING_TYPE_KERNEL, e.k);
        fKms->freeMemory(fDev, e.m);
        fVramBytes -= e.n;
        e = {};
        IOLockUnlock(fVramLock);
        r->handle = nullptr; r->kva = nullptr; r->phys = 0; r->actualSize = 0; r->allocationCookie = 0;
        return kIOReturnSuccess;
    }
    IOLockUnlock(fVramLock);
    return kIOReturnBadArgument;
}

IODeviceMemory *NMIntelFB::getApertureRange(IOPixelAperture aperture)
{
    IFB_ONCE("apr");
    if (aperture != kIOFBSystemAperture) return nullptr;
    return IODeviceMemory::withRange(fApertureBase + fSurf, (IOPhysicalLength)fRowBytes * fHeight);
}

IOReturn NMIntelFB::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info)
{
    IFB_ONCE("info");
    if (mode != 1 || !info) return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->maxDepthIndex = 0; info->nominalWidth = fWidth; info->nominalHeight = fHeight;
    info->refreshRate = fRefresh16; info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

IOReturn NMIntelFB::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi)
{
    IFB_ONCE("pix");
    if (mode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
    bzero(pi, sizeof(*pi));
    pi->bytesPerRow = fRowBytes; pi->bytesPerPlane = 0; pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
    pi->componentCount = 3; pi->bitsPerComponent = 8;
    pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
    strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
    pi->activeWidth = fWidth; pi->activeHeight = fHeight;
    return kIOReturnSuccess;
}

IOReturn NMIntelFB::getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value)
{
    ifb_attr("gc", (uint32_t)attr);
    if (idx != 0 || !value) return kIOReturnBadArgument;
    switch (attr) {
    case kConnectionEnable: *value = 1; return kIOReturnSuccess;
    case kConnectionCheckEnable: *value = 1; return kIOReturnSuccess;
    case kConnectionFlags: *value = kIOConnectionBuiltIn; return kIOReturnSuccess;
    // the colour answers NVRMFB gives (RGB, 8 bits per component), which WindowServer asks before compositing
    case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
    case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
        *value = 0x00000002; return kIOReturnSuccess;
    default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
    }
}

// NVAccel's display pipe asks every head for its scan-out (NVRM_VRAM_FN_SCANOUT). This head answers with a CPU mapping and
// no VRAM address: phys 0 keeps NVAccel from treating Intel memory as NVIDIA VRAM (it is not in the NVIDIA card's BAR1),
// so frames reach the panel only through the copy into kva.
IOReturn NMIntelFB::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && fn->isEqualTo(NVRM_VRAM_FN_SCANOUT)) {
        IFB_ONCE("scan");
        NVRMVramRequest *r = (NVRMVramRequest *)p1;
        if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
        if (!fSurfMap) return kIOReturnNotReady;
        r->kva = (void *)fSurfMap->getVirtualAddress();
        r->phys = 0; r->actualSize = 0;
        // intelfb-0016: with phys 0 NVAccel gave the display pipe no framebuffer resource, and WindowServer's every
        // transaction failed before reaching NVAccel ("Fallback GPU Display Pipe transaction submission failed", 186
        // times in RTX-14; nullmoth's copy path was only ever tried beside a real NVIDIA head). A VRAM block of our
        // own the size of the panel stands for the scanout, so the pipe gets a resource as an NVIDIA head's does; kva
        // stays the Intel surface, where performTransaction copies each frame. nullmoth-intelfb-scanres=0: phys 0.
        if (fDev && ifb_knob("nullmoth-intelfb-scanres", 1)) {
            if (!fScanRes.phys) {
                NVRMVramRequest q = {};
                q.version = NVRM_VRAM_ABI_VERSION; q.size = (unsigned long long)fRowBytes * fHeight;
                if (vramGrant(&q) == kIOReturnSuccess) { fScanRes = q; ifb_trail("scanres"); IFBLOG("scanout resource: VRAM phys 0x%llx (%llu bytes)", q.phys, q.actualSize); }
                else ifb_trail("scanres-fail");
            }
            if (fScanRes.phys) { r->phys = fScanRes.phys; r->actualSize = fScanRes.actualSize; r->handle = fScanRes.handle; }
        }
        r->width = fWidth; r->height = fHeight; r->pitch = fRowBytes;
        return kIOReturnSuccess;
    }
    // intelfb-0011: what NVAccel asks its primary framebuffer for, answered from our NVKMS device
    if (fn && fn->isEqualTo(NVRM_GPUVA_FN_HANDLES)) {
        NVRMRmHandles *h = (NVRMRmHandles *)p1;
        if (!h || h->version != NVRM_GPUVA_ABI_VERSION) return kIOReturnBadArgument;
        if (!fDev) return kIOReturnNotReady;
        IFB_ONCE("rmh");
        h->kapiDevice = fDev; h->hClient = 0; h->hDevice = 0; h->hSubDevice = 0;
        return kIOReturnSuccess;
    }
    const bool alloc = fn && fn->isEqualTo(NVRM_VRAM_FN_ALLOC), release = fn && fn->isEqualTo(NVRM_VRAM_FN_FREE);
    if (alloc || release) {
        NVRMVramRequest *r = (NVRMVramRequest *)p1;
        if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
        return alloc ? vramGrant(r) : vramRelease(r);
    }
    return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

// intelfb-0013. CoreDisplay pairs a framebuffer with the GPU above it in the registry: under the Intel GPU the panel
// had no display pipe ("System set to use IOP but no display pipes are present", RTX-11) while NVAccel's pipes sat on
// the NVIDIA GPU with no framebuffer. NMIntelFBClaim holds the Intel GPU's framebuffer match category (so no
// IONDRVFramebuffer takes it, as NVRMFBClaim does on the NVIDIA GPU) and publishes an NVRMDisplay nub marked as the
// Intel panel under NVRM; NMIntelFB starts on that nub, as in nullmoth's virtual build, and drives the Intel GPU from
// gIfbPCI. nullmoth-intelfb-nvparent=0: no claim, NMIntelFB on the Intel GPU as before.
class NMIntelFBClaim : public IOService {
    OSDeclareDefaultStructors(NMIntelFBClaim)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    static void publishThread(void *, wait_result_t);
};
OSDefineMetaClassAndStructors(NMIntelFBClaim, IOService)

IOService *NMIntelFBClaim::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    uint8_t ver = 0;
    if (!pci || ifb_knob("nullmoth-intelfb-nvparent", 1) == 0 || !ifb_pci_ok(pci, &ver)) return nullptr;
    return IOService::probe(provider, score);
}

bool NMIntelFBClaim::start(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci || !IOService::start(provider)) return false;
    pci->retain(); gIfbPCI = pci;
    ifb_trail("claim");
    thread_t t = nullptr;
    if (kernel_thread_start(publishThread, nullptr, &t) != KERN_SUCCESS) { ifb_trail("claim-nothread"); return true; }
    thread_deallocate(t);
    return true;
}

// NVRM publishes its NVRMDisplay nubs some seconds into the boot; wait for nub 0 and add the Intel panel's beside it
void NMIntelFBClaim::publishThread(void *, wait_result_t)
{
    IOService *nub0 = nullptr, *nvrm = nullptr;
    for (int i = 0; i < 120 && !nub0; i++) {
        if (OSDictionary *m = IOService::serviceMatching("NVRMDisplay")) {
            if (OSIterator *it = IOService::getMatchingServices(m)) {
                while (OSObject *o = it->getNextObject()) {
                    IOService *n = OSDynamicCast(IOService, o);
                    OSNumber *fi = n ? OSDynamicCast(OSNumber, n->getProperty("fb-index")) : nullptr;
                    if (fi && fi->unsigned32BitValue() == 0) { nub0 = n; nub0->retain(); break; }
                }
                it->release();
            }
            m->release();
        }
        if (!nub0) IOSleep(1000);
    }
    if (nub0) { nvrm = nub0->getProvider(); if (nvrm) nvrm->retain(); }
    IOService *nub = (nub0 && nvrm) ? OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NVRMDisplay")) : nullptr;
    bool ok = nub && nub->init();
    if (ok) {
        static const char *const kCopy[] = {"nvkms-kapi", "gpu-id", "phys-for-va"};
        for (const char *k : kCopy) if (OSObject *v = nub0->getProperty(k)) nub->setProperty(k, v);
        nub->setProperty("fb-index", 4ull, 32);
        nub->setProperty("nm-intel-panel", kOSBooleanTrue);
        ok = nub->attach(nvrm);
        if (ok) nub->registerService();
    }
    ifb_trail(ok ? "nub" : nub0 ? "nub-fail" : "nub-no-nvrm");
    IFBLOG("claim: Intel-panel NVRMDisplay nub %s", ok ? "published under NVRM" : nub0 ? "could not be published" : "not published: no NVRMDisplay 0 after 120 s");
    OSSafeReleaseNULL(nub); OSSafeReleaseNULL(nub0); OSSafeReleaseNULL(nvrm);
    thread_terminate(current_thread());
}

// intelfb-0014. WindowServer builds one display pipe per framebuffer that the GPU's AppleGraphicsDeviceControl lists
// in its reply to command 0x980 (nullmoth's NVRMAGDC); without an AGDC, IOPresentment logged "Unable to get AGDC
// information ... Unable to acquire map" and CoreDisplay "System set to use IOP but no display pipes are present"
// (RTX-12, with the panel already under the NVIDIA GPU). NVRMAGDC lists only NVRMFB framebuffers, so this one lists
// the panel: vendor info (1) and the framebuffer table (0x980), as NVRMAGDC answers them. Started by sysctl
// debug.nmintelfb_agdc=1 (tools/intelfb/enable.sh), in the order NVRM's auto-go uses: accelerator, AGDC, iop.
#pragma pack(push, 1)
struct IfbAgdcVendorInfo { uint32_t version; char vendor[0x20]; uint32_t vendorID, vendorClass; };
struct IfbAgdcGPUCapability {
    uint64_t portMask, portFeatureA, portFeatureB, portFeatureC;
    uint32_t countA, countB, countC, countD, maxFramebuffers;
    IOService *pciDevice;
    IOService *framebuffer[20];
};
#pragma pack(pop)
static_assert(sizeof(IfbAgdcVendorInfo) == 0x2c, "AGDCVendorInfo_t is 0x2c");
static_assert(sizeof(IfbAgdcGPUCapability) == 0xdc, "AGDCGPUCapability_t is 0xdc");

class NMIntelFBAGDC : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(NMIntelFBAGDC)
public:
    IOService *fPci = nullptr, *fFB = nullptr;
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;
    IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize, unsigned long *out,
                                      unsigned long *outSize, IOExternalMethodArguments *args) override;
};
OSDefineMetaClassAndStructors(NMIntelFBAGDC, AppleGraphicsDeviceControl)

static int gIfbAgdcState = 0;   // 0 off, 2 starting, 1 up, < 0 failed
static unsigned gIfbAgdcLogged = 0;

IOReturn NMIntelFBAGDC::vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                                 unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *)
{
    if (cmd == 1) {
        if (!out || !outSize || *outSize != sizeof(IfbAgdcVendorInfo)) return kIOReturnBadArgument;
        IfbAgdcVendorInfo *v = (IfbAgdcVendorInfo *)out; bzero(v, sizeof *v);
        v->version = 0x30000; strlcpy(v->vendor, "NVIDIA", sizeof v->vendor); v->vendorID = 0x10de; v->vendorClass = 2;
        return kIOReturnSuccess;
    }
    if (cmd == 0x980) {
        if (!out || !outSize || *outSize != sizeof(IfbAgdcGPUCapability)) return kIOReturnBadArgument;
        IfbAgdcGPUCapability *c = (IfbAgdcGPUCapability *)out; bzero(c, sizeof *c);
        c->portMask = 1ull << 1;
        c->countA = c->countB = c->countC = c->countD = 1;
        c->maxFramebuffers = 1; c->pciDevice = fPci; c->framebuffer[0] = fFB;
        IFB_ONCE("agdc-980");
        return kIOReturnSuccess;
    }
    // intelfb-0015: kAGDCLinkConfig (0x921) and the plane-scaler capability (0x711), which NVRMAGDC answers once NVRM's
    // auto-go sets debug.nvrmfb_agdc_k5; unanswered, CoreDisplay logged "Attempting to get capabilities ... with no
    // devices" and "No plane indices", and WindowServer sent no frame (RTX-13)
    if (cmd == NVK5_CMD_LINKCONFIG || cmd == NVK5_CMD_FBCAPEX) {
        int why = 0;
        int r = nvk5_answer(cmd, in, inSize, out, outSize ? *outSize : 0, gIfbK5, sizeof gIfbK5, &why);
        if (r == NVK5_R_LINK) { IFB_ONCE("k5-link"); return kIOReturnSuccess; }
        if (r == NVK5_R_CAPS) { IFB_ONCE("k5-caps"); return kIOReturnSuccess; }
        if (r != NVK5_R_UNSUPPORTED) {
            if (gIfbAgdcLogged < 20) { gIfbAgdcLogged++; IFBLOG("AGDC K5: command %#x not answered (why %#x)", cmd, why); }
            return r == NVK5_R_BADARG ? kIOReturnBadArgument : kIOReturnNotFound;
        }
    }
    if (gIfbAgdcLogged < 20) { gIfbAgdcLogged++; IFBLOG("AGDC: command %#x (in %lu bytes) -> Unsupported", cmd, inSize); }
    return kIOReturnUnsupported;
}

static void ifb_agdc_up(void *, wait_result_t)
{
    IOService *fb = gIfbNubFB, *pci = fb ? fb->getProvider() : nullptr;
    for (int hops = 0; pci && !OSDynamicCast(IOPCIDevice, pci) && hops < 8; hops++) pci = pci->getProvider();
    NMIntelFBAGDC *d = (fb && OSDynamicCast(IOPCIDevice, pci)) ? OSTypeAlloc(NMIntelFBAGDC) : nullptr;
    int st = !d ? -1 : !d->init(nullptr) ? -2 : 0;
    if (!st) {
        d->fPci = pci; d->fFB = fb;
        if (!d->attach(pci)) st = -3;
        else if (!d->start(pci)) { d->detach(pci); st = -4; }
        else st = 1;
    }
    if (st != 1) OSSafeReleaseNULL(d);   // an AGDC that came up stays until the next boot
    gIfbAgdcState = st;
    ifb_trail(st == 1 ? "agdc" : "agdc-fail");
    IFBLOG("AGDC: %s (state %d) on %s", st == 1 ? "UP" : "not started", st, pci ? pci->getName() : "no NVIDIA PCI device");
    thread_terminate(current_thread());
}

static int ifb_agdc_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gIfbAgdcState, err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 1) return EINVAL;
    if (gIfbAgdcState == 1 || gIfbAgdcState == 2) return 0;
    if (!gIfbNubFB) return ENXIO;
    gIfbAgdcState = 2;
    thread_t t = nullptr;
    if (kernel_thread_start(ifb_agdc_up, nullptr, &t) != KERN_SUCCESS) { gIfbAgdcState = -5; return ENOMEM; }
    thread_deallocate(t);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_agdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, nullptr, 0,
            ifb_agdc_sysctl, "I", "1 = list the Intel panel in an AppleGraphicsDeviceControl on the NVIDIA GPU (1 up, 2 starting, < 0 failed)");
static void ifb_agdc_register(bool on)
{
    if (on) sysctl_register_oid(&sysctl__debug_nmintelfb_agdc); else sysctl_unregister_oid(&sysctl__debug_nmintelfb_agdc);
}

#ifdef NMINTELFB_VIRTUAL
// Test loader: attaches a virtual panel under the NVIDIA PCI device on demand, the way the real driver will place the
// Intel panel so WindowServer composites it on the NVIDIA accelerator.
class NMIntelFBLoader : public IOService {
    OSDeclareDefaultStructors(NMIntelFBLoader)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};
OSDefineMetaClassAndStructors(NMIntelFBLoader, IOService)
static int gAttach = 0;
static int nmintelfb_attach_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gAttach, err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr || v != 1 || gAttach == 1) return err;
    // A spare NVRMDisplay nub under NVRM (NVRM's own class), marked as the Intel panel: NMIntelFB claims it ahead of
    // NVRMFB, so the panel sits where NVRMFB does (GPU -> NVRM -> NVRMDisplay -> framebuffer). Starting a framebuffer
    // directly on the NVIDIA PCI device hung the kernel.
    IOService *nub0 = nullptr, *nvrm = nullptr;
    if (OSDictionary *m = IOService::serviceMatching("NVRMDisplay")) {
        if (OSIterator *it = IOService::getMatchingServices(m)) {
            while (OSObject *o = it->getNextObject()) {
                IOService *n = OSDynamicCast(IOService, o);
                OSNumber *fi = n ? OSDynamicCast(OSNumber, n->getProperty("fb-index")) : nullptr;
                if (fi && fi->unsigned32BitValue() == 0) { nub0 = n; nub0->retain(); break; }
            }
            it->release();
        }
        m->release();
    }
    if (nub0) { nvrm = nub0->getProvider(); if (nvrm) nvrm->retain(); }
    if (!nub0 || !nvrm) { OSSafeReleaseNULL(nub0); OSSafeReleaseNULL(nvrm); IFBLOG("attach: no NVRMDisplay 0 / NVRM"); return ENODEV; }
    IOService *nub = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NVRMDisplay"));
    if (!nub || !nub->init()) { OSSafeReleaseNULL(nub); nub0->release(); nvrm->release(); return ENOMEM; }
    static const char *const kCopy[] = {"gpu-id"};
    for (const char *k : kCopy) if (OSObject *v = nub0->getProperty(k)) nub->setProperty(k, v);
    nub->setProperty("fb-index", 4ull, 32);
    nub->setProperty("nm-intel-panel", kOSBooleanTrue);
    nub0->release();
    if (!nub->attach(nvrm)) { nub->release(); nvrm->release(); return EIO; }
    nvrm->release();
    nub->registerService();
    nub->release();
    gAttach = 1;
    IFBLOG("attach: Intel-panel NVRMDisplay nub published under NVRM");
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_attach, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, nullptr, 0,
            nmintelfb_attach_sysctl, "I", "1 = attach the virtual panel under the NVIDIA display device (test build)");
bool NMIntelFBLoader::start(IOService *provider)
{
    if (version_major != 24 && version_major != 25) return false;   /* the 2.x line supports macOS 15 (Darwin 24) and macOS 26 (Darwin 25) only */
    if (!IOService::start(provider)) return false;
    sysctl_register_oid(&sysctl__debug_nmintelfb_attach);
    IFBLOG("loader idle until debug.nmintelfb_attach=1");
    return true;
}
void NMIntelFBLoader::stop(IOService *provider)
{
    sysctl_unregister_oid(&sysctl__debug_nmintelfb_attach);
    IOService::stop(provider);
}
#endif
