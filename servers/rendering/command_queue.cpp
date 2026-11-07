/**
 * @file command_queue.cpp
 * @brief CommandQueue 实现 — SPSC 序列化环形缓冲区（B2 重构 v3）
 *
 * @par B2 重构要点
 *   - Entry 槽数组 → 1MB 字节环形缓冲区，命令 = 头 + 定长负载 + 内联变长数据
 *   - 序列化 PushBound / PushBoundBlob（绑定命令，按值拷贝，异步安全）
 *   - ADR-0010：RD 遗留函数指针代理（kFunctionCall /
 * PushAndSync）及其同步机器已移除
 *   - 回绕用 kPadding 填充命令处理；满缓冲区自旋背压
 *
 * @author yameng.he
 * @version 3.2
 * @date 2026-08-22
 * @date 2026-10-13 移除 PushAndSync 同步机器（队列收敛为纯异步 SPSC）
 * @date 2026-10-13 对齐 Godot：整批屏障 Sync() + FlushIfPending() + Flush
 * 再入保护/执行者断言
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "command_queue.h"

#include <thread>

#include "io/logger.h"
#include "os/sync.h"
#include "os/thread.h"

namespace arhud {
namespace {
/** @brief Flush 再入标志（对齐 Godot CommandQueueMT::flushing）
 *
 *  只有"正在执行 Flush 的线程"会读写它，天然无跨线程竞争；
 *  放在 .cpp 文件内而不进头文件，避免对外暴露实现细节。 */
thread_local bool t_in_flush = false;

/** @brief Sync() 等待屏障唤醒的兜底轮询间隔（ms）
 *
 *  谓词以 read_pos_ 为准，sync_semaphore_ 的唤醒只是加速手段，
 *  故本值仅决定"唤醒信号恰好丢失时"的最坏额外延迟。 */
constexpr uint32 kSyncPollMs = 2;

/** @brief 单调字节计数器的"已追平"判定（对 uint32 自然回绕安全）
 *  @note 在途字节数恒 < kCapacity(1MB) ≪ 2^31，有符号差值判定成立 */
bool Reached(uint32 p_pos, uint32 p_target) {
  return static_cast<int32>(p_pos - p_target) >= 0;
}
}  // namespace

// ─────────────────────────────────────────────────────────────────────
//  私有：序列化写入
// ─────────────────────────────────────────────────────────────────────

/** @brief 序列化写入一条命令（含回绕填充与背压等待）
 *
 * @param[in] p_type        命令类型（kCmdTypeNone / kPadding）
 * @param[in] p_thunk       自派发 thunk（null 时 Flush 丢弃并记错误）
 * @param[in] p_fixed       定长负载指针（可空）
 * @param[in] p_fixed_size  定长负载字节数
 * @param[in] p_extra       变长数据段数组（可空）
 * @param[in] p_extra_count 变长数据段数量
 * @return 写入总字节数（含填充）；超限或被 shutdown 丢弃时返回 0
 *
 * @par 写入布局
 *   `[CommandHeader][定长负载][变长段0][变长段1]…`，整体对齐到 kAlignment，
 *   保证任何命令起始偏移均为 kAlignment 的倍数。
 */
uint32 CommandQueue::WriteCommand(uint8 p_type, CommandExecuteThunk p_thunk,
                                  const void* p_fixed, uint32 p_fixed_size,
                                  const CommandDataSpan* p_extra,
                                  uint32 p_extra_count) {
  // 总大小 = 头 + 定长负载 + 变长数据段，对齐到 kAlignment（ADR-0009 起为 16）
  uint32 total = sizeof(CommandHeader) + p_fixed_size;
  for (uint32 i = 0; i < p_extra_count; ++i) {
    total += p_extra[i].size;
  }
  total = (total + kAlignment - 1) & ~(kAlignment - 1);

  if (total > kMaxCommandSize) {
    ARHUD_LOG_ERROR("kInvalidParameter",
                    "CommandQueue: command too large (%u > %u), rejected. "
                    "Large payloads belong in ItemStorage state or RD staging "
                    "uploads, not commands.",
                    total, kMaxCommandSize);
    return 0;
  }

  uint32 pos = write_pos_.load(std::memory_order_relaxed);
  uint32 offset = pos & kMask;

  // 回绕判定：命令必须连续存放。跨越尾部时先填充到尾部再从 0 开始。
  // offset 恒为 kAlignment 的倍数（ADR-0009 起为 16），故 pad 为 0 或 >= 16
  // （= sizeof(CommandHeader)），填充头必能容纳。
  const uint32 pad = (offset + total > kCapacity) ? (kCapacity - offset) : 0;

  // 背压：可用空间不足则等待消费者前进（禁止覆盖未消费数据）
  while (pos - read_pos_.load(std::memory_order_acquire) + pad + total >
         kCapacity) {
    if (shutting_down_.load(std::memory_order_acquire)) {
      return 0;
    }
    std::this_thread::yield();
  }

  // 写填充命令（回绕时）
  if (pad > 0) {
    CommandHeader pad_header{};
    pad_header.type = kCmdTypePadding;
    pad_header.size = static_cast<uint16>(pad);
    memcpy(&buffer_[offset], &pad_header, sizeof(pad_header));
  }

  // 写真实命令（回绕后从 0 开始，命令内部不再跨越尾部）
  uint32 w = offset + pad;
  CommandHeader header{};
  header.type = p_type;
  header.size = static_cast<uint16>(total);
  header.exec = p_thunk;
  memcpy(&buffer_[w & kMask], &header, sizeof(header));
  w += sizeof(header);

  if (p_fixed_size > 0 && p_fixed != nullptr) {
    memcpy(&buffer_[w & kMask], p_fixed, p_fixed_size);
    w += p_fixed_size;
  }

  for (uint32 i = 0; i < p_extra_count; ++i) {
    if (p_extra[i].size > 0 && p_extra[i].data != nullptr) {
      memcpy(&buffer_[w & kMask], p_extra[i].data, p_extra[i].size);
    }
    w += p_extra[i].size;
  }

  // 发布：消费者 acquire 读 write_pos_ 后可见全部命令字节
  write_pos_.store(pos + pad + total, std::memory_order_release);
  return pad + total;
}

