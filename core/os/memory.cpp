/**
 * @file memory.cpp
 * @brief 全局内存分配器实现（Godot 级）
 *
 * 底层使用系统堆分配（Windows HeapAlloc / POSIX malloc），
 * 提供可替换的 AllocatorHooks 后端。
 *
 * 三条分配路径：
 *   - Alloc/Free：          Debug 嵌入 AllocHeader 精确追踪，Release 零开销
 *   - AllocStatic/FreeStatic：p_pad_align=true 始终带头（Release 需
 * element_count 析构数组）
 *   - AllocAligned/FreeAligned：对齐分配，Debug 用映射表追踪精确大小
 *
 * Debug vs Release 行为差异（四类 Debug-only 功能）：
 *
 *   1. 来源追踪（RecordAlloc/RecordFree + g_alloc_records）：
 *      Debug 记录每次分配的 file:line，Release 编译为零。
 *
 *   2. 聚合统计（TrackAlloc/TrackFree + GlobalState 计数器）：
 *      Debug 更新 current_usage/peak_usage/allocation_count 等，Release
 * 编译为零。 Release 下 GetStats() 返回全零，GetMemMaxUsage() 返回 0，
 *      IsOverBudget() 始终返回 false（预算不执行）。
 *
 *   3. 金丝雀校验（ValidateCanaries/WriteTailCanary + kTailCanarySize）：
 *      Debug 启用头尾校验 + 尾部 8 字节金丝雀，Release 不分配尾部金丝雀。
 *      head_canary 在两种模式下均写入（保持布局一致），但 Release 不校验。
 *
 *   4. 来源位置参数（file/line）：
 *      Debug 下
 * Alloc/AllocZeroed/Realloc/AllocStatic/ReallocStatic/AllocAligned/ReallocAligned
 *      额外接受 const char* p_file / int32 p_line 参数。
 *      Release 下这些参数不存在，函数签名更精简。
 *
 * 条件编译风格统一：
 *   - 所有 Debug-only 代码使用 #if ARHUD_DEBUG / #else / #endif 包裹
 *   - TrackAlloc/TrackFree 在 Release 下为 inline 空函数（非宏），编译器优化为
 * nop
 *   - 不使用 #define 宏替换函数
 *
 * @author yameng.he
 * @version 1.3
 * @date 2026-08-29
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#include "os/memory.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

// 平台特定头文件（仅用于 GetMemAvailable / GetMemUsage，分配器统一使用 malloc）
// 注意：必须在 memory.h（引入 typedefs.h 的平台宏）之后，
//       否则 ARHUD_PLATFORM_WINDOWS 未定义，Windows 误走 POSIX 分支
#if defined(ARHUD_PLATFORM_WINDOWS)
#define NOMINMAX
#include <windows.h>  // Must precede psapi.h (SIZE_T and friends live here).
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <unistd.h>
#endif

#include "io/logger.h"
#include "math_funcs.h"

namespace arhud::memory {
namespace {
// ═══════════════════════════════════════════════════════════════════════
// 全局状态
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 分配器进程级全局状态（单实例 g_state）
 *
 * 统计计数器全部 atomic —— 多线程并发分配/释放时无锁更新；
 * hooks 为三个函数指针，按契约仅在初始化期写入一次
 * （见 SetAllocatorHooks），此后热路径无锁直读；
 * aligned_alloc_sizes（仅 Debug）登记对齐块的真实总量，
 * 供 FreeAligned 配对扣减统计（对齐块无 AllocHeader 可读）。
 */
struct GlobalState {
  // 统计（原子计数器，精确双向追踪）
  std::atomic<size_t> current_usage{0};
  std::atomic<size_t> peak_usage{0};
  std::atomic<size_t> allocation_count{0};
  std::atomic<size_t> total_alloc_calls{0};
  std::atomic<size_t> total_free_calls{0};
  // 预算（0 = 不限制）
  std::atomic<size_t> budget{0};
  // 自定义 Hook（初始化时设置，之后不再加锁，热路径零开销）
  AllocatorHooks hooks{};
#if ARHUD_DEBUG
  std::unordered_map<const void*, size_t> aligned_alloc_sizes;
#endif
};

GlobalState g_state;

// ═══════════════════════════════════════════════════════════════════════
// Debug 分配来源追踪（仅 ARHUD_DEBUG 模式）
// ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG

/**
 * @brief 单次分配的来源记录
 */
struct AllocRecord {
  const char* file;  ///< 分配时的源文件名
  int32 line;        ///< 分配时的行号
  size_t size;       ///< 分配的字节数
};

/**
 * @brief 分配来源追踪表：用户指针 → AllocRecord
 */
std::unordered_map<const void*, AllocRecord> g_alloc_records;

/**
 * @brief 追踪表互斥锁（std::unordered_map 内部用 malloc，无循环依赖）
 */
std::mutex g_alloc_records_mutex;

/**
 * @brief 登记一次分配的来源（仅 Debug）
 *
 * 以用户指针为键写 {file, line, size}；键冲突（地址复用，
 * 如 UAF 后新分配落在同址）按覆盖处理 —— 最新分配胜出。
 *
 * @param[in] p_ptr  用户数据指针（nullptr 忽略）
 * @param[in] p_size 分配字节数
 * @param[in] p_file 来源文件名（__builtin_FILE 在调用点捕获）
 * @param[in] p_line 来源行号
 */
void RecordAlloc(const void* p_ptr, size_t p_size, const char* p_file,
                 int32 p_line) {
  if (p_ptr == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_alloc_records_mutex);
  g_alloc_records[p_ptr] = {p_file, p_line, p_size};
}

/**
 * @brief 注销一次分配的来源记录（仅 Debug）
 *
 * 键不存在（外来指针、或双重释放的第二次）静默忽略 ——
 * 双重释放另有金丝雀与统计兜底报告，此处不重复报警。
 *
 * @param[in] p_ptr 用户数据指针（nullptr 忽略）
 */
