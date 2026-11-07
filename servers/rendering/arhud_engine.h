/**
 * @file arhud_engine.h
 * @brief AR HUD 渲染引擎 — 统一入口（参考 Godot Main + MainLoop）
 *
 * ARHudEngine 是渲染系统的顶层入口，内部创建并拥有所有平台/驱动资源，
 * 应用层只需提供简单配置即可启动引擎。
 *
 * @par 架构概览
 *   @code
 *   ARHudEngine (统一入口)
 *   ├── Impl (PIMPL)
 *   │   ├── Win32DisplayServer / GLContextManagerWin32 /
 * GLRenderingContextDriver │   └── 共享渲染栈（B3-M0.1：从 RenderViewport
 * 上提） │       ├── RenderingDevice rd          ×1 全局 │       ├──
 * CommandQueue  ×1 │       ├── ARHudCompositor             ×1（全部 Storage +
 * Renderer） ├── ItemStorage                 ×1（全局 RID） └──
 * RenderingServer（门面）      ×1（app 唯一入口，内部委托旧 RenderServer） ├──
 * RenderViewport[0]  ──► HUD 屏幕（轻量：surface + 后处理 + EngineStackRefs
 * 指针） └── RenderViewport[1]  ──► 仪表盘（同上，共享同一套栈）
 *   @endcode
 *
 * @par 方式 A：手动帧循环（demo 采用）
 *   @code
 *   ARHudEngine engine;
 *   ARHudEngine::Config cfg;
 *   cfg.vsync = true;
 *   engine.Initialize(cfg);              // 引擎级资源（不创建窗口）
 *
 *   ARHudEngine::ScreenConfig scfg;      // B3-M0.2：窗口参数在 ScreenConfig
 *   scfg.title = "My AR HUD";
 *   scfg.width = 1920; scfg.height = 720;
 *   engine.AddScreen(scfg);              // 建窗口 + surface + context + 共享栈
 *
 *   RenderingServer *rs = engine.GetRenderServer();  // 资源/保留模式 item
 * 唯一入口 while (!engine.ShouldClose()) { engine.BeginFrame();
 *       // 经 rs 创建/更新/释放资源与 item（保留模式，无需每帧重建）
 *       rs->SubmitScene();               // B3：帧提交信号（异步，立即返回）
 *       engine.EndFrame();
 *   }
 *   engine.Finalize();
 *   @endcode
 *
 * @par 方式 B：IAppLoop 回调（可选；引擎驱动帧循环）
 *   @code
 *   class MyApp : public IAppLoop {
 *   public:
 *       RenderingServer *rs = nullptr;   // 由外部注入
 *       void OnUpdate(float32 dt) override { ... }
 *       void OnRender() override { rs->SubmitScene(); }  // 每帧须提交本帧
 *   };
 *   ARHudEngine engine;
 *   engine.Initialize(cfg);
 *   engine.AddScreen(scfg);
 *   MyApp my_app;
 *   my_app.rs = engine.GetRenderServer();
 *   engine.Run(my_app);
 *   engine.Finalize();
 *   @endcode
 *
 * @par 所有权
 *   - ARHudEngine 拥有所有平台/驱动资源（通过 PIMPL 隔离）
 *   - ARHudEngine
 * 拥有共享渲染栈（RD/CommandQueue/Compositor/ItemStorage/RenderingServer 门面，
 *     B3-M0.1 从 RenderViewport 上提，所有屏共享一套——RID 全局、VRAM 不重复）
 *   - ARHudEngine 拥有所有 RenderViewport 实例（轻量视口，收 EngineStackRefs
 * 指针）
 *   - 应用层无需关心 DisplayServer、RCD 等底层细节
 *
 * @see IAppLoop           应用循环接口
 * @see RenderViewport       单屏渲染管线
 * @see ARHudCompositor    HUD 场景合成器
 *
 * @author yameng.he
 * @version 3.0
 * @date 2026-08-24
 * @date 2026-08-15
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_ARHUD_ENGINE_H_
#define ARHUD_SERVERS_RENDERING_ARHUD_ENGINE_H_

#include "render_viewport.h"
#include "typedefs.h"
#include "viewport_provider.h"

namespace arhud {

/**
 * @brief 应用循环接口（参考 Godot MainLoop）
 *
 * 分离引擎帧循环与应用逻辑的关键抽象。
 * 应用层实现此接口，引擎在每帧调用 OnUpdate/OnRender。
 *
 * @par 生命周期
 *   1. 引擎调用 OnUpdate(delta_time) — 应用逻辑更新
 *   2. 引擎调用 OnRender() — 应用经 RenderServer 发渲染命令（Set* /
 * SubmitScene）
 *
 * @par 用法
 *   @code
 *   class MyApp : public IAppLoop {
 *       void OnUpdate(float32 dt) override { /* 动画、输入响应 *\/ }
 *       void OnRender() override { /* RenderServer Set* / SubmitScene *\/ }
 *   };
 *   engine.Run(my_app);
 *   @endcode
 */
