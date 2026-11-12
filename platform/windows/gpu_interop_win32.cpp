/**
 * @file gpu_interop_win32.cpp
 * @brief WGL_NV_DX_interop2 实现——D3D11 显存别名成 GL 可写对象。
 *
 * @par 职责边界
 *   本文件只做 WGL 扩展的装载与调用转发：入口点全部经平台统一加载器
 *   （CombinedGLLoader：wglGetProcAddress 优先 + opengl32.dll 回退），
 *   GL 侧的调用时机与配对由 drivers/gl 的 _gl_dx.cpp 负责。
 *
 * @par 入口点判定的坑
 *   `wglGetProcAddress` 对"函数不存在"可返回 1..3 的伪地址，必须按
 *   `> 3` 判定；而统一加载器只在 nullptr 时回退，故本层复查一次
 *   （否则会把伪地址当真函数调用，崩在驱动里）。
 *
 * @par 句柄语义
 *   - `p_device_handle` = HANDLE（wglDXOpenDeviceNV 返回）
 *   - `p_alias_handle`  = HANDLE（wglDXRegisterObjectNV 返回）
 *   两者都由调用方（GL 后端）保存并配对归还；本实现不持有所有权。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-11-08
 * @copyright Copyright (c) 2026 ARHud Project
 * @ingroup platform
 */

#include "gpu_interop_win32.h"

#ifdef ARHUD_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

#include "gl_context_manager_win32.h"
#include "io/logger.h"

