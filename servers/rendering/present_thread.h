/**
 * @file present_thread.h
 * @brief 每屏上屏线程（B3：不发渲染命令，只经平台接口提交帧）
 *
 * @par 三种呈现模式（PresentMode）
 *   kReadback（默认兜底）:
 *     Loop: AcquireReady → Present(CPU 像素) → Release
 *     帧数据 = FrameImageQueue 槽内 CPU 像素（渲染线程 glReadPixels 回读）
 *
 *   kSwap（打法 3 + 方案 B，Windows）:
 *     Loop: AcquireReady → SwapPresent(SwapBuffers + DwmFlush) → Release
 *     帧数据 = 窗口 GL backbuffer 本身（零回读零拷贝）；队列槽退化为
 *     渲染/呈现线程的信号握手。DwmFlush 返回 = 帧已被 DWM 原子收编，
 *     Release 后渲染线程可安全复用 backbuffer（防撕裂保证）。
 *
 *   kShared（Mode X：零回读，且不切 drawable）:
 *     Loop: AcquireReady → pool->CommitFrame(token) → Release
 *     帧数据 = **显示侧分配**的共享显存（渲染线程经 interop 直写）。
 *     本线程零 GL、零 RCD，只调 DXGI。
 *
 * @par 共享模式下仍是"逐帧数据驱动"
 *   渲染侧在运行期可能降级（帧池失效 / 尺寸重建失败），那时它写的是
 *   CPU 槽且不登记 token。本线程按**帧**判断出路（有 token 走帧池提交，
 *   无 token 走 GDI 上屏），而不是把模式在启动时定死——否则降级后
 *   画面会停在最后一帧。
 *
 * 各屏 vsync 等待由本线程并行承担（每屏一条）。
 */

#ifndef ARHUD_SERVERS_RENDERING_PRESENT_THREAD_H_
#define ARHUD_SERVERS_RENDERING_PRESENT_THREAD_H_

#include <cstdlib>

#include "frame_image_queue.h"
#include "frame_presenter.h"
#include "io/logger.h"
#include "os/sync.h"
#include "os/thread.h"
#include "shared_frame_pool.h"
#include "typedefs.h"

namespace arhud {

/** @brief 呈现模式（见文件头 @par 三种呈现模式） */
enum class PresentMode : uint8 {
  kReadback = 0,  ///< 回读路径：CPU 像素 → Present()（GDI StretchDIBits）
  kSwap = 1,      ///< Swap 路径（方案 B）：SwapBuffers + DwmFlush，零拷贝
  kShared = 2,    ///< 共享显存路径（Mode X）：帧池提交，零回读且不切 drawable
};

/**
 * @brief 每屏上屏线程
 *
 * @par 常驻统计（Finalize 一行汇总）
 *   共享提交 / 回读提交 / 降级次数 / 失败次数——用于验收"零回读"是否
 *   真的成立，以及运行期是否发生了未被察觉的降级。
 */
class PresentThread {
 public:
  /** @brief kShared 等待显示侧消费完成的上限（毫秒；超时仅降级节拍） */
  static constexpr uint32 kCommitTimeoutMs = 1000;

  /** @brief ARHUD_DEBUG_SAMPLE=1 时，每多少帧采样一次画面亮度 */
  static constexpr uint64 kSampleEvery = 300;

  PresentThread() = default;
  ~PresentThread() {
    Finalize();
  }

  ARHUD_DISABLE_COPY_MOVE(PresentThread);

