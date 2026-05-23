/**
 * @file bitfield.h
 * @brief 类型安全的位域包装器
 *
 * 为 enum class 提供位运算支持，避免裸整数位运算的类型丢失问题。
 * 参考 Godot 4.6 BitField 设计，结合 Vulkan 风格的标志位枚举。
 *
 * 设计动机：
 *   C++ 的 enum class 禁止隐式转换为整数，因此无法直接使用 | & ^ ~
 *   等位运算符。裸 static_cast<uint32_t> 既冗长又易出错。
 *   BitField<T> 包装 enum class 的底层值，提供完整的位运算接口，
 *   同时保持类型安全——不同枚举类型的 BitField 不能混用。
 *
 * 典型用途：
 * - GPU 缓冲区用途标志（BitField<BufferUsageBits>）
 * - GPU 纹理用途标志（BitField<TextureUsageBits>）
 * - 渲染管线阶段标志（BitField<PipelineStageBits>）
 *
 * 用法示例：
 * @code
 *   enum class BufferUsageBits : uint32_t
 *   {
 *       kTransferFrom = (1 << 0),
 *       kTransferTo   = (1 << 1),
 *       kUniform      = (1 << 4),
 *       kVertex       = (1 << 7),
 *   };
 *
 *   BitField<BufferUsageBits> flags = BufferUsageBits::kVertex |
 *                                     BufferUsageBits::kUniform;
 *   flags.SetFlag(BufferUsageBits::kTransferTo);
 *   bool has_vertex = flags.HasFlag(BufferUsageBits::kVertex);
 *   flags.ClearFlag(BufferUsageBits::kUniform);
 * @endcode
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-05
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <type_traits>

#include "typedefs.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // BitField
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 类型安全的位域包装器
     *
     * 包装 enum class 的底层整数值，提供位运算操作。
     * 要求枚举类型的底层类型为无符号整数（uint32_t 等）。
     *
     * @tparam T 枚举类型（必须为 enum class，底层类型为无符号整数）
     */
    template <typename T>
    class BitField
    {
        static_assert(std::is_enum_v<T>, "BitField<T> requires T to be an enum type");

        using Integral = std::underlying_type_t<T>;

        static_assert(std::is_unsigned_v<Integral>,
                      "BitField<T> requires T to have an unsigned underlying type");

    public:
        // ═══════════════════════════════════════════════════════════════
        // 构造
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 默认构造，所有位清零
         */
        constexpr BitField() = default;

        /**
         * @brief 从枚举值构造（单标志位）
         *
         * @param[in] p_flag 枚举值
         */
        constexpr BitField(T p_flag) : bits_(static_cast<Integral>(p_flag)) {}

        /**
         * @brief 从底层整数值构造（显式）
         *
         * 用于反序列化或与 C API 交互，不推荐常规使用。
         *
         * @param[in] p_bits 底层整数值
         */
        static constexpr BitField FromRaw(Integral p_bits)
        {
            BitField bf;
            bf.bits_ = p_bits;
            return bf;
        }

        // ═══════════════════════════════════════════════════════════════
        // 位运算符
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 位或运算（合并标志）
         */
        constexpr BitField operator|(BitField p_other) const
        {
            return FromRaw(bits_ | p_other.bits_);
        }

        /**
         * @brief 位与运算（交集标志）
         */
        constexpr BitField operator&(BitField p_other) const
        {
            return FromRaw(bits_ & p_other.bits_);
        }

        /**
         * @brief 位异或运算（翻转标志）
         */
        constexpr BitField operator^(BitField p_other) const
        {
            return FromRaw(bits_ ^ p_other.bits_);
        }

        /**
         * @brief 位取反运算
         */
        constexpr BitField operator~() const
        {
            return FromRaw(~bits_);
        }

        // ═══════════════════════════════════════════════════════════════
        // 位运算赋值
        // ═══════════════════════════════════════════════════════════════

        constexpr BitField &operator|=(BitField p_other)
        {
            bits_ |= p_other.bits_;
            return *this;
        }

        constexpr BitField &operator&=(BitField p_other)
        {
            bits_ &= p_other.bits_;
            return *this;
        }

        constexpr BitField &operator^=(BitField p_other)
        {
            bits_ ^= p_other.bits_;
            return *this;
        }

        // ═══════════════════════════════════════════════════════════════
        // 枚举值位运算（允许 BitField | Enum 混合写法）
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 与枚举值的位或运算
         */
        constexpr BitField operator|(T p_flag) const
        {
            return FromRaw(bits_ | static_cast<Integral>(p_flag));
        }

        /**
         * @brief 与枚举值的位与运算
         */
        constexpr BitField operator&(T p_flag) const
        {
            return FromRaw(bits_ & static_cast<Integral>(p_flag));
        }

        /**
         * @brief 与枚举值的位异或运算
         */
        constexpr BitField operator^(T p_flag) const
        {
            return FromRaw(bits_ ^ static_cast<Integral>(p_flag));
        }

        constexpr BitField &operator|=(T p_flag)
        {
            bits_ |= static_cast<Integral>(p_flag);
            return *this;
        }

        constexpr BitField &operator&=(T p_flag)
        {
            bits_ &= static_cast<Integral>(p_flag);
            return *this;
        }

        constexpr BitField &operator^=(T p_flag)
        {
            bits_ ^= static_cast<Integral>(p_flag);
            return *this;
        }

        // ═══════════════════════════════════════════════════════════════
        // 比较
        // ═══════════════════════════════════════════════════════════════

        constexpr bool operator==(BitField p_other) const { return bits_ == p_other.bits_; }
        constexpr bool operator!=(BitField p_other) const { return bits_ != p_other.bits_; }

        // ═══════════════════════════════════════════════════════════════
        // 标志操作
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 设置指定标志位
         *
         * @param[in] p_flag 要设置的标志
         */
        void SetFlag(T p_flag)
        {
            bits_ |= static_cast<Integral>(p_flag);
        }

        /**
         * @brief 清除指定标志位
         *
         * @param[in] p_flag 要清除的标志
         */
        void ClearFlag(T p_flag)
        {
            bits_ &= ~static_cast<Integral>(p_flag);
        }

        /**
         * @brief 检查是否包含指定标志位
         *
         * @param[in] p_flag 要检查的标志
         *
         * @return true 包含该标志
         */
        constexpr bool HasFlag(T p_flag) const
        {
            return (bits_ & static_cast<Integral>(p_flag)) != 0;
        }

        /**
         * @brief 检查是否包含指定 BitField 的所有标志位
         *
         * @param[in] p_flags 要检查的标志集合
         *
         * @return true 包含所有指定标志
         */
        constexpr bool HasAll(BitField p_flags) const
        {
            return (bits_ & p_flags.bits_) == p_flags.bits_;
        }

        /**
         * @brief 检查是否包含指定 BitField 中的任意标志位
         *
         * @param[in] p_flags 要检查的标志集合
         *
         * @return true 包含至少一个指定标志
         */
        constexpr bool HasAny(BitField p_flags) const
        {
            return (bits_ & p_flags.bits_) != 0;
        }

        // ═══════════════════════════════════════════════════════════════
        // 状态查询
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 检查是否没有任何标志位设置
         *
         * @return true 所有位为零
         */
        constexpr bool IsEmpty() const
        {
            return bits_ == 0;
        }

        /**
         * @brief 检查是否至少有一个标志位设置
         *
         * @return true 至少有一个位非零
         */
        constexpr bool HasAnyFlag() const
        {
            return bits_ != 0;
        }

        // ═══════════════════════════════════════════════════════════════
        // 原始值访问
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 获取底层整数值
         *
         * 用于与 C API 交互或序列化。
         *
         * @return 底层整数值
         */
        constexpr Integral GetRaw() const
        {
            return bits_;
        }

        /**
         * @brief 隐式转换为底层整数值
         *
         * 便于直接传入期望整数类型的 C API。
         */
        constexpr operator Integral() const
        {
            return bits_;
        }

    private:
        Integral bits_ = 0;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 自由运算符：Enum op BitField / Enum op Enum
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 枚举值 | BitField
     */
    template <typename T>
    constexpr BitField<T> operator|(T p_flag, BitField<T> p_bf)
    {
        return p_bf | p_flag;
    }

    /**
     * @brief 枚举值 & BitField
     */
    template <typename T>
    constexpr BitField<T> operator&(T p_flag, BitField<T> p_bf)
    {
        return p_bf & p_flag;
    }

    /**
     * @brief 枚举值 ^ BitField
     */
    template <typename T>
    constexpr BitField<T> operator^(T p_flag, BitField<T> p_bf)
    {
        return p_bf ^ p_flag;
    }

    /**
     * @brief 枚举值 | 枚举值 → BitField
     *
     * 允许直接写 BufferUsageBits::kVertex | BufferUsageBits::kUniform
     * 而无需先构造 BitField 对象。
     */
    template <typename T>
    constexpr BitField<T> operator|(T p_a, T p_b)
    {
        return BitField<T>(p_a) | p_b;
    }

} // namespace arhud
