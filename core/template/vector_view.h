/**
 * @file vector_view.h
 * @brief 非拥有内存视图（类似 C++20 std::span）
 *
 * 参考 Godot 4.6 VectorView + Span 设计，为 ARHud 渲染引擎提供
 * 轻量级、非拥有的连续内存视图。
 *
 * 设计取舍：
 *   - 非拥有：不管理底层内存的生命周期，仅持有指针和长度
 *   - 只读视图：数据指针为 const T*，防止通过视图意外修改
 *   - 轻量：仅两个成员（指针 + 长度），可按值传递
 *   - 隐式构造：可从 LocalVector / FixedVector / C 数组 / 单个元素隐式构造
 *
 * ⚠️ 安全警告：
 *   - 不要存储 VectorView 作为长期持有的引用！
 *   - VectorView 的生命周期内，不要 resize / 释放底层容器
 *   - 违反以上规则会导致悬空指针和未定义行为
 *
 * 与 Godot VectorView / Span 的关键差异：
 *   - Godot VectorView 是只读 const 视图，Span 也是只读的
 *   - ARHud VectorView 合并了两者功能，提供 SubView / Find 等便捷方法
 *   - ARHud 命名遵循 PascalCase 约定
 *   - ARHud 增加 ReinterpretCast 用于类型安全的字节重解释
 *
 * 典型用途：
 * - RenderingDeviceDriver 接口传参（避免拷贝数组）
 * - 命令录制时传递 barrier / attachment 列表
 * - 函数参数中替代 const vector& 避免隐式堆分配
 * - 字节流解析（ReinterpretCast<uint8_t>）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-06
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <cstddef>
#include <cstring>
#include <type_traits>

#include "typedefs.h"

namespace arhud {

template <typename T, uint32 N>
class FixedVector;

template <typename T, typename IndexType, bool kTight>
class LocalVector;

// ═══════════════════════════════════════════════════════════════════════
// VectorView
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 非拥有只读内存视图
 *
 * 持有指向连续内存的指针和元素数量，不管理底层内存的生命周期。
 * 适用于函数参数传递，避免拷贝整个数组。
 *
 * ⚠️ 不要长期存储 VectorView，底层容器可能被释放或 resize。
 *
 * @tparam T 元素类型
 */
