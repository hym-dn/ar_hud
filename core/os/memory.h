/**
 * @file memory.h
 * @brief 全局内存分配器（Godot 级完整实现）
 *
 * 提供统一的堆分配、精确统计、内存预算、对齐分配、系统内存查询和自定义 Hook。
 *
 * 核心机制 —— p_pad_align 分配头：
 * 启用时，每次分配在返回指针前嵌入 AllocHeader，
 * 记录实际分配大小和元素数量，使 Free/Realloc 能精确追踪统计。
 *
 * Debug vs Release 行为差异：
 *   - 单对象（ARHUD_NEW）：  Debug 带头追踪，Release 不带头（零开销）
 *   - 数组（ARHUD_NEW_ARR）： 始终带头（Release 也需要 element_count_ 析构）
 *   - 金丝雀检测：           Debug 启用头尾校验，Release 不分配尾部金丝雀
 *
 * 禁止直接调用 malloc/free 或裸 new/delete，统一经过本模块。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-04-30
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include "typedefs.h"

namespace arhud::memory
{

    // ═══════════════════════════════════════════════════════════════════════
    // 类型定义
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 内存使用统计快照
     */
    struct MemoryStats
    {
        size_t current_usage_ = 0;     ///< 当前已分配字节数
        size_t peak_usage_ = 0;        ///< 历史峰值字节数（可通过 ResetPeak 重置）
        size_t allocation_count_ = 0;  ///< 当前活跃分配次数
        size_t total_alloc_calls_ = 0; ///< 累计分配调用次数
        size_t total_free_calls_ = 0;  ///< 累计释放调用次数
    };

    /**
     * @brief 可替换的后端分配器 Hook
     *
     * 设置后 Alloc/Free 等操作会转发到 hooks 而非系统堆。
     * 所有函数指针必须同时设置，或同时为 nullptr 恢复系统默认。
     */
    struct AllocatorHooks
    {
        void *(*alloc_fn)(size_t p_size) = nullptr;
        void *(*realloc_fn)(void *p_ptr, size_t p_size) = nullptr;
        void (*free_fn)(void *p_ptr) = nullptr;
    };

    /**
     * @brief 内存踩踏检测头部金丝雀值
     */
    static constexpr uint64_t kHeadCanaryValue = 0xDEADBEEFDEADBEEFull;

    /**
     * @brief 内存踩踏检测尾部金丝雀值
     */
    static constexpr uint64_t kTailCanaryValue = 0xCAFEBABECAFEBABFull;

#if ARHUD_DEBUG
    static constexpr size_t kTailCanarySize = 8; ///< Debug 尾部金丝雀字节数
#else
    static constexpr size_t kTailCanarySize = 0; ///< Release 不分配尾部金丝雀
#endif

    /**
     * @brief p_pad_align 分配头（位于返回指针之前）
     *
     * Debug 完整内存布局：
     * @code
     *   [AllocHeader 24B]           [user_data: alloc_size_ 字节]     [tail 8B]
     *   ┌──────────┬──────────┬─────┬────────────────────────────────┬─────────┐
     *   │head_canay│alloc_size│elem_│ (返回指针 ← 调用方操作区域)     │ tail    │
     *   │          │          │count│                                 │ canary  │
     *   └──────────┴──────────┴─────┴────────────────────────────────┴─────────┘
     * @endcode
     *
     * tail canary 不在结构体中，由 AllocWithHeader 写入
     * `(uint8_t*)returned_ptr + alloc_size_` 处，FreeWithHeader 从该位置校验。
     * Release 下 kTailCanarySize == 0，尾部不分配。
     * head_canary_ Release 也写入但不校验，保证布局一致。
     */
    struct AllocHeader
    {
        uint64_t head_canary_;   ///< 头部金丝雀（Debug 校验防 underflow / UAF）
        uint64_t alloc_size_;    ///< 实际分配字节数（不含头和尾部金丝雀）
        uint64_t element_count_; ///< 0 = 单个对象, >0 = 数组元素数
    };

    static constexpr size_t kAllocHeaderSize = sizeof(AllocHeader);

    // ═══════════════════════════════════════════════════════════════════════
    // 基础分配
    // ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
    /**
     * @brief 编译器内置：获取调用点源文件名（GCC/Clang/MSVC 2019+）
     *
     * 作为函数默认参数时，在**调用点**展开而非定义点，
     * 使 Alloc/Realloc 等公开 API 能自动记录分配来源。
     *
     * @note MSVC 2019 之前不支持此内置，回退到 __FILE__（显示 memory.h 位置）
     */
