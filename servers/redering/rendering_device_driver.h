/**
 * @file rendering_device_driver.h
 * @brief 渲染设备驱动抽象接口
 *
 * IRenderingDeviceDriver 是 GPU 资源操作的直接封装层，位于渲染架构的驱动层。
 * 所有 GPU 资源（Buffer/Texture/Sampler/Shader/Pipeline/Framebuffer/CommandBuffer）
 * 的创建、绑定和操作都通过此接口完成。
 *
 * @par 架构定位
 *   @code
 *   ┌─────────────────────────────────────────────────────┐
 *   │                  RenderingServer                     │  ← 业务层
 *   ├─────────────────────────────────────────────────────┤
 *   │                  RenderingDevice (RD)                │  ← 设备层
 *   ├─────────────────────────────────────────────────────┤
 *   │        IRenderingDeviceDriver (RDD)  ← 本文件       │  ← 驱动层
 *   ├─────────────────────────────────────────────────────┤
 *   │          IRenderingContextDriver (RCD)               │  ← 上下文层
 *   └─────────────────────────────────────────────────────┘
 *   @endcode
 *
 * @par 设计原则（来自 Godot 源码注释）
 *   1. 极少验证，仅在 Debug 构建中做
 *   2. 错误报告简单：返回 id=0 或 false
 *   3. 枚举/常量/结构体尽量对齐 Vulkan 值，使 Vulkan 驱动可直接 assert 兼容
 *   4. 热路径尽量零分配，使用 alloca()
 *   5. 使用 PagedAllocator 管理簿记结构
 *   6. 使用 VectorView 传递数组参数，避免拷贝
 *   7. 如果驱动需要高层信息，应存储所需数据的副本，
 *      不存在从驱动到 RenderingDevice 的反向查询
 *
 * @par 与 Godot 的差异
 *   - 去掉 Godot Object/GDSOFTCLASS 继承，纯 C++ 虚接口
 *   - 去掉 RenderingShaderContainer 依赖（简化为直接 GLSL 源码编译）
 *   - 去掉 CommandQueue/CommandPool（使用 ICommandBuffer 统一抽象）
 *   - 去掉 Compute Pipeline（HUD 场景暂不需要）
 *   - 去掉 Pipeline Cache（OpenGL 无 PSO 缓存概念）
 *   - 去掉 Timestamp Query（简化，后续可按需添加）
 *   - 去掉 Debug Labels/Breadcrumbs（简化，后续可按需添加）
 *   - 命名遵循 ARHud PascalCase + k 前缀约定
 *
 * @par 命令模式
 *   支持两种命令提交方式：
 *   1. **CommandBuffer 模式**（推荐）：通过 CommandBufferCreate() 创建命令缓冲，
 *      在任意线程录制命令，在渲染线程回放执行。支持主线程录制+渲染线程回放。
 *   2. **立即模式**（向后兼容）：直接调用 CommandXxx() 方法，
 *      命令立即执行到 GL API。适用于简单场景或调试。
 *
 * @par OpenGL 特殊策略
 *   OpenGL 没有 Pipeline State Object (PSO)，需要模拟：
 *   - RenderPipelineCreate() 记录光栅化/深度/混合状态到结构体
 *   - CommandBindRenderPipeline() 时逐个设置 GL 状态
 *   - GLStateCache 避免冗余 GL 状态切换调用
 *
 * @par 生命周期
 *   1. 通过 IRenderingContextDriver::CreateDeviceDriver() 创建
 *   2. Initialize() — 初始化驱动（查询 GL 限制等）
 *   3. 资源创建/使用/销毁
 *   4. Shutdown() — 释放所有资源
 *
 * @par 设计参考
 *   - Godot 4.6 RenderingDeviceDriver（rendering_device_driver.h）
 *   - arch_skill.md §1.1 三层 GPU API 抽象
 *   - arch_skill.md §Phase 2 第 5 项
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-08
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#pragma once

#include "rendering_device_commons.h"
#include "rendering_context_driver.h"
#include "command_buffer.h"

namespace arhud
{

    class IRenderingContextDriver;

    /**
     * @brief 渲染设备驱动抽象接口
     *
     * 封装所有 GPU 资源操作，是 API 抽象的核心。
     * 使用强类型 ID（BufferID/TextureID/ShaderID 等）替代裸指针，
     * 确保类型安全和资源追踪。
     *
     * @par 关键职责
     *   - Buffer 管理（创建/映射/释放）
     *   - Texture 管理（创建/更新/释放）
     *   - Sampler 管理（创建/释放）
     *   - Shader 编译链接（从 GLSL 源码）
     *   - Pipeline 状态管理（模拟 PSO）
     *   - Framebuffer 管理（FBO 创建/绑定/释放）
     *   - RenderPass 管理（附件描述/子 Pass）
     *   - Uniform Set 绑定（纹理/Buffer 绑定到着色器）
     *   - 命令录制（绘制/状态设置/资源绑定）
     *   - 顶点格式管理
     *
     * @par 线程安全
     *   - 资源创建/销毁应在主线程或资源管理线程
     *   - 命令录制在渲染线程（需要对应 Surface 的 GL 上下文已 MakeCurrent）
     *   - GLStateCache 为线程本地状态，每个渲染线程独立
     *
     * @see IRenderingContextDriver  上下文驱动（创建 RDD 实例）
     * @see rendering_device_commons.h  共享枚举和类型定义
     */
    class IRenderingDeviceDriver
    {
    public:
        virtual ~IRenderingDeviceDriver() = default;

        // ═══════════════════════════════════════════════════════════════════
        // 生命周期
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 初始化设备驱动
         *
         * 查询 GPU 能力和限制，初始化内部状态。
         * 必须在创建任何资源之前调用。
         *
         * @param[in] p_device_index  设备索引（由 RCD 枚举）
         * @param[in] p_frame_count   帧缓冲数量（用于 Staging Buffer 管理）
         *
         * @retval Error::kOK 初始化成功
         * @retval Error::kFailed 初始化失败
         *
         * @pre RCD 已初始化，GL 上下文已 MakeCurrent
         */
        virtual Error Initialize(uint32_t p_device_index, uint32_t p_frame_count) = 0;

        /**
         * @brief 关闭设备驱动，释放所有资源
         *
         * @pre 所有 GPU 资源已释放或不再使用
         */
        virtual void Shutdown() = 0;

        // ═══════════════════════════════════════════════════════════════════
        // Buffer 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建 GPU 缓冲区
         *
         * @param[in] p_size            缓冲区大小（字节）
         * @param[in] p_usage           用途标志位
         * @param[in] p_allocation_type 内存分配类型
         *
         * @retval BufferID 缓冲区 ID，失败返回空 ID
         *
         * @see BufferUsageBits
         * @see MemoryAllocationType
         */
        virtual BufferID BufferCreate(uint64_t p_size,
                                      BitField<BufferUsageBits> p_usage,
                                      MemoryAllocationType p_allocation_type) = 0;

        /**
         * @brief 释放 GPU 缓冲区
         *
         * @param[in] p_buffer 缓冲区 ID
         *
         * @pre p_buffer 为有效 ID
         * @post p_buffer 不再有效
         */
        virtual void BufferFree(BufferID p_buffer) = 0;

        /**
         * @brief 映射缓冲区到 CPU 内存
         *
         * @param[in] p_buffer 缓冲区 ID
         *
         * @return 映射的 CPU 内存指针，失败返回 nullptr
         *
         * @note 调用者应尽快 BufferUnmap()，不要长期持有映射
         */
        virtual uint8_t *BufferMap(BufferID p_buffer) = 0;

        /**
         * @brief 取消映射缓冲区
         *
         * @param[in] p_buffer 缓冲区 ID
         */
        virtual void BufferUnmap(BufferID p_buffer) = 0;

        /**
         * @brief 获取缓冲区分配大小
         *
         * @param[in] p_buffer 缓冲区 ID
         *
         * @return 缓冲区分配大小（字节）
         */
        virtual uint64_t BufferGetAllocationSize(BufferID p_buffer) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // Texture 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建纹理
         *
         * @param[in] p_format 纹理格式描述
         * @param[in] p_view   纹理视图描述
         *
         * @retval TextureID 纹理 ID，失败返回空 ID
         */
        virtual TextureID TextureCreate(const TextureFormat &p_format,
                                        const TextureView &p_view) = 0;

        /**
         * @brief 释放纹理
         *
         * @param[in] p_texture 纹理 ID
         */
        virtual void TextureFree(TextureID p_texture) = 0;

        /**
         * @brief 获取纹理分配大小
         *
         * @param[in] p_texture 纹理 ID
         *
         * @return 纹理分配大小（字节）
         */
        virtual uint64_t TextureGetAllocationSize(TextureID p_texture) = 0;

        /**
         * @brief 查询纹理格式支持的用途
         *
         * @param[in] p_format       数据格式
         * @param[in] p_cpu_readable 是否需要 CPU 可读
         *
         * @return 支持的用途标志位
         */
        virtual BitField<TextureUsageBits> TextureGetUsagesSupportedByFormat(
            DataFormat p_format, bool p_cpu_readable) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // Sampler 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建采样器
         *
         * @param[in] p_state 采样器状态描述
         *
         * @retval SamplerID 采样器 ID，失败返回空 ID
         */
        virtual SamplerID SamplerCreate(const SamplerState &p_state) = 0;

        /**
         * @brief 释放采样器
         *
         * @param[in] p_sampler 采样器 ID
         */
        virtual void SamplerFree(SamplerID p_sampler) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 顶点格式
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建顶点格式
         *
         * @param[in] p_vertex_attribs 顶点属性数组
         *
         * @retval VertexFormatID 顶点格式 ID，失败返回空 ID
         */
        virtual VertexFormatID VertexFormatCreate(
            VectorView<VertexAttribute> p_vertex_attribs) = 0;

        /**
         * @brief 释放顶点格式
         *
         * @param[in] p_vertex_format 顶点格式 ID
         */
        virtual void VertexFormatFree(VertexFormatID p_vertex_format) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // Shader 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 从 GLSL 源码创建着色器
         *
         * 编译并链接多个着色器阶段。
         *
         * @param[in] p_stage_sources   着色器阶段源码数组
         * @param[in] p_uniforms        着色器 Uniform 描述数组
         * @param[in] p_push_constant_size Push Constant 大小（字节），0 表示不使用
         *
         * @retval ShaderID 着色器 ID，编译/链接失败返回空 ID
         *
         * @note OpenGL 实现会在编译时查询所有 Uniform 位置并缓存
         */
        virtual ShaderID ShaderCreateFromGLSL(
            VectorView<ShaderStageSource> p_stage_sources,
            VectorView<ShaderUniform> p_uniforms,
            uint32_t p_push_constant_size) = 0;

        /**
         * @brief 释放着色器
         *
         * @param[in] p_shader 着色器 ID
         */
        virtual void ShaderFree(ShaderID p_shader) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // Uniform Set 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建 Uniform Set
         *
         * @param[in] p_uniforms  绑定 Uniform 数组
         * @param[in] p_shader    关联的着色器
         * @param[in] p_set_index Set 索引（对应着色器中的 set N）
         *
         * @retval UniformSetID Uniform Set ID，失败返回空 ID
         */
        virtual UniformSetID UniformSetCreate(
            VectorView<BoundUniform> p_uniforms,
            ShaderID p_shader,
            uint32_t p_set_index) = 0;

        /**
         * @brief 释放 Uniform Set
         *
         * @param[in] p_uniform_set Uniform Set ID
         */
        virtual void UniformSetFree(UniformSetID p_uniform_set) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // RenderPass 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建渲染 Pass
         *
         * @param[in] p_attachments       附件描述数组
         * @param[in] p_subpasses         子 Pass 描述数组
         * @param[in] p_subpass_dependencies 子 Pass 依赖数组
         *
         * @retval RenderPassID 渲染 Pass ID，失败返回空 ID
         *
         * @note OpenGL 实现仅记录附件格式和清除值，不创建任何 GL 对象
         */
        virtual RenderPassID RenderPassCreate(
            VectorView<Attachment> p_attachments,
            VectorView<Subpass> p_subpasses,
            VectorView<SubpassDependency> p_subpass_dependencies) = 0;

        /**
         * @brief 释放渲染 Pass
         *
         * @param[in] p_render_pass 渲染 Pass ID
         */
        virtual void RenderPassFree(RenderPassID p_render_pass) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // Framebuffer 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建帧缓冲
         *
         * @param[in] p_render_pass 渲染 Pass（用于验证附件兼容性）
         * @param[in] p_attachments 附件纹理 ID 数组（颜色 + 深度）
         * @param[in] p_width       帧缓冲宽度
         * @param[in] p_height      帧缓冲高度
         *
         * @retval FramebufferID 帧缓冲 ID，失败返回空 ID
         */
        virtual FramebufferID FramebufferCreate(
            RenderPassID p_render_pass,
            VectorView<TextureID> p_attachments,
            uint32_t p_width,
            uint32_t p_height) = 0;

        /**
         * @brief 释放帧缓冲
         *
         * @param[in] p_framebuffer 帧缓冲 ID
         */
        virtual void FramebufferFree(FramebufferID p_framebuffer) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // Pipeline 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建渲染管线
         *
         * OpenGL 实现记录管线状态到结构体，不创建任何 GL 对象。
         * 在 CommandBindRenderPipeline() 时才逐个设置 GL 状态。
         *
         * @param[in] p_shader               着色器
         * @param[in] p_vertex_format        顶点格式
         * @param[in] p_render_primitive     渲染图元
         * @param[in] p_rasterization_state  光栅化状态
         * @param[in] p_multisample_state    多重采样状态
         * @param[in] p_depth_stencil_state  深度/模板状态
         * @param[in] p_blend_state          颜色混合状态
         * @param[in] p_dynamic_state        动态状态标志位
         * @param[in] p_render_pass          渲染 Pass
         *
         * @retval PipelineID 管线 ID，失败返回空 ID
         */
        virtual PipelineID RenderPipelineCreate(
            ShaderID p_shader,
            VertexFormatID p_vertex_format,
            RenderPrimitive p_render_primitive,
            const PipelineRasterizationState &p_rasterization_state,
            const PipelineMultisampleState &p_multisample_state,
            const PipelineDepthStencilState &p_depth_stencil_state,
            const PipelineColorBlendState &p_blend_state,
            BitField<PipelineDynamicStateFlags> p_dynamic_state,
            RenderPassID p_render_pass) = 0;

        /**
         * @brief 释放管线
         *
         * @param[in] p_pipeline 管线 ID
         */
        virtual void PipelineFree(PipelineID p_pipeline) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // CommandBuffer 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建命令缓冲
         *
         * 返回的 ICommandBuffer 可用于录制渲染命令。
         * 主线程录制，渲染线程回放。
         *
         * @retval ICommandBuffer* 命令缓冲指针，失败返回 nullptr
         *
         * @note 调用者负责通过 CommandBufferFree() 释放
         *
         * @par 线程安全
         *   创建操作本身线程安全
         *   录制操作（ICommandBuffer::Begin/End/Draw 等）也线程安全
         *   回放操作（ICommandBuffer::Execute）必须在 GL 上下文线程
         */
        virtual ICommandBuffer *CommandBufferCreate() = 0;

        /**
         * @brief 释放命令缓冲
         *
         * @param[in] p_command_buffer 命令缓冲指针
         *
         * @pre p_command_buffer 不在 Executing 状态
         */
        virtual void CommandBufferFree(ICommandBuffer *p_command_buffer) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 命令录制 — 渲染 Pass（立即模式，向后兼容）
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 开始渲染 Pass
         *
         * 绑定帧缓冲，设置清除值和绘制区域。
         *
         * @param[in] p_render_pass  渲染 Pass
         * @param[in] p_framebuffer  帧缓冲
         * @param[in] p_clear_values 清除值数组（颜色 + 深度/模板）
         * @param[in] p_rect         渲染区域（x, y, width, height）
         *
         * @note OpenGL 实现：glBindFramebuffer + glClear + glViewport
         */
        virtual void CommandBeginRenderPass(
            RenderPassID p_render_pass,
            FramebufferID p_framebuffer,
            VectorView<RenderPassClearValue> p_clear_values,
            int32_t p_rect_x, int32_t p_rect_y,
            uint32_t p_rect_w, uint32_t p_rect_h) = 0;

        /**
         * @brief 结束渲染 Pass
         *
         * @note OpenGL 实现：glBindFramebuffer(GL_FRAMEBUFFER, 0)
         */
        virtual void CommandEndRenderPass() = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 命令录制 — 绑定
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 绑定渲染管线
         *
         * 设置着色器程序和所有管线状态（光栅化/深度/混合）。
         *
         * @param[in] p_pipeline 管线 ID
         *
         * @note OpenGL 实现：glUseProgram + 设置 GL 状态 + GLStateCache
         */
        virtual void CommandBindRenderPipeline(PipelineID p_pipeline) = 0;

        /**
         * @brief 绑定 Uniform Set
         *
         * 将纹理/Buffer 绑定到着色器的指定 Set。
         *
         * @param[in] p_uniform_set Uniform Set ID
         * @param[in] p_set_index   Set 索引
         *
         * @note OpenGL 实现：glUniform + glBindTexture + glBindBufferBase
         */
        virtual void CommandBindUniformSet(UniformSetID p_uniform_set,
                                           uint32_t p_set_index) = 0;

        /**
         * @brief 绑定顶点缓冲区
         *
         * @param[in] p_buffers  顶点缓冲区 ID 数组
         * @param[in] p_offsets  偏移量数组
         * @param[in] p_count    缓冲区数量
         */
        virtual void CommandBindVertexBuffers(
            const BufferID *p_buffers,
            const uint64_t *p_offsets,
            uint32_t p_count) = 0;

        /**
         * @brief 绑定索引缓冲区
         *
         * @param[in] p_buffer 索引缓冲区 ID
         * @param[in] p_format 索引格式
         * @param[in] p_offset 偏移量（字节）
         */
        virtual void CommandBindIndexBuffer(BufferID p_buffer,
                                            IndexBufferFormat p_format,
                                            uint64_t p_offset) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 命令录制 — 绘制
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 非索引绘制
         *
         * @param[in] p_vertex_count  顶点数量
         * @param[in] p_instance_count 实例数量
         * @param[in] p_base_vertex   起始顶点偏移
         * @param[in] p_first_instance 起始实例偏移
         */
        virtual void CommandDraw(uint32_t p_vertex_count,
                                 uint32_t p_instance_count,
                                 uint32_t p_base_vertex,
                                 uint32_t p_first_instance) = 0;

        /**
         * @brief 索引绘制
         *
         * @param[in] p_index_count   索引数量
         * @param[in] p_instance_count 实例数量
         * @param[in] p_first_index   起始索引偏移
         * @param[in] p_vertex_offset 顶点偏移
         * @param[in] p_first_instance 起始实例偏移
         */
        virtual void CommandDrawIndexed(uint32_t p_index_count,
                                        uint32_t p_instance_count,
                                        uint32_t p_first_index,
                                        int32_t p_vertex_offset,
                                        uint32_t p_first_instance) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 命令录制 — 状态设置
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 设置视口
         *
         * @param[in] p_x      视口左下角 X
         * @param[in] p_y      视口左下角 Y
         * @param[in] p_width  视口宽度
         * @param[in] p_height 视口高度
         */
        virtual void CommandSetViewport(int32_t p_x, int32_t p_y,
                                        uint32_t p_width, uint32_t p_height) = 0;

        /**
         * @brief 设置裁剪矩形
         *
         * @param[in] p_x      矩形左下角 X
         * @param[in] p_y      矩形左下角 Y
         * @param[in] p_width  矩形宽度
         * @param[in] p_height 矩形高度
         */
        virtual void CommandSetScissor(int32_t p_x, int32_t p_y,
                                       uint32_t p_width, uint32_t p_height) = 0;

        /**
         * @brief 设置混合常量
         *
         * @param[in] p_r 红色分量
         * @param[in] p_g 绿色分量
         * @param[in] p_b 蓝色分量
         * @param[in] p_a 透明度分量
         */
        virtual void CommandSetBlendConstants(float p_r, float p_g,
                                              float p_b, float p_a) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 命令录制 — 数据传输
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 清除缓冲区
         *
         * @param[in] p_buffer 缓冲区 ID
         * @param[in] p_offset 起始偏移（字节）
         * @param[in] p_size   清除大小（字节）
         */
        virtual void CommandClearBuffer(BufferID p_buffer,
                                        uint64_t p_offset,
                                        uint64_t p_size) = 0;

        /**
         * @brief 拷贝缓冲区
         *
         * @param[in] p_src_buffer 源缓冲区
         * @param[in] p_dst_buffer 目标缓冲区
         * @param[in] p_regions    拷贝区域数组
         */
        virtual void CommandCopyBuffer(BufferID p_src_buffer,
                                       BufferID p_dst_buffer,
                                       VectorView<BufferCopyRegion> p_regions) = 0;

        /**
         * @brief 拷贝缓冲区数据到纹理
         *
         * 将 Buffer 中的像素数据上传到 Texture 的指定子资源。
         * OpenGL 实现：绑定 Buffer 为 PBO，调用 glTexSubImage2D。
         * Vulkan 实现：vkCmdCopyBufferToImage。
         *
         * @param[in] p_src_buffer 源缓冲区
         * @param[in] p_dst_texture 目标纹理
         * @param[in] p_regions    拷贝区域数组
         *
         * @note OpenGL 实现使用 PBO 方式：先绑定 Buffer 到 GL_PIXEL_UNPACK_BUFFER，
         *       然后调用 glTexSubImage2D，最后解绑 PBO。
         */
        virtual void CommandCopyBufferToTexture(
            BufferID p_src_buffer,
            TextureID p_dst_texture,
            VectorView<BufferTextureCopyRegion> p_regions) = 0;

        /**
         * @brief 清除颜色纹理
         *
         * @param[in] p_texture     纹理 ID
         * @param[in] p_color_r     清除颜色 R
         * @param[in] p_color_g     清除颜色 G
         * @param[in] p_color_b     清除颜色 B
         * @param[in] p_color_a     清除颜色 A
         * @param[in] p_subresources 子资源范围
         */
        virtual void CommandClearColorTexture(
            TextureID p_texture,
            float p_color_r, float p_color_g,
            float p_color_b, float p_color_a,
            const TextureSubresourceRange &p_subresources) = 0;

        /**
         * @brief 清除深度/模板纹理
         *
         * @param[in] p_texture     纹理 ID
         * @param[in] p_depth       清除深度值
         * @param[in] p_stencil     清除模板值
         * @param[in] p_subresources 子资源范围
         */
        virtual void CommandClearDepthStencilTexture(
            TextureID p_texture,
            float p_depth, uint32_t p_stencil,
            const TextureSubresourceRange &p_subresources) = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 帧同步
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 开始帧段
         *
         * @param[in] p_frame_index 帧索引
         * @param[in] p_frames_drawn 已绘制帧数
         */
        virtual void BeginSegment(uint32_t p_frame_index,
                                  uint32_t p_frames_drawn) = 0;

        /**
         * @brief 结束帧段
         */
        virtual void EndSegment() = 0;

        // ═══════════════════════════════════════════════════════════════════
        // 查询
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 查询设备限制
         *
         * @param[in] p_limit 限制项
         *
         * @return 限制值
         */
        virtual uint64_t LimitGet(Limit p_limit) = 0;

        /**
         * @brief 查询设备是否支持某特性
         *
         * @param[in] p_feature 特性项
         *
         * @return true 支持
         */
        virtual bool HasFeature(Features p_feature) = 0;

        /**
         * @brief 获取设备能力描述
         *
         * @return Capabilities 常引用
         */
        virtual const Capabilities &GetCapabilities() const = 0;

        /**
         * @brief 获取 API 名称
         *
         * @return API 名称字符串（如 "OpenGL 3.3"）
         */
        virtual const char *GetApiName() const = 0;

        /**
         * @brief 获取管线缓存 UUID
         *
         * @return UUID 字符串
         *
         * @note OpenGL 实现返回空字符串（无 PSO 缓存）
         */
        virtual const char *GetPipelineCacheUuid() const = 0;

        // ═══════════════════════════════════════════════════════════════════
        // SwapChain 管理
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief SwapChain ID 类型
         *
         * SwapChain 是渲染表面与上下文的绑定抽象。
         * Vulkan: VkSwapchainKHR 的封装。
         * OpenGL: Context + Surface 的绑定关系。
         */
        using SwapChainID = uint64_t;

        /** @brief 无效 SwapChain ID */
        static constexpr SwapChainID kInvalidSwapChainId = 0;

        /**
         * @brief 创建 SwapChain
         *
         * 将指定 Surface 与渲染上下文绑定，创建可呈现的 SwapChain。
         *
         * @param[in] p_surface  RCD Surface ID
         * @param[in] p_context  RCD Context ID（GL 使用，Vulkan 忽略）
         *
         * @return SwapChain ID
         * @retval kInvalidSwapChainId 创建失败
         *
         * @par OpenGL 实现
         *   记录 Surface 对应的 GL Context ID 和 GL Surface ID，
         *   SwapChainAcquire 时调用 IGLManager::MakeCurrent。
         *
         * @par Vulkan 实现（未来）
         *   创建 VkSwapchainKHR，关联 VkImage 队列。
         */
        virtual SwapChainID SwapChainCreate(IRenderingContextDriver::SurfaceID p_surface,
                                            IRenderingContextDriver::ContextID p_context)
        {
            (void)p_surface;
            (void)p_context;
            return kInvalidSwapChainId;
        }

        /**
         * @brief 销毁 SwapChain
         *
         * @param[in] p_swapchain SwapChain ID
         *
         * @note OpenGL 实现为空操作（GL 无 SwapChain 概念）
         */
        virtual void SwapChainDestroy(SwapChainID p_swapchain) { (void)p_swapchain; }

        /**
         * @brief 获取 SwapChain 的下一帧图像
         *
         * 在渲染开始前调用，准备下一帧的渲染目标。
         *
         * @param[in] p_swapchain SwapChain ID
         *
         * @return Error::kOK 获取成功
         *
         * @par OpenGL 实现
         *   调用 IGLManager::MakeCurrent() 绑定 Context 到 Surface。
         *
         * @par Vulkan 实现（未来）
         *   调用 vkAcquireNextImageKHR 获取下一帧图像索引。
         */
        virtual Error SwapChainAcquire(SwapChainID p_swapchain) { (void)p_swapchain; return Error::kOK; }

        /**
         * @brief 呈现 SwapChain 的渲染结果
         *
         * 在渲染完成后调用，将渲染结果呈现到屏幕。
         *
         * @param[in] p_swapchain SwapChain ID
         *
         * @par OpenGL 实现
         *   调用 IGLManager::SwapBuffers() 交换前后缓冲区。
         *
         * @par Vulkan 实现（未来）
         *   调用 vkQueuePresentKHR 呈现渲染结果。
         */
        virtual void SwapChainPresent(SwapChainID p_swapchain) { (void)p_swapchain; }

        // ═══════════════════════════════════════════════════════════════════
        // Vulkan 预留接口（多线程同步与队列提交）
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief GPU 信号量 ID 类型
         *
         * 用于 GPU-GPU 同步（命令队列间、渲染-呈现间）。
         * Vulkan: VkSemaphore 的封装。
         * OpenGL: 不需要（SwapBuffers 隐式同步）。
         */
        using SemaphoreID = uint64_t;

        /** @brief 无效信号量 ID */
        static constexpr SemaphoreID kInvalidSemaphoreId = 0;

        /**
         * @brief GPU 栅栏 ID 类型
         *
         * 用于 GPU-CPU 同步（等待 GPU 命令完成）。
         * Vulkan: VkFence 的封装。
         * OpenGL: 不需要（glFinish 可替代，但性能差）。
         */
        using FenceID = uint64_t;

        /** @brief 无效栅栏 ID */
        static constexpr FenceID kInvalidFenceId = 0;

        /**
         * @brief 管线阶段标志位
         *
         * 对齐 Vulkan VkPipelineStageFlags，用于 QueueSubmit 的等待阶段。
         * OpenGL 实现忽略此参数（GL 命令隐式有序）。
         */
        enum class PipelineStageBits : uint32_t
        {
            kTopOfPipe = 0,
            kColorAttachmentOutput = 1,
            kFragmentShader = 2,
            kTransfer = 3,
        };

        /**
         * @brief 队列提交信息
         *
         * 描述一次命令缓冲提交的同步参数。
         * Vulkan: 对应 VkSubmitInfo 的核心字段。
         * OpenGL: 忽略（GL 命令串行执行）。
         */
        struct QueueSubmitInfo
        {
            CommandBufferID command_buffer;        ///< 命令缓冲
            SemaphoreID wait_semaphore = kInvalidSemaphoreId;  ///< 等待的信号量
            SemaphoreID signal_semaphore = kInvalidSemaphoreId; ///< 完成后触发的信号量
            PipelineStageBits wait_stage = PipelineStageBits::kTopOfPipe; ///< 等待阶段
        };

        /**
         * @brief 队列呈现信息
         *
         * 描述一次 Present 操作的同步参数。
         * Vulkan: 对应 VkPresentInfoKHR 的核心字段。
         * OpenGL: 忽略（SwapBuffers 隐式同步）。
         */
        struct QueuePresentInfo
        {
            uint32_t surface = UINT32_MAX; ///< 目标表面 ID
            SemaphoreID wait_semaphore = kInvalidSemaphoreId; ///< 等待的信号量
        };

        /**
         * @brief 创建 GPU 信号量
         *
         * @return 信号量 ID
         * @retval kInvalidSemaphoreId 创建失败或 OpenGL 不支持
         *
         * @note OpenGL 实现始终返回 kInvalidSemaphoreId
         */
        virtual SemaphoreID SemaphoreCreate() { return kInvalidSemaphoreId; }

        /**
         * @brief 销毁 GPU 信号量
         *
         * @param[in] p_semaphore 信号量 ID
         *
         * @note OpenGL 实现为空操作
         */
        virtual void SemaphoreDestroy(SemaphoreID p_semaphore) { (void)p_semaphore; }

        /**
         * @brief 创建 GPU 栅栏
         *
         * @param[in] p_signaled 是否创建为已触发状态
         *
         * @return 栅栏 ID
         * @retval kInvalidFenceId 创建失败或 OpenGL 不支持
         *
         * @note OpenGL 实现始终返回 kInvalidFenceId
         */
        virtual FenceID FenceCreate(bool p_signaled = false) { (void)p_signaled; return kInvalidFenceId; }

        /**
         * @brief 销毁 GPU 栅栏
         *
         * @param[in] p_fence 栅栏 ID
         *
         * @note OpenGL 实现为空操作
         */
        virtual void FenceDestroy(FenceID p_fence) { (void)p_fence; }

        /**
         * @brief 重置 GPU 栅栏
         *
         * @param[in] p_fence 栅栏 ID
         *
         * @note OpenGL 实现为空操作
         */
        virtual void FenceReset(FenceID p_fence) { (void)p_fence; }

        /**
         * @brief 查询 GPU 栅栏是否已触发
         *
         * @param[in] p_fence 栅栏 ID
         *
         * @return true 已触发
         * @retval true OpenGL 始终返回 true（无栅栏概念）
         */
        virtual bool FenceIsSignaled(FenceID p_fence) { (void)p_fence; return true; }

        /**
         * @brief 等待 GPU 栅栏触发
         *
         * @param[in] p_fence 栅栏 ID
         * @param[in] p_timeout_ns 超时时间（纳秒）
         *
         * @return true 栅栏已触发
         * @retval true OpenGL 始终返回 true
         *
         * @note OpenGL 实现始终返回 true（无栅栏概念）
         */
        virtual bool FenceWait(FenceID p_fence, uint64_t p_timeout_ns) { (void)p_fence; (void)p_timeout_ns; return true; }

        /**
         * @brief 提交命令缓冲到 GPU 队列
         *
         * Vulkan: 对应 vkQueueSubmit，支持信号量同步。
         * OpenGL: 空操作（GL 命令在调用时立即执行）。
         *
         * @param[in] p_queue_family 队列族索引
         * @param[in] p_queue_index 队列索引
         * @param[in] p_submits 提交信息数组
         * @param[in] p_fence 完成栅栏
         *
         * @note OpenGL 实现为空操作
         */
        virtual void QueueSubmit(uint32_t p_queue_family,
                                 uint32_t p_queue_index,
                                 VectorView<QueueSubmitInfo> p_submits,
                                 FenceID p_fence) { (void)p_queue_family; (void)p_queue_index; (void)p_submits; (void)p_fence; }

        /**
         * @brief 呈现渲染结果到 Surface
         *
         * Vulkan: 对应 vkQueuePresentKHR，支持信号量等待。
         * OpenGL: 空操作（由 RDD::SwapChainPresent 替代）。
         *
         * @param[in] p_info 呈现信息
         *
         * @note OpenGL 实现为空操作
         */
        virtual void QueuePresent(QueuePresentInfo p_info) { (void)p_info; }
    };

} // namespace arhud
