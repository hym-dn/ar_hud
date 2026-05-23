/**
 * @file thread.cpp
 * @brief Thread 类实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-01
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#include "os/thread.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // Thread 静态成员初始化
    // ═══════════════════════════════════════════════════════════════════════

    Thread::PlatformFunctions Thread::platform_functions_;

    // ═══════════════════════════════════════════════════════════════════════
    // 析构
    // ═══════════════════════════════════════════════════════════════════════

    Thread::~Thread()
    {
        ARHUD_ASSERT(!started_);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Start
    // ═══════════════════════════════════════════════════════════════════════

    Thread::ID Thread::Start(Callback p_callback, void *p_userdata,
                             const Settings &p_settings)
    {
        if (started_)
        {
            return kUnassignedId;
        }

        if (p_callback == nullptr)
        {
            return kUnassignedId;
        }

        PlatformFunctions pf = platform_functions_;

        thread_ = std::thread(
            &Thread::EntryFunction, p_callback, p_userdata,
            p_settings, pf);

        if (!thread_.joinable())
        {
            return kUnassignedId;
        }

        id_ = static_cast<ID>(std::hash<std::thread::id>{}(thread_.get_id()));
        started_ = true;
        
        return id_;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // WaitToFinish
    // ═══════════════════════════════════════════════════════════════════════

    void Thread::WaitToFinish()
    {
        if (!started_)
        {
            return;
        }

        if (id_ == GetCallerId())
        {
            ARHUD_ASSERT(false, "Thread::WaitToFinish: deadlock - "
                                "thread cannot wait for itself");
            return;
        }

        if (thread_.joinable())
        {
            thread_.join();
        }

        started_ = false;
        id_ = kUnassignedId;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 查询方法
    // ═══════════════════════════════════════════════════════════════════════

    bool Thread::IsStarted() const
    {
        return started_;
    }

    Thread::ID Thread::GetId() const
    {
        return id_;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 静态方法
    // ═══════════════════════════════════════════════════════════════════════

    Thread::ID Thread::GetCallerId()
    {
        return static_cast<ID>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    Thread::ID Thread::GetMainId()
    {
        static ID main_id = GetCallerId();
        return main_id;
    }

    bool Thread::IsMainThread()
    {
        return GetCallerId() == GetMainId();
    }

    void Thread::Yield()
    {
        std::this_thread::yield();
    }

    void Thread::SleepUsec(uint32 p_usec)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(p_usec));
    }

    void Thread::SetPlatformFunctions(const PlatformFunctions &p_functions)
    {
        platform_functions_ = p_functions;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 线程入口包装
    // ═══════════════════════════════════════════════════════════════════════

    void Thread::EntryFunction(Callback p_callback, void *p_userdata,
                               Settings p_settings, PlatformFunctions p_pf)
    {
        if (p_pf.init)
        {
            p_pf.init();
        }

        if (p_pf.set_priority)
        {
            p_pf.set_priority(p_settings.priority);
        }

        p_callback(p_userdata);

        if (p_pf.term)
        {
            p_pf.term();
        }
    }

} // namespace arhud
