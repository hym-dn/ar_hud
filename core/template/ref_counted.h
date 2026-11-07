/**
 * @file ref_counted.h
 * @brief 引用计数基类 + 外部控制块
 *
 * 采用外部控制块模式（参考 std::shared_ptr/weak_ptr）：
 *   - RefCounted 是纯标记基类，不持有任何引用计数
 *   - RefCountBlock 独立分配，持有 strong_count / weak_count / object
 *   - 删除对象和删除控制块分离，消除竞态
 *
 * @par 为什么不用内部引用计数？
 *
 * 如果把引用计数放在对象内部（如 Godot 的 RefCounted），会导致致命问题：
 *
 * @code
 * // 错误方案：内部引用计数
 * class RefCounted {
 *     atomic<uint32> strong_count_;
 *     atomic<uint32> weak_count_;
 * };
 *
 * // 场景：Ref 和 WeakRef 同时析构
 * Ref<T>::~Ref() {
 *     if (ptr_->ReleaseStrongRef() == 0) {
 *         delete ptr_;  // 对象被删除，strong_count_ 和 weak_count_ 也被销毁！
 *     }
 * }
 *
 * WeakRef<T>::~WeakRef() {
 *     if (ptr_->ReleaseWeakRef() == 0 && ptr_->GetStrongCount() == 0) {
 *         delete ptr_;  // 问题：ptr_ 已经是悬空指针，访问 weak_count_ 是 UB！
 *     }
 * }
 * @endcode
 *
 * 根本问题：对象被删除后，对象内部的计数也被销毁。WeakRef
 * 无法再访问对象来检查计数。
 *
 * @par 外部控制块如何解决？
 *
 * 把计数放在独立于对象的地方（控制块），并确保控制块在所有 WeakRef
 * 释放前不被删除。
 *
 * @code
 * // 正确方案：外部控制块
 * Ref<T>:      object_ + block_  (16 字节)
 * WeakRef<T>:  block_            (8 字节)   ← 只持有控制块
 * RefCountBlock: strong + weak + object_    ← 独立于对象
 *
 * // 场景：Ref 和 WeakRef 同时析构
 * Ref<T>::~Ref() {
 *     if (block_->ReleaseStrongRef() == 0) {
 *         delete object_;           // 对象被删除
 *         block_->object_ = nullptr; // 标记对象已删除
 *         if (block_->ReleaseWeakRef() == 0) {
 *             delete block_;        // 控制块可能还存活
 *         }
 *     }
 * }
 *
 * WeakRef<T>::~WeakRef() {
 *     if (block_->ReleaseWeakRef() == 0) {
 *         delete block_;            // 控制块被删除（对象早已被删除）
 *     }
 * }
 * @endcode
 *
 * 关键：控制块独立于对象，对象被删除后控制块仍然存活。WeakRef
 * 通过控制块访问计数， 而不是通过对象。这就是为什么 std::shared_ptr 和
 * std::weak_ptr 也采用外部控制块。
 *
 * @par 隐式弱引用
 *
 * MakeRef 创建对象时，weak_count 初始为 1（不是 0），这称为"隐式弱引用"。
 * 它保证控制块在最后一个 Ref 释放前不会被删除。
 *
 * @code
 * Ref<T> r = MakeRef<T>();
 * // 此时：strong=1, weak=1（隐式弱引用）
 *
 * WeakRef<T> w = r;
 * // 此时：strong=1, weak=2（隐式弱引用 + 显式弱引用）
 *
 * r.Reset();
 * // 此时：strong=0, weak=1（隐式弱引用）
 * // 对象被删除，但控制块还存活（因为 weak=1）
 *
 * w.Reset();
 * // 此时：strong=0, weak=0
 * // 控制块被删除
 * @endcode
 *
 * @par 生命周期规则
 *   - strong_count 降为 0 → 删除对象（但保留控制块）
 *   - weak_count 降为 0 → 删除控制块
 *   - 每个"降为 0"的事件只有一个线程能观察到（fetch_sub 原子性）
 *
 * @par 线程安全保证
 *
 * 1. 只有一个线程能观察到 strong_count 从 1→0（fetch_sub 原子性）
 * 2. 只有一个线程能观察到 weak_count 从 1→0（fetch_sub 原子性）
 * 3. WeakRef::Lock() 使用 CAS 循环原子地检查并递增 strong_count
 * 4. 如果 CAS 成功，对象确定还活着；如果失败，对象已被删除
 *
 * @par 内存布局
 *   @code
 *   RefCountBlock（控制块，独立分配）
 *     ├── atomic<uint32> strong_count
 *     ├── atomic<uint32> weak_count
 *     └── RefCounted *object
 *
 *   RefCounted（对象，纯标记基类）
 *     └── ... 业务数据 ...
 *   @endcode
 *
 * @author yameng.he
 * @version 5.0
 * @date 2026-08-16
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#ifndef ARHUD_CORE_TEMPLATE_REF_COUNTED_H_
#define ARHUD_CORE_TEMPLATE_REF_COUNTED_H_

#include "os/memory.h"
#include "template/safe_refcount.h"
#include "typedefs.h"

namespace arhud {

class RefCounted;

// ═══════════════════════════════════════════════════════════════════════
// RefCountBlock（外部控制块）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 引用计数控制块
 *
 * 独立于对象分配，管理引用计数。这是外部控制块模式的核心。
 *
 * @par 为什么需要控制块？
 *
 * 对象被删除后，WeakRef 需要一种安全的方式检查对象是否还活着。
 * 如果计数在对象内部，对象删除后计数也被销毁，WeakRef 无法访问。
 * 控制块独立于对象，在所有 WeakRef 释放前保持存活。
 *
 * @par 关键设计
 *   - strong_count 降为 0 时删除对象，但保留控制块
 *   - weak_count 降为 0 时删除控制块
 *   - 每个"降为 0"只有一个线程能观察到（fetch_sub 原子性）
 *   - 无需同时检查两个计数，消除竞态
 *
 * @par 为什么 object_ 是 RefCounted* 而不是 void*？
 *
 * 1. 类型安全：确保只能管理继承自 RefCounted 的对象
 * 2. 虚析构：通过 RefCounted* 删除派生类对象时，虚析构保证正确调用派生类析构
 * 3. WeakRef 安全：Lock() 时从 block_->object_ 获取对象指针，类型已知
 *
 * @par 线程安全
 *
 * 所有计数操作使用原子操作（acq_rel 语义），无锁实现。
 * fetch_sub 的原子性保证只有一个线程能观察到计数从 1→0。
 */
