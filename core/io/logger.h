/**
 * @file logger.h
 * @brief 日志系统
 *
 * 提供引擎全局日志功能：
 *   - LogLevel       五级日志级别（Verbose / Info / Warn / Error / Fatal）
 *   - Logger         抽象日志输出接口
 *   - StdLogger      stderr 输出（默认后端）
 *   - CompositeLogger 多通道组合输出
 *   - 全局宏 API      ARHUD_LOG_VERBOSE / INFO / WARN / ERROR / FATAL
 *
 * 设计决策：
 *   - 格式化使用 vsnprintf（计算函数，非输出函数，不违反"禁止 printf"规则）
 *   - 输出通道完全自控：stderr + 平台 API，不走 stdout
 *   - 宏级短路：Release 中 ARHUD_LOG_VERBOSE 编译为空
 *   - 线程安全：每次 LogV 调用内部加锁，渲染线程零阻塞设计留给后续优化
 *
 * 参考 Godot core/os/logger.h 设计。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-02
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <cstdarg>
#include <cstdio>
#include "typedefs.h"

namespace arhud
{

    class FileLogger;

    // ═══════════════════════════════════════════════════════════════════════
    // LogLevel
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 日志级别
     *
     * 数值越大越严重。全局阈值以下的日志被丢弃。
     */
    enum class LogLevel : int32
    {
        kVerbose = 0, ///< 详细调试信息（仅 Debug 构建启用）
        kInfo = 1,    ///< 一般信息
        kWarn = 2,    ///< 警告（可恢复的异常）
        kError = 3,   ///< 错误（功能失败）
        kFatal = 4,   ///< 致命错误（程序即将终止）
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Logger（抽象接口）
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 日志输出抽象接口
     *
     * 所有日志后端（StdLogger / FileLogger / DebugLogger 等）
     * 必须实现 LogV 和 LogError 虚函数。
     *
     * @note 参考 Godot core/os/logger.h Logger
     */
    class Logger
    {
    public:
        virtual ~Logger() = default;

        /**
         * @brief 格式化日志输出
         *
         * @param[in] p_level  日志级别
         * @param[in] p_format vsnprintf 兼容的格式字符串
         * @param[in] p_list   可变参数列表
         * @param[in] p_err    true 时输出到 stderr，false 时输出到 stdout
         *
         * @note p_err 参数保留用于区分标准输出和错误输出通道，
         *       但引擎禁止使用 stdout，实际行为由实现决定
         */
        virtual void LogV(LogLevel p_level, const char *p_format,
                          va_list p_list, bool p_err) = 0;

        /**
         * @brief 结构化错误日志
         *
         * 输出包含源码位置的错误信息，格式由实现决定。
         *
         * @param[in] p_function  出错函数名
         * @param[in] p_file      出错源文件名
         * @param[in] p_line      出错行号
         * @param[in] p_code      错误码/标识字符串
         * @param[in] p_rationale 错误原因描述（可为 nullptr）
         * @param[in] p_level     日志级别
         */
        virtual void LogError(const char *p_function, const char *p_file,
                              int32 p_line, const char *p_code,
                              const char *p_rationale,
                              LogLevel p_level = LogLevel::kError) = 0;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // StdLogger
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 标准错误输出日志
     *
     * 将日志输出到 stderr（禁止使用 stdout）。
     * 这是引擎的默认日志后端。
     *
     * @note 参考 Godot core/os/logger.h StdLogger
     */
    class StdLogger : public Logger
    {
    public:
        void LogV(LogLevel p_level, const char *p_format,
                  va_list p_list, bool p_err) override;

        void LogError(const char *p_function, const char *p_file,
                      int32 p_line, const char *p_code,
                      const char *p_rationale,
                      LogLevel p_level = LogLevel::kError) override;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // CompositeLogger
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 组合日志（多通道输出）
     *
     * 同时向多个 Logger 输出，用于同时写 stderr + 文件 + 调试器等场景。
     *
     * @note 参考 Godot core/os/logger.h CompositeLogger
     */
    class CompositeLogger : public Logger
    {
        static constexpr uint32 kMaxLoggers = 8;

        Logger *loggers_[kMaxLoggers] = {};
        uint32 count_ = 0;

    public:
        /**
         * @brief 添加一个 Logger
         *
         * @param[in] p_logger Logger 实例指针（不获取所有权）
         * @return true  添加成功
         * @return false 已达上限（kMaxLoggers）
         */
        bool AddLogger(Logger *p_logger);

        /**
         * @brief 移除一个 Logger
         *
         * @param[in] p_logger 要移除的 Logger 指针
         * @return true  移除成功
         * @return false 未找到
         */
        bool RemoveLogger(Logger *p_logger);

        /**
         * @brief 获取 Logger 数量
         * @return 当前已添加的 Logger 数量
         */
        uint32 GetLoggerCount() const;

        void LogV(LogLevel p_level, const char *p_format,
                  va_list p_list, bool p_err) override;

        void LogError(const char *p_function, const char *p_file,
                      int32 p_line, const char *p_code,
                      const char *p_rationale,
                      LogLevel p_level = LogLevel::kError) override;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 全局日志接口
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 获取全局日志阈值
     * @return 当前阈值级别，低于此级别的日志被丢弃
     */
    LogLevel GetLogLevel();

    /**
     * @brief 设置全局日志阈值
     *
     * 低于 p_level 的日志将被全局丢弃（宏级短路之外的第二层过滤）。
     *
     * @param[in] p_level 新的阈值级别
     */
    void SetLogLevel(LogLevel p_level);

    /**
     * @brief 获取全局 CompositeLogger
     *
     * 引擎启动时自动创建，包含默认的 StdLogger。
     * 可通过 AddLogger 添加更多输出通道。
     *
     * @return 全局 CompositeLogger 指针
     */
    CompositeLogger *GetGlobalLogger();

    /**
     * @brief 初始化全局日志系统
     *
     * 创建 CompositeLogger + StdLogger 并设为全局默认。
     * 应在程序入口最早调用。
     */
    void InitializeLogger();

    /**
     * @brief 初始化文件日志后端
     *
     * 创建 FileLogger 并添加到全局 CompositeLogger。
     * 必须在 InitializeLogger() 之后调用。
     *
     * @param[in] p_file_path 日志文件路径（nullptr 则使用默认 "arhud.log"）
     * @return Error::kOK     成功
     * @return Error::kFailed 文件打开或线程启动失败
     */
    Error InitializeFileLogger(const char *p_file_path = nullptr);

    /**
     * @brief 关闭全局日志系统
     *
     * 停止 FileLogger（刷盘）→ 移除所有 Logger → 释放资源。
     * 应在程序退出时调用。
     */
    void ShutdownLogger();

    /**
     * @brief 全局格式化日志输出（内部使用）
     *
     * 由宏 API 调用，不应直接使用。
     *
     * @param[in] p_level  日志级别
     * @param[in] p_format 格式字符串
     * @param[in] ...      可变参数
     */
    void LogMessage(LogLevel p_level, const char *p_format, ...);

    /**
     * @brief 全局结构化错误输出（内部使用）
     *
     * 由宏 API 调用，不应直接使用。
     *
     * @param[in] p_function  函数名
     * @param[in] p_file      文件名
     * @param[in] p_line      行号
     * @param[in] p_code      错误码
     * @param[in] p_rationale 原因
     * @param[in] p_level     日志级别
     */
    void LogErrorGlobal(const char *p_function, const char *p_file,
                        int32 p_line, const char *p_code,
                        const char *p_rationale,
                        LogLevel p_level = LogLevel::kError);

} // namespace arhud

// ═══════════════════════════════════════════════════════════════════════
// 日志宏 API
// ═══════════════════════════════════════════════════════════════════════
//
// 使用方式：
//   ARHUD_LOG_INFO("Texture created: %s (id=%u)", name, id);
//   ARHUD_LOG_WARN("Budget exceeded: %zu / %zu", used, budget);
//   ARHUD_LOG_ERROR("Failed to create shader: %s", error_msg);
//
// 宏级短路：
//   - ARHUD_LOG_VERBOSE 在 Release 构建中编译为空操作
//   - 其他级别始终编译，运行时由全局阈值过滤
//
// ARHUD_LOG_ERROR / FATAL 自动附带源码位置（__func__, __FILE__, __LINE__）

/**
 * @def ARHUD_LOG_VERBOSE
 * @brief 详细调试日志（Debug 构建可用，Release 编译为空）
 */
#if ARHUD_DEBUG
#define ARHUD_LOG_VERBOSE(fmt, ...) \
    ::arhud::LogMessage(::arhud::LogLevel::kVerbose, fmt, ##__VA_ARGS__)
#else
#define ARHUD_LOG_VERBOSE(fmt, ...) ((void)0)
#endif

/**
 * @def ARHUD_LOG_INFO
 * @brief 一般信息日志
 */
#define ARHUD_LOG_INFO(fmt, ...) \
    ::arhud::LogMessage(::arhud::LogLevel::kInfo, fmt, ##__VA_ARGS__)

/**
 * @def ARHUD_LOG_WARN
 * @brief 警告日志
 */
#define ARHUD_LOG_WARN(fmt, ...) \
    ::arhud::LogMessage(::arhud::LogLevel::kWarn, fmt, ##__VA_ARGS__)

/**
 * @def ARHUD_LOG_ERROR
 * @brief 错误日志（自动附带源码位置）
 */
#define ARHUD_LOG_ERROR(code, fmt, ...)                                     \
    do                                                                      \
    {                                                                       \
        ::arhud::LogErrorGlobal(__func__, __FILE__, __LINE__,               \
                                code, nullptr, ::arhud::LogLevel::kError);  \
        ::arhud::LogMessage(::arhud::LogLevel::kError, fmt, ##__VA_ARGS__); \
    } while (0)

/**
 * @def ARHUD_LOG_FATAL
 * @brief 致命错误日志（自动附带源码位置，输出后 abort）
 */
#define ARHUD_LOG_FATAL(code, fmt, ...)                                     \
    do                                                                      \
    {                                                                       \
        ::arhud::LogErrorGlobal(__func__, __FILE__, __LINE__,               \
                                code, nullptr, ::arhud::LogLevel::kFatal);  \
        ::arhud::LogMessage(::arhud::LogLevel::kFatal, fmt, ##__VA_ARGS__); \
        std::abort();                                                       \
    } while (0)
