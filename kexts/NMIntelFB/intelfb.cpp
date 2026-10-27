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
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
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
// intelfb-0017: the panel's scan-out pages in RAM, for NVAccel's copy engine (same layout in nmx-accel's nvrm-accel.cpp)
#define NMINTELFB_RAM_VERSION 1u
#define NMINTELFB_FN_RAM      "nmIntelFBRamPages"
#define NMINTELFB_FN_RAM_FLIP "nmIntelFBRamFlip"   // intelfb-0019: p1 = the pages NVAccel just wrote
static volatile SInt64 gIfbFlips, gIfbFlipWaits, gIfbFlipWaitUs, gIfbFlipLate;
struct NMIntelFBRamPages { unsigned int version, pitch; const unsigned long long *pages; unsigned long long count, length; volatile void *kva; };
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
// intelfb-0021: true when nullmoth's boot guard has NVRM off this boot (same test as nvrm_driver_off, minus -nvoff,
// which ifb_pci_ok checks first) and no NVRM instance is attached anywhere
static bool ifb_nvrm_guard_off()
{
    IODTNVRAM *n = ifb_nvram();
    if (!n) return false;   // NVRAM not published: NVRM cannot read the guard either
    OSObject *flag = n->copyProperty("nullmoth-remove");
    OSData *d = OSDynamicCast(OSData, n->copyProperty("nullmoth-nv-starts"));
    const unsigned starts = (d && d->getLength() >= 1) ? *(const unsigned char *)d->getBytesNoCopy() : 0;
    if (d) d->release();
    n->release();
    const bool removing = flag != nullptr;
    if (flag) flag->release();
    const bool off = removing || starts >= 2;
    if (!off) return false;
    bool nvrm = false;
    if (IORegistryIterator *it = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively)) {
        while (IORegistryEntry *e = it->getNextObject()) if (strcmp(e->getMetaClass()->getClassName(), "NVRM") == 0) { nvrm = true; break; }
        it->release();
    }
    IFBLOG("boot guard: nullmoth-remove %s, nullmoth-nv-starts %u, NVRM %s", removing ? "set" : "unset", starts, nvrm ? "attached" : "absent");
    return !nvrm;
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

static bool gIfbDepNvrm = false;   // the panel sits in NVRM's group (dep=2, or dep=3 with a head 0)
static unsigned ifb_nvrm_heads(void);

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
    // intelfb-0017 (stage 4a): the panel scans out of a buffer in RAM instead of stolen memory, so NVAccel's copy engine
    // can write each frame there (stolen memory is fenced from other devices). Same GGTT offset, PLANE_SURF and
    // aperture view; only the GGTT entries change. sysctl debug.nmintelfb_ram=1 / 0.
    // intelfb-0019 (stage 4b): mode 2 adds a second buffer at its own GGTT offset; NVAccel writes the one the plane is
    // not showing and asks for a flip (PLANE_SURF, latched at the next vblank), so a frame is never shown half-written.
    IOBufferMemoryDescriptor *fRam[2] = {};
    unsigned long long *fRamPages[2] = {}, *fOldPte[2] = {};
    uint32_t fRamGgtt[2] = {};
    unsigned fRamCount = 0;
    volatile int fRamMode = 0;        // 0 firmware pages, 1 one RAM buffer, 2 two RAM buffers flipped
    volatile unsigned fFront = 0;     // the buffer PLANE_SURF was last set to
    uint32_t fSurfLow = 0;            // PLANE_SURF's non-address bits, kept on every flip
    void ramPtes(unsigned b, bool ours);
    int ramMap(unsigned b);
    bool flipLatched(unsigned wantUs);
    void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(fRegs + off) = v; }
    class NMIntelFBBacklight *fBacklight = nullptr;   // intelfb-0020: the panel's brightness control
    void publishBacklight();
