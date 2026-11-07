/**
 * @file local_vector.h
 * @brief 无写时复制的动态数组容器
 *
 * 参考 Godot 4.6 LocalVector 设计，去除 COW 语义，直接持有数据指针。
 * 渲染系统内部热路径大量使用（绘制命令、顶点数组、资源列表等）。
 *
 * 与 std::vector 的关键区别：
 *   - 使用 arhud::memory 全局分配器（非 std::allocator）
 *   - 索引类型可配置（默认 uint32，节省内存）
 *   - 提供 remove_at_unordered（O(1) 删除，不保序）
 *   - 提供 resize_uninitialized（避免多余默认构造）
 *   - 不使用异常，错误通过 ARHUD_ASSERT 处理
 *
 * 增长策略：
 *   - 默认模式：1.5 倍增长（接近黄金比例，减少 realloc 次数）
 *   - tight 模式：严格按需增长（节省内存，适合只读后不再增长的场景）
 *
 * @author yameng.he
 * @version 1.1
 * @date 2026-08-29
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#ifndef ARHUD_CORE_TEMPLATE_LOCAL_VECTOR_H_
#define ARHUD_CORE_TEMPLATE_LOCAL_VECTOR_H_

#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <utility>

#include "os/memory.h"
#include "typedefs.h"

namespace arhud {

/**
 * @brief 无写时复制的动态数组
 *
 * @par 内存模型
 *   data_/size_/capacity_ 三成员直管裸缓冲，所有分配经
 *   arhud::memory 全局分配器（Debug 带追踪头与统计）。
 *   元素用 placement-new 原地构造、显式析构 —— 不依赖
 *   数组 new/delete。
 *
 * @par 与 std::vector 的关键差异
 *   - 分配器：memory::Alloc（引擎统一追踪），非 std::allocator
 *   - 索引类型：IndexType 可配置（默认 uint32，大容器可省内存）
 *   - RemoveAtUnordered：O(1) 删除不保序（std::vector 无对应）
 *   - ResizeUninitialized：跳过默认构造（std::vector 无对应）
 *   - 错误处理：ARHUD_ASSERT，无异常无 at()
 *   - 迭代器命名 PascalCase（Begin/End），非 STL 兼容类型
 *
 * @par 迭代器/指针失效规则（与 std::vector 相同）
 *   - 扩容（Reserve/扩容触发的 PushBack/Insert）：全部失效
 *   - RemoveAt/Insert：操作点之后失效
 *   - RemoveAtUnordered：被删位与末位失效
 *   - Clear/PopBack：仅末元素失效（Data() 指针仍有效）
 *
 * @par 增长策略
 *   - 默认（kTight=false）：1.5 倍增长，均摊 O(1) 追加
 *   - kTight=true：精确按需分配，见 TightLocalVector
 *
 * @tparam T         元素类型
 * @tparam IndexType 索引/大小类型（默认 uint32，须为无符号）
 * @tparam kTight    是否严格按需增长（false = 1.5 倍扩容）
 */
template <typename T, typename IndexType = uint32, bool kTight = false>
class LocalVector {
  static_assert(std::is_unsigned_v<IndexType>, "IndexType must be unsigned");

