/**
 * @file gl_context_manager_win32.cpp
 * @brief Win32 平台 OpenGL 上下文管理实现
 *
 * @par 统一 Context 模型
 *   所有 Context 地位平等，不共享资源。
 *   MakeCurrent(ContextID, SurfaceID) 统一绑定。
 *
 * @par 本文件的关键实现细节（导航）
 *   - ConfigurePixelFormat: 像素格式索引进程内共享（双路径 + 回退）
 *   - MakeCurrent:          drawable 切换幂等（context+DC 双比对）
 *   - SwapBuffers:          纯 GDI（surface 属性），present 线程可调
 *   - SetVsyncMode:         模式记录 + wglSwapIntervalEXT 映射
 *   - LoadWGLExtensions:    两步法上下文创建的扩展加载（仅首次）
 *   - CombinedGLLoader:     wglGetProcAddress 优先 + opengl32.dll 回退
 *
 * @author yameng.he
 * @version 4.2
 * @date 2026-08-30
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_os
 */

#include "gl_context_manager_win32.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "io/logger.h"
#include "os/memory.h"

/**
 * @brief WGL_ARB_create_context 属性常量
 *
 * @par 说明
 *   部分 SDK 头文件未提供 WGL_ARB_create_context 的属性枚举，
 *   此处按官方注册值本地定义，供 CreateCoreProfileContext() 构造属性数组。
 */
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x00000002
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define WGL_CONTEXT_DEBUG_BIT_ARB 0x0001

