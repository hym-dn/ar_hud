/**
 * @file ring_buffer.h
 * @brief 环形缓冲区（FIFO 队列）
 *
 * 参考 Godot 4.6 RingBuffer 设计，为 ARHud 渲染引擎提供高效的
 * 定容 FIFO 数据结构。容量必须为 2 的幂，使用位掩码替代取模运算。
 *
 * 设计取舍：
 *   - 容量为 2 的幂：位掩码 (& mask) 替代取模 (% capacity)，零分支开销
 *   - 浪费一个槽位：区分"满"和"空"状态无需额外标志位
 *   - 基于 LocalVector：堆分配缓冲区，支持运行时 Resize
 *   - 非线程安全：多线程场景需外部加锁（与 SpinLock/Mutex 配合）
 *
 * 与 Godot RingBuffer 的关键差异：
 *   - Godot 使用 int 索引，ARHud 使用 uint32_t
 *   - Godot 的 resize() 接受 2 的幂指数，ARHud 的 Reserve() 接受实际容量
 *   - ARHud 命名遵循 PascalCase 约定
 *   - ARHud 增加 Peek / ReadBatch / WriteBatch 等便捷接口
 *
 * 典型用途：
 * - 主线程→渲染线程命令队列（单生产者单消费者）
 * - Staging Buffer 帧循环管理
 * - FileLogger 异步写入缓冲区
 * - 音频采样环形缓冲
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-06
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <type_traits>
#include <utility>

#include "template/local_vector.h"
#include "typedefs.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // RingBuffer
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 环形缓冲区（FIFO 队列）
     *
     * 定容环形缓冲区，容量必须为 2 的幂。
     * 使用位掩码替代取模运算，浪费一个槽位区分满/空状态。
     *
     * @tparam T 元素类型（必须可复制或可移动）
     */
    template <typename T>
    class RingBuffer
    {
    public:
        // ═══════════════════════════════════════════════════════════════
        // 构造 / 析构
        // ═══════════════════════════════════════════════════════════════

        RingBuffer() = default;

        /**
         * @brief 构造并预留容量
         *
         * @param[in] p_capacity 期望容量（自动向上取整到 2 的幂）
         */
        explicit RingBuffer(uint32_t p_capacity)
        {
            Reserve(p_capacity);
        }

        ~RingBuffer() { Clear(); }

        RingBuffer(const RingBuffer &p_other)
            : buffer_(p_other.buffer_),
              read_pos_(p_other.read_pos_),
              write_pos_(p_other.write_pos_),
              size_mask_(p_other.size_mask_)
        {
        }

        RingBuffer(RingBuffer &&p_other) noexcept
            : buffer_(std::move(p_other.buffer_)),
              read_pos_(p_other.read_pos_),
              write_pos_(p_other.write_pos_),
              size_mask_(p_other.size_mask_)
        {
            p_other.read_pos_ = 0;
            p_other.write_pos_ = 0;
            p_other.size_mask_ = 0;
        }

        RingBuffer &operator=(const RingBuffer &p_other)
        {
            if (this != &p_other)
            {
                Clear();
                buffer_ = p_other.buffer_;
                read_pos_ = p_other.read_pos_;
                write_pos_ = p_other.write_pos_;
                size_mask_ = p_other.size_mask_;
            }
            return *this;
        }

        RingBuffer &operator=(RingBuffer &&p_other) noexcept
        {
            if (this != &p_other)
            {
                Clear();
                buffer_ = std::move(p_other.buffer_);
                read_pos_ = p_other.read_pos_;
                write_pos_ = p_other.write_pos_;
                size_mask_ = p_other.size_mask_;
                p_other.read_pos_ = 0;
                p_other.write_pos_ = 0;
                p_other.size_mask_ = 0;
            }
            return *this;
        }

        // ═══════════════════════════════════════════════════════════════
        // 容量查询
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 获取缓冲区总容量（含浪费的 1 个槽位）
         *
         * 实际可存储的最大元素数为 Capacity() - 1。
         */
        ARHUD_ALWAYS_INLINE uint32_t Capacity() const { return buffer_.Size(); }

        /**
         * @brief 获取当前已存储的元素数量
         */
        uint32_t Size() const
        {
            int32_t diff = static_cast<int32_t>(write_pos_) - static_cast<int32_t>(read_pos_);
            if (diff < 0)
            {
                return static_cast<uint32_t>(static_cast<int32_t>(Capacity()) + diff);
            }
            return static_cast<uint32_t>(diff);
        }

        /**
         * @brief 获取剩余可写入空间
         */
        uint32_t SpaceLeft() const
        {
            return Capacity() - Size() - 1;
        }

        /**
         * @brief 检查缓冲区是否为空
         */
        ARHUD_ALWAYS_INLINE bool IsEmpty() const { return read_pos_ == write_pos_; }

        /**
         * @brief 检查缓冲区是否已满
         */
        bool IsFull() const
        {
            return ((write_pos_ + 1) & size_mask_) == read_pos_;
        }

        // ═══════════════════════════════════════════════════════════════
        // 写入操作
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 写入单个元素
         *
         * @param[in] p_val 要写入的值
         *
         * @return Error::kOK 成功，Error::kOutOfMemory 缓冲区已满
         */
        Error Write(const T &p_val)
        {
            if (IsFull())
            {
                return Error::kOutOfMemory;
            }
            buffer_[Inc(write_pos_, 1)] = p_val;
            return Error::kOK;
        }

        /**
         * @brief 写入单个元素（移动语义）
         *
         * @param[in] p_val 要写入的值（右值）
         *
         * @return Error::kOK 成功，Error::kOutOfMemory 缓冲区已满
         */
        Error Write(T &&p_val)
        {
            if (IsFull())
            {
                return Error::kOutOfMemory;
            }
            buffer_[Inc(write_pos_, 1)] = std::move(p_val);
            return Error::kOK;
        }

        /**
         * @brief 批量写入元素
         *
         * @param[in] p_buf 源数据数组
         * @param[in] p_count 要写入的元素数量
         *
         * @return 实际写入的元素数量（可能小于 p_count）
         */
        uint32_t WriteBatch(const T *p_buf, uint32_t p_count)
        {
            ARHUD_ASSERT(p_buf != nullptr, "RingBuffer::WriteBatch() null buffer");

            uint32_t left = SpaceLeft();
            p_count = p_count < left ? p_count : left;

            uint32_t pos = write_pos_;
            uint32_t to_write = p_count;
            uint32_t src = 0;

            while (to_write > 0)
            {
                uint32_t end = pos + to_write;
                end = end < Capacity() ? end : Capacity();
                uint32_t total = end - pos;

                for (uint32_t i = 0; i < total; ++i)
                {
                    buffer_[pos + i] = p_buf[src++];
                }
                to_write -= total;
                pos = 0;
            }

            Inc(write_pos_, p_count);
            return p_count;
        }

        // ═══════════════════════════════════════════════════════════════
        // 读取操作
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 读取并移除队首元素
         *
         * @param[out] r_val 接收读取值的引用
         *
         * @return Error::kOK 成功，Error::kFailed 缓冲区为空
         */
        Error Read(T &r_val)
        {
            if (IsEmpty())
            {
                return Error::kFailed;
            }
            r_val = buffer_[Inc(read_pos_, 1)];
            return Error::kOK;
        }

        /**
         * @brief 读取并移除队首元素（移动语义）
         *
         * @param[out] r_val 接收读取值的引用
         *
         * @return Error::kOK 成功，Error::kFailed 缓冲区为空
         */
        Error ReadMove(T &r_val)
        {
            if (IsEmpty())
            {
                return Error::kFailed;
            }
            r_val = std::move(buffer_[Inc(read_pos_, 1)]);
            return Error::kOK;
        }

        /**
         * @brief 批量读取元素
         *
         * @param[out] p_buf   目标数组
         * @param[in]  p_count 要读取的元素数量
         *
         * @return 实际读取的元素数量（可能小于 p_count）
         */
        uint32_t ReadBatch(T *p_buf, uint32_t p_count)
        {
            return ReadBatch(p_buf, p_count, true);
        }

        /**
         * @brief 批量读取元素（可选是否推进读指针）
         *
         * @param[out] p_buf     目标数组
         * @param[in]  p_count   要读取的元素数量
         * @param[in]  p_advance 是否推进读指针（true=读取并移除，false=仅复制）
         *
         * @return 实际读取的元素数量（可能小于 p_count）
         */
        uint32_t ReadBatch(T *p_buf, uint32_t p_count, bool p_advance)
        {
            ARHUD_ASSERT(p_buf != nullptr, "RingBuffer::ReadBatch() null buffer");

            uint32_t left = Size();
            p_count = p_count < left ? p_count : left;

            uint32_t pos = read_pos_;
            uint32_t to_read = p_count;
            uint32_t dst = 0;

            while (to_read > 0)
            {
                uint32_t end = pos + to_read;
                end = end < Capacity() ? end : Capacity();
                uint32_t total = end - pos;

                for (uint32_t i = 0; i < total; ++i)
                {
                    p_buf[dst++] = buffer_[pos + i];
                }
                to_read -= total;
                pos = 0;
            }

            if (p_advance)
            {
                Inc(read_pos_, p_count);
            }

            return p_count;
        }

        /**
         * @brief 查看队首元素但不移除
         *
         * @param[out] r_val 接收队首值的引用
         *
         * @return Error::kOK 成功，Error::kFailed 缓冲区为空
         */
        Error Peek(T &r_val) const
        {
            if (IsEmpty())
            {
                return Error::kFailed;
            }
            r_val = buffer_[read_pos_];
            return Error::kOK;
        }

        /**
         * @brief 从指定偏移处复制元素（不移除，不推进读指针）
         *
         * @param[out] p_buf    目标数组
         * @param[in]  p_offset 从读位置开始的偏移量
         * @param[in]  p_count  要复制的元素数量
         *
         * @return 实际复制的元素数量
         */
        uint32_t Copy(T *p_buf, uint32_t p_offset, uint32_t p_count) const
        {
            ARHUD_ASSERT(p_buf != nullptr, "RingBuffer::Copy() null buffer");

            uint32_t left = Size();
            if (p_offset >= left)
            {
                return 0;
            }

            uint32_t available = left - p_offset;
            p_count = p_count < available ? p_count : available;

            uint32_t pos = read_pos_;
            Inc(pos, p_offset);

            uint32_t to_read = p_count;
            uint32_t dst = 0;

            while (to_read > 0)
            {
                uint32_t end = pos + to_read;
                end = end < Capacity() ? end : Capacity();
                uint32_t total = end - pos;

                for (uint32_t i = 0; i < total; ++i)
                {
                    p_buf[dst++] = buffer_[pos + i];
                }
                to_read -= total;
                pos = 0;
            }

            return p_count;
        }

        /**
         * @brief 在缓冲区中查找元素
         *
         * @param[in] p_val      要查找的值
         * @param[in] p_offset   从读位置开始的偏移量
         * @param[in] p_max_size 最大搜索范围
         *
         * @return 找到返回相对偏移量，未找到返回 -1
         */
        int64_t Find(const T &p_val, uint32_t p_offset = 0, uint32_t p_max_size = UINT32_MAX) const
        {
            uint32_t left = Size();
            if (p_offset >= left)
            {
                return -1;
            }

            uint32_t available = left - p_offset;
            uint32_t search_size = p_max_size < available ? p_max_size : available;

            uint32_t pos = read_pos_;
            Inc(pos, p_offset);

            uint32_t to_read = search_size;
            uint32_t base_offset = 0;

            while (to_read > 0)
            {
                uint32_t end = pos + to_read;
                end = end < Capacity() ? end : Capacity();
                uint32_t total = end - pos;

                for (uint32_t i = 0; i < total; ++i)
                {
                    if (buffer_[pos + i] == p_val)
                    {
                        return static_cast<int64_t>(base_offset + i);
                    }
                }
                base_offset += total;
                to_read -= total;
                pos = 0;
            }

            return -1;
        }

        // ═══════════════════════════════════════════════════════════════
        // 指针操作
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 推进读指针（跳过指定数量的元素）
         *
         * @param[in] p_count 要跳过的元素数量
         *
         * @return 实际跳过的元素数量
         */
        uint32_t AdvanceRead(uint32_t p_count)
        {
            uint32_t left = Size();
            p_count = p_count < left ? p_count : left;
            Inc(read_pos_, p_count);
            return p_count;
        }

        /**
         * @brief 回退写指针（撤销最近写入的元素）
         *
         * @param[in] p_count 要回退的元素数量
         *
         * @return 实际回退的元素数量
         */
        uint32_t DecreaseWrite(uint32_t p_count)
        {
            uint32_t left = SpaceLeft();
            p_count = p_count < left ? p_count : left;
            Inc(write_pos_, Capacity() - p_count);
            return p_count;
        }

        // ═══════════════════════════════════════════════════════════════
        // 容量管理
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 预留缓冲区容量
         *
         * 将 p_capacity 向上取整到最近的 2 的幂。
         * 若当前缓冲区有数据，会保留已有内容。
         *
         * @param[in] p_capacity 期望容量（自动向上取整到 2 的幂）
         */
        void Reserve(uint32_t p_capacity)
        {
            if (p_capacity == 0)
            {
                return;
            }

            uint32_t new_cap = NextPowerOfTwo(p_capacity);
            uint32_t new_mask = new_cap - 1;
            uint32_t old_cap = Capacity();

            buffer_.Resize(new_cap);

            if (old_cap < new_cap && read_pos_ > write_pos_)
            {
                if constexpr (std::is_trivially_copyable_v<T>)
                {
                    std::memmove(static_cast<void *>(&buffer_[old_cap]),
                                 static_cast<const void *>(&buffer_[0]),
                                 write_pos_ * sizeof(T));
                }
                else
                {
                    for (uint32_t i = 0; i < write_pos_; ++i)
                    {
                        buffer_[old_cap + i] = std::move(buffer_[i]);
                    }
                }
                write_pos_ = (old_cap + write_pos_) & new_mask;
            }
            else
            {
                read_pos_ = read_pos_ & new_mask;
                write_pos_ = write_pos_ & new_mask;
            }

            size_mask_ = new_mask;
        }

        /**
         * @brief 清空缓冲区（不释放内存）
         */
        void Clear()
        {
            read_pos_ = 0;
            write_pos_ = 0;
        }

        /**
         * @brief 重置缓冲区（释放内存）
         */
        void Reset()
        {
            buffer_.Clear();
            read_pos_ = 0;
            write_pos_ = 0;
            size_mask_ = 0;
        }

    private:
        LocalVector<T> buffer_;
        uint32_t read_pos_ = 0;
        uint32_t write_pos_ = 0;
        uint32_t size_mask_ = 0;

        /**
         * @brief 递增位置指针（环绕）
         *
         * @param[in,out] p_pos  位置指针
         * @param[in]     p_step 步长
         *
         * @return 递增前的位置
         */
        ARHUD_ALWAYS_INLINE uint32_t Inc(uint32_t &p_pos, uint32_t p_step) const
        {
            uint32_t ret = p_pos;
            p_pos = (p_pos + p_step) & size_mask_;
            return ret;
        }

        /**
         * @brief 计算大于等于 p_val 的最小 2 的幂
         *
         * @param[in] p_val 输入值
         *
         * @return 大于等于 p_val 的最小 2 的幂
         */
        static uint32_t NextPowerOfTwo(uint32_t p_val)
        {
            if (p_val == 0)
            {
                return 1;
            }
            p_val--;
            p_val |= p_val >> 1;
            p_val |= p_val >> 2;
            p_val |= p_val >> 4;
            p_val |= p_val >> 8;
            p_val |= p_val >> 16;
            p_val++;
            return p_val;
        }
    };

} // namespace arhud
