/**
 * @file resource_tracker.h
 * @brief 轻量级资源追踪器 — 资源使用记录与冲突检测
 *
 * 本文件定义了命令缓冲系统的资源追踪基础设施：
 * - **ResourceUsage 枚举**：资源访问模式（读/写/绑定等）
 * - **ResourceType 枚举**：被追踪的资源类型分类
 * - **ResourceTrack 结构体**：单个 GPU 资源的使用快照
 * - **ResourceTracker 类**：帧级资源追踪管理器
 *
 * @par 设计目标（两阶段演进）
 *
 * **Phase 1（当前，OpenGL 阶段）：**
 *   - ✅ 资源注册与注销（显式生命周期管理）
 *   - ✅ 使用记录（读/写/绑定操作的时间戳）
 *   - ✅ 冲突检测（Debug 断言模式，捕获明显错误）
 *   - ❌ 不做屏障推导（OpenGL 单队列隐式同步）
 *   - ❌ 不做 DAG 编译（保持线性回放顺序）
 *
 * **Phase 2（未来，Vulkan 阶段）：**
 *   - ➕ PipelineStage 追踪（顶点/片段/计算等管线阶段）
 *   - ➕ 自动屏障推导（RAW/WAR/WAW hazard 检测）
 *   - ➕ DAG 编译与命令重排序（无依赖命令并行化）
 *   - ➕ 子资源追踪（纹理 mip/layer 级别粒度）
 *
 * @par 与 Godot ResourceTracker 的对比
 *   | 特性 | Godot RDG ResourceTracker | ARHud ResourceTracker (Phase 1) |
 *   |------|---------------------------|----------------------------------|
 *   | 屏障推导 | ✅ 完整实现 | ❌ 暂不实现 |
 *   | 子资源追踪 | ✅ TextureSlice 链表 | ❌ 整资源级别 |
 *   | PipelineStage | ✅ BitField 追踪 | ❌ 暂不追踪 |
 *   | 依赖链表 | ✅ 读/写链表结构 | ❌ 仅最后使用记录 |
 *   | 可丢弃标记 | ✅ is_discardable | ❌ 暂不实现 |
 *   | 帧重置 | ✅ command_frame 检测 | ✅ BeginFrame/EndFrame |
 *   | 内存开销 | ~200 bytes/resource | ~32 bytes/resource |
 *   | 实现复杂度 | ~1500 行 | ~300 行 |
 *
 * @par 典型使用流程
 *   @code
 *   // 帧开始：重置追踪器
 *   tracker.BeginFrame();
 *
 *   // 注册当前帧使用的资源
 *   tracker.Register(texture_id, ResourceType::kTexture);
 *   tracker.Register(buffer_id, ResourceType::kBuffer);
 *   tracker.Register(pipeline_id, ResourceType::kPipeline);
 *
 *   // 命令录制时记录使用（在 ICommandBuffer 方法内部自动调用）
 *   tracker.RecordUsage(buffer_id, ResourceUsage::kVertexRead, cmd_index);
 *   tracker.RecordUsage(texture_id, ResourceUsage::kSampled, cmd_index);
 *
 *   // 冲突检测（Debug 模式下自动断言）
 *   ARHUD_ASSERT(tracker.ValidateUsage(uniform_set_id, ResourceUsage::kUniformRead));
 *
 *   // 帧结束：清理临时数据
 *   tracker.EndFrame();
 *   @endcode
 *
 * @par 线程安全模型
 *   - **BeginFrame/EndFrame**：必须由录制线程调用（与 CommandBuffer Begin/Reset 同一线程）
 *   - **Register/Unregister**：可由任意线程调用（需外部 Mutex 保护）
 *   - **RecordUsage/ValidateUsage**：通常由录制线程调用（CommandBuffer 内部自动调用）
 *   - **注意**：ResourceTracker 本身不做内部同步，依赖外部 CommandBuffer 的 Mutex
 *
 * @par 实际调用线程（当前实现）
 *   ResourceTracker 的所有方法都在 **主线程（录制线程）** 调用：
 *   - BeginFrame()  → GLCommandBuffer::Begin() 内部调用
 *   - Register()    → GLCommandBuffer::BindXxx() 内部调用
 *   - RecordUsage() → GLCommandBuffer::Draw/BindXxx() 内部调用
 *   - EndFrame()    → GLCommandBuffer::Reset()/析构函数内部调用
 *
 * @see command_buffer.h          命令缓冲接口（集成 ResourceTracker）
 * @see GLCommandBuffer            OpenGL 实现（内部持有 ResourceTracker）
 * @see rendering_device_commons.h 资源 ID 类型定义
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-09
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#pragma once

#include "rendering_device_commons.h"
#include "template/hash_map.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // 资源使用类型枚举
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 资源访问模式（来自 rendering_device_commons.h）
     *
     * 标识命令对 GPU 资源的访问方式。用于冲突检测和未来的屏障推导。
     * 直接复用 rendering_device_commons.h 中的定义，避免重复。
     *
     * @par 使用场景映射
     *   | ResourceUsage | 典型命令 | 说明 |
     *   |---------------|----------|------|
     *   | kNone | — | 未使用或已注销 |
     *   | kCopyFrom | CopyBuffer/CopyTexture | 拷贝源（只读） |
     *   | kCopyTo | CopyBuffer/CopyTexture | 拷贝目标（只写） |
     *   | kTextureSample | 绑定到采样器 | 纹理采样（着色器 texelFetch） |
     *   | kAttachmentColorReadWrite | BeginRenderPass (color attachment) | 渲染目标读写 |
     *   | kAttachmentDepthStencilReadWrite | BeginRenderPass (depth/stencil) | 深度模板读写 |
     *   | kStorageImageReadWrite | （Phase 2）存储图像读写 | 计算着色器访问 |
     *
     * @note 扩展用途：在 command_buffer_gl.cpp 中，我们额外使用以下非标准值用于更细粒度的追踪：
     *   - kVertexRead: 顶点数据读取（VB/IB）— 映射为 kNone（暂不追踪）
     *   - kIndexRead: 索引数据读取（IB only）— 映射为 kNone（暂不追踪）
     *   - kUniformRead: Uniform 数据读取（UB/SB）— 映射为 kTextureSample（近似）
     *   - kClear: 清除操作（只写）— 映射为 kCopyTo（近似）
     */
    using ResourceUsage = arhud::ResourceUsage;

    /**
     * @brief 检查是否为只读使用
     * @param[in] p_usage 资源使用类型
     * @return true 如果该使用模式不会修改资源内容
     */
    inline bool IsReadOnlyUsage(ResourceUsage p_usage)
    {
        return p_usage == ResourceUsage::kCopyFrom ||
               p_usage == ResourceUsage::kTextureSample;
    }

    /**
     * @brief 检查是否为只写使用
     * @param[in] p_usage 资源使用类型
     * @return true 如果该使用模式只会写入资源
     */
    inline bool IsWriteOnlyUsage(ResourceUsage p_usage)
    {
        return p_usage == ResourceUsage::kCopyTo;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 资源类型枚举
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 被追踪的资源类型分类
     *
     * 用于区分不同类型的 GPU 资源，以便应用特定的验证规则。
     */
    enum class ResourceType : uint8_t
    {
        kBuffer = 0,  ///< 通用缓冲区（VB/IB/UB/SB/Staging）
        kTexture,     ///< 纹理（2D/3D/Cube/Array）
        kSampler,     ///< 采样器状态
        kUniformSet,  ///< Uniform 描述符集合
        kPipeline,    ///< 渲染/计算管线
        kRenderPass,  ///< 渲染 Pass 定义
        kFramebuffer, ///< 帧缓冲（Attachment 集合）
        kMax          ///< 哨兵值
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 单个资源追踪信息
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @struct ResourceTrack
     * @brief 单个 GPU 资源的使用快照
     *
     * 记录一个资源在当前帧中的最后一次使用信息。
     * 当新命令使用同一资源时，通过比较新旧 usage 检测潜在冲突。
     *
     * @par 内存布局（Phase 1，紧凑设计）
     *   @code
     *   ┌──────────────────────────────────────┐
     *   │ ResourceTrack (~24 bytes on 64-bit)  │
     *   ├──────────────────────────────────────┤
     *   │ type_          : 1 byte  (enum class)│
     *   │ last_usage_    : 1 byte  (enum class)│
     *   │ last_cmd_index_: 4 bytes (uint32)    │
     *   │ is_active_     : 1 byte  (bool)      │
     *   │ padding        : 3 bytes             │
     *   │ debug_name_    : 8 bytes (ptr)       │ ← 仅 Debug 构建
     *   │ access_count_  : 4 bytes (uint32)    │ ← 仅 Debug 构建
     *   └──────────────────────────────────────┘
     *   @endcode
     *
     * @par Phase 2 扩展预留
     *   未来 Vulkan 阶段将扩展为：
     *   - `previous_stages_`: BitField<PipelineStageBits>（前一命令的管线阶段）
     *   - `current_stages_`: BitField<PipelineStageBits>（当前命令的管线阶段）
     *   - `read_chain_head_`: int32_t（读依赖链表头索引）
     *   - `write_chain_head_`: int32_t（写依赖链表头索引）
     *   - `is_discardable_`: bool（可丢弃标记，优化 loadOp）
     *
     * @note 此结构体设计为可平凡拷贝（trivially copyable），
     *       可安全存储在 HashMap 中。
     */
    struct ResourceTrack
    {
        ResourceType type_ = ResourceType::kMax;          ///< 资源类型分类
        ResourceUsage last_usage_ = ResourceUsage::kNone; ///< 最后一次使用模式
        uint32_t last_cmd_index_ = UINT32_MAX;            ///< 最后一次使用的命令索引
        bool is_active_ = false;                          ///< 当前帧是否活跃

#ifdef ARHUD_DEBUG
        const char *debug_name_ = nullptr; ///< 调试用名称（可选）
        uint32_t access_count_ = 0;        ///< 当前帧访问次数统计
#endif
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 资源追踪器主类
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @class ResourceTracker
     * @brief 帧级资源追踪管理器
     *
     * 管理当前帧中所有 GPU 资源的使用记录，提供：
     * - **资源注册/注销**：显式声明当前帧涉及的资源
     * - **使用记录**：记录每个资源的最后一次使用方式
     * - **冲突检测**：检查是否存在明显的资源竞争（Debug 断言）
     * - **帧级生命周期**：BeginFrame/EndFrame 自动重置状态
     *
     * @par 架构定位
     *   @code
     *   ┌─────────────────────────────────────────────────┐
     *   │              GLCommandBuffer                     │
     *   ├─────────────────────────────────────────────────┤
     *   │  commands_[]: LocalVector<RecordedCommand*>      │
     *   │  command_allocator_: PagedAllocator<>            │
     *   │                                                 │
     *   │  resource_tracker_: ResourceTracker  ◄── 本类    │
     *   │    ├── tracks_: HashMap<uint64_t, ResourceTrack> │
     *   │    ├── current_frame_: uint64_t                  │
     *   │    └── active_count_: uint32_t                   │
     *   └─────────────────────────────────────────────────┘
     *   @endcode
     *
     * @par 与 CommandBuffer 的集成方式
     *   ResourceTracker 作为 GLCommandBuffer 的成员存在，
     *   在以下时机自动调用：
     *
     *   1. **Begin()** → tracker_.BeginFrame()
     *   2. **录制方法** → tracker_.RecordUsage(id, usage, cmd_index)
     *   3. **Execute() 前** → tracker_.ValidateAll()（可选）
     *   4. **Reset()** → tracker_.EndFrame()
     *
     * @par 性能特征（Phase 1）
     *   | 操作 | 时间复杂度 | 开销 |
     *   |------|-----------|------|
     *   | Register | O(1) amortized | HashMap insert |
     *   | Unregister | O(1) expected | HashMap erase |
     *   | RecordUsage | O(1) expected | HashMap lookup + write |
     *   | ValidateUsage | O(1) expected | HashMap lookup + compare |
     *   | BeginFrame | O(n) | 遍历所有 track 重置 is_active_ |
     *   | EndFrame | O(1) | 仅重置计数器 |
     *
     * @warning ResourceTracker **不做内部线程同步**，
     *          依赖外部 CommandBuffer 的 Mutex 保护并发访问。
     *
     * @see command_buffer.h      命令缓冲接口定义
     * @see GLCommandBuffer       OpenGL 命令缓冲实现
     * @see ResourceTrack         单个资源追踪信息
     */
    class ResourceTracker
    {
    public:
        ResourceTracker() = default;
        ~ResourceTracker() = default;

        ARHUD_DISABLE_COPY_MOVE(ResourceTracker);

        // ══════════════════════════════════════════════════════════════
        // 生命周期管理
        // ══════════════════════════════════════════════════════════════

        /**
         * @brief 开始新的一帧追踪
         *
         * 重置所有已注册资源的活跃状态，准备接收新的使用记录。
         * 应在 CommandBuffer::Begin() 时调用。
         *
         * @par 内部操作
         *   1. current_frame_++（递增帧号）
         *   2. 遍历 tracks_，将所有 is_active_ 设为 false
         *   3. 重置 active_count_ 为 0
         *
         * @pre 必须在录制线程调用（与 CommandBuffer::Begin() 同一线程）
         * @post 所有资源标记为非活跃，可重新注册
         *
         * @par 调用时机
         *   在 GLCommandBuffer::Begin() 内部自动调用，用户无需手动调用。
         *   录制线程通常是主线程或工作线程。
         *
         * @thread_safety Caller must hold CommandBuffer lock (called inside Begin())
         */
        void BeginFrame();

        /**
         * @brief 结束当前帧追踪
         *
         * 清理帧末的临时数据。应在 CommandBuffer::Reset() 时调用。
         *
         * @par 内部操作
         *   1. （Phase 1 无额外操作，保留接口供 Phase 2 扩展）
         *   2. 未来可能输出统计信息或检测泄漏
         *
         * @pre 必须在录制线程调用（与 CommandBuffer::Reset() 同一线程）
         * @post 追踪器进入空闲状态
         *
         * @par 调用时机
         *   在 GLCommandBuffer::Reset() 或析构函数中自动调用。
         *
         * @thread_safety Caller must hold CommandBuffer lock (called inside Reset())
         */
        void EndFrame();

        // ══════════════════════════════════════════════════════════════
        // 资源注册与注销
        // ══════════════════════════════════════════════════════════════

        /**
         * @brief 注册资源到追踪器
         *
         * 声明当前帧将要使用某个 GPU 资源。注册后才能 RecordUsage 和 ValidateUsage。
         *
         * @param[in] p_id 资源 ID（转换为 uint64_t 作为 key）
         * @param[in] p_type 资源类型分类
         * @param[in] p_debug_name 可选的调试名称（仅 Debug 构建）
         *
         * @par 典型调用时机
         *   - 在 CommandBuffer::BeginRenderPass() 时注册 framebuffer、render_pass
         *   - 在 BindPipeline() 时注册 pipeline
         *   - 在 BindVertexBuffer() 时注册 buffer
         *   - 在 BindUniformSet() 时注册 uniform_set
         *
         * @note 如果资源已存在，更新 type_ 并重置使用记录。
         * @note 此方法通常由 GLCommandBuffer 的录制方法内部自动调用，
         *       用户无需手动注册。
         *
         * @thread_safety Caller must hold external lock (e.g., CommandBuffer mutex)
         */
        void Register(uint64_t p_id, ResourceType p_type, const char *p_debug_name = nullptr);

        /**
         * @brief 注销资源
         *
         * 从追踪器中移除资源。通常在资源销毁时调用。
         *
         * @param[in] p_id 资源 ID
         *
         * @note 如果资源不存在，静默忽略（无错误）。
         * @note 通常由 RenderingDeviceDriver 的 xxx_free() 方法调用。
         *
         * @thread_safety Caller must hold external lock
         */
        void Unregister(uint64_t p_id);

        // ══════════════════════════════════════════════════════════════
        // 使用记录
        // ══════════════════════════════════════════════════════════════

        /**
         * @brief 记录资源使用
         *
         * 更新资源的最后一次使用信息。每次命令录制时调用。
         *
         * @param[in] p_id 资源 ID
         * @param[in] p_usage 使用模式（读/写/绑定等）
         * @param[in] p_cmd_index 当前命令在 CommandBuffer 中的索引
         *
         * @par 内部操作
         *   1. 在 tracks_ 中查找 p_id
         *   2. 如果找到且 is_active_ == true：
         *      a. 更新 last_usage_ = p_usage
         *      b. 更新 last_cmd_index_ = p_cmd_index
         *      c. Debug: access_count_++
         *   3. 如果未找到或不活跃：
         *      a. Debug 模式输出警告（使用了未注册的资源）
         *      b. 静默忽略（不崩溃）
         *
         * @note 此方法**不做冲突检测**，仅记录使用情况。
         *       冲突检测由 ValidateUsage() 或 ValidateAll() 完成。
         *
         * @thread_safety Caller must hold external lock
         */
        void RecordUsage(uint64_t p_id, ResourceUsage p_usage, uint32_t p_cmd_index);

        // ══════════════════════════════════════════════════════════════
        // 冲突检测
        // ══════════════════════════════════════════════════════════════

        /**
         * @brief 验证单个资源的使用合法性
         *
         * 检查对资源的指定使用是否与已有使用冲突。
         *
         * @param[in] p_id 资源 ID
         * @param[in] p_usage 即将执行的使用模式
         * @return true 如果使用合法（无冲突），false 如果存在潜在冲突
         *
         * @par 冲突检测规则（Phase 1）
         *   1. **写-写冲突（W-W）**：
         *      - last_usage_ 是写模式（CopyDst/Clear/ColorAttachRW/DepthStencilRW）
         *      - p_usage 也是写模式
         *      - ⚠️ 可能是 WAW hazard（两次写入同一资源）
         *
         *   2. **读-写冲突（R-W）**：
         *      - last_usage_ 是读模式（VertexRead/IndexRead/UniformRead/Sampled/CopySrc）
         *      - p_usage 是写模式
         *      - ⚠️ 可能是 RAW hazard（先读后写）
         *
         *   3. **未注册资源**：
         *      - p_id 不在 tracks_ 中
         *      - ⚠️ 可能使用了已释放或从未创建的资源
         *
         * @note Phase 1 仅返回 bool 并在 Debug 模式输出日志。
         *       Phase 2 将生成实际的 Barrier 命令插入到 CommandBuffer 中。
         *
         * @thread_safe Yes (read-only operation on shared data)
         */
        bool ValidateUsage(uint64_t p_id, ResourceUsage p_usage) const;

        /**
         * @brief 验证所有已注册资源的一致性
         *
         * 遍历所有活跃资源，检查是否存在异常状态。
         * 应在 Execute() 之前调用，用于最终一致性检查。
         *
         * @return true 如果所有资源状态一致，false 如果发现异常
         *
         * @par 检查项
         *   - 是否有资源被注册但从未使用（僵尸资源）
         *   - 是否有资源的 last_cmd_index_ 顺序异常
         *   - （Phase 2）是否所有依赖都已正确解析
         *
         * @thread_safe Yes (read-only operation)
         */
        bool ValidateAll() const;

        // ══════════════════════════════════════════════════════════════
        // 查询接口
        // ══════════════════════════════════════════════════════════════

        /**
         * @brief 检查资源是否已注册且活跃
         * @param[in] p_id 资源 ID
         * @return true 如果资源当前在追踪器中且标记为活跃
         */
        bool IsActive(uint64_t p_id) const;

        /**
         * @brief 获取资源的最后一次使用信息
         * @param[in] p_id 资源 ID
         * @return 指向 ResourceTrack 的指针，如果不存在返回 nullptr
         *
         * @note 返回的指针在下次 Register/Unregister/BeginFrame 前有效
         */
        const ResourceTrack *GetTrack(uint64_t p_id) const;

        /**
         * @brief 获取当前活跃资源数量
         * @return 当前帧已注册且活跃的资源总数
         */
        uint32_t GetActiveCount() const { return active_count_; }

        /**
         * @brief 获取当前帧号
         * @return 自创建以来（或上次 Reset）的帧计数
         */
        uint64_t GetCurrentFrame() const { return current_frame_; }

#ifdef ARHUD_DEBUG
        /**
         * @brief 输出所有资源追踪状态（Debug 用）
         *
         * 通过 logger 输出当前所有活跃资源的详细信息，
         * 包括类型、使用模式、命令索引、访问次数等。
         *
         * @par 输出格式示例
         *   @code
         *   [ResourceTracker] Frame=42 Active=5
         *   [0x1401] Texture  sampled      cmd=3  count=5  "HUD_Background"
         *   [0x1402] Buffer   vertex_read  cmd=1  count=1  "Quad_VB"
         *   [0x1403] Pipeline  uniform_read cmd=2  count=3  "UI_Pipeline"
         *   @endcode
         */
        void DumpState() const;
#endif

    private:
        /**
         * @brief 资源追踪表
         *
         * key = 资源 ID 的原始 uint64_t 值
         * value = 该资源的使用快照
         *
         * 使用 HashMap 提供 O(1) 的查找性能。
         * 内存占用取决于当前帧注册的资源数量。
         */
        HashMap<uint64_t, ResourceTrack> tracks_;
        uint64_t current_frame_ = 0; ///< 当前帧号（递增计数器）
        uint32_t active_count_ = 0;  ///< 当前活跃资源数量
    };

} // namespace arhud
