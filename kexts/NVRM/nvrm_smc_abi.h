/*
 * What SMCNvidiaGPU calls and reads inside NVRM. The compiler fills it in on every build, so that kext keeps no table of
 * offsets for one binary and compares no hash: a rebuilt NVRM needs nothing from it.
 * Shared by NVRM.cpp, nvrm_physctl.c and SMCNvidiaGPU; the copy there must stay the same.
 *
 * The struct grows only at its end, and size says how far each side goes: the caller states how much of it it has (0 from
 * a caller that predates the rule and has NVRM_SMC_API_SIZE_V1 bytes), NVRM fills no more than that and reports how much
 * it filled. So either side can be the older one.
 */
#pragma once
#define NVRM_SMC_API_FN      "NVRMSmcApi"
#define NVRM_SMC_API_VERSION 1u
#define NVRM_SMC_API_SIZE_V1 112u   /* the struct up to usdData, as it was before kextStart */

struct NVRMSmcApi {
    unsigned int version;   /* in: NVRM_SMC_API_VERSION */
    unsigned int size;      /* in: sizeof the caller's struct, or 0; out: how much of it NVRM filled */
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
    /* Where the kernel loaded NVRM, from NVRM's own kmod_info: the range the kernel itself uses to tell which kext a code
     * address belongs to. Every entry point above lies in [kextStart, kextStart + kextSize), so the caller can check them
     * before it calls any. Both 0 from an NVRM that does not state it. */
    unsigned long long kextStart, kextSize;
};
#ifdef __cplusplus
static_assert(
#else
_Static_assert(
#endif
    __builtin_offsetof(struct NVRMSmcApi, kextStart) == NVRM_SMC_API_SIZE_V1,
    "callers built before kextStart compare the size NVRM reports with this one");
