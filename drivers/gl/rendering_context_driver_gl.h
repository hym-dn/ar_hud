/**
 * @file rendering_context_driver_gl.h
 * @brief OpenGL 渲染上下文驱动实现
 *
 * GLRenderingContextDriver 是 IRenderingContextDriver 的 OpenGL 实现，
 * 通过 IGLContextManager 管理 GL 上下文，使用 GL 字符串查询枚举 GPU 设备。
 *
 * @par 架构定位
 *   @code
 *   IRenderingContextDriver (接口)
 *       └── GLRenderingContextDriver (OpenGL 实现)
 *               ├── 持有 IGLContextManager* 引用（不拥有所有权）
 *               ├── 管理 SurfaceInfo[] 数组
 *               ├── 持有 Device[] 数组（GL 设备信息）
 *               └── 创建 GLDeviceDriver 实例（未来实现）
 *   @endcode
 *
 * @par 与 Vulkan RCD 的对比
 *   | 维度         | OpenGL RCD                    | Vulkan RCD |
 *   |-------------|-------------------------------|-------------------------------|
 *   | API 实例化   | 无需（GL 无 Instance 概念）      | VkInstance 创建 | |
 * 设备枚举     | glGetString 查询当前设备         | vkEnumeratePhysicalDevices
 * | | 设备数量     | 始终 1（当前上下文所在设备）      | 可多个 | | Surface |
 * IGLContextManager 屏幕上下文映射 | VkSurfaceKHR                  | | 驱动创建
 * | GLDeviceDriver（直接 GL 调用）  | VkDevice + 队列选择            |
 *
 * @par 厂商识别策略
 *   OpenGL 通过 glGetString(GL_VENDOR) 返回字符串匹配厂商 ID：
 *   - "NVIDIA Corporation" → Vendor::kNVIDIA (0x10DE)
 *   - "Intel"              → Vendor::kIntel (0x8086)
 *   - "AMD" / "ATI"        → Vendor::kAMD (0x1002)
 *   - "ARM"                → Vendor::kARM (0x13B5)
 *   - "Qualcomm"           → Vendor::kQualcomm (0x5143)
 *
 * @par 设备类型推断
 *   - NVIDIA → kDiscreteGpu（NVIDIA 桌面/移动 GPU 均为独立显卡）
 *   - Intel  → kIntegratedGpu（Intel UHD/Iris 为集成显卡）
 *   - AMD    → 需要检查名称区分集成/独立
 *   - ARM/Qualcomm → kIntegratedGpu（移动端 SoC GPU）
 *
 * @par 设计参考
 *   - Godot 4.6 RenderingContextDriverVulkan（设备枚举模式）
 *   - Godot 4.6 GLManagerNative_Windows（GL 上下文管理）
 *   - arch_skill.md §1.1 三层 GPU API 抽象
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-08
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_DRIVERS_GL_RENDERING_CONTEXT_DRIVER_GL_H_
#define ARHUD_DRIVERS_GL_RENDERING_CONTEXT_DRIVER_GL_H_

#include "gl_context_manager.h"
#include "rendering_context_driver.h"

namespace arhud {

/**
 * @brief OpenGL 渲染上下文驱动
 *
 * 通过 IGLContextManager 管理 GL 上下文，使用 GL 字符串查询枚举 GPU 设备。
 * Surface 操作直接映射到 IGLContextManager 的屏幕上下文操作。
 *
 * @par 关键设计
 *   - 不拥有 IGLContextManager（由外部注入，通常是 Engine 或 DisplayServer）
 *   - Surface 数组使用 LocalVector，索引即为 SurfaceID
 *   - 设备信息在 Initialize() 时一次性查询并缓存
 *   - 驱动变通方案在设备枚举时根据厂商设置
 *
 * @par 线程安全
 *   - Initialize / Shutdown / SurfaceCreate / SurfaceDestroy 应在主线程调用
 *   - SurfaceSetSize / SurfaceSetVsyncMode 可在任意线程调用
 *   - ContextCreate / ContextDestroy 应在主线程调用
 *
 * @see IRenderingContextDriver  基类接口
 */
class GLRenderingContextDriver : public IRenderingContextDriver {
 public:
  /**
   * @brief 构造函数
   *
   * @param[in] p_gl_manager GL 管理器指针（不拥有所有权）
   *
   * @pre p_gl_manager 非 nullptr
   * @pre p_gl_manager->Initialize() 已成功调用
   */
  explicit GLRenderingContextDriver(IGLContextManager* p_gl_manager);

  /**
   * @brief 析构函数
   *
   * 自动调用 Shutdown() 释放资源。
   * 不销毁 IGLContextManager（由外部管理）。
   */
  ~GLRenderingContextDriver() override;

