/**
 * @file ref.h
 * @brief 引用计数智能指针（外部控制块模式）
 *
 * 采用外部控制块模式（参考 std::shared_ptr）：
 *   - Ref<T> 持有对象指针 + 控制块指针（16 字节）
 *   - 引用计数由独立的 RefCountBlock 管理
 *   - strong_count 降为 0 → 删除对象，保留控制块
 *   - weak_count 降为 0 → 删除控制块
 *
 * @par 为什么 Ref 需要同时持有 object_ 和 block_？
 *
 * 从功能角度，block_->object_ 已经存储了对象指针，Ref 只持有 block_ 就够了（8
 * 字节）。 但 Ref 同时持有 object_（16 字节）是为了性能优化：
 *
 * @code
 * // 如果 Ref 只持有 block_（8 字节）
 * T* operator->() const {
 *     return static_cast<T*>(block_->object_);  // 需要解引用 block_，可能
 * cache miss
 * }
 *
 * // 当前设计（16 字节）
 * T* operator->() const {
 *     return object_;  // 直接返回，大概率在寄存器或 L1 cache
 * }
 * @endcode
 *
 * 在渲染热路径中，operator->() 可能被调用几万次。16
 * 字节方案避免了额外的内存访问， 性能更好。对于资源对象（Texture、Shader
 * 等），Ref 的数量不会特别大，内存开销可接受。
 *
 * @par 与 std::shared_ptr 的区别
 *
 * std::shared_ptr 也采用外部控制块，但有以下不同：
 *   - std::shared_ptr 支持自定义删除器（类型擦除），Ref<T> 只支持 ARHUD_DELETE
 *   - std::shared_ptr 的控制块可以合并分配（make_shared），Ref<T> 总是分开分配
 *   - std::shared_ptr 支持数组版本（shared_ptr<T[]>），Ref<T> 只支持单对象
 *   - Ref<T> 更简单，开销更可预测，适合实时渲染引擎
 *
 * @par 内存布局
 *   @code
 *   Ref<T>（16 字节）
 *     ├── T *object_          ← 对象指针，用于快速访问
 *     └── RefCountBlock *block_  ← 控制块指针，用于引用计数管理
 *
 *   RefCountBlock（控制块，独立分配，24 字节）
 *     ├── atomic<uint32> strong_count  ← 强引用计数
 *     ├── atomic<uint32> weak_count    ← 弱引用计数（含隐式弱引用）
 *     └── RefCounted *object           ← 对象指针（供 WeakRef 使用）
 *
 *   对象（继承自 RefCounted）
 *     └── ... 业务数据 ...
 *   @endcode
 *
 * @par 线程安全
 *   - 引用计数操作使用原子操作（acq_rel 语义），无锁
 *   - 可以安全地在不同线程间传递 Ref<T>（拷贝/移动/析构）
 *   - 对象内容访问不是线程安全的，需要外部同步
 *
 * @par 生命周期
 *   - 拷贝时 strong_count++
 *   - 析构时 strong_count--
 *   - strong_count 降为 0 → delete 对象，block->object_ = nullptr
 *   - 然后释放隐式弱引用，weak_count--
 *   - weak_count 降为 0 → delete 控制块
 *
 * @par 用法
 *   @code
 *   // 创建
 *   Ref<MyResource> res = MakeRef<MyResource>(arg1, arg2);
 *
 *   // 拷贝（strong_count++）
 *   Ref<MyResource> copy = res;
 *
 *   // 移动（无引用计数操作）
 *   Ref<MyResource> moved = std::move(res);
 *
 *   // 访问
 *   res->DoSomething();
 *
 *   // 类型转换（Ref<Derived> → Ref<Base>）
 *   Ref<Base> base = derived;
 *
 *   // 释放
 *   res.Reset();
 *   @endcode
 *
 * @author yameng.he
 * @version 5.0
 * @date 2026-08-16
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#ifndef ARHUD_CORE_TEMPLATE_REF_H_
#define ARHUD_CORE_TEMPLATE_REF_H_

#include <type_traits>
#include <utility>

#include "os/memory.h"
#include "template/ref_counted.h"
#include "typedefs.h"

namespace arhud {

template <typename T>
class WeakRef;

template <typename T, typename... Args>
Ref<T> MakeRef(Args&&... p_args);

// ═══════════════════════════════════════════════════════════════════════
// Ref<T>
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 引用计数强引用智能指针
 *
 * 管理 RefCounted 派生类的生命周期。通过外部 RefCountBlock 管理引用计数。
 *
 * @tparam T 必须继承自 RefCounted（通过 static_assert 保证）
 *
 * @par 设计原理
 *
 * Ref<T> 同时持有对象指针和控制块指针，而不是只持有控制块指针：
 *   - object_ 用于快速访问对象（operator->、operator*、Get）
 *   - block_ 用于管理引用计数和 WeakRef 协作
 *
 * 这种设计的权衡：
 *   - 优点：operator->() 直接返回 object_，无需解引用 block_，性能更好
 *   - 缺点：每个 Ref<T> 多 8 字节（16 字节 vs 8 字节）
 *
 * 对于渲染引擎，operator->() 在热路径上被频繁调用，性能优先于内存。
 *
 * @par 线程安全
 *   - 引用计数操作使用原子操作，无锁
 *   - 可以安全地在不同线程间传递 Ref<T>（拷贝/移动/析构）
 *   - 对象内容访问不是线程安全的，需要外部同步
 *
 * @par 生命周期
 *   - 拷贝时 strong_count++
 *   - 析构时 strong_count--
 *   - strong_count 降为 0 → delete 对象，block->object_ = nullptr
 *   - 然后释放隐式弱引用，weak_count 降为 0 → delete 控制块
 */
