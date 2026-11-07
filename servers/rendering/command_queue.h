/**
 * @file command_queue.h
 * @brief 命令队列 — 主线程到渲染线程的命令代理（SPSC 序列化环形缓冲区）
 *
 * B2 重构：从 Entry 槽位数组改为序列化字节环形缓冲区。
 *
 * @par 重构演进
 *   | 版本 | 存储 | 同步 | 说明 |
 *   |---|---|---|---|
 *   | v1 | LocalVector\<Entry\> + SpinLock | 栈 SyncFlag | 存在 P0 悬垂 |
 *   | v2 (B1) | 定长 Entry 数组 + 原子读写指针 | 每线程 Semaphore | 修复 P0；仅
 * PushAndSync | | v3 (B2) | 1MB 字节环形缓冲区 + 序列化命令 | 每线程 Semaphore
 * | 命令按值拷贝，异步 Push 安全 | | v3.1 (ADR-0010) | 同上（16B 头含自派发
 * thunk） | 无（纯异步） | 删除 PushAndSync 及其同步机器，仅存
 * PushBound/PushBoundBlob | | v3.2 (Godot 对齐) | 同上 | 整批屏障 Sync() | 补
 * Godot `sync()` 等价的"排空到最新"屏障 + 消费者直调前 FlushIfPending |
 *
 *   表中"同步"列描述**各自版本**。v3.1
 * 起队列内不再有**逐命令**同步代理（无返回值回传、 无超时机器）；v3.2 的 Sync()
 * 是**整批屏障**——只等"调用瞬间已发布的全部命令执行完"。 跨线程结果回传仍由
 * RenderServer 的 RID 分裂承担（见 render_server.h）。
 *
 * @par v3 核心变化
 *   - 命令 = 16 字节定长头（含 ADR-0009 自派发 thunk）+ 定长负载 +
 * 内联变长数据， 整体按值序列化进缓冲区
 *   - 异步 Push 安全：负载是拷贝而非外部指针，调用方返回后数据仍有效
 *   - 命令字节数上限 64KB（受 CommandHeader::size 的 uint16
 * 限制）；更大体积的数据 不入队——item 状态留在 ItemStorage，像素上传走 RD
 * staging 缓冲
 *   - ADR-0010 后 RD 遗留函数指针代理路径（kFunctionCall / PushAndSync）及其
 *     同步机器已彻底移除；队列仅保留绑定命令（PushBound / PushBoundBlob）
 *
 * @par 线程模型
 *   @code
 *   主线程（生产者）                    渲染线程（消费者）
 *     │                                   │
 *     ├─ PushBound / PushBoundBlob        │
 *     │  ├─ memcpy 序列化到 buffer_       │
 *     │  ├─ write_pos_.store(release)     │
 *     │  └─ notify_semaphore_->Post() ──►├─ Flush()
 *     │                                   │  ├─ read write_pos_ (acquire)
 *     │                                   │  ├─ 逐条执行（自派发 thunk）
 *     │                                   │  ├─ read_pos_.store(release)
 *     │                                   │  └─ sync_waiting_ ?
 * sync_semaphore_.Post() ├─ Sync() ─ 整批屏障（可选）        │ │  ├─ target =
 * write_pos_           │ │  ├─ Post notify + 等 sync_semaphore_ │  └─ 复查
 * read_pos_ 追平 target ◄──┘ └─（渲染线程回调本类时）FlushIfPending() —
 * 仅消费者线程可排空
 *   @endcode
 *
 * @par Flush 的执行者约束
 *   Flush 只在"事实上的执行者"线程上调用：已注册消费者时必须是该消费者，
 *   未注册消费者时由调用方自行排空（单线程/测试形态）。两个线程并发 Flush
 *   会各自持有未发布的局部 read 游标 → 重复执行命令，故 Flush()
 * 入口断言该约束。 批处理内部的再入请求（thunk → FlushIfPending/Sync）由
 * thread_local 标志挡住。
 *
 * @par SPSC 无锁正确性
 *   生产者写命令字节后 release 序写 write_pos_；消费者 acquire 读
 *   write_pos_ 后看到命令全部字节。消费者 release 写 read_pos_；生产者
 *   acquire 读 read_pos_ 确认空间。读写指针为单调递增 uint32 字节计数，
 *   自然回绕（差值 = 在途字节数，恒小于 kCapacity）。
 *
 * @par 回绕处理
 *   命令必须连续存放。若写入会跨越缓冲区尾部，先写一条 kPadding 填充
 *   命令到尾部（大小 = 剩余字节数），再从缓冲区头部写入真实命令。
 *   所有命令（含填充）大小 16 字节对齐（= sizeof(CommandHeader)），保证任何命令
 *   起始偏移均为 16 的倍数 → 填充区恒为 0 或 ≥ 16 字节，必能容纳填充头。
 *
 * @author yameng.he
 * @version 3.2
 * @date 2026-08-22
 * @date 2026-10-13 移除 PushAndSync 同步机器（队列收敛为纯异步）；对齐 16B
 * 命令头，清理失效引用
 * @date 2026-10-13 对齐 Godot：新增整批屏障 Sync()、消费者直调前
 * FlushIfPending() 与 Flush 再入保护
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_COMMAND_QUEUE_H_
#define ARHUD_SERVERS_RENDERING_COMMAND_QUEUE_H_

#include <atomic>
#include <cstring>
#include <type_traits>
#include <utility>

#include "io/logger.h"
#include "os/sync.h"
#include "os/thread.h"
#include "typedefs.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
//  队列内部条目类型 + 自派发 thunk（ADR-0009/0010）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 队列内部条目类型（ADR-0010 后不再有业务命令枚举）
 *
 * 业务命令一律为绑定命令（kCmdTypeNone）。仅保留两种内部条目：
 *   kCmdTypeNone    ：绑定命令（Flush 直接跑 thunk）；
 *   kCmdTypePadding ：环形缓冲区回绕填充，Flush 跳过。
 */
