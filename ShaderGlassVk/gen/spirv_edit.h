/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

Small edits to compiled SPIR-V, applied before it reaches the catalogue.
*/

#pragma once

#include <cstdint>
#include <vector>

namespace shaderglass {

// Remove every RelaxedPrecision decoration from a module.
//
// RelaxedPrecision is what "precision mediump float;" becomes. It is a hint -- an implementation
// may honour it or compute at full precision, and full precision always satisfies it -- so dropping
// it changes nothing a shader is entitled to observe.
//
// It is dropped from *vertex* stages because leaving it there is not free: `crt/simple-crt` and
// `crt/simple-crt-fxaa` segfault NVIDIA's shader compiler inside vkCreateGraphicsPipelines, and
// removing the vertex stage's RelaxedPrecision decorations is sufficient to avoid it (removing the
// fragment stage's is not). Against that, a fullscreen-quad pass runs its vertex shader over four
// vertices a frame, where relaxed precision cannot buy anything measurable. The fragment stage,
// where it might, keeps its decorations.
//
// Returns how many decorations were removed. IDs and the id-bound are untouched, so the result is
// still a valid module.
size_t StripRelaxedPrecision(std::vector<uint32_t>& spirv);

}  // namespace shaderglass
