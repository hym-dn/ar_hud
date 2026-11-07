/**
 * @file present_thread.h
 * @brief Per-screen present thread (B3: no GL, calls IFramePresenter)
 *
 * @par 两种呈现模式（PresentMode）
 *
 *   kReadback（现状默认）:
 *     Loop: AcquireReady → Present(CPU 像素) → Release
 *     帧数据 = FrameImageQueue 槽内 CPU 像素（渲染线程 glReadPixels 回读）
 *
 *   kSwap（打法 3 + 方案 B，Windows）:
 *     Loop: AcquireReady → SwapPresent(SwapBuffers + DwmFlush) → Release
 *     帧数据 = 窗口 GL backbuffer 本身（零回读零拷贝）；队列槽退化为
 *     渲染/呈现线程的信号握手。DwmFlush 返回 = 帧已被 DWM 原子收编，
 *     Release 后渲染线程可安全复用 backbuffer（防撕裂保证）。
 *
 * Vsync waiting is parallel across screens (each thread independently).
 */

#ifndef ARHUD_SERVERS_RENDERING_PRESENT_THREAD_H_
#define ARHUD_SERVERS_RENDERING_PRESENT_THREAD_H_

#include "frame_image_queue.h"
#include "frame_presenter.h"
#include "io/logger.h"
#include "os/sync.h"
#include "os/thread.h"
#include "typedefs.h"

namespace arhud {

/** @brief 呈现模式（见文件头 @par 两种呈现模式） */
enum class PresentMode : uint8 {
  kReadback = 0,  ///< 回读路径：CPU 像素 → Present()（GDI StretchDIBits）
  kSwap = 1,      ///< Swap 路径（方案 B）：SwapBuffers + DwmFlush，零拷贝
};

class PresentThread {
 public:
  PresentThread() = default;
  ~PresentThread() {
    Finalize();
  }

  ARHUD_DISABLE_COPY_MOVE(PresentThread);

  /**
   * @brief 启动呈现线程
   *
   * @param[in] p_queue     帧队列（信号握手 + 回读模式下的像素槽）
   * @param[in] p_presenter 平台上屏器
   * @param[in] p_mode      呈现模式（默认 kReadback；kSwap 需渲染路径
   *                        配套——渲染线程直画窗口 backbuffer）
   */
  void Initialize(FrameImageQueue* p_queue, IFramePresenter* p_presenter,
                  PresentMode p_mode = PresentMode::kReadback) {
    queue_ = p_queue;
    presenter_ = p_presenter;
    mode_ = p_mode;
    running_.store(true, std::memory_order_release);
    thread_.Start(Entry, this, {Thread::kPriorityNormal});
  }

  void Finalize() {
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }
    running_.store(false, std::memory_order_release);
    if (queue_ != nullptr) {
      queue_->SignalShutdown();
    }
    thread_.WaitToFinish();
    queue_ = nullptr;
    presenter_ = nullptr;
  }

  bool IsRunning() const {
    return running_.load(std::memory_order_acquire);
  }

 private:
  static void Entry(void* p_userdata) {
    auto* self = reinterpret_cast<PresentThread*>(p_userdata);
    self->Run();
  }

  void Run() {
    while (running_.load(std::memory_order_acquire)) {
      // 1. Acquire latest ready frame (blocks until ready or shutdown)
      uint32 slot = queue_->AcquireReady();
      if (slot == UINT32_MAX) {
        if (!running_.load(std::memory_order_acquire)) {
          break;
        }
        continue;
      }

      if (mode_ == PresentMode::kSwap) {
        // 方案 B：SwapBuffers 提交 → DwmFlush 等合成完成。
        // 帧数据即窗口 GL backbuffer（无 CPU 拷贝），
        // slot 仅承载信号语义（渲染完成 → 呈现 → 可复用）。
        // DwmFlush 失败已在 presenter 内降级处理（节拍退化，
        // 帧序仍由本线程 FIFO 保证），此处仅告警不中断。
        Error err = presenter_->SwapPresent();
        if (err != Error::kOK) {
          ARHUD_LOG_WARN("PresentThread: SwapPresent failed (err=%d)",
                         static_cast<int32>(err));
        }
      } else {
        // 回读路径：CPU 像素 → 平台 API
        //    Windows: StretchDIBits (synchronous — DWM composites)
        //    QNX: screen_post_window (blocks on vsync)
        presenter_->Present(queue_->GetPixels(slot), queue_->GetWidth(),
                            queue_->GetHeight(), queue_->GetStride(slot));
        ARHUD_LOG_INFO("B3: PresentThread slot=%u presented", slot);
      }

      // 3. Mark on-screen
      //    kSwap 语义：DwmFlush 已返回 = 帧被 DWM 收编
      //    kReadback 语义：Present 已完成拷贝
      queue_->MarkOnScreen(slot);

      // 4/5. Release slot (free-after-flip → wakes render thread)
      //    kSwap 语义：backbuffer 可复用信号（防撕裂保证的兑现点）
      queue_->Release(slot);
    }
  }

  FrameImageQueue* queue_ = nullptr;
  IFramePresenter* presenter_ = nullptr;
  PresentMode mode_ = PresentMode::kReadback;
  Thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_PRESENT_THREAD_H_