  // 禁止拷贝和移动
  ARHUD_DISABLE_COPY_MOVE(GLRenderingContextDriver);

  // ─── IRenderingContextDriver 接口实现 ─────────────────────────

  /**
   * @copydoc IRenderingContextDriver::Initialize()
   *
   * @par OpenGL 实现流程
   *   1. 通过 IGLContextManager 创建临时 Surface + Context
   *   2. MakeCurrent 绑定临时上下文
   *   3. gladLoadGL() 加载 GL 函数指针
   *   4. QueryDeviceInfo() 枚举 GPU 设备信息
   *   5. 销毁临时 Surface + Context（不保留）
   *
   * @note 不创建任何用户可见的 Surface/Context。
   *       用户通过 SurfaceCreate/ContextCreate 统一创建。
   */
  Error Initialize(void* p_native_window = nullptr) override;

  /**
   * @copydoc IRenderingContextDriver::Shutdown()
   *
   * @par OpenGL 实现
   *   逆序销毁所有 RCD 创建的 GL 上下文和 Surface，
   *   清空设备信息和 Surface 数组。不销毁外部注入的 IGLContextManager。
   */
  void Shutdown() override;

  /**
   * @copydoc IRenderingContextDriver::GetDeviceCount()
   * @note OpenGL 始终返回 1（只能看到当前上下文所在 GPU）
   */
  uint32 GetDeviceCount() const override;

  /**
   * @copydoc IRenderingContextDriver::GetDevice(uint32) const
   */
  const Device& GetDevice(uint32 p_device_index) const override;

  /**
   * @copydoc IRenderingContextDriver::DeviceSupportsPresent(uint32,SurfaceID)
   * const
   * @note OpenGL 始终返回 true（上下文已绑定到特定设备）
   */
  bool DeviceSupportsPresent(uint32 p_device_index,
                             SurfaceID p_surface) const override;

  /**
   * @copydoc IRenderingContextDriver::CreateDeviceDriver()
   *
   * @par OpenGL 实现
   *   创建 GLRenderingDeviceDriver 实例，注入 this 和 IGLContextManager 引用。
   */
  IRenderingDeviceDriver* CreateDeviceDriver() override;

  /**
   * @copydoc IRenderingContextDriver::DriverFree(IRenderingDeviceDriver*)
   */
  void DriverFree(IRenderingDeviceDriver* p_driver) override;

  /**
   * @copydoc
   * IRenderingContextDriver::SurfaceCreate(IScreen::ScreenID,void*,uint32,uint32)
   *
   * @par OpenGL 实现
   *   调用 IGLContextManager::CreateSurface() 创建渲染表面，
   *   SurfaceID = surfaces_ 数组索引。
   */
  SurfaceID SurfaceCreate(IScreen::ScreenID p_screen_id, void* p_native_window,
                          uint32 p_width, uint32 p_height) override;

  /**
   * @copydoc IRenderingContextDriver::SurfaceDestroy(SurfaceID)
   *
   * @par OpenGL 实现
   *   调用 IGLContextManager::DestroySurface() 释放 GL 表面，
   *   数组元素标记为无效（不删除，保持索引稳定）。
   */
  void SurfaceDestroy(SurfaceID p_surface) override;

  /**
   * @copydoc IRenderingContextDriver::SurfaceSetSize(SurfaceID,uint32,uint32)
   */
  void SurfaceSetSize(SurfaceID p_surface, uint32 p_width,
                      uint32 p_height) override;

  /**
   * @copydoc IRenderingContextDriver::SurfaceSetVsyncMode(SurfaceID,VSyncMode)
   *
   * @par OpenGL 实现
   *   委托给 IGLContextManager::SetVsyncMode()（内部调用 wglSwapIntervalEXT）。
   */
  void SurfaceSetVsyncMode(SurfaceID p_surface, VSyncMode p_mode) override;

  /**
   * @copydoc IRenderingContextDriver::SurfaceGetVsyncMode(SurfaceID) const
   */
  VSyncMode SurfaceGetVsyncMode(SurfaceID p_surface) const override;

  /**
   * @copydoc IRenderingContextDriver::SurfaceGetWidth(SurfaceID) const
   */
  uint32 SurfaceGetWidth(SurfaceID p_surface) const override;

  /**
   * @copydoc IRenderingContextDriver::SurfaceGetHeight(SurfaceID) const
   */
  uint32 SurfaceGetHeight(SurfaceID p_surface) const override;

  /**
   * @copydoc IRenderingContextDriver::SurfaceSetNeedsResize(SurfaceID,bool)
   */
  void SurfaceSetNeedsResize(SurfaceID p_surface, bool p_needs_resize) override;

  /**
   * @copydoc IRenderingContextDriver::SurfaceGetNeedsResize(SurfaceID) const
   */
  bool SurfaceGetNeedsResize(SurfaceID p_surface) const override;