template <typename T>
class Ref {
  static_assert(std::is_base_of<RefCounted, T>::value,
                "Ref<T> requires T to inherit from RefCounted");

  /**
   * @brief 对象指针
   *
   * 指向被管理的对象。用于快速访问（operator->、operator*、Get）。
   *
   * 为什么不从 block_->object_ 读取？
   * - 性能：直接返回 object_ 比解引用 block_ 更快
   * - 缓存局部性：object_ 在 Ref 内部，大概率在寄存器或 L1 cache
   *
   * 注意：这个指针和 block_->object_ 指向同一个对象，但类型不同。
   * object_ 是 T*（派生类），block_->object_ 是 RefCounted*（基类）。
   */
  T* object_ = nullptr;

  /**
   * @brief 控制块指针
   *
   * 指向管理引用计数的 RefCountBlock。
   *
   * 为什么需要这个指针？
   * - 引用计数操作（AddStrongRef、ReleaseStrongRef）通过 block_ 进行
   * - WeakRef 通过 GetBlock() 获取 block_，实现协作
   * - 控制块在所有 WeakRef 释放前保持存活
   */
  RefCountBlock* block_ = nullptr;

  /**
   * @brief 私有构造函数
   *
   * 用于 MakeRef 和 WeakRef::Lock() 构造 Ref<T>。
   * 不增加引用计数（调用者已经处理了引用计数）。
   *
   * @param[in] p_object 对象指针
   * @param[in] p_block  控制块指针
   *
   * 为什么是 private？
   * - 防止用户直接构造（应该使用 MakeRef）
   * - 确保引用计数正确管理
   * - 只有 MakeRef 和 WeakRef 需要这个构造函数
   */
  Ref(T* p_object, RefCountBlock* p_block)
      : object_(p_object), block_(p_block) {}

  template <typename U, typename... Args>
  friend Ref<U> MakeRef(Args&&... p_args);

  template <typename U>
  friend class WeakRef;