class RefCountBlock {
  ARHUD_DISABLE_COPY_MOVE(RefCountBlock);

 public:
  /**
   * @brief 强引用计数
   *
   * 管理对象生命周期。降为 0 时删除对象。
   * 每个 Ref<T> 持有一个强引用。
   */
  SafeNumeric<uint32> strong_count_;

  /**
   * @brief 弱引用计数
   *
   * 管理控制块生命周期。降为 0 时删除控制块。
   * 每个 WeakRef<T> 持有一个弱引用。
   * MakeRef 创建时初始为 1（隐式弱引用），保证控制块在最后一个 Ref 释放前存活。
   */
  SafeNumeric<uint32> weak_count_;

  /**
   * @brief 对象指针
   *
   * 指向被管理的对象。对象被删除后置为 nullptr。
   * WeakRef::Lock() 通过此指针获取对象。
   *
   * 为什么需要这个指针？
   * - WeakRef 只持有 block_，不持有 object_
   * - Lock() 时需要从 block_ 获取对象指针
   * - 对象删除后置为 nullptr，避免悬空指针
   */
  RefCounted* object_;

  /**
   * @brief 构造控制块
   *
   * @param[in] p_object 被管理的对象指针
   *
   * 初始化：strong_count=0, weak_count=1（隐式弱引用）
   * MakeRef 会随后设置 strong_count=1。
   *
   * 为什么 weak_count 初始为 1？
   * - 保证控制块在最后一个 Ref 释放前不被删除
   * - 这个"1"是隐式弱引用，由 MakeRef 创建的 Ref 持有
   * - 当最后一个 Ref 析构时，会释放这个隐式弱引用
   */
  RefCountBlock(RefCounted* p_object)
      : strong_count_(0), weak_count_(1), object_(p_object) {}

  ~RefCountBlock() {}

  /**
   * @brief 递增强引用计数
   *
   * 由 Ref<T> 拷贝构造/赋值时调用。
   *
   * @return 递增后的强引用计数
   */
  uint32 AddStrongRef() {
    return strong_count_.Increment();
  }

  /**
   * @brief 释放强引用
   *
   * 由 Ref<T> 析构/Reset 时调用。
   *
   * @return 递减后的强引用计数
   *
   * 如果返回 0，表示这是最后一个强引用，应该删除对象。
   * 注意：对象删除后，控制块仍然存活（因为 weak_count > 0）。
   */
  uint32 ReleaseStrongRef() {
    return strong_count_.Decrement();
  }

