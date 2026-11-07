/**
 * @file test_command_queue.cpp
 * @brief CommandQueue 单测 — SPSC 序列化环形缓冲区 + 绑定命令（PushBound）
 *
 * @par 测试覆盖
 *   1. 绑定命令往返：主线程 PushBound → 渲染线程 Flush 执行 → 结果可见
 *   2. 多次连续 PushBound：验证 FIFO 顺序执行 + 环形缓冲区稳态
 *   3. 手动排空：无消费者线程时，PushBound 后直接 Flush() 即执行
 *   4. Shutdown 已触发：PushBound 丢弃命令，Flush 不执行
 *   5. Flush 空队列：无崩溃
 *   6. IsShuttingDown 查询
 *   7. FlushIfPending / HasPending：按需排空 + 空队列无副作用
 *   8. Sync() 整批屏障：返回即代表已发布命令全部执行完（不靠测试侧轮询）
 *   9. Sync() 无消费者形态：就地排空，绝不自我等待
 *  10. 批内嵌套 Flush / FlushIfPending / Sync：被再入保护挡住，命令不重复执行
 *
 * @note ADR-0010 后队列仅保留异步绑定命令（PushBound / PushBoundBlob）；
 *       RD 遗留的同步函数指针代理（PushAndSync）及其同步机器已移除。
 *       仅存的阻塞出口是 Sync()（整批屏障，对齐 Godot
 * `CommandQueueMT::sync()`）， 没有逐命令的返回值回传与超时机器。 CommandQueue
 * 是纯 CPU 类，不需要 GL/窗口/NullDriver。 只需 arhud_servers（含
 * CommandQueue）+ arhud_core（Thread/Semaphore）。
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "command_queue.h"
#include "os/memory.h"
#include "os/sync.h"
#include "os/thread.h"

using namespace arhud;

namespace {
// 绑定命令的接收方：累加实参并计数，供测试在异步执行完成后观察结果。
struct Sink {
  std::atomic<int> executed{0};
  std::atomic<int> sum{0};

  void Add(int p_value) {
    sum.fetch_add(p_value, std::memory_order_relaxed);
    executed.fetch_add(1, std::memory_order_release);
  }
};

/**
 * @brief 再入探针：命令执行过程中再次请求排空 / 屏障
 *
 * 模拟"渲染线程在 Flush 批处理内回调 RenderServer"的路径
 * （CanBypassQueue → FlushIfPending，以及批内误调 Sync）。
 * 若队列缺少再入保护，嵌套 Flush 会用外层未发布的陈旧游标重复执行命令，
 * executed 就会超过预期计数。
 */
struct ReentrantSink {
  CommandQueue* queue = nullptr;
  std::atomic<int> executed{0};

  void AddAndReenter(int) {
    executed.fetch_add(1, std::memory_order_release);
    queue->Flush();           // 必须被再入保护挡住
    queue->FlushIfPending();  // 同上
    queue->Sync();            // 消费者线程：退化为就地排空 → 也被挡住
  }
};

// 自旋等待命令执行到期望数量（除 Sync
// 屏障用例外，测试侧仍以轮询观察异步执行）。
template <typename TSink>
void WaitExecuted(const TSink& p_sink, int p_expected) {
  while (p_expected < 0 ||
         p_sink.executed.load(std::memory_order_acquire) != p_expected) {
    std::this_thread::yield();
  }
}
}  // namespace

// ═══════════════════════════════════════════════════════════════════════
//  带消费者线程的测试夹具
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 提供 CommandQueue + 消费者线程，模拟主线程→渲染线程交互。
 *
 * SetUp 启动消费者线程，等待其就绪（SetRenderThreadId 完成）。
 * TearDown 停止消费者线程，确保无残留命令。
 */
class CommandQueueTest : public ::testing::Test {
 protected:
  CommandQueue queue_;
  Semaphore notify_sem_;
  Semaphore ready_sem_;
  Thread consumer_;
  std::atomic<bool> running_{true};

  void SetUp() override {
    queue_.SetNotifySemaphore(&notify_sem_);
    consumer_.Start(ConsumerEntry, this);
    ready_sem_.Wait();  // 等消费者就绪
  }

  void TearDown() override {
    running_.store(false, std::memory_order_release);
    notify_sem_.Post();  // 唤醒消费者退出
    consumer_.WaitToFinish();
  }

 private:
  static void ConsumerEntry(void* p_data) {
    auto* self = static_cast<CommandQueueTest*>(p_data);
    self->queue_.SetRenderThreadId(Thread::GetCallerId());
    self->ready_sem_.Post();  // 通知主线程：已就绪

    while (self->running_.load(std::memory_order_acquire)) {
      if (self->notify_sem_.WaitTimeout(100)) {
        self->queue_.Flush();
      }
    }
    self->queue_.Flush();  // 退出前排空
  }
};

// ═══════════════════════════════════════════════════════════════════════
//  测试用例
// ═══════════════════════════════════════════════════════════════════════

// 1. 基本绑定命令往返（异步 PushBound → 渲染线程执行 → 结果可见）
TEST_F(CommandQueueTest, BoundCommandRoundtrip) {
  Sink sink;
  queue_.PushBound(&sink, &Sink::Add, 42);
  WaitExecuted(sink, 1);
  EXPECT_EQ(sink.sum.load(std::memory_order_acquire), 42);
}

// 2. 多次连续 PushBound（验证顺序执行 + 环形缓冲区稳态）
TEST_F(CommandQueueTest, MultipleBoundCommands) {
  Sink sink;
  constexpr int kCount = 100;
  for (int i = 0; i < kCount; ++i) {
    queue_.PushBound(&sink, &Sink::Add, i);
  }
  WaitExecuted(sink, kCount);
  // 0 + 1 + … + 99 = 4950
  EXPECT_EQ(sink.sum.load(std::memory_order_acquire),
            kCount * (kCount - 1) / 2);
}