#ifdef _MSC_VER
#define ARHUD_CALLER_FILE __FILE__
#define ARHUD_CALLER_LINE __LINE__
#else
#define ARHUD_CALLER_FILE __builtin_FILE()
#define ARHUD_CALLER_LINE __builtin_LINE()
#endif
#endif

    /**
     * @brief 分配未初始化内存
     *
     * Debug：嵌入 AllocHeader，Free 时精确递减统计，含金丝雀校验，自动记录分配来源。
     * Release：直接调用系统分配器，零额外开销。
     *
     * @param[in] p_size 分配的字节数
     * @param[in] p_file 分配时的源文件名（Debug 默认自动捕获，无需手动传入）
     * @param[in] p_line 分配时的行号（Debug 默认自动捕获，无需手动传入）
     * @return 分配的内存指针
     * @retval nullptr 分配失败（超出预算或系统 OOM）
     *
     * @pre p_size > 0
     */
#if ARHUD_DEBUG
    void *Alloc(size_t p_size, const char *p_file = ARHUD_CALLER_FILE,
                int p_line = ARHUD_CALLER_LINE);
#else
    void *Alloc(size_t p_size);
#endif

    /**
     * @brief 分配清零内存
     *
     * Debug：嵌入 AllocHeader，Free 时精确递减统计，含金丝雀校验，自动记录分配来源。
     * Release：直接调用系统分配器，零额外开销。
     *
     * @param[in] p_size 分配的字节数
     * @param[in] p_file 分配时的源文件名（Debug 默认自动捕获）
     * @param[in] p_line 分配时的行号（Debug 默认自动捕获）
     * @return 清零后的内存指针
     * @retval nullptr 分配失败
     *
     * @pre p_size > 0
     */
#if ARHUD_DEBUG
    void *AllocZeroed(size_t p_size, const char *p_file = ARHUD_CALLER_FILE,
                      int p_line = ARHUD_CALLER_LINE);
#else
    void *AllocZeroed(size_t p_size);
#endif

    /**
     * @brief 重新分配内存
     *
     * Debug：通过 AllocHeader 精确追踪新旧大小，统计双向更新，自动记录分配来源。
     * Release：直接调用系统 realloc，零额外开销。
     *
     * @param[in] p_ptr  原指针（可为 nullptr，此时等同 Alloc）
     * @param[in] p_size 新大小（可为 0，此时等同 Free）
     * @param[in] p_file 分配时的源文件名（Debug 默认自动捕获）
     * @param[in] p_line 分配时的行号（Debug 默认自动捕获）
     * @return 重新分配后的指针，可能与 p_ptr 不同
     * @retval nullptr 分配失败，原指针 p_ptr 仍有效且未被释放
     */
#if ARHUD_DEBUG
    void *Realloc(void *p_ptr, size_t p_size, const char *p_file = ARHUD_CALLER_FILE,
                  int p_line = ARHUD_CALLER_LINE);
#else
    void *Realloc(void *p_ptr, size_t p_size);