  /**
   * @brief 友元声明：允许 Ref<Base> 访问 Ref<Derived> 的私有成员
   *
   * 用于实现 Ref<Derived> → Ref<Base> 的类型转换。
   * 转换构造函数需要访问 Ref<Derived> 的 object_ 和 block_。
   */
  template <typename U>
  friend class Ref;

 public:
  /**
   * @brief 默认构造（空引用）
   *
   * 构造一个空的 Ref<T>，object_ 和 block_ 都是 nullptr。
   */
  Ref() = default;

  /**
   * @brief 拷贝构造
   *
   * 从另一个 Ref<T> 拷贝，strong_count++。
   *
   * @param[in] p_other 源引用
   *
   * 为什么先复制指针再递增计数？
   * - 如果 p_other 为空（block_ == nullptr），不递增计数
   * - 如果 p_other 有效，递增计数后两个 Ref 共享同一个对象和控制块
   *
   * 线程安全：AddStrongRef() 使用原子操作，可以安全地在多线程间拷贝。
   */
  Ref(const Ref& p_other) : object_(p_other.object_), block_(p_other.block_) {
    if (block_) {
      block_->AddStrongRef();
    }
  }

  /**
   * @brief 移动构造
   *
   * 从另一个 Ref<T> 移动，引用计数不变，源引用置空。
   *
   * @param[in] p_other 源引用（移动后置空）
   *
   * 为什么不修改引用计数？
   * - 移动是"转移所有权"，不是"共享所有权"
   * - 源引用不再持有对象，所以不需要递减计数
   * - 目标引用接管了源引用的所有权
   *
   * 性能：移动操作是 O(1)，无原子操作，比拷贝更快。
   */
  Ref(Ref&& p_other) noexcept
      : object_(p_other.object_), block_(p_other.block_) {
    p_other.object_ = nullptr;
    p_other.block_ = nullptr;
  }

  /**
   * @brief 派生类型转换构造（拷贝）
   *
   * 支持 Ref<Derived> → Ref<Base> 隐式转换。
   *
   * @tparam U 源类型，必须可转换为 T*（通过 std::enable_if 约束）
   * @param[in] p_other 源引用
   *
   * 实现原理：
   * - std::is_convertible<U*, T*> 确保 U* 可以隐式转换为 T*
   * - 复制 object_ 和 block_，然后递增 strong_count
   * - 由于 U* 可以转换为 T*，object_ 的类型转换是安全的
   *
   * 为什么需要 friend class Ref<U>？
   * - 需要访问 Ref<U> 的私有成员 object_ 和 block_
   * - 没有友元声明，无法访问其他模板实例的私有成员
   *
   * 用法：
   * @code
   * class Base : public RefCounted {};
   * class Derived : public Base {};
   *
   * Ref<Derived> derived = MakeRef<Derived>();
   * Ref<Base> base = derived;  // 隐式转换
   * @endcode
   */
  template <typename U, typename = typename std::enable_if<
                            std::is_convertible<U*, T*>::value>::type>
  Ref(const Ref<U>& p_other)
      : object_(p_other.object_), block_(p_other.block_) {
    if (block_) {
      block_->AddStrongRef();
    }
  }

  /**
   * @brief 派生类型转换构造（移动）
   *
   * 支持 Ref<Derived> → Ref<Base> 移动转换。
   *
   * @tparam U 源类型，必须可转换为 T*
   * @param[in] p_other 源引用（移动后置空）
   *
   * 与拷贝版本的区别：
   * - 不递增引用计数（移动语义）
   * - 源引用被置空
   */
  template <typename U, typename = typename std::enable_if<
                            std::is_convertible<U*, T*>::value>::type>
  Ref(Ref<U>&& p_other) noexcept
      : object_(p_other.object_), block_(p_other.block_) {
    p_other.object_ = nullptr;
    p_other.block_ = nullptr;
  }

