/**
 * @file rendering_device_driver_gl.h
 * @brief OpenGL 渲染设备驱动实现
 *
 * GLRenderingDeviceDriver 是 IRenderingDeviceDriver 的 OpenGL 实现，
 * 使用 OpenGL 3.3 Core Profile API 实现 GPU 资源管理和命令录制。
 *
 * @par 不可变纹理存储
 *   当 GL 版本 ≥ 4.2 时，TextureCreate 使用 glTexStorage2D/glTexStorage3D
 *   一次性分配所有 mipmap 层级的不可变存储，避免 glGenerateMipmap 时的
 *   延迟分配和帧内卡顿。GL < 4.2 时回退到 glTexImage2D 逐级分配。
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
 *      到 GLPipelineState 结构体，CommandBindRenderPipeline() 时逐个设置 GL
 * 状态
 *   2. **命令直录**：OpenGL 是立即模式，CommandXxx() 直接调用 GL API，
 *      无 CommandBuffer 录制/提交层（对齐 Godot GLES3）
 *   3. **SwapChain 简化**：OpenGL 使用 FBO 0 + SwapBuffers，
 *      不需要 Vulkan 式的 acquire/present 流程
 *   4. **Fence/Semaphore 简化**：OpenGL 同步由 SwapBuffers 隐式完成
 *   5. **GLStateCache**：缓存当前 GL 状态，避免冗余 glEnable/glDisable 调用
 *
 * @par 与 Vulkan RDD 的对比
 *   | 维度             | OpenGL RDD                      | Vulkan RDD |
 *   |-----------------|---------------------------------|-------------------------------|
 *   | Pipeline        | 状态结构体 + 延迟设置            | VkPipeline (PSO) |
 *   | CommandBuffer   | 立即模式（直接 GL 调用）          |
 * VkCommandBuffer（延迟提交）    | | SwapChain       | FBO 0 + SwapBuffers |
 * VkSwapchainKHR                | | 同步             | SwapBuffers 隐式 |
 * VkFence + VkSemaphore         | | Barrier          | 忽略（GL 自动同步） |
 * vkCmdPipelineBarrier          | | Shader           | GLSL 运行时编译 | SPIRV
 * → VkShaderModule        | | Uniform Set      | glUniform + glBindTexture |
 * VkDescriptorSet               |
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
 *   OpenGL 的资源 ID（GLuint）是 32 位整数，ARHud 的 ID 类型是 uint64。
 *   映射方式：直接将 GLuint 零扩展到 uint64。
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

#ifndef ARHUD_DRIVERS_GL_RENDERING_DEVICE_DRIVER_GL_H_
#define ARHUD_DRIVERS_GL_RENDERING_DEVICE_DRIVER_GL_H_

#include "gl_context_manager.h"
#include "rendering_device_driver.h"
#include "template/hash_map.h"
#include "template/local_vector.h"
#include "template/paged_allocator.h"

namespace arhud {

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
class GLStateCache {
 public:
  /**
   * @brief 重置所有缓存状态
   *
   * 在上下文切换或帧开始时调用，强制下次所有状态设置都执行 GL 调用。
   */
  void Reset();

  /**
   * @brief 绑定着色器程序
   *
   * 仅在程序改变时调用 glUseProgram。
   *
   * @param[in] p_program GL 程序对象 ID
   */
  void UseProgram(uint32 p_program);

  /**
   * @brief 绑定顶点数组对象
   *
   * 仅在 VAO 改变时调用 glBindVertexArray。
   *
   * @param[in] p_vao GL VAO ID
   */
  void BindVertexArray(uint32 p_vao);

  /**
   * @brief 绑定帧缓冲对象
   *
   * 仅在 FBO 改变时调用 glBindFramebuffer。
   *
   * @param[in] p_fbo GL FBO ID
   */
  void BindFramebuffer(uint32 p_fbo);

  /**
   * @brief 绑定顶点缓冲区
   *
   * 仅在缓冲区改变时调用 glBindBuffer(GL_ARRAY_BUFFER)。
   *
   * @param[in] p_buffer GL VBO ID
   */
  void BindVertexBuffer(uint32 p_buffer);

  /**
   * @brief 绑定索引缓冲区
   *
   * 仅在缓冲区改变时调用 glBindBuffer(GL_ELEMENT_ARRAY_BUFFER)。
   *
   * @param[in] p_buffer GL IBO ID
   */
  void BindIndexBuffer(uint32 p_buffer);

  /**
   * @brief 绑定 Uniform 缓冲区到指定绑定点
   *
   * 仅在缓冲区改变时调用 glBindBufferBase(GL_UNIFORM_BUFFER)。
   *
   * @param[in] p_index  绑定点索引
   * @param[in] p_buffer GL UBO ID
   */
  void BindUniformBuffer(uint32 p_index, uint32 p_buffer);

  /**
   * @brief 绑定 Uniform 缓冲区范围到指定绑定点
   *
   * 仅在缓冲区或偏移改变时调用 glBindBufferRange(GL_UNIFORM_BUFFER)。
   *
   * @param[in] p_index  绑定点索引
   * @param[in] p_buffer GL UBO ID
   * @param[in] p_offset 缓冲区偏移（字节）
   * @param[in] p_size   绑定范围大小（字节）
   */
  void BindUniformBufferRange(uint32 p_index, uint32 p_buffer, uint32 p_offset,
                              uint32 p_size);

  /**
   * @brief 激活纹理单元
   *
   * 仅在单元改变时调用 glActiveTexture。
   *
   * @param[in] p_unit 纹理单元索引（GL_TEXTURE0 + unit）
   */
  void ActiveTexture(uint32 p_unit);

  /**
   * @brief 绑定 2D 纹理到指定纹理单元
   *
   * 仅在纹理改变时调用 glBindTexture(GL_TEXTURE_2D)。
   *
   * @param[in] p_unit    纹理单元索引
   * @param[in] p_texture GL 纹理 ID
   */
  void BindTexture2D(uint32 p_unit, uint32 p_texture);

  /**
   * @brief 绑定纹理到指定目标（低频路径，不缓存判重）
   *
   * 直接调用 glBindTexture，不进行判重优化。
   * 因为同一纹理单元可同时绑定不同 target 的纹理，
   * 缓存无法按 target 判重。
   *
   * @note 渲染热路径应使用 BindTexture2D()，支持判重缓存。
   *
   * @param[in] p_target  GL 纹理目标（如 GL_TEXTURE_2D、GL_TEXTURE_CUBE_MAP）
   * @param[in] p_texture GL 纹理 ID
   */
  void BindTexture(uint32 p_target, uint32 p_texture);

  /**
   * @brief 绑定采样器到指定纹理单元
   *
   * 仅在采样器改变时调用 glBindSampler。
   *
   * @param[in] p_unit    纹理单元索引
   * @param[in] p_sampler GL 采样器 ID
   */
  void BindSampler(uint32 p_unit, uint32 p_sampler);

  /**
   * @brief 绑定像素解包缓冲区
   *
   * 仅在缓冲区改变时调用 glBindBuffer(GL_PIXEL_UNPACK_BUFFER)。
   *
   * @param[in] p_buffer GL PBO ID
   */
  void BindPixelUnpackBuffer(uint32 p_buffer);

  /**
   * @brief 使 VBO 缓存失效
   *
   * 强制下次 BindVertexBuffer 重新调用 GL API。
   * 用于 VBO 被外部操作改变后的缓存同步。
   */
  void InvalidateVertexBuffer();

  /**
   * @brief 使 IBO 缓存失效
   *
   * 强制下次 BindIndexBuffer 重新调用 GL API。
   * 用于 IBO 被外部操作改变后的缓存同步。
   */
  void InvalidateIndexBuffer();

  /**
   * @brief 设置深度测试
   *
   * 仅在状态改变时调用 glEnable/glDisable(GL_DEPTH_TEST)。
   *
   * @param[in] p_enable true 启用，false 禁用
   */
  void SetDepthTest(bool p_enable);

  /**
   * @brief 设置深度写入
   *
   * 仅在状态改变时调用 glDepthMask。
   *
   * @param[in] p_enable true 启用写入，false 禁用写入
   */
  void SetDepthWrite(bool p_enable);

  /**
   * @brief 设置深度比较函数
   *
   * 仅在函数改变时调用 glDepthFunc。
   *
   * @param[in] p_func GL 深度函数（如 GL_LESS、GL_LEQUAL）
   */
  void SetDepthFunc(uint32 p_func);

  /**
   * @brief 设置混合
   *
   * 仅在状态改变时调用 glEnable/glDisable(GL_BLEND)。
   *
   * @param[in] p_enable true 启用，false 禁用
   */
  void SetBlend(bool p_enable);

  /**
   * @brief 设置混合函数
   *
   * 仅在参数改变时调用 glBlendFuncSeparate。
   *
   * @param[in] p_src_rgb    RGB 源因子
   * @param[in] p_dst_rgb    RGB 目标因子
   * @param[in] p_src_alpha  Alpha 源因子
   * @param[in] p_dst_alpha  Alpha 目标因子
   */
  void SetBlendFunc(uint32 p_src_rgb, uint32 p_dst_rgb, uint32 p_src_alpha,
                    uint32 p_dst_alpha);

  /**
   * @brief 设置面剔除
   *
   * 仅在状态改变时调用 glEnable/glDisable(GL_CULL_FACE)。
   *
   * @param[in] p_enable true 启用，false 禁用
   */
  void SetCullFace(bool p_enable);

  /**
   * @brief 设置剔除模式
   *
   * 仅在模式改变时调用 glCullFace。
   *
   * @param[in] p_mode GL 剔除模式（GL_FRONT、GL_BACK、GL_FRONT_AND_BACK）
   */
  void SetCullMode(uint32 p_mode);

  /**
   * @brief 设置正面方向
   *
   * 仅在方向改变时调用 glFrontFace。
   *
   * @param[in] p_face GL 正面方向（GL_CW、GL_CCW）
   */
  void SetFrontFace(uint32 p_face);

  /**
   * @brief 设置视口
   *
   * 仅在参数改变时调用 glViewport。
   *
   * @param[in] p_x      左下角 X
   * @param[in] p_y      左下角 Y
   * @param[in] p_width  宽度
   * @param[in] p_height 高度
   */
  void SetViewport(int32 p_x, int32 p_y, uint32 p_width, uint32 p_height);

  /**
   * @brief 设置裁剪矩形
   *
   * 仅在参数改变时调用 glScissor。
   *
   * @param[in] p_x      左下角 X
   * @param[in] p_y      左下角 Y
   * @param[in] p_width  宽度
   * @param[in] p_height 高度
   */
  void SetScissor(int32 p_x, int32 p_y, uint32 p_width, uint32 p_height);

  /**
   * @brief 设置裁剪测试
   *
   * 仅在状态改变时调用 glEnable/glDisable(GL_SCISSOR_TEST)。
   *
   * @param[in] p_enable true 启用，false 禁用
   */
  void SetScissorEnable(bool p_enable);

  /**
   * @brief 设置多重采样
   *
   * 仅在状态改变时调用 glEnable/glDisable(GL_MULTISAMPLE)。
   *
   * @param[in] p_enable true 启用，false 禁用
   */
  void SetMultisampleEnable(bool p_enable);

  /**
   * @brief 设置采样着色
   *
   * 仅在状态改变时调用 glEnable/glDisable(GL_SAMPLE_SHADING)。
   *
   * @param[in] p_enable true 启用，false 禁用
   */
  void SetSampleShadingEnable(bool p_enable);

  /**
   * @brief 设置最小采样着色率
   *
   * 仅在值改变时调用 glMinSampleShading。
   *
   * @param[in] p_value 最小采样率 [0.0, 1.0]
   */
  void SetMinSampleShading(float p_value);

  /**
   * @brief 设置 Alpha to Coverage
   *
   * 仅在状态改变时调用 glEnable/glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE)。
   *
   * @param[in] p_enable true 启用，false 禁用
   */
  void SetAlphaToCoverage(bool p_enable);

  /**
   * @brief 设置 Alpha to One
   *
   * 仅在状态改变时调用 glEnable/glDisable(GL_SAMPLE_ALPHA_TO_ONE)。
   *
   * @param[in] p_enable true 启用，false 禁用
   */
  void SetAlphaToOne(bool p_enable);

 private:
  uint32 program_ = 0;                 ///< 当前绑定的 GL 程序对象
  uint32 vao_ = 0;                     ///< 当前绑定的 GL VAO
  uint32 fbo_ = 0;                     ///< 当前绑定的 GL FBO
  uint32 vertex_buffer_ = 0;           ///< 当前绑定的 GL VBO
  uint32 index_buffer_ = 0;            ///< 当前绑定的 GL IBO
  uint32 pixel_unpack_buffer_ = 0;     ///< 当前绑定的 GL PBO
  uint32 active_texture_unit_ = 0;     ///< 当前激活的纹理单元
  uint32 bound_textures_[16] = {};     ///< 各纹理单元绑定的纹理 ID
  uint32 bound_ubos_[16] = {};         ///< 各绑定点的 UBO ID
  uint32 bound_ubo_offsets_[16] = {};  ///< 各绑定点的 UBO 偏移
  uint32 bound_ubo_sizes_[16] = {};    ///< 各绑定点的 UBO 范围大小
  uint32 bound_samplers_[16] = {};     ///< 各纹理单元绑定的采样器 ID

  bool depth_test_ = false;     ///< 深度测试状态
  bool depth_write_ = true;     ///< 深度写入状态
  uint32 depth_func_ = 0x0201;  ///< 深度比较函数（GL_LESS）
  bool blend_ = false;          ///< 混合状态
  uint32 blend_src_rgb_ = 1;    ///< RGB 源混合因子（GL_ONE）
  uint32 blend_dst_rgb_ = 0;    ///< RGB 目标混合因子（GL_ZERO）
  uint32 blend_src_alpha_ = 1;  ///< Alpha 源混合因子
  uint32 blend_dst_alpha_ = 0;  ///< Alpha 目标混合因子
  bool cull_face_ = false;      ///< 面剔除状态
  uint32 cull_mode_ = 0x0405;   ///< 剔除模式（GL_BACK）
  uint32 front_face_ = 0x0900;  ///< 正面方向（GL_CW）

  bool multisample_enable_ = false;     ///< 多重采样状态
  bool sample_shading_enable_ = false;  ///< 采样着色状态
  float min_sample_shading_ = 0.0f;     ///< 最小采样着色率
  bool alpha_to_coverage_ = false;      ///< Alpha to Coverage 状态
  bool alpha_to_one_ = false;           ///< Alpha to One 状态

  int32 viewport_x_ = 0;        ///< 视口左下角 X
  int32 viewport_y_ = 0;        ///< 视口左下角 Y
  uint32 viewport_w_ = 0;       ///< 视口宽度
  uint32 viewport_h_ = 0;       ///< 视口高度
  bool viewport_dirty_ = true;  ///< 视口是否需要更新

  int32 scissor_x_ = 0;          ///< 裁剪矩形左下角 X
  int32 scissor_y_ = 0;          ///< 裁剪矩形左下角 Y
  uint32 scissor_w_ = 0;         ///< 裁剪矩形宽度
  uint32 scissor_h_ = 0;         ///< 裁剪矩形高度
  bool scissor_dirty_ = true;    ///< 裁剪矩形是否需要更新
  bool scissor_enable_ = false;  ///< 裁剪测试状态
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
struct GLPipelineState {
  ShaderID shader_id;                              ///< 关联的着色器 ID
  uint32 shader_program = 0;                       ///< GL 程序对象 ID
  uint32 vao = 0;                                  ///< 关联的 GL VAO ID
  LocalVector<VertexAttribute> vertex_attributes;  ///< 顶点属性描述
  RenderPrimitive render_primitive =
      RenderPrimitive::kTriangles;        ///< 渲染图元类型
  bool cull_enable = false;               ///< 面剔除启用
  uint32 cull_mode = 0x0405;              ///< 剔除模式（GL_BACK）
  uint32 front_face = 0x0900;             ///< 正面方向（GL_CW）
  bool depth_test_enable = false;         ///< 深度测试启用
  bool depth_write_enable = false;        ///< 深度写入启用
  uint32 depth_func = 0x0201;             ///< 深度比较函数（GL_LESS）
  bool blend_enable = false;              ///< 混合启用
  uint32 blend_src_rgb = 1;               ///< RGB 源混合因子（GL_ONE）
  uint32 blend_dst_rgb = 0;               ///< RGB 目标混合因子（GL_ZERO）
  uint32 blend_src_alpha = 1;             ///< Alpha 源混合因子
  uint32 blend_dst_alpha = 0;             ///< Alpha 目标混合因子
  bool multisample_enable = false;        ///< 多重采样启用
  bool sample_shading_enable = false;     ///< 采样着色启用
  float min_sample_shading = 0.0f;        ///< 最小采样着色率
  bool alpha_to_coverage_enable = false;  ///< Alpha to Coverage 启用
  bool alpha_to_one_enable = false;       ///< Alpha to One 启用
  bool wireframe = false;                 ///< 线框模式
  BitField<PipelineDynamicStateFlags> dynamic_state;  ///< 动态状态标志位
};