namespace arhud {

// ══════════════════════════════════════════════════════════════════════════
// 构造与析构
// ══════════════════════════════════════════════════════════════════════════

/**
 * @brief 默认构造（空实现）
 *
 * @par 实现细节
 *   成员均有默认初值，无需显式初始化；
 *   is_initialized_ 初值为 false，必须后续调用 Initialize()。
 */
GLContextManagerWin32::GLContextManagerWin32() = default;

/**
 * @brief 析构，保证资源释放
 *
 * @par 实现细节
 *   若已初始化则调用 Shutdown()：卸载 opengl32.dll、
 *   wglDeleteContext 释放所有 HGLRC、清空 surface/context 表。
 */
GLContextManagerWin32::~GLContextManagerWin32() {
  if (is_initialized_) {
    Shutdown();
  }
}

// ══════════════════════════════════════════════════════════════════════════
// IGLContextManager 接口实现 - 生命周期
// ══════════════════════════════════════════════════════════════════════════

/**
 * @brief 初始化 GL 管理器
 *
 * @return Error::kOK 成功
 * @return Error::kFailed 已初始化或驱动加载失败
 *
 * @par 实现步骤
 *   1. 重复初始化检测
 *   2. LoadOpenGLDriver() 动态加载 opengl32.dll 并取 4 个基础 WGL 函数
 *   3. 置 is_initialized_ = true
 *
 * @note 此时不加载 WGL 扩展（wglCreateContextAttribsARB 等），
 *       扩展延迟到首次 CreateContext() 用临时上下文加载。
 */
Error GLContextManagerWin32::Initialize() {
  if (is_initialized_) {
    ARHUD_LOG_WARN("GLContextManagerWin32: Already initialized");
    return Error::kFailed;
  }

  Error err = LoadOpenGLDriver();
  if (err != Error::kOK) {
    return err;
  }

  is_initialized_ = true;
  ARHUD_LOG_INFO("GLContextManagerWin32: Initialized");
  return Error::kOK;
}

/**
 * @brief 关闭并释放全部资源
 *
 * @par 实现步骤
 *   1. surfaces_ 逐项置空字段后 Clear（HDC 未逐个 ReleaseDC，依赖进程退出回收）
 *   2. contexts_ 逐个 wglDeleteContext 后 Clear
 *   3. FreeLibrary(opengl32.dll)
 *   4. 所有 WGL 函数指针置 nullptr、扩展标志复位
 *   5. is_initialized_ = false
 *
 * @note 未初始化时直接返回（幂等）。
 */
void GLContextManagerWin32::Shutdown() {
  if (!is_initialized_) {
    return;
  }

  for (uint32 i = 0; i < surfaces_.Size(); ++i) {
    GLSurface& surf = surfaces_[i];
    surf.hdc = nullptr;
    surf.hwnd = nullptr;
    surf.is_valid = false;
  }
  surfaces_.Clear();

  for (uint32 i = 0; i < contexts_.Size(); ++i) {
    GLContext& ctx = contexts_[i];
    if (ctx.hrc && wgl_delete_context_) {
      wgl_delete_context_(ctx.hrc);
      ctx.hrc = nullptr;
    }
    ctx.is_valid = false;
  }
  contexts_.Clear();

  if (opengl_module_) {
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
  shared_pixel_format_ = 0;  // 像素格式索引缓存失效（新会话重新选定）

  is_initialized_ = false;
  ARHUD_LOG_INFO("GLContextManagerWin32: Shutdown");
}

// ══════════════════════════════════════════════════════════════════════════
// IGLContextManager 接口实现 - 上下文管理
// ══════════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 GL 渲染上下文
 *
 * @param[in] p_surface 所属 Surface ID（提供 HDC）
 * @return ContextID 新 ID；失败返回 kInvalidContextId
 *
 * @par 实现步骤
 *   1. 已初始化 + Surface/HDC 有效性校验
 *   2. 首次调用时 LoadWGLExtensions(hdc)：用临时上下文取
 * wglCreateContextAttribsARB
 *   3. CreateCoreProfileContext(hdc) 建 3.3 Core Profile HGLRC
 *   4. 入 contexts_，返回索引作 ContextID
 *
 * @par 资源语义
 *   新 Context 不与任何已有 Context 共享资源（share 列传 nullptr）。
 */
IGLContextManager::ContextID GLContextManagerWin32::CreateContext(
    SurfaceID p_surface) {
  if (!is_initialized_) {
    ARHUD_LOG_ERROR("kFailed", "GLContextManagerWin32: Not initialized");
    return kInvalidContextId;
  }

  GLSurface* surf = GetSurface(p_surface);
  if (!surf || !surf->hdc) {
    ARHUD_LOG_ERROR("kInvalidParameter",
                    "GLContextManagerWin32: Invalid surface ID %u",
                    static_cast<uint32>(p_surface));
    return kInvalidContextId;
  }

  HDC hdc = surf->hdc;

  // 首次创建上下文时，通过临时上下文加载 WGL 扩展
  if (!extensions_loaded_) {
    if (!LoadWGLExtensions(hdc)) {
      return kInvalidContextId;
    }
    extensions_loaded_ = true;
  }

  HGLRC hrc = CreateCoreProfileContext(hdc);
  if (!hrc) {
    return kInvalidContextId;
  }

  GLContext ctx;
  ctx.hrc = hrc;
  ctx.is_valid = true;
  contexts_.PushBack(ctx);
  ContextID new_id = static_cast<ContextID>(contexts_.Size() - 1);

  ARHUD_LOG_INFO(
      "GLContextManagerWin32: Context created (ContextID=%u, SurfaceID=%u)",
      static_cast<uint32>(new_id), static_cast<uint32>(p_surface));

  return new_id;
}

/**
 * @brief 销毁 GL 渲染上下文
 *
 * @param[in] p_context_id Context ID
 *
 * @par 实现步骤
 *   1. 越界/失效校验，无效则告警返回
 *   2. 解除所有 Surface 对该 Context 的占有关系
 *   3. 若该 Context 正是当前线程 current，先 wglMakeCurrent(dc, nullptr) 解绑
 *   4. wglDeleteContext 释放 HGLRC，标记 is_valid=false
 *
 * @note 槽位不回收（contexts_ 不 Erase），ContextID 保持稳定。
 */
void GLContextManagerWin32::DestroyContext(ContextID p_context_id) {
  if (p_context_id >= contexts_.Size() || !contexts_[p_context_id].is_valid) {
    ARHUD_LOG_WARN("GLContextManagerWin32: Cannot destroy context %u (invalid)",
                   static_cast<uint32>(p_context_id));
    return;
  }

  GLContext& ctx = contexts_[p_context_id];

  for (uint32 i = 0; i < surfaces_.Size(); ++i) {
    if (surfaces_[i].is_valid &&
        surfaces_[i].owner_context_id == p_context_id) {
      surfaces_[i].owner_context_id = kInvalidContextId;
    }
  }

  if (ctx.hrc && wgl_delete_context_) {
    HGLRC current = wglGetCurrentContext();
    if (current == ctx.hrc) {
      HDC current_dc = wglGetCurrentDC();
      wgl_make_current_(current_dc, nullptr);
    }
    wgl_delete_context_(ctx.hrc);
    ctx.hrc = nullptr;
  }

  ctx.is_valid = false;

  ARHUD_LOG_INFO("GLContextManagerWin32: Context %u destroyed",
                 static_cast<uint32>(p_context_id));
}

/**
 * @brief 查询上下文条目数
 *
 * @return uint32 contexts_.Size()，含已销毁但未回收的槽位
 *
 * @note 因 DestroyContext 不回收槽位，返回值不等于存活上下文数。
 */
uint32 GLContextManagerWin32::GetContextCount() const {
  return contexts_.Size();
}

// ══════════════════════════════════════════════════════════════════════════
// IGLContextManager 接口实现 - 渲染表面
// ══════════════════════════════════════════════════════════════════════════

/**
 * @brief 创建渲染表面
 *
 * @param[in] p_screen_id 关联屏幕 ID
 * @param[in] p_native_window 原生窗口句柄 HWND；为空时用桌面 DC
 * @param[in] p_width 宽（仅用于日志）
 * @param[in] p_height 高（仅用于日志）
 * @return SurfaceID 新 ID；失败返回 kInvalidSurfaceId
 *
 * @par 实现步骤
 *   1. 已初始化校验
 *   2. p_native_window 非空则 GetDC(hwnd)，否则 GetDC(nullptr) 取桌面 DC
 *   3. ConfigurePixelFormat(hdc) 设 RGBA8888 + 深度24 + 模板8 + 双缓冲
 *   4. 填 GLSurface 入 surfaces_，返回索引作 SurfaceID
 *
 * @note p_width/p_height 仅写日志，GLSurface 不保存尺寸。
 */
IGLContextManager::SurfaceID GLContextManagerWin32::CreateSurface(
    IScreen::ScreenID p_screen_id, void* p_native_window, uint32 p_width,
    uint32 p_height) {
  if (!is_initialized_) {
    ARHUD_LOG_ERROR("kFailed", "GLContextManagerWin32: Not initialized");
    return kInvalidSurfaceId;
  }

  HWND hwnd = static_cast<HWND>(p_native_window);
  HDC hdc;

  if (hwnd) {
    hdc = GetDC(hwnd);
  } else {
    // 无窗口句柄时使用桌面 DC（用于临时上下文，如 gladLoadGL）
    hdc = GetDC(nullptr);
  }

  if (!hdc) {
    ARHUD_LOG_ERROR("kFailed",
                    "GLContextManagerWin32: GetDC failed for screen %u",
                    static_cast<uint32>(p_screen_id));
    return kInvalidSurfaceId;
  }

  Error pf_result = ConfigurePixelFormat(hdc);
  if (pf_result != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "GLContextManagerWin32: Pixel format configuration failed "
                    "for screen %u",
                    static_cast<uint32>(p_screen_id));
    return kInvalidSurfaceId;
  }

  GLSurface surf;
  surf.hwnd = hwnd;
  surf.hdc = hdc;
  surf.vsync_mode = VSyncMode::kEnabled;
  surf.screen_id = p_screen_id;
  surf.is_valid = true;

  surfaces_.PushBack(surf);
  SurfaceID new_id = static_cast<SurfaceID>(surfaces_.Size() - 1);

  ARHUD_LOG_INFO(
      "GLContextManagerWin32: Surface created (SurfaceID=%u, ScreenID=%u, "
      "%ux%u)",
      static_cast<uint32>(new_id), static_cast<uint32>(p_screen_id), p_width,
      p_height);

  return new_id;
}

/**
 * @brief 销毁渲染表面
 *
 * @param[in] p_surface Surface ID
 *
 * @par 实现步骤
 *   1. 越界/失效校验，无效则告警返回
 *   2. ReleaseDC(hwnd, hdc) 释放 DC（hwnd 为空即 ReleaseDC(nullptr, hdc)）
 *   3. 解除占有该 Surface 的 Context 的 bound_surface
 *   4. 清扫所有指向该 Surface 的 bound_surface
 *   5. 置空字段、is_valid=false
 *
 * @note 槽位不回收，SurfaceID 保持稳定。
 */
void GLContextManagerWin32::DestroySurface(SurfaceID p_surface) {
  if (p_surface >= surfaces_.Size() || !surfaces_[p_surface].is_valid) {
    ARHUD_LOG_WARN("GLContextManagerWin32: Cannot destroy surface %u (invalid)",
                   static_cast<uint32>(p_surface));
    return;
  }

  GLSurface& surf = surfaces_[p_surface];

  // 释放 DC（桌面 DC 用 ReleaseDC(nullptr, hdc)，窗口 DC 用 ReleaseDC(hwnd,
  // hdc)）
  if (surf.hdc) {
    ReleaseDC(surf.hwnd, surf.hdc);
  }

  if (surf.owner_context_id != kInvalidContextId) {
    GLContext* owner = GetContext(surf.owner_context_id);
    if (owner && owner->bound_surface == p_surface) {
      owner->bound_surface = kInvalidSurfaceId;
    }
  }

  for (uint32 i = 0; i < contexts_.Size(); ++i) {
    if (contexts_[i].is_valid && contexts_[i].bound_surface == p_surface) {
      contexts_[i].bound_surface = kInvalidSurfaceId;
    }
  }

  surf.hdc = nullptr;
  surf.hwnd = nullptr;
  surf.is_valid = false;

  ARHUD_LOG_INFO("GLContextManagerWin32: Surface %u destroyed",
                 static_cast<uint32>(p_surface));
}

// ══════════════════════════════════════════════════════════════════════════
// IGLContextManager 接口实现 - 上下文绑定
// ══════════════════════════════════════════════════════════════════════════

/**
 * @brief 将 Context 绑定到 Surface（当前线程）
 *
 * @param[in] p_context_id Context ID
 * @param[in] p_surface Surface ID
 * @return Error::kOK 成功
 * @return Error::kInvalidParameter Context/Surface 无效
 * @return Error::kFailed Surface 被其它 Context 占有或 wglMakeCurrent 失败
 *
 * @par 实现步骤
 *   1. 取并校验 Surface(HDC) 与 Context(HGLRC)
 *   2. 若 Surface 已被其它 Context 占有，拒绝（单一占有）
 *   3. 已是当前绑定（context 与 DC 均一致）则直接返回 kOK
 *      （幂等 —— drawable 切换循环里同屏重绑零开销）
 *   4. wglMakeCurrent(hdc, hrc)
 *   5. 回填 ctx->bound_surface；首次绑定时记 Surface 的 owner_context_id
 *
 * @note 步骤 3 的 DC 比对不可省：同一 context 在不同 surface 间
 *       切换时 wglGetCurrentContext 相同但 DC 不同，必须两者
 *       都一致才算"已是当前绑定"。
 */
Error GLContextManagerWin32::MakeCurrent(ContextID p_context_id,
                                         SurfaceID p_surface) {
  GLSurface* surf = GetSurface(p_surface);
  if (!surf || !surf->hdc) {
    ARHUD_LOG_ERROR("kInvalidParameter",
                    "GLContextManagerWin32: Invalid surface ID %u",
                    static_cast<uint32>(p_surface));
    return Error::kInvalidParameter;
  }

  GLContext* ctx = GetContext(p_context_id);
  if (!ctx || !ctx->hrc) {
    ARHUD_LOG_ERROR("kInvalidParameter",
                    "GLContextManagerWin32: Invalid context ID %u",
                    static_cast<uint32>(p_context_id));
    return Error::kInvalidParameter;
  }

  if (surf->owner_context_id != kInvalidContextId &&
      surf->owner_context_id != p_context_id) {
    ARHUD_LOG_ERROR("kFailed",
                    "GLContextManagerWin32: Surface %u owned by context %u, "
                    "cannot bind to context %u",
                    static_cast<uint32>(p_surface),
                    static_cast<uint32>(surf->owner_context_id),
                    static_cast<uint32>(p_context_id));
    return Error::kFailed;
  }

  if (wglGetCurrentContext() == ctx->hrc && wglGetCurrentDC() == surf->hdc) {
    return Error::kOK;
  }

  if (!wgl_make_current_(surf->hdc, ctx->hrc)) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "GLContextManagerWin32: wglMakeCurrent failed for context %u "
        "surface %u (error=%lu)",
        static_cast<uint32>(p_context_id), static_cast<uint32>(p_surface),
        GetLastError());
    return Error::kFailed;
  }

  ctx->bound_surface = p_surface;

  if (surf->owner_context_id == kInvalidContextId) {
    surf->owner_context_id = p_context_id;
  }

  return Error::kOK;
}