  /**
   * @copydoc IRenderingContextDriver::SurfaceGetAPISurface(SurfaceID) const
   */
  IGLContextManager::SurfaceID SurfaceGetAPISurface(
      SurfaceID p_surface) const override;

  /**
   * @copydoc IRenderingContextDriver::ContextCreate(SurfaceID)
   *
   * @par OpenGL 实现
   *   调用 IGLContextManager::CreateContext() 创建 HGLRC，
   *   复用 Surface 的 HDC（已配置像素格式）。
   */
  ContextID ContextCreate(SurfaceID p_surface) override;

  /**
   * @copydoc IRenderingContextDriver::ContextDestroy(ContextID)
   */
  void ContextDestroy(ContextID p_context_id) override;

  /**
   * @copydoc IRenderingContextDriver::GetContextCount() const
   */
  uint32 GetContextCount() const override;

  /**
   * @copydoc IRenderingContextDriver::IsInitialized() const
   */
  bool IsInitialized() const override;

  /**
   * @copydoc IRenderingContextDriver::MakeCurrent(ContextID,SurfaceID)
   *
   * @par OpenGL 实现
   *   委托给 IGLContextManager::MakeCurrent(gl_ctx, gl_surface)，
   *   最终执行 wglMakeCurrent(hdc, hglrc)。
   */
  Error MakeCurrent(ContextID p_context_id, SurfaceID p_surface) override;

  /**
   * @copydoc IRenderingContextDriver::ReleaseCurrent()
   *
   * @par OpenGL 实现
   *   委托给 IGLContextManager::ReleaseCurrent()，
   *   最终执行 wglMakeCurrent(hdc, nullptr)。
   */
  void ReleaseCurrent() override;

 private:
  // ─── 内部方法 ────────────────────────────────────────────────

  /**
   * @brief 查询 GPU 设备信息
   *
   * 通过 GL 字符串查询填充 Device 结构。
   * 必须在 GL 上下文已绑定的线程上调用。
   */
  void QueryDeviceInfo();

  /**
   * @brief 从 GL 厂商字符串推断 PCI 厂商 ID
   *
   * @param[in] p_vendor_string glGetString(GL_VENDOR) 返回的字符串
   * @return PCI 厂商 ID
   */
  static uint32 DetermineVendorId(const char* p_vendor_string);

  /**
   * @brief 从厂商 ID 推断设备类型
   *
   * @param[in] p_vendor_id PCI 厂商 ID
   * @return 推断的设备类型
   */
  static DeviceType DetermineDeviceType(uint32 p_vendor_id);

  /**
   * @brief 检查驱动变通方案
   *
   * 根据厂商和驱动版本设置 Workarounds。
   *
   * @param[in] p_vendor_id PCI 厂商 ID
   * @param[in] p_renderer_string glGetString(GL_RENDERER) 返回的字符串
   * @param[in] p_version_string glGetString(GL_VERSION) 返回的字符串
   * @param[out] p_workarounds 填充的变通方案
   */
  static void CheckWorkarounds(uint32 p_vendor_id,
                               const char* p_renderer_string,
                               const char* p_version_string,
                               Workarounds& p_workarounds);

  /**
   * @brief 获取 Surface 信息指针
   *
   * @param[in] p_surface Surface ID
   * @return SurfaceInfo 指针
   * @retval nullptr Surface 不存在
   */
  SurfaceInfo* GetSurfaceInfo(SurfaceID p_surface);
  const SurfaceInfo* GetSurfaceInfo(SurfaceID p_surface) const;

  // ─── 成员变量 ────────────────────────────────────────────────

  /** @brief GL 管理器指针（不拥有所有权） */
  IGLContextManager* gl_manager_ = nullptr;

  /** @brief 是否已初始化 */
  bool initialized_ = false;

  /** @brief GPU 设备信息数组（OpenGL 通常只有 1 个设备） */
  LocalVector<Device> devices_;

  /** @brief 渲染表面信息数组（索引 = SurfaceID） */
  LocalVector<SurfaceInfo> surfaces_;

  /** @brief RCD 创建的 GL 上下文 ID 列表（用于 Shutdown 时清理） */
  LocalVector<IGLContextManager::ContextID> created_contexts_;

  /** @brief 缓存的 GL 渲染器字符串（由 QueryDeviceInfo 填充） */
  char renderer_string_[256] = {};

  /** @brief 缓存的 GL 厂商字符串（由 QueryDeviceInfo 填充） */
  char vendor_string_[256] = {};

  /** @brief 缓存的 GL 版本字符串（由 QueryDeviceInfo 填充） */
  char version_string_[256] = {};
};

}  // namespace arhud

#endif  // ARHUD_DRIVERS_GL_RENDERING_CONTEXT_DRIVER_GL_H_
