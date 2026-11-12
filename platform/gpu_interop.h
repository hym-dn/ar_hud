/**
 * @file gpu_interop.h
 * @brief 跨 API GPU 资源共享契约（IGPUInterop）——把显示侧显存别名成
 *        图形 API 可写对象。
 *
 * @par 为什么存在（design skill §5.5/§7.7 Mode X）
 *   零回读上屏要求"帧缓冲归显示侧、渲染侧写入同一块显存"。两端各自
 *   属于不同的图形 API（Win32: D3D11 分配 + OpenGL 绘制），必须有一层
 *   **显式的所有权交接**机制。本接口就是那层的最小表达：
 *   所有句柄对本工程不透明（void*），只有同平台的实现知道怎么解释。
 *
 * @par 分层约束
 *   - 契约在本文件（platform 根），实现只在 `platform/windows/`；
 *   - drivers/gl **不触碰 wgl 家族函数**（gl_context_manager.h 既有
 *     硬规则），只经 `IGLContextManager::GetGPUInterop()` 取本接口；
 *   - 后端语义映射：GL=WGL_NV_DX_interop2，Vulkan=
 *     VK_EXT_external_memory_*，EGL=EGLImage/OpenGL ES EGL extension。
 *
 * @par 线程与上下文要求
 *   `OpenDevice` / `RegisterResource` / `Lock` / `Unlock` 都要求调用线程
 *   持有当前图形上下文（Win32 的 wglDX* 系列即有此约束），因此全部
 *   发生在**渲染线程**。`CloseDevice` 同。
 *
 * @par 所有权交接语义（唯一的 GPU 侧同步点）
 *   @code
 *   LockResource   // 显示侧 → 渲染侧：此后图形 API 的写入才可见/可用
 *   ...绘制...
 *   UnlockResource // 渲染侧 → 显示侧：实现保证写完成后对端可读
 *   @endcode
 *   未 Lock 就绘制属未定义行为（扩展规范明示）。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-11-08
 * @copyright Copyright (c) 2026 ARHud Project
 * @ingroup platform
 */

#ifndef ARHUD_PLATFORM_GPU_INTEROP_H_
#define ARHUD_PLATFORM_GPU_INTEROP_H_

#include "typedefs.h"

namespace arhud {

/**
 * @brief 跨 API GPU 资源共享接口（不透明句柄 + 显式加解锁）
 *
 * 实现者不得持有资源所有权（资源由 ISharedFramePool 等分配方拥有）；
 * 注册关系只是"别名"，销毁别名不得释放底层资源。
 *
 * @par 命名说明
 *   方法名刻意避开 `LockResource`/`UnlockResource`——那两个名字在
 *   winuser.h 里是遗留宏，会把成员函数定义炸成语法错误。
 */
class IGPUInterop {
 public:
  virtual ~IGPUInterop() = default;

  /**
   * @brief 与当前图形上下文建立设备关联
   *
   * @param[in] p_native_device 拥有资源的平台设备（Win32: ID3D11Device*）
   *
   * @return 不透明设备句柄
   * @retval nullptr 平台不支持 / 设备打不开（扩展缺失、跨适配器等）
   *
   * @pre 调用线程已 MakeCurrent 一个图形上下文
   */
  virtual void* OpenDevice(void* p_native_device) = 0;

  /**
   * @brief 关闭设备关联（其下所有别名随之失效）
   *
   * @param[in] p_device_handle OpenDevice 的返回值（null 静默忽略）
   */
  virtual void CloseDevice(void* p_device_handle) = 0;

  /**
   * @brief 把平台资源别名到既有图形对象名上
   *
   * @param[in] p_device_handle    OpenDevice 的返回值
   * @param[in] p_native_resource  平台资源（Win32: ID3D11Texture2D*）
   * @param[in] p_gl_name          调用方先生成的对象名（GL texture/renderbuffer）
   * @param[in] p_gl_target        对象类型（GL target，如 GL_TEXTURE_2D）
   * @param[in] p_writable         图形侧是否写入（false = 只读）
   *
   * @return 不透明别名句柄
   * @retval nullptr 建立失败（资源类型/用法不受支持）
   */
  virtual void* CreateAlias(void* p_device_handle, void* p_native_resource,
                            uint32 p_gl_name, uint32 p_gl_target,
                            bool p_writable) = 0;

  /**
   * @brief 解除别名（不得释放底层资源）
   *
   * @param[in] p_device_handle  OpenDevice 的返回值
   * @param[in] p_alias_handle   CreateAlias 的返回值（null 静默忽略）
   *
   * @pre 别名未被占用（锁定中解除别名属未定义行为）
   */
  virtual void DestroyAlias(void* p_device_handle, void* p_alias_handle) = 0;

  /**
   * @brief 取得绘制权（显示侧 → 渲染侧）
   *
   * 实现须保证返回时图形 API 可以安全写入：若对端仍有在途读取，
   * 本调用要么阻塞到其完成，要么由驱动在后续命令中插入等待。
   *
   * @param[in] p_device_handle OpenDevice 的返回值
   * @param[in] p_alias_handle  CreateAlias 的返回值
   *
   * @return true 已取得绘制权
   * @retval false 取得失败（对端仍占用或句柄失效）
   */
  virtual bool AcquireForWriting(void* p_device_handle,
                                 void* p_alias_handle) = 0;

  /**
   * @brief 交还绘制权（渲染侧 → 显示侧；实现须保证写完成的可见性）
   *
   * @param[in] p_device_handle OpenDevice 的返回值
   * @param[in] p_alias_handle  CreateAlias 的返回值
   *
   * @return true 已交还
   */
  virtual bool ReleaseFromWriting(void* p_device_handle,
                                  void* p_alias_handle) = 0;

  /** @brief 后端名（诊断日志，ASCII 字面量） */
  virtual const char* GetName() const = 0;
};

}  // namespace arhud

#endif  // ARHUD_PLATFORM_GPU_INTEROP_H_