public:
    // intelfb-0020: BXT_BLC_PWM_CTL1/FREQ1/DUTY1 (0xC8250/54/58), the PWM the firmware left driving the panel (RTX-22)
    static constexpr uint32_t kBlcCtl = 0xC8250, kBlcFreq = 0xC8254, kBlcDuty = 0xC8258;
    int backlightPercent() {
        const uint32_t f = rd(kBlcFreq), d = rd(kBlcDuty);
        if (!f) return -1;
        const uint32_t on = (rd(kBlcCtl) & (1u << 29)) ? f - d : d;   // bit 29: inverted polarity
        return (int)(((uint64_t)on * 100 + f / 2) / f);
    }
    bool setBacklightPercent(unsigned pct) {
        const uint32_t f = rd(kBlcFreq);
        if (!f || !(rd(kBlcCtl) & (1u << 31))) return false;   // PWM off (after S3): nothing to drive
        const uint32_t on = (uint32_t)(((uint64_t)f * pct + 50) / 100);
        wr(kBlcDuty, (rd(kBlcCtl) & (1u << 29)) ? f - on : on);
        return true;
    }
    IOReturn getAppleSense(IOIndex connectIndex, UInt32 *senseType, UInt32 *primary, UInt32 *extended, UInt32 *displayType) override;
    int ramSet(int mode);
    // intelfb-0018: the display registers that say whether the panel is lit (pipe, plane, panel power, PWM backlight,
    // first scan-out PTE): logged at sleep and wake, and read on demand through sysctl debug.nmintelfb_regs
    void regsInto(char *b, size_t n) {
        if (!fRegs || fPipe < 0) { snprintf(b, n, "no registers"); return; }
        snprintf(b, n, "trans %08x plane %08x surf %08x pp %08x/%08x blc %08x/%08x/%08x blc2 %08x/%08x/%08x pte0 %llx",
                 rd(TRANSCONF(fPipe)), rd(PLANE_CTL(fPipe)), rd(PLANE_SURF(fPipe)), rd(0xC7200), rd(0xC7204),
                 rd(0xC8250), rd(0xC8254), rd(0xC8258), rd(0xC8350), rd(0xC8354), rd(0xC8358),
                 (unsigned long long)*((volatile uint64_t *)(fRegs + 0x800000) + (fSurf >> 12)));
    }
    // the Type-C ports, power wells, PLLs and DBUF, as the firmware left them. Reads only, no
    // MMIO write; logged once at start and read on demand through sysctl debug.nmintelfb_tcdiag. Offsets: Linux 7.3 i915
    // (MIT) for display 13 (ADL-P/RPL-P): TC1-TC4 = ports D-G, modular FIA (FIA1 holds TC1-2, FIA2 TC3-4). A port in
    // TC-cold reads 0xffffffff in TCSS/FIA.
    void tcDiagInto(char *b, size_t n) {
        if (!fRegs) { snprintf(b, n, "no registers\n"); return; }
        size_t o = 0;
        auto put = [&](const char *fmt, auto... a) { if (o < n) { int w = snprintf(b + o, n - o, fmt, a...); if (w > 0) o += (size_t)w; } };
        put("power dc %08x pw1 %08x pw2 %08x aux1 %08x aux2 %08x ddi1 %08x ddi2 %08x fuse %08x\n", rd(0x45504), rd(0x45400),
            rd(0x45404), rd(0x45440), rd(0x45444), rd(0x45450), rd(0x45454), rd(0x42000));
        put("clock cdclk %08x dssm %08x dpclka %08x dpll0 %08x dpll1 %08x tbtpll %08x\n", rd(0x46000), rd(0x51004),
            rd(0x164280), rd(0x46010), rd(0x46014), rd(0x46020));
        put("dbuf mbus %08x s0 %08x s1 %08x s2 %08x s3 %08x abox %08x/%08x/%08x\n", rd(0x4438C), rd(0x45008), rd(0x44FE8),
            rd(0x44300), rd(0x44304), rd(0x45038), rd(0x45048), rd(0x4504C));
        put("hpd isr %08x imr %08x ier %08x tcctl %08x sdeisr %08x\n", rd(0x44470), rd(0x44474), rd(0x4447C), rd(0x44038),
            rd(0xC4000));
        for (uint32_t p = 0; p < 4; p++)
            put("pipe%c trans %08x func %08x clksel %08x plane %08x buf1 %08x buf2 %08x cur %08x\n", 'A' + p,
                rd(0x70008 + p * 0x1000), rd(0x60400 + p * 0x1000), rd(0x46140 + p * 4), rd(0x70180 + p * 0x1000),
                rd(0x7027C + p * 0x1000), rd(0x7037C + p * 0x1000), rd(0x7017C + p * 0x1000));
        for (uint32_t d = 0; d < 2; d++)
            put("ddi%c buf %08x aux %08x\n", 'A' + d, rd(0x64000 + d * 0x100), rd(0x64010 + d * 0x100));
        for (uint32_t t = 0; t < 4; t++)
            put("tc%u status %08x buf %08x aux %08x clksel %08x pll %08x\n", t + 1, rd(0x161500 + t * 4),
                rd(0x64300 + t * 0x100), rd(0x64310 + t * 0x100), rd(0x4610C + t * 4), rd(0x46038 + t * 8));
        for (uint32_t f = 0; f < 2; f++) {
            const uint32_t base = f ? 0x16E000 : 0x163000;
            put("fia%u pa1 %08x pms %08x csss %08x dpsp %08x mle1 %08x\n", f + 1, rd(base + 0x880), rd(base + 0x890),
                rd(base + 0x894), rd(base + 0x8A0), rd(base + 0x8C0));
        }
    }
private:
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
        // in NVRM's group the same walk stopped at the empty slot after NVRMFB's heads, so the panel at 4 was
        // never in it and WindowServer found no pipe for it (pipe 1 active, "Unable to find display pipe").
        // It takes the slot right after the NVIDIA heads, which is also the pipe number NVAccel gave it.
        else if (gIfbDepNvrm) { setProperty("IOFBDependentIndex", (unsigned long long)ifb_nvrm_heads(), 32); IFB_ONCE("open-idxn"); }
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
        { char r[200]; regsInto(r, sizeof r); IFBLOG("power %lu: %s", ordinal, r); }
        for (unsigned b = 0; ordinal && b < (unsigned)fRamMode; b++) ramPtes(b, true);   // intelfb-0017: in case the GGTT was rewritten while asleep
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

// intelfb-0020: panel brightness, as NVRMFB does it for a panel on the NVIDIA GPU (nullmoth 60dc7c8). macOS gives the
// brightness keys and slider to an AppleBacklightDisplay, which IOGraphics makes for a built-in connection whose
// getAppleSense() says LCD, provided a service named "backlight" existed when the first framebuffer opened. This
// handler is that service, attached below the framebuffer before IOFramebuffer::start(); IODisplay finds it and sends
// "brightness"/"linear-brightness" (0-100) here. Applied from a thread call: IODisplay holds the framebuffer lock.
static const SInt32 kIfbMinBacklight = 5;   // never 0: some panels switch the backlight off completely
class NMIntelFBBacklight : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(NMIntelFBBacklight)
    NMIntelFB *fFB = nullptr;
    thread_call_t fApply = nullptr;
    volatile SInt32 fPending = -1;
    SInt32 fLevel = 100;
    static void applyFire(thread_call_param_t me, thread_call_param_t);
public:
    static NMIntelFBBacklight *withFB(NMIntelFB *fb, int level);
    bool setDisplay(IODisplay *display) override;
    bool doIntegerSet(OSDictionary *params, const OSSymbol *paramName, UInt32 value) override;
    bool doDataSet(const OSSymbol *, OSData *) override { return false; }
    bool doUpdate() override { return true; }
    void free() override;
};
OSDefineMetaClassAndStructors(NMIntelFBBacklight, IODisplayParameterHandler)

NMIntelFBBacklight *NMIntelFBBacklight::withFB(NMIntelFB *fb, int level)
{
    NMIntelFBBacklight *h = OSTypeAlloc(NMIntelFBBacklight);
    if (!h || !h->init()) { OSSafeReleaseNULL(h); return nullptr; }
    h->fFB = fb;   // not retained: the framebuffer owns the handler and detaches it in stop()
    h->fApply = thread_call_allocate(applyFire, h);
    h->fLevel = level < 0 ? 100 : level > 100 ? 100 : level;
    if (!h->fApply) { h->release(); return nullptr; }
    return h;
}

void NMIntelFBBacklight::free()
{
    if (fApply) { thread_call_cancel(fApply); while (!thread_call_free(fApply)) { thread_call_cancel(fApply); IOSleep(1); } fApply = nullptr; }
    IODisplayParameterHandler::free();
}

