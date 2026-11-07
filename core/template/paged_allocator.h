/**
 * @file paged_allocator.h
 * @brief 分页内存分配器，用于固定大小小对象的高效分配/释放
 *
 * @par 定位与选型
 *   解决的问题：高频创建/销毁**同一种类型**的小对象时，直接走
 *   全局分配器（malloc 路径 + Debug 追踪头 + 统计原子操作）的
 *   开销不可接受。本分配器按页批发内存、槽位 O(1) 弹栈/压栈，
 *   热路径只有一次指针解引用和一次加减法。
 *
 *   典型使用者（渲染热路径）：
 *   - RenderingDevice 的资源簿记结构（Buffer/Texture/Pipeline 等）
 *   - RenderBuffers / 各 Storage 的 per-resource 数据
 *
 * @par 三层内存布局（完整图示见 PagedAllocator 类文档"内存布局"段）
 *   @code
 *   page_pool_      : T**   — 页指针表（每项指向一页对象槽位数组）
 *   available_pool_ : T***  — 可用槽位栈（二维：按页分桶的全局 LIFO 栈）
 *   每页本体        : T[]   — page_size_ 个连续 T 槽位（AllocAligned，按
 * alignof(T) 对齐）
 *   @endcode
 *
 * @par 与全局分配器的关系
 *   页内存来自 memory::AllocAligned / 元数据来自 memory::Alloc /
 *   扩容用 memory::Realloc —— 全部经全局分配器（Debug 统计覆盖），
 *   但对象级 Alloc/Free 不再触碰全局路径。
 *
 * @par 车规模式
 *   Preallocate() 启动期预分配全部页 → 运行期零 malloc，
 *   满足 QNX"运行时禁止动态分配"的安全约束。
 *
 * @see memory.h  全局分配器（页内存的来源）
 *
 * @author yameng.he
 * @version 1.2
 * @date 2026-08-29
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#ifndef ARHUD_CORE_TEMPLATE_PAGED_ALLOCATOR_H_
#define ARHUD_CORE_TEMPLATE_PAGED_ALLOCATOR_H_

#include <type_traits>

#include "os/memory.h"
#include "os/sync.h"
#include "typedefs.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// 辅助函数
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 计算最近的 2 的幂（向上取整）
 *
 * @par 算法（位填充法）
 *   先减一，然后连续右移+或：5 轮后所有低位被 1 填满
 *   （0b0100 → 0b0111），再 +1 进位到下一个 2 的幂。
 *   5 轮移位覆盖 uint32 全部 32 位（1+2+4+8+16）。
 *
 *   @code
 *   输入 5  : 0b101 → 减一 0b100 → 填充 0b111 → +1 = 8
 *   输入 256: 已是 2 的幂 → 减一 0b11111111 → 填充不变 → +1 = 256
 *   @endcode
 *
 * @param[in] p_number 输入正整数（0 视为 1）
 *
 * @return >= p_number 的最小 2 的幂
 */
ARHUD_ALWAYS_INLINE uint32 NearestPow2(uint32 p_number) {
  if (p_number == 0) {
    return 1;
  }
  --p_number;
  p_number |= p_number >> 1;
  p_number |= p_number >> 2;
  p_number |= p_number >> 4;
  p_number |= p_number >> 8;
  p_number |= p_number >> 16;
  return ++p_number;
}

/**
 * @brief 从 2 的幂值计算移位数
 *
 * 供 PagedAllocator 把"页大小"编译为"全局索引 → (页号, 槽位)"
 * 的位运算映射（右移取页号、位与取槽内偏移）—— 这正是
 * 页大小必须对齐到 2 的幂的原因。
 *
 * @param[in] p_value 2 的幂值（必须 > 0）
 *
 * @return log2(p_value)；非 2 的幂输入返回 0（视为不合法，
 *         调用方保证不发生 —— 页大小先经 NearestPow2 归一）
 */