// ═══════════════════════════════════════════════════════════════════════
// GL 着色器信息
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief OpenGL 着色器编译信息
 *
 * 存储着色器程序 ID 和所有 Uniform 位置缓存。
 * Uniform 位置在 ShaderCreateFromSource() 时一次性查询并缓存，
 * 避免每次绑定时重复查询。
 */
struct GLShaderInfo {
  uint32 program = 0;                  ///< GL 程序对象 ID
  LocalVector<uint32> shader_objects;  ///< 编译的着色器对象 ID 列表

  /**
   * @brief Uniform 位置缓存条目
   *
   * 记录单个 Uniform 的绑定点、位置和类型。
   */
  struct UniformLocation {
    uint32 binding = UINT32_MAX;  ///< Uniform 绑定点索引
    int32 location = -1;  ///< Uniform 位置（glGetUniformLocation 返回值）
    UniformType type = UniformType::kMax;  ///< Uniform 类型
  };

  LocalVector<UniformLocation> uniform_locations;  ///< 所有 Uniform 位置缓存

  /**
   * @brief 按绑定点和类型查找 Uniform 位置
   *
   * @param[in] p_binding 绑定点索引
   * @param[in] p_type    Uniform 类型
   *
   * @return Uniform 位置，未找到返回 -1
   */
  int32 FindLocation(uint32 p_binding, UniformType p_type) const;
};

