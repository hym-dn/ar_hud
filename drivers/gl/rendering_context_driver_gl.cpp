/**
 * @file rendering_context_driver_gl.cpp
 * @brief GLRenderingContextDriver — IRenderingContextDriver 的 OpenGL 实现
 *
 * 本文件实现渲染上下文驱动的 OpenGL 后端，核心职责：
 *   - 通过 IGLContextManager 管理 GL 上下文（HGLRC）和渲染表面（HDC）
 *   - 使用 glGetString 枚举 GPU 设备信息（厂商、型号、类型）
 *   - 创建 / 销毁渲染表面（Surface）和上下文（Context）
 *   - 工厂方法创建 GLRenderingDeviceDriver 实例
 *
 * @par 关键实现细节
 *   - Initialize() 采用两步法：先创建临时上下文加载 GL 函数，再创建正式上下文
 *   - SurfaceID 直接映射为 surfaces_ 数组索引，O(1) 查找
 *   - 已销毁的 Surface 标记为无效但不删除数组元素，保持索引稳定
 *   - 厂商识别通过 glGetString(GL_VENDOR) 字符串匹配 PCI Vendor ID
 *
 * @see rendering_context_driver_gl.h  类声明
 * @see rendering_context_driver.h     基类接口定义
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-08
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "rendering_context_driver_gl.h"

#include <glad/gl.h>

#include "io/logger.h"
#include "os/memory.h"
#include "rendering_device_driver_gl.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// 构造 / 析构
// ═══════════════════════════════════════════════════════════════════════

GLRenderingContextDriver::GLRenderingContextDriver(
    IGLContextManager* p_gl_manager)
    : gl_manager_(p_gl_manager) {}

GLRenderingContextDriver::~GLRenderingContextDriver() {
  if (initialized_) {
    Shutdown();
  }
}

// ═══════════════════════════════════════════════════════════════════════
// IRenderingContextDriver 接口实现 — 生命周期
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 初始化 OpenGL 渲染上下文驱动
 *
 * 执行流程：
 *   1. 校验 IGLContextManager 状态
 *   2. 创建临时 Surface + Context（用于加载 GL 函数）
 *   3. MakeCurrent 绑定临时上下文
 *   4. gladLoadGL() 加载 OpenGL 函数指针
 *   5. QueryDeviceInfo() 查询 GPU 厂商、型号、设备类型
 *   6. 销毁临时 Context + Surface（不保留）
 *
 * @note 不创建任何用户可见的 Surface/Context。
 *       用户通过 SurfaceCreate/ContextCreate 统一创建。
 *
 * @see IRenderingContextDriver::Initialize  接口文档
 */
Error GLRenderingContextDriver::Initialize(void* p_native_window) {
  if (initialized_) {
    return Error::kOK;
  }

  if (!gl_manager_ || !gl_manager_->IsInitialized()) {
    return Error::kUnconfigured;
  }

  // ── 创建临时 Surface + Context，用于 gladLoadGL 和设备查询 ──
  // 优先使用调用方提供的窗口句柄（SetPixelFormat 不能用于桌面 DC）
  IGLContextManager::SurfaceID tmp_surface =
      gl_manager_->CreateSurface(0, p_native_window, 1, 1);
  if (tmp_surface == IGLContextManager::kInvalidSurfaceId) {
    ARHUD_LOG_ERROR("kFailed",
                    "GLRenderingContextDriver: Failed to create temp surface "
                    "for GL loading");
    return Error::kFailed;
  }

  IGLContextManager::ContextID tmp_context =
      gl_manager_->CreateContext(tmp_surface);
  if (tmp_context == IGLContextManager::kInvalidContextId) {
    ARHUD_LOG_ERROR("kFailed",
                    "GLRenderingContextDriver: Failed to create temp context "
                    "for GL loading");
    gl_manager_->DestroySurface(tmp_surface);
    return Error::kFailed;
  }

  Error err = gl_manager_->MakeCurrent(tmp_context, tmp_surface);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "GLRenderingContextDriver: Failed to make temp context current");
    gl_manager_->DestroyContext(tmp_context);
    gl_manager_->DestroySurface(tmp_surface);
    return err;
  }

  // ── 加载 GL 函数指针 ──
  int gl_version = gladLoadGL(
      reinterpret_cast<GLADloadfunc>(gl_manager_->GetGLProcAddress()));
  if (gl_version == 0) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "GLRenderingContextDriver: Failed to load GL functions via glad");
    gl_manager_->DestroyContext(tmp_context);
    gl_manager_->DestroySurface(tmp_surface);
    return Error::kFailed;
  }

  // ── 查询 GPU 设备信息 ──
  QueryDeviceInfo();

  // ── 销毁临时上下文（GL 函数已加载，不再需要） ──
  gl_manager_->ReleaseCurrent();
  gl_manager_->DestroyContext(tmp_context);
  gl_manager_->DestroySurface(tmp_surface);

  initialized_ = true;

  return Error::kOK;
}

