/**
 * @file arhud_engine.cpp
 * @brief AR HUD 渲染引擎实现（参考 Godot Main + MainLoop）
 *
 * ARHudEngine 内部创建并拥有所有平台/驱动资源，
 * 应用层只需提供简单配置即可启动引擎。
 *
 * @par 关键设计决策：
 *
 *   1. PIMPL 隔离：所有平台/驱动类型（DisplayServer、RCD 等）
 *      通过 Impl 结构体隔离在 .cpp 中，头文件不暴露任何
 *      平台/驱动类型。
 *
 *   2. 引擎拥有资源：Initialize() 内部创建 DisplayServer、Window、
 *      RCD、Surface、Context、共享渲染栈（RD/CommandQueue/Compositor/
 *      ItemStorage/RenderServer），Finalize() 时逆序释放。
 *
 *   3. IAppLoop 接口：参考 Godot MainLoop，分离引擎帧循环
 *      与应用逻辑。Run() 方法驱动帧循环直到 ShouldClose()。
 *
 *   4. 共享渲染栈（B3-M0.1）：RD/CommandQueue/Compositor/ItemStorage/
 *      RenderServer 从 RenderViewport 上提到 Impl，所有屏共享一套——
 *      RID 全局、VRAM 不重复、渲染线程只搬一次。RenderViewport 改收
 *      EngineStackRefs 指针，不再自建栈。
 *
 * @see arhud_engine.h
 *
 * @author yameng.he
 * @version 3.0
 * @date 2026-08-24
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "arhud_engine.h"

#include <chrono>

#include "io/logger.h"
#include "os/memory.h"

#ifdef ARHUD_PLATFORM_WINDOWS
#include "windows/display_server_win32.h"
#include "windows/gl_context_manager_win32.h"
#endif

#ifdef ARHUD_API_OPENGL
#include "gl/rendering_context_driver_gl.h"
#endif

#ifdef ARHUD_PLATFORM_WINDOWS
#include "windows/frame_presenter_win32.h"
#endif

#include "frame_image_queue.h"
#include "present_thread.h"
#include "render_globals.h"
#include "rendering_server_default.h"

namespace arhud {

struct ARHudEngine::Impl {
#ifdef ARHUD_PLATFORM_WINDOWS
  Win32DisplayServer display_server;
  GLContextManagerWin32 gl_context_manager;
#endif

#ifdef ARHUD_API_OPENGL
  GLRenderingContextDriver context_driver;
  Impl() : context_driver(&gl_context_manager) {}
#endif

  IWindow::WindowID window_id = IWindow::kInvalidWindowId;
  uint32 width = 0;
  uint32 height = 0;

  // B3-M0.2: 引擎级配置（从 Config 存储，AddScreen 时使用）
  bool vsync = true;
  Thread::Priority render_thread_priority = Thread::kPriorityHigh;

  // ── B3-M0.1: Engine 级共享栈（从 RenderViewport 上提）──
  RenderingDevice rd;
  CommandQueue command_queue;
  ItemStorage item_storage;
  RenderingServerDefault rendering_server;
  ARHudCompositor compositor;

  // 共享栈是否已创建（Initialize 建一次，所有 RenderViewport 共享）
  bool stack_initialized = false;

  // ── B3-M0.3: 离屏 GL context（隐藏窗口，所有屏共享）──
  IWindow::WindowID offscreen_window_id = IWindow::kInvalidWindowId;
  IRenderingContextDriver::SurfaceID offscreen_surface_id =
      IRenderingContextDriver::kInvalidSurfaceId;
  IRenderingContextDriver::ContextID offscreen_context_id =
      IRenderingContextDriver::kInvalidContextId;

  // ── B3: Engine 级渲染线程 + per-screen present 基础设施 ──
  Thread render_thread_;
  std::atomic<bool> render_thread_running_{false};
  Semaphore render_thread_ready_;
  Semaphore render_wake_;  ///< B3: 渲染线程唤醒（三路：present 释放 / 命令 push
                           ///< / 启动 pre-post）
  Semaphore frame_done_;
  std::atomic<bool> submit_pending_{
      false};  ///< B3: kSubmitScene 信号（executor 置位，本线程消费）
  std::atomic<bool> force_redraw_{
      false};  ///< ADR-0008:
               ///< 外部重绘请求（窗口回调置位，渲染线程消费，强制全屏重渲）
  bool render_thread_started_ = false;

  LocalVector<FrameImageQueue*> frame_queues_;
  LocalVector<PresentThread*> present_threads_;
  LocalVector<IFramePresenter*> presenters_;

  EngineStackRefs GetStackRefs(IFramePresenter* p_presenter) {
    return EngineStackRefs{&rd,
                           &command_queue,
                           &item_storage,
                           &rendering_server,
                           &compositor,
                           offscreen_surface_id,
                           offscreen_context_id,
                           p_presenter};
  }
};

ARHudEngine::~ARHudEngine() {
  if (initialized_) {
    Finalize();
  }
}

Error ARHudEngine::Initialize(const Config& p_config) {
  if (initialized_) {
    return Error::kAlreadyExists;
  }

  impl_ = ARHUD_NEW(Impl);

#ifdef ARHUD_PLATFORM_WINDOWS
  // 1. DisplayServer
  Error err = impl_->display_server.Initialize();
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed", "DisplayServer initialization failed");
    ARHUD_DELETE(impl_);
    impl_ = nullptr;
    return Error::kFailed;
  }

  // 2. GLContextManager
  err = impl_->gl_context_manager.Initialize();
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed", "GLContextManager initialization failed");
    impl_->display_server.Shutdown();
    ARHUD_DELETE(impl_);
    impl_ = nullptr;
    return Error::kFailed;
  }

  // 3. 离屏隐藏窗口（B3-M0.3：GL context 载体，不显示）
  WindowDesc offscreen_desc;
  offscreen_desc.width = 100;  // Win32Window 最小 100×100
  offscreen_desc.height = 100;
  offscreen_desc.title = "ARHud Offscreen";
  impl_->offscreen_window_id =
      impl_->display_server.WindowCreate(offscreen_desc);
  if (impl_->offscreen_window_id == IWindow::kInvalidWindowId) {
    ARHUD_LOG_ERROR("kFailed", "Offscreen window creation failed");
    impl_->gl_context_manager.Shutdown();
    impl_->display_server.Shutdown();
    ARHUD_DELETE(impl_);
    impl_ = nullptr;
    return Error::kFailed;
  }
  // 离屏窗口隐藏（ShowWindow SW_HIDE）
  IWindow* offscreen_win =
      impl_->display_server.GetWindow(impl_->offscreen_window_id);
  if (offscreen_win) {
    offscreen_win->SetVisible(false);
  }
  void* offscreen_handle =
      (offscreen_win != nullptr) ? offscreen_win->GetNativeHandle() : nullptr;
#endif

#ifdef ARHUD_API_OPENGL
  // 4. RCD（需要 HWND 加载 GL 函数）
  err = impl_->context_driver.Initialize(offscreen_handle);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed", "RCD initialization failed");
    impl_->display_server.WindowDestroy(impl_->offscreen_window_id);
    impl_->gl_context_manager.Shutdown();
    impl_->display_server.Shutdown();
    ARHUD_DELETE(impl_);
    impl_ = nullptr;
    return Error::kFailed;
  }

  // 5. 离屏 surface + context（GL 像素格式设在隐藏窗口上）
  IScreen::ScreenID screen_id = impl_->display_server.GetPrimaryScreen();
  impl_->offscreen_surface_id = impl_->context_driver.SurfaceCreate(
      screen_id, offscreen_handle, 100, 100);
  if (impl_->offscreen_surface_id ==
      IRenderingContextDriver::kInvalidSurfaceId) {
    ARHUD_LOG_ERROR("kFailed", "Offscreen surface creation failed");
    impl_->context_driver.Shutdown();
    impl_->display_server.WindowDestroy(impl_->offscreen_window_id);
    impl_->gl_context_manager.Shutdown();
    impl_->display_server.Shutdown();
    ARHUD_DELETE(impl_);
    impl_ = nullptr;
    return Error::kFailed;
  }

  impl_->offscreen_context_id =
      impl_->context_driver.ContextCreate(impl_->offscreen_surface_id);
  if (impl_->offscreen_context_id ==
      IRenderingContextDriver::kInvalidContextId) {
    ARHUD_LOG_ERROR("kFailed", "Offscreen context creation failed");
    impl_->context_driver.SurfaceDestroy(impl_->offscreen_surface_id);
    impl_->context_driver.Shutdown();
    impl_->display_server.WindowDestroy(impl_->offscreen_window_id);
    impl_->gl_context_manager.Shutdown();
    impl_->display_server.Shutdown();
    ARHUD_DELETE(impl_);
    impl_ = nullptr;
    return Error::kFailed;
  }

  // 6. 共享栈（RD + Compositor + Executor + Queue + Server）
  // RD::Initialize 内部完成 offscreen context 的首次 MakeCurrent；
  // 渲染线程入口（RenderThreadFunc）与栈销毁前会经 RCD 重新绑定。
  err = impl_->rd.Initialize(&impl_->context_driver, 2, 4 * 1024 * 1024,
                             impl_->offscreen_context_id,
                             impl_->offscreen_surface_id);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed", "RD initialization failed");
    goto fail_stack;
  }

  err = impl_->compositor.Initialize(&impl_->rd);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed", "Compositor initialization failed");
    impl_->rd.Finalize();
    goto fail_stack;
  }

  impl_->rendering_server.Initialize(&impl_->command_queue, &impl_->compositor,
                                     &impl_->item_storage, this);

  // S0: 装配 RenderingServer 全局单例（app 可经 RenderingServer::GetSingleton()
  // 访问）
  RenderingServer::singleton = &impl_->rendering_server;

  // ADR-0010: 装配渲染层全局可达点（RSG 式平铺），供命令实现体直达存储
  RenderGlobals::shader_storage = impl_->compositor.GetShaderStorage();
  RenderGlobals::texture_storage = impl_->compositor.GetTextureStorage();
  RenderGlobals::material_storage = impl_->compositor.GetMaterialStorage();
  RenderGlobals::mesh_storage = impl_->compositor.GetMeshStorage();
  RenderGlobals::font_storage = impl_->compositor.GetFontStorage();
  RenderGlobals::canvas_renderer = impl_->compositor.GetCanvasRenderer();
  RenderGlobals::spatial_renderer = impl_->compositor.GetSpatialRenderer();
  RenderGlobals::item_storage = &impl_->item_storage;
  RenderGlobals::viewport_provider = this;
  RenderGlobals::submit_signal = &impl_->submit_pending_;

  impl_->stack_initialized = true;
#endif

  impl_->vsync = p_config.vsync;
  impl_->render_thread_priority = p_config.render_thread_priority;

  initialized_ = true;
  ARHUD_LOG_INFO(
      "ARHudEngine initialized (offscreen GL context + shared stack, no screen "
      "yet)");
  return Error::kOK;

#ifdef ARHUD_API_OPENGL
fail_stack:
  impl_->context_driver.ContextDestroy(impl_->offscreen_context_id);
  impl_->context_driver.SurfaceDestroy(impl_->offscreen_surface_id);
  impl_->context_driver.Shutdown();
  impl_->display_server.WindowDestroy(impl_->offscreen_window_id);
  impl_->gl_context_manager.Shutdown();
  impl_->display_server.Shutdown();
  ARHUD_DELETE(impl_);
  impl_ = nullptr;
  return Error::kFailed;
#endif
}

ARHudEngine::ViewportID ARHudEngine::AddScreen(const ScreenConfig& p_config) {
  if (!initialized_ || impl_ == nullptr) {
    return RenderViewport::kInvalidId;
  }

#ifdef ARHUD_API_OPENGL
  // 1. 创建可见窗口（不设 GL 像素格式——不调 SurfaceCreate）
  WindowDesc window_desc;
  window_desc.width = p_config.width;
  window_desc.height = p_config.height;
  window_desc.title = p_config.title;

  IWindow::WindowID wid = impl_->display_server.WindowCreate(window_desc);
  if (wid == IWindow::kInvalidWindowId) {
    ARHUD_LOG_ERROR("kFailed", "AddScreen: window creation failed");
    return RenderViewport::kInvalidId;
  }

  // 首屏的 window_id 存一份（ShouldClose 用）
  if (impl_->window_id == IWindow::kInvalidWindowId) {
    impl_->window_id = wid;
  }

  // 2. 创建 IFramePresenter（GDI 上屏，纯平台无 GL）
  IWindow* window = impl_->display_server.GetWindow(wid);
  void* native_handle =
      (window != nullptr) ? window->GetNativeHandle() : nullptr;

  // ADR-0008: 注册窗口重绘回调——WM_PAINT / WM_SIZE → 强制全屏重渲。
  // 回调在主线程（BeginFrame → ProcessEvents），故只置原子标志 + 唤醒
  // 渲染线程；置屏位由渲染线程消费标志时完成。
  if (window != nullptr) {
    WindowCallbacks wc{};
    wc.userdata = this;
    wc.OnPaint = &ARHudEngine::OnWindowPaint;
    wc.OnResize = &ARHudEngine::OnWindowResize;
    window->SetCallbacks(wc);
  }

  IFramePresenter* presenter = ARHUD_NEW(FramePresenterWin32);
  Error err =
      presenter->Initialize(native_handle, p_config.width, p_config.height);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "AddScreen: IFramePresenter initialization failed");
    ARHUD_DELETE(presenter);
    impl_->display_server.WindowDestroy(wid);
    return RenderViewport::kInvalidId;
  }

  // 3. RenderViewport（收离屏 context + presenter + 共享栈指针）
  RenderViewport::Config screen_config;
  screen_config.width = p_config.width;
  screen_config.height = p_config.height;

  auto* screen = ARHUD_NEW(RenderViewport);
  err = screen->Initialize(&impl_->context_driver, screen_config,
                           impl_->GetStackRefs(presenter));
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "AddScreen: RenderViewport initialization failed");
    ARHUD_DELETE(screen);
    presenter->Finalize();
    ARHUD_DELETE(presenter);
    impl_->display_server.WindowDestroy(wid);
    return RenderViewport::kInvalidId;
  }

  // B3: 创建 per-screen FrameImageQueue + PresentThread
  auto* queue = ARHUD_NEW(FrameImageQueue);
  queue->Initialize(p_config.width, p_config.height);
  queue->SetWakeSemaphore(
      &impl_->render_wake_);  // B3: 释放/启动事件唤醒渲染线程
  screen->SetFrameImageQueue(queue);

  auto* present_thread = ARHUD_NEW(PresentThread);
  present_thread->Initialize(queue, presenter);

  ViewportID id = static_cast<ViewportID>(screens_.Size());
  screens_.PushBack(screen);
  screen->SetViewportIndex(id);
  impl_->frame_queues_.PushBack(queue);
  impl_->present_threads_.PushBack(present_thread);
  impl_->presenters_.PushBack(presenter);

  // P0-2: 如果渲染线程已启动，给新 queue pre-post
  if (impl_->render_thread_started_) {
    queue->PostRenderWake();
  }

  ARHUD_LOG_INFO("AddScreen: screen %u created (%ux%u \"%s\")", id,
                 p_config.width, p_config.height, p_config.title);
  return id;
#else
  return RenderViewport::kInvalidId;
#endif
}

void ARHudEngine::Finalize() {
  if (!initialized_) {
    return;
  }

  // 1. 停止渲染线程（B3: Engine 级）
  if (impl_->render_thread_started_) {
    impl_->render_thread_running_.store(false, std::memory_order_release);
    impl_->render_wake_.Post();
    impl_->render_thread_.WaitToFinish();
    impl_->render_thread_started_ = false;
  }

  // 2. 停止 present 线程（per-screen）
  for (uint32 i = 0; i < impl_->present_threads_.Size(); ++i) {
    impl_->present_threads_[i]->Finalize();
    ARHUD_DELETE(impl_->present_threads_[i]);
  }
  impl_->present_threads_.Clear();

  // 3. 停止 RenderViewport + 释放 FrameImageQueue
  for (uint32 i = 0; i < screens_.Size(); ++i) {
    screens_[i]->Finalize();
    ARHUD_DELETE(screens_[i]);
  }
  screens_.Clear();

  // 4. 释放 FrameImageQueue + IFramePresenter
  for (uint32 i = 0; i < impl_->frame_queues_.Size(); ++i) {
    impl_->frame_queues_[i]->Finalize();
    ARHUD_DELETE(impl_->frame_queues_[i]);
  }
  impl_->frame_queues_.Clear();

  for (uint32 i = 0; i < impl_->presenters_.Size(); ++i) {
    impl_->presenters_[i]->Finalize();
    ARHUD_DELETE(impl_->presenters_[i]);
  }
  impl_->presenters_.Clear();

  if (impl_) {
    // 2. 销毁共享栈（MakeCurrent 离屏 context 确保 GL 状态有效）
    if (impl_->stack_initialized) {
      impl_->context_driver.MakeCurrent(impl_->offscreen_context_id,
                                        impl_->offscreen_surface_id);
      impl_->rd.MakeRenderThread();

      RenderingServer::singleton =
          nullptr;  // S0: 撤销全局单例（共享栈即将释放）
      impl_->rendering_server.Finalize();
      RenderGlobals::Reset();  // ADR-0010: 共享栈即将释放，先清空全局可达点
      impl_->compositor.Finalize();
      impl_->rd.Finalize();
      impl_->stack_initialized = false;
    }

#ifdef ARHUD_API_OPENGL
    // 3. 销毁离屏 context + surface
    if (impl_->offscreen_context_id !=
        IRenderingContextDriver::kInvalidContextId) {
      impl_->context_driver.ContextDestroy(impl_->offscreen_context_id);
    }
    if (impl_->offscreen_surface_id !=
        IRenderingContextDriver::kInvalidSurfaceId) {
      impl_->context_driver.SurfaceDestroy(impl_->offscreen_surface_id);
    }
    impl_->context_driver.Shutdown();
#endif
#ifdef ARHUD_PLATFORM_WINDOWS
    // 4. 销毁窗口（离屏 + 可见）
    if (impl_->offscreen_window_id != IWindow::kInvalidWindowId) {
      impl_->display_server.WindowDestroy(impl_->offscreen_window_id);
    }
    if (impl_->window_id != IWindow::kInvalidWindowId) {
      impl_->display_server.WindowDestroy(impl_->window_id);
    }
    impl_->gl_context_manager.Shutdown();
    impl_->display_server.Shutdown();
#endif
    ARHUD_DELETE(impl_);
    impl_ = nullptr;
  }

  initialized_ = false;
  ARHUD_LOG_INFO("ARHudEngine finalized");
}

bool ARHudEngine::IsInitialized() const {
  return initialized_;
}

bool ARHudEngine::ShouldClose() const {
  if (!initialized_ || impl_ == nullptr) {
    return true;
  }
#ifdef ARHUD_PLATFORM_WINDOWS
  // B3-M0.2: 窗口在 AddScreen 创建，首屏的 window_id 存在 impl_
  if (impl_->window_id == IWindow::kInvalidWindowId) {
    return true;  // 无屏幕
  }
  IWindow* window = impl_->display_server.GetWindow(impl_->window_id);
  return (window == nullptr || window->ShouldClose());
#else
  return true;
#endif
}

// 旧 AddScreen(const RenderViewport::Config &) 已移除——B3-M0.2 改为
// AddScreen(const ScreenConfig &)，内部创建窗口+surface+context+RenderViewport

RenderViewport* ARHudEngine::GetScreen(ViewportID p_id) {
  if (p_id >= screens_.Size()) {
    return nullptr;
  }
  return screens_[p_id];
}

RenderViewport* ARHudEngine::GetViewport(uint32 p_index) {
  return GetScreen(static_cast<ViewportID>(p_index));
}

uint32 ARHudEngine::GetScreenCount() const {
  return screens_.Size();
}

void ARHudEngine::BeginFrame() {
  if (!initialized_ || impl_ == nullptr) {
    return;
  }

#ifdef ARHUD_PLATFORM_WINDOWS
  impl_->display_server.ProcessEvents();
#endif

  // B3: resize 检测（通过 IWindow，不依赖 RCD surface）
  if (impl_->window_id != IWindow::kInvalidWindowId) {
    IWindow* win = impl_->display_server.GetWindow(impl_->window_id);
    if (win != nullptr && !screens_.IsEmpty()) {
      uint32 win_w = win->GetWidth();
      uint32 win_h = win->GetHeight();
      if (win_w > 0 && win_h > 0 &&
          (win_w != screens_[0]->GetWidth() ||
           win_h != screens_[0]->GetHeight())) {
        screens_[0]->HandleResize(win_w, win_h);
      }
    }
  }

  // B3: 启动 Engine 级渲染线程（首次 BeginFrame）
  if (!impl_->render_thread_started_) {
    impl_->context_driver.ReleaseCurrent();
    impl_->render_thread_running_.store(true, std::memory_order_release);
    impl_->render_thread_.Start(RenderThreadEntry, this,
                                {Thread::kPriorityHigh});
    impl_->render_thread_ready_.Wait();
    impl_->command_queue.SetNotifySemaphore(&impl_->render_wake_);
    impl_->rd.SetCommandQueue(&impl_->command_queue);
    impl_->render_thread_started_ = true;
  }

  // B3: 主线程只发命令（渲染决策全部在渲染线程）。
  // RD 帧簿记（staging/延迟销毁）随渲染搬入渲染线程
  // （RenderThreadFunc 每轮 BeginFrame/EndFrame）；此处仅 Compositor 节拍回调。
  impl_->compositor.BeginFrame(1.0 / 60.0);
}

void ARHudEngine::EndFrame() {
  if (!initialized_) {
    return;
  }
  // B3: 主线程不碰 RD（渲染线程独占）。SubmitScene 信号已由 app 经
  // RenderServer::SubmitScene() 异步推送；渲染线程唤醒源 = render_wake_。

  impl_->compositor.EndFrame();
}

// ── B3: Engine 级渲染线程 ──

void ARHudEngine::RenderThreadEntry(void* p_userdata) {
  auto* self = reinterpret_cast<ARHudEngine*>(p_userdata);
  self->RenderThreadFunc();
}

/**
 * @brief B3 渲染线程主循环（§3.5 of multiscreen）
 *
 * @par 循环形状
 *   render_wake_.Wait → Flush（含 kSubmitScene → submit_pending_）
 *   → submit_pending 且 ItemStorage dirty → 逐屏：
 *   HasInFlight 防超前 → TryAcquireFree → 渲 FBO → 回读 → MarkReady
 *   → frame_done 节拍
 *
 * @par 唤醒源（三路，P0-2 防死锁）
 *   ① present 线程 buffer 释放（ReleaseSlot→free_sem_ 与本唤醒共用
 *      Queue 的 PostRenderWake——本线程的 wake 由 Queue free_sem_
 *      post 聚合，见 frame_image_queue 的双 sem 设计）
 *   ② CommandQueue push（含 SubmitScene）→ notify_semaphore_ = render_wake_
 *   ③ 启动 pre-post（初始双 buffer 全 FREE，无释放事件，不预唤醒首帧死锁）
 */
