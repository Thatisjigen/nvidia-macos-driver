/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Opt-in D3cold for a discrete laptop GPU: the firmware's power methods switch it off when it is idle and on again when
 * it is wanted (nv_rtd3_policy.h has the rules). Nothing here runs unless the boot argument nvrtd3 is set, to 1 (power off
 * only a GPU nothing is attached to) or 2 (power off by activity): without it no timer is armed, no switch is registered
 * and no firmware method is called. With it the rule and its times can be changed on a running system (debug.nvrm_rtd3*). The power resource's ACPI path may come from
 * the boot argument string nvrtd3res (for example \_SB.PCI0.PEG1.PG01), because the _PR3 package's references are not
 * readable through the registry on every machine; without it, or where the ACPI family refuses it, the GPU's own node is
 * asked the way its firmware provides for NVIDIA's driver.
 *
 * The callers (the driver's idle and wake paths, owned by the power-management code) ask nv_rtd3_may_off first.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <string.h>
#include "nv_rtd3_policy.h"

// kprintf alone does not reach the system log, where a power switch that went wrong has to be read afterwards
#define RLOG(fmt, ...) do { kprintf("NVRM-xnu: RTD3 " fmt "\n", ##__VA_ARGS__); IOLog("NVRM-xnu: RTD3 " fmt "\n", ##__VA_ARGS__); } while (0)

static struct nv_rtd3 g_rtd3;
static char g_res[96];
static int g_init;
static int g_asked;   // the boot argument asked for runtime D3: the rule may change later, this does not

static uint64_t now_ms(void) { uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000ULL; }

static void nv_rtd3_init(void)
{
    if (g_init) return;
    g_init = 1;
    uint32_t v = 0;
    if (PE_parse_boot_argn("nvrtd3", &v, sizeof v) && (v == NV_RTD3_NO_CLIENT || v == NV_RTD3_BY_ACTIVITY)) g_rtd3.enabled = (int)v;
    g_asked = g_rtd3.enabled != NV_RTD3_NEVER;
    if (!PE_parse_boot_argn("nvrtd3res", g_res, sizeof g_res) || g_res[0] != '\\') g_res[0] = 0;
    // Which way reaches the power is looked for once the adapter is up (nv_rtd3_reach): being asked for is enough here.
    g_rtd3.have_resource = g_rtd3.enabled;
    if (g_rtd3.enabled) RLOG("rule %d, power resource %s", g_rtd3.enabled, g_res[0] ? g_res : "not given: the GPU node's own methods");
}

/* ---- asking the firmware.
 * A power resource is not a device, and the ACPI family resolves an object name against the node it is asked through.
 * Which node and which spelling of the path reach the resource is found by reading its _STA, which changes nothing, and
 * is then kept. Where none does, the GPU's own node still has the pair its firmware carries for NVIDIA's driver: _DSM
 * function 0x1A with the power-control flags, after which _PS3 runs the resource's _OFF, and _PS0 its _ON.
 * nodes[]: the root port's ACPI node, the GPU's, and the first one up the provider chain (the host bridge's). */
enum { NV_RTD3_VIA_NONE = 0, NV_RTD3_VIA_RESOURCE = 1, NV_RTD3_VIA_OPTIMUS = 2 };
enum { NV_RTD3_NODE_PORT = 0, NV_RTD3_NODE_GPU = 1, NV_RTD3_NODE_CHAIN = 2, NV_RTD3_NODES = 3 };
static const char *const g_node_name[NV_RTD3_NODES] = { "root port", "GPU", "host bridge" };
static int g_via, g_via_node = -1, g_looked;
static int g_how;                 // 0 the resource if it can be reached, else the pair on the GPU node; 1 the resource only; 2 the pair only
static char g_via_name[112];      // the resource as the chosen node sees it

// NVIDIA's Optimus _DSM, function 0x1A. Bit 0 of the argument says the flags changed and bits 24-25 are the power control
// asked for (3: _PS3 may cut the power). Bits 3-4 of the answer are 3 while the firmware holds the GPU on, 0 once it is off.
static IOReturn nv_rtd3_optimus_caps(IOACPIPlatformDevice *gpu, bool ask_power_control, UInt32 *answer)
{
    static const UInt8 uuid[16] = { 0xF8, 0xD8, 0x86, 0xA4, 0xDA, 0x0B, 0x1B, 0x47, 0xA7, 0x2B, 0x60, 0x42, 0xA6, 0xB5, 0xBE, 0xE0 };
    const UInt8 flags[4] = { (UInt8)(ask_power_control ? 0x01 : 0x00), 0x00, 0x00, (UInt8)(ask_power_control ? 0x03 : 0x00) };
    OSObject *params[4] = { OSData::withBytes(uuid, sizeof uuid), OSNumber::withNumber(0x100ULL, 32), OSNumber::withNumber(0x1AULL, 32),
                            OSData::withBytes(flags, sizeof flags) };
    OSObject *res = nullptr;
    IOReturn r = kIOReturnNoMemory;
    if (params[0] && params[1] && params[2] && params[3]) r = gpu->evaluateObject("_DSM", &res, params, 4);
    for (int i = 0; i < 4; i++) OSSafeReleaseNULL(params[i]);
    if (r == kIOReturnSuccess) {
        OSData *d = OSDynamicCast(OSData, res);   // an integer (0x80000001) is the firmware's answer to what it does not know
        if (d && d->getLength() >= 4) {
            const UInt8 *v = (const UInt8 *)d->getBytesNoCopy();
            *answer = (UInt32)v[0] | ((UInt32)v[1] << 8) | ((UInt32)v[2] << 16) | ((UInt32)v[3] << 24);
        } else r = kIOReturnUnsupported;
    }
    OSSafeReleaseNULL(res);
    return r;
}

/* The firmware's power sequence moves two pads of the root port, the power enable and the reset, which it takes from
 * packages it fills with statements written outside any method (PWRG and RSTG in Intel's reference code). An interpreter
 * that runs only what is inside methods and If blocks leaves them empty: _OFF then parks the link and leaves the GPU
 * powered and out of reset, _ON returns at once because the power-good line never fell, and the GPU does not answer again
 * until the machine restarts. Where those packages exist and are empty, the power is not switched at all. */
static bool nv_rtd3_pads_empty(IOACPIPlatformDevice *port)
{
    if (!port) return false;
    static const char *const pkg[] = { "PWRG", "RSTG" };
    int seen = 0, empty = 0;
    for (const char *name : pkg) {
        if (port->validateObject(name) != kIOReturnSuccess) continue;
        OSObject *o = nullptr;
        if (port->evaluateObject(name, &o) == kIOReturnSuccess) {
            OSArray *a = OSDynamicCast(OSArray, o);
            OSNumber *pad = a && a->getCount() ? OSDynamicCast(OSNumber, a->getObject(0)) : nullptr;
            seen++;
            if (!pad || pad->unsigned64BitValue() == 0) empty++;
        }
        OSSafeReleaseNULL(o);
    }
    if (seen) RLOG("reach: the root port's power-enable and reset pads are %s", empty ? "EMPTY" : "set");
    return empty > 0;
}

extern "C" int nv_rtd3_reach(IOACPIPlatformDevice *const *nodes)
{
    nv_rtd3_init();
    g_via = NV_RTD3_VIA_NONE; g_via_node = -1; g_via_name[0] = 0; g_looked = 1;
    if (!nodes || !g_rtd3.enabled) return 0;
    if (nv_rtd3_pads_empty(nodes[NV_RTD3_NODE_PORT])) {
        RLOG("the firmware would not cut the power on this system (no table fills its pads): the GPU will not be switched off");
        return 0;
    }
    // every tail of the path, as it stands and (the two shortest) from one scope below, then the path whole
    enum { kSpellings = 12 };
    static char sp[kSpellings][sizeof g_via_name];
    int n = 0, found_node = -1, found_sp = -1, refused = 0;
    if (g_res[0] && g_how != 2) {
        const char *tail[8]; int nt = 0;
        for (const char *p = g_res; *p; p++) if ((p == g_res && *p == '\\') || *p == '.') { if (nt < 8) tail[nt++] = p + 1; }
        for (int i = nt - 1; i >= 0 && n + 2 < kSpellings; i--) {
            strlcpy(sp[n++], tail[i], sizeof sp[0]);
            if (i >= nt - 2) snprintf(sp[n++], sizeof sp[0], "^%s", tail[i]);
        }
        strlcpy(sp[n++], g_res, sizeof sp[0]);
    }
    for (int s = 0; s < n && found_node < 0; s++)
        for (int k = 0; k < NV_RTD3_NODES && found_node < 0; k++) {
            if (!nodes[k]) continue;
            char name[128]; UInt32 v = 0xffffffffU;
            snprintf(name, sizeof name, "%s._STA", sp[s]);
            if (nodes[k]->evaluateInteger(name, &v) == kIOReturnSuccess) {
                RLOG("reach: %s node, %s -> %u", g_node_name[k], name, (unsigned)v);
                found_node = k; found_sp = s;
            } else refused++;
        }
    if (n && found_node < 0) RLOG("reach: the ACPI family refused the resource's _STA %d time(s), in %d spelling(s) on every node", refused, n);
    bool pair = false;
    if (IOACPIPlatformDevice *gpu = nodes[NV_RTD3_NODE_GPU]) {
        UInt32 caps = 0;
        const IOReturn dsm = gpu->validateObject("_DSM"), ps3 = gpu->validateObject("_PS3"), ps0 = gpu->validateObject("_PS0");
        const IOReturn c = dsm == kIOReturnSuccess ? nv_rtd3_optimus_caps(gpu, false, &caps) : dsm;
        pair = dsm == kIOReturnSuccess && ps3 == kIOReturnSuccess && ps0 == kIOReturnSuccess && c == kIOReturnSuccess;
        RLOG("reach: GPU node has _DSM 0x%x, _PS3 0x%x, _PS0 0x%x; _DSM function 0x1A -> 0x%x, answer 0x%08x", dsm, ps3, ps0, c, (unsigned)caps);
    } else RLOG("reach: the GPU has no ACPI node");
    if (found_node >= 0) { g_via = NV_RTD3_VIA_RESOURCE; g_via_node = found_node; strlcpy(g_via_name, sp[found_sp], sizeof g_via_name); }
    else if (pair && g_how != 1) g_via = NV_RTD3_VIA_OPTIMUS;
    if (g_via == NV_RTD3_VIA_RESOURCE) RLOG("the power is switched through the resource: %s on the %s node", g_via_name, g_node_name[g_via_node]);
    else if (g_via == NV_RTD3_VIA_OPTIMUS) RLOG("the power is switched through the GPU node: _DSM 0x1A, then _PS3, and _PS0 to give it back");
    else RLOG("no way to the GPU's power was found: the GPU will not be suspended");
    return g_via != NV_RTD3_VIA_NONE;
}

static bool nv_rtd3_firmware(IOACPIPlatformDevice *const *nodes, bool on, bool quiet)
{
    // The firmware's power methods are called only when asked for (a method of this kind behaves differently on every
    // board), or to give the power back to a GPU this code switched off.
    if (!g_rtd3.enabled && g_rtd3.state != NV_RTD3_OFF) return false;
    if (!nodes) return false;
    if (g_via == NV_RTD3_VIA_RESOURCE && g_via_node >= 0 && nodes[g_via_node]) {
        IOACPIPlatformDevice *a = nodes[g_via_node];
        char name[128], sta[128]; UInt32 v = 0xffffffffU;
        snprintf(name, sizeof name, "%s.%s", g_via_name, on ? "_ON" : "_OFF");
        snprintf(sta, sizeof sta, "%s._STA", g_via_name);
        OSObject *o = nullptr;
        const uint64_t t0 = now_ms();
        const IOReturn r = a->evaluateObject(name, &o);
        const uint64_t ms = now_ms() - t0;
        OSSafeReleaseNULL(o);
        // _STA is the resource's own word on whether the GPU has power: on most boards it reads the power-good line
        const IOReturn rs = a->evaluateInteger(sta, &v);
        if (!quiet) RLOG("%s on the %s node -> 0x%x in %llu ms, _STA %s%u", name, g_node_name[g_via_node], r, ms,
                         rs == kIOReturnSuccess ? "" : "unreadable, ", rs == kIOReturnSuccess ? (unsigned)v : (unsigned)rs);
        return r == kIOReturnSuccess;
    }
    if (g_via == NV_RTD3_VIA_OPTIMUS && nodes[NV_RTD3_NODE_GPU]) {
        IOACPIPlatformDevice *a = nodes[NV_RTD3_NODE_GPU];
        UInt32 asked = 0, after = 0;
        if (!on) {
            const IOReturn rd = nv_rtd3_optimus_caps(a, true, &asked);
            if (rd != kIOReturnSuccess) { RLOG("_DSM 0x1A asking for power control -> 0x%x: _PS3 not called", rd); return false; }
        }
        const uint64_t t0 = now_ms();
        const IOReturn r = a->evaluateObject(on ? "_PS0" : "_PS3");
        const uint64_t ms = now_ms() - t0;
        const IOReturn ra = nv_rtd3_optimus_caps(a, false, &after);
        const bool held_on = ((after >> 3) & 3) != 0;
        if (!quiet) RLOG("%s on the GPU node -> 0x%x in %llu ms; the firmware then answers 0x%08x (0x%x): it holds the GPU %s",
                         on ? "_PS0" : "_PS3", r, ms, (unsigned)after, ra,
                         ra != kIOReturnSuccess ? "in a state it does not tell" : held_on ? "on" : "off");
        if (r != kIOReturnSuccess) return false;
        return on ? true : (ra == kIOReturnSuccess && !held_on);
    }
    if (!quiet) RLOG("%s: no way to the GPU's power", on ? "power-on" : "power-off");
    return false;
}

extern "C" int nv_rtd3_may_off(int rm_suspended, int heads_lit, int clients, unsigned idle_s)
{
    nv_rtd3_init();
    return nv_rtd3_may_power_off(&g_rtd3, rm_suspended, heads_lit, clients, idle_s);
}

extern "C" int nv_rtd3_reachable(void) { nv_rtd3_init(); return nv_rtd3_gpu_reachable(&g_rtd3); }

extern "C" int nv_rtd3_power_off(IOACPIPlatformDevice *const *nodes)
{
    nv_rtd3_init();
    if (!nodes || !g_rtd3.enabled || !g_rtd3.have_resource || g_rtd3.state != NV_RTD3_ON) return 0;
    bool ok = nv_rtd3_firmware(nodes, false, false);
    nv_rtd3_off_done(&g_rtd3, ok);
    return ok;
}

// The firmware's part of the way back. The GPU counts as on only once the caller has seen it answer on the bus, so the
// state is the monitor's to change, not this function's: a power-on the GPU did not follow is tried again.
extern "C" int nv_rtd3_power_on(IOACPIPlatformDevice *const *nodes, int quiet)
{
    nv_rtd3_init();
    if (!nodes || g_rtd3.state != NV_RTD3_OFF) return 1;
    return nv_rtd3_firmware(nodes, true, quiet != 0);
}

/* ---- the monitor: a thread call that polls once a second while the GPU is on and four times a second while it is off,
 * so the first client or framebuffer after power-off wakes it promptly (calls that arrive meanwhile wait at the RM gate
 * the suspend closed). Armed by the driver once the adapter is up, and only when RTD3 is asked for. */
#include <kern/thread_call.h>
static thread_call_t g_call;
static struct nv_rtd3_ops g_ops;
static unsigned g_idle;
static volatile int g_stop;
// One poll at a time: a thread call entered again while it runs starts on a second thread, and a poll can be inside the
// suspend or the resume for seconds. System sleep takes the same lock before it touches the GPU.
static IOLock *g_lock;
// From the system's sleep to the end of its wake's resume the GPU belongs to the system-sleep path: the monitor stands
// still, so the resume may run on a thread of its own (NVRM::resumeOnOwnThread) without a poll suspending it meanwhile.
static volatile SInt32 g_sys_sleep;
static char g_why[48] = "-";          // what the last power-on was for
static volatile SInt32 g_force;       // debug.nvrm_rtd3_now=1: the idle time counts as served, once
static volatile SInt32 g_wake;        // the GPU was asked for while it is off
static struct {
    unsigned offs, backs, off_failed, on_waits, notifications, last_notification, heavy;
    uint64_t off_at, off_ms;
} g_stat;

static void nv_rtd3_kick(void) { if (g_call && !g_stop) thread_call_enter(g_call); }

/* Something needs a GPU that is off: a call that waits at the RM gate, a client that goes to wait for an event, a display
 * plugged in, the sysctl. The monitor gives the power back at its next poll, which this starts at once. A request that
 * comes while the monitor is still switching the GPU off is kept, and brings it back as soon as the power is off: a
 * display plugged in during those seconds would otherwise stay dark. The monitor drops one it finds with the GPU on. */
extern "C" void nv_rtd3_want(const char *why)
{
    if (OSCompareAndSwap(0, 1, &g_wake)) strlcpy(g_why, why ? why : "?", sizeof g_why);
    if (g_rtd3.state == NV_RTD3_OFF) nv_rtd3_kick();
}

static int nv_rtd3_wanted(void *)
{
    if (g_wake) return 1;
    if (g_ops.wanted && g_ops.wanted(g_ops.ctx)) { strlcpy(g_why, "a call into the RM", sizeof g_why); return 1; }
    return 0;
}
static int nv_rtd3_none(void *) { return 0; }

static void nv_rtd3_account(int r)
{
    if (r == NV_RTD3_TICK_POWERED_OFF) {
        g_stat.offs++; g_stat.off_at = now_ms(); g_stat.on_waits = 0;
        RLOG("GPU powered off (#%u)", g_stat.offs);
    } else if (r == NV_RTD3_TICK_OFF_FAILED) {
        g_stat.off_failed++;
        RLOG("power-off failed, GPU resumed and left on");
    } else if (r == NV_RTD3_TICK_WOKE) {
        const uint64_t ms = now_ms() - g_stat.off_at;
        g_stat.backs++; g_stat.off_ms += ms;
        RLOG("GPU powered on after %llu s off, for: %s", ms / 1000ULL, g_why);
    } else if (r == NV_RTD3_TICK_WAKE_FAILED) {
        // asked again at every poll until the GPU answers or is given up (NVRM::rtd3PowerOn): a few lines, then one a minute
        if (g_stat.on_waits++ < 3 || g_stat.on_waits % 240 == 0) RLOG("power-on asked for, the GPU does not answer yet (%u)", g_stat.on_waits);
    }
    if (r == NV_RTD3_TICK_HEAVY) g_stat.heavy++;   // NVRM's question logs the figures
    if (r == NV_RTD3_TICK_WOKE || r == NV_RTD3_TICK_OFF_FAILED) OSCompareAndSwap(1, 0, &g_wake);
}

// Give the power back now, whatever the rule says, and wait until the GPU answers or is given up. Under g_lock.
static int nv_rtd3_back_now(void)
{
    if (g_rtd3.state != NV_RTD3_OFF || !g_ops.acpi_on) return NV_RTD3_TICK_NONE;
    int ok = 0;
    for (int i = 0; i < 200 && !(ok = g_ops.acpi_on(g_ops.ctx)); i++) IOSleep(250);   // the pace NVRM::rtd3PowerOn expects
    nv_rtd3_on_done(&g_rtd3, ok);
    if (!ok) return NV_RTD3_TICK_WAKE_FAILED;
    g_ops.resume(g_ops.ctx);
    g_idle = 0;
    return NV_RTD3_TICK_WOKE;
}

static void nv_rtd3_poll(thread_call_param_t, thread_call_param_t)
{
    if (g_stop || !g_lock) return;
    if (!IOLockTryLock(g_lock)) {   // a poll that still runs, or the system sleep: look again shortly
        uint64_t dl; clock_interval_to_deadline(250, kMillisecondScale, &dl);
        if (!g_stop) thread_call_enter_delayed(g_call, dl);
        return;
    }
    int r = NV_RTD3_TICK_NONE;
    const bool sys = g_sys_sleep != 0;   // the system-sleep path owns the GPU: nothing until nv_rtd3_after_system_wake
    if (sys) {
    } else if (!g_rtd3.enabled || g_wake) {
        // switched off while the GPU has no power, or asked for by name: the same way back for both rules
        if (!g_rtd3.enabled && g_rtd3.state == NV_RTD3_OFF) strlcpy(g_why, "RTD3 switched off", sizeof g_why);
        r = nv_rtd3_back_now();
        if (g_rtd3.state == NV_RTD3_ON) OSCompareAndSwap(1, 0, &g_wake);   // also when it was on already
    } else {
        struct nv_rtd3_ops o = g_ops;
        o.wanted = g_ops.wanted ? nv_rtd3_wanted : nullptr;
        if (g_rtd3.state == NV_RTD3_ON && !g_looked && g_ops.reach) g_ops.reach(g_ops.ctx);
        if (g_rtd3.state == NV_RTD3_ON && g_via == NV_RTD3_VIA_NONE) {
            // no way to the power: the GPU is not suspended for nothing, and a request to switch it off now is dropped
            OSCompareAndSwap(1, 0, &g_force);
            o.heads_lit = [](void *) { return 1; };
        }
        if (g_rtd3.state == NV_RTD3_ON && OSCompareAndSwap(1, 0, &g_force)) {
            // asked to power off now: clients and activity are not looked at this once, a lit head still is
            o.clients = nv_rtd3_none; if (o.active) o.active = nv_rtd3_none;
            g_idle = g_rtd3.idle_need ? g_rtd3.idle_need : NV_RTD3_IDLE_SECONDS;
        }
        if (g_rtd3.state == NV_RTD3_OFF && g_rtd3.enabled == NV_RTD3_NO_CLIENT) strlcpy(g_why, "a client or a display", sizeof g_why);
        r = nv_rtd3_tick(&g_rtd3, &o, &g_idle, 1);
    }
    nv_rtd3_account(r);
    const unsigned period_ms = g_rtd3.state == NV_RTD3_ON ? 1000 : 250;
    const bool again = !g_stop && !sys && (g_rtd3.enabled || g_rtd3.state == NV_RTD3_OFF);
    IOLockUnlock(g_lock);
    uint64_t dl; clock_interval_to_deadline(period_ms, kMillisecondScale, &dl);
    if (again) thread_call_enter_delayed(g_call, dl);
}

/* ---- the switches, for a running system. They exist only when the boot argument asked for runtime D3:
 *   debug.nvrm_rtd3        0 never, 1 only with no client attached, 2 by activity
 *   debug.nvrm_rtd3_res    the power resource's ACPI path
 *   debug.nvrm_rtd3_idle   idle seconds before the power goes
 *   debug.nvrm_rtd3_now    write 1: power off at the next poll unless a display is lit; write 2: power on
 *   debug.nvrm_rtd3_how    0 the power resource if it can be reached, else _DSM and _PS3 on the GPU node; 1, 2: only one of them
 *   debug.nvrm_rtd3_state  read: where it stands and what it did
 * Strings go out with SYSCTL_OUT and come in with SYSCTL_IN, as the driver's other string switches do. */
static int nv_rtd3_text_out(struct sysctl_req *req, const char *text) { return SYSCTL_OUT(req, text, strlen(text) + 1); }
static int nv_rtd3_text_in(struct sysctl_req *req, char *b, size_t size)
{
    if (req->newlen >= size) return EINVAL;
    const int e = SYSCTL_IN(req, b, req->newlen);
    b[e ? 0 : req->newlen] = 0;
    return e;
}
static int nv_rtd3_sysctl_level SYSCTL_HANDLER_ARGS
{
    int v = g_rtd3.enabled;
    const int e = sysctl_handle_int(oidp, &v, 0, req);
    if (e || !req->newptr) return e;
    if (v != NV_RTD3_NEVER && v != NV_RTD3_NO_CLIENT && v != NV_RTD3_BY_ACTIVITY) return EINVAL;
    g_rtd3.enabled = v; g_idle = 0;
    RLOG("rule set to %d (%s)", v, v == NV_RTD3_NEVER ? "never" : v == NV_RTD3_NO_CLIENT ? "with no client" : "by activity");
    nv_rtd3_kick();
    return 0;
}
static int nv_rtd3_sysctl_res SYSCTL_HANDLER_ARGS
{
    char b[sizeof g_res];
    int e = nv_rtd3_text_out(req, g_res);
    if (e || !req->newptr) return e;
    if ((e = nv_rtd3_text_in(req, b, sizeof b))) return e;
    if (b[0] && b[0] != '\\') return EINVAL;
    if (!g_lock) return ENXIO;
    IOLockLock(g_lock);
    const bool on = g_rtd3.state == NV_RTD3_ON;   // the way that switched the power off must also switch it on
    if (on) { strlcpy(g_res, b, sizeof g_res); g_looked = 0; }
    IOLockUnlock(g_lock);
    return on ? 0 : EBUSY;
}
static int nv_rtd3_sysctl_idle SYSCTL_HANDLER_ARGS
{
    int v = (int)(g_rtd3.idle_need ? g_rtd3.idle_need : NV_RTD3_IDLE_SECONDS);
    const int e = sysctl_handle_int(oidp, &v, 0, req);
    if (e || !req->newptr) return e;
    if (v < 5 || v > 86400) return EINVAL;
    g_rtd3.idle_need = (unsigned)v;
    return 0;
}
static int nv_rtd3_sysctl_now SYSCTL_HANDLER_ARGS
{
    int v = 0;
    const int e = sysctl_handle_int(oidp, &v, 0, req);
    if (e || !req->newptr) return e;
    if (v == 1) { if (!g_rtd3.enabled) return ENXIO; OSCompareAndSwap(0, 1, &g_force); }
    else if (v == 2) nv_rtd3_want("the sysctl");
    else return EINVAL;
    nv_rtd3_kick();
    return 0;
}
static int nv_rtd3_sysctl_how SYSCTL_HANDLER_ARGS
{
    int v = g_how;
    const int e = sysctl_handle_int(oidp, &v, 0, req);
    if (e || !req->newptr) return e;
    if (v < 0 || v > 2) return EINVAL;
    if (g_rtd3.state != NV_RTD3_ON) return EBUSY;   // the way that switched the power off must also switch it on
    g_how = v; g_looked = 0;
    nv_rtd3_kick();
    return 0;
}
static int nv_rtd3_sysctl_state SYSCTL_HANDLER_ARGS
{
    char b[520];
    const bool off = g_rtd3.state == NV_RTD3_OFF;
    const char *way = !g_looked ? "not looked for yet" : g_via == NV_RTD3_VIA_RESOURCE ? "the power resource"
                    : g_via == NV_RTD3_VIA_OPTIMUS ? "_DSM and _PS3 on the GPU node" : "nothing found";
    const uint64_t ms = g_stat.off_ms + (off ? now_ms() - g_stat.off_at : 0);
    snprintf(b, sizeof b, "%s, rule %d, resource %s, switched through %s, idle %u of %u s, %d head(s) lit, off %u time(s) for %llu s in all, "
             "back %u time(s) (last for: %s), kept on for its video memory %u time(s), failed: off %u, ACPI notifications %u (last 0x%x)",
             off ? "off" : "on", g_rtd3.enabled, g_res[0] ? g_res : "(none)", way, off ? 0 : g_idle,
             g_rtd3.idle_need ? g_rtd3.idle_need : NV_RTD3_IDLE_SECONDS, g_ops.heads_lit ? g_ops.heads_lit(g_ops.ctx) : -1,
             g_stat.offs, ms / 1000ULL, g_stat.backs, g_why, g_stat.heavy, g_stat.off_failed, g_stat.notifications, g_stat.last_notification);
    return nv_rtd3_text_out(req, b);
}
SYSCTL_PROC(_debug, OID_AUTO, nvrm_rtd3, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, nv_rtd3_sysctl_level, "I",
            "GPU D3cold: 0 never, 1 with no client attached, 2 by activity");
SYSCTL_PROC(_debug, OID_AUTO, nvrm_rtd3_res, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, nv_rtd3_sysctl_res, "A",
            "ACPI path of the GPU's power resource");
SYSCTL_PROC(_debug, OID_AUTO, nvrm_rtd3_idle, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, nv_rtd3_sysctl_idle, "I",
            "idle seconds before the GPU's power goes");
SYSCTL_PROC(_debug, OID_AUTO, nvrm_rtd3_now, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, nv_rtd3_sysctl_now, "I",
            "write 1: power the GPU off now unless a display is lit; 2: power it on");
SYSCTL_PROC(_debug, OID_AUTO, nvrm_rtd3_how, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, nv_rtd3_sysctl_how, "I",
            "how the firmware is asked: 0 the power resource, else _DSM and _PS3 on the GPU node; 1 the resource only; 2 the GPU node only");
SYSCTL_PROC(_debug, OID_AUTO, nvrm_rtd3_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, nv_rtd3_sysctl_state, "A",
            "GPU D3cold: state and counters");

// Whether the boot argument asked for runtime D3. Known before the monitor is armed: SMCNvidiaGPU and the sensor readers
// can come earlier than the adapter.
extern "C" int nv_rtd3_asked(void) { nv_rtd3_init(); return g_asked; }

extern "C" void nv_rtd3_arm(const struct nv_rtd3_ops *ops)
{
    nv_rtd3_init();
    if (!g_asked || g_call) return;   // not asked for: nothing is allocated, armed or registered
    g_ops = *ops;
    g_lock = IOLockAlloc();
    g_call = g_lock ? thread_call_allocate(nv_rtd3_poll, nullptr) : nullptr;
    if (!g_call) { if (g_lock) { IOLockFree(g_lock); g_lock = nullptr; } RLOG("not armed: no memory"); return; }
    // Registered once and never taken away, like the driver's other switches: NVRM is not unloaded.
    sysctl_register_oid(&sysctl__debug_nvrm_rtd3); sysctl_register_oid(&sysctl__debug_nvrm_rtd3_res);
    sysctl_register_oid(&sysctl__debug_nvrm_rtd3_idle); sysctl_register_oid(&sysctl__debug_nvrm_rtd3_now);
    sysctl_register_oid(&sysctl__debug_nvrm_rtd3_how); sysctl_register_oid(&sysctl__debug_nvrm_rtd3_state);
    uint64_t dl; clock_interval_to_deadline(1000, kMillisecondScale, &dl);
    thread_call_enter_delayed(g_call, dl);
    RLOG("armed (rule %d, idle %u s)", g_rtd3.enabled, NV_RTD3_IDLE_SECONDS);
}

extern "C" void nv_rtd3_disarm(void)
{
    g_stop = 1;
    // thread_call_cancel_wait is not exported to kexts (a kext that names it cannot be linked into the kernel collection):
    // cancel, and retry the free until a tick that was already running is out. The tick never re-arms once g_stop is set.
    if (g_call) {
        thread_call_cancel(g_call);
        for (int i = 0; i < 100 && !thread_call_free(g_call); i++) { IOSleep(10); thread_call_cancel(g_call); }
        g_call = nullptr;
    }
}

/* System sleep. The driver's own sleep and wake paths are written for a GPU that has power when the sleep starts: its
 * configuration space is saved and restored by the bus driver around the sleep, not by this file. So a GPU that is off
 * gets its power back first and sleeps like any other; the monitor stands still until the wake's resume is over. */
extern "C" void nv_rtd3_before_system_sleep(void)
{
    if (!g_lock) return;
    IOLockLock(g_lock);
    g_sys_sleep = 1;
    if (g_rtd3.state == NV_RTD3_OFF) {
        strlcpy(g_why, "system sleep", sizeof g_why);
        nv_rtd3_account(nv_rtd3_back_now());
    }
    IOLockUnlock(g_lock);
}
extern "C" void nv_rtd3_after_system_wake(void)
{
    g_idle = 0;
    if (OSCompareAndSwap(1, 0, &g_sys_sleep)) nv_rtd3_kick();
}

/* System wake: the resource must be on before the driver's own resume touches the GPU. The GPU went to sleep with its
 * power on (nv_rtd3_before_system_sleep), so this finds it off only when that power-on failed. */
extern "C" int nv_rtd3_before_system_resume(void)
{
    if (g_rtd3.state != NV_RTD3_OFF || !g_ops.acpi_on) return 1;
    int ok = 0;
    for (int i = 0; i < 200 && !(ok = g_ops.acpi_on(g_ops.ctx)); i++) IOSleep(250);
    nv_rtd3_on_done(&g_rtd3, ok);
    g_idle = 0;
    return ok;
}

/* A notification the firmware sent to the GPU's ACPI node. 0x81 on a display adapter is "the state of an output device
 * changed": on a laptop whose GPU can lose power it is how a cable plugged into one of its outputs is told, because the
 * GPU's own hot-plug interrupt cannot fire without power. */
extern "C" void nv_rtd3_acpi_notification(unsigned value)
{
    g_stat.notifications++; g_stat.last_notification = value;
    if (g_stat.notifications <= 32) RLOG("ACPI notification 0x%x to the GPU while it is %s", value, g_rtd3.state == NV_RTD3_OFF ? "off" : "on");
    if (value == 0x81) nv_rtd3_want("a display plugged in (ACPI 0x81)");
}