#endif

    /**
     * @brief 释放内存
     *
     * Debug：从 AllocHeader 读取精确大小后递减统计，校验金丝雀，自动移除来源记录。
     * Release：直接调用系统 free，零额外开销。
     *
     * @param[in] p_ptr 要释放的指针（可为 nullptr，此时为空操作）
     *
     * @pre p_ptr 为 nullptr 或由 Alloc / AllocZeroed / Realloc 分配
     */
    void Free(void *p_ptr);

    // ═══════════════════════════════════════════════════════════════════════
    // 静态分配（带 p_pad_align 追踪）
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 分配内存（可选嵌入追踪头）
     *
     * p_pad_align = true：
     *   在返回指针前写入 AllocHeader，记录 alloc_size 和 element_count=0。
     *   FreeStatic 据此精确递减统计。Debug 宏默认启用。
     *
     * p_pad_align = false：
     *   等同 Alloc()，零额外开销。Release 宏默认关闭。
     *
     * @param[in] p_size      分配的字节数
     * @param[in] p_pad_align 是否嵌入追踪头
     * @param[in] p_file      分配时的源文件名（Debug 默认自动捕获）
     * @param[in] p_line      分配时的行号（Debug 默认自动捕获）
     * @return 对齐后的数据指针
     * @retval nullptr 分配失败
     */
#if ARHUD_DEBUG
    void *AllocStatic(size_t p_size, bool p_pad_align,
                      const char *p_file = ARHUD_CALLER_FILE, int p_line = ARHUD_CALLER_LINE);
#else
    void *AllocStatic(size_t p_size, bool p_pad_align);
#endif

    /**
     * @brief 重新分配内存（兼容追踪头）
     *
     * 原指针带追踪头时新分配也带头，自动拷贝原数据和 AllocHeader。
     *
     * @param[in] p_ptr       原指针（可为 nullptr）
     * @param[in] p_size      新大小
     * @param[in] p_pad_align 是否嵌入追踪头
     * @param[in] p_file      分配时的源文件名（Debug 默认自动捕获）
     * @param[in] p_line      分配时的行号（Debug 默认自动捕获）
     * @return 重新分配后的指针
     * @retval nullptr 分配失败，p_ptr 仍有效
     */
#if ARHUD_DEBUG
    void *ReallocStatic(void *p_ptr, size_t p_size, bool p_pad_align,
                        const char *p_file = ARHUD_CALLER_FILE, int p_line = ARHUD_CALLER_LINE);
#else
    void *ReallocStatic(void *p_ptr, size_t p_size, bool p_pad_align);
#endif

    /**
     * @brief 释放内存（兼容追踪头）
     *
     * p_pad_align = true 时从 AllocHeader 读取 alloc_size 后精确递减统计。
     *
     * @param[in] p_ptr       要释放的指针（可为 nullptr）
     * @param[in] p_pad_align 原分配是否带追踪头
     *
     * @pre p_ptr 由 AllocStatic / ReallocStatic 且 p_pad_align 值与分配时一致
     */
    void FreeStatic(void *p_ptr, bool p_pad_align);

    /**
     * @brief 析构前验证分配块的 canary
     *
     * 在 ARHUD_DELETE 中于析构函数调用之前执行，
     * 确保对象内存未被越界写入损坏。
     * 若 canary 已损坏则记录 FATAL 日志。
     *
     * @param[in] p_ptr 要验证的指针
     *
     * @pre p_ptr 由 AllocStatic(..., true) 分配
     */
    void ValidateAllocCanary(void *p_ptr);

    /**
     * @brief 析构后释放内存（跳过 canary 检查）
     *
     * 对象析构函数可能通过 PagedAllocator::Reset 等操作
     * 释放与对象相邻的内存块，导致 C 运行时 free() 修改
     * 相邻块的头部数据（包括 canary 字段）。
     * 因此析构后跳过 canary 检查，直接释放内存。
     *
     * @param[in] p_ptr 要释放的指针
     *
     * @pre p_ptr 由 AllocStatic(..., true) 分配
     * @pre ValidateAllocCanary(p_ptr) 已在析构前调用
     */
    void FreeStaticPostDtor(void *p_ptr);

    // ═══════════════════════════════════════════════════════════════════════
    // 对齐分配
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 分配对齐内存
     *
     * 用于 GPU 缓冲区映射（16/64/256 字节对齐）和 SIMD 数据。
     * Debug 模式自动记录分配来源。
     *
     * @param[in] p_size      分配的字节数
     * @param[in] p_alignment 对齐要求（2 的幂，>= sizeof(void*)）
     * @param[in] p_file      分配时的源文件名（Debug 默认自动捕获）
     * @param[in] p_line      分配时的行号（Debug 默认自动捕获）
     * @return 对齐后的内存指针
     * @retval nullptr 分配失败
     *
     * @pre p_alignment 是 2 的幂且 >= sizeof(void*)
     */