  /**
   * @brief 析构函数
   *
   * 释放强引用，调用 Unref()。
   *
   * 如果这是最后一个强引用（strong_count 降为 0）：
   *   1. 删除对象
   *   2. 设置 block_->object_ = nullptr
   *   3. 释放隐式弱引用（weak_count--）
   *   4. 如果 weak_count 也降为 0，删除控制块
   */
  ~Ref() {
    Unref();
  }

  /**
   * @brief 拷贝赋值
   *
   * 从另一个 Ref<T> 拷贝赋值。
   *
   * @param[in] p_other 源引用
   * @return *this
   *
   * 实现步骤：
   *   1. 检查自赋值（this != &p_other）
   *   2. 释放当前引用（Unref）
   *   3. 复制指针
   *   4. 递增引用计数
   *
   * 为什么先 Unref 再 AddStrongRef？
   * - 如果先 AddStrongRef，再 Unref，可能导致对象被意外删除
   * - 先 Unref 确保当前引用被正确释放
   * - 再 AddStrongRef 递增新引用的计数
   */
  Ref& operator=(const Ref& p_other) {
    if (this != &p_other) {
      Unref();
      object_ = p_other.object_;
      block_ = p_other.block_;
      if (block_) {
        block_->AddStrongRef();
      }
    }
    return *this;
  }

  /**
   * @brief 移动赋值
   *
   * 从另一个 Ref<T> 移动赋值。
   *
   * @param[in] p_other 源引用（移动后置空）
   * @return *this
   *
   * 实现步骤：
   *   1. 检查自赋值
   *   2. 释放当前引用
   *   3. 窃取指针
   *   4. 源引用置空
   */
  Ref& operator=(Ref&& p_other) noexcept {
    if (this != &p_other) {
      Unref();
      object_ = p_other.object_;
      block_ = p_other.block_;
      p_other.object_ = nullptr;
      p_other.block_ = nullptr;
    }
    return *this;
  }

  /**
   * @brief 派生类型拷贝赋值
   *
   * 支持 Ref<Derived> → Ref<Base> 拷贝赋值。
   *
   * @tparam U 源类型，必须可转换为 T*
   * @param[in] p_other 源引用
   * @return *this
   */
  template <typename U, typename = typename std::enable_if<
                            std::is_convertible<U*, T*>::value>::type>
  Ref& operator=(const Ref<U>& p_other) {
    Unref();
    object_ = p_other.object_;
    block_ = p_other.block_;
    if (block_) {
      block_->AddStrongRef();
    }
    return *this;
  }

  /**
   * @brief 派生类型移动赋值
   *
   * 支持 Ref<Derived> → Ref<Base> 移动赋值。
   *
   * @tparam U 源类型，必须可转换为 T*
   * @param[in] p_other 源引用（移动后置空）
   * @return *this
   */
  template <typename U, typename = typename std::enable_if<
                            std::is_convertible<U*, T*>::value>::type>
  Ref& operator=(Ref<U>&& p_other) noexcept {
    Unref();
    object_ = p_other.object_;
    block_ = p_other.block_;
    p_other.object_ = nullptr;
    p_other.block_ = nullptr;
    return *this;
  }

  /**
   * @brief 成员访问操作符
   *
   * 返回对象指针，用于访问成员函数。
   *
   * @return T* 对象指针
   *
   * 断言：object_ 不为 nullptr。如果为空，触发断言失败。
   *
   * 性能：直接返回 object_，无额外开销。
   */
  T* operator->() const {
    ARHUD_ASSERT(object_ != nullptr);
    return object_;
  }

  /**
   * @brief 解引用操作符
   *
   * 返回对象引用，用于访问对象本身。
   *
   * @return T& 对象引用
   *
   * 断言：object_ 不为 nullptr。
   */
  T& operator*() const {
    ARHUD_ASSERT(object_ != nullptr);
    return *object_;
  }

