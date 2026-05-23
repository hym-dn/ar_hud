/**
 * @file thread.h
 * @brief 线程抽象
 *
 * 对 std::thread 的封装，提供：
 * - 统一的线程启动/等待接口
 * - 主线程 ID 缓存与查询
 * - 平台注入点（PlatformFunctions）用于 QNX 等特殊平台
 * - 线程优先级设置
 *
 * 依赖关系：
 *   safe_refcount.h  (零 OS 依赖)
 *         ↑
 *      sync.h         (<mutex> + <condition_variable> + <shared_mutex>)
 *         ↑
 *      thread.h       ← 本文件（<thread>）
 *
 * 生命周期：
 *   1. 构造 → 未启动状态
 *   2. Start() → 启动线程
 *   3. WaitToFinish() → 等待线程结束并回收
 *   4. 析构 → 断言线程已结束（Debug）
 *
 * @note 参考 Godot core/os/thread.h
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-01
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <chrono>
#include <thread>
#include "os/sync.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // Thread
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 线程抽象
     *
     * 对 std::thread 的封装，提供：
     * - 统一的线程启动/等待接口
     * - 主线程 ID 缓存与查询
     * - 平台注入点（PlatformFunctions）用于 QNX 等特殊平台
     * - 线程优先级设置
     *
     * 生命周期：
     *   1. 构造 → 未启动状态
     *   2. Start() → 启动线程
     *   3. WaitToFinish() → 等待线程结束并回收
     *   4. 析构 → 断言线程已结束（Debug）
     *
     * @note 参考 Godot core/os/thread.h
     */
    class Thread
    {
    public:
        /**
         * @brief 线程 ID 类型
         */
        using ID = uint64;

        /**
         * @brief 线程入口回调类型
         *
         * @param p_userdata 用户数据指针
         */
        using Callback = void (*)(void *p_userdata);

        /**
         * @brief 线程优先级
         */
        enum Priority
        {
            kPriorityLow,    ///< 低优先级（后台任务）
            kPriorityNormal, ///< 正常优先级（默认）
            kPriorityHigh    ///< 高优先级（渲染线程）
        };

        /**
         * @brief 线程启动设置
         */
        struct Settings
        {
            Priority priority = kPriorityNormal; ///< 线程优先级
        };

        /**
         * @brief 平台注入函数
         *
         * 允许平台层注入自定义的线程包装逻辑，
         * 用于 QNX 线程初始化/清理、线程命名等。
         * 所有函数指针默认为 nullptr（不注入）。
         */
        struct PlatformFunctions
        {
            Error (*set_name)(const char *p_name) = nullptr;
            void (*set_priority)(Priority p_priority) = nullptr;
            void (*init)() = nullptr;
            void (*term)() = nullptr;
        };

        static constexpr ID kUnassignedId = 0; ///< 未启动线程的 ID

        Thread() = default;

        /**
         * @brief 析构，断言线程已结束
         *
         * Debug 构建下如果线程仍在运行则触发断言。
         * 必须在析构前调用 WaitToFinish()。
         */
        ~Thread();

        ARHUD_DISABLE_COPY_MOVE(Thread);

        /**
         * @brief 启动线程
         *
         * @param[in] p_callback  线程入口函数
         * @param[in] p_userdata  传递给入口函数的用户数据
         * @param[in] p_settings  线程设置（优先级等）
         * @return 线程 ID，或 kUnassignedId 表示启动失败
         *
         * @pre 线程未启动（IsStarted() == false）
         * @post IsStarted() == true（成功时）
         */
        ID Start(Callback p_callback, void *p_userdata,
                 const Settings &p_settings = Settings());

        /**
         * @brief 等待线程结束
         *
         * 阻塞调用线程直到目标线程执行完毕。
         * 调用后线程回到未启动状态，可再次 Start()。
         *
         * @pre 线程已启动（IsStarted() == true）
         * @post IsStarted() == false
         *
         * @note 从自身线程调用会导致死锁（Debug 断言）
         */
        void WaitToFinish();

        /**
         * @brief 查询线程是否已启动
         * @return true 线程正在运行
         */
        bool IsStarted() const;

        /**
         * @brief 获取此线程对象的 ID
         * @return 线程 ID，未启动时为 kUnassignedId
         */
        ID GetId() const;

        /**
         * @brief 获取当前调用线程的 ID
         * @return 当前线程的 ID
         */
        static ID GetCallerId();

        /**
         * @brief 获取主线程 ID
         *
         * 首次调用时缓存当前线程 ID 作为主线程。
         * 应在程序入口（main 函数开始处）首次调用以确保正确性。
         *
         * @return 主线程 ID
         */
        static ID GetMainId();

        /**
         * @brief 判断当前线程是否为主线程
         * @return true 当前线程是主线程
         */
        static bool IsMainThread();

        /**
         * @brief 让出当前线程的时间片
         *
         * 建议操作系统调度器切换到其他就绪线程。
         */
        static void Yield();

        /**
         * @brief 休眠指定微秒数
         *
         * @param[in] p_usec 休眠时间（微秒）
         *
         * @note 实际精度取决于操作系统调度器（通常 1-15ms）
         */
        static void SleepUsec(uint32 p_usec);

        /**
         * @brief 设置平台注入函数
         *
         * 应在程序初始化时调用，设置后所有新启动的线程
         * 将使用注入的 init/term/set_name/set_priority。
         *
         * @param[in] p_functions 平台函数集合
         */
        static void SetPlatformFunctions(const PlatformFunctions &p_functions);

    private:
        std::thread thread_;
        ID id_ = kUnassignedId;
        bool started_ = false;
        
        static PlatformFunctions platform_functions_;

        /**
         * @brief 线程入口包装
         *
         * 调用顺序：PlatformFunctions::init → set_priority →
         *           用户回调 → PlatformFunctions::term
         *
         * @param[in] p_callback  用户回调
         * @param[in] p_userdata  用户数据
         * @param[in] p_settings  线程设置
         * @param[in] p_pf        平台函数快照（值捕获，避免竞态）
         */
        static void EntryFunction(Callback p_callback, void *p_userdata,
                                  Settings p_settings, PlatformFunctions p_pf);
    };

} // namespace arhud
