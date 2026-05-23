/**
 * @file view_win32.cpp
 * @brief Win32 平台视图实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#include "view_win32.h"

#ifdef ARHUD_PLATFORM_WINDOWS

namespace arhud
{

    Win32View::Win32View(IView::ViewID id, IWindow::WindowID window_id, const ViewConfig &config)
        : id_(id),
          window_id_(window_id),
          config_(config)
    {
    }

    IView::ViewID Win32View::GetId() const
    {
        return id_;
    }

    ViewType Win32View::GetType() const
    {
        return config_.type;
    }

    int32_t Win32View::GetX() const
    {
        return config_.x;
    }

    int32_t Win32View::GetY() const
    {
        return config_.y;
    }

    int32_t Win32View::GetWidth() const
    {
        return config_.width;
    }

    int32_t Win32View::GetHeight() const
    {
        return config_.height;
    }

    void Win32View::SetViewport(int32_t x, int32_t y, int32_t w, int32_t h)
    {
        config_.x = x;
        config_.y = y;
        config_.width = w;
        config_.height = h;
    }

    float Win32View::GetZOrder() const
    {
        return config_.z_order;
    }

    void Win32View::SetZOrder(float order)
    {
        config_.z_order = order;
    }

    uint32_t Win32View::GetWindowId() const
    {
        return static_cast<uint32_t>(window_id_);
    }

}

#endif