void RecordFree(const void* p_ptr) {
  if (p_ptr == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_alloc_records_mutex);
  g_alloc_records.erase(p_ptr);
}

/**
 * @brief 重分配时迁移来源记录（仅 Debug）
 *
 * 持锁原子完成“删旧键 + 建新键”：扩容路径块地址会变，
 * 泄漏报告必须指向新地址才有效。
 *
 * @param[in] p_old_ptr 旧用户指针（nullptr 跳过删除）
 * @param[in] p_new_ptr 新用户指针（nullptr 跳过登记）
 * @param[in] p_size    新分配字节数
 * @param[in] p_file    来源文件名
 * @param[in] p_line    来源行号
 */
void RecordRealloc(const void* p_old_ptr, const void* p_new_ptr, size_t p_size,
                   const char* p_file, int32 p_line) {
  std::lock_guard<std::mutex> lock(g_alloc_records_mutex);
  if (p_old_ptr != nullptr)
    g_alloc_records.erase(p_old_ptr);
  if (p_new_ptr != nullptr)
    g_alloc_records[p_new_ptr] = {p_file, p_line, p_size};
}

#endif

// ═══════════════════════════════════════════════════════════════════════
// 平台堆后端
// ═══════════════════════════════════════════════════════════════════════

// ═══ 系统堆后端 ═══
// 统一走 std::malloc / realloc / free（全平台可用，MSVC 下为 UCRT 分段堆），
// 不用平台特定 API（HeapAlloc / mmap）：保证设置 AllocatorHooks 前后行为一致；
// 清零路径显式 memset 而非 HeapAlloc(HEAP_ZERO_MEMORY) 之类的零分配
// API，消除平台差异。

/**
 * @brief 系统堆分配（未初始化）
 * @param[in] p_size 分配字节数
 * @return 原始内存指针，失败返回 nullptr
 */
void* SysAlloc(size_t p_size) {
  return std::malloc(p_size);
}

/**
 * @brief 系统堆分配（清零）
 * @param[in] p_size 分配字节数
 * @return 清零后的内存指针，失败返回 nullptr
 */
void* SysAllocZeroed(size_t p_size) {
  void* p = std::malloc(p_size);
  if (p)
    std::memset(p, 0, p_size);
  return p;
}

/**
 * @brief 系统堆重分配
 * @param[in] p 原指针（nullptr 时等同分配）
 * @param[in] s 新字节数
 * @return 新指针（块可能已移动），失败返回 nullptr
 */
void* SysRealloc(void* p, size_t s) {
  return std::realloc(p, s);
}

/**
 * @brief 系统堆释放
 * @param[in] p 待释放指针（nullptr 为空操作）
 */
void SysFree(void* p) {
  std::free(p);
}

// ═══════════════════════════════════════════════════════════════════════
// 后端分配器（处理 Hook 转发）— 热路径，强制内联
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 后端分配入口：Hook 已设置则转发，否则落回系统堆
 *
 * 热路径（所有分配的必经点）强制内联；hooks 整体按值拷贝
 * 到栈上再判空，热路径只触碰一次全局状态。
 *
 * @param[in] p_size 分配字节数
 * @return 内存指针，失败返回 nullptr
 */
ARHUD_ALWAYS_INLINE void* BackendAlloc(size_t p_size) {
  AllocatorHooks h = g_state.hooks;
  if (h.alloc_fn != nullptr) {
    return h.alloc_fn(p_size);
  } else {
    return SysAlloc(p_size);
  }
}

/**
 * @brief 后端清零分配入口
 *
 * Hook 接口没有“清零”变体 —— Hook 路径先普通分配再补 memset，
 * 保证自定义后端与系统后端语义一致。
 *
 * @param[in] p_size 分配字节数
 * @return 清零后的内存指针，失败返回 nullptr
 */
ARHUD_ALWAYS_INLINE void* BackendAllocZeroed(size_t p_size) {
  AllocatorHooks h = g_state.hooks;
  if (h.alloc_fn != nullptr) {
    void* ptr = h.alloc_fn(p_size);
    if (ptr != nullptr) {
      std::memset(ptr, 0, p_size);
    }
    return ptr;
  } else {
    return SysAllocZeroed(p_size);
  }
}

/**
 * @brief 后端重分配入口（Hook 转发 / 系统堆兜底）
 *
 * @param[in] p_ptr 原指针（可为 nullptr）
 * @param[in] p_size 新字节数
 * @return 新指针，失败返回 nullptr（原块保持有效）
 */
inline void* BackendRealloc(void* p_ptr, size_t p_size) {
  AllocatorHooks h = g_state.hooks;
  if (h.realloc_fn != nullptr) {
    return h.realloc_fn(p_ptr, p_size);
  } else {
    return SysRealloc(p_ptr, p_size);
  }
}

/**
 * @brief 后端释放入口（Hook 转发 / 系统堆兜底）
 *
 * @param[in] p_ptr 待释放指针（nullptr 直接返回，后端零调用）
 */
ARHUD_ALWAYS_INLINE void BackendFree(void* p_ptr) {
  if (p_ptr == nullptr) {
    return;
  }
  AllocatorHooks h = g_state.hooks;
  if (h.free_fn != nullptr) {
    h.free_fn(p_ptr);
    return;
  } else {
    SysFree(p_ptr);
  }
}

// ═══════════════════════════════════════════════════════════════════════
// 统计追踪
//
// Debug：精确双向追踪（原子计数器 + 来源记录）。
// Release：编译期消除，零开销（inline 空函数，编译器优化为 nop）。
// ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
/**
 * @brief 把一次分配累计进全局统计（仅 Debug）
 *
 * 分配计数与使用量直接原子累加；峰值用 CAS 循环推进 ——
 * 竞争失败方携带读到的最新峰值重试，只有看到更大值的
 * 线程才能抬高 peak_usage，保证峰值单调不减。
 * 全部 relaxed 序：计数器彼此独立，无跨变量同步语义。
 *
 * @param[in] p_size 本次分配总字节数（含头部与金丝雀开销）
 */
