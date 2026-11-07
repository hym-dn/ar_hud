/**
 * @file weak_ref.h
 * @brief 弱引用智能指针（外部控制块模式）
 *
 * 采用外部控制块模式（参考 std::weak_ptr）：
 *   - WeakRef<T> 仅持有控制块指针（8 字节）
 *   - 不增加 strong_count，不影响对象生命周期
 *   - 拷贝时 weak_count++，析构时 weak_count--
 *   - Lock() 通过 CAS 原子操作安全转换为 Ref<T>
 *
 * @par 为什么 WeakRef 只持有 block_（8 字节）？
 *
 * 从直觉上，WeakRef 可以像 Ref 一样同时持有 object_ 和 block_（16 字节）。
 * 但这样做会导致致命问题：
 *
 * @code
 * // 错误方案：WeakRef 持有 object_
 * template <typename T>
 * class WeakRef {
 *     T *object_;              // ← 危险！
 *     RefCountBlock *block_;
 * };
 *
 * // 场景：对象被删除后，WeakRef 的 object_ 变成悬空指针
 * Ref<T> r = MakeRef<T>();
 * WeakRef<T> w = r;
 * r.Reset();                   // 对象被删除
 *                              // 此时 w.object_ 是悬空指针！
 *                              // 即使只是持有这个指针（不访问），
 *                              // 那块内存可能已被重新分配给其他对象
 * @endcode
 *
 * 正确方案：WeakRef 只持有 block_，不持有 object_：
 *
 * @code
 * // 正确方案
 * template <typename T>
 * class WeakRef {
 *     RefCountBlock *block_;   // ← 只持有控制块
 * };
 *
 * Ref<T> r = MakeRef<T>();
 * WeakRef<T> w = r;
 * r.Reset();                   // 对象被删除
 *                              // block_->object_ = nullptr（已标记）
 *                              // block_ 仍然存活（因为 weak_count > 0）
 *                              // w.block_ 是有效指针，可以安全访问
 *
 * Ref<T> locked = w.Lock();    // CAS 检查 strong_count
 *                              // 发现 strong_count = 0，返回空 Ref
 *                              // 全程没有悬空指针
 * @endcode
 *
 * 关键：WeakRef 通过 block_ 间接访问对象，而不是直接持有对象指针。
 * 对象删除后，block_->object_ 被置为 nullptr，但 block_ 本身仍然存活。
 * Lock() 通过 TryAddStrongRef() 原子地检查对象是否还活着，避免悬空指针。
 *
 * @par 与 std::weak_ptr 的区别
 *
 * std::weak_ptr 也采用类似设计，但有以下不同：
 *   - std::weak_ptr 的控制块使用互斥锁保护（某些实现），WeakRef 完全无锁
 *   - std::weak_ptr 支持 expired() 方法，WeakRef 用 IsValid() 替代
 *   - std::weak_ptr 可以跨类型转换，WeakRef 目前只支持同类型
 *   - WeakRef 更简单，开销更可预测，适合实时渲染引擎
 *
 * @par 内存布局
 *   @code
 *   WeakRef<T>（8 字节）
 *     └── RefCountBlock *block_  ← 只持有控制块指针
 *
 *   RefCountBlock（控制块，独立分配，24 字节）
 *     ├── atomic<uint32> strong_count  ← Lock() 通过 CAS 检查
 *     ├── atomic<uint32> weak_count    ← WeakRef 持有弱引用
 *     └── RefCounted *object           ← 对象删除后置为 nullptr
 *   @endcode
 *
 * @par 线程安全
 *   - Lock() 使用 CAS 循环，无锁实现
 *   - 无竞态窗口，完全线程安全
 *   - 可以安全地在多线程间传递和使用
 *
 * @par 用法
 *   @code
 *   // 打破循环引用
 *   class Parent : public RefCounted {
 *       Ref<Child> child_;       // 强引用
 *   };
 *   class Child : public RefCounted {
 *       WeakRef<Parent> parent_; // 弱引用，避免循环
 *   };
 *
 *   // 安全访问
 *   if (Ref<Node> locked = weak.Lock()) {
 *       locked->DoSomething();
 *   }
 *   @endcode
 *
 * @author yameng.he
 * @version 5.0
 * @date 2026-08-16
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#ifndef ARHUD_CORE_TEMPLATE_WEAK_REF_H_
#define ARHUD_CORE_TEMPLATE_WEAK_REF_H_

#include "template/ref.h"
#include "typedefs.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// WeakRef<T>
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 弱引用智能指针
 *
 * 不增加 strong_count，用于打破循环引用。通过 Lock() 安全转换为 Ref<T>。
 *
 * @tparam T 必须继承自 RefCounted（通过 static_assert 保证）
 *
 * @par 设计原理
 *
 * WeakRef 只持有控制块指针（8 字节），不持有对象指针。这是关键设计：
 *
 * 1. **避免悬空指针**：对象删除后，WeakRef 不持有对象指针，不会有悬空指针问题
 * 2. **延长控制块生命周期**：WeakRef 持有弱引用，保证控制块在所有 WeakRef
 * 释放前存活
 * 3. **安全访问**：Lock() 通过 CAS 原子地检查对象是否还活着，避免竞态
 *
 * @par 为什么不提供 operator-> 和 operator*？
 *
 * WeakRef 不保证对象存活，不能直接访问对象。必须通过 Lock()
 * 获取强引用后才能访问。 如果提供 operator->，用户可能误以为对象一定存活，导致
 * use-after-free。
 *
 * @par 线程安全
 *   - Lock() 使用 CAS 循环，无锁实现
 *   - 无竞态窗口，完全线程安全
 *   - 可以安全地在多线程间传递和使用
 *
 * @par 循环引用场景
 *   @code
 *   class Parent : public RefCounted {
 *       Ref<Child> child_;       // 强引用
 *   };
 *
 *   class Child : public RefCounted {
 *       WeakRef<Parent> parent_; // 弱引用，避免循环
 *   };
 *
 *   // 创建对象
 *   Ref<Parent> parent = MakeRef<Parent>();
 *   Ref<Child> child = MakeRef<Child>();
 *
 *   // 建立关系
 *   parent->child_ = child;      // 强引用
 *   child->parent_ = parent;     // 弱引用
 *
 *   // 释放外部引用
 *   parent.Reset();              // strong: 1→0，删除 parent
 *                                // 但 child 还持有 parent 的弱引用
 *                                // parent 的控制块仍然存活
 *
 *   child.Reset();               // strong: 1→0，删除 child
 *                                // child 的弱引用也被释放
 *                                // parent 的控制块被删除
 *   @endcode
 *
 * @par Lock() 线程安全原理
 *   @code
 *   WeakRef::Lock() 使用 CAS 循环：
 *
 *   1. 读取 strong_count
 *   2. 如果 strong_count > 0，尝试 CAS(strong_count, strong_count + 1)
 *   3. CAS 成功：对象确定还活着，返回 Ref<T>
 *   4. CAS 失败：重试（strong_count 已被其他线程修改）
 *   5. strong_count = 0：对象已被销毁，返回空 Ref<T>
 *
 *   关键点：CAS 同时完成了"检查"和"递增"，无竞态窗口。
 *   @endcode
 */
