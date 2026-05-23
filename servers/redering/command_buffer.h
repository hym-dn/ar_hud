/**
 * @file command_buffer.h
 * @brief 命令缓冲抽象接口与命令类型定义
 *
 * 本文件定义了渲染命令缓冲系统的核心抽象：
 * - **CommandType 枚举**：所有支持的渲染命令类型标识
 * - **CmdXxx 结构体**：每种命令的扁平化数据载体（POD/半 POD）
 * - **RecordedCommand**：已录制命令条目（类型 + 数据联合体）
 * - **CommandBufferState 枚举**：命令缓冲生命周期状态机
 * - **ICommandBuffer 接口**：录制/回放的统一抽象
 *
 * @par 设计目标（参考 Godot RenderingDeviceGraph）
 *   1. **主线程录制、渲染线程回放** — 生产者-消费者模式解耦逻辑与 GPU 调用
 *   2. **零 GL 调用开销** — 录制阶段仅记录参数，不触发任何 OpenGL API
 *   3. **缓存友好布局** — 使用 PagedAllocator + 指针数组存储命令，
 *      避免不可复制 union 类型导致的 LocalVector 扩容问题
 *   4. **线程安全录制** — 所有录制方法内部使用 Mutex 保护
 *   5. **可复用设计** — 同一 CommandBuffer 对象可跨帧复用（Begin → 录制 → End → Execute → Reset）
 *
 * @par 线程模型
 *   @code
 *   ┌──────────────────┐     Submit()      ┌──────────────────┐
 *   │  Main Thread     │ ──────────────→    │  Render Thread    │
 *   │                  │                    │                  │
 *   │  ICommandBuffer  │   (thread-safe)    │  GLCommandBuffer  │
 *   │  - BindPipeline  │                    │  - Execute()      │
 *   │  - Draw          │                    │  - glDrawArrays   │
 *   │  - EndRenderPass │                    │  - glUseProgram   │
 *   └──────────────────┘                    └──────────────────┘
 *   @endcode
 *
 *   - **录制阶段**：任意线程（通常主/工作线程），Mutex 保护
 *   - **回放阶段**：必须持有 GL 上下文的渲染线程，无锁
 *
 * @par 与 Godot RenderingDeviceGraph 的差异
 *   | 特性 | Godot RDG | ARHud CommandBuffer |
 *   |------|-----------|---------------------|
 *   | 资源追踪 | ✅ 完整 ResourceTracker + DAG | ✅ 轻量级 ResourceTracker (Phase 1) |
 *   | 屏障推导 | ✅ 自动 (PipelineStage + Usage) | ⏳ Phase 2 (Vulkan 时实现) |
 *   | 子资源追踪 | ✅ TextureSlice 链表 | ⏳ Phase 2 (整资源级别) |
 *   | Compute List | ✅ 支持 | ❌ HUD 不需要 |
 *   | Timestamp Query | ✅ 支持 | ❌ 简化 |
 *   | Debug Labels | ✅ 支持 | ❌ 后续按需 |
 *   | 命令复杂度 | 高（图编译） | 低（线性回放 + 追踪） |
 *
 * @see IRenderingDeviceDriver  设备驱动接口（CommandBufferCreate/Free）
 * @see GLCommandBuffer         OpenGL 命令缓冲实现
 * @see ResourceTracker         轻量级资源追踪器
 * @see rendering_device_driver.h 设备驱动完整接口
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-09
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#pragma once

#include "rendering_device_commons.h"
#include "resource_tracker.h"
#include "template/local_vector.h"
#include "template/fixed_vector.h"

namespace arhud
{

    class IRenderingContextDriver;

    // ═══════════════════════════════════════════════════════════════════════
    // 命令类型枚举
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 渲染命令类型枚举
     *
     * 标识已录制命令的具体类型。每种类型对应一个扁平结构体（CmdXxx），
     * 通过 RecordedCommand::type 字段区分联合体中的实际数据。
     *
     * @par 值分配规则
     *   - 从 0 开始紧凑排列，便于 switch-case 分发
     *   - 按 RenderPass → 绑定 → 绘制 → 状态设置 → 数据传输 顺序分组
     *   - kMax 作为哨兵值，表示无效类型
     *
     * @par 可扩展性
     *   新增命令类型时：
     *   1. 在此枚举中添加新值（kMax 之前）
     *   2. 定义对应的 CmdXxx 结构体
     *   3. 在 RecordedCommand::Data 联合体中添加成员
     *   4. 在 ICommandBuffer 中添加录制方法
     *   5. 在 GLCommandBuffer::ReplayCommand() 中添加 case 分支
     */
    enum class CommandType : uint8_t
    {
        kBeginRenderPass = 0,      ///< 开始渲染 Pass
        kEndRenderPass,            ///< 结束渲染 Pass
        kBindPipeline,             ///< 绑定渲染管线（含 PSO 状态）
        kBindUniformSet,           ///< 绑定 Uniform 描述符集
        kBindVertexBuffers,        ///< 绑定顶点缓冲区数组
        kBindIndexBuffer,          ///< 绑定索引缓冲区
        kDraw,                     ///< 非索引绘制调用
        kDrawIndexed,              ///< 索引绘制调用
        kSetViewport,              ///< 设置视口变换区域
        kSetScissor,               ///< 设置像素裁剪矩形
        kSetBlendConstants,        ///< 设置混合常量颜色 (RGBA)
        kClearBuffer,              ///< 清除缓冲区内容（填充 0）
        kCopyBuffer,               ///< 缓冲区间内存拷贝
        kClearColorTexture,        ///< 清除颜色纹理（填充指定颜色）
        kClearDepthStencilTexture, ///< 清除深度/模板纹理
        kMax,                      ///< 哨兵值：最大类型数 + 1
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 命令数据结构（扁平化设计，无虚函数/堆分配）
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 开始渲染 Pass 命令数据
     *
     * 记录 BeginRenderPass 调用的所有参数。
     * 对应 OpenGL 操作：glBindFramebuffer + glClear + glViewport。
     *
     * @note clear_values 使用 FixedVector 存储以避免 union 拷贝问题；
     *       最大支持 4 个附件清除值（Color0-3 + DepthStencil）。
     */
    struct CmdBeginRenderPass
    {
        static constexpr uint32_t kMaxClearValues = 4; ///< 最大清除值数量

        RenderPassID render_pass;                                        ///< 渲染 Pass ID（定义附件格式/操作）
        FramebufferID framebuffer;                                       ///< 帧缓冲 ID（绑定具体纹理附件）
        FixedVector<RenderPassClearValue, kMaxClearValues> clear_values; ///< 每个附件的清除值
        int32_t rect_x = 0;                                              ///< 渲染区域左下角 X（像素）
        int32_t rect_y = 0;                                              ///< 渲染区域左下角 Y（像素）
        uint32_t rect_w = 0;                                             ///< 渲染区域宽度（像素）
        uint32_t rect_h = 0;                                             ///< 渲染区域高度（像素）
    };

    /**
     * @brief 绑定渲染管线命令数据
     *
     * 触发 PSO 状态应用到 GL 状态机：
     * glUseProgram + glEnable/glDisable + glBlendFunc 等。
     */
    struct CmdBindPipeline
    {
        PipelineID pipeline; ///< 要绑定的管线 ID
    };

    /**
     * @brief 绑定 Uniform Set 命令数据
     *
     * 将 Uniform Set 中的 Uniform Buffer / Sampler 绑定到对应的 GL 绑定点。
     *
     * @par OpenGL 映射
     *   set_index=0 → UBO binding point 0~N
     *   set_index=1 → Texture unit 0~N
     */
    struct CmdBindUniformSet
    {
        UniformSetID uniform_set; ///< Uniform Set ID
        uint32_t set_index = 0;   ///< Set 索引（对应 binding layout(set = N)）
    };

    /**
     * @brief 绑定顶点缓冲区命令数据
     *
     * 同时绑定多个顶点缓冲区及其偏移量。
     * 对应 OpenGL：glBindVertexBuffer(i, buffer, offset, stride)。
     *
     * @note 最大支持 kMaxBindings 个同时绑定（HUD 场景通常 ≤ 2）。
     */
    struct CmdBindVertexBuffers
    {
        static constexpr uint32_t kMaxBindings = 4; ///< 最大同时绑定数量

        BufferID buffers[kMaxBindings] = {}; ///< 顶点缓冲区 ID 数组
        uint64_t offsets[kMaxBindings] = {}; ///< 每个缓冲区的字节偏移
        uint32_t count = 0;                  ///< 实际绑定数量
    };

    /**
     * @brief 绑定索引缓冲区命令数据
     *
     * 对应 OpenGL：glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ...)。
     *
     * @note format 决定索引元素的字节宽度和类型：
     *       kUint16 → GL_UNSIGNED_SHORT（2 字节）
     *       kUint32 → GL_UNSIGNED_INT（4 字节）
     */
    struct CmdBindIndexBuffer
    {
        BufferID buffer;                                       ///< 索引缓冲区 ID
        IndexBufferFormat format = IndexBufferFormat::kUint16; ///< 索引格式
        uint64_t offset = 0;                                   ///< 首个索引的字节偏移
    };

    /**
     * @brief 非索引绘制命令数据
     *
     * 对应 OpenGL：glDrawArraysInstancedBaseInstance(mode, first, count, instance_count, base_instance)。
     *
     * @par 参数说明
     *   - vertex_count：从 vertex_offset 开始连续读取的顶点数
     *   - instance_count：实例化绘制次数（1 = 非实例化）
     *   - base_vertex：加到每个顶点索引上的偏移（对 Draw 无效，保留兼容性）
     *   - first_instance：gl_InstanceID 的起始值
     */
    struct CmdDraw
    {
        uint32_t vertex_count = 0;   ///< 顶点数量
        uint32_t instance_count = 1; ///< 实例化数量（默认 1 = 单次绘制）
        uint32_t base_vertex = 0;    ///< 基础顶点偏移
        uint32_t first_instance = 0; ///< 首实例 ID
    };

    /**
     * @brief 索引绘制命令数据
     *
     * 对应 OpenGL：glDrawElementsInstancedBaseVertexBaseInstance(...)。
     *
     * @par 与 CmdDraw 的区别
     *   使用索引缓冲区中的间接寻址而非连续顶点流，
     *   支持非连续顶点和顶点重用（节省带宽）。
     */
    struct CmdDrawIndexed
    {
        uint32_t index_count = 0;    ///< 索引数量
        uint32_t instance_count = 1; ///< 实例化数量
        uint32_t first_index = 0;    ///< 首索引位置（元素数，非字节数）
        int32_t vertex_offset = 0;   ///< 顶点偏移（加到索引值上）
        uint32_t first_instance = 0; ///< 首实例 ID
    };

    /**
     * @brief 设置视口命令数据
     *
     * 对应 OpenGL：glViewport(x, y, width, height)。
     *
     * @note 坐标系为屏幕空间，原点在左下角（OpenGL 惯例）。
     */
    struct CmdSetViewport
    {
        int32_t x = 0;       ///< 视口左下角 X
        int32_t y = 0;       ///< 视口左下角 Y
        uint32_t width = 0;  ///< 视口宽度
        uint32_t height = 0; ///< 视口高度
    };

    /**
     * @brief 设置裁剪矩形命令数据
     *
     * 对应 OpenGL：glScissor(x, y, width, height)。
     *
     * @note 需要先 glEnable(GL_SCISSOR_TEST) 才生效（通常在管线创建时启用）。
     */
    struct CmdSetScissor
    {
        int32_t x = 0;       ///< 裁剪矩形左下角 X
        int32_t y = 0;       ///< 裁剪矩形左下角 Y
        uint32_t width = 0;  ///< 裁剪矩形宽度
        uint32_t height = 0; ///< 裁剪矩形高度
    };

    /**
     * @brief 设置混合常量命令数据
     *
     * 对应 OpenGL：glBlendColor(r, g, b, a)。
     *
     * @note 仅当管线启用了 CONSTANT_COLOR / CONSTANT_ALPHA 混合因子时有效。
     */
    struct CmdSetBlendConstants
    {
        float r = 0.0f; ///< 红色通道常量 [0, 1]
        float g = 0.0f; ///< 绿色通道常量 [0, 1]
        float b = 0.0f; ///< 蓝色通道常量 [0, 1]
        float a = 0.0f; ///< Alpha 通道常量 [0, 1]
    };

    /**
     * @brief 清除缓冲区命令数据
     *
     * 将指定范围的缓冲区内存填充为 0。
     * 对应 OpenGL：glClearBufferData 或 glClearBufferSubData。
     *
     * @warning 此操作是同步的，可能造成 GPU Stall。
     */
    struct CmdClearBuffer
    {
        BufferID buffer;     ///< 目标缓冲区 ID
        uint64_t offset = 0; ///< 清除起始偏移（字节）
        uint64_t size = 0;   ///< 清除大小（字节），0 表示全部
    };

    /**
     * @brief 复制缓冲区命令数据
     *
     * 将源缓冲区的指定区域拷贝到目标缓冲区。
     * 对应 OpenGL：glCopyBufferSubData（多次调用）。
     *
     * @note regions 支持一次提交多个拷贝区域，减少 API 调用次数。
     */
    struct CmdCopyBuffer
    {
        static constexpr uint32_t kMaxRegions = 8; ///< 最大拷贝区域数

        BufferID src_buffer;                                ///< 源缓冲区 ID
        BufferID dst_buffer;                                ///< 目标缓冲区 ID
        FixedVector<BufferCopyRegion, kMaxRegions> regions; ///< 拷贝区域列表
    };

    /**
     * @brief 清除颜色纹理命令数据
     *
     * 将纹理的指定子资源范围填充为指定颜色。
     * 对应 OpenGL：glClearTexSubImage。
     *
     * @param subresources 指定要清除的 Mip 层和 Array 层范围
     */
    struct CmdClearColorTexture
    {
        TextureID texture;                    ///< 目标纹理 ID
        float color_r = 0.0f;                 ///< 清除颜色 R 通道
        float color_g = 0.0f;                 ///< 清除颜色 G 通道
        float color_b = 0.0f;                 ///< 清除颜色 B 通道
        float color_a = 0.0f;                 ///< 清除颜色 A 通道
        TextureSubresourceRange subresources; ///< 子资源范围
    };

    /**
     * @brief 清除深度/模板纹理命令数据
     *
     * 将深度/模板纹理的指定子资源范围填充为指定值。
     * 对应 OpenGL：glClearTexSubImage（depth_stencil 格式）。
     *
     * @note depth 和 stencil 同时写入（GL 不支持单独清除其中一个分量）。
     */
    struct CmdClearDepthStencilTexture
    {
        TextureID texture;                    ///< 目标纹理 ID
        float depth = 0.0f;                   ///< 清除深度值 [0, 1]
        uint32_t stencil = 0;                 ///< 清除模板值 [0, 255]
        TextureSubresourceRange subresources; ///< 子资源范围
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 已录制命令条目（类型标签 + 数据联合体）
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 已录制命令条目
     *
     * 命令缓冲区的基本存储单元。由两部分组成：
     * - **type**：命令类型标签（CommandType 枚举），用于 switch-case 分发
     * - **data**：命令参数联合体，根据 type 解释为不同的 CmdXxx 结构体
     *
     * @par 内存布局
     *   @code
     *   ┌─────────────┬────────────────────────────────────┐
     *   │ type (1B)   │ padding (7B)                       │
     *   ├─────────────┼────────────────────────────────────┤
     *   │ data (union)│ CmdBeginRenderPass (~120B max)     │
     *   │             │ CmdBindPipeline (8B)               │
     *   │             │ CmdDraw (16B)                      │
     *   │             │ ...                                │
     *   └─────────────┴────────────────────────────────────┘
     *   总大小 = sizeof(largest CmdXxx) + alignment padding
     *   @endcode
     *
     * @par 设计决策
     *   - **不使用虚函数/多态**：避免 vtable 指针开销（8 bytes/对象）
     *   - **不使用 std::variant**：避免异常相关元数据和堆分配
     *   - **联合体 + placement new**：手动构造/析构正确成员
     *   - **PagedAllocator 存储**：对象从 PagedAllocator 分配，
     *     命令缓冲通过指针数组记录顺序（避免 union 不可复制问题）
     *
     * @warning 联合体成员的生命周期必须由外部显式管理：
     *   - Emplace<TCmd>() 时 placement new 构造
     *   - Reset()/Begin() 时显式调用析构函数后 Free()
     */
    struct RecordedCommand
    {
        CommandType type = CommandType::kMax; ///< 命令类型标签

        /**
         * @brief 命令数据联合体
         *
         * 根据 type 字段的值解释为不同的命令结构体。
         * 所有成员共享同一块内存，大小等于最大成员的大小。
         */
        union Data
        {
            CmdBeginRenderPass begin_render_pass;                    ///< kBeginRenderPass 数据
            CmdBindPipeline bind_pipeline;                           ///< kBindPipeline 数据
            CmdBindUniformSet bind_uniform_set;                      ///< kBindUniformSet 数据
            CmdBindVertexBuffers bind_vertex_buffers;                ///< kBindVertexBuffers 数据
            CmdBindIndexBuffer bind_index_buffer;                    ///< kBindIndexBuffer 数据
            CmdDraw draw;                                            ///< kDraw 数据
            CmdDrawIndexed draw_indexed;                             ///< kDrawIndexed 数据
            CmdSetViewport set_viewport;                             ///< kSetViewport 数据
            CmdSetScissor set_scissor;                               ///< kSetScissor 数据
            CmdSetBlendConstants set_blend_constants;                ///< kSetBlendConstants 数据
            CmdClearBuffer clear_buffer;                             ///< kClearBuffer 数据
            CmdCopyBuffer copy_buffer;                               ///< kCopyBuffer 数据
            CmdClearColorTexture clear_color_texture;                ///< kClearColorTexture 数据
            CmdClearDepthStencilTexture clear_depth_stencil_texture; ///< kClearDepthStencilTexture 数据

            Data() {}  ///< 默认构造（不初始化任何成员）
            ~Data() {} ///< 默认析构（不销毁任何成员，由外部管理）
        } data;        ///< 命令参数数据
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 命令缓冲状态枚举
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 命令缓冲生命周期状态
     *
     * 状态转换图：
     *   @code
     *   ┌──────────┐  Begin()   ┌───────────┐  End()   ┌────────┐
     *   │ Initial  │ ─────────→ │ Recording │ ───────→│ Ready  │
     *   │(or Reset)│            │ (可录制)  │          │(就绪)  │
     *   └──────────┘            └───────────┘          └───┬────┘
     *       ▲                                              │
     *       │           Execute()                           │ Reset()
     *       │         ┌────────▼────────┐                   │
     *       └─────────│ Executing        │←──────────────────┘
     *       Reset()   │ (正在回放)       │
     *                 └────────┬────────┘
     *                          │ 回放完成
     *                     ┌────▼─────┐
     *                     │ Submitted │
     *                     │ (已提交)  │
     *                     └───────────┘
     *   @endcode
     *
     * @par 各状态下允许的操作
     *   | 操作 | Recording | Ready | Executing | Submitted |
     *   |------|:---------:|:-----:|:---------:|:---------:|
     *   | BindXxx / Draw | ✅ | ❌ | ❌ | ❌ |
     *   | End() | ✅ | ❌ | ❌ | ❌ |
     *   | Execute() | ❌ | ✅ | ❌ | ❌ |
     *   | Reset() | ✅ | ✅ | ❌ | ✅ |
     *   | Begin() | ✅ | ✅ | ❌ | ✅ |
     */
    enum class CommandBufferState : uint8_t
    {
        kRecording, ///< 录制中：接受新的录制请求
        kReady,     ///< 就绪：录制完成，等待 Execute()
        kExecuting, ///< 回放中：正在逐条执行命令
        kSubmitted, ///< 已提交：Execute 完成，等待 Reset
    };

    // ═══════════════════════════════════════════════════════════════════════
    // ICommandBuffer 接口
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 命令缓冲抽象接口
     *
     * 定义渲染命令的录制和回放契约。实现类负责：
     * - 命令数据的底层存储策略
     * - 录制操作的线程安全保证
     * - 回放操作到具体图形 API 的映射
     *
     * @par 典型使用流程（双缓冲模式）
     *   @code
     *   // === 主线程（逻辑更新）===
     *   ICommandBuffer *cmd = driver->CommandBufferCreate();
     *
     *   cmd->Begin();                                    // 进入录制模式
     *   cmd->BeginRenderPass(rp, fb, clears, 0, 0, w, h);
     *   cmd->BindPipeline(pipeline);
     *   cmd->BindUniformSet(uniform_set, 0);
     *   cmd->BindVertexBuffers(vbos, offsets, 1);
     *   cmd->Draw(vertex_count, 1, 0, 0);              // 录制绘制命令
     *   cmd->EndRenderPass();
     *   cmd->End();                                      // 结束录制
     *
     *   render_queue->Submit(cmd);                       // 传递给渲染线程
     *
     *   // === 渲染线程（GL 上下文已 MakeCurrent）===
     *   while (ICommandBuffer *cmd = render_queue->Dequeue())
     *   {
     *       cmd->Execute();                              // 回放到 GL
     *       cmd->Reset();                                // 重置以便复用
     *   }
     *   @endcode
     *
     * @par 线程安全契约
     *   | 方法类别 | 线程安全 | 加锁方式 | 要求 |
     *   |----------|:--------:|----------|------|
     *   | Begin / End / BindXxx / Draw 等 | ✅ 线程安全 | 内部 Mutex | 任意线程 |
     *   | Execute | ⚠️ 需外部同步 | 无锁 | 必须在 GL 上下文线程 |
     *   | GetState / GetCommandCount | ✅ 线程安全 | 原子读或 Mutex | 任意线程 |
     *   | Reset | ✅ 线程安全 | 内部 Mutex | 不能在 Execute 期间调用 |
     *
     * @par 内存所有权
     *   - ICommandBuffer 对象由 IRenderingDeviceDriver::CommandBufferCreate() 创建
     *   - 必须通过 IRenderingDeviceDriver::CommandBufferFree() 释放
     *   - 内部命令对象的内存由 PagedAllocator 管理，随 CommandBuffer 销毁而释放
     *
     * @see GLCommandBuffer           OpenGL 实现
     * @see IRenderingDeviceDriver    创建/释放入口
     */
    class ICommandBuffer
    {
    public:
        virtual ~ICommandBuffer() = default;

        // ═══════════════════════════════════════════════════════════════════
        // 生命周期管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 开始命令录制
         *
         * 重置命令缓冲区状态，释放上一轮录制的所有命令对象，
         * 进入 Recording 状态。此后可调用录制方法添加命令。
         *
         * @par 内部行为
         *   1. 获取 Mutex 锁
         *   2. 遍历 commands_ 数组，逐一调用 PagedAllocator::Free() 释放命令对象
         *   3. 清空 commands_ 指针数组
         *   4. 设置 state_ = kRecording
         *   5. 释放 Mutex 锁
         *
         * @note 可在任何非 Executing 状态下调用（包括 Ready、Submitted）。
         *       如果当前有未执行的命令，它们将被丢弃。
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void Begin() = 0;

        /**
         * @brief 结束命令录制
         *
         * 将状态从 Recording 切换为 Ready。
         * 此后不再接受任何录制方法的调用（调用会被忽略）。
         *
         * @pre state_ == kRecording（否则操作被忽略）
         *
         * @post state_ == kReady，可以调用 Execute()
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void End() = 0;

        /**
         * @brief 回放所有已录制命令到图形 API
         *
         * 按录制顺序遍历 commands_ 数组，将每条命令转换为
         * 对应的 GL API 调用（通过 driver_->CommandXxx() 方法）。
         *
         * @par 内部行为
         *   1. 检查 state_ == kReady，否则直接返回
         *   2. 设置 state_ = kExecuting
         *   3. 无锁遍历 commands_[i]，对每条命令调用 ReplayCommand()
         *   4. ReplayCommand 内部根据 type 做 switch-case 分发到 driver_
         *   5. 设置 state_ = kSubmitted
         *
         * @pre End() 已成功调用（state_ == kReady）
         * @pre 当前线程拥有有效的 GL 上下文（MakeCurrent）
         *
         * @warning 此方法**不加锁**！必须在 GL 上下文线程调用，
         *          且确保没有其他线程同时在录制。
         *
         * @thread_safety 非线程安全（必须在 GL 线程独占调用）
         */
        virtual void Execute() = 0;

        /**
         * @brief 重置命令缓冲区
         *
         * 释放所有已录制命令对象，清空命令列表，
         * 回到初始 Recording 状态。用于 CommandBuffer 对象的跨帧复用。
         *
         * @par 与 Begin() 的区别
         *   - Begin()：开始新一轮录制（通常在帧首调用）
         *   - Reset()：强制清理（通常在 Execute 后或错误恢复时调用）
         *   - 两者内部行为相同，语义上 Begin 更强调"开始"，Reset 更强调"清理"
         *
         * @pre state_ != kExecuting（不能在回放中途重置）
         *
         * @post state_ == kRecording，commands_ 为空
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void Reset() = 0;

        /**
         * @brief 获取命令缓冲区当前状态
         *
         * 用于外部检查是否可以进行下一步操作。
         *
         * @return CommandBufferState 当前状态值
         *
         * @thread_safety 线程安全（原子读或 Mutex 保护）
         */
        virtual CommandBufferState GetState() const = 0;

        /**
         * @brief 获取已录制的命令数量
         *
         * 用于性能监控和调试。
         *
         * @return uint32_t 当前命令列表中的命令条数
         *
         * @note 返回值仅在持有 lock_ 或处于 Ready/Submitted 状态时稳定。
         *
         * @thread_safety 线程安全
         */
        virtual uint32_t GetCommandCount() const = 0;

        // ═══════════════════════════════════════════════════════════════════
        // Render Pass 命令
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 开始渲染 Pass
         *
         * 录制 BeginRenderPass 命令。绑定帧缓冲、执行清除操作、设置视口。
         *
         * @param[in] p_render_pass   渲染 Pass ID（定义附件格式和 Load/Store 操作）
         * @param[in] p_framebuffer   帧缓冲 ID（绑定具体的颜色/深度/模板纹理）
         * @param[in] p_clear_values  每个附件的初始清除值数组
         * @param[in] p_rect_x       渲染区域左下角 X 坐标（像素）
         * @param[in] p_rect_y       渲染区域左下角 Y 坐标（像素）
         * @param[in] p_rect_w       渲染区域宽度（像素）
         * @param[in] p_rect_h       渲染区域高度（像素）
         *
         * @pre state_ == kRecording
         *
         * @par OpenGL 等效操作
         *   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         *   glClearBufferfv(...);  // 对每个附件
         *   glViewport(x, y, w, h);
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void BeginRenderPass(
            RenderPassID p_render_pass,
            FramebufferID p_framebuffer,
            VectorView<RenderPassClearValue> p_clear_values,
            int32_t p_rect_x, int32_t p_rect_y,
            uint32_t p_rect_w, uint32_t p_rect_h) = 0;

        /**
         * @brief 结束渲染 Pass
         *
         * 录制 EndRenderPass 命令。标记当前渲染 Pass 的结束。
         *
         * @pre state_ == kRecording
         * @pre 已匹配的 BeginRenderPass 调用
         *
         * @par OpenGL 等效操作
         *   （无显式操作，Pass 边界用于状态验证和调试）
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void EndRenderPass() = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 资源绑定命令
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 绑定渲染管线
         *
         * 录制 BindPipeline 命令。激活指定的渲染管线状态
         * （着色器程序、混合模式、深度测试、Cull 模式等）。
         *
         * @param[in] p_pipeline  由 PipelineCreate() 创建的管线 ID
         *
         * @pre state_ == kRecording
         * @pre p_pipeline 有效（由本驱动创建且未释放）
         *
         * @par OpenGL 等效操作
         *   glUseProgram(program);
         *   // 应用缓存的 PSO 状态到 GL 状态机...
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void BindRenderPipeline(PipelineID p_pipeline) = 0;

        /**
         * @brief 绑定 Uniform Set
         *
         * 录制 BindUniformSet 命令。将描述符集中的 Uniform Buffer
         * 和采样器绑定到 GL 对应的绑定点。
         *
         * @param[in] p_uniform_set  Uniform Set ID
         * @param[in] p_set_index    Set 索引（对应 shader 中的 layout(set = N)）
         *
         * @pre state_ == kRecording
         *
         * @par OpenGL 等效操作
         *   glBindBufferBase(GL_UNIFORM_BUFFER, binding, ubo);
         *   glActiveTexture(unit); glBindTexture(target, tex);
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void BindUniformSet(UniformSetID p_uniform_set,
                                    uint32_t p_set_index) = 0;

        /**
         * @brief 绑定顶点缓冲区数组
         *
         * 录制 BindVertexBuffers 命令。同时绑定多个顶点缓冲区
         * 及其起始偏移到 VAO 的各个绑定点上。
         *
         * @param[in] p_buffers  顶点缓冲区 ID 数组指针
         * @param[in] p_offsets 每个缓冲区的字节偏移数组指针
         * @param[in] p_count   要绑定的缓冲区数量（≤ kMaxBindings）
         *
         * @pre state_ == kRecording
         * @pre p_buffers != nullptr && p_offsets != nullptr
         * @pre p_count > 0 && p_count <= kMaxBindings
         *
         * @par OpenGL 等效操作
         *   for (i = 0..count)
         *       glBindVertexBuffer(i, buffers[i], offsets[i], strides[i]);
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void BindVertexBuffers(
            const BufferID *p_buffers,
            const uint64_t *p_offsets,
            uint32_t p_count) = 0;

        /**
         * @brief 绑定索引缓冲区
         *
         * 录制 BindIndexBuffer 命令。绑定用于索引绘制的索引缓冲区。
         *
         * @param[in] p_buffer  索引缓冲区 ID
         * @param[in] p_format  索引格式（kUint16 或 kUint32）
         * @param[in] p_offset 首个索引的字节偏移
         *
         * @pre state_ == kRecording
         *
         * @par OpenGL 等效操作
         *   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void BindIndexBuffer(BufferID p_buffer,
                                     IndexBufferFormat p_format,
                                     uint64_t p_offset) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 绘制命令
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 非索引绘制
         *
         * 录制 Draw 命令。按顶点缓冲区中的顺序连续读取顶点进行绘制。
         *
         * @param[in] p_vertex_count    要绘制的顶点数量
         * @param[in] p_instance_count  实例化绘制次数（1 = 非实例化）
         * @param[in] p_base_vertex     顶点基础偏移（加到 VertexID 上）
         * @param[in] p_first_instance  首实例 ID（glInstanceID 的起始值）
         *
         * @pre state_ == kRecording
         * @pre p_vertex_count > 0
         * @pre 已绑定有效的管线、顶点缓冲区和 VAO
         *
         * @par OpenGL 等效操作
         *   glDrawArraysInstancedBaseInstance(mode, first, count, primcount, baseinst)
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void Draw(uint32_t p_vertex_count,
                          uint32_t p_instance_count,
                          uint32_t p_base_vertex,
                          uint32_t p_first_instance) = 0;

        /**
         * @brief 索引绘制
         *
         * 录制 DrawIndexed 命令。通过索引缓冲区间接寻址顶点进行绘制。
         *
         * @param[in] p_index_count     索引数量
         * @param[in] p_instance_count  实例化绘制次数（1 = 非实例化）
         * @param[in] p_first_index     首索引的位置（元素数，非字节数）
         * @param[in] p_vertex_offset   顶点偏移（加到每个索引值上）
         * @param[in] p_first_instance  首实例 ID
         *
         * @pre state_ == kRecording
         * @pre p_index_count > 0
         * @pre 已绑定有效的管线、顶点缓冲区、VAO 和索引缓冲区
         *
         * @par OpenGL 等效操作
         *   glDrawElementsInstancedBaseVertexBaseInstance(mode, count, type,
         *       indices, primcount, basevertex, baseinstance)
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void DrawIndexed(uint32_t p_index_count,
                                 uint32_t p_instance_count,
                                 uint32_t p_first_index,
                                 int32_t p_vertex_offset,
                                 uint32_t p_first_instance) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 动态状态设置命令
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 设置视口
         *
         * 录制 SetViewport 命令。定义 NDC 到屏幕空间的变换区域。
         *
         * @param[in] p_x      视口左下角 X 坐标（像素）
         * @param[in] p_y      视口左下角 Y 坐标（像素）
         * @param[in] p_width  视口宽度（像素）
         * @param[in] p_height 视口高度（像素）
         *
         * @pre state_ == kRecording
         *
         * @par OpenGL 等效操作
         *   glViewport(x, y, width, height)
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void SetViewport(int32_t p_x, int32_t p_y,
                                 uint32_t p_width, uint32_t p_height) = 0;

        /**
         * @brief 设置裁剪矩形
         *
         * 录制 SetScissor 命令。限制光栅化的像素区域。
         *
         * @param[in] p_x      裁剪矩形左下角 X（像素）
         * @param[in] p_y      裁剪矩形左下角 Y（像素）
         * @param[in] p_width  裁剪矩形宽度（像素）
         * @param[in] p_height 裁剪矩形高度（像素）
         *
         * @pre state_ == kRecording
         * @pre 当前管线启用了 GL_SCISSOR_TEST
         *
         * @par OpenGL 等效操作
         *   glScissor(x, y, width, height)
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void SetScissor(int32_t p_x, int32_t p_y,
                                uint32_t p_width, uint32_t p_height) = 0;

        /**
         * @brief 设置混合常量颜色
         *
         * 录制 SetBlendConstants 命令。设置常量混合因子使用的 RGBA 颜色值。
         *
         * @param[in] p_r  红色通道 [0.0, 1.0]
         * @param[in] p_g  绿色通道 [0.0, 1.0]
         * @param[in] p_b  蓝色通道 [0.0, 1.0]
         * @param[in] p_a  Alpha 通道 [0.0, 1.0]
         *
         * @pre state_ == kRecording
         *
         * @par OpenGL 等效操作
         *   glBlendColor(r, g, b, a)
         *
         * @note 仅当管线使用了 BLEND_CONSTANT_COLOR / BLEND_CONSTANT_ALPHA
         *       作为 SrcFactor 或 DstFactor 时才有效果。
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void SetBlendConstants(float p_r, float p_g,
                                       float p_b, float p_a) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 数据传输命令
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 清除缓冲区内容
         *
         * 录制 ClearBuffer 命令。将缓冲区指定范围填充为零。
         *
         * @param[in] p_buffer 目标缓冲区 ID
         * @param[in] p_offset 清除起始字节偏移
         * @param[in] p_size   清除字节数（0 = 全部）
         *
         * @pre state_ == kRecording
         * @pre p_buffer 有效
         *
         * @warning 此操作可能导致 GPU-CPU 同步等待（GPU Stall）。
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void ClearBuffer(BufferID p_buffer,
                                 uint64_t p_offset,
                                 uint64_t p_size) = 0;

        /**
         * @brief 缓冲区间内存拷贝
         *
         * 录制 CopyBuffer 命令。将源缓冲区的数据拷贝到目标缓冲区。
         *
         * @param[in] p_src_buffer 源缓冲区 ID
         * @param[in] p_dst_buffer 目标缓冲区 ID
         * @param[in] p_regions    拷贝区域数组（src_offset → dst_offset × size）
         *
         * @pre state_ == kRecording
         * @pre p_src_buffer 和 p_dst_buffer 有效
         *
         * @par OpenGL 等效操作
         *   for each region:
         *       glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
         *           read_offset, write_offset, size)
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void CopyBuffer(BufferID p_src_buffer,
                                BufferID p_dst_buffer,
                                VectorView<BufferCopyRegion> p_regions) = 0;

        /**
         * @brief 清除颜色纹理
         *
         * 录制 ClearColorTexture 命令。将颜色纹理的指定子资源填充为指定颜色。
         *
         * @param[in] p_texture      目标纹理 ID
         * @param[in] p_color_r      清除颜色 R 通道 [0.0, 1.0]
         * @param[in] p_color_g      清除颜色 G 通道 [0.0, 1.0]
         * @param[in] p_color_b      清除颜色 B 通道 [0.0, 1.0]
         * @param[in] p_color_a      清除颜色 A 通道 [0.0, 1.0]
         * @param[in] p_subresources 子资源范围（Mip 层 / Array 层）
         *
         * @pre state_ == kRecording
         * @pre p_texture 有效且为颜色格式纹理
         *
         * @par OpenGL 等效操作
         *   glClearTexSubImage(texture, level, x, y, z, w, h, d,
         *       format, type, &color)
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void ClearColorTexture(
            TextureID p_texture,
            float p_color_r, float p_color_g,
            float p_color_b, float p_color_a,
            const TextureSubresourceRange &p_subresources) = 0;

        /**
         * @brief 清除深度/模板纹理
         *
         * 录制 ClearDepthStencilTexture 命令。将深度/模板纹理的
         * 指定子资源填充为指定深度值和模板值。
         *
         * @param[in] p_texture      目标纹理 ID
         * @param[in] p_depth        清除深度值 [0.0, 1.0]
         * @param[in] p_stencil      清除模板值 [0, 255]
         * @param[in] p_subresources 子资源范围（Mip 层 / Array 层）
         *
         * @pre state_ == kRecording
         * @pre p_texture 有效且为深度/深度-模板格式纹理
         *
         * @par OpenGL 等效操作
         *   glClearTexSubImage(texture, level, x, y, z, w, h, d,
         *       GL_DEPTH_STENCIL, GL_FLOAT_24_UNSIGNED_INT_24_8_REV, &value)
         *
         * @thread_safety 线程安全（内部 Mutex）
         */
        virtual void ClearDepthStencilTexture(
            TextureID p_texture,
            float p_depth, uint32_t p_stencil,
            const TextureSubresourceRange &p_subresources) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 资源追踪查询接口
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 获取资源追踪器（可选接口）
         *
         * 返回内部 ResourceTracker 的常量指针，用于外部查询资源使用情况。
         *
         * @return 指向 ResourceTracker 的常量指针，如果实现不支持追踪则返回 nullptr
         *
         * @par 典型用途
         *   - 调试时检查资源使用状态：`cmd->GetResourceTracker()->DumpState()`
         *   - 性能分析时统计活跃资源数
         *   - 单元测试中验证命令录制是否正确注册了资源
         *
         * @note 默认返回 nullptr。GLCommandBuffer 会重写此方法返回实际 tracker。
         * @note 返回的指针在 CommandBuffer 生命周期内有效。
         *
         * @thread_safety 线程安全（返回只读指针）
         */
        virtual const ResourceTracker *GetResourceTracker() const { return nullptr; }
    };

} // namespace arhud