/**
 * @brief 释放当前线程的 GL 上下文
 *
 * @par 实现步骤
 *   1. wglGetCurrentContext() 找到当前 HGLRC 对应的 Context，清其 bound_surface
 *   2. wglMakeCurrent(wglGetCurrentDC(), nullptr) 解绑
 */
void GLContextManagerWin32::ReleaseCurrent() {
  HGLRC current_hrc = wglGetCurrentContext();
  for (uint32 i = 0; i < contexts_.Size(); ++i) {
    if (contexts_[i].hrc == current_hrc) {
      contexts_[i].bound_surface = kInvalidSurfaceId;
      break;
    }
  }

  HDC hdc = wglGetCurrentDC();
  if (hdc && wgl_make_current_) {
    wgl_make_current_(hdc, nullptr);
  }
}

// ══════════════════════════════════════════════════════════════════════════
// IGLContextManager 接口实现 - 渲染
// ══════════════════════════════════════════════════════════════════════════

/**
 * @brief 交换指定 Surface 的前后缓冲
 *
 * @param[in] p_surface Surface ID
 *
 * @par 实现细节
 *   取 surf->hdc 调 gdi32 ::SwapBuffers —— 纯 GDI 调用，
 *   不触碰 GL 上下文（缓冲交换是 surface 属性），present
 *   线程无需 current context 即可调用（Swap 呈现路径基础）。
 *   Surface 无效/HDC 为空时静默返回。
 *   VSync 实际生效依赖先前 SetVsyncMode 设置的 swap interval；
 *   DWM 之下为提交语义（非阻塞），翻入由 DWM 合成节拍决定。
 */
