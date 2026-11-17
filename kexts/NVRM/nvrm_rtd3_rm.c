/*
 * NullMoth NVIDIA driver for macOS
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * After a power cut at run time: tell the host about the work clients queued while the GPU had no power.
 *
 * A client submits without a system call. It writes its channel's put pointer in USERD, which is system memory and
 * keeps the value, and then its work-submit token in the usermode doorbell, which is a register of the GPU: with the
 * power off that second write goes nowhere, and nothing sends it again. So once the GPU is back every channel's
 * doorbell is rung once, the way RM rings it for its own channels (kfifoGenerateWorkSubmitToken, then
 * kfifoUpdateUsermodeDoorbell). A channel with nothing queued finds put equal to get and does nothing.
 *
 * Compiled with RM's own osapi.c command line (rmcc.py) because it needs KernelFifo and KernelChannel.
 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#include <nv.h>
#include <nv-priv.h>
#include <os/os.h>
#include <osapi.h>
#include "gpu/gpu.h"
#include <core/thread_state.h>
#include <core/locks.h>
#include "kernel/gpu/fifo/kernel_fifo.h"
#include "kernel/gpu/fifo/kernel_channel.h"
#pragma clang diagnostic pop

NV_STATUS rm_xnu_ring_channels(nv_state_t *nv, NvU32 *pRung, NvU32 *pChannels);

NV_STATUS rm_xnu_ring_channels(nv_state_t *nv, NvU32 *pRung, NvU32 *pChannels)
{
    THREAD_STATE_NODE threadState;
    NV_STATUS st;
    void *fp;
    nvidia_stack_t *sp = NULL;
    OBJGPU *pGpu = nv ? NV_GET_NV_PRIV_PGPU(nv) : NULL;
    NvU32 rung = 0, channels = 0;

    if (pRung) *pRung = 0;
    if (pChannels) *pChannels = 0;
    if (pGpu == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    NV_ENTER_RM_RUNTIME(sp, fp);
    threadStateInit(&threadState, THREAD_STATE_FLAGS_NONE);
    // Same order as every RM entry: API lock, then the GPU locks
    if ((st = rmapiLockAcquire(API_LOCK_FLAGS_NONE, RM_LOCK_MODULES_FIFO)) == NV_OK) {
        if ((st = rmGpuLocksAcquire(GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_FIFO)) == NV_OK) {
            KernelFifo *pKernelFifo = GPU_GET_KERNEL_FIFO(pGpu);
            if (pKernelFifo == NULL || !FULL_GPU_SANITY_CHECK(pGpu)) {
                st = NV_ERR_GPU_NOT_FULL_POWER;
            } else {
                CHANNEL_ITERATOR it;
                KernelChannel *pKernelChannel = NULL;
                kfifoGetChannelIterator(pGpu, pKernelFifo, &it, INVALID_RUNLIST_ID);
                while (kfifoGetNextKernelChannel(pGpu, pKernelFifo, &it, &pKernelChannel) == NV_OK) {
                    NvU32 token = 0;
                    channels++;
                    if (kfifoGenerateWorkSubmitToken(pGpu, pKernelFifo, pKernelChannel, &token, NV_TRUE) != NV_OK)
                        continue;
                    if (kfifoUpdateUsermodeDoorbell(pGpu, pKernelFifo, token) == NV_OK)
                        rung++;
                }
            }
            rmGpuLocksRelease(GPUS_LOCK_FLAGS_NONE, NULL);
        }
        rmapiLockRelease();
    }
    threadStateFree(&threadState, THREAD_STATE_FLAGS_NONE);
    NV_EXIT_RM_RUNTIME(sp, fp);
    if (pRung) *pRung = rung;
    if (pChannels) *pChannels = channels;
    return st;
}
