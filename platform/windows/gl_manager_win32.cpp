/**
 * @file gl_manager_win32.cpp
 * @brief Win32 平台 OpenGL 上下文管理实现
 *
 * @par 统一 Context 模型
 *   所有 Context 地位平等，不共享资源。
 *   MakeCurrent(ContextID, SurfaceID) 统一绑定。
 *
 * @author yameng.he
 * @version 4.0
 * @date 2026-05-16
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_os
 */

#include "gl_manager_win32.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "io/logger.h"
#include "os/memory.h"

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x00000002
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define WGL_CONTEXT_DEBUG_BIT_ARB 0x0001

namespace arhud
{

    // ══════════════════════════════════════════════════════════════════════════
    // 构造与析构
    // ══════════════════════════════════════════════════════════════════════════

    Win32GLManager::Win32GLManager() = default;

    Win32GLManager::~Win32GLManager()
    {
        if (is_initialized_)
        {
            Shutdown();
        }
    }

    // ══════════════════════════════════════════════════════════════════════════
    // IGLManager 接口实现 — 生命周期
    // ══════════════════════════════════════════════════════════════════════════

    Error Win32GLManager::Initialize()
    {
        if (is_initialized_)
        {
            ARHUD_LOG_WARN("Win32GLManager: Already initialized");
            return Error::kFailed;
        }

        Error err = LoadOpenGLDriver();
        if (err != Error::kOK)
        {
            return err;
        }

        is_initialized_ = true;
        ARHUD_LOG_INFO("Win32GLManager: Initialized");
        return Error::kOK;
    }

    void Win32GLManager::Shutdown()
    {
        if (!is_initialized_)
        {
            return;
        }

        for (uint32_t i = 0; i < surfaces_.Size(); ++i)
        {
            GLSurface &surf = surfaces_[i];
            surf.hdc_ = nullptr;
            surf.hwnd_ = nullptr;
            surf.is_valid = false;
        }
        surfaces_.Clear();

        for (uint32_t i = 0; i < contexts_.Size(); ++i)
        {
            GLContext &ctx = contexts_[i];
            if (ctx.hrc_ && wgl_delete_context_)
            {
                wgl_delete_context_(ctx.hrc_);
                ctx.hrc_ = nullptr;
            }
            ctx.is_valid = false;
        }
        contexts_.Clear();

        if (opengl_module_)
        {
            FreeLibrary(opengl_module_);
            opengl_module_ = nullptr;
        }

        wgl_create_context_ = nullptr;
        wgl_make_current_ = nullptr;
        wgl_delete_context_ = nullptr;
        wgl_get_proc_address_ = nullptr;
        wgl_create_context_attribs_arb_ = nullptr;
        wgl_swap_interval_ext_ = nullptr;
        wgl_get_swap_interval_ext_ = nullptr;
        swap_control_tear_supported_ = false;
        extensions_loaded_ = false;

        is_initialized_ = false;
        ARHUD_LOG_INFO("Win32GLManager: Shutdown");
    }

    // ══════════════════════════════════════════════════════════════════════════
    // IGLManager 接口实现 — 上下文管理
    // ══════════════════════════════════════════════════════════════════════════

    IGLManager::ContextID Win32GLManager::CreateContext(SurfaceID p_surface)
    {
        if (!is_initialized_)
        {
            ARHUD_LOG_ERROR("kFailed", "Win32GLManager: Not initialized");
            return kInvalidContextId;
        }

        GLSurface *surf = GetSurface(p_surface);
        if (!surf || !surf->hdc_)
        {
            ARHUD_LOG_ERROR("kInvalidParameter", "Win32GLManager: Invalid surface ID %u",
                            static_cast<uint32_t>(p_surface));
            return kInvalidContextId;
        }

        HDC hdc = surf->hdc_;

        HGLRC hrc = nullptr;

        if (!extensions_loaded_)
        {
            hrc = CreateHGLRCWithTempContext(hdc);
            if (!hrc)
            {
                ARHUD_LOG_ERROR("kFailed",
                                "Win32GLManager: Failed to create context via temp context");
                return kInvalidContextId;
            }
            extensions_loaded_ = true;
            wgl_make_current_(hdc, nullptr);
        }
        else
        {
            if (!wgl_create_context_attribs_arb_)
            {
                ARHUD_LOG_ERROR("kFailed",
                                "Win32GLManager: wglCreateContextAttribsARB not available");
                return kInvalidContextId;
            }

            int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
                0};

            hrc = wgl_create_context_attribs_arb_(hdc, nullptr, attribs);
            if (!hrc)
            {
                ARHUD_LOG_ERROR("kFailed",
                                "Win32GLManager: Failed to create context (error=%lu)",
                                GetLastError());
                return kInvalidContextId;
            }
        }

