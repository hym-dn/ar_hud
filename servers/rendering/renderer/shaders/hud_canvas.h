/**
 * @file hud_canvas.h
 * @brief Inline GLSL source for the 2D Canvas shader
 *
 * Provides vertex + fragment shader source code for the CanvasRenderer.
 * The shader uses UBO binding point for projection, a sampler for the
 * material texture, and a per-vertex color attribute for opacity/modulate.
 *
 * @par Uniform layout:
 *   | Set | Binding | Type              | Stage    | GLSL Name       |
 * Description        |
 *   |-----|---------|-------------------|----------|-----------------|--------------------|
 *   | 0   | 0       | UniformBuffer     | Vertex   | ProjectionBlock |
 * Projection matrix  | | 1   | 0       | SamplerWithTexture| Fragment |
 * u_texture       | Material texture   |
 *
 * @par Vertex format (batched):
 *   - Location 0: vec2 a_position (offset 0, stride 32)
 *   - Location 1: vec2 a_uv       (offset 8, stride 32)
 *   - Location 2: vec4 a_color    (offset 16, stride 32)
 *
 * @par Batching design:
 *   Model transforms are baked into vertex positions on CPU, so no Model UBO
 *   is needed. Per-item opacity is passed via the vertex color alpha channel,
 *   eliminating the per-item Material UBO update. This allows same-material
 *   items to be merged into a single draw call.
 *
 * @author yameng.he
 * @version 2.0
 * @date 2026-06-10
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_CANVAS_H_
#define ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_CANVAS_H_

namespace arhud {
static const char* kHudCanvasVert = R"(
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

inline const char* kHudCanvasFrag = R"(
        #version 330 core
        in vec2 v_uv;
        in vec4 v_color;
        uniform sampler2D u_texture;
        out vec4 frag_color;
        void main() {
            frag_color = texture(u_texture, v_uv) * v_color;
        }
        )";
}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_CANVAS_H_