// ═══════════════════════════════════════════════════════════════════════
// GL 帧缓冲信息
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief OpenGL 帧缓冲信息
 *
 * 存储 FBO ID 和附件映射。
 */
struct GLFramebufferInfo {
  uint32 fbo = 0;                         ///< GL FBO ID
  uint32 width = 0;                       ///< 帧缓冲宽度
  uint32 height = 0;                      ///< 帧缓冲高度
  LocalVector<uint32> color_attachments;  ///< 颜色附件纹理 ID 列表
  uint32 depth_stencil_attachment = 0;    ///< 深度/模板附件纹理 ID
};

// ═══════════════════════════════════════════════════════════════════════
// GL 纹理信息
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief OpenGL 纹理元数据
 *
 * 记录纹理的格式、尺寸和 GL 目标，供 CommandCopyBufferToTexture 使用。
 * TextureID 本身只存储 GLuint，不包含格式信息，因此需要额外簿记。
 *
 * @par 不可变存储
 *   is_immutable 标记纹理是否通过 glTexStorage2D/glTexStorage3D 创建。
 *   不可变纹理的所有 mipmap 层级在创建时一次性分配，
 *   glGenerateMipmap 只填充数据，不涉及显存分配。
 */
struct GLTextureInfo {
  uint32 target = 0x0DE1;  ///< GL 纹理目标（GL_TEXTURE_2D）
  DataFormat format = DataFormat::kR8G8B8A8Unorm;  ///< 数据格式
  uint32 width = 0;                                ///< 纹理宽度
  uint32 height = 0;                               ///< 纹理高度
  uint32 depth = 1;                                ///< 纹理深度（3D 纹理使用）
  uint32 mipmaps = 1;                              ///< Mipmap 层级数
  uint32 layers = 1;                               ///< 纹理层数（数组纹理使用）
  bool is_immutable = false;  ///< 是否使用不可变存储（glTexStorage）
  void* dx_alias = nullptr;  ///< DX interop 别名句柄（HANDLE；null = 自有存储）
  bool dx_held = false;  ///< 绘制权当前是否在 GL 侧（Begin/EndWrite 配对凭据）
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
struct GLRenderPassInfo {
  LocalVector<Attachment> attachments;  ///< 附件描述列表
};

// ═══════════════════════════════════════════════════════════════════════
// GL Uniform Set 信息
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief OpenGL Uniform Set 信息
 *
 * 存储绑定的纹理和 Buffer，在 CommandBindUniformSet() 时使用。
 */
struct GLUniformSetInfo {
  /**
   * @brief Uniform Set 中绑定的单个资源
   */
  struct BoundResource {
    UniformType type = UniformType::kMax;  ///< 资源类型
    uint32 binding = UINT32_MAX;           ///< 绑定点索引
    uint32 gl_name = 0;                    ///< 资源 GL 名称（纹理/Buffer ID）
    uint32 gl_sampler_name = 0;  ///< 采样器 GL 名称（kSamplerWithTexture 使用）
    uint32 offset = 0;           ///< UBO 基础偏移（kUniformBufferDynamic 使用）
    uint32 range_size = 0;       ///< UBO UniformSetCreate 使用）
  };