enum : uint8 {
  kCmdTypeNone = 0,
  kCmdTypePadding = 1,
};

/**
 * @brief 变长数据段描述（原 render_commands.h 定义，随命令结构体一并迁入本头）
 *
 * PushBoundBlob 用它描述内联 blob 的各段（如 `[像素][名称]`）。
 */
struct CommandDataSpan {
  const void* data = nullptr;  ///< 段数据指针
  uint32 size = 0;             ///< 段字节数
};

/**
 * @brief 命令自派发 thunk（ADR-0009/0010）
 *
 * 把环形缓冲区里类型擦除的负载还原为 (目标, 方法, 参数) 并调用。
 * 由 PushBound/PushBoundBlob 在编译期绑定，随命令头写入缓冲区；Flush 直接调用。
 * ADR-0010 起 thunk 不再需要 `CommandExecutor&` 与命令类型枚举。
 */
using CommandExecuteThunk = void (*)(const uint8* p_payload,
                                     uint32 p_payload_size);

/**
 * @brief 绑定命令的内部实现细节（不应被外部直接引用）
 *
 * 存放 ADR-0010 绑定命令 (目标, 方法, 参数) 的定长负载结构体与对应的自派发
 * thunk。
 *
 * @par 实参存储约定
 *   实参以**显式 POD 成员**存放（不用 std::tuple：MSVC 下 std::tuple 不保证
 *   trivially_copyable，而本队列以 memcpy 搬运）。故按实参个数给出 0~3 元形态。
 *   对齐 Godot `Command<T,M,Args…>`。
 */