/**
 * @brief 关闭 OpenGL 渲染上下文驱动
 *
 * 逆序释放所有资源：
 *   1. 销毁所有 RCD 创建的 GL 上下文（HGLRC）
 *   2. 销毁所有渲染表面（HDC）
 *   3. 清空设备信息和 Surface 数组
 *
 * @note 不销毁外部注入的 IGLContextManager（由 Engine / DisplayServer 管理）
 * @see IRenderingContextDriver::Shutdown  接口文档
 */
void GLRenderingContextDriver::Shutdown() {
  if (!initialized_) {
    return;
  }

  for (uint32 i = 0; i < created_contexts_.Size(); ++i) {
    gl_manager_->DestroyContext(created_contexts_[i]);
  }
  created_contexts_.Clear();

  for (uint32 i = 0; i < surfaces_.Size(); ++i) {
    SurfaceInfo& info = surfaces_[i];
    if (info.gl_surface != IGLContextManager::kInvalidSurfaceId) {
      gl_manager_->DestroySurface(info.gl_surface);
    }
  }
  surfaces_.Clear();

  devices_.Clear();

  initialized_ = false;
}

// ═══════════════════════════════════════════════════════════════════════
// IRenderingContextDriver 接口实现 — 设备枚举
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 获取 GPU 设备数量
 *
 * @return 设备数量（OpenGL 通常为 1）
 *
 * @see IRenderingContextDriver::GetDeviceCount  接口文档
 */
uint32 GLRenderingContextDriver::GetDeviceCount() const {
  return static_cast<uint32>(devices_.Size());
}

/**
 * @brief 获取指定索引的 GPU 设备信息
 *
 * @param[in] p_device_index 设备索引
 *
 * @return Device 常引用
 *
 * @see IRenderingContextDriver::GetDevice  接口文档
 */
const IRenderingContextDriver::Device& GLRenderingContextDriver::GetDevice(
    uint32 p_device_index) const {
  return devices_[p_device_index];
}

/**
 * @brief 查询设备是否支持在指定 Surface 上呈现（GL：恒真占位）
 *
 * @par 占位实现说明
 *   GL 不存在"设备 × 表面"呈现支持矩阵 —— 呈现能力随
 *   wglMakeCurrent 隐式确定（绑定成功即可呈现）。本实现仅做
 *   索引边界检查（非有效性承诺），恒真式占位。
 *   真实主场是 Vulkan 设备选择（vkGetPhysicalDeviceSurfaceSupportKHR
 *   映射），GL 阶段保留为契约占位（见接口文档"占位说明"）。
 *
 * @param[in] p_device_index 设备索引（仅边界检查）
 * @param[in] p_surface      渲染表面 ID（仅边界检查，不校验 is_valid）
 *
 * @return true 两索引均未越界（占位恒真语义，非呈现能力承诺）
 *
 * @see IRenderingContextDriver::DeviceSupportsPresent  接口文档
 */
bool GLRenderingContextDriver::DeviceSupportsPresent(
    uint32 p_device_index, SurfaceID p_surface) const {
  return p_device_index < devices_.Size() && p_surface < surfaces_.Size();
}

// ═══════════════════════════════════════════════════════════════════════
// IRenderingContextDriver 接口实现 — 驱动工厂
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 OpenGL 渲染设备驱动
 *
 * 使用 ARHUD_NEW 分配 GLRenderingDeviceDriver 实例，
 * 注入 this 和 IGLContextManager 引用。
 *
 * @return IRenderingDeviceDriver 指针
 *
 * @see IRenderingContextDriver::CreateDeviceDriver  接口文档
 */