  LocalVector<BoundResource> resources;  ///< 绑定的资源列表
  ShaderID shader;                       ///< 关联的着色器 ID
  uint32 set_index = 0;                  ///< Set 索引
};

// ═══════════════════════════════════════════════════════════════════════
// GL 顶点格式信息
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief OpenGL 顶点格式信息
 *
 * 存储 VAO ID 和属性描述，在 CommandBindRenderPipeline() 时绑定。
 */
struct GLVertexFormatInfo {
  uint32 vao = 0;                           ///< GL VAO ID
  LocalVector<VertexAttribute> attributes;  ///< 顶点属性描述列表
};

// ═══════════════════════════════════════════════════════════════════════
// GL SwapChain 信息
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief OpenGL SwapChain 信息
 *
 * 记录 RCD Surface 与 GL Context/Surface 的绑定关系。
 * SwapChainAcquire 时调用 IGLContextManager::MakeCurrent，
 * SwapChainPresent 时调用 IGLContextManager::SwapBuffers。
 */
struct GLSwapChain {
  IRenderingContextDriver::SurfaceID rcd_surface =
      IRenderingContextDriver::kInvalidSurfaceId;  ///< RCD 层 Surface ID
  IGLContextManager::ContextID gl_context =
      IGLContextManager::kInvalidContextId;  ///< 关联的 GL 上下文 ID
  IGLContextManager::SurfaceID gl_surface =
      IGLContextManager::kInvalidSurfaceId;  ///< 关联的 GL 渲染表面 ID
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
 *   - 命令直录：CommandXxx() 直接调 GL API（无录制/回放层）
 *
 * @par 线程安全
 *   - 资源创建/销毁应在拥有 GL 上下文的线程
 *   - CommandXxx() 必须在渲染线程（需要 GL 上下文已 MakeCurrent）
 *   - GLStateCache 为线程本地状态
 *
 * @see IRenderingDeviceDriver  基类接口
 * @see GLStateCache            GL 状态缓存
 * @see GLPipelineState         管线状态
 */
class GLRenderingDeviceDriver : public IRenderingDeviceDriver {
 public:
  /**
   * @brief 构造函数
   *
   * @param[in] p_context_driver 渲染上下文驱动指针（不拥有所有权）
   * @param[in] p_gl_manager     GL 上下文管理器指针（不拥有所有权）
   *
   * @pre p_context_driver 非 nullptr
   * @pre p_gl_manager 非 nullptr
   */
  explicit GLRenderingDeviceDriver(IRenderingContextDriver* p_context_driver,
                                   IGLContextManager* p_gl_manager);

  /**
   * @brief 析构函数
   *
   * 自动调用 Shutdown() 释放资源。
   * 不销毁外部注入的 IRenderingContextDriver 和 IGLContextManager。
   */
  ~GLRenderingDeviceDriver() override;

  ARHUD_DISABLE_COPY(GLRenderingDeviceDriver);