#if ARHUD_DEBUG
    void *AllocAligned(size_t p_size, size_t p_alignment,
                       const char *p_file = ARHUD_CALLER_FILE, int p_line = ARHUD_CALLER_LINE);
#else
    void *AllocAligned(size_t p_size, size_t p_alignment);
#endif

    /**
     * @brief 重新分配对齐内存
     *
     * @param[in] p_ptr        原指针（可为 nullptr）
     * @param[in] p_size       新大小
     * @param[in] p_prev_size  原数据大小（用于拷贝，不知则传 0）
     * @param[in] p_alignment  对齐要求
     * @return 重新分配后的对齐指针
     * @retval nullptr 分配失败，p_ptr 仍有效
     *
     * @note p_prev_size 传 0 时不拷贝旧数据
     */
    void *ReallocAligned(void *p_ptr, size_t p_size, size_t p_prev_size,
                         size_t p_alignment);

    /**
     * @brief 释放对齐内存
     *
     * @param[in] p_ptr 要释放的对齐指针（可为 nullptr）
     *
     * @pre p_ptr 由 AllocAligned / ReallocAligned 分配
     */
    void FreeAligned(void *p_ptr);

    // ═══════════════════════════════════════════════════════════════════════
    // 系统内存信息
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 获取系统可用物理内存
     *
     * @return 可用物理内存字节数，获取失败返回 0
     */
    uint64_t GetMemAvailable();

    /**
     * @brief 获取当前进程堆内存使用量
     *
     * 优先读取 OS 统计（Windows WorkingSet / Linux VmRSS），
     * 兜底返回分配器内部统计（仅堆分配量，不含 mmap）。
     *
     * @return 进程堆内存字节数，获取失败返回 0
     */
    uint64_t GetMemUsage();

    /**
     * @brief 获取当前进程堆内存历史峰值
     *
     * @return 进程堆内存历史峰值字节数
     */
    uint64_t GetMemMaxUsage();

    // ═══════════════════════════════════════════════════════════════════════
    // 内存统计
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 获取当前内存统计快照
     *
     * @return 包含 current_usage / peak_usage / allocation_count 等字段的快照
     *
     * @note 多线程环境下各字段并非原子一致快照，仅供参考
     */
    MemoryStats GetStats();

    /**
     * @brief 重置峰值使用量为当前值
     */
    void ResetPeak();

    // ═══════════════════════════════════════════════════════════════════════
    // Debug 泄漏检测（仅 ARHUD_DEBUG 模式）
    // ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
    /**
     * @brief 输出当前未释放的分配记录（泄漏报告）
     *
     * 遍历追踪表，将所有仍存活的分配按 file:line 输出到 stderr。
     * 典型用法：程序退出前调用，若输出非空则存在内存泄漏。
     *
     * @note 使用 std::fprintf 输出，不依赖引擎 Logger（避免循环依赖）
     */
    void DumpLeaks();