// NMIntelFB reports no display parameters of its own, so IODisplayParameters may lack these; IODisplay::setProperties()
// only accepts parameters listed there
bool NMIntelFBBacklight::setDisplay(IODisplay *display)
{
    if (!display) return false;
    OSObject *old = display->copyProperty(gIODisplayParametersKey);
    OSDictionary *cur = OSDynamicCast(OSDictionary, old);
    OSDictionary *params = cur ? OSDictionary::withDictionary(cur) : OSDictionary::withCapacity(2);
    OSSafeReleaseNULL(old);
    if (!params) return false;
    IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, 100);
    IODisplay::setParameter(params, gIODisplayBrightnessKey, fLevel);
    IODisplay::addParameter(params, gIODisplayLinearBrightnessKey, 0, 100);
    IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, fLevel);
    display->setProperty(gIODisplayParametersKey, params);
    params->release();
    IFBLOG("backlight: brightness parameters on %s, panel at %d%%", display->getName(), (int)fLevel);
    ifb_trail("bl-display");
    return true;
}

bool NMIntelFBBacklight::doIntegerSet(OSDictionary *, const OSSymbol *paramName, UInt32 value)
{
    if (paramName != gIODisplayBrightnessKey && paramName != gIODisplayLinearBrightnessKey) return false;
    fPending = value > 100 ? 100 : (SInt32)value;
    thread_call_enter(fApply);
    return true;
}

void NMIntelFBBacklight::applyFire(thread_call_param_t p, thread_call_param_t)
{
    NMIntelFBBacklight *me = (NMIntelFBBacklight *)p;
    SInt32 v;
    do { v = me->fPending; } while (!OSCompareAndSwap((UInt32)v, (UInt32)-1, (volatile UInt32 *)&me->fPending));
    if (v < 0) return;
    const unsigned pct = (unsigned)(v < kIfbMinBacklight ? kIfbMinBacklight : v);
    if (me->fFB->setBacklightPercent(pct)) me->fLevel = v;
    else IFBLOG("backlight: setting %u%% failed (PWM off)", pct);
    static unsigned n; if (n < 8) { n++; IFBLOG("backlight: %d%% -> duty %u%%", (int)v, pct); }
}

// intelfb-0020: only when the PWM the firmware left is running (it always is at boot here; nullmoth-intelfb-backlight=0
// leaves brightness out)
void NMIntelFB::publishBacklight()
{
    if (!ifb_knob("nullmoth-intelfb-backlight", 1)) { ifb_trail("bl-off"); return; }
    const int level = backlightPercent();
    if (level < 0 || !(rd(kBlcCtl) & (1u << 31))) { IFBLOG("backlight: PWM 1 not running (ctl 0x%x) - no brightness control", rd(kBlcCtl)); return; }
    fBacklight = NMIntelFBBacklight::withFB(this, level);
    if (!fBacklight) { IFBLOG("backlight: handler allocation failed"); return; }
    fBacklight->setName("backlight");
    if (!fBacklight->attach(this)) { OSSafeReleaseNULL(fBacklight); IFBLOG("backlight: handler attach failed"); return; }
    fBacklight->registerService(kIOServiceSynchronous);
    IFBLOG("backlight: panel at %d%% - brightness keys and slider through AppleBacklightDisplay", level);
    ifb_trail("bl");
}

