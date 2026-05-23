/**
 * @file window_win32.cpp
 * @brief Win32 平台窗口实现
 *
 * 使用原生 Win32 API 实现 IWindow 接口的完整功能。
 *
 * @par 初始化流程：
 *   1. 获取 HINSTANCE
 *   2. RegisterClassEx 注册窗口类
 *   3. CreateWindowEx 创建原生窗口
 *   4. ShowWindow 显示窗口
 *
 * @par 关闭流程（逆序）：
 *   1. DestroyWindow 销毁窗口
 *   2. UnregisterClass 注销窗口类
 *
 * @par WndProc 消息路由：
 *   WM_NCCREATE  → 存储 this 指针到 GWLP_USERDATA
 *   WM_SIZE      → 触发 OnResize 回调
 *   WM_CLOSE     → 设置 should_close_ 标志，触发 OnClose 回调
 *   WM_DESTROY   → PostQuitMessage
 *   WM_SETFOCUS / WM_KILLFOCUS → 触发 OnFocus 回调
 *   WM_KEYDOWN / WM_KEYUP      → 触发 OnKey 回调
 *   WM_CHAR      → 触发 OnChar 回调
 *   WM_LBUTTON... / WM_RBUTTON... / WM_MBUTTON... → 触发 OnMouse 回调
 *   WM_MOUSEMOVE → 触发 OnMouseMove 回调
 *   WM_MOUSEWHEEL → 触发 OnScroll 回调
 *   WM_SYSKEYDOWN → 处理 Alt+F4
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-06
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#include "window_win32.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "io/logger.h"

namespace arhud
{
    // ═══════════════════════════════════════════════════════════════════════
    // 构造 / 析构
    // ═══════════════════════════════════════════════════════════════════════

    Win32Window::Win32Window()
        : hwnd_(nullptr),
          hinstance_(nullptr),
          is_initialized_(false),
          should_close_(false),
          is_external_(false),
          is_class_registered_(false),
          is_minimized_(false),
          is_maximized_(false),
          width_(0),
          height_(0),
          pos_x_(0),
          pos_y_(0),
          screen_id_(UINT32_MAX),
          dpi_scale_(1.0f),
          window_id_(kInvalidWindowId),
          current_mode_(WindowMode::kWindowed),
          desc_(),
          callbacks_()
    {
        hinstance_ = GetModuleHandleA(nullptr);
        if (!hinstance_)
        {
            ARHUD_LOG_ERROR("kFailed", "Win32Window: Failed to get HINSTANCE");
        }
    }

    Win32Window::~Win32Window()
    {
        Shutdown();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // IWindow 接口实现
    // ═══════════════════════════════════════════════════════════════════════

    Error Win32Window::Initialize(const WindowDesc &desc)
    {
        if (is_initialized_)
        {
            ARHUD_LOG_WARN("Win32Window: Already initialized");
            return Error::kFailed;
        }

        if (!hinstance_)
        {
            ARHUD_LOG_ERROR("kFailed", "Win32Window: HINSTANCE is null");
            return Error::kFailed;
        }

        constexpr uint32_t kMinWidth = 100;
        constexpr uint32_t kMinHeight = 100;
        if (desc.width < kMinWidth || desc.height < kMinHeight)
        {
            ARHUD_LOG_ERROR("kInvalidParameter",
                            "Win32Window: Dimensions too small: %ux%u, minimum: %ux%u",
                            desc.width, desc.height, kMinWidth, kMinHeight);
            return Error::kInvalidParameter;
        }

        desc_ = desc;
        current_mode_ = desc.mode;
        screen_id_ = desc.screen_id;

        ARHUD_LOG_INFO("Win32Window: Initializing %ux%u, mode=%d, external=%d",
                       desc.width, desc.height,
                       static_cast<int32_t>(desc.mode), desc.external_window);

        if (desc.external_window)
        {
            if (!desc.native_window)
            {
                ARHUD_LOG_ERROR("kInvalidParameter",
                                "Win32Window: External window specified but no native_window handle");
                return Error::kInvalidParameter;
            }

            hwnd_ = static_cast<HWND>(desc.native_window);
            is_external_ = true;

            RECT client_rect;
            if (GetClientRect(hwnd_, &client_rect))
            {
                width_ = static_cast<uint32_t>(client_rect.right - client_rect.left);
                height_ = static_cast<uint32_t>(client_rect.bottom - client_rect.top);
            }
            else
            {
                width_ = desc.width;
                height_ = desc.height;
            }

            ARHUD_LOG_INFO("Win32Window: Using external window handle: 0x%p",
                           reinterpret_cast<void *>(hwnd_));
        }
        else
        {
            if (!RegisterWindowClass())
            {
                ARHUD_LOG_ERROR("kFailed", "Win32Window: Failed to register window class");
                return Error::kFailed;
            }

            if (!CreateNativeWindow(desc.title, desc.width, desc.height))
            {
                ARHUD_LOG_ERROR("kFailed", "Win32Window: Failed to create native window");
                UnregisterWindowClass();
                return Error::kFailed;
            }
        }

        if (hwnd_)
        {
            HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            if (monitor)
            {
                HMODULE shcore = LoadLibraryA("shcore.dll");
                if (shcore)
                {
                    using GetDpiForMonitorFunc = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
                    auto func = reinterpret_cast<GetDpiForMonitorFunc>(
                        GetProcAddress(shcore, "GetDpiForMonitor"));
                    if (func)
                    {
                        UINT dpi_x = 0;
                        UINT dpi_y = 0;
                        HRESULT hr = func(monitor, 0, &dpi_x, &dpi_y);
                        if (SUCCEEDED(hr) && dpi_y > 0)
                        {
                            dpi_scale_ = static_cast<float>(dpi_y) / 96.0f;
                        }
                    }
                    FreeLibrary(shcore);
                }
            }
        }

        is_initialized_ = true;

        ARHUD_LOG_INFO("Win32Window: Initialized successfully (%ux%u, dpi_scale=%.2f)",
                       width_, height_, dpi_scale_);
        return Error::kOK;
    }

    void Win32Window::Shutdown()
    {
        if (!is_initialized_)
        {
            return;
        }

        ARHUD_LOG_INFO("Win32Window: Shutting down");

        if (hwnd_ && !is_external_)
        {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        else if (is_external_)
        {
            hwnd_ = nullptr;
        }

        if (!is_external_ && is_class_registered_)
        {
            UnregisterWindowClass();
        }

        is_initialized_ = false;
        should_close_ = false;
        is_external_ = false;
        is_class_registered_ = false;
        is_minimized_ = false;
        is_maximized_ = false;
        width_ = 0;
        height_ = 0;
        pos_x_ = 0;
        pos_y_ = 0;
        dpi_scale_ = 1.0f;
    }

    void Win32Window::PollEvents()
    {
        if (!is_initialized_)
        {
            return;
        }

        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    bool Win32Window::ShouldClose() const
    {
        return should_close_;
    }

    void *Win32Window::GetNativeHandle() const
    {
        return hwnd_;
    }

    uint32_t Win32Window::GetWidth() const
    {
        return width_;
    }

    uint32_t Win32Window::GetHeight() const
    {
        return height_;
    }

    bool Win32Window::IsVisible() const
    {
        if (!hwnd_)
        {
            return false;
        }
        return IsWindowVisible(hwnd_) != 0;
    }

    bool Win32Window::IsMinimized() const
    {
        return is_minimized_;
    }

    bool Win32Window::IsMaximized() const
    {
        return is_maximized_;
    }

    void Win32Window::GetPosition(int32_t &x, int32_t &y) const
    {
        x = pos_x_;
        y = pos_y_;
    }

    uint32_t Win32Window::GetScreenId() const
    {
        return screen_id_;
    }

    float Win32Window::GetDpiScale() const
    {
        return dpi_scale_;
    }

    void Win32Window::Resize(uint32_t width, uint32_t height)
    {
        if (!hwnd_)
        {
            ARHUD_LOG_WARN("Win32Window: Cannot resize, window handle is null");
            return;
        }

        if (width < 1 || height < 1)
        {
            return;
        }

        RECT window_rect;
        window_rect.left = 0;
        window_rect.top = 0;
        window_rect.right = static_cast<LONG>(width);
        window_rect.bottom = static_cast<LONG>(height);

        DWORD style = static_cast<DWORD>(GetWindowLongPtrA(hwnd_, GWL_STYLE));
        AdjustWindowRect(&window_rect, style, FALSE);

        SetWindowPos(hwnd_, nullptr, 0, 0,
                     window_rect.right - window_rect.left,
                     window_rect.bottom - window_rect.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void Win32Window::SetVisible(bool visible)
    {
        if (!hwnd_)
        {
            return;
            SetFocus(hwnd_);
        }
        else
        {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }

    void Win32Window::SetTitle(const char *title)
    {
        if (!hwnd_ || !title)
        {
            return;
        }
        SetWindowTextA(hwnd_, title);
    }

    void Win32Window::SetPosition(int32_t x, int32_t y)
    {
        if (!hwnd_)
        {
            return;
        }
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void Win32Window::SetFullscreenMode(WindowMode mode)
    {
        if (!hwnd_ || mode == current_mode_)
        {
            return;
        }

        if (current_mode_ == WindowMode::kWindowed || current_mode_ == WindowMode::kBorderless)
        {
            if (mode == WindowMode::kFullscreen || mode == WindowMode::kExclusiveFullscreen)
            {
                DEVMODEA dm = {};
                dm.dmSize = sizeof(DEVMODEA);
                dm.dmPelsWidth = width_;
                dm.dmPelsHeight = height_;
                dm.dmBitsPerPel = 32;
                dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;

                LONG result = ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
                if (result == DISP_CHANGE_SUCCESSFUL)
                {
                    SetWindowLongPtrA(hwnd_, GWL_STYLE,
                                      WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
                    SetWindowPos(hwnd_, HWND_TOP, 0, 0,
                                 static_cast<int32_t>(width_),
                                 static_cast<int32_t>(height_),
                                 SWP_NOZORDER | SWP_FRAMECHANGED);
                    current_mode_ = mode;
                }
                else
                {
                    ARHUD_LOG_WARN("Win32Window: Failed to switch to fullscreen mode");
                }
            }
        }
        else
        {
            if (mode == WindowMode::kWindowed)
            {
                ChangeDisplaySettingsA(nullptr, 0);
                DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
                if (!desc_.resizable)
                {
                    style &= ~WS_THICKFRAME;
                    style &= ~WS_MAXIMIZEBOX;
                }
                SetWindowLongPtrA(hwnd_, GWL_STYLE, static_cast<LONG_PTR>(style));
                SetWindowPos(hwnd_, nullptr, CW_USEDEFAULT, CW_USEDEFAULT,
                             static_cast<int32_t>(width_),
                             static_cast<int32_t>(height_),
                             SWP_NOZORDER | SWP_FRAMECHANGED);
                current_mode_ = WindowMode::kWindowed;
            }
            else if (mode == WindowMode::kBorderless)
            {
                ChangeDisplaySettingsA(nullptr, 0);
                SetWindowLongPtrA(hwnd_, GWL_STYLE,
                                  static_cast<LONG_PTR>(WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS));
                SetWindowPos(hwnd_, nullptr, CW_USEDEFAULT, CW_USEDEFAULT,
                             static_cast<int32_t>(width_),
                             static_cast<int32_t>(height_),
                             SWP_NOZORDER | SWP_FRAMECHANGED);
                current_mode_ = WindowMode::kBorderless;
            }
        }
    }

    WindowMode Win32Window::GetWindowMode() const
    {
        return current_mode_;
    }

    IWindow::WindowID Win32Window::GetWindowId() const
    {
        return window_id_;
    }

    void Win32Window::SetWindowId(WindowID id)
    {
        window_id_ = id;
    }

    void Win32Window::SetCallbacks(const WindowCallbacks &callbacks)
    {
        callbacks_ = callbacks;
    }

    bool Win32Window::IsInitialized() const
    {
        return is_initialized_;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Win32 窗口过程
    // ═══════════════════════════════════════════════════════════════════════

    LRESULT CALLBACK Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        Win32Window *window = reinterpret_cast<Win32Window *>(
            GetWindowLongPtrA(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_NCCREATE:
        {
            CREATESTRUCTA *create_struct = reinterpret_cast<CREATESTRUCTA *>(lParam);
            if (create_struct && create_struct->lpCreateParams)
            {
                SetWindowLongPtrA(hwnd, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(create_struct->lpCreateParams));
                window = reinterpret_cast<Win32Window *>(create_struct->lpCreateParams);
                window->hwnd_ = hwnd;
            }
            return DefWindowProcA(hwnd, msg, wParam, lParam);
        }

        default:
            break;
        }

        if (window)
        {
            return window->HandleMessage(msg, wParam, lParam);
        }

        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    LRESULT Win32Window::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_SIZE:
        {
            if (wParam == SIZE_MINIMIZED)
            {
                is_minimized_ = true;
                is_maximized_ = false;
            }
            else if (wParam == SIZE_MAXIMIZED)
            {
                is_minimized_ = false;
                is_maximized_ = true;
            }
            else if (wParam == SIZE_RESTORED)
            {
                is_minimized_ = false;
                is_maximized_ = false;
            }

            if (wParam != SIZE_MINIMIZED && is_initialized_)
            {
                uint32_t new_width = static_cast<uint32_t>(LOWORD(lParam));
                uint32_t new_height = static_cast<uint32_t>(HIWORD(lParam));
                UpdateSize(new_width, new_height);

                if (callbacks_.OnResize)
                {
                    callbacks_.OnResize(window_id_,
                                        static_cast<int32_t>(new_width),
                                        static_cast<int32_t>(new_height),
                                        callbacks_.userdata);
                }
            }
            return 0;
        }

        case WM_MOVE:
        {
            pos_x_ = static_cast<int32_t>(static_cast<int16_t>(LOWORD(lParam)));
            pos_y_ = static_cast<int32_t>(static_cast<int16_t>(HIWORD(lParam)));

            if (hwnd_)
            {
                HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
                if (monitor)
                {
                    MONITORINFOEXA mi;
                    mi.cbSize = sizeof(MONITORINFOEXA);
                    if (GetMonitorInfoA(monitor, &mi))
                    {
                        screen_id_ = static_cast<uint32_t>(
                            reinterpret_cast<uintptr_t>(monitor) & 0xFFFFFFFF);
                    }
                }
            }
            return 0;
        }

        case WM_CLOSE:
        {
            should_close_ = true;

            if (callbacks_.OnClose)
            {
                callbacks_.OnClose(window_id_, callbacks_.userdata);
            }
            return 0;
        }

        case WM_DESTROY:
        {
            PostQuitMessage(0);
            return 0;
        }

        case WM_SETFOCUS:
        {
            if (callbacks_.OnFocus)
            {
                callbacks_.OnFocus(window_id_, true, callbacks_.userdata);
            }
            return 0;
        }

        case WM_KILLFOCUS:
        {
            if (callbacks_.OnFocus)
            {
                callbacks_.OnFocus(window_id_, false, callbacks_.userdata);
            }
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        {
            if (callbacks_.OnKey)
            {
                int32_t scancode = static_cast<int32_t>((lParam >> 16) & 0xFF);
                uint32_t modifiers = GetKeyModifiers();
                callbacks_.OnKey(window_id_, static_cast<int32_t>(wParam),
                                 scancode, true, modifiers, callbacks_.userdata);
            }

            if (wParam == VK_F4 && (lParam & (1 << 29)))
            {
                should_close_ = true;
                if (callbacks_.OnClose)
                {
                    callbacks_.OnClose(window_id_, callbacks_.userdata);
                }
            }
            return 0;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP:
        {
            if (callbacks_.OnKey)
            {
                int32_t scancode = static_cast<int32_t>((lParam >> 16) & 0xFF);
                uint32_t modifiers = GetKeyModifiers();
                callbacks_.OnKey(window_id_, static_cast<int32_t>(wParam),
                                 scancode, false, modifiers, callbacks_.userdata);
            }
            return 0;
        }

        case WM_CHAR:
        {
            if (callbacks_.OnChar)
            {
                callbacks_.OnChar(window_id_, static_cast<uint32_t>(wParam),
                                  callbacks_.userdata);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
        {
            if (callbacks_.OnMouse)
            {
                int32_t x = static_cast<int32_t>(static_cast<int16_t>(LOWORD(lParam)));
                int32_t y = static_cast<int32_t>(static_cast<int16_t>(HIWORD(lParam)));
                bool pressed = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
                                msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN);
                MouseButton button = Win32ButtonToMouseButton(wParam);
                uint32_t modifiers = GetKeyModifiers();
                callbacks_.OnMouse(window_id_, x, y, button, pressed,
                                   modifiers, callbacks_.userdata);
            }

            if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP)
            {
                return TRUE;
            }
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            if (callbacks_.OnMouseMove)
            {
                int32_t x = static_cast<int32_t>(static_cast<int16_t>(LOWORD(lParam)));
                int32_t y = static_cast<int32_t>(static_cast<int16_t>(HIWORD(lParam)));
                uint32_t modifiers = GetKeyModifiers();
                callbacks_.OnMouseMove(window_id_, x, y, modifiers, callbacks_.userdata);
            }
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            if (callbacks_.OnScroll)
            {
                double y_offset = static_cast<double>(static_cast<int16_t>(HIWORD(wParam))) /
                                  static_cast<double>(WHEEL_DELTA);
                uint32_t modifiers = GetKeyModifiers();
                callbacks_.OnScroll(window_id_, 0.0, y_offset, modifiers, callbacks_.userdata);
            }
            return 0;
        }

        case WM_MOUSEHWHEEL:
        {
            if (callbacks_.OnScroll)
            {
                double x_offset = static_cast<double>(static_cast<int16_t>(HIWORD(wParam))) /
                                  static_cast<double>(WHEEL_DELTA);
                uint32_t modifiers = GetKeyModifiers();
                callbacks_.OnScroll(window_id_, x_offset, 0.0, modifiers, callbacks_.userdata);
            }
            return 0;
        }

        default:
            break;
        }

        return DefWindowProcA(hwnd_, msg, wParam, lParam);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 内部方法
    // ═══════════════════════════════════════════════════════════════════════

    bool Win32Window::RegisterWindowClass()
    {
        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(WNDCLASSEXA);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hinstance_;
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW);
        wc.lpszClassName = kWindowClassName;
        wc.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
        wc.hIconSm = LoadIconA(nullptr, IDI_APPLICATION);

        if (!RegisterClassExA(&wc))
        {
            DWORD error = GetLastError();
            if (error == ERROR_CLASS_ALREADY_EXISTS)
            {
                ARHUD_LOG_INFO("Win32Window: Window class '%s' already registered, reusing",
                               kWindowClassName);
                is_class_registered_ = true;
                return true;
            }

            ARHUD_LOG_ERROR("kFailed", "Win32Window: RegisterClassExA failed with error: %lu", error);
            return false;
        }

        is_class_registered_ = true;
        return true;
    }

    void Win32Window::UnregisterWindowClass()
    {
        if (is_class_registered_)
        {
            UnregisterClassA(kWindowClassName, hinstance_);
            is_class_registered_ = false;
        }
    }

    bool Win32Window::CreateNativeWindow(const char *title, uint32_t width, uint32_t height)
    {
        DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        DWORD ex_style = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;

        if (!desc_.resizable)
        {
            style &= ~WS_THICKFRAME;
            style &= ~WS_MAXIMIZEBOX;
        }

        if (desc_.mode == WindowMode::kBorderless)
        {
            style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
            ex_style = WS_EX_APPWINDOW;
        }

        RECT window_rect;
        window_rect.left = 0;
        window_rect.top = 0;
        window_rect.right = static_cast<LONG>(width);
        window_rect.bottom = static_cast<LONG>(height);
        AdjustWindowRect(&window_rect, style, FALSE);

        int32_t create_x = desc_.x >= 0 ? desc_.x : CW_USEDEFAULT;
        int32_t create_y = desc_.y >= 0 ? desc_.y : CW_USEDEFAULT;

        hwnd_ = CreateWindowExA(
            ex_style,
            kWindowClassName,
            title,
            style,
            create_x, create_y,
            window_rect.right - window_rect.left,
            window_rect.bottom - window_rect.top,
            nullptr,
            nullptr,
            hinstance_,
            this);

        if (!hwnd_)
        {
            ARHUD_LOG_ERROR("kFailed", "Win32Window: CreateWindowExA failed with error: %lu",
                            GetLastError());
            return false;
        }

        width_ = width;
        height_ = height;

        if (desc_.mode == WindowMode::kFullscreen || desc_.mode == WindowMode::kExclusiveFullscreen)
        {
            DEVMODEA dm = {};
            dm.dmSize = sizeof(DEVMODEA);
            dm.dmPelsWidth = width;
            dm.dmPelsHeight = height;
            dm.dmBitsPerPel = 32;
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;

            LONG result = ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
            if (result != DISP_CHANGE_SUCCESSFUL)
            {
                ARHUD_LOG_WARN("Win32Window: Failed to switch to fullscreen mode, falling back to windowed");
            }
            else
            {
                SetWindowLongPtrA(hwnd_, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
                SetWindowPos(hwnd_, HWND_TOP, 0, 0, static_cast<int32_t>(width),
                             static_cast<int32_t>(height), SWP_NOZORDER | SWP_FRAMECHANGED);
            }
        }

        if (desc_.visible)
        {
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
        }

        RECT win_rect;
        if (GetWindowRect(hwnd_, &win_rect))
        {
            pos_x_ = win_rect.left;
            pos_y_ = win_rect.top;
        }

        ARHUD_LOG_INFO("Win32Window: Native window created: %s (%ux%u)", title, width, height);
        return true;
    }

    void Win32Window::UpdateSize(uint32_t width, uint32_t height)
    {
        if (width > 0 && height > 0)
        {
            width_ = width;
            height_ = height;
        }
    }

    uint32_t Win32Window::GetKeyModifiers() const
    {
        uint32_t modifiers = kModifierNone;

        if (GetKeyState(VK_SHIFT) & 0x8000)
        {
            modifiers |= kModifierShift;
        }
        if (GetKeyState(VK_CONTROL) & 0x8000)
        {
            modifiers |= kModifierCtrl;
        }
        if (GetKeyState(VK_MENU) & 0x8000)
        {
            modifiers |= kModifierAlt;
        }
        if (GetKeyState(VK_LWIN) & 0x8000 || GetKeyState(VK_RWIN) & 0x8000)
        {
            modifiers |= kModifierSuper;
        }
        if (GetKeyState(VK_CAPITAL) & 0x0001)
        {
            modifiers |= kModifierCapsLock;
        }
        if (GetKeyState(VK_NUMLOCK) & 0x0001)
        {
            modifiers |= kModifierNumLock;
        }

        return modifiers;
    }

    MouseButton Win32Window::Win32ButtonToMouseButton(WPARAM wParam)
    {
        switch (wParam)
        {
        case MK_LBUTTON:
            return MouseButton::kLeft;
        case MK_RBUTTON:
            return MouseButton::kRight;
        case MK_MBUTTON:
            return MouseButton::kMiddle;
        case MK_XBUTTON1:
            return MouseButton::kX1;
        case MK_XBUTTON2:
            return MouseButton::kX2;
        default:
            return MouseButton::kLeft;
        }
    }

}

#endif