#endif

    // ═══════════════════════════════════════════════════════════════════════
    // 内存预算
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 设置总内存预算上限
     *
     * 超出预算后 Alloc / AllocAligned 等函数将返回 nullptr。
     * 设为 0 表示不限制。
     *
     * @param[in] p_total_bytes 预算上限（字节），0 = 不限制
     */
    void SetBudget(size_t p_total_bytes);

    /**
     * @brief 当前使用量是否超出预算
     *
     * @return true  超出预算
     * @return false 未超出或未设置预算
     */
    bool IsBudgetExceeded();

    /**
     * @brief 获取当前设定的预算上限
     *
     * @return 预算上限（字节），0 表示未设置
     */
    size_t GetBudget();

    // ═══════════════════════════════════════════════════════════════════════
    // 数组长度查询
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 查询带追踪头的数组元素数量
     *
     * 从 AllocHeader::element_count_ 读取，仅对 p_pad_align=true 分配的内存有效。
     * 等价于 Godot 的 memarr_len()。
     *
     * @param[in] p_ptr 数组数据指针（由 AllocStatic/ReallocStatic 带 pad 分配）
     * @return 元素数量，0 表示单个对象或未记录
     *
     * @pre p_ptr != nullptr 且由 p_pad_align=true 的 AllocStatic/ReallocStatic 分配
     */
    size_t GetArrayLength(const void *p_ptr);

    // ═══════════════════════════════════════════════════════════════════════
    // 自定义分配器 Hook
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 替换后端分配器
     *
     * hooks 全为 nullptr 时恢复系统默认后端。
     * 应在初始化时设置，设置后热路径直读 hools_，不额外加锁。
     *
     * @param[in] p_hooks 包含 alloc/realloc/free 函数指针
     */
    void SetAllocatorHooks(const AllocatorHooks &p_hooks);

    /**
     * @brief 获取当前后端分配器
     *
     * @return 当前的 AllocatorHooks，全 nullptr 表示使用系统默认
     */
    AllocatorHooks GetAllocatorHooks();

} // namespace arhud::memory

// ═══════════════════════════════════════════════════════════════════════
// 默认分配器标签 + operator new/delete 重载（全局作用域）
// ═══════════════════════════════════════════════════════════════════════
//
// C++ 标准要求：placement operator new/delete 必须在全局作用域声明，
// 不能放在任何命名空间中（MSVC C2323 / GCC 错误）。
// 参考 Godot 的 DefaultAllocator 设计：
//   - 使用 placement-new 语法 ::new (DefaultAllocator{}) T
//   - 编译器自动传入 sizeof(T)，避免手动写 sizeof
//   - 分配失败返回 nullptr（而非抛 std::bad_alloc），匹配引擎无异常策略
//
// 与裸 placement-new 的区别：
//   裸写法 ::new (AllocStatic(sizeof(T), true)) T
//   如果 AllocStatic 返回 nullptr，则 ::new (nullptr) T 是未定义行为。
//   通过 operator new 重载，可以在分配失败时安全返回 nullptr。

namespace arhud::memory
{

    /**
     * @brief 默认分配器标签
     *
     * 空结构体，仅作为 operator new 重载的标签类型，
     * 通过 placement-new 语法选择对应的重载：
     * @code
     *   ::new (DefaultAllocator{}) T(args...)
     * @endcode
     */
    struct DefaultAllocator
    {
    };

} // namespace arhud::memory

/**
 * @brief operator new 重载：使用引擎分配器分配内存
 *
 * Debug 带追踪头 + 来源位置记录（精确统计 + 泄漏定位）。
 * Release 不带头（零开销）。
 * 分配失败返回 nullptr（不抛异常），由调用方检查。
 *
 * @param[in] p_size      编译器自动传入的 sizeof(T)
 * @param[in] p_allocator 标签参数（DefaultAllocator{}）
 * @return 分配的内存指针
 * @retval nullptr 分配失败
 */
inline void *operator new(size_t p_size, arhud::memory::DefaultAllocator p_allocator)
{
    (void)p_allocator;
#if ARHUD_DEBUG
    return arhud::memory::AllocStatic(p_size, true);
#else
    return arhud::memory::Alloc(p_size);
#endif
}

/**
 * @brief operator delete 配对重载
 *
 * 仅在构造函数抛异常时由编译器调用（匹配对应的 operator new 签名）。
 * 由于引擎禁用异常，此函数理论上不会被调用，
 * 但 C++ 标准要求每个 operator new 重载都有配对的 operator delete，
 * 否则 MSVC 会产生 C4291 警告。
 *
 * @param[in] p_ptr       构造失败前分配的内存
 * @param[in] p_allocator 标签参数（与 operator new 匹配）
 */
