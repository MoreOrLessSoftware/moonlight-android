#version 450
#extension GL_GOOGLE_include_directive : require

// Full range YCbCr 4:2:0 in separate Y, Cb and Cr planes, as PyroWave decodes it
layout(set = 0, binding = 0) uniform sampler2D uPlanes[3];

#include "video_common.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = clamp(vUv, pc.uvClamp.xy, pc.uvClamp.zw);
    float y = texture(uPlanes[0], uv).r;

    // The host sites chroma with the left luma sample of each pair, not between them
    vec2 chromaUv = uv + vec2(pc.ycbcr.w, 0.0);
    float cb = texture(uPlanes[1], chromaUv).r - pc.ycbcr.z;
    float cr = texture(uPlanes[2], chromaUv).r - pc.ycbcr.z;

    float kr = pc.ycbcr.x;
    float kb = pc.ycbcr.y;
    float r = y + 2.0 * (1.0 - kr) * cr;
    float b = y + 2.0 * (1.0 - kb) * cb;
    float g = (y - kr * r - kb * b) / (1.0 - kr - kb);
    outColor = outputColor(vec3(r, g, b));
}
