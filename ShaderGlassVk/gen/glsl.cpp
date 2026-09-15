/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "glsl.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <ostream>
#include <sstream>
#include <stdexcept>

namespace shaderglass {

void GlslangInit() { glslang_initialize_process(); }
void GlslangShutdown() { glslang_finalize_process(); }

std::vector<uint32_t> GenerateSPIRV(const char* source, bool fragment, std::ostream& log,
                                    bool& warn) {
    std::vector<uint32_t> bin;
    const glslang_stage_t stage = fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;

    glslang_input_t input {};
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = stage;
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_0;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_0;
    input.code = source;
    input.default_version = 100;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.force_default_version_and_profile = false;
    input.forward_compatible = false;
    input.messages = GLSLANG_MSG_DEFAULT_BIT;
    input.resource = glslang_default_resource();

    glslang_shader_t* shader = glslang_shader_create(&input);

    if (!glslang_shader_preprocess(shader, &input)) {
        std::ostringstream msg;
        msg << "GLSL preprocessing failed:\n"
            << glslang_shader_get_info_log(shader) << "\n"
            << glslang_shader_get_info_debug_log(shader);
        glslang_shader_delete(shader);
        throw std::runtime_error(msg.str());
    }

    if (!glslang_shader_parse(shader, &input)) {
        std::ostringstream msg;
        msg << "GLSL parsing failed:\n"
            << glslang_shader_get_info_log(shader) << "\n"
            << glslang_shader_get_info_debug_log(shader);
        glslang_shader_delete(shader);
        throw std::runtime_error(msg.str());
    }

    glslang_program_t* program = glslang_program_create();
    glslang_program_add_shader(program, shader);

    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        std::ostringstream msg;
        msg << "GLSL linking failed:\n"
            << glslang_program_get_info_log(program) << "\n"
            << glslang_program_get_info_debug_log(program);
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        throw std::runtime_error(msg.str());
    }

    glslang_program_SPIRV_generate(program, stage);

    bin.resize(glslang_program_SPIRV_get_size(program));
    glslang_program_SPIRV_get(program, bin.data());

    if (const char* messages = glslang_program_SPIRV_get_messages(program)) {
        log << messages << std::endl;
        warn = true;
    }

    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return bin;
}

}  // namespace shaderglass