inline void operator delete(void *p_ptr, arhud::memory::DefaultAllocator p_allocator)
{
    (void)p_allocator;
#if ARHUD_DEBUG
    arhud::memory::FreeStatic(p_ptr, true);
#else
    arhud::memory::Free(p_ptr);
#endif
}

// ═══════════════════════════════════════════════════════════════════════
// 对象构造/析构宏
// ═══════════════════════════════════════════════════════════════════════

/**
 * @def ARHUD_NEW
 * @brief 分配 + 构造单个对象
 *
 * 通过 operator new(DefaultAllocator{}) 重载分配内存：
 *   - Debug 带追踪头 + 来源位置记录（精确统计 + 泄漏定位）
 *   - Release 不带头（零开销）
 *   - 分配失败返回 nullptr（安全，无 UB）
 *
 * @param m_class 对象类型
 *
 * Usage: @code ARHUD_NEW(ARHudTexture) @endcode
 */
#define ARHUD_NEW(m_class) \
    ::new (arhud::memory::DefaultAllocator{}) m_class

/**
 * @def ARHUD_DELETE
 * @brief 析构 + 释放单个对象
 *
 * Debug 通过 FreeStatic 精确递减统计，Release 直接 Free。
 *
 * @param m_ptr 对象指针
 *
 * Usage: @code ARHUD_DELETE(texture) @endcode
 */
#if ARHUD_DEBUG
#define ARHUD_DELETE(m_ptr)                                                         \
    do                                                                              \
    {                                                                               \
        if ((m_ptr) != nullptr)                                                     \
        {                                                                           \
            using _DT = std::remove_reference_t<decltype (*(m_ptr))>;               \
            arhud::memory::ValidateAllocCanary(m_ptr);                              \
            (m_ptr)->~_DT();                                                        \
            arhud::memory::FreeStaticPostDtor(m_ptr);                               \
        }                                                                           \
    } while (0)
#else
#define ARHUD_DELETE(m_ptr)                                                         \
    do                                                                              \
    {                                                                               \
        if ((m_ptr) != nullptr)                                                     \
        {                                                                           \
            using _DT = std::remove_reference_t<decltype (*(m_ptr))>;               \
            (m_ptr)->~_DT();                                                        \
            arhud::memory::Free(m_ptr);                                             \
        }                                                                           \
    } while (0)
#endif

// ─── 数组辅助模板 ──────────────────────────────────────────────────

namespace arhud
{
    namespace memory
    {
        namespace detail
        {

            /**
             * @brief 分配并默认构造 T 数组
             *
             * p_pad = true 时嵌入 AllocHeader，记录 element_count 供逆序析构。
             * Debug 模式自动通过默认参数捕获分配来源。
             *
             * @tparam T 元素类型
             * @param[in] p_count 元素数量
             * @param[in] p_pad   是否嵌入追踪头
             * @return T* 数组指针
             * @retval nullptr 分配失败
             *
             * @pre p_count > 0
             */
            template <typename T>
            T *NewArrayImpl(size_t p_count, bool p_pad)
            {
                size_t bytes = sizeof(T) * p_count;
                void *block = AllocStatic(bytes, p_pad);
                if (block == nullptr)
                    return nullptr;

                if (p_pad)
                {
                    AllocHeader *h = reinterpret_cast<AllocHeader *>(block) - 1;
                    h->element_count_ = p_count;
                }

                T *arr = static_cast<T *>(block);
                for (size_t i = 0; i < p_count; ++i)
                {
                    ::new (&arr[i]) T();
                }
                return arr;
            }