  /**
   * @brief 递增弱引用计数
   *
   * 由 WeakRef<T> 拷贝构造/赋值时调用。
   */
  void AddWeakRef() {
    weak_count_.Increment();
  }

  /**
   * @brief 释放弱引用
   *
   * 由 WeakRef<T> 析构/Reset 时调用。
   *
   * @return 递减后的弱引用计数
   *
   * 如果返回 0，表示这是最后一个弱引用，应该删除控制块。
   * 注意：此时对象可能早已被删除（strong_count 已经是 0）。
   */
  uint32 ReleaseWeakRef() {
    return weak_count_.Decrement();
  }

  /**
   * @brief 尝试原子递增强引用计数（用于 WeakRef::Lock）
   *
   * CAS 循环：仅当 strong_count > 0 时递增。
   *
   * @return true  成功递增，对象还活着
   * @return false strong_count 已经是 0，对象已被删除
   *
   * @par 线程安全原理
   *
   * CAS（Compare-And-Swap）同时完成"检查"和"递增"，无竞态窗口：
   *
   * 1. 读取 strong_count 的当前值
   * 2. 如果 > 0，尝试 CAS(strong_count, old, old+1)
   * 3. CAS 成功：strong_count 被递增，对象确定还活着
   * 4. CAS 失败：strong_count 已被其他线程修改，重试
   * 5. strong_count == 0：对象已被删除，返回 false
   *
   * 为什么不会有竞态？
   * - fetch_sub 和 CAS 都是原子操作，不会交错
   * - 只有一个线程能观察到 strong_count 从 1→0
   * - 如果 CAS 成功，strong_count 被递增，fetch_sub 不可能观察到 0
   * - 如果 fetch_sub 观察到 0，CAS 不可能成功
   */
  bool TryAddStrongRef() {
    uint32 old_count = strong_count_.Get();
    while (old_count > 0) {
      if (strong_count_.CompareExchange(old_count, old_count + 1)) {
        return true;
      }
    }
    return false;
  }
};

// ═══════════════════════════════════════════════════════════════════════
// RefCounted（纯标记基类）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 引用计数基类（纯标记）
 *
 * 不持有任何引用计数相关成员。引用计数由外部 RefCountBlock 管理。
 *
 * @par 为什么需要这个基类？
 *
 * 1. **虚析构函数**：RefCountBlock 持有 RefCounted*，通过它删除派生类对象时，
 *    虚析构保证正确调用派生类析构。没有虚析构，通过基类指针删除派生类对象
 *    是未定义行为。
 *
 * 2. **类型约束**：Ref<T> 和 WeakRef<T> 通过 static_assert 确保 T 继承自
 *    RefCounted，防止对非引用计数类型误用。
 *
 * 3. **禁止拷贝**：ARHUD_DISABLE_COPY_MOVE 确保派生类不会被意外按值拷贝，
 *    拷贝应该通过 Ref<T> 进行。
 *
 * @par 为什么不把计数放在这里？
 *
 * 如果计数在对象内部，对象删除后计数也被销毁。WeakRef 无法再访问对象来检查
 * 计数，导致悬空指针问题。外部控制块解决了这个问题。
 *
 * @par 内存开销
 *
 * 每个继承自 RefCounted 的对象多 8 字节（虚表指针）。对于资源对象（Texture、
 * Shader 等），这个开销可忽略。
 *
 * @par 用法
 *
 * @code
 * class MyResource : public RefCounted {
 *     int data_;
 * public:
 *     MyResource(int data) : data_(data) {}
 *     void DoSomething() { ... }
 * };
 *
 * Ref<MyResource> res = MakeRef<MyResource>(42);
 * res->DoSomething();
 * @endcode
 */
class RefCounted {
  ARHUD_DISABLE_COPY_MOVE(RefCounted);

 protected:
  /**
   * @brief 默认构造
   *
   * protected，确保只能通过派生类构造。
   */
  RefCounted() = default;

  /**
   * @brief 虚析构
   *
   * 关键：保证通过 RefCounted* 删除派生类对象时，正确调用派生类析构。
   *
   * 场景：
   * @code
   * RefCounted* ptr = new MyResource(42);
   * delete ptr;  // 如果没有 virtual，只调用 ~RefCounted()，MyResource
   * 的成员泄漏
   * @endcode
   */
  virtual ~RefCounted() = default;
};

}  // namespace arhud

#endif  // ARHUD_CORE_TEMPLATE_REF_COUNTED_H_