ARHUD_ALWAYS_INLINE uint32 ShiftFromPow2(uint32 p_value) {
  for (uint32 i = 0; i < 32; ++i) {
    if (p_value == (uint32(1) << i)) {
      return i;
    }
  }
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════
// NoOpLock
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 空操作锁，用于 PagedAllocator 单线程模式的零开销抽象
 *
 * 所有方法均为内联空函数，编译器在非线程安全模式下完全消除锁开销
 * （Lock/Unlock 调用点编译为 nop）。
 *
 * @par 设计动机（策略模式消除分支）
 *   PagedAllocator 的热路径（Alloc/Free）不写两份代码，而是
 *   用 LockType = SpinLock | NoOpLock 的条件类型统一写法：
 *   kThreadSafe=true → 真锁；false → 空锁。
 *   这让单线程路径既无 if 分支也无函数调用开销，
 *   且两份代码永不漂移（同一份源码）。
 *
 * @see SpinLock  线程安全模式对应实现（os/sync.h）
 */
class NoOpLock {
 public:
  /** @brief 空操作（编译器消除） */
  void Lock() {}
  /** @brief 空操作（编译器消除） */
  void Unlock() {}
};

// ═══════════════════════════════════════════════════════════════════════
// PagedAllocator
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 分页内存分配器
 *
 * 按 page_size 为单位批量分配内存页，每页包含 page_size 个 T 对象槽位。
 * 使用 LIFO 可用槽位栈实现 O(1) 分配/释放，避免频繁调用全局分配器。
 *
 * @par 分配/释放热路径
 *   @code
 *   Alloc: available_pool_ 弹栈（--allocs_available_，位运算定位槽位）
 *          → placement-new 构造（锁外执行，缩短临界区）
 *   Free : 析构 → 压栈（++allocs_available_，写回槽位指针）
 *   @endcode
 *   稳态零全局分配器调用；仅当栈空时 Grow() 批发一整页。
 *
 * @par 典型用途
 * - GPU 资源簿记结构分配（RenderingDevice 各 Owner）
 * - 帧级对象池（配合 Rewind 跨帧复用，见下）
 *
 * @par 线程安全
 *   通过 kThreadSafe 模板参数选择 SpinLock 或 NoOpLock，
 *   编译期确定，零运行时开销（见 NoOpLock 文档）。
 *   锁只保护槽位栈操作；placement-new 在锁外。
 *
 * @par 车规模式
 *   调用 Preallocate() 在启动时预分配所有内存页，
 *   运行时不再调用 malloc，满足 QNX 安全要求。
 *
 * @par 内存布局（三层结构 + 具体实例）
 *
 *   三层独立分配的内存块（元数据两层 + 数据一层）：
 *   @code
 *   ┌─────────────────────────────────────────────────────────────────┐
 *   │ 第 1 层：page_pool_ : T**（页指针表，Realloc 随页数增长）         │
 *   │   page_pool_[i] ─────────────────────┐                          │
 *   └──────────────────────────────────────┼──────────────────────────┘
 *                                          │ 指向
 *   ┌──────────────────────────────────────▼──────────────────────────┐
 *   │ 第 2 层：每页本体 page_pool_[i] : T[page_size_]（AllocAligned）   │
 *   │   ┌────┬────┬────┬────┬─────────┬────┐                          │
 *   │   │ T0 │ T1 │ T2 │ T3 │   ...   │T255│   ← 对象槽位（构造/析构） │
 *   │   └────┴────┴────┴────┴─────────┴────┘                          │
 *   │   释放时以页为单位 FreeAligned 归还（前缀存指针法，见 memory.h）  │
 *   └──────────────────────────────────────────────────────────────────┘
 *
 *   ┌─────────────────────────────────────────────────────────────────┐
 *   │ 第 1 层：available_pool_ : T***（栈桶表，Realloc 随页数增长）      │
 *   │   available_pool_[i] ────────────────────┐                      │
 *   └──────────────────────────────────────────┼──────────────────────┘
 *                                              │ 指向
 *   ┌──────────────────────────────────────────▼──────────────────────┐
 *   │ 第 2 层：每页栈桶 available_pool_[i] : T*[page_size_]（Alloc）    │
 *   │   ┌─────────────┬─────────────┬─────────────┬──────────────┐   │
 *   │   │ &page0[0]   │ &page0[1]   │ &page0[2]   │   ...        │   │
 *   │   └─────────────┴─────────────┴─────────────┴──────────────┘   │
 *   │   桶内容 = 指向第 2 层页本体槽位的指针                           │
 *   └──────────────────────────────────────────────────────────────────┘
 *   @endcode
 *
 *   栈桶逻辑上拼成一条全局 LIFO 栈（allocs_available_ = 栈顶全局索引）：
 *   @code
 *   实例：page_size_=4，已 Grow 两页，栈满（allocs_available_ = 8）
 *
 *   全局索引:   0        1        2        3        4        5        6 7
 *            ┌────────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┐
 *   栈内容:    │&p0[0]  │&p0[1]  │&p0[2]  │&p0[3]  │&p1[0]  │&p1[1]  │&p1[2]
 * │&p1[3] │
 *            └────────┴────────┴────────┴────────┴────────┴────────┴────────┴────────┘
 *               └────────── page 0 桶 ──────────┘└────────── page 1 桶
 * ──────────┘ ▲ Alloc 弹栈: 取 [7]，allocs_available_: 8→7 │ 栈顶 Free  压栈:
 * 存 [7]，allocs_available_: 7→8                     allocs_available_
 *   @endcode
 *
 *   全局索引 → (页号, 桶内下标) 的映射（页大小为 2 的幂，纯位运算）：
 *   @code
 *   page_idx = global_index >> page_shift_   // 右移 = 除以 page_size_
 *   slot_idx = global_index &  page_mask_    // 位与 = 模 page_size_
 *   实例（page_size_=4, page_shift_=2, page_mask_=3）：
 *     全局索引 5 → 页 1（5>>2），桶内下标 1（5&3）→ available_pool_[1][1]
 *   @endcode
 *
 *   扩容时（Grow）：新页的 page_size_ 个槽位**追加在栈顶**（全局索引
 *   从旧栈顶继续编号）—— LIFO 保证新页槽位最先被分配。
 *   Rewind 时：按 page 0 → page N-1 顺序重填整条栈（page 0 沉底，
 *   新页在顶），下一轮 Alloc 优先使用最近批发的页（cache 局部性）。
 *
 * @par 模板参数选型
 *   | 参数 | 默认 | 选型建议 |
 *   |------|------|---------|
 *   | kThreadSafe | false | 默认单线程：渲染簿记池的
 *   |   |   | 消费方（RD、Storage）各自
 *   |   |   | 单线程持有；只有跨线程共享同一实例才置 true
 *   |   |   | （代价：热路径 SpinLock 临界区） |
 *   | kPageSize | 256 | 经验值：太小 → Grow 频繁、元数据表
 *   |   |   | Realloc 多；太大 → 碎片尾巴浪费。HUD 场景
 *   |   |   | 实测 32（CommandNode）~256（簿记）两档足够；
 *   |   |   | 传入任意值会被 NearestPow2 归一到 2 的幂 |
 *
 * @par 与 Reset/Rewind 的职责分界
 *   | 方法 | 页内存 | 槽位 | 对象析构 | 使用场景 |
 *   |------|--------|------|---------|---------|
 *   | Free | 保留 | 归还 1 个 | 单个，自动 | 常规释放 |
 *   | Rewind | 保留 | 归还全部 | **不调**（调用方自理） | 帧级池跨帧复用 |
 *   | Reset | 全释放 | 清零 | 不调（断言检查） | 销毁/重建 |
 *
 * @tparam T            分配的对象类型
 * @tparam kThreadSafe  是否线程安全（true 时内部使用 SpinLock）
 * @tparam kPageSize    每页包含的槽位数量（自动对齐到 2 的幂）
 *
 * @pre alignof(T) <= alignof(std::max_align_t)（即 T 不需要超对齐）
 */
template <typename T, bool kThreadSafe = false, uint32 kPageSize = 256>
class PagedAllocator {
  static_assert(alignof(T) <= alignof(std::max_align_t),
                "PagedAllocator does not support over-aligned types");
  using LockType =
      typename std::conditional<kThreadSafe, SpinLock, NoOpLock>::type;

 public:
  /**
   * @brief 构造：推导页几何（零内存分配）
   *
   * kPageSize 经 NearestPow2 归一到 2 的幂，预计算
   * page_shift_/page_mask_ —— 此后全局索引 ↔ (页, 槽位)
   * 换算全是位运算（无除法/取模）。首次 Alloc 才分配
   * 任何内存（延迟初始化，构造期无 malloc）。
   *
   * @post page_size_ 为 2 的幂；所有元数据指针为空
   */
  PagedAllocator();

  /**
   * @brief 析构：泄漏检查 + 全量释放
   *
   * @par 泄漏检查
   *   仍有对象未归还（allocs_available_ < 总槽位）时，
   *   仅对**非平凡析构**类型断言 —— 平凡类型（如命令
   *   节点）的"泄漏"只是内存归还，无资源风险；帧级
   *   Rewind 池析构时槽位几乎必然在用，属合法模式。
   *
   * @par 释放顺序
   *   栈桶（Free）→ 页本体（FreeAligned）→ 元数据表
   *   （Free）—— 与分配路径严格配对（memory.h 纪律）。
   */
  ~PagedAllocator();

  ARHUD_DISABLE_COPY(PagedAllocator);

  /**
   * @brief 分配一个 T 对象（弹栈 + placement-new 就地构造）
   *
   * @par 执行路径
   *   1. 加锁；可用栈空则 Grow() 批发一整页（均摊 O(1)）
   *   2. allocs_available_--（弹栈），
   *      位运算换算 (页号, 桶内下标) 取槽位指针
   *   3. 解锁（槽位归本线程独占）
   *   4. **锁外** placement-new 构造：构造函数耗时不可控，
   *      移出临界区最小化多线程锁竞争
   *
   * @par 失败语义
   *   无空返回：Grow 底层 AllocAligned 失败会经全局分配器
   *   断言/致命错误路径（引擎无异常），调用方拿到的一定是
   *   已构造对象。
   *
   * @tparam Args 构造函数参数类型（完美转发）
   * @param[in] p_args 传递给 T 构造函数的参数
   *
   * @return 指向新构造对象的指针（槽位有效，直到 Free/Rewind）
   *
   * @note 稳态（栈非空）路径 = 一次锁 + 一次减法 +
   *       两次移位/位与 + 一次间接寻址。
   */
  template <typename... Args>
  T* Alloc(Args&&... p_args);

  /**
   * @brief 释放一个 T 对象（析构 + 压栈归还）
   *
   * @par 执行路径
   *   1. Debug 双断言：非空 + 属于本分配器
   *      （IsOwnedPointer 拦截跨分配器 Free —— 那会
   *      污染目标池的栈元数据，早死早清晰）
   *   2. 加锁；**锁内**析构（与 Alloc 的锁外构造刻意
   *      不对称：析构必须先于回栈完成，否则另一线程
   *      可能立刻 Alloc 到该槽位并发构造，与本次析构
   *      读写同一块内存）
   *   3. 槽位指针压栈，allocs_available_++
   *
   * @param[in] p_mem 指向要释放的对象
   *
   * @warning p_mem 必须由本分配器 Alloc() 返回，且不得
   *          双重 Free（同一指针压两次栈 = 元数据损坏）。
   *          释放后的 p_mem 立即失效，持有副本属悬垂。
   *
   * @note LIFO 回收意味着刚 Free 的槽位是下一次 Alloc
   *       的首选 —— 临时对象的 cache 局部性最优。
   */
  void Free(T* p_mem);

  /**
   * @brief 预分配指定数量的对象槽位（车规模式）
   *
   * 向上取整到整页数后循环 Grow，全部槽位进栈待用。
   * 启动阶段调用一次即锁定"运行期零 malloc"。
   *
   * @par 使用契约
   *   - 必须在首次 Alloc() 之前调用（之后的调用仍可执行，
   *     但只补足差额页数，已分配的页不受影响）
   *   - 传入 0 合法但无意义（0 页）
   *
   * @param[in] p_max_count 预分配的最大对象数量
   *
   * @note 预分配量偏大只是浪费内存（页常驻），偏小则
   *       运行期 Grow 触发 malloc —— 车规场景按实测峰值
   *       + 余量取值。
   */
  void Preallocate(uint32 p_max_count);

  /**
   * @brief 重置分配器（全量释放，回到未分配态）
   *
   * @par 释放顺序（与析构一致，全局分配器配对纪律）
   *   1. 逐页 Free 栈桶（与 Alloc 配对）
   *   2. 逐页 FreeAligned 页本体（与 AllocAligned 配对）
   *   3. Free 两张元数据表（与 Realloc 配对）
   *
   * @par 泄漏断言语义
   *   p_allow_unfreed = false 且存在未归还对象时：
   *   仅**非平凡析构**类型断言（平凡类型"泄漏"只是内存
   *   归还，无资源风险）。shutdown 路径对簿记池传
   *   true（GL 对象的 glDeleteXxx 已在各 XxxFree 执行，
   *   allocator 只管簿记内存 —— 见 GLRenderingDeviceDriver::
   *   Shutdown）。
   *
   * @param[in] p_allow_unfreed 是否允许存在未释放的对象
   *
   * @warning 若 p_allow_unfreed=false 且存在未释放对象，
   *          非平凡析构类型将触发断言
   */
  void Reset(bool p_allow_unfreed = false);

  /**
   * @brief 归还所有槽位但保留页内存（帧级池跨帧复用）
   *
   * 与 Reset() 的本质差异：不触碰 malloc/free —— 页内存、
   * 元数据表、栈桶全部原地保留，仅把槽位指针重填回可用栈。
   * 稳态帧循环零分配器流量。
   *
   * @par 典型用途
   *   帧级对象池每帧 Rewind：上一帧对象全部作废
   *   （对象析构由调用方自理），
   *   Rewind 只回收槽位 —— 这正是"页内存跨帧复用"
   *   设计的兑现点。
   *
   * @par 填栈顺序的讲究
   *   page 0 沉栈底、最近批发的页在栈顶 → 下一轮 Alloc
   *   优先弹最近用过的页（大概率仍在 cache）—— 局部性
   *   与 Grow 的追加方向天然一致。
   *
   * @pre 所有非平凡析构对象必须已显式调用析构函数。
   *      本方法不扫槽位、不调析构 —— 违反则下一帧
   *      placement-new 叠在活对象上，UB。
   *
   * @post allocs_available_ == pages_allocated_ * page_size_
   *       （全槽位可用）；GetUsedCount() == 0
   *
   * @note 与 Free 的另一个区别：Free 精确归还单槽位，
   *       Rewind 视所有槽位为已死（类型擦除下无从析构，
   *       责任上移给持有对象语义的调用方）。
   */
  void Rewind();

  /**
   * @brief 获取在用对象数
   *
   * @par 计算口径
   *   总槽位（pages_allocated_ × page_size_）- 可用槽位
   *   （allocs_available_）。注意 Rewind 后必为 0（槽位
   *   全回收，即便对象"逻辑上还在被引用"）—— 本计数
   *   只反映池视角，不反映调用方的对象生命周期。
   *
   * @return 已分配但未释放（未回收）的对象数
   *
   * @note 线程安全模式下锁内读，返回一致快照；
   *       单线程模式为普通读。
   */
  uint32 GetUsedCount() const;

  /**
   * @brief 获取已批发的内存页数
   *
   * @return 页数（Grow 次数；Preallocate 后即预期值）
   */
  uint32 GetPageCount() const;

  /**
   * @brief 获取每页槽位数量
   *
   * @return 每页槽位数（kPageSize 经 NearestPow2 归一后的
   *         实际值，恒为 2 的幂；构造后不变）
   *
   * @note 页几何构造时确定，读取无需加锁。
   */
  uint32 GetPageSize() const;

 private:
  /**
   * @brief 批发一个新内存页（私有；仅 Alloc 栈空时调用）
   *
   * @par 步骤
   *   1. 页数 +1；page_pool_ / available_pool_ 两张元数据
   *      表 Realloc 扩容（元数据成本平摊到每次批发）
   *   2. AllocAligned 分配页本体（page_size_ 个连续 T，
   *      alignof(T) 对齐）
   *   3. Alloc 分配新页的栈桶（page_size_ 个 T*）
   *   4. 新页全部槽位**追加在全局栈顶**（全局索引从旧栈
   *      顶连续编号）—— LIFO 下新页槽位最先被分配
   *
   * @par 失败语义
   *   Realloc/AllocAligned 失败走全局分配器的断言/致命
   *   路径（引擎无异常）；本方法无静默失败。
   *
   * @note 调用方必须已持锁（Alloc/Preallocate 保证）。
   */
  void Grow();

#ifdef ARHUD_DEBUG
  /**
   * @brief 验证指针是否属于本分配器的某一页
   *
   * 仅 Debug 构建启用，用于 Free() 的防御性检查。
   *
   * @par 判定逻辑
   *   1. 落在某页 [begin, begin+page_size_) 区间内
   *   2. 且相对页首偏移是 sizeof(T) 的整数倍
   *      （拦截"页内指针算术越界后恰好落回同页"的野指针）
   *
   * @param[in] p_mem 待验证指针
   *
   * @return true 表示指针属于本分配器
   */
  bool IsOwnedPointer(const T* p_mem) const;
#endif
  // ── 元数据（随页数 Realloc 增长）──
  T** page_pool_ = nullptr;  ///< 页指针表：page_pool_[i] = 第 i
                             ///< 页槽位数组首地址（Realloc 扩容）
  T*** available_pool_ =
      nullptr;  ///< 可用槽位栈（二维桶结构，见类文档"内存布局"；桶按页分配）
  uint32 pages_allocated_ = 0;  ///< 已批发页数（= Grow 次数；元数据表长度）
  uint32 allocs_available_ =
      0;  ///< 栈顶全局索引（总槽位 - 在用数；Rewind 后 = 总槽位）

  // ── 页几何（构造时由 kPageSize 推导，此后不变；读取免锁）──
  uint32 page_shift_ =
      0;  ///< log2(page_size_)：全局索引右移 >> page_shift_ 得页号
  uint32 page_mask_ =
      0;  ///< page_size_-1：全局索引位与 & page_mask_ 得桶内下标
  uint32 page_size_ =
      0;  ///< 每页槽位数（kPageSize 经 NearestPow2 归一，恒 2 的幂）

  LockType lock_;  ///< 互斥策略（SpinLock / NoOpLock，kThreadSafe
                   ///< 编译期选择；热路径唯一锁点）
};

// ═══════════════════════════════════════════════════════════════════════
// PagedAllocator 模板实现
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 构造：推导页几何（零内存分配）
 *
 * kPageSize 归一到 2 的幂并预计算 shift/mask —— 此后所有
 * 全局索引 ↔ (页, 槽位) 换算都是位运算，无除法/取模。
 * 首次 Alloc 才分配任何内存（延迟初始化）。
 */
template <typename T, bool kThreadSafe, uint32 kPageSize>
PagedAllocator<T, kThreadSafe, kPageSize>::PagedAllocator() {
  page_size_ = NearestPow2(kPageSize);
  page_mask_ = page_size_ - 1;
  page_shift_ = ShiftFromPow2(page_size_);
}

/**
 * @brief 析构：泄漏检查 + 全量释放
 *
 * @par 泄漏检查语义
 *   allocs_available_ < 总槽位数 = 仍有对象未归还。仅对
 *   **非平凡析构**类型断言 —— 平凡类型（如 POD 命令节点）
 *   "泄漏"只是内存归还，无资源风险，且帧级 Rewind 池在
 *   析构时槽位几乎必然处于在用状态，属合法使用模式。
 *
 * @par 释放顺序
 *   1. 逐页释放 available_pool_ 桶（普通 Free，Alloc 分配）
 *   2. 逐页释放页本体（FreeAligned，AllocAligned 分配 ——
 *      配对纪律见 memory.h 前缀存指针法）
 *   3. 释放两张元数据表（page_pool_ / available_pool_）
 */
template <typename T, bool kThreadSafe, uint32 kPageSize>
PagedAllocator<T, kThreadSafe, kPageSize>::~PagedAllocator() {
  lock_.Lock();

  bool leaked = allocs_available_ < pages_allocated_ * page_size_;
  if (leaked) {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      ARHUD_ASSERT(allocs_available_ == pages_allocated_ * page_size_,
                   "PagedAllocator has unfreed objects at destruction");
    }
  }

  for (uint32 i = 0; i < pages_allocated_; ++i) {
    memory::Free(available_pool_[i]);
  }

  for (uint32 i = 0; i < pages_allocated_; ++i) {
    memory::FreeAligned(page_pool_[i]);
  }

  if (page_pool_ != nullptr) {
    memory::Free(page_pool_);
    memory::Free(available_pool_);
  }

  page_pool_ = nullptr;
  available_pool_ = nullptr;
  pages_allocated_ = 0;
  allocs_available_ = 0;

  lock_.Unlock();
}