 public:
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
  Error Initialize(uint32 p_device_index, uint32 p_frame_count) override;

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
  BufferID BufferCreate(uint64 p_size, BitField<BufferUsageBits> p_usage,
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
  uint8* BufferMap(BufferID p_buffer) override;

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
  uint64 BufferGetAllocationSize(BufferID p_buffer) override;

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
  TextureID TextureCreate(const TextureFormat& p_format,
                          const TextureView& p_view) override;

  /**
   * @brief 释放纹理
   *
   * @param[in] p_texture 纹理 ID
   *
   * @note OpenGL 实现：glDeleteTextures
   */
  void TextureFree(TextureID p_texture) override;

  /**
   * @brief 生成纹理 Mipmap 链
   *
   * @param[in] p_texture 纹理 ID
   *
   * @note OpenGL 实现：glGenerateMipmap
   */
  void TextureGenerateMipmaps(TextureID p_texture) override;

  /**
   * @brief 获取纹理分配大小
   *
   * @param[in] p_texture 纹理 ID
   *
   * @return 纹理分配大小（字节）
   */
  uint64 TextureGetAllocationSize(TextureID p_texture) override;

  // ── 外部资源导入（零拷贝上屏 Mode X；实现见 _gl_dx.cpp）──
  /**
   * @brief 是否可用外部资源导入
   *
   * @note 保守语义：探测（wglDXOpenDeviceNV）需要 D3D device 指针与
   *       当前 GL 上下文，只能在首次 TextureCreateExternal 时完成。
   *       因此本方法在探测成功前返回 false，调用方应以
   *       TextureCreateExternal 的实际结果为准。
   */
  bool SupportsExternalTexture() const override {
    return dx_interop_ready_;
  }

  /** @brief 以平台侧显存（ID3D11Texture2D）建立 GL 别名纹理 */
  TextureID TextureCreateExternal(
      const ExternalTextureSource& p_source) override;

  /** @brief 跨 API 锁定（wglDXLockObjectsNV）——写之前唯一同步点 */
  bool TextureExternalBeginWrite(TextureID p_texture) override;

  /** @brief 跨 API 解锁（wglDXUnlockObjectsNV）——交还显示侧 */
  void TextureExternalEndWrite(TextureID p_texture) override;

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
  SamplerID SamplerCreate(const SamplerState& p_state) override;

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
   * @brief 从源码创建着色器
   *
   * @param[in] p_stage_sources   着色器阶段源码数组
   * @param[in] p_source_format   源码格式（GL 后端仅接受 kGLSL）
   * @param[in] p_uniforms       Uniform 描述数组
   * @param[in] p_push_constant_size Push Constant 大小（字节）
   *
   * @retval ShaderID 着色器 ID，编译/链接失败返回空 ID
   *
   * @note OpenGL 实现：glCreateShader + glShaderSource + glCompileShader +
   *       glCreateProgram + glAttachShader + glLinkProgram
   * @note Uniform 位置在创建时查询并缓存
   */
  ShaderID ShaderCreateFromSource(VectorView<ShaderStageSource> p_stage_sources,
                                  ShaderSourceFormat p_source_format,
                                  VectorView<ShaderUniform> p_uniforms,
                                  uint32 p_push_constant_size) override;

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
   * @param[in] p_shader    关联的着色器
   * @param[in] p_set_index Set 索引
   *
   * @retval UniformSetID Uniform Set ID，失败返回空 ID
   *
   * @note OpenGL 实现：记录纹理/Buffer 绑定信息
   * @note Uniform 位置已由 ShaderCreateFromSource 缓存
   */
  UniformSetID UniformSetCreate(VectorView<BoundUniform> p_uniforms,
                                ShaderID p_shader, uint32 p_set_index) override;

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
      VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses,
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
  FramebufferID FramebufferCreate(RenderPassID p_render_pass,
                                  VectorView<TextureID> p_attachments,
                                  uint32 p_width, uint32 p_height) override;

  /**
   * @brief 释放帧缓冲
   *
   * @param[in] p_framebuffer 帧缓冲 ID
   *
   * @note OpenGL 实现：glDeleteFramebuffers
   */
  void FramebufferFree(FramebufferID p_framebuffer) override;

  void FramebufferReadback(FramebufferID p_framebuffer, uint32 p_x, uint32 p_y,
                           uint32 p_width, uint32 p_height,
                           void* p_dst) override;

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
   * @param[in] p_rasterization_state   光栅化状态
   * @param[in] p_multisample_state     多重采样状态
   * @param[in] p_depth_stencil_state   深度/模板状态
   * @param[in] p_blend_state           颜色混合状态
   * @param[in] p_dynamic_state         动态状态标志位
   * @param[in] p_render_pass           渲染 Pass
   *
   * @retval PipelineID 管线 ID，失败返回空 ID
   *
   * @note OpenGL 实现：仅记录状态到 GLPipelineState
   */
  PipelineID RenderPipelineCreate(
      ShaderID p_shader, VertexFormatID p_vertex_format,
      RenderPrimitive p_render_primitive,
      const PipelineRasterizationState& p_rasterization_state,
      const PipelineMultisampleState& p_multisample_state,
      const PipelineDepthStencilState& p_depth_stencil_state,
      const PipelineColorBlendState& p_blend_state,
      BitField<PipelineDynamicStateFlags> p_dynamic_state,
      RenderPassID p_render_pass) override;

  /**
   * @brief 创建计算管线
   *
   * @param[in] p_shader 计算着色器
   *
   * @retval PipelineID 管线 ID，失败返回空 ID
   *
   * @note OpenGL 实现：仅记录着色器程序到 GLPipelineState，vao=0
   */
  PipelineID ComputePipelineCreate(ShaderID p_shader) override;

  /**
   * @brief 释放管线
   *
   * @param[in] p_pipeline 管线 ID
   */
  void PipelineFree(PipelineID p_pipeline) override;

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
  void CommandBeginRenderPass(RenderPassID p_render_pass,
                              FramebufferID p_framebuffer,
                              VectorView<RenderPassClearValue> p_clear_values,
                              uint32 p_clear_mask, int32 p_rect_x,
                              int32 p_rect_y, uint32 p_rect_w,
                              uint32 p_rect_h) override;

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
  void CommandBindUniformSet(UniformSetID p_uniform_set, uint32 p_set_index,
                             uint32 p_dynamic_offset = 0) override;

  /**
   * @brief 绑定顶点缓冲区
   *
   * @param[in] p_buffers 顶点缓冲区 ID 数组
   * @param[in] p_offsets 偏移量数组
   * @param[in] p_count   缓冲区数量
   *
   * @note OpenGL 实现：glBindVertexBuffer
   */
  void CommandBindVertexBuffers(const BufferID* p_buffers,
                                const uint64* p_offsets,
                                uint32 p_count) override;

  /**
   * @brief 绑定索引缓冲区
   *
   * @param[in] p_buffer 索引缓冲区 ID
   * @param[in] p_format 索引格式
   * @param[in] p_offset 偏移量（字节）
   *
   * @note OpenGL 实现：glBindBuffer + glVertexAttribPointer
   */
  void CommandBindIndexBuffer(BufferID p_buffer, IndexBufferFormat p_format,
                              uint64 p_offset) override;

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
  void CommandDraw(uint32 p_vertex_count, uint32 p_instance_count,
                   uint32 p_base_vertex, uint32 p_first_instance) override;

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
  void CommandDrawIndexed(uint32 p_index_count, uint32 p_instance_count,
                          uint32 p_first_index, int32 p_vertex_offset,
                          uint32 p_first_instance) override;

  /**
   * @brief 非索引实例化绘制
   *
   * 使用已绑定的实例缓冲区进行实例化绘制。
   * 顶点数量从绑定的顶点缓冲区元数据中推导。
   *
   * @param[in] p_instance_count 实例数量
   * @param[in] p_base_instance  起始实例偏移
   *
   * @note OpenGL 实现：glDrawArraysInstanced（base_instance 需要 GL 4.2+）
   */
  void CommandDrawInstanced(uint32 p_instance_count,
                            uint32 p_base_instance) override;

  /**
   * @brief 索引实例化绘制
   *
   * 使用已绑定的实例缓冲区和索引缓冲区进行实例化绘制。
   * 索引数量从绑定的索引缓冲区元数据中推导。
   *
   * @param[in] p_instance_count 实例数量
   * @param[in] p_base_instance  起始实例偏移
   *
   * @note OpenGL 实现：glDrawElementsInstanced（base_instance 需要 GL 4.2+）
   */
  void CommandDrawIndexedInstanced(uint32 p_instance_count,
                                   uint32 p_base_instance) override;

  // ═══════════════════════════════════════════════════════════════════════
  // 命令录制 — 计算（立即模式）
  // ═══════════════════════════════════════════════════════════════════════

  /**
   * @brief 派发计算着色器
   *
   * @param[in] p_x_groups X 维度工作组数量
   * @param[in] p_y_groups Y 维度工作组数量
   * @param[in] p_z_groups Z 维度工作组数量
   *
   * @note OpenGL 实现：glDispatchCompute（需要 OpenGL 4.3+）
   */
  void CommandDispatch(uint32 p_x_groups, uint32 p_y_groups,
                       uint32 p_z_groups) override;

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
  void CommandSetViewport(int32 p_x, int32 p_y, uint32 p_width,
                          uint32 p_height) override;

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
  void CommandSetScissor(int32 p_x, int32 p_y, uint32 p_width,
                         uint32 p_height) override;

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
  void CommandSetBlendConstants(float p_r, float p_g, float p_b,
                                float p_a) override;

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
  void CommandClearBuffer(BufferID p_buffer, uint64 p_offset,
                          uint64 p_size) override;

  /**
   * @brief 复制缓冲区
   *
   * @param[in] p_src_buffer 源缓冲区
   * @param[in] p_dst_buffer 目标缓冲区
   * @param[in] p_regions   复制区域数组
   *
   * @note OpenGL 实现：glCopyNamedBufferSubData
   */
  void CommandCopyBuffer(BufferID p_src_buffer, BufferID p_dst_buffer,
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
      TextureID p_texture, float p_color_r, float p_color_g, float p_color_b,
      float p_color_a, const TextureSubresourceRange& p_subresources) override;

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
      TextureID p_texture, float p_depth, uint32 p_stencil,
      const TextureSubresourceRange& p_subresources) override;

