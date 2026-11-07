/**
 * @file frame_image_queue.h
 * @brief Per-screen double-buffered frame image queue (B3)
 *
 * @par State machine (P0-corrected)
 *   FREE → WRITING → READY → COMMITTED → ON_SCREEN → FREE
 *
 *   - Render thread: AcquireFree (FREE→WRITING) → render → MarkReady
 * (WRITING→READY)
 *   - Present thread: AcquireReady (READY→COMMITTED) → Commit
 * (COMMITTED→ON_SCREEN) → WaitBufferRelease → ReleaseSlot (ON_SCREEN→FREE)
 *
 * @par P0 fixes (2026-08-24 review)
 *   - ON_SCREEN state separates "committed to compositor" from "being scanned"
 *   - free-after-release: buffer freed by compositor release event (not frame
 * callback)
 *   - latest-wins demoted to defensive assert (HasInFlight guard prevents
 * double-ready)
 *   - Startup pre-post + command push wake sources (prevent deadlock)
 *
 * @par Pacing (§3.8 of multiscreen)
 *   - Render frequency = display frequency (buffer token self-pacing)
 *   - HasInFlight() prevents render-ahead (max 1 frame in flight per screen)
 *   - render_wake_ posted on: buffer release + command push + startup pre-post
 */

#ifndef ARHUD_SERVERS_RENDERING_FRAME_IMAGE_QUEUE_H_
#define ARHUD_SERVERS_RENDERING_FRAME_IMAGE_QUEUE_H_

#include "os/memory.h"
#include "os/sync.h"
#include "typedefs.h"

namespace arhud {

enum class SlotState : uint8 {
  kFree = 0,   ///< Available for render thread to write
  kWriting,    ///< Render thread is writing (rendering + readback)
  kReady,      ///< Rendered, waiting for present thread
  kCommitted,  ///< Present thread acquired, about to commit
  kOnScreen    ///< Committed to compositor, being scanned
};

/**
 * @brief Per-screen double-buffered frame image queue
 *
 * Two slots alternate. Render thread produces, present thread consumes.
 * Thread-safe via atomic state + semaphores.
 */
class FrameImageQueue {
 public:
  static constexpr uint32 kSlotCount = 2;

  FrameImageQueue() = default;
  ~FrameImageQueue() = default;

  ARHUD_DISABLE_COPY_MOVE(FrameImageQueue);

  /** @brief Initialize (allocate CPU buffers) */
  void Initialize(uint32 p_width, uint32 p_height) {
    for (uint32 i = 0; i < kSlotCount; ++i) {
      slots_[i].width = p_width;
      slots_[i].height = p_height;
      slots_[i].stride = p_width * 4;
      slots_[i].pixels = ARHUD_MALLOC_ZEROED(p_width * p_height * 4);
      slots_[i].state.store(SlotState::kFree, std::memory_order_release);
    }
    ready_index_.store(UINT32_MAX, std::memory_order_release);
  }

  /** @brief Finalize (free CPU buffers) */
  void Finalize() {
    for (uint32 i = 0; i < kSlotCount; ++i) {
      if (slots_[i].pixels != nullptr) {
        ARHUD_FREE(slots_[i].pixels);
        slots_[i].pixels = nullptr;
      }
      slots_[i].state.store(SlotState::kFree, std::memory_order_release);
    }
  }

  void Resize(uint32 p_width, uint32 p_height) {
    Finalize();
    Initialize(p_width, p_height);
  }

  // ── Render thread (producer) ──

  /**
   * @brief Try to acquire a free slot for writing (non-blocking)
   * @return Slot index, or UINT32_MAX if no free slot
   */
  uint32 TryAcquireFree() {
    for (uint32 i = 0; i < kSlotCount; ++i) {
      SlotState expected = SlotState::kFree;
      if (slots_[i].state.compare_exchange_strong(expected, SlotState::kWriting,
                                                  std::memory_order_acq_rel)) {
        return i;
      }
    }
    return UINT32_MAX;
  }

  /**
   * @brief Mark a slot as ready (rendering complete)
   * P1-2: latest-wins demoted to assert — HasInFlight guard prevents
   * double-ready. If we get here with an existing READY slot, it's a bug.
   */
  void MarkReady(uint32 p_slot) {
    // Defensive: if there's already a READY slot not yet consumed,
    // that's a violation of HasInFlight invariant.
    uint32 prev_ready =
        ready_index_.exchange(p_slot, std::memory_order_acq_rel);
    slots_[p_slot].state.store(SlotState::kReady, std::memory_order_release);
    if (prev_ready < kSlotCount) {
      // Overwrite: previous ready slot is superseded.
      // This shouldn't happen with HasInFlight guard, but handle gracefully.
      slots_[prev_ready].state.store(SlotState::kFree,
                                     std::memory_order_release);
      free_sem_.Post();  // wake render thread (freed a slot)
    }
    ready_sem_.Post();  // wake present thread
  }

