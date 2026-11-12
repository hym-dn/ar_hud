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
 * @par 两种载体形态（同一状态机，只加 token，不改结构）
 *   | 形态 | 槽内容 | 边界上传递 | 用于 |
 *   |---|---|---|---|
 *   | 回读 | CPU 像素（实际被读走） | 槽号（等价于传图像）| Mode R |
 *   | 令牌 | CPU 像素（**保留但不碰**）+ frame_token | 槽号 + token | Mode X |
 *
 *   Mode X 下像素从不进 CPU，槽只是握手；frame_token 携带平台帧池的
 *   index（环深度不假设等于 kSlotCount）。CPU 像素背衬在两种形态下
 *   都分配——它是运行期降级回 Mode R 的安全网，见 Initialize 的说明。
 *   Vulkan/QNX 后端若要省掉这份内存，改动点是给 Initialize 加
 *   "按需分配背衬"开关，而不是改状态机。
 *
 * @par 线程模型
 *   原子状态 + 两个信号量（ready_sem_ 唤醒上屏、free_sem_ 背压）；
 *   生产=渲染线程，消费=上屏线程；背压保证每屏最多 1 帧在飞。
 *
 * @par P0 修正（2026-08-24 评审）
 *   - ON_SCREEN 状态分离"已交给合成器"与"正在被扫描"
 *   - free-after-release：buffer 由合成器 release 事件释放（非帧回调）
 *   - latest-wins 降级为防御性断言（HasInFlight 防双 ready）
 *   - 启动预投递 + 命令 push 唤醒源（防死锁）
 *
 * @par 节拍（§3.8 multiscreen）
 *   - 渲染频率 = 显示频率（buffer token 自节拍）
 *   - HasInFlight() 防渲染超前（每屏最多 1 帧在飞）
 *   - render_wake_ 投递于：buffer release + 命令 push + 启动预投递
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
  static constexpr uint32 kInvalidToken = UINT32_MAX;  ///< 未设置的帧 token

  FrameImageQueue() = default;
  ~FrameImageQueue() = default;

  ARHUD_DISABLE_COPY_MOVE(FrameImageQueue);

  /**
   * @brief 初始化槽位（分配 CPU 像素背衬）
   *
   * @par 为何 Mode X 也保留 CPU 背衬
   *   共享路径不碰这些缓冲，但它们是**回落的余地**：设备丢失、
   *   帧池 resize 失败等运行期降级必须能立刻改走回读上屏，而不是
   *   在中途重分配（那会引入与上屏线程的槽位竞态）。
   *   代价 = 每屏 2×w×h×4 常驻内存；换来的是零危险降级路径。
   *
   * @param[in] p_width  帧宽（像素）
   * @param[in] p_height 帧高（像素）
   */
  void Initialize(uint32 p_width, uint32 p_height) {
    for (uint32 i = 0; i < kSlotCount; ++i) {
      slots_[i].width = p_width;
      slots_[i].height = p_height;
      slots_[i].stride = p_width * 4;
      slots_[i].pixels = ARHUD_MALLOC_ZEROED(p_width * p_height * 4);
      slots_[i].token = kInvalidToken;
      slots_[i].state.store(SlotState::kFree, std::memory_order_release);
    }
    ready_index_.store(UINT32_MAX, std::memory_order_release);
  }

  /** @brief 终止（释放 CPU 像素背衬） */
  void Finalize() {
    for (uint32 i = 0; i < kSlotCount; ++i) {
      if (slots_[i].pixels != nullptr) {
        ARHUD_FREE(slots_[i].pixels);
        slots_[i].pixels = nullptr;
      }
      slots_[i].token = kInvalidToken;
      slots_[i].state.store(SlotState::kFree, std::memory_order_release);
    }
  }

  /** @brief 尺寸变更（重建背衬与 token） */
  void Resize(uint32 p_width, uint32 p_height) {
    Finalize();
    Initialize(p_width, p_height);
  }

  // ── 渲染线程（生产者）──

  /**
   * @brief 尝试取得一个空闲槽用于写入（非阻塞）
   * @return 槽号；无空闲槽返回 UINT32_MAX
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
   * @brief 标记槽就绪（渲染完成）
   *
   * P1-2：latest-wins 降级为断言——HasInFlight 已挡住双 READY，
   * 走到这里仍有旧 READY 未消费属不变量违例。
   *
   * @param[in] p_slot  槽号
   * @param[in] p_token Mode X 的平台帧 index（CPU 背衬形态用默认值）
   */
  void MarkReady(uint32 p_slot, uint32 p_token = kInvalidToken) {
    slots_[p_slot].token = p_token;
    // 防御：已有未消费的 READY 槽 = HasInFlight 不变量被破坏
    uint32 prev_ready =
        ready_index_.exchange(p_slot, std::memory_order_acq_rel);
    slots_[p_slot].state.store(SlotState::kReady, std::memory_order_release);
    if (prev_ready < kSlotCount && prev_ready != p_slot) {
      // 覆盖：前一个 READY 槽被取代。HasInFlight 下不应发生，
      // 但仍优雅处理。
      // 注：prev_ready == p_slot 已不可能——AcquireReady 消费时会清
      // ready_index_；该护栏防止陈旧自引用把刚置 READY 的槽降级
      // （曾表现为每次自覆盖、上屏线程饿死）。
      slots_[prev_ready].state.store(SlotState::kFree,
                                     std::memory_order_release);
      free_sem_.Post();  // 唤醒渲染线程（腾出一个槽）
    }
    ready_sem_.Post();  // 唤醒上屏线程
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

  // ── 上屏线程（消费者）──

  /**
   * @brief 取得最新就绪槽（阻塞直到就绪或关停）
   * @return 槽号；关停返回 UINT32_MAX
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
      // Consume the index: without this clear, ready_index_ keeps pointing
      // at this slot after Release and the next MarkReady on the same slot
      // demotes itself (stale self-reference) — the present thread then
      // starves while the render thread spins. Only clear if no newer
      // MarkReady has already replaced it.
      uint32 expect_idx = idx;
      ready_index_.compare_exchange_strong(expect_idx, UINT32_MAX,
                                           std::memory_order_acq_rel);
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

  // ── 访问器 ──

  /** @brief 槽内 CPU 像素基址（令牌形态返回 nullptr） */
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

  /**
   * @brief 读取槽携带的平台帧 index（Mode X）
   *
   * @param[in] p_slot 槽号
   * @return 帧 index；未设置返回 kInvalidToken
   */
  uint32 GetFrameToken(uint32 p_slot) const {
    return slots_[p_slot].token;
  }

 private:
  struct Slot {
    uint32 width = 0;
    uint32 height = 0;
    uint32 stride = 0;
    void* pixels = nullptr;    ///< CPU 背衬像素（令牌形态 = nullptr）
    uint32 token = kInvalidToken;  ///< Mode X 的平台帧 index
    std::atomic<SlotState> state{SlotState::kFree};
  };

  Slot slots_[kSlotCount];
  bool cpu_backing_ = true;  ///< 载体形态（Initialize 决定，Resize 沿用）
  Semaphore ready_sem_;      ///< 上屏线程等待：新帧就绪
  Semaphore free_sem_;       ///< 空闲槽计数（背压）
  Semaphore* wake_sem_ =
      nullptr;  ///< B3: Engine render_wake_（线程唤醒通道，可空）
  std::atomic<uint32> ready_index_{UINT32_MAX};
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_FRAME_IMAGE_QUEUE_H_