IRenderingDeviceDriver* GLRenderingContextDriver::CreateDeviceDriver() {
  GLRenderingDeviceDriver* driver =
      ARHUD_NEW(GLRenderingDeviceDriver)(this, gl_manager_);
  return driver;
}

/**
 * @brief 释放渲染设备驱动
 *
 * @param[in] p_driver 设备驱动指针（必须由 CreateDeviceDriver 创建）
 *
 * @see IRenderingContextDriver::DriverFree  接口文档
 */
void GLRenderingContextDriver::DriverFree(IRenderingDeviceDriver* p_driver) {
  ARHUD_DELETE(p_driver);
}

// ═══════════════════════════════════════════════════════════════════════
// IRenderingContextDriver 接口实现 — Surface 管理
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 OpenGL 渲染表面
 *
 * 通过 IGLContextManager::CreateSurface() 创建 HDC 并配置像素格式，
 * 将返回的 GL SurfaceID 记录到 surfaces_ 数组，RCD SurfaceID = 数组索引。
 *
 * @see IRenderingContextDriver::SurfaceCreate  接口文档
 */
IRenderingContextDriver::SurfaceID GLRenderingContextDriver::SurfaceCreate(
    IScreen::ScreenID p_screen_id, void* p_native_window, uint32 p_width,
    uint32 p_height) {
  if (!initialized_) {
    return kInvalidSurfaceId;
  }

  if (!p_native_window) {
    return kInvalidSurfaceId;
  }

  // 通过 IGLContextManager 创建渲染表面
  IGLContextManager::SurfaceID gl_surf = gl_manager_->CreateSurface(
      p_screen_id, p_native_window, p_width, p_height);

  if (gl_surf == IGLContextManager::kInvalidSurfaceId) {
    return kInvalidSurfaceId;
  }

  // 记录 Surface 信息
  SurfaceInfo info;
  info.gl_surface = gl_surf;
  info.width = p_width;
  info.height = p_height;
  info.needs_resize = false;

  surfaces_.PushBack(info);

  // SurfaceID = 数组索引
  SurfaceID new_id = static_cast<SurfaceID>(surfaces_.Size() - 1);
  return new_id;
}

/**
 * @brief 销毁 OpenGL 渲染表面
 *
 * 调用 IGLContextManager::DestroySurface() 释放 HDC，
 * 然后将数组元素标记为无效（gl_surface = kInvalidSurfaceId）。
 * 不删除数组元素，保持已有 SurfaceID 索引稳定。
 *
 * @see IRenderingContextDriver::SurfaceDestroy  接口文档
 */
void GLRenderingContextDriver::SurfaceDestroy(SurfaceID p_surface) {
  SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info) {
    return;
  }

  // 销毁关联的 GL 渲染表面
  if (info->gl_surface != IGLContextManager::kInvalidSurfaceId) {
    gl_manager_->DestroySurface(info->gl_surface);
  }

  // 标记为无效（不删除数组元素，保持索引稳定）
  info->gl_surface = IGLContextManager::kInvalidSurfaceId;
  info->width = 0;
  info->height = 0;
}

/**
 * @brief 设置渲染表面尺寸
 *
 * 更新 Surface 的宽高并标记需要 resize。
 *
 * @param[in] p_surface 渲染表面 ID
 * @param[in] p_width   新宽度
 * @param[in] p_height  新高度
 *
 * @see IRenderingContextDriver::SurfaceSetSize  接口文档
 */
void GLRenderingContextDriver::SurfaceSetSize(SurfaceID p_surface,
                                              uint32 p_width, uint32 p_height) {
  SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info) {
    return;
  }

  info->width = p_width;
  info->height = p_height;
  info->needs_resize = true;
}

/**
 * @brief 设置渲染表面的垂直同步模式
 *
 * 委托给 IGLContextManager::SetVsyncMode()。
 *
 * @param[in] p_surface 渲染表面 ID
 * @param[in] p_mode    垂直同步模式
 *
 * @see IRenderingContextDriver::SurfaceSetVsyncMode  接口文档
 */
void GLRenderingContextDriver::SurfaceSetVsyncMode(SurfaceID p_surface,
                                                   VSyncMode p_mode) {
  const SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info || info->gl_surface == IGLContextManager::kInvalidSurfaceId) {
    return;
  }

  gl_manager_->SetVsyncMode(info->gl_surface, p_mode);
}