// AppleBacklightDisplay::probe takes only an LCD connection
IOReturn NMIntelFB::getAppleSense(IOIndex connectIndex, UInt32 *senseType, UInt32 *primary, UInt32 *extended, UInt32 *displayType)
{
    if (connectIndex != 0 || !fBacklight) return kIOReturnUnsupported;
    if (senseType) *senseType = 0;
    if (primary) *primary = 0;
    if (extended) *extended = 0;
    if (displayType) *displayType = kGenericLCD;
    return kIOReturnSuccess;
}
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
static IOService *gIfbClaim;   // NMIntelFBClaim, parent of the GPUWrangler accelerator
static void ifb_gwready(bool panel);
static bool gIfbAutopipeOn;   // the autopipe thread runs, and publishes the Intel GPU when done
static bool gIfbBootMonitor;  // a monitor on the NVIDIA port at boot (NVRMFB head 0 when start() ran)
static IOService *gIfbNubFB = nullptr;   // intelfb-0014: the framebuffer on the nub, which the AGDC reports
static uint32_t gIfbK5[NVK5_TIMING_WORDS];   // intelfb-0015: the panel's timing in NVRMFB's NVRMTiming layout
// the panel beside a monitor on NVRMFB's head 0. In NVRM's group the panel is
// NVAccel's head 4, whose pipe comes after head 0's, and WindowServer builds one pipe per entry of the AGDC 0x980
// table, in order: the AGDC lists head 0 first, then the panel (listing the panel alone left it without one)
static IOService *gIfbAgdcHead0 = nullptr;    // NVRMFB's head 0 listed by the AGDC, kept until reboot
static IOService *ifb_nvrm_head0(void);
static void ifb_autopipe_start(void);
static void ifb_agdc_register(bool on);
static void ifb_ram_register(NMIntelFB *fb);
static NMIntelFB *volatile gIfbRamFB;   // intelfb-0017: the panel debug.nmintelfb_ram acts on
static volatile SInt32 gIfbScanAsks;    // NVAccel's scanout questions, one per frame it copies to the panel
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
    // intelfb-0021: nullmoth's boot guard (NVRM/nvrm_off.h) turns NVRM off after two starts that never lit a display, or
    // while "nullmoth-remove" is set. Then no NVRMDisplay ever comes, and a claim held here only keeps IONDRVFramebuffer
    // off this GPU: WindowServer loops on a black screen instead of the firmware desktop. The counter is read before
    // looking for NVRM: an NVRM already attached has passed its own check (it is on, and its increment is in the value),
    // and with none attached yet the value read is the one NVRM's probe will judge. Undo: nullmoth-intelfb-guard=0
    if (ifb_knob("nullmoth-intelfb-guard", 1) && ifb_nvrm_guard_off()) {
        IFBLOG("off: nullmoth's boot guard turned NVRM off - leaving the firmware screen; sudo nvram -d nullmoth-nv-starts");
        ifb_nv_set("nullmoth-intelfb-trail", "guard: NVRM off (nullmoth-nv-starts/nullmoth-remove)");
        return false;
    }
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
    // the short delay and the early GPUWrangler publish were tested only with a monitor on the NVIDIA
    // port at boot (NVRM's group, autopipe). Without one (no NVRMFB head 0 when this nub is published, which NVRM does
    // after nub 0's framebuffer started) the Mac hung about 10 s after start() with WindowServer already on the panel
    //: there the panel waits nullmoth-intelfb-delay-own (default 45 s, as before) and the Intel
    // GPU is not published, so WindowServer waits GPUWrangler's 60 s as it always did in that mode.
    IOService *h0 = ifb_nvrm_head0();
    gIfbBootMonitor = h0 != nullptr;
    OSSafeReleaseNULL(h0);
    if (uint32_t w = gIfbBootMonitor ? ifb_knob("nullmoth-intelfb-delay", 0) : ifb_knob("nullmoth-intelfb-delay-own", 45)) {
        if (w > 120) w = 120;
        IFBLOG("waiting %u s before taking the panel (nullmoth-intelfb-delay)", w);
        ifb_trail("wait");
        IOSleep(w * 1000);
        ifb_trail("go");
    }
    bool ok = startReal(provider);
    ifb_trail(ok ? "ok" : "declined");
    // WindowServer's wait may end only when the panel is ready for it. With the autopipe that is after
    // the autopipe (ifb_autopipe), which must finish before WindowServer opens its framebuffers: published right here,
    // WindowServer opened the panel 0.2 s after start() and the Mac hung (both times).
    // In its own group (no monitor at boot) nothing is published: the 60 s wait, as before (see the delay above);
    // nullmoth-intelfb-gwready-own=1 publishes there too, untested.
    if (!ok) ifb_gwready(false);
    else if (!gIfbAutopipeOn && ifb_knob("nullmoth-intelfb-gwready-own", 0)) ifb_gwready(true);
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
    publishBacklight();   // intelfb-0020: before IOFramebuffer::start(), whose open() looks for "backlight"
    uint32_t dep = fSurfMap ? ifb_knob("nullmoth-intelfb-dep", 1) : 0;
    // dep=3 chooses at each boot, after the delay: NVRM's group when NVRMFB started a head 0 (a monitor on the
    // NVIDIA port at boot), its own group otherwise. Never two head 0s (WindowServer crashes)
    if (dep == 3) {
        IOService *h0 = ifb_nvrm_head0();
        dep = h0 ? 2 : 1;
        OSSafeReleaseNULL(h0);
        ifb_trail(dep == 2 ? "auto-nvrm" : "auto-own");
    }
    if (dep) {
        fOwnGroup = dep != 2;
        gIfbDepNvrm = !fOwnGroup;
        setProperty("IOFBDependentID", fOwnGroup ? 0x4E4D494E00000001ull : 0x4E56524D00000001ull, 64);
        // intelfb-0010: NVAccel makes only head 0 its primary framebuffer (fFB), and registers pipes (debug.nvaccelfb)
        // only once fFB is set; NVRMFB head 0 never starts here, so as head 4 the panel was never registered and the
        // 60 s headless check gave WindowServer up (RTX-8). In its own group the panel publishes as head 0
        // (nullmoth-intelfb-head=4: head 4, as in RTX-8).
        uint32_t head = fOwnGroup ? ifb_knob("nullmoth-intelfb-head", 0) : 4;
        setProperty("IOFBDependentIndex", head == 4 ? 4ull : 0ull, 32);
        ifb_trail(!fOwnGroup ? "dep-nvrm" : head == 4 ? "dep-own-h4" : "dep-own-h0");
    } else IFBLOG("the surface would not map write-combined, or nullmoth-intelfb-dep=0: NVIDIA rendering stays off for this panel");
    if (!IOFramebuffer::start(provider)) { ifb_trail("fb-start-failed"); if (fBacklight) { fBacklight->detach(this); OSSafeReleaseNULL(fBacklight); } OSSafeReleaseNULL(fSurfMap); OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false; }
    ifb_trail("fb-started");
    setProperty("NMIntelFBPipe", fPipe, 32);
    setProperty("NMIntelFBDisplayVersion", fVer, 32);
    setProperty("built-in", kOSBooleanTrue);   // the panel is the laptop's own (kConnectionFlags says so too)
    fVblAlways = ifb_knob("nullmoth-intelfb-vbl", 1) != 0;
    armVbl();
    ifb_trail(fVblAlways ? "vbl-always" : "vbl");
    // in NVRM's group too. Without fDev the panel's pipe has no scanout resource (intelfb-0016) and stays
    // inactive: a boot without it had pipe 1 "active 0", "Unable to find display pipe" and iop_ok_h4 0
    if ((fOwnGroup && ifb_knob("nullmoth-intelfb-head", 0) != 4) || gIfbDepNvrm) kapiInit();
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
        // in NVRM's group NVRM's auto-go has armed head 0 long before this panel exists, and nmx-intel's
        // auto.sh leaves an armed accelerator alone; nullmoth-intelfb-autopipe=0 leaves it to debug.* by hand
        if (gIfbDepNvrm && ifb_knob("nullmoth-intelfb-autopipe", 1)) ifb_autopipe_start();
    }
    ifb_ram_register(this);
    // the Type-C state the firmware left, before anything else runs on the display engine
    if (char *d = (char *)IOMallocZero(4096)) {
        tcDiagInto(d, 4096);
        for (char *line = d, *end; *line; line = end + 1) {
            if (!(end = strchr(line, '\n'))) { IFBLOG("tcdiag %s", line); break; }
            *end = 0; IFBLOG("tcdiag %s", line);
        }
        IOFree(d, 4096);
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
    if (gIfbRamFB == this) ifb_ram_register(nullptr);
    ramSet(0);
    for (unsigned b = 0; b < 2; b++) {
        if (fRam[b]) { fRam[b]->complete(); OSSafeReleaseNULL(fRam[b]); }
        if (fRamPages[b]) IODelete(fRamPages[b], unsigned long long, fRamCount);
        if (fOldPte[b]) IODelete(fOldPte[b], unsigned long long, fRamCount);
        fRamPages[b] = fOldPte[b] = nullptr;
    }
    kapiFree();
    if (fBacklight) { fBacklight->detach(this); OSSafeReleaseNULL(fBacklight); }
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
        OSIncrementAtomic(&gIfbScanAsks);
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
    if (fn && fn->isEqualTo(NMINTELFB_FN_RAM)) {
        NMIntelFBRamPages *r = (NMIntelFBRamPages *)p1;
        if (!r || r->version != NMINTELFB_RAM_VERSION) return kIOReturnBadArgument;
        const int mode = fRamMode;
        if (!mode) return kIOReturnNotReady;
        // mode 2: the buffer not on screen, once the last flip has reached the panel (else it may still be scanned)
        unsigned b = 0;
        if (mode == 2) { b = fFront ^ 1u; flipLatched(20000); }
        r->pitch = fRowBytes; r->pages = fRamPages[b]; r->count = fRamCount; r->length = (unsigned long long)fRowBytes * fHeight;
        r->kva = fRam[b]->getBytesNoCopy();
        return kIOReturnSuccess;
    }
    // intelfb-0019: NVAccel wrote the buffer whose page list is p1; show it from the next vblank
    if (fn && fn->isEqualTo(NMINTELFB_FN_RAM_FLIP)) {
        const unsigned b = fFront ^ 1u;
        if (fRamMode != 2 || p1 != (void *)fRamPages[b]) return kIOReturnSuccess;
        wr(PLANE_SURF(fPipe), fRamGgtt[b] | fSurfLow);
        fFront = b;
        OSIncrementAtomic64(&gIfbFlips);
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

// AppleGPUWrangler makes a GPU of every display-class PCI device and holds WindowServer's
// GPUWranglerWaitForPublish (60 s, from CoreDisplay) until each GPU has an accelerator, an AGDC or a framebuffer under
// it. The panel's framebuffer and AGDC sit under the NVIDIA GPU, so the Intel GPU never published and every boot waited
// the full 60 s. WindowServer lists the framebuffers once, when the wait ends, and does not add one
// that comes later (the NVIDIA GPU published at +20 s, the panel at +31.6 s). So an
// empty accelerator goes under the claim when the panel is ready for WindowServer (after the autopipe, see
// NMIntelFB::start): the Intel GPU publishes then, and the wait ends with the panel there. No IOAccelIndex, no Metal
// plugin: nothing renders on it.
// nullmoth-intelfb-gwready=0: not published (the 60 s wait comes back).
class NMIntelFBGPUReady : public IOAccelerator {
    OSDeclareDefaultStructors(NMIntelFBGPUReady)
};
OSDefineMetaClassAndStructors(NMIntelFBGPUReady, IOAccelerator)

static void ifb_gwready(bool panel)
{
    static SInt32 done;
    if (!gIfbClaim || !OSCompareAndSwap(0, 1, &done)) return;
    if (!ifb_knob("nullmoth-intelfb-gwready", 1)) { ifb_trail("gwready-off"); return; }
    NMIntelFBGPUReady *a = OSTypeAlloc(NMIntelFBGPUReady);
    bool up = a && a->init() && a->attach(gIfbClaim);
    if (up) a->registerService();
    OSSafeReleaseNULL(a);   // the registry keeps it while attached
    ifb_trail(up ? "gwready" : "gwready-fail");
    IFBLOG("GPUWrangler: Intel GPU %s (panel %s)", up ? "published" : "NOT published", panel ? "up" : "declined");
}

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
    retain(); gIfbClaim = this;   // NMIntelFB::start() publishes the Intel GPU under it
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
        // beside NVRMFB's head 0, list it first and the panel second, NVAccel's pipe order
        if (gIfbDepNvrm && !gIfbAgdcHead0) {
            IOService *h0 = ifb_nvrm_head0();
            if (h0 && !OSCompareAndSwapPtr(nullptr, h0, (void *volatile *)&gIfbAgdcHead0)) h0->release();
        }
        if (gIfbDepNvrm && gIfbAgdcHead0) {
            c->portMask = (1ull << 1) | (1ull << 2);
            c->countA = c->countB = c->countC = c->countD = 2;
            c->maxFramebuffers = 2; c->pciDevice = fPci; c->framebuffer[0] = gIfbAgdcHead0; c->framebuffer[1] = fFB;
            IFB_ONCE("agdc-980-2");
            return kIOReturnSuccess;
        }
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
        int why = 0, r;
        if (gIfbDepNvrm && gIfbAgdcHead0) {
            // entry 0 is head 0, answered from its NVRMTiming as NVRMAGDC does; entry 1 is the panel
            uint32_t t0[NVK5_TIMING_WORDS] = {}; unsigned long t0Bytes = 0;
            OSObject *o = gIfbAgdcHead0->copyProperty("NVRMTiming"); OSData *d = OSDynamicCast(OSData, o);
            if (d) { t0Bytes = d->getLength(); if (t0Bytes == sizeof t0) memcpy(t0, d->getBytesNoCopy(), sizeof t0); }
            OSSafeReleaseNULL(o);
            const struct nvk5_head h[2] = { { d ? t0 : nullptr, t0Bytes }, { gIfbK5, sizeof gIfbK5 } };
            int32_t idx = -1;
            r = nvk5_answer_heads(cmd, in, inSize, out, outSize ? *outSize : 0, h, 2, &why, &idx);
        } else r = nvk5_answer(cmd, in, inSize, out, outSize ? *outSize : 0, gIfbK5, sizeof gIfbK5, &why);
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

static int ifb_agdc_start(void)
{
    if (gIfbAgdcState == 1 || gIfbAgdcState == 2) return 0;
    if (!gIfbNubFB) return ENXIO;
    gIfbAgdcState = 2;
    thread_t t = nullptr;
    if (kernel_thread_start(ifb_agdc_up, nullptr, &t) != KERN_SUCCESS) { gIfbAgdcState = -5; return ENOMEM; }
    thread_deallocate(t);
    return 0;
}

static int ifb_agdc_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gIfbAgdcState, err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 1) return EINVAL;
    return ifb_agdc_start();
}

