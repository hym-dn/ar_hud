/**
 * @file local_vector.h
 * @brief 无写时复制的动态数组容器
 *
 * 参考 Godot 4.6 LocalVector 设计，去除 COW 语义，直接持有数据指针。
 * 渲染系统内部热路径大量使用（命令缓冲、顶点数组、资源列表等）。
 *
 * 与 std::vector 的关键区别：
 *   - 使用 arhud::memory 全局分配器（非 std::allocator）
 *   - 索引类型可配置（默认 uint32_t，节省内存）
 *   - 提供 remove_at_unordered（O(1) 删除，不保序）
 *   - 提供 resize_uninitialized（避免多余默认构造）
 *   - 不使用异常，错误通过 ARHUD_ASSERT 处理
 *
 * 增长策略：
 *   - 默认模式：1.5 倍增长（接近黄金比例，减少 realloc 次数）
 *   - tight 模式：严格按需增长（节省内存，适合只读后不再增长的场景）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-03
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <utility>

#include "os/memory.h"
#include "typedefs.h"

namespace arhud
{

    /**
     * @brief 无写时复制的动态数组
     *
     * @tparam T              元素类型
     * @tparam IndexType      索引/大小类型（默认 uint32_t）
     * @tparam kTight         是否严格按需增长（false = 1.5 倍扩容）
     */
    template <typename T, typename IndexType = uint32_t, bool kTight = false>
    class LocalVector
    {
        static_assert(std::is_unsigned_v<IndexType>, "IndexType must be unsigned");

    public:
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

        LocalVector() = default;

        LocalVector(std::initializer_list<T> p_init)
        {
            Reserve(static_cast<IndexType>(p_init.size()));
            for (const T &elem : p_init)
            {
                PushBack(elem);
            }
        }

        LocalVector(const LocalVector &p_other)
        {
            Resize(p_other.size_);
            for (IndexType i = 0; i < size_; ++i)
            {
                data_[i] = p_other.data_[i];
            }
        }

        LocalVector(LocalVector &&p_other) noexcept
            : data_(p_other.data_),
              size_(p_other.size_),
              capacity_(p_other.capacity_)
        {
            p_other.data_ = nullptr;
            p_other.size_ = 0;
            p_other.capacity_ = 0;
        }

        ~LocalVector() { Reset(); }

        LocalVector &operator=(const LocalVector &p_other)
        {
            if (this != &p_other)
            {
                Resize(p_other.size_);
                for (IndexType i = 0; i < size_; ++i)
                {
                    data_[i] = p_other.data_[i];
                }
            }
            return *this;
        }

        LocalVector &operator=(LocalVector &&p_other) noexcept
        {
            if (this != &p_other)
            {
                Clear();
                if (data_ != nullptr)
                {
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

        ARHUD_ALWAYS_INLINE T *Data() { return data_; }
        ARHUD_ALWAYS_INLINE const T *Data() const { return data_; }

        ARHUD_ALWAYS_INLINE T &operator[](IndexType p_index)
        {
            ARHUD_ASSERT(p_index < size_, "LocalVector index out of bounds");
            return data_[p_index];
        }

        ARHUD_ALWAYS_INLINE const T &operator[](IndexType p_index) const
        {
            ARHUD_ASSERT(p_index < size_, "LocalVector index out of bounds");
            return data_[p_index];
        }

        ARHUD_ALWAYS_INLINE T &Front()
        {
            ARHUD_ASSERT(size_ > 0, "LocalVector::Front() on empty vector");
            return data_[0];
        }

        ARHUD_ALWAYS_INLINE const T &Front() const
        {
            ARHUD_ASSERT(size_ > 0, "LocalVector::Front() on empty vector");
            return data_[0];
        }

        ARHUD_ALWAYS_INLINE T &Back()
        {
            ARHUD_ASSERT(size_ > 0, "LocalVector::Back() on empty vector");
            return data_[size_ - 1];
        }

        ARHUD_ALWAYS_INLINE const T &Back() const
        {
            ARHUD_ASSERT(size_ > 0, "LocalVector::Back() on empty vector");
            return data_[size_ - 1];
        }

        // ═══════════════════════════════════════════════════════════════
        // 容量查询
        // ═══════════════════════════════════════════════════════════════

        ARHUD_ALWAYS_INLINE IndexType Size() const { return size_; }
        ARHUD_ALWAYS_INLINE IndexType Capacity() const { return capacity_; }
        ARHUD_ALWAYS_INLINE bool IsEmpty() const { return size_ == 0; }

        // ═══════════════════════════════════════════════════════════════
        // 迭代器
        // ═══════════════════════════════════════════════════════════════

        ARHUD_ALWAYS_INLINE Iterator Begin() { return Iterator(data_); }
        ARHUD_ALWAYS_INLINE Iterator End() { return Iterator(data_ + size_); }
        ARHUD_ALWAYS_INLINE ConstIterator Begin() const { return ConstIterator(data_); }
        ARHUD_ALWAYS_INLINE ConstIterator End() const { return ConstIterator(data_ + size_); }

        // ═══════════════════════════════════════════════════════════════
        // 修改操作
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 尾部追加元素
         *
         * @param[in] p_elem 要追加的元素（按值传递，支持移动）
         */
        ARHUD_ALWAYS_INLINE void PushBack(T p_elem)
        {
            if (ARHUD_UNLIKELY(size_ == capacity_))
            {
                Reserve(size_ + 1);
            }
            ::new (static_cast<void *>(&data_[size_])) T(std::move(p_elem));
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
        T &EmplaceBack(Args &&...p_args)
        {
            if (ARHUD_UNLIKELY(size_ == capacity_))
            {
                Reserve(size_ + 1);
            }
            T *ptr = ::new (static_cast<void *>(&data_[size_])) T(std::forward<Args>(p_args)...);
            ++size_;
            return *ptr;
        }

        /**
         * @brief 移除末尾元素
         */
        void PopBack()
        {
            ARHUD_ASSERT(size_ > 0, "LocalVector::PopBack() on empty vector");
            --size_;
            data_[size_].~T();
        }

        /**
         * @brief 按索引移除元素（保序，O(n)）
         *
         * 将 p_index 之后的所有元素前移一位。
         *
         * @param[in] p_index 要移除的索引
         */
        void RemoveAt(IndexType p_index)
        {
            ARHUD_ASSERT(p_index < size_, "LocalVector::RemoveAt() index out of bounds");
            --size_;
            for (IndexType i = p_index; i < size_; ++i)
            {
                data_[i] = std::move(data_[i + 1]);
            }
            data_[size_].~T();
        }

        /**
         * @brief 按索引移除元素（不保序，O(1)）
         *
         * 将末尾元素移动到 p_index 位置。适用于不需要保序的场景。
         *
         * @param[in] p_index 要移除的索引
         */
        void RemoveAtUnordered(IndexType p_index)
        {
            ARHUD_ASSERT(p_index < size_, "LocalVector::RemoveAtUnordered() index out of bounds");
            --size_;
            if (p_index < size_)
            {
                data_[p_index] = std::move(data_[size_]);
            }
            data_[size_].~T();
        }

        /**
         * @brief 按值查找并移除第一个匹配元素（保序）
         *
         * @param[in] p_val 要移除的值
         *
         * @return true 成功移除
         * @retval false 未找到
         */
        bool Erase(const T &p_val)
        {
            int64_t idx = Find(p_val);
            if (idx >= 0)
            {
                RemoveAt(static_cast<IndexType>(idx));
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
         * @retval false 未找到
         */
        bool EraseUnordered(const T &p_val)
        {
            int64_t idx = Find(p_val);
            if (idx >= 0)
            {
                RemoveAtUnordered(static_cast<IndexType>(idx));
                return true;
            }
            return false;
        }

        /**
         * @brief 在指定位置插入元素
         *
         * @param[in] p_pos  插入位置
         * @param[in] p_val  要插入的值
         */
        void Insert(IndexType p_pos, T p_val)
        {
            ARHUD_ASSERT(p_pos <= size_, "LocalVector::Insert() position out of bounds");
            if (p_pos == size_)
            {
                PushBack(std::move(p_val));
            }
            else
            {
                Reserve(size_ + 1);
                ++size_;
                for (IndexType i = size_ - 1; i > p_pos; --i)
                {
                    data_[i] = std::move(data_[i - 1]);
                }
                data_[p_pos] = std::move(p_val);
            }
        }

        /**
         * @brief 反转元素顺序
         */
        void Reverse()
        {
            for (IndexType i = 0; i < size_ / 2; ++i)
            {
                T tmp = std::move(data_[i]);
                data_[i] = std::move(data_[size_ - i - 1]);
                data_[size_ - i - 1] = std::move(tmp);
            }
        }

        /**
         * @brief 清空所有元素（不释放内存）
         */
        void Clear()
        {
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                for (IndexType i = 0; i < size_; ++i)
                {
                    data_[i].~T();
                }
            }
            size_ = 0;
        }

        /**
         * @brief 清空元素并释放内存
         */
        void Reset()
        {
            Clear();
            if (data_ != nullptr)
            {
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
         * 若 p_new_capacity > 当前容量，重新分配内存。
         * 增长策略：
         *   - kTight=false：取 max(1.5x 当前容量, p_new_capacity)
         *   - kTight=true：直接使用 p_new_capacity
         *
         * @param[in] p_new_capacity 需要的最小容量
         */
        void Reserve(IndexType p_new_capacity)
        {
            if (p_new_capacity <= capacity_)
            {
                return;
            }

            IndexType new_cap;
            if constexpr (kTight)
            {
                new_cap = p_new_capacity;
            }
            else
            {
                new_cap = (capacity_ + (capacity_ + 1) / 2);
                if (new_cap < 2)
                {
                    new_cap = 2;
                }
                if (p_new_capacity > new_cap)
                {
                    new_cap = p_new_capacity;
                }
            }

            T *new_data = static_cast<T *>(memory::Alloc(sizeof(T) * new_cap));

            if (data_ != nullptr)
            {
                if constexpr (std::is_trivially_copyable_v<T>)
                {
                    std::memcpy(static_cast<void *>(new_data),
                                static_cast<const void *>(data_), sizeof(T) * size_);
                }
                else
                {
                    for (IndexType i = 0; i < size_; ++i)
                    {
                        ::new (static_cast<void *>(&new_data[i])) T(std::move(data_[i]));
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
         * @param[in] p_new_size 新大小
         */
        void Resize(IndexType p_new_size)
        {
            ResizeInternal<true>(p_new_size);
        }

        /**
         * @brief 调整大小（新元素零初始化）
         *
         * 对平凡类型用 memset 清零，对非平凡类型默认构造。
         *
         * @param[in] p_new_size 新大小
         */
        void ResizeInitialized(IndexType p_new_size)
        {
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
        void ResizeUninitialized(IndexType p_new_size)
        {
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
         * @param[in] p_val  要查找的值
         * @param[in] p_from 起始索引（默认 0）
         *
         * @return 元素索引，未找到返回 -1
         */
        int64_t Find(const T &p_val, IndexType p_from = 0) const
        {
            for (IndexType i = p_from; i < size_; ++i)
            {
                if (data_[i] == p_val)
                {
                    return static_cast<int64_t>(i);
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

    private:
        /**
         * @brief 内部调整大小实现
         *
         * @tparam kInit 是否初始化新元素
         * @param[in] p_new_size 新大小
         */
        template <bool kInit>
        void ResizeInternal(IndexType p_new_size)
        {
            if (p_new_size < size_)
            {
                if constexpr (!std::is_trivially_destructible_v<T>)
                {
                    for (IndexType i = p_new_size; i < size_; ++i)
                    {
                        data_[i].~T();
                    }
                }
                size_ = p_new_size;
            }
            else if (p_new_size > size_)
            {
                Reserve(p_new_size);
                if constexpr (kInit)
                {
                    for (IndexType i = size_; i < p_new_size; ++i)
                    {
                        ::new (static_cast<void *>(&data_[i])) T();
                    }
                }
                size_ = p_new_size;
            }
        }

        T *data_ = nullptr;
        IndexType size_ = 0;
        IndexType capacity_ = 0;
    };

    /**
     * @brief 严格按需增长的 LocalVector 别名
     *
     * 适用于只追加一次后不再增长、或内存敏感的场景。
     *
     * @tparam T         元素类型
     * @tparam IndexType 索引类型
     */
    template <typename T, typename IndexType = uint32_t>
    using TightLocalVector = LocalVector<T, IndexType, true>;

} // namespace arhud