template <typename T>
class WeakRef {
  static_assert(std::is_base_of<RefCounted, T>::value,
                "WeakRef<T> requires T to inherit from RefCounted");

  /**
   * @brief 控制块指针
   *
   * 指向管理引用计数的 RefCountBlock。
   *
   * 为什么只持有 block_，不持有 object_？
   * - 对象删除后，object_ 会变成悬空指针
   * - block_ 在所有 WeakRef 释放前保持存活
   * - Lock() 通过 block_ 间接访问对象，避免悬空指针
   *
   * 生命周期：
   * - 构造时从 Ref 或另一个 WeakRef 获取 block_
   * - 析构时释放弱引用（weak_count--）
   * - 如果 weak_count 降为 0，删除控制块
   */
  RefCountBlock* block_ = nullptr;

 public:
  /**
   * @brief 默认构造（空引用）
   *
   * 构造一个空的 WeakRef<T>，block_ 为 nullptr。
   */
  WeakRef() = default;

  /**
   * @brief 从 Ref<T> 构造
   *
   * 从强引用构造弱引用，weak_count++。
   *
   * @param[in] p_ref 强引用
   *
   * 实现步骤：
   *   1. 从 Ref 获取控制块指针（GetBlock()）
   *   2. 递增弱引用计数（AddWeakRef()）
   *
   * 为什么不递增 strong_count？
   * - WeakRef 是弱引用，不影响对象生命周期
   * - 只递增 weak_count，保证控制块存活
   *
   * 如果 p_ref 为空（block_ == nullptr），不递增计数。
   */
  WeakRef(const Ref<T>& p_ref) : block_(p_ref.GetBlock()) {
    if (block_) {
      block_->AddWeakRef();
    }
  }