// NVRMFB's head 0 (an NVRMNVDAFramebuffer published with IOFBDependentIndex 0), retained, or null
static IOService *ifb_nvrm_head0(void)
{
    IOService *found = nullptr;
    OSDictionary *m = IOService::serviceMatching("NVRMNVDAFramebuffer");
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    OSSafeReleaseNULL(m);
    if (!it) return nullptr;
    while (OSObject *o = it->getNextObject()) {
        IOService *f = OSDynamicCast(IOService, o);
        OSNumber *n = f ? OSDynamicCast(OSNumber, f->getProperty("IOFBDependentIndex")) : nullptr;
        if (n && n->unsigned32BitValue() == 0) { f->retain(); found = f; break; }
    }
    it->release();
    return found;
}

// how many NVRMFB heads sit at IOFBDependentIndex 0, 1, ... without a gap (NVAccel registers them in that
// order and the panel after them)
static unsigned ifb_nvrm_heads(void)
{
    unsigned mask = 0, n = 0;
    OSDictionary *m = IOService::serviceMatching("NVRMNVDAFramebuffer");
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    OSSafeReleaseNULL(m);
    if (it) {
        while (OSObject *o = it->getNextObject()) {
            IOService *f = OSDynamicCast(IOService, o);
            OSNumber *i = f ? OSDynamicCast(OSNumber, f->getProperty("IOFBDependentIndex")) : nullptr;
            if (i && i->unsigned32BitValue() < 4) mask |= 1u << i->unsigned32BitValue();
        }
        it->release();
    }
    while (n < 4 && (mask >> n) & 1) n++;
    return n;
}

