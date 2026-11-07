#ifndef ARHUD_PLATFORM_FRAME_PRESENTER_H_
#define ARHUD_PLATFORM_FRAME_PRESENTER_H_

#include "typedefs.h"

namespace arhud {
/**
 * @brief 平台上屏接口（B3-M0.3：纯平台 API，无 GL）
 *
 * 实现不使用 GL——纯平台 API（Windows GDI / QNX Screen / Linux DRM）。
 * Render 线程侧用 RD::FramebufferReadback 回读 FBO 像素到 CPU 缓冲，
 * 然后调 Present 提交到屏幕。
 *
 * @par 两条呈现路径
 *
 *   | 路径 | 数据流 | 激活条件 |
 *   |---|---|---|
 *   | 回读（kReadback） | FBO→glReadPixels→CPU 像素→Present() | 现状默认 |
 *   | Swap（kSwap，打法 3+方案 B） | 渲染线程直接画窗口 GL
 * backbuffer→SwapPresent() | 窗口带双缓冲 GL 像素格式 + 渲染路径切换 |
 *
 *   Swap 路径下帧数据就是窗口 backbuffer 本身（零回读零拷贝），
 *   Present() 不被调用；PresentThread 的队列槽退化为
 *   渲染/呈现两线程的信号握手（FREE→READY→FREE）。
 */
class IFramePresenter {
 public:
  virtual ~IFramePresenter() = default;

  virtual Error Initialize(void* p_native_window, uint32 p_width,
                           uint32 p_height) = 0;
  virtual void Finalize() = 0;
  virtual void Resize(uint32 p_width, uint32 p_height) = 0;

  /** @brief 把 CPU 像素缓冲提交到屏幕（Windows: StretchDIBits → DWM） */
  virtual void Present(const void* p_pixels, uint32 p_width, uint32 p_height,
                       uint32 p_stride) = 0;

  /**
   * @brief 是否支持 Swap 呈现
   *
   * Win32 实现：窗口像素格式含 PFD_DOUBLEBUFFER | PFD_SUPPORT_OPENGL
   * 时为 true（渲染路径切换后由引擎查询决定呈现模式）。
   *
   * @return true 窗口具备双缓冲 GL 像素格式，可用 SwapPresent()
   */
  virtual bool SupportsSwapPresent() const {
    return false;
  }

  /**
   * @brief Swap 呈现：提交已渲染的窗口 GL backbuffer（方案 B）
   *
   * @par 方案 B 语义（防撕裂的 API 保证）
   *   1. SwapBuffers(HDC)：提交 backbuffer 给 DWM（非阻塞，
   *      纯 GDI 调用，不需要 current 的 GL context——这正是
   *      present 线程能独立于渲染线程调它的原因）
   *   2. DwmFlush()：阻塞到 DWM 完成一次合成。返回时本帧已被
   *      DWM 原子收编进合成表面——此后渲染线程复用这块
   *      backbuffer 画下一帧不会污染在途帧（撕裂的根因被消灭）
   *
   *   对应生产平台语义：DwmFlush = wl_buffer.release / QNX Screen
   *   release 事件的 Windows 翻译（"合成器说：帧我收好了"）。
   *
   * @return Error::kOK      成功（DwmFlush 失败仅降级节拍，仍返回 kOK）
   * @return Error::kFailed  SwapBuffers 失败（无双缓冲像素格式/DC 无效）
   * @return Error::kUnavailable 平台不支持 Swap 路径（默认实现）
   *
   * @pre 渲染线程已把帧画进本窗口的 GL backbuffer
   * @pre 仅 present 线程调用（每屏串行 FIFO，保证帧序）
   */
  virtual Error SwapPresent() {
    return Error::kUnavailable;
  }
};
}  // namespace arhud

#endif  // ARHUD_PLATFORM_FRAME_PRESENTER_H_