/**
 * @brief Grow() 实现（步骤与契约见类内声明处的 Doxygen）
 */
template <typename T, bool kThreadSafe, uint32 kPageSize>
void PagedAllocator<T, kThreadSafe, kPageSize>::Grow() {
  // 1. 元数据表 Realloc 扩容（调用方已持锁，无并发）
  uint32 pages_used = pages_allocated_;
  pages_allocated_++;

  page_pool_ = static_cast<T**>(
      memory::Realloc(page_pool_, sizeof(T*) * pages_allocated_));
  available_pool_ = static_cast<T***>(
      memory::Realloc(available_pool_, sizeof(T**) * pages_allocated_));

  // 2. 页本体：AllocAligned 保证 alignof(T)（类前置条件已排除超对齐）
  page_pool_[pages_used] =
      static_cast<T*>(memory::AllocAligned(sizeof(T) * page_size_, alignof(T)));
  // 3. 新页的栈桶
  available_pool_[pages_used] =
      static_cast<T**>(memory::Alloc(sizeof(T*) * page_size_));

  // 4. 新页全部槽位追加到全局栈顶（全局索引连续编号，
  //    新页槽位排在所有旧槽位之后 → LIFO 下最先被分配）
  for (uint32 i = 0; i < page_size_; ++i) {
    uint32 global_idx = allocs_available_ + i;
    available_pool_[global_idx >> page_shift_][global_idx & page_mask_] =
        &page_pool_[pages_used][i];
  }
  allocs_available_ += page_size_;
}

