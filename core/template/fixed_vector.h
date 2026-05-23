/**
 * @file fixed_vector.h
 * @brief 栈上固定容量向量（零堆分配）
 *
 * 参考 Godot 4.6 FixedVector 设计，为渲染热路径提供零堆分配的动态数组。
 * 所有数据存储在对象内部（栈或嵌入其他结构体），容量在编译期确定。
 *
 * 设计取舍：
 *   - 栈分配：数据内嵌于对象，无堆分配开销，缓存友好
 *   - 编译期容量：模板参数 N 决定最大容量，越界写入触发断言
 *   - 无 COW：直接持有数据，无引用计数开销
 *   - 平凡类型优化：memcpy 加速拷贝，跳过析构循环
 *
 * 与 LocalVector 的关键区别：
 *   - FixedVector 在栈上分配，LocalVector 在堆上分配
 *   - FixedVector 容量编译期固定，LocalVector 容量运行时增长
 *   - FixedVector 适用于元素数量已知上限的小数组（< 256）
 *   - LocalVector 适用于元素数量不确定或较大的数组
 *
 * 典型用途：
 * - 每帧绘制命令列表（HUD 元素数量有限，通常 < 256）
 * - Uniform 绑定列表（每 shader 通常 < 16 个 binding）
 * - 顶点属性临时数组
 * - 渲染 Pass 的 attachment 列表
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-06
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <initializer_list>
#include <type_traits>
#include <utility>

#include "typedefs.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // FixedVector
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 栈上固定容量向量
     *
     * 数据内嵌于对象内部（栈或嵌入结构体），容量在编译期由模板参数 N 确定。
     * 适用于元素数量已知上限、需要零堆分配的渲染热路径。
     *
     * @tparam T 元素类型
     * @tparam N 最大容量（编译期常量）
     */
    template <typename T, uint32 N>
    class FixedVector
    {
        static_assert(N > 0, "FixedVector capacity N must be greater than 0");

    public:
        // ═══════════════════════════════════════════════════════════════
        // 迭代器
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 正向迭代器
         */
        class Iterator
        {
        public:
            ARHUD_ALWAYS_INLINE T &operator*() const { return *ptr_; }
            ARHUD_ALWAYS_INLINE T *operator->() const { return ptr_; }
            ARHUD_ALWAYS_INLINE Iterator &operator++()
            {
                ++ptr_;
                return *this;
            }
            ARHUD_ALWAYS_INLINE Iterator &operator--()
            {
                --ptr_;
                return *this;
            }
            ARHUD_ALWAYS_INLINE bool operator==(const Iterator &p_other) const { return ptr_ == p_other.ptr_; }
            ARHUD_ALWAYS_INLINE bool operator!=(const Iterator &p_other) const { return ptr_ != p_other.ptr_; }

            Iterator() = default;
            explicit Iterator(T *p_ptr) : ptr_(p_ptr) {}
            Iterator(const Iterator &p_other) = default;

        private:
            T *ptr_ = nullptr;
        };

        /**
         * @brief 只读正向迭代器
         */
        class ConstIterator
        {
        public:
            ARHUD_ALWAYS_INLINE const T &operator*() const { return *ptr_; }
            ARHUD_ALWAYS_INLINE const T *operator->() const { return ptr_; }
            ARHUD_ALWAYS_INLINE ConstIterator &operator++()
            {
                ++ptr_;
                return *this;
            }
            ARHUD_ALWAYS_INLINE ConstIterator &operator--()
            {
                --ptr_;
                return *this;
            }
            ARHUD_ALWAYS_INLINE bool operator==(const ConstIterator &p_other) const { return ptr_ == p_other.ptr_; }
            ARHUD_ALWAYS_INLINE bool operator!=(const ConstIterator &p_other) const { return ptr_ != p_other.ptr_; }

            ConstIterator() = default;
            explicit ConstIterator(const T *p_ptr) : ptr_(p_ptr) {}
            ConstIterator(const ConstIterator &p_other) = default;

        private:
            const T *ptr_ = nullptr;
        };

        // ═══════════════════════════════════════════════════════════════
        // 构造 / 析构 / 赋值
        // ═══════════════════════════════════════════════════════════════

        FixedVector() = default;

        FixedVector(std::initializer_list<T> p_init)
        {
            ARHUD_ASSERT(p_init.size() <= N, "FixedVector initializer_list exceeds capacity");
            for (const T &elem : p_init)
            {
                PushBack(elem);
            }
        }

        FixedVector(const FixedVector &p_other)
        {
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                std::memcpy(static_cast<void *>(Data()), static_cast<const void *>(p_other.Data()),
                            p_other.size_ * sizeof(T));
            }
            else
            {
                for (uint32 i = 0; i < p_other.size_; ++i)
                {
                    ::new (static_cast<void *>(&Data()[i])) T(p_other.Data()[i]);
                }
            }
            size_ = p_other.size_;
        }

        FixedVector(FixedVector &&p_other) noexcept
        {
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                std::memcpy(static_cast<void *>(Data()), static_cast<const void *>(p_other.Data()),
                            p_other.size_ * sizeof(T));
            }
            else
            {
                for (uint32 i = 0; i < p_other.size_; ++i)
                {
                    ::new (static_cast<void *>(&Data()[i])) T(std::move(p_other.Data()[i]));
                }
            }
            size_ = p_other.size_;
            p_other.Clear();
        }

        ~FixedVector() { Clear(); }

        FixedVector &operator=(const FixedVector &p_other)
        {
            if (this != &p_other)
            {
                Clear();
                if constexpr (std::is_trivially_copyable_v<T>)
                {
                    std::memcpy(static_cast<void *>(Data()), static_cast<const void *>(p_other.Data()),
                                p_other.size_ * sizeof(T));
                }
                else
                {
                    for (uint32 i = 0; i < p_other.size_; ++i)
                    {
                        ::new (static_cast<void *>(&Data()[i])) T(p_other.Data()[i]);
                    }
                }
                size_ = p_other.size_;
            }
            return *this;
        }

        FixedVector &operator=(FixedVector &&p_other) noexcept
        {
            if (this != &p_other)
            {
                Clear();
                if constexpr (std::is_trivially_copyable_v<T>)
                {
                    std::memcpy(static_cast<void *>(Data()), static_cast<const void *>(p_other.Data()),
                                p_other.size_ * sizeof(T));
                }
                else
                {
                    for (uint32 i = 0; i < p_other.size_; ++i)
                    {
                        ::new (static_cast<void *>(&Data()[i])) T(std::move(p_other.Data()[i]));
                    }
                }
                size_ = p_other.size_;
                p_other.Clear();
            }
            return *this;
        }

        // ═══════════════════════════════════════════════════════════════
        // 元素访问
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 获取底层数组指针
         */
        ARHUD_ALWAYS_INLINE T *Data() { return reinterpret_cast<T *>(storage_); }

        /**
         * @brief 获取底层数组指针（只读）
         */
        ARHUD_ALWAYS_INLINE const T *Data() const { return reinterpret_cast<const T *>(storage_); }

        ARHUD_ALWAYS_INLINE T &operator[](uint32 p_index)
        {
            ARHUD_ASSERT(p_index < size_, "FixedVector index out of bounds");
            return Data()[p_index];
        }

        ARHUD_ALWAYS_INLINE const T &operator[](uint32 p_index) const
        {
            ARHUD_ASSERT(p_index < size_, "FixedVector index out of bounds");
            return Data()[p_index];
        }

        /**
         * @brief 获取首元素
         */
        ARHUD_ALWAYS_INLINE T &Front()
        {
            ARHUD_ASSERT(size_ > 0, "FixedVector::Front() on empty vector");
            return Data()[0];
        }

        /**
         * @brief 获取首元素（只读）
         */
        ARHUD_ALWAYS_INLINE const T &Front() const
        {
            ARHUD_ASSERT(size_ > 0, "FixedVector::Front() on empty vector");
            return Data()[0];
        }

        /**
         * @brief 获取尾元素
         */
        ARHUD_ALWAYS_INLINE T &Back()
        {
            ARHUD_ASSERT(size_ > 0, "FixedVector::Back() on empty vector");
            return Data()[size_ - 1];
        }

        /**
         * @brief 获取尾元素（只读）
         */
        ARHUD_ALWAYS_INLINE const T &Back() const
        {
            ARHUD_ASSERT(size_ > 0, "FixedVector::Back() on empty vector");
            return Data()[size_ - 1];
        }

        // ═══════════════════════════════════════════════════════════════
        // 容量查询
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 获取当前元素数量
         */
        ARHUD_ALWAYS_INLINE uint32 Size() const { return size_; }

        /**
         * @brief 获取最大容量（编译期常量）
         */
        ARHUD_ALWAYS_INLINE constexpr uint32 Capacity() const { return N; }

        /**
         * @brief 检查是否为空
         */
        ARHUD_ALWAYS_INLINE bool IsEmpty() const { return size_ == 0; }

        /**
         * @brief 检查是否已满
         */
        ARHUD_ALWAYS_INLINE bool IsFull() const { return size_ == N; }

        // ═══════════════════════════════════════════════════════════════
        // 迭代器
        // ═══════════════════════════════════════════════════════════════

        ARHUD_ALWAYS_INLINE Iterator Begin() { return Iterator(Data()); }
        ARHUD_ALWAYS_INLINE Iterator End() { return Iterator(Data() + size_); }
        ARHUD_ALWAYS_INLINE ConstIterator Begin() const { return ConstIterator(Data()); }
        ARHUD_ALWAYS_INLINE ConstIterator End() const { return ConstIterator(Data() + size_); }

        // ═══════════════════════════════════════════════════════════════
        // 修改操作
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 尾部追加元素
         *
         * @param[in] p_val 要追加的值
         *
         * @return 新元素的引用
         *
         * @pre Size() < Capacity()
         */
        T &PushBack(const T &p_val)
        {
            ARHUD_ASSERT(size_ < N, "FixedVector::PushBack() exceeds capacity");
            T *ptr = ::new (static_cast<void *>(&Data()[size_])) T(p_val);
            ++size_;
            return *ptr;
        }

        /**
         * @brief 尾部追加元素（移动语义）
         *
         * @param[in] p_val 要追加的值（右值）
         *
         * @return 新元素的引用
         *
         * @pre Size() < Capacity()
         */
        T &PushBack(T &&p_val)
        {
            ARHUD_ASSERT(size_ < N, "FixedVector::PushBack() exceeds capacity");
            T *ptr = ::new (static_cast<void *>(&Data()[size_])) T(std::move(p_val));
            ++size_;
            return *ptr;
        }

        /**
         * @brief 尾部原地构造元素
         *
         * @tparam Args 构造参数类型
         * @param[in] p_args 传递给 T 构造函数的参数
         *
         * @return 新元素的引用
         *
         * @pre Size() < Capacity()
         */
        template <typename... Args>
        T &EmplaceBack(Args &&...p_args)
        {
            ARHUD_ASSERT(size_ < N, "FixedVector::EmplaceBack() exceeds capacity");
            T *ptr = ::new (static_cast<void *>(&Data()[size_])) T(std::forward<Args>(p_args)...);
            ++size_;
            return *ptr;
        }

        /**
         * @brief 移除末尾元素
         */
        void PopBack()
        {
            ARHUD_ASSERT(size_ > 0, "FixedVector::PopBack() on empty vector");
            --size_;
            Data()[size_].~T();
        }

        /**
         * @brief 按索引移除元素（保序，O(n)）
         *
         * @param[in] p_index 要移除的索引
         */
        void RemoveAt(uint32 p_index)
        {
            ARHUD_ASSERT(p_index < size_, "FixedVector::RemoveAt() index out of bounds");
            --size_;
            for (uint32 i = p_index; i < size_; ++i)
            {
                Data()[i] = std::move(Data()[i + 1]);
            }
            Data()[size_].~T();
        }

        /**
         * @brief 按索引移除元素（不保序，O(1)）
         *
         * 将末尾元素移动到 p_index 位置。
         *
         * @param[in] p_index 要移除的索引
         */
        void RemoveAtUnordered(uint32 p_index)
        {
            ARHUD_ASSERT(p_index < size_, "FixedVector::RemoveAtUnordered() index out of bounds");
            --size_;
            if (p_index < size_)
            {
                Data()[p_index] = std::move(Data()[size_]);
            }
            Data()[size_].~T();
        }

        /**
         * @brief 在指定位置插入元素
         *
         * @param[in] p_pos 插入位置
         * @param[in] p_val 要插入的值
         *
         * @pre Size() < Capacity()
         */
        void Insert(uint32 p_pos, const T &p_val)
        {
            ARHUD_ASSERT(p_pos <= size_, "FixedVector::Insert() position out of bounds");
            ARHUD_ASSERT(size_ < N, "FixedVector::Insert() exceeds capacity");

            if (p_pos == size_)
            {
                PushBack(p_val);
            }
            else
            {
                ::new (static_cast<void *>(&Data()[size_])) T(std::move(Data()[size_ - 1]));
                ++size_;
                for (uint32 i = size_ - 2; i > p_pos; --i)
                {
                    Data()[i] = std::move(Data()[i - 1]);
                }
                Data()[p_pos] = p_val;
            }
        }

        /**
         * @brief 清空所有元素
         */
        void Clear()
        {
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                for (uint32 i = 0; i < size_; ++i)
                {
                    Data()[i].~T();
                }
            }
            size_ = 0;
        }

        /**
         * @brief 调整大小（新元素默认构造）
         *
         * @param[in] p_new_size 新大小
         *
         * @return Error::kOK 成功，Error::kOutOfMemory 超出容量
         */
        Error Resize(uint32 p_new_size)
        {
            if (p_new_size > N)
            {
                return Error::kOutOfMemory;
            }

            if (p_new_size < size_)
            {
                if constexpr (!std::is_trivially_destructible_v<T>)
                {
                    for (uint32 i = p_new_size; i < size_; ++i)
                    {
                        Data()[i].~T();
                    }
                }
            }
            else if (p_new_size > size_)
            {
                for (uint32 i = size_; i < p_new_size; ++i)
                {
                    ::new (static_cast<void *>(&Data()[i])) T();
                }
            }

            size_ = p_new_size;
            return Error::kOK;
        }

        /**
         * @brief 调整大小（新元素不初始化）
         *
         * 仅允许平凡可析构类型使用。新元素值未定义，
         * 调用者必须在读取前自行赋值。
         *
         * @param[in] p_new_size 新大小
         *
         * @return Error::kOK 成功，Error::kOutOfMemory 超出容量
         *
         * @pre T 必须是平凡可析构类型
         */
        Error ResizeUninitialized(uint32 p_new_size)
        {
            static_assert(std::is_trivially_destructible_v<T>,
                          "T must be trivially destructible for ResizeUninitialized");

            if (p_new_size > N)
            {
                return Error::kOutOfMemory;
            }

            size_ = p_new_size;
            return Error::kOK;
        }

        /**
         * @brief 反转元素顺序
         */
        void Reverse()
        {
            for (uint32_t i = 0; i < size_ / 2; ++i)
            {
                T tmp = std::move(Data()[i]);
                Data()[i] = std::move(Data()[size_ - i - 1]);
                Data()[size_ - i - 1] = std::move(tmp);
            }
        }

        // ═══════════════════════════════════════════════════════════════
        // 查找
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 线性查找元素
         *
         * @param[in] p_val 要查找的值
         *
         * @return 元素索引，未找到返回 -1
         */
        int64 Find(const T &p_val) const
        {
            for (uint32 i = 0; i < size_; ++i)
            {
                if (Data()[i] == p_val)
                {
                    return static_cast<int64>(i);
                }
            }
            return -1;
        }

        /**
         * @brief 检查是否包含指定元素
         *
         * @param[in] p_val 要查找的值
         *
         * @return true 包含
         */
        bool Has(const T &p_val) const
        {
            return Find(p_val) >= 0;
        }

        /**
         * @brief 按值查找并移除第一个匹配元素（保序）
         *
         * @param[in] p_val 要移除的值
         *
         * @return true 成功移除
         */
        bool Erase(const T &p_val)
        {
            int64 idx = Find(p_val);
            if (idx >= 0)
            {
                RemoveAt(static_cast<uint32>(idx));
                return true;
            }
            return false;
        }

        /**
         * @brief 按值查找并移除第一个匹配元素（不保序）
         *
         * @param[in] p_val 要移除的值
         *
         * @return true 成功移除
         */
        bool EraseUnordered(const T &p_val)
        {
            int64 idx = Find(p_val);
            if (idx >= 0)
            {
                RemoveAtUnordered(static_cast<uint32>(idx));
                return true;
            }
            return false;
        }

    private:
        alignas(T) uint8 storage_[sizeof(T) * N];
        uint32 size_ = 0;
    };

} // namespace arhud
