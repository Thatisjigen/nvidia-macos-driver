#!/usr/bin/env python3
"""A shader's constexpr samplers are made on the device that draws. Made on the default device, with a second GPU next to this one, they
have no VkSampler here and the slot holds the dummy sampler (seen in WindowServer: the glass read its SDF past the edge and repeated it)."""
import pathlib
root = pathlib.Path(__file__).resolve().parents[1]
embedded = (root / 'plugin/NVMTLEmbedded.m').read_text()
made = embedded[embedded.index('static id<MTLSamplerState> nvmtl_static_sampler_from'):]
made = made[:made.index('\n}\n')]
code = [l for l in made.splitlines() if not l.strip().startswith('//')]
assert not any('MTLCreateSystemDefaultDevice' in l for l in code)
assert any('[gNVMTLMainDevice newSamplerStateWithDescriptor:d]' in l for l in code)
assert not any('MTLCreateSystemDefaultDevice' in l and 'Sampler' in l for l in embedded.splitlines() if not l.strip().startswith('//'))
vk = (root / 'plugin/nvmtl_vk.c').read_text()
dummy = vk[vk.index('The sampler a shader gets at a slot nothing was bound to'):]
dummy = dummy[:dummy.index('&g_dummySampler')]
assert 'sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;' in dummy
print('pass: constexpr samplers are made on the device that draws, and the dummy sampler clamps')
