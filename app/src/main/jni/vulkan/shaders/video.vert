#version 450

// Keep in sync with PushConstants in vulkan_renderer.cpp
layout(push_constant) uniform PushConstants {
    vec4 uvRect;   // xy = UV of the crop's top-left corner, zw = UV size of the crop
    vec4 uvClamp;  // xy = min UV, zw = max UV (half a texel inside the crop)
    vec4 params;   // x = dither amplitude, y = frame counter, z = output mode, w = content peak nits
    vec4 params2;  // x = SDR reference white nits
} pc;

layout(location = 0) out vec2 vUv;

void main() {
    // One triangle that covers the whole viewport: (0,0), (2,0), (0,2)
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUv = pc.uvRect.xy + pos * pc.uvRect.zw;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
