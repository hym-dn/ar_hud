/**
 * @file rid.h
 * @brief 资源 ID 系统（RID + RIDOwner + DEFINE_ID）
 *
 * 参考 Godot 4.6 core/templates/rid.h 设计，实现基于代际索引的不透明资源 ID。
 *
 * 核心机制：
 *   - RID 是 64 位不透明 ID，隐藏内部指针，保证 API 安全性
 *   - 低 32 位 = 内部条目索引（O(1） 数组查找）
 *   - 高 32 位 = 世代计数器（释放后递增，检测 use-after-free）
 *   - 空闲链表复用已释放槽位，避免数组无限增长
 *   - RIDOwner 管理 RID 到对象指针的双向映射
 *
 * 设计取舍：
 *   - 代际索引而非指针编码：安全、可验证、支持 HashMap 键
 *   - LocalVector 存储条目：缓存友好，无间接寻址开销
 *   - List 维护空闲链表：O(1） 复用释放槽位
 *   - 可选线程安全：通过模板参数控制，无锁路径零开销
 *   - 不持有对象所有权：只管理映射关系，调用者负责对象生命周期
 *
 * 典型用途：
 * - GPU 资源管理（BufferID / TextureID / ShaderID / PipelineID）
 * - RenderingDevice 资源生命周期（RID_Owner<Buffer> uniform_buffer_owner）
 * - 依赖追踪（HashMap<RID, HashSet<RID>> dependency_map）
 *
 * 使用示例：
 * @code
 *   // 定义强类型 ID
 *   DEFINE_ID(Buffer);
 *   DEFINE_ID(Texture);
 *
 *   // 创建资源所有者
 *   RIDOwner<GpuBuffer> buffer_owner;
 *
 *   // 分配资源 ID
 *   auto* buf = new GpuBuffer();
 *   BufferID bid = BufferID(buffer_owner.MakeRid(buf));
 *
 *   // 通过 ID 查找对象
 *   GpuBuffer* ptr = buffer_owner.GetOrNull(bid.GetRid());
 *
 *   // 释放资源
 *   buffer_owner.Free(bid.GetRid());
 *   delete buf;
 * @endcode
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-06
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <cstddef>

#include "os/memory.h"
#include "os/sync.h"
#include "template/list.h"
#include "template/local_vector.h"
#include "typedefs.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // RID — 不透明资源 ID
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 不透明资源标识符（Resource Identifier）
     *
     * 64 位不透明 ID，内部使用代际索引编码：
     *   - bits [31:0]   → 条目索引（指向 RIDOwner 内部数组的下标）
     *   - bits [63:32]  → 世代计数器（每次释放后递增，用于检测悬空引用）
     *
     * 默认构造的 RID（id_=0）表示无效/空 ID。
     * 有效 RID 的 id_ 始终非零（索引从 1 开始，或世代非零）。
     *
     * 可作为 HashMap 键（提供 GetHash() 和 operator==）。
     */
    class RID
    {
        uint64_t id_ = 0;

    public:
        /**
         * @brief 构造无效 RID
         */
        ARHUD_ALWAYS_INLINE RID() = default;

        /**
         * @brief 从原始 64 位值构造（仅供 RIDOwner 内部使用）
         * @param[in] p_id 原始 ID 值
         */
        explicit ARHUD_ALWAYS_INLINE RID(uint64_t p_id)
            : id_(p_id)
        {
        }

        /**
         * @brief 检查是否为有效 RID
         * @return true 非空且有效
         */
        ARHUD_ALWAYS_INLINE bool IsValid() const
        {
            return id_ != 0;
        }

        /**
         * @brief 检查是否为空 RID
         * @return true 为空（id_ == 0）
         */
        ARHUD_ALWAYS_INLINE bool IsNull() const
        {
            return id_ == 0;
        }

        /**
         * @brief 获取原始 64 位 ID 值
         * @return 原始 ID
         */
        ARHUD_ALWAYS_INLINE uint64_t GetId() const
        {
            return id_;
        }

        /**
         * @brief 提取低 32 位作为本地索引
         *
         * 用于 O(1) 数组索引访问。此值仅对创建该 RID 的 RIDOwner 有意义，
         * 不同 RIDOwner 实例间不可混用。
         *
         * @return 条目索引（0 表示无效）
         */
        ARHUD_ALWAYS_INLINE uint32_t GetLocalIndex() const
        {
            return static_cast<uint32_t>(id_ & 0xFFFFFFFFULL);
        }

        /**
         * @brief 相等比较
         */
        ARHUD_ALWAYS_INLINE bool operator==(const RID &p_other) const
        {
            return id_ == p_other.id_;
        }

        /**
         * @brief 不等比较
         */
        ARHUD_ALWAYS_INLINE bool operator!=(const RID &p_other) const
        {
            return id_ != p_other.id_;
        }

        /**
         * @brief 小于比较（用于有序容器）
         */
        ARHUD_ALWAYS_INLINE bool operator<(const RID &p_other) const
        {
            return id_ < p_other.id_;
        }

        /**
         * @brief 计算哈希值（用于 HashMap 键）
         *
         * 对 64 位 ID 做位移混合哈希，与 DefaultHasher<uint64_t> 一致。
         *
         * @return 32 位哈希值
         */
        ARHUD_ALWAYS_INLINE uint32_t GetHash() const
        {
            uint64_t h = id_;
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33;
            h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return static_cast<uint32_t>(h) ^ static_cast<uint32_t>(h >> 32);
        }
    };

    // ═══════════════════════════════════════════════════════════════════════
    // RIDOwner — 资源 ID 所有者
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 资源 ID 管理器（RID → 对象指针映射）
     *
     * 管理不透明 RID 与对象指针之间的双向映射。
     * 内部使用代际索引 + 空闲链表实现：
     *   - MakeRid(): 分配新 RID 或复用空闲槽位
     *   GetOrNull(): 通过 RID O(1) 查找对象指针
     *   Free(): 释放 RID 并将槽位加入空闲链表
     *   - 世代计数器确保已释放的 RID 无法再找到对象（use-after-free 安全）
     *
     * 不持有对象所有权——调用者负责对象的分配和销毁。
     * RIDOwner 只维护 RID ↔ 指针的映射关系。
     *
     * @tparam T          托管的对象类型
     * @tparam kThreadSafe 是否启用线程安全（默认 false，无额外开销）
     *
     * @note 线程安全模式使用 Mutex 保护所有操作，适用于跨线程资源共享场景。
     *       但在单线程渲染循环中应保持关闭以获得最佳性能。
     */
    template <typename T, bool kThreadSafe = false>
    class RIDOwner
    {
        struct Entry
        {
            T *object_ = nullptr;     ///< 托管的对象指针（不拥有所有权）
            uint32_t generation_ = 0; ///< 世代计数器（释放后递增）
            bool valid_ = false;      ///< 当前是否有效
        };

        LocalVector<Entry> entries_;
        List<uint32_t> free_list_;

#if ARHUD_DEBUG
        uint32_t alloc_count_ = 0; ///< Debug: 当前活跃分配数
#endif

    public:
        RIDOwner() = default;

        ~RIDOwner()
        {
            Clear();
        }

        ARHUD_DISABLE_COPY(RIDOwner);

        /**
         * @brief 为对象分配新的 RID
         *
         * 若空闲链表有可用槽位则复用（并递增世代计数器），
         * 否则在数组末尾追加新条目。
         *
         * @param[in] p_object 对象指针（不能为 nullptr）
         *
         * @return 新分配的 RID
         *
         * @pre p_object != nullptr
         */
        RID MakeRid(T *p_object)
        {
            ARHUD_ASSERT(p_object != nullptr, "RIDOwner::MakeRid() object must not be null");

#if kThreadSafe
            LockGuard lock(mutex_);
#endif

            uint32_t index;
            uint32_t generation;

            if (!free_list_.IsEmpty())
            {
                index = free_list_.Front()->Get();
                free_list_.Erase(free_list_.Front());

                Entry &entry = entries_[index];
                ARHUD_ASSERT(!entry.valid_, "RIDOwner::MakeRid() free slot should be invalid");
                generation = entry.generation_ + 1;
                entry.object_ = p_object;
                entry.generation_ = generation;
                entry.valid_ = true;
            }
            else
            {
                index = static_cast<uint32_t>(entries_.Size()) + 1;
                generation = 1;
                Entry entry;
                entry.object_ = p_object;
                entry.generation_ = generation;
                entry.valid_ = true;
                entries_.PushBack(entry);
            }

#if ARHUD_DEBUG
            ++alloc_count_;
#endif

            return RID((static_cast<uint64_t>(generation) << 32) | index);
        }

        /**
         * @brief 通过 RID 查找对象指针
         *
         * O(1) 数组索引查找 + 世代校验。
         * 若 RID 无效、索引越界、或世代不匹配，返回 nullptr。
         *
         * @param[in] p_rid 要查询的 RID
         *
         * @return 对象指针，未找到返回 nullptr
         */
        T *GetOrNull(const RID &p_rid)
        {
            if (ARHUD_UNLIKELY(p_rid.IsNull()))
            {
                return nullptr;
            }

#if kThreadSafe
            LockGuard lock(mutex_);
#endif

            uint32_t index = p_rid.GetLocalIndex();
            if (index == 0 || index > entries_.Size())
            {
                return nullptr;
            }

            const Entry &entry = entries_[index - 1];
            uint32_t expected_gen = static_cast<uint32_t>(p_rid.GetId() >> 32);

            if (ARHUD_UNLIKELY(!entry.valid_ || entry.generation_ != expected_gen))
            {
                return nullptr;
            }

            return entry.object_;
        }

        /**
         * @brief 通过 RID 查找对象指针（const 版本）
         *
         * @param[in] p_rid 要查询的 RID
         *
         * @return 对象指针（const），未找到返回 nullptr
         */
        const T *GetOrNull(const RID &p_rid) const
        {
            if (ARHUD_UNLIKELY(p_rid.IsNull()))
            {
                return nullptr;
            }

#if kThreadSafe
            LockGuard lock(mutex_);
#endif

            uint32_t index = p_rid.GetLocalIndex();
            if (index == 0 || index > entries_.Size())
            {
                return nullptr;
            }

            const Entry &entry = entries_[index - 1];
            uint32_t expected_gen = static_cast<uint32_t>(p_rid.GetId() >> 32);

            if (ARHUD_UNLIKELY(!entry.valid_ || entry.generation_ != expected_gen))
            {
                return nullptr;
            }

            return entry.object_;
        }

        /**
         * @brief 检查本所有者是否拥有指定 RID
         *
         * @param[in] p_rid 要检查的 RID
         *
         * @return true 本所有者拥有该 RID 且当前有效
         */
        bool Owns(const RID &p_rid) const
        {
            if (p_rid.IsNull())
            {
                return false;
            }

#if kThreadSafe
            LockGuard lock(mutex_);
#endif

            uint32_t index = p_rid.GetLocalIndex();
            if (index == 0 || index > entries_.Size())
            {
                return false;
            }

            const Entry &entry = entries_[index - 1];
            uint32_t expected_gen = static_cast<uint32_t>(p_rid.GetId() >> 32);

            return entry.valid_ && entry.generation_ == expected_gen;
        }

        /**
         * @brief 释放指定的 RID
         *
         * 将条目标记为无效，递增世代计数器，并将索加入空闲链表。
         * 释放后的 RID 再调用 GetOrNull() 将返回 nullptr（世代不匹配）。
         *
         * @param[in] p_rid 要释放的 RID
         *
         * @return true 成功释放
         * @retval false RID 无效或不属于本所有者
         */
        bool Free(const RID &p_rid)
        {
            if (p_rid.IsNull())
            {
                return false;
            }

#if kThreadSafe
            LockGuard lock(mutex_);
#endif

            uint32_t index = p_rid.GetLocalIndex();
            if (index == 0 || index > entries_.Size())
            {
                return false;
            }

            Entry &entry = entries_[index - 1];
            uint32_t expected_gen = static_cast<uint32_t>(p_rid.GetId() >> 32);

            if (ARHUD_UNLIKELY(!entry.valid_ || entry.generation_ != expected_gen))
            {
                return false;
            }

            entry.valid_ = false;
            entry.object_ = nullptr;
            ++entry.generation_;

            free_list_.PushBack(index);

#if ARHUD_DEBUG
            --alloc_count_;
#endif

            return true;
        }

        /**
         * @brief 释放所有 RID 并清空映射
         *
         * 清除所有有效条目，重置空闲链表和内部数组。
         * 不销毁托管的对象（不持有所有权）。
         */
        void Clear()
        {
#if kThreadSafe
            LockGuard lock(mutex_);
#endif

            free_list_.Clear();

            for (uint32_t i = 0; i < entries_.Size(); ++i)
            {
                entries_[i].object_ = nullptr;
                entries_[i].valid_ = false;
                entries_[i].generation_ = 0;
            }

            entries_.Clear();

#if ARHUD_DEBUG
            alloc_count_ = 0;
#endif
        }

        /**
         * @brief 获取当前活跃 RID 数量
         * @return 活跃数量
         */
        ARHUD_ALWAYS_INLINE uint32_t Size() const
        {
#if ARHUD_DEBUG
            return alloc_count_;
#else
#if kThreadSafe
            LockGuard lock(mutex_);
#endif
            uint32_t count = 0;
            for (uint32_t i = 0; i < entries_.Size(); ++i)
            {
                if (entries_[i].valid_)
                {
                    ++count;
                }
            }
            return count;
#endif
        }

        /**
         * @brief 检查是否为空
         * @return true 无活跃 RID
         */
        ARHUD_ALWAYS_INLINE bool IsEmpty() const
        {
            return Size() == 0;
        }

        /**
         * @brief 遍历所有活跃条目
         *
         * 对每个有效条目调用回调函数，传入 RID 和对象指针。
         * 可用于批量释放资源等场景。
         *
         * @param[in] p_callback 回调函数，签名 void(RID, T*)
         *
         * @note 回调中不应修改 RIDOwner（增删 RID），否则行为未定义
         */
        template <typename F>
        void ForEach(F &&p_callback)
        {
#if kThreadSafe
            LockGuard lock(mutex_);
#endif

            for (uint32_t i = 0; i < entries_.Size(); ++i)
            {
                const Entry &entry = entries_[i];
                if (entry.valid_ && entry.object_ != nullptr)
                {
                    uint32_t index = i + 1;
                    uint64_t rid_id = (static_cast<uint64_t>(entry.generation_) << 32) | static_cast<uint64_t>(index);
                    p_callback(RID(rid_id), entry.object_);
                }
            }
        }

    private:
#if kThreadSafe
        Mutex mutex_;
#endif

        struct LockGuard
        {
#if kThreadSafe
            Mutex &mutex_ref_;
            explicit LockGuard(Mutex &p_mutex) : mutex_ref_(p_mutex) { mutex_ref_.Lock(); }
            ~LockGuard() { mutex_ref_.Unlock(); }
#else
            explicit LockGuard(Mutex &)
            {
            }
#endif
        };
    };

    // ═══════════════════════════════════════════════════════════════════════
    // DEFINE_ID — 强类型 ID 定义宏
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @def DEFINE_ID(m_name)
     * @brief 定义强类型资源 ID 类
     *
     * 在 RID 外层包装类型安全的 ID 类，防止不同类型资源 ID 混用。
     * 生成的类名为 `m_nameID`，例如：
     *   @code
     *     DEFINE_ID(Buffer);    // 生成 BufferID 类
     *     DEFINE_ID(Texture);   // 生成 TextureID 类
     *     DEFINE_ID(Shader);    // 生成 ShaderID 类
     *   @endcode
     *
     * 每个 ID 类型都包装一个 RID，提供类型安全的比较和有效性检查。
     * 可隐式转换为 RID 以便传入 RIDOwner::GetOrNull() 等通用接口。
     *
     * @param m_name ID 类型名称前缀（如 Buffer、Texture、Shader）
     */
