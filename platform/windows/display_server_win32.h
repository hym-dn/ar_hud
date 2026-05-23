/**
 * @file display_server_win32.h
 * @brief Win32 平台显示服务器实现声明
 *
 * @par 概述
 *   使用 Win32 多显示器 API 实现显示服务器功能。
 *   继承 IDisplayServer 接口，提供 Windows 平台特定的显示管理。
 *
 * @par 架构设计
 *   Win32DisplayServer 是整个窗口系统的核心，协调以下组件：
 *   - Win32Screen：屏幕管理（多显示器支持）
 *   - Win32Window：窗口管理（多窗口支持）
 *   - Win32View：视图管理（2D HUD 分层渲染）
 *
 * @par 核心职责
 *   1. 显示器枚举（Display Enumeration）
 *      - 系统启动时通过 EnumDisplayMonitors 枚举所有显示器
 *      - 为每个显示器创建 Win32Screen 实例
 *      - 维护 screens_ 列表
 *
 *   2. 窗口生命周期管理（Window Lifecycle）
 *      - 创建：分配 WindowID，创建 Win32Window，添加到 windows_ 映射
 *      - 销毁：移除窗口，释放 Win32Window，回收 WindowID
 *      - 查询：通过窗口 ID 快速查找窗口
 *
 *   3. 视图管理（View Management）
 *      - 创建：为窗口创建视图，分配 ViewID
 *      - 销毁：移除视图，释放 ViewID
 *      - Z-order：视图按 Z 顺序排列，数值越大越在前
 *
 *   4. 事件处理（Event Processing）
 *      - 主循环调用 ProcessEvents() 处理所有窗口消息
 *      - 将消息路由到对应窗口的回调
 *
 * @par 消息循环
 *   Win32DisplayServer 不直接运行消息循环，而是提供 ProcessEvents() 方法：
 *   - 由上层应用的主循环调用
 *   - 内部调用每个窗口的 PollEvents()
 *   - PeekMessage 检查并分发消息
 *
 * @par 资源管理
 *   - 屏幕：在 Initialize() 时枚举，之后保持静态
 *   - 窗口：通过 WindowCreate/WindowDestroy 动态管理
 *   - 视图：通过 ViewCreate/ViewDestroy 动态管理
 *   - Shutdown() 时销毁所有窗口和视图
 *
 * @par 线程安全
 *   非线程安全。所有方法应在主线程调用。
 *   Win32 窗口消息只在创建窗口的线程分发。
 *
 * @par 单例模式
 *   Win32DisplayServer 通过 IDisplayServer::GetSingleton() 获取单例。
 *   单例在第一次调用时创建，在 Shutdown() 时销毁。
 *
 * @note 仅在 ARHUD_PLATFORM_WINDOWS 定义时可用
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

#include "display_server.h"
#include "screen_win32.h"
#include "view.h"
#include "window.h"
#include "view_win32.h"
#include "template/hash_map.h"
#include "template/local_vector.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace arhud
{

    /**
     * @struct WindowContext
     * @brief 窗口上下文结构
     *
     * @par 概述
     *   存储与窗口关联的所有上下文信息。
     *   用于在 windows_ HashMap 中快速查找窗口相关数据。
     *
     * @par 包含内容
     *   - window_id：窗口唯一标识符
     *   - screen_id：窗口所在屏幕 ID
     *   - window：窗口对象指针
     *   - views：窗口关联的所有视图列表
     *   - vsync_mode：垂直同步模式
     *
     * @par 内存管理
     *   - window 指针由 Win32DisplayServer 创建和拥有
     *   - views 列表中的指针由 DisplayServer 创建，WindowContext 不负责销毁
     *
     * @par 线程安全
     *   非线程安全。应在主线程访问。
     */
    struct WindowContext
    {
        /** @brief 窗口唯一标识符 */
        IWindow::WindowID window_id = IWindow::kInvalidWindowId;

        /** @brief 窗口所在屏幕的 ID */
        uint32_t screen_id = IScreen::kInvalidScreenId;

        /** @brief 窗口对象指针 */
        IWindow *window = nullptr;

        /** @brief 窗口关联的视图列表（按 Z 顺序排序） */
        LocalVector<IView *> views;

        /** @brief 垂直同步模式 */
        VSyncMode vsync_mode = VSyncMode::kEnabled;
    };

    /**
     * @class Win32DisplayServer
     * @brief Win32 平台显示服务器实现
     *
     * @par 继承层次
     *   IDisplayServer（抽象接口）
     *     └── Win32DisplayServer（具体实现）
     *
     * @par 单例模式
     *   Win32DisplayServer 实现为单例：
     *   - 通过 IDisplayServer::GetSingleton() 访问
     *   - 第一次调用时创建实例
     *   - Shutdown() 时销毁实例
     *
     * @par 生命周期
     *   1. GetSingleton()：获取或创建单例
     *   2. Initialize()：枚举屏幕、初始化内部状态
     *   3. 使用：窗口管理、视图管理、事件处理
     *   4. Shutdown()：销毁所有窗口、视图、屏幕
     *
     * @par 窗口管理
     *   - WindowCreate()：创建新窗口
     *     1. 分配新的 WindowID
     *     2. 创建 Win32Window 实例
     *     3. 调用 Initialize()
     *     4. 添加到 windows_ HashMap
     *
     *   - WindowDestroy()：销毁窗口
     *     1. 从 windows_ 移除
     *     2. 调用 Shutdown()
     *     3. 删除 Win32Window 实例
     *
     * @par 视图管理
     *   - ViewCreate()：为窗口创建视图
     *     1. 分配新的 ViewID
     *     2. 创建 Win32View 实例
     *     3. 添加到窗口的 views_ 列表
     *
     *   - ViewDestroy()：销毁视图
     *     1. 从窗口的 views_ 列表移除
     *     2. 删除 Win32View 实例
     *
     * @par 屏幕枚举
     *   Initialize() 时调用 EnumerateScreens()：
     *   1. 使用 EnumDisplayMonitors 枚举所有显示器
     *   2. 为每个显示器调用 MonitorEnumProc 回调
     *   3. 创建 Win32Screen 并添加到 screens_ 列表
     *
     * @par 事件分发
     *   ProcessEvents() 分发事件到对应窗口：
     *   1. 遍历 windows_ 中的所有窗口
     *   2. 调用窗口的 PollEvents()
     *   3. 窗口内部处理消息并触发回调
     *
     * @par VSync 控制
     *   - SetVsyncMode()：设置窗口的 VSync 模式
     *   - GetVsyncMode()：获取窗口的 VSync 模式
     *
     *   VSync 模式：
     *   - kDisabled：完全关闭 VSync
     *   - kEnabled：开启 VSync（默认）
     *   - kAdaptive：适应性 VSync（仅在帧率高于刷新率时同步）
     *
     * @par ID 分配策略
     *   - WindowID：从 1 开始递增
     *   - ViewID：从 1 开始递增
     *   - kInvalidWindowId 和 kInvalidViewId 表示无效 ID
     *   - ID 不回收，理论上可支持 2^32-1 个对象
     *
     * @par 线程安全
     *   非线程安全。所有方法应在主线程调用。
     *
     * @par 内存管理
     *   - 使用 ARHUD_DISABLE_COPY_MOVE 禁止拷贝和移动
     *   - 窗口和视图通过 ARHUD_NEW 创建，通过 ARHUD_DELETE 销毁
     *   - screens_ 和 windows_ 使用容器管理
     *
     * @see IDisplayServer
     * @see Win32Window
     * @see Win32Screen
     * @see Win32View
     */
    class Win32DisplayServer : public IDisplayServer
    {
    public:
        /**
         * @brief 默认构造函数
         *
         * @par 行为
         *   创建 Win32DisplayServer 实例。
         *   此时还未初始化，需要调用 Initialize() 完成初始化。
         */
        Win32DisplayServer();

        /**
         * @brief 析构函数
         *
         * @par 行为
         *   确保已调用 Shutdown()，安全释放所有资源。
         *   如果尚未关闭，自动调用 Shutdown()。
         */
        virtual ~Win32DisplayServer() override;

        ARHUD_DISABLE_COPY_MOVE(Win32DisplayServer);

        /**
         * @brief 初始化显示服务器
         *
         * @par 初始化步骤
         *   1. 调用 EnumerateScreens() 枚举所有显示器
         *   2. 初始化 ID 计数器（next_window_id_ = 1, next_view_id_ = 1）
         *   3. 设置 is_initialized_ = true
         *
         * @par 屏幕枚举
         *   使用 EnumDisplayMonitors 枚举：
         *   - 为每个显示器创建 Win32Screen
         *   - 分配唯一的屏幕 ID
         *
         * @return kNoError 初始化成功
         * @return kAlreadyInitialized 已初始化，不能重复初始化
         * @return kOutOfMemory 内存分配失败
         *
         * @note 初始化失败时应调用 Shutdown() 清理
         */
        virtual Error Initialize() override;

        /**
         * @brief 关闭显示服务器并释放资源
         *
         * @par 关闭步骤
         *   1. DestroyAllWindowsContexts() 销毁所有窗口上下文
         *   2. DestroyAllViews() 销毁所有视图
         *   3. DestroyAllScreens() 销毁所有屏幕
         *   4. 重置 is_initialized_ = false
         *
         * @par 销毁顺序
         *   窗口必须在视图之前销毁（窗口持有视图引用）。
         *   视图必须在窗口之前从列表移除。
         *
         * @note 即使未初始化，调用 Shutdown() 也是安全的（无操作）
         */
        virtual void Shutdown() override;

        /**
         * @brief 获取屏幕数量
         *
         * @return uint32_t 当前连接的显示器数量
         *
         * @par 多显示器
         *   返回系统中所有显示器的数量，包括：
         *   - 主显示器
         *   - 扩展显示器
         *   - 投影仪（如果系统识别为显示器）
         */
        virtual uint32_t GetScreenCount() const override;

        /**
         * @brief 获取指定 ID 的屏幕
         *
         * @param[in] id 屏幕 ID
         *
         * @return IScreen* 屏幕指针，不存在返回 nullptr
         *
         * @par 屏幕查找
         *   在 screens_ 列表中查找对应 ID 的屏幕。
         *   O(n) 复杂度。
         *
         * @par 生命周期
         *   返回的指针由 Win32DisplayServer 拥有，不要删除。
         */
        virtual IScreen *GetScreen(ScreenID id) const override;

        /**
         * @brief 获取主屏幕 ID
         *
         * @return ScreenID 主显示器的 ID
         *
         * @par 主显示器
         *   返回桌面扩展模式下主显示器的 ID。
         *   主显示器是任务栏和桌面图标所在的显示器。
         *
         * @par 实现
         *   遍历 screens_，查找 is_primary == true 的屏幕。
         */
        virtual ScreenID GetPrimaryScreen() const override;

        /**
         * @brief 创建新窗口
         *
         * @param[in] desc 窗口描述配置
         *
         * @return WindowID 新窗口的 ID，失败返回 kInvalidWindowId
         *
         * @par 创建步骤
         *   1. 分配新的 WindowID（next_window_id_++）
         *   2. 创建 Win32Window 实例
         *   3. 调用 window->Initialize(desc)
         *   4. 创建 WindowContext 并添加到 windows_ HashMap
         *   5. 返回 WindowID
         *
         * @par 失败处理
         *   如果初始化失败：
         *   1. 从 windows_ 移除
         *   2. 删除 Win32Window 实例
         *   3. 返回 kInvalidWindowId
         *
         * @par 屏幕关联
         *   根据窗口位置自动关联到对应屏幕。
         *   使用 MonitorFromWindow 或 MonitorFromPoint 确定屏幕。
         *
         * @par 示例
         *   @code
         *   WindowDesc desc = {};
         *   desc.title = "AR HUD";
         *   desc.width = 1280;
         *   desc.height = 720;
         *   desc.mode = WindowMode::kWindowed;
         *   WindowID id = display_server->WindowCreate(desc);
         *   @endcode
         */
        virtual WindowID WindowCreate(const WindowDesc &desc) override;

        /**
         * @brief 销毁指定窗口
         *
         * @param[in] id 要销毁的窗口 ID
         *
         * @par 销毁步骤
         *   1. 从 windows_ HashMap 查找 WindowContext
         *   2. 销毁窗口关联的所有视图
         *   3. 调用 window->Shutdown()
         *   4. 删除 Win32Window 实例
         *   5. 从 windows_ 移除
         *
         * @par 安全检查
         *   如果 id 无效，函数无操作。
         *
         * @warning 销毁后不要再访问该窗口的指针
         */
        virtual void WindowDestroy(WindowID id) override;

        /**
         * @brief 获取指定 ID 的窗口
         *
         * @param[in] id 窗口 ID
         *
         * @return IWindow* 窗口指针，不存在返回 nullptr
         *
         * @par 窗口查找
         *   在 windows_ HashMap 中查找。
         *   O(1) 复杂度（HashMap 查找）。
         *
         * @par 生命周期
         *   返回的指针由 Win32DisplayServer 拥有，不要删除。
         */
        virtual IWindow *GetWindow(WindowID id) const override;

        /**
         * @brief 获取当前窗口数量
         *
         * @return uint32_t 当前窗口数量
         */
        virtual uint32_t GetWindowCount() const override;

        /**
         * @brief 为窗口创建视图
         *
         * @param[in] window 窗口 ID
         * @param[in] config 视图配置
         *
         * @return ViewID 新视图的 ID，失败返回 kInvalidViewId
         *
         * @par 创建步骤
         *   1. 分配新的 ViewID（next_view_id_++）
         *   2. 创建 Win32View 实例
         *   3. 添加到窗口的 views_ 列表
         *   4. 按 Z 顺序排序
         *   5. 返回 ViewID
         *
         * @par 视图排序
         *   新视图添加后按 Z 值排序。
         *   渲染时按 Z 值从低到高绘制。
         *
         * @par 失败处理
         *   如果窗口不存在，返回 kInvalidViewId。
         *
         * @par 示例
         *   @code
         *   ViewConfig config = {};
         *   config.type = ViewType::kOverlay;
         *   config.x = 0;
         *   config.y = 0;
         *   config.width = 1280;
         *   config.height = 720;
         *   config.z_order = 0.5f;
         *   ViewID id = display_server->ViewCreate(window_id, config);
         *   @endcode
         */
        virtual ViewID ViewCreate(WindowID window, const ViewConfig &config) override;

        /**
         * @brief 销毁指定视图
         *
         * @param[in] id 要销毁的视图 ID
         *
         * @par 销毁步骤
         *   1. 在 views_ HashMap 中查找 Win32View
         *   2. 获取视图所属窗口
         *   3. 从窗口的 views_ 列表移除
         *   4. 删除 Win32View 实例
         *   5. 从 views_ 移除
         *
         * @par 安全检查
         *   如果 id 无效，函数无操作。
         */
        virtual void ViewDestroy(ViewID id) override;

        /**
         * @brief 获取指定 ID 的视图
         *
         * @param[in] id 视图 ID
         *
         * @return IView* 视图指针，不存在返回 nullptr
         *
         * @par 视图查找
         *   在 views_ HashMap 中查找。
         *   O(1) 复杂度。
         *
         * @par 生命周期
         *   返回的指针由 Win32DisplayServer 拥有，不要删除。
         */
        virtual IView *GetView(ViewID id) const override;

        /**
         * @brief 处理所有窗口的事件
         *
         * @par 事件处理
         *   遍历所有窗口，调用每个窗口的 PollEvents()。
         *   每个窗口内部处理其消息队列。
         *
         * @par 调用时机
         *   应在应用主循环中调用：
         *   @code
         *   while (!should_exit) {
         *       display_server->ProcessEvents();
         *       renderer->Render();
         *   }
         *   @endcode
         *
         * @par 消息分发
         *   消息由 Win32Window::PollEvents() 处理：
         *   - PeekMessage 检查消息
         *   - TranslateMessage 翻译消息
         *   - DispatchMessage 分发到 WndProc
         */
        virtual void ProcessEvents() override;

        /**
         * @brief 设置窗口的垂直同步模式
         *
         * @param[in] window 窗口 ID
         * @param[in] mode 垂直同步模式
         *
         * @par VSync 模式
         *   - kDisabled：完全关闭 VSync
         *   - kEnabled：开启 VSync（默认）
         *   - kAdaptive：适应性 VSync
         *
         * @par 实现
         *   在 DisplayServer 层记录 VSync 模式。
         *   实际 VSync 控制由 RenderingContextDriver 实现。
         *
         * @par 安全检查
         *   如果窗口不存在，函数无操作。
         */
        virtual void SetVsyncMode(WindowID window, VSyncMode mode) override;

        /**
         * @brief 获取窗口的垂直同步模式
         *
         * @param[in] window 窗口 ID
         *
         * @return VSyncMode 当前 VSync 模式
         *
         * @par 默认值
         *   如果窗口不存在，返回 VSyncMode::kEnabled。
         *
         * @see SetVsyncMode()
         */
        virtual VSyncMode GetVsyncMode(WindowID window) const override;

    private:
        /**
         * @brief 枚举所有显示器
         *
         * @par 枚举过程
         *   使用 EnumDisplayMonitors 枚举：
         *   1. 获取主 HDC
         *   2. 调用 EnumDisplayMonitors，传入 MonitorEnumProc 回调
         *   3. 回调中为每个显示器创建 Win32Screen
         *
         * @par 回调函数
         *   MonitorEnumProc 静态方法作为 EnumDisplayMonitors 的回调。
         *
         * @see MonitorEnumProc
         */
        void EnumerateScreens();

        /**
         * @brief 销毁所有屏幕
         *
         * @par 销毁步骤
         *   1. 遍历 screens_ 列表
         *   2. 删除每个 Win32Screen
         *   3. 清空 screens_ 列表
         *
         * @par 调用时机
         *   在 Shutdown() 中调用。
         */
        void DestroyAllScreens();

        /**
         * @brief 销毁所有窗口上下文
         *
         * @par 销毁步骤
         *   1. 遍历 windows_ HashMap
         *   2. 对每个 WindowContext：
         *      a. 销毁关联的视图（调用 ViewDestroy）
         *      b. 调用 window->Shutdown()
         *      c. 删除 Win32Window
         *   3. 清空 windows_ HashMap
         *
         * @par 调用时机
         *   在 Shutdown() 中调用。
         */
        void DestroyAllWindowsContexts();

        /**
         * @brief 销毁所有视图
         *
         * @par 销毁步骤
         *   1. 遍历 views_ HashMap
         *   2. 删除每个 Win32View
         *   3. 清空 views_ HashMap
         *
         * @note 此方法在 DestroyAllWindowsContexts 之后调用，
         *       因为窗口的 views_ 列表还持有指针。
         */
        void DestroyAllViews();

        /**
         * @brief 显示器枚举回调函数
         *
         * @param[in] hMonitor 显示器句柄
         * @param[in] hdcMonitor 显示器 HDC（未使用）
         * @param[in] lprcMonitor 显示器矩形区域
         * @param[in] dwData 用户数据（指向 Win32DisplayServer 的指针）
         *
         * @return BOOL 返回 TRUE 继续枚举，FALSE 停止枚举
         *
         * @par 回调行为
         *   1. 将 dwData 转换为 Win32DisplayServer*
         *   2. 使用 Win32Screen::Create(hMonitor) 创建屏幕
         *   3. 如果成功，添加到 screens_ 列表
         *   4. 返回 TRUE 继续枚举
         *
         * @par 使用方式
         *   @code
         *   EnumDisplayMonitors(hdc, nullptr, MonitorEnumProc, (LPARAM)this);
         *   @endcode
         */
        static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor,
                                             LPRECT lprcMonitor, LPARAM dwData);

        /** @brief 已枚举的屏幕列表（按 ID 顺序存储） */
        LocalVector<Win32Screen *> screens_;

        /** @brief 窗口上下文映射表（WindowID → WindowContext） */
        HashMap<WindowID, WindowContext> windows_;

        /** @brief 视图映射表（ViewID → Win32View*） */
        HashMap<ViewID, Win32View *> views_;

        /** @brief 下一个可用的窗口 ID */
        WindowID next_window_id_ = 1;

        /** @brief 下一个可用的视图 ID */
        ViewID next_view_id_ = 1;

        /** @brief 是否已初始化 */
        bool is_initialized_ = false;
    };

}

#endif