  /**
   * @brief 更新缓冲区数据
   *
   * 将 CPU 内存数据写入 GPU 缓冲区的指定范围。
   *
   * @param[in] p_dst_buffer 目标缓冲区 ID
   * @param[in] p_offset     写入偏移（字节）
   * @param[in] p_size       写入大小（字节）
   * @param[in] p_data       源数据指针
   *
   * @note OpenGL 实现：glNamedBufferSubData
   */
  void CommandBufferUpdate(BufferID p_dst_buffer, uint32 p_offset,
                           uint32 p_size, const void* p_data) override;

  /**
   * @brief 拷贝缓冲区数据到纹理
   *
   * 使用 PBO 方式上传像素数据到纹理。
   * OpenGL 实现：绑定 Buffer 到 GL_PIXEL_UNPACK_BUFFER，
   * 然后调用 glTexSubImage2D/3D，最后解绑 PBO。
   *
   * @param[in] p_src_buffer  源缓冲区（PBO）
   * @param[in] p_dst_texture 目标纹理
   * @param[in] p_regions     拷贝区域数组
   */
  void CommandCopyBufferToTexture(
      BufferID p_src_buffer, TextureID p_dst_texture,
      VectorView<BufferTextureCopyRegion> p_regions) override;

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
  void BeginSegment(uint32 p_frame_index, uint32 p_frames_drawn) override;

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
  uint64 LimitGet(Limit p_limit) override;

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
  const Capabilities& GetCapabilities() const override;

  /**
   * @brief 获取 API 名称
   *
   * @return API 名称字符串（如 "OpenGL 3.3 Core"）
   */
  const char* GetApiName() const override;

