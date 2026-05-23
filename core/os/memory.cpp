/**
 * @file memory.cpp
 * @brief 全局内存分配器实现（Godot 级）
 *
 * 底层使用系统堆分配（Windows HeapAlloc / POSIX malloc），
 * 提供可替换的 AllocatorHooks 后端。
 *
 * 三条分配路径：
 *   - Alloc/Free：          Debug 嵌入 AllocHeader 精确追踪，Release 零开销
 *   - AllocStatic/FreeStatic：p_pad_align=true 始终带头（Release 需 element_count_ 析构数组）
 *   - AllocAligned/FreeAligned：对齐分配，Debug 用映射表追踪精确大小
 *
 * Debug vs Release 行为差异：
 *   - Alloc/Free：           Debug 带头追踪+金丝雀，Release 不带头（零开销）
 *   - 单对象（ARHUD_NEW）：  Debug 带头追踪，Release 不带头（零开销）
 *   - 数组（ARHUD_NEW_ARR）： 始终带头（Release 也需要 element_count_ 析构）
 *   - 金丝雀检测：           Debug 启用头尾校验，Release 不分配尾部金丝雀
 *   - 统计追踪：             Debug 精确双向追踪，Release 编译为零
 *
 * @author yameng.he
 * @version 1.1
 * @date 2026-03-23
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#include "os/memory.h"
#include "io/logger.h"
#include "math_funcs.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

// 平台特定头文件（仅用于 GetMemAvailable / GetMemUsage，分配器统一使用 malloc）
#if defined(ARHUD_PLATFORM_WINDOWS)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <unistd.h>
#endif

namespace arhud::memory
{
    namespace
    {
        // ═══════════════════════════════════════════════════════════════════════
        // 全局状态
        // ═══════════════════════════════════════════════════════════════════════

        struct GlobalState
        {
            // 统计（原子计数器，精确双向追踪）
            std::atomic<size_t> current_usage_{0};
            std::atomic<size_t> peak_usage_{0};
            std::atomic<size_t> allocation_count_{0};
            std::atomic<size_t> total_alloc_calls_{0};
            std::atomic<size_t> total_free_calls_{0};
            // 预算（0 = 不限制）
            std::atomic<size_t> budget_{0};
            // 自定义 Hook（初始化时设置，之后不再加锁，热路径零开销）
            AllocatorHooks hooks_{};
#if ARHUD_DEBUG
            std::unordered_map<const void *, size_t> aligned_alloc_sizes_;
#endif
        };

        GlobalState g_state;

        // ═══════════════════════════════════════════════════════════════════════
        // Debug 分配来源追踪（仅 ARHUD_DEBUG 模式）
        // ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
        /**
         * @brief 单次分配的来源记录
         */
        struct AllocRecord
        {
            const char *file; ///< 分配时的源文件名
            int line;         ///< 分配时的行号
            size_t size;      ///< 分配的字节数
        };

        /** @brief 分配来源追踪表：用户指针 → AllocRecord */
        std::unordered_map<const void *, AllocRecord> g_alloc_records;

        /** @brief 追踪表互斥锁（std::unordered_map 内部用 malloc，无循环依赖） */
        std::mutex g_alloc_records_mutex;

        /** @brief 记录一次带来源的分配 */
        void RecordAlloc(const void *p_ptr, size_t p_size, const char *p_file, int p_line)
        {
            if (p_ptr == nullptr)
                return;
            std::lock_guard<std::mutex> lock(g_alloc_records_mutex);
            g_alloc_records[p_ptr] = {p_file, p_line, p_size};
        }

        /** @brief 移除一次分配记录（Free 时调用，指针不存在则静默忽略） */
        void RecordFree(const void *p_ptr)
        {
            if (p_ptr == nullptr)
                return;
            std::lock_guard<std::mutex> lock(g_alloc_records_mutex);
            g_alloc_records.erase(p_ptr);
        }

        /** @brief Realloc 时更新记录（旧指针移除，新指针注册） */
        void RecordRealloc(const void *p_old_ptr, const void *p_new_ptr, size_t p_size,
                           const char *p_file, int p_line)
        {
            std::lock_guard<std::mutex> lock(g_alloc_records_mutex);
            if (p_old_ptr != nullptr)
                g_alloc_records.erase(p_old_ptr);
            if (p_new_ptr != nullptr)
                g_alloc_records[p_new_ptr] = {p_file, p_line, p_size};
        }