void GLContextManagerWin32::SwapBuffers(SurfaceID p_surface) {
  GLSurface* surf = GetSurface(p_surface);
  if (!surf || !surf->hdc) {
    return;
  }
  ::SwapBuffers(surf->hdc);
}

// ══════════════════════════════════════════════════════════════════════════
// IGLContextManager 接口实现 - VSync
// ══════════════════════════════════════════════════════════════════════════

/**
 * @brief 设置 Surface 的 VSync 模式
 *
 * @param[in] p_surface Surface ID
 * @param[in] p_mode VSync 模式
 *
 * @par 实现细节
 *   - kDisabled  -> wglSwapIntervalEXT(0)
 *   - kEnabled   -> wglSwapIntervalEXT(1)
 *   - kAdaptive  -> wglSwapIntervalEXT(-1)，需 WGL_EXT_swap_control_tear；
 *     不支持时降级为 kEnabled(1) 并告警
 *   驱动无 wglSwapIntervalEXT 时告警且不改变现有模式。
 */
void GLContextManagerWin32::SetVsyncMode(SurfaceID p_surface,
                                         VSyncMode p_mode) {
  GLSurface* surf = GetSurface(p_surface);
  if (!surf) {
    return;
  }

  if (!wgl_swap_interval_ext_) {
    ARHUD_LOG_WARN("GLContextManagerWin32: VSync not supported by driver");
    return;
  }

  int interval = 0;
  switch (p_mode) {
    case VSyncMode::kDisabled:
      interval = 0;
      break;
    case VSyncMode::kEnabled:
      interval = 1;
      break;
    case VSyncMode::kAdaptive:
      if (swap_control_tear_supported_) {
        interval = -1;
      } else {
        ARHUD_LOG_WARN(
            "GLContextManagerWin32: Adaptive VSync not supported, "
            "falling back to enabled");
        interval = 1;
        p_mode = VSyncMode::kEnabled;
      }
      break;
  }

  if (!wgl_swap_interval_ext_(interval)) {
    ARHUD_LOG_WARN("GLContextManagerWin32: wglSwapIntervalEXT(%d) failed",
                   interval);
    return;
  }

  surf->vsync_mode = p_mode;
}