// NVAccel's debug.* knobs, from here. The walk does what `sysctl` does for a name: the debug node's children,
// through the kernel's sysctlbyname (kernel buffers, its own locking and checks; NVRM imports it too). A first
// version walked sysctl__debug_children itself and never found NVAccel's knobs on Tahoe.
extern "C" int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
// reads debug.<name> into *v and, with set, writes v first; the errno of sysctlbyname (ENOENT: no such knob)
static int ifb_knob_rw(const char *name, int *v, bool set)
{
    char full[64];
    snprintf(full, sizeof full, "debug.%s", name);
    int now = 0, want = *v;
    size_t len = sizeof now;
    int err = sysctlbyname(full, &now, &len, set ? &want : nullptr, set ? sizeof want : 0);
    *v = now;
    return err;
}

// the panel's fast copy with the HDMI as well, in the steps nmx-intel's auto.sh takes without
// it and that were checked by hand: NVAccel's direct copy, the RAM scanout (two buffers flipped unless
// nullmoth-accel-ram says 1 or 0), then the copy engine straight into it. Like auto.sh it starts once WindowServer's
// frames reach the panel; nullmoth-accel-fast=0 keeps the base copy. The kernel can write NVAccel's two knobs only in
// a build with the fix for the stale frames of the direct copy; with any other the base copy stays.
static void ifb_fastcopy(void)
{
    if (!ifb_knob("nullmoth-accel-fast", 1)) { ifb_trail("fast-off"); return; }
    int i = 0;
    for (; i < 900 && gIfbScanAsks < 10; i++) IOSleep(200);   // up to 3 minutes
    if (gIfbScanAsks < 10) { ifb_trail("fast-noframes"); IFBLOG("fast copy: only %d copied frames in 3 min, base copy", (int)gIfbScanAsks); return; }
    IOSleep(5000);
    int mc = 1, d1 = 1, d2 = 2, e1 = ifb_knob_rw("nvaccel_iop_memcpy", &mc, true), e2 = -2, e3 = -2, e4 = -2;
    if (!e1) e2 = ifb_knob_rw("nvaccel_iop_direct", &d1, true);
    if (e1 || e2) {
        ifb_trail("fast-nokern");
        IFBLOG("fast copy: NVAccel did not take its knobs from the kernel (memcpy err %d, direct err %d): base copy", e1, e2);
        return;
    }
    IOSleep(2000);
    uint32_t ram = ifb_knob("nullmoth-accel-ram", 2);
    int rv = ram > 2 ? 2 : (int)ram;
    if (rv) e3 = ifb_knob_rw("nmintelfb_ram", &rv, true);
    if (!e3) e4 = ifb_knob_rw("nvaccel_iop_direct", &d2, true);
    ifb_trail(!e4 ? "fast-on" : "fast-direct1");
    IFBLOG("fast copy after %d copied frames: memcpy 1, direct 1; ram scanout %u (err %d); direct 2 (err %d)",
           (int)gIfbScanAsks, ram > 2 ? 2u : ram, e3, e4);
}

// the pipe and AGDC setup a script did by hand, now at boot and before WindowServer opens its framebuffers
// (measured at boot: the panel publishes about 17 s before), so no WindowServer restart is needed
static void ifb_autopipe(void *, wait_result_t)
{
    int pub = 0, igpu = 1, fb = 3, nv = 0, nvst = -9, e0 = -2, e1 = -2, e2 = -2, e3 = -2, i;
    for (i = 0; i < 100; i++) {   // NVAccel counts the panel as published: bit 4
        if ((e0 = ifb_knob_rw("nvaccel_heads_published", &pub, false)) == 0 && (pub & 0x10)) break;
        IOSleep(200);
    }
    if (pub & 0x10) {
        e1 = ifb_knob_rw("nvaccel_igpu", &igpu, true);
        e2 = ifb_knob_rw("nvaccelfb", &fb, true);
        // NVRM's auto-go starts NullMoth's AGDC for head 0 at boot, and WindowServer sends its link and plane questions
        // (0x2001, K5) there: with an AGDC of ours beside it the panel was never configured and showed only the cursor
        // and NullMoth's cannot be terminated once anything opened it ("terminate -> 0"). NVRMAGDC with
        // this change lists the panel after the NVIDIA heads, so it is the only AGDC. nullmoth-intelfb-ownagdc=1 brings
        // back ours (and nullmoth-intelfb-agdcsolo=1 with it tries to terminate NullMoth's first).
        if (ifb_knob("nullmoth-intelfb-ownagdc", 0)) {
            if (ifb_knob("nullmoth-intelfb-agdcsolo", 1)) {
                e3 = ifb_knob_rw("nvrmfb_agdc", &nv, true);
                for (int j = 0; j < 30; j++) {   // 2 = terminated, 0 = never up
                    if (ifb_knob_rw("nvrmfb_agdc", &nvst, false) == 0 && nvst != 1) break;
                    IOSleep(100);
                }
            }
            ifb_agdc_start();
            for (i = 0; i < 30 && gIfbAgdcState == 2; i++) IOSleep(100);
        }
    }
    // judged by the two writes: debug.nvaccel_heads_piped has no CTLFLAG_KERN, so the kernel cannot read it (it read 0
    // while it was 0x11); NVAccel logs "index 4 -> pipe N" itself
    ifb_trail(!e1 && !e2 ? "autopipe" : "autopipe-fail");
    IFBLOG("autopipe: published %#x (err %d) after %d tries, igpu was %d (err %d), nvaccelfb was %d (err %d), "
           "NullMoth AGDC was %d now %d (err %d), ours %d", pub, e0, i, igpu, e1, fb, e2, nv, nvst, e3, gIfbAgdcState);
    // only now may WindowServer open its framebuffers; nullmoth-intelfb-gwdelay (s, default 3) keeps a
    // margin. ifb_fastcopy waits for WindowServer's frames, so it follows.
    if (uint32_t m = ifb_knob("nullmoth-intelfb-gwdelay", 3)) IOSleep((m > 30 ? 30 : m) * 1000);
    ifb_gwready(true);
    if (!e1 && !e2) ifb_fastcopy();
    thread_terminate(current_thread());
}
static void ifb_autopipe_start(void)
{
    thread_t t = nullptr;
    if (kernel_thread_start(ifb_autopipe, nullptr, &t) == KERN_SUCCESS) { gIfbAutopipeOn = true; thread_deallocate(t); }
    else ifb_trail("autopipe-nothread");
}
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_agdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, nullptr, 0,
            ifb_agdc_sysctl, "I", "1 = list the Intel panel in an AppleGraphicsDeviceControl on the NVIDIA GPU (1 up, 2 starting, < 0 failed)");
