/**
 * @file logger.cpp
 * @brief Logger 系统实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-02
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "io/logger.h"
#include "io/logger_file.h"
#include "os/memory.h"

#if defined(ARHUD_PLATFORM_WINDOWS)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // 内部辅助
    // ═══════════════════════════════════════════════════════════════════════

    namespace
    {

        /**
         * @brief 日志级别前缀字符串
         */
        const char *LogLevelPrefix(LogLevel p_level)
        {
            switch (p_level) {
            case LogLevel::kVerbose: return "[VERBOSE]";
            case LogLevel::kInfo:    return "[INFO]   ";
            case LogLevel::kWarn:    return "[WARN]   ";
            case LogLevel::kError:   return "[ERROR]  ";
            case LogLevel::kFatal:   return "[FATAL]  ";
            default:                 return "[?????]  ";
            }
        }

        LogLevel g_log_level = LogLevel::kVerbose;

        CompositeLogger *g_global_logger = nullptr;
        StdLogger *g_std_logger = nullptr;
        FileLogger *g_file_logger = nullptr;

    } // anonymous namespace

    // ═══════════════════════════════════════════════════════════════════════
    // StdLogger
    // ═══════════════════════════════════════════════════════════════════════

    void StdLogger::LogV(LogLevel p_level, const char *p_format,
                         va_list p_list, bool p_err)
    {
        std::fprintf(stderr, "%s ", LogLevelPrefix(p_level));
        std::vfprintf(stderr, p_format, p_list);
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }

    void StdLogger::LogError(const char *p_function, const char *p_file,
                             int32 p_line, const char *p_code,
                             const char *p_rationale, LogLevel p_level)
    {
        const char *level_str = "";
        switch (p_level) {
        case LogLevel::kWarn:  level_str = "WARNING"; break;
        case LogLevel::kError: level_str = "ERROR";   break;
        case LogLevel::kFatal: level_str = "FATAL";   break;
        default:               level_str = "???";     break;
        }

        if (p_rationale != nullptr && p_rationale[0] != '\0') {
            std::fprintf(stderr, "%s: %s: %s\n"
                         "     At: %s (%s:%d)\n",
                         level_str, p_code, p_rationale,
                         p_function, p_file, p_line);
        } else {
            std::fprintf(stderr, "%s: %s\n"
                         "     At: %s (%s:%d)\n",
                         level_str, p_code,
                         p_function, p_file, p_line);
        }
        std::fflush(stderr);

#if defined(ARHUD_PLATFORM_WINDOWS)
        char debug_buf[1024];
        if (p_rationale != nullptr && p_rationale[0] != '\0') {
            std::snprintf(debug_buf, sizeof(debug_buf),
                          "%s: %s: %s\n     At: %s (%s:%d)\n",
                          level_str, p_code, p_rationale,
                          p_function, p_file, p_line);
        } else {
            std::snprintf(debug_buf, sizeof(debug_buf),
                          "%s: %s\n     At: %s (%s:%d)\n",
                          level_str, p_code,
                          p_function, p_file, p_line);
        }
        OutputDebugStringA(debug_buf);
#endif
    }

    // ═══════════════════════════════════════════════════════════════════════
    // CompositeLogger
    // ═══════════════════════════════════════════════════════════════════════

    bool CompositeLogger::AddLogger(Logger *p_logger)
    {
        if (p_logger == nullptr || count_ >= kMaxLoggers) {
            return false;
        }
        loggers_[count_] = p_logger;
        ++count_;
        return true;
    }

    bool CompositeLogger::RemoveLogger(Logger *p_logger)
    {
        if (p_logger == nullptr) {
            return false;
        }
        for (uint32 i = 0; i < count_; ++i) {
            if (loggers_[i] == p_logger) {
                loggers_[i] = loggers_[count_ - 1];
                loggers_[count_ - 1] = nullptr;
                --count_;
                return true;
            }
        }
        return false;
    }

    uint32 CompositeLogger::GetLoggerCount() const
    {
        return count_;
    }

    void CompositeLogger::LogV(LogLevel p_level, const char *p_format,
                               va_list p_list, bool p_err)
    {
        for (uint32 i = 0; i < count_; ++i) {
            va_list list_copy;
            va_copy(list_copy, p_list);
            loggers_[i]->LogV(p_level, p_format, list_copy, p_err);
            va_end(list_copy);
        }
    }

    void CompositeLogger::LogError(const char *p_function, const char *p_file,
                                   int32 p_line, const char *p_code,
                                   const char *p_rationale, LogLevel p_level)
    {
        for (uint32 i = 0; i < count_; ++i) {
            loggers_[i]->LogError(p_function, p_file, p_line,
                                  p_code, p_rationale, p_level);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 全局日志接口
    // ═══════════════════════════════════════════════════════════════════════

    LogLevel GetLogLevel()
    {
        return g_log_level;
    }

    void SetLogLevel(LogLevel p_level)
    {
        g_log_level = p_level;
    }

    CompositeLogger *GetGlobalLogger()
    {
        return g_global_logger;
    }

    void InitializeLogger()
    {
        if (g_global_logger != nullptr) {
            return;
        }

        g_std_logger = ARHUD_NEW(StdLogger)();
        g_global_logger = ARHUD_NEW(CompositeLogger)();
        g_global_logger->AddLogger(g_std_logger);
    }

    Error InitializeFileLogger(const char *p_file_path)
    {
        if (g_global_logger == nullptr) {
            return Error::kFailed;
        }

        if (g_file_logger != nullptr) {
            return Error::kOK;
        }

        FileLogger::Config config;
        if (p_file_path != nullptr) {
            config.file_path = p_file_path;
        }

        g_file_logger = ARHUD_NEW(FileLogger)(config);
        Error err = g_file_logger->Start();
        if (err != Error::kOK) {
            ARHUD_DELETE(g_file_logger);
            g_file_logger = nullptr;
            return err;
        }

        g_global_logger->AddLogger(g_file_logger);
        return Error::kOK;
    }

    void ShutdownLogger()
    {
        if (g_global_logger != nullptr && g_file_logger != nullptr) {
            g_global_logger->RemoveLogger(g_file_logger);
        }

        if (g_file_logger != nullptr) {
            g_file_logger->Stop();
            ARHUD_DELETE(g_file_logger);
            g_file_logger = nullptr;
        }

        if (g_global_logger != nullptr) {
            ARHUD_DELETE(g_global_logger);
            g_global_logger = nullptr;
        }
        if (g_std_logger != nullptr) {
            ARHUD_DELETE(g_std_logger);
            g_std_logger = nullptr;
        }
    }

    void LogMessage(LogLevel p_level, const char *p_format, ...)
    {
        if (p_level < g_log_level) {
            return;
        }

        if (g_global_logger == nullptr) {
            return;
        }

        va_list list;
        va_start(list, p_format);
        bool is_err = (p_level >= LogLevel::kWarn);
        g_global_logger->LogV(p_level, p_format, list, is_err);
        va_end(list);
    }

    void LogErrorGlobal(const char *p_function, const char *p_file,
                        int32 p_line, const char *p_code,
                        const char *p_rationale, LogLevel p_level)
    {
        if (p_level < g_log_level) {
            return;
        }

        if (g_global_logger == nullptr) {
            return;
        }

        g_global_logger->LogError(p_function, p_file, p_line,
                                  p_code, p_rationale, p_level);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // AssertFailed（由 ARHUD_ASSERT 宏调用）
    // ═══════════════════════════════════════════════════════════════════════

    void AssertFailed(const char *p_cond, const char *p_msg)
    {
        if (g_global_logger != nullptr)
        {
            g_global_logger->LogError("ARHUD_ASSERT", __FILE__, __LINE__,
                                      p_cond, p_msg, LogLevel::kFatal);
            LogMessage(LogLevel::kFatal, "ASSERT: %s - %s", p_cond, p_msg);
        }
        else
        {
            std::fprintf(stderr, "[FATAL]  ASSERT: %s - %s\n", p_cond, p_msg);
            std::fflush(stderr);
        }
        std::abort();
    }

} // namespace arhud