// 3. 手动排空：无消费者线程时，PushBound 后直接 Flush() 即执行
// @note CommandQueue 内含 1MB 环形缓冲区，超出默认线程栈，须堆分配。
TEST(CommandQueueFlush, ExecutesEnqueuedBoundCommand) {
  CommandQueue* queue = ARHUD_NEW(CommandQueue);
  Semaphore sem;
  queue->SetNotifySemaphore(&sem);

  Sink sink;
  queue->PushBound(&sink, &Sink::Add, 7);
  queue->Flush();  // 本线程充当消费者
  EXPECT_EQ(sink.executed.load(std::memory_order_acquire), 1);
  EXPECT_EQ(sink.sum.load(std::memory_order_acquire), 7);

  ARHUD_DELETE(queue);
}

// 4. Shutdown 已触发：PushBound 丢弃命令，Flush 不执行
TEST(CommandQueueShutdown, ShutdownDropsCommand) {
  CommandQueue* queue = ARHUD_NEW(CommandQueue);
  Semaphore sem;
  queue->SetNotifySemaphore(&sem);
  queue->SignalShutdown();

  Sink sink;
  queue->PushBound(&sink, &Sink::Add, 42);
  queue->Flush();
  EXPECT_EQ(sink.executed.load(std::memory_order_acquire), 0);  // 未执行

  ARHUD_DELETE(queue);
}

// 5. Flush 空队列：无崩溃
TEST(CommandQueueFlush, EmptyQueueNoCrash) {
  CommandQueue* queue = ARHUD_NEW(CommandQueue);
  queue->Flush();
  SUCCEED();
  ARHUD_DELETE(queue);
}

// 6. IsShuttingDown 查询
TEST(CommandQueueShutdown, IsShuttingDownQuery) {
  CommandQueue* queue = ARHUD_NEW(CommandQueue);
  EXPECT_FALSE(queue->IsShuttingDown());
  queue->SignalShutdown();
  EXPECT_TRUE(queue->IsShuttingDown());
  ARHUD_DELETE(queue);
}

// 7. FlushIfPending / HasPending：按需排空，空队列无副作用
TEST(CommandQueueFlush, FlushIfPendingDrainsPendingCommands) {
  CommandQueue* queue = ARHUD_NEW(CommandQueue);
  Semaphore sem;
  queue->SetNotifySemaphore(&sem);

  Sink sink;
  EXPECT_FALSE(queue->HasPending());

  queue->PushBound(&sink, &Sink::Add, 5);
  EXPECT_TRUE(queue->HasPending());

  queue->FlushIfPending();
  EXPECT_EQ(sink.executed.load(std::memory_order_acquire), 1);
  EXPECT_FALSE(queue->HasPending());

  queue->FlushIfPending();  // 已空：不得产生副作用
  EXPECT_EQ(sink.executed.load(std::memory_order_acquire), 1);

  ARHUD_DELETE(queue);
}

// 8. Sync() 整批屏障：返回即代表已发布命令全部执行完（本用例不靠测试侧轮询）
TEST_F(CommandQueueTest, SyncBarrierWaitsForPublishedCommands) {
  Sink sink;
  constexpr int kCount = 50;
  for (int i = 0; i < kCount; ++i) {
    queue_.PushBound(&sink, &Sink::Add, 1);
  }

  queue_.Sync();  // 生产者阻塞至消费者跨过批边界并发布 read_pos_

  EXPECT_EQ(sink.executed.load(std::memory_order_acquire), kCount);
  EXPECT_EQ(sink.sum.load(std::memory_order_acquire), kCount);
  EXPECT_FALSE(queue_.HasPending());
}

// 8b. Sync() 可重复使用：不漏唤醒（唤醒信号丢失时由轮询兜底）
TEST_F(CommandQueueTest, SyncBarrierIsRepeatable) {
  Sink sink;
  for (int round = 0; round < 3; ++round) {
    queue_.PushBound(&sink, &Sink::Add, round + 1);
    queue_.Sync();
    EXPECT_EQ(sink.executed.load(std::memory_order_acquire), round + 1);
  }
  EXPECT_EQ(sink.sum.load(std::memory_order_acquire), 1 + 2 + 3);
}

// 9. Sync() 的无消费者形态：调用方即执行者，就地排空，绝不自我等待
TEST(CommandQueueFlush, SyncWithoutConsumerDrainsInline) {
  CommandQueue* queue = ARHUD_NEW(CommandQueue);
  Semaphore sem;
  queue->SetNotifySemaphore(&sem);

  Sink sink;
  queue->PushBound(&sink, &Sink::Add, 9);
  queue->Sync();
  EXPECT_EQ(sink.executed.load(std::memory_order_acquire), 1);

  ARHUD_DELETE(queue);
}

// 10. 批内嵌套 Flush / FlushIfPending / Sync：被再入保护挡住，命令不重复执行
TEST_F(CommandQueueTest, NestedFlushInBatchDoesNotDoubleExecute) {
  ReentrantSink sink;
  sink.queue = &queue_;

  queue_.PushBound(&sink, &ReentrantSink::AddAndReenter, 1);
  queue_.Sync();
  EXPECT_EQ(sink.executed.load(std::memory_order_acquire), 1);

  // 同批压入两条：外层游标若被嵌套排空破坏，这里会重复执行而计数超标
  queue_.PushBound(&sink, &ReentrantSink::AddAndReenter, 1);
  queue_.PushBound(&sink, &ReentrantSink::AddAndReenter, 1);
  queue_.Sync();
  EXPECT_EQ(sink.executed.load(std::memory_order_acquire), 3);
}
