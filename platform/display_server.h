/**
 * @file display_server.h
 * @brief 平台无关显示服务器抽象接口
 *
 * 显示服务器是窗口管理系统的核心，负责屏幕枚举、窗口创建/销毁、
 * 视图管理和全局事件处理。参考 Godot 4.6 DisplayServer 设计。
 *
 * @par 架构定位：
 *   IDisplayServer 是平台抽象层（platform/）的核心接口，
 *   统一管理 IScreen（屏幕）、IWindow（窗口）、IView（视图）三层抽象。
 *   应用层通过 IDisplayServer 单例访问所有显示相关功能。
 *
 * @par 架构层次：
 *   IDisplayServer（本接口）
 *     ├── IScreen    — 屏幕管理（多显示器支持）
 *     ├── IWindow    — 窗口管理（多窗口支持）
 *     └── IView      — 视图管理（HUD 分层渲染）
 *
 * @par 设计参考：
 *   - Godot 4.6 DisplayServer（display_server_windows.cpp）
 *   - Godot SubViewportContainer + SubViewport（视图层级）
 *   - SDL2 DisplayServer + Window 管理模式
 *   - arch_skill.md §2.2.2 显示服务器设计
 *
 * @par 生命周期：
 *   1. 创建平台实现（如 Win32DisplayServer）
 *   2. Initialize() — 枚举屏幕、初始化子系统
 *   3. WindowCreate() / ViewCreate() — 创建窗口和视图
 *   4. ProcessEvents() — 每帧事件轮询
 *   5. Shutdown() — 释放所有资源
 *
 * @par 单例模式：
 *   IDisplayServer 使用单例模式，全局唯一实例。
 *   通过 GetSingleton() 获取，Initialize() 前返回 nullptr。
 *
 * @par 使用示例：
 *   @code{.cpp}
 *   // 获取显示服务器单例
 *   auto* ds = IDisplayServer::GetSingleton();
 *
 *   // 初始化
 *   if (ds->Initialize() != Error::kOK) {
 *       return;
 *   }
 *
 *   // 枚举屏幕
 *   printf("Screens: %u\n", ds->GetScreenCount());
 *   for (uint32_t i = 0; i < ds->GetScreenCount(); ++i) {
 *       auto* screen = ds->GetScreen(i);
 *       printf("  Screen %u: %ux%u\n", screen->GetId(),
 *              screen->GetWidth(), screen->GetHeight());
 *   }
 *
 *   // 创建窗口
 *   WindowDesc desc;
 *   desc.width = 1280;
 *   desc.height = 720;
 *   desc.title = "AR HUD";
 *   auto window_id = ds->WindowCreate(desc);
 *
 *   // 创建视图
 *   ViewConfig view_config;
 *   view_config.type = ViewType::kOverlay2D;
 *   view_config.width = 1280;
 *   view_config.height = 720;
 *   view_config.z_order = 0.0f;
 *   auto view_id = ds->ViewCreate(window_id, view_config);
 *
 *   // 事件循环
 *   while (!ds->GetWindow(window_id)->ShouldClose()) {
 *       ds->ProcessEvents();
 *       // 渲染
 *   }
 *
 *   ds->Shutdown();
 *   @endcode
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include "typedefs.h"
#include "screen.h"
#include "view.h"
#include "window.h"

namespace arhud
{

    /**
     * @brief 平台无关显示服务器接口
     *
     * 统一管理窗口系统所有功能的核心接口。
     * 提供屏幕枚举、窗口生命周期管理、视图管理和全局事件分发。
     *
     * @par 关键职责：
     *   - 显示服务器初始化和关闭
     *   - 屏幕枚举和多显示器支持
     *   - 窗口创建、销毁和查询
     *   - 视图创建、销毁和 Z-Order 管理
     *   - 全局事件轮询和分发
     *   - VSync 模式控制
     *
     * @par 线程安全：
     *   非线程安全。所有方法应在主线程调用。
     *   如需多线程窗口管理，应使用外部同步。
     *
     * @par 平台实现：
     *   - Win32: Win32DisplayServer（已实现）
     *   - X11:   X11DisplayServer（未来支持）
     *   - Android: AndroidDisplayServer（未来支持）
     *
     * @see IScreen    屏幕接口
     * @see IWindow    窗口接口
     * @see IView      视图接口
     * @see Win32DisplayServer Win32 平台实现
     */
    class IDisplayServer
    {
    public:
        /** @brief 屏幕 ID 类型（同 IScreen::ScreenID） */
        using ScreenID = IScreen::ScreenID;

        /** @brief 窗口 ID 类型（同 IWindow::WindowID） */
        using WindowID = IWindow::WindowID;

        /** @brief 视图 ID 类型（同 IView::ViewID） */
        using ViewID = IView::ViewID;

        /** @brief 虚析构，确保子类正确清理 */
        virtual ~IDisplayServer() = default;

        /**
         * @brief 获取全局单例
         *
         * 返回显示服务器的全局唯一实例。
         *
         * @return DisplayServer 实例指针
         * @retval nullptr 尚未创建或已销毁
         *
         * @par 线程安全：
         *   返回的指针在 Initialize()/Shutdown() 期间可能无效。
         *   应在 Initialize() 成功后使用。
         */
        static IDisplayServer *GetSingleton();

        // ─── 生命周期 ─────────────────────────────────────────────────────

        /**
         * @brief 初始化显示服务器
         *
         * 枚举显示器、初始化平台子系统、注册全局回调。
         * 必须在所有其他操作前调用。
         *
         * @return Error 错误码
         * @retval Error::kOK          初始化成功
         * @retval Error::kFailed      平台 API 初始化失败
         *
         * @pre 尚未初始化（重复调用返回 Error::kFailed）
         * @post GetSingleton() 返回有效指针
         * @post 可调用 WindowCreate() / GetScreenCount() 等
         *
         * @note 如果已有窗口/视图存在，会在 Shutdown() 时一并销毁
         */
        virtual Error Initialize() = 0;

        /**
         * @brief 关闭显示服务器并释放所有资源
         *
         * 销毁所有窗口和视图，释放屏幕资源。
         * 调用后 GetSingleton() 返回 nullptr。
         *
         * @post 所有窗口句柄无效
         * @post GetSingleton() 返回 nullptr
         *
         * @note 会先销毁所有视图，再销毁所有窗口，最后释放屏幕
         */
        virtual void Shutdown() = 0;

        // ─── 屏幕管理 ─────────────────────────────────────────────────────

        /**
         * @brief 获取屏幕数量
         *
         * @return 当前连接的显示器数量
         *
         * @par AR HUD 场景：
         *   典型配置：1-3 个屏幕（仪表盘、抬头显示、乘客娱乐）
         */
        virtual uint32_t GetScreenCount() const = 0;

        /**
         * @brief 获取屏幕对象
         *
         * @param[in] id 屏幕 ID
         *
         * @return 屏幕指针
         * @retval nullptr 屏幕不存在（ID 无效）
         *
         * @par 生命周期：
         *   返回的指针在 Shutdown() 前始终有效。
         *   不要 delete 返回的指针，由 DisplayServer 管理。
         */
        virtual IScreen *GetScreen(ScreenID id) const = 0;

        /**
         * @brief 获取主屏 ID
         *
         * @return 主屏 ID，没有屏幕时返回 IScreen::kInvalidScreenId
         *
         * @see IScreen::is_primary
         */
        virtual ScreenID GetPrimaryScreen() const = 0;

        // ─── 窗口管理 ─────────────────────────────────────────────────────

        /**
         * @brief 创建窗口
         *
         * 在指定屏幕上创建新窗口。
         *
         * @param[in] desc 窗口创建参数
         *
         * @return 窗口 ID
         * @retval IWindow::kInvalidWindowId 创建失败
         *
         * @retval Error::kOK          创建成功
         * @retval Error::kFailed       创建失败（平台 API 错误）
         * @retval Error::kInvalidParameter 参数校验失败
         *
         * @par 窗口与屏幕：
         *   窗口可以不属于任何特定屏幕（screen_id = UINT32_MAX），
         *   此时窗口使用系统默认屏幕。
         *
         * @see IWindow::Initialize()
         */
        virtual WindowID WindowCreate(const WindowDesc &desc) = 0;

        /**
         * @brief 销毁窗口
         *
         * 销毁指定窗口及其所有视图。
         * 销毁后窗口 ID 失效，视图 ID 全部失效。
         *
         * @param[in] id 窗口 ID
         *
         * @note 销毁窗口会级联销毁所有关联视图
         */
        virtual void WindowDestroy(WindowID id) = 0;

        /**
         * @brief 获取窗口对象
         *
         * @param[in] id 窗口 ID
         *
         * @return 窗口指针
         * @retval nullptr 窗口不存在
         *
         * @par 生命周期：
         *   返回的指针在 WindowDestroy() 或 Shutdown() 前有效。
         */
        virtual IWindow *GetWindow(WindowID id) const = 0;

        /**
         * @brief 获取窗口数量
         *
         * @return 当前存在的窗口数量
         */
        virtual uint32_t GetWindowCount() const = 0;

        // ─── 视图管理 ─────────────────────────────────────────────────────

        /**
         * @brief 创建视图
         *
         * 在指定窗口中创建新视图。
         * 视图代表窗口内的渲染区域，支持 Z-Order 分层。
         *
         * @param[in] window 目标窗口 ID
         * @param[in] config 视图配置参数
         *
         * @return 视图 ID
         * @retval IView::kInvalidViewId 创建失败
         *
         * @par Z-Order：
         *   视图按 z_order 从小到大排序，值越小越在底层。
         *   典型分层：
         *   - z_order = 0.0f: 3D 场景层（背景）
         *   - z_order = 1.0f: 导航层
         *   - z_order = 2.0f: 仪表盘层
         *   - z_order = 3.0f: 警告层（最高优先级）
         *
         * @par 视图类型（ViewType）：
         *   - kOverlay2D: 2D HUD 叠加（仪表盘、导航箭头）
         *   - kScene3D:   3D 场景（ADAS 可视化）
         *   - kStereoLeft/kStereoRight: AR 立体渲染
         *
         * @see ViewConfig
         * @see ViewType
         */
        virtual ViewID ViewCreate(WindowID window, const ViewConfig &config) = 0;

        /**
         * @brief 销毁视图
         *
         * @param[in] id 视图 ID
         *
         * @note 销毁后视图 ID 失效
         */
        virtual void ViewDestroy(ViewID id) = 0;

        /**
         * @brief 获取视图对象
         *
         * @param[in] id 视图 ID
         *
         * @return 视图指针
         * @retval nullptr 视图不存在
         */
        virtual IView *GetView(ViewID id) const = 0;

        // ─── 事件处理 ─────────────────────────────────────────────────────

        /**
         * @brief 处理所有待处理事件
         *
         * 从系统事件队列中取出并分发事件到对应窗口。
         * 应在主循环中每帧调用一次。
         *
         * @par 调用频率：
         *   推荐每帧调用一次。
         *   高频调用浪费 CPU，低频调用导致事件响应延迟。
         *
         * @par 内部实现：
         *   Win32: PeekMessage + DispatchMessage
         *   X11:   XPending + XNextEvent（未来支持）
         */
        virtual void ProcessEvents() = 0;

        // ─── VSync 控制 ─────────────────────────────────────────────────────

        /**
         * @brief 设置窗口 VSync 模式
         *
         * @param[in] window 目标窗口 ID
         * @param[in] mode   垂直同步模式
         *
         * @see VSyncMode
         */
        virtual void SetVsyncMode(WindowID window, VSyncMode mode) = 0;

        /**
         * @brief 获取窗口 VSync 模式
         *
         * @param[in] window 目标窗口 ID
         *
         * @return 当前 VSync 模式，默认 kEnabled
         *
         * @see VSyncMode
         */
        virtual VSyncMode GetVsyncMode(WindowID window) const = 0;

    protected:
        /** @brief 全局单例指针（子类在构造时设置，析构时置空） */
        static IDisplayServer *singleton_;
    };

} // namespace arhud