void ARHudEngine::RenderThreadFunc() {
  Impl* impl = impl_;

  impl->context_driver.MakeCurrent(impl->offscreen_context_id,
                                   impl->offscreen_surface_id);
  impl->rd.MakeRenderThread();

  impl->render_thread_ready_.Post();

  // P0-2: 启动 pre-post（初始双 buffer 全 FREE，需预唤醒才能画首帧）
  for (uint32 i = 0; i < impl->frame_queues_.Size(); ++i) {
    impl->frame_queues_[i]->PostRenderWake();
  }
  while (impl->render_thread_running_.load(std::memory_order_acquire)) {
    impl->render_wake_.Wait();

    if (!impl->render_thread_running_.load(std::memory_order_acquire)) {
      break;
    }

    // 1. Flush：Create（RID 分裂的异步初始化）/ item 变更 / SubmitScene
    // 在此生效
    impl->command_queue.Flush();

    // 2. 消费 submit 信号（无 SubmitScene 不渲染——纯命令帧只是
    //    资源/状态更新，等下一次 SubmitScene 一并画）
    bool should_render =
        impl->submit_pending_.exchange(false, std::memory_order_acq_rel);

    // 2b. 外部重绘请求（ADR-0008）：WM_PAINT/WM_SIZE → 强制全屏重渲。
    //     置全屏脏；同时令 should_render=true——dirty 单独置位不足
    //     以触发渲染（渲染由 submit_pending_ 门控）。
    if (impl->force_redraw_.exchange(false, std::memory_order_acq_rel)) {
      impl->item_storage.MarkAllScreens();
      should_render = true;
    }

    // 2c. 窗口 resize（B3-M0.3）：逐屏应用挂起的 resize（重建 FBO/帧队列）。
    //     有在途帧时 ApplyPendingResize 保留 pending 返回 false，待 present
    //     线程 Release（唤醒本线程）后下轮重试——故每轮都必须尝试。
    bool resized = false;
    for (uint32 i = 0; i < screens_.Size(); ++i) {
      if (screens_[i]->ApplyPendingResize()) {
        resized = true;
      }
    }
    if (resized) {
      impl->item_storage.MarkAllScreens();
      should_render = true;
    }

    // 3. 逐屏渲染（§3.8：HasInFlight 防超前 = 精确 1 帧延迟；
    //    dirty=false 跳过 = 静态 HUD 零成本）。
    //    RD 帧簿记搬入本线程：Begin → 渲 →
    //    End（立即执行 + GPU Fence + 延迟销毁回收——同线程录放）
    if (should_render) {
      impl->rd.BeginFrame();
      for (uint32 i = 0; i < screens_.Size(); ++i) {
        // 逐屏消费：仅当该屏真正完成渲染+提交才清本屏脏位；背压跳过的
        // 屏保留脏位，下一轮（present 释放唤醒 + SubmitScene）重试。
        // 旧做法是循环后全局 ClearDirty——会把背压跳过屏的更新一并吞掉
        // （首帧资源创建命令的 dirty 偶发丢失即此竞态）。
        if (screens_[i]->RenderAndSubmit()) {
          impl->item_storage.ClearScreenDirty(i);
        }
      }
      impl->rd.EndFrame();
      impl->frame_done_.Post();
    }
  }

  impl->command_queue.Flush();
  impl->context_driver.ReleaseCurrent();
}

