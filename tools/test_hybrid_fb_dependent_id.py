#!/usr/bin/env python3
"""Hybrid panel framebuffers (NMIntelFB, NMGopFB) set IOFBDependentID before IOFramebuffer::start (issue 555).

IOFramebuffer::start attaches a framebuffer to its shared controller only when the dependent ID already exists. Set
afterwards, the controller stays unset until open(), and the power change that registerPowerDriver() queues at the end
of start() takes the controller's lock through a null pointer. NVRMFB already sets it first.

This fork's NMIntelFB is not upstream's: start() keeps the virtual test build and hands the built-in panel to
startReal() and the USB-C screen to usbcStart(); a failed panel start and stop() share startUnwind(). The checks read
those functions.

   python3 tools/test_hybrid_fb_dependent_id.py [tree-root]
The negative control runs the same check on the pre-fix sources (git HEAD~ or a supplied old tree) and must fail."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
FILES = {"NMIntelFB": "kexts/NMIntelFB/intelfb.cpp", "NMGopFB": "kexts/NMGopFB/gopfb.cpp"}
fails = []


def arm(name, ok):
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        fails.append(name)


def function_body(src, head):
    i = src.index(head)
    return src[i:src.index("\n}\n", i)]


def start_bodies(src, cls):
    """Each `bool <cls>::start(` body, split at #ifdef branches so both start paths are checked on their own, then the
    start functions this fork's NMIntelFB::start hands over to (startReal for the panel, usbcStart for the USB-C screen)
    where the source has them."""
    body = function_body(src, f"bool {cls}::start(IOService *provider)")
    paths = [body]
    if "#ifdef" in body and "#endif" in body:
        # NMIntelFB: the virtual panel path returns inside #ifdef ... #endif; the real panel path follows it
        a = body.index("#ifdef")
        c = body.index("#endif", a)
        paths = [body[a:c], body[c:]]
    for helper in ("startReal", "usbcStart"):
        head = f"bool {cls}::{helper}(IOService *provider)"
        if head in src:
            paths.append(function_body(src, head))
    return paths


def panel_body(src):
    """NMIntelFB's built-in panel path: startReal in this fork, the end of start() in upstream's layout."""
    head = "bool NMIntelFB::startReal(IOService *provider)"
    return function_body(src, head) if head in src else start_bodies(src, "NMIntelFB")[-1]


def ordered(body):
    """True when every IOFramebuffer::start call comes after both dependent properties are set."""
    if "IOFramebuffer::start(provider)" not in body:
        return None
    s = body.index("IOFramebuffer::start(provider)")
    d = body.find('setProperty("IOFBDependentID"')
    x = body.find('setProperty("IOFBDependentIndex"')
    return 0 <= d < s and 0 <= x < s


def check(read):
    results = {}
    for cls, rel in FILES.items():
        src = read(rel)
        for n, body in enumerate(start_bodies(src, cls)):
            r = ordered(body)
            if r is not None:
                results[f"{cls} start path {n}"] = r
    return results


now = check(lambda rel: (ROOT / rel).read_text(encoding="utf-8"))
arm("four start paths were found (NMIntelFB virtual, panel and USB-C screen, NMGopFB)", len(now) == 4)
for name, ok in now.items():
    arm(f"{name}: the dependent ID and index are set before IOFramebuffer::start", ok)

ifb = (ROOT / FILES["NMIntelFB"]).read_text(encoding="utf-8")
panel = panel_body(ifb)
arm("NMIntelFB: a surface that will not map stops start before IOFramebuffer::start (no controller-less framebuffer)",
    panel.find("if (!fSurfMap) {") != -1 and panel.find("if (!fSurfMap) {") < panel.index("IOFramebuffer::start(provider)"))
# nullmoth-intelfb-dep=0 starts the panel without a dependent ID on purpose, so without a controller until open(). The
# order checked above does not cover that case: there the power registration must be left out.
PM = 'if (dep && ifb_knob("nullmoth-intelfb-pm", 1)) {'
arm("NMIntelFB: a panel started without a dependent ID (nullmoth-intelfb-dep=0) registers no power driver",
    0 <= panel.find(PM) < panel.find("registerPowerDriver(this"))
# The backlight service is what the panel path publishes before IOFramebuffer::start: the nub's framebuffer pointer it
# sets first is read only by the AGDC sysctl and the autopipe, both started after IOFramebuffer::start.
PUBLISHERS = ("publishBacklight();",)


def map_before_publish(body):
    """The surface map check returns before anything that publishes state other threads can reach."""
    m = body.find("if (!fSurfMap) {")
    return m >= 0 and all(body.find(x) > m for x in PUBLISHERS)


arm("NMIntelFB: the surface-map failure return comes before the backlight service is published",
    map_before_publish(panel))
fail_ret = panel[panel.index("if (!IOFramebuffer::start(provider)) {"):]
fail_ret = fail_ret[:fail_ret.index("return false;")]
arm("NMIntelFB: a failed IOFramebuffer::start (after state is published) runs the shared unwind", "startUnwind();" in fail_ret)
unwind = ifb[ifb.index("void NMIntelFB::startUnwind()"):]
unwind = unwind[:unwind.index("\n}\n")]
arm("NMIntelFB: the unwind stops the VBL thread, unregisters the AGDC sysctl and clears the nub's framebuffer, turns "
    "TC1 off, takes the RAM scan-out down, frees the NVKMS device, removes the backlight and releases both mappings",
    all(x in unwind for x in ("fVblOn = false;", "while (fVblRunning)", "ifb_agdc_register(false); gIfbNubFB = nullptr;",
                              "tc1Set(0)", "ifb_ram_register(nullptr)", "ramSet(0)", "kapiFree();",
                              "fBacklight->detach(this)", "OSSafeReleaseNULL(fSurfMap)", "OSSafeReleaseNULL(fMMIO)")))
AGDC = "static void ifb_agdc_register(bool on)\n{"   # the definition, not the declaration above it
arm("NMIntelFB: the unwind of a failed start does not unregister the AGDC sysctl that start never registered",
    AGDC in ifb and "if (on == registered) return;" in function_body(ifb, AGDC))
stop = ifb[ifb.index("void NMIntelFB::stop(IOService *provider)"):]
stop = stop[:stop.index("\n}\n")]
arm("NMIntelFB: stop() uses the same unwind, so the two cannot drift apart", "startUnwind();" in stop)
# Negative control: upstream's first fix for issue 555, with its map check after the DMC target, tcStart and the
# backlight (upstream's layout, the panel path at the end of start()), must be caught.
first_fix = subprocess.run(["git", "-C", str(ROOT), "show", "06666bf5:kexts/NMIntelFB/intelfb.cpp"],
                           capture_output=True, text=True)
arm("negative control: the map-after-publish order of the first fix is caught",
    first_fix.returncode == 0 and not map_before_publish(panel_body(first_fix.stdout)))
nvfb = (ROOT / "kexts/NVRMFB/fb/nvrm-fb.cpp").read_text(encoding="utf-8")
arm("NVRMFB still sets its dependent ID before super::start (the reference order)",
    nvfb.index('setProperty("IOFBDependentID"') < nvfb.index("if (!super::start(provider)) {"))

# Negative control: the old order (properties after start) must be caught by the same check.
old_tree = Path(sys.argv[2]) if len(sys.argv) > 2 else None


def read_old(rel):
    if old_tree:
        return (old_tree / rel).read_text(encoding="utf-8")
    # the commit that moved the properties first, found by its comment text, so later commits do not move the base. For
    # NMIntelFB that is this fork's move in the panel path (startReal): its parent has the old order on the virtual path
    # too, which kept it until the fix for issue 555.
    mark = "Set before IOFramebuffer::start" if "gopfb" in rel else "the dependent ID decides"
    intro = subprocess.run(["git", "-C", str(ROOT), "log", "--format=%H", "-S", mark, "--", rel],
                           capture_output=True, text=True, check=True).stdout.split()
    rev = intro[-1] + "~1" if intro else "HEAD"
    return subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{rel}"], capture_output=True, text=True,
                          check=True).stdout


try:
    old = check(read_old)
    # three paths there: the USB-C screen came later, with its dependent ID set first from the beginning
    arm("negative control: the check fails on the old order (pre-fix sources)", len(old) == 3 and not any(old.values()))
except (subprocess.CalledProcessError, ValueError, FileNotFoundError) as e:
    arm(f"negative control could not run ({e.__class__.__name__}): pass the pre-fix tree as the second argument", False)

print(f"{len(fails)} failed")
sys.exit(1 if fails else 0)
