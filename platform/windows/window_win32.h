/**
 * @file window_win32.h
 * @brief Win32 平台窗口实现声明
 *
 * @par 概述
 *   使用原生 Win32 API 实现窗口创建、事件处理和生命周期管理。
 *   继承 IWindow 接口，提供 Windows 平台特定功能。
 *
 * @par 架构设计
 *   Win32Window 通过以下机制实现多窗口支持：
 *   - RegisterClassEx 注册自定义窗口类（每个进程只需一次）
 *   - CreateWindowEx 创建原生窗口句柄
 *   - WndProc 静态回调处理所有窗口消息
 *   - GWLP_USERDATA 存储 Win32Window* 指针，实现消息路由到具体实例
 *
 * @par 消息处理流程
 *   Win32 消息循环（PeekMessage/DispatchMessage）由 DisplayServer 驱动，
 *   Win32Window::PollEvents() 内部不直接循环，而是通知 DisplayServer 轮询。
 *   WndProc 收到消息后根据消息类型分发给各个回调 handler。
 *
 * @par 支持的窗口模式
 *   - 窗口模式（Windowed）：标准可调边框窗口
 *   - 全屏模式（Fullscreen）：独占式全屏，切换时保存窗口位置和尺寸
 *   - 边框less窗口（Borderless）：无标题栏窗口，保持窗口尺寸可调
 *   - 最小化/最大化：通过系统按钮或 API 触发
 *
 * @par DPI 感知
 *   窗口使用 Per-Monitor DPI 感知（PROCESS_PER_MONITOR_DPI_AWARE），
 *   每个窗口可独立响应不同显示器的 DPI 缩放。
 *
 * @par 事件回调
 *   所有窗口事件通过 WindowCallbacks 结构回调给上层：
 *   - OnClose / OnDestroy：窗口关闭
 *   - OnResize / OnMove：尺寸/位置变化
 *   - OnFocus / OnBlur：焦点变化
 *   - OnMouseButton / OnMouseMove / OnMouseWheel：鼠标事件
 *   - OnKeyDown / OnKeyUp：键盘事件
 *   - OnScroll / OnHitTest：滚动和命中测试
 *
 * @par 限制
 *   - Phase 1 不包含图形上下文创建（Phase 2 由 RenderingContextDriver 负责）
 *   - SwapBuffers 不在此层实现
 *   - 暂不支持窗口拖拽缩放（需 Phase 2 添加 WM_NCHITTEST 处理）
 *
 * @note 仅在 ARHUD_PLATFORM_WINDOWS 定义时可用
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-06
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include "typedefs.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "window.h"

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
     * @class Win32Window
     * @brief Win32 平台窗口实现类
     *
     * @par 继承层次
     *   IWindow（抽象接口）
     *     └── Win32Window（具体实现）
     *
     * @par 生命周期
     *   1. 构造：创建空壳 window_id_ = kInvalidWindowId
     *   2. Initialize()：注册窗口类、创建原生窗口、初始化成员变量
     *   3. 使用：PollEvents() 轮询事件，属性 getter/setter 访问状态
     *   4. Shutdown()：销毁窗口句柄、注销窗口类
     *   5. 析构：确保已 Shutdown()，安全释放资源
     *
     * @par 线程安全
     *   非线程安全。所有方法应在主线程调用。
     *   Win32 窗口消息只在创建窗口的线程分发。
     *
     * @par 内存管理
     *   - 窗口类注册一次，所有 Win32Window 实例共享
     *   - 最后一个窗口销毁时注销窗口类
     *   - 使用 ARHUD_DISABLE_COPY_MOVE 禁止拷贝和移动
     */
    class Win32Window : public IWindow
    {
    public:
        /**
         * @brief 默认构造函数
         *
         * @par 行为
         *   创建空窗口对象，hwnd_ = nullptr，is_initialized_ = false。
         *   实际窗口创建在 Initialize() 中完成。
         *
         * @note 此时 window_id_ = kInvalidWindowId，需在 Initialize 之后通过 SetWindowId() 设置
         */
        Win32Window();

        /**
         * @brief 析构函数
         *
         * @par 行为
         *   确保窗口已关闭（调用 Shutdown()），安全释放所有资源。
         *   如果窗口仍处于初始化状态但未关闭，尝试自动关闭。
         *
         * @warning 如果有窗口仍在运行，析构前必须先调用 Shutdown()，
         *          以避免在消息循环中访问已销毁对象的成员变量。
         */
        virtual ~Win32Window() override;

        ARHUD_DISABLE_COPY_MOVE(Win32Window);

        /**
         * @brief 初始化窗口
         *
         * @param[in] desc 窗口描述结构，包含标题、尺寸、位置、模式等配置
         *
         * @par 初始化步骤
         *   1. 保存 desc_ 配置副本
         *   2. 保存 hinstance_（从 WinMain 传入或 GetModuleHandle）
         *   3. RegisterWindowClass() 注册窗口类（如尚未注册）
         *   4. CreateNativeWindow() 创建原生窗口
         *   5. 获取初始 DPI 缩放因子
         *   6. 初始化 is_initialized_ = true
         *
         * @par 窗口类注册
         *   窗口类名为 "ARHUD_WINDOW_CLASS"，包含：
         *   - CS_HREDRAW | CS_VREDRAW | CS_OWNDC 风格
         *   - WndProc 静态回调函数
         *   - 图标、背景、菜单为系统默认值
         *
         * @par 原生窗口创建参数
         *   - ExStyle: WS_EX_APPWINDOW | WS_EX_WINDOWEDGE（顶层窗口）
         *   - Style: WS_OVERLAPPEDWINDOW，可根据模式调整为 WS_POPUP
         *   - Width/Height: desc 中指定，pos 由 desc.position 或 CW_USEDEFAULT 确定
         *
         * @return kNoError 初始化成功
         * @return kInvalidArgument desc 中尺寸为 0 或模式无效
         * @return kOutOfMemory 窗口类注册或窗口创建失败
         * @return kAlreadyInitialized 窗口已初始化，不能重复初始化
         *
         * @note 初始化失败时会设置 should_close_ = true，窗口将被视为已请求关闭
         */
        virtual Error Initialize(const WindowDesc &desc) override;

        /**
         * @brief 关闭窗口并释放资源
         *
         * @par 关闭步骤
         *   1. 触发 OnClose 回调（如已设置）
         *   2. 调用 DestroyWindow(hwnd_) 销毁原生窗口
         *   3. 重置 hwnd_ = nullptr
         *   4. 如果是最后一个窗口，调用 UnregisterWindowClass()
         *   5. 重置 is_initialized_ = false
         *
         * @par 回调触发时机
         *   OnClose 回调在 DestroyWindow 之前触发，允许上层取消关闭。
         *   但本实现不处理取消，直接继续销毁。
         *
         * @note 即使窗口未初始化，调用 Shutdown() 也是安全的（无操作）
         */
        virtual void Shutdown() override;

        /**
         * @brief 轮询并处理窗口消息
         *
         * @par 消息处理
         *   使用 PeekMessage 检查并移除消息队列中的消息：
         *   - WM_QUIT：设置 should_close_ = true，不分发（消息已处理）
         *   - 其他消息：TranslateMessage + DispatchMessage 派发到 WndProc
         *
         * @par 消息类型处理
         *   WndProc 静态回调根据消息类型调用 HandleMessage()：
         *   - WM_SIZE → 更新 width_/height_，触发 OnResize
         *   - WM_MOVE → 更新 pos_x_/pos_y_，触发 OnMove
         *   - WM_CLOSE → 触发 OnClose
         *   - WM_DESTROY → 触发 OnDestroy，设置 should_close_ = true
         *   - WM_KEYDOWN/WM_KEYUP → 触发 OnKeyDown/OnKeyUp
         *   - WM_LBUTTONDOWN 等 → 触发 OnMouseButton
         *   - WM_MOUSEMOVE → 触发 OnMouseMove
         *   - WM_MOUSEWHEEL → 触发 OnMouseWheel
         *   - WM_SETFOCUS/WM_KILLFOCUS → 触发 OnFocus/OnBlur
         *   - WM_PAINT → 触发 OnPaint（仅更新区域）
         *
         * @note 此方法不阻塞，如果没有消息立即返回
         * @note 必须在主线程调用，Win32 消息只能在创建窗口的线程处理
         */
        virtual void PollEvents() override;

        /**
         * @brief 查询窗口是否已请求关闭
         *
         * @return true 用户点击关闭按钮、调用 PostQuitMessage、或收到 WM_DESTROY
         * @return false 窗口正常运行，未请求关闭
         *
         * @par 使用场景
         *   主循环使用 while(!window->ShouldClose()) 检查是否继续渲染。
         *   应在 PollEvents() 之后调用以获取最新状态。
         *
         * @par 线程安全
         *   should_close_ 仅在 WndProc 的 WM_DESTROY 处理中写入，
         *   读取发生在主循环，均在同一线程，无竞态。
         */
        virtual bool ShouldClose() const override;

        /**
         * @brief 获取原生窗口句柄
         *
         * @return void* 指向 HWND 的指针，类型为 HWND*
         *
         * @par 使用场景
         *   - RenderingContextDriver 用此创建 OpenGL/D3D 上下文
         *   - 获取 HDC 用于 GDI 绘图（如需要）
         *
         * @warning 不要直接对返回的 HWND 调用 DestroyWindow，
         *          应通过 Shutdown() 方法以正确顺序销毁
         *
         * @par 示例
         *   @code
         *   HWND hwnd = *(static_cast<HWND*>(window->GetNativeHandle()));
         *   HDC hdc = GetDC(hwnd);
         *   // ... 渲染操作
         *   ReleaseDC(hwnd, hdc);
         *   @endcode
         */
        virtual void *GetNativeHandle() const override;

        /**
         * @brief 获取窗口内容宽度（像素）
         *
         * @return uint32_t 窗口客户区宽度，不包含标题栏和边框
         *
         * @par 与 WM_SIZE 的区别
         *   - GetWidth() 返回缓存的 width_ 值
         *   - WM_SIZE 的 lParam HIWORD 包含整个窗口（包括非客户区）
         *   - 对于 AR HUD 渲染，使用客户区尺寸
         *
         * @par DPI 影响
         *   返回物理像素值，非 DPI 缩放后值。
         *   如需逻辑像素，应除以 GetDpiScale()。
         */
        virtual uint32_t GetWidth() const override;

        /**
         * @brief 获取窗口内容高度（像素）
         *
         * @return uint32_t 窗口客户区高度，不包含标题栏和边框
         *
         * @par 与 WM_SIZE 的区别
         *   - GetHeight() 返回缓存的 height_ 值
         *   - WM_SIZE 的 lParam LOWORD 包含整个窗口
         *   - 对于 AR HUD 渲染，使用客户区尺寸
         *
         * @par DPI 影响
         *   返回物理像素值，非 DPI 缩放后值。
         */
        virtual uint32_t GetHeight() const override;

        /**
         * @brief 查询窗口是否可见
         *
         * @return true 窗口完全或部分可见（IsWindowVisible 返回 true）
         * @return false 窗口被隐藏、最小化或尚未创建
         *
         * @par 可见性状态
         *   - WS_VISIBLE 样式已设置
         *   - 窗口未被 ShowWindow(hwnd, SW_HIDE) 隐藏
         *   - 窗口未被最小化到任务栏
         *
         * @par 注意
         *   最小化窗口 IsVisible() 可能返回 true（取决于系统），
         *   如需判断是否最小化，应使用 IsMinimized()。
         */
        virtual bool IsVisible() const override;

        /**
         * @brief 查询窗口是否处于最小化状态
         *
         * @return true 窗口已最小化到任务栏
         * @return false 窗口未最小化
         *
         * @par 检测方式
         *   检查 WS_MINIMIZE 样式位，或调用 IsIconic(hwnd)。
         *
         * @par AR HUD 行为
         *   最小化时可选择暂停渲染循环以节省资源。
         */
        virtual bool IsMinimized() const override;

        /**
         * @brief 查询窗口是否处于最大化状态
         *
         * @return true 窗口已最大化（占据整个屏幕）
         * @return false 窗口未最大化
         *
         * @par 检测方式
         *   检查 WS_MAXIMIZE 样式位，或调用 IsZoomed(hwnd)。
         */
        virtual bool IsMaximized() const override;

        /**
         * @brief 获取窗口在屏幕上的位置
         *
         * @param[out] x 窗口左上角 X 坐标（屏幕坐标系，原点为屏幕左上角）
         * @param[out] y 窗口左上角 Y 坐标（屏幕坐标系，原点为屏幕左上角）
         *
         * @par 坐标系
         *   返回屏幕坐标，非客户区坐标。
         *   包含标题栏和边框的完整窗口位置。
         *
         * @par 多显示器
         *   对于多显示器系统，坐标可能是负值（主显示器左侧的显示器）。
         *
         * @par 示例
         *   @code
         *   int32_t x, y;
         *   window->GetPosition(x, y);
         *   // x, y 为屏幕坐标，可用于 SetPosition()
         *   @endcode
         */
        virtual void GetPosition(int32_t &x, int32_t &y) const override;

        /**
         * @brief 获取窗口所在屏幕的 ID
         *
         * @return uint32_t 窗口所在的屏幕 ID（由 DisplayServer 分配）
         * @return IScreen::kInvalidScreenId 窗口尚未关联到任何屏幕
         *
         * @par 屏幕关联
         *   窗口创建时根据位置自动关联到对应屏幕。
         *   窗口移动时屏幕 ID 可能变化（需要手动更新）。
         *
         * @par 用途
         *   用于获取屏幕的 DPI、刷新率等信息。
         *   @code
         *   IScreen* screen = display_server->GetScreen(window->GetScreenId());
         *   float scale = screen->GetScale();
         *   @endcode
         */
        virtual uint32_t GetScreenId() const override;

        /**
         * @brief 获取窗口的 DPI 缩放因子
         *
         * @return float DPI 缩放因子（1.0 = 100%, 1.25 = 125%, 1.5 = 150% 等）
         *
         * @par 计算方式
         *   scale = dpi / 96.0f
         *   其中 dpi 由 GetDpiForWindow(hwnd) 获取（Per-Monitor DPI）。
         *
         * @par 用途
         *   - UI 元素根据 scale 缩放以保持物理大小一致
         *   - 渲染分辨率可与 DPI 缩放挂钩（高分屏优化）
         *
         * @par 示例
         *   @code
         *   float scale = window->GetDpiScale();
         *   int32_t logical_width = static_cast<int32_t>(window->GetWidth() / scale);
         *   @endcode
         */
        virtual float GetDpiScale() const override;

        /**
         * @brief 调整窗口尺寸
         *
         * @param[in] width 新的客户区宽度（像素）
         * @param[in] height 新的客户区高度（像素）
         *
         * @par 行为
         *   调用 MoveWindow(hwnd, x, y, width, height, TRUE) 调整尺寸。
         *   新的 width/height 作为客户区尺寸传入（MoveWindow 会加上非客户区）。
         *
         * @par 消息通知
         *   调整成功后会收到 WM_SIZE 消息，触发 OnResize 回调。
         *
         * @par 约束
         *   - width/height 不能为 0
         *   - 最小尺寸由系统限制（通常为 1x1）
         *
         * @warning 如果窗口处于最大化状态，调用 Resize 无效。
         *          应先调用 SetFullscreenMode(WindowMode::kWindowed) 恢复窗口。
         */
        virtual void Resize(uint32_t width, uint32_t height) override;

        /**
         * @brief 设置窗口可见性
         *
         * @param[in] visible true 显示窗口，false 隐藏窗口
         *
         * @par 显示窗口
         *   调用 ShowWindow(hwnd, SW_SHOW) 或 SW_SHOWNA。
         *   窗口恢复到最后一次显示的位置和尺寸（如之前最小化/最大化）。
         *
         * @par 隐藏窗口
         *   调用 ShowWindow(hwnd, SW_HIDE)。
         *   窗口从屏幕上消失，但仍然存在。
         *
         * @par 与 SetVisible 的区别
         *   - 创建时 SetVisible(true) 会显示窗口
         *   - 运行中 SetVisible(false) 可隐藏窗口（不销毁）
         *   - 运行中 SetVisible(true) 可恢复窗口
         *
         * @par 回调触发
         *   - 显示时可能触发 WM_SHOWWINDOW、WM_SETFOCUS
         *   - 隐藏时可能触发 WM_SHOWWINDOW、WM_KILLFOCUS
         */
        virtual void SetVisible(bool visible) override;

        /**
         * @brief 设置窗口标题
         *
         * @param[in] title UTF-8 编码的窗口标题字符串
         *
         * @par 行为
         *   调用 SetWindowTextA(hwnd, title) 设置标题。
         *   标题显示在窗口标题栏。
         *
         * @par 编码
         *   使用 ANSI 版本（SetWindowTextA），假设标题为系统默认编码。
         *   如需支持 Unicode 标题，应使用 SetWindowTextW 并转换编码。
         *
         * @par 限制
         *   - Windows 窗口标题长度理论上无限制
         *   - 实际受系统资源限制
         *
         * @par 示例
         *   @code
         *   window->SetTitle("AR HUD - Main Window");
         *   @endcode
         */
        virtual void SetTitle(const char *title) override;

        /**
         * @brief 设置窗口在屏幕上的位置
         *
         * @param[in] x 窗口左上角 X 坐标（屏幕坐标系）
         * @param[in] y 窗口左上角 Y 坐标（屏幕坐标系）
         *
         * @par 行为
         *   调用 SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0,
         *                      SWP_NOZORDER | SWP_NOSIZE) 移动窗口。
         *   窗口尺寸保持不变。
         *
         * @par 坐标系
         *   使用屏幕坐标，原点 (0, 0) 为主显示器左上角。
         *   多显示器系统中，负坐标表示在主显示器左侧或上方。
         *
         * @par 消息通知
         *   移动成功后会收到 WM_MOVE 消息，触发 OnMove 回调。
         *
         * @par 约束
         *   - 坐标不能超出所有显示器的总范围（系统会限制）
         *   - 如果窗口超出边界，系统会自动调整
         */
        virtual void SetPosition(int32_t x, int32_t y) override;

        /**
         * @brief 设置窗口全屏模式
         *
         * @param[in] mode 目标窗口模式
         *
         * @par 窗口模式
         *   - kWindowed：标准窗口，可调整尺寸、有标题栏
         *   - kFullscreen：独占式全屏，切换显示分辨率
         *   - kBorderless：边框less窗口，保持当前分辨率
         *
         * @par 全屏模式切换
         *   切换到全屏时：
         *   1. 保存当前窗口位置、尺寸、模式
         *   2. 获取窗口所在屏幕的分辨率
         *   3. 切换分辨率（如 kFullscreen）
         *   4. 调整窗口覆盖整个屏幕
         *
         *   切换回窗口模式时：
         *   1. 恢复原始分辨率
         *   2. 恢复原始窗口位置和尺寸
         *
         * @par kBorderless 特别说明
         *   - 不切换分辨率
         *   - 窗口尺寸设为屏幕分辨率
         *   - 移除 WS_CAPTION、WS_THICKFRAME 等样式
         *   - 窗口保持可移动、可关闭
         *
         * @par 回调触发
         *   模式切换后触发 OnResize 回调。
         */
        virtual void SetFullscreenMode(WindowMode mode) override;

        /**
         * @brief 获取当前窗口模式
         *
         * @return WindowMode 当前窗口模式
         *
         * @par 可能值
         *   - kWindowed：标准窗口模式
         *   - kFullscreen：全屏独占模式
         *   - kBorderless：边框less窗口模式
         *
         * @see SetFullscreenMode()
         */
        virtual WindowMode GetWindowMode() const override;

        /**
         * @brief 获取窗口唯一标识符
         *
         * @return WindowID 窗口 ID，由 DisplayServer 分配
         * @return kInvalidWindowId 窗口尚未分配 ID
         *
         * @par ID 分配
         *   ID 在 WindowCreate 时由 DisplayServer 分配，
         *   并通过 SetWindowId() 设置到 Win32Window。
         *
         * @par 用途
         *   - 区分不同窗口
         *   - 作为窗口相关 API 的句柄
         *   - 在窗口映射表中快速查找
         */
        virtual WindowID GetWindowId() const override;

        /**
         * @brief 设置窗口唯一标识符
         *
         * @param[in] id 要分配的窗口 ID
         *
         * @par 调用时机
         *   通常由 DisplayServer 在 WindowCreate() 时调用。
         *
         * @par 约束
         *   - ID 应在窗口创建后、加入窗口列表前设置
         *   - 同一个 DisplayServer 实例中 ID 应唯一
         */
        virtual void SetWindowId(WindowID id) override;

        /**
         * @brief 设置窗口事件回调函数
         *
         * @param[in] callbacks 窗口回调结构，包含各类事件的回调函数指针
         *
         * @par 回调覆盖
         *   多次调用 SetCallbacks() 会覆盖之前的回调。
         *   可以在窗口创建后随时更改回调。
         *
         * @par 线程安全
         *   回调设置后，WndProc 收到对应消息时会调用。
         *   应确保回调指针在窗口生命周期内保持有效。
         *
         * @par 示例
         *   @code
         *   WindowCallbacks callbacks;
         *   callbacks.on_close = [](IWindow& w) { return true; };  // 允许关闭
         *   callbacks.on_resize = [](IWindow& w, uint32_t w, uint32_t h) {
         *       renderer->Resize(w, h);
         *   };
         *   window->SetCallbacks(callbacks);
         *   @endcode
         *
         * @see WindowCallbacks
         */
        virtual void SetCallbacks(const WindowCallbacks &callbacks) override;

        /**
         * @brief 检查窗口是否已初始化
         *
         * @return true 窗口已成功初始化（Initialize() 已调用且成功）
         * @return false 窗口未初始化或初始化失败
         *
         * @par 用途
         *   用于判断窗口是否可以使用。
         *   初始化失败时 should_close_ 会被设为 true。
         */
        bool IsInitialized() const;

    private:
        /**
         * @brief Win32 窗口过程函数（静态回调）
         *
         * @param[in] hwnd 窗口句柄
         * @param[in] msg 消息类型
         * @param[in] wParam 消息参数（取决于消息类型）
         * @param[in] lParam 消息参数（取决于消息类型）
         *
         * @return LRESULT 消息处理结果（取决于消息类型）
         *
         * @par 职责
         *   所有窗口消息的入口点。根据 hwnd 获取关联的 Win32Window*，
         *   然后调用 HandleMessage() 分发到具体实例。
         *
         * @par 消息路由
         *   1. 从 hwnd 获取 GWLP_USERDATA（存储 Win32Window*）
         *   2. 如果 Win32Window* 有效，调用 instance->HandleMessage()
         *   3. 否则调用 DefWindowProc()
         *
         * @par this 指针获取
         *   @code
         *   Win32Window* self = reinterpret_cast<Win32Window*>(
         *       GetWindowLongPtr(hwnd, GWLP_USERDATA));
         *   @endcode
         */
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        /**
         * @brief 处理窗口消息（实例方法）
         *
         * @param[in] msg 消息类型
         * @param[in] wParam 消息参数
         * @param[in] lParam 消息参数
         *
         * @return LRESULT 消息处理结果
         *
         * @par 消息处理
         *   根据 msg 类型执行不同操作：
         *   - 输入消息：转换并触发回调
         *   - 状态消息：更新内部状态，触发回调
         *   - 系统消息：执行默认行为或自定义处理
         *
         * @par 返回值
         *   - 已处理的消息返回 0
         *   - 需要默认处理的消息调用 DefWindowProc 并返回其结果
         *   - 某些消息（如 WM_PAINT）有特殊返回值
         */
        LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

        /**
         * @brief 注册窗口类
         *
         * @return true 窗口类注册成功，或已注册
         * @return false 注册失败（GetLastError() 获取详细错误）
         *
         * @par 注册时机
         *   第一次创建窗口时注册。
         *   使用 is_class_registered_ 标志避免重复注册。
         *
         * @par 类结构
         *   WNDCLASSEXA ex = {
         *       .cbSize = sizeof(WNDCLASSEXA),
         *       .style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
         *       .lpfnWndProc = WndProc,
         *       .hInstance = hinstance_,
         *       .lpszClassName = kWindowClassName,
         *       .hIcon = LoadIcon(nullptr, IDI_APPLICATION),
         *       .hCursor = LoadCursor(nullptr, IDC_ARROW),
         *       .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
         *   };
         */
        bool RegisterWindowClass();

        /**
         * @brief 注销窗口类
         *
         * @par 注销时机
         *   最后一个窗口销毁时注销。
         *   使用 UnregisterClass(kWindowClassName, hinstance_)。
         *
         * @par 线程安全
         *   应在所有窗口销毁后调用。
         */
        void UnregisterWindowClass();

        /**
         * @brief 创建原生 Win32 窗口
         *
         * @param[in] title 窗口标题
         * @param[in] width 窗口客户区宽度
         * @param[in] height 窗口客户区高度
         *
         * @return true 原生窗口创建成功
         * @return false 创建失败（GetLastError() 获取详细错误）
         *
         * @par 创建参数
         *   - ExStyle: WS_EX_APPWINDOW | WS_EX_WINDOWEDGE
         *   - ClassName: kWindowClassName
         *   - WindowName: title
         *   - Style: 根据 current_mode_ 设置（WS_OVERLAPPEDWINDOW 或 WS_POPUP）
         *   - x, y: 根据 desc_.position 或 CW_USEDEFAULT
         *   - Width, Height: 包含非客户区
         *   - Parent: nullptr（顶层窗口）
         *   - Menu: nullptr
         *   - lpParam: this 指针（在 WM_CREATE 中取出）
         *
         * @par WM_CREATE 处理
         *   在 WM_CREATE 的 lParam 中可获取 CREATESTRUCT*，
         *   其中 lpParam 存储的 this 指针被取出并设置为 GWLP_USERDATA。
         */
        bool CreateNativeWindow(const char *title, uint32_t width, uint32_t height);

        /**
         * @brief 更新内部尺寸缓存
         *
         * @param[in] width 新的客户区宽度
         * @param[in] height 新的客户区高度
         *
         * @par 用途
         *   当收到 WM_SIZE 消息时调用，更新缓存的 width_/height_。
         *   触发 OnResize 回调。
         */
        void UpdateSize(uint32_t width, uint32_t height);

        /**
         * @brief 获取当前键盘修饰键状态
         *
         * @return uint32_t 修饰键标志位
         *
         * @par 标志位
         *   - 0x01: Shift 按下
         *   - 0x02: Ctrl 按下
         *   - 0x04: Alt 按下
         *   - 0x08: CapsLock 开启
         *
         * @par 实现
         *   使用 GetKeyState(VK_SHIFT)、GetKeyState(VK_CONTROL) 等。
         */
        uint32_t GetKeyModifiers() const;

        /**
         * @brief 将 Win32 鼠标按钮标志转换为 MouseButton 枚举
         *
         * @param[in] wParam MK_* 标志（来自 WM_*BUTTON* 消息的 wParam）
         *
         * @return MouseButton 转换后的按钮枚举
         *
         * @par 映射关系
         *   - MK_LBUTTON → MouseButton::kLeft
         *   - MK_RBUTTON → MouseButton::kRight
         *   - MK_MBUTTON → MouseButton::kMiddle
         *   - MK_XBUTTON1 → MouseButton::kX1
         *   - MK_XBUTTON2 → MouseButton::kX2
         *
         * @note 此方法只返回按下的按钮，不处理坐标信息
         */
        static MouseButton Win32ButtonToMouseButton(WPARAM wParam);

        /** @brief 窗口类名，所有 ARHUD 窗口共享此类名 */
        static constexpr const char *kWindowClassName = "ARHUD_WINDOW_CLASS";

        /** @brief 原生窗口句柄，nullptr 表示尚未创建或已销毁 */
        HWND hwnd_ = nullptr;

        /** @brief 进程实例句柄，从 WinMain 或 GetModuleHandle 获取 */
        HINSTANCE hinstance_ = nullptr;

        /** @brief 窗口是否已成功初始化 */
        bool is_initialized_ = false;

        /** @brief 窗口是否已请求关闭（收到 WM_DESTROY 或 ShouldClose 被调用） */
        bool should_close_ = false;

        /** @brief 窗口是否由外部拥有（如嵌入到其他窗口） */
        bool is_external_ = false;

        /** @brief 窗口类是否已注册 */
        bool is_class_registered_ = false;

        /** @brief 窗口是否处于最小化状态 */
        bool is_minimized_ = false;

        /** @brief 窗口是否处于最大化状态 */
        bool is_maximized_ = false;

        /** @brief 窗口客户区宽度（像素） */
        uint32_t width_ = 0;

        /** @brief 窗口客户区高度（像素） */
        uint32_t height_ = 0;

        /** @brief 窗口左上角 X 坐标（屏幕坐标系） */
        int32_t pos_x_ = 0;

        /** @brief 窗口左上角 Y 坐标（屏幕坐标系） */
        int32_t pos_y_ = 0;

        /** @brief 窗口所在屏幕的 ID */
        uint32_t screen_id_ = UINT32_MAX;

        /** @brief DPI 缩放因子（1.0 = 100%） */
        float dpi_scale_ = 1.0f;

        /** @brief 窗口唯一标识符（由 DisplayServer 分配） */
        WindowID window_id_ = kInvalidWindowId;

        /** @brief 当前窗口模式 */
        WindowMode current_mode_ = WindowMode::kWindowed;

        /** @brief 窗口配置副本（保存初始化时的参数） */
        WindowDesc desc_;

        /** @brief 窗口事件回调函数集合 */
        WindowCallbacks callbacks_;
    };

}

#endif