/**
 * @brief 查询 Surface 的 VSync 模式
 *
 * @param[in] p_surface Surface ID
 * @return VSyncMode 当前模式；Surface 无效返回 kEnabled（默认）
 */
VSyncMode GLContextManagerWin32::GetVsyncMode(SurfaceID p_surface) const {
  const GLSurface* surf = GetSurface(p_surface);
  return surf ? surf->vsync_mode : VSyncMode::kEnabled;
}

// ══════════════════════════════════════════════════════════════════════════
// IGLContextManager 接口实现 - 原生句柄
// ══════════════════════════════════════════════════════════════════════════

/**
 * @brief 取 Surface 的 HDC
 *
 * @param[in] p_surface Surface ID
 * @return void* HDC 指针；无效返回 nullptr
 */
void* GLContextManagerWin32::GetDeviceContext(SurfaceID p_surface) const {
  const GLSurface* surf = GetSurface(p_surface);
  return surf ? surf->hdc : nullptr;
}

/**
 * @brief 取 Context 的 HGLRC
 *
 * @param[in] p_context_id Context ID
 * @return void* HGLRC 指针；无效返回 nullptr
 */
void* GLContextManagerWin32::GetRenderContext(ContextID p_context_id) const {
  const GLContext* ctx = GetContext(p_context_id);
  return ctx ? ctx->hrc : nullptr;
}