static void ifb_agdc_register(bool on)
{
    if (on) sysctl_register_oid(&sysctl__debug_nmintelfb_agdc); else sysctl_unregister_oid(&sysctl__debug_nmintelfb_agdc);
}

// intelfb-0017. GGTT entries live behind the register BAR at half its size (8 MiB here, i915 gen6_gttadr_offset), one
// 64-bit PTE per 4 KiB: phys | present (gen8_ggtt_pte_encode). Display 11+ maps them uncached, as our BAR0 map is;
// GFX_FLSH_CNTL_GEN6 (0x101008) = 1 flushes the GGTT TLBs as the GuC path does.
void NMIntelFB::ramPtes(unsigned b, bool ours)
{
    volatile uint64_t *g = (volatile uint64_t *)(fRegs + 0x800000) + (fRamGgtt[b] >> 12);
    for (unsigned i = 0; i < fRamCount; i++) g[i] = ours ? (fRamPages[b][i] | 1ull) : fOldPte[b][i];
    *(volatile uint32_t *)(fRegs + 0x101008) = 1;
    (void)g[fRamCount - 1];   // posts the writes
}

// intelfb-0019: PLANE_SURFLIVE (PLANE_SURF + 0x10) is the address the plane is scanning now; equal to the front
// buffer once the last flip latched at a vblank. Waits up to wantUs.
bool NMIntelFB::flipLatched(unsigned wantUs)
{
    const uint32_t want = fRamGgtt[fFront];
    if ((rd(PLANE_SURF(fPipe) + 0x10) & ~0xFFFu) == want) return true;
    OSIncrementAtomic64(&gIfbFlipWaits);
    const uint64_t t0 = mach_absolute_time();
    for (unsigned us = 0; us < wantUs; us += 1000) {
        IOSleep(1);
        if ((rd(PLANE_SURF(fPipe) + 0x10) & ~0xFFFu) == want) {
            uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
            OSAddAtomic64((SInt64)(ns / 1000), &gIfbFlipWaitUs);
            return true;
        }
    }
    OSIncrementAtomic64(&gIfbFlipLate);
    return false;
}

// Allocates buffer b if needed, fills it with the picture on the panel, saves the GGTT entries at fRamGgtt[b] and
// points them at it. 0 or < 0 as ramSet.
int NMIntelFB::ramMap(unsigned b)
{
    const unsigned long long len = (unsigned long long)fRowBytes * fHeight;
    const unsigned n = fRamCount;
    if (fMMIO->getLength() < 0x800000ull + ((fRamGgtt[b] >> 12) + (unsigned long long)n) * 8) return -2;
    if (!fRam[b]) {
        // physically contiguous when it can be, so the copy engine makes one run
        const IOOptionBits o = kIODirectionInOut;
        IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, o | kIOMemoryPhysicallyContiguous, (vm_size_t)n * 4096, 0x7FFFFFF000ull);
        if (!m) m = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, o, (vm_size_t)n * 4096, 0x7FFFFFF000ull);
        if (!m) return -3;
        if (m->prepare() != kIOReturnSuccess) { m->release(); return -3; }
        unsigned long long *pg = IONew(unsigned long long, n), *old = IONew(unsigned long long, n);
        bool ok = pg && old;
        for (unsigned i = 0; ok && i < n; i++) {
            IOByteCount sl = 0;
            pg[i] = m->getPhysicalSegment((IOByteCount)i * 4096, &sl, kIOMemoryMapperNone);
            ok = pg[i] && !(pg[i] & 0xFFF);
        }
        if (!ok) { if (pg) IODelete(pg, unsigned long long, n); if (old) IODelete(old, unsigned long long, n); m->complete(); m->release(); return -4; }
        unsigned runs = 1; for (unsigned i = 1; i < n; i++) if (pg[i] != pg[i - 1] + 4096) runs++;
        fRam[b] = m; fRamPages[b] = pg; fOldPte[b] = old;
        IFBLOG("ram scanout: buffer %u, %u pages at 0x%llx in %u run(s)", b, n, pg[0], runs);
    }
    volatile uint64_t *g = (volatile uint64_t *)(fRegs + 0x800000) + (fRamGgtt[b] >> 12);
    for (unsigned i = 0; i < n; i++) fOldPte[b][i] = g[i];
    // the picture on the panel first, so the switch shows no black frame (buffer 0 reads the aperture: slow, once)
    memcpy(fRam[b]->getBytesNoCopy(), b ? fRam[0]->getBytesNoCopy() : (const void *)fSurfMap->getVirtualAddress(), (size_t)len);
    // the display does not snoop CPU caches: push the copy out to RAM (the copy engine's writes reach RAM themselves)
    for (size_t o = 0; o < len; o += 64) __asm__ volatile("clflush %0" : : "m"(((volatile const char *)fRam[b]->getBytesNoCopy())[o]));
    __asm__ volatile("mfence" ::: "memory");
    ramPtes(b, true);
    for (unsigned i = 0; i < n; i++) if (g[i] != (fRamPages[b][i] | 1ull)) {
        ramPtes(b, false);
        IFBLOG("ram scanout: GGTT entry %u of buffer %u read back 0x%llx, not 0x%llx - old entries restored", i, b, (unsigned long long)g[i], fRamPages[b][i] | 1ull);
        return -5;
    }
    IFBLOG("ram scanout: GGTT 0x%x..0x%x now buffer %u (old PTE[0] 0x%llx)", fRamGgtt[b], fRamGgtt[b] + n * 4096, b, fOldPte[b][0]);
    return 0;
}

