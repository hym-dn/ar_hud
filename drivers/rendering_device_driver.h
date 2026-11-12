/**
 * @file rendering_device_driver.h
 * @brief 渲染设备驱动抽象接口
 *
 * IRenderingDeviceDriver 是 GPU 资源操作的直接封装层，位于渲染架构的驱动层。
 * 所有 GPU 资源（Buffer/Texture/Sampler/Shader/Pipeline/Framebuffer）
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
 *   - 去掉 CommandQueue/CommandPool（命令直录，GL 立即模式）
 *   - 去掉 Compute Pipeline（HUD 场景暂不需要）
 *   - 去掉 Pipeline Cache（OpenGL 无 PSO 缓存概念）
 *   - 去掉 Timestamp Query（简化，后续可按需添加）
 *   - 去掉 Debug Labels/Breadcrumbs（简化，后续可按需添加）
 *   - 命名遵循 ARHud PascalCase + k 前缀约定
 *
 * @par 命令模式（单一：命令直录族）
 *   **命令直录族**（`CommandXxx()`）：渲染器经 RD DrawList 直调本驱动，
 *   命令立即执行到 GL API（渲染线程同线程"录+放"）。
 *
 *   Route A 遗留的 **CommandBuffer 模式**（`CommandBufferCreate()` 录制 →
 *   渲染线程回放）已整体移除：B3 渲染搬线程后跨线程职能退役，录制-回放层
 *   在本后端退化为直通冗余 —— 对齐 Godot GLES3（无 RD / 无录制层，命令直录）。
 *   未来 Vulkan 后端的原生命令缓冲由该驱动**内部**持有（`VkCommandBuffer`），
 *   不经 RD↔驱动契约暴露录制对象。
 *
 * @par B3 呈现路径下各族状态表（2026-08-30 快照）
 *
 *   呈现架构已定案：渲染线程单 Context 逐屏切 drawable 渲到窗口
 *   backbuffer，per-screen PresentThread 承担呈现（SwapBuffers +
 *   DwmFlush / 生产平台 EGLImage 导出 + compositor）。据此各接口
 *   族的活/退/占位状态（防止后人把过渡态当设计）：
 *
 *   | 接口族                | 状态   | 说明 |
 *   |-----------------------|--------|------|
 *   | Buffer/Texture/       | 活     | 资源 CRUD 与呈现路径无关，Vulkan 1:1 |
 *   | Sampler/VertexFormat/ |        | 映射，契约稳定 |
 *   | UniformSet/RenderPass/|        | |
 *   | Framebuffer/Pipeline  |        | |
 *   | Command 直录族 ×21    | 活     | B3 后主路径（渲染线程同线程录+放） |
 *   | CommandBuffer 族      | 已退   | Route A 遗留录制/回放层已移除（B3 跨  |
 *   |                       |        | 线程职能退役 + 直通冗余）；契约即命令直
 * | |                       |        | 录族；Vulkan 命令缓冲由驱动内部持有，  |
 *   |                       |        | 不暴露录制对象（对齐 Godot GLES3）    |
 *   | SwapChain 族          | 退中   | GL 实现走 IGLContextManager 的        |
 *   |                       |        | MakeCurrent+SwapBuffers；呈现职责已移交
 * | |                       |        | IFramePresenter，引擎不再调用。契约保留
 * | |                       |        | 为 Vulkan 映射占位（VkSwapchainKHR）， |
 *   |                       |        | B5 时 GL 实现标 deprecated |
 *   | Fence/Semaphore 族    | 占位   | GL 空实现（返回无效 ID），Vulkan 落地 |
 *   |                       |        | 时激活（VkFence/VkSemaphore 映射）|
 *   | FramebufferReadback   | 偏差   | 模式 R（回读呈现）专用口子，泄漏进后端 |
 *   |                       |        | 契约；生产零拷贝路径不用。B3 后应移出 |
 *   |                       |        | 契约（降为 GL 实现私有或归            |
 *   |                       |        | IFramePresenter 家族）—— 已立项待执行 |
 *   | Texture*External 族   | 活     | 模式 X（零回读上屏）：显示侧分配的帧  |
 *   |                       |        | 缓冲别名成本后端可写纹理（GL 实现走    |
 *   |                       |        | WGL_NV_DX_interop2）；Begin/EndWrite   |
 *   |                       |        | 是唯一跨 API 同步点。Vulkan 映射     |
 *   |                       |        | VK_EXT_external_memory_* 原生       |
 *   | ShaderCreateFromSource  | 已改   | 治理 P1 已执行（2026-09-27）：改名 + |
 *   |                       |        | 加 ShaderSourceFormat 参数（kGLSL/     |
 *   |                       |        | kSPIRV），GLSL 命名泄漏已消除 |
 *
 *   判定标准：**活** = 当前与目标架构都用；**退中** = 目标架构中职能
 *   转移但契约保留（Vulkan 映射价值）；**占位** = 仅后端差异空实现；
 *   **偏差** = 与目标架构相悖、待移出的实现细节泄漏。
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
 * @version 1.1
 * @date 2026-08-30
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_DRIVERS_RENDERING_DEVICE_DRIVER_H_
#define ARHUD_DRIVERS_RENDERING_DEVICE_DRIVER_H_

#include "rendering_context_driver.h"
#include "rendering_device_commons.h"

namespace arhud {

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
class IRenderingDeviceDriver {
 public:
  virtual ~IRenderingDeviceDriver() = default;

  // ═════════════════════════════════════════════════════════

  // 生命周期
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 初始化设备驱动
   *
   * 查询 GPU 能力和限制，初始化内部状态。
   * 必须在创建任何资源之前调用。
   *
   * @param[in] p_device_index  设备索引（由 RCD 枚举）
   * @param[in] p_frame_count   帧缓冲数量（用于 S     taging Buffer 管理）
   *
   * @retval Error::kOK 初始化成功
   * @retval Error::kFailed 初始化失败
   *
   * @pre RCD 已初始化，GL 上下文已 MakeCurrent
   */
  virtual Error Initialize(uint32 p_device_index, uint32 p_frame_count) = 0;

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
  virtual BufferID BufferCreate(uint64 p_size,
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
  virtual uint8* BufferMap(BufferID p_buffer) = 0;

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
  virtual uint64 BufferGetAllocationSize(BufferID p_buffer) = 0;

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
  virtual TextureID TextureCreate(const TextureFormat& p_format,
                                  const TextureView& p_view) = 0;

  /**
   * @brief 释放纹理
   *
   * @param[in] p_texture 纹理 ID
   */
  virtual void TextureFree(TextureID p_texture) = 0;

  /**
   * @brief 生成纹理 Mipmap 链
   *
   * 从基础层级自动生成完整 Mipmap 链。
   * OpenGL 实现：glGenerateMipmap。
   *
   * @param[in] p_texture 纹理 ID
   */
  virtual void TextureGenerateMipmaps(TextureID p_texture) = 0;

  /**
   * @brief 获取纹理分配大小
   *
   * @param[in] p_texture 纹理 ID
   *
   * @return 纹理分配大小（字节）
   */
  virtual uint64 TextureGetAllocationSize(TextureID p_texture) = 0;

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
  // Texture 管理——外部资源导入（零拷贝上屏，Mode X）
  //
  // 帧缓冲归显示侧（platform::ISharedFramePool）分配，渲染侧把它
  // 别名成本后端的可写纹理，于是"一帧的像素"从不离开显存：
  // 上屏线程只提交该显存，不读 CPU。默认实现全部不可用，
  // 调用方据 SupportsExternalTexture() 回落 Mode R。
  //
  // 时序契约（后端必须实现为显式同步点）：
  //   BeginWrite → 写 → EndWrite，且 Begin/End 与 GPU 命令同线程；
  //   EndWrite 返回后，显示侧才可读该资源。
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 本后端是否支持外部 GPU 资源导入
   *
   * @par 默认 false；GL 后端在 WGL_NV_DX_interop2 入口点可装载
   *      且与帧池的 GPU device 同适配器时为 true（探测发生在
   *      首次 TextureCreateExternal，此前返回保守值 false）。
   *
   * @return true 可调用 TextureCreateExternal
   */
  virtual bool SupportsExternalTexture() const {
    return false;
  }

  /**
   * @brief 以外部 GPU 资源创建纹理（不分配显存）
   *
   * 返回的 TextureID 与 TextureCreate 的产物同构，可直接作为
   * Framebuffer 附件（可挂 DEPTH 之外的颜色附件）。
   *
   * @param[in] p_source 外部资源描述（须 IsValid()）
   *
   * @return 纹理 ID；不支持、参数无效或别名建立失败返回空 ID
   *
   * @pre 调用线程持有当前 GL/图形上下文
   */
  virtual TextureID TextureCreateExternal(
      const ExternalTextureSource& p_source) {
    (void)p_source;
    return TextureID();
  }

  /**
   * @brief 开始写外部导入纹理（跨 API 同步点：显示侧 → 渲染侧）
   *
   * @param[in] p_texture TextureCreateExternal 的产物
   *
   * @return true 已锁定，可写；false 锁定失败（本帧须跳过或回落）
   */
  virtual bool TextureExternalBeginWrite(TextureID p_texture) {
    (void)p_texture;
    return false;
  }

  /**
   * @brief 结束写外部导入纹理（渲染侧 → 显示侧，归还所有权）
   *
   * @param[in] p_texture TextureCreateExternal 的产物
   */
  virtual void TextureExternalEndWrite(TextureID p_texture) {
    (void)p_texture;
  }

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
  virtual SamplerID SamplerCreate(const SamplerState& p_state) = 0;

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
   * @brief 从源码创建着色器
   *
   * 编译并链接多个着色器阶段。
   *
   * @param[in] p_stage_sources   着色器阶段源码数组
   * @param[in] p_source_format   源码格式（OpenGL 仅支持 kGLSL）
   * @param[in] p_uniforms        着色器 Uniform 描述数组
   * @param[in] p_push_constant_size Push Constant 大小（字节），0 表示不使用
   *
   * @retval ShaderID 着色器 ID，编译/链接失败返回空 ID
   *
   * @note OpenGL 实现会在编译时查询所有 Uniform 位置并缓存
   */
  virtual ShaderID ShaderCreateFromSource(
      VectorView<ShaderStageSource> p_stage_sources,
      ShaderSourceFormat p_source_format, VectorView<ShaderUniform> p_uniforms,
      uint32 p_push_constant_size) = 0;

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
  virtual UniformSetID UniformSetCreate(VectorView<BoundUniform> p_uniforms,
                                        ShaderID p_shader,
                                        uint32 p_set_index) = 0;

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
      VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses,
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
  virtual FramebufferID FramebufferCreate(RenderPassID p_render_pass,
                                          VectorView<TextureID> p_attachments,
                                          uint32 p_width, uint32 p_height) = 0;

  /**
   * @brief 释放帧缓冲
   *
   * @param[in] p_framebuffer 帧缓冲 ID
   */
  virtual void FramebufferFree(FramebufferID p_framebuffer) = 0;

  /** @brief 回读 framebuffer 像素到 CPU 缓冲（B3-M0.3：FBO 导出供平台上屏） */
  virtual void FramebufferReadback(FramebufferID p_framebuffer, uint32 p_x,
                                   uint32 p_y, uint32 p_width, uint32 p_height,
                                   void* p_dst) {
    (void)p_framebuffer;
    (void)p_x;
    (void)p_y;
    (void)p_width;
    (void)p_height;
    (void)p_dst;
  }

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
      ShaderID p_shader, VertexFormatID p_vertex_format,
      RenderPrimitive p_render_primitive,
      const PipelineRasterizationState& p_rasterization_state,
      const PipelineMultisampleState& p_multisample_state,
      const PipelineDepthStencilState& p_depth_stencil_state,
      const PipelineColorBlendState& p_blend_state,
      BitField<PipelineDynamicStateFlags> p_dynamic_state,
      RenderPassID p_render_pass) = 0;

  /**
   * @brief 创建计算管线
   *
   * OpenGL 实现记录计算着色器程序到 GLPipelineState 结构体，
   * 不创建任何 GL 对象。在 CommandBindRenderPipeline() 时设置 GL 状态。
   *
   * @param[in] p_shader 计算着色器（必须包含 kCompute 阶段）
   *
   * @retval PipelineID 管线 ID，失败返回空 ID
   *
   * @note 需要 OpenGL 4.3+ 支持
   */
  virtual PipelineID ComputePipelineCreate(ShaderID p_shader) {
    (void)p_shader;
    return PipelineID();
  }

  /**
   * @brief 释放管线
   *
   * @param[in] p_pipeline 管线 ID
   */
  virtual void PipelineFree(PipelineID p_pipeline) = 0;

  // ═══════════════════════════════════════════════════════════════════
  // 命令录制 — 渲染 Pass（立即模式）
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
      RenderPassID p_render_pass, FramebufferID p_framebuffer,
      VectorView<RenderPassClearValue> p_clear_values, uint32 p_clear_mask,
      int32 p_rect_x, int32 p_rect_y, uint32 p_rect_w, uint32 p_rect_h) = 0;

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
                                     uint32 p_set_index,
                                     uint32 p_dynamic_offset = 0) = 0;

  /**
   * @brief 绑定顶点缓冲区
   *
   * @param[in] p_buffers  顶点缓冲区 ID 数组
   * @param[in] p_offsets  偏移量数组
   * @param[in] p_count    缓冲区数量
   */
  virtual void CommandBindVertexBuffers(const BufferID* p_buffers,
                                        const uint64* p_offsets,
                                        uint32 p_count) = 0;

  /**
   * @brief 绑定索引缓冲区
   *
   * @param[in] p_buffer 索引缓冲区 ID
   * @param[in] p_format 索引格式
   * @param[in] p_offset 偏移量（字节）
   */
  virtual void CommandBindIndexBuffer(BufferID p_buffer,
                                      IndexBufferFormat p_format,
                                      uint64 p_offset) = 0;

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
  virtual void CommandDraw(uint32 p_vertex_count, uint32 p_instance_count,
                           uint32 p_base_vertex, uint32 p_first_instance) = 0;

  /**
   * @brief 索引绘制
   *
   * @param[in] p_index_count   索引数量
   * @param[in] p_instance_count 实例数量
   * @param[in] p_first_index   起始索引偏移
   * @param[in] p_vertex_offset 顶点偏移
   * @param[in] p_first_instance 起始实例偏移
   */
  virtual void CommandDrawIndexed(uint32 p_index_count, uint32 p_instance_count,
                                  uint32 p_first_index, int32 p_vertex_offset,
                                  uint32 p_first_instance) = 0;

  /**
   * @brief 非索引实例化绘制
   *
   * 使用已绑定的实例缓冲区进行实例化绘制。
   *
   * @param[in] p_instance_count 实例数量
   * @param[in] p_base_instance  起始实例偏移（GL 3.3 基线被忽略，见 @note）
   *
   * @note 能力可选：基类为 no-op 默认实现，不支持的后端可省略覆写。
   * @note OpenGL 实现：glDrawArraysInstanced（GL 3.3 基线不带
   *       BaseInstance 形态）；p_base_instance 需 ARB_base_instance
   *       （GL 4.2+），当前基线被忽略。
   */
  virtual void CommandDrawInstanced(uint32 p_instance_count,
                                    uint32 p_base_instance) {
    (void)p_instance_count;
    (void)p_base_instance;
  }

  /**
   * @brief 索引实例化绘制
   *
   * 使用已绑定的实例缓冲区和索引缓冲区进行实例化绘制。
   *
   * @param[in] p_instance_count 实例数量
   * @param[in] p_base_instance  起始实例偏移（GL 3.3 基线被忽略，见 @note）
   *
   * @note 能力可选：基类为 no-op 默认实现，不支持的后端可省略覆写。
   * @note OpenGL 实现：glDrawElementsInstanced（GL 3.3 基线不带
   *       BaseInstance 形态）；p_base_instance 需 ARB_base_instance
   *       （GL 4.2+），当前基线被忽略。
   */
  virtual void CommandDrawIndexedInstanced(uint32 p_instance_count,
                                           uint32 p_base_instance) {
    (void)p_instance_count;
    (void)p_base_instance;
  }

  // ═══════════════════════════════════════════════════════════════════
  // 命令录制 — 计算
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 派发计算着色器
   *
   * @param[in] p_x_groups X 维度工作组数量
   * @param[in] p_y_groups Y 维度工作组数量
   * @param[in] p_z_groups Z 维度工作组数量
   *
   * @note OpenGL 实现：glDispatchCompute（需要 OpenGL 4.3+）
   */
  virtual void CommandDispatch(uint32 p_x_groups, uint32 p_y_groups,
                               uint32 p_z_groups) {
    (void)p_x_groups;
    (void)p_y_groups;
    (void)p_z_groups;
  }

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
  virtual void CommandSetViewport(int32 p_x, int32 p_y, uint32 p_width,
                                  uint32 p_height) = 0;

  /**
   * @brief 设置裁剪矩形
   *
   * @param[in] p_x      矩形左下角 X
   * @param[in] p_y      矩形左下角 Y
   * @param[in] p_width  矩形宽度
   * @param[in] p_height 矩形高度
   */
  virtual void CommandSetScissor(int32 p_x, int32 p_y, uint32 p_width,
                                 uint32 p_height) = 0;

  /**
   * @brief 设置混合常量
   *
   * @param[in] p_r 红色分量
   * @param[in] p_g 绿色分量
   * @param[in] p_b 蓝色分量
   * @param[in] p_a 透明度分量
   */
  virtual void CommandSetBlendConstants(float p_r, float p_g, float p_b,
                                        float p_a) = 0;

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
  virtual void CommandClearBuffer(BufferID p_buffer, uint64 p_offset,
                                  uint64 p_size) = 0;

  /**
   * @brief 拷贝缓冲区
   *
   * @param[in] p_src_buffer 源缓冲区
   * @param[in] p_dst_buffer 目标缓冲区
   * @param[in] p_regions    拷贝区域数组
   */
  virtual void CommandCopyBuffer(BufferID p_src_buffer, BufferID p_dst_buffer,
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
      BufferID p_src_buffer, TextureID p_dst_texture,
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
      TextureID p_texture, float p_color_r, float p_color_g, float p_color_b,
      float p_color_a, const TextureSubresourceRange& p_subresources) = 0;

  /**
   * @brief 清除深度/模板纹理
   *
   * @param[in] p_texture     纹理 ID
   * @param[in] p_depth       清除深度值
   * @param[in] p_stencil     清除模板值
   * @param[in] p_subresources 子资源范围
   */
  virtual void CommandClearDepthStencilTexture(
      TextureID p_texture, float p_depth, uint32 p_stencil,
      const TextureSubresourceRange& p_subresources) = 0;

  /**
   * @brief 更新缓冲区数据
   *
   * 将 CPU 内存数据写入 GPU 缓冲区的指定范围。
   *
   * @param[in] p_dst_buffer 目标缓冲区 ID
   * @param[in] p_offset     写入偏移（字节）
   * @param[in] p_size       写入大小（字节）
   * @param[in] p_data       源数据指针
   */
  virtual void CommandBufferUpdate(BufferID p_dst_buffer, uint32 p_offset,
                                   uint32 p_size, const void* p_data) = 0;

  // ═══════════════════════════════════════════════════════════════════
  // 帧同步
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 开始帧段
   *
   * @param[in] p_frame_index 帧索引
   * @param[in] p_frames_drawn 已绘制帧数
   */
  virtual void BeginSegment(uint32 p_frame_index, uint32 p_frames_drawn) = 0;

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
  virtual uint64 LimitGet(Limit p_limit) = 0;

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
  virtual const Capabilities& GetCapabilities() const = 0;

  /**
   * @brief 获取 API 名称
   *
   * @return API 名称字符串（如 "OpenGL 3.3"）
   */
  virtual const char* GetApiName() const = 0;

  /**
   * @brief 获取管线缓存 UUID
   *
   * @return UUID 字符串
   *
   * @note OpenGL 实现返回空字符串（无 PSO 缓存）
   */
  virtual const char* GetPipelineCacheUuid() const = 0;

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
  using SwapChainID = uint64;

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
   *   SwapChainAcquire 时调用 IGLContextManager::MakeCurrent。
   *
   * @par Vulkan 实现（未来）
   *   创建 VkSwapchainKHR，关联 VkImage 队列。
   */
  virtual SwapChainID SwapChainCreate(
      IRenderingContextDriver::SurfaceID p_surface,
      IRenderingContextDriver::ContextID p_context) {
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
  virtual void SwapChainDestroy(SwapChainID p_swapchain) {
    (void)p_swapchain;
  }

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
   *   调用 IGLContextManager::MakeCurrent() 绑定 Context 到 Surface。
   *
   * @par Vulkan 实现（未来）
   *   调用 vkAcquireNextImageKHR 获取下一帧图像索引。
   */
  virtual Error SwapChainAcquire(SwapChainID p_swapchain) {
    (void)p_swapchain;
    return Error::kOK;
  }

  /**
   * @brief 呈现 SwapChain 的渲染结果
   *
   * 在渲染完成后调用，将渲染结果呈现到屏幕。
   *
   * @param[in] p_swapchain SwapChain ID
   *
   * @par OpenGL 实现
   *   调用 IGLContextManager::SwapBuffers() 交换前后缓冲区。
   *
   * @par Vulkan 实现（未来）
   *   调用 vkQueuePresentKHR 呈现渲染结果。
   */
  virtual void SwapChainPresent(SwapChainID p_swapchain) {
    (void)p_swapchain;
  }

  /**
   * @brief 重建 SwapChain（窗口 resize 时调用）
   *
   * 当窗口尺寸变化时，需要重建 SwapChain 以匹配新的尺寸。
   *
   * @param[in] p_swapchain SwapChain ID
   * @param[in] p_width     新宽度
   * @param[in] p_height    新高度
   *
   * @note OpenGL 实现为空操作（GL 默认 FBO 自动适配窗口尺寸）
   * @note Vulkan 实现需要销毁旧 SwapChain 并创建新的
   */
  virtual void SwapChainResize(SwapChainID p_swapchain, uint32 p_width,
                               uint32 p_height) {
    (void)p_swapchain;
    (void)p_width;
    (void)p_height;
  }

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
  using SemaphoreID = uint64;

  /** @brief 无效信号量 ID */
  static constexpr SemaphoreID kInvalidSemaphoreId = 0;

  /**
   * @brief GPU 栅栏 ID 类型
   *
   * 用于 GPU-CPU 同步（等待 GPU 命令完成）。
   * Vulkan: VkFence 的封装。
   * OpenGL: 不需要（glFinish 可替代，但性能差）。
   */
  using FenceID = uint64;

  /** @brief 无效栅栏 ID */
  static constexpr FenceID kInvalidFenceId = 0;

  /**
   * @brief 管线阶段标志位
   *
   * 对齐 Vulkan VkPipelineStageFlags，用于 QueueSubmit 的等待阶段。
   * OpenGL 实现忽略此参数（GL 命令隐式有序）。
   */
  enum class PipelineStageBits : uint32 {
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
  struct QueueSubmitInfo {
    CommandBufferID command_buffer;                      ///< 命令缓冲
    SemaphoreID wait_semaphore = kInvalidSemaphoreId;    ///< 等待的信号量
    SemaphoreID signal_semaphore = kInvalidSemaphoreId;  ///< 完成后触发的信号量
    PipelineStageBits wait_stage = PipelineStageBits::kTopOfPipe;  ///< 等待阶段
  };

  /**
   * @brief 队列呈现信息
   *
   * 描述一次 Present 操作的同步参数。
   * Vulkan: 对应 VkPresentInfoKHR 的核心字段。
   * OpenGL: 忽略（SwapBuffers 隐式同步）。
   */
  struct QueuePresentInfo {
    uint32 surface = UINT32_MAX;                       ///< 目标表面 ID
    SemaphoreID wait_semaphore = kInvalidSemaphoreId;  ///< 等待的信号量
  };

  /**
   * @brief 创建 GPU 信号量
   *
   * @return 信号量 ID
   * @retval kInvalidSemaphoreId 创建失败或 OpenGL 不支持
   *
   * @note OpenGL 实现始终返回 kInvalidSemaphoreId
   */
  virtual SemaphoreID SemaphoreCreate() {
    return kInvalidSemaphoreId;
  }

  /**
   * @brief 销毁 GPU 信号量
   *
   * @param[in] p_semaphore 信号量 ID
   *
   * @note OpenGL 实现为空操作
   */
  virtual void SemaphoreDestroy(SemaphoreID p_semaphore) {
    (void)p_semaphore;
  }

  /**
   * @brief 创建 GPU 栅栏
   *
   * @param[in] p_signaled 是否创建为已触发状态
   *
   * @return 栅栏 ID
   * @retval kInvalidFenceId 创建失败
   *
   * @note OpenGL 实现使用 glFenceSync
   */
  virtual FenceID FenceCreate(bool p_signaled = false) {
    (void)p_signaled;
    return kInvalidFenceId;
  }

  /**
   * @brief 销毁 GPU 栅栏
   *
   * @param[in] p_fence 栅栏 ID
   *
   * @note OpenGL 实现使用 glDeleteSync
   */
  virtual void FenceDestroy(FenceID p_fence) {
    (void)p_fence;
  }

  /**
   * @brief 重置 GPU 栅栏
   *
   * @param[in] p_fence 栅栏 ID
   *
   * @note OpenGL GLsync 不支持 reset，需销毁后重建
   */
  virtual void FenceReset(FenceID p_fence) {
    (void)p_fence;
  }

  /**
   * @brief 查询 GPU 栅栏是否已触发
   *
   * @param[in] p_fence 栅栏 ID
   *
   * @return true 已触发
   */
  virtual bool FenceIsSignaled(FenceID p_fence) {
    (void)p_fence;
    return true;
  }

  /**
   * @brief 等待 GPU 栅栏触发
   *
   * @param[in] p_fence 栅栏 ID
   * @param[in] p_timeout_ns 超时时间（纳秒）
   *
   * @return true 栅栏已触发
   *
   * @note OpenGL 实现使用 glClientWaitSync
   */
  virtual bool FenceWait(FenceID p_fence, uint64 p_timeout_ns) {
    (void)p_fence;
    (void)p_timeout_ns;
    return true;
  }

  // ═══════════════════════════════════════════════════════════════════════
  // GPU 时间戳查询
  // ═══════════════════════════════════════════════════════════════════════

  /**
   * @brief 时间戳查询 ID 类型
   *
   * 用于 GPU 执行时间测量。
   * OpenGL: glQueryCounter(GL_TIMESTAMP) 的查询对象。
   * Vulkan: VkQueryPool 的查询索引。
   */
  using TimestampQueryID = uint64;

  /** @brief 无效时间戳查询 ID */
  static constexpr TimestampQueryID kInvalidTimestampQueryId = 0;

  /**
   * @brief 创建时间戳查询对象
   *
   * @return 查询对象 ID，失败返回 kInvalidTimestampQueryId
   *
   * @note OpenGL 实现：glGenQueries(1, &id)
   */
  virtual TimestampQueryID TimestampQueryCreate() {
    return kInvalidTimestampQueryId;
  }

  /**
   * @brief 销毁时间戳查询对象
   *
   * @param[in] p_query 查询对象 ID
   *
   * @note OpenGL 实现：glDeleteQueries(1, &id)
   */
  virtual void TimestampQueryDestroy(TimestampQueryID p_query) {
    (void)p_query;
  }

  /**
   * @brief 记录当前 GPU 时间戳
   *
   * 在命令流中插入时间戳标记，用于测量 GPU 执行时间。
   * 通常成对使用：Begin 前记录一次，End 后记录一次，
   * 两次时间戳之差即为 GPU 执行时间。
   *
   * @param[in] p_query 查询对象 ID
   *
   * @note OpenGL 实现：glQueryCounter(id, GL_TIMESTAMP)
   * @note 需要 OpenGL 3.3+
   */
  virtual void TimestampQueryRecord(TimestampQueryID p_query) {
    (void)p_query;
  }

  /**
   * @brief 获取时间戳查询结果（纳秒）
   *
   * @param[in] p_query 查询对象 ID
   *
   * @return GPU 时间戳（纳秒），查询不可用时返回 0
   *
   * @note OpenGL 实现：glGetQueryObjectui64v(id, GL_QUERY_RESULT, &result)
   * @note 此调用会阻塞等待 GPU 结果，不应在热路径中使用
   */
  virtual uint64 TimestampQueryGetResult(TimestampQueryID p_query) {
    (void)p_query;
    return 0;
  }

  /**
   * @brief 检查时间戳查询结果是否可用
   *
   * @param[in] p_query 查询对象 ID
   *
   * @return true 结果已可用
   *
   * @note OpenGL 实现：glGetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE,
   * &available)
   */
  virtual bool TimestampQueryIsAvailable(TimestampQueryID p_query) {
    (void)p_query;
    return false;
  }

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
  virtual void QueueSubmit(uint32 p_queue_family, uint32 p_queue_index,
                           VectorView<QueueSubmitInfo> p_submits,
                           FenceID p_fence) {
    (void)p_queue_family;
    (void)p_queue_index;
    (void)p_submits;
    (void)p_fence;
  }

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
  virtual void QueuePresent(QueuePresentInfo p_info) {
    (void)p_info;
  }
};

}  // namespace arhud

#endif  // ARHUD_DRIVERS_RENDERING_DEVICE_DRIVER_H_