#ifdef ARHUD_DEBUG
/**
 * @brief IsOwnedPointer() 实现（判定逻辑见类内声明处的 Doxygen）
 */
template <typename T, bool kThreadSafe, uint32 kPageSize>
bool PagedAllocator<T, kThreadSafe, kPageSize>::IsOwnedPointer(
    const T* p_mem) const {
  for (uint32 i = 0; i < pages_allocated_; ++i) {
    const T* page_begin = page_pool_[i];
    const T* page_end = page_begin + page_size_;
    if (p_mem >= page_begin && p_mem < page_end) {
      // 落在页区间内：再验证槽位对齐（偏移须为 sizeof(T) 整数倍）
      uintptr_t offset = reinterpret_cast<uintptr_t>(p_mem) -
                         reinterpret_cast<uintptr_t>(page_begin);
      return (offset % sizeof(T)) == 0;
    }
  }
  return false;
}
#endif

/**
 * @brief Alloc() 实现
 *
 * @par 关键点
 *   - 栈空才 Grow（批发摊销，均摊 O(1)）
 *   - placement-new 在**锁外**：构造可能耗时（任意用户构造
 *     函数），缩短临界区让多线程并发分配时锁竞争最小化。
 *     安全性：槽位弹出后归本线程独占，锁外构造无竞态。
 */
