/**
 * @file gl_manager_win32.h
 * @brief Win32 平台 OpenGL 上下文管理实现声明
 *
 * @par 概述
 *   Win32GLManager 是 IGLManager 接口的 Windows 平台实现，
 *   使用原生 WGL API 管理 OpenGL 渲染上下文和渲染表面。
 *   采用统一 Context 模型，所有 Context 地位平等，无主从区分。
 *
 * @par 统一 Context 模型
 *   @code
 *   contexts_[0..N] — 所有 Context 地位平等，不共享资源
 *   surfaces_[0..M] — 渲染表面（HDC + HWND），可绑定到任意 Context
 *
 *   单线程模式：1 Context + N Surface，MakeCurrent 切换
 *   多线程模式：N Context + N Surface，每线程独立
 *   @endcode
 *
 * @par Context 创建流程
 *   首次 CreateContext() 使用两步法：
 *   1. 创建临时 2.1 上下文 → 加载 WGL 扩展 → 创建正式 3.3 Core Profile
 *   后续 CreateContext() 直接使用 wglCreateContextAttribsARB
 *
 * @par VSync 实现
 *   - VSyncMode::kDisabled：wglSwapIntervalEXT(0)
 *   - VSyncMode::kEnabled：wglSwapIntervalEXT(1)
 *   - VSyncMode::kAdaptive：wglSwapIntervalEXT(-1)
 *     （需 WGL_EXT_swap_control_tear 扩展支持）
 *
 * @author yameng.he
 * @version 4.0
 * @date 2026-05-16
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_os
 */

#pragma once