  /**
   * @brief 获取对象指针
   *
   * 返回对象指针，可能为 nullptr。
   *
   * @return T* 对象指针（可能为空）
   *
   * 与 operator->() 的区别：
   * - Get() 不做断言检查，允许返回 nullptr
   * - 适用于需要检查有效性的场景
   */
  T* Get() const {
    return object_;
  }

  /**
   * @brief 获取对象指针（别名）
   *
   * 与 Get() 相同，提供 Ptr() 作为别名。
   *
   * @return T* 对象指针（可能为空）
   */
  T* Ptr() const {
    return object_;
  }

  /**
   * @brief 获取控制块指针
   *
   * 返回控制块指针，供 WeakRef 使用。
   *
   * @return RefCountBlock* 控制块指针（可能为空）
   *
   * 为什么需要这个接口？
   * - WeakRef 通过 GetBlock() 获取控制块
   * - WeakRef::Lock() 通过控制块访问对象
   */
  RefCountBlock* GetBlock() const {
    return block_;
  }

  /**
   * @brief 检查是否有效
   *
   * @return true  对象指针不为 nullptr
   * @return false 对象指针为 nullptr
   */
  bool IsValid() const {
    return object_ != nullptr;
  }

  /**
   * @brief 检查是否为空
   *
   * @return true  对象指针为 nullptr
   * @return false 对象指针不为 nullptr
   */
  bool IsNull() const {
    return object_ == nullptr;
  }

  /**
   * @brief 布尔转换操作符
   *
   * 允许在条件语句中使用 Ref<T>。
   *
   * @return true  对象指针不为 nullptr
   * @return false 对象指针为 nullptr
   *
   * 用法：
   * @code
   * Ref<MyResource> res = MakeRef<MyResource>();
   * if (res) {
   *     res->DoSomething();
   * }
   * @endcode
   */
  explicit operator bool() const {
    return object_ != nullptr;
  }

  /**
   * @brief 释放强引用
   *
   * 递减 strong_count。如果降为 0，执行清理流程。
   *
   * @par 清理流程
   *
   * 当 strong_count 降为 0 时（只有一个线程能观察到）：
   *
   * @code
   * 1. ARHUD_DELETE(object_)
   *    - 调用对象的虚析构函数
   *    - 释放对象内存
   *    - 此时对象已被销毁，但控制块仍然存活
   *
   * 2. block_->object_ = nullptr
   *    - 标记对象已被删除
   *    - WeakRef::Lock() 会检查这个标志
   *    - 避免悬空指针
   *
   * 3. block_->ReleaseWeakRef()
   *    - 释放隐式弱引用（weak_count--）
   *    - 如果 weak_count 也降为 0，删除控制块
   *    - 此时所有 WeakRef 都已释放
   * @endcode
   *
   * @par 线程安全
   *
   * ReleaseStrongRef() 和 ReleaseWeakRef() 使用原子操作。
   * fetch_sub 的原子性保证只有一个线程能观察到计数从 1→0。
   * 因此，对象删除和控制块删除都是唯一的，不会有竞态。
   *
   * @par 为什么先删除对象，再释放隐式弱引用？
   *
   * 顺序很重要：
   * - 先删除对象：确保对象在所有强引用释放后立即被销毁
   * - 再释放隐式弱引用：确保控制块在所有 WeakRef 释放前保持存活
   * - 如果反过来，控制块可能在对象删除前就被销毁，WeakRef 无法访问
   */
  void Unref() {
    if (block_) {
      if (block_->ReleaseStrongRef() == 0) {
        ARHUD_DELETE(object_);
        block_->object_ = nullptr;

        if (block_->ReleaseWeakRef() == 0) {
          ARHUD_DELETE(block_);
        }
      }
      object_ = nullptr;
      block_ = nullptr;
    }
  }