/**
 * @brief 获取渲染表面的垂直同步模式
 *
 * @param[in] p_surface 渲染表面 ID
 *
 * @return 当前垂直同步模式，Surface 无效时返回 kEnabled
 *
 * @see IRenderingContextDriver::SurfaceGetVsyncMode  接口文档
 */
VSyncMode GLRenderingContextDriver::SurfaceGetVsyncMode(
    SurfaceID p_surface) const {
  const SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info || info->gl_surface == IGLContextManager::kInvalidSurfaceId) {
    return VSyncMode::kEnabled;
  }

  return gl_manager_->GetVsyncMode(info->gl_surface);
}

/**
 * @brief 获取渲染表面宽度
 *
 * @param[in] p_surface 渲染表面 ID
 *
 * @return 表面宽度（像素），Surface 无效时返回 0
 *
 * @see IRenderingContextDriver::SurfaceGetWidth  接口文档
 */
uint32 GLRenderingContextDriver::SurfaceGetWidth(SurfaceID p_surface) const {
  const SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info) {
    return 0;
  }

  return info->width;
}

/**
 * @brief 获取渲染表面高度
 *
 * @param[in] p_surface 渲染表面 ID
 *
 * @return 表面高度（像素），Surface 无效时返回 0
 *
 * @see IRenderingContextDriver::SurfaceGetHeight  接口文档
 */
uint32 GLRenderingContextDriver::SurfaceGetHeight(SurfaceID p_surface) const {
  const SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info) {
    return 0;
  }

  return info->height;
}

/**
 * @brief 设置渲染表面是否需要 resize
 *
 * @param[in] p_surface     渲染表面 ID
 * @param[in] p_needs_resize 是否需要 resize
 *
 * @see IRenderingContextDriver::SurfaceSetNeedsResize  接口文档
 */
void GLRenderingContextDriver::SurfaceSetNeedsResize(SurfaceID p_surface,
                                                     bool p_needs_resize) {
  SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info) {
    return;
  }

  info->needs_resize = p_needs_resize;
}

/**
 * @brief 查询渲染表面是否需要 resize
 *
 * @param[in] p_surface 渲染表面 ID
 *
 * @return true 需要 resize，Surface 无效时返回 false
 *
 * @see IRenderingContextDriver::SurfaceGetNeedsResize  接口文档
 */
bool GLRenderingContextDriver::SurfaceGetNeedsResize(
    SurfaceID p_surface) const {
  const SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info) {
    return false;
  }

  return info->needs_resize;
}

/**
 * @brief 获取 Surface 的底层 GL 表面句柄
 *
 * 返回 IGLContextManager 分配的 SurfaceID，供 RDD 层
 * 创建交换链和执行上下文绑定时使用。
 *
 * @param[in] p_surface RCD 层渲染表面 ID
 * @return IGLContextManager::SurfaceID，Surface 无效时返回 kInvalidSurfaceId
 *
 * @par 线程安全
 *   可在任意线程调用（只读操作）
 *
 * @see IRenderingContextDriver::SurfaceGetAPISurface  接口文档
 */
IGLContextManager::SurfaceID GLRenderingContextDriver::SurfaceGetAPISurface(
    SurfaceID p_surface) const {
  const SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info) {
    return IGLContextManager::kInvalidSurfaceId;
  }

  return info->gl_surface;
}

// ═══════════════════════════════════════════════════════════════════════
// IRenderingContextDriver 接口实现 — Context 管理
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 OpenGL 渲染上下文
 *
 * 复用 Surface 的 HDC（已配置像素格式），调用
 * IGLContextManager::CreateContext() 创建 HGLRC。新上下文 ID 记录到
 * created_contexts_ 数组，Shutdown() 时统一释放。
 *
 * @see IRenderingContextDriver::ContextCreate  接口文档
 */
IRenderingContextDriver::ContextID GLRenderingContextDriver::ContextCreate(
    SurfaceID p_surface) {
  if (!initialized_) {
    return kInvalidContextId;
  }

  SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info) {
    return kInvalidContextId;
  }

  IGLContextManager::ContextID gl_ctx =
      gl_manager_->CreateContext(info->gl_surface);

  if (gl_ctx != IGLContextManager::kInvalidContextId) {
    created_contexts_.PushBack(gl_ctx);
  }

  return static_cast<ContextID>(gl_ctx);
}