template <typename T, bool kThreadSafe, uint32 kPageSize>
template <typename... Args>
T* PagedAllocator<T, kThreadSafe, kPageSize>::Alloc(Args&&... p_args) {
  lock_.Lock();
  if (allocs_available_ == 0) {
    Grow();
  }
  allocs_available_--;
  // 先取槽位指针再解锁 —— 索引换算在锁内完成（栈状态一致）
  T* ptr = available_pool_[allocs_available_ >> page_shift_]
                          [allocs_available_ & page_mask_];
  lock_.Unlock();
  return new (ptr) T(std::forward<Args>(p_args)...);
}

/**
 * @brief Free() 实现
 *
 * @par 关键点
 *   - Debug 双断言：非空 + 属于本分配器（跨分配器 Free =
 *     元数据损坏，早死早清晰）
 *   - 析构在**锁内**（与 Alloc 的锁外构造不对称）：析构必须
 *     先于槽位回栈完成 —— 若先回栈再析构，另一线程可能
 *     立刻 Alloc 到该槽位并构造，与本次析构并发读写同块内存
 */
template <typename T, bool kThreadSafe, uint32 kPageSize>
void PagedAllocator<T, kThreadSafe, kPageSize>::Free(T* p_mem) {
#ifdef ARHUD_DEBUG
  ARHUD_ASSERT(p_mem != nullptr, "PagedAllocator::Free(nullptr)");
  ARHUD_ASSERT(IsOwnedPointer(p_mem),
               "PagedAllocator::Free() pointer not owned by this allocator");
#endif
  lock_.Lock();
  p_mem->~T();
  available_pool_[allocs_available_ >> page_shift_]
                 [allocs_available_ & page_mask_] = p_mem;
  allocs_available_++;
  lock_.Unlock();
}