ARHUD_ALWAYS_INLINE void TrackAlloc(size_t p_size) {
  g_state.allocation_count.fetch_add(1, std::memory_order_relaxed);
  g_state.total_alloc_calls.fetch_add(1, std::memory_order_relaxed);
  size_t prev =
      g_state.current_usage.fetch_add(p_size, std::memory_order_relaxed);
  size_t new_usage = prev + p_size;
  size_t peak = g_state.peak_usage.load(std::memory_order_relaxed);
  while (new_usage > peak) {
    if (g_state.peak_usage.compare_exchange_weak(peak, new_usage,
                                                 std::memory_order_relaxed)) {
      break;
    }
  }
}

/**
 * @brief 把一次释放从全局统计扣除（仅 Debug）
 *
 * 与 TrackAlloc 严格配对：p_size 必须与分配时计入的总量
 * （含开销）一致，否则 current_usage / allocation_count 漂移。
 *
 * @param[in] p_size 释放块的总字节数
 */
ARHUD_ALWAYS_INLINE void TrackFree(size_t p_size) {
  g_state.allocation_count.fetch_sub(1, std::memory_order_relaxed);
  g_state.total_free_calls.fetch_add(1, std::memory_order_relaxed);
  g_state.current_usage.fetch_sub(p_size, std::memory_order_relaxed);
}
#else
ARHUD_ALWAYS_INLINE void TrackAlloc(size_t) {}
ARHUD_ALWAYS_INLINE void TrackFree(size_t) {}
#endif

/**
 * @brief 预算检查：再分配 p_requested 字节是否超限
 *
 * 预算为 0（未设置）恒通过；Release 编译为恒 false，
 * 原子读与分支整体消除，零开销。
 *
 * @param[in] p_requested 请求字节数
 * @return true 超出预算（调用方应拒绝本次分配）
 */
ARHUD_ALWAYS_INLINE bool IsOverBudget(size_t p_requested) {
#if ARHUD_DEBUG
  size_t b = g_state.budget.load(std::memory_order_relaxed);
  if (b == 0)
    return false;
  size_t cur = g_state.current_usage.load(std::memory_order_relaxed);
  return cur + p_requested > b;
#else
  (void)p_requested;
  return false;
#endif
}

// ═══════════════════════════════════════════════════════════════════════
// 对齐分配辅助
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 指针向上对齐到 p_alignment 的整数倍
 *
 * 位技巧 (addr + align - 1) & ~(align - 1)：先加 align-1
 * 跨过下一个对齐边界，再屏蔽低位回落到边界上。
 *
 * @param[in] p_ptr       原始指针
 * @param[in] p_alignment 对齐值
 * @return 对齐后的指针（>= p_ptr）
 *
 * @pre p_alignment 为 2 的幂
 */
ARHUD_ALWAYS_INLINE void* AlignForward(void* p_ptr, size_t p_alignment) {
  uintptr_t addr = reinterpret_cast<uintptr_t>(p_ptr);
  uintptr_t aligned = (addr + p_alignment - 1) & ~(p_alignment - 1);
  return reinterpret_cast<void*>(aligned);
}

/**
 * @brief 手动对齐分配（前缀存储原始指针法）
 *
 * 在返回给用户的对齐指针前方紧挨着存放一个 void*，
 * 记录 BackendAlloc 返回的原始指针，供 AlignedFreeRaw 释放时使用。
 * 无需依赖平台特有的 aligned_alloc / _aligned_malloc，兼容自定义 AllocatorHooks
 * 后端。
 *
 * @par 内存布局
 *   @code
 *   BackendAlloc 返回的原始内存 (total = p_size + p_alignment + sizeof(void*))
 *
 *   ┌────────────┬──────────┬──────────────────────────────────────┐
 *   │  padding   │  void*   │         用户可用空间 (p_size)          │
 *   │ (0~N 字节)  │  raw     │                                      │
 *   └────────────┴──────────┴──────────────────────────────────────┘
 *   ↑            ↑          ↑
 *   raw          slot       aligned ← 返回给用户的指针
 *
 *   - raw:     BackendAlloc 返回的原始地址
 *   - slot:    aligned 前面紧挨的一个 void* 位置，存放 raw 指针
 *   - aligned: 满足 p_alignment 对齐要求的地址
 *   @endcode
 *
 * @param[in] p_size      用户请求的字节数
 * @param[in] p_alignment 对齐要求（2 的幂，>= sizeof(void*)）
 * @return 对齐后的内存指针
 * @retval nullptr 分配失败
 *
 * @pre p_alignment 为 2 的幂
 */
void* AlignedAllocRaw(size_t p_size, size_t p_alignment) {
  size_t total = p_size + p_alignment + sizeof(void*);
  void* raw = BackendAlloc(total);
  if (raw == nullptr)
    return nullptr;
  void* aligned =
      AlignForward(static_cast<uint8*>(raw) + sizeof(void*), p_alignment);
  void** slot = static_cast<void**>(aligned) - 1;
  *slot = raw;
  return aligned;
}

/**
 * @brief 释放 AlignedAllocRaw 分配的对齐内存
 *
 * 从用户指针回退一个 void* 取出原始指针，再用原始指针调用 BackendFree。
 *
 * @par 释放流程
 *   @code
 *   用户传入 p_ptr (aligned)
 *          │
 *          ▼
 *   ┌────────────┬──────────┬──────────────────────────────────────┐
 *   │  padding   │  void*   │         用户可用空间                  │
 *   │            │  raw ────────────► BackendFree(raw)
 *   └────────────┴──────────┴──────────────────────────────────────┘
 *   ↑            ↑          ↑
 *   raw        slot=p_ptr-1  p_ptr
 *   @endcode
 *
 * @param[in] p_ptr AlignedAllocRaw 返回的对齐指针（可为 nullptr，安全忽略）
 *
 * @pre p_ptr 为 AlignedAllocRaw 返回的指针，或 nullptr
 */
