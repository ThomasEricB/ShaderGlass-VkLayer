#version 450
/*
ShaderGlassVk built-in passthrough -- fragment stage.

Samples the source and writes it out unchanged. That is the point: with a nearest sampler and
matching rasters the composed frame is bit-identical to the one the game presented, so the whole
path -- copy in, render pass, draw, copy back -- can be proved correct before any real shader is
involved.
*/

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
    vec4 SourceSize;
    vec4 OriginalSize;
    vec4 OutputSize;
    uint FrameCount;
} global;

layout(set = 0, binding = 1) uniform sampler2D Source;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
    FragColor = texture(Source, vTexCoord);
}