namespace command_detail {
// ── ADR-0010: 绑定命令 (目标, 方法, 参数) —— 对齐 Godot Command<T,M,Args…> ──
//
// 实参以**显式 POD 成员**存放（不用 std::tuple：MSVC 下 std::tuple 不保证
// trivially_copyable，而本队列以 memcpy 搬运）。故按实参个数给出 0~3 元形态。

/** @brief 0 元实参绑定命令的定长负载：仅 (目标, 方法) */
template <typename S, typename M>
struct BoundCall0 {
  S* instance;  ///< 目标对象指针
  M method;     ///< 成员函数指针
};

/** @brief 1 元实参绑定命令的定长负载： (目标, 方法, a0) */
template <typename S, typename M, typename A0>
struct BoundCall1 {
  S* instance;  ///< 目标对象指针
  M method;     ///< 成员函数指针
  A0 a0;        ///< 实参 0
};

/** @brief 2 元实参绑定命令的定长负载： (目标, 方法, a0, a1) */
template <typename S, typename M, typename A0, typename A1>
struct BoundCall2 {
  S* instance;  ///< 目标对象指针
  M method;     ///< 成员函数指针
  A0 a0;        ///< 实参 0
  A1 a1;        ///< 实参 1
};

/** @brief 3 元实参绑定命令的定长负载： (目标, 方法, a0, a1, a2) */
template <typename S, typename M, typename A0, typename A1, typename A2>
struct BoundCall3 {
  S* instance;  ///< 目标对象指针
  M method;     ///< 成员函数指针
  A0 a0;        ///< 实参 0
  A1 a1;        ///< 实参 1
  A2 a2;        ///< 实参 2
};

/** @brief BoundCall0 的自派发 thunk */
template <typename S, typename M>
void BoundThunk0(const uint8* p_payload, uint32) {
  const auto* c = reinterpret_cast<const BoundCall0<S, M>*>(p_payload);
  (c->instance->*(c->method))();
}

/** @brief BoundCall1 的自派发 thunk */
template <typename S, typename M, typename A0>
void BoundThunk1(const uint8* p_payload, uint32) {
  const auto* c = reinterpret_cast<const BoundCall1<S, M, A0>*>(p_payload);
  (c->instance->*(c->method))(c->a0);
}

/** @brief BoundCall2 的自派发 thunk */
template <typename S, typename M, typename A0, typename A1>
void BoundThunk2(const uint8* p_payload, uint32) {
  const auto* c = reinterpret_cast<const BoundCall2<S, M, A0, A1>*>(p_payload);
  (c->instance->*(c->method))(c->a0, c->a1);
}

/** @brief BoundCall3 的自派发 thunk */
template <typename S, typename M, typename A0, typename A1, typename A2>
void BoundThunk3(const uint8* p_payload, uint32) {
  const auto* c =
      reinterpret_cast<const BoundCall3<S, M, A0, A1, A2>*>(p_payload);
  (c->instance->*(c->method))(c->a0, c->a1, c->a2);
}

// ── ADR-0010: 带变长 blob 的绑定命令（1 个 POD 实参 + 内联 blob）──
// blob 紧随定长负载内联（p_payload + sizeof(BoundCallBlob1)），方法末参收 blob
// 基址。 对齐 Godot `command_queue.push(obj, &Class::method, args…,
// 变长字节)`。

/** @brief 1 元实参 + 内联 blob 的绑定命令定长负载： (目标, 方法, a0) */
template <typename S, typename M, typename A0>
struct BoundCallBlob1 {
  S* instance;  ///< 目标对象指针
  M method;     ///< 成员函数指针（末参为 `const uint8*`，接收 blob 基址）
  A0 a0;        ///< 实参（POD）
};

/** @brief BoundCallBlob1 的自派发 thunk（在定长负载后定位 blob 基址） */
template <typename S, typename M, typename A0>
void BoundThunkBlob1(const uint8* p_payload, uint32) {
  const auto* c = reinterpret_cast<const BoundCallBlob1<S, M, A0>*>(p_payload);
  const uint8* blob = p_payload + sizeof(BoundCallBlob1<S, M, A0>);
  (c->instance->*(c->method))(c->a0, blob);
}

/** @brief 2 元实参 + 内联 blob 的绑定命令定长负载： (目标, 方法, a0, a1) */
template <typename S, typename M, typename A0, typename A1>
struct BoundCallBlob2 {
  S* instance;  ///< 目标对象指针
  M method;     ///< 成员函数指针（末参为 `const uint8*`，接收 blob 基址）
  A0 a0;        ///< 实参 0（POD）
  A1 a1;        ///< 实参 1（POD）
};

/** @brief BoundCallBlob2 的自派发 thunk（在定长负载后定位 blob 基址） */
template <typename S, typename M, typename A0, typename A1>
void BoundThunkBlob2(const uint8* p_payload, uint32) {
  const auto* c =
      reinterpret_cast<const BoundCallBlob2<S, M, A0, A1>*>(p_payload);
  const uint8* blob = p_payload + sizeof(BoundCallBlob2<S, M, A0, A1>);
  (c->instance->*(c->method))(c->a0, c->a1, blob);
}
}  // namespace command_detail

