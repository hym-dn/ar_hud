/**
 * @file window.h
 * @brief 平台无关窗口抽象接口
 *
 * 定义窗口创建参数、事件回调和 IWindow 抽象接口。
 * 平台特定实现（Win32/X11/Android/QNX）继承此接口。
 *
 * @par 设计哲学：
 *   窗口接口采用组合优于继承的设计，IWindow 只定义最小必要的抽象，
 *   平台特定行为通过子类重写实现。事件处理使用 C 函数指针回调，
 *   避免虚函数表开销，适合嵌入式场景。
 *
 * @par 架构层次：
 *   IDisplayServer ──┬──> IWindow ──> Win32Window
 *                    ├──> IScreen ──> Win32Screen
 *                    └──> IView  ──> Win32View
 *
 * 设计参考：
 *   - Godot DisplayServer 窗口管理接口（platform/windows/display_server_windows.cpp）
 *   - SDL_Window 窗口抽象
 *   - 3D HUD 项目 IWindow 接口
 *   - arch_skill.md ARHudWindow 设计
 *
 * 生命周期：
 *   1. 构造 → 未初始化状态
 *   2. Initialize() → 创建平台窗口
 *   3. PollEvents() / ShouldClose() → 事件循环
 *   4. Shutdown() → 销毁窗口资源
 *   5. 析构 → 自动调用 Shutdown()
 *
 * @par 使用示例：
 *   @code{.cpp}
 *   Win32Window window;
 *   WindowDesc desc;
 *   desc.width = 1280;
 *   desc.height = 720;
 *   desc.title = "AR HUD";
 *   if (window.Initialize(desc) != Error::kOK) {
 *       return;
 *   }
 *
 *   WindowCallbacks callbacks;
 *   callbacks.OnResize = [](uint32_t id, int32_t w, int32_t h, void*) {
 *       printf("Resize: %ux%u\n", w, h);
 *   };
 *   callbacks.userdata = nullptr;
 *   window.SetCallbacks(callbacks);
 *
 *   while (!window.ShouldClose()) {
 *       window.PollEvents();
 *       // 渲染逻辑
 *   }
 *   window.Shutdown();
 *   @endcode
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-06
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include "typedefs.h"

namespace arhud
{
    // ═══════════════════════════════════════════════════════════════════════════
    // WindowMode
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief 窗口显示模式
     *
     * 定义窗口的视觉效果和行为模式。
     * 不同模式对性能、兼容性和用户体验有显著影响。
     *
     * @note ExclusiveFullscreen 模式下引擎直接控制显示器输出刷新率，
     *       可实现最低延迟，但切换回窗口模式时有短暂黑屏
     * @see IWindow::SetFullscreenMode() IWindow::GetWindowMode()
     */
    enum class WindowMode
    {
        /**
         * @brief 窗口模式（默认）
         *
         * 标准窗口，带标题栏和边框，可自由调整大小。
         * 适合开发和调试阶段。
         */
        kWindowed,

        /**
         * @brief 全屏窗口
         *
         * 无边框窗口，覆盖整个桌面区域。
         * 分辨率保持为桌面分辨率，切换时无黑屏。
         * 与其他窗口共享桌面，Alt+Tab 可切换。
         */
        kFullscreen,

        /**
         * @brief 无边框窗口
         *
         * 无标题栏和边框的透明窗口，可覆盖全屏。
         * 与 kFullscreen 的区别是不会隐藏桌面上的其他窗口。
         * 常用于 AR HUD 叠加层渲染（仪表盘、导航箭头）。
         */
        kBorderless,

        /**
         * @brief 独占全屏模式
         *
         * 直接控制显示器输出模式，切换分辨率以匹配窗口尺寸。
         * 提供最低的输入延迟和最高的渲染性能。
         * 切换时会出现短暂黑屏（1-2秒）。
         * 常见于游戏和专业可视化应用。
         */
        kExclusiveFullscreen
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // VSyncMode
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief 垂直同步模式
     *
     * 控制渲染帧与显示器刷新率之间的同步方式。
     * VSync 可防止画面撕裂（tearing），但会增加输入延迟。
     *
     * @par 画面撕裂原理：
     *   当 GPU 输出帧率与显示器刷新率不同步时，
     *   显示器在刷新过程中接收到新的帧数据，导致画面上下部分显示不同帧
     *
     * @see IWindow::SetVsyncMode() IWindow::GetVsyncMode()
     */
    enum class VSyncMode
    {
        /**
         * @brief 关闭垂直同步
         *
         * 渲染帧直接输出到显示器，不等待垂直回扫期。
         * 可实现最高帧率和最低输入延迟，
         * 但在高帧率下会出现明显画面撕裂。
         * 适合：帧率上限为刷新率 2 倍以上的电竞场景。
         */
        kDisabled,

        /**
         * @brief 启用垂直同步
         *
         * 每帧等待显示器垂直回扫期完成后输出。
         * 完全消除画面撕裂，但会增加 1 帧延迟。
         * 如果帧率低于刷新率，会出现倍帧卡顿（stuttering）。
         * 适合：一般游戏和注重画面质量的场景。
         */
        kEnabled,

        /**
         * @brief 自适应垂直同步
         *
         * 帧率低于刷新率时自动启用 VSync，
         * 帧率高于刷新率时自动关闭 VSync。
         * 平衡画面撕裂和延迟问题。
         * 适合：帧率波动大的场景（如复杂场景、光线追踪）。
         *
         * @note 实现依赖 GPU 驱动支持（GL: WGL_EXT_swap_control_tear,
         *       Vulkan: VK_PRESENT_MODE_FIFO_RELAXED_KHR）
         */
        kAdaptive
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // MouseButton
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief 鼠标按键枚举
     *
     * 定义支持的鼠标按键，与平台无关的抽象。
     * 支持 5 键鼠标，滚轮点击为中键。
     *
     * @par 按键编号对应：
     *   - kLeft   : 主键（右手鼠标的左键）
     *   - kRight  : 次键（右手鼠标的右键）
     *   - kMiddle : 滚轮按下
     *   - kX1/kX2 : 侧键（浏览器前进/后退）
     *
     * @see WindowCallbacks::OnMouse
     */
    enum class MouseButton
    {
        kLeft,   ///< 鼠标左键（主键）
        kRight,  ///< 鼠标右键（次键）
        kMiddle, ///< 滚轮按下（中键）
        kX1,     ///< 侧键 1（通常映射到"前进"）
        kX2      ///< 侧键 2（通常映射到"后退"）
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // KeyModifier
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief 键盘修饰键位掩码
     *
     * 用于表示键盘修饰键（Shift/Ctrl/Alt 等）的按下状态。
     * 使用位掩码表示，支持多个修饰键同时按下。
     *
     * @par 使用示例：
     *   @code{.cpp}
     *   if (modifiers & KeyModifier::kModifierCtrl) {
     *       if (key == 'S') { // Ctrl+S 保存
     *           Save();
     *       }
     *   }
     *   @endcode
     *
     * @note 修饰键状态在 WindowCallbacks::OnKey 回调中提供
     * @see WindowCallbacks::OnKey
     */
    enum KeyModifier : uint32_t
    {
        kModifierNone = 0,           ///< 无修饰键
        kModifierShift = 1U << 0,    ///< Shift 键
        kModifierCtrl = 1U << 1,     ///< Ctrl 键
        kModifierAlt = 1U << 2,      ///< Alt 键
        kModifierSuper = 1U << 3,    ///< Super/Win/Cmd 键
        kModifierCapsLock = 1U << 4, ///< 大写锁定（ Caps Lock 开启状态）
        kModifierNumLock = 1U << 5   ///< 数字锁定（ Num Lock 开启状态）
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // WindowDesc
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief 窗口创建参数
     *
     * 封装创建窗口所需的全部参数，提供合理默认值。
     * 传递给 IWindow::Initialize() 使用。
     *
     * @par 默认配置：
     *   1280x720 窗口、VSync 启用、可调整大小、窗口模式、主屏幕。
     *
     * @par 参数校验：
     *   实现类应检查 width >= 100 && height >= 100，
     *   过小的窗口尺寸无实际意义且可能导致渲染问题。
     *
     * @par 位置参数 x/y：
     *   - 设置为 -1 表示使用系统默认位置（通常为居中）
     *   - 正值表示左上角相对于屏幕的像素坐标
     *   - 全屏模式通常忽略 x/y，使用屏幕分辨率
     *
     * @see IWindow::Initialize()
     */
    struct WindowDesc
    {
        /**
         * @brief 窗口客户区宽度（像素）
         *
         * 不包括标题栏和窗口边框的内部绘图区域宽度。
         * 全屏模式时此值为目标显示器分辨率宽度。
         */
        uint32_t width = 1280;

        /**
         * @brief 窗口客户区高度（像素）
         *
         * 不包括标题栏和窗口边框的内部绘图区域高度。
         * 全屏模式时此值为目标显示器分辨率高度。
         */
        uint32_t height = 720;

        /**
         * @brief 窗口初始 X 位置（像素）
         *
         * 窗口左上角相对于屏幕的 X 坐标。
         * -1 表示系统默认（通常居中）。
         */
        int32_t x = -1;

        /**
         * @brief 窗口初始 Y 位置（像素）
         *
         * 窗口左上角相对于屏幕的 Y 坐标。
         * -1 表示系统默认（通常居中）。
         */
        int32_t y = -1;

        /**
         * @brief 窗口标题（UTF-8）
         *
         * 显示在窗口标题栏的文字。
         * 对于 AR HUD 应用，通常设置为 "AR HUD" 或应用名称。
         */
        const char *title = "AR HUD";

        /**
         * @brief 窗口显示模式
         *
         * 控制窗口的视觉效果。
         * @see WindowMode
         */
        WindowMode mode = WindowMode::kWindowed;

        /**
         * @brief 垂直同步模式
         *
         * 控制渲染帧与显示器刷新率的同步方式。
         * @see VSyncMode
         */
        VSyncMode vsync_mode = VSyncMode::kEnabled;

        /**
         * @brief 是否允许调整窗口大小
         *
         * true 时窗口可拖动边缘调整大小，带 WS_THICKFRAME 样式。
         * false 时窗口大小固定，适合需要精确控制分辨率的场景。
         */
        bool resizable = true;

        /**
         * @brief 创建后是否立即可见
         *
         * true 时调用 Initialize() 后窗口立即显示。
         * false 时窗口隐藏，需要显式调用 SetVisible(true) 显示。
         * 用于需要先完成初始化再显示的场景。
         */
        bool visible = true;

        /**
         * @brief 目标屏幕 ID
         *
         * 指定窗口创建在哪个显示器上。
         * UINT32_MAX 表示使用系统默认主屏幕。
         * 多显示器 AR HUD 场景可指定具体屏幕 ID。
         */
        uint32_t screen_id = UINT32_MAX;

        /**
         * @brief 平台原生窗口句柄
         *
         * 仅在 external_window = true 时使用。
         * Win32 平台为 HWND，X11 平台为 Window。
         * 当引擎需要嵌入到已有窗口时使用。
         */
        void *native_window = nullptr;

        /**
         * @brief 是否使用外部窗口
         *
         * true 表示窗口由外部创建，引擎仅管理该窗口的消息和属性，
         * 不负责创建和销毁窗口。
         * false（默认）表示引擎完全控制窗口生命周期。
         */
        bool external_window = false;
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // WindowCallbacks
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief 窗口事件回调集合
     *
     * 使用 C 函数指针 + userdata 模式，避免虚函数开销和继承耦合。
     * 所有回调默认为 nullptr（不触发），按需设置。
     *
     * @par 回调调用时机：
     *   所有回调在 PollEvents() 内部同步调用，处于主线程上下文。
     *   这意味着在回调中可以直接调用 OpenGL/Vulkan 等渲染 API，
     *   而无需额外线程同步。
     *
     * @par 线程安全：
     *   回调在主线程调用，但实现类应保证回调设置的原子性。
     *   建议在 Initialize() 之后、PollEvents() 之前设置所有回调。
     *
     * @par userdata 使用模式：
     *   @code{.cpp}
     *   struct AppContext {
     *       Renderer* renderer;
     *       Scene* scene;
     *   };
     *
     *   void OnResize(uint32_t id, int32_t w, int32_t h, void* userdata) {
     *       auto* ctx = static_cast<AppContext*>(userdata);
     *       ctx->renderer->ResizeViewport(w, h);
     *   }
     *
     *   AppContext ctx;
     *   callbacks.userdata = &ctx;
     *   callbacks.OnResize = OnResize;
     *   @endcode
     *
     * @see IWindow::SetCallbacks()
     */
    struct WindowCallbacks
    {
        /**
         * @brief 窗口尺寸改变回调
         *
         * 当窗口客户区大小改变时触发（用户拖动边缘、最大化、还原等）。
         * 初次创建窗口时也会触发一次。
         *
         * @param[in] id        窗口 ID
         * @param[in] width     新的客户区宽度（像素）
         * @param[in] height    新的客户区高度（像素）
         * @param[in] userdata  用户数据指针（来自 callbacks.userdata）
         */
        void (*OnResize)(uint32_t id, int32_t width, int32_t height, void *userdata) = nullptr;

        /**
         * @brief 窗口关闭回调
         *
         * 当用户点击关闭按钮（X）时触发。
         * 此时窗口尚未销毁，可以进行清理工作。
         * 调用 SetFullscreenMode(WindowMode::kWindowed) 可取消关闭。
         *
         * @param[in] id        窗口 ID
         * @param[in] userdata  用户数据指针
         */
        void (*OnClose)(uint32_t id, void *userdata) = nullptr;

        /**
         * @brief 窗口焦点变化回调
         *
         * 当窗口获得或失去键盘焦点时触发。
         * 获得焦点时 focused = true，失去时 focused = false。
         * 失去焦点时可暂停渲染以节省资源。
         *
         * @param[in] id        窗口 ID
         * @param[in] focused   true 表示获得焦点，false 表示失去焦点
         * @param[in] userdata  用户数据指针
         */
        void (*OnFocus)(uint32_t id, bool focused, void *userdata) = nullptr;

        /**
         * @brief 按键事件回调
         *
         * 当键盘按键被按下（pressed=true）或释放（pressed=false）时触发。
         *
         * @param[in] id        窗口 ID
         * @param[in] key       虚拟键码（与平台无关的抽象，参考 VK_* Win32 常量）
         * @param[in] scancode  原始扫描码（用于区分同一键码的不同物理按键）
         * @param[in] pressed   true 表示按下，false 表示释放
         * @param[in] modifiers 当前修饰键状态（KeyModifier 位掩码）
         * @param[in] userdata  用户数据指针
         *
         * @note 持续按住某键会重复触发 OnKey（按键 repeat），
         *       通常 pressed=true 会多次触发
         */
        void (*OnKey)(uint32_t id, int32_t key, int32_t scancode, bool pressed, uint32_t modifiers, void *userdata) = nullptr;

        /**
         * @brief 字符输入回调
         *
         * 当有可打印字符输入时触发（考虑 Shift/Caps Lock 的字符映射）。
         * 用于文本输入场景，比 OnKey 更适合处理文字输入。
         *
         * @param[in] id         窗口 ID
         * @param[in] codepoint  Unicode 码点（如 'A' = 0x41, '中' = 0x4E2D）
         * @param[in] userdata   用户数据指针
         *
         * @note 对于非 ASCII 字符（如中文），可能需要多次 OnChar 调用
         *       才能组成一个完整字符（取决于输入法状态）
         */
        void (*OnChar)(uint32_t id, uint32_t codepoint, void *userdata) = nullptr;

        /**
         * @brief 鼠标按键回调
         *
         * 当鼠标按键被按下（pressed=true）或释放（pressed=false）时触发。
         *
         * @param[in] id         窗口 ID
         * @param[in] x          鼠标 X 坐标（客户区坐标，原点为窗口左上角）
         * @param[in] y          鼠标 Y 坐标（客户区坐标）
         * @param[in] button      鼠标按键（MouseButton 枚举）
         * @param[in] pressed     true 表示按下，false 表示释放
         * @param[in] modifiers   当前修饰键状态
         * @param[in] userdata    用户数据指针
         *
         * @see MouseButton
         */
        void (*OnMouse)(uint32_t id, int32_t x, int32_t y, MouseButton button, bool pressed, uint32_t modifiers, void *userdata) = nullptr;

        /**
         * @brief 鼠标移动回调
         *
         * 当鼠标在窗口客户区内移动时触发。
         * 不带按键信息的纯移动事件（对应 WM_MOUSEMOVE）。
         *
         * @param[in] id         窗口 ID
         * @param[in] x          鼠标 X 坐标（客户区坐标）
         * @param[in] y          鼠标 Y 坐标（客户区坐标）
         * @param[in] modifiers  当前修饰键状态
         * @param[in] userdata   用户数据指针
         */
        void (*OnMouseMove)(uint32_t id, int32_t x, int32_t y, uint32_t modifiers, void *userdata) = nullptr;

        /**
         * @brief 鼠标滚轮回调
         *
         * 当鼠标滚轮滚动时触发。
         * 正值表示向前/上滚动，负值表示向后/下滚动。
         *
         * @param[in] id          窗口 ID
         * @param[in] x_offset    水平滚轮偏移（支持水平滚轮的鼠标）
         * @param[in] y_offset    垂直滚轮偏移（大多数鼠标）
         * @param[in] modifiers   当前修饰键状态
         * @param[in] userdata    用户数据指针
         *
         * @note y_offset 的单位是"线"（通常 1 格 = 120）
         */
        void (*OnScroll)(uint32_t id, double x_offset, double y_offset, uint32_t modifiers, void *userdata) = nullptr;

        /**
         * @brief 触摸事件回调
         *
         * 当触摸屏/触控板有点按（pressed=true）或释放（pressed=false）时触发。
         * 支持多点触控，touch_id 标识不同触点。
         *
         * @param[in] id         窗口 ID
         * @param[in] touch_id   触控点 ID（同一触控点的按下和释放共享同一 ID）
         * @param[in] x          触控点 X 坐标（客户区坐标）
         * @param[in] y          触控点 Y 坐标（客户区坐标）
         * @param[in] pressed    true 表示按下，false 表示释放
         * @param[in] userdata   用户数据指针
         *
         * @note 移动事件（pressed=true 持续）在移动过程中可能合并为少量回调，
         *       具体频率取决于平台实现
         */
        void (*OnTouch)(uint32_t id, int32_t touch_id, int32_t x, int32_t y, bool pressed, void *userdata) = nullptr;

        /**
         * @brief 用户数据指针
         *
         * 传递给所有回调函数的用户数据。
         * 用于在回调中访问调用者上下文。
         */
        void *userdata = nullptr;
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // IWindow
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief 平台无关窗口抽象接口
     *
     * 定义窗口生命周期管理、事件处理和属性查询的统一接口。
     * 平台特定实现（Win32Window / X11Window / AndroidWindow 等）继承此接口。
     *
     * @par 关键职责：
     *   - 窗口创建与销毁（生命周期管理）
     *   - 事件轮询与分发（主循环集成）
     *   - 窗口属性查询与修改（尺寸、位置、模式）
     *   - 原生句柄访问（供渲染层创建图形上下文）
     *
     * @par 线程安全：
     *   非线程安全。所有方法应在主线程调用。
     *   如需多线程访问，应使用外部同步机制保护。
     *
     * @par 平台实现注意事项：
     *   - Win32: 使用 RegisterClassEx + CreateWindowEx + WndProc
     *   - X11:   使用 XCreateWindow + XNextEvent（未来支持）
     *   - Android: 使用 ANativeWindow（未来支持）
     *
     * @par 与图形上下文的关联：
     *   IWindow 只管理窗口句柄，不负责创建图形上下文（GL/Vulkan）。
     *   图形上下文创建由 RenderingContextDriver 负责，
     *   通过 GetNativeHandle() 获取 HWND/Window 句柄。
     *
     * @see WindowDesc       窗口创建参数
     * @see WindowCallbacks  事件回调集合
     * @see IDisplayServer   窗口管理器（管理多个 IWindow 实例）
     */
    class IWindow
    {
    public:
        /** @brief 窗口 ID 类型 */
        using WindowID = uint32_t;

        /** @brief 无效窗口 ID 常量 */
        static constexpr WindowID kInvalidWindowId = UINT32_MAX;

        /** @brief 虚析构，确保子类正确清理 */
        virtual ~IWindow() = default;

        // ─── 生命周期 ─────────────────────────────────────────────────────

        /**
         * @brief 初始化窗口
         *
         * 根据 WindowDesc 创建平台窗口。
         * 必须在创建后首先调用。
         *
         * @param[in] desc 窗口创建参数
         *
         * @return Error 错误码
         * @retval Error::kOK          初始化成功
         * @retval Error::kInvalidParameter 参数校验失败（尺寸过小等）
         * @retval Error::kFailed       平台 API 调用失败
         *
         * @pre desc.width >= 100 && desc.height >= 100
         * @post 窗口已创建且（如果 desc.visible=true）可见
         *
         * @note 重复调用会返回 Error::kFailed，需先 Shutdown()
         */
        virtual Error Initialize(const WindowDesc &desc) = 0;

        /**
         * @brief 关闭窗口并释放资源
         *
         * 销毁平台窗口句柄，释放所有关联资源。
         * 调用后窗口回到未初始化状态，可再次 Initialize()。
         *
         * @post 窗口句柄无效，ShouldClose() 返回 true
         */
        virtual void Shutdown() = 0;

        // ─── 事件处理 ─────────────────────────────────────────────────────

        /**
         * @brief 处理待处理事件
         *
         * 从系统事件队列中取出并分发事件，触发相应回调。
         * 应在主循环中每帧调用一次。
         *
         * @par 调用频率：
         *   推荐每帧调用一次。高频调用（每帧多次）会浪费 CPU，
         *   低频调用（多帧一次）会导致事件响应延迟。
         *
         * @note 即使没有事件，调用也是安全的（PeekMessage 模式）
         */
        virtual void PollEvents() = 0;

        /**
         * @brief 查询窗口是否应关闭
         *
         * 当用户点击关闭按钮或调用 Shutdown() 后返回 true。
         *
         * @return true 表示窗口应关闭，主循环应退出
         *
         * @par 使用模式：
         *   @code{.cpp}
         *   while (!window.ShouldClose()) {
         *       window.PollEvents();
         *       Render();
         *   }
         *   @endcode
         */
        virtual bool ShouldClose() const = 0;

        // ─── 属性查询 ─────────────────────────────────────────────────────

        /**
         * @brief 获取原生窗口句柄
         *
         * 返回平台特定的原生窗口句柄。
         * 渲染层使用此句柄创建图形上下文。
         *
         * @return 原生句柄指针
         * @retval Win32: HWND
         * @retval X11:   Window
         * @retval Android: ANativeWindow*
         *
         * @note 不要直接操作此句柄，仅供 RenderingContextDriver 使用
         */
        virtual void *GetNativeHandle() const = 0;

        /**
         * @brief 获取窗口客户区宽度
         *
         * @return 客户区宽度（像素），不包括标题栏和边框
         */
        virtual uint32_t GetWidth() const = 0;

        /**
         * @brief 获取窗口客户区高度
         *
         * @return 客户区高度（像素），不包括标题栏和边框
         */
        virtual uint32_t GetHeight() const = 0;

        /**
         * @brief 查询窗口是否可见
         *
         * @return true 表示窗口在屏幕上可见
         *
         * @note 最小化窗口返回 false
         */
        virtual bool IsVisible() const = 0;

        /**
         * @brief 查询窗口是否最小化
         *
         * @return true 表示窗口当前处于最小化（任务栏）状态
         */
        virtual bool IsMinimized() const = 0;

        /**
         * @brief 查询窗口是否最大化
         *
         * @return true 表示窗口当前处于最大化状态
         */
        virtual bool IsMaximized() const = 0;

        /**
         * @brief 获取窗口位置
         *
         * @param[out] x 窗口左上角 X 坐标（屏幕坐标）
         * @param[out] y 窗口左上角 Y 坐标（屏幕坐标）
         *
         * @note 全屏窗口返回屏幕原点 (0, 0)
         */
        virtual void GetPosition(int32_t &x, int32_t &y) const = 0;

        /**
         * @brief 获取窗口所在屏幕 ID
         *
         * @return 屏幕 ID，未关联时返回 UINT32_MAX
         *
         * @see IScreen::ScreenID
         */
        virtual uint32_t GetScreenId() const = 0;

        /**
         * @brief 获取窗口 DPI 缩放因子
         *
         * 返回系统报告的 DPI 缩放比例。
         * 用于高 DPI（HiDPI/Retina）显示器上正确渲染。
         *
         * @return 缩放因子
         * @retval 1.0  标准 DPI（96 DPI）
         * @retval 1.25 Windows 中等缩放（120 DPI）
         * @retval 1.5  Windows 较大缩放（144 DPI）
         * @retval 2.0  Retina 级别（192 DPI）
         *
         * @par 使用示例：
         *   @code{.cpp}
         *   float scale = window.GetDpiScale();
         *   uint32_t ui_width = static_cast<uint32_t>(window.GetWidth() * scale);
         *   @endcode
         *
         * @note AR HUD 场景通常需要精确 DPI 信息以正确渲染 HUD 元素
         */
        virtual float GetDpiScale() const = 0;

        // ─── 属性修改 ─────────────────────────────────────────────────────

        /**
         * @brief 调整窗口大小
         *
         * 改变窗口客户区尺寸。
         * 内部调用 SetWindowPos 实现。
         *
         * @param[in] width  新的客户区宽度（像素）
         * @param[in] height 新的客户区高度（像素）
         *
         * @note 全屏模式下通常忽略此调用
         */
        virtual void Resize(uint32_t width, uint32_t height) = 0;

        /**
         * @brief 设置窗口可见性
         *
         * @param[in] visible true 显示窗口，false 隐藏窗口
         *
         * @note 隐藏窗口不会触发 OnClose 回调
         */
        virtual void SetVisible(bool visible) = 0;

        /**
         * @brief 设置窗口标题
         *
         * @param[in] title 新标题（UTF-8 编码）
         *
         * @note Win32 平台限制标题最长约 65535 字符，实际通常更短
         */
        virtual void SetTitle(const char *title) = 0;

        /**
         * @brief 设置窗口位置
         *
         * 将窗口左上角移动到指定屏幕坐标。
         *
         * @param[in] x 窗口左上角 X 坐标（屏幕坐标）
         * @param[in] y 窗口左上角 Y 坐标（屏幕坐标）
         *
         * @note 全屏模式下通常忽略此调用
         */
        virtual void SetPosition(int32_t x, int32_t y) = 0;

        /**
         * @brief 设置全屏模式
         *
         * 切换窗口的全屏状态。
         * 不同模式切换可能涉及分辨率变更和显示器模式切换。
         *
         * @param[in] mode 目标全屏模式
         *
         * @par 模式切换行为：
         *   - kWindowed → kFullscreen: 隐藏边框，覆盖桌面，分辨率不变
         *   - kWindowed → kBorderless: 同 kFullscreen
         *   - kWindowed → kExclusiveFullscreen: 变更分辨率，可能短暂黑屏
         *   - 任意 → kWindowed: 恢复窗口模式和原始分辨率
         *
         * @see WindowMode
         */
        virtual void SetFullscreenMode(WindowMode mode) = 0;

        /**
         * @brief 获取当前全屏模式
         *
         * @return 当前窗口模式
         *
         * @see WindowMode
         */
        virtual WindowMode GetWindowMode() const = 0;

        // ─── 标识 ───────────────────────────────────────────────────────

        /**
         * @brief 获取窗口 ID
         *
         * @return 窗口 ID，由 DisplayServer 分配
         *
         * @see IDisplayServer::WindowCreate()
         */
        virtual WindowID GetWindowId() const = 0;

        /**
         * @brief 设置窗口 ID
         *
         * 由 DisplayServer 调用，为窗口分配唯一 ID。
         * 应用代码通常不需要调用此方法。
         *
         * @param[in] id 窗口 ID
         */
        virtual void SetWindowId(WindowID id) = 0;

        // ─── 回调 ───────────────────────────────────────────────────────

        /**
         * @brief 设置事件回调
         *
         * 安装窗口事件回调函数。
         * 通常在 Initialize() 之后、PollEvents() 之前调用。
         *
         * @param[in] callbacks 回调函数集合
         *
         * @see WindowCallbacks
         */
        virtual void SetCallbacks(const WindowCallbacks &callbacks) = 0;
    };

} // namespace arhud