  /**
   * @brief 启动呈现线程
   *
   * @param[in] p_queue     帧队列（信号握手 + 回读模式下的像素槽）
   * @param[in] p_presenter GDI 上屏器（可空；回读与降级路径必需）
   * @param[in] p_mode      呈现模式（默认 kReadback）
   * @param[in] p_pool      共享显存帧池（仅 kShared 使用，非拥有）
   */
  void Initialize(FrameImageQueue* p_queue, IFramePresenter* p_presenter,
                  PresentMode p_mode = PresentMode::kReadback,
                  ISharedFramePool* p_pool = nullptr) {
    queue_ = p_queue;
    presenter_ = p_presenter;
    mode_ = p_mode;
    pool_ = p_pool;
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

    if (shared_frames_ > 0 || readback_frames_ > 0 || swap_frames_ > 0) {
      ARHUD_LOG_INFO(
          "PresentThread finalized (shared=%llu, readback=%llu, swap=%llu, "
          "degraded=%llu, failed=%llu)",
          static_cast<unsigned long long>(shared_frames_),
          static_cast<unsigned long long>(readback_frames_),
          static_cast<unsigned long long>(swap_frames_),
          static_cast<unsigned long long>(degraded_),
          static_cast<unsigned long long>(failed_));
    }

    queue_ = nullptr;
    presenter_ = nullptr;
    pool_ = nullptr;
  }

  bool IsRunning() const {
    return running_.load(std::memory_order_acquire);
  }

  /** @brief 当前呈现模式（诊断/验收用） */
  PresentMode GetMode() const {
    return mode_;
  }

 private:
  static void Entry(void* p_userdata) {
    auto* self = reinterpret_cast<PresentThread*>(p_userdata);
    self->Run();
  }

  void Run() {
    while (running_.load(std::memory_order_acquire)) {
      // 1. 取最新就绪帧（阻塞直到就绪或关停）
      uint32 slot = queue_->AcquireReady();
      if (slot == UINT32_MAX) {
        if (!running_.load(std::memory_order_acquire)) {
          break;
        }
        continue;
      }

      // 2. 逐帧决定出路（模式只是意图，token 才是事实）
      const uint32 token = queue_->GetFrameToken(slot);
      const bool shared_frame = mode_ == PresentMode::kShared &&
                                pool_ != nullptr && pool_->IsAvailable() &&
                                token != FrameImageQueue::kInvalidToken;

      if (shared_frame) {
        SubmitShared(token);
      } else if (mode_ == PresentMode::kSwap) {
        SubmitSwap();
      } else {
        // 共享模式却拿到无 token 的帧 = 渲染侧已降级，本帧是 CPU 像素
        SubmitReadback(slot, /*p_degraded=*/mode_ == PresentMode::kShared);
      }

      // 3. 标记上屏
      //    kSwap 语义：DwmFlush 已返回 = 帧被 DWM 收编
      //    kShared 语义：CommitFrame 已返回 = 已交合成器
      //    kReadback 语义：Present 已完成拷贝
      queue_->MarkOnScreen(slot);

      // 4/5. 归还槽位（free-after-flip → 唤醒渲染线程）
      //    kSwap/kShared：这是"缓冲可复用"信号的兑现点（防撕裂保证）
      queue_->Release(slot);
    }
  }

  /** @brief kShared：把共享显存交给显示侧 */
  void SubmitShared(uint32 p_token) {
    const Error err = pool_->CommitFrame(p_token, kCommitTimeoutMs);
    if (err != Error::kOK) {
      ++failed_;
      ARHUD_LOG_WARN("PresentThread: CommitFrame(%u) failed (err=%d)", p_token,
                     static_cast<int32>(err));
      return;
    }
    ++shared_frames_;
    SampleSharedFrame(p_token);
  }

  /** @brief kSwap：窗口 GL backbuffer 提交（SwapBuffers + DwmFlush） */
  void SubmitSwap() {
    if (presenter_ == nullptr) {
      ++failed_;
      return;
    }
    const Error err = presenter_->SwapPresent();
    if (err != Error::kOK) {
      ++failed_;
      ARHUD_LOG_WARN("PresentThread: SwapPresent failed (err=%d)",
                     static_cast<int32>(err));
      return;
    }
    ++swap_frames_;
  }