#endif

        // ═══════════════════════════════════════════════════════════════════════
        // 平台堆后端
        // ═══════════════════════════════════════════════════════════════════════

        // 统一使用 malloc/free/realloc（全平台可用，MSVC 下为 UCRT 分段堆）
        // 内存清零路径显式调用 memset 而非 HeapAlloc(HEAP_ZERO_MEMORY)，避免平台差异

        void *SysAlloc(size_t p_size)
        {
            return std::malloc(p_size);
        }

        void *SysAllocZeroed(size_t p_size)
        {
            void *p = std::malloc(p_size);
            if (p)
                std::memset(p, 0, p_size);
            return p;
        }

        void *SysRealloc(void *p, size_t s)
        {
            return std::realloc(p, s);
        }

        void SysFree(void *p)
        {
            std::free(p);
        }

        // ═══════════════════════════════════════════════════════════════════════
        // 后端分配器（处理 Hook 转发）— 热路径，强制内联
        // ═══════════════════════════════════════════════════════════════════════

        ARHUD_ALWAYS_INLINE void *BackendAlloc(size_t p_size)
        {
            AllocatorHooks h = g_state.hooks_;
            if (h.alloc_fn != nullptr)
            {
                return h.alloc_fn(p_size);
            }
            return SysAlloc(p_size);
        }

        ARHUD_ALWAYS_INLINE void *BackendAllocZeroed(size_t p_size)
        {
            AllocatorHooks h = g_state.hooks_;
            if (h.alloc_fn != nullptr)
            {
                void *ptr = h.alloc_fn(p_size);
                if (ptr != nullptr)
                {
                    std::memset(ptr, 0, p_size);
                }
                return ptr;
            }
            return SysAllocZeroed(p_size);
        }

        inline void *BackendRealloc(void *p_ptr, size_t p_size)
        {
            AllocatorHooks h = g_state.hooks_;
            if (h.realloc_fn != nullptr)
            {
                return h.realloc_fn(p_ptr, p_size);
            }
            return SysRealloc(p_ptr, p_size);
        }

        ARHUD_ALWAYS_INLINE void BackendFree(void *p_ptr)
        {
            if (p_ptr == nullptr)
                return;
            AllocatorHooks h = g_state.hooks_;
            if (h.free_fn != nullptr)
            {
                h.free_fn(p_ptr);
                return;
            }
            SysFree(p_ptr);
        }

        // ═══════════════════════════════════════════════════════════════════════
        // 统计追踪（Debug 构建启用，Release 编译为零）
        // ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
        ARHUD_ALWAYS_INLINE void TrackAlloc(size_t p_size)
        {
            g_state.allocation_count_.fetch_add(1, std::memory_order_relaxed);
            g_state.total_alloc_calls_.fetch_add(1, std::memory_order_relaxed);
            size_t prev = g_state.current_usage_.fetch_add(p_size, std::memory_order_relaxed);
            size_t new_usage = prev + p_size;

            size_t peak = g_state.peak_usage_.load(std::memory_order_relaxed);
            while (new_usage > peak)
            {
                if (g_state.peak_usage_.compare_exchange_weak(
                        peak, new_usage, std::memory_order_relaxed))
                {
                    break;
                }
            }
        }

        ARHUD_ALWAYS_INLINE void TrackFree(size_t p_size)
        {
            g_state.allocation_count_.fetch_sub(1, std::memory_order_relaxed);
            g_state.total_free_calls_.fetch_add(1, std::memory_order_relaxed);
            g_state.current_usage_.fetch_sub(p_size, std::memory_order_relaxed);
        }

        ARHUD_ALWAYS_INLINE bool IsOverBudget(size_t p_requested)
        {
            size_t b = g_state.budget_.load(std::memory_order_relaxed);
            if (b == 0)
                return false;
            size_t cur = g_state.current_usage_.load(std::memory_order_relaxed);
            return cur + p_requested > b;
        }
