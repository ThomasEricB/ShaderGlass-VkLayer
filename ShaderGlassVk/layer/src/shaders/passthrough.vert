#version 450
/*
ShaderGlassVk built-in passthrough -- vertex stage.

Written to the libretro slang contract, not to something convenient, so the pass executor built
around it is the one that will run real presets in phase 4. That means: a vec4 Position and a vec2
TexCoord attribute, an MVP that maps the unit quad into clip space, and the standard uniform block
with SourceSize / OriginalSize / OutputSize / FrameCount -- which real presets read even when this
one does not.
*/

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
    vec4 SourceSize;    // w, h, 1/w, 1/h of what this pass samples
    vec4 OriginalSize;  // the same for the frame as the game presented it
    vec4 OutputSize;    // the same for what this pass writes
    uint FrameCount;
} global;

layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;

layout(location = 0) out vec2 vTexCoord;

void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}