  /**
   * @brief kReadback：CPU 像素 → GDI
   *
   * @param[in] p_slot     槽号
   * @param[in] p_degraded 本帧是否来自共享路径的运行期降级（首帧告警）
   */
  void SubmitReadback(uint32 p_slot, bool p_degraded) {
    if (p_degraded && !degrade_warned_) {
      degrade_warned_ = true;
      ++degraded_;
      ARHUD_LOG_WARN(
          "PresentThread: shared present degraded to GDI readback — "
          "later frames take the readback path");
    }
    if (presenter_ == nullptr) {
      ++failed_;
      ARHUD_LOG_WARN("PresentThread: readback frame without presenter "
                     "(slot=%u) — frame dropped",
                     p_slot);
      return;
    }
    presenter_->Present(queue_->GetPixels(p_slot), queue_->GetWidth(),
                        queue_->GetHeight(), queue_->GetStride(p_slot));
    ++readback_frames_;
    SampleCpuSlot(p_slot);
  }

  /** @brief ARHUD_DEBUG_SAMPLE=1 时开启画面亮度采样（验证期用） */
  static bool SamplingEnabled() {
    static const bool enabled = []() {
      const char* value = std::getenv("ARHUD_DEBUG_SAMPLE");
      return value != nullptr && value[0] == '1';
    }();
    return enabled;
  }

  /**
   * @brief 采样共享帧亮度
   *
   * @par 为什么值得留
   *   零拷贝路径的像素从不进 CPU，"画面到底画进去了没有"没有现成出口。
   *   本采样把指定帧临时读回一次（代价与 Mode R 同级），换算成平均亮度
   *   打进日志，与回读路径的同一指标可直接 A/B 对照。
   */
  void SampleSharedFrame(uint32 p_token) {
    if (!SamplingEnabled() || (shared_frames_ % kSampleEvery) != 0) {
      return;
    }
    float mean = 0.0f;
    if (pool_->SampleFrameLuminance(p_token, &mean)) {
      ARHUD_LOG_INFO("PresentThread: shared frame %u luminance %.2f/255",
                     p_token, static_cast<double>(mean));
    }
  }

  /** @brief 采样 CPU 槽亮度（与共享路径同节奏） */
  void SampleCpuSlot(uint32 p_slot) {
    if (!SamplingEnabled() || (readback_frames_ % kSampleEvery) != 0) {
      return;
    }
    const uint8* pixels = static_cast<const uint8*>(queue_->GetPixels(p_slot));
    if (pixels == nullptr) {
      return;
    }
    const uint32 w = queue_->GetWidth();
    const uint32 h = queue_->GetHeight();
    const uint32 stride = queue_->GetStride(p_slot);
    const uint32 step_x = w > 64 ? w / 64 : 1;
    const uint32 step_y = h > 64 ? h / 64 : 1;
    uint64 sum = 0;
    uint64 channels = 0;
    for (uint32 y = 0; y < h; y += step_y) {
      const uint8* row = pixels + static_cast<size_t>(y) * stride;
      for (uint32 x = 0; x < w; x += step_x) {
        const uint8* px = row + static_cast<size_t>(x) * 4;
        sum += px[0] + px[1] + px[2];
        channels += 3;
      }
    }
    if (channels > 0) {
      ARHUD_LOG_INFO("PresentThread: readback slot %u luminance %.2f/255",
                     p_slot,
                     static_cast<double>(sum) / static_cast<double>(channels));
    }
  }

  FrameImageQueue* queue_ = nullptr;
  IFramePresenter* presenter_ = nullptr;
  ISharedFramePool* pool_ = nullptr;  ///< kShared 使用（非拥有，Engine 所有）
  PresentMode mode_ = PresentMode::kReadback;
  Thread thread_;
  std::atomic<bool> running_{false};

  // ── 常驻统计 ──
  uint64 shared_frames_ = 0;   ///< 共享显存提交帧数（零 CPU 搬运）
  uint64 readback_frames_ = 0; ///< 回读提交帧数（GPU→CPU 全帧搬运）
  uint64 swap_frames_ = 0;     ///< Swap 提交帧数（未启用路径，留口径一致）
  uint64 degraded_ = 0;        ///< 发生过的降级次数（>0 需查原因）
  uint64 failed_ = 0;          ///< 提交失败次数
  bool degrade_warned_ = false;  ///< 降级只告警一次
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_PRESENT_THREAD_H_