  /**
   * @brief 获取管线缓存 UUID
   *
   * @return UUID 字符串
   *
   * @retval "" OpenGL 无 PSO 缓存，返回空字符串
   */
  const char* GetPipelineCacheUuid() const override;

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
  SwapChainID SwapChainCreate(
      IRenderingContextDriver::SurfaceID p_surface,
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
   * 调用 IGLContextManager::MakeCurrent() 绑定 Context 到 Surface。
   *
   * @param[in] p_swapchain SwapChain ID
   *
   * @return Error::kOK 绑定成功
   */
  Error SwapChainAcquire(SwapChainID p_swapchain) override;

  /**
   * @brief 呈现 SwapChain 渲染结果
   *
   * 调用 IGLContextManager::SwapBuffers() 交换前后缓冲区。
   *
   * @param[in] p_swapchain SwapChain ID
   */
  void SwapChainPresent(SwapChainID p_swapchain) override;

  // ═══════════════════════════════════════════════════════════════════════
  // GPU Fence（glFenceSync / glClientWaitSync）
  // ═══════════════════════════════════════════════════════════════════════

  /**
   * @brief 创建 GPU 栅栏
   *
   * @param[in] p_signaled 是否创建为已触发状态
   *
   * @return 栅栏 ID
   * @retval kInvalidFenceId 创建失败
   *
   * @note OpenGL 实现：glFenceSync
   */
  FenceID FenceCreate(bool p_signaled = false) override;

  /**
   * @brief 销毁 GPU 栅栏
   *
   * @param[in] p_fence 栅栏 ID
   *
   * @note OpenGL 实现：glDeleteSync
   */
  void FenceDestroy(FenceID p_fence) override;

  /**
   * @brief 重置 GPU 栅栏
   *
   * @param[in] p_fence 栅栏 ID
   *
   * @note OpenGL GLsync 不支持 reset，销毁后重建
   */
  void FenceReset(FenceID p_fence) override;

  /**
   * @brief 查询 GPU 栅栏是否已触发
   *
   * @param[in] p_fence 栅栏 ID
   *
   * @return true 已触发
   *
   * @note OpenGL 实现：glClientWaitSync(SYNC_ALREADY_FLUSHED)
   */
  bool FenceIsSignaled(FenceID p_fence) override;

  /**
   * @brief 等待 GPU 栅栏触发
   *
   * @param[in] p_fence       栅栏 ID
   * @param[in] p_timeout_ns  超时时间（纳秒）
   *
   * @return true 栅栏已触发
   *
   * @note OpenGL 实现：glClientWaitSync
   */
  bool FenceWait(FenceID p_fence, uint64 p_timeout_ns) override;

  // ═══════════════════════════════════════════════════════════════════════
  // GPU 时间戳查询
  // ═══════════════════════════════════════════════════════════════════════

  /**
   * @brief 创建时间戳查询对象
   *
   * @return 查询对象 ID
   * @retval kInvalidTimestampQueryId 创建失败
   *
   * @note OpenGL 实现：glGenQueries
   */
  TimestampQueryID TimestampQueryCreate() override;

  /**
   * @brief 销毁时间戳查询对象
   *
   * @param[in] p_query 查询对象 ID
   *
   * @note OpenGL 实现：glDeleteQueries
   */
  void TimestampQueryDestroy(TimestampQueryID p_query) override;

  /**
   * @brief 记录当前 GPU 时间戳
   *
   * @param[in] p_query 查询对象 ID
   *
   * @note OpenGL 实现：glQueryCounter(id, GL_TIMESTAMP)
   */
  void TimestampQueryRecord(TimestampQueryID p_query) override;

  /**
   * @brief 获取时间戳查询结果（纳秒）
   *
   * @param[in] p_query 查询对象 ID
   *
   * @return GPU 时间戳（纳秒），不可用时返回 0
   *
   * @note OpenGL 实现：glGetQueryObjectui64v
   */
  uint64 TimestampQueryGetResult(TimestampQueryID p_query) override;

  /**
   * @brief 检查时间戳查询结果是否可用
   *
   * @param[in] p_query 查询对象 ID
   *
   * @return true 结果已可用
   *
   * @note OpenGL 实现：glGetQueryObjectiv(GL_QUERY_RESULT_AVAILABLE)
   */
  bool TimestampQueryIsAvailable(TimestampQueryID p_query) override;

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
  static uint32 CompileShader(const char* p_source, uint32 p_type);

  /**
   * @brief 链接着色器程序
   *
   * @param[in] p_vertex_shader   顶点着色器 ID
   * @param[in] p_fragment_shader 片段着色器 ID
   *
   * @return 程序对象 ID，失败返回 0
   */
  static uint32 LinkProgram(const LocalVector<uint32>& p_shader_objects);

  /**
   * @brief 将 DataFormat 映射到 GL 内部格式
   */
  static uint32 DataFormatToGLInternalFormat(DataFormat p_format);

  /**
   * @brief 将 DataFormat 映射到 GL 类型
   */
  static uint32 DataFormatToGLType(DataFormat p_format);

  /**
   * @brief 将 DataFormat 映射到 GL 格式
   */
  static uint32 DataFormatToGLFormat(DataFormat p_format);

  /**
   * @brief 将 RenderPrimitive 映射到 GL 图元模式
   */
  static uint32 RenderPrimitiveToGLMode(RenderPrimitive p_primitive);

  /**
   * @brief 将 BlendFactor 映射到 GL 混合因子
   */
  static uint32 BlendFactorToGL(BlendFactor p_factor);

  /**
   * @brief 将 CompareOperator 映射到 GL 比较函数
   */
  static uint32 CompareOperatorToGL(CompareOperator p_op);

  /**
   * @brief 将 SamplerFilter 映射到 GL 过滤模式
   */
  static uint32 SamplerFilterToGL(SamplerFilter p_filter);

  /**
   * @brief 将 SamplerRepeatMode 映射到 GL 寻址模式
   */
  static uint32 SamplerRepeatModeToGL(SamplerRepeatMode p_mode);

  /**
   * @brief 获取 DataFormat 的像素大小（字节）
   */
  static uint32 DataFormatGetPixelSize(DataFormat p_format);

  /**
   * @brief 判定 DataFormat 是否深度/模板附件格式族
   *
   * 统一判定 D16/D32/D16S8/D24S8/D32S8/S8 全族（common层
   * 枚举共 6 项）。FramebufferCreate 附件挂点选择与
   * CommandBeginRenderPass clear 通道分拣共用 —— 全引擎
   * 唯一权威判定，禁止消费方再手写格式枚举清单（历史上
   * 两处清单已漂移：一处漏 D16S8、一处漏 S8）。
   *
   * @param[in] p_format 数据格式
   *
   * @return 是否深度或模板格式（含纯模板 kS8Uint）
   */
  static bool DataFormatIsDepthStencil(DataFormat p_format);

  /**
   * @brief 判定 DataFormat 是否含模板通道
   *
   * D16S8/D24S8/D32S8/S8 返回 true —— clear 逻辑据此决定
   * 是否追加 GL_STENCIL_BUFFER_BIT。
   *
   * @param[in] p_format 数据格式
   *
   * @return 是否含模板位面
   */
  static bool DataFormatHasStencil(DataFormat p_format);

  /**
   * @brief 检查 GL 错误（仅 Debug 模式）
   *
   * 在 Debug 构建中，调用 glGetError() 检查是否有未处理的 GL 错误。
   * 如果有错误，记录错误日志。
   *
   * @param[in] p_function 调用 glGetError 前的函数名
   * @param[in] p_line     行号
   */
  static void CheckGLError(const char* p_function, int p_line);

 private:
  IRenderingContextDriver* context_driver_ =
      nullptr;  ///< 渲染上下文驱动（不拥有所有权）
  IGLContextManager* gl_manager_ =
      nullptr;                ///< GL 上下文管理器（不拥有所有权）
  bool initialized_ = false;  ///< 是否已初始化

  GLStateCache state_cache_;                     ///< GL 状态缓存
  GLPipelineState* current_pipeline_ = nullptr;  ///< 当前绑定的管线状态
  IndexBufferFormat current_index_format_ =
      IndexBufferFormat::kUint16;  ///< 当前索引格式
  uint32 bound_vertex_count_ = 0;  ///< 当前绑定的顶点数量
  uint32 bound_index_count_ = 0;   ///< 当前绑定的索引数量

  HashMap<uint64, uint64> buffer_sizes_;  ///< Buffer ID → 分配大小映射

  LocalVector<GLSwapChain> swap_chains_;  ///< SwapChain 列表

  PagedAllocator<GLShaderInfo> shader_allocator_;       ///< 着色器信息分配器
  PagedAllocator<GLPipelineState> pipeline_allocator_;  ///< 管线状态分配器
  PagedAllocator<GLFramebufferInfo>
      framebuffer_allocator_;  ///< 帧缓冲信息分配器
  PagedAllocator<GLRenderPassInfo>
      render_pass_allocator_;  ///< 渲染 Pass 信息分配器
  PagedAllocator<GLUniformSetInfo>
      uniform_set_allocator_;  ///< Uniform Set 信息分配器
  PagedAllocator<GLVertexFormatInfo>
      vertex_format_allocator_;  ///< 顶点格式信息分配器

  HashMap<uint64, GLTextureInfo>
      texture_infos_;  ///< Texture ID → 纹理元数据映射

  // ── WGL_NV_DX_interop2 状态（Mode X 零拷贝上屏；见 _gl_dx.cpp）──
  // 句柄以 void* 保存，避免本头文件传染 <windows.h>。
  bool dx_interop_ready_ = false;   ///< 探测成功（入口点 + OpenDevice 均可用）
  bool dx_interop_probed_ = false;  ///< 是否已尝试探测（失败只尝试一次）
  void* dx_interop_device_ = nullptr;  ///< HANDLE（wglDXOpenDeviceNV 返回值）
  void* dx_interop_dx_device_ =
        nullptr;  ///< 打开时的 ID3D11Device*（换设备即失效）

  /**
   * @brief 探测并打开 GL↔DX 互操作设备（幂等，首次导入时调用）
   *
   * @param[in] p_dx_device 帧池的 ID3D11Device*（opaque）
   *
   * @retval Error::kOK          可用（dx_interop_ready_ 置位）
   * @retval Error::kUnavailable 扩展缺失/OpenDevice 失败
   */
  Error DXInteropOpen(void* p_dx_device);

  /** @brief 关闭互操作设备（Shutdown 时调用，须在 GL 上下文内） */
  void DXInteropClose();

  /**
   * @brief 解除指定 GL 纹理名的 interop 别名（TextureFree 前置步骤）
   *
   * @param[in] p_gl_name GL 纹理名；非外部纹理静默返回
   *
   * @par 为何必须在 glDeleteTextures 之前
   *   别名句柄持有 DX 资源引用；先删 GL 名会让注册关系悬空，
   *   而帧池随后释放该显存 —— 双重释放风险。
   */
  void DXInteropUnregisterTexture(uint32 p_gl_name);

  uint64 limits_[static_cast<uint32>(Limit::kMax)] = {};  ///< 设备限制值数组
  Capabilities capabilities_;                             ///< 设备能力描述
  char api_name_[64] = {};                                ///< API 名称缓存
  char pipeline_cache_uuid_[37] = {};  ///< 管线缓存 UUID 缓存

  uint32 frame_index_ = 0;   ///< 当前帧索引
  uint32 frames_drawn_ = 0;  ///< 已绘制帧数

  static constexpr uint32 kMaxTextureUnits = 16;  ///< 最大纹理单元数

  // ═══════════════════════════════════════════════════════════════════
  // Shader 编译缓存
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 计算着色器源码哈希值
   *
   * 将所有阶段的源码字符串合并计算 FNV-1a 哈希，
   * 用作编译缓存的磁盘文件名。
   */
  uint64 ComputeSourceHash(VectorView<ShaderStageSource> p_stage_sources) const;

  /**
   * @brief 尝试从磁盘缓存加载预编译着色器
   *
   * @param[in]  p_hash          源码哈希（文件名）
   * @param[in]  p_uniforms      Uniform 描述（用于绑定 UBO）
   * @param[out] r_info          成功时填充的 GLShaderInfo
   * @return true 加载成功，r_info 可用
   */
  bool LoadShaderCache(uint64 p_hash, VectorView<ShaderUniform> p_uniforms,
                       GLShaderInfo& r_info);

  /**
   * @brief 保存预编译着色器到磁盘缓存
   *
   * @param[in] p_hash  源码哈希（文件名）
   * @param[in] p_info  已编译的 GLShaderInfo
   */
  void SaveShaderCache(uint64 p_hash, const GLShaderInfo& p_info);

  /** @brief 编译缓存目录路径（相对于工作目录） */
  static constexpr const char* kShaderCacheDir = "shader_cache";
};

// ═══════════════════════════════════════════════════════════════════════
// GL 错误检查宏
// ═══════════════════════════════════════════════════════════════════════

#if ARHUD_DEBUG
#define GL_CHECK_ERROR() CheckGLError(__FUNCTION__, __LINE__)
#else
#define GL_CHECK_ERROR() ((void)0)
#endif

}  // namespace arhud

#endif  // ARHUD_DRIVERS_GL_RENDERING_DEVICE_DRIVER_GL_H_