class IAppLoop {
 public:
  virtual ~IAppLoop() = default;

  /**
   * @brief 每帧逻辑更新
   * @param[in] p_delta_time 距上一帧的时间（秒）
   */
  virtual void OnUpdate(float32 p_delta_time) = 0;

  /**
   * @brief 每帧渲染操作
   * @note 在此方法中经 RenderServer 发 item/资源增量命令（Set* /
   * SubmitScene）等
   */
  virtual void OnRender() = 0;
};

/**
 * @brief AR HUD 渲染引擎 — 统一入口
 *
 * 内部创建并拥有所有平台/驱动资源（DisplayServer、RCD 等），
 * 应用层只需提供简单配置即可启动引擎。
 *
 * @par 生命周期
 *   1. 构造 ARHudEngine（默认构造，无副作用）
 *   2. Initialize(config) — 创建平台资源（DisplayServer/GLContextManager）
 *   3. AddScreen(screen_config) — 创建窗口 + 共享栈 + RenderViewport
 *   4. 帧循环：BeginFrame() → 渲染操作 → EndFrame()
 *      或使用 Run(IAppLoop&) 引擎驱动帧循环
 *   5. Finalize() — 销毁所有资源
 *   6. 析构时若仍初始化，自动调用 Finalize()
 *
 * @note 不可复制、不可移动（ARHUD_DISABLE_COPY_MOVE）。
 */
class ARHudEngine : public IViewportProvider {
  ARHUD_DISABLE_COPY_MOVE(ARHudEngine);

 public:
  /** @brief 屏幕 ID 类型，与 RenderViewport::ID 一致。 */
  using ViewportID = RenderViewport::ID;

  /**
   * @brief 引擎初始化配置（B3-M0.2：仅引擎级参数）
   *
   * 窗口参数（title/width/height）已移至 ScreenConfig，由 AddScreen 传入。
   *
   * @par 典型配置：
   *   @code
   *   ARHudEngine::Config cfg;
   *   cfg.vsync = true;
   *   engine.Initialize(cfg);
   *   @endcode
   */
  struct Config {
    bool vsync = true;
    Thread::Priority render_thread_priority = Thread::kPriorityHigh;
  };

  /**
   * @brief 屏幕创建配置（B3-M0.2：窗口参数从 Engine::Config 移出）
   *
   * AddScreen 时传入，引擎内部创建窗口 + surface + context + RenderViewport。
   *
   * @par 典型用法：
   *   @code
   *   ARHudEngine::ScreenConfig scfg;
   *   scfg.title = "My AR HUD";
   *   scfg.width = 1920;
   *   scfg.height = 720;
   *   engine.AddScreen(scfg);
   *   @endcode
   */
  struct ScreenConfig {
    const char* title = "AR HUD";
    uint32 width = 1280;
    uint32 height = 720;
  };

  /** @brief 默认构造。需随后调用 Initialize() 完成初始化。 */
  ARHudEngine() = default;