  /**
   * @brief 拷贝构造
   *
   * 从另一个 WeakRef<T> 拷贝，weak_count++。
   *
   * @param[in] p_other 源弱引用
   *
   * 两个 WeakRef 共享同一个控制块，各自持有一个弱引用。
   * 控制块的 weak_count 递增，保证控制块在两个 WeakRef 都释放前存活。
   */
  WeakRef(const WeakRef& p_other) : block_(p_other.block_) {
    if (block_) {
      block_->AddWeakRef();
    }
  }

  /**
   * @brief 移动构造
   *
   * 从另一个 WeakRef<T> 移动，引用计数不变，源引用置空。
   *
   * @param[in] p_other 源弱引用（移动后置空）
   *
   * 为什么不修改引用计数？
   * - 移动是"转移所有权"，不是"共享所有权"
   * - 源 WeakRef 不再持有控制块，所以不需要递减计数
   * - 目标 WeakRef 接管了源 WeakRef 的弱引用
   *
   * 性能：移动操作是 O(1)，无原子操作，比拷贝更快。
   */
  WeakRef(WeakRef&& p_other) noexcept : block_(p_other.block_) {
    p_other.block_ = nullptr;
  }

  /**
   * @brief 析构函数
   *
   * 释放弱引用，调用 Release()。
   *
   * 如果这是最后一个弱引用（weak_count 降为 0），删除控制块。
   * 注意：此时对象可能早已被删除（strong_count 已经是 0）。
   */
  ~WeakRef() {
    Release();
  }

  /**
   * @brief 拷贝赋值
   *
   * 从另一个 WeakRef<T> 拷贝赋值。
   *
   * @param[in] p_other 源弱引用
   * @return *this
   *
   * 实现步骤：
   *   1. 检查自赋值（this != &p_other）
   *   2. 释放当前弱引用（Release）
   *   3. 复制控制块指针
   *   4. 递增弱引用计数
   *
   * 为什么先 Release 再 AddWeakRef？
   * - 先释放当前弱引用，确保计数正确
   * - 再递增新弱引用的计数
   */
  WeakRef& operator=(const WeakRef& p_other) {
    if (this != &p_other) {
      Release();
      block_ = p_other.block_;
      if (block_) {
        block_->AddWeakRef();
      }
    }
    return *this;
  }

  /**
   * @brief 移动赋值
   *
   * 从另一个 WeakRef<T> 移动赋值。
   *
   * @param[in] p_other 源弱引用（移动后置空）
   * @return *this
   *
   * 实现步骤：
   *   1. 检查自赋值
   *   2. 释放当前弱引用
   *   3. 窃取控制块指针
   *   4. 源引用置空
   */
  WeakRef& operator=(WeakRef&& p_other) noexcept {
    if (this != &p_other) {
      Release();
      block_ = p_other.block_;
      p_other.block_ = nullptr;
    }
    return *this;
  }

