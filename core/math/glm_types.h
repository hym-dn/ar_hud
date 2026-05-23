/**
 * @file glm_types.h
 * @brief GLM 数学库集成 — 类型桥接与高级数学函数
 *
 * 本文件将 glm 库集成到 arhud 引擎中，提供：
 *   1. arhud 数学类型与 glm 类型的互转（Vec2 ↔ glm::vec2 等）
 *   2. glm 高级数学函数的 arhud 风格包装（投影、变换等）
 *   3. 直接 using glm 的 SIMD 优化类型别名
 *
 * @par 设计原则
 *   - 现有 arhud 类型（Vec2/Mat4 等）保持不变，不依赖 glm
 *   - 渲染层需要 glm 时通过本文件桥接
 *   - glm 的 SIMD 优化在批量矩阵运算中提供性能优势
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-23
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_math
 */

#pragma once

#include "math_funcs.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // 类型别名 — 直接暴露 glm 类型供渲染层使用
    // ═══════════════════════════════════════════════════════════════════════

    using GLMVec2 = glm::vec2;
    using GLMVec3 = glm::vec3;
    using GLMVec4 = glm::vec4;
    using GLMMat3 = glm::mat3;
    using GLMMat4 = glm::mat4;
    using GLMIVec2 = glm::ivec2;
    using GLMIVec3 = glm::ivec3;
    using GLMIVec4 = glm::ivec4;

    // ═══════════════════════════════════════════════════════════════════════
    // 类型转换 — arhud ↔ glm
    // ═══════════════════════════════════════════════════════════════════════

    ARHUD_ALWAYS_INLINE GLMVec2 ToGLM(const Vec2 &p_v) { return GLMVec2(p_v.x, p_v.y); }
    ARHUD_ALWAYS_INLINE GLMVec3 ToGLM(const Vec3 &p_v) { return GLMVec3(p_v.x, p_v.y, p_v.z); }
    ARHUD_ALWAYS_INLINE GLMVec4 ToGLM(const Vec4 &p_v) { return GLMVec4(p_v.x, p_v.y, p_v.z, p_v.w); }
    ARHUD_ALWAYS_INLINE GLMIVec2 ToGLM(const Vec2i &p_v) { return GLMIVec2(p_v.x, p_v.y); }

    ARHUD_ALWAYS_INLINE Vec2 FromGLM(const GLMVec2 &p_v) { return Vec2(p_v.x, p_v.y); }
    ARHUD_ALWAYS_INLINE Vec3 FromGLM(const GLMVec3 &p_v) { return Vec3(p_v.x, p_v.y, p_v.z); }
    ARHUD_ALWAYS_INLINE Vec4 FromGLM(const GLMVec4 &p_v) { return Vec4(p_v.x, p_v.y, p_v.z, p_v.w); }
    ARHUD_ALWAYS_INLINE Vec2i FromGLM(const GLMIVec2 &p_v) { return Vec2i(p_v.x, p_v.y); }

    ARHUD_ALWAYS_INLINE GLMMat4 ToGLM(const Mat4 &p_m)
    {
        return glm::make_mat4(p_m.elements);
    }

    ARHUD_ALWAYS_INLINE Mat4 FromGLM(const GLMMat4 &p_m)
    {
        Mat4 result;
        memcpy(result.elements, glm::value_ptr(p_m), sizeof(float32) * 16);
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 高级数学函数 — 基于 glm 的投影和变换
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 创建正交投影矩阵（glm 实现）
     *
     * @param[in] p_left   左侧裁剪平面
     * @param[in] p_right  右侧裁剪平面
     * @param[in] p_bottom 底部裁剪平面
     * @param[in] p_top    顶部裁剪平面
     * @param[in] p_near   近裁剪平面
     * @param[in] p_far    远裁剪平面
     * @return 正交投影矩阵
     */
    ARHUD_ALWAYS_INLINE Mat4 Orthographic(float32 p_left, float32 p_right,
                                          float32 p_bottom, float32 p_top,
                                          float32 p_near, float32 p_far)
    {
        return FromGLM(glm::ortho(p_left, p_right, p_bottom, p_top, p_near, p_far));
    }

    /**
     * @brief 创建透视投影矩阵（glm 实现）
     *
     * @param[in] p_fov_y  垂直视野角度（弧度）
     * @param[in] p_aspect 宽高比
     * @param[in] p_near   近裁剪平面
     * @param[in] p_far    远裁剪平面
     * @return 透视投影矩阵
     */
    ARHUD_ALWAYS_INLINE Mat4 Perspective(float32 p_fov_y, float32 p_aspect,
                                         float32 p_near, float32 p_far)
    {
        return FromGLM(glm::perspective(p_fov_y, p_aspect, p_near, p_far));
    }

    /**
     * @brief 创建平移矩阵（glm 实现）
     *
     * @param[in] p_offset 平移向量
     * @return 平移矩阵
     */
    ARHUD_ALWAYS_INLINE Mat4 Translate(const Vec3 &p_offset)
    {
        return FromGLM(glm::translate(GLMMat4(1.0f), ToGLM(p_offset)));
    }

    /**
     * @brief 创建缩放矩阵（glm 实现）
     *
     * @param[in] p_scale 缩放向量
     * @return 缩放矩阵
     */
    ARHUD_ALWAYS_INLINE Mat4 Scale(const Vec3 &p_scale)
    {
        return FromGLM(glm::scale(GLMMat4(1.0f), ToGLM(p_scale)));
    }

    /**
     * @brief 创建绕轴旋转矩阵（glm 实现）
     *
     * @param[in] p_axis   旋转轴（需归一化）
     * @param[in] p_angle  旋转角度（弧度）
     * @return 旋转矩阵
     */
    ARHUD_ALWAYS_INLINE Mat4 Rotate(const Vec3 &p_axis, float32 p_angle)
    {
        return FromGLM(glm::rotate(GLMMat4(1.0f), p_angle, ToGLM(p_axis)));
    }

    /**
     * @brief 矩阵求逆（glm 实现，SIMD 优化）
     *
     * @param[in] p_m 输入矩阵
     * @return 逆矩阵
     */
    ARHUD_ALWAYS_INLINE Mat4 Inverse(const Mat4 &p_m)
    {
        return FromGLM(glm::inverse(ToGLM(p_m)));
    }

    /**
     * @brief 矩阵转置（glm 实现，SIMD 优化）
     *
     * @param[in] p_m 输入矩阵
     * @return 转置矩阵
     */
    ARHUD_ALWAYS_INLINE Mat4 Transpose(const Mat4 &p_m)
    {
        return FromGLM(glm::transpose(ToGLM(p_m)));
    }

    /**
     * @brief 获取矩阵数据指针（用于 glUniformMatrix4fv）
     *
     * @param[in] p_m 输入矩阵
     * @return 指向列主序 float 数组的指针
     */
    ARHUD_ALWAYS_INLINE const float32 *ValuePtr(const Mat4 &p_m)
    {
        return p_m.elements;
    }

} // namespace arhud