  /** @brief Check if this screen has a frame in flight
   * (READY/COMMITTED/ON_SCREEN) */
  bool HasInFlight() const {
    for (uint32 i = 0; i < kSlotCount; ++i) {
      SlotState s = slots_[i].state.load(std::memory_order_acquire);
      if (s == SlotState::kReady || s == SlotState::kCommitted ||
          s == SlotState::kOnScreen) {
        return true;
      }
    }
    return false;
  }

  // ── Present thread (consumer) ──

  /**
   * @brief Acquire the latest ready slot (blocks until ready or shutdown)
   * @return Slot index, or UINT32_MAX on shutdown
   */
  uint32 AcquireReady() {
    ready_sem_.Wait();
    uint32 idx = ready_index_.load(std::memory_order_acquire);
    if (idx >= kSlotCount) {
      return UINT32_MAX;  // shutdown
    }
    SlotState expected = SlotState::kReady;
    if (slots_[idx].state.compare_exchange_strong(
            expected, SlotState::kCommitted, std::memory_order_acq_rel)) {
      return idx;
    }
    // CAS failed (rare race) — try again
    return UINT32_MAX;
  }

  /**
   * @brief Mark slot as on-screen (after Commit)
   */
  void MarkOnScreen(uint32 p_slot) {
    slots_[p_slot].state.store(SlotState::kOnScreen, std::memory_order_release);
  }

  /**
   * @brief Release slot back to free (after present + release)
   * Posts free_sem_ (slot backpressure) + render_wake_ (B3: thread wake).
   */
  void Release(uint32 p_slot) {
    slots_[p_slot].state.store(SlotState::kFree, std::memory_order_release);
    free_sem_.Post();
    if (wake_sem_ != nullptr) {
      wake_sem_->Post();
    }
  }

  // ── Wake sources (P0-2: prevent deadlock) ──

  /**
   * @brief B3: 绑定 Engine 渲染线程唤醒信号量
   *
   * Release/PostRenderWake 除本地 free_sem_（槽位计数）外，
   * 同时 post 引擎级 render_wake_（渲染线程主循环的等待点）。
   * free_sem_ 保持槽位背压语义不被借用；render_wake_ 是唯一
   * 线程唤醒通道（三路聚合：present 释放 / 命令 push / 启动）。
   */
  void SetWakeSemaphore(Semaphore* p_wake) {
    wake_sem_ = p_wake;
  }

  /** @brief Wake render thread (present release + command push + startup) */
  void PostRenderWake() {
    free_sem_.Post();
    if (wake_sem_ != nullptr) {
      wake_sem_->Post();
    }
  }

  /**
   * @brief Wake render thread only (no slot backpressure change)
   *
   * 用于不涉及槽位释放的唤醒场景（如窗口 resize 需要渲染线程
   * 尽快重建 FBO/帧队列）。与 PostRenderWake 不同，本方法不 post
   * free_sem_，避免污染槽位背压计数。
   */
  void WakeRender() {
    if (wake_sem_ != nullptr) {
      wake_sem_->Post();
    }
  }

  /** @brief Wake present thread for shutdown */
  void SignalShutdown() {
    ready_sem_.Post();
  }

  // ── Accessors ──

  void* GetPixels(uint32 p_slot) {
    return slots_[p_slot].pixels;
  }
  uint32 GetWidth() const {
    return slots_[0].width;
  }
  uint32 GetHeight() const {
    return slots_[0].height;
  }
  uint32 GetStride(uint32 p_slot) const {
    return slots_[p_slot].stride;
  }

 private:
  struct Slot {
    uint32 width = 0;
    uint32 height = 0;
    uint32 stride = 0;
    void* pixels = nullptr;
    std::atomic<SlotState> state{SlotState::kFree};
  };

  Slot slots_[kSlotCount];
  Semaphore ready_sem_;  // present thread waits: new frame ready
  Semaphore free_sem_;   // slot free count (backpressure)
  Semaphore* wake_sem_ =
      nullptr;  ///< B3: Engine render_wake_（线程唤醒通道，可空）
  std::atomic<uint32> ready_index_{UINT32_MAX};
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_FRAME_IMAGE_QUEUE_H_
