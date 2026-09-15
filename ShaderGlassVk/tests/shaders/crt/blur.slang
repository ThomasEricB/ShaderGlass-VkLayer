#version 450
// A second pass, sampling both its own input and the original frame -- which is what gives it two
// textures to reflect rather than one.
#pragma parameter BLUR_RADIUS "Blur Radius" 1.0 0.0 4.0 0.1

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
    vec4 SourceSize;
    vec4 OriginalSize;
    vec4 OutputSize;
    uint FrameCount;
    float BLUR_RADIUS;
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
layout(set = 0, binding = 2) uniform sampler2D Original;

void main()
{
    vec2 o = global.SourceSize.zw * global.BLUR_RADIUS;
    vec3 c = texture(Source, vTexCoord).rgb * 0.5;
    c += texture(Source, vTexCoord + vec2(o.x, 0.0)).rgb * 0.25;
    c += texture(Source, vTexCoord - vec2(o.x, 0.0)).rgb * 0.25;
    c = mix(c, texture(Original, vTexCoord).rgb, 0.1);
    FragColor = vec4(c, 1.0);
}