 public:
  /**
   * @brief 正向随机访问迭代器（非 const 版本）
   *
   * 裸指针包装 —— 不持有容器引用，不做过期检测
   * （容器扩容会使所有已持有的迭代器失效，与 std::vector 相同）。
   * 支持前缀 ++/-- 与解引用；不含后缀自增/算术运算
   * （项目内仅用范围 for 与逐个遍历，保持最小接口）。
   *
   * @note 命名遵循项目 PascalCase（Begin/End），非 STL 小写 ——
   *       刻意与 std 容器区分，防止误当作 STL 兼容类型传给
   *       泛型算法（接口子集不满足 Iterator 概念）。
   */
  class Iterator {
   public:
    /** @brief 解引用：返回指向的元素引用 */
    ARHUD_ALWAYS_INLINE T& operator*() const {
      return *ptr_;
    }
    /** @brief 成员访问：返回指向的元素指针 */
    ARHUD_ALWAYS_INLINE T* operator->() const {
      return ptr_;
    }
    /** @brief 前缀自增（移动到下一个元素），返回自引用 */
    ARHUD_ALWAYS_INLINE Iterator& operator++() {
      ++ptr_;
      return *this;
    }
    /** @brief 前缀自减（移动到上一个元素），返回自引用 */
    ARHUD_ALWAYS_INLINE Iterator& operator--() {
      --ptr_;
      return *this;
    }
    /** @brief 相等比较（底层指针相等） */
    ARHUD_ALWAYS_INLINE bool operator==(const Iterator& p_other) const {
      return ptr_ == p_other.ptr_;
    }
    /** @brief 不等比较 */
    ARHUD_ALWAYS_INLINE bool operator!=(const Iterator& p_other) const {
      return ptr_ != p_other.ptr_;
    }

    /** @brief 构造空迭代器（不可解引用，仅作哨兵） */
    Iterator() = default;
    /**
     * @brief 从裸指针构造
     * @param[in] p_ptr 指向的元素位置
     */
    explicit Iterator(T* p_ptr) : ptr_(p_ptr) {}
    /** @brief 拷贝构造（默认） */
    Iterator(const Iterator& p_other) = default;

   private:
    T* ptr_ = nullptr;  ///< 当前指向的元素
  };

  /**
   * @brief 只读正向迭代器（const 版本）
   *
   * 与 Iterator 同构，但解引用只暴露 const T&/const T* ——
   * 通过 const 容器（Begin()/End() const 重载）遍历时获得。
   */
  class ConstIterator {
   public:
    /** @brief 解引用：返回指向元素的只读引用 */
    ARHUD_ALWAYS_INLINE const T& operator*() const {
      return *ptr_;
    }
    /** @brief 成员访问：返回只读指针 */
    ARHUD_ALWAYS_INLINE const T* operator->() const {
      return *ptr_;
    }
    /** @brief 前缀自增 */
    ARHUD_ALWAYS_INLINE ConstIterator& operator++() {
      ++ptr_;
      return *this;
    }
    /** @brief 前缀自减 */
    ARHUD_ALWAYS_INLINE ConstIterator& operator--() {
      --ptr_;
      return *this;
    }
    /** @brief 相等比较 */
    ARHUD_ALWAYS_INLINE bool operator==(const ConstIterator& p_other) const {
      return ptr_ == p_other.ptr_;
    }
    /** @brief 不等比较 */
    ARHUD_ALWAYS_INLINE bool operator!=(const ConstIterator& p_other) const {
      return ptr_ != p_other.ptr_;
    }

    /** @brief 构造空迭代器 */
    ConstIterator() = default;
    /**
     * @brief 从裸指针构造
     * @param[in] p_ptr 指向的元素位置
     */
    explicit ConstIterator(const T* p_ptr) : ptr_(p_ptr) {}
    /** @brief 拷贝构造（默认） */
    ConstIterator(const ConstIterator& p_other) = default;

   private:
    const T* ptr_ = nullptr;  ///< 当前指向的元素（只读）
  };

  // ═══════════════════════════════════════════════════════════════
  // 构造 / 析构 / 赋值
  // ═══════════════════════════════════════════════════════════════

  /** @brief 默认构造：空容器（零分配，首次 PushBack 时才分配） */
  LocalVector() = default;

  /**
   * @brief 初始化列表构造
   *
   * @param[in] p_init 元素列表
   *
   * @note 单次 Reserve 精确预留，无中间扩容。
   */
  LocalVector(std::initializer_list<T> p_init) {
    Reserve(static_cast<IndexType>(p_init.size()));
    for (const T& elem : p_init) {
      PushBack(elem);
    }
  }