  /**
   * @brief 从 Ref<T> 赋值
   *
   * 从强引用赋值给弱引用。
   *
   * @param[in] p_ref 强引用
   * @return *this
   *
   * 实现步骤：
   *   1. 释放当前弱引用
   *   2. 从 Ref 获取控制块指针
   *   3. 递增弱引用计数
   */
  WeakRef& operator=(const Ref<T>& p_ref) {
    Release();
    block_ = p_ref.GetBlock();
    if (block_) {
      block_->AddWeakRef();
    }
    return *this;
  }

  /**
   * @brief 尝试获取强引用
   *
   * 使用 CAS 循环原子地检查并递增 strong_count。
   * 如果对象仍被强引用（strong_count > 0），返回有效的 Ref<T>。
   * 如果对象不再被强引用（strong_count = 0），返回空的 Ref<T>。
   *
   * @return Ref<T> 强引用（可能为空）
   *
   * @par 实现原理
   *
   * @code
   * Ref<T> Lock() const {
   *     if (!block_) {
   *         return Ref<T>();  // 空的 WeakRef
   *     }
   *
   *     if (block_->TryAddStrongRef()) {
   *         // CAS 成功：strong_count 被递增，对象确定还活着
   *         T *obj = static_cast<T *>(block_->object_);
   *         return Ref<T>(obj, block_);  // 返回强引用
   *     }
   *
   *     // CAS 失败：strong_count 已经是 0，对象已被删除
   *     return Ref<T>();
   * }
   * @endcode
   *
   * @par 为什么 static_cast 是安全的？
   *
   * block_->object_ 的类型是 RefCounted*（基类），需要转换为 T*（派生类）。
   *
   * 安全性保证：
   * - TryAddStrongRef() 成功意味着 strong_count > 0
   * - strong_count > 0 意味着至少有一个 Ref<T> 持有对象
   * - Ref<T> 持有对象意味着对象还没被删除
   * - 对象没被删除意味着 block_->object_ 是有效指针
   * - 由于类型约束（static_assert），T 必须继承自 RefCounted
   * - 因此 static_cast<T*> 是安全的
   *
   * @par 线程安全
   *
   * CAS（Compare-And-Swap）同时完成"检查"和"递增"，无竞态窗口：
   *
   * @code
   * // TryAddStrongRef() 的 CAS 循环
   * uint32 old_count = strong_count_.Get();
   * while (old_count > 0) {
   *     if (strong_count_.CompareExchange(old_count, old_count + 1)) {
   *         return true;  // CAS 成功
   *     }
   *     // CAS 失败，重试
   * }
   * return false;  // strong_count == 0
   * @endcode
   *
   * 为什么不会有竞态？
   * - fetch_sub 和 CAS 都是原子操作，不会交错
   * - 只有一个线程能观察到 strong_count 从 1→0
   * - 如果 CAS 成功，strong_count 被递增，fetch_sub 不可能观察到 0
   * - 如果 fetch_sub 观察到 0，CAS 不可能成功
   *
   * @par 用法
   *   @code
   *   if (Ref<Node> locked = weak.Lock()) {
   *       locked->DoSomething();
   *   }
   *   @endcode
   */
  Ref<T> Lock() const {
    if (!block_) {
      return Ref<T>();
    }

    if (block_->TryAddStrongRef()) {
      T* obj = static_cast<T*>(block_->object_);
      return Ref<T>(obj, block_);
    }

    return Ref<T>();
  }

