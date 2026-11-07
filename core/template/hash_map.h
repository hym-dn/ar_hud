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

#ifndef ARHUD_CORE_TEMPLATE_HASH_MAP_H_
#define ARHUD_CORE_TEMPLATE_HASH_MAP_H_

#include <cstring>
#include <type_traits>
#include <utility>

#include "os/memory.h"
#include "typedefs.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// 默认哈希函数
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 默认哈希函数对象
 *
 * 根据键类型自动选择哈希策略：
 * - 整数类型（<=4 字节）：使用 MurmurHash3 32-bit 最终混合（fmix32）
 * - 整数类型（>4 字节）：使用 MurmurHash3 64-bit 最终混合（fmix64）
 * - 指针类型：将地址转换为整数后，按平台字长选择 32/64 位混合
 * - 其他类型：要求类型提供 @c GetHash() 成员方法
 *
 * 可通过模板偏特化为自定义类型提供专用哈希实现。
 *
 * @note 混合函数的目的是使低位和高位的比特充分扩散，
 *       减少线性探测哈希表中的聚集现象。
 */
struct DefaultHasher {
  /**
   * @brief 计算键的哈希值
   *
   * 根据 @p p_key 的类型在编译期选择最优哈希路径（via @c if constexpr）。
   *
   * @tparam T 键类型，需为整数、指针或提供 GetHash() 方法的类型
   * @param[in] p_key 待哈希的键
   * @return 32 位哈希值（保证非零，零保留为空槽标记）
   */
  template <typename T>
  static uint32 Hash(const T& p_key) {
    if constexpr (std::is_integral_v<T>) {
      if constexpr (sizeof(T) <= 4) {
        uint32 h = static_cast<uint32>(p_key);
        h = ((h >> 16) ^ h) * 0x45d9f3b;
        h = ((h >> 16) ^ h) * 0x45d9f3b;
        h = (h >> 16) ^ h;
        return h;
      } else {
        uint64 h = static_cast<uint64>(p_key);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return static_cast<uint32>(h) ^ static_cast<uint32>(h >> 32);
      }
    } else if constexpr (std::is_pointer_v<T>) {
      uintptr_t h = reinterpret_cast<uintptr_t>(p_key);
      if constexpr (sizeof(uintptr_t) <= 4) {
        uint32 h32 = static_cast<uint32>(h);
        h32 = ((h32 >> 16) ^ h32) * 0x45d9f3b;
        h32 = ((h32 >> 16) ^ h32) * 0x45d9f3b;
        h32 = (h32 >> 16) ^ h32;
        return h32;
      } else {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return static_cast<uint32>(h) ^ static_cast<uint32>(h >> 32);
      }
    } else {
      return static_cast<uint32>(p_key.GetHash());
    }
  }
};

/**
 * @brief 默认键比较器
 *
 * 使用 @c operator== 进行相等性判断。
 * 可通过模板偏特化为自定义类型提供专用比较逻辑。
 */