// ─────────────────────────────────────────────────────────────────────
//  渲染线程 API
// ─────────────────────────────────────────────────────────────────────

/** @brief 刷新队列，执行所有待处理命令（渲染线程调用）
 *
 * 无锁遍历环形缓冲区直至 read == write：跳过 kPadding 填充，其余命令经
 * 自派发 thunk 直接执行。单条命令失败不影响后续命令。
 */
void CommandQueue::Flush() {
  // 再入保护（对齐 Godot CommandQueueMT 的 thread_local flushing）：
  // read_pos_ 只在批末发布，批处理内嵌套排空会拿陈旧的局部游标重复执行命令。
  if (t_in_flush) {
    return;
  }

  // 执行者约束：注册了消费者时只有该消费者可排空；未注册（单线程/测试形态）
  // 时调用方就是执行者。两线程并发 Flush 各自推进未发布的游标 → 重复执行。
  ARHUD_ASSERT(!HasConsumer() || IsConsumerThread(),
               "CommandQueue::Flush must run on the consumer thread");

  t_in_flush = true;

  const uint32 start = read_pos_.load(std::memory_order_relaxed);
  uint32 read = start;
  const uint32 write = write_pos_.load(std::memory_order_acquire);

  // 逐条执行。单条命令失败不影响后续命令的遍历与派发。
  while (read != write) {
    uint32 offset = read & kMask;

    CommandHeader header{};
    memcpy(&header, &buffer_[offset], sizeof(header));
    const uint8* payload = &buffer_[offset + sizeof(CommandHeader)];
    const uint32 payload_size = header.size >= sizeof(CommandHeader)
                                    ? header.size - sizeof(CommandHeader)
                                    : 0;

    // 命令自派发：kPadding 为回绕填充，直接跳过；其余经 thunk 执行。
    if (header.type == kCmdTypePadding) {
      read += header.size;
      continue;
    }

    if (header.exec != nullptr) {
      header.exec(payload, payload_size);
    } else {
      ARHUD_LOG_ERROR("kInvalidState",
                      "CommandQueue::Flush: null thunk, drop command type=%u",
                      header.type);
    }

    read += header.size;
  }

  // 批末一次性发布（release）：生产者的背压自旋与 Sync 屏障都以此为准
  const bool did_work = (read != start);
  read_pos_.store(read, std::memory_order_release);
  t_in_flush = false;

  // 屏障唤醒：空批不必打扰等待者（其谓词本就已满足），
  // 且仅在有等待者时付出一次 Semaphore 代价
  // （对齐 Godot 批内遇到 sync 命令时 sync_cond_var.notify_all()）
  if (did_work && sync_waiting_.load(std::memory_order_relaxed)) {
    sync_semaphore_.Post();
  }
}

/** @brief 有待处理命令时才排空（对齐 Godot flush_if_pending）
 *
 *  批处理内部调用是无害空操作：再入保护直接返回，外层循环稍后会跑到这里。
 */
void CommandQueue::FlushIfPending() {
  if (t_in_flush) {
    return;
  }
  if (HasPending()) {
    Flush();
  }
}

/** @brief 只读探测：是否存在尚未执行的命令 */
bool CommandQueue::HasPending() const {
  return read_pos_.load(std::memory_order_relaxed) !=
         write_pos_.load(std::memory_order_acquire);
}

/** @brief 整批屏障：等"调用瞬间已发布的全部命令"执行完（对齐 Godot sync()）
 *
 * Godot 用 push_and_sync(this, &_no_op) 把等待落在批边界上；这里以
 * read_pos_ 追平快照 target 为谓词，语义等价且不需要逐命令的同步槽位。
 */
void CommandQueue::Sync() {
  // 自我等待没有意义：消费者线程 / 批处理内部 / 尚无消费者 → 就地排空
  if (t_in_flush || !HasConsumer() || IsConsumerThread()) {
    FlushIfPending();
    return;
  }

  // 关闭中命令已被丢弃，屏障永远不会达成
  if (IsShuttingDown()) {
    return;
  }

  // 先置等待标志，避免与"恰在此时结束的批"错过唤醒（漏了也只多等一个轮询周期）
  sync_waiting_.store(true, std::memory_order_relaxed);
  const uint32 target = write_pos_.load(std::memory_order_acquire);

  while (!Reached(read_pos_.load(std::memory_order_acquire), target)) {
    // 消费者可能已越过本轮 Flush（正在渲染），重新敲门确保它回来排空；
    // 多余 token 只会让它多做一次空 Flush，无害。
    if (notify_semaphore_ != nullptr) {
      notify_semaphore_->Post();
    }

    // 唤醒只是加速：信号丢失/多余均由 WaitTimeout 轮询 + 谓词复查兜底
    sync_semaphore_.WaitTimeout(kSyncPollMs);

    if (IsShuttingDown()) {
      break;
    }
  }

  sync_waiting_.store(false, std::memory_order_relaxed);
}

}  // namespace arhud