#define DEFINE_ID(m_name)                                                                                     \
    class m_name##ID                                                                                          \
    {                                                                                                         \
        RID rid_;                                                                                             \
                                                                                                              \
    public:                                                                                                   \
        m_name##ID() = default;                                                                               \
        explicit m_name##ID(const RID &p_rid) : rid_(p_rid) {}                                                \
        explicit m_name##ID(uint64_t p_id) : rid_(RID(p_id)) {}                                               \
                                                                                                              \
        ARHUD_ALWAYS_INLINE bool operator==(const m_name##ID &p_other) const { return rid_ == p_other.rid_; } \
        ARHUD_ALWAYS_INLINE bool operator!=(const m_name##ID &p_other) const { return rid_ != p_other.rid_; } \
        ARHUD_ALWAYS_INLINE bool IsValid() const { return rid_.IsValid(); }                                   \
        ARHUD_ALWAYS_INLINE bool IsNull() const { return rid_.IsNull(); }                                     \
        ARHUD_ALWAYS_INLINE RID GetRid() const { return rid_; }                                               \
        ARHUD_ALWAYS_INLINE uint64_t GetId() const { return rid_.GetId(); }                                   \
        ARHUD_ALWAYS_INLINE uint32_t GetLocalIndex() const { return rid_.GetLocalIndex(); }                   \
        ARHUD_ALWAYS_INLINE uint32_t GetHash() const { return rid_.GetHash(); }                               \
    }

} // namespace arhud
