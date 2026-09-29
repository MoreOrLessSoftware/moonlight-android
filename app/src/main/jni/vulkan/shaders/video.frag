#version 450
#extension GL_GOOGLE_include_directive : require

// The sampler carries an immutable VkSamplerYcbcrConversion, so sampling returns
// non-linear R'G'B' in the stream's own color encoding.
layout(set = 0, binding = 0) uniform sampler2D uVideo;

#include "video_common.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = clamp(vUv, pc.uvClamp.xy, pc.uvClamp.zw);
    outColor = outputColor(texture(uVideo, uv).rgb);
}
