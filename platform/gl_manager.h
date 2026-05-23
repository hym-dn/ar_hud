/**
 * @file gl_manager.h
 * @brief 平台无关 OpenGL 上下文管理抽象接口
 *
 * @par 概述
 *   IGLManager 将 GL 上下文（Context）和渲染表面（Surface）分离，
 *   所有 Context 地位平等，无主从区分。通过 Context/Surface 的
 *   灵活组合，统一支持单线程多 Surface 和多线程多 Context 两种模式。
 *
 * @par 架构层次
 *   @code
 *   IDisplayServer ──> IScreen ──> IGLManager ──> RenderingContextDriver
 *                      (屏幕)       (上下文管理)     (GPU API 抽象)
 *   @endcode
 *
 * @par 统一 Context 模型
 *   所有 Context 地位平等，不区分"主上下文"和"独立上下文"：
 *   - 每个 Context 持有独立的 GL 状态和资源（HGLRC）
 *   - Context 之间不共享资源（不调用 wglShareLists）
 *   - 第一个创建的 Context（ContextID=0）无特殊地位
 *   - 使用模式由调用者决定，而非 API 强制区分
 *
 * @code
 * 模式 1：单线程 + 多 Surface（1 Context + N Surface）
 * ┌─────────────────────────────────────────────────┐
 * │  Context 0 (HGLRC)                              │
 * │  ├── Surface 0 (HDC_0) ← HUD 屏幕              │
 * │  ├── Surface 1 (HDC_1) ← 仪表盘屏幕            │
 * │  └── Surface 2 (HDC_2) ← 后视镜屏幕            │
 * │                                                 │
 * │  MakeCurrent(ctx0, surf0) → Render → Swap       │
 * │  MakeCurrent(ctx0, surf1) → Render → Swap       │
 * │  MakeCurrent(ctx0, surf2) → Render → Swap       │
 * └─────────────────────────────────────────────────┘
 *
 * 模式 2：多线程 + 多 Context（N Context + N Surface）
 * ┌──────────────────────┐  ┌──────────────────────┐
 * │ Thread 0             │  │ Thread 1             │
 * │ Context 0 (HGLRC_0)  │  │ Context 1 (HGLRC_1)  │
 * │ └── Surface 0 (HDC)  │  │ └── Surface 1 (HDC)  │
 * │                      │  │                      │
 * │ 资源不共享，         │  │ 资源不共享，         │
 * │ 各线程独立创建       │  │ 各线程独立创建       │
 * └──────────────────────┘  └──────────────────────┘
 * @endcode
 *
 * @par Context 与 Surface 的关系
 *   - **Context（上下文）**：持有 GL 状态和资源（HGLRC / EGLContext），
 *     同一时刻只能绑定到一个线程
 *   - **Surface（表面）**：持有可渲染的窗口区域（HDC + HWND），
 *     可绑定到任意 Context
 *   - **MakeCurrent(ContextID, SurfaceID)**：将指定上下文绑定到指定表面，
 *     等价于 wglMakeCurrent(surfaces_[surf].hdc, contexts_[ctx].hrc)
 *   - **Surface 归属**：Surface 首次被某 Context 绑定后，记录归属关系，
 *     其他 Context 不可再绑定该 Surface
 *
 * @par 生命周期
 *   1. 创建平台实现（如 Win32GLManager）
 *   2. Initialize() — 加载 OpenGL 驱动
 *   3. CreateSurface() — 创建第一个渲染表面
 *   4. CreateContext() — 基于第一个 Surface 创建 GL 上下文
 *   5. CreateSurface() × N — 可选，为 N 个屏幕创建更多渲染表面
 *   6. MakeCurrent() / SwapBuffers() — 渲染
 *   7. CreateSurface() + CreateContext() × N — 可选，为多线程创建更多上下文
 *   8. DestroySurface() / DestroyContext() — 销毁资源
 *   9. Shutdown() — 释放所有资源
 *
 * @par 设计参考
 *   - Godot 4.6 GLManager（统一上下文模型）
 *   - WGL_ARB_create_context 扩展规范
 *   - arch_skill.md §4.4 多线程渲染
 *
 * @author yameng.he
 * @version 4.0
 * @date 2026-05-16
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_os
 */

