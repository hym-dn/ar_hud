/**
 * @file display_server_win32.cpp
 * @brief Win32 平台显示服务器实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_os
 */

#include "display_server_win32.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "gl_context_manager_win32.h"
#include "io/logger.h"
#include "os/memory.h"
#include "window_win32.h"

namespace arhud {
namespace detail {
/**
 * @brief 安全销毁堆对象并置空指针
 *
 * @par 实现细节
 *   调用 ARHUD_DELETE 释放对象，随后将指针置 nullptr，
 *   保证重复调用安全（幂等）。
 *
 * @tparam T 对象类型
 * @param[in,out] ptr 待销毁的对象指针引用
 */
template <typename T>
void DestroyObject(T*& ptr) {
  ARHUD_DELETE(ptr);
  ptr = nullptr;
}
}  // namespace detail

/** @brief 显示服务器单例指针（静态定义，初始为空） */
IDisplayServer* IDisplayServer::singleton_ = nullptr;

/**
 * @brief 获取显示服务器单例
 *
 * @return IDisplayServer* 单例指针，未创建时为 nullptr
 */
IDisplayServer* IDisplayServer::GetSingleton() {
  return singleton_;
}

// ═══════════════════════════════════════════════════════════════════════
// Win32DisplayServer
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 构造 Win32DisplayServer 并注册为单例
 *
 * @par 实现细节
 *   将 this 写入 IDisplayServer::singleton_，供全局访问。
 *   此时未执行任何 Win32 资源初始化，需后续调用 Initialize()。
 */
Win32DisplayServer::Win32DisplayServer() {
  singleton_ = this;
}

/**
 * @brief 析构函数，保证资源释放
 *
 * @par 实现细节
 *   若仍处于已初始化状态，自动调用 Shutdown()；
 *   随后解除单例注册，避免悬垂指针。
 */
Win32DisplayServer::~Win32DisplayServer() {
  if (is_initialized_) {
    Shutdown();
  }
  singleton_ = nullptr;
}

/**
 * @brief 初始化显示服务器
 *
 * @return Error::kOK 成功
 * @return Error::kFailed 已初始化、无显示器或 GL 管理器初始化失败
 *
 * @par 实现步骤
 *   1. 重复初始化检测
 *   2. EnumerateScreens() 枚举显示器，空列表视为失败
 *   3. 初始化 GLContextManager，失败时回滚屏幕列表
 *   4. 置 is_initialized_ = true
 *
 * @par 失败回滚
 *   GL 管理器初始化失败时调用 DestroyAllScreens() 清理已枚举屏幕。
 */
Error Win32DisplayServer::Initialize() {
  if (is_initialized_) {
    ARHUD_LOG_WARN("Win32DisplayServer: Already initialized");
    return Error::kFailed;
  }
  EnumerateScreens();
  if (screens_.Size() == 0) {
    ARHUD_LOG_ERROR("kFailed", "Win32DisplayServer: No screens found");
    return Error::kFailed;
  }
  Error gl_err = gl_context_manager_.Initialize();
  if (gl_err != Error::kOK) {
    ARHUD_LOG_ERROR(
        "kFailed", "Win32DisplayServer: Failed to initialize GLContextManager");
    DestroyAllScreens();
    return Error::kFailed;
  }
  is_initialized_ = true;
  ARHUD_LOG_INFO("Win32DisplayServer: Initialized with %u screens",
                 static_cast<uint32>(screens_.Size()));
  return Error::kOK;
}

/**
 * @brief 关闭显示服务器并释放全部资源
 *
 * @par 实现步骤（销毁顺序）
 *   1. DestroyAllWindowsContexts() 销毁窗口上下文
 *   2. gl_context_manager_.Shutdown() 关闭 GL 管理器
 *   3. DestroyAllScreens() 销毁屏幕
 *   4. 置 is_initialized_ = false
 *
 * @note 未初始化时直接返回，调用安全（幂等）。
 */
void Win32DisplayServer::Shutdown() {
  if (!is_initialized_) {
    return;
  }
  DestroyAllWindowsContexts();
  gl_context_manager_.Shutdown();
  DestroyAllScreens();
  is_initialized_ = false;
}

// ─── 屏幕管理 ───────────────────────────────────────────────────────

/**
 * @brief 获取已枚举的显示器数量
 *
 * @return uint32 当前连接的显示器数量
 */
uint32 Win32DisplayServer::GetScreenCount() const {
  return static_cast<uint32>(screens_.Size());
}

/**
 * @brief 按 ScreenID 查找屏幕
 *
 * @param[in] id 屏幕 ID
 * @return IScreen* 命中返回屏幕指针，否则 nullptr
 *
 * @par 实现细节
 *   线性遍历 screens_ 比较 GetId()，O(n) 复杂度。
 *   返回指针由 DisplayServer 拥有，调用方不可释放。
 */
IScreen* Win32DisplayServer::GetScreen(ScreenID id) const {
  for (uint32 i = 0; i < screens_.Size(); ++i) {
    if (screens_[i]->GetId() == id) {
      return screens_[i];
    }
  }
  return nullptr;
}

/**
 * @brief 获取主显示器 ID
 *
 * @return ScreenID 主屏幕 ID；不存在返回 kInvalidScreenId
 *
 * @par 实现细节
 *   线性遍历 screens_，返回首个 IsPrimary() 为真的屏幕 ID。
 */
IScreen::ScreenID Win32DisplayServer::GetPrimaryScreen() const {
  for (uint32 i = 0; i < screens_.Size(); ++i) {
    if (screens_[i]->IsPrimary()) {
      return screens_[i]->GetId();
    }
  }
  return IScreen::kInvalidScreenId;
}

// ─── 窗口管理 ───────────────────────────────────────────────────────

/**
 * @brief 创建新窗口
 *
 * @param[in] desc 窗口描述配置
 * @return WindowID 新窗口 ID；失败返回 kInvalidWindowId
 *
 * @par 实现步骤
 *   1. 已初始化校验
 *   2. ARHUD_NEW 分配 Win32Window，调用 Initialize(desc)
 *   3. 失败则销毁窗口对象并返回无效 ID
 *   4. 分配 next_window_id_，回填 SetWindowId
 *   5. 按 desc.screen_id 关联屏幕（用于 DPI 查询）
 *   6. 构造 WindowContext 并入 windows_ 映射
 *
 * @par 失败回滚
 *   Win32Window::Initialize 失败时立即 detail::DestroyObject，
 *   不留半成品对象。
 */
IDisplayServer::WindowID Win32DisplayServer::WindowCreate(
    const WindowDesc& desc) {
  if (!is_initialized_) {
    ARHUD_LOG_ERROR("kNotInitialized", "Win32DisplayServer: Not initialized");
    return IWindow::kInvalidWindowId;
  }

  Win32Window* window = ARHUD_NEW(Win32Window)();
  Error err = window->Initialize(desc);
  if (err != Error::kOK) {
    detail::DestroyObject(window);
    ARHUD_LOG_ERROR("kFailed", "Win32DisplayServer: Failed to create window");
    return IWindow::kInvalidWindowId;
  }

  WindowID id = next_window_id_++;
  window->SetWindowId(id);

  // 设置窗口关联的屏幕（用于 DPI 查询等）
  IScreen* screen = GetScreen(desc.screen_id);
  if (screen) {
    window->SetScreen(screen);
  }

  WindowContext ctx;
  ctx.window_id = id;
  ctx.screen_id = desc.screen_id;
  ctx.window = window;
  ctx.vsync_mode = desc.vsync_mode;
  windows_.Insert(id, ctx);

  ARHUD_LOG_INFO("Win32DisplayServer: Window %u created (%ux%u)", id,
                 desc.width, desc.height);
  return id;
}

/**
 * @brief 销毁指定窗口
 *
 * @param[in] id 窗口 ID
 *
 * @par 实现步骤
 *   1. 取 WindowContext，不存在则告警返回
 *   2. 调用 window->Shutdown() 并 delete 窗口对象
 *   3. 从 windows_ 移除该上下文
 */
void Win32DisplayServer::WindowDestroy(WindowID id) {
  WindowContext* ctx = windows_.GetPtr(id);
  if (!ctx) {
    ARHUD_LOG_WARN("Win32DisplayServer: Window %u not found", id);
    return;
  }

  if (ctx->window) {
    Win32Window* ww = static_cast<Win32Window*>(ctx->window);
    ctx->window->Shutdown();
    detail::DestroyObject(ww);
    ctx->window = nullptr;
  }

  windows_.Erase(id);
  ARHUD_LOG_INFO("Win32DisplayServer: Window %u destroyed", id);
}

/**
 * @brief 按窗口 ID 取窗口对象
 *
 * @param[in] id 窗口 ID
 * @return IWindow* 命中返回，否则 nullptr
 *
 * @par 实现细节
 *   HashMap O(1) 查找，取 WindowContext::window。
 *   指针由 DisplayServer 拥有，不可释放。
 */
IWindow* Win32DisplayServer::GetWindow(WindowID id) const {
  const WindowContext* ctx = windows_.GetPtr(id);
  return ctx ? ctx->window : nullptr;
}

/**
 * @brief 获取当前窗口数量
 *
 * @return uint32 windows_ 中 WindowContext 数量
 */
uint32 Win32DisplayServer::GetWindowCount() const {
  return static_cast<uint32>(windows_.Size());
}

// ─── 事件处理 ───────────────────────────────────────────────────────

/**
 * @brief 泵取并分发 Win32 消息
 *
 * @par 实现细节
 *   循环 PeekMessageA(PM_REMOVE) 取出消息，
 *   经 TranslateMessage + DispatchMessageA 分发到对应 WndProc。
 *   非阻塞：队列空即返回。
 *
 * @note 应在主线程主循环中每帧调用。
 */
void Win32DisplayServer::ProcessEvents() {
  MSG msg;
  while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
}

// ─── VSync 控制 ─────────────────────────────────────────────────────

/**
 * @brief 设置窗口 VSync 模式
 *
 * @param[in] window 窗口 ID
 * @param[in] mode 垂直同步模式
 *
 * @par 实现细节
 *   仅更新 WindowContext::vsync_mode；真正的同步由
 *   RenderingContextDriver 在 SwapBuffers 时按该值生效。
 *   窗口不存在时静默忽略。
 */
void Win32DisplayServer::SetVsyncMode(WindowID window, VSyncMode mode) {
  WindowContext* ctx = windows_.GetPtr(window);
  if (ctx) {
    ctx->vsync_mode = mode;
  }
}

/**
 * @brief 查询窗口 VSync 模式
 *
 * @param[in] window 窗口 ID
 * @return VSyncMode 当前模式；窗口不存在返回 kEnabled（默认）
 */
VSyncMode Win32DisplayServer::GetVsyncMode(WindowID window) const {
  const WindowContext* ctx = windows_.GetPtr(window);
  return ctx ? ctx->vsync_mode : VSyncMode::kEnabled;
}

/**
 * @brief 获取内部 GL 上下文管理器
 *
 * @return IGLContextManager* 管理器指针
 *
 * @par 实现细节
 *   返回内部 gl_context_manager_ 的非常量指针；
 *   生命周期随 DisplayServer，Initialize 时就绪、Shutdown 时关闭。
 */
IGLContextManager* Win32DisplayServer::GetGLContextManager() const {
  return const_cast<IGLContextManager*>(
      static_cast<const IGLContextManager*>(&gl_context_manager_));
}

// ─── 内部方法 ───────────────────────────────────────────────────────

/**
 * @brief 枚举所有显示器
 *
 * @par 实现细节
 *   调用 EnumDisplayMonitors，以 MonitorEnumProc 为回调、
 *   screens_ 地址作为 LPARAM 透传，逐显示器填充列表。
 *
 * @see MonitorEnumProc
 */
void Win32DisplayServer::EnumerateScreens() {
  EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                      reinterpret_cast<LPARAM>(&screens_));
}

