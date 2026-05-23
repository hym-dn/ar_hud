/**
 * @file view.h
 * @brief 平台无关视图抽象接口
 *
 * 定义 HUD 分层渲染视图，一个窗口可包含多个视图（Z-Order 排序）。
 * 典型场景：仪表盘层、导航层、警告层分别对应不同 View。
 *
 * 设计参考：
 *   - arch_skill.md ARHudView 设计
 *   - Godot SubViewport 概念
 *
 * 视图类型：
 *   - kOverlay2D: 2D HUD 叠加层（仪表盘、导航箭头）
 *   - kScene3D: 3D 场景视图（ADAS 可视化）
 *   - kStereoLeft/kStereoRight: AR 立体渲染左右眼
 *   - kCustom: 自定义视图
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_os
 */

#pragma once

#include "typedefs.h"

namespace arhud
{

    /** @brief 视图类型枚举 */
    enum class ViewType
    {
        kOverlay2D,   /**< 2D HUD 叠加层 */
        kScene3D,     /**< 3D 场景视图 */
        kStereoLeft,  /**< AR 立体渲染左眼 */
        kStereoRight, /**< AR 立体渲染右眼 */
        kCustom       /**< 自定义视图 */
    };

    /** @brief 视图创建配置参数 */
    struct ViewConfig
    {
        uint32_t id = 0;                      /**< 视图唯一标识符 */
        ViewType type = ViewType::kOverlay2D; /**< 视图类型 */
        int32_t x = 0;                        /**< 视口 X 坐标 */
        int32_t y = 0;                        /**< 视口 Y 坐标 */
        int32_t width = 0;                    /**< 视口宽度 */
        int32_t height = 0;                   /**< 视口高度 */
        float z_order = 0.0f;                 /**< Z-Order 排序值，越大越靠上 */
        bool clear_color = true;              /**< 每帧清除颜色缓冲 */
        bool clear_depth = true;              /**< 每帧清除深度缓冲 */
        float clear_r = 0.0f;                 /**< 清除颜色 R 分量 [0,1] */
        float clear_g = 0.0f;                 /**< 清除颜色 G 分量 [0,1] */
        float clear_b = 0.0f;                 /**< 清除颜色 B 分量 [0,1] */
        float clear_a = 0.0f;                 /**< 清除颜色 A 分量 [0,1] */
    };

    /** @brief 平台无关视图抽象接口
     *
     * 每个视图对应一个独立渲染视口，由 Window 管理。
     * 多视图按 Z-Order 排序叠加渲染。 */
    class IView
    {
    public:
        /** @brief 视图 ID 类型 */
        using ViewID = uint32_t;
        /** @brief 无效视图 ID 常量 */
        static constexpr ViewID kInvalidViewId = UINT32_MAX;

        virtual ~IView() = default;

        /** @brief 获取视图唯一标识符 */
        virtual ViewID GetId() const = 0;
        /** @brief 获取视图类型 */
        virtual ViewType GetType() const = 0;
        /** @brief 获取视口 X 坐标 */
        virtual int32_t GetX() const = 0;
        /** @brief 获取视口 Y 坐标 */
        virtual int32_t GetY() const = 0;
        /** @brief 获取视口宽度 */
        virtual int32_t GetWidth() const = 0;
        /** @brief 获取视口高度 */
        virtual int32_t GetHeight() const = 0;
        /** @brief 设置视口位置和尺寸
         *  @param[in] x  X 坐标
         *  @param[in] y  Y 坐标
         *  @param[in] w  宽度
         *  @param[in] h  高度 */
        virtual void SetViewport(int32_t x, int32_t y, int32_t w, int32_t h) = 0;
        /** @brief 获取 Z-Order 排序值 */
        virtual float GetZOrder() const = 0;
        /** @brief 设置 Z-Order 排序值
         *  @param[in] order  Z-Order 值 */
        virtual void SetZOrder(float order) = 0;
        /** @brief 获取所属窗口 ID */
        virtual uint32_t GetWindowId() const = 0;
    };

}