// 0 firmware pages, 1 one RAM buffer, 2 two flipped; returns the mode now, or < 0 why it would not go up (the mode
// reached so far stays)
int NMIntelFB::ramSet(int mode)
{
    if (mode < 0 || mode > 2) return -1;
    if (mode && (!fRegs || !fSurfMap || fVer < 12)) return -1;
    const unsigned long long len = (unsigned long long)fRowBytes * fHeight;
    if (mode && !fRamCount) {
        fRamCount = (unsigned)((len + 4095) >> 12);
        fRamGgtt[0] = fSurf;
        // linear planes need 256 KiB alignment (i915 intel_linear_alignment); 2 MiB past buffer 0
        fRamGgtt[1] = (uint32_t)((fSurf + (unsigned long long)fRamCount * 4096 + 0x1FFFFF) & ~0x1FFFFFull);
        fSurfLow = rd(PLANE_SURF(fPipe)) & 0xFFFu;
    }
    while (fRamMode > mode) {
        if (fRamMode == 2) {
            // back to buffer 0 at the firmware's offset before buffer 1's entries go
            fRamMode = 1;
            wr(PLANE_SURF(fPipe), fRamGgtt[0] | fSurfLow); fFront = 0;
            flipLatched(50000);
            ramPtes(1, false);
            IFBLOG("ram scanout: flipping off, one buffer"); ifb_trail("flip-off");
        } else {
            fRamMode = 0;
            ramPtes(0, false);
            IFBLOG("ram scanout off: GGTT back to the firmware's pages"); ifb_trail("ram-off");
        }
    }
    while (fRamMode < mode) {
        const unsigned b = (unsigned)fRamMode;
        if (int r = ramMap(b)) return r;
        if (b == 0) { fFront = 0; fRamMode = 1; ifb_trail("ram-on"); }
        else { fFront = 0; fRamMode = 2; IFBLOG("ram scanout: flipping between GGTT 0x%x and 0x%x", fRamGgtt[0], fRamGgtt[1]); ifb_trail("flip-on"); }
    }
    return fRamMode;
}

static int gIfbRamState;
static volatile SInt32 gIfbRamBusy;
static int ifb_ram_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gIfbRamState, err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0 || v > 2) return EINVAL;
    NMIntelFB *fb = gIfbRamFB;
    if (!fb) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gIfbRamBusy)) return EBUSY;
    gIfbRamState = fb->ramSet(v);
    gIfbRamBusy = 0;
    return gIfbRamState < 0 ? EIO : 0;
}
static int ifb_regs_sysctl SYSCTL_HANDLER_ARGS
{
    char b[200] = "no panel";
    if (NMIntelFB *fb = gIfbRamFB) fb->regsInto(b, sizeof b);
    return sysctl_handle_string(oidp, b, sizeof b, req);
}
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_regs, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
            ifb_regs_sysctl, "A", "intelfb-0018: pipe, plane, panel power, backlight PWM and the first scan-out PTE");
// the Type-C/DBUF read-only dump
static int ifb_tcdiag_sysctl SYSCTL_HANDLER_ARGS
{
    char *b = (char *)IOMallocZero(4096);
    if (!b) return ENOMEM;
    if (NMIntelFB *fb = gIfbRamFB) fb->tcDiagInto(b, 4096); else strlcpy(b, "no panel\n", 4096);
    int err = sysctl_handle_string(oidp, b, 4096, req);
    IOFree(b, 4096);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_tcdiag, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
            ifb_tcdiag_sysctl, "A", "Type-C ports, FIA, power wells, PLLs and DBUF (reads only)");
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_ram, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, nullptr, 0,
            ifb_ram_sysctl, "I", "1 = the panel scans out of RAM that NVAccel's copy engine writes, 2 = two such buffers flipped at vblank (0 off, < 0 failed)");
SYSCTL_QUAD(_debug, OID_AUTO, nmintelfb_flips, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&gIfbFlips, "intelfb-0019: flips to the buffer NVAccel just wrote");
SYSCTL_QUAD(_debug, OID_AUTO, nmintelfb_flip_waits, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&gIfbFlipWaits, "frames that waited for the previous flip to reach the panel");
SYSCTL_QUAD(_debug, OID_AUTO, nmintelfb_flip_wait_us, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&gIfbFlipWaitUs, "microseconds spent in those waits");
SYSCTL_QUAD(_debug, OID_AUTO, nmintelfb_flip_late, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&gIfbFlipLate, "waits given up after 20 ms (that frame may tear)");
static void ifb_ram_register(NMIntelFB *fb)
{
    sysctl_oid *o[] = { &sysctl__debug_nmintelfb_ram, &sysctl__debug_nmintelfb_regs, &sysctl__debug_nmintelfb_tcdiag, &sysctl__debug_nmintelfb_flips,
                        &sysctl__debug_nmintelfb_flip_waits, &sysctl__debug_nmintelfb_flip_wait_us, &sysctl__debug_nmintelfb_flip_late };
    if (fb && !gIfbRamFB) { gIfbRamFB = fb; for (sysctl_oid *x : o) sysctl_register_oid(x); }
    else if (!fb && gIfbRamFB) { for (sysctl_oid *x : o) sysctl_unregister_oid(x); gIfbRamFB = nullptr; }
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
