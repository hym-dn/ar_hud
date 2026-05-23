/**
 * @file logger_file.cpp
 * @brief FileLogger 实现（异步写入 + 文件轮转）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-01
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "io/logger_file.h"
#include "os/memory.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // 构造 / 析构
    // ═══════════════════════════════════════════════════════════════════════

    FileLogger::FileLogger(const Config &p_config)
        : capacity_(p_config.ring_buffer_capacity),
          max_file_size_(p_config.max_file_size),
          max_backup_files_(p_config.max_backup_files),
          flush_interval_ms_(p_config.flush_interval_ms)
    {
        if (p_config.file_path != nullptr)
        {
            size_t src_len = std::strlen(p_config.file_path);
            size_t copy_len = (src_len < kMaxPathLen - 1) ? src_len : kMaxPathLen - 1;
            std::memcpy(file_path_, p_config.file_path, copy_len);
            file_path_[copy_len] = '\0';
        }
        ring_buffer_ = static_cast<Entry *>(
            memory::AllocZeroed(sizeof(Entry) * capacity_));
    }

    FileLogger::~FileLogger()
    {
        Stop();
        if (ring_buffer_ != nullptr)
        {
            memory::Free(ring_buffer_);
            ring_buffer_ = nullptr;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Start / Stop
    // ═══════════════════════════════════════════════════════════════════════

    Error FileLogger::Start()
    {
        if (running_.IsSet())
        {
            return Error::kOK;
        }

        Error err = OpenFile();
        if (err != Error::kOK)
        {
            return err;
        }

        running_.Set();
        drop_warned_ = false;

        Thread::ID tid = writer_thread_.Start(
            WriterThreadFunc, this,
            Thread::Settings{Thread::kPriorityLow});

        if (tid == Thread::kUnassignedId)
        {
            running_.Clear();
            CloseFile();
            return Error::kFailed;
        }

        return Error::kOK;
    }

    void FileLogger::Stop()
    {
        if (!running_.IsSet())
        {
            return;
        }

        running_.Clear();
        semaphore_.Post();

        writer_thread_.WaitToFinish();

        FlushBuffer();
        CloseFile();
    }

    bool FileLogger::IsRunning() const
    {
        return running_.IsSet();
    }

    uint32 FileLogger::GetDroppedCount() const
    {
        return dropped_count_.Get();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Logger 接口实现（生产者 - 热路径）
    // ═══════════════════════════════════════════════════════════════════════

    void FileLogger::LogV(LogLevel p_level, const char *p_format,
                          va_list p_list, bool p_err)
    {
        if (!running_.IsSet())
        {
            return;
        }

        Entry entry;
        entry.level = p_level;

        int len = std::vsnprintf(entry.message, kMaxMessageLen, p_format, p_list);
        if (len < 0)
        {
            return;
        }
        if (static_cast<uint32>(len) >= kMaxMessageLen)
        {
            len = static_cast<int>(kMaxMessageLen) - 1;
        }
        entry.len = static_cast<uint16>(len);

        {
            SpinLock::Guard guard(buffer_lock_);

            if (count_ >= capacity_)
            {
                dropped_count_.Increment();
                if (!drop_warned_)
                {
                    drop_warned_ = true;
                }
                return;
            }

            ring_buffer_[head_] = entry;
            head_ = (head_ + 1) % capacity_;
            ++count_;
        }

        semaphore_.Post();
    }

    void FileLogger::LogError(const char *p_function, const char *p_file,
                              int32 p_line, const char *p_code,
                              const char *p_rationale, LogLevel p_level)
    {
        if (!running_.IsSet())
        {
            return;
        }

        Entry entry;
        entry.level = p_level;

        if (p_rationale != nullptr && p_rationale[0] != '\0')
        {
            int len = std::snprintf(entry.message, kMaxMessageLen,
                                    "%s: %s: %s\n     At: %s (%s:%d)",
                                    (p_level == LogLevel::kWarn ? "WARNING" : "ERROR"),
                                    p_code, p_rationale, p_function, p_file, p_line);
            entry.len = static_cast<uint16>((len < 0) ? 0 : (static_cast<uint32>(len) >= kMaxMessageLen ? kMaxMessageLen - 1 : static_cast<uint16>(len)));
        }
        else
        {
            int len = std::snprintf(entry.message, kMaxMessageLen,
                                    "%s: %s\n     At: %s (%s:%d)",
                                    (p_level == LogLevel::kWarn ? "WARNING" : "ERROR"),
                                    p_code, p_function, p_file, p_line);
            entry.len = static_cast<uint16>((len < 0) ? 0 : (static_cast<uint32>(len) >= kMaxMessageLen ? kMaxMessageLen - 1 : static_cast<uint16>(len)));
        }

        {
            SpinLock::Guard guard(buffer_lock_);

            if (count_ >= capacity_)
            {
                dropped_count_.Increment();
                return;
            }

            ring_buffer_[head_] = entry;
            head_ = (head_ + 1) % capacity_;
            ++count_;
        }

        semaphore_.Post();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 写入线程（消费者）
    // ═══════════════════════════════════════════════════════════════════════

    void FileLogger::WriterThreadFunc(void *p_userdata)
    {
        auto *self = static_cast<FileLogger *>(p_userdata);
        self->WriterLoop();
    }

    void FileLogger::WriterLoop()
    {
        while (running_.IsSet())
        {
            semaphore_.Wait();

            FlushBuffer();

            if (flush_interval_ms_ > 0 && file_ != nullptr)
            {
                std::fflush(file_);
            }
        }

        FlushBuffer();
        if (file_ != nullptr)
        {
            std::fflush(file_);
        }
    }

    void FileLogger::FlushBuffer()
    {
        for (;;)
        {
            Entry entry;
            {
                SpinLock::Guard guard(buffer_lock_);
                if (count_ == 0)
                {
                    break;
                }
                entry = ring_buffer_[tail_];
                tail_ = (tail_ + 1) % capacity_;
                --count_;
            }

            WriteEntry(entry);

            if (max_file_size_ > 0 && current_file_size_ >= max_file_size_)
            {
                RotateFile();
            }
        }

        if (drop_warned_ && dropped_count_.Get() > 0)
        {
            uint32 dropped = dropped_count_.Exchange(0);
            if (dropped > 0 && file_ != nullptr)
            {
                char msg[128];
                int len = std::snprintf(
                    msg, sizeof(msg),
                    "[WARN]   %u log entries dropped (buffer full)\n", dropped);
                if (len > 0)
                {
                    std::fwrite(msg, 1, static_cast<size_t>(len), file_);
                    current_file_size_ += static_cast<uint32>(len);
                }
            }
            drop_warned_ = false;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 文件操作
    // ═══════════════════════════════════════════════════════════════════════

    void FileLogger::WriteEntry(const Entry &p_entry)
    {
        if (file_ == nullptr)
        {
            return;
        }

        const char *prefix = "";
        switch (p_entry.level)
        {
        case LogLevel::kVerbose:
            prefix = "[VERBOSE] ";
            break;
        case LogLevel::kInfo:
            prefix = "[INFO]    ";
            break;
        case LogLevel::kWarn:
            prefix = "[WARN]    ";
            break;
        case LogLevel::kError:
            prefix = "[ERROR]   ";
            break;
        case LogLevel::kFatal:
            prefix = "[FATAL]   ";
            break;
        default:
            prefix = "[?????]   ";
            break;
        }

        size_t prefix_len = std::strlen(prefix);
        std::fwrite(prefix, 1, prefix_len, file_);
        std::fwrite(p_entry.message, 1, p_entry.len, file_);
        std::fwrite("\n", 1, 1, file_);

        current_file_size_ += static_cast<uint32>(prefix_len + p_entry.len + 1);
    }

    Error FileLogger::OpenFile()
    {
        file_ = std::fopen(file_path_, "a");
        if (file_ == nullptr)
        {
            return Error::kFailed;
        }

        std::fseek(file_, 0, SEEK_END);
        current_file_size_ = static_cast<uint32>(std::ftell(file_));

        return Error::kOK;
    }

    void FileLogger::CloseFile()
    {
        if (file_ != nullptr)
        {
            std::fflush(file_);
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    void FileLogger::RotateFile()
    {
        CloseFile();

        for (uint32 i = max_backup_files_; i >= 1; --i)
        {
            char old_path[kMaxPathLen];
            char new_path[kMaxPathLen];

            if (i == 1)
            {
                std::snprintf(old_path, kMaxPathLen, "%s", file_path_);
            }
            else
            {
                std::snprintf(old_path, kMaxPathLen, "%s.%u", file_path_, i - 1);
            }
            std::snprintf(new_path, kMaxPathLen, "%s.%u", file_path_, i);

            std::remove(new_path);
            std::rename(old_path, new_path);
        }

        OpenFile();
    }

} // namespace arhud
