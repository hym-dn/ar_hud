/**
 * @file view_win32.h
 * @brief Win32 平台视图实现声明
 *
 * @par 概述
 *   视图（View）是 2D HUD 渲染层的基本单位。
 *   每个窗口可以包含多个视图，视图按 Z 顺序叠加显示。
 *
 * @par 继承层次
 *   IView（抽象接口）
 *     └── Win32View（具体实现）
 *
 * @par Z-Order 层级
 *   视图按 Z 值排序，典型层级：
 *   - Z = 0.0f：背景层（地图、视频等）
 *   - Z = 0.5f：中间层（信息面板）
 *   - Z = 1.0f：前景层（交互元素）
 *
 * @par 视图类型
 *   ViewType 枚举定义视图类型：
 *   - kBackground：背景层
 *   - kOverlay：覆盖层
 *   - kForeground：前景层
 *
 * @par 线程安全
 *   非线程安全。所有方法应在渲染线程调用。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include "typedefs.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "view.h"
#include "window.h"

namespace arhud
{

    class Win32View : public IView
    {
    public:
        Win32View(IView::ViewID id, IWindow::WindowID window_id, const ViewConfig &config);

        virtual ~Win32View() override = default;

        virtual ViewID GetId() const override;

        virtual ViewType GetType() const override;

        virtual int32_t GetX() const override;

        virtual int32_t GetY() const override;

        virtual int32_t GetWidth() const override;

        virtual int32_t GetHeight() const override;

        virtual void SetViewport(int32_t x, int32_t y, int32_t w, int32_t h) override;

        virtual float GetZOrder() const override;

        virtual void SetZOrder(float order) override;

        virtual uint32_t GetWindowId() const override;

    private:
        IView::ViewID id_;
        IWindow::WindowID window_id_;
        ViewConfig config_;
    };

}

#endif