/**
 * @brief 显示器枚举回调（EnumDisplayMonitors 约定）
 *
 * @param[in] hMonitor 显示器句柄
 * @param[in] hdcMonitor 显示器 HDC（本实现未使用）
 * @param[in] lprcMonitor 显示器矩形（本实现未使用）
 * @param[in] dwData 透传的 screens_ 列表指针
 * @return BOOL 恒为 TRUE，继续枚举后续显示器
 *
 * @par 实现细节
 *   将 dwData 还原为 LocalVector<Win32Screen*>*，
 *   Win32Screen::Create(hMonitor) 失败时跳过，成功则 PushBack。
 */
BOOL CALLBACK Win32DisplayServer::MonitorEnumProc(HMONITOR hMonitor,
                                                  HDC hdcMonitor,
                                                  LPRECT lprcMonitor,
                                                  LPARAM dwData) {
  auto* screens = reinterpret_cast<LocalVector<Win32Screen*>*>(dwData);
  Win32Screen* screen = Win32Screen::Create(hMonitor);
  if (screen) {
    screens->PushBack(screen);
  }
  return TRUE;
}

/**
 * @brief 销毁并清空所有屏幕对象
 *
 * @par 实现细节
 *   遍历 screens_ 调用 detail::DestroyObject，再 Clear()。
 *   置空后 screens_ 可安全重建。
 */
void Win32DisplayServer::DestroyAllScreens() {
  for (uint32 i = 0; i < screens_.Size(); ++i) {
    Win32Screen* s = screens_[i];
    detail::DestroyObject(s);
  }
  screens_.Clear();
}

/**
 * @brief 销毁所有窗口上下文
 *
 * @par 实现步骤
 *   遍历 windows_，对每个 WindowContext：
 *   1. Shutdown + delete 窗口对象
 *   最后 windows_.Clear()。
 */
void Win32DisplayServer::DestroyAllWindowsContexts() {
  for (auto it = windows_.Begin(); it != windows_.End(); ++it) {
    auto pair = *it;
    WindowContext& ctx = pair.value;
    if (ctx.window) {
      Win32Window* ww = static_cast<Win32Window*>(ctx.window);
      ctx.window->Shutdown();
      detail::DestroyObject(ww);
      ctx.window = nullptr;
    }
  }
  windows_.Clear();
}

}  // namespace arhud

#endif