void AlignedFreeRaw(void* p_ptr) {
  if (p_ptr == nullptr)
    return;
  void** slot = static_cast<void**>(p_ptr) - 1;
  void* raw = *slot;
  BackendFree(raw);
}

// ═══════════════════════════════════════════════════════════════════════
// 金丝雀校验辅助
// ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
/**
 * @brief 校验单个分配块的头尾金丝雀（仅 Debug）
 *
 * 头金丝雀（AllocHeader 内）损坏 → 缓冲区下溢或 use-after-free
 * （用户区之前被写入，或头部本身被踩）；
 * 尾金丝雀（用户区末尾 8 字节）损坏 → 缓冲区上溢（越界写）。
 * 损坏输出 FATAL 日志（含指针与越界字节数）但不中止进程，
 * 交由上层决定处置。
 *
 * @param[in] p_h    分配头指针
 * @param[in] p_data 用户数据指针（尾部定位与日志定位用）
 */
inline void ValidateCanaries(const AllocHeader* p_h, const void* p_data) {
  if (p_h->head_canary != kHeadCanaryValue) {
    ARHUD_LOG_FATAL("HEAD_CANARY_CORRUPTED",
                    "[ARHud Memory] HEAD CANARY CORRUPTED at %p "
                    "(buffer underflow or use-after-free)",
                    p_data);
  }
  const uint64* tail = reinterpret_cast<const uint64*>(
      static_cast<const uint8*>(p_data) + p_h->alloc_size);
  if (*tail != kTailCanaryValue) {
    ARHUD_LOG_FATAL("TAIL_CANARY_CORRUPTED",
                    "[ARHud Memory] TAIL CANARY CORRUPTED at %p "
                    "(buffer overflow, %zu bytes written past end)",
                    p_data, p_h->alloc_size);
  }
}

/**
 * @brief 在用户区末尾写入尾金丝雀（仅 Debug）
 *
 * 写入位置 = p_data + p_alloc_size（恰好越界 0 字节处），
 * 用户合法访问范围内不受影响；任何越界写 >= 1 字节即破坏它。
 *
 * @param[in] p_data       用户数据指针
 * @param[in] p_alloc_size 用户区字节数
 */
inline void WriteTailCanary(void* p_data, size_t p_alloc_size) {
  uint64* tail =
      reinterpret_cast<uint64*>(static_cast<uint8*>(p_data) + p_alloc_size);
  *tail = kTailCanaryValue;
}
#endif

// ═══════════════════════════════════════════════════════════════════════
// p_pad_align 头操作
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 取用户指针前方的 AllocHeader
 *
 * AllocHeader* 做 -1 运算 = 前移 sizeof(AllocHeader) 字节，
 * 与 AllocWithHeader 里 h + 1 的正向偏移互为逆运算。
 *
 * @param[in] p_ptr 用户数据指针（须来自带头分配）
 * @return 头部指针（块真实起点）
 */
ARHUD_ALWAYS_INLINE AllocHeader* GetAllocHeader(void* p_ptr) {
  return reinterpret_cast<AllocHeader*>(p_ptr) - 1;
}

/**
 * @brief 带头分配核心路径（构造 [头][数据][尾金丝雀] 布局）
 *
 * 预算与统计都按 total（含头部与金丝雀开销）计 ——
 * Debug 统计反映真实占用而非用户请求量。
 *
 * @param[in] p_size          用户请求字节数
 * @param[in] p_element_count 数组元素数（单对象 = 0，数组 > 0；
 *                            ARHUD_NEW_ARR / GetArrayLength 依赖它）
 * @return 用户数据指针（紧跟头部之后）
 * @retval nullptr 预算超限或后端分配失败
 */
inline void* AllocWithHeader(size_t p_size, uint64 p_element_count) {
  size_t total = p_size + kAllocHeaderSize + kTailCanarySize;
  if (IsOverBudget(total))
    return nullptr;
  void* raw = BackendAlloc(total);
  if (raw == nullptr)
    return nullptr;
  AllocHeader* h = static_cast<AllocHeader*>(raw);
  h->head_canary = kHeadCanaryValue;
  h->alloc_size = p_size;
  h->element_count = p_element_count;
  void* data = static_cast<void*>(h + 1);
#if ARHUD_DEBUG
  WriteTailCanary(data, p_size);
#endif
  TrackAlloc(total);
  return data;
}

/**
 * @brief 带头释放核心路径（AllocWithHeader 的逆操作）
 *
 * 先校验金丝雀（Debug），再按头部记录的 alloc_size 还原
 * total 扣减统计，最后把头部指针（块起点）交还后端。
 *
 * @param[in] p_ptr 用户数据指针（nullptr 安全）
 */
inline void FreeWithHeader(void* p_ptr) {
  if (p_ptr == nullptr)
    return;
  AllocHeader* h = GetAllocHeader(p_ptr);
#if ARHUD_DEBUG
  ValidateCanaries(h, p_ptr);
#endif
  size_t total = h->alloc_size + kAllocHeaderSize + kTailCanarySize;
  TrackFree(total);
  BackendFree(h);
}

/**
 * @brief 带头重分配核心路径
 *
 * 四分支：
 *   1. p_ptr == nullptr → 转 AllocWithHeader（等同分配）
 *   2. p_size == 0      → 转 FreeWithHeader，返回 nullptr
 *   3. 收缩（<= 旧尺寸）→ 原地：只改 alloc_size、尾金丝雀搬到
 *      新末尾、统计减去差值 —— 零后端调用、零数据拷贝
 *   4. 扩容 → 新块 + memcpy 旧数据 + 释放旧块。不走底层
 *      realloc：头部在块首，块移动后旧头部偏移全部失效；
 *      且 Hook 后端不保证 realloc 语义。element_count
 *      原样透传（数组语义不因扩容丢失）
 *
 * @param[in] p_ptr  旧用户指针（可为 nullptr）
 * @param[in] p_size 新字节数（可为 0）
 * @return 新用户指针（收缩时原样返回），失败 nullptr（旧块保持有效）
 */