            /**
             * @brief 析构并释放 T 数组
             *
             * 从 AllocHeader 读取元素数量，非平凡析构时逐个逆序析构。
             * 平凡析构类型跳过循环，直接释放。
             *
             * @tparam T 元素类型
             * @param[in] p_ptr 数组指针（可为 nullptr）
             * @param[in] p_pad 原分配是否带追踪头
             *
             * @pre p_ptr 由 NewArrayImpl<T> 分配且 p_pad 值一致
             */
            template <typename T>
            void DeleteArrayImpl(T *p_ptr, bool p_pad)
            {
                if (p_ptr == nullptr)
                    return;

                uint64_t count = 0;
                if (p_pad)
                {
                    AllocHeader *h = reinterpret_cast<AllocHeader *>(p_ptr) - 1;
                    count = h->element_count_;
                }

                if constexpr (!std::is_trivially_destructible_v<T>)
                {
                    for (uint64_t i = count; i > 0; --i)
                    {
                        p_ptr[i - 1].~T();
                    }
                }

                FreeStatic(p_ptr, p_pad);
            }

        } // namespace detail
    } // namespace memory
} // namespace arhud

/**
 * @def ARHUD_NEW_ARR
 * @brief 分配 + 默认构造 T 数组
 *
 * 始终嵌入追踪头记录元素数量，确保非平凡析构正确调用。
 * Debug 模式自动记录来源位置（通过 AllocStatic 默认参数）。
 *
 * @param m_class 元素类型
 * @param m_count 元素数量
 *
 * Usage: @code ARHUD_NEW_ARR(ARHudVertex, 128) @endcode
 */
#define ARHUD_NEW_ARR(m_class, m_count) \
    arhud::memory::detail::NewArrayImpl<m_class>(m_count, true)

/**
 * @def ARHUD_DELETE_ARR
 * @brief 析构 + 释放 T 数组
 *
 * 从追踪头读取元素数量后逐个逆序析构。
 *
 * @param m_ptr   数组指针
 * @param m_class 元素类型
 *
 * Usage: @code ARHUD_DELETE_ARR(vertices, ARHudVertex) @endcode
 */
#define ARHUD_DELETE_ARR(m_ptr, m_class) \
    arhud::memory::detail::DeleteArrayImpl<m_class>(m_ptr, true)

/**
 * @def ARHUD_NEW_PLACEMENT
 * @brief 就地构造对象
 *
 * @param m_placement 已分配的内存指针
 * @param m_class     对象类型
 *
 * @pre m_placement != nullptr 且指向足够容纳 m_class 的内存
 */
#define ARHUD_NEW_PLACEMENT(m_placement, m_class) \
    ::new (m_placement) m_class

/**
 * @def ARHUD_NEW_ALLOCATOR
 * @brief 使用自定义分配器分配并构造
 *
 * @param m_class    对象类型
 * @param m_alloc_fn 分配函数（void*(size_t)）
 * @param m_free_fn  释放函数（void(void*)）
 *
 * Usage: @code ARHUD_NEW_ALLOCATOR(MyObj, my_alloc, my_free) @endcode
 */
#define ARHUD_NEW_ALLOCATOR(m_class, m_alloc_fn, m_free_fn) \
    ::new (m_alloc_fn(sizeof(m_class))) m_class

/**
 * @def ARHUD_DELETE_ALLOCATOR
 * @brief 使用自定义分配器析构并释放
 *
 * @param m_ptr     对象指针
 * @param m_free_fn 释放函数（void(void*)）
 */
#define ARHUD_DELETE_ALLOCATOR(m_ptr, m_free_fn) \
    do                                           \
    {                                            \
        if ((m_ptr) != nullptr)                  \
        {                                        \
            (m_ptr)->~decltype (*(m_ptr))();     \
            m_free_fn(m_ptr);                    \
        }                                        \
    } while (0)

// ─── C 风格 ────────────────────────────────────────────────────────

/** @brief C 风格分配（等同 Alloc，不调用构造函数，Debug 自动追踪来源） */
#define ARHUD_MALLOC(m_size) arhud::memory::Alloc(m_size)

/** @brief C 风格清零分配 */
#define ARHUD_MALLOC_ZEROED(m_size) arhud::memory::AllocZeroed(m_size)

/** @brief C 风格重分配 */
#define ARHUD_REALLOC(m_ptr, m_size) arhud::memory::Realloc(m_ptr, m_size)

/** @brief C 风格释放 */
#define ARHUD_FREE(m_ptr) arhud::memory::Free(m_ptr)