#else
        // Release 空实现 — 编译期消除，零开销
#define TrackAlloc(p_size) ((void)0)
#define TrackFree(p_size) ((void)0)
        ARHUD_ALWAYS_INLINE bool IsOverBudget(size_t) { return false; }
#endif

        // ═══════════════════════════════════════════════════════════════════════
        // 对齐分配辅助
        // ═══════════════════════════════════════════════════════════════════════

        ARHUD_ALWAYS_INLINE void *AlignForward(void *p_ptr, size_t p_alignment)
        {
            uintptr_t addr = reinterpret_cast<uintptr_t>(p_ptr);
            uintptr_t aligned = (addr + p_alignment - 1) & ~(p_alignment - 1);
            return reinterpret_cast<void *>(aligned);
        }

        /**
         * 整体分配对齐内存。
         *
         * 布局：
         *   [raw_block]
         *   [overhead_ptr  sizeof(void*)]  ← 存储 raw 指针
         *   [padding]
         *   [aligned_data]                  ← 返回此指针
         */
        void *AlignedAllocRaw(size_t p_size, size_t p_alignment)
        {
            size_t total = p_size + p_alignment + sizeof(void *);
            void *raw = BackendAlloc(total);
            if (raw == nullptr)
                return nullptr;
            void *aligned = AlignForward(
                static_cast<uint8_t *>(raw) + sizeof(void *), p_alignment);
            void **slot = static_cast<void **>(aligned) - 1;
            *slot = raw;
            return aligned;
        }

        void AlignedFreeRaw(void *p_ptr)
        {
            if (p_ptr == nullptr)
                return;
            void **slot = static_cast<void **>(p_ptr) - 1;
            void *raw = *slot;
            BackendFree(raw);
        }

        // ═══════════════════════════════════════════════════════════════════════
        // 金丝雀校验辅助
        // ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
        inline void ValidateCanaries(const AllocHeader *p_h, const void *p_data)
        {
            if (p_h->head_canary_ != kHeadCanaryValue)
            {
                ARHUD_LOG_FATAL("HEAD_CANARY_CORRUPTED",
                                "[ARHud Memory] HEAD CANARY CORRUPTED at %p "
                                "(buffer underflow or use-after-free)",
                                p_data);
            }
            const uint64_t *tail = reinterpret_cast<const uint64_t *>(
                static_cast<const uint8_t *>(p_data) + p_h->alloc_size_);
            if (*tail != kTailCanaryValue)
            {
                ARHUD_LOG_FATAL("TAIL_CANARY_CORRUPTED",
                                "[ARHud Memory] TAIL CANARY CORRUPTED at %p "
                                "(buffer overflow, %zu bytes written past end)",
                                p_data, p_h->alloc_size_);
            }
        }

        inline void WriteTailCanary(void *p_data, size_t p_alloc_size)
        {
            uint64_t *tail = reinterpret_cast<uint64_t *>(
                static_cast<uint8_t *>(p_data) + p_alloc_size);
            *tail = kTailCanaryValue;
        }