inline void* ReallocWithHeader(void* p_ptr, size_t p_size) {
  if (p_ptr == nullptr) {
    return AllocWithHeader(p_size, 0);
  }
  if (p_size == 0) {
    FreeWithHeader(p_ptr);
    return nullptr;
  }
  AllocHeader* old_h = GetAllocHeader(p_ptr);
  size_t old_size = old_h->alloc_size;
  size_t old_total = old_size + kAllocHeaderSize + kTailCanarySize;
  // 只是缩小或相等 → 原地
  if (p_size <= old_size) {
    old_h->alloc_size = p_size;
#if ARHUD_DEBUG
    // 缩小后 tail canary 位置变了，写入新的
    WriteTailCanary(p_ptr, p_size);
#endif
    // 缩小后释放超额统计
#if ARHUD_DEBUG
    size_t shrink = old_size - p_size;
    g_state.current_usage.fetch_sub(shrink, std::memory_order_relaxed);
#endif
    return p_ptr;
  }
  // 扩容 → 分配新块、拷贝数据、释放旧块
  size_t new_total = p_size + kAllocHeaderSize + kTailCanarySize;
  if (IsOverBudget(new_total)) {
    return nullptr;
  }
  void* raw = BackendAlloc(new_total);
  if (raw == nullptr)
    return nullptr;
  // 拷贝旧 header 和数据
  AllocHeader* new_h = static_cast<AllocHeader*>(raw);
  new_h->head_canary = kHeadCanaryValue;
  new_h->alloc_size = p_size;
  new_h->element_count = old_h->element_count;
  void* new_data = new_h + 1;
  std::memcpy(new_data, p_ptr, old_size < p_size ? old_size : p_size);
#if ARHUD_DEBUG
  WriteTailCanary(new_data, p_size);
#endif
  // 释放旧块、更新统计
  TrackFree(old_total);
  TrackAlloc(new_total);
  BackendFree(old_h);
  return new_data;
}

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
// 基础分配
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 分配未初始化内存（实现侧；API 契约见 memory.h）
 *
 * @par 路径
 *   Debug：AllocWithHeader（嵌头 + 金丝雀 + 统计）→ RecordAlloc 记来源；
 *   Release：预算检查 → BackendAlloc 直达系统堆，零额外开销。
 *
 * @note p_size == 0 直接返回 nullptr，不视为错误。
 */
#if ARHUD_DEBUG
void* Alloc(size_t p_size, const char* p_file, int32 p_line)
#else
void* Alloc(size_t p_size)
#endif
{
  if (p_size == 0)
    return nullptr;
#if ARHUD_DEBUG
  void* ptr = AllocWithHeader(p_size, 0);
  RecordAlloc(ptr, p_size, p_file, p_line);
  return ptr;
#else
  if (IsOverBudget(p_size))
    return nullptr;
  void* ptr = BackendAlloc(p_size);
  return ptr;
#endif
}

/**
 * @brief 分配清零内存（实现侧；API 契约见 memory.h）
 *
 * @par 路径
 *   Debug：AllocWithHeader 后对用户区 memset（金丝雀在用户区
 *   之外，清零不触碰）；Release：BackendAllocZeroed 一步完成。
 */
#if ARHUD_DEBUG
void* AllocZeroed(size_t p_size, const char* p_file, int32 p_line)
#else
void* AllocZeroed(size_t p_size)
#endif
{
  if (p_size == 0)
    return nullptr;
#if ARHUD_DEBUG
  void* ptr = AllocWithHeader(p_size, 0);
  if (ptr != nullptr) {
    std::memset(ptr, 0, p_size);
  }
  RecordAlloc(ptr, p_size, p_file, p_line);
  return ptr;
#else
  if (IsOverBudget(p_size))
    return nullptr;
  void* ptr = BackendAllocZeroed(p_size);
  return ptr;
#endif
}

/**
 * @brief 重分配内存（实现侧；API 契约见 memory.h）
 *
 * @par 路径
 *   nullptr / 0 先归一到 Alloc / Free 语义；Debug 走
 *   ReallocWithHeader（原地收缩或新块拷贝）+ RecordRealloc
 *   迁移来源记录；Release 直接 BackendRealloc（块可移动）。
 */
#if ARHUD_DEBUG
void* Realloc(void* p_ptr, size_t p_size, const char* p_file, int32 p_line)
#else
void* Realloc(void* p_ptr, size_t p_size)
#endif
{
  if (p_ptr == nullptr)
    return Alloc(p_size
#if ARHUD_DEBUG
                 ,
                 p_file, p_line
#endif
    );
  if (p_size == 0) {
    Free(p_ptr);
    return nullptr;
  }
#if ARHUD_DEBUG
  void* new_ptr = ReallocWithHeader(p_ptr, p_size);
  RecordRealloc(p_ptr, new_ptr, p_size, p_file, p_line);
  return new_ptr;
#else
  void* new_ptr = BackendRealloc(p_ptr, p_size);
  return new_ptr;
#endif
}

/**
 * @brief 释放内存（实现侧；API 契约见 memory.h）
 *
 * @par 路径
 *   Debug：RecordFree 移除来源记录 → FreeWithHeader（校验金丝雀、
 *   按头部总量扣统计）；Release：BackendFree 直达系统堆。
 */
void Free(void* p_ptr) {
  if (p_ptr == nullptr)
    return;
#if ARHUD_DEBUG
  RecordFree(p_ptr);
  FreeWithHeader(p_ptr);
#else
  BackendFree(p_ptr);
#endif
}

// ═══════════════════════════════════════════════════════════════════════
// 静态分配（p_pad_align）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 静态分配（实现侧；API 契约见 memory.h）
 *
 * @par 路径（p_pad_align 分流）
 *   true：AllocWithHeader —— 可被 FreeStatic 精确追踪，
 *         ARHUD_NEW / ARHUD_NEW_ARR / GetArrayLength 依赖此路径；
 *   false：纯转发 Alloc（Release 默认，零开销）。
 */