#pragma once

#include "typedefs.h"
#include "screen.h"
#include "window.h"

namespace arhud
{

    /**
     * @brief 平台无关 OpenGL 上下文管理接口
     *
     * 管理 OpenGL 渲染上下文和渲染表面的创建、切换和 VSync 控制。
     * 采用 Context/Surface 分离模型，所有 Context 地位平等。
     *
     * @par 线程安全
     *   - CreateContext / CreateSurface / DestroySurface /
     *     DestroyContext / Shutdown 应在主线程调用（初始化/清理阶段）
     *   - MakeCurrent / SwapBuffers / SetVsyncMode 可在任意线程调用
     *   - 每个 Context 应只在其对应的渲染线程上使用
     *
     * @see IDisplayServer  窗口管理器（持有 IGLManager 实例）
     * @see IScreen         屏幕接口（提供 ScreenID）
     * @see IWindow         窗口接口（提供原生句柄给 IGLManager）
     */
    class IGLManager
    {
    public:
        /** @brief GL 上下文 ID 类型 */
        using ContextID = uint32_t;

        /** @brief 渲染表面 ID 类型 */
        using SurfaceID = uint32_t;

        /** @brief 无效上下文 ID */
        static constexpr ContextID kInvalidContextId = UINT32_MAX;

        /** @brief 无效表面 ID */
        static constexpr SurfaceID kInvalidSurfaceId = UINT32_MAX;

        virtual ~IGLManager() = default;

        // ─── 生命周期 ─────────────────────────────────────────────────────

        virtual Error Initialize() = 0;
        virtual void Shutdown() = 0;

        // ─── 上下文管理 ──────────────────────────────────────────────────

        /**
         * @brief 创建 GL 渲染上下文
         *
         * 基于已有 Surface 的 HDC 创建独立的 GL 3.3 Core Profile 上下文。
         * 首次调用时会加载 WGL 扩展（内部实现细节）。
         * 所有 Context 地位平等，不共享资源。
         *
         * @param[in] p_surface 已创建的渲染表面 ID（提供 HDC 和像素格式）
         *
         * @return 新创建的 Context ID
         * @retval kInvalidContextId 创建失败
         *
         * @pre Initialize() 已成功调用
         * @pre p_surface 对应的 Surface 已通过 CreateSurface() 创建
         *
         * @par 调用顺序
         *   必须先创建 Surface，再基于 Surface 创建 Context。
         *   CreateContext 复用 Surface 的 HDC（已配置像素格式），
         *   避免跨线程 GetDC/ReleaseDC 操作。
         *
         * @par 单线程模式
         *   @code
         *   SurfaceID surf = gl_mgr->CreateSurface(screen_id, hwnd, w, h);
         *   ContextID ctx = gl_mgr->CreateContext(surf);
         *   gl_mgr->MakeCurrent(ctx, surf);
         *   @endcode
         *
         * @par 多线程模式
         *   @code
         *   SurfaceID surf0 = gl_mgr->CreateSurface(screen0, hwnd0, w, h);
         *   SurfaceID surf1 = gl_mgr->CreateSurface(screen1, hwnd1, w, h);
         *   ContextID ctx0 = gl_mgr->CreateContext(surf0);
         *   ContextID ctx1 = gl_mgr->CreateContext(surf1);
         *   // Thread 0: gl_mgr->MakeCurrent(ctx0, surf0)
         *   // Thread 1: gl_mgr->MakeCurrent(ctx1, surf1)
         *   @endcode
         */
        virtual ContextID CreateContext(SurfaceID p_surface) = 0;

