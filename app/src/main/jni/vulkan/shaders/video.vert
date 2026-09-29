#version 450

// Keep in sync with PushConstants in vulkan_renderer.cpp
layout(push_constant) uniform PushConstants {
    vec4 uvRect;   // xy = UV of the crop's top-left corner, zw = UV size of the crop
    vec4 uvClamp;  // xy = min UV, zw = max UV (half a texel inside the crop)
    vec4 params;   // x = dither amplitude, y = frame counter, z = output mode, w = content peak nits
    vec4 params2;  // x = SDR reference white nits, y = quarter turns clockwise to pre-rotate by
    vec4 ycbcr;    // Used by the planar fragment shader
} pc;

layout(location = 0) out vec2 vUv;

void main() {
    // One triangle that covers the whole viewport: (0,0), (2,0), (0,2)
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUv = pc.uvRect.xy + pos * pc.uvRect.zw;

    // The swapchain is in the display's natural orientation, so turn the picture into it
    // here rather than leave the compositor to. Clockwise on screen, with y pointing down.
    float turn = pc.params2.y * 1.5707963;
    float c = cos(turn);
    float s = sin(turn);
    vec2 clip = pos * 2.0 - 1.0;
    gl_Position = vec4(c * clip.x - s * clip.y, s * clip.x + c * clip.y, 0.0, 1.0);
}