#if ARHUD_DEBUG
void* AllocStatic(size_t p_size, bool p_pad_align, const char* p_file,
                  int32 p_line)
#else
void* AllocStatic(size_t p_size, bool p_pad_align)
#endif
{
  if (p_pad_align) {
    void* ptr = AllocWithHeader(p_size, 0);
#if ARHUD_DEBUG
    RecordAlloc(ptr, p_size, p_file, p_line);
#endif
    return ptr;
  }
  return Alloc(p_size
#if ARHUD_DEBUG
               ,
               p_file, p_line
#endif
  );
}

/**
 * @brief 静态重分配（实现侧；API 契约见 memory.h）
 *
 * @par 路径（按原分配的 p_pad_align 分流）
 *   true：ReallocWithHeader + RecordRealloc（保持头部与来源迁移）；
 *   false：纯转发 Realloc。
 *
 * @note p_pad_align 必须与原分配时一致，否则释放路径错配。
 */
#if ARHUD_DEBUG
void* ReallocStatic(void* p_ptr, size_t p_size, bool p_pad_align,
                    const char* p_file, int32 p_line)
#else
void* ReallocStatic(void* p_ptr, size_t p_size, bool p_pad_align)
#endif
{
  if (p_pad_align) {
    void* ptr = ReallocWithHeader(p_ptr, p_size);
#if ARHUD_DEBUG
    RecordRealloc(p_ptr, ptr, p_size, p_file, p_line);
#endif
    return ptr;
  }
  return Realloc(p_ptr, p_size
#if ARHUD_DEBUG
                 ,
                 p_file, p_line
#endif
  );
}

/**
 * @brief 静态释放（实现侧；API 契约见 memory.h）
 *
 * @par 路径（p_pad_align 分流，须与分配时一致）
 *   true：RecordFree + FreeWithHeader（读头精确扣统计）；
 *   false：纯转发 Free。
 */
void FreeStatic(void* p_ptr, bool p_pad_align) {
  if (p_pad_align) {
#if ARHUD_DEBUG
    RecordFree(p_ptr);
#endif
    FreeWithHeader(p_ptr);
    return;
  }
  Free(p_ptr);
}

/**
 * @brief 析构后释放（跳过金丝雀校验；背景与用途见 memory.h）
 *
 * 与 FreeWithHeader 的唯一差异：不调 ValidateCanaries ——
 * 析构函数可能释放相邻块（如 PagedAllocator::Reset），
 * 后端 free() 回写邻块元数据会连带破坏本块头金丝雀，
 * 属运行期合法行为而非踩踏。统计与来源记录照常清理。
 * 配对前置：ValidateAllocCanary 已在析构前调用（ARHUD_DELETE 宏）。
 */
void FreeStaticPostDtor(void* p_ptr) {
  if (p_ptr == nullptr) {
    return;
  }
#if ARHUD_DEBUG
  RecordFree(p_ptr);
#endif
  AllocHeader* h = GetAllocHeader(p_ptr);
  size_t total = h->alloc_size + kAllocHeaderSize + kTailCanarySize;
  TrackFree(total);
  BackendFree(h);
}

/**
 * @brief 析构前校验头金丝雀（ARHUD_DELETE 宏的第一步）
 *
 * 只查头部：此刻邻块尚未被本次析构扰动，头部损坏即确凿的
 * 下溢 / UAF。损坏输出 FATAL 并附带金丝雀现值，便于比对
 * 被覆写成了什么数据（常能直接认出越界写的来源类型）。
 */
void ValidateAllocCanary(void* p_ptr) {
  if (p_ptr == nullptr) {
    return;
  }
  AllocHeader* h = GetAllocHeader(p_ptr);
  if (h->head_canary != kHeadCanaryValue) {
    ARHUD_LOG_FATAL(
        "HEAD_CANARY_CORRUPTED_BEFORE_DTOR",
        "[ARHud Memory] HEAD CANARY CORRUPTED BEFORE DESTRUCTOR at %p "
        "(buffer underflow or use-after-free, canary=0x%llX)",
        p_ptr, (unsigned long long)h->head_canary);
  }
}

// ═══════════════════════════════════════════════════════════════════════
// 对齐分配
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 对齐分配（实现侧；API 契约见 memory.h）
 *
 * @par 实现要点
 *   - 对齐下限提升到 sizeof(void*)：前缀存指针法要求 slot
 *     至少能放下一个指针（见 AlignedAllocRaw 布局图）
 *   - 预算与统计按 estimate（用户尺寸 + 对齐前缀 + 填充余量）计
 *   - Debug 额外登记 aligned_alloc_sizes[ptr] = estimate，
 *     FreeAligned 据此配对扣减（对齐块无 AllocHeader 可读）
 */
#if ARHUD_DEBUG
void* AllocAligned(size_t p_size, size_t p_alignment, const char* p_file,
                   int32 p_line)
#else
void* AllocAligned(size_t p_size, size_t p_alignment)
#endif
{
  if (p_size == 0)
    return nullptr;
  if (p_alignment < sizeof(void*)) {
    p_alignment = sizeof(void*);
  }
  size_t estimate = p_size + p_alignment + sizeof(void*);
  if (IsOverBudget(estimate))
    return nullptr;
  void* ptr = AlignedAllocRaw(p_size, p_alignment);
  if (ptr != nullptr) {
    TrackAlloc(estimate);
#if ARHUD_DEBUG
    g_state.aligned_alloc_sizes[ptr] = estimate;
    RecordAlloc(ptr, estimate, p_file, p_line);
#endif
  }
  return ptr;
}

/**
 * @brief 对齐重分配（实现侧；API 契约见 memory.h）
 *
 * 前缀存指针法的布局固定，无法原地收缩 —— 统一走
 * “新块 + memcpy(p_prev_size) + FreeAligned 旧块”。
 * p_prev_size 传 0 表示不拷贝旧数据（调用方自管）。
 */
