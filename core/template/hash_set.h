/**
 * @file hash_set.h
 * @brief 开放寻址哈希集合（线性探测）
 *
 * 参考 Godot 4.6 HashSet 设计，与 HashMap 共享哈希基础设施。
 * 仅存储键，无值部分，适用于去重、成员检测、依赖追踪等场景。
 *
 * 设计取舍：
 *   - 线性探测而非 Robin Hood：与 HashMap 一致，实现简单
 *   - 内联存储而非链表：缓存友好，减少内存分配
 *   - 质数容量表：与 HashMap 共享，减少聚集
 *   - 负载因子 0.75：与 HashMap 一致
 *   - 不保持插入顺序
 *
 * 典型用途：
 * - GPU 资源依赖追踪（HashMap<RID, HashSet<RID>>）
 * - 去重集合
 * - 快速成员检测
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-05
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <cstring>
#include <type_traits>
#include <utility>

#include "os/memory.h"
#include "typedefs.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // HashSet
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 开放寻址哈希集合
     *
     * 使用线性探测解决冲突，内联存储键。
     * 容量使用质数序列，负载因子超过 0.75 时自动扩容。
     * 与 HashMap 共享 DefaultHasher / DefaultComparator。
     *
     * @tparam TKey       键类型（必须可复制、可比较）
     * @tparam Hasher     哈希函数对象（默认 DefaultHasher）
     * @tparam Comparator 键比较器（默认 DefaultComparator）
     */
    template <typename TKey,
              typename Hasher = DefaultHasher,
              typename Comparator = DefaultComparator>
    class HashSet
    {
        static constexpr uint32_t kEmptyHash = 0;
        static constexpr float kMaxOccupancy = 0.75f;
        static constexpr uint32_t kMinCapacityIndex = 2;

        struct Slot
        {
            uint32_t hash;
            TKey key;
        };

    public:
        /**
         * @brief 迭代器
         */
        class Iterator
        {
            friend class HashSet;

        public:
            ARHUD_ALWAYS_INLINE const TKey &operator*() const
            {
                return set_->slots_[idx_].key;
            }
            ARHUD_ALWAYS_INLINE Iterator &operator++()
            {
                ++idx_;
                SkipEmpty();
                return *this;
            }
            ARHUD_ALWAYS_INLINE bool operator==(const Iterator &p_other) const { return idx_ == p_other.idx_; }
            ARHUD_ALWAYS_INLINE bool operator!=(const Iterator &p_other) const { return idx_ != p_other.idx_; }

            Iterator() = default;
            Iterator(HashSet *p_set, uint32_t p_idx) : set_(p_set), idx_(p_idx) {}

        private:
            void SkipEmpty()
            {
                while (idx_ < set_->capacity_ && set_->slots_[idx_].hash == kEmptyHash)
                {
                    ++idx_;
                }
            }

            HashSet *set_ = nullptr;
            uint32_t idx_ = 0;
        };

        /**
         * @brief 只读迭代器
         */
        class ConstIterator
        {
            friend class HashSet;

        public:
            ARHUD_ALWAYS_INLINE const TKey &operator*() const
            {
                return set_->slots_[idx_].key;
            }
            ARHUD_ALWAYS_INLINE ConstIterator &operator++()
            {
                ++idx_;
                SkipEmpty();
                return *this;
            }
            ARHUD_ALWAYS_INLINE bool operator==(const ConstIterator &p_other) const { return idx_ == p_other.idx_; }
            ARHUD_ALWAYS_INLINE bool operator!=(const ConstIterator &p_other) const { return idx_ != p_other.idx_; }

            ConstIterator() = default;
            ConstIterator(const HashSet *p_set, uint32_t p_idx) : set_(p_set), idx_(p_idx) {}

        private:
            void SkipEmpty()
            {
                while (idx_ < set_->capacity_ && set_->slots_[idx_].hash == kEmptyHash)
                {
                    ++idx_;
                }
            }

            const HashSet *set_ = nullptr;
            uint32_t idx_ = 0;
        };

        // ═══════════════════════════════════════════════════════════════
        // 构造 / 析构 / 赋值
        // ═══════════════════════════════════════════════════════════════

        HashSet() = default;

        HashSet(const HashSet &p_other)
        {
            Reserve(p_other.Size());
            for (auto it = p_other.Begin(); it != p_other.End(); ++it)
            {
                Insert(*it);
            }
        }

        HashSet(HashSet &&p_other) noexcept
            : slots_(p_other.slots_),
              capacity_(p_other.capacity_),
              size_(p_other.size_),
              capacity_index_(p_other.capacity_index_)
        {
            p_other.slots_ = nullptr;
            p_other.capacity_ = 0;
            p_other.size_ = 0;
            p_other.capacity_index_ = 0;
        }

        ~HashSet() { Clear(); }

        HashSet &operator=(const HashSet &p_other)
        {
            if (this != &p_other)
            {
                Clear();
                Reserve(p_other.Size());
                for (auto it = p_other.Begin(); it != p_other.End(); ++it)
                {
                    Insert(*it);
                }
            }
            return *this;
        }

        HashSet &operator=(HashSet &&p_other) noexcept
        {
            if (this != &p_other)
            {
                Clear();
                if (slots_ != nullptr)
                {
                    memory::Free(slots_);
                }

                slots_ = p_other.slots_;
                capacity_ = p_other.capacity_;
                size_ = p_other.size_;
                capacity_index_ = p_other.capacity_index_;

                p_other.slots_ = nullptr;
                p_other.capacity_ = 0;
                p_other.size_ = 0;
                p_other.capacity_index_ = 0;
            }
            return *this;
        }

        // ═══════════════════════════════════════════════════════════════
        // 容量查询
        // ═══════════════════════════════════════════════════════════════

        ARHUD_ALWAYS_INLINE uint32_t Size() const { return size_; }
        ARHUD_ALWAYS_INLINE bool IsEmpty() const { return size_ == 0; }
        ARHUD_ALWAYS_INLINE uint32_t Capacity() const { return capacity_; }

        // ═══════════════════════════════════════════════════════════════
        // 迭代器
        // ═══════════════════════════════════════════════════════════════

        Iterator Begin()
        {
            Iterator it(this, 0);
            it.SkipEmpty();
            return it;
        }

        Iterator End()
        {
            return Iterator(this, capacity_);
        }

        ConstIterator Begin() const
        {
            ConstIterator it(this, 0);
            it.SkipEmpty();
            return it;
        }

        ConstIterator End() const
        {
            return ConstIterator(this, capacity_);
        }

        // ═══════════════════════════════════════════════════════════════
        // 查找
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 检查是否包含指定键
         *
         * @param[in] p_key 要查找的键
         *
         * @return true 包含
         */
        bool Has(const TKey &p_key) const
        {
            uint32_t idx = 0;
            return LookupIndex(p_key, idx);
        }

        // ═══════════════════════════════════════════════════════════════
        // 插入
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 插入键
         *
         * 若键已存在则忽略，否则插入新条目。
         *
         * @param[in] p_key 键
         *
         * @return true 新插入，false 键已存在
         */
        bool Insert(const TKey &p_key)
        {
            uint32_t hash = ComputeHash(p_key);

            uint32_t idx = 0;
            if (LookupIndexWithHash(p_key, hash, idx))
            {
                return false;
            }

            if (slots_ == nullptr || size_ + 1 > static_cast<uint32_t>(kMaxOccupancy * capacity_))
            {
                Grow(capacity_index_ + 1);
            }

            InsertInternal(hash, p_key);
            ++size_;
            return true;
        }

        // ═══════════════════════════════════════════════════════════════
        // 删除
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 删除指定键
         *
         * 使用向后移位删除（backward shift deletion），
         * 避免线性探测中的空洞问题。
         *
         * @param[in] p_key 要删除的键
         *
         * @return true 成功删除
         * @retval false 键不存在
         */
        bool Erase(const TKey &p_key)
        {
            if (size_ == 0)
            {
                return false;
            }

            uint32_t idx = 0;
            if (!LookupIndex(p_key, idx))
            {
                return false;
            }

            DestroySlot(idx);

            uint32_t capacity = capacity_;
            uint32_t next = (idx + 1) % capacity;
            while (slots_[next].hash != kEmptyHash)
            {
                uint32_t desired = slots_[next].hash % capacity;
                if (desired == next)
                {
                    break;
                }
                if ((next > idx && (desired <= idx || desired > next)) ||
                    (next < idx && (desired <= idx && desired > next)))
                {
                    slots_[idx].hash = slots_[next].hash;
                    slots_[idx].key = std::move(slots_[next].key);
                    DestroySlot(next);
                    idx = next;
                }
                next = (next + 1) % capacity;
            }

            --size_;
            return true;
        }

        /**
         * @brief 清空所有元素（不释放内存）
         */
        void Clear()
        {
            if (slots_ == nullptr || size_ == 0)
            {
                return;
            }

            for (uint32_t i = 0; i < capacity_; ++i)
            {
                if (slots_[i].hash != kEmptyHash)
                {
                    DestroySlot(i);
                }
            }
            size_ = 0;
        }

        /**
         * @brief 清空所有元素并释放内存
         */
        void Reset()
        {
            Clear();
            if (slots_ != nullptr)
            {
                memory::Free(slots_);
                slots_ = nullptr;
                capacity_ = 0;
                capacity_index_ = 0;
            }
        }

        /**
         * @brief 预留容量
         *
         * @param[in] p_size 预期的元素数量
         */
        void Reserve(uint32_t p_size)
        {
            uint32_t needed = static_cast<uint32_t>(p_size / kMaxOccupancy) + 1;
            uint32_t idx = capacity_index_;
            while (idx < kPrimesCount && kPrimes[idx] < needed)
            {
                ++idx;
            }
            if (idx > capacity_index_)
            {
                Grow(idx);
            }
        }

    private:
        static constexpr uint32_t kPrimes[] = {
            0, 0, 5, 13, 23, 53, 97, 193, 389, 769,
            1543, 3079, 6151, 12289, 24593, 49157, 98317,
            196613, 393241, 786433, 1572869, 3145739,
            6291469, 12582917, 25165843, 50331653,
            100663319, 201326611, 402653189, 805306457,
            1610612741};
        static constexpr uint32_t kPrimesCount = sizeof(kPrimes) / sizeof(kPrimes[0]);

        ARHUD_ALWAYS_INLINE uint32_t ComputeHash(const TKey &p_key) const
        {
            uint32_t h = Hasher::Hash(p_key);
            return (h == kEmptyHash) ? kEmptyHash + 1 : h;
        }

        bool LookupIndex(const TKey &p_key, uint32_t &r_idx) const
        {
            return slots_ != nullptr && size_ > 0 &&
                   LookupIndexWithHash(p_key, ComputeHash(p_key), r_idx);
        }

        bool LookupIndexWithHash(const TKey &p_key, uint32_t p_hash, uint32_t &r_idx) const
        {
            if (capacity_ == 0)
            {
                return false;
            }

            uint32_t idx = p_hash % capacity_;
            uint32_t start = idx;

            do
            {
                if (slots_[idx].hash == kEmptyHash)
                {
                    return false;
                }
                if (slots_[idx].hash == p_hash && Comparator::Compare(slots_[idx].key, p_key))
                {
                    r_idx = idx;
                    return true;
                }
                idx = (idx + 1) % capacity_;
            } while (idx != start);

            return false;
        }

        void InsertInternal(uint32_t p_hash, const TKey &p_key)
        {
            uint32_t idx = p_hash % capacity_;
            while (slots_[idx].hash != kEmptyHash)
            {
                idx = (idx + 1) % capacity_;
            }

            slots_[idx].hash = p_hash;
            ::new (static_cast<void *>(&slots_[idx].key)) TKey(p_key);
        }

        void DestroySlot(uint32_t p_idx)
        {
            if constexpr (!std::is_trivially_destructible_v<TKey>)
            {
                slots_[p_idx].key.~TKey();
            }
            slots_[p_idx].hash = kEmptyHash;
        }

        void Grow(uint32_t p_new_capacity_index)
        {
            if (p_new_capacity_index >= kPrimesCount)
            {
                p_new_capacity_index = kPrimesCount - 1;
            }
            if (p_new_capacity_index < kMinCapacityIndex)
            {
                p_new_capacity_index = kMinCapacityIndex;
            }

            Slot *old_slots = slots_;
            uint32_t old_capacity = capacity_;

            capacity_index_ = p_new_capacity_index;
            capacity_ = kPrimes[capacity_index_];
            size_ = 0;

            slots_ = static_cast<Slot *>(memory::Alloc(sizeof(Slot) * capacity_));
            std::memset(slots_, 0, sizeof(Slot) * capacity_);

            for (uint32_t i = 0; i < old_capacity; ++i)
            {
                if (old_slots[i].hash != kEmptyHash)
                {
                    InsertInternal(old_slots[i].hash, old_slots[i].key);
                    ++size_;

                    if constexpr (!std::is_trivially_destructible_v<TKey>)
                    {
                        old_slots[i].key.~TKey();
                    }
                }
            }

            if (old_slots != nullptr)
            {
                memory::Free(old_slots);
            }
        }

        Slot *slots_ = nullptr;
        uint32_t capacity_ = 0;
        uint32_t size_ = 0;
        uint32_t capacity_index_ = 0;
    };

} // namespace arhud