template <typename T> class VectorView {
public:
  // ═══════════════════════════════════════════════════════════════
  // 迭代器
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 只读正向迭代器
   */
  class ConstIterator {
  public:
    ARHUD_ALWAYS_INLINE const T &operator*() const { return *ptr_; }
    ARHUD_ALWAYS_INLINE const T *operator->() const { return ptr_; }
    ARHUD_ALWAYS_INLINE ConstIterator &operator++() {
      ++ptr_;
      return *this;
    }
    ARHUD_ALWAYS_INLINE ConstIterator &operator--() {
      --ptr_;
      return *this;
    }
    ARHUD_ALWAYS_INLINE bool operator==(const ConstIterator &p_other) const {
      return ptr_ == p_other.ptr_;
    }
    ARHUD_ALWAYS_INLINE bool operator!=(const ConstIterator &p_other) const {
      return ptr_ != p_other.ptr_;
    }

    ConstIterator() = default;
    explicit ConstIterator(const T *p_ptr) : ptr_(p_ptr) {}

  private:
    const T *ptr_ = nullptr;
  };

  // ═══════════════════════════════════════════════════════════════
  // 构造
  // ═══════════════════════════════════════════════════════════════

  VectorView() = default;

  /**
   * @brief 从指针和元素数量构造
   *
   * @param[in] p_ptr  数据指针（可为 nullptr）
   * @param[in] p_size 元素数量
   */
  VectorView(const T *p_ptr, uint32_t p_size) : ptr_(p_ptr), size_(p_size) {
    if (p_ptr == nullptr && p_size > 0) {
      size_ = 0;
    }
  }

  /**
   * @brief 从单个元素构造（便捷，用于传递单个对象）
   *
   * @param[in] p_val 元素引用
   */
  VectorView(const T &p_val) : ptr_(&p_val), size_(1) {}

  /**
   * @brief 从 C 数组构造
   *
   * @tparam N 数组大小（自动推导）
   * @param[in] p_array C 数组引用
   */
  template <uint32_t N>
  VectorView(const T (&p_array)[N]) : ptr_(p_array), size_(N) {}

  /**
   * @brief 从 LocalVector 构造
   */
  template <typename U>
  VectorView(
      const LocalVector<U> &p_vec,
      typename std::enable_if<std::is_same<U, T>::value>::type * = nullptr);

  /**
   * @brief 从 FixedVector 构造
   */
  template <typename U, uint32_t N>
  VectorView(
      const FixedVector<U, N> &p_vec,
      typename std::enable_if<std::is_same<U, T>::value>::type * = nullptr);

  // ═══════════════════════════════════════════════════════════════
  // 元素访问
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 获取底层数据指针
   */
  ARHUD_ALWAYS_INLINE const T *Data() const { return ptr_; }

  ARHUD_ALWAYS_INLINE const T &operator[](uint32_t p_index) const {
    ARHUD_ASSERT(p_index < size_, "VectorView index out of bounds");
    return ptr_[p_index];
  }

  /**
   * @brief 获取首元素
   */
  ARHUD_ALWAYS_INLINE const T &Front() const {
    ARHUD_ASSERT(size_ > 0, "VectorView::Front() on empty view");
    return ptr_[0];
  }

  /**
   * @brief 获取尾元素
   */
  ARHUD_ALWAYS_INLINE const T &Back() const {
    ARHUD_ASSERT(size_ > 0, "VectorView::Back() on empty view");
    return ptr_[size_ - 1];
  }

  // ═══════════════════════════════════════════════════════════════
  // 容量查询
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 获取元素数量
   */
  ARHUD_ALWAYS_INLINE uint32_t Size() const { return size_; }

  /**
   * @brief 获取字节大小
   */
  ARHUD_ALWAYS_INLINE uint32_t SizeInBytes() const {
    return size_ * static_cast<uint32_t>(sizeof(T));
  }

  /**
   * @brief 检查是否为空
   */
  ARHUD_ALWAYS_INLINE bool IsEmpty() const { return size_ == 0; }

  // ═══════════════════════════════════════════════════════════════
  // 迭代器
  // ═══════════════════════════════════════════════════════════════

  ARHUD_ALWAYS_INLINE ConstIterator Begin() const {
    return ConstIterator(ptr_);
  }
  ARHUD_ALWAYS_INLINE ConstIterator End() const {
    return ConstIterator(ptr_ + size_);
  }

  // ═══════════════════════════════════════════════════════════════
  // 子视图
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 创建子视图
   *
   * @param[in] p_offset 起始偏移
   * @param[in] p_count  元素数量
   *
   * @return 子视图
   */
  VectorView SubView(uint32_t p_offset, uint32_t p_count) const {
    ARHUD_ASSERT(p_offset + p_count <= size_,
                 "VectorView::SubView() out of bounds");
    return VectorView(ptr_ + p_offset, p_count);
  }

  /**
   * @brief 创建从偏移到末尾的子视图
   *
   * @param[in] p_offset 起始偏移
   *
   * @return 子视图
   */
  VectorView SubView(uint32_t p_offset) const {
    ARHUD_ASSERT(p_offset <= size_,
                 "VectorView::SubView() offset out of bounds");
    return VectorView(ptr_ + p_offset, size_ - p_offset);
  }

  // ═══════════════════════════════════════════════════════════════
  // 类型重解释
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 类型安全的字节重解释
   *
   * 将视图重解释为另一种类型的视图。
   * 要求：总字节数必须能被 sizeof(T1) 整除。
   *
   * 典型用途：将结构体数组视为 uint8_t 字节流。
   *
   * @tparam T1 目标类型
   *
   * @return 重解释后的视图
   */
  template <typename T1> VectorView<T1> ReinterpretCast() const {
    static_assert(sizeof(T1) > 0, "Target type must be complete");
    ARHUD_ASSERT((size_ * sizeof(T)) % sizeof(T1) == 0,
                 "VectorView::ReinterpretCast() size mismatch");
    return VectorView<T1>(
        reinterpret_cast<const T1 *>(ptr_),
        static_cast<uint32_t>((size_ * sizeof(T)) / sizeof(T1)));
  }

  // ═══════════════════════════════════════════════════════════════
  // 查找
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 线性查找元素
   *
   * @param[in] p_val  要查找的值
   * @param[in] p_from 起始索引
   *
   * @return 找到返回索引，未找到返回 -1
   */
  int64_t Find(const T &p_val, uint32_t p_from = 0) const {
    for (uint32_t i = p_from; i < size_; ++i) {
      if (ptr_[i] == p_val) {
        return static_cast<int64_t>(i);
      }
    }
    return -1;
  }

  /**
   * @brief 反向查找元素
   *
   * @param[in] p_val  要查找的值
   * @param[in] p_from 起始索引（从后往前）
   *
   * @return 找到返回索引，未找到返回 -1
   */
  int64_t RFind(const T &p_val, uint32_t p_from) const {
    ARHUD_ASSERT(p_from < size_, "VectorView::RFind() index out of bounds");
    for (int64_t i = static_cast<int64_t>(p_from); i >= 0; --i) {
      if (ptr_[static_cast<uint32_t>(i)] == p_val) {
        return i;
      }
    }
    return -1;
  }

  /**
   * @brief 反向查找元素（从末尾开始）
   *
   * @param[in] p_val 要查找的值
   *
   * @return 找到返回索引，未找到返回 -1
   */
  int64_t RFind(const T &p_val) const {
    if (size_ == 0) {
      return -1;
    }
    return RFind(p_val, size_ - 1);
  }

  /**
   * @brief 统计元素出现次数
   *
   * @param[in] p_val 要统计的值
   *
   * @return 出现次数
   */
  uint32_t Count(const T &p_val) const {
    uint32_t amount = 0;
    for (uint32_t i = 0; i < size_; ++i) {
      if (ptr_[i] == p_val) {
        ++amount;
      }
    }
    return amount;
  }

  // ═══════════════════════════════════════════════════════════════
  // 比较
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 比较两个视图的内容是否相等
   */
  bool Equals(const VectorView<T> &p_other) const {
    if (size_ != p_other.size_) {
      return false;
    }
    if constexpr (std::is_same_v<T, T> && std::is_fundamental_v<T>) {
      return std::memcmp(ptr_, p_other.ptr_, size_ * sizeof(T)) == 0;
    } else {
      for (uint32_t i = 0; i < size_; ++i) {
        if (ptr_[i] != p_other.ptr_[i]) {
          return false;
        }
      }
      return true;
    }
  }

private:
  const T *ptr_ = nullptr;
  uint32_t size_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════
// LocalVector / FixedVector 隐式构造（延迟实现，避免循环包含）
// ═══════════════════════════════════════════════════════════════════════

template <typename T>
template <typename U>
VectorView<T>::VectorView(
    const LocalVector<U> &p_vec,
    typename std::enable_if<std::is_same<U, T>::value>::type *)
    : ptr_(p_vec.Data()), size_(p_vec.Size()) {}

template <typename T>
template <typename U, uint32_t N>
VectorView<T>::VectorView(
    const FixedVector<U, N> &p_vec,
    typename std::enable_if<std::is_same<U, T>::value>::type *)
    : ptr_(p_vec.Data()), size_(p_vec.Size()) {}

} // namespace arhud