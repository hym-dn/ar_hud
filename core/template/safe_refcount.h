/**
 * @file safe_refcount.h
 * @brief 原子操作基础组件（零 OS 依赖）
 *
 * 提供最底层的原子操作封装，被几乎所有模块依赖：
 *   - CpuPause()      自旋等待的 CPU 流水线暂停提示
 *   - SafeNumeric<T>   acquire-release 原子数值（引用计数、帧号、版本号）
 *   - SafeFlag         原子布尔标记（一次性同步事件）
 *
 * 依赖关系：
 *   safe_refcount.h  (零 OS 依赖，仅 <atomic> + typedefs.h)
 *         ↑
 *      sync.h        (<mutex> + <condition_variable> + <shared_mutex>)
 *         ↑
 *      thread.h      (<thread>)
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-01
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <atomic>

#include "typedefs.h"

#if defined(ARHUD_COMPILER_MSVC)
#include <intrin.h>
#endif

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // CPU Pause
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief CPU 流水线暂停提示
     *
     * 在自旋等待循环中调用，降低功耗并减少总线争用。
     * - x86-64: `_mm_pause()`（约 140 周期延迟）
     * - ARM64:  `__yield()`（WFE 提示）
     * - 其他:   空操作
     */
    ARHUD_ALWAYS_INLINE void CpuPause()
    {
#if defined(ARHUD_ARCH_X86_64)
#if defined(ARHUD_COMPILER_MSVC)
        _mm_pause();
#else
        __builtin_ia32_pause();
#endif
#elif defined(ARHUD_ARCH_ARM64)
#if defined(ARHUD_COMPILER_MSVC)
        __yield();
#else
        __asm__ volatile("yield");
#endif
#endif
    }

    // ═══════════════════════════════════════════════════════════════════════
    // SafeNumeric<T>
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 原子数值封装
     *
     * 对 std::atomic<T> 的 acquire-release 语义封装，用于引用计数、
     * 帧号、资源版本号等需要线程安全读写的数值。
     *
     * @tparam T 整数类型（uint32 / uint64 / int32 等），必须满足
     *           std::atomic<T>::is_always_lock_free
     *
     * @note 参考 Godot core/os/safe_refcount.h
     */
    template <typename T>
    class SafeNumeric
    {
        static_assert(std::atomic<T>::is_always_lock_free,
                      "SafeNumeric<T> requires lock-free atomic<T>");

        std::atomic<T> value_{0};

    public:
        /**
         * @brief 原子写入（release 语义）
         * @param[in] p_value 新值
         */
        void Set(T p_value)
        {
            value_.store(p_value, std::memory_order_release);
        }

        /**
         * @brief 原子读取（acquire 语义）
         * @return 当前值
         */
        T Get() const
        {
            return value_.load(std::memory_order_acquire);
        }

        /**
         * @brief 原子递增并返回新值
         * @return 递增后的值
         */
        T Increment()
        {
            return value_.fetch_add(1, std::memory_order_acq_rel) + 1;
        }

        /**
         * @brief 原子递减并返回新值
         * @return 递减后的值
         */
        T Decrement()
        {
            return value_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        }

        /**
         * @brief 原子加法
         * @param[in] p_value 加数
         * @return 加法后的新值
         */
        T Add(T p_value)
        {
            return value_.fetch_add(p_value, std::memory_order_acq_rel) + p_value;
        }

        /**
         * @brief 原子减法
         * @param[in] p_value 减数
         * @return 减法后的新值
         */
        T Sub(T p_value)
        {
            return value_.fetch_sub(p_value, std::memory_order_acq_rel) - p_value;
        }

        /**
         * @brief 条件递增（仅当当前值 > 0 时递增）
         *
         * 使用 CAS 循环实现，适用于引用计数从 0 恢复的场景。
         *
         * @return true  递增成功
         * @return false 当前值为 0，未递增
         */
        bool ConditionalIncrement()
        {
            T old_val = value_.load(std::memory_order_acquire);
            while (old_val > 0)
            {
                if (value_.compare_exchange_weak(old_val, old_val + 1,
                                                 std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 原子交换为新值
         * @param[in] p_value 新值
         * @return 交换前的旧值
         */
        T Exchange(T p_value)
        {
            return value_.exchange(p_value, std::memory_order_acq_rel);
        }

        /**
         * @brief 原子比较并交换
         *
         * @param[in]     p_expected  期望值（成功时写入实际值）
         * @param[in]     p_desired   新值
         * @return true  交换成功
         * @return false 交换失败，p_expected 被更新为当前值
         */
        bool CompareExchange(T &p_expected, T p_desired)
        {
            return value_.compare_exchange_strong(p_expected, p_desired,
                                                  std::memory_order_acq_rel, std::memory_order_acquire);
        }
    };

    // ═══════════════════════════════════════════════════════════════════════
    // SafeFlag
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 原子布尔标记
     *
     * 用于一次性同步事件（如初始化完成标志、关闭信号）。
     * Set 使用 release 语义，IsSet 使用 acquire 语义，
     * 确保 Set 之前的所有写操作对 IsSet 的调用者可见。
     *
     * @note 参考 Godot core/os/safe_refcount.h SafeFlag
     */
    class SafeFlag
    {
        std::atomic<bool> flag_{false};

    public:
        /**
         * @brief 设置标记为 true（release 语义）
         */
        void Set()
        {
            flag_.store(true, std::memory_order_release);
        }

        /**
         * @brief 查询标记状态（acquire 语义）
         * @return 标记是否已设置
         */
        bool IsSet() const
        {
            return flag_.load(std::memory_order_acquire);
        }

        /**
         * @brief 清除标记（release 语义）
         */
        void Clear()
        {
            flag_.store(false, std::memory_order_release);
        }

        /**
         * @brief 查询并清除标记（acq_rel 语义）
         *
         * 原子地读取标记值并清除，适用于"消费一次"模式。
         *
         * @return 清除前的标记状态
         */
        bool IsSetClear()
        {
            return flag_.exchange(false, std::memory_order_acq_rel);
        }
    };

} // namespace arhud