  /**
   * @brief 析构函数。若仍处于初始化状态，自动调用 Finalize()。
   *
   * @note 建议在析构前显式调用 Finalize()，以确保
   *       IRenderingContextDriver / IDisplayServer 仍有效时完成资源释放。
   */
  ~ARHudEngine();

  /**
   * @brief 初始化引擎（仅引擎级资源，不创建窗口）
   *
   * 内部创建 DisplayServer、GLContextManager。
   * 不创建窗口/RCD/共享栈——这些在首次 AddScreen 时延迟初始化。
   *
   * @param[in] p_config 引擎配置
   *
   * @retval Error::kOK              初始化成功
   * @retval Error::kAlreadyExists   重复初始化
   * @retval Error::kFailed          平台初始化失败
   *
   * @post initialized_ == true
   * @post screens_ 为空（需调用 AddScreen 创建屏幕）
   */
  Error Initialize(const Config& p_config);

  /**
   * @brief 终止引擎并销毁所有屏幕
   *
   * @par 执行流程：
   *   1. 若未初始化，直接返回
   *   2. 遍历 screens_，对每个 RenderViewport 调用 Finalize() + ARHUD_DELETE
   *   3. 清空 screens_ 数组
   *   4. 置空 context_driver_ / display_server_ / window_id_
   *   5. 置 initialized_ = false
   *
   * @pre 若已初始化，IRenderingContextDriver 和 IDisplayServer 仍有效
   *
   * @post initialized_ == false
   * @post screens_ 为空
   * @post 所有 RenderViewport 已销毁
   *
   * @note 安全调用：若未初始化则直接返回，不执行任何操作。
   */
  void Finalize();

  /**
   * @brief 查询初始化状态
   *
   * @return true 已初始化，false 未初始化
   */
  bool IsInitialized() const;

  /**
   * @brief 查询是否应该关闭引擎
   *
   * 检查默认窗口是否收到关闭信号（用户点击关闭、系统销毁等）。
   *
   * @par 异常安全：
   *   若引擎未初始化或 display_server_ 为空，返回 true（安全退出）。
   *   若窗口已销毁（GetWindow 返回 nullptr），也返回 true。
   *
   * @return true 应关闭（窗口收到关闭信号或异常状态），false 继续运行
   */
  bool ShouldClose() const;

  /**
   * @brief 添加屏幕（创建窗口 + surface + context + RenderViewport）
   *
   * 首次调用时延迟初始化 RCD + 共享栈（RD/Compositor/CommandQueue/...）。
   * 后续调用复用共享栈，仅创建新窗口 + surface + RenderViewport。
   *
   * @param[in] p_config 屏幕配置（title/width/height）
   *
   * @return 新屏幕的 ViewportID，失败返回 kInvalidId
   *
   * @pre Initialize() 已成功调用
   */
  ViewportID AddScreen(const ScreenConfig& p_config);

  /**
   * @brief 获取指定屏幕
   *
   * @param[in] p_id 屏幕 ID（即 screens_ 数组下标）
   *
   * @return RenderViewport 指针，无效 ID 返回 nullptr
   */
  RenderViewport* GetScreen(ViewportID p_id);

  /**
   * @brief IViewportProvider 实现（ADR-0007）：按 ViewportID 解析
   * RenderViewport
   *
   * 由 RenderServer::Apply* 经注入的 IViewportProvider 调用
   * （相机/灯光命令按视口索引落账）。等价于 GetScreen(p_index)。
   *
   * @param[in] p_index 视口索引（即 ViewportID / screens_ 数组下标）
   * @return RenderViewport 指针，越界返回 nullptr
   */
  RenderViewport* GetViewport(uint32 p_index) override;

  /**
   * @brief 获取屏幕数量
   *
   * @return 当前已创建的屏幕数量（含默认屏幕）
   */
  uint32 GetScreenCount() const;

  /**
   * @brief 开始新帧（所有屏幕）
   *
   * @par 执行流程：
   *   1. DisplayServer::ProcessEvents() — 处理窗口输入事件
   *   2. 首次调用启动 Engine 级渲染线程；此后仅推进 Compositor 帧节拍
   *      （B3 起帧编排归渲染线程 RenderThreadFunc，不再逐屏调用 BeginFrame）
   *
   * @pre Initialize() 已成功调用
   *
   * @note 若未初始化，直接返回（安全操作）。
   */
  void BeginFrame();

