#version 450
// A deliberately realistic first pass: parameters, an include, and the standard libretro block.
#pragma format R8G8B8A8_UNORM
#pragma name scanline_pass

#include "common.inc"

#pragma parameter SCANLINE_WEIGHT "Scanline Weight" 0.3 0.0 1.0 0.01
#pragma parameter SCANLINE_GAP "Scanline Gap" 2.0 1.0 8.0 1.0

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
    vec4 SourceSize;
    vec4 OriginalSize;
    vec4 OutputSize;
    uint FrameCount;
    float BRIGHTNESS;
    float SCANLINE_WEIGHT;
    float SCANLINE_GAP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 1) uniform sampler2D Source;

void main()
{
    vec3 c = texture(Source, vTexCoord).rgb;
    float line = mod(floor(vTexCoord.y * global.SourceSize.y), global.SCANLINE_GAP);
    float dim = line < 1.0 ? (1.0 - global.SCANLINE_WEIGHT) : 1.0;
    FragColor = vec4(apply_brightness(c * dim, global.BRIGHTNESS), 1.0);
}
