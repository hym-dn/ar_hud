/**
 * @file logger_file.h
 * @brief 文件日志后端（异步写入 + 文件轮转）
 *
 * FileLogger 使用生产者-消费者模型实现高性能异步文件日志：
 *
 *   调用线程（热路径）：
 *     ARHUD_LOG_INFO(...)
 *       → LogV() → vsnprintf 格式化 → SpinLock 入队 → Semaphore 通知 → 立即返回
 *       耗时：~200ns（一次 SpinLock + 一次 memcpy + 一次 semaphore Post）
 *
 *   写入线程（后台）：
 *     循环等待 Semaphore → 批量出队 → fwrite → 按策略 flush
 *     磁盘 I/O 完全不阻塞调用线程
 *
 * 性能设计要点：
 *   - 热路径仅 SpinLock（极短临界区：head_++ + memcpy）
 *   - 固定大小 Entry 避免堆分配
 *   - 批量写入：一次出队所有待写条目，减少 fwrite 调用次数
 *   - 文件轮转：超过大小阈值自动备份，保留 N 个历史文件
 *
 * 缓冲区溢出策略：
 *   - 环形缓冲区满时丢弃新条目，输出一条 "[LOG DROPPED]" 警告
 *   - 不阻塞调用线程（宁可丢日志也不卡帧）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-02
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <cstdio>
#include "template/safe_refcount.h"
#include "os/sync.h"
#include "os/thread.h"
#include "io/logger.h"

namespace arhud
{

    /**
     * @brief 文件日志后端（异步写入 + 文件轮转）
     *
     * 使用环形缓冲区 + 后台写入线程实现零阻塞日志。
     * 适用于渲染引擎等对帧率敏感的场景——日志写入不卡主线程/渲染线程。
     *
     * 生命周期：
     *   1. 构造 → 配置参数
     *   2. Start() → 启动写入线程
     *   3. LogV/LogError → 异步写入（生产者）
     *   4. Stop() → 刷盘 + 停止写入线程
     *   5. 析构 → 释放缓冲区
     *
     * @note 参考 Godot core/os/logger.h RotatedFileLogger
     */
    class FileLogger : public Logger
    {
    public:
        /**
         * @brief 单条日志条目最大长度（含 '\0'）
         */
        static constexpr uint32 kMaxMessageLen = 512;

        /**
         * @brief 文件路径最大长度
         */
        static constexpr uint32 kMaxPathLen = 260;

        /**
         * @brief 配置参数
         */
        struct Config
        {
            const char *file_path = "arhud.log";     ///< 日志文件路径
            uint32 ring_buffer_capacity = 1024;      ///< 环形缓冲区条目数
            uint32 max_file_size = 10 * 1024 * 1024; ///< 单文件最大字节数（默认 10MB）
            uint32 max_backup_files = 5;             ///< 最大备份文件数
            uint32 flush_interval_ms = 1000;         ///< 定时刷盘间隔（毫秒），0 = 仅在条目写入时刷盘
        };

        /**
         * @brief 构造 FileLogger
         *
         * 仅初始化参数，不启动线程。需调用 Start() 开始写入。
         *
         * @param[in] p_config 配置参数
         */
        explicit FileLogger(const Config &p_config = Config());

        /**
         * @brief 析构，自动调用 Stop()
         */
        ~FileLogger() override;

        ARHUD_DISABLE_COPY_MOVE(FileLogger);

        /**
         * @brief 启动写入线程并打开日志文件
         *
         * @return Error::kOk       启动成功
         * @return Error::kFailed   文件打开失败或线程启动失败
         *
         * @pre 未启动（只能调用一次）
         */
        Error Start();

        /**
         * @brief 停止写入线程并关闭日志文件
         *
         * 刷盘所有缓冲条目后关闭文件。
         *
         * @pre 已启动
         */
        void Stop();

        /**
         * @brief 查询是否已启动
         * @return true 写入线程正在运行
         */
        bool IsRunning() const;

        /**
         * @brief 获取已丢弃的日志条目数
         * @return 因缓冲区满而丢弃的条目总数
         */
        uint32 GetDroppedCount() const;

        void LogV(LogLevel p_level, const char *p_format,
                  va_list p_list, bool p_err) override;

        void LogError(const char *p_function, const char *p_file,
                      int32 p_line, const char *p_code,
                      const char *p_rationale,
                      LogLevel p_level = LogLevel::kError) override;

    private:
        /**
         * @brief 环形缓冲区条目
         */
        struct Entry
        {
            LogLevel level;               ///< 日志级别
            uint16 len = 0;               ///< 消息实际长度（不含 '\0'）
            char message[kMaxMessageLen]; ///< 格式化后的消息
        };

        // ---- 环形缓冲区 ----
        Entry *ring_buffer_ = nullptr; ///< 环形缓冲区数组
        uint32 capacity_ = 0;          ///< 缓冲区容量（条目数）
        uint32 head_ = 0;              ///< 下一个写入位置（生产者）
        uint32 tail_ = 0;              ///< 下一个读取位置（消费者）
        uint32 count_ = 0;             ///< 当前缓冲区中的条目数
        mutable SpinLock buffer_lock_; ///< 保护 head_/tail_/count_ 的自旋锁

        // ---- 丢弃统计 ----
        SafeNumeric<uint32> dropped_count_; ///< 丢弃条目计数（原子）
        bool drop_warned_ = false;          ///< 是否已输出丢弃警告

        // ---- 同步原语 ----
        Semaphore semaphore_; ///< 通知写入线程有新条目
        SafeFlag running_;    ///< 写入线程运行标志

        // ---- 写入线程 ----
        Thread writer_thread_;

        // ---- 文件 ----
        FILE *file_ = nullptr;
        char file_path_[kMaxPathLen] = {};
        uint32 max_file_size_ = 0;
        uint32 max_backup_files_ = 0;
        uint32 flush_interval_ms_ = 0;
        uint32 current_file_size_ = 0; ///< 当前文件已写入字节数

        /**
         * @brief 写入线程入口
         *
         * @param[in] p_userdata FileLogger 指针
         */
        static void WriterThreadFunc(void *p_userdata);

        /**
         * @brief 写入线程主循环
         */
        void WriterLoop();

        /**
         * @brief 批量出队并写入文件
         *
         * 一次出队所有待写条目，减少 fwrite 调用次数。
         */
        void FlushBuffer();

        /**
         * @brief 检查并执行文件轮转
         *
         * 当 current_file_size_ >= max_file_size_ 时：
         *   1. 关闭当前文件
         *   2. 将 .log → .log.1, .log.1 → .log.2, ...
         *   3. 删除超出 max_backup_files_ 的最旧备份
         *   4. 打开新文件
         */
        void RotateFile();

        /**
         * @brief 打开日志文件
         * @return Error::kOk     打开成功
         * @return Error::kFailed 打开失败
         */
        Error OpenFile();

        /**
         * @brief 关闭日志文件
         */
        void CloseFile();

        /**
         * @brief 写入一条 Entry 到文件
         *
         * @param[in] p_entry 日志条目
         */
        void WriteEntry(const Entry &p_entry);
    };

} // namespace arhud
