/**
 * @file sync.h
 * @brief 同步原语（OS 内核级锁 + 自旋锁）
 *
 * 提供线程间同步所需的锁和信号量：
 *   - SpinLock        纯用户态自旋锁（极短临界区）
 *   - MutexImpl<T>    互斥锁模板（递归/非递归）
 *   - MutexLock<T>    RAII 锁守卫（含 TempUnlock/Relock）
 *   - Semaphore       计数信号量（帧同步、生产者-消费者）
 *   - RWLock          读写锁 + RAII 守卫
 *
 * 依赖关系：
 *   safe_refcount.h  (零 OS 依赖)
 *         ↑
 *      sync.h         ← 本文件（引入 <mutex> + <condition_variable> + <shared_mutex>）
 *         ↑
 *      thread.h       (<thread>)
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-01
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include "template/safe_refcount.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // SpinLock
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 自旋锁
     *
     * 纯用户态自旋锁，适用于极短临界区（< 几百周期），
     * 如 PagedAllocator 的分配/释放、引用计数操作。
     *
     * 设计要点：
     * - 使用 CACHE_LINE_SIZE 对齐，避免伪共享（false sharing）
     * - 自旋时调用 CpuPause() 降低功耗
     * - 不使用 OS 级锁，无上下文切换开销
     * - 不可递归加锁
     *
     * @note 参考 Godot core/os/spin_lock.h
     */
    class SpinLock
    {
        union
        {
            mutable std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
            char aligner_[ARHUD_CACHE_LINE_SIZE];
        };

    public:
        /**
         * @brief 加锁（自旋等待）
         *
         * 持续调用 test_and_set + CpuPause 直到获取锁。
         */
        void Lock() const
        {
            while (flag_.test_and_set(std::memory_order_acquire))
            {
                CpuPause();
            }
        }

        /**
         * @brief 解锁
         */
        void Unlock() const
        {
            flag_.clear(std::memory_order_release);
        }

        /**
         * @brief 尝试加锁
         * @return true  加锁成功
         * @return false 锁已被占用
         */
        bool TryLock() const
        {
            return !flag_.test_and_set(std::memory_order_acquire);
        }

        /**
         * @brief RAII 自旋锁守卫
         *
         * 构造时加锁，析构时解锁。确保异常路径下也能正确释放。
         *
         * Usage:
         * @code
         *   SpinLock lock;
         *   {
         *       SpinLock::Guard guard(lock);
         *       // ... 极短临界区 ...
         *   }
         * @endcode
         */
        class Guard
        {
            const SpinLock &lock_;

        public:
            explicit Guard(const SpinLock &p_lock)
                : lock_(p_lock)
            {
                lock_.Lock();
            }

            ~Guard()
            {
                lock_.Unlock();
            }

            ARHUD_DISABLE_COPY_MOVE(Guard);
        };
    };

    // ═══════════════════════════════════════════════════════════════════════
    // MutexImpl / Mutex / BinaryMutex
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 互斥锁模板
     *
     * 对 std::recursive_mutex / std::mutex 的统一封装，
     * 提供 PascalCase 接口并暴露底层类型供 MutexLock 使用。
     *
     * @tparam StdMutex 标准互斥锁类型
     *
     * @note 参考 Godot core/os/mutex.h MutexImpl
     */
    template <typename StdMutex>
    class MutexImpl
    {
        template <typename>
        friend class MutexLock;

        StdMutex mutex_;

    public:
        using StdMutexType = StdMutex;

        /**
         * @brief 加锁（阻塞直到获取）
         */
        void Lock()
        {
            mutex_.lock();
        }

        /**
         * @brief 解锁
         */
        void Unlock()
        {
            mutex_.unlock();
        }

        /**
         * @brief 尝试加锁
         * @return true  加锁成功
         * @return false 锁已被占用
         */
        bool TryLock()
        {
            return mutex_.try_lock();
        }
    };

    /**
     * @brief 递归互斥锁（通用场景）
     *
     * 允许同一线程多次加锁，适用于回调链、插件系统等
     * 难以精确控制加锁层次的场景。
     */
    using Mutex = MutexImpl<std::recursive_mutex>;

    /**
     * @brief 非递归互斥锁（高性能场景）
     *
     * 不允许同一线程重复加锁（会死锁），但开销比递归锁低。
     * 与 std::condition_variable 配合使用时必须选择此类型。
     */
    using BinaryMutex = MutexImpl<std::mutex>;

    // ═══════════════════════════════════════════════════════════════════════
    // MutexLock
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief RAII 锁守卫
     *
     * 构造时加锁，析构时解锁。提供 TempUnlock/TempRelock
     * 用于条件变量等待模式。
     *
     * @tparam MutexT MutexImpl 实例化类型（Mutex 或 BinaryMutex）
     *
     * @note 参考 Godot core/os/mutex.h MutexLock
     *
     * Usage:
     * @code
     *   BinaryMutex mtx;
     *   std::condition_variable cv;
     *
     *   {
     *       MutexLock<BinaryMutex> lock(mtx);
     *       // ... 临界区 ...
     *       lock.TempUnlock();
     *       cv.wait(lock.GetNativeLock());
     *       lock.TempRelock();
     *   }
     * @endcode
     */
    template <typename MutexT>
    class MutexLock
    {
        std::unique_lock<typename MutexT::StdMutexType> lock_;

        /**
         * @brief 延迟构造（不自动加锁）
         * @param[in] p_mutex 目标互斥锁
         *
         * 构造后不持有锁，需手动调用 Lock()/TryLock()。
         */
        explicit MutexLock(MutexT &p_mutex, bool p_deferred)
            : lock_(p_mutex.mutex_, std::defer_lock)
        {
            (void)p_deferred;
        }

    public:
        /**
         * @brief 构造并立即加锁（阻塞模式）
         * @param[in] p_mutex 目标互斥锁
         */
        explicit MutexLock(MutexT &p_mutex)
            : lock_(p_mutex.mutex_)
        {
        }

        ~MutexLock() = default;

        ARHUD_DISABLE_COPY_MOVE(MutexLock);

        // ---- 手动加锁/解锁（延迟模式 + 条件等待场景） ----

        /**
         * @brief 加锁（阻塞直到获取）
         *
         * 可在 deferred 模式下首次加锁，或在 Unlock 后重新获取锁。
         */
        void Lock()
        {
            lock_.lock();
        }

        /**
         * @brief 解锁
         *
         * 解锁后可重新调用 Lock() 或 TryLock() 再次获取。
         */
        void Unlock()
        {
            lock_.unlock();
        }

        /**
         * @brief 尝试加锁（非阻塞）
         * @return true  获取成功
         * @return false 锁已被占用
         */
        bool TryLock()
        {
            return lock_.try_lock();
        }

        // ---- 条件变量兼容接口（TempUnlock/Relock 保留为别名） ----

        /**
         * @brief 临时解锁（用于条件变量等待）
         *
         * 等价于 Unlock()，语义更明确地表达"临时释放"意图。
         * 必须配对调用 TempRelock()。
         */
        void TempUnlock()
        {
            Unlock();
        }

        /**
         * @brief 重新加锁（配对 TempUnlock）
         *
         * 等价于 Lock()。
         */
        void TempRelock()
        {
            Lock();
        }

        // ---- 底层访问 ----

        /**
         * @brief 获取底层 unique_lock 引用
         *
         * 用于 std::condition_variable::wait() 等需要
         * unique_lock 参数的标准库接口。
         *
         * @return 底层 unique_lock 的非常量引用
         */
        std::unique_lock<typename MutexT::StdMutexType> &GetNativeLock()
        {
            return lock_;
        }
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Semaphore
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 计数信号量
     *
     * 用于线程间通知/同步：
     * - 主线程与渲染线程的帧同步
     * - 生产者-消费者模式：主线程提交命令，渲染线程消费执行
     * - RenderingDevice::swap_buffers 中等待前一帧 GPU 完成
     *
     * 内部使用 std::mutex + std::condition_variable 实现，
     * 不使用 C++20 std::counting_semaphore 以保持 C++17 兼容。
     *
     * @note 参考 Godot core/os/semaphore.h
     */
    class Semaphore
    {
        mutable std::mutex mutex_;
        mutable std::condition_variable condition_;
        mutable uint32 count_ = 0;

    public:
        /**
         * @brief 释放信号量（增加计数）
         *
         * @param[in] p_count 释放的信号量数量，默认 1
         *
         * @note 唤醒一个或多个等待线程
         */
        void Post(uint32 p_count = 1) const
        {
            std::lock_guard<std::mutex> guard(mutex_);
            count_ += p_count;
            if (p_count == 1)
            {
                condition_.notify_one();
            }
            else
            {
                condition_.notify_all();
            }
        }

        /**
         * @brief 等待信号量（阻塞直到计数 > 0）
         *
         * 获取一个信号量，计数减 1。计数为 0 时阻塞。
         */
        void Wait() const
        {
            std::unique_lock<std::mutex> guard(mutex_);
            while (count_ == 0)
            {
                condition_.wait(guard);
            }
            --count_;
        }

        /**
         * @brief 尝试等待信号量（非阻塞）
         *
         * @return true  成功获取一个信号量
         * @return false 计数为 0，未获取
         */
        bool TryWait() const
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (count_ > 0)
            {
                --count_;
                return true;
            }
            return false;
        }
    };

    // ═══════════════════════════════════════════════════════════════════════
    // RWLock
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 读写锁
     *
     * 允许多个读者并发访问，写者独占访问。
     * 适用于渲染资源（纹理/材质/网格）的读多写少场景——
     * 渲染线程频繁读取，主线程偶尔更新。
     *
     * 内部使用 C++17 std::shared_mutex 实现。
     *
     * @note 参考 Godot core/os/rw_lock.h
     */
    class RWLock
    {
        mutable std::shared_mutex mutex_;

    public:
        /**
         * @brief 获取共享读锁（阻塞）
         *
         * 多个读者可同时持有读锁。
         */
        void ReadLock() const
        {
            mutex_.lock_shared();
        }

        /**
         * @brief 释放共享读锁
         */
        void ReadUnlock() const
        {
            mutex_.unlock_shared();
        }

        /**
         * @brief 尝试获取共享读锁
         * @return true  获取成功
         * @return false 已有写者持有锁
         */
        bool ReadTryLock() const
        {
            return mutex_.try_lock_shared();
        }

        /**
         * @brief 获取独占写锁（阻塞）
         *
         * 写锁与所有读锁互斥。
         */
        void WriteLock()
        {
            mutex_.lock();
        }

        /**
         * @brief 释放独占写锁
         */
        void WriteUnlock()
        {
            mutex_.unlock();
        }

        /**
         * @brief 尝试获取独占写锁
         * @return true  获取成功
         * @return false 已有读者或写者持有锁
         */
        bool WriteTryLock()
        {
            return mutex_.try_lock();
        }
    };

    /**
     * @brief RAII 读锁守卫
     *
     * 构造时获取共享读锁，析构时释放。
     *
     * Usage:
     * @code
     *   RWLock lock;
     *   {
     *       RWLockRead read_guard(lock);
     *       // ... 只读访问 ...
     *   }
     * @endcode
     */
    class RWLockRead
    {
        const RWLock &lock_;

    public:
        /**
         * @brief 构造并获取共享读锁
         * @param[in] p_lock 目标读写锁
         */
        explicit RWLockRead(const RWLock &p_lock)
            : lock_(p_lock)
        {
            lock_.ReadLock();
        }

        ~RWLockRead()
        {
            lock_.ReadUnlock();
        }
    };

    /**
     * @brief RAII 写锁守卫
     *
     * 构造时获取独占写锁，析构时释放。
     *
     * Usage:
     * @code
     *   RWLock lock;
     *   {
     *       RWLockWrite write_guard(lock);
     *       // ... 独占写入 ...
     *   }
     * @endcode
     */
    class RWLockWrite
    {
        RWLock &lock_;

    public:
        /**
         * @brief 构造并获取独占写锁
         * @param[in] p_lock 目标读写锁
         */
        explicit RWLockWrite(RWLock &p_lock)
            : lock_(p_lock)
        {
            lock_.WriteLock();
        }

        ~RWLockWrite()
        {
            lock_.WriteUnlock();
        }
    };

} // namespace arhud