struct DefaultComparator {
  /**
   * @brief 判断两个键是否相等
   *
   * @tparam T 键类型，需支持 @c operator==
   * @param[in] p_a 第一个键
   * @param[in] p_b 第二个键
   * @return true 两键相等
   * @return false 两键不等
   */
  template <typename T>
  static bool Compare(const T& p_a, const T& p_b) {
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
template <typename TKey, typename TValue, typename Hasher = DefaultHasher,
          typename Comparator = DefaultComparator>
class HashMap {
  /** @brief 空槽标记值，哈希为 0 表示该槽位未被使用 */
  static constexpr uint32 kEmptyHash = 0;
  /** @brief 最大负载因子，超过此比例触发扩容（75%） */
  static constexpr float kMaxOccupancy = 0.75f;
  /** @brief 最小容量在质数表中的索引（对应容量 5），避免过小初始分配 */
  static constexpr uint32 kMinCapacityIndex = 2;

  /**
   * @brief 哈希表槽位，内联存储一个键值对及其哈希值
   *
   * 缓存哈希值以避免查找时重复计算，同时用 @c hash == kEmptyHash
   * 作为槽位是否被占用的判据。
   */
  struct Slot {
    uint32 hash;  /**< 键的哈希值（0 表示空槽） */
    TKey key;     /**< 键（原地构造，无指针间接） */
    TValue value; /**< 值（原地构造，无指针间接） */
  };

 public:
  /**
   * @brief 可变键值对引用（用于迭代器解引用）
   */
  struct Pair {
    const TKey& key; /**< 键（只读，修改会破坏哈希表不变量） */
    TValue& value;   /**< 值（可写） */
  };

  /**
   * @brief 只读键值对引用（用于 const 迭代器解引用）
   */
  struct ConstPair {
    const TKey& key;     /**< 键（只读） */
    const TValue& value; /**< 值（只读） */
  };

  /**
   * @brief 可变前向迭代器
   *
   * 遍历哈希表中所有已占用槽位，跳过空槽。
   * 解引用返回 @ref Pair（包含键的 const 引用和值的可变引用）。
   *
   * @note 迭代顺序取决于槽位在数组中的物理位置，非插入顺序。
   *       插入/删除操作可能使迭代器失效。
   */
  class Iterator {
    friend class HashMap;

   public:
    /**
     * @brief 解引用，返回当前槽位的键值对
     * @return Pair 包含 const TKey& 和 TValue&
     */
    ARHUD_ALWAYS_INLINE Pair operator*() const {
      return {map_->slots_[idx_].key, map_->slots_[idx_].value};
    }

    /**
     * @brief 前缀递增，前进到下一个已占用槽位
     * @return 引用到自身
     */
    ARHUD_ALWAYS_INLINE Iterator& operator++() {
      ++idx_;
      SkipEmpty();
      return *this;
    }

    /** @brief 相等比较 */
    ARHUD_ALWAYS_INLINE bool operator==(const Iterator& p_other) const {
      return idx_ == p_other.idx_;
    }
    /** @brief 不等比较 */
    ARHUD_ALWAYS_INLINE bool operator!=(const Iterator& p_other) const {
      return idx_ != p_other.idx_;
    }

    /** @brief 默认构造（空迭代器） */
    Iterator() = default;

    /**
     * @brief 构造指向指定位置的迭代器
     * @param[in] p_map 所属哈希表
     * @param[in] p_idx 槽位索引
     */
    Iterator(HashMap* p_map, uint32 p_idx) : map_(p_map), idx_(p_idx) {}

   private:
    /**
     * @brief 跳过后续空槽，将 idx_ 定位到下一个已占用槽位或末尾
     */
    void SkipEmpty() {
      while (idx_ < map_->capacity_ && map_->slots_[idx_].hash == kEmptyHash) {
        ++idx_;
      }
    }

    HashMap* map_ = nullptr; /**< 所属哈希表指针 */
    uint32 idx_ = 0;         /**< 当前槽位索引 */
  };

  /**
   * @brief 只读前向迭代器
   *
   * 遍历哈希表中所有已占用槽位，跳过空槽。
   * 解引用返回 @ref ConstPair（键和值均为 const 引用）。
   *
   * @note 迭代顺序取决于槽位在数组中的物理位置，非插入顺序。
   */
  class ConstIterator {
    friend class HashMap;

   public:
    /**
     * @brief 解引用，返回当前槽位的只读键值对
     * @return ConstPair 包含 const TKey& 和 const TValue&
     */
    ARHUD_ALWAYS_INLINE ConstPair operator*() const {
      return {map_->slots_[idx_].key, map_->slots_[idx_].value};
    }

    /**
     * @brief 前缀递增，前进到下一个已占用槽位
     * @return 引用到自身
     */
    ARHUD_ALWAYS_INLINE ConstIterator& operator++() {
      ++idx_;
      SkipEmpty();
      return *this;
    }

    /** @brief 相等比较 */
    ARHUD_ALWAYS_INLINE bool operator==(const ConstIterator& p_other) const {
      return idx_ == p_other.idx_;
    }
    /** @brief 不等比较 */
    ARHUD_ALWAYS_INLINE bool operator!=(const ConstIterator& p_other) const {
      return idx_ != p_other.idx_;
    }

    /** @brief 默认构造（空迭代器） */
    ConstIterator() = default;

    /**
     * @brief 构造指向指定位置的只读迭代器
     * @param[in] p_map 所属哈希表（const）
     * @param[in] p_idx 槽位索引
     */
    ConstIterator(const HashMap* p_map, uint32 p_idx)
        : map_(p_map), idx_(p_idx) {}

   private:
    /**
     * @brief 跳过后续空槽，将 idx_ 定位到下一个已占用槽位或末尾
     */
    void SkipEmpty() {
      while (idx_ < map_->capacity_ && map_->slots_[idx_].hash == kEmptyHash) {
        ++idx_;
      }
    }

    const HashMap* map_ = nullptr; /**< 所属哈希表指针（const） */
    uint32 idx_ = 0;               /**< 当前槽位索引 */
  };

  // ═══════════════════════════════════════════════════════════════
  // 构造 / 析构 / 赋值
  // ═══════════════════════════════════════════════════════════════

  /** @brief 默认构造，创建空哈希表（不分配内存） */
  HashMap() = default;

  /**
   * @brief 拷贝构造，深拷贝另一个哈希表的所有键值对
   *
   * 先预留足够容量以避免多次扩容，然后逐个插入。
   *
   * @param[in] p_other 源哈希表
   */
  HashMap(const HashMap& p_other) {
    Reserve(p_other.Size());
    for (auto it = p_other.Begin(); it != p_other.End(); ++it) {
      ConstPair pair = *it;
      Insert(pair.key, pair.value);
    }
  }

  /**
   * @brief 移动构造，接管另一个哈希表的内部资源
   *
   * 构造后 @p p_other 被置为空状态（不持有任何内存）。
   *
   * @param[in,out] p_other 源哈希表（移动后置空）
   */
  HashMap(HashMap&& p_other) noexcept
      : slots_(p_other.slots_),
        capacity_(p_other.capacity_),
        size_(p_other.size_),
        capacity_index_(p_other.capacity_index_) {
    p_other.slots_ = nullptr;
    p_other.capacity_ = 0;
    p_other.size_ = 0;
    p_other.capacity_index_ = 0;
  }

  /**
   * @brief 析构，销毁所有元素并释放内存
   */
  ~HashMap() {
    Clear();
  }

  /**
   * @brief 拷贝赋值，清空自身后深拷贝源表的所有键值对
   *
   * @param[in] p_other 源哈希表
   * @return 引用到自身
   */
  HashMap& operator=(const HashMap& p_other) {
    if (this != &p_other) {
      Clear();
      Reserve(p_other.Size());
      for (auto it = p_other.Begin(); it != p_other.End(); ++it) {
        ConstPair pair = *it;
        Insert(pair.key, pair.value);
      }
    }
    return *this;
  }

  /**
   * @brief 移动赋值，释放自身资源后接管源表的内部资源
   *
   * 赋值后 @p p_other 被置为空状态。
   *
   * @param[in,out] p_other 源哈希表（移动后置空）
   * @return 引用到自身
   */
  HashMap& operator=(HashMap&& p_other) noexcept {
    if (this != &p_other) {
      Clear();
      if (slots_ != nullptr) {
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

  ARHUD_ALWAYS_INLINE uint32 Size() const {
    return size_;
  }
  ARHUD_ALWAYS_INLINE bool IsEmpty() const {
    return size_ == 0;
  }
  ARHUD_ALWAYS_INLINE uint32 Capacity() const {
    return capacity_;
  }

  // ═══════════════════════════════════════════════════════════════
  // 迭代器
  // ═══════════════════════════════════════════════════════════════

  /**
   * @brief 返回指向第一个已占用槽位的可变迭代器
   * @return Iterator 若表为空则等价于 End()
   */
  Iterator Begin() {
    Iterator it(this, 0);
    it.SkipEmpty();
    return it;
  }

  /**
   * @brief 返回尾后哨兵迭代器（不可解引用）
   * @return Iterator 索引等于 capacity_
   */
  Iterator End() {
    return Iterator(this, capacity_);
  }

  /**
   * @brief 返回指向第一个已占用槽位的只读迭代器
   * @return ConstIterator 若表为空则等价于 End()
   */
  ConstIterator Begin() const {
    ConstIterator it(this, 0);
    it.SkipEmpty();
    return it;
  }

  /**
   * @brief 返回尾后只读哨兵迭代器（不可解引用）
   * @return ConstIterator 索引等于 capacity_
   */
  ConstIterator End() const {
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
  TValue* GetPtr(const TKey& p_key) {
    uint32 idx = 0;
    if (LookupIndex(p_key, idx)) {
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
  const TValue* GetPtr(const TKey& p_key) const {
    uint32 idx = 0;
    if (LookupIndex(p_key, idx)) {
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
  bool Has(const TKey& p_key) const {
    uint32 idx = 0;
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
  void Insert(const TKey& p_key, const TValue& p_value) {
    uint32 hash = ComputeHash(p_key);

    uint32 idx = 0;
    if (LookupIndexWithHash(p_key, hash, idx)) {
      slots_[idx].value = p_value;
      return;
    }

    if (slots_ == nullptr ||
        size_ + 1 > static_cast<uint32>(kMaxOccupancy * capacity_)) {
      Grow(capacity_index_ + 1);
      if (LookupIndexWithHash(p_key, hash, idx)) {
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
  TValue& operator[](const TKey& p_key) {
    uint32 hash = ComputeHash(p_key);

    uint32 idx = 0;
    if (LookupIndexWithHash(p_key, hash, idx)) {
      return slots_[idx].value;
    }

    if (slots_ == nullptr ||
        size_ + 1 > static_cast<uint32>(kMaxOccupancy * capacity_)) {
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
  bool Erase(const TKey& p_key) {
    if (size_ == 0) {
      return false;
    }

    uint32 idx = 0;
    if (!LookupIndex(p_key, idx)) {
      return false;
    }

    DestroySlot(idx);

    uint32 capacity = capacity_;
    uint32 next = (idx + 1) % capacity;
    while (slots_[next].hash != kEmptyHash) {
      uint32 desired = slots_[next].hash % capacity;
      if (desired == next) {
        break;
      }
      if ((next > idx && (desired <= idx || desired > next)) ||
          (next < idx && (desired <= idx && desired > next))) {
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
  void Clear() {
    if (slots_ == nullptr || size_ == 0) {
      return;
    }

    for (uint32 i = 0; i < capacity_; ++i) {
      if (slots_[i].hash != kEmptyHash) {
        DestroySlot(i);
      }
    }
    size_ = 0;
  }

  /**
   * @brief 清空所有元素并释放内存
   */
  void Reset() {
    Clear();
    if (slots_ != nullptr) {
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
  void Reserve(uint32 p_size) {
    uint32 needed = static_cast<uint32>(p_size / kMaxOccupancy) + 1;
    uint32 idx = capacity_index_;
    while (idx < kPrimesCount && kPrimes[idx] < needed) {
      ++idx;
    }
    if (idx > capacity_index_) {
      Grow(idx);
    }
  }

 private:
  /**
   * @brief 质数容量表
   *
   * 索引 0~1 为占位（容量 0），有效容量从索引 2 开始（5, 13, 23, ...）。
   * 使用质数容量可使 @c hash % capacity 在探测过程中
   * 更均匀地覆盖整个表空间，减少聚集。
   */
  static constexpr uint32 kPrimes[] = {
      0,         0,         5,         13,       23,       53,        97,
      193,       389,       769,       1543,     3079,     6151,      12289,
      24593,     49157,     98317,     196613,   393241,   786433,    1572869,
      3145739,   6291469,   12582917,  25165843, 50331653, 100663319, 201326611,
      402653189, 805306457, 1610612741};
  /** @brief 质数表的元素个数 */
  static constexpr uint32 kPrimesCount = sizeof(kPrimes) / sizeof(kPrimes[0]);

  /**
   * @brief 计算键的哈希值，保证结果非零
   *
   * 若 Hasher 返回 0（与 kEmptyHash 冲突），则映射为 1，
   * 确保空槽标记的唯一性。
   *
   * @param[in] p_key 待计算哈希的键
   * @return 非零的 32 位哈希值
   */
  ARHUD_ALWAYS_INLINE uint32 ComputeHash(const TKey& p_key) const {
    uint32 h = Hasher::Hash(p_key);
    return (h == kEmptyHash) ? kEmptyHash + 1 : h;
  }

  /**
   * @brief 查找键所在的槽位索引（自动计算哈希）
   *
   * 内部调用 ComputeHash() 后委托给 LookupIndexWithHash()。
   *
   * @param[in] p_key 要查找的键
   * @param[out] r_idx 找到时输出槽位索引
   * @return true 找到
   * @return false 未找到或表为空
   */
  bool LookupIndex(const TKey& p_key, uint32& r_idx) const {
    return slots_ != nullptr && size_ > 0 &&
           LookupIndexWithHash(p_key, ComputeHash(p_key), r_idx);
  }

  /**
   * @brief 使用预计算哈希值查找键所在的槽位索引（线性探测）
   *
   * 从 @c p_hash % capacity_ 出发，逐槽线性探测：
   * 1. 遇到空槽（hash == kEmptyHash）→ 键不存在，返回 false
   * 2. 哈希值匹配且键相等 → 找到目标，通过 r_idx 输出索引
   * 3. 否则前进一格，直到绕回起点（表满且未找到）
   *
   * @param[in] p_key 要查找的键
   * @param[in] p_hash 预计算的哈希值（必须经 ComputeHash 保证非零）
   * @param[out] r_idx 找到时输出槽位索引
   * @return true 找到
   * @return false 未找到或容量为 0
   */
  bool LookupIndexWithHash(const TKey& p_key, uint32 p_hash,
                           uint32& r_idx) const {
    if (capacity_ == 0) {
      return false;
    }

    uint32 idx = p_hash % capacity_;
    uint32 start = idx;

    do {
      if (slots_[idx].hash == kEmptyHash) {
        return false;
      }
      if (slots_[idx].hash == p_hash &&
          Comparator::Compare(slots_[idx].key, p_key)) {
        r_idx = idx;
        return true;
      }
      idx = (idx + 1) % capacity_;
    } while (idx != start);

    return false;
  }

  /**
   * @brief 在第一个空槽位插入键值对（placement new）
   *
   * 从 @c p_hash % capacity_ 出发线性探测找到空槽，
   * 然后通过 placement new 原地构造键和值。
   * 不检查重复、不更新 size_，由调用者负责。
   *
   * @param[in] p_hash 键的哈希值
   * @param[in] p_key 键（拷贝构造）
   * @param[in] p_value 值（拷贝构造）
   */
  void InsertInternal(uint32 p_hash, const TKey& p_key, const TValue& p_value) {
    uint32 idx = p_hash % capacity_;
    while (slots_[idx].hash != kEmptyHash) {
      idx = (idx + 1) % capacity_;
    }

    slots_[idx].hash = p_hash;
    ::new (static_cast<void*>(&slots_[idx].key)) TKey(p_key);
    ::new (static_cast<void*>(&slots_[idx].value)) TValue(p_value);
  }

  /**
   * @brief 销毁指定槽位的键和值，并标记为空槽
   *
   * 对 trivially destructible 类型跳过析构调用（编译期优化）。
   *
   * @param[in] p_idx 要销毁的槽位索引
   */
  void DestroySlot(uint32 p_idx) {
    if constexpr (!std::is_trivially_destructible_v<TKey>) {
      slots_[p_idx].key.~TKey();
    }
    if constexpr (!std::is_trivially_destructible_v<TValue>) {
      slots_[p_idx].value.~TValue();
    }
    slots_[p_idx].hash = kEmptyHash;
  }

  /**
   * @brief 扩容并重新哈希所有元素
   *
   * 按质数容量表增长，将旧槽位中的所有键值对迁移到新数组。
   * 迁移完成后销毁旧元素并释放旧内存。
   *
   * 流程：
   * 1. 将 p_new_capacity_index 钳位到 [kMinCapacityIndex, kPrimesCount-1]
   * 2. 分配新槽位数组并零初始化（所有槽位标记为空）
   * 3. 遍历旧数组，将非空槽位 InsertInternal 到新数组
   * 4. 逐个析构旧数组中的元素
   * 5. 释放旧数组内存
   *
   * @param[in] p_new_capacity_index 目标容量在 kPrimes 表中的索引
   */
  void Grow(uint32 p_new_capacity_index) {
    if (p_new_capacity_index >= kPrimesCount) {
      p_new_capacity_index = kPrimesCount - 1;
    }
    if (p_new_capacity_index < kMinCapacityIndex) {
      p_new_capacity_index = kMinCapacityIndex;
    }

    Slot* old_slots = slots_;
    uint32 old_capacity = capacity_;

    capacity_index_ = p_new_capacity_index;
    capacity_ = kPrimes[capacity_index_];
    size_ = 0;

    slots_ = static_cast<Slot*>(memory::Alloc(sizeof(Slot) * capacity_));
    std::memset(slots_, 0, sizeof(Slot) * capacity_);

    for (uint32 i = 0; i < old_capacity; ++i) {
      if (old_slots[i].hash != kEmptyHash) {
        InsertInternal(old_slots[i].hash, old_slots[i].key, old_slots[i].value);
        ++size_;

        if constexpr (!std::is_trivially_destructible_v<TKey>) {
          old_slots[i].key.~TKey();
        }
        if constexpr (!std::is_trivially_destructible_v<TValue>) {
          old_slots[i].value.~TValue();
        }
      }
    }

    if (old_slots != nullptr) {
      memory::Free(old_slots);
    }
  }

  Slot* slots_ = nullptr;     /**< 槽位数组指针（由 memory::Alloc 分配） */
  uint32 capacity_ = 0;       /**< 当前容量（质数表中的某个值） */
  uint32 size_ = 0;           /**< 已存储的键值对数量 */
  uint32 capacity_index_ = 0; /**< 当前容量在 kPrimes 表中的索引 */
};

}  // namespace arhud

#endif  // ARHUD_CORE_TEMPLATE_HASH_MAP_H_