namespace arhud {
namespace {

// WGL_NV_DX_interop 的 access 取值（wglDXRegisterObjectNV 的 access 参数）
constexpr uint32 kWglAccessReadOnlyNV = 0x0000;
constexpr uint32 kWglAccessWriteDiscardNV = 0x0002;

// GL 类型（GLenum/GLuint）在本层用 unsigned int 承载：platform 不依赖
// glad，而两者在 Windows ABI 下同宽，入口点签名可安全互转。
using GLNameUInt = unsigned int;

using WglDXOpenDeviceFunc = HANDLE(WINAPI*)(void* dx_device);
using WglDXCloseDeviceFunc = BOOL(WINAPI*)(HANDLE dx_device);
using WglDXRegisterObjectFunc = HANDLE(WINAPI*)(HANDLE dx_device,
                                               void* dx_object,
                                               GLNameUInt name,
                                               GLNameUInt type,
                                               GLNameUInt access);
using WglDXUnregisterObjectFunc = BOOL(WINAPI*)(HANDLE dx_device,
                                                HANDLE dx_object);
using WglDXLockObjectsFunc = BOOL(WINAPI*)(HANDLE dx_device, int count,
                                           HANDLE* dx_objects);
using WglDXUnlockObjectsFunc = BOOL(WINAPI*)(HANDLE dx_device, int count,
                                             HANDLE* dx_objects);
using WglGetExtensionsStringFunc = const char*(WINAPI*)(HDC dc);

/**
 * @brief 判定 WGL 扩展入口是否为真函数
 *
 * @param[in] p_entry 加载器返回值
 * @return true 可用（伪地址 0..3 判为不可用）
 */
inline bool IsValidWglEntry(void* p_entry) {
  return reinterpret_cast<uintptr_t>(p_entry) > 3u;
}

/**
 * @brief 经平台统一加载器取 WGL 扩展入口（含伪地址复查）
 *
 * @param[in] p_name 函数名
 * @return 函数指针；不可用返回 nullptr
 */
void* LoadWglEntry(const char* p_name) {
  void* entry = GLContextManagerWin32::CombinedGLLoader(p_name);
  return IsValidWglEntry(entry) ? entry : nullptr;
}

/**
 * @brief 读取当前上下文的 WGL 扩展字符串
 *
 * 优先 WGL_ARB_extensions_string（需当前 DC），退化到
 * WGL_EXT_extensions_string（opengl32 直接导出，Vista+ 恒有）。
 *
 * @return 扩展字符串；无法获取返回 nullptr
 */
const char* QueryWglExtensions() {
  HDC dc = wglGetCurrentDC();
  if (dc == nullptr) {
    return nullptr;
  }

  void* arb_entry = LoadWglEntry("wglGetExtensionsStringARB");
  if (arb_entry != nullptr) {
    auto fn = reinterpret_cast<WglGetExtensionsStringFunc>(arb_entry);
    const char* extensions = fn(dc);
    if (extensions != nullptr) {
      return extensions;
    }
  }

  void* ext_entry = LoadWglEntry("wglGetExtensionsStringEXT");
  if (ext_entry != nullptr) {
    auto fn = reinterpret_cast<WglGetExtensionsStringFunc>(ext_entry);
    return fn(dc);
  }
  return nullptr;
}

/**
 * @brief 在空格分隔的扩展字符串中整词查找扩展名
 *
 * @param[in] p_extensions 扩展字符串（可为 null）
 * @param[in] p_name       待查扩展名
 * @return true 存在
 */
bool HasWglExtension(const char* p_extensions, const char* p_name) {
  if (p_extensions == nullptr || p_name == nullptr) {
    return false;
  }
  const size_t name_len = std::strlen(p_name);
  const char* cursor = p_extensions;
  while ((cursor = std::strstr(cursor, p_name)) != nullptr) {
    const bool left_ok = cursor == p_extensions || cursor[-1] == ' ';
    const char* after = cursor + name_len;
    const bool right_ok = *after == '\0' || *after == ' ';
    if (left_ok && right_ok) {
      return true;
    }
    cursor = after;
  }
  return false;
}

}  // namespace

IGPUInterop* GPUInteropWin32::GetSingleton() {
  static GPUInteropWin32 s_instance;
  return &s_instance;
}

bool GPUInteropWin32::EnsureEntries() {
  if (entries_attempted_) {
    return entries_ok_;
  }
  entries_attempted_ = true;

  const char* extensions = QueryWglExtensions();
  // interop2 是 DX10/11 资源的门槛；只探到 interop1 会在注册时必败
  if (!HasWglExtension(extensions, "WGL_NV_DX_interop2")) {
    ARHUD_LOG_INFO(
        "GPUInteropWin32: WGL_NV_DX_interop2 absent — zero-copy present "
        "path unavailable");
    return entries_ok_;
  }

  open_device_fn_ = LoadWglEntry("wglDXOpenDeviceNV");
  close_device_fn_ = LoadWglEntry("wglDXCloseDeviceNV");
  create_alias_fn_ = LoadWglEntry("wglDXRegisterObjectNV");
  destroy_alias_fn_ = LoadWglEntry("wglDXUnregisterObjectNV");
  acquire_write_fn_ = LoadWglEntry("wglDXLockObjectsNV");
  release_write_fn_ = LoadWglEntry("wglDXUnlockObjectsNV");

  entries_ok_ = open_device_fn_ != nullptr && close_device_fn_ != nullptr &&
                create_alias_fn_ != nullptr && destroy_alias_fn_ != nullptr &&
                acquire_write_fn_ != nullptr && release_write_fn_ != nullptr;

  if (!entries_ok_) {
    ARHUD_LOG_WARN(
        "GPUInteropWin32: extension advertised but entry points missing — "
        "zero-copy present path unavailable");
  }
  return entries_ok_;
}

void* GPUInteropWin32::OpenDevice(void* p_native_device) {
  if (p_native_device == nullptr || !EnsureEntries()) {
    return nullptr;
  }

  auto open_fn = reinterpret_cast<WglDXOpenDeviceFunc>(open_device_fn_);
  HANDLE device = open_fn(p_native_device);
  if (device == nullptr) {
    // 典型失败原因：DX11 device 非 multithreaded、与 GL context 不在
    // 同一适配器（混合显卡），或驱动不支持 DX11 资源注册。
    ARHUD_LOG_INFO(
        "GPUInteropWin32: wglDXOpenDeviceNV failed (error=%lu) — adapter "
        "mismatch or device not multithreaded?",
        GetLastError());
    return nullptr;
  }
  return device;
}

void GPUInteropWin32::CloseDevice(void* p_device_handle) {
  if (p_device_handle == nullptr) {
    return;
  }
  auto close_fn = reinterpret_cast<WglDXCloseDeviceFunc>(close_device_fn_);
  if (!close_fn(static_cast<HANDLE>(p_device_handle))) {
    ARHUD_LOG_WARN("GPUInteropWin32: wglDXCloseDeviceNV failed (error=%lu)",
                   GetLastError());
  }
}

void* GPUInteropWin32::CreateAlias(void* p_device_handle,
                                   void* p_native_resource, uint32 p_gl_name,
                                   uint32 p_gl_target, bool p_writable) {
  if (p_device_handle == nullptr || p_native_resource == nullptr ||
      p_gl_name == 0) {
    return nullptr;
  }

  // 只写用途取 WRITE_DISCARD：显示侧不需要读旧内容，驱动可跳过
  // 保留上一帧内容的开销。
  const GLNameUInt access =
      p_writable ? kWglAccessWriteDiscardNV : kWglAccessReadOnlyNV;

  auto register_fn =
      reinterpret_cast<WglDXRegisterObjectFunc>(create_alias_fn_);
  HANDLE alias = register_fn(static_cast<HANDLE>(p_device_handle),
                             p_native_resource,
                             static_cast<GLNameUInt>(p_gl_name),
                             static_cast<GLNameUInt>(p_gl_target), access);
  if (alias == nullptr) {
    ARHUD_LOG_WARN(
        "GPUInteropWin32: wglDXRegisterObjectNV failed (error=%lu, "
        "gl_name=%u target=0x%04X)",
        GetLastError(), p_gl_name, p_gl_target);
    return nullptr;
  }
  return alias;
}

void GPUInteropWin32::DestroyAlias(void* p_device_handle,
                                   void* p_alias_handle) {
  if (p_device_handle == nullptr || p_alias_handle == nullptr) {
    return;
  }
  auto unregister_fn =
      reinterpret_cast<WglDXUnregisterObjectFunc>(destroy_alias_fn_);
  if (!unregister_fn(static_cast<HANDLE>(p_device_handle),
                     static_cast<HANDLE>(p_alias_handle))) {
    // 占用中解除别名会失败——那属调用顺序错误，必须留痕
    ARHUD_LOG_WARN("GPUInteropWin32: wglDXUnregisterObjectNV failed "
                   "(error=%lu) — still held by GL?",
                   GetLastError());
  }
}

bool GPUInteropWin32::AcquireForWriting(void* p_device_handle,
                                        void* p_alias_handle) {
  if (p_device_handle == nullptr || p_alias_handle == nullptr) {
    return false;
  }
  HANDLE alias = static_cast<HANDLE>(p_alias_handle);
  auto lock_fn = reinterpret_cast<WglDXLockObjectsFunc>(acquire_write_fn_);
  if (lock_fn(static_cast<HANDLE>(p_device_handle), 1, &alias)) {
    return true;
  }
  // ERROR_BUSY / ERROR_LOCK_FAILED 是"对端仍在占用"的正常背压信号，
  // 用 Info 级避免刷 WARN。
  ARHUD_LOG_INFO("GPUInteropWin32: wglDXLockObjectsNV failed (error=%lu)",
                 GetLastError());
  return false;
}

bool GPUInteropWin32::ReleaseFromWriting(void* p_device_handle,
                                         void* p_alias_handle) {
  if (p_device_handle == nullptr || p_alias_handle == nullptr) {
    return false;
  }
  HANDLE alias = static_cast<HANDLE>(p_alias_handle);
  auto unlock_fn =
      reinterpret_cast<WglDXUnlockObjectsFunc>(release_write_fn_);
  if (unlock_fn(static_cast<HANDLE>(p_device_handle), 1, &alias)) {
    return true;
  }
  ARHUD_LOG_WARN("GPUInteropWin32: wglDXUnlockObjectsNV failed (error=%lu)",
                 GetLastError());
  return false;
}

}  // namespace arhud
#endif  // ARHUD_PLATFORM_WINDOWS