/**
 * @brief Preallocate() 实现
 *
 * 向上取整到整页数后循环 Grow —— 槽位全部进栈待用。
 * 首次 Alloc 前调用一次即满足"运行期零 malloc"的车规约束。
 */
template <typename T, bool kThreadSafe, uint32 kPageSize>
void PagedAllocator<T, kThreadSafe, kPageSize>::Preallocate(
    uint32 p_max_count) {
  lock_.Lock();
  uint32 pages_needed = (p_max_count + page_size_ - 1) / page_size_;
  while (pages_allocated_ < pages_needed) {
    Grow();
  }
  lock_.Unlock();
}

/**
 * @brief Reset() 实现（全量释放，回到未分配态）
 *
 * 释放顺序与析构函数一致（栈桶 → 页本体 → 元数据表）。
 * 释放前按 p_allow_unfreed 与析构性做泄漏断言（语义同析构，
 * 平凡类型跳过）。此后分配器等价于新构造状态。
 */
template <typename T, bool kThreadSafe, uint32 kPageSize>
void PagedAllocator<T, kThreadSafe, kPageSize>::Reset(bool p_allow_unfreed) {
  lock_.Lock();
  // !p_allow_unfreed && 非平凡析构 && 存在未归还对象 → 断言
  // （std::is_trivially_destructible_v 是编译期常量，非平凡
  //  类型时条件短路后段才可能为真）
  if (!p_allow_unfreed && !std::is_trivially_destructible_v<T>) {
    ARHUD_ASSERT(allocs_available_ == pages_allocated_ * page_size_,
                 "PagedAllocator::Reset() called with unfreed objects");
  }
  for (uint32 i = 0; i < pages_allocated_; ++i) {
    memory::Free(available_pool_[i]);
  }
  for (uint32 i = 0; i < pages_allocated_; ++i) {
    memory::FreeAligned(page_pool_[i]);
  }
  if (page_pool_ != nullptr) {
    memory::Free(page_pool_);
    memory::Free(available_pool_);
  }
  page_pool_ = nullptr;
  available_pool_ = nullptr;
  pages_allocated_ = 0;
  allocs_available_ = 0;
  lock_.Unlock();
}

