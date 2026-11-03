#!/usr/bin/env python3
"""A dirty mark on a surface another accelerator owns must not pass for a mark.

Intel writes an IOSurface, the RTX writes it, and the CPU and Intel still read Intel's pixels: the RTX's mark calls
IOSurfaceDeviceCache::setCurrent, set_current_device_cache refuses while another cache is current, and neither says so.
The model below follows IOSurface 393.5.11 (macOS 26.7.2) as disassembled: set_current_device_cache, bind_accel through
synchronize_device_caches, lockSurface. The decision is lifted from nvrm-accel.cpp and exercised in every mode of
debug.nvaccel_sv_take; the negative control is the pre-fix mark, which must lose the write. It runs on the CPU and says
nothing about the real kernel or the two GPUs: that is xgpuprobe's T8, after a reboot on this kext."""
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

vram = body('static IOReturn nvAccelSurfVram(struct NVRMSurfShareEsc *p, task_t task, unsigned cmd)')
take = vram[vram.index('p->status == NVRM_SS_OTHER_OWNER && !tryOnly'):]
assert before(take, 'nvFamUnlock(a)', '_ZN9IOSurface10bind_accelEjj(surf,', 'nvFamLock(a, false)', 'ctx = ', 'nvAccelSurfDirty(cache, res, p->plane)'), \
    'bind_accel takes the owner\'s accelerator lock and waits for its GPU: it must run outside ours, and the resource must be read again after'
assert 'gSVTake > 1' in take[:take.index('{')], 'debug.nvaccel_sv_take no longer decides the bind'
install = accel[accel.index('static void nvaccelInstallSysctl(void)'):]
for oid in ('nvaccel_sv_take', 'nvaccel_sv_taken', 'nvaccel_sv_other_owner'):
    assert 'sysctl_register_oid(&sysctl__debug_%s)' % oid in install, '%s is not registered' % oid

control = 'static inline bool nvAccelSurfOtherOwner(const void *, const void *) { return false; }\n'

fixture = r'''
#include <cstdio>
%s
struct Cache { unsigned accel, plane, pixels; };
struct Surface { Cache *owner = nullptr; unsigned seed = 0, sysmem = 0; };
static void set_current(Surface &s, Cache *c) { if (s.owner && s.owner != c) return; s.owner = c; s.seed++; }
static void synchronize(Surface &s, unsigned accel, unsigned plane) {
    Cache *o = s.owner;
    if (o && (o->accel != accel || o->plane != plane)) { s.owner = nullptr; s.sysmem = o->pixels; }
}
static unsigned cpu_lock(Surface &s) { synchronize(s, 0, 0); return s.sysmem; }
enum { OK = 0, OTHER_OWNER = 8 };
// the mark of nvAccelSurfDirty and the retry of nvAccelSurfVram; a refused mark that reaches the plugin is copied out under a CPU lock
static int mark(Surface &s, Cache *c, int take) {
    set_current(s, c);
    return take && nvAccelSurfOtherOwner(s.owner, c) ? OTHER_OWNER : OK;
}
static void gpu_write(Surface &s, Cache *c, unsigned pixels, int take, bool tryOnly = false) {
    c->pixels = pixels;
    int st = mark(s, c, take);
    if (st == OTHER_OWNER && !tryOnly && take > 1) { synchronize(s, c->accel, c->plane); st = mark(s, c, take); }
    if (st == OTHER_OWNER && !tryOnly) { cpu_lock(s); s.sysmem = pixels; s.seed++; }
}
int main() {
    int bad = 0;
    for (int take = 1; take <= 2; take++) {
        Surface s; Cache intel{2, 0, 0}, rtx{1, 0, 0};
        gpu_write(s, &intel, 10, take);
        gpu_write(s, &rtx, 11, take);
        const unsigned seed = s.seed; const unsigned got = cpu_lock(s);
        if (got != 11) { printf("FAIL mode %%d: the CPU reads %%u after Intel 10, RTX 11\n", take, got); bad++; }
        if (seed < 2) { printf("FAIL mode %%d: the seed did not move for the RTX's write\n", take); bad++; }
    }
    {   // debug.nvaccel_sv_take=0 is the A/B: it must give the old behaviour back, defect included
        Surface s; Cache intel{2, 0, 0}, rtx{1, 0, 0};
        gpu_write(s, &intel, 10, 0); gpu_write(s, &rtx, 11, 0);
        if (cpu_lock(s) != 10) { puts("FAIL mode 0 no longer ignores the refusal"); bad++; }
    }
    {   // the bind leaves the owner of an unrelated surface alone when it is already ours, and an empty surface takes the mark at once
        Surface s; Cache rtx{1, 0, 0}; gpu_write(s, &rtx, 1, 2); const unsigned seed = s.seed; gpu_write(s, &rtx, 2, 2);
        if (s.owner != &rtx || s.seed != seed + 1 || s.sysmem) { puts("FAIL an RTX surface was paged off by its own mark"); bad++; }
    }
    {   // a try never waits: the refusal goes back and nothing moves
        Surface s; Cache intel{2, 0, 0}, rtx{1, 0, 0}; gpu_write(s, &intel, 10, 2); const unsigned seed = s.seed; gpu_write(s, &rtx, 11, 2, true);
        if (s.owner != &intel || s.seed != seed) { puts("FAIL a try took the surface"); bad++; }
    }
    {   // no owner: the mark was taken and a lock cleared it since, which is not a refusal
        Cache rtx{1, 0, 0}, intel{2, 0, 0};
        if (nvAccelSurfOtherOwner(nullptr, &rtx)) { puts("FAIL no owner counted as a refusal"); bad++; }
        if (nvAccelSurfOtherOwner(&rtx, &rtx)) { puts("FAIL our own cache counted as a refusal"); bad++; }
        if (!nvAccelSurfOtherOwner(&intel, &rtx)) { puts("FAIL another accelerator's cache did not count as a refusal"); bad++; }
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