  /**
   * @brief 拷贝构造
   *
   * 元素级拷贝（拷贝赋值算子，非 memcpy）—— 保留源容器的 size
   * 但不保留其 capacity（按 size 精确分配，走 kTight 式一次性预留）。
   * 平凡可拷贝类型的逐元素循环由编译器优化为 memcpy。
   *
   * @param[in] p_other 源容器
   */
  LocalVector(const LocalVector& p_other) {
    Resize(p_other.size_);
    for (IndexType i = 0; i < size_; ++i) {
      data_[i] = p_other.data_[i];
    }
  }

  /**
   * @brief 移动构造（O(1)，窃取三指针）
   *
   * 接管 p_other 的 data_/size_/capacity_，源容器置为有效空态
   * （可析构、可继续使用）。
   *
   * @param[in] p_other 源容器（移动后变为空）
   */
  LocalVector(LocalVector&& p_other) noexcept
      : data_(p_other.data_),
        size_(p_other.size_),
        capacity_(p_other.capacity_) {
    p_other.data_ = nullptr;
    p_other.size_ = 0;
    p_other.capacity_ = 0;
  }

  /**
   * @brief 析构：析构全部元素并释放缓冲
   *
   * 等效 Reset()；平凡可析构类型跳过析构循环（Clear 内
   * if constexpr 分派）。
   */
  ~LocalVector() {
    Reset();
  }

  /**
   * @brief 拷贝赋值
   *
   * 自赋值检查后复用已有缓冲（Resize 按需扩容，容量够时
   * 零分配）。size 缩小时多出的元素被析构。
   *
   * @param[in] p_other 源容器
   * @return 自引用
   */
  LocalVector& operator=(const LocalVector& p_other) {
    if (this != &p_other) {
      Resize(p_other.size_);
      for (IndexType i = 0; i < size_; ++i) {
        data_[i] = p_other.data_[i];
      }
    }
    return *this;
  }

  /**
   * @brief 移动赋值（O(1)）
   *
   * 先释放自身缓冲（Clear 析构元素 + Free），再窃取 p_other
   * 三指针，源置空。自赋值检查保证不 Free 自身后悬垂。
   *
   * @param[in] p_other 源容器（移动后变为空）
   * @return 自引用
   */
  LocalVector& operator=(LocalVector&& p_other) noexcept {
    if (this != &p_other) {
      Clear();
      if (data_ != nullptr) {
        memory::Free(data_);
      }

      data_ = p_other.data_;
      size_ = p_other.size_;
      capacity_ = p_other.capacity_;

      p_other.data_ = nullptr;
      p_other.size_ = 0;
      p_other.capacity_ = 0;
    }
    return *this;
  }

  // ═══════════════════════════════════════════════════════════════
  // 元素访问
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 获取底层缓冲指针（可写）
   *
   * 空容器返回 nullptr。指针在下次扩容（Reserve/PushBack 触发
   * 重分配）后失效。
   *
   * @return 底层数组首地址，可用于 memcpy / GL 上传等批量操作
   */
  ARHUD_ALWAYS_INLINE T* Data() {
    return data_;
  }

  /** @brief 获取底层缓冲指针（只读版本） */
  ARHUD_ALWAYS_INLINE const T* Data() const {
    return data_;
  }

  /**
   * @brief 下标访问（可写）
   *
   * @param[in] p_index 元素索引（< Size()）
   * @return 元素引用
   *
   * @note Debug 断言越界；Release 为零开销裸访问（越界 = UB，
   *       与 std::vector::operator[] 一致，无 at() 变体）。
   */
  ARHUD_ALWAYS_INLINE T& operator[](IndexType p_index) {
    ARHUD_ASSERT(p_index < size_, "LocalVector index out of bounds");
    return data_[p_index];
  }

  /** @brief 下标访问（只读版本） */
  ARHUD_ALWAYS_INLINE const T& operator[](IndexType p_index) const {
    ARHUD_ASSERT(p_index < size_, "LocalVector index out of bounds");
    return data_[p_index];
  }

