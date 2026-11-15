#!/usr/bin/env python3
"""Only two function-constant translations hold a slot at a time, so a warmed sibling can sit in the queue behind
dozens of others. The main thread used to wait up to 60 s for such a sibling, and WindowServer's watchdog kills it
after 40 s without a checkin - the login-window crash after a driver update. A join now waits only for a translation
that already holds a slot, and its timeout fits several joins inside one checkin interval. The control is the source
before the change (commit ca0be5b), which waits for queued work with a 60 s timeout."""
import pathlib, re, subprocess, sys

R = pathlib.Path(__file__).resolve().parents[1]
SRC = "plugin/NVMTLLibraryLoad.m"
WATCHDOG_SEC = 40
fails = 0


def arm(name, ok, detail=""):
    global fails
    print(("ok   " if ok else "FAIL ") + name + (f" ({str(detail).strip()[-200:]})" if detail and not ok else ""))
    fails += not ok


def body(src, signature):
    a = src.index(signature)
    return src[a:src.index("\n}\n", a)]


def join_waits_only_for_running(src):
    """True when a join declines a sibling that has not taken a slot yet, before it waits."""
    b = body(src, "BOOL nvmtl_fcwarm_join(")
    if "dispatch_group_wait" not in b:
        return None
    w = b.index("dispatch_group_wait")
    head = b[:w]
    return "gFcWarmStarted" in head and "return NO" in head


def marks_started_under_the_slot(src):
    """True when the path is marked started after the slot is taken, not when the work is queued."""
    b = body(src, "void nvmtl_fcwarm_kick(")
    if "dispatch_semaphore_wait" not in b:
        return None
    s = b.index("dispatch_semaphore_wait")
    return "[gFcWarmStarted addObject:path]" in b[s:]


def join_sec(src):
    m = re.search(r"#define\s+NVMTL_FCWARM_JOIN_SEC\s+(\d+)", src)
    return int(m.group(1)) if m else None


src = (R / SRC).read_text()
arm("the join is still the function this check reads", join_waits_only_for_running(src) is not None)
arm("a join declines a sibling still in the queue, before waiting", join_waits_only_for_running(src) is True)
arm("a path is marked started only once it holds a slot", marks_started_under_the_slot(src) is True)
sec = join_sec(src)
arm("the join timeout is set", sec is not None)
arm(f"several joins fit inside one {WATCHDOG_SEC}s watchdog interval", sec is not None and sec * 4 < WATCHDOG_SEC, f"JOIN_SEC={sec}")
arm("the started set is cleared when the translation finishes", "[gFcWarmStarted removeObject:path]" in body(src, "void nvmtl_fcwarm_kick("))
old = subprocess.run(["git", "-C", str(R), "show", f"ca0be5b:{SRC}"], capture_output=True, text=True)
if old.returncode == 0:
    arm("control: the earlier join waited for queued work too", join_waits_only_for_running(old.stdout) is False)
    arm("control: the earlier timeout exceeded the watchdog interval", join_sec(old.stdout) >= WATCHDOG_SEC)
else:
    arm("control: the earlier source is readable", False, old.stderr)
print("FAILED" if fails else "PASS")
sys.exit(1 if fails else 0)