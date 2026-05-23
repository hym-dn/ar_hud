/**
 * @file hash_map.h
 * @brief 开放寻址哈希表（线性探测）
 *
 * 参考 Godot 4.6 HashMap 设计，简化为线性探测开放寻址方案。
 * 内联存储键值对（无指针间接），缓存友好。
 *
 * 设计取舍：
 *   - 线性探测而非 Robin Hood：实现更简单，性能足够 HUD 场景
 *   - 内联存储而非链表：减少内存分配次数，缓存友好
 *   - 质数容量表：减少聚集，改善分布均匀性
 *   - 负载因子 0.75：平衡空间与性能
 *   - 不保持插入顺序：HUD 场景不需要
 *
 * 典型用途：
 * - GPU 资源依赖追踪（HashMap<RID, HashSet<RID>>）
 * - Shader 缓存（HashMap<String, ShaderProgram>）
 * - 配置键值存储
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-03
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
    // 默认哈希函数
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 默认哈希函数对象
     *
     * 对整数类型直接使用值，对指针类型做位移混合。
     * 特化时可通过偏特化提供自定义哈希。
     */
    struct DefaultHasher
    {
        template <typename T>
        static uint32_t Hash(const T &p_key)
        {
            if constexpr (std::is_integral_v<T>)
            {
                if constexpr (sizeof(T) <= 4)
                {
                    uint32_t h = static_cast<uint32_t>(p_key);
                    h = ((h >> 16) ^ h) * 0x45d9f3b;
                    h = ((h >> 16) ^ h) * 0x45d9f3b;
                    h = (h >> 16) ^ h;
                    return h;
                }
                else
                {
                    uint64_t h = static_cast<uint64_t>(p_key);
                    h ^= h >> 33;
                    h *= 0xff51afd7ed558ccdULL;
                    h ^= h >> 33;
                    h *= 0xc4ceb9fe1a85ec53ULL;
                    h ^= h >> 33;
                    return static_cast<uint32_t>(h) ^ static_cast<uint32_t>(h >> 32);
                }
            }
            else if constexpr (std::is_pointer_v<T>)
            {
                uintptr_t h = reinterpret_cast<uintptr_t>(p_key);
                if constexpr (sizeof(uintptr_t) <= 4)
                {
                    uint32_t h32 = static_cast<uint32_t>(h);
                    h32 = ((h32 >> 16) ^ h32) * 0x45d9f3b;
                    h32 = ((h32 >> 16) ^ h32) * 0x45d9f3b;
                    h32 = (h32 >> 16) ^ h32;
                    return h32;
                }
                else
                {
                    h ^= h >> 33;
                    h *= 0xff51afd7ed558ccdULL;
                    h ^= h >> 33;
                    h *= 0xc4ceb9fe1a85ec53ULL;
                    h ^= h >> 33;
                    return static_cast<uint32_t>(h) ^ static_cast<uint32_t>(h >> 32);
                }
            }
            else
            {
                return static_cast<uint32_t>(p_key.GetHash());
            }
        }
    };

    /**
     * @brief 默认键比较器
     */
    struct DefaultComparator
    {
        template <typename T>
        static bool Compare(const T &p_a, const T &p_b)
        {
            return p_a == p_b;
        }
    };

    // ═══════════════════════════════════════════════════════════════════════
    // HashMap
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 开放寻址哈希表
     *
     * 使用线性探测解决冲突，内联存储键值对。
     * 容量使用质数序列，负载因子超过 0.75 时自动扩容。
     *
     * @tparam TKey       键类型（必须可复制、可比较）
     * @tparam TValue     值类型
     * @tparam Hasher     哈希函数对象
     * @tparam Comparator 键比较器
     */
    template <typename TKey, typename TValue,
              typename Hasher = DefaultHasher,
              typename Comparator = DefaultComparator>
    class HashMap
    {
        static constexpr uint32_t kEmptyHash = 0;
        static constexpr float kMaxOccupancy = 0.75f;
        static constexpr uint32_t kMinCapacityIndex = 2;

        struct Slot
        {
            uint32_t hash;
            TKey key;
            TValue value;
        };

    public:
        /**
         * @brief 键值对（用于迭代）
         */
        struct Pair
        {
            const TKey &key;
            TValue &value;
        };

        /**
         * @brief 只读键值对
         */
        struct ConstPair
        {
            const TKey &key;
            const TValue &value;
        };

        /**
         * @brief 迭代器
         */
        class Iterator
        {
            friend class HashMap;

        public:
            ARHUD_ALWAYS_INLINE Pair operator*() const
            {
                return {map_->slots_[idx_].key, map_->slots_[idx_].value};
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
            Iterator(HashMap *p_map, uint32_t p_idx) : map_(p_map), idx_(p_idx) {}

        private:
            void SkipEmpty()
            {
                while (idx_ < map_->capacity_ && map_->slots_[idx_].hash == kEmptyHash)
                {
                    ++idx_;
                }
            }

            HashMap *map_ = nullptr;
            uint32_t idx_ = 0;
        };

        /**
         * @brief 只读迭代器
         */
        class ConstIterator
        {
            friend class HashMap;

        public:
            ARHUD_ALWAYS_INLINE ConstPair operator*() const
            {
                return {map_->slots_[idx_].key, map_->slots_[idx_].value};
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
            ConstIterator(const HashMap *p_map, uint32_t p_idx) : map_(p_map), idx_(p_idx) {}

        private:
            void SkipEmpty()
            {
                while (idx_ < map_->capacity_ && map_->slots_[idx_].hash == kEmptyHash)
                {
                    ++idx_;
                }
            }

            const HashMap *map_ = nullptr;
            uint32_t idx_ = 0;
        };

        // ═══════════════════════════════════════════════════════════════
        // 构造 / 析构 / 赋值
        // ═══════════════════════════════════════════════════════════════

        HashMap() = default;

        HashMap(const HashMap &p_other)
        {
            Reserve(p_other.Size());
            for (auto it = p_other.Begin(); it != p_other.End(); ++it)
            {
                ConstPair pair = *it;
                Insert(pair.key, pair.value);
            }
        }

        HashMap(HashMap &&p_other) noexcept
            : slots_(p_other.slots_), capacity_(p_other.capacity_),
              size_(p_other.size_), capacity_index_(p_other.capacity_index_)
        {
            p_other.slots_ = nullptr;
            p_other.capacity_ = 0;
            p_other.size_ = 0;
            p_other.capacity_index_ = 0;
        }

        ~HashMap() { Clear(); }

        HashMap &operator=(const HashMap &p_other)
        {
            if (this != &p_other)
            {
                Clear();
                Reserve(p_other.Size());
                for (auto it = p_other.Begin(); it != p_other.End(); ++it)
                {
                    ConstPair pair = *it;
                    Insert(pair.key, pair.value);
                }
            }
            return *this;
        }

        HashMap &operator=(HashMap &&p_other) noexcept
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
         * @brief 查找键对应的值指针
         *
         * @param[in] p_key 要查找的键
         *
         * @return 值指针，未找到返回 nullptr
         */
        TValue *GetPtr(const TKey &p_key)
        {
            uint32_t idx = 0;
            if (LookupIndex(p_key, idx))
            {
                return &slots_[idx].value;
            }
            return nullptr;
        }

        /**
         * @brief 查找键对应的值指针（const 版本）
         *
         * @param[in] p_key 要查找的键
         *
         * @return 值指针，未找到返回 nullptr
         */
        const TValue *GetPtr(const TKey &p_key) const
        {
            uint32_t idx = 0;
            if (LookupIndex(p_key, idx))
            {
                return &slots_[idx].value;
            }
            return nullptr;
        }

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
         * @brief 插入或更新键值对
         *
         * 若键已存在则更新值，否则插入新条目。
         *
         * @param[in] p_key   键
         * @param[in] p_value 值
         */
        void Insert(const TKey &p_key, const TValue &p_value)
        {
            uint32_t hash = ComputeHash(p_key);

            uint32_t idx = 0;
            if (LookupIndexWithHash(p_key, hash, idx))
            {
                slots_[idx].value = p_value;
                return;
            }

            if (slots_ == nullptr || size_ + 1 > static_cast<uint32_t>(kMaxOccupancy * capacity_))
            {
                Grow(capacity_index_ + 1);
                if (LookupIndexWithHash(p_key, hash, idx))
                {
                    slots_[idx].value = p_value;
                    return;
                }
            }

            InsertInternal(hash, p_key, p_value);
            ++size_;
        }

        /**
         * @brief 插入键并返回值引用（若键不存在则默认构造值）
         *
         * @param[in] p_key 键
         *
         * @return 值的引用
         */
        TValue &operator[](const TKey &p_key)
        {
            uint32_t hash = ComputeHash(p_key);

            uint32_t idx = 0;
            if (LookupIndexWithHash(p_key, hash, idx))
            {
                return slots_[idx].value;
            }

            if (slots_ == nullptr || size_ + 1 > static_cast<uint32_t>(kMaxOccupancy * capacity_))
            {
                Grow(capacity_index_ + 1);
            }

            InsertInternal(hash, p_key, TValue());
            ++size_;

            LookupIndexWithHash(p_key, hash, idx);
            return slots_[idx].value;
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
                    slots_[idx].value = std::move(slots_[next].value);
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
            1610612741
        };
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

        void InsertInternal(uint32_t p_hash, const TKey &p_key, const TValue &p_value)
        {
            uint32_t idx = p_hash % capacity_;
            while (slots_[idx].hash != kEmptyHash)
            {
                idx = (idx + 1) % capacity_;
            }

            slots_[idx].hash = p_hash;
            ::new (static_cast<void *>(&slots_[idx].key)) TKey(p_key);
            ::new (static_cast<void *>(&slots_[idx].value)) TValue(p_value);
        }

        void DestroySlot(uint32_t p_idx)
        {
            if constexpr (!std::is_trivially_destructible_v<TKey>)
            {
                slots_[p_idx].key.~TKey();
            }
            if constexpr (!std::is_trivially_destructible_v<TValue>)
            {
                slots_[p_idx].value.~TValue();
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
                    InsertInternal(old_slots[i].hash, old_slots[i].key, old_slots[i].value);
                    ++size_;

                    if constexpr (!std::is_trivially_destructible_v<TKey>)
                    {
                        old_slots[i].key.~TKey();
                    }
                    if constexpr (!std::is_trivially_destructible_v<TValue>)
                    {
                        old_slots[i].value.~TValue();
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