        GLContext ctx;
        ctx.hrc_ = hrc;
        ctx.is_valid = true;
        contexts_.PushBack(ctx);
        ContextID new_id = static_cast<ContextID>(contexts_.Size() - 1);

        ARHUD_LOG_INFO("Win32GLManager: Context created (ContextID=%u, SurfaceID=%u)",
                       static_cast<uint32_t>(new_id),
                       static_cast<uint32_t>(p_surface));
        return new_id;
    }

    void Win32GLManager::DestroyContext(ContextID p_context_id)
    {
        if (p_context_id >= contexts_.Size() || !contexts_[p_context_id].is_valid)
        {
            ARHUD_LOG_WARN("Win32GLManager: Cannot destroy context %u (invalid)",
                           static_cast<uint32_t>(p_context_id));
            return;
        }

        GLContext &ctx = contexts_[p_context_id];

        for (uint32_t i = 0; i < surfaces_.Size(); ++i)
        {
            if (surfaces_[i].is_valid && surfaces_[i].owner_context_id_ == p_context_id)
            {
                surfaces_[i].owner_context_id_ = kInvalidContextId;
            }
        }

        if (ctx.hrc_ && wgl_delete_context_)
        {
            HGLRC current = wglGetCurrentContext();
            if (current == ctx.hrc_)
            {
                HDC current_dc = wglGetCurrentDC();
                wgl_make_current_(current_dc, nullptr);
            }
            wgl_delete_context_(ctx.hrc_);
            ctx.hrc_ = nullptr;
        }

        ctx.is_valid = false;

        ARHUD_LOG_INFO("Win32GLManager: Context %u destroyed",
                       static_cast<uint32_t>(p_context_id));
    }

    uint32_t Win32GLManager::GetContextCount() const
    {
        return contexts_.Size();
    }

    // ══════════════════════════════════════════════════════════════════════════
    // IGLManager 接口实现 — 渲染表面
    // ══════════════════════════════════════════════════════════════════════════

    IGLManager::SurfaceID Win32GLManager::CreateSurface(IScreen::ScreenID p_screen_id,
                                                        void *p_native_window,
                                                        uint32_t p_width, uint32_t p_height)
    {
        if (!is_initialized_)
        {
            ARHUD_LOG_ERROR("kFailed", "Win32GLManager: Not initialized");
            return kInvalidSurfaceId;
        }

        HWND hwnd = static_cast<HWND>(p_native_window);
        if (!hwnd)
        {
            ARHUD_LOG_ERROR("kInvalidParameter", "Win32GLManager: Null native window handle");
            return kInvalidSurfaceId;
        }

        HDC hdc = GetDC(hwnd);
        if (!hdc)
        {
            ARHUD_LOG_ERROR("kFailed", "Win32GLManager: GetDC failed for screen %u",
                            static_cast<uint32_t>(p_screen_id));
            return kInvalidSurfaceId;
        }

        Error pf_result = ConfigurePixelFormat(hdc);
        if (pf_result != Error::kOK)
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: Pixel format configuration failed for screen %u",
                            static_cast<uint32_t>(p_screen_id));
            return kInvalidSurfaceId;
        }

        GLSurface surf;
        surf.hwnd_ = hwnd;
        surf.hdc_ = hdc;
        surf.vsync_mode_ = VSyncMode::kEnabled;
        surf.screen_id_ = p_screen_id;
        surf.is_valid = true;

        surfaces_.PushBack(surf);
        SurfaceID new_id = static_cast<SurfaceID>(surfaces_.Size() - 1);

        ARHUD_LOG_INFO("Win32GLManager: Surface created (SurfaceID=%u, ScreenID=%u, %ux%u)",
                       static_cast<uint32_t>(new_id),
                       static_cast<uint32_t>(p_screen_id),
                       p_width, p_height);
        return new_id;
    }

    void Win32GLManager::DestroySurface(SurfaceID p_surface)
    {
        if (p_surface >= surfaces_.Size() || !surfaces_[p_surface].is_valid)
        {
            ARHUD_LOG_WARN("Win32GLManager: Cannot destroy surface %u (invalid)",
                           static_cast<uint32_t>(p_surface));
            return;
        }

        GLSurface &surf = surfaces_[p_surface];

        if (surf.owner_context_id_ != kInvalidContextId)
        {
            GLContext *owner = GetContext(surf.owner_context_id_);
            if (owner && owner->bound_surface_ == p_surface)
            {
                owner->bound_surface_ = kInvalidSurfaceId;
            }
        }

        for (uint32_t i = 0; i < contexts_.Size(); ++i)
        {
            if (contexts_[i].is_valid && contexts_[i].bound_surface_ == p_surface)
            {
                contexts_[i].bound_surface_ = kInvalidSurfaceId;
            }
        }

        surf.hdc_ = nullptr;
        surf.hwnd_ = nullptr;
        surf.is_valid = false;

        ARHUD_LOG_INFO("Win32GLManager: Surface %u destroyed",
                       static_cast<uint32_t>(p_surface));
    }

    // ══════════════════════════════════════════════════════════════════════════
    // IGLManager 接口实现 — 上下文绑定
    // ══════════════════════════════════════════════════════════════════════════

    Error Win32GLManager::MakeCurrent(ContextID p_context_id, SurfaceID p_surface)
    {
        GLSurface *surf = GetSurface(p_surface);
        if (!surf || !surf->hdc_)
        {
            ARHUD_LOG_ERROR("kInvalidParameter",
                            "Win32GLManager: Invalid surface ID %u",
                            static_cast<uint32_t>(p_surface));
            return Error::kInvalidParameter;
        }

        GLContext *ctx = GetContext(p_context_id);
        if (!ctx || !ctx->hrc_)
        {
            ARHUD_LOG_ERROR("kInvalidParameter",
                            "Win32GLManager: Invalid context ID %u",
                            static_cast<uint32_t>(p_context_id));
            return Error::kInvalidParameter;
        }

        if (surf->owner_context_id_ != kInvalidContextId &&
            surf->owner_context_id_ != p_context_id)
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: Surface %u owned by context %u, "
                            "cannot bind to context %u",
                            static_cast<uint32_t>(p_surface),
                            static_cast<uint32_t>(surf->owner_context_id_),
                            static_cast<uint32_t>(p_context_id));
            return Error::kFailed;
        }

        if (wglGetCurrentContext() == ctx->hrc_ && wglGetCurrentDC() == surf->hdc_)
        {
            return Error::kOK;
        }

        if (!wgl_make_current_(surf->hdc_, ctx->hrc_))
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: wglMakeCurrent failed for context %u "
                            "surface %u (error=%lu)",
                            static_cast<uint32_t>(p_context_id),
                            static_cast<uint32_t>(p_surface),
                            GetLastError());
            return Error::kFailed;
        }

        ctx->bound_surface_ = p_surface;

        if (surf->owner_context_id_ == kInvalidContextId)
        {
            surf->owner_context_id_ = p_context_id;
        }

        return Error::kOK;
    }

    void Win32GLManager::ReleaseCurrent()
    {
        HGLRC current_hrc = wglGetCurrentContext();
        for (uint32_t i = 0; i < contexts_.Size(); ++i)
        {
            if (contexts_[i].hrc_ == current_hrc)
            {
                contexts_[i].bound_surface_ = kInvalidSurfaceId;
                break;
            }
        }

        HDC hdc = wglGetCurrentDC();
        if (hdc && wgl_make_current_)
        {
            wgl_make_current_(hdc, nullptr);
        }
    }

    // ══════════════════════════════════════════════════════════════════════════
    // IGLManager 接口实现 — 渲染
    // ══════════════════════════════════════════════════════════════════════════

    void Win32GLManager::SwapBuffers(SurfaceID p_surface)
    {
        GLSurface *surf = GetSurface(p_surface);
        if (!surf || !surf->hdc_)
        {
            return;
        }
        ::SwapBuffers(surf->hdc_);
    }

    // ══════════════════════════════════════════════════════════════════════════
    // IGLManager 接口实现 — VSync
    // ══════════════════════════════════════════════════════════════════════════

    void Win32GLManager::SetVsyncMode(SurfaceID p_surface, VSyncMode p_mode)
    {
        GLSurface *surf = GetSurface(p_surface);
        if (!surf)
        {
            return;
        }

        if (!wgl_swap_interval_ext_)
        {
            ARHUD_LOG_WARN("Win32GLManager: VSync not supported by driver");
            return;
        }

        int interval = 0;
        switch (p_mode)
        {
        case VSyncMode::kDisabled:
            interval = 0;
            break;
        case VSyncMode::kEnabled:
            interval = 1;
            break;
        case VSyncMode::kAdaptive:
            if (swap_control_tear_supported_)
            {
                interval = -1;
            }
            else
            {
                ARHUD_LOG_WARN("Win32GLManager: Adaptive VSync not supported, "
                               "falling back to enabled");
                interval = 1;
                p_mode = VSyncMode::kEnabled;
            }
            break;
        }

        if (!wgl_swap_interval_ext_(interval))
        {
            ARHUD_LOG_WARN("Win32GLManager: wglSwapIntervalEXT(%d) failed", interval);
            return;
        }

        surf->vsync_mode_ = p_mode;
    }

    VSyncMode Win32GLManager::GetVsyncMode(SurfaceID p_surface) const
    {
        const GLSurface *surf = GetSurface(p_surface);
        return surf ? surf->vsync_mode_ : VSyncMode::kEnabled;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // IGLManager 接口实现 — 原生句柄
    // ══════════════════════════════════════════════════════════════════════════

    void *Win32GLManager::GetDeviceContext(SurfaceID p_surface) const
    {
        const GLSurface *surf = GetSurface(p_surface);
        return surf ? surf->hdc_ : nullptr;
    }

    void *Win32GLManager::GetRenderContext(ContextID p_context_id) const
    {
        const GLContext *ctx = GetContext(p_context_id);
        return ctx ? ctx->hrc_ : nullptr;
    }

    void *Win32GLManager::GetGLProcAddress() const
    {
        return reinterpret_cast<void *>(CombinedGLLoader);
    }

    void *Win32GLManager::CombinedGLLoader(const char *name)
    {
        void *proc = reinterpret_cast<void *>(wglGetProcAddress(name));
        if (!proc)
        {
            HMODULE module = GetModuleHandleW(L"opengl32.dll");
            if (module)
            {
                proc = reinterpret_cast<void *>(GetProcAddress(module, name));
            }
        }
        return proc;
    }

    void *Win32GLManager::GetOpenGLModule() const
    {
        return reinterpret_cast<void *>(opengl_module_);
    }

    bool Win32GLManager::IsInitialized() const
    {
        return is_initialized_;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // 私有方法实现
    // ══════════════════════════════════════════════════════════════════════════

    Error Win32GLManager::ConfigurePixelFormat(HDC p_hdc)
    {
        int current_format = GetPixelFormat(p_hdc);
        if (current_format != 0)
        {
            return Error::kOK;
        }

        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 24;
        pfd.cAlphaBits = 8;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;

        int pixel_format = ChoosePixelFormat(p_hdc, &pfd);
        if (!pixel_format)
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: ChoosePixelFormat failed (error=%lu)",
                            GetLastError());
            return Error::kFailed;
        }

        if (!SetPixelFormat(p_hdc, pixel_format, &pfd))
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: SetPixelFormat failed (error=%lu)",
                            GetLastError());
            return Error::kFailed;
        }

        return Error::kOK;
    }

    Error Win32GLManager::LoadOpenGLDriver()
    {
        opengl_module_ = LoadLibraryW(L"opengl32.dll");
        if (!opengl_module_)
        {
            ARHUD_LOG_ERROR("kFailed", "Win32GLManager: Failed to load opengl32.dll");
            return Error::kFailed;
        }

        wgl_create_context_ = reinterpret_cast<WglCreateContextFunc>(
            GetProcAddress(opengl_module_, "wglCreateContext"));
        wgl_make_current_ = reinterpret_cast<WglMakeCurrentFunc>(
            GetProcAddress(opengl_module_, "wglMakeCurrent"));
        wgl_delete_context_ = reinterpret_cast<WglDeleteContextFunc>(
            GetProcAddress(opengl_module_, "wglDeleteContext"));
        wgl_get_proc_address_ = reinterpret_cast<WglGetProcAddressFunc>(
            GetProcAddress(opengl_module_, "wglGetProcAddress"));

        if (!wgl_create_context_ || !wgl_make_current_ ||
            !wgl_delete_context_ || !wgl_get_proc_address_)
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: Failed to get WGL function pointers");
            FreeLibrary(opengl_module_);
            opengl_module_ = nullptr;
            wgl_create_context_ = nullptr;
            wgl_make_current_ = nullptr;
            wgl_delete_context_ = nullptr;
            wgl_get_proc_address_ = nullptr;
            return Error::kFailed;
        }

        return Error::kOK;
    }

    HGLRC Win32GLManager::CreateHGLRCWithTempContext(HDC p_hdc)
    {
        HGLRC temp_context = wgl_create_context_(p_hdc);
        if (!temp_context)
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: Failed to create temp context (error=%lu)",
                            GetLastError());
            return nullptr;
        }

        if (!wgl_make_current_(p_hdc, temp_context))
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: Failed to make temp context current (error=%lu)",
                            GetLastError());
            wgl_delete_context_(temp_context);
            return nullptr;
        }

        wgl_create_context_attribs_arb_ = reinterpret_cast<WglCreateContextAttribsARBFunc>(
            wgl_get_proc_address_(reinterpret_cast<LPCSTR>("wglCreateContextAttribsARB")));

        if (!wgl_create_context_attribs_arb_)
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: wglCreateContextAttribsARB not available");
            wgl_make_current_(p_hdc, nullptr);
            wgl_delete_context_(temp_context);
            return nullptr;
        }

        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
            0};

        HGLRC hrc = wgl_create_context_attribs_arb_(p_hdc, nullptr, attribs);
        if (!hrc)
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: Failed to create 3.3 core profile context");
            wgl_make_current_(p_hdc, nullptr);
            wgl_delete_context_(temp_context);
            return nullptr;
        }

        wgl_make_current_(p_hdc, nullptr);
        wgl_delete_context_(temp_context);

        if (!wgl_make_current_(p_hdc, hrc))
        {
            ARHUD_LOG_ERROR("kFailed",
                            "Win32GLManager: Failed to make 3.3 context current (error=%lu)",
                            GetLastError());
            wgl_delete_context_(hrc);
            return nullptr;
        }

        LoadExtensions();

        return hrc;
    }

    void Win32GLManager::LoadExtensions()
    {
        wgl_swap_interval_ext_ = reinterpret_cast<WglSwapIntervalEXTFunc>(
            wgl_get_proc_address_(reinterpret_cast<LPCSTR>("wglSwapIntervalEXT")));
        wgl_get_swap_interval_ext_ = reinterpret_cast<WglGetSwapIntervalEXTFunc>(
            wgl_get_proc_address_(reinterpret_cast<LPCSTR>("wglGetSwapIntervalEXT")));

        if (wgl_get_proc_address_(reinterpret_cast<LPCSTR>("wglSwapControlTearEXT")))
        {
            swap_control_tear_supported_ = true;
        }

        if (wgl_swap_interval_ext_)
        {
            ARHUD_LOG_INFO("Win32GLManager: WGL_EXT_swap_control available");
            if (swap_control_tear_supported_)
            {
                ARHUD_LOG_INFO("Win32GLManager: WGL_EXT_swap_control_tear available "
                               "(adaptive VSync)");
            }
        }
        else
        {
            ARHUD_LOG_WARN("Win32GLManager: WGL_EXT_swap_control not available, "
                           "VSync disabled");
        }
    }

    Win32GLManager::GLSurface *Win32GLManager::GetSurface(SurfaceID p_surface)
    {
        if (p_surface >= surfaces_.Size() || !surfaces_[p_surface].is_valid)
        {
            return nullptr;
        }
        return &surfaces_[p_surface];
    }

    const Win32GLManager::GLSurface *Win32GLManager::GetSurface(SurfaceID p_surface) const
    {
        if (p_surface >= surfaces_.Size() || !surfaces_[p_surface].is_valid)
        {
            return nullptr;
        }
        return &surfaces_[p_surface];
    }

    Win32GLManager::GLContext *Win32GLManager::GetContext(ContextID p_context_id)
    {
        if (p_context_id >= contexts_.Size() || !contexts_[p_context_id].is_valid)
        {
            return nullptr;
        }
        return &contexts_[p_context_id];
    }

    const Win32GLManager::GLContext *Win32GLManager::GetContext(ContextID p_context_id) const
    {
        if (p_context_id >= contexts_.Size() || !contexts_[p_context_id].is_valid)
        {
            return nullptr;
        }
        return &contexts_[p_context_id];
    }

} // namespace arhud

#endif // ARHUD_PLATFORM_WINDOWS
