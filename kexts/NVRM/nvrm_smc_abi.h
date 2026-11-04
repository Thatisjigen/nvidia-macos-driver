/*
 * What SMCNvidiaGPU calls and reads inside NVRM. The compiler fills it in on every build, so that kext keeps no table of
 * offsets for one binary and compares no hash: a rebuilt NVRM needs nothing from it.
 * Shared by NVRM.cpp, nvrm_physctl.c and SMCNvidiaGPU; the copy there must stay the same.
 */
#pragma once
#define NVRM_SMC_API_FN      "NVRMSmcApi"
#define NVRM_SMC_API_VERSION 1u

struct NVRMSmcApi {
    unsigned int version;   /* in: NVRM_SMC_API_VERSION */
    unsigned int size;      /* out: sizeof this struct */
    void *gpumgrGetGpu;     /* OBJGPU *gpumgrGetGpu(NvU32 gpuInstance) */
    void *createRusd;       /* NV_STATUS gpuCreateRusdMemory_IMPL(OBJGPU *): allocates and maps the RUSD block, links it to the GSP */
    void *linkRusd;         /* NV_STATUS gpuRusdInitPhysical_KERNEL(OBJGPU *): sends the RUSD address to the GSP again */
    void *lock;             /* NV_STATUS rmapiLockAcquire(NvU32 flags, NvU32 module) */
    void *unlock;           /* void rmapiLockRelease(void) */
    void *gpuLock;          /* NV_STATUS rmGpuLocksAcquire(NvU32 flags, NvU32 module) */
    void *gpuUnlock;        /* NV_STATUS rmGpuLocksRelease(NvU32 flags, OBJGPU *pDpcGpu) */
    void *pmEnter;          /* int nv_xnu_pm_enter(void): NVRM's system-sleep gate */
    void *pmExit;           /* void nv_xnu_pm_exit(int counted) */
    /* byte offsets in OBJGPU */
    unsigned int hInternalClient, hInternalSubdevice;
    unsigned int physicalRmApi, rmapiControl;           /* the RM_API, and its Control member */
    unsigned int usdMemDesc, usdMapBuffer, usdData;     /* userSharedData: pMemDesc, pMapBuffer, data */
};
