/**
 * @file hud_composite.h
 * @brief Inline GLSL source for the post-processing composite shader
 *
 * Provides vertex + fragment shader source code for the post-processing
 * pipeline. The composite shader reads the scene color from an offscreen
 * texture and applies barrel distortion correction, gamma correction,
 * and alpha premultiplication.
 *
 * @par Architecture (reference: Godot EffectsRD::tonemapper):
 *   Godot renders the 3D scene to an offscreen FBO, then applies
 *   post-processing (tonemapping, FXAA, etc.) via a full-screen quad
 *   blit to the default framebuffer. ARHud follows the same pattern
 *   but with HUD-specific effects:
 *     1. Barrel distortion correction (AR HUD projector curvature)
 *     2. Gamma correction (Linear → sRGB)
 *     3. Alpha premultiplication (for compositing with real world)
 *
 * @par Uniform layout:
 *   | Set | Binding | Type              | Stage    | GLSL Name         |
 * Description         |
 *   |-----|---------|-------------------|----------|-------------------|---------------------|
 *   | 0   | 0       | SamplerWithTexture| Fragment | u_scene_color     |
 * Offscreen scene     | | 0   | 1       | UniformBuffer     | Fragment |
 * CompositeParams   | Post-process params |
 *
 * @par Vertex format (full-screen quad):
 *   - Location 0: vec2 a_position (NDC coordinates: -1 to 1)
 *   - Location 1: vec2 a_uv       (UV coordinates: 0 to 1)
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-06-15
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_COMPOSITE_H_
#define ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_COMPOSITE_H_

namespace arhud {

static const char* kHudCompositeVert = R"(
        #version 330 core
        layout(location = 0) in vec2 a_position;
        layout(location = 1) in vec2 a_uv;
        out vec2 v_uv;
        void main() {
            gl_Position = vec4(a_position, 0.0, 1.0);
            v_uv = a_uv;
        }
        )";

inline const char* kHudCompositeFrag = R"(
        #version 330 core
        in vec2 v_uv;
        uniform sampler2D u_scene_color;
        uniform CompositeParams {
            float u_gamma;                // Gamma correction value (default 2.2)
            float u_enable_distortion;    // Non-zero to enable barrel distortion
            float u_distortion_k1;        // Barrel distortion coefficient k1
            float u_distortion_k2;        // Barrel distortion coefficient k2
            float u_enable_premul_alpha;  // Non-zero to enable alpha premultiplication
        };
        out vec4 frag_color;
        void main() {
            vec2 uv = v_uv;

            // 1. Barrel distortion correction (compensate for AR HUD projector curvature)
            //    Reference: OpenCV fisheye model
            //    r_distorted = r * (1 + k1*r^2 + k2*r^4)
            //    We invert: compute undistorted UV from distorted UV
            if (u_enable_distortion > 0.5) {
                vec2 centered = uv * 2.0 - 1.0;
                float r2 = dot(centered, centered);
                float distortion = 1.0 + u_distortion_k1 * r2 + u_distortion_k2 * r2 * r2;
                centered *= distortion;
                uv = centered * 0.5 + 0.5;

                // Discard fragments outside [0,1] range
                if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
                    frag_color = vec4(0.0);
                    return;
                }
            }

            // 2. Sample scene color from offscreen texture
            vec4 color = texture(u_scene_color, uv);

            // 3. Gamma correction (Linear → sRGB)
            if (u_gamma > 0.0) {
                color.rgb = pow(color.rgb, vec3(1.0 / u_gamma));
            }

            // 4. Alpha premultiplication (for compositing with real-world background)
            if (u_enable_premul_alpha > 0.5) {
                color.rgb *= color.a;
            }

            frag_color = color;
        }
        )";

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDERER_SHADERS_HUD_COMPOSITE_H_