  /**
   * @brief 访问首元素（可写）
   * @return 首元素引用
   * @pre Size() > 0（Debug 断言）
   */
  ARHUD_ALWAYS_INLINE T& Front() {
    ARHUD_ASSERT(size_ > 0, "LocalVector::Front() on empty vector");
    return data_[0];
  }

  /** @brief 访问首元素（只读版本） @pre Size() > 0 */
  ARHUD_ALWAYS_INLINE const T& Front() const {
    ARHUD_ASSERT(size_ > 0, "LocalVector::Front() on empty vector");
    return data_[0];
  }

  /**
   * @brief 访问末尾元素（可写）
   * @return 末尾元素引用
   * @pre Size() > 0（Debug 断言）
   */
  ARHUD_ALWAYS_INLINE T& Back() {
    ARHUD_ASSERT(size_ > 0, "LocalVector::Back() on empty vector");
    return data_[size_ - 1];
  }

  /** @brief 访问末尾元素（只读版本） @pre Size() > 0 */
  ARHUD_ALWAYS_INLINE const T& Back() const {
    ARHUD_ASSERT(size_ > 0, "LocalVector::Back() on empty vector");
    return data_[size_ - 1];
  }

  // ═══════════════════════════════════════════════════════════════
  // 容量查询
  // ═══════════════════════════════════════════════════════════════

  /** @brief 当前元素数量 */
  ARHUD_ALWAYS_INLINE IndexType Size() const {
    return size_;
  }
  /** @brief 当前已分配容量（元素数，非字节数） */
  ARHUD_ALWAYS_INLINE IndexType Capacity() const {
    return capacity_;
  }
  /** @brief 是否为空（Size() == 0，未分配缓冲也为空） */
  ARHUD_ALWAYS_INLINE bool IsEmpty() const {
    return size_ == 0;
  }

  // ═══════════════════════════════════════════════════════════════
  // 迭代器
  // ═══════════════════════════════════════════════════════════════

  /** @brief 起始迭代器（指向首元素；空容器时 == End()） */
  ARHUD_ALWAYS_INLINE Iterator Begin() {
    return Iterator(data_);
  }
  /** @brief 结束迭代器（指向末尾元素之后，不可解引用） */
  ARHUD_ALWAYS_INLINE Iterator End() {
    return Iterator(data_ + size_);
  }
  /** @brief 起始迭代器（只读版本） */
  ARHUD_ALWAYS_INLINE ConstIterator Begin() const {
    return ConstIterator(data_);
  }
  /** @brief 结束迭代器（只读版本） */
  ARHUD_ALWAYS_INLINE ConstIterator End() const {
    return ConstIterator(data_ + size_);
  }

  // ═══════════════════════════════════════════════════════════════
  // 修改操作
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 尾部追加元素
   *
   * 按值收参再移动构造 —— 单次调用同时支持左值（拷入形参后
   * 移动）与右值（直接移动），避免双重载模板膨胀。
   * 容量满时先 Reserve(size+1)（kTight=false 走 1.5 倍增长）。
   * placement-new 原地构造，无临时对象。
   *
   * @param[in] p_elem 要追加的元素（按值传递，支持移动）
   *
   * @note 触发扩容时所有迭代器/指针失效；均摊 O(1)。
   */
  ARHUD_ALWAYS_INLINE void PushBack(T p_elem) {
    if (ARHUD_UNLIKELY(size_ == capacity_)) {
      Reserve(size_ + 1);
    }
    ::new (static_cast<void*>(&data_[size_])) T(std::move(p_elem));
    ++size_;
  }

  /**
   * @brief 尾部原地构造元素
   *
   * @tparam Args 构造参数类型
   * @param[in] p_args 传递给 T 构造函数的参数
   *
   * @return 新元素的引用
   */
  template <typename... Args>
  T& EmplaceBack(Args&&... p_args) {
    if (ARHUD_UNLIKELY(size_ == capacity_)) {
      Reserve(size_ + 1);
    }
    T* ptr = ::new (static_cast<void*>(&data_[size_]))
        T(std::forward<Args>(p_args)...);
    ++size_;
    return *ptr;
  }