#endif

        // ═══════════════════════════════════════════════════════════════════════
        // p_pad_align 头操作
        // ═══════════════════════════════════════════════════════════════════════

        ARHUD_ALWAYS_INLINE AllocHeader *GetAllocHeader(void *p_ptr)
        {
            return reinterpret_cast<AllocHeader *>(p_ptr) - 1;
        }

        inline void *AllocWithHeader(size_t p_size, uint64_t p_element_count)
        {
            size_t total = p_size + kAllocHeaderSize + kTailCanarySize;
            if (IsOverBudget(total))
                return nullptr;

            void *raw = BackendAlloc(total);
            if (raw == nullptr)
                return nullptr;

            AllocHeader *h = static_cast<AllocHeader *>(raw);
            h->head_canary_ = kHeadCanaryValue;
            h->alloc_size_ = p_size;
            h->element_count_ = p_element_count;

            void *data = static_cast<void *>(h + 1);

#if ARHUD_DEBUG
            WriteTailCanary(data, p_size);
#endif

            TrackAlloc(total);
            return data;
        }

        inline void FreeWithHeader(void *p_ptr)
        {
            if (p_ptr == nullptr)
                return;
            AllocHeader *h = GetAllocHeader(p_ptr);

#if ARHUD_DEBUG
            ValidateCanaries(h, p_ptr);
#endif

            size_t total = h->alloc_size_ + kAllocHeaderSize + kTailCanarySize;
            TrackFree(total);
            BackendFree(h);
        }

        inline void *ReallocWithHeader(void *p_ptr, size_t p_size)
        {
            if (p_ptr == nullptr)
            {
                return AllocWithHeader(p_size, 0);
            }
            if (p_size == 0)
            {
                FreeWithHeader(p_ptr);
                return nullptr;
            }

            AllocHeader *old_h = GetAllocHeader(p_ptr);
            size_t old_size = old_h->alloc_size_;
            size_t old_total = old_size + kAllocHeaderSize + kTailCanarySize;

            // 只是缩小或相等 → 原地
            if (p_size <= old_size)
            {
                old_h->alloc_size_ = p_size;
#if ARHUD_DEBUG
                // 缩小后 tail canary 位置变了，写入新的
                WriteTailCanary(p_ptr, p_size);
#endif
                // 缩小后释放超额统计
                size_t shrink = old_size - p_size;
#if ARHUD_DEBUG
                g_state.current_usage_.fetch_sub(shrink, std::memory_order_relaxed);
#endif
                return p_ptr;
            }

            // 扩容 → 分配新块、拷贝数据、释放旧块
            size_t new_total = p_size + kAllocHeaderSize + kTailCanarySize;
            if (IsOverBudget(new_total))
            {
                return nullptr;
            }

            void *raw = BackendAlloc(new_total);
            if (raw == nullptr)
                return nullptr;

            // 拷贝旧 header 和数据
            AllocHeader *new_h = static_cast<AllocHeader *>(raw);
            new_h->head_canary_ = kHeadCanaryValue;
            new_h->alloc_size_ = p_size;
            new_h->element_count_ = old_h->element_count_;
            void *new_data = new_h + 1;
            std::memcpy(new_data, p_ptr, old_size < p_size ? old_size : p_size);

#if ARHUD_DEBUG
            WriteTailCanary(new_data, p_size);
#endif

            // 释放旧块、更新统计
            TrackFree(old_total);
            TrackAlloc(new_total);
            BackendFree(old_h);

            return new_data;
        }

    } // anonymous namespace

    // ═══════════════════════════════════════════════════════════════════════
    // 基础分配
    // ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
    void *Alloc(size_t p_size, const char *p_file, int p_line)
#else
    void *Alloc(size_t p_size)
#endif
    {
        if (p_size == 0)
            return nullptr;

#if ARHUD_DEBUG
        void *ptr = AllocWithHeader(p_size, 0);
        RecordAlloc(ptr, p_size, p_file, p_line);
        return ptr;
#else
        if (IsOverBudget(p_size))
            return nullptr;

        void *ptr = BackendAlloc(p_size);
        return ptr;
#endif
    }

#if ARHUD_DEBUG
    void *AllocZeroed(size_t p_size, const char *p_file, int p_line)
#else
    void *AllocZeroed(size_t p_size)
#endif
    {
        if (p_size == 0)
            return nullptr;

#if ARHUD_DEBUG
        void *ptr = AllocWithHeader(p_size, 0);
        if (ptr != nullptr)
        {
            std::memset(ptr, 0, p_size);
        }
        RecordAlloc(ptr, p_size, p_file, p_line);
        return ptr;
#else
        if (IsOverBudget(p_size))
            return nullptr;
        void *ptr = BackendAllocZeroed(p_size);
        return ptr;
#endif
    }