  /**
   * @brief 检查对象是否可能仍存活
   *
   * @return true  控制块存在且 strong_count > 0
   * @return false 控制块为空或 strong_count == 0
   *
   * @warning 这是非原子检查，返回值可能立即过期。
   *
   * 为什么是非原子的？
   * - 检查 block_ != nullptr 和 strong_count > 0 是两个独立操作
   * - 在检查之间，其他线程可能删除对象
   * - 返回值可能立即失效
   *
   * 要安全访问对象，请使用 Lock()：
   * @code
   * // 错误：非原子检查
   * if (weak.IsValid()) {
   *     // 此时对象可能已被删除！
   * }
   *
   * // 正确：原子检查
   * if (Ref<T> locked = weak.Lock()) {
   *     // 对象确定还活着
   *     locked->DoSomething();
   * }
   * @endcode
   */
  bool IsValid() const {
    return block_ != nullptr && block_->strong_count_.Get() > 0;
  }

  /**
   * @brief 检查是否为空
   *
   * @return true  控制块为空（从未绑定或已 Reset）
   * @return false 控制块存在
   *
   * 注意：IsNull() 只检查 WeakRef 是否为空，不检查对象是否存活。
   * 即使 IsNull() 返回 false，对象也可能已被删除。
   */
  bool IsNull() const {
    return block_ == nullptr;
  }

  /**
   * @brief 重置为空引用
   *
   * 释放当前弱引用，WeakRef 变为空。
   *
   * 实现步骤：
   *   1. 释放弱引用（Release）
   *   2. 置空 block_
   *
   * 如果这是最后一个弱引用，控制块会被删除。
   */
  void Reset() {
    Release();
    block_ = nullptr;
  }

  /**
   * @brief 相等比较
   *
   * 比较两个 WeakRef 是否指向同一个控制块。
   *
   * @param[in] p_other 另一个 WeakRef
   * @return true  指向同一个控制块
   * @return false 指向不同控制块（或其中一个为空）
   *
   * 注意：比较的是控制块，不是对象。两个 WeakRef 指向同一个控制块，
   * 意味着它们观察的是同一个对象（或都观察到空）。
   */
  bool operator==(const WeakRef& p_other) const {
    return block_ == p_other.block_;
  }

  /**
   * @brief 不等比较
   */
  bool operator!=(const WeakRef& p_other) const {
    return block_ != p_other.block_;
  }

 private:
  /**
   * @brief 释放弱引用
   *
   * 递减 weak_count。如果降为 0，删除控制块。
   *
   * @par 清理流程
   *
   * 当 weak_count 降为 0 时（只有一个线程能观察到）：
   *
   * @code
   * 1. block_->ReleaseWeakRef()
   *    - 递减 weak_count
   *    - 如果返回 0，表示这是最后一个弱引用
   *
   * 2. ARHUD_DELETE(block_)
   *    - 删除控制块
   *    - 此时对象可能早已被删除（strong_count 已经是 0）
   *    - 控制块是最后被删除的
   * @endcode
   *
   * @par 线程安全
   *
   * ReleaseWeakRef() 使用原子操作。
   * fetch_sub 的原子性保证只有一个线程能观察到 weak_count 从 1→0。
   * 因此，控制块删除是唯一的，不会有竞态。
   *
   * @par 为什么 WeakRef 析构时不检查对象是否存活？
   *
   * WeakRef 只关心控制块，不关心对象。
   * - 对象删除时，Ref::Unref() 会设置 block_->object_ = nullptr
   * - 对象删除时，Ref::Unref() 会释放隐式弱引用（weak_count--）
   * - WeakRef 析构时，只需要释放自己的弱引用（weak_count--）
   * - 如果 weak_count 降为 0，说明所有 WeakRef 都已释放，可以删除控制块
   */
  void Release() {
    if (block_) {
      if (block_->ReleaseWeakRef() == 0) {
        ARHUD_DELETE(block_);
      }
      block_ = nullptr;
    }
  }
};

}  // namespace arhud

#endif  // ARHUD_CORE_TEMPLATE_WEAK_REF_H_
