/*
 * NvGfx: issue one RM control with kernel privilege, for the internal PRH/PMGR controls that userland cannot reach
 * (notes/power.md). Its only caller is the TGP mode switch in nvrm_power.cpp, and only that control is accepted: no
 * user client reaches this, and the allow-list keeps it from becoming a generic escape if one ever does. Compiled
 * with RM's own osapi.c command line (rmcc.py) because it needs OBJGPU.
 */
// <wtl> fork: nvgfx | author: watchthelight </wtl>

#include <nv.h>
#include <nv-priv.h>
#include <os/os.h>
#include <osapi.h>
#include "gpu/gpu.h"
#include <core/thread_state.h>
#include <core/locks.h>
#include "rmapi/rmapi.h"
#include "nvrm_physctl_abi.h"
#include "gpu_mgr/gpu_mgr.h"
#include "nvrm_smc_abi.h"

NV_STATUS rm_nvgfx_phys_control(nv_state_t *nv, NvU32 mode, NvU32 hClient, NvU32 hObject, NvU32 cmd, void *pParams,
                                NvU32 paramsSize);

NV_STATUS rm_nvgfx_phys_control(nv_state_t *nv, NvU32 mode, NvU32 hClient, NvU32 hObject, NvU32 cmd, void *pParams,
                                NvU32 paramsSize)
{
    THREAD_STATE_NODE threadState;
    NV_STATUS st;
    void *fp;
    nvidia_stack_t *sp = NULL;
    OBJGPU *pGpu = nv ? NV_GET_NV_PRIV_PGPU(nv) : NULL;

    if (pGpu == NULL || paramsSize > NVRM_PHYSCTL_MAX_PARAMS || (paramsSize && pParams == NULL) || mode > NVRM_PHYSCTL_KERNEL)
        return NV_ERR_INVALID_ARGUMENT;
    if (cmd != NVGFX_CTRL_CONFIGURE_TGP_MODE || mode != NVRM_PHYSCTL_PHYS_SUBDEV)
        return NV_ERR_NOT_SUPPORTED;
    if (paramsSize == 0) pParams = NULL;

    NV_ENTER_RM_RUNTIME(sp, fp);
    threadStateInit(&threadState, THREAD_STATE_FLAGS_NONE);
    // Same order as every RM entry: API lock, then the GPU locks (RMAPI_GPU_LOCK_INTERNAL and the GSP RPC path both
    // expect both held).
    if ((st = rmapiLockAcquire(API_LOCK_FLAGS_NONE, RM_LOCK_MODULES_OSAPI)) == NV_OK) {
        if ((st = rmGpuLocksAcquire(GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_OSAPI)) == NV_OK) {
            if (!FULL_GPU_SANITY_CHECK(pGpu)) {
                st = NV_ERR_GPU_NOT_FULL_POWER;
            } else if (mode == NVRM_PHYSCTL_KERNEL) {
                RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
                st = pRmApi->Control(pRmApi, hClient, hObject, cmd, pParams, paramsSize);
            } else {
                RM_API *pRmApi = GPU_GET_PHYSICAL_RMAPI(pGpu);
                NvHandle h = mode == NVRM_PHYSCTL_PHYS_DEVICE ? pGpu->hInternalDevice
                           : mode == NVRM_PHYSCTL_PHYS_CLIENT ? pGpu->hInternalClient : pGpu->hInternalSubdevice;
                st = pRmApi->Control(pRmApi, pGpu->hInternalClient, h, cmd, pParams, paramsSize);
            }
            rmGpuLocksRelease(GPUS_LOCK_FLAGS_NONE, NULL);
        }
        rmapiLockRelease();
    }
    threadStateFree(&threadState, THREAD_STATE_FLAGS_NONE);
    NV_EXIT_RM_RUNTIME(sp, fp);
    return st;
}

void rm_nvgfx_smc_api(struct NVRMSmcApi *api);

/* What this build's RM looks like to SMCNvidiaGPU, from the compiler. NVRM.cpp adds the sleep gate, which is its own. */
void rm_nvgfx_smc_api(struct NVRMSmcApi *api)
{
    api->size = sizeof *api;
    api->gpumgrGetGpu = (void *)gpumgrGetGpu;
    api->createRusd = (void *)gpuCreateRusdMemory_IMPL;
    api->linkRusd = (void *)gpuRusdInitPhysical_KERNEL;
    api->lock = (void *)rmapiLockAcquire;
    api->unlock = (void *)rmapiLockRelease;
    api->gpuLock = (void *)rmGpuLocksAcquire;
    api->gpuUnlock = (void *)rmGpuLocksRelease;
    api->hInternalClient = __builtin_offsetof(struct OBJGPU, hInternalClient);
    api->hInternalSubdevice = __builtin_offsetof(struct OBJGPU, hInternalSubdevice);
    api->physicalRmApi = __builtin_offsetof(struct OBJGPU, physicalRmApi);
    api->rmapiControl = __builtin_offsetof(struct OBJGPU, physicalRmApi) + __builtin_offsetof(RM_API, Control);
    api->usdMemDesc = __builtin_offsetof(struct OBJGPU, userSharedData) + __builtin_offsetof(GpuSharedDataMap, pMemDesc);
    api->usdMapBuffer = __builtin_offsetof(struct OBJGPU, userSharedData) + __builtin_offsetof(GpuSharedDataMap, pMapBuffer);
    api->usdData = __builtin_offsetof(struct OBJGPU, userSharedData) + __builtin_offsetof(GpuSharedDataMap, data);
}
