#ifndef ARHUD_PLATFORM_WINDOWS_DXGI_SHARED_FRAME_POOL_WIN32_H_
#define ARHUD_PLATFORM_WINDOWS_DXGI_SHARED_FRAME_POOL_WIN32_H_

/**
 * @file dxgi_shared_frame_pool_win32.h
 * @brief Win32 共享显存帧池——D3D11 帧缓冲环 + DXGI flip-model 呈现。
 *
 * @par 零回读上屏（Mode X，design skill §5.5）
 *   帧缓冲由本池（显示侧）分配为 ID3D11Texture2D，渲染侧经
 *   WGL_NV_DX_interop2 把它当成 FBO 颜色附件写入；上屏线程只做
 *   `CopyResource` → `Present1`。像素全程留在显存，跨线程只传帧 index。
 *
 * @par "帧已显示完"由谁保证（本实现的关键取舍）
 *   本池**不**等待拷贝在 GPU 上完成。归还保证来自 interop 本身：
 *   扩展规范规定 Lock/Unlock 就是跨驱动的同步点——"any rendering
 *   operations that affect the resource on one driver are complete
 *   before the other driver takes ownership of it"。因此渲染线程下一次
 *   `AcquireForWriting`（Lock）会自动等到该帧的显示侧读取结束。
 *   这正是生产平台 release event 的等价物，只是兑现点落在渲染线程；
 *   环深度取 3 让那次等待在实践中几乎不发生。
 *
 * @par COM 生命周期
 *   全部 COM 引用为裸指针，释放集中在 DestroyFrames /
 *   DestroySwapChain / ReleaseSharedDevice 三处（错误路径亦只经
 *   Finalize 出口），不使用智能指针包装——与本库既有所有权风格一致。
 *
 * @par 设备与线程
 *   D3D11 device/factory 进程内共享（引用计数），按扩展要求**不得**
 *   使用 D3D11_CREATE_DEVICE_SINGLETHREADED（默认即 multithreaded，
 *   运行时对 immediate context 调用加锁）。因此多屏各上屏线程共用
 *   同一 context 是安全的，代价是运行时锁。
 *
 * @author yameng.he
 * @version 1.1
 * @date 2026-11-08
 * @copyright Copyright (c) 2026 ARHud Project
 * @ingroup platform
 */

#include "shared_frame_pool.h"

#ifdef ARHUD_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIFactory2;
struct IDXGISwapChain1;

namespace arhud {

/**
 * @brief ISharedFramePool 的 Win32/DXGI 实现（每屏一个）
 *
 * 不可用（device 创建失败 / 窗口不支持 flip-model / 尺寸非法）时
 * `IsAvailable()` 返回 false，调用方须回落 Mode R；本类不自行降级，
 * 以免呈现路径在运行期静默改变。
 */
class DxgiSharedFramePoolWin32 : public ISharedFramePool {
 public:
  /**
   * @brief 帧缓冲环深度
   *
   * 3 = 与 DXGI flip-model 的默认队列深度同形：1 帧在写、1 帧在合成、
   * 1 帧余量，使渲染线程的 interop Lock 不必真的等 GPU。
   */
  static constexpr uint32 kRingFrames = 3;

  /** @brief swap chain back buffer 数量（与 kRingFrames 同深度） */
  static constexpr uint32 kSwapChainBuffers = 3;

  DxgiSharedFramePoolWin32() = default;
  ~DxgiSharedFramePoolWin32() override {
    Finalize();
  }

  ARHUD_DISABLE_COPY_MOVE(DxgiSharedFramePoolWin32);

  Error Initialize(void* p_native_window, uint32 p_width,
                   uint32 p_height) override;
  void Finalize() override;
  void Resize(uint32 p_width, uint32 p_height) override;

  bool IsAvailable() const override {
    return available_;
  }

  uint32 GetFrameCount() const override {
    return frame_count_;
  }

  Frame GetFrame(uint32 p_index) const override;

  Error CommitFrame(uint32 p_index, uint32 p_timeout_ms) override;

  /**
   * @brief 诊断采样：把指定帧拷到 staging 纹理后读回求平均亮度
   *
   * 仅 `ARHUD_DEBUG_SAMPLE=1` 时被上屏线程调用；正常呈现不经过这里。
   * 代价是一次 GPU→CPU 全帧读回（等于临时把该帧变回 Mode R 的开销），
   * 所以只在验证期开。
   */
  bool SampleFrameLuminance(uint32 p_index, float* o_mean) override;

  const char* GetBackendName() const override {
    return "dxgi_shared";
  }

 private:
  /**
   * @brief 取得进程内共享的 D3D11 device 与 DXGI factory（引用计数）
   *
   * @param[out] o_device  共享 device（本池不持有所有权）
   * @param[out] o_context 共享 immediate context（仅上屏线程使用）
   * @param[out] o_factory DXGI factory2
   *
   * @retval Error::kOK          成功
   * @retval Error::kUnavailable 创建失败（无 D3D11 硬件/驱动）
   */
  Error AcquireSharedDevice(ID3D11Device** o_device,
                            ID3D11DeviceContext** o_context,
                            IDXGIFactory2** o_factory);

  /** @brief 归还进程内共享 device 引用 */
  void ReleaseSharedDevice();

  /** @brief 创建帧缓冲环（Usage DEFAULT + RenderTarget|ShaderResource） */
  Error CreateFrames(uint32 p_width, uint32 p_height);

  /** @brief 释放帧缓冲环 */
  void DestroyFrames();

  /** @brief 创建 flip-model swap chain 并缓存 back buffer */
  Error CreateSwapChain(HWND p_hwnd, uint32 p_width, uint32 p_height);

  /** @brief 释放 back buffer 引用与 swap chain */
  void DestroySwapChain();

  HWND hwnd_ = nullptr;
  uint32 width_ = 0;
  uint32 height_ = 0;
  uint32 frame_count_ = 0;
  uint32 back_index_ = 0;  ///< flip-model back buffer 轮转位（仅上屏线程读写）
  bool available_ = false;
  bool holds_device_ = false;

  ID3D11Device* device_ = nullptr;          ///< 共享（不拥有，计数归还）
  ID3D11DeviceContext* context_ = nullptr;  ///< 共享（不拥有）
  IDXGIFactory2* factory_ = nullptr;        ///< 共享（不拥有）

  ID3D11Texture2D* frames_[kRingFrames] = {};        ///< 帧缓冲环（本池拥有）
  ID3D11Texture2D* back_buffers_[kSwapChainBuffers]
      = {};  ///< swap chain 缓冲引用（本池拥有，ResizeBuffers 前必须归还）
  IDXGISwapChain1* swap_chain_ = nullptr;  ///< 呈现通道（本池拥有）
  ID3D11Texture2D* sample_staging_ =
      nullptr;  ///< 诊断采样用 staging（仅 ARHUD_DEBUG_SAMPLE 时存在）
};

}  // namespace arhud
#endif  // ARHUD_PLATFORM_WINDOWS

#endif  // ARHUD_PLATFORM_WINDOWS_DXGI_SHARED_FRAME_POOL_WIN32_H_
