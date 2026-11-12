#ifndef ARHUD_PLATFORM_WINDOWS_GPU_INTEROP_WIN32_H_
#define ARHUD_PLATFORM_WINDOWS_GPU_INTEROP_WIN32_H_

/**
 * @file gpu_interop_win32.h
 * @brief Win32 跨 API 共享实现——WGL_NV_DX_interop2（OpenGL ↔ D3D11）。
 *
 * @par 形态
 *   无状态单例（全部状态在调用方持有的不透明句柄里），因此可安全
 *   进程内共享；类型平凡可析构，符合"全局对象须平凡可析构"约束。
 *
 * @par 可用性探测
 *   扩展字符串 + 入口点双重校验：仅 `wglGetProcAddress` 返回非空不足够
 *   （失败值可能是 1..3 的伪地址）。任一条件不满足即视为不支持，
 *   调用方须回落 Mode R 回读路径。
 *
 * @par 为何需要 interop2
 *   interop1 的资源表只有 DX9 类型；DX10/11 资源由 interop2 追加
 *   （且明确"DX11 资源无需 share handle"）。只探到 interop1 的驱动
 *   会给出可调用但注册 DX11 资源必败的行为。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-11-08
 * @copyright Copyright (c) 2026 ARHud Project
 * @ingroup platform
 */

#include "gpu_interop.h"

#ifdef ARHUD_PLATFORM_WINDOWS

namespace arhud {

/**
 * @brief WGL_NV_DX_interop2 实现（零回读上屏的 GL 侧别名）
 *
 * 要求 D3D11 device 为 multithreaded（扩展硬约束）且与 GL context
 * 落在同一适配器，否则 `OpenDevice` 返回 nullptr
 * （混合显卡/多适配器是本路径的主要风险点，须实测确认）。
 */
class GPUInteropWin32 : public IGPUInterop {
 public:
  /**
   * @brief 进程内共享实例（无状态，平凡可析构）
   *
   * @return 永远非空的接口指针
   */
  static IGPUInterop* GetSingleton();

  void* OpenDevice(void* p_native_device) override;
  void CloseDevice(void* p_device_handle) override;

  void* CreateAlias(void* p_device_handle, void* p_native_resource,
                    uint32 p_gl_name, uint32 p_gl_target,
                    bool p_writable) override;
  void DestroyAlias(void* p_device_handle, void* p_alias_handle) override;

  bool AcquireForWriting(void* p_device_handle, void* p_alias_handle) override;
  bool ReleaseFromWriting(void* p_device_handle,
                          void* p_alias_handle) override;

  const char* GetName() const override {
    return "wgl_nv_dx_interop2";
  }

 private:
  /** @brief 装载 wglDX* 入口点并校验扩展字符串（幂等，失败只尝试一次） */
  bool EnsureEntries();

  bool entries_attempted_ = false;  ///< 是否已尝试装载
  bool entries_ok_ = false;         ///< 入口点与扩展字符串均可用

  void* open_device_fn_ = nullptr;       ///< wglDXOpenDeviceNV
  void* close_device_fn_ = nullptr;      ///< wglDXCloseDeviceNV
  void* create_alias_fn_ = nullptr;      ///< wglDXRegisterObjectNV
  void* destroy_alias_fn_ = nullptr;     ///< wglDXUnregisterObjectNV
  void* acquire_write_fn_ = nullptr;     ///< wglDXLockObjectsNV
  void* release_write_fn_ = nullptr;     ///< wglDXUnlockObjectsNV
};

}  // namespace arhud
#endif  // ARHUD_PLATFORM_WINDOWS

#endif  // ARHUD_PLATFORM_WINDOWS_GPU_INTEROP_WIN32_H_