/**
 * @brief Rewind() 实现（槽位全回收、页内存全保留）
 *
 * @par 为什么不调任何 Free
 *   本方法的目标场景是帧级对象池（每帧重填）：
 *   页内存跨帧复用，malloc/free 归零。对象析构由调用方
 *   自理（声明处 @pre 已约定）—— 本方法不扫槽位、不碰对象。
 *
 * @par 填栈顺序的讲究（page 0 在栈底，page N-1 在栈顶）
 *   下一轮 Alloc 从栈顶弹 → 先用最近批发的页 → 该页大概率
 *   还在 cache 里（上一帧刚用过）→ 局部性友好。这与 Grow
 *   的追加方向天然一致，重建循环即为顺序遍历。
 */
template <typename T, bool kThreadSafe, uint32 kPageSize>
void PagedAllocator<T, kThreadSafe, kPageSize>::Rewind() {
  lock_.Lock();
  allocs_available_ = 0;
  for (uint32 page = 0; page < pages_allocated_; ++page) {
    for (uint32 slot = 0; slot < page_size_; ++slot) {
      uint32 global_idx = allocs_available_;
      available_pool_[global_idx >> page_shift_][global_idx & page_mask_] =
          &page_pool_[page][slot];
      allocs_available_++;
    }
  }
  lock_.Unlock();
}

/** @brief GetUsedCount() 实现（总槽位 - 可用槽位；锁内读一致） */
template <typename T, bool kThreadSafe, uint32 kPageSize>
uint32 PagedAllocator<T, kThreadSafe, kPageSize>::GetUsedCount() const {
  lock_.Lock();
  uint32 count = pages_allocated_ * page_size_ - allocs_available_;
  lock_.Unlock();
  return count;
}

/** @brief GetPageCount() 实现 */
template <typename T, bool kThreadSafe, uint32 kPageSize>
uint32 PagedAllocator<T, kThreadSafe, kPageSize>::GetPageCount() const {
  lock_.Lock();
  uint32 count = pages_allocated_;
  lock_.Unlock();
  return count;
}

/** @brief GetPageSize() 实现（页几何构造后不变，无需加锁） */
template <typename T, bool kThreadSafe, uint32 kPageSize>
uint32 PagedAllocator<T, kThreadSafe, kPageSize>::GetPageSize() const {
  return page_size_;
}

}  // namespace arhud

#endif  // ARHUD_CORE_TEMPLATE_PAGED_ALLOCATOR_H_
