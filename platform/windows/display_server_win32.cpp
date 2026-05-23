/**
 * @file display_server_win32.cpp
 * @brief Win32 平台显示服务器实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_os
 */

#include "display_server_win32.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "io/logger.h"
#include "os/memory.h"
#include "window_win32.h"

namespace arhud
{
    namespace detail
    {
        template <typename T>
        void DestroyObject(T *&ptr)
        {
            ARHUD_DELETE(ptr);
            ptr = nullptr;
        }
    }

    IDisplayServer *IDisplayServer::singleton_ = nullptr;

    IDisplayServer *IDisplayServer::GetSingleton()
    {
        return singleton_;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Win32DisplayServer
    // ═══════════════════════════════════════════════════════════════════════

    Win32DisplayServer::Win32DisplayServer()
    {
        singleton_ = this;
    }

    Win32DisplayServer::~Win32DisplayServer()
    {
        if (is_initialized_)
        {
            Shutdown();
        }
        singleton_ = nullptr;
    }

    Error Win32DisplayServer::Initialize()
    {
        if (is_initialized_)
        {
            ARHUD_LOG_WARN("Win32DisplayServer: Already initialized");
            return Error::kFailed;
        }

        EnumerateScreens();

        if (screens_.Size() == 0)
        {
            ARHUD_LOG_ERROR("kFailed", "Win32DisplayServer: No screens found");
            return Error::kFailed;
        }

        is_initialized_ = true;
        ARHUD_LOG_INFO("Win32DisplayServer: Initialized with %u screens",
                       static_cast<uint32_t>(screens_.Size()));
        return Error::kOK;
    }

    void Win32DisplayServer::Shutdown()
    {
        if (!is_initialized_)
        {
            return;
        }

        DestroyAllViews();
        DestroyAllWindowsContexts();
        DestroyAllScreens();

        is_initialized_ = false;
    }

    // ─── 屏幕管理 ───────────────────────────────────────────────────────

    uint32_t Win32DisplayServer::GetScreenCount() const
    {
        return static_cast<uint32_t>(screens_.Size());
    }

    IScreen *Win32DisplayServer::GetScreen(ScreenID id) const
    {
        for (uint32_t i = 0; i < screens_.Size(); ++i)
        {
            if (screens_[i]->GetId() == id)
            {
                return screens_[i];
            }
        }
        return nullptr;
    }

    IScreen::ScreenID Win32DisplayServer::GetPrimaryScreen() const
    {
        for (uint32_t i = 0; i < screens_.Size(); ++i)
        {
            if (screens_[i]->IsPrimary())
            {
                return screens_[i]->GetId();
            }
        }
        return IScreen::kInvalidScreenId;
    }

    // ─── 窗口管理 ───────────────────────────────────────────────────────

    IDisplayServer::WindowID Win32DisplayServer::WindowCreate(const WindowDesc &desc)
    {
        if (!is_initialized_)
        {
            ARHUD_LOG_ERROR("kNotInitialized", "Win32DisplayServer: Not initialized");
            return IWindow::kInvalidWindowId;
        }

        Win32Window *window = ARHUD_NEW(Win32Window)();
        Error err = window->Initialize(desc);
        if (err != Error::kOK)
        {
            detail::DestroyObject(window);
            ARHUD_LOG_ERROR("kFailed", "Win32DisplayServer: Failed to create window");
            return IWindow::kInvalidWindowId;
        }

        WindowID id = next_window_id_++;
        window->SetWindowId(id);

        WindowContext ctx;
        ctx.window_id = id;
        ctx.screen_id = desc.screen_id;
        ctx.window = window;
        ctx.vsync_mode = desc.vsync_mode;
        windows_.Insert(id, ctx);

        ARHUD_LOG_INFO("Win32DisplayServer: Window %u created (%ux%u)",
                       id, desc.width, desc.height);
        return id;
    }

    void Win32DisplayServer::WindowDestroy(WindowID id)
    {
        WindowContext *ctx = windows_.GetPtr(id);
        if (!ctx)
        {
            ARHUD_LOG_WARN("Win32DisplayServer: Window %u not found", id);
            return;
        }

        for (uint32_t i = 0; i < ctx->views.Size(); ++i)
        {
            IView::ViewID vid = ctx->views[i]->GetId();
            views_.Erase(vid);
            Win32View *wv = static_cast<Win32View *>(ctx->views[i]);
            detail::DestroyObject(wv);
        }
        ctx->views.Clear();

        if (ctx->window)
        {
            Win32Window *ww = static_cast<Win32Window *>(ctx->window);
            ctx->window->Shutdown();
            detail::DestroyObject(ww);
            ctx->window = nullptr;
        }

        windows_.Erase(id);
        ARHUD_LOG_INFO("Win32DisplayServer: Window %u destroyed", id);
    }

    IWindow *Win32DisplayServer::GetWindow(WindowID id) const
    {
        const WindowContext *ctx = windows_.GetPtr(id);
        return ctx ? ctx->window : nullptr;
    }

    uint32_t Win32DisplayServer::GetWindowCount() const
    {
        return static_cast<uint32_t>(windows_.Size());
    }

    // ─── 视图管理 ───────────────────────────────────────────────────────

    IDisplayServer::ViewID Win32DisplayServer::ViewCreate(WindowID window, const ViewConfig &config)
    {
        WindowContext *ctx = windows_.GetPtr(window);
        if (!ctx)
        {
            ARHUD_LOG_ERROR("kInvalidParameter", "Win32DisplayServer: Window %u not found for view", window);
            return IView::kInvalidViewId;
        }

        ViewID id = next_view_id_++;
        ViewConfig actual_config = config;
        actual_config.id = static_cast<uint32_t>(id);

        Win32View *view = ARHUD_NEW(Win32View)(id, window, actual_config);
        views_.Insert(id, view);
        ctx->views.PushBack(view);

        ARHUD_LOG_INFO("Win32DisplayServer: View %u created on window %u (z=%.1f)",
                       id, window, config.z_order);
        return id;
    }

    void Win32DisplayServer::ViewDestroy(ViewID id)
    {
        Win32View **view_ptr = views_.GetPtr(id);
        if (!view_ptr || !(*view_ptr))
        {
            ARHUD_LOG_WARN("Win32DisplayServer: View %u not found", id);
            return;
        }

        Win32View *view = *view_ptr;
        IWindow::WindowID window_id = static_cast<IWindow::WindowID>(view->GetWindowId());
        WindowContext *ctx = windows_.GetPtr(window_id);
        if (ctx)
        {
            for (uint32_t i = 0; i < ctx->views.Size(); ++i)
            {
                if (ctx->views[i] == view)
                {
                    ctx->views.RemoveAt(i);
                    break;
                }
            }
        }

        views_.Erase(id);
        detail::DestroyObject(view);
    }

    IView *Win32DisplayServer::GetView(ViewID id) const
    {
        Win32View *const *ptr = views_.GetPtr(id);
        return ptr ? *ptr : nullptr;
    }

    // ─── 事件处理 ───────────────────────────────────────────────────────

    void Win32DisplayServer::ProcessEvents()
    {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    // ─── VSync 控制 ─────────────────────────────────────────────────────

    void Win32DisplayServer::SetVsyncMode(WindowID window, VSyncMode mode)
    {
        WindowContext *ctx = windows_.GetPtr(window);
        if (ctx)
        {
            ctx->vsync_mode = mode;
        }
    }

    VSyncMode Win32DisplayServer::GetVsyncMode(WindowID window) const
    {
        const WindowContext *ctx = windows_.GetPtr(window);
        return ctx ? ctx->vsync_mode : VSyncMode::kEnabled;
    }

    // ─── 内部方法 ───────────────────────────────────────────────────────

    void Win32DisplayServer::EnumerateScreens()
    {
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                            reinterpret_cast<LPARAM>(&screens_));
    }

    BOOL CALLBACK Win32DisplayServer::MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor,
                                                      LPRECT lprcMonitor, LPARAM dwData)
    {
        auto *screens = reinterpret_cast<LocalVector<Win32Screen *> *>(dwData);
        Win32Screen *screen = Win32Screen::Create(hMonitor);
        if (screen)
        {
            screens->PushBack(screen);
        }
        return TRUE;
    }

    void Win32DisplayServer::DestroyAllScreens()
    {
        for (uint32_t i = 0; i < screens_.Size(); ++i)
        {
            Win32Screen *s = screens_[i];
            detail::DestroyObject(s);
        }
        screens_.Clear();
    }

    void Win32DisplayServer::DestroyAllWindowsContexts()
    {
        for (auto it = windows_.Begin(); it != windows_.End(); ++it)
        {
            auto pair = *it;
            WindowContext &ctx = pair.value;
            if (ctx.window)
            {
                Win32Window *ww = static_cast<Win32Window *>(ctx.window);
                ctx.window->Shutdown();
                detail::DestroyObject(ww);
                ctx.window = nullptr;
            }
            ctx.views.Clear();
        }
        windows_.Clear();
    }

    void Win32DisplayServer::DestroyAllViews()
    {
        for (auto it = views_.Begin(); it != views_.End(); ++it)
        {
            auto pair = *it;
            Win32View *v = pair.value;
            detail::DestroyObject(v);
        }
        views_.Clear();
    }

}

#endif