  /**
   * @brief 移除末尾元素
   *
   * 显式调用析构（placement-new 的配对义务），size 减一。
   *
   * @pre Size() > 0（Debug 断言）
   */
  void PopBack() {
    ARHUD_ASSERT(size_ > 0, "LocalVector::PopBack() on empty vector");
    --size_;
    data_[size_].~T();
  }

  /**
   * @brief 按索引移除元素（保序，O(n)）
   *
   * 将 p_index 之后的所有元素前移一位（move 赋值，非拷贝），
   * 末位腾出的槽显式析构。
   *
   * @param[in] p_index 要移除的索引
   *
   * @note 移除点之后的迭代器/指针失效（元素物理移动）。
   */
  void RemoveAt(IndexType p_index) {
    ARHUD_ASSERT(p_index < size_,
                 "LocalVector::RemoveAt() index out of bounds");
    --size_;
    for (IndexType i = p_index; i < size_; ++i) {
      data_[i] = std::move(data_[i + 1]);
    }
    data_[size_].~T();
  }

  /**
   * @brief 按索引移除元素（不保序，O(1)）
   *
   * 将末尾元素移动到 p_index 位置，末位显式析构。适用于
   * 不依赖元素顺序的场景（如待处理任务队列、以交换删除
   * 为惯例的 free-list）。
   *
   * @param[in] p_index 要移除的索引
   *
   * @note p_index 恰为末位时退化为 PopBack；p_index 处与
   *       末位处的迭代器/指针失效。
   */
  void RemoveAtUnordered(IndexType p_index) {
    ARHUD_ASSERT(p_index < size_,
                 "LocalVector::RemoveAtUnordered() index out of bounds");
    --size_;
    if (p_index < size_) {
      data_[p_index] = std::move(data_[size_]);
    }
    data_[size_].~T();
  }

  /**
   * @brief 按值查找并移除第一个匹配元素（保序）
   *
   * Find（O(n)）+ RemoveAt（O(n)）的组合。
   *
   * @param[in] p_val 要移除的值（需 operator== 可用）
   *
   * @return true 成功移除
   * @retval false 未找到（容器不变）
   */
  bool Erase(const T& p_val) {
    int64 idx = Find(p_val);
    if (idx >= 0) {
      RemoveAt(static_cast<IndexType>(idx));
      return true;
    }
    return false;
  }

  /**
   * @brief 按值查找并移除第一个匹配元素（不保序）
   *
   * Find（O(n)）+ RemoveAtUnordered（O(1)）的组合 ——
   * 查找仍是线性，删除段省掉搬移。
   *
   * @param[in] p_val 要移除的值
   *
   * @return true 成功移除
   * @retval false 未找到（容器不变）
   */
  bool EraseUnordered(const T& p_val) {
    int64 idx = Find(p_val);
    if (idx >= 0) {
      RemoveAtUnordered(static_cast<IndexType>(idx));
      return true;
    }
    return false;
  }

  /**
   * @brief 在指定位置插入元素
   *
   * p_pos == Size() 时退化为 PushBack（零搬移）；否则
   * Reserve 后自后向前逐位 move 后移，腾出槽位。
   *
   * @param[in] p_pos 插入位置（0 ~ Size()，== Size() 即尾插）
   * @param[in] p_val 要插入的值
   *
   * @note O(n)；插入点及之后的迭代器/指针失效。
   */
  void Insert(IndexType p_pos, T p_val) {
    ARHUD_ASSERT(p_pos <= size_,
                 "LocalVector::Insert() position out of bounds");
    if (p_pos == size_) {
      PushBack(std::move(p_val));
    } else {
      Reserve(size_ + 1);
      ++size_;
      for (IndexType i = size_ - 1; i > p_pos; --i) {
        data_[i] = std::move(data_[i - 1]);
      }
      data_[p_pos] = std::move(p_val);
    }
  }

