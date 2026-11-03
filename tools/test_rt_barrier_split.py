#!/usr/bin/env python3
"""A render-target barrier splits the pass only when NVK draws through a tiled shadow, and mipmap autofill covers the wide formats."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
objects = (root / 'plugin/NVMTLObjects.m').read_text()
vk = (root / 'plugin/nvmtl_vk.c').read_text()
fixture = r'''
#include <assert.h>
#include "nvmtl_rt_shadow.h"
int main(void) {
    // the rule of nvk_image_plane_aligned_for_linear_attachment: row and address multiples of 128 bytes
    assert(nvmtl_lin_kind(7680, 0) == NVMTL_LIN_DIRECT);     // a 1920 wide display surface
    assert(nvmtl_lin_kind(1280, 4096) == NVMTL_LIN_DIRECT);
    assert(nvmtl_lin_kind(1856, 0) == NVMTL_LIN_SHADOW);     // a 460 wide window with rows aligned to 64 bytes
    assert(nvmtl_lin_kind(1216, 0) == NVMTL_LIN_SHADOW);
    assert(nvmtl_lin_kind(1280, 64) == NVMTL_LIN_SHADOW);
    uint32_t direct[] = { NVMTL_LIN_DIRECT }, shadow[] = { NVMTL_LIN_SHADOW }, none[] = { NVMTL_LIN_NONE };
    uint32_t mixed[] = { NVMTL_LIN_DIRECT, NVMTL_LIN_NONE }, two[] = { NVMTL_LIN_DIRECT, NVMTL_LIN_DIRECT };
    assert(!nvmtl_pass_shadowed(direct, 1, 0));
    assert(nvmtl_pass_shadowed(shadow, 1, 0));
    assert(!nvmtl_pass_shadowed(none, 1, 0));                // an optimal image is drawn directly
    assert(!nvmtl_pass_shadowed(none, 1, 1));
    assert(nvmtl_pass_shadowed(mixed, 2, 0));                // NVK: every attachment must be linear to draw linear
    assert(!nvmtl_pass_shadowed(two, 2, 0));
    assert(nvmtl_pass_shadowed(direct, 1, 1));               // NVK: depth is never linear
    assert(!nvmtl_pass_shadowed(direct, 0, 0));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    src = pathlib.Path(d) / 't.c'
    src.write_text(fixture)
    subprocess.check_call(['clang', '-Wall', '-Werror', '-I', str(root / 'plugin'), str(src), '-o', str(pathlib.Path(d) / 't')])
    subprocess.check_call([str(pathlib.Path(d) / 't')])
# the wiring the fixture cannot reach
barrier = objects[objects.index('- (void)nvmtlRenderTargetBarrier {'):]
barrier = barrier[:barrier.index('- (void)updateFence:')]
for need in ('nvmtl_pass_shadowed(kinds, n, 0)', 'nvmtl_vk_cmd_end_render', 'NVMTL_LOAD_COLOUR(i)', 'nvmtl_vk_cmd_begin_pass',
             '_pass.load = keep', '_svFor = nil', 'nvmtl_vk_cmd_set_scissor', '!_passDepth', '_passSamples <= 1', '!_visActive', '!_resolve'):
    assert need in barrier, need
assert barrier.index('nvmtl_vk_cmd_end_render') < barrier.index('nvmtl_vk_cmd_begin_pass')
assert '- (void)textureBarrier { [self nvmtlRenderTargetBarrier]; }' in objects
assert 'if (scope & MTLBarrierScopeRenderTargets) [self nvmtlRenderTargetBarrier]' in objects
assert 'out->lin = nvmtl_lin_kind(row_pitch, at);' in vk
autofill = objects[objects.index('static void nvmtl_mip_autofill('):]
autofill = autofill[:autofill.index('- (void)replaceRegion:')]
for fmt in ('MTLPixelFormatRGBA16Float', 'MTLPixelFormatBGR10A2Unorm', 'MTLPixelFormatRG11B10Float'):
    assert fmt in autofill, fmt
assert autofill.index('MTLPixelFormatRGBA16Float') < autofill.index('nvmtl_vk_image_gen_mipmaps') < autofill.index('default: return')
print('pass: render target barrier and mipmap autofill')