#if ARHUD_DEBUG
    void *Realloc(void *p_ptr, size_t p_size, const char *p_file, int p_line)
#else
    void *Realloc(void *p_ptr, size_t p_size)
#endif
    {
        if (p_ptr == nullptr)
            return Alloc(p_size
#if ARHUD_DEBUG
                         ,
                         p_file, p_line
#endif
            );
        if (p_size == 0)
        {
            Free(p_ptr);
            return nullptr;
        }

#if ARHUD_DEBUG
        void *new_ptr = ReallocWithHeader(p_ptr, p_size);
        RecordRealloc(p_ptr, new_ptr, p_size, p_file, p_line);
        return new_ptr;
#else
        void *new_ptr = BackendRealloc(p_ptr, p_size);
        return new_ptr;
#endif
    }

    void Free(void *p_ptr)
    {
        if (p_ptr == nullptr)
            return;

#if ARHUD_DEBUG
        RecordFree(p_ptr);
        FreeWithHeader(p_ptr);
#else
        BackendFree(p_ptr);
#endif
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 静态分配（p_pad_align）
    // ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
    void *AllocStatic(size_t p_size, bool p_pad_align, const char *p_file, int p_line)
#else
    void *AllocStatic(size_t p_size, bool p_pad_align)
#endif
    {
        if (p_pad_align)
        {
            void *ptr = AllocWithHeader(p_size, 0);
#if ARHUD_DEBUG
            RecordAlloc(ptr, p_size, p_file, p_line);
#endif
            return ptr;
        }
        return Alloc(p_size
#if ARHUD_DEBUG
                     ,
                     p_file, p_line
#endif
        );
    }

#if ARHUD_DEBUG
    void *ReallocStatic(void *p_ptr, size_t p_size, bool p_pad_align,
                        const char *p_file, int p_line)
#else
    void *ReallocStatic(void *p_ptr, size_t p_size, bool p_pad_align)
#endif
    {
        if (p_pad_align)
        {
            void *ptr = ReallocWithHeader(p_ptr, p_size);
#if ARHUD_DEBUG
            RecordRealloc(p_ptr, ptr, p_size, p_file, p_line);
#endif
            return ptr;
        }
        return Realloc(p_ptr, p_size
#if ARHUD_DEBUG
                       ,
                       p_file, p_line
#endif
        );
    }

    void FreeStatic(void *p_ptr, bool p_pad_align)
    {
        if (p_pad_align)
        {
#if ARHUD_DEBUG
            RecordFree(p_ptr);
#endif
            FreeWithHeader(p_ptr);
            return;
        }
        Free(p_ptr);
    }

    void FreeStaticPostDtor(void *p_ptr)
    {
        if (p_ptr == nullptr)
        {
            return;
        }
#if ARHUD_DEBUG
        RecordFree(p_ptr);
#endif
        AllocHeader *h = GetAllocHeader(p_ptr);
        size_t total = h->alloc_size_ + kAllocHeaderSize + kTailCanarySize;
        TrackFree(total);
        BackendFree(h);
    }

    void ValidateAllocCanary(void *p_ptr)
    {
        if (p_ptr == nullptr)
        {
            return;
        }
        AllocHeader *h = GetAllocHeader(p_ptr);
        if (h->head_canary_ != kHeadCanaryValue)
        {
            ARHUD_LOG_FATAL("HEAD_CANARY_CORRUPTED_BEFORE_DTOR",
                            "[ARHud Memory] HEAD CANARY CORRUPTED BEFORE DESTRUCTOR at %p "
                            "(buffer underflow or use-after-free, canary=0x%llX)",
                            p_ptr, (unsigned long long)h->head_canary_);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 对齐分配
    // ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
    void *AllocAligned(size_t p_size, size_t p_alignment, const char *p_file, int p_line)
#else
    void *AllocAligned(size_t p_size, size_t p_alignment)
#endif
    {
        if (p_size == 0)
            return nullptr;
        if (p_alignment < sizeof(void *))
        {
            p_alignment = sizeof(void *);
        }

        size_t estimate = p_size + p_alignment + sizeof(void *);
        if (IsOverBudget(estimate))
            return nullptr;

        void *ptr = AlignedAllocRaw(p_size, p_alignment);
        if (ptr != nullptr)
        {
            TrackAlloc(estimate);
#if ARHUD_DEBUG
            g_state.aligned_alloc_sizes_[ptr] = estimate;
            RecordAlloc(ptr, estimate, p_file, p_line);
#endif
        }
        return ptr;
    }

    void *ReallocAligned(void *p_ptr, size_t p_size, size_t p_prev_size, size_t p_alignment)
    {
        if (p_ptr == nullptr)
        {
            return AllocAligned(p_size, p_alignment);
        }
        if (p_size == 0)
        {
            FreeAligned(p_ptr);
            return nullptr;
        }

        size_t new_total = p_size + p_alignment + sizeof(void *);
        if (IsOverBudget(new_total))
            return nullptr;

        void *new_ptr = AlignedAllocRaw(p_size, p_alignment);
        if (new_ptr == nullptr)
            return nullptr;

        size_t copy_size = (p_prev_size > 0) ? p_prev_size : 0;
        if (copy_size > p_size)
            copy_size = p_size;
        if (copy_size > 0)
        {
            std::memcpy(new_ptr, p_ptr, copy_size);
        }

        FreeAligned(p_ptr);
        TrackAlloc(new_total);
#if ARHUD_DEBUG
        g_state.aligned_alloc_sizes_[new_ptr] = new_total;
#endif

        return new_ptr;
    }

    void FreeAligned(void *p_ptr)
    {
        if (p_ptr == nullptr)
            return;

#if ARHUD_DEBUG
        RecordFree(p_ptr);
        auto it = g_state.aligned_alloc_sizes_.find(p_ptr);
        if (it != g_state.aligned_alloc_sizes_.end())
        {
            TrackFree(it->second);
            g_state.aligned_alloc_sizes_.erase(it);
        }
#endif
        AlignedFreeRaw(p_ptr);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 系统内存信息
    // ═══════════════════════════════════════════════════════════════════════

    uint64_t GetMemAvailable()
    {
#if defined(ARHUD_PLATFORM_WINDOWS)
        MEMORYSTATUSEX status;
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status))
        {
            return status.ullAvailPhys;
        }
#elif defined(ARHUD_PLATFORM_LINUX) || defined(ARHUD_PLATFORM_ANDROID)
        // /proc/meminfo 的 MemAvailable
        FILE *fp = std::fopen("/proc/meminfo", "r");
        if (fp != nullptr)
        {
            char line[128];
            while (std::fgets(line, sizeof(line), fp))
            {
                if (std::strncmp(line, "MemAvailable:", 13) == 0)
                {
                    uint64_t kb = 0;
                    std::sscanf(line + 13, "%llu", &kb);
                    std::fclose(fp);
                    return kb * 1024;
                }
            }
            std::fclose(fp);
        }
#elif defined(ARHUD_PLATFORM_QNX)
        // QNX: sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE)
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long psize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && psize > 0)
        {
            return static_cast<uint64_t>(pages) * static_cast<uint64_t>(psize);
        }
#endif
        return 0;
    }

    uint64_t GetMemUsage()
    {
#if defined(ARHUD_PLATFORM_WINDOWS)
        PROCESS_MEMORY_COUNTERS pmc;
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        {
            return pmc.WorkingSetSize;
        }
#elif defined(ARHUD_PLATFORM_LINUX) || defined(ARHUD_PLATFORM_ANDROID)
        // /proc/self/status 的 VmRSS
        FILE *fp = std::fopen("/proc/self/status", "r");
        if (fp != nullptr)
        {
            char line[128];
            while (std::fgets(line, sizeof(line), fp))
            {
                if (std::strncmp(line, "VmRSS:", 6) == 0)
                {
                    uint64_t kb = 0;
                    std::sscanf(line + 6, "%llu", &kb);
                    std::fclose(fp);
                    return kb * 1024;
                }
            }
            std::fclose(fp);
        }
#elif defined(ARHUD_PLATFORM_QNX)
        // QNX: /proc/self/statm-like or procfs
        long psize = sysconf(_SC_PAGESIZE);
        FILE *fp = std::fopen("/proc/self/statm", "r");
        if (fp != nullptr)
        {
            long rss = 0;
            std::fscanf(fp, "%*s %ld", &rss);
            std::fclose(fp);
            return static_cast<uint64_t>(rss) * static_cast<uint64_t>(psize);
        }
#endif
        // 兜底：返回分配器内部统计（仅堆分配量，不含 mmap 等）
        return g_state.current_usage_.load(std::memory_order_acquire);
    }

    uint64_t GetMemMaxUsage()
    {
        return g_state.peak_usage_.load(std::memory_order_acquire);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 内存统计
    // ═══════════════════════════════════════════════════════════════════════

    MemoryStats GetStats()
    {
        MemoryStats s;
        s.current_usage_ = g_state.current_usage_.load(std::memory_order_acquire);
        s.peak_usage_ = g_state.peak_usage_.load(std::memory_order_acquire);
        s.allocation_count_ = g_state.allocation_count_.load(std::memory_order_acquire);
        s.total_alloc_calls_ = g_state.total_alloc_calls_.load(std::memory_order_acquire);
        s.total_free_calls_ = g_state.total_free_calls_.load(std::memory_order_acquire);
        return s;
    }

    void ResetPeak()
    {
        size_t cur = g_state.current_usage_.load(std::memory_order_relaxed);
        g_state.peak_usage_.store(cur, std::memory_order_relaxed);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 预算
    // ═══════════════════════════════════════════════════════════════════════

    void SetBudget(size_t p_total_bytes)
    {
        g_state.budget_.store(p_total_bytes, std::memory_order_release);
    }

    bool IsBudgetExceeded()
    {
        size_t b = g_state.budget_.load(std::memory_order_acquire);
        if (b == 0)
            return false;
        size_t cur = g_state.current_usage_.load(std::memory_order_acquire);
        return cur > b;
    }

    size_t GetBudget()
    {
        return g_state.budget_.load(std::memory_order_acquire);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 数组长度查询
    // ═══════════════════════════════════════════════════════════════════════

    size_t GetArrayLength(const void *p_ptr)
    {
        if (p_ptr == nullptr)
            return 0;
        const AllocHeader *h = reinterpret_cast<const AllocHeader *>(p_ptr) - 1;
        return static_cast<size_t>(h->element_count_);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Debug 泄漏检测（仅 ARHUD_DEBUG 模式）
    // ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
    void DumpLeaks()
    {
        std::lock_guard<std::mutex> lock(g_alloc_records_mutex);

        if (g_alloc_records.empty())
        {
            std::fprintf(stderr, "[ARHud Memory] No leaks detected.\n");
            return;
        }

        std::fprintf(stderr, "[ARHud Memory] === LEAK REPORT ===\n");
        std::fprintf(stderr, "[ARHud Memory] %zu allocation(s) still alive:\n",
                     g_alloc_records.size());

        size_t total_leaked = 0;
        for (const auto &[ptr, record] : g_alloc_records)
        {
            std::fprintf(stderr, "[ARHud Memory]   %p  %zu bytes  at %s:%d\n",
                         ptr, record.size, record.file, record.line);
            total_leaked += record.size;
        }

        std::fprintf(stderr, "[ARHud Memory] Total leaked: %zu bytes\n", total_leaked);
        std::fprintf(stderr, "[ARHud Memory] === END LEAK REPORT ===\n");
    }
#endif

    // ═══════════════════════════════════════════════════════════════════════
    // 自定义分配器 Hook
    // ═══════════════════════════════════════════════════════════════════════

    void SetAllocatorHooks(const AllocatorHooks &p_hooks)
    {
        g_state.hooks_ = p_hooks;
    }

    AllocatorHooks GetAllocatorHooks()
    {
        return g_state.hooks_;
    }

} // namespace arhud::memory
