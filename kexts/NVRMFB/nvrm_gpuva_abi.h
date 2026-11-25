/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once

#define NVRM_GPUVA_ABI_VERSION 1u

#define NVRM_GPUVA_FLAG_UNMAP_ONLY 1u

#define NVRM_GPUVA_FN_ALLOC   "nvGpuVaAlloc"
#define NVRM_GPUVA_FN_MAP     "nvGpuVaMap"
#define NVRM_GPUVA_FN_FREE    "nvGpuVaFree"

// Asks NVRM for the two kernel pointers a display kext needs. They were IORegistry properties, which any process can
// read, and that gave away the kernel's address layout.
#define NVRM_DISPLAY_PTRS_ABI_VERSION 1u
#define NVRM_DISPLAY_PTRS_FN "nvDisplayPointers"
struct NVRMDisplayPointers {
    unsigned int version;
    void        *kapi;
    void        *physForVa;
};

// Asks NVRM what its NVKMS interface does beyond the stock one: NVRM_KAPI_CAP_* bits in *(unsigned int *)p1. An NVRM that
// does not know the call answers kIOReturnUnsupported, and the display kext keeps to what every NVRM does.
#define NVRM_KAPI_CAPS_FN "nvKapiCaps"
// A request that changes the HDR infoframe (and the output LUT) and nothing else on a lit head is a flip, not a mode set,
// also where the display hardware cannot tie infoframes to a flip (Blackwell GB20x)
#define NVRM_KAPI_CAP_HDR_INFOFRAME_FLIP 0x1u

#define NVRM_FLIP_ABI_VERSION 2u

#define NVRM_FLIP_FN  "nvFlipToSurface"
// NVRMFlipRequest.flags: 0x1 back to the framebuffer's own surface (nvFlipToSurfacePure); 0x2 the surface is l10r (10-bit,
// red in the high bits), sent only to a framebuffer whose "NVRMFBFlipL10R" property is true.
#define NVRM_FLIP_FLAG_HOME 0x1u
#define NVRM_FLIP_FLAG_L10R 0x2u

struct NVRMFlipRequest {
    unsigned int version;
    unsigned int flags;
    void        *kapiMemory;
    unsigned int width, height;
    unsigned int pitch;
    int          flipResult;
    unsigned long long allocationCookie;
};
#define NVRM_GPUVA_FN_HANDLES "nvRmHandles"

struct NVRMGpuVaRequest {
    unsigned int       version;
    unsigned int       flags;
    void              *kapiDevice;
    void              *kapiMemory;
    unsigned int       hClient;
    unsigned int       hDevice;
    unsigned int       hMemory;
    unsigned int       hVirt;
    unsigned long long size;
    unsigned long long align;
    unsigned long long gpuva;
    unsigned int       isVidmem;
    int                status;
};

struct NVRMRmHandles {
    unsigned int version;
    void        *kapiDevice;
    unsigned int hClient;
    unsigned int hDevice;
    unsigned int hSubDevice;
};
