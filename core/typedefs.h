/**
 * @file typedefs.h
 * @brief 基础类型别名、编译器宏、平台检测、通用宏
 *
 * 参考 Godot core/typedefs.h 设计。
 * 所有源文件应优先包含此头文件以获取统一的基础类型和宏定义。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-04-30
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <cstdint>
#include <cstddef>

// ═══════════════════════════════════════════════════════════════════════
// 平台检测
// ═══════════════════════════════════════════════════════════════════════

/**
 * @def ARHUD_PLATFORM_WINDOWS
 * @brief Windows 平台标记
 */

/**
 * @def ARHUD_PLATFORM_ANDROID
 * @brief Android 平台标记
 */

/**
 * @def ARHUD_PLATFORM_QNX
 * @brief QNX 平台标记
 */

/**
 * @def ARHUD_PLATFORM_LINUX
 * @brief Linux 平台标记
 */

#if defined(_WIN32)
#define ARHUD_PLATFORM_WINDOWS 1
#elif defined(__ANDROID__)
#define ARHUD_PLATFORM_ANDROID 1
#elif defined(__QNX__)
#define ARHUD_PLATFORM_QNX 1
#elif defined(__linux__)
#define ARHUD_PLATFORM_LINUX 1
#endif

/**
 * @def ARHUD_ARCH_X86_64
 * @brief x86-64 架构
 */

/**
 * @def ARHUD_ARCH_ARM64
 * @brief ARM64 (AArch64) 架构
 */

#if defined(__x86_64__) || defined(_M_X64)
#define ARHUD_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ARHUD_ARCH_ARM64 1
#endif

// ═══════════════════════════════════════════════════════════════════════
// 编译器检测与宏
// ═══════════════════════════════════════════════════════════════════════

/**
 * @def ARHUD_COMPILER_CLANG
 * @brief Clang 编译器
 */

/**
 * @def ARHUD_COMPILER_GCC
 * @brief GCC 编译器
 */

/**
 * @def ARHUD_COMPILER_MSVC
 * @brief MSVC 编译器
 */

#if defined(__clang__)
#define ARHUD_COMPILER_CLANG 1
#elif defined(__GNUC__) || defined(__GNUG__)
#define ARHUD_COMPILER_GCC 1
#elif defined(_MSC_VER)
#define ARHUD_COMPILER_MSVC 1
#endif

/**
 * @def ARHUD_ALWAYS_INLINE
 * @brief 强制内联修饰符
 *
 * Clang/GCC: `inline __attribute__((always_inline))`
 * MSVC:      `__forceinline`
 */

/**
 * @def ARHUD_NEVER_INLINE
 * @brief 禁止内联修饰符
 */

/**
 * @def ARHUD_LIKELY(x)
 * @brief 分支预测：条件大概率成立
 *
 * @param x 条件表达式
 */

/**
 * @def ARHUD_UNLIKELY(x)
 * @brief 分支预测：条件大概率不成立
 *
 * @param x 条件表达式
 */

#if defined(ARHUD_COMPILER_CLANG) || defined(ARHUD_COMPILER_GCC)
#define ARHUD_ALWAYS_INLINE inline __attribute__((always_inline))
#define ARHUD_NEVER_INLINE __attribute__((noinline))
#define ARHUD_LIKELY(x) __builtin_expect(!!(x), 1)
#define ARHUD_UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(ARHUD_COMPILER_MSVC)
#define ARHUD_ALWAYS_INLINE __forceinline
#define ARHUD_NEVER_INLINE __declspec(noinline)
#define ARHUD_LIKELY(x) (x)
#define ARHUD_UNLIKELY(x) (x)
#else
#define ARHUD_ALWAYS_INLINE inline
#define ARHUD_NEVER_INLINE
#define ARHUD_LIKELY(x) (x)
#define ARHUD_UNLIKELY(x) (x)
#endif

/**
 * @def ARHUD_DEBUG
 * @brief 调试构建标记
 *
 * 由 NDEBUG 推导：ARHUD_DEBUG = 1 表示 Debug 构建。
 */

#ifndef ARHUD_DEBUG
#if defined(NDEBUG)
#define ARHUD_DEBUG 0
#else
#define ARHUD_DEBUG 1
#endif
#endif

// ═══════════════════════════════════════════════════════════════════════
// 基础类型别名
// ═══════════════════════════════════════════════════════════════════════

using uint8 = uint8_t;   ///< 8-bit  无符号整数
using uint16 = uint16_t; ///< 16-bit 无符号整数
using uint32 = uint32_t; ///< 32-bit 无符号整数
using uint64 = uint64_t; ///< 64-bit 无符号整数

using int8 = int8_t;   ///< 8-bit  有符号整数
using int16 = int16_t; ///< 16-bit 有符号整数
using int32 = int32_t; ///< 32-bit 有符号整数
using int64 = int64_t; ///< 64-bit 有符号整数

using float32 = float;  ///< 32-bit 单精度浮点数
using float64 = double; ///< 64-bit 双精度浮点数

// ═══════════════════════════════════════════════════════════════════════
// 错误码
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 通用错误码
 *
 * 参考 Godot core/error/error_list.h 设计，仅保留引擎所需子集。
 * 所有返回 Error 的函数在成功时返回 Error::kOK。
 */