  /**
   * @brief 重置为空引用
   *
   * 等价于 Unref()，释放当前引用。
   *
   * 用法：
   * @code
   * Ref<MyResource> res = MakeRef<MyResource>();
   * res.Reset();  // 释放引用，res 变为空
   * @endcode
   */
  void Reset() {
    Unref();
  }

  /**
   * @brief 相等比较（Ref vs Ref）
   *
   * 比较两个 Ref 是否指向同一个对象。
   *
   * @param[in] p_other 另一个 Ref
   * @return true  指向同一个对象
   * @return false 指向不同对象（或其中一个为空）
   */
  bool operator==(const Ref& p_other) const {
    return object_ == p_other.object_;
  }

  /**
   * @brief 不等比较（Ref vs Ref）
   */
  bool operator!=(const Ref& p_other) const {
    return object_ != p_other.object_;
  }

  /**
   * @brief 相等比较（Ref vs 裸指针）
   *
   * 比较 Ref 是否指向指定的裸指针。
   *
   * @param[in] p_ptr 裸指针
   * @return true  指向同一个对象
   * @return false 指向不同对象
   */
  bool operator==(const T* p_ptr) const {
    return object_ == p_ptr;
  }

  /**
   * @brief 不等比较（Ref vs 裸指针）
   */
  bool operator!=(const T* p_ptr) const {
    return object_ != p_ptr;
  }
};

// ═══════════════════════════════════════════════════════════════════════
// 工厂函数
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 Ref<T>
 *
 * 分配对象和控制块，初始化引用计数。
 *
 * @tparam T 必须继承自 RefCounted
 * @tparam Args 构造函数参数类型
 * @param[in] p_args 构造函数参数（完美转发）
 * @return Ref<T> 智能指针
 *
 * @par 实现步骤
 *
 * @code
 * 1. ARHUD_NEW(T)(args...)
 *    - 分配对象内存
 *    - 调用构造函数
 *    - 返回 T* 指针
 *
 * 2. ARHUD_NEW(RefCountBlock)(obj)
 *    - 分配控制块内存
 *    - 初始化：strong_count=0, weak_count=1, object_=obj
 *    - weak_count 初始为 1（隐式弱引用）
 *
 * 3. block->strong_count_.Set(1)
 *    - 设置强引用计数为 1
 *    - 这个 Ref 持有唯一的强引用
 *
 * 4. return Ref<T>(obj, block)
 *    - 调用私有构造函数
 *    - 返回 Ref<T>
 * @endcode
 *
 * @par 为什么 weak_count 初始为 1？
 *
 * 这个"1"是隐式弱引用，保证控制块在最后一个 Ref 释放前不被删除。
 *
 * @code
 * Ref<T> r = MakeRef<T>();
 * // 此时：strong=1, weak=1（隐式弱引用）
 *
 * r.Reset();
 * // ReleaseStrongRef() → strong: 1→0，删除对象
 * // ReleaseWeakRef() → weak: 1→0，删除控制块
 * // 如果没有隐式弱引用，weak 初始为 0，控制块会被提前删除
 * @endcode
 *
 * @par 用法
 *   @code
 *   Ref<MyResource> res = MakeRef<MyResource>(arg1, arg2);
 *   res->DoSomething();
 *   @endcode
 *
 * @par 错误处理
 *
 * 如果对象分配失败（ARHUD_NEW 返回 nullptr），返回空的 Ref<T>。
 * 控制块不会被分配，避免内存泄漏。
 */
template <typename T, typename... Args>
Ref<T> MakeRef(Args&&... p_args) {
  T* obj = ARHUD_NEW(T)(std::forward<Args>(p_args)...);
  if (!obj) {
    return Ref<T>();
  }

  RefCountBlock* block = ARHUD_NEW(RefCountBlock)(obj);
  block->strong_count_.Set(1);

  return Ref<T>(obj, block);
}

}  // namespace arhud

#endif  // ARHUD_CORE_TEMPLATE_REF_H_
