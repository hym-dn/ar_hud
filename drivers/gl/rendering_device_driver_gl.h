/**
 * @file rendering_device_driver_gl.h
 * @brief OpenGL 渲染设备驱动实现
 *
 * GLRenderingDeviceDriver 是 IRenderingDeviceDriver 的 OpenGL 实现，
 * 使用 OpenGL 3.3 Core Profile API 实现 GPU 资源管理和命令录制。
 *
 * @par 架构定位
 *   @code
 *   IRenderingDeviceDriver (接口)
 *       └── GLRenderingDeviceDriver (OpenGL 实现)
 *               ├── 持有 IRenderingContextDriver* 引用
 *               ├── GLStateCache（GL 状态缓存，避免冗余调用）
 *               ├── GLPipelineState（模拟 Vulkan PSO）
 *               ├── GLShaderInfo（着色器编译信息 + Uniform 位置缓存）
 *               └── GLFramebufferInfo（FBO 附件映射）
 *   @endcode
 *
 * @par OpenGL 特殊策略
 *   1. **Pipeline 模拟**：OpenGL 没有 PSO，RenderPipelineCreate() 记录状态
 *      到 GLPipelineState 结构体，CommandBindRenderPipeline() 时逐个设置 GL 状态
 *   2. **CommandBuffer 简化**：OpenGL 是立即模式，命令直接执行，
 *      不需要 CommandBuffer 录制/提交。CommandXxx() 函数直接调用 GL API
 *   3. **SwapChain 简化**：OpenGL 使用 FBO 0 + SwapBuffers，
 *      不需要 Vulkan 式的 acquire/present 流程
 *   4. **Fence/Semaphore 简化**：OpenGL 同步由 SwapBuffers 隐式完成
 *   5. **GLStateCache**：缓存当前 GL 状态，避免冗余 glEnable/glDisable 调用
 *
 * @par 与 Vulkan RDD 的对比
 *   | 维度             | OpenGL RDD                      | Vulkan RDD                    |
 *   |-----------------|---------------------------------|-------------------------------|
 *   | Pipeline        | 状态结构体 + 延迟设置            | VkPipeline (PSO)              |
 *   | CommandBuffer   | 立即模式（直接 GL 调用）          | VkCommandBuffer（延迟提交）    |
 *   | SwapChain       | FBO 0 + SwapBuffers              | VkSwapchainKHR                |
 *   | 同步             | SwapBuffers 隐式                 | VkFence + VkSemaphore         |
 *   | Barrier          | 忽略（GL 自动同步）               | vkCmdPipelineBarrier          |
 *   | Shader           | GLSL 运行时编译                  | SPIRV → VkShaderModule        |
 *   | Uniform Set      | glUniform + glBindTexture        | VkDescriptorSet               |
 *
 * @par GLStateCache 设计
 *   GL 状态切换是 OpenGL 的主要性能瓶颈之一。GLStateCache 缓存当前 GL 状态，
 *   仅在状态实际改变时才调用 GL API，减少驱动开销。
 *
 *   缓存的状态包括：
 *   - 当前绑定的着色器程序（glUseProgram）
 *   - 当前绑定的 VAO（glBindVertexArray）
 *   - 当前绑定的 FBO（glBindFramebuffer）
 *   - 当前绑定的 VBO（glBindBuffer(GL_ARRAY_BUFFER)）
 *   - 当前绑定的 IBO（glBindBuffer(GL_ELEMENT_ARRAY_BUFFER)）
 *   - 当前绑定的 UBO（glBindBufferBase(GL_UNIFORM_BUFFER)）
 *   - 当前激活的纹理单元（glActiveTexture）
 *   - 深度测试/写入状态
 *   - 混合状态
 *   - 剔除状态
 *
 * @par 资源 ID 映射策略
 *   OpenGL 的资源 ID（GLuint）是 32 位整数，ARHud 的 ID 类型是 uint64_t。
 *   映射方式：直接将 GLuint 零扩展到 uint64_t。
 *   对于需要额外簿记信息的资源（如 Shader、Pipeline），使用 PagedAllocator
 *   分配簿记结构，将簿记结构指针作为 ID。
 *
 * @par 设计参考
 *   - Godot 4.6 RenderingDeviceDriverGLES3（未公开，参考 GLES3 实现）
 *   - Godot 4.6 RenderingDeviceDriverVulkan（Vulkan 实现参考）
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

#include "rendering_device_driver.h"
#include "command_buffer_gl.h"
#include "gl_manager.h"
#include "template/paged_allocator.h"
#include "template/local_vector.h"
#include "template/hash_map.h"

namespace arhud
{

    class IRenderingContextDriver;

    // ═══════════════════════════════════════════════════════════════════════
    // GL 状态缓存
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief OpenGL 状态缓存
     *
     * 缓存当前 GL 状态，避免冗余 GL API 调用。
     * 每个渲染线程应有独立的 GLStateCache 实例。
     *
     * @par 使用方式
     *   @code
     *   // 设置深度测试（仅在状态改变时才调用 glEnable/glDisable）
     *   state_cache.SetDepthTest(true);
     *   state_cache.SetDepthWrite(true);
     *   state_cache.SetDepthFunc(GL_LESS);
     *
     *   // 绑定着色器程序（仅在程序改变时才调用 glUseProgram）
     *   state_cache.UseProgram(shader_program);
     *
     *   // 绑定 VAO
     *   state_cache.BindVertexArray(vao);
     *   @endcode
     *
     * @par 线程安全
     *   非线程安全。每个渲染线程应持有独立的 GLStateCache 实例。
     *   GL 上下文本身也是线程绑定的，因此状态缓存不需要跨线程同步。
     */
    class GLStateCache
    {
    public:
        GLStateCache();

        /**
         * @brief 重置所有缓存状态
         *
         * 在上下文切换或帧开始时调用，强制下次所有状态设置都执行 GL 调用。
         */
        void Reset();

        void UseProgram(uint32_t p_program);
        void BindVertexArray(uint32_t p_vao);
        void BindFramebuffer(uint32_t p_fbo);
        void BindVertexBuffer(uint32_t p_buffer);
        void BindIndexBuffer(uint32_t p_buffer);
        void BindUniformBuffer(uint32_t p_index, uint32_t p_buffer);
        void ActiveTexture(uint32_t p_unit);
        void BindTexture2D(uint32_t p_unit, uint32_t p_texture);
        void BindTexture(uint32_t p_target, uint32_t p_texture);
        void BindSampler(uint32_t p_unit, uint32_t p_sampler);

        void SetDepthTest(bool p_enable);
        void SetDepthWrite(bool p_enable);
        void SetDepthFunc(uint32_t p_func);
        void SetBlend(bool p_enable);
        void SetBlendFunc(uint32_t p_src_rgb, uint32_t p_dst_rgb,
                          uint32_t p_src_alpha, uint32_t p_dst_alpha);
        void SetCullFace(bool p_enable);
        void SetCullMode(uint32_t p_mode);
        void SetFrontFace(uint32_t p_face);
        void SetViewport(int32_t p_x, int32_t p_y,
                         uint32_t p_width, uint32_t p_height);
        void SetScissor(int32_t p_x, int32_t p_y,
                        uint32_t p_width, uint32_t p_height);

    private:
        uint32_t program_ = 0;
        uint32_t vao_ = 0;
        uint32_t fbo_ = 0;
        uint32_t vertex_buffer_ = 0;
        uint32_t index_buffer_ = 0;
        uint32_t active_texture_unit_ = 0;
        uint32_t bound_textures_[16] = {};
        uint32_t bound_ubos_[16] = {};
        uint32_t bound_samplers_[16] = {};

        bool depth_test_ = false;
        bool depth_write_ = true;
        uint32_t depth_func_ = 0x0201;
        bool blend_ = false;
        uint32_t blend_src_rgb_ = 1;
        uint32_t blend_dst_rgb_ = 0;
        uint32_t blend_src_alpha_ = 1;
        uint32_t blend_dst_alpha_ = 0;
        bool cull_face_ = false;
        uint32_t cull_mode_ = 0x0405;
        uint32_t front_face_ = 0x0900;

        int32_t viewport_x_ = 0;
        int32_t viewport_y_ = 0;
        uint32_t viewport_w_ = 0;
        uint32_t viewport_h_ = 0;
        bool viewport_dirty_ = true;

        int32_t scissor_x_ = 0;
        int32_t scissor_y_ = 0;
        uint32_t scissor_w_ = 0;
        uint32_t scissor_h_ = 0;
        bool scissor_dirty_ = true;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // GL 管线状态（模拟 Vulkan PSO）
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief OpenGL 渲染管线状态
     *
     * 记录完整的渲染管线状态，在 CommandBindRenderPipeline() 时
     * 通过 GLStateCache 逐个设置到 GL 状态机。
     *
     * @par 与 Vulkan PSO 的区别
     *   Vulkan 的 VkPipeline 是不可变对象，创建后不能修改。
     *   GLPipelineState 是可变结构体，但 ARHud 遵循 Vulkan 语义，
     *   创建后不应修改（除非通过动态状态标志位）。
     */
    struct GLPipelineState
    {
        uint32_t shader_program = 0;

        RenderPrimitive render_primitive = RenderPrimitive::kTriangles;

        bool cull_enable = false;
        uint32_t cull_mode = 0x0405;  // GL_BACK
        uint32_t front_face = 0x0900; // GL_CW

        bool depth_test_enable = false;
        bool depth_write_enable = false;
        uint32_t depth_func = 0x0201; // GL_LESS

        bool blend_enable = false;
        uint32_t blend_src_rgb = 1; // GL_ONE
        uint32_t blend_dst_rgb = 0; // GL_ZERO
        uint32_t blend_src_alpha = 1;
        uint32_t blend_dst_alpha = 0;

        bool wireframe = false;

        BitField<PipelineDynamicStateFlags> dynamic_state;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // GL 着色器信息
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief OpenGL 着色器编译信息
     *
     * 存储着色器程序 ID 和所有 Uniform 位置缓存。
     * Uniform 位置在 ShaderCreateFromGLSL() 时一次性查询并缓存，
     * 避免每次绑定时重复查询。
     */
    struct GLShaderInfo
    {
        uint32_t program = 0;
        uint32_t vertex_shader = 0;
        uint32_t fragment_shader = 0;

        struct UniformLocation
        {
            uint32_t binding = UINT32_MAX;
            int32_t location = -1;
            UniformType type = UniformType::kMax;
        };

        LocalVector<UniformLocation> uniform_locations;

        int32_t FindLocation(uint32_t p_binding) const;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // GL 帧缓冲信息
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief OpenGL 帧缓冲信息
     *
     * 存储 FBO ID 和附件映射。
     */
    struct GLFramebufferInfo
    {
        uint32_t fbo = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        LocalVector<uint32_t> color_attachments;
        uint32_t depth_stencil_attachment = 0;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // GL 渲染 Pass 信息
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief OpenGL 渲染 Pass 信息
     *
     * 记录附件描述和清除值，在 CommandBeginRenderPass() 时使用。
     * OpenGL 不需要创建 GL 对象来表示 RenderPass。
     */
    struct GLRenderPassInfo
    {
        LocalVector<Attachment> attachments;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // GL Uniform Set 信息
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief OpenGL Uniform Set 信息
     *
     * 存储绑定的纹理和 Buffer，在 CommandBindUniformSet() 时使用。
     */
    struct GLUniformSetInfo
    {
        struct BoundResource
        {
            UniformType type = UniformType::kMax;
            uint32_t binding = UINT32_MAX;
            uint32_t gl_name = 0;
        };

        LocalVector<BoundResource> resources;
        ShaderID shader;
        uint32_t set_index = 0;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // GL 顶点格式信息
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief OpenGL 顶点格式信息
     *
     * 存储 VAO ID 和属性描述，在 CommandBindRenderPipeline() 时绑定。
     */
    struct GLVertexFormatInfo
    {
        uint32_t vao = 0;
        LocalVector<VertexAttribute> attributes;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // GL SwapChain 信息
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief OpenGL SwapChain 信息
     *
     * 记录 RCD Surface 与 GL Context/Surface 的绑定关系。
     * SwapChainAcquire 时调用 IGLManager::MakeCurrent，
     * SwapChainPresent 时调用 IGLManager::SwapBuffers。
     */
    struct GLSwapChain
    {
        IRenderingContextDriver::SurfaceID rcd_surface = IRenderingContextDriver::kInvalidSurfaceId;
        IGLManager::ContextID gl_context = IGLManager::kInvalidContextId;
        IGLManager::SurfaceID gl_surface = IGLManager::kInvalidSurfaceId;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // GLRenderingDeviceDriver
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief OpenGL 渲染设备驱动
     *
     * 使用 OpenGL 3.3 Core Profile API 实现 IRenderingDeviceDriver 接口。
     * 所有 GPU 资源操作直接映射到 GL API 调用。
     *
     * @par 关键设计
     *   - Pipeline 状态延迟设置（模拟 PSO）
     *   - GLStateCache 避免冗余 GL 状态切换
     *   - Uniform 位置在着色器创建时缓存
     *   - VAO 在顶点格式创建时生成
     *   - FBO 在帧缓冲创建时生成
     *   - 支持两种命令模式：CommandBuffer（录制/回放）和立即模式（向后兼容）
     *
     * @par 线程安全
     *   - 资源创建/销毁应在拥有 GL 上下文的线程
     *   - CommandBuffer 录制可在任意线程（内部 Mutex 保护）
     *   - CommandBuffer 回放必须在拥有 GL 上下文的渲染线程
     *   - 立即模式命令必须在渲染线程（需要 GL 上下文已 MakeCurrent）
     *   - GLStateCache 为线程本地状态
     *
     * @see IRenderingDeviceDriver  基类接口
     * @see ICommandBuffer        命令缓冲接口
     * @see GLCommandBuffer        OpenGL 命令缓冲实现
     * @see GLStateCache           GL 状态缓存
     * @see GLPipelineState        管线状态
     */
    class GLRenderingDeviceDriver : public IRenderingDeviceDriver
    {
    public:
        explicit GLRenderingDeviceDriver(IRenderingContextDriver *p_context_driver,
                                         IGLManager *p_gl_manager);
        ~GLRenderingDeviceDriver() override;

        ARHUD_DISABLE_COPY(GLRenderingDeviceDriver);

        // ═══════════════════════════════════════════════════════════════════════
        // 生命周期
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 初始化 OpenGL 渲染设备驱动
         *
         * @param[in] p_device_index  设备索引
         * @param[in] p_frame_count   帧缓冲数量
         *
         * @retval Error::kOK 初始化成功
         * @retval Error::kFailed 初始化失败
         * @retval Error::kNotSupported 不支持的 GL 版本
         *
         * @pre GL 上下文已 MakeCurrent
         */
        Error Initialize(uint32_t p_device_index, uint32_t p_frame_count) override;

        /**
         * @brief 关闭驱动，释放所有资源
         */
        void Shutdown() override;

        // ═══════════════════════════════════════════════════════════════════════
        // Buffer 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 创建 GPU 缓冲区
         *
         * @param[in] p_size            缓冲区大小（字节）
         * @param[in] p_usage           用途标志位
         * @param[in] p_allocation_type 内存分配类型
         *
         * @retval BufferID 缓冲区 ID，失败返回空 ID
         *
         * @note OpenGL 实现：glGenBuffers + glNamedBufferStorage
         */
        BufferID BufferCreate(uint64_t p_size,
                              BitField<BufferUsageBits> p_usage,
                              MemoryAllocationType p_allocation_type) override;

        /**
         * @brief 释放 GPU 缓冲区
         *
         * @param[in] p_buffer 缓冲区 ID
         *
         * @note OpenGL 实现：glDeleteBuffers
         */
        void BufferFree(BufferID p_buffer) override;

        /**
         * @brief 映射缓冲区到 CPU 内存
         *
         * @param[in] p_buffer 缓冲区 ID
         *
         * @retval 映射的 CPU 内存指针，失败返回 nullptr
         *
         * @note OpenGL 实现：glMapNamedBuffer
         * @note 调用者应尽快 BufferUnmap()，不要长期持有映射
         */
        uint8_t *BufferMap(BufferID p_buffer) override;

        /**
         * @brief 取消映射缓冲区
         *
         * @param[in] p_buffer 缓冲区 ID
         *
         * @note OpenGL 实现：glUnmapNamedBuffer
         */
        void BufferUnmap(BufferID p_buffer) override;

        /**
         * @brief 获取缓冲区分配大小
         *
         * @param[in] p_buffer 缓冲区 ID
         *
         * @return 缓冲区分配大小（字节）
         */
        uint64_t BufferGetAllocationSize(BufferID p_buffer) override;

        // ═══════════════════════════════════════════════════════════════════════
        // Texture 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 创建纹理
         *
         * @param[in] p_format 纹理格式描述
         * @param[in] p_view   纹理视图描述
         *
         * @retval TextureID 纹理 ID，失败返回空 ID
         *
         * @note OpenGL 实现：glCreateTextures + glTextureStorage2D
         */
        TextureID TextureCreate(const TextureFormat &p_format,
                                const TextureView &p_view) override;

        /**
         * @brief 释放纹理
         *
         * @param[in] p_texture 纹理 ID
         *
         * @note OpenGL 实现：glDeleteTextures
         */
        void TextureFree(TextureID p_texture) override;

        /**
         * @brief 获取纹理分配大小
         *
         * @param[in] p_texture 纹理 ID
         *
         * @return 纹理分配大小（字节）
         */
        uint64_t TextureGetAllocationSize(TextureID p_texture) override;

        /**
         * @brief 查询纹理格式支持的用途
         *
         * @param[in] p_format       数据格式
         * @param[in] p_cpu_readable 是否需要 CPU 可读
         *
         * @return 支持的用途标志位
         */
        BitField<TextureUsageBits> TextureGetUsagesSupportedByFormat(
            DataFormat p_format, bool p_cpu_readable) override;

        // ═══════════════════════════════════════════════════════════════════════
        // Sampler 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 创建采样器
         *
         * @param[in] p_state 采样器状态描述
         *
         * @retval SamplerID 采样器 ID，失败返回空 ID
         *
         * @note OpenGL 实现：glCreateSamplers + glSamplerParameter
         */
        SamplerID SamplerCreate(const SamplerState &p_state) override;

        /**
         * @brief 释放采样器
         *
         * @param[in] p_sampler 采样器 ID
         *
         * @note OpenGL 实现：glDeleteSamplers
         */
        void SamplerFree(SamplerID p_sampler) override;

        // ═══════════════════════════════════════════════════════════════════════
        // Vertex Format 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 创建顶点格式
         *
         * @param[in] p_vertex_attribs 顶点属性描述数组
         *
         * @retval VertexFormatID 顶点格式 ID，失败返回空 ID
         *
         * @note OpenGL 实现：glCreateVertexArrays + 配置顶点属性
         * @note VAO 在此创建并永久绑定，直到 VertexFormatFree
         */
        VertexFormatID VertexFormatCreate(
            VectorView<VertexAttribute> p_vertex_attribs) override;

        /**
         * @brief 释放顶点格式
         *
         * @param[in] p_vertex_format 顶点格式 ID
         *
         * @note OpenGL 实现：glDeleteVertexArrays
         */
        void VertexFormatFree(VertexFormatID p_vertex_format) override;

        // ═══════════════════════════════════════════════════════════════════════
        // Shader 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 从 GLSL 源码创建着色器
         *
         * @param[in] p_vertex_source    顶点着色器 GLSL 源码
         * @param[in] p_fragment_source 片段着色器 GLSL 源码
         * @param[in] p_uniforms       Uniform 描述数组
         * @param[in] p_push_constant_size Push Constant 大小（字节）
         *
         * @retval ShaderID 着色器 ID，编译/链接失败返回空 ID
         *
         * @note OpenGL 实现：glCreateShader + glShaderSource + glCompileShader +
         *       glCreateProgram + glAttachShader + glLinkProgram
         * @note Uniform 位置在创建时查询并缓存
         */
        ShaderID ShaderCreateFromGLSL(
            const char *p_vertex_source,
            const char *p_fragment_source,
            VectorView<ShaderUniform> p_uniforms,
            uint32_t p_push_constant_size) override;

        /**
         * @brief 释放着色器
         *
         * @param[in] p_shader 着色器 ID
         *
         * @note OpenGL 实现：glDeleteProgram + glDeleteShader
         */
        void ShaderFree(ShaderID p_shader) override;

        // ═══════════════════════════════════════════════════════════════════════
        // Uniform Set 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 创建 Uniform Set
         *
         * @param[in] p_uniforms  绑定 Uniform 数组
         * @param[in] p_shader   关联的着色器
         * @param[in] p_set_index Set 索引
         *
         * @retval UniformSetID Uniform Set ID，失败返回空 ID
         *
         * @note OpenGL 实现：记录纹理/Buffer 绑定信息
         * @note Uniform 位置已由 ShaderCreateFromGLSL 缓存
         */
        UniformSetID UniformSetCreate(
            VectorView<BoundUniform> p_uniforms,
            ShaderID p_shader,
            uint32_t p_set_index) override;

        /**
         * @brief 释放 Uniform Set
         *
         * @param[in] p_uniform_set Uniform Set ID
         */
        void UniformSetFree(UniformSetID p_uniform_set) override;

        // ═══════════════════════════════════════════════════════════════════════
        // RenderPass 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 创建渲染 Pass
         *
         * @param[in] p_attachments        附件描述数组
         * @param[in] p_subpasses         子 Pass 描述数组
         * @param[in] p_subpass_dependencies 子 Pass 依赖数组
         *
         * @retval RenderPassID 渲染 Pass ID，失败返回空 ID
         *
         * @note OpenGL 实现：仅记录附件信息，不创建 GL 对象
         */
        RenderPassID RenderPassCreate(
            VectorView<Attachment> p_attachments,
            VectorView<Subpass> p_subpasses,
            VectorView<SubpassDependency> p_subpass_dependencies) override;

        /**
         * @brief 释放渲染 Pass
         *
         * @param[in] p_render_pass 渲染 Pass ID
         */
        void RenderPassFree(RenderPassID p_render_pass) override;

        // ═══════════════════════════════════════════════════════════════════════
        // Framebuffer 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 创建帧缓冲
         *
         * @param[in] p_render_pass 渲染 Pass
         * @param[in] p_attachments 附件纹理 ID 数组
         * @param[in] p_width      宽度
         * @param[in] p_height     高度
         *
         * @retval FramebufferID 帧缓冲 ID，失败返回空 ID
         *
         * @note OpenGL 实现：glGenFramebuffers + glNamedFramebufferTexture
         */
        FramebufferID FramebufferCreate(
            RenderPassID p_render_pass,
            VectorView<TextureID> p_attachments,
            uint32_t p_width,
            uint32_t p_height) override;

        /**
         * @brief 释放帧缓冲
         *
         * @param[in] p_framebuffer 帧缓冲 ID
         *
         * @note OpenGL 实现：glDeleteFramebuffers
         */
        void FramebufferFree(FramebufferID p_framebuffer) override;

        // ═══════════════════════════════════════════════════════════════════════
        // Pipeline 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 创建渲染管线
         *
         * OpenGL 实现记录管线状态到结构体，不创建 GL 对象。
         *
         * @param[in] p_shader                着色器
         * @param[in] p_vertex_format         顶点格式
         * @param[in] p_render_primitive      渲染图元
         * @param[in] p_rasterization_state  光栅化状态
         * @param[in] p_multisample_state    多重采样状态
         * @param[in] p_depth_stencil_state  深度/模板状态
         * @param[in] p_blend_state         颜色混合状态
         * @param[in] p_dynamic_state        动态状态标志位
         * @param[in] p_render_pass          渲染 Pass
         *
         * @retval PipelineID 管线 ID，失败返回空 ID
         *
         * @note OpenGL 实现：仅记录状态到 GLPipelineState
         */
        PipelineID RenderPipelineCreate(
            ShaderID p_shader,
            VertexFormatID p_vertex_format,
            RenderPrimitive p_render_primitive,
            const PipelineRasterizationState &p_rasterization_state,
            const PipelineMultisampleState &p_multisample_state,
            const PipelineDepthStencilState &p_depth_stencil_state,
            const PipelineColorBlendState &p_blend_state,
            BitField<PipelineDynamicStateFlags> p_dynamic_state,
            RenderPassID p_render_pass) override;

        /**
         * @brief 释放管线
         *
         * @param[in] p_pipeline 管线 ID
         */
        void PipelineFree(PipelineID p_pipeline) override;

        // ═══════════════════════════════════════════════════════════════════════
        // CommandBuffer 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 创建命令缓冲
         *
         * 返回 GLCommandBuffer 实例，支持线程安全录制和 GL 上下文回放。
         *
         * @retval ICommandBuffer* 命令缓冲指针
         *
         * @note 使用 PagedAllocator 分配，调用者通过 CommandBufferFree() 释放
         */
        ICommandBuffer *CommandBufferCreate() override;

        /**
         * @brief 释放命令缓冲
         *
         * @param[in] p_command_buffer 命令缓冲指针（必须由本驱动创建）
         */
        void CommandBufferFree(ICommandBuffer *p_command_buffer) override;

        // ═══════════════════════════════════════════════════════════════════════
        // 命令录制 — RenderPass（立即模式）
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 开始渲染 Pass
         *
         * @param[in] p_render_pass  渲染 Pass
         * @param[in] p_framebuffer 帧缓冲
         * @param[in] p_clear_values 清除值数组
         * @param[in] p_rect_x      渲染区域 X
         * @param[in] p_rect_y      渲染区域 Y
         * @param[in] p_rect_w      渲染区域宽度
         * @param[in] p_rect_h      渲染区域高度
         *
         * @note OpenGL 实现：glBindFramebuffer + glClear + glViewport
         */
        void CommandBeginRenderPass(
            RenderPassID p_render_pass,
            FramebufferID p_framebuffer,
            VectorView<RenderPassClearValue> p_clear_values,
            int32_t p_rect_x, int32_t p_rect_y,
            uint32_t p_rect_w, uint32_t p_rect_h) override;

        /**
         * @brief 结束渲染 Pass
         *
         * @note OpenGL 实现：glBindFramebuffer(GL_FRAMEBUFFER, 0)
         */
        void CommandEndRenderPass() override;

        // ═══════════════════════════════════════════════════════════════════════
        // 命令录制 — 绑定
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 绑定渲染管线
         *
         * @param[in] p_pipeline 管线 ID
         *
         * @note OpenGL 实现：glUseProgram + 设置 GL 状态 + GLStateCache
         */
        void CommandBindRenderPipeline(PipelineID p_pipeline) override;

        /**
         * @brief 绑定 Uniform Set
         *
         * @param[in] p_uniform_set Uniform Set ID
         * @param[in] p_set_index  Set 索引
         *
         * @note OpenGL 实现：glUniform + glBindTexture + glBindBufferBase
         */
        void CommandBindUniformSet(UniformSetID p_uniform_set,
                                   uint32_t p_set_index) override;

        /**
         * @brief 绑定顶点缓冲区
         *
         * @param[in] p_buffers 顶点缓冲区 ID 数组
         * @param[in] p_offsets 偏移量数组
         * @param[in] p_count   缓冲区数量
         *
         * @note OpenGL 实现：glBindVertexBuffer
         */
        void CommandBindVertexBuffers(
            const BufferID *p_buffers,
            const uint64_t *p_offsets,
            uint32_t p_count) override;

        /**
         * @brief 绑定索引缓冲区
         *
         * @param[in] p_buffer 索引缓冲区 ID
         * @param[in] p_format 索引格式
         * @param[in] p_offset 偏移量（字节）
         *
         * @note OpenGL 实现：glBindBuffer + glVertexAttribPointer
         */
        void CommandBindIndexBuffer(BufferID p_buffer,
                                    IndexBufferFormat p_format,
                                    uint64_t p_offset) override;

        // ═══════════════════════════════════════════════════════════════════════
        // 命令录制 — 绘制
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 非索引绘制
         *
         * @param[in] p_vertex_count   顶点数量
         * @param[in] p_instance_count 实例数量
         * @param[in] p_base_vertex   起始顶点偏移
         * @param[in] p_first_instance 起始实例偏移
         *
         * @note OpenGL 实现：glDrawArrays / glDrawArraysInstanced
         */
        void CommandDraw(uint32_t p_vertex_count,
                         uint32_t p_instance_count,
                         uint32_t p_base_vertex,
                         uint32_t p_first_instance) override;

        /**
         * @brief 索引绘制
         *
         * @param[in] p_index_count    索引数量
         * @param[in] p_instance_count 实例数量
         * @param[in] p_first_index   起始索引偏移
         * @param[in] p_vertex_offset 顶点偏移
         * @param[in] p_first_instance 起始实例偏移
         *
         * @note OpenGL 实现：glDrawElements / glDrawElementsInstanced
         */
        void CommandDrawIndexed(uint32_t p_index_count,
                                uint32_t p_instance_count,
                                uint32_t p_first_index,
                                int32_t p_vertex_offset,
                                uint32_t p_first_instance) override;

        // ═══════════════════════════════════════════════════════════════════════
        // 命令录制 — 状态设置
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 设置视口
         *
         * @param[in] p_x      视口左下角 X
         * @param[in] p_y      视口左下角 Y
         * @param[in] p_width  视口宽度
         * @param[in] p_height 视口高度
         *
         * @note OpenGL 实现：glViewport
         */
        void CommandSetViewport(int32_t p_x, int32_t p_y,
                                uint32_t p_width, uint32_t p_height) override;

        /**
         * @brief 设置裁剪矩形
         *
         * @param[in] p_x      矩形左下角 X
         * @param[in] p_y      矩形左下角 Y
         * @param[in] p_width  矩形宽度
         * @param[in] p_height 矩形高度
         *
         * @note OpenGL 实现：glScissor
         */
        void CommandSetScissor(int32_t p_x, int32_t p_y,
                               uint32_t p_width, uint32_t p_height) override;

        /**
         * @brief 设置混合常量
         *
         * @param[in] p_r 红色分量
         * @param[in] p_g 绿色分量
         * @param[in] p_b 蓝色分量
         * @param[in] p_a 透明度分量
         *
         * @note OpenGL 实现：glBlendColor
         */
        void CommandSetBlendConstants(float p_r, float p_g,
                                      float p_b, float p_a) override;

        // ═══════════════════════════════════════════════════════════════════════
        // 命令录制 — 数据传输
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 清除缓冲区
         *
         * @param[in] p_buffer 缓冲区 ID
         * @param[in] p_offset 起始偏移（字节）
         * @param[in] p_size   清除大小（字节）
         *
         * @note OpenGL 实现：glClearNamedBufferData
         */
        void CommandClearBuffer(BufferID p_buffer,
                                uint64_t p_offset,
                                uint64_t p_size) override;

        /**
         * @brief 复制缓冲区
         *
         * @param[in] p_src_buffer 源缓冲区
         * @param[in] p_dst_buffer 目标缓冲区
         * @param[in] p_regions   复制区域数组
         *
         * @note OpenGL 实现：glCopyNamedBufferSubData
         */
        void CommandCopyBuffer(BufferID p_src_buffer,
                               BufferID p_dst_buffer,
                               VectorView<BufferCopyRegion> p_regions) override;

        /**
         * @brief 清除颜色纹理
         *
         * @param[in] p_texture     纹理 ID
         * @param[in] p_color_r    清除颜色 R
         * @param[in] p_color_g    清除颜色 G
         * @param[in] p_color_b    清除颜色 B
         * @param[in] p_color_a    清除颜色 A
         * @param[in] p_subresources 子资源范围
         *
         * @note OpenGL 实现：glClearTexSubImage
         */
        void CommandClearColorTexture(
            TextureID p_texture,
            float p_color_r, float p_color_g,
            float p_color_b, float p_color_a,
            const TextureSubresourceRange &p_subresources) override;

        /**
         * @brief 清除深度/模板纹理
         *
         * @param[in] p_texture     纹理 ID
         * @param[in] p_depth       清除深度值
         * @param[in] p_stencil     清除模板值
         * @param[in] p_subresources 子资源范围
         *
         * @note OpenGL 实现：glClearTexSubImage
         */
        void CommandClearDepthStencilTexture(
            TextureID p_texture,
            float p_depth, uint32_t p_stencil,
            const TextureSubresourceRange &p_subresources) override;

        // ═══════════════════════════════════════════════════════════════════════
        // 帧同步
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 开始帧段
         *
         * @param[in] p_frame_index   帧索引
         * @param[in] p_frames_drawn 已绘制帧数
         *
         * @note OpenGL 实现：更新 frame_index_ 和 frames_drawn_
         */
        void BeginSegment(uint32_t p_frame_index,
                          uint32_t p_frames_drawn) override;

        /**
         * @brief 结束帧段
         *
         * @note OpenGL 实现：SwapBuffers 由 IRenderingContextDriver 管理
         */
        void EndSegment() override;

        // ═══════════════════════════════════════════════════════════════════════
        // 查询
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 查询设备限制
         *
         * @param[in] p_limit 限制项
         *
         * @return 限制值
         */
        uint64_t LimitGet(Limit p_limit) override;

        /**
         * @brief 查询设备是否支持某特性
         *
         * @param[in] p_feature 特性项
         *
         * @retval true 支持
         * @retval false 不支持
         */
        bool HasFeature(Features p_feature) override;

        /**
         * @brief 获取设备能力描述
         *
         * @return Capabilities 常引用
         */
        const Capabilities &GetCapabilities() const override;

        /**
         * @brief 获取 API 名称
         *
         * @return API 名称字符串（如 "OpenGL 3.3 Core"）
         */
        const char *GetApiName() const override;

        /**
         * @brief 获取管线缓存 UUID
         *
         * @return UUID 字符串
         *
         * @retval "" OpenGL 无 PSO 缓存，返回空字符串
         */
        const char *GetPipelineCacheUuid() const override;

        // ═══════════════════════════════════════════════════════════════════════
        // SwapChain 管理
        // ═══════════════════════════════════════════════════════════════════════

        /**
         * @brief 创建 SwapChain
         *
         * 记录 RCD Surface 与 GL Context/Surface 的绑定关系。
         * 从 RCD 的 SurfaceInfo 查询 gl_surface ID。
         *
         * @param[in] p_surface RCD Surface ID
         * @param[in] p_context RCD Context ID
         *
         * @return SwapChain ID
         * @retval kInvalidSwapChainId 创建失败
         */
        SwapChainID SwapChainCreate(IRenderingContextDriver::SurfaceID p_surface,
                                    IRenderingContextDriver::ContextID p_context) override;

        /**
         * @brief 销毁 SwapChain
         *
         * OpenGL 无 SwapChain 概念，仅清理内部簿记。
         *
         * @param[in] p_swapchain SwapChain ID
         */
        void SwapChainDestroy(SwapChainID p_swapchain) override;

        /**
         * @brief 获取 SwapChain 下一帧
         *
         * 调用 IGLManager::MakeCurrent() 绑定 Context 到 Surface。
         *
         * @param[in] p_swapchain SwapChain ID
         *
         * @return Error::kOK 绑定成功
         */
        Error SwapChainAcquire(SwapChainID p_swapchain) override;

        /**
         * @brief 呈现 SwapChain 渲染结果
         *
         * 调用 IGLManager::SwapBuffers() 交换前后缓冲区。
         *
         * @param[in] p_swapchain SwapChain ID
         */
        void SwapChainPresent(SwapChainID p_swapchain) override;

    private:
        /**
         * @brief 查询 GL 限制并填充 limits_ 数组
         *
         * @pre GL 上下文已 MakeCurrent
         */
        void QueryLimits();

        /**
         * @brief 编译单个着色器
         *
         * @param[in] p_source GLSL 源码
         * @param[in] p_type   着色器类型（GL_VERTEX_SHADER / GL_FRAGMENT_SHADER）
         *
         * @return 着色器对象 ID，失败返回 0
         */
        static uint32_t CompileShader(const char *p_source, uint32_t p_type);

        /**
         * @brief 链接着色器程序
         *
         * @param[in] p_vertex_shader   顶点着色器 ID
         * @param[in] p_fragment_shader 片段着色器 ID
         *
         * @return 程序对象 ID，失败返回 0
         */
        static uint32_t LinkProgram(uint32_t p_vertex_shader,
                                    uint32_t p_fragment_shader);

        /**
         * @brief 将 DataFormat 映射到 GL 内部格式
         */
        static uint32_t DataFormatToGLInternalFormat(DataFormat p_format);

        /**
         * @brief 将 DataFormat 映射到 GL 类型
         */
        static uint32_t DataFormatToGLType(DataFormat p_format);

        /**
         * @brief 将 DataFormat 映射到 GL 格式
         */
        static uint32_t DataFormatToGLFormat(DataFormat p_format);

        /**
         * @brief 将 RenderPrimitive 映射到 GL 图元模式
         */
        static uint32_t RenderPrimitiveToGLMode(RenderPrimitive p_primitive);

        /**
         * @brief 将 BlendFactor 映射到 GL 混合因子
         */
        static uint32_t BlendFactorToGL(BlendFactor p_factor);

        /**
         * @brief 将 CompareOperator 映射到 GL 比较函数
         */
        static uint32_t CompareOperatorToGL(CompareOperator p_op);

        /**
         * @brief 将 SamplerFilter 映射到 GL 过滤模式
         */
        static uint32_t SamplerFilterToGL(SamplerFilter p_filter);

        /**
         * @brief 将 SamplerRepeatMode 映射到 GL 寻址模式
         */
        static uint32_t SamplerRepeatModeToGL(SamplerRepeatMode p_mode);

        /**
         * @brief 获取 DataFormat 的像素大小（字节）
         */
        static uint32_t DataFormatGetPixelSize(DataFormat p_format);

        IRenderingContextDriver *context_driver_ = nullptr;
        IGLManager *gl_manager_ = nullptr;
        bool initialized_ = false;

        GLStateCache state_cache_;

        LocalVector<GLSwapChain> swap_chains_;

        PagedAllocator<GLShaderInfo> shader_allocator_;
        PagedAllocator<GLPipelineState> pipeline_allocator_;
        PagedAllocator<GLFramebufferInfo> framebuffer_allocator_;
        PagedAllocator<GLRenderPassInfo> render_pass_allocator_;
        PagedAllocator<GLUniformSetInfo> uniform_set_allocator_;
        PagedAllocator<GLVertexFormatInfo> vertex_format_allocator_;
        PagedAllocator<GLCommandBuffer> command_buffer_allocator_;

        uint64_t limits_[static_cast<uint32_t>(Limit::kMax)] = {};
        Capabilities capabilities_;
        char api_name_[64] = {};
        char pipeline_cache_uuid_[37] = {};

        uint32_t frame_index_ = 0;
        uint32_t frames_drawn_ = 0;

        static constexpr uint32_t kMaxTextureUnits = 16;
    };

} // namespace arhud