#include "typedefs.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "gl_manager.h"
#include "template/local_vector.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace arhud
{

    /**
     * @brief Win32 平台 OpenGL 上下文管理实现
     *
     * 使用原生 WGL API 实现统一 Context 模型。
     * 所有 Context 地位平等，不共享资源。
     *
     * @see IGLManager  基类接口
     */
    class Win32GLManager : public IGLManager
    {
    public:
        Win32GLManager();
        virtual ~Win32GLManager() override;

        /**
         * @brief 初始化 Win32 GL 管理器
         *
         * 加载 opengl32.dll 并获取基础 WGL 函数指针。
         *
         * @return Error::kOK 初始化成功
         * @return Error::kFailed 已初始化或加载驱动失败
         */
        virtual Error Initialize() override;

        /**
         * @brief 关闭 Win32 GL 管理器并释放所有资源
         *
         * 清理所有 OpenGL 上下文、渲染表面和动态加载的函数指针。
         */
        virtual void Shutdown() override;

        /**
         * @brief 创建 GL 渲染上下文
         *
         * 首次调用使用两步法（临时上下文 → 加载扩展 → 正式 3.3 Core Profile）。
         * 后续调用直接使用 wglCreateContextAttribsARB。
         * 所有 Context 不共享资源。
         *
         * @param[in] p_native_window 原生窗口句柄 (HWND)
         * @param[in] p_width 窗口客户区宽度（像素）
         * @param[in] p_height 窗口客户区高度（像素）
         *
         * @return 新创建的 Context ID
         * @retval kInvalidContextId 创建失败
         *
         * @pre Initialize() 已成功调用
         */
        virtual ContextID CreateContext(SurfaceID p_surface) override;

        /**
         * @brief 销毁 GL 渲染上下文
         *
         * @param[in] p_context_id Context ID
         *
         * @pre p_context_id 有效
         */
        virtual void DestroyContext(ContextID p_context_id) override;

        /**
         * @brief 查询上下文数量
         * @return 当前存活的上下文数量
         */
        virtual uint32_t GetContextCount() const override;

        /**
         * @brief 创建渲染表面
         *
         * @param[in] p_screen_id 关联的屏幕 ID
         * @param[in] p_native_window 原生窗口句柄 (HWND)
         * @param[in] p_width 窗口客户区宽度（像素）
         * @param[in] p_height 窗口客户区高度（像素）
         *
         * @return 新创建的 Surface ID
         * @retval kInvalidSurfaceId 创建失败
         */
        virtual SurfaceID CreateSurface(IScreen::ScreenID p_screen_id,
                                        void *p_native_window,
                                        uint32_t p_width, uint32_t p_height) override;

        /**
         * @brief 销毁渲染表面
         *
         * @param[in] p_surface Surface ID
         */
        virtual void DestroySurface(SurfaceID p_surface) override;

        /**
         * @brief 将指定上下文绑定到指定表面的调用线程
         *
         * @param[in] p_context_id Context ID
         * @param[in] p_surface Surface ID
         *
         * @return Error::kOK 绑定成功
         * @return Error::kInvalidParameter Context ID 或 Surface ID 无效
         * @return Error::kFailed Surface 被其他 Context 占有或平台绑定失败
         */
        virtual Error MakeCurrent(ContextID p_context_id,
                                   SurfaceID p_surface) override;

        /**
         * @brief 释放当前线程的 GL 上下文
         */
        virtual void ReleaseCurrent() override;

        /**
         * @brief 交换指定渲染表面的前后缓冲区
         *
         * @param[in] p_surface Surface ID
         */
        virtual void SwapBuffers(SurfaceID p_surface) override;

        /**
         * @brief 设置指定渲染表面的垂直同步模式
         *
         * @param[in] p_surface Surface ID
         * @param[in] p_mode VSync 模式
         */
        virtual void SetVsyncMode(SurfaceID p_surface, VSyncMode p_mode) override;

        /**
         * @brief 获取指定渲染表面的垂直同步模式
         *
         * @param[in] p_surface Surface ID
         * @return 当前的 VSync 模式
         */
        virtual VSyncMode GetVsyncMode(SurfaceID p_surface) const override;

        /**
         * @brief 获取指定渲染表面的设备上下文句柄
         *
         * @param[in] p_surface Surface ID
         * @return 设备上下文句柄 (HDC)，无效时返回 nullptr
         */
        virtual void *GetDeviceContext(SurfaceID p_surface) const override;

        /**
         * @brief 获取指定 OpenGL 上下文的渲染上下文句柄
         *
         * @param[in] p_context_id Context ID
         * @return 渲染上下文句柄 (HGLRC)，无效时返回 nullptr
         */
        virtual void *GetRenderContext(ContextID p_context_id) const override;

        virtual void *GetGLProcAddress() const override;

        static void *CombinedGLLoader(const char *name);

        void *GetOpenGLModule() const;

        /**
         * @brief 检查管理器是否已初始化
         * @return true 已初始化，false 未初始化
         */
        virtual bool IsInitialized() const override;

    private:
        struct GLSurface
        {
            HWND hwnd_ = nullptr;
            HDC hdc_ = nullptr;
            VSyncMode vsync_mode_ = VSyncMode::kEnabled;
            IScreen::ScreenID screen_id_ = IScreen::kInvalidScreenId;
            ContextID owner_context_id_ = kInvalidContextId;
            bool is_valid = false;
        };

        struct GLContext
        {
            HGLRC hrc_ = nullptr;
            SurfaceID bound_surface_ = kInvalidSurfaceId;
            bool is_valid = false;
        };

        /**
         * @brief 配置设备上下文的像素格式
         *
         * 设置 OpenGL 渲染所需的像素格式，包括颜色深度、深度缓冲、模板缓冲等。
         *
         * @param p_hdc 设备上下文句柄
         * @return Error::kOK 配置成功
         * @return Error::kFailed 配置失败
         *
         * @par 像素格式属性
         *   - 颜色深度：32位 RGBA
         *   - 深度缓冲：24位
         *   - 模板缓冲：8位
         *   - 双缓冲：启用
         *   - 累积缓冲：禁用
         *
         * @par 注意事项
         *   - 每个设备上下文只能配置一次
         *   - 必须在创建 OpenGL 上下文之前调用
         */
        static Error ConfigurePixelFormat(HDC p_hdc);

        /**
         * @brief 加载 OpenGL 驱动并获取基础 WGL 函数指针
         *
         * 动态加载 opengl32.dll 并获取基础 WGL 函数指针。
         *
         * @return Error::kOK 加载成功
         * @return Error::kFailed 加载失败
         *
         * @par 获取的函数指针
         *   - wglCreateContext：创建 OpenGL 上下文
         *   - wglMakeCurrent：绑定上下文到设备上下文
         *   - wglDeleteContext：删除 OpenGL 上下文
         *   - wglGetProcAddress：获取 OpenGL 扩展函数指针
         */
        Error LoadOpenGLDriver();

        /**
         * @brief 使用临时上下文创建 OpenGL 3.3 Core Profile 上下文
         *
         * 采用两步法创建 OpenGL 3.3 Core Profile 上下文：
         * 1. 创建临时 OpenGL 2.1 上下文
         * 2. 使用 wglCreateContextAttribsARB 创建正式的 3.3 Core Profile 上下文
         *
         * @param p_hdc 设备上下文句柄
         * @return HGLRC 创建的 OpenGL 3.3 Core Profile 上下文句柄
         * @return nullptr 创建失败
         *
         * @par 创建流程
         *   1. 创建临时 OpenGL 2.1 上下文
         *   2. 绑定临时上下文以获取扩展函数
         *   3. 获取 wglCreateContextAttribsARB 函数指针
         *   4. 创建 OpenGL 3.3 Core Profile 上下文
         *   5. 删除临时上下文
         *   6. 绑定正式上下文
         *   7. 加载 WGL 扩展函数
         *
         * @par 上下文属性
         *   - OpenGL 版本：3.3 Core Profile
         *   - 前向兼容：启用
         *   - 不共享资源：nullptr
         */
        HGLRC CreateHGLRCWithTempContext(HDC p_hdc);

        /**
         * @brief 加载 WGL 扩展函数
         *
         * 获取 VSync 相关的 WGL 扩展函数指针，并检测自适应 VSync 支持。
         * 必须在 OpenGL 上下文绑定后调用。
         *
         * @par 加载的扩展函数
         *   - wglSwapIntervalEXT：设置垂直同步间隔
         *   - wglGetSwapIntervalEXT：获取当前垂直同步间隔
         *
         * @par 检测的扩展
         *   - WGL_EXT_swap_control：VSync 控制支持
         *   - WGL_EXT_swap_control_tear：自适应 VSync 支持
         */
        void LoadExtensions();

        /**
         * @brief 获取渲染表面指针
         *
         * @param p_surface 表面 ID
         * @return GLSurface* 表面指针
         * @return nullptr 表面 ID 无效
         */
        GLSurface *GetSurface(SurfaceID p_surface);

        /**
         * @brief 获取渲染表面常量指针
         *
         * @param p_surface 表面 ID
         * @return const GLSurface* 表面常量指针
         * @return nullptr 表面 ID 无效
         */
        const GLSurface *GetSurface(SurfaceID p_surface) const;

        /**
         * @brief 获取 OpenGL 上下文指针
         *
         * @param p_context_id 上下文 ID
         * @return GLContext* 上下文指针
         * @return nullptr 上下文 ID 无效
         */
        GLContext *GetContext(ContextID p_context_id);

        /**
         * @brief 获取 OpenGL 上下文常量指针
         *
         * @param p_context_id 上下文 ID
         * @return const GLContext* 上下文常量指针
         * @return nullptr 上下文 ID 无效
         */
        const GLContext *GetContext(ContextID p_context_id) const;

        LocalVector<GLSurface> surfaces_;
        LocalVector<GLContext> contexts_;

        bool is_initialized_ = false;
        bool extensions_loaded_ = false;
        HMODULE opengl_module_ = nullptr;

        using WglCreateContextFunc = HGLRC(WINAPI *)(HDC);
        using WglMakeCurrentFunc = BOOL(WINAPI *)(HDC, HGLRC);
        using WglDeleteContextFunc = BOOL(WINAPI *)(HGLRC);
        using WglGetProcAddressFunc = PROC(WINAPI *)(LPCSTR);
        using WglCreateContextAttribsARBFunc = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
        using WglSwapIntervalEXTFunc = BOOL(WINAPI *)(int);
        using WglGetSwapIntervalEXTFunc = int(WINAPI *)();

        WglCreateContextFunc wgl_create_context_ = nullptr;
        WglMakeCurrentFunc wgl_make_current_ = nullptr;
        WglDeleteContextFunc wgl_delete_context_ = nullptr;
        WglGetProcAddressFunc wgl_get_proc_address_ = nullptr;
        WglCreateContextAttribsARBFunc wgl_create_context_attribs_arb_ = nullptr;
        WglSwapIntervalEXTFunc wgl_swap_interval_ext_ = nullptr;
        WglGetSwapIntervalEXTFunc wgl_get_swap_interval_ext_ = nullptr;

        bool swap_control_tear_supported_ = false;
    };

} // namespace arhud

#endif // ARHUD_PLATFORM_WINDOWS