/**
 * @brief 销毁 OpenGL 渲染上下文
 *
 * 委托给 IGLContextManager::DestroyContext() 执行 wglDeleteContext。
 *
 * @see IRenderingContextDriver::ContextDestroy  接口文档
 */
/**
 * @brief 销毁 OpenGL 渲染上下文
 *
 * 委托给 IGLContextManager::DestroyContext() 执行 wglDeleteContext。
 *
 * @param[in] p_context_id 上下文 ID
 *
 * @see IRenderingContextDriver::ContextDestroy  接口文档
 */
void GLRenderingContextDriver::ContextDestroy(ContextID p_context_id) {
  if (p_context_id == kInvalidContextId) {
    return;
  }

  gl_manager_->DestroyContext(
      static_cast<IGLContextManager::ContextID>(p_context_id));
}

/**
 * @brief 获取当前存活的 GL 上下文数量
 *
 * 直接委托给 IGLContextManager::GetContextCount()。
 *
 * @return 上下文数量
 *
 * @see IRenderingContextDriver::GetContextCount  接口文档
 */
uint32 GLRenderingContextDriver::GetContextCount() const {
  return gl_manager_->GetContextCount();
}

// ═══════════════════════════════════════════════════════════════════════
// IRenderingContextDriver 接口实现 — 访问器
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 查询驱动是否已完成初始化
 *
 * @return true 已初始化，false 未初始化或已关闭
 *
 * @see IRenderingContextDriver::IsInitialized  接口文档
 */
bool GLRenderingContextDriver::IsInitialized() const {
  return initialized_;
}

/**
 * @brief 将 GL 上下文绑定到当前线程
 *
 * 将 RCD 层的 ContextID 和 SurfaceID 转换为 IGLContextManager 层的
 * ContextID 和 SurfaceID，然后委托给 IGLContextManager::MakeCurrent()
 * 执行 wglMakeCurrent(hdc, hglrc)。
 *
 * @see IRenderingContextDriver::MakeCurrent  接口文档
 */
Error GLRenderingContextDriver::MakeCurrent(ContextID p_context_id,
                                            SurfaceID p_surface) {
  if (!initialized_) {
    return Error::kFailed;
  }

  const SurfaceInfo* info = GetSurfaceInfo(p_surface);
  if (!info) {
    return Error::kInvalidParameter;
  }

  IGLContextManager::ContextID gl_ctx =
      static_cast<IGLContextManager::ContextID>(p_context_id);
  return gl_manager_->MakeCurrent(gl_ctx, info->gl_surface);
}

/**
 * @brief 释放当前线程的 GL 上下文
 *
 * 委托给 IGLContextManager::ReleaseCurrent()，
 * 最终执行 wglMakeCurrent(hdc, nullptr)。
 *
 * @see IRenderingContextDriver::ReleaseCurrent  接口文档
 */
void GLRenderingContextDriver::ReleaseCurrent() {
  if (!gl_manager_) {
    return;
  }

  gl_manager_->ReleaseCurrent();
}

// ═══════════════════════════════════════════════════════════════════════
// 内部方法
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 查询 GPU 设备信息
 *
 * 通过 GL 字符串查询填充 Device 结构。
 * 必须在 GL 上下文已绑定的线程上调用。
 *
 * @par 查询流程
 *   1. 查询 GL_VENDOR（厂商名称）
 *   2. 查询 GL_RENDERER（GPU 型号）
 *   3. 查询 GL_VERSION（GL 版本）
 *   4. 从厂商字符串推断 PCI 厂商 ID
 *   5. 从厂商 ID 推断设备类型
 *   6. 检查驱动变通方案
 *   7. 填充 Device 信息并添加到数组
 *
 * @par 线程安全
 *   - 应在 GL 上下文已绑定的线程调用
 *   - 不是线程安全的
 */