  /**
   * @brief 结束当前帧（所有屏幕）
   *
   * 推进 Compositor 帧节拍（B3 起帧编排归渲染线程 RenderThreadFunc）。
   * 渲染线程完成渲染后经每屏 present 线程上屏（IFramePresenter，从不
   * SwapBuffers）。
   *
   * @pre BeginFrame() 已调用
   *
   * @note 若未初始化，直接返回（安全操作）。
   */
  void EndFrame();

  /**
   * @brief 引擎驱动帧循环（参考 Godot Main::iteration）
   *
   * 内部执行：ProcessEvents → OnUpdate → OnRender → 提交帧（SubmitScene；上屏由
   * present 线程完成） 直到 ShouldClose() 返回 true。
   *
   * @param[in] p_app_loop 应用循环回调
   *
   * @pre Initialize() 已成功调用
   */
  void Run(IAppLoop& p_app_loop);

  /**
   * @brief 获取默认屏幕（ViewportID=0）的渲染服务
   *
   * app 层资源创建/变更/释放的推荐入口：等价于
   * GetScreen(0)->GetRenderServer()。 S0 起返回 RenderingServer
   * 门面（内部委托旧 RenderServer，行为不变）； 亦可经
   * RenderingServer::GetSingleton() 获取。
   *
   * @return 渲染服务指针（RenderingServer 门面），无屏幕时返回 nullptr
   */
  RenderingServer* GetRenderServer();

  /**
   * @brief 获取默认屏幕（ViewportID=0）的宽度
   *
   * 便捷方法，等价于 GetScreen(0)->GetWidth()。
   *
   * @return 宽度（像素），无屏幕时返回 0
   */
  uint32 GetWidth() const;

  /**
   * @brief 获取默认屏幕（ViewportID=0）的高度
   *
   * 便捷方法，等价于 GetScreen(0)->GetHeight()。
   *
   * @return 高度（像素），无屏幕时返回 0
   */
  uint32 GetHeight() const;

 private:
  /** @brief B3: Engine 级渲染线程入口 */
  static void RenderThreadEntry(void* p_userdata);
  void RenderThreadFunc();

  /**
   * @brief 外部重绘请求 → 强制全屏重渲（ADR-0008）
   *
   * 窗口 WM_PAINT / WM_SIZE 经 WindowCallbacks 回调到**主线程**时调用。
   * 主线程只置原子标志 force_redraw_ 并唤醒渲染线程（**不触碰
   * ItemStorage**——渲染线程独占）；实际置屏位由渲染线程消费标志时执行
   * （`MarkAllScreens()`）。
   */
  void RequestFullRedraw();

  /** @brief WindowCallbacks::OnPaint → RequestFullRedraw 的静态桥（ADR-0008）
   */
  static void OnWindowPaint(uint32 p_id, void* p_userdata);
  /** @brief WindowCallbacks::OnResize → RequestFullRedraw 的静态桥（ADR-0008）
   */
  static void OnWindowResize(uint32 p_id, int32 p_width, int32 p_height,
                             void* p_userdata);

  /**
   * @brief 平台实现数据（PIMPL）
   *
   * 定义在 arhud_engine.cpp 中，包含：
   * - Win32DisplayServer（拥有所有权）
   * - GLContextManagerWin32（拥有所有权）
   * - GLRenderingContextDriver（拥有所有权）
   * - IWindow::WindowID（默认窗口 ID）
   *
   * 所有平台/驱动类型通过 PIMPL 隔离在 .cpp 中，
   * 头文件不暴露任何平台/驱动类型。
   */
  struct Impl;
  Impl* impl_ = nullptr;
  bool initialized_ = false;              ///< 是否已初始化
  LocalVector<RenderViewport*> screens_;  ///< 屏幕指针数组（拥有所有权）
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_ARHUD_ENGINE_H_
