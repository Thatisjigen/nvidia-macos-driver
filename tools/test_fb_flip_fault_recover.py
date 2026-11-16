#!/usr/bin/env python3
"""A flip that never lands no longer blocks the head until reboot, and a refused colour setting goes back.

On an HDMI monitor a debug.nvrmfb_color_* change asked NVKMS for a mode set it refused; the next accepted flip never
completed, NVRMFB set fSubmissionFaulted and refused every flip after it, so the accelerator copied on the CPU at about
3 frames a second into a surface the head no longer showed, and the panel froze until reboot. Now every place that marks
the fault also queues recoverFire: it clears the fault if the flip landed late, otherwise sets the current mode again on
the driver's own surface, a few times apart (nvrmRecoverDelayMs); a fault inside that attempt queues nothing more. A colour
setting NVKMS refuses on a lit head is put back, the head lit again as it was, and the sysctl answers EINVAL. The control
is the source of before: the fault set with no way back, and a sysctl that kept a refused value."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
fb = (root / 'kexts/NVRMFB/fb/nvrm-fb.cpp').read_text()
def body(signature):
    s = fb.index(signature)
    return fb[s:fb.index('\n}\n', s)]
fault = body('void NVRMNVDAFramebuffer::submissionFault(const char *why)')
assert 'fSubmissionFaulted = true;' in fault and 'scheduleCall(fRecover, deadline);' in fault
assert 'if (fRecovering || !fRecover || fRecoverTries >= kNvrmRecoverTries) return;' in fault
# the only other place that sets the fault is a failed recovery attempt
assert fb.count('fSubmissionFaulted = true;') == 2, 'a fault is set somewhere without queueing the recovery'
assert fb.count('submissionFault("') == 3
rec = body('bool NVRMNVDAFramebuffer::recoverHead()')
for need in ['getFlipPendingStatus(fDev, fHead, NVKMS_KAPI_LAYER_PRIMARY_IDX, &pending) && !pending', 'lr->config.surface = fSurf;',
             'fSubmissionFaulted = false; fSubmissionSeen = false; fRecovering = true;', 'fRecovering = false;',
             'fFrontSurf = fSurf; fFrontL10r = false; fRecoverTries = 0;']:
    assert need in rec, 'recoverHead: ' + need
fire = body('void NVRMNVDAFramebuffer::recoverFire(')
assert '++me->fRecoverTries >= kNvrmRecoverTries' in fire and 'me->scheduleCall(me->fRecover, deadline);' in fire
assert 'fRecover = nvrmfbCall(recoverFire, this);' in fb and '&fRecover,' in fb, 'the recovery call is not allocated or not freed'
sysctl = body('static int nvrmfbColorSysctl SYSCTL_HANDLER_ARGS')
assert sysctl.index('*(int *)arg1 = v;') < sysctl.index('if (!nvrmfbColorChanged(which != 3)) {') < sysctl.index('*(int *)arg1 = was;')
assert 'return EINVAL;' in sysctl
changed = body('bool nvrmfbColorChanged(bool needsModeset)\n{')
assert 'if (!fb->reflip() && lit) ok = false;' in changed
fixture = r'''
#include <cassert>
#include <cstdio>
#include "nvrm-fb-safety.h"
int main() {
    unsigned total = 0, prev = 0;
    for (unsigned t = 0; t < (unsigned)kNvrmRecoverTries; t++) { const unsigned d = nvrmRecoverDelayMs(t); assert(d >= prev); prev = d; total += d; }
    assert(nvrmRecoverDelayMs(0) <= 250);                       // the first try comes soon
    assert(total >= 15000 && total <= 30000);                   // the tries cover some seconds, not minutes
    assert(nvrmRecoverDelayMs(99) == nvrmRecoverDelayMs(kNvrmRecoverTries - 1));
    puts("PASS: a blocked head is lit again soon, then a few times further apart");
}
'''
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d); (d / 't.cpp').write_text(fixture)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Werror', f'-I{root}/kexts/NVRMFB/fb', str(d / 't.cpp'), '-o', str(d / 't')], check=True)
    subprocess.run([str(d / 't')], check=True)
print('PASS: every fault queues the recovery, recovery clears a late flip or lights the head again, a refused colour goes back')