// ADR-0008: 外部重绘请求 → 强制全屏重渲（主线程入口：窗口回调）
void ARHudEngine::RequestFullRedraw() {
  if (impl_ == nullptr) {
    return;
  }
  // 主线程：只置原子 + 唤醒（不触碰 ItemStorage——渲染线程独占）；
  // 置屏位由渲染线程消费 force_redraw_ 时执行（MarkAllScreens）。
  impl_->force_redraw_.store(true, std::memory_order_release);
  impl_->render_wake_.Post();
}

void ARHudEngine::OnWindowPaint(uint32 p_id, void* p_userdata) {
  (void)p_id;
  if (p_userdata != nullptr) {
    static_cast<ARHudEngine*>(p_userdata)->RequestFullRedraw();
  }
}

void ARHudEngine::OnWindowResize(uint32 p_id, int32 p_width, int32 p_height,
                                 void* p_userdata) {
  (void)p_id;
  (void)p_width;
  (void)p_height;
  if (p_userdata != nullptr) {
    static_cast<ARHudEngine*>(p_userdata)->RequestFullRedraw();
  }
}

void ARHudEngine::Run(IAppLoop& p_app_loop) {
  if (!initialized_) {
    return;
  }

  using Clock = std::chrono::high_resolution_clock;
  auto last_time = Clock::now();

  while (!ShouldClose()) {
    auto current_time = Clock::now();
    std::chrono::duration<float32> delta = current_time - last_time;
    float32 delta_time = delta.count();
    last_time = current_time;

    BeginFrame();
    p_app_loop.OnUpdate(delta_time);
    p_app_loop.OnRender();
    EndFrame();
  }
}

RenderingServer* ARHudEngine::GetRenderServer() {
  if (impl_ == nullptr || !impl_->stack_initialized) {
    return nullptr;
  }
  return &impl_->rendering_server;
}

uint32 ARHudEngine::GetWidth() const {
  if (screens_.IsEmpty()) {
    return 0;
  }
  return screens_[0]->GetWidth();
}

uint32 ARHudEngine::GetHeight() const {
  if (screens_.IsEmpty()) {
    return 0;
  }
  return screens_[0]->GetHeight();
}

}  // namespace arhud
