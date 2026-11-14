#!/usr/bin/env python3
"""Every image creation looks its memory up in the allocation history. That lookup scanned from the newest record
backwards, so a handle that is not recent walked the whole history under g_mm_lock - the same scan the image table
grew a hash index to get rid of. The lookup now walks a bucket chain. The control is the source before the change
(commit 718f6ab), which has the backwards scan and no index."""
import pathlib, subprocess, sys

R = pathlib.Path(__file__).resolve().parents[1]
SRC = "plugin/nvmtl_mem.h"
fails = 0


def arm(name, ok, detail=""):
    global fails
    print(("ok   " if ok else "FAIL ") + name + (f" ({str(detail).strip()[-200:]})" if detail and not ok else ""))
    fails += not ok


def body(src, signature):
    a = src.index(signature)
    return src[a:src.index("\n}\n", a)]


def indexed(src):
    """True when the lookup walks a bucket chain instead of scanning the history."""
    b = body(src, "nvmtl_mm_entry *nvmtl_mm_by_mem(")
    if "if (!mem) return NULL;" not in b:
        return None
    return "s->heads[nvmtl_mm_bucket(mem)]" in b and ".chain" in b and "i-- > 0" not in b


new = indexed((R / SRC).read_text())
arm("the memory lookup is still the function this check reads", new is not None)
arm("the lookup walks a bucket chain, not the whole history", new is True)
src = (R / SRC).read_text()
arm("an added record is linked into its bucket", "e->chain = s->heads[b]; s->heads[b] = s->n;" in body(src, "uint32_t nvmtl_mm_add("))
arm("a sweep rebuilds the buckets, so the index cannot go stale", "memset(s->heads, 0, sizeof s->heads);" in body(src, "void nvmtl_mm_compact("))
old = subprocess.run(["git", "-C", str(R), "show", f"718f6ab:{SRC}"], capture_output=True, text=True)
if old.returncode == 0:
    arm("control: the earlier lookup scanned the history backwards", indexed(old.stdout) is False)
else:
    arm("control: the earlier source is readable", False, old.stderr)
print("FAILED" if fails else "PASS")
sys.exit(1 if fails else 0)