void GLRenderingContextDriver::QueryDeviceInfo() {
  const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
  if (vendor) {
    size_t len = 0;
    while (vendor[len] && len < sizeof(vendor_string_) - 1) {
      vendor_string_[len] = vendor[len];
      ++len;
    }
    vendor_string_[len] = '\0';
  } else {
    vendor_string_[0] = '\0';
  }

  const char* renderer =
      reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  if (renderer) {
    size_t len = 0;
    while (renderer[len] && len < sizeof(renderer_string_) - 1) {
      renderer_string_[len] = renderer[len];
      ++len;
    }
    renderer_string_[len] = '\0';
  } else {
    renderer_string_[0] = '\0';
  }

  const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  if (version) {
    size_t len = 0;
    while (version[len] && len < sizeof(version_string_) - 1) {
      version_string_[len] = version[len];
      ++len;
    }
    version_string_[len] = '\0';
  } else {
    version_string_[0] = '\0';
  }

  Device dev;
  dev.name = renderer_string_;
  dev.vendor = DetermineVendorId(vendor_string_);
  dev.type = DetermineDeviceType(dev.vendor);
  CheckWorkarounds(dev.vendor, renderer_string_, version_string_,
                   dev.workarounds);
  devices_.PushBack(dev);
}

/**
 * @brief 从 GL 厂商字符串推断 PCI 厂商 ID
 *
 * @param p_vendor_string glGetString(GL_VENDOR) 返回的字符串
 * @return PCI 厂商 ID
 *
 * @par 厂商识别策略
 *   - "NVIDIA Corporation" → Vendor::kNVIDIA (0x10DE)
 *   - "Intel" → Vendor::kIntel (0x8086)
 *   - "AMD" / "ATI" → Vendor::kAMD (0x1002)
 *   - "ARM" → Vendor::kARM (0x13B5)
 *   - "Qualcomm" → Vendor::kQualcomm (0x5143)
 *   - "Apple" → Vendor::kApple (0x106B)
 *   - "Microsoft" → Vendor::kMicrosoft (0x1414)
 *   - "Imagination" / "IMG" → Vendor::kImgTec (0x1010)
 *
 * @par 线程安全
 *   - 可在任意线程调用
 *   - 线程安全（只读操作）
 */
uint32 GLRenderingContextDriver::DetermineVendorId(
    const char* p_vendor_string) {
  if (!p_vendor_string || p_vendor_string[0] == '\0') {
    return Vendor::kUnknown;
  }

  // NVIDIA
  if (strstr(p_vendor_string, "NVIDIA") != nullptr) {
    return Vendor::kNVIDIA;
  }

  // Intel
  if (strstr(p_vendor_string, "Intel") != nullptr) {
    return Vendor::kIntel;
  }

  // AMD / ATI
  if (strstr(p_vendor_string, "AMD") != nullptr ||
      strstr(p_vendor_string, "ATI") != nullptr ||
      strstr(p_vendor_string, "Advanced Micro Devices") != nullptr) {
    return Vendor::kAMD;
  }

  // ARM (Mali)
  if (strstr(p_vendor_string, "ARM") != nullptr) {
    return Vendor::kARM;
  }

  // Qualcomm (Adreno)
  if (strstr(p_vendor_string, "Qualcomm") != nullptr) {
    return Vendor::kQualcomm;
  }

  // Apple
  if (strstr(p_vendor_string, "Apple") != nullptr) {
    return Vendor::kApple;
  }

  // Microsoft (WARP / D3D12 software rasterizer)
  if (strstr(p_vendor_string, "Microsoft") != nullptr) {
    return Vendor::kMicrosoft;
  }

  // Imagination Technologies (PowerVR)
  if (strstr(p_vendor_string, "Imagination") != nullptr ||
      strstr(p_vendor_string, "IMG") != nullptr) {
    return Vendor::kImgTec;
  }

  return Vendor::kUnknown;
}

/**
 * @brief 从厂商 ID 推断设备类型
 *
 * @param p_vendor_id PCI 厂商 ID
 * @return 推断的设备类型
 *
 * @par 设备类型推断
 *   - NVIDIA → kDiscreteGpu（NVIDIA 桌面/移动 GPU 均为独立显卡）
 *   - Intel → kIntegratedGpu（Intel UHD/Iris 为集成显卡）
 *   - AMD → kDiscreteGpu（简化处理：AMD 桌面平台通常为独立 GPU）
 *   - ARM/Qualcomm → kIntegratedGpu（移动端 SoC GPU）
 *   - Microsoft → kCpu（WARP / D3D12 software rasterizer）
 *   - Apple → kIntegratedGpu（Apple Silicon 为集成显卡）
 *
 * @par 线程安全
 *   - 可在任意线程调用
 *   - 线程安全（只读操作）
 */
