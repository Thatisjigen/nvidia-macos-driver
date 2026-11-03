#pragma once
// NVK draws a linear colour attachment directly only when its row and address are multiples of 128 bytes and every attachment
// of the pass is linear and so aligned, with no depth (nvk_rendering_linear). Otherwise it draws into a tiled shadow, copied
// from the image when the pass begins and back when it ends, so until the pass ends the image itself holds old pixels.
#include <stddef.h>
#include <stdint.h>
enum { NVMTL_LIN_NONE = 0, NVMTL_LIN_DIRECT = 1, NVMTL_LIN_SHADOW = 2 };
static inline uint32_t nvmtl_lin_kind(size_t row_pitch, size_t offset)
{
    return (row_pitch % 128 || offset % 128) ? NVMTL_LIN_SHADOW : NVMTL_LIN_DIRECT;
}
// kinds: the nvk_image lin kind of each colour attachment the pass has. YES when one of them is linear and shadowed.
static inline int nvmtl_pass_shadowed(const uint32_t *kinds, uint32_t n, int has_depth)
{
    int linear = 0, other = 0, shadow = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (kinds[i] == NVMTL_LIN_NONE) other = 1;
        else { linear = 1; if (kinds[i] == NVMTL_LIN_SHADOW) shadow = 1; }
    }
    return linear && (shadow || other || has_depth);
}