/**
 * @brief 取 GL 过程地址加载器入口
 *
 * @return void* 指向 CombinedGLLoader 的指针，供 GLAD 等加载器解析 GL 扩展
 */
void* GLContextManagerWin32::GetGLProcAddress() const {
  return reinterpret_cast<void*>(CombinedGLLoader);
}

/**
 * @brief 组合式 GL 函数加载器
 *
 * @param[in] name GL 函数名
 * @return void* 函数指针；未找到返回 nullptr
 *
 * @par 实现细节
 *   先 wglGetProcAddress(name) 取扩展入口；
 *   未命中则回退到 opengl32.dll 的 GetProcAddress（核心函数走这条路）。
 */
void* GLContextManagerWin32::CombinedGLLoader(const char* name) {
  void* proc = reinterpret_cast<void*>(wglGetProcAddress(name));
  if (!proc) {
    HMODULE module = GetModuleHandleW(L"opengl32.dll");
    if (module) {
      proc = reinterpret_cast<void*>(GetProcAddress(module, name));
    }
  }
  return proc;
}

/**
 * @brief 取已加载的 opengl32.dll 模块句柄
 *
 * @return void* HMODULE；未加载返回 nullptr
 */
void* GLContextManagerWin32::GetOpenGLModule() const {
  return reinterpret_cast<void*>(opengl_module_);
}

/** @brief 是否已成功 Initialize() */
bool GLContextManagerWin32::IsInitialized() const {
  return is_initialized_;
}

// ══════════════════════════════════════════════════════════════════════════
// 私有方法实现
// ══════════════════════════════════════════════════════════════════════════

/**
 * @brief 配置 HDC 的像素格式
 *
 * @param[in] p_hdc 设备上下文
 * @return Error::kOK 成功；kFailed ChoosePixelFormat/SetPixelFormat 失败
 *
 * @par 双路径实现（与 .h 声明处 Doxygen 一致，此处记实现细节）
 *   - 已有格式（GetPixelFormat != 0）→ 幂等返回 kOK
 *   - 快路径：shared_pixel_format_ 已缓存 → SetPixelFormat 沿用
 *     （索引一致 = 单 Context 多 Drawable 的绑定合法性）；
 *     失败（混合适配器征兆）→ WARN 后落慢路径独立选择
 *   - 慢路径：ChoosePixelFormat 选定 → SetPixelFormat →
 *     缓存空时写 shared_pixel_format_（首窗定标）
 *
 * @par PFD 请求
 *   RGBA8888 + 深度24 + 模板8 + 双缓冲 + PFD_DRAW_TO_WINDOW +
 *   PFD_SUPPORT_OPENGL（Swap 路径两前提的来源）。
 *
 * @note 同一 HDC 只能设置一次像素格式；仅主线程 Surface 创建期调用。
 */
Error GLContextManagerWin32::ConfigurePixelFormat(HDC p_hdc) {
  int current_format = GetPixelFormat(p_hdc);
  if (current_format != 0) {
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

  // ── 快路径：沿用已缓存索引（单 Context 多 Drawable 的一致性保证）──
  // WGL 契约要求绑定同一 HGLRC 的所有 HDC 像素格式索引一致；
  // 各窗口独立 ChoosePixelFormat 在多适配器上可能拿到不同索引。
  // SetPixelFormat 的 pfd 参数仅作元文件记录，索引才是权威。
  if (shared_pixel_format_ != 0) {
    if (SetPixelFormat(p_hdc, shared_pixel_format_, &pfd)) {
      return Error::kOK;
    }

    // 回退：缓存索引在该窗口上不可用（典型：另一适配器）。
    // 独立选择并 WARN——该 surface 与共享上下文的格式可能不一致。
    ARHUD_LOG_WARN(
        "GLContextManagerWin32: SetPixelFormat(cached index %d) "
        "failed (error=%lu) — falling back to independent "
        "ChoosePixelFormat; pixel format may diverge from the "
        "shared context (mixed-adapter topology?)",
        shared_pixel_format_, GetLastError());
  }

  // ── 慢路径：首个窗口（或回退）独立选择，成功后缓存索引 ──
  int pixel_format = ChoosePixelFormat(p_hdc, &pfd);
  if (!pixel_format) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "GLContextManagerWin32: ChoosePixelFormat failed (error=%lu)",
        GetLastError());
    return Error::kFailed;
  }

  if (!SetPixelFormat(p_hdc, pixel_format, &pfd)) {
    ARHUD_LOG_ERROR("kFailed",
                    "GLContextManagerWin32: SetPixelFormat failed (error=%lu)",
                    GetLastError());
    return Error::kFailed;
  }

  if (shared_pixel_format_ == 0) {
    shared_pixel_format_ = pixel_format;
  }

  return Error::kOK;
}

