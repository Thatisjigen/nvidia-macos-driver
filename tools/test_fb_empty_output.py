#!/usr/bin/env python3
"""A monitor plugged into a card that had none at start lights up: the card's output starts with the card.

macOS opens a GPU's framebuffers when WindowServer starts (CoreDisplay builds its list of them once per GPU, at its own
start or when a GPU is added) and never looks for one that registers later; IOFramebuffer::open() has no other caller.
The driver started a framebuffer only once a display was there, so with no display at start the framebuffer of the
monitor plugged in later registered into a session that had stopped looking: nothing opened it, no connect handler was
ever registered ("connect notify: nothing registered yet"), and the monitor stayed dark until a restart.

A card with one output and no display now starts that output with its connection off, the way a Mac's framebuffer of
an empty port is there from boot with no display under it. The monitor that arrives is a connection change on a
framebuffer macOS already holds. Checked here:
  - the rule (one output, no display, no panel proxy, not switched off) and the stand-in raster, from nvrm-fb-safety.h;
  - the wiring in nvrm-fb.cpp: the count auto-go reads is published after the decision, the connection is off from the
    start, the open does no mode set, the scans go on, the head gets its first mode set after the lock is dropped;
  - the real takeFirstDisplay(), compiled against a stand-in NVKMS: the stand-in timing gives way to the monitor's own
    mode at the same raster;
  - a model of the adoption rule above, where the old driver's framebuffer is never opened (the control)."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()

def body(signature, text=src):
    s = text.index(signature)
    return text[s:text.index('\n}\n', s) + 3]

# ---- wiring ----
init = body('bool NVRMNVDAFramebuffer::kapiInit()')
assert init.index('nvrmKeepEmptyOutput(') < init.index('setProperty("connected-displays", (unsigned long long)gNDpy'), \
    'the display count is published before the output that starts empty is counted: auto-go can read 0 and release its hold'
assert 'gDpy[0] = gEmptyOut; gNDpy = 1;' in init and 'PE_parse_boot_argn("nvfbemptyout"' in init and 'PE_parse_boot_argn("nvfbigpu"' in init
assert '(dd->connected || n == 1) ? fKms->getStaticDisplayInfo(' in init, 'an empty output is never asked for its connector and heads'
assert 'fNoDisplayYet = fDpyLost = !r->connected;' in init, 'an output with no display reads as connected'
assert 'takeEdid(r->edid, r->connected ? r->edidSize : 0);' in init
# the guard that turns the driver off after starts that never finished stays armed until this start has finished too
assert 'if (n > 0 && gNDpy == 0) fbGuardCleared(' in init
standin = init[init.index('if (fNoDisplayYet) {'):]
standin = standin[:standin.index('} else {')]
assert 'nvrmEmptyOutputTiming()' in standin and 'getDisplayMode' not in standin, 'a display that is not there is asked for its modes'
enable = body('IOReturn NVRMNVDAFramebuffer::enableController()')
assert enable.index('if (fDpyLost) {') < enable.index('applyMode()'), 'the open tries a mode set that names no display'
hot = body('void NVRMNVDAFramebuffer::hotplugFire(')
unlock = hot.index('if (sl) IORecursiveLockUnlock(sl);')
assert hot.index('if (fb->fNoDisplayYet) { gDpy[kdpy].vrr = dd->vrrSupported; fb->takeFirstDisplay(); }') < unlock, \
    "the first monitor's mode is read outside the start lock"
assert unlock < hot.index('ok = fb->applyMode();') < hot.index('fb->notifyConnectionChanged(500);\n        }\n        fb->release();'), \
    'the first mode set has to come with the start lock dropped, and before macOS is told of the connection'
assert 'if (fb->fModeSet || !fb->fSurf) ok = fb->reflip();' in hot, 'a first mode set on an instance that has no surface'
assert '!nvrmAnyDisplayConnected() && gBootRetry < kNvrmBootRetries' in hot, 'the scans stop while the output is still empty'
assert 'hasDDCConnect(IOIndex) override { return !fDpyLost' in src
afc = body('IOReturn NVRMNVDAFramebuffer::getAttributeForConnection(')
assert 'case kConnectionCheckEnable:               if (value) *value = (fDpyLost || fHidden) ? 0 : 1;' in afc

take = body('void NVRMNVDAFramebuffer::takeFirstDisplay()')
fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "nvrm-fb-safety.h"
using NvU32 = uint32_t; using NvBool = bool;
enum { NV_TRUE = 1, NV_FALSE = 0 };
#define FBLOG(...) ((void)0)
struct NvKmsKapiDisplayModeTimings {
    NvU32 refreshRate = 0, pixelClockHz = 0, hVisible = 0, hSyncStart = 0, hSyncEnd = 0, hTotal = 0, vVisible = 0, vSyncStart = 0, vSyncEnd = 0, vTotal = 0;
    struct { NvU32 interlaced : 1, doubleScan : 1, hSyncPos : 1, hSyncNeg : 1, vSyncPos : 1, vSyncNeg : 1; } flags = {};
};
struct NvKmsKapiDisplayMode { NvKmsKapiDisplayModeTimings timings; };
struct Row { NvKmsKapiDisplayMode m; bool valid, preferred; };
struct Kms {
    std::vector<Row> rows; unsigned asked = 0;
    int getDisplayMode(void *, unsigned, NvU32 i, NvKmsKapiDisplayMode *m, NvBool *valid, NvBool *pref) {
        asked++;
        if (i >= rows.size()) return -1;
        *m = rows[i].m; *valid = rows[i].valid; *pref = rows[i].preferred; return 1;
    }
};
static struct { bool vrr; } gDpy[2];
static int gNDpy = 1;
struct NVRMNVDAFramebuffer {
    Kms *fKms = nullptr; void *fDev = this; unsigned fDisplay = 0x100, fIndex = 0, fEdidSize = 256;
    bool fNoDisplayYet = true, fEdidSwapped = false, fVrrSupported = false, fModeSet = false, fHaveKmsMode = true, vrrArg = false;
    NvU32 fBootW = kNvrmEmptyW, fBootH = kNvrmEmptyH;
    NvKmsKapiDisplayMode fMode, fKmsMode, fBootMode;
    int link = 0, startup = 0, caps = 0;
    bool vrrAllowed() { return vrrArg; }
    void publishLinkTiming() { link++; } void publishStartupTiming() { startup++; } void publishTimingCaps() { caps++; }
    void takeFirstDisplay();
    NVRMNVDAFramebuffer() {            // as kapiInit leaves an output that started empty
        const NvrmEmptyTiming e = nvrmEmptyOutputTiming();
        fMode.timings.pixelClockHz = e.pixelClockHz; fMode.timings.hVisible = e.hVisible; fMode.timings.vVisible = e.vVisible;
        fMode.timings.hTotal = e.hTotal; fMode.timings.vTotal = e.vTotal; fMode.timings.refreshRate = e.refreshMilliHz;
        fKmsMode = fBootMode = fMode;
    }
};
TAKE
static NvKmsKapiDisplayMode mode(NvU32 w, NvU32 h, NvU32 hz, bool interlaced = false) {
    NvKmsKapiDisplayMode m; auto &t = m.timings;
    t.hVisible = w; t.vVisible = h; t.hTotal = w + 160; t.vTotal = h + 55;
    t.pixelClockHz = (NvU32)((uint64_t)t.hTotal * t.vTotal * hz); t.refreshRate = hz * 1000; t.flags.interlaced = interlaced; return m;
}
int main() {
    // ---- the rule ----
    assert(nvrmKeepEmptyOutput(0, 1, true, false, false, 0));                 // one output, nothing on it
    assert(nvrmKeepEmptyOutput(0, 1, true, false, true, 1));
    assert(!nvrmKeepEmptyOutput(0, 1, true, false, true, 0));                 // nvfbemptyout=0
    assert(!nvrmKeepEmptyOutput(1, 1, true, false, false, 0));                // its display answered: an ordinary start
    assert(!nvrmKeepEmptyOutput(0, 4, true, false, false, 0));                // several outputs: which one would be a guess
    assert(!nvrmKeepEmptyOutput(0, 0, true, false, false, 0));
    assert(!nvrmKeepEmptyOutput(0, 1, false, false, false, 0));               // its connector and heads could not be read
    assert(!nvrmKeepEmptyOutput(0, 1, true, true, false, 0));                 // the head stands in for another GPU's panel
    // ---- the stand-in raster: CTA-861 format 16 ----
    const NvrmEmptyTiming e = nvrmEmptyOutputTiming();
    assert(e.hVisible == kNvrmEmptyW && e.vVisible == kNvrmEmptyH && e.hVisible == 1920 && e.vVisible == 1080);
    assert((uint64_t)e.hTotal * e.vTotal * 60 == e.pixelClockHz && e.refreshMilliHz == 60000);   // 2200 x 1125 x 60 = 148.5 MHz
    assert(e.hVisible <= e.hSyncStart && e.hSyncStart < e.hSyncEnd && e.hSyncEnd <= e.hTotal);
    assert(e.vVisible <= e.vSyncStart && e.vSyncStart < e.vSyncEnd && e.vSyncEnd <= e.vTotal);
    assert(e.hSyncStart - e.hVisible == 88 && e.hSyncEnd - e.hSyncStart == 44 && e.vSyncStart - e.vVisible == 4 && e.vSyncEnd - e.vSyncStart == 5);
    assert(nvrmKmsRasterValid(e.hVisible, e.vVisible));
    // ---- the first monitor ----
    {   // a 1920x1080 monitor whose preferred mode is its 144 Hz one: the head is as a start with it connected leaves it
        Kms k; k.rows = { { mode(1920, 1080, 60), true, false }, { mode(3840, 2160, 60), true, false },
                          { mode(1920, 1080, 144), true, true }, { mode(1920, 1080, 120), true, false } };
        NVRMNVDAFramebuffer f; f.fKms = &k; gDpy[0].vrr = true; f.vrrArg = true; f.takeFirstDisplay();
        assert(f.fMode.timings.refreshRate == 144000 && f.fMode.timings.hVisible == 1920 && f.fMode.timings.vVisible == 1080);
        assert(!memcmp(&f.fMode, &f.fBootMode, sizeof f.fMode) && !memcmp(&f.fMode, &f.fKmsMode, sizeof f.fMode) && f.fHaveKmsMode);
        assert(!f.fNoDisplayYet && f.fEdidSwapped && f.fVrrSupported && f.link == 1 && f.startup == 1 && f.caps == 1);
    }
    {   // no preferred flag: the raster's highest rate; an interlaced or refused mode at that raster is not it
        Kms k; k.rows = { { mode(1920, 1080, 60), true, false }, { mode(1920, 1080, 240), false, false },
                          { mode(1920, 1080, 200, true), true, false }, { mode(1920, 1080, 165), true, false },
                          { mode(2560, 1440, 240), true, false } };
        NVRMNVDAFramebuffer f; f.fKms = &k; gDpy[0].vrr = true; f.takeFirstDisplay();
        assert(f.fMode.timings.refreshRate == 165000 && f.fMode.timings.hVisible == 1920 && !f.fVrrSupported);   // nvfbvrr not given
    }
    {   // a monitor with no mode at that raster: the stand-in stays for the first mode set, and macOS is still told
        Kms k; k.rows = { { mode(2560, 1440, 144), true, true }, { mode(1280, 720, 60), true, false } };
        NVRMNVDAFramebuffer f; f.fKms = &k; const NvKmsKapiDisplayMode was = f.fMode; f.takeFirstDisplay();
        assert(!memcmp(&f.fMode, &was, sizeof was) && !memcmp(&f.fBootMode, &was, sizeof was) && f.link == 0 && f.startup == 0);
        assert(!f.fNoDisplayYet && f.fEdidSwapped && f.caps == 1);
    }
    {   // a head that already has a mode keeps it: nothing is asked of NVKMS
        Kms k; k.rows = { { mode(1920, 1080, 144), true, true } };
        NVRMNVDAFramebuffer f; f.fKms = &k; f.fModeSet = true; const NvKmsKapiDisplayMode was = f.fMode; f.takeFirstDisplay();
        assert(k.asked == 0 && !memcmp(&f.fMode, &was, sizeof was) && !f.fNoDisplayYet);
    }
    assert(nvrmFirstModeBetter(false, false, 0, false, 1) && nvrmFirstModeBetter(true, false, 900, true, 100));
    assert(!nvrmFirstModeBetter(true, true, 100, false, 900) && !nvrmFirstModeBetter(true, false, 900, false, 900));
    puts("PASS: one empty output starts with the card; the first monitor's own mode replaces the stand-in at the same raster");
}
'''
# The adoption rule, as read in macOS: WindowServer opens the framebuffers that are registered when it starts, and a
# connection change reaches it only through a handler it registered on a framebuffer it opened.
model = r'''
#include <cassert>
#include <cstdio>
struct Framebuffer { bool registered = false, opened = false, handler = false, connected = false, shown = false; };
static void windowServerStarts(Framebuffer &fb) { if (fb.registered) { fb.opened = true; fb.handler = true; if (fb.connected) fb.shown = true; } }
static void connectInterrupt(Framebuffer &fb) { if (fb.handler && fb.connected) fb.shown = true; }   // "nothing registered yet" otherwise
// One boot with no monitor, then the cable. startsEmpty: the output is started with the card.
static bool monitorLights(bool startsEmpty) {
    Framebuffer fb;
    if (startsEmpty) fb.registered = true;             // at start: registered, connection off
    windowServerStarts(fb);
    fb.connected = true;                               // the cable
    if (!startsEmpty) fb.registered = true;            // before: the framebuffer started now
    connectInterrupt(fb);
    return fb.shown;
}
int main() {
    assert(monitorLights(true));
    assert(!monitorLights(false));                     // control: started late, never opened, the monitor stays dark
    puts("PASS: a framebuffer open since the start gets the connection change; one that starts with the monitor is never opened");
}
'''
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    (d/'t.cpp').write_text(fixture.replace('TAKE', take))
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Werror', f'-I{root}/kexts/NVRMFB/fb', str(d/'t.cpp'), '-o', str(d/'t')], check=True)
    subprocess.run([str(d/'t')], check=True)
    (d/'m.cpp').write_text(model)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Werror', str(d/'m.cpp'), '-o', str(d/'m')], check=True)
    subprocess.run([str(d/'m')], check=True)
