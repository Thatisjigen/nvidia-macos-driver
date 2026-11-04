#!/usr/bin/env python3
"""A dirty mark on a surface another accelerator owns must not pass for a mark.

Intel writes an IOSurface, the RTX writes it, and the CPU and Intel still read Intel's pixels: the RTX's mark calls
IOSurfaceDeviceCache::setCurrent, set_current_device_cache refuses while another cache is current, and neither says so.
The model below follows IOSurface 393.5.11 (macOS 26.7.2) as disassembled: set_current_device_cache and its answer in the
cache, bind_accel through synchronize_device_caches, lockSurface. The decision is lifted from nvrm-accel.cpp and exercised in
every mode of debug.nvaccel_sv_take, and with a CPU lock or unlock landing right after the mark; the negative control is
the pre-fix mark, which must lose the write. It runs on the CPU and says nothing about the real kernel or the two GPUs: that
is xgpuprobe's T8, after a reboot on this kext."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
accel = (root / 'kexts/NVRM/accel/nvrm-accel.cpp').read_text()
abi = (root / 'kexts/NVRM/nvrm_surfshare_abi.h').read_text()

def lift(name):
    i = accel.index('static inline bool %s(' % name)
    return accel[i:accel.index('\n', i) + 1]

def body(signature):
    i = accel.index(signature + '\n{')
    return accel[i:accel.index('\n}\n', i)]

def before(text, *marks):
    at = [text.index(m) for m in marks]
    return at == sorted(at)

assert 'NVRM_SS_OTHER_OWNER' in abi, 'the status is gone from the ABI header'

dirty = body('static unsigned nvAccelSurfDirty(void *cache, NVResource *res, unsigned plane)')
assert before(dirty, '_ZN20IOSurfaceDeviceCache10setCurrentEv(cache)', 'nvAccelSurfOtherOwner(', 'return NVRM_SS_OTHER_OWNER', 'valid[6] |='), \
    'the refusal must be read after setCurrent and before the resource says VRAM is newest'
assert 'cache + 0x50' in dirty and '_ZNK9IOSurface13getUnlockSeedEv(surf)' in dirty, \
    'the answer of set_current_device_cache is in the cache: the owner can be cleared right after a refusal'
assert 'static int gSVTake = 1;' in accel, 'the bind waits inside the escape: it stays off unless asked for'

vram = body('static IOReturn nvAccelSurfVram(struct NVRMSurfShareEsc *p, task_t task, unsigned cmd)')
take = vram[vram.index('p->status == NVRM_SS_OTHER_OWNER && !tryOnly'):]
assert before(take, 'nvFamUnlock(a)', '_ZN9IOSurface10bind_accelEjj(surf,', 'nvFamLock(a, false)', 'ctx = ', 'nvAccelSurfDirty(cache, res, p->plane)'), \
    'bind_accel takes the owner\'s accelerator lock and waits for its GPU: it must run outside ours, and the resource must be read again after'
assert 'gSVTake > 1' in take[:take.index('{')], 'debug.nvaccel_sv_take no longer decides the bind'
install = accel[accel.index('static void nvaccelInstallSysctl(void)'):]
for oid in ('nvaccel_sv_take', 'nvaccel_sv_taken', 'nvaccel_sv_other_owner'):
    assert 'sysctl_register_oid(&sysctl__debug_%s)' % oid in install, '%s is not registered' % oid

control = 'static inline bool nvAccelSurfOtherOwner(unsigned, unsigned) { return false; }\n'

fixture = r'''
#include <cstdio>
%s
struct Cache { unsigned accel, plane, pixels, seen; };
struct Surface { Cache *owner = nullptr; unsigned seed = 0, sysmem = 0; };
static void set_current(Surface &s, Cache *c) {
    if (s.owner && s.owner != c) { c->seen = s.seed - 1; return; }
    s.owner = c; c->seen = ++s.seed;
}
static void synchronize(Surface &s, unsigned accel, unsigned plane) {
    Cache *o = s.owner;
    if (o && (o->accel != accel || o->plane != plane)) { s.owner = nullptr; s.sysmem = o->pixels; }
}
static unsigned cpu_lock(Surface &s) { synchronize(s, 0, 0); return s.sysmem; }
enum { OK = 0, OTHER_OWNER = 8 };
// the mark of nvAccelSurfDirty and the retry of nvAccelSurfVram; a refused mark that reaches the plugin is copied out under a CPU lock
static int answer(Surface &s, Cache *c, int take) { return take && nvAccelSurfOtherOwner(c->seen, s.seed) ? OTHER_OWNER : OK; }
static int mark(Surface &s, Cache *c, int take) { set_current(s, c); return answer(s, c, take); }
static void gpu_write(Surface &s, Cache *c, unsigned pixels, int take, bool tryOnly = false) {
    c->pixels = pixels;
    int st = mark(s, c, take);
    if (st == OTHER_OWNER && !tryOnly && take > 1) { synchronize(s, c->accel, c->plane); st = mark(s, c, take); }
    if (st == OTHER_OWNER && !tryOnly) { cpu_lock(s); s.sysmem = pixels; s.seed++; }
}
int main() {
    int bad = 0;
    for (int take = 1; take <= 2; take++) {
        Surface s; Cache intel{2, 0, 0, 0}, rtx{1, 0, 0, 0};
        gpu_write(s, &intel, 10, take);
        gpu_write(s, &rtx, 11, take);
        const unsigned seed = s.seed; const unsigned got = cpu_lock(s);
        if (got != 11) { printf("FAIL mode %%d: the CPU reads %%u after Intel 10, RTX 11\n", take, got); bad++; }
        if (seed < 2) { printf("FAIL mode %%d: the seed did not move for the RTX's write\n", take); bad++; }
    }
    {   // debug.nvaccel_sv_take=0 is the A/B: it must give the old behaviour back, defect included
        Surface s; Cache intel{2, 0, 0, 0}, rtx{1, 0, 0, 0};
        gpu_write(s, &intel, 10, 0); gpu_write(s, &rtx, 11, 0);
        if (cpu_lock(s) != 10) { puts("FAIL mode 0 no longer ignores the refusal"); bad++; }
    }
    {   // refused, and a CPU lock clears the owner before the answer is read: still refused, the owner alone would call it taken
        Surface s; Cache intel{2, 0, 0, 0}, rtx{1, 0, 0, 0};
        gpu_write(s, &intel, 10, 1);
        set_current(s, &rtx); cpu_lock(s);
        if (answer(s, &rtx, 1) != OTHER_OWNER) { puts("FAIL a refused mark counted as taken once a lock had cleared the owner"); bad++; }
    }
    {   // taken, and a CPU lock clears the owner: still taken, its page-off finds VRAM newest
        Surface s; Cache rtx{1, 0, 0, 0};
        set_current(s, &rtx); cpu_lock(s);
        if (answer(s, &rtx, 1) != OK) { puts("FAIL a taken mark counted as refused once a lock had cleared the owner"); bad++; }
    }
    {   // taken, and an unlock moves the seed: it looks refused, and the second mark, on a surface still ours, puts it right
        Surface s; Cache rtx{1, 0, 0, 0};
        set_current(s, &rtx); s.seed++;
        if (answer(s, &rtx, 1) != OTHER_OWNER) { puts("FAIL an unlock right after the mark went unnoticed"); bad++; }
        synchronize(s, rtx.accel, rtx.plane);
        if (s.owner != &rtx || mark(s, &rtx, 2) != OK) { puts("FAIL the second mark did not put it right"); bad++; }
    }
    {   // bind on a surface already ours pages nothing off
        Surface s; Cache rtx{1, 0, 0, 0}; gpu_write(s, &rtx, 1, 2); synchronize(s, rtx.accel, rtx.plane);
        if (s.owner != &rtx || s.sysmem) { puts("FAIL a bind paged off the surface's own owner"); bad++; }
    }
    {   // a try never waits: the refusal goes back and nothing moves
        Surface s; Cache intel{2, 0, 0, 0}, rtx{1, 0, 0, 0}; gpu_write(s, &intel, 10, 2); const unsigned seed = s.seed; gpu_write(s, &rtx, 11, 2, true);
        if (s.owner != &intel || s.seed != seed) { puts("FAIL a try took the surface"); bad++; }
    }
    return bad;
}
'''

def run(code, label):
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / 'owner.cpp'
        src.write_text(fixture % code)
        exe = pathlib.Path(d) / 'owner'
        subprocess.run(['clang++', '-std=c++17', '-o', str(exe), str(src)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print('%s: rc %d%s' % (label, r.returncode, (' ' + r.stdout.strip().replace('\n', ' | ')) if r.stdout.strip() else ''))
        return r.returncode

ok = run(lift('nvAccelSurfOtherOwner'), 'shipped decision')
assert ok == 0, 'the shipped decision failed an arm'
bad = run(control, 'control: the refusal ignored')
assert bad > 0, 'the control did not fire: the test cannot see the difference the fix makes'
print('ok: a refused mark never passes for a mark; ignoring it loses the write (%d arm(s) fired)' % bad)