#if ARHUD_DEBUG
void* ReallocAligned(void* p_ptr, size_t p_size, size_t p_prev_size,
                     size_t p_alignment, const char* p_file, int32 p_line)
#else
void* ReallocAligned(void* p_ptr, size_t p_size, size_t p_prev_size,
                     size_t p_alignment)
#endif
{
  if (p_ptr == nullptr) {
    return AllocAligned(p_size, p_alignment
#if ARHUD_DEBUG
                        ,
                        p_file, p_line
#endif
    );
  }
  if (p_size == 0) {
    FreeAligned(p_ptr);
    return nullptr;
  }

  size_t new_total = p_size + p_alignment + sizeof(void*);
  if (IsOverBudget(new_total))
    return nullptr;

  void* new_ptr = AlignedAllocRaw(p_size, p_alignment);
  if (new_ptr == nullptr)
    return nullptr;

  size_t copy_size = (p_prev_size > 0) ? p_prev_size : 0;
  if (copy_size > p_size)
    copy_size = p_size;
  if (copy_size > 0) {
    std::memcpy(new_ptr, p_ptr, copy_size);
  }

  FreeAligned(p_ptr);
  TrackAlloc(new_total);
#if ARHUD_DEBUG
  g_state.aligned_alloc_sizes[new_ptr] = new_total;
  RecordAlloc(new_ptr, new_total, p_file, p_line);
#endif

  return new_ptr;
}

/**
 * @brief 对齐释放（实现侧；API 契约见 memory.h）
 *
 * @par 路径
 *   Debug：RecordFree + 从 aligned_alloc_sizes 取登记的
 *   estimate 扣统计（无 AllocHeader，靠登记表配对）；
 *   随后 AlignedFreeRaw 经前缀指针找回块起点交还后端。
 */
void FreeAligned(void* p_ptr) {
  if (p_ptr == nullptr)
    return;

#if ARHUD_DEBUG
  RecordFree(p_ptr);
  auto it = g_state.aligned_alloc_sizes.find(p_ptr);
  if (it != g_state.aligned_alloc_sizes.end()) {
    TrackFree(it->second);
    g_state.aligned_alloc_sizes.erase(it);
  }
#endif
  AlignedFreeRaw(p_ptr);
}

// ═══════════════════════════════════════════════════════════════════════
// 系统内存信息
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 系统可用物理内存（实现侧；API 契约见 memory.h）
 *
 * @par 平台路径
 *   Windows:       GlobalMemoryStatusEx → ullAvailPhys
 *   Linux/Android: /proc/meminfo → MemAvailable
 *   QNX:           sysconf(_SC_AVPHYS_PAGES) × _SC_PAGESIZE
 *
 * @return 可用物理内存字节数；平台查询失败统一返回 0
 */
uint64 GetMemAvailable() {
#if defined(ARHUD_PLATFORM_WINDOWS)
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status)) {
    return status.ullAvailPhys;
  }
#elif defined(ARHUD_PLATFORM_LINUX) || defined(ARHUD_PLATFORM_ANDROID)
  // /proc/meminfo 的 MemAvailable
  FILE* fp = std::fopen("/proc/meminfo", "r");
  if (fp != nullptr) {
    char line[128];
    while (std::fgets(line, sizeof(line), fp)) {
      if (std::strncmp(line, "MemAvailable:", 13) == 0) {
        uint64 kb = 0;
        std::sscanf(line + 13, "%llu", &kb);
        std::fclose(fp);
        return kb * 1024;
      }
    }
    std::fclose(fp);
  }
#elif defined(ARHUD_PLATFORM_QNX)
  // QNX: sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE)
  long pages = sysconf(_SC_AVPHYS_PAGES);
  long psize = sysconf(_SC_PAGESIZE);
  if (pages > 0 && psize > 0) {
    return static_cast<uint64>(pages) * static_cast<uint64>(psize);
  }
#endif
  return 0;
}

/**
 * @brief 进程物理内存占用（实现侧；API 契约见 memory.h）
 *
 * @par 平台路径
 *   Windows:       GetProcessMemoryInfo → WorkingSetSize
 *   Linux/Android: /proc/self/status → VmRSS
 *   QNX:           /proc/self/statm 第二字段 × 页大小
 *
 * @par 兜底
 *   平台路径全部失败时退回分配器内部 current_usage
 *   （Release 下统计被编译消除，恒 0）—— 仅覆盖引擎分配量，
 *   不含 mmap / 静态数据段等系统映射。
 *
 * @return 进程占用字节数；无可用数据源时返回内部统计
 */
uint64 GetMemUsage() {
#if defined(ARHUD_PLATFORM_WINDOWS)
  PROCESS_MEMORY_COUNTERS pmc;
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return pmc.WorkingSetSize;
  }
#elif defined(ARHUD_PLATFORM_LINUX) || defined(ARHUD_PLATFORM_ANDROID)
  // /proc/self/status 的 VmRSS
  FILE* fp = std::fopen("/proc/self/status", "r");
  if (fp != nullptr) {
    char line[128];
    while (std::fgets(line, sizeof(line), fp)) {
      if (std::strncmp(line, "VmRSS:", 6) == 0) {
        uint64 kb = 0;
        std::sscanf(line + 6, "%llu", &kb);
        std::fclose(fp);
        return kb * 1024;
      }
    }
    std::fclose(fp);
  }
#elif defined(ARHUD_PLATFORM_QNX)
  // QNX: /proc/self/statm-like or procfs
  long psize = sysconf(_SC_PAGESIZE);
  FILE* fp = std::fopen("/proc/self/statm", "r");
  if (fp != nullptr) {
    long rss = 0;
    std::fscanf(fp, "%*s %ld", &rss);
    std::fclose(fp);
    return static_cast<uint64>(rss) * static_cast<uint64>(psize);
  }
#endif
  // 兜底：返回分配器内部统计（仅堆分配量，不含 mmap 等）
  return g_state.current_usage.load(std::memory_order_acquire);
}