        /**
         * @brief 销毁 GL 渲染上下文
         *
         * 释放上下文的所有资源，包括 HGLRC。
         * 销毁前应确保该上下文不在任何线程上绑定。
         *
         * @param[in] p_context_id Context ID
         *
         * @pre p_context_id 有效
         * @post 上下文已销毁，ID 失效
         */
        virtual void DestroyContext(ContextID p_context_id) = 0;

        /**
         * @brief 查询上下文数量
         * @return 当前存活的上下文数量
         */
        virtual uint32_t GetContextCount() const = 0;

        // ─── 渲染表面 ─────────────────────────────────────────────────────

        /**
         * @brief 创建渲染表面
         *
         * @param[in] p_screen_id 关联的屏幕 ID
         * @param[in] p_native_window 原生窗口句柄（Win32: HWND）
         * @param[in] p_width 窗口客户区宽度（像素）
         * @param[in] p_height 窗口客户区高度（像素）
         *
         * @return 新创建的 Surface ID
         * @retval kInvalidSurfaceId 创建失败
         */
        virtual SurfaceID CreateSurface(IScreen::ScreenID p_screen_id,
                                        void *p_native_window,
                                        uint32_t p_width, uint32_t p_height) = 0;

        /**
         * @brief 销毁渲染表面
         *
         * @param[in] p_surface Surface ID
         */
        virtual void DestroySurface(SurfaceID p_surface) = 0;

        // ─── 上下文绑定 ───────────────────────────────────────────────────

        /**
         * @brief 将指定上下文绑定到指定表面的调用线程
         *
         * @param[in] p_context_id Context ID
         * @param[in] p_surface Surface ID
         *
         * @return Error::kOK 绑定成功
         * @return Error::kInvalidParameter Context ID 或 Surface ID 无效
         * @return Error::kFailed 平台绑定失败
         *
         * @par 实现细节（Win32）
         *   wglMakeCurrent(surfaces_[p_surface].hdc, contexts_[p_context_id].hrc)
         *
         * @par Surface 归属
         *   Surface 首次被某 Context 绑定后，记录归属关系。
         *   其他 Context 尝试绑定该 Surface 时会返回错误。
         *   归属关系在 DestroyContext 时自动清除。
         */
        virtual Error MakeCurrent(ContextID p_context_id,
                                   SurfaceID p_surface) = 0;

        /**
         * @brief 释放当前线程的 GL 上下文
         */
        virtual void ReleaseCurrent() = 0;

        // ─── 渲染 ─────────────────────────────────────────────────────────

        virtual void SwapBuffers(SurfaceID p_surface) = 0;

        // ─── VSync 控制 ───────────────────────────────────────────────────

        virtual void SetVsyncMode(SurfaceID p_surface, VSyncMode p_mode) = 0;
        virtual VSyncMode GetVsyncMode(SurfaceID p_surface) const = 0;

        // ─── 原生句柄 ─────────────────────────────────────────────────────

        virtual void *GetDeviceContext(SurfaceID p_surface) const = 0;
        virtual void *GetRenderContext(ContextID p_context_id) const = 0;

        /**
         * @brief 获取 OpenGL 函数指针加载函数
         *
         * 返回一个组合 loader 函数指针，该函数先尝试 wglGetProcAddress 加载扩展函数，
         * 失败后回退到 GetProcAddress(opengl32.dll) 加载核心函数。
         * 用于 glad 2.x 的 gladLoadGL() 调用。
         *
         * @return 组合 loader 函数指针（签名: void* (*)(const char*)）
         * @retval nullptr 未初始化或加载失败
         *
         * @pre Initialize() 已成功调用
         * @pre 调用 gladLoadGL 前必须有有效的 GL 上下文绑定到当前线程
         */
        virtual void *GetGLProcAddress() const = 0;

        // ─── 状态查询 ─────────────────────────────────────────────────────

        virtual bool IsInitialized() const = 0;
    };

} // namespace arhud