/**
 * @class CommandQueue
 * @brief SPSC 序列化环形缓冲区命令队列 — 主线程到渲染线程的命令代理
 *
 * 主线程通过 PushBound() / PushBoundBlob() 异步推送绑定命令
 * （目标, 方法, 参数）到渲染线程，由消费者 Flush() 执行；大体积变长数据
 * 以 blob 内联。需要结果回传的资源创建走 RenderServer 的 RID 分裂路径
 * （ADR-0010），不在队列内做同步代理。
 *
 * @par 唯一的阻塞出口
 *   Sync() —— 整批屏障，等"调用瞬间已发布的命令全部执行完"（对齐 Godot
 *   `CommandQueueMT::sync()`）。除此之外生产者不阻塞（除背压自旋）。
 *
 * @par 背压
 *   缓冲区满时生产者自旋等待消费者前进（不丢弃、不覆盖）；
 *   shutdown 触发时退出自旋并丢弃命令。
 *
 * @note 命令大小（含头 + 内联变长数据）不得超过 kMaxCommandSize，
 *       超限命令被拒绝并记录错误（返回前不产生副作用）。
 */
class CommandQueue {
 public:
  /** @brief 环形缓冲区容量（字节，2 的幂） */
  static constexpr uint32 kCapacity = 1u << 20;
  /** @brief 环形缓冲区索引掩码（kCapacity - 1），用于按位取模 */
  static constexpr uint32 kMask = kCapacity - 1;
  /** @brief 命令对齐（字节）。ADR-0009 起随 16B 命令头提升为 16：保证回绕填充区
   *  （0 或 ≥ 命令头大小）必能容纳 16B 头 */
  static constexpr uint32 kAlignment = 16;
  /** @brief 单条命令大小上限（受 CommandHeader.size 的 uint16 限制） */
  static constexpr uint32 kMaxCommandSize = 0xFFFF;

  /**
   * @brief 命令头（16 字节，定长；ADR-0009 起含自派发 thunk）
   */
  struct CommandHeader {
    uint8 type;       ///< 队列内部条目类型（kCmdTypeNone/kPadding）
    uint8 reserved0;  ///< 对齐填充（同步机制移除后腾出的字节）
    uint16 size;      ///< 含头总字节数（对齐到 kAlignment，遍历步进用）
    uint32 reserved;  ///< 对齐填充
    CommandExecuteThunk
        exec;  ///< ADR-0009: 自派发 thunk（null = 丢弃并记错误）
  };
  static_assert(sizeof(CommandHeader) == 16, "CommandHeader must be 16 bytes");

 private:
  // SPSC 环形缓冲区（cache-line 对齐避免伪共享）
  alignas(ARHUD_CACHE_LINE_SIZE) std::atomic<uint32> write_pos_{
      0};  ///< 生产者写指针（单调递增字节计数）
  alignas(ARHUD_CACHE_LINE_SIZE) std::atomic<uint32> read_pos_{
      0};  ///< 消费者读指针（单调递增字节计数）
  alignas(
      ARHUD_CACHE_LINE_SIZE) uint8 buffer_[kCapacity];  ///< 序列化命令字节存储

  Semaphore* notify_semaphore_ =
      nullptr;  ///< 通知消费者有命令的"敲门"信号量（非拥有）
  Thread::ID render_thread_id_ =
      Thread::kUnassignedId;  ///< 消费者（渲染）线程 ID
  std::atomic<bool> shutting_down_{
      false};  ///< 关闭标志，置位后丢弃命令并退出自旋

  // ── 整批屏障 Sync() 专用（Godot sync_sems/notify_all 的最小等价物）──
  // 消费者仅在 sync_waiting_ 置位时才付出一次 Semaphore::Post 的代价；
  // 谓词以 read_pos_ 为准，故本标志的丢失/多余唤醒都只影响延迟。
  std::atomic<bool> sync_waiting_{false};  ///< 是否有生产者正等待屏障
  Semaphore sync_semaphore_;               ///< 屏障唤醒（拥有；仅 Sync() 等待）

