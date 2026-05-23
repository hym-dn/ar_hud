/**
 * @file paged_allocator.h
 * @brief 分页内存分配器，用于固定大小小对象的高效分配/释放
 *
 * @author yameng.he
 * @version 1.1
 * @date 2026-05-02
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <type_traits>

#include "os/memory.h"
#include "os/sync.h"
#include "typedefs.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // 辅助函数
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 计算最近的 2 的幂（向上取整）
     *
     * @param[in] p_number 输入正整数
     *
     * @return >= p_number 的最小 2 的幂
     */
    ARHUD_ALWAYS_INLINE uint32 NearestPow2(uint32 p_number)
    {
        if (p_number == 0)
        {
            return 1;
        }
        --p_number;
        p_number |= p_number >> 1;
        p_number |= p_number >> 2;
        p_number |= p_number >> 4;
        p_number |= p_number >> 8;
        p_number |= p_number >> 16;
        return ++p_number;
    }

    /**
     * @brief 从 2 的幂值计算移位数
     *
     * @param[in] p_value 2 的幂值（必须 > 0）
     *
     * @return log2(p_value)，若非 2 的幂则返回 0
     */
    ARHUD_ALWAYS_INLINE uint32 ShiftFromPow2(uint32 p_value)
    {
        for (uint32 i = 0; i < 32; ++i)
        {
            if (p_value == (uint32(1) << i))
            {
                return i;
            }
        }
        return 0;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // NoOpLock
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 空操作锁，用于 PagedAllocator 单线程模式的零开销抽象
     *
     * 所有方法均为内联空函数，编译器在非线程安全模式下完全消除锁开销。
     */
    class NoOpLock
    {
    public:
        void Lock() {}
        void Unlock() {}
    };

    // ═══════════════════════════════════════════════════════════════════════
    // PagedAllocator
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 分页内存分配器
     *
     * 按 page_size 为单位批量分配内存页，每页包含 page_size 个 T 对象槽位。
     * 使用 LIFO 可用槽位栈实现 O(1) 分配/释放，避免频繁调用全局分配器。
     *
     * 典型用途：
     * - 渲染命令节点分配（RenderingDeviceGraph）
     * - GPU 资源簿记结构分配（RenderingDevice）
     * - RID_Owner 内部对象存储
     *
     * 线程安全：通过 kThreadSafe 模板参数选择 SpinLock 或 NoOpLock，
     * 编译期确定，零运行时开销。
     *
     * 车规模式：调用 Preallocate() 在启动时预分配所有内存页，
     * 运行时不再调用 malloc，满足 QNX 安全要求。
     *
     * 内存布局：
     * @code
     * available_pool_ 是二维 T* 数组，按 page_size 分页：
     *   available_pool_[page_idx][slot_idx]
     * 全局栈索引 → 页+偏移的映射：
     *   page_idx = global_index >> page_shift_
     *   slot_idx = global_index &  page_mask_
     * @endcode
     *
     * @tparam T            分配的对象类型
     * @tparam kThreadSafe  是否线程安全（true 时内部使用 SpinLock）
     * @tparam kPageSize    每页包含的槽位数量（自动对齐到 2 的幂）
     *
     * @pre alignof(T) <= alignof(std::max_align_t)（即 T 不需要超对齐）
     */
    template <typename T, bool kThreadSafe = false, uint32 kPageSize = 256>
    class PagedAllocator
    {
        static_assert(alignof(T) <= alignof(std::max_align_t),
                      "PagedAllocator does not support over-aligned types");

        using LockType =
            typename std::conditional<kThreadSafe, SpinLock, NoOpLock>::type;

    public:
        /**
         * @brief 构造分页分配器
         *
         * 自动将 page_size 对齐到最近的 2 的幂。
         */
        PagedAllocator();

        /**
         * @brief 析构分页分配器
         *
         * 释放所有已分配的内存页。
         * Debug 模式下若有未释放对象则触发断言。
         */
        ~PagedAllocator();

        PagedAllocator(const PagedAllocator &) = delete;
        PagedAllocator &operator=(const PagedAllocator &) = delete;

        /**
         * @brief 分配一个 T 对象（从可用池中取出一个槽位，就地构造）
         *
         * 若可用池为空则自动分配新页。
         * 线程安全模式下内部加锁。
         *
         * @tparam Args 构造函数参数类型
         * @param[in] p_args 传递给 T 构造函数的参数
         *
         * @return 指向新构造对象的指针
         */
        template <typename... Args>
        T *Alloc(Args &&...p_args);

        /**
         * @brief 释放一个 T 对象（析构后归还到可用池）
         *
         * Debug 模式下会验证 p_mem 是否属于本分配器。
         *
         * @param[in] p_mem 指向要释放的对象（必须由本分配器分配）
         *
         * @warning p_mem 必须是由本分配器 Alloc() 返回的指针
         */
        void Free(T *p_mem);

        /**
         * @brief 预分配指定数量的对象槽位
         *
         * 车规模式下在启动阶段调用，确保运行时不再触发 malloc。
         * 必须在首次 Alloc() 之前调用。
         *
         * @param[in] p_max_count 预分配的最大对象数量
         */
        void Preallocate(uint32 p_max_count);

        /**
         * @brief 重置分配器（释放所有内存）
         *
         * @param[in] p_allow_unfreed 是否允许存在未释放的对象
         *
         * @warning 若 p_allow_unfreed=false 且存在未释放对象，
         *          非平凡析构类型将触发断言
         */
        void Reset(bool p_allow_unfreed = false);

        /**
         * @brief 获取已分配的总对象数量
         *
         * 线程安全模式下内部加锁。
         *
         * @return 已分配但未释放的对象数
         */
        uint32 GetUsedCount() const;

        /**
         * @brief 获取已分配的内存页数量
         *
         * 线程安全模式下内部加锁。
         *
         * @return 内存页数
         */
        uint32 GetPageCount() const;

        /**
         * @brief 获取每页的槽位数量
         *
         * @return 每页槽位数（2 的幂）
         */
        uint32 GetPageSize() const;

    private:
        /**
         * @brief 分配一个新内存页
         *
         * 从全局分配器申请 page_size 个 T 对象的连续内存，
         * 并将所有槽位按全局栈索引映射加入可用池。
         */
        void Grow();

#ifdef ARHUD_DEBUG
        /**
         * @brief 验证指针是否属于本分配器的某一页
         *
         * 仅 Debug 构建启用，用于 Free() 的防御性检查。
         *
         * @param[in] p_mem 待验证指针
         *
         * @return true 表示指针属于本分配器
         */
        bool IsOwnedPointer(const T *p_mem) const;
#endif

        T **page_pool_ = nullptr;
        T ***available_pool_ = nullptr;
        uint32 pages_allocated_ = 0;
        uint32 allocs_available_ = 0;

        uint32 page_shift_ = 0;
        uint32 page_mask_ = 0;
        uint32 page_size_ = 0;

        LockType lock_;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // PagedAllocator 模板实现
    // ═══════════════════════════════════════════════════════════════════════

    template <typename T, bool kThreadSafe, uint32 kPageSize>
    PagedAllocator<T, kThreadSafe, kPageSize>::PagedAllocator()
    {
        page_size_ = NearestPow2(kPageSize);
        page_mask_ = page_size_ - 1;
        page_shift_ = ShiftFromPow2(page_size_);
    }

    template <typename T, bool kThreadSafe, uint32 kPageSize>
    PagedAllocator<T, kThreadSafe, kPageSize>::~PagedAllocator()
    {
        lock_.Lock();

        bool leaked = allocs_available_ < pages_allocated_ * page_size_;
        if (leaked)
        {
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                ARHUD_ASSERT(allocs_available_ == pages_allocated_ * page_size_,
                             "PagedAllocator has unfreed objects at destruction");
            }
        }

        for (uint32 i = 0; i < pages_allocated_; ++i)
        {
            memory::Free(available_pool_[i]);
        }
        for (uint32 i = 0; i < pages_allocated_; ++i)
        {
            memory::FreeAligned(page_pool_[i]);
        }
        if (page_pool_ != nullptr)
        {
            memory::Free(page_pool_);
            memory::Free(available_pool_);
        }

        page_pool_ = nullptr;
        available_pool_ = nullptr;
        pages_allocated_ = 0;
        allocs_available_ = 0;

        lock_.Unlock();
    }

    template <typename T, bool kThreadSafe, uint32 kPageSize>
    void PagedAllocator<T, kThreadSafe, kPageSize>::Grow()
    {
        uint32 pages_used = pages_allocated_;
        pages_allocated_++;

        page_pool_ = static_cast<T **>(
            memory::Realloc(page_pool_, sizeof(T *) * pages_allocated_));
        available_pool_ = static_cast<T ***>(
            memory::Realloc(available_pool_, sizeof(T **) * pages_allocated_));

        page_pool_[pages_used] = static_cast<T *>(
            memory::AllocAligned(sizeof(T) * page_size_, alignof(T)));
        available_pool_[pages_used] =
            static_cast<T **>(memory::Alloc(sizeof(T *) * page_size_));

        for (uint32 i = 0; i < page_size_; ++i)
        {
            uint32 global_idx = allocs_available_ + i;
            available_pool_[global_idx >> page_shift_][global_idx & page_mask_] =
                &page_pool_[pages_used][i];
        }
        allocs_available_ += page_size_;
    }

#ifdef ARHUD_DEBUG
    template <typename T, bool kThreadSafe, uint32 kPageSize>
    bool PagedAllocator<T, kThreadSafe, kPageSize>::IsOwnedPointer(const T *p_mem) const
    {
        for (uint32 i = 0; i < pages_allocated_; ++i)
        {
            const T *page_begin = page_pool_[i];
            const T *page_end = page_begin + page_size_;
            if (p_mem >= page_begin && p_mem < page_end)
            {
                uintptr_t offset = reinterpret_cast<uintptr_t>(p_mem) -
                                   reinterpret_cast<uintptr_t>(page_begin);
                return (offset % sizeof(T)) == 0;
            }
        }
        return false;
    }
#endif

    template <typename T, bool kThreadSafe, uint32 kPageSize>
    template <typename... Args>
    T *PagedAllocator<T, kThreadSafe, kPageSize>::Alloc(Args &&...p_args)
    {
        lock_.Lock();

        if (allocs_available_ == 0)
        {
            Grow();
        }

        allocs_available_--;
        T *ptr = available_pool_[allocs_available_ >> page_shift_]
                                [allocs_available_ & page_mask_];

        lock_.Unlock();

        return new (ptr) T(std::forward<Args>(p_args)...);
    }

    template <typename T, bool kThreadSafe, uint32 kPageSize>
    void PagedAllocator<T, kThreadSafe, kPageSize>::Free(T *p_mem)
    {
#ifdef ARHUD_DEBUG
        ARHUD_ASSERT(p_mem != nullptr, "PagedAllocator::Free(nullptr)");
        ARHUD_ASSERT(IsOwnedPointer(p_mem),
                     "PagedAllocator::Free() pointer not owned by this allocator");
#endif

        lock_.Lock();

        p_mem->~T();

        available_pool_[allocs_available_ >> page_shift_]
                       [allocs_available_ & page_mask_] = p_mem;
        allocs_available_++;

        lock_.Unlock();
    }

    template <typename T, bool kThreadSafe, uint32 kPageSize>
    void PagedAllocator<T, kThreadSafe, kPageSize>::Preallocate(
        uint32 p_max_count)
    {
        lock_.Lock();

        uint32 pages_needed = (p_max_count + page_size_ - 1) / page_size_;
        while (pages_allocated_ < pages_needed)
        {
            Grow();
        }

        lock_.Unlock();
    }

    template <typename T, bool kThreadSafe, uint32 kPageSize>
    void PagedAllocator<T, kThreadSafe, kPageSize>::Reset(bool p_allow_unfreed)
    {
        lock_.Lock();

        if (!p_allow_unfreed && !std::is_trivially_destructible_v<T>)
        {
            ARHUD_ASSERT(allocs_available_ == pages_allocated_ * page_size_,
                         "PagedAllocator::Reset() called with unfreed objects");
        }

        for (uint32 i = 0; i < pages_allocated_; ++i)
        {
            memory::Free(available_pool_[i]);
        }
        for (uint32 i = 0; i < pages_allocated_; ++i)
        {
            memory::FreeAligned(page_pool_[i]);
        }
        if (page_pool_ != nullptr)
        {
            memory::Free(page_pool_);
            memory::Free(available_pool_);
        }

        page_pool_ = nullptr;
        available_pool_ = nullptr;
        pages_allocated_ = 0;
        allocs_available_ = 0;

        lock_.Unlock();
    }

    template <typename T, bool kThreadSafe, uint32 kPageSize>
    uint32 PagedAllocator<T, kThreadSafe, kPageSize>::GetUsedCount() const
    {
        lock_.Lock();
        uint32 count = pages_allocated_ * page_size_ - allocs_available_;
        lock_.Unlock();
        return count;
    }

    template <typename T, bool kThreadSafe, uint32 kPageSize>
    uint32 PagedAllocator<T, kThreadSafe, kPageSize>::GetPageCount() const
    {
        lock_.Lock();
        uint32 count = pages_allocated_;
        lock_.Unlock();
        return count;
    }

    template <typename T, bool kThreadSafe, uint32 kPageSize>
    uint32 PagedAllocator<T, kThreadSafe, kPageSize>::GetPageSize() const
    {
        return page_size_;
    }

} // namespace arhud