/**
 * @brief 进程内存历史峰值（实现侧）
 *
 * 直接读 peak_usage；Release 下 TrackAlloc 编译消除，
 * 峰值恒 0（与 GetStats 的 Release 行为一致，见 memory.h）。
 *
 * @return 峰值字节数；Release 恒 0
 */
uint64 GetMemMaxUsage() {
  return g_state.peak_usage.load(std::memory_order_acquire);
}

// ═══════════════════════════════════════════════════════════════════════
// 内存统计
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 统计快照（实现侧；字段语义见 memory.h）
 *
 * 各字段独立 acquire 读取 —— 多线程并发分配/释放时快照内
 * 字段并非同一时刻的一致值（可能互相穿插），仅作监控参考。
 *
 * @return 当前统计快照（Release 下全零）
 */
MemoryStats GetStats() {
  MemoryStats s;
  s.current_usage = g_state.current_usage.load(std::memory_order_acquire);
  s.peak_usage = g_state.peak_usage.load(std::memory_order_acquire);
  s.allocation_count = g_state.allocation_count.load(std::memory_order_acquire);
  s.total_alloc_calls =
      g_state.total_alloc_calls.load(std::memory_order_acquire);
  s.total_free_calls = g_state.total_free_calls.load(std::memory_order_acquire);
  return s;
}

/**
 * @brief 峰值重置为当前值（实现侧）
 *
 * 典型用途：分段观察峰值（如先测启动期峰值，重置后再测稳态）。
 * relaxed 序足够 —— 峰值本身是启发式统计量。
 */
void ResetPeak() {
  size_t cur = g_state.current_usage.load(std::memory_order_relaxed);
  g_state.peak_usage.store(cur, std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════════════════════
// 预算
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 设置预算上限（实现侧；API 契约见 memory.h）
 *
 * 仅 Debug 生效：IsOverBudget 据此拦截后续分配；
 * Release 下预算检查整体编译消除，该值不被消费。
 *
 * @param[in] p_total_bytes 上限字节数（0 = 解除限制）
 */
void SetBudget(size_t p_total_bytes) {
  g_state.budget.store(p_total_bytes, std::memory_order_release);
}

/**
 * @brief 查询当前用量是否已超预算（实现侧）
 *
 * 与 IsOverBudget（内部函数）的区别：本函数检查“已发生的
 * 占用”，后者检查“再分配 p_requested 是否会超” —— 后者
 * 用于分配前拦截，本函数用于运行期巡检。
 *
 * @return true 已超预算；未设置预算或 Release 恒 false
 */
bool IsBudgetExceeded() {
  size_t b = g_state.budget.load(std::memory_order_acquire);
  if (b == 0)
    return false;
  size_t cur = g_state.current_usage.load(std::memory_order_acquire);
  return cur > b;
}

/**
 * @brief 读取当前预算上限（0 = 未设置；Release 恒 0）
 */
size_t GetBudget() {
  return g_state.budget.load(std::memory_order_acquire);
}

// ═══════════════════════════════════════════════════════════════════════
// 数组长度查询
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 查询带头分配的数组元素数（实现侧；API 契约见 memory.h）
 *
 * 直接读 AllocHeader::element_count（-1 偏移反查头部）。
 * 调用方须保证 p_ptr 来自 p_pad_align=true 的分配，
 * 否则头部无意义（返回垃圾值，但不会崩溃）。
 *
 * @return 元素数量；nullptr 或单对象分配返回 0
 */
size_t GetArrayLength(const void* p_ptr) {
  if (p_ptr == nullptr)
    return 0;
  const AllocHeader* h = reinterpret_cast<const AllocHeader*>(p_ptr) - 1;
  return static_cast<size_t>(h->element_count);
}

// ═══════════════════════════════════════════════════════════════════════
// Debug 泄漏检测（仅 ARHUD_DEBUG 模式）
// ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
/**
 * @brief 泄漏报告（实现侧；用法见 memory.h）
 *
 * 持锁遍历整张来源表，逐条输出“指针 / 字节数 / file:line”，
 * 末尾汇总泄漏总量。程序退出前（ShutdownLogger 之前）调用，
 * 输出 "No leaks detected" 即无泄漏。
 *
 * @note 报告基于 Debug 来源表；Release 下本函数不存在。
 */
void DumpLeaks() {
  std::lock_guard<std::mutex> lock(g_alloc_records_mutex);

  if (g_alloc_records.empty()) {
    ARHUD_LOG_INFO("[Memory] No leaks detected.");
    return;
  }

  ARHUD_LOG_WARN("[Memory] === LEAK REPORT ===");
  ARHUD_LOG_WARN("[Memory] %zu allocation(s) still alive:",
                 g_alloc_records.size());

  size_t total_leaked = 0;
  for (const auto& [ptr, record] : g_alloc_records) {
    ARHUD_LOG_WARN("[Memory]   %p  %zu bytes  at %s:%d", ptr, record.size,
                   record.file, record.line);
    total_leaked += record.size;
  }

  ARHUD_LOG_WARN("[Memory] Total leaked: %zu bytes", total_leaked);
  ARHUD_LOG_WARN("[Memory] === END LEAK REPORT ===");
}
#endif

// ═══════════════════════════════════════════════════════════════════════
// 自定义分配器 Hook
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 替换后端分配器（实现侧；API 契约见 memory.h）
 *
 * 直接覆写 g_state.hooks（非原子）—— 按契约仅在初始化期、
 * 单线程环境下调用一次；传全 nullptr 结构恢复系统默认。
 * 设置后热路径直读，无锁。
 *
 * @param[in] p_hooks 三个函数指针须同时有效或同时为 nullptr
 */
void SetAllocatorHooks(const AllocatorHooks& p_hooks) {
  g_state.hooks = p_hooks;
}

/** @brief 读取当前后端分配器（全 nullptr 表示系统默认） */
AllocatorHooks GetAllocatorHooks() {
  return g_state.hooks;
}

}  // namespace arhud::memory