IRenderingContextDriver::DeviceType
GLRenderingContextDriver::DetermineDeviceType(uint32 p_vendor_id) {
  switch (p_vendor_id) {
    case Vendor::kNVIDIA:
      return DeviceType::kDiscreteGpu;

    case Vendor::kIntel:
      return DeviceType::kIntegratedGpu;

    case Vendor::kAMD:
      // AMD 可能是集成或独立 GPU
      // 简化处理：AMD 桌面平台通常为独立 GPU
      return DeviceType::kDiscreteGpu;

    case Vendor::kARM:
    case Vendor::kQualcomm:
      return DeviceType::kIntegratedGpu;

    case Vendor::kMicrosoft:
      return DeviceType::kCpu;

    case Vendor::kApple:
      return DeviceType::kIntegratedGpu;

    default:
      return DeviceType::kOther;
  }
}

/**
 * @brief 检查驱动变通方案
 *
 * 根据厂商和驱动版本设置 Workarounds。
 *
 * @param p_vendor_id PCI 厂商 ID
 * @param p_renderer_string glGetString(GL_RENDERER) 返回的字符串
 * @param p_version_string glGetString(GL_VERSION) 返回的字符串
 * @param p_workarounds 填充的变通方案
 *
 * @par 已知驱动缺陷
 *   - AMD 驱动在某些版本中，绘制命令后立即执行计算命令可能导致 GPU 挂起
 *   - 设置 avoid_compute_after_draw = true
 *
 * @par 扩展方式
 *   其他厂商特定变通方案可在此扩展。
 *   可根据 p_renderer_string 和 p_version_string 进一步细化。
 *
 * @par 线程安全
 *   - 可在任意线程调用
 *   - 线程安全（只读操作）
 */
void GLRenderingContextDriver::CheckWorkarounds(uint32 p_vendor_id,
                                                const char* p_renderer_string,
                                                const char* p_version_string,
                                                Workarounds& p_workarounds) {
  // AMD 驱动在某些版本中，绘制命令后立即执行计算命令可能导致 GPU 挂起
  if (p_vendor_id == Vendor::kAMD) {
    p_workarounds.avoid_compute_after_draw = true;
  }

  // 其他厂商特定变通方案可在此扩展
  (void)p_renderer_string;
  (void)p_version_string;
}

/**
 * @brief 获取 Surface 信息指针
 *
 * @param p_surface Surface ID
 * @return SurfaceInfo 指针
 * @retval nullptr Surface 不存在或已销毁
 *
 * @par 验证逻辑
 *   1. 检查 Surface ID 是否在有效范围内
 *   2. 检查 Surface 是否已销毁（gl_surface == kInvalidSurfaceId）
 *
 * @par 线程安全
 *   - 可在任意线程调用
 *   - 线程安全（只读操作）
 */
IRenderingContextDriver::SurfaceInfo* GLRenderingContextDriver::GetSurfaceInfo(
    SurfaceID p_surface) {
  if (p_surface >= surfaces_.Size()) {
    return nullptr;
  }

  SurfaceInfo* info = &surfaces_[p_surface];

  // 已销毁的 Surface 返回 nullptr
  if (info->gl_surface == IGLContextManager::kInvalidSurfaceId) {
    return nullptr;
  }

  return info;
}

/**
 * @brief 获取 Surface 信息常量指针
 *
 * @param p_surface Surface ID
 * @return const SurfaceInfo 指针
 * @retval nullptr Surface 不存在或已销毁
 *
 * @par 验证逻辑
 *   1. 检查 Surface ID 是否在有效范围内
 *   2. 检查 Surface 是否已销毁（gl_surface == kInvalidSurfaceId）
 *
 * @par 线程安全
 *   - 可在任意线程调用
 *   - 线程安全（只读操作）
 */
const IRenderingContextDriver::SurfaceInfo*
GLRenderingContextDriver::GetSurfaceInfo(SurfaceID p_surface) const {
  if (p_surface >= surfaces_.Size()) {
    return nullptr;
  }

  const SurfaceInfo* info = &surfaces_[p_surface];

  if (info->gl_surface == IGLContextManager::kInvalidSurfaceId) {
    return nullptr;
  }

  return info;
}

}  // namespace arhud