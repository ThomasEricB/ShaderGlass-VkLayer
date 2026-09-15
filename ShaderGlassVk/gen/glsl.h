/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Ported from ShaderGC's GLSL.cpp, essentially unchanged -- because it was already right. It compiles
with GLSLANG_CLIENT_VULKAN and GLSLANG_TARGET_SPV, so ShaderGC has always produced Vulkan SPIR-V.
The Windows build then handed that to SPIRV-Cross to turn back into HLSL for its D3D11 renderer; this
port simply keeps the SPIR-V and deletes the step that threw it away.
*/

#pragma once

#include <cstdint>
#include <iosfwd>
#include <vector>

namespace shaderglass {

// glslang keeps process-wide state; call these once around any compilation.
void GlslangInit();
void GlslangShutdown();

std::vector<uint32_t> GenerateSPIRV(const char* source, bool fragment, std::ostream& log,
                                    bool& warn);

}  // namespace shaderglass