  /**
   * @brief 反转元素顺序
   *
   * 首尾对称交换（move），n/2 次交换，O(n)。
   */
  void Reverse() {
    for (IndexType i = 0; i < size_ / 2; ++i) {
      T tmp = std::move(data_[i]);
      data_[i] = std::move(data_[size_ - i - 1]);
      data_[size_ - i - 1] = std::move(tmp);
    }
  }

  /**
   * @brief 清空所有元素（不释放内存）
   *
   * 析构全部元素、size 归零；capacity 保留 —— 后续 PushBack
   * 直接复用缓冲，稳态零分配（渲染热路径逐帧复用的关键）。
   * 平凡可析构类型编译期跳过析构循环。
   *
   * @note Data() 指针在 Clear 后仍有效（缓冲未释放）。
   */
  void Clear() {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      for (IndexType i = 0; i < size_; ++i) {
        data_[i].~T();
      }
    }
    size_ = 0;
  }

  /**
   * @brief 清空元素并释放内存
   *
   * Clear() + Free(data_)，容器回到默认构造态（零分配空态）。
   * 析构函数走此路径；长生命周期容器想归还内存也用它。
   */
  void Reset() {
    Clear();
    if (data_ != nullptr) {
      memory::Free(data_);
      data_ = nullptr;
      capacity_ = 0;
    }
  }

  // ═══════════════════════════════════════════════════════════════
  // 容量管理
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 预留容量
   *
   * 若 p_new_capacity > 当前容量，重新分配内存：
   * 分配新缓冲 → 迁移元素（平凡可拷贝 memcpy / 否则逐个
   * move 构造 + 析构旧元素）→ 释放旧缓冲。
   * 容量足够时零开销直接返回。
   * 增长策略：
   *   - kTight=false：取 max(1.5x 当前容量, p_new_capacity)
   *   - kTight=true：直接使用 p_new_capacity
   *
   * @param[in] p_new_capacity 需要的最小容量
   *
   * @note 扩容后所有迭代器/指针/引用失效。
   * @note 内存来自 memory::Alloc（Debug 带追踪头）。
   */
  void Reserve(IndexType p_new_capacity) {
    if (p_new_capacity <= capacity_) {
      return;
    }

    IndexType new_cap;
    if constexpr (kTight) {
      new_cap = p_new_capacity;
    } else {
      new_cap = (capacity_ + (capacity_ + 1) / 2);
      if (new_cap < 2) {
        new_cap = 2;
      }
      if (p_new_capacity > new_cap) {
        new_cap = p_new_capacity;
      }
    }

    T* new_data = static_cast<T*>(memory::Alloc(sizeof(T) * new_cap));

    if (data_ != nullptr) {
      if constexpr (std::is_trivially_copyable_v<T>) {
        std::memcpy(static_cast<void*>(new_data),
                    static_cast<const void*>(data_), sizeof(T) * size_);
      } else {
        for (IndexType i = 0; i < size_; ++i) {
          ::new (static_cast<void*>(&new_data[i])) T(std::move(data_[i]));
          data_[i].~T();
        }
      }
      memory::Free(data_);
    }

    data_ = new_data;
    capacity_ = new_cap;
  }

  /**
   * @brief 调整大小（新元素默认构造）
   *
   * 扩张：新元素 placement-new 默认构造（平凡类型为未定义值
   * 而非零 —— 与 std::vector 一致；需要零值用 ResizeInitialized
   * 或自行赋值）。收缩：多出的元素析构，容量不变。
   *
   * @param[in] p_new_size 新大小
   */
  void Resize(IndexType p_new_size) {
    ResizeInternal<true>(p_new_size);
  }

  /**
   * @brief 调整大小（新元素零初始化）
   *
   * 对平凡类型用 memset 清零，对非平凡类型默认构造。
   *
   * @param[in] p_new_size 新大小
   *
   * @note 当前实现与 Resize 相同（默认构造对平凡类型即
   *       "未初始化"）—— 名称保留作为意图声明，语义与
   *       Godot LocalVector 对齐；如需真实清零可在此处
   *       补 memset 分支。
   */
  void ResizeInitialized(IndexType p_new_size) {
    ResizeInternal<true>(p_new_size);
  }

  /**
   * @brief 调整大小（新元素不初始化）
   *
   * 仅允许平凡可析构类型使用。新元素值未定义，
   * 调用者必须在读取前自行赋值。
   *
   * @param[in] p_new_size 新大小
   *
   * @pre T 必须是平凡可析构类型
   */
  void ResizeUninitialized(IndexType p_new_size) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "T must be trivially destructible for ResizeUninitialized");
    ResizeInternal<false>(p_new_size);
  }

  // ═══════════════════════════════════════════════════════════════
  // 查找
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 线性查找元素
   *
   * @param[in] p_val  要查找的值（需 operator== 可用）
   * @param[in] p_from 起始索引（默认 0，可 > 0 做多次查找）
   *
   * @return 元素索引，未找到返回 -1
   *
   * @note 返回 int64 而非 IndexType —— -1 哨兵无法用
   *       无符号索引表达（这也是 IndexType 必须无符号的
   *       原因之一）。
   */
  int64 Find(const T& p_val, IndexType p_from = 0) const {
    for (IndexType i = p_from; i < size_; ++i) {
      if (data_[i] == p_val) {
        return static_cast<int64>(i);
      }
    }
    return -1;
  }

  /**
   * @brief 检查是否包含指定元素
   *
   * Find 的布尔封装，O(n)。
   *
   * @param[in] p_val 要查找的值
   *
   * @return true 包含至少一个匹配元素
   */
  bool Has(const T& p_val) const {
    return Find(p_val) >= 0;
  }

 private:
  /**
   * @brief 内部调整大小实现
   *
   * 三分支：收缩（析构多余元素）/ 相等（零开销）/ 扩张
   * （Reserve 后按 kInit 决定是否默认构造新元素）。
   * 收缩路径与 kInit 无关 —— 被删元素只析构不初始化。
   *
   * @tparam kInit 是否初始化新元素（false = 留原始内存，
   *                仅 ResizeUninitialized 使用）
   * @param[in] p_new_size 新大小
   */
  template <bool kInit>
  void ResizeInternal(IndexType p_new_size) {
    if (p_new_size < size_) {
      if constexpr (!std::is_trivially_destructible_v<T>) {
        for (IndexType i = p_new_size; i < size_; ++i) {
          data_[i].~T();
        }
      }
      size_ = p_new_size;
    } else if (p_new_size > size_) {
      Reserve(p_new_size);
      if constexpr (kInit) {
        for (IndexType i = size_; i < p_new_size; ++i) {
          ::new (static_cast<void*>(&data_[i])) T();
        }
      }
      size_ = p_new_size;
    }
  }

  T* data_ = nullptr;   ///< 元素缓冲（memory::Alloc 分配；空容器为 nullptr）
  IndexType size_ = 0;  ///< 当前元素数量（<= capacity_）
  IndexType capacity_ = 0;  ///< 缓冲容量（元素数；0 表示未分配）
};

/**
 * @brief 严格按需增长的 LocalVector 别名（kTight=true）
 *
 * 适用于只追加一次后不再增长、或内存敏感的场景。
 * 增长策略见 Reserve：每次扩容精确分配到所需值，不做 1.5 倍
 * 预留 —— 省内存，代价是反复 PushBack 时 realloc 次数更多。
 *
 * @tparam T         元素类型
 * @tparam IndexType 索引类型
 */
template <typename T, typename IndexType = uint32>
using TightLocalVector = LocalVector<T, IndexType, true>;

}  // namespace arhud

#endif  // ARHUD_CORE_TEMPLATE_LOCAL_VECTOR_H_