  /**
   * @brief 序列化写入一条命令（含回绕填充）
   *
   * @param[in] p_type         命令类型
   * @param[in] p_thunk        ADR-0009 自派发 thunk（null = Flush
   * 丢弃并记错误）
   * @param[in] p_fixed        定长负载指针
   * @param[in] p_fixed_size   定长负载字节数
   * @param[in] p_extra        变长数据段数组（可空）
   * @param[in] p_extra_count  变长数据段数量
   *
   * @return 写入总字节数（含填充），shutdown 丢弃返回 0
   */
  uint32 WriteCommand(uint8 p_type, CommandExecuteThunk p_thunk,
                      const void* p_fixed, uint32 p_fixed_size,
                      const CommandDataSpan* p_extra, uint32 p_extra_count);

  /** @brief ADR-0010: 绑定命令异步入队公共尾部（shutdown / 信号量 / 回绕检查）
   *
   * @param[in] p_thunk 自派发 thunk
   * @param[in] p_data  定长负载指针（按值拷贝进缓冲区）
   * @param[in] p_size  定长负载字节数
   */
  void EnqueueBound(CommandExecuteThunk p_thunk, const void* p_data,
                    uint32 p_size) {
    if (shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    if (notify_semaphore_ == nullptr) {
      ARHUD_ASSERT(false,
                   "CommandQueue::PushBound: notify_semaphore_ is null, "
                   "forgot SetNotifySemaphore?");
      return;
    }
    if (WriteCommand(kCmdTypeNone, p_thunk, p_data, p_size, nullptr, 0) == 0) {
      return;
    }
    notify_semaphore_->Post();
  }

  /** @brief ADR-0010: 带变长 blob 的绑定命令入队（blob 内联在定长负载之后）
   *
   * @param[in] p_thunk      自派发 thunk
   * @param[in] p_data       定长负载指针（按值拷贝进缓冲区）
   * @param[in] p_size       定长负载字节数
   * @param[in] p_spans      变长数据段数组（按序内联在定长负载之后）
   * @param[in] p_span_count 变长数据段数量
   */
  void EnqueueBoundBlob(CommandExecuteThunk p_thunk, const void* p_data,
                        uint32 p_size, const CommandDataSpan* p_spans,
                        uint32 p_span_count) {
    if (shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    if (notify_semaphore_ == nullptr) {
      ARHUD_ASSERT(false,
                   "CommandQueue::PushBoundBlob: notify_semaphore_ is null, "
                   "forgot SetNotifySemaphore?");
      return;
    }
    if (WriteCommand(kCmdTypeNone, p_thunk, p_data, p_size, p_spans,
                     p_span_count) == 0) {
      return;
    }
    notify_semaphore_->Post();
  }

 public:
  /** @brief 默认构造（空队列，未绑定消费者） */
  CommandQueue() = default;
  /** @brief 默认析构（缓冲区与信号量生命周期随队列本身） */
  ~CommandQueue() = default;

  ARHUD_DISABLE_COPY_MOVE(CommandQueue);

  /** @brief 绑定"敲门"信号量（消费者等待命令用，不取得所有权）
   *  @param[in] p_semaphore 渲染线程用于等待命令的信号量
   */
  void SetNotifySemaphore(Semaphore* p_semaphore) {
    notify_semaphore_ = p_semaphore;
  }

  /** @brief 注册消费者（渲染）线程 ID
   *  @param[in] p_thread_id 渲染线程 ID
   */
  void SetRenderThreadId(Thread::ID p_thread_id) {
    render_thread_id_ = p_thread_id;
  }

  /** @brief 消费者（渲染线程）是否已注册
   *  @return true 表示已通过 SetRenderThreadId 注册
   */
  bool HasConsumer() const {
    return render_thread_id_ != Thread::kUnassignedId;
  }

  /** @brief 调用者是否为消费者线程（渲染线程同线程短路用）
   *  @return true 表示当前调用线程即已注册的渲染线程
   */
  bool IsConsumerThread() const {
    return render_thread_id_ != Thread::kUnassignedId &&
           Thread::GetCallerId() == render_thread_id_;
  }

  /** @brief 标记队列进入关闭状态（生产者退出自旋并丢弃后续命令） */
  void SignalShutdown() {
    shutting_down_.store(true, std::memory_order_release);
  }

  /** @brief 查询是否已进入关闭状态
   *  @return true 表示已关闭
   */
  bool IsShuttingDown() const {
    return shutting_down_.load(std::memory_order_acquire);
  }

  // ─────────────────────────────────────────────────────────────────
  //  主线程 API（生产者）
  // ─────────────────────────────────────────────────────────────────

  // ADR-0010：类型标签命令与 RD 遗留函数指针同步代理（PushAndSync /
  // kFunctionCall） 均已随命令结构体一并删除；全部业务命令走下方 PushBound /
  // PushBoundBlob —— 绑定 (目标, 方法, 参数)。

  /**
   * @brief 整批屏障：阻塞当前线程，直到"调用瞬间已发布的全部命令"执行完毕
   *
   * 对齐 Godot `CommandQueueMT::sync()` —— Godot 的实现是
   * `push_and_sync(this, &CommandQueueMT::_no_op)`，即压一条 no-op 同步命令、
   * 等 sync_head 越过它；本实现等价地等 `read_pos_` 追平调用瞬间的
   * `write_pos_`，
   * **不引入逐命令返回值回传（push_and_ret）与超时机器**。
   *
   * @par 各线程行为
   *   - 生产者线程：阻塞至屏障达成（帧边界收口、关闭前 flush_all 语义）；
   *   - 消费者线程 / 批处理内部 / 未注册消费者：退化为 FlushIfPending()，
   *     就地排空，绝不自我等待。
   *
   * @par 为什么信号量丢失不影响正确性
   *   循环谓词只认 `read_pos_`；每轮都重新 Post notify_semaphore_（消费者可能
   *   已在两轮 Flush 之间）并以 WaitTimeout 兜底，因此 sync_semaphore_ 的唤醒
   *   是"加速手段"而非"正确性依赖"。
   *
   * @note 假定单一生产者（SPSC）。多个线程同时等待时，先离场者会清除
   *       sync_waiting_，其余等待者退化为 kSyncPollMs 轮询，仅延迟变差。
   */
  void Sync();

  // ─────────────────────────────────────────────────────────────────
  //  ADR-0010: 绑定命令入队 —— (目标, 方法, 参数)
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 异步推送"绑定命令"：直接点名 (目标对象, 方法)
   *
   * 对齐 Godot `command_queue.push(RSG::material_storage, &Storage::method,
   * args…)`。 实参以显式 POD 成员存放（0~3 元），随命令 memcpy
   * 进缓冲区，无动态分配。
   *
   * @param[in] p_instance 目标对象（通常取 `RenderGlobals::xxx_storage`）
   * @param[in] p_method   成员函数指针（其所属类 S 决定调用目标类型）
   * @param[in] p_a0…p_a2  实参（须为 trivially copyable）
   */
  template <typename S, typename M>
  void PushBound(S* p_instance, M p_method) {
    using Cmd = command_detail::BoundCall0<S, M>;
    static_assert(std::is_trivially_copyable_v<Cmd>,
                  "bound command must be trivially copyable");
    const Cmd cmd{p_instance, p_method};
    EnqueueBound(&command_detail::BoundThunk0<S, M>, &cmd, sizeof(Cmd));
  }

  template <typename S, typename M, typename A0>
  void PushBound(S* p_instance, M p_method, A0 p_a0) {
    using Cmd = command_detail::BoundCall1<S, M, A0>;
    static_assert(std::is_trivially_copyable_v<Cmd>,
                  "bound command must be trivially copyable");
    const Cmd cmd{p_instance, p_method, p_a0};
    EnqueueBound(&command_detail::BoundThunk1<S, M, A0>, &cmd, sizeof(Cmd));
  }

  template <typename S, typename M, typename A0, typename A1>
  void PushBound(S* p_instance, M p_method, A0 p_a0, A1 p_a1) {
    using Cmd = command_detail::BoundCall2<S, M, A0, A1>;
    static_assert(std::is_trivially_copyable_v<Cmd>,
                  "bound command must be trivially copyable");
    const Cmd cmd{p_instance, p_method, p_a0, p_a1};
    EnqueueBound(&command_detail::BoundThunk2<S, M, A0, A1>, &cmd, sizeof(Cmd));
  }

  template <typename S, typename M, typename A0, typename A1, typename A2>
  void PushBound(S* p_instance, M p_method, A0 p_a0, A1 p_a1, A2 p_a2) {
    using Cmd = command_detail::BoundCall3<S, M, A0, A1, A2>;
    static_assert(std::is_trivially_copyable_v<Cmd>,
                  "bound command must be trivially copyable");
    const Cmd cmd{p_instance, p_method, p_a0, p_a1, p_a2};
    EnqueueBound(&command_detail::BoundThunk3<S, M, A0, A1, A2>, &cmd,
                 sizeof(Cmd));
  }

  /**
   * @brief 异步推送"带变长 blob 的绑定命令"：1 个 POD 实参 + 内联 blob
   *
   * blob 各段被连续 memcpy 进环形缓冲区定长负载之后；被绑定方法的**末参**
   * 接收 blob 基址（`const uint8*`），据结构体内记录的段大小自行切片。
   * 对齐 Godot `command_queue.push(obj, &Class::method, args…, 变长数据)`。
   *
   * @param[in] p_instance   目标对象
   * @param[in] p_method     成员函数指针（末参为 `const uint8*`）
   * @param[in] p_a0         实参（POD）
   * @param[in] p_spans      变长数据段数组（按序内联）
   * @param[in] p_span_count 段数
   */
  template <typename S, typename M, typename A0>
  void PushBoundBlob(S* p_instance, M p_method, A0 p_a0,
                     const CommandDataSpan* p_spans, uint32 p_span_count) {
    using Cmd = command_detail::BoundCallBlob1<S, M, A0>;
    static_assert(std::is_trivially_copyable_v<Cmd>,
                  "bound blob command must be trivially copyable");
    const Cmd cmd{p_instance, p_method, p_a0};
    EnqueueBoundBlob(&command_detail::BoundThunkBlob1<S, M, A0>, &cmd,
                     sizeof(Cmd), p_spans, p_span_count);
  }

  /** @brief PushBoundBlob 的 2 元实参变体（如 `(RID, params, blob)`） */
  template <typename S, typename M, typename A0, typename A1>
  void PushBoundBlob(S* p_instance, M p_method, A0 p_a0, A1 p_a1,
                     const CommandDataSpan* p_spans, uint32 p_span_count) {
    using Cmd = command_detail::BoundCallBlob2<S, M, A0, A1>;
    static_assert(std::is_trivially_copyable_v<Cmd>,
                  "bound blob command must be trivially copyable");
    const Cmd cmd{p_instance, p_method, p_a0, p_a1};
    EnqueueBoundBlob(&command_detail::BoundThunkBlob2<S, M, A0, A1>, &cmd,
                     sizeof(Cmd), p_spans, p_span_count);
  }

  // ─────────────────────────────────────────────────────────────────
  //  渲染线程 API（消费者）
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 刷新队列，执行所有待处理命令
   *
   * 渲染线程调用。无锁遍历环形缓冲区：
   *   1. 跳过 kPadding 填充
   *   2. 其余命令经自派发 thunk 直接执行
   *
   * @note 入口有 thread_local 再入保护：批处理内（thunk 里）再次调用本函数
   *       立即返回——read_pos_ 只在批末发布，嵌套排空会用陈旧游标重复执行命令。
   * @note 断言调用者为"事实上的执行者"（见文件头 Flush 的执行者约束）。
   */
  void Flush();

  /**
   * @brief 有待处理命令时才排空（对齐 Godot `flush_if_pending()`）
   *
   * 用途：渲染线程在 Flush 之外直调 Storage 前，先把更早入队的命令跑掉，
   * 保证程序顺序（RenderServer::CanBypassQueue 即在此调用）。
   * 批处理内部调用为无害空操作（被再入保护挡住，外层循环随后会跑到）。
   */
  void FlushIfPending();

  /**
   * @brief 是否存在待处理命令（只读探测，不执行）
   * @return true 表示 read_pos_ 尚未追平 write_pos_
   */
  bool HasPending() const;
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_COMMAND_QUEUE_H_