enum class Error : int32
{
    kOK = 0,              ///< 成功
    kFailed,              ///< 通用失败
    kUnavailable,         ///< 资源/功能不可用
    kUnconfigured,        ///< 未配置
    kUnauthorized,        ///< 未授权
    kParameterRangeError, ///< 参数越界
    kOutOfMemory,         ///< 内存不足
    kBusy,                ///< 资源忙
    kLocked,              ///< 资源已锁定
    kTimeout,             ///< 超时
    kCantConnect,         ///< 无法连接
    kAlreadyExists,       ///< 已存在
    kDoesNotExist,        ///< 不存在
    kInvalidParameter,    ///< 无效参数
};

// ═══════════════════════════════════════════════════════════════════════
// 通用宏
// ═══════════════════════════════════════════════════════════════════════

/**
 * @def ARHUD_DISABLE_COPY
 * @brief 禁用拷贝构造和拷贝赋值
 *
 * @param m_class 类名
 */
#define ARHUD_DISABLE_COPY(m_class)    \
    m_class(const m_class &) = delete; \
    m_class &operator=(const m_class &) = delete

/**
 * @def ARHUD_DISABLE_MOVE
 * @brief 禁用移动构造和移动赋值
 *
 * @param m_class 类名
 */
#define ARHUD_DISABLE_MOVE(m_class) \
    m_class(m_class &&) = delete;   \
    m_class &operator=(m_class &&) = delete

/**
 * @def ARHUD_DISABLE_COPY_MOVE
 * @brief 同时禁用拷贝和移动
 *
 * @param m_class 类名
 */
#define ARHUD_DISABLE_COPY_MOVE(m_class) \
    ARHUD_DISABLE_COPY(m_class);         \
    ARHUD_DISABLE_MOVE(m_class)

/**
 * @def ARHUD_ASSERT
 * @brief 调试断言
 *
 * Debug 构建条件不满足时触发 crash（写空指针），Release 为空操作。
 *
 * @param cond 条件表达式
 */
/**
 * @def ARHUD_ASSERT_MSG
 * @brief 调试断言（带消息）
 *
 * Debug 构建时打印消息并 abort，Release 为空操作。
 *
 * @param cond 条件表达式
 * @param msg  断言消息字符串
 */

namespace arhud
{

    /**
     * @brief 断言失败处理函数
     *
     * 由 ARHUD_ASSERT 宏调用，通过日志系统输出断言信息后 abort。
     * 声明在 typedefs.h 中（避免 logger.h 循环依赖），实现在 logger.cpp 中。
     *
     * @param[in] p_cond 断言条件字符串
     * @param[in] p_msg  断言消息
     */
    void AssertFailed(const char *p_cond, const char *p_msg);

} // namespace arhud

#if ARHUD_DEBUG
#define ARHUD_ASSERT_IMPL(cond, ...)          \
    do                              \
    {                               \
        if (!(cond))                \
        {                           \
            *(volatile int *)0 = 0; \
        }                           \
    } while (0)

#define ARHUD_ASSERT_MSG_IMPL(cond, msg)                                  \
    do                                                                \
    {                                                                 \
        if (!(cond))                                                  \
        {                                                             \
            ::arhud::AssertFailed(#cond, msg);                        \
        }                                                             \
    } while (0)

#define ARHUD_ASSERT_GET_MACRO(_1, _2, NAME, ...) NAME
#define ARHUD_ASSERT(...) ARHUD_ASSERT_GET_MACRO(__VA_ARGS__, ARHUD_ASSERT_MSG_IMPL, ARHUD_ASSERT_IMPL)(__VA_ARGS__)
#else
#define ARHUD_ASSERT(...) ((void)0)
#endif

/**
 * @def ARHUD_ARRAY_SIZE
 * @brief 获取静态数组元素数量
 *
 * @param arr 静态数组
 * @return 元素数量 (size_t)
 */
#define ARHUD_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/**
 * @def ARHUD_ALIGN_AS
 * @brief 按指定类型对齐
 *
 * @param m_type 对齐基准类型
 */
#define ARHUD_ALIGN_AS(m_type) alignas(m_type)

/**
 * @def ARHUD_CACHE_LINE_SIZE
 * @brief 缓存行大小（64 字节）
 */
#define ARHUD_CACHE_LINE_SIZE 64

/**
 * @def ARHUD_TOSTR
 * @brief 将宏展开为字符串
 *
 * Usage: @code const char *v = ARHUD_TOSTR(ARHUD_PLATFORM_WINDOWS); @endcode
 *
 * @param m_x 宏名称
 */
#define ARHUD_TOSTR_IMPL(m_x) #m_x
#define ARHUD_TOSTR(m_x) ARHUD_TOSTR_IMPL(m_x)

// ═══════════════════════════════════════════════════════════════════════
// 入口函数宏
// ═══════════════════════════════════════════════════════════════════════

/**
 * @def ARHUD_MAIN
 * @brief 跨平台入口函数宏
 *
 * Windows 下使用 WinMain（WINDOWS 子系统入口），
 * 其他平台使用标准 main。
 */
#ifdef ARHUD_PLATFORM_WINDOWS
#define ARHUD_MAIN() \
    int __stdcall WinMain(void *, void *, char *, int)
#else
#define ARHUD_MAIN() \
    int main(int, char **)
#endif