/**
 * @brief 动态加载 opengl32.dll 并取基础 WGL 函数
 *
 * @return Error::kOK 成功；kFailed 加载或取函数失败
 *
 * @par 实现细节
 *   LoadLibraryW("opengl32.dll")，GetProcAddress 取
 *   wglCreateContext / wglMakeCurrent / wglDeleteContext / wglGetProcAddress。
 *   任一缺失则 FreeLibrary 回滚并清空指针。
 */
Error GLContextManagerWin32::LoadOpenGLDriver() {
  opengl_module_ = LoadLibraryW(L"opengl32.dll");
  if (!opengl_module_) {
    ARHUD_LOG_ERROR("kFailed",
                    "GLContextManagerWin32: Failed to load opengl32.dll");
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

  if (!wgl_create_context_ || !wgl_make_current_ || !wgl_delete_context_ ||
      !wgl_get_proc_address_) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "GLContextManagerWin32: Failed to get WGL function pointers");
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

/**
 * @brief 用临时上下文加载 WGL 扩展
 *
 * @param[in] p_hdc 已配好像素格式的 HDC
 * @return true 成功；false 创建临时上下文或取 ARB 入口失败
 *
 * @par 实现步骤
 *   1. wglCreateContext(hdc) 建临时 2.1 上下文
 *   2. wglMakeCurrent(hdc, temp) 激活
 *   3. wglGetProcAddress("wglCreateContextAttribsARB") 取 ARB 入口
 *   4. LoadExtensions() 取 VSync 等扩展
 *   5. 解绑并删除临时上下文
 *
 * @note 仅首次 CreateContext() 调用一次（extensions_loaded_ 守卫）。
 */
bool GLContextManagerWin32::LoadWGLExtensions(HDC p_hdc) {
  // 创建临时上下文，仅用于获取 wglCreateContextAttribsARB
  HGLRC temp_context = wgl_create_context_(p_hdc);
  if (!temp_context) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "GLContextManagerWin32: Failed to create temp context (error=%lu)",
        GetLastError());
    return false;
  }

  if (!wgl_make_current_(p_hdc, temp_context)) {
    ARHUD_LOG_ERROR("kFailed",
                    "GLContextManagerWin32: Failed to make temp context "
                    "current (error=%lu)",
                    GetLastError());
    wgl_delete_context_(temp_context);
    return false;
  }

  wgl_create_context_attribs_arb_ =
      reinterpret_cast<WglCreateContextAttribsARBFunc>(wgl_get_proc_address_(
          reinterpret_cast<LPCSTR>("wglCreateContextAttribsARB")));

  if (!wgl_create_context_attribs_arb_) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "GLContextManagerWin32: wglCreateContextAttribsARB not available");
    wgl_make_current_(p_hdc, nullptr);
    wgl_delete_context_(temp_context);
    return false;
  }

  // 加载 VSync 等扩展
  LoadExtensions();

  // 清理临时上下文
  wgl_make_current_(p_hdc, nullptr);
  wgl_delete_context_(temp_context);

  return true;
}

/**
 * @brief 创建 OpenGL 3.3 Core Profile 上下文
 *
 * @param[in] p_hdc 设备上下文
 * @return HGLRC 新上下文；失败返回 nullptr
 *
 * @par 实现细节
 *   用 wglCreateContextAttribsARB(hdc, nullptr, attribs) 创建，
 *   属性：3.3 / Core Profile / 前向兼容 / 不共享资源（share=nullptr）。
 *   未加载 ARB 入口时直接失败。
 */
