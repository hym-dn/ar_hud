/**
 * @file hud_spatial.h
 * @brief Inline GLSL source for the 3D Spatial shader
 *
 * Provides vertex + fragment shader source code for the SpatialRenderer.
 * The shader implements 3D vertex transformation with directional lighting.
 *
 * @par Uniform layout (binding 分配对齐 CanvasRenderer):
 *   | Binding | Type              | Stage    | GLSL Name    | Description |
 *   |---------|-------------------|----------|--------------|--------------------|
 *   | 0       | UniformBuffer     | Vertex   | CameraBlock  | Projection + View
 * | | 1       | UniformBuffer     | Fragment | MaterialBlock| Material params |
 *   | 2       | UniformBuffer     | Vertex   | ModelBlock   | Model matrix | |
 * 3       | UniformBuffer     | Fragment | LightBlock   | Directional light  |
 *   | —       | SamplerWithTexture| Fragment | u_texture    | Material texture
 * |
 *
 * @par Binding 说明（与 hud_canvas.h 一致）：
 *   - UBO 的 binding 对应 GLSL 中的 layout(binding=X)，通过 glBindBufferBase
 * 绑定
 *   - 同一着色器阶段的 UBO 必须使用不同的 binding（GL UBO binding point
 * 全局唯一）
 *   - Sampler 没有 layout(binding=)，通过 glGetUniformLocation("u_texture") +
 *     glUniform1i(location, texture_unit) 绑定
 *   - UBO 和 Sampler 在 GL 中使用不同的绑定命名空间，不会冲突
 *
 * @par Vertex format:
 *   - Location 0: vec3 a_position (offset 0,  stride 32)
 *   - Location 1: vec3 a_normal   (offset 12, stride 32)
 *   - Location 2: vec2 a_uv       (offset 24, stride 32)
 *
 * @par Lighting model:
 *   Simple Blinn-Phong with one directional light:
 *     ambient  = u_ambient * base_color
 *     diffuse  = max(dot(N, L), 0) * u_light_color * u_light_energy *
 * base_color result   = ambient + diffuse No specular, no shadows (Phase 4
 * simplification).
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-30
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_SPATIAL_H_
#define ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_SPATIAL_H_

namespace arhud {

inline const char* kHudSpatialVert = R"(
        #version 330 core
        layout(location = 0) in vec3 a_position;
        layout(location = 1) in vec3 a_normal;
        layout(location = 2) in vec2 a_uv;
        uniform CameraBlock {
            mat4 projection;
            mat4 view;
        };
        uniform ModelBlock {
            mat4 model;
        };
        out vec3 v_world_position;
        out vec3 v_normal;
        out vec2 v_uv;
        void main() {
            vec4 world_pos = model * vec4(a_position, 1.0);
            v_world_position = world_pos.xyz;
        #ifdef VARIANT_DEPTH_PASS
            // Depth pre-pass: skip normal computation (not needed for depth-only)
            v_normal = vec3(0.0);
            v_uv = vec2(0.0);
        #else
            v_normal = mat3(transpose(inverse(model))) * a_normal;
            v_uv = a_uv;
        #endif
            gl_Position = projection * view * world_pos;
        }
        )";

inline const char* kHudSpatialFrag = R"(
        #version 330 core
        in vec3 v_world_position;
        in vec3 v_normal;
        in vec2 v_uv;
        uniform sampler2D u_texture;
        uniform MaterialBlock {
            float opacity;
        };
        uniform LightBlock {
            vec3 light_direction;
            float _pad0;
            vec3 light_color;
            float light_energy;
            float ambient;
            float _pad1;
            float _pad2;
            float _pad3;
        };
        out vec4 frag_color;
        void main() {
        #ifdef VARIANT_DEPTH_PASS
            // Depth pre-pass: only write depth, discard fragment output
            // Color write is disabled via pipeline blend state (write_r/g/b/a = false)
            // but we still need to output a dummy value for GLSL correctness
            frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        #else
            vec3 N = normalize(v_normal);
            vec3 L = normalize(-light_direction);
            float NdotL = max(dot(N, L), 0.0);
            vec4 base_color = texture(u_texture, v_uv);
            vec3 ambient_color = ambient * base_color.rgb;
            vec3 diffuse_color = NdotL * light_color * light_energy * base_color.rgb;
            frag_color = vec4(ambient_color + diffuse_color, base_color.a * opacity);
        #endif
        }
        )";

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_SPATIAL_H_
