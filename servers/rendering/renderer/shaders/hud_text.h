/**
 * @file hud_text.h
 * @brief Inline GLSL source for the SDF text shader
 *
 * Provides vertex + fragment shader source code for SDF text rendering.
 * The vertex shader reuses the Canvas vertex format (position + UV + color),
 * and the fragment shader performs SDF distance-based alpha calculation
 * with screen-space smoothing via fwidth().
 *
 * @par Uniform layout:
 *   | Set | Binding | Type              | Stage    | GLSL Name       |
 * Description              |
 *   |-----|---------|-------------------|----------|-----------------|--------------------------|
 *   | 0   | 0       | UniformBuffer     | Vertex   | ProjectionBlock |
 * Projection matrix        | | 1   | 0       | SamplerWithTexture| Fragment |
 * u_font_sdf      | SDF font atlas           | | 2   | 0       | UniformBuffer
 * | Fragment | SmoothingBlock  | SDF smoothing parameter  |
 *
 * @par Vertex format (same as Canvas):
 *   - Location 0: vec2 a_position (offset 0, stride 32)
 *   - Location 1: vec2 a_uv       (offset 8, stride 32)
 *   - Location 2: vec4 a_color    (offset 16, stride 32)
 *
 * @par SDF rendering:
 *   The fragment shader reads the SDF distance value from the R channel of
 *   the font atlas texture, then applies smoothstep with screen-space
 *   derivative (fwidth) for anti-aliased edge rendering:
 *     alpha = smoothstep(0.5 - smooth_width, 0.5 + smooth_width, distance)
 *   where smooth_width = fwidth(distance) * u_smoothing
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-06-12
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_TEXT_H_
#define ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_TEXT_H_

namespace arhud {

/**
 * @brief SDF 文字渲染顶点着色器
 *
 * 与 Canvas 顶点着色器相同，变换顶点位置并传递 UV 和颜色。
 */
static const char* kHudTextVert = R"(
        #version 330 core
        layout(location = 0) in vec2 a_position;
        layout(location = 1) in vec2 a_uv;
        layout(location = 2) in vec4 a_color;
        uniform ProjectionBlock {
            mat4 projection;
        };
        out vec2 v_uv;
        out vec4 v_color;
        void main() {
            gl_Position = projection * vec4(a_position, 0.0, 1.0);
            v_uv = a_uv;
            v_color = a_color;
        }
        )";

/**
 * @brief SDF 文字渲染片段着色器
 *
 * 从 SDF 字体图集采样距离值，使用 smoothstep + fwidth
 * 实现屏幕空间抗锯齿的文字边缘渲染。
 */
inline const char* kHudTextFrag = R"(
        #version 330 core
        in vec2 v_uv;
        in vec4 v_color;

        uniform sampler2D u_font_sdf;

        uniform SmoothingBlock {
            float u_smoothing;
        };

        out vec4 frag_color;

        void main() {
            float distance = texture(u_font_sdf, v_uv).r;
            float smooth_width = fwidth(distance) * u_smoothing;
            float alpha = smoothstep(0.5 - smooth_width, 0.5 + smooth_width, distance);

            frag_color = vec4(v_color.rgb, v_color.a * alpha);
        }
        )";

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_TEXT_H_