HGLRC GLContextManagerWin32::CreateCoreProfileContext(HDC p_hdc) {
  if (!wgl_create_context_attribs_arb_) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "GLContextManagerWin32: wglCreateContextAttribsARB not available");
    return nullptr;
  }

  int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB,
                   3,
                   WGL_CONTEXT_MINOR_VERSION_ARB,
                   3,
                   WGL_CONTEXT_PROFILE_MASK_ARB,
                   WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                   WGL_CONTEXT_FLAGS_ARB,
                   WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
                   0};

  HGLRC hrc = wgl_create_context_attribs_arb_(p_hdc, nullptr, attribs);
  if (!hrc) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "GLContextManagerWin32: Failed to create 3.3 core profile context "
        "(error=%lu)",
        GetLastError());
    return nullptr;
  }

  return hrc;
}

/**
 * @brief 加载 VSync 相关 WGL 扩展
 *
 * @par 实现细节
 *   取 wglSwapIntervalEXT / wglGetSwapIntervalEXT，
 *   探测 WGL_EXT_swap_control_tear（支持则 kAdaptive 可用 -1 间隔）。
 *   必须在上下文 current 时调用。
 */
void GLContextManagerWin32::LoadExtensions() {
  wgl_swap_interval_ext_ = reinterpret_cast<WglSwapIntervalEXTFunc>(
      wgl_get_proc_address_(reinterpret_cast<LPCSTR>("wglSwapIntervalEXT")));
  wgl_get_swap_interval_ext_ = reinterpret_cast<WglGetSwapIntervalEXTFunc>(
      wgl_get_proc_address_(reinterpret_cast<LPCSTR>("wglGetSwapIntervalEXT")));

  if (wgl_get_proc_address_(
          reinterpret_cast<LPCSTR>("wglSwapControlTearEXT"))) {
    swap_control_tear_supported_ = true;
  }

  if (wgl_swap_interval_ext_) {
    ARHUD_LOG_INFO("GLContextManagerWin32: WGL_EXT_swap_control available");
    if (swap_control_tear_supported_) {
      ARHUD_LOG_INFO(
          "GLContextManagerWin32: WGL_EXT_swap_control_tear available "
          "(adaptive VSync)");
    }
  } else {
    ARHUD_LOG_WARN(
        "GLContextManagerWin32: WGL_EXT_swap_control not available, "
        "VSync disabled");
  }
}

/**
 * @brief 按 ID 取 Surface 指针（非常量）
 *
 * @param[in] p_surface Surface ID
 * @return GLSurface* 命中返回；越界或已销毁返回 nullptr
 */
GLContextManagerWin32::GLSurface* GLContextManagerWin32::GetSurface(
    SurfaceID p_surface) {
  if (p_surface >= surfaces_.Size() || !surfaces_[p_surface].is_valid) {
    return nullptr;
  }
  return &surfaces_[p_surface];
}

/**
 * @brief 按 ID 取 Surface 常量指针（const 重载）
 *
 * @param[in] p_surface Surface ID
 * @return const GLSurface* 命中返回；越界或已销毁返回 nullptr
 */
const GLContextManagerWin32::GLSurface* GLContextManagerWin32::GetSurface(
    SurfaceID p_surface) const {
  if (p_surface >= surfaces_.Size() || !surfaces_[p_surface].is_valid) {
    return nullptr;
  }
  return &surfaces_[p_surface];
}

/**
 * @brief 按 ID 取 Context 指针（非常量）
 *
 * @param[in] p_context_id Context ID
 * @return GLContext* 命中返回；越界或已销毁返回 nullptr
 */
GLContextManagerWin32::GLContext* GLContextManagerWin32::GetContext(
    ContextID p_context_id) {
  if (p_context_id >= contexts_.Size() || !contexts_[p_context_id].is_valid) {
    return nullptr;
  }
  return &contexts_[p_context_id];
}

/**
 * @brief 按 ID 取 Context 常量指针（const 重载）
 *
 * @param[in] p_context_id Context ID
 * @return const GLContext* 命中返回；越界或已销毁返回 nullptr
 */
const GLContextManagerWin32::GLContext* GLContextManagerWin32::GetContext(
    ContextID p_context_id) const {
  if (p_context_id >= contexts_.Size() || !contexts_[p_context_id].is_valid) {
    return nullptr;
  }
  return &contexts_[p_context_id];
}

}  // namespace arhud

#endif  // ARHUD_PLATFORM_WINDOWS
