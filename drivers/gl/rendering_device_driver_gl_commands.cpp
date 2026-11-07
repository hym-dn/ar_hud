/**
 * @file rendering_device_driver_gl_commands.cpp
 * @brief GLRenderingDeviceDriver 命令录制实现
 *
 * 包含所有 Command* 系列方法的 OpenGL 实现：
 * - RenderPass 命令（Begin/End/Bind）
 * - 绘制命令（Draw/DrawIndexed/DrawInstanced/DrawIndexedInstanced）
 * - 状态设置命令（SetViewport/SetScissor/SetBlendConstants）
 * - 缓冲区操作命令（ClearBuffer/CopyBuffer/BufferUpdate）
 * -
 * 纹理操作命令（CopyBufferToTexture/ClearColorTexture/ClearDepthStencilTexture）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-06-14
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include <glad/gl.h>

#include <cstring>

#include "io/logger.h"
#include "os/memory.h"
#include "rendering_device_driver_gl.h"

namespace arhud {
/**
 * @brief 开始渲染 Pass（GL 立即模式）
 *
 * 绑定目标 framebuffer、设置视口，并按 `p_clear_mask` 逐附件执行清除。
 *
 * @param[in] p_render_pass  RenderPass ID（提供附件格式 / 加载操作元数据）
 * @param[in] p_framebuffer  Framebuffer ID；无效时绑定默认帧缓冲（0）
 * @param[in] p_clear_values 各附件清除值（与 p_clear_mask 按位对应）
 * @param[in] p_clear_mask   清除通道位掩码（bit i 对应附件 i）
 * @param[in] p_rect_x       渲染区域 X
 * @param[in] p_rect_y       渲染区域 Y
 * @param[in] p_rect_w       渲染区域宽度
 * @param[in] p_rect_h       渲染区域高度
 *
 * @par 清除通道分拣（深度 / 模板判定）
 *   逐附件用 `DataFormatIsDepthStencil()` 判定位面归属：颜色附件走
 *   `glClearColor`；纯模板附件（`kS8Uint`）仅清模板位面；其余深度
 *   （含模板）走 `glClearDepth`，且仅当附件 `stencil_load_op == kClear`
 *   时附加模板清除位。唯一权威判定见 `_format.cpp`。
 *
 * @note 实现映射：`glBindFramebuffer` + `state_cache_.SetViewport` +
 *       `glClearColor` / `glClearDepth` / `glClearStencil` + `glClear`。
 * @see
 * IRenderingDeviceDriver::CommandBeginRenderPass（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandBeginRenderPass(
    RenderPassID p_render_pass, FramebufferID p_framebuffer,
    VectorView<RenderPassClearValue> p_clear_values, uint32 p_clear_mask,
    int32 p_rect_x, int32 p_rect_y, uint32 p_rect_w, uint32 p_rect_h) {
  (void)p_render_pass;
  GLFramebufferInfo* fb_info = nullptr;
  if (p_framebuffer.IsValid()) {
    fb_info = reinterpret_cast<GLFramebufferInfo*>(p_framebuffer.GetId());
  }
  if (fb_info) {
    state_cache_.BindFramebuffer(fb_info->fbo);
  } else {
    state_cache_.BindFramebuffer(0);
  }
  state_cache_.SetViewport(p_rect_x, p_rect_y, p_rect_w, p_rect_h);
  GLbitfield clear_mask = 0;
  GLRenderPassInfo* rp_info = nullptr;
  if (p_render_pass.IsValid()) {
    rp_info = reinterpret_cast<GLRenderPassInfo*>(p_render_pass.GetId());
  }
  for (uint32 i = 0; i < p_clear_values.Size(); ++i) {
    if (!(p_clear_mask & (1u << i))) {
      continue;
    }
    const RenderPassClearValue& cv = p_clear_values[i];
    // 统一判定（唯一权威判定见 _format.cpp）：此前此处硬编码
    // 清单漏 kS8Uint —— 纯模板附件的 clear 值被 glClearColor
    // 误清颜色通道
    bool is_depth_stencil = false;
    if (rp_info && i < rp_info->attachments.Size()) {
      is_depth_stencil =
          DataFormatIsDepthStencil(rp_info->attachments[i].format);
    }
    if (!is_depth_stencil) {
      glClearColor(cv.color[0], cv.color[1], cv.color[2], cv.color[3]);
      clear_mask |= GL_COLOR_BUFFER_BIT;
    } else {
      const DataFormat att_format = rp_info->attachments[i].format;
      if (att_format == DataFormat::kS8Uint) {
        // 纯模板：只清模板位面（无深度位）
        glClearStencil(static_cast<GLint>(cv.stencil));
        clear_mask |= GL_STENCIL_BUFFER_BIT;
      } else {
        glClearDepth(static_cast<GLdouble>(cv.depth));
        clear_mask |= GL_DEPTH_BUFFER_BIT;
        if (DataFormatHasStencil(att_format) &&
            rp_info->attachments[i].stencil_load_op ==
                AttachmentLoadOp::kClear) {
          clear_mask |= GL_STENCIL_BUFFER_BIT;
        }
      }
    }
  }
  if (clear_mask != 0) {
    glClear(clear_mask);
    GL_CHECK_ERROR();
  }
}

/**
 * @brief 结束渲染 Pass（GL 立即模式）
 *
 * @par 空实现
 *   GL 无 render pass 对象：BeginRenderPass 时已直接切换 framebuffer，
 *   结束时无需恢复（下一个 pass 会重新绑定），故本方法为空操作。
 *
 * @note 契约侧仅用于对称配对。
 * @see
 * IRenderingDeviceDriver::CommandEndRenderPass（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandEndRenderPass() {}

/**
 * @brief 绑定渲染管线（GL 立即模式）
 *
 * 把 `GLPipelineState` 中烘焙的全部状态经 `state_cache_` 逐项下发到 GL，
 * 并记录 `current_pipeline_` 供后续 uniform / 属性解析复用。
 *
 * @param[in] p_pipeline 管线 ID（封装 `GLPipelineState` 簿记指针）
 *
 * @par 下发的状态项
 *   shader program（`glUseProgram`）、VAO、面剔除（开关 / 模式 / 正面绕序）、
 *   深度测试 / 写 / 比较函数、混合（开关 / 因子）、MSAA（多重采样 / 采样着色 /
 *   最小着色率 / alpha-to-coverage / alpha-to-one）。
 *
 * @note 无效 ID 记 warning 并直接返回；仅当对应开关启用时才设置子状态
 *       （如 cull_mode、blend_func），避免无谓 GL 调用。
 * @see GLRenderingDeviceDriver::RenderPipelineCreate（状态烘焙来源）
 */
void GLRenderingDeviceDriver::CommandBindRenderPipeline(PipelineID p_pipeline) {
  if (!p_pipeline.IsValid()) {
    ARHUD_LOG_WARN("CommandBindRenderPipeline: invalid pipeline ID");
    return;
  }
  GLPipelineState* pipeline =
      reinterpret_cast<GLPipelineState*>(p_pipeline.GetId());
  if (!pipeline) {
    return;
  }
  current_pipeline_ = pipeline;
  state_cache_.UseProgram(pipeline->shader_program);
  if (pipeline->vao != 0) {
    state_cache_.BindVertexArray(pipeline->vao);
  }
  state_cache_.SetCullFace(pipeline->cull_enable);
  if (pipeline->cull_enable) {
    state_cache_.SetCullMode(pipeline->cull_mode);
    state_cache_.SetFrontFace(pipeline->front_face);
  }
  state_cache_.SetDepthTest(pipeline->depth_test_enable);
  state_cache_.SetDepthWrite(pipeline->depth_write_enable);
  state_cache_.SetDepthFunc(pipeline->depth_func);
  state_cache_.SetBlend(pipeline->blend_enable);
  if (pipeline->blend_enable) {
    state_cache_.SetBlendFunc(pipeline->blend_src_rgb, pipeline->blend_dst_rgb,
                              pipeline->blend_src_alpha,
                              pipeline->blend_dst_alpha);
  }
  // MSAA 状态
  state_cache_.SetMultisampleEnable(pipeline->multisample_enable);
  state_cache_.SetSampleShadingEnable(pipeline->sample_shading_enable);
  if (pipeline->sample_shading_enable) {
    state_cache_.SetMinSampleShading(pipeline->min_sample_shading);
  }
  state_cache_.SetAlphaToCoverage(pipeline->alpha_to_coverage_enable);
  state_cache_.SetAlphaToOne(pipeline->alpha_to_one_enable);
}

/**
 * @brief 绑定 Uniform Set（GL 立即模式）
 *
 * 遍历 `GLUniformSetInfo` 中登记的绑定资源，按类型下发纹理 / 采样器 / UBO
 * 绑定，并把纹理单元序号写入当前管线 shader 的 uniform location。
 *
 * @param[in] p_uniform_set    Uniform Set ID（封装 `GLUniformSetInfo`）
 * @param[in] p_set_index      Set 索引（GL 忽略——无 descriptor set 概念）
 * @param[in] p_dynamic_offset 动态 UBO 附加偏移（仅 kUniformBufferDynamic
 * 使用）
 *
 * @par 纹理单元分配
 *   采样器 / 纹理类资源按遍历顺序占用递增纹理单元（上限
 *   `kMaxTextureUnits`），并通过 `glUniform1i(loc, unit)` 回写 sampler
 * uniform。
 *
 * @par uniform location 解析
 *   `glUniform1i` 只作用于**当前绑定的 program**，故 location 必须从
 *   `current_pipeline_->shader_id` 关联的 `GLShaderInfo` 查找
 *   （`FindLocation(binding, type)`）；未绑定管线时跳过回写。
 *
 * @note 无效 ID 记 warning 并返回。实现映射：`glBindTexture` /
 *       `glBindSampler` / `glBindBufferBase` / `glBindBufferRange`。
 * @see
 * IRenderingDeviceDriver::CommandBindUniformSet（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandBindUniformSet(UniformSetID p_uniform_set,
                                                    uint32 p_set_index,
                                                    uint32 p_dynamic_offset) {
  (void)p_set_index;
  if (!p_uniform_set.IsValid()) {
    ARHUD_LOG_WARN("CommandBindUniformSet: invalid uniform set ID");
    return;
  }
  GLUniformSetInfo* info =
      reinterpret_cast<GLUniformSetInfo*>(p_uniform_set.GetId());
  if (!info) {
    return;
  }
  // 使用当前管线关联的 shader 查找 uniform location
  // glUniform1i 只对当前绑定的 program 有效
  GLShaderInfo* pipeline_shader_info = nullptr;
  if (current_pipeline_ != nullptr && current_pipeline_->shader_id.IsValid()) {
    pipeline_shader_info =
        reinterpret_cast<GLShaderInfo*>(current_pipeline_->shader_id.GetId());
  }
  uint32 texture_unit = 0;
  for (uint32 i = 0; i < info->resources.Size(); ++i) {
    const GLUniformSetInfo::BoundResource& res = info->resources[i];
    switch (res.type) {
      case UniformType::kSamplerWithTexture:
      case UniformType::kSamplerWithTextureBuffer:
        if (texture_unit < kMaxTextureUnits) {
          state_cache_.BindTexture2D(texture_unit, res.gl_name);
          state_cache_.BindSampler(texture_unit, res.gl_sampler_name);
          if (pipeline_shader_info) {
            int32 loc =
                pipeline_shader_info->FindLocation(res.binding, res.type);
            if (loc >= 0) {
              glUniform1i(loc, static_cast<GLint>(texture_unit));
            }
          }
          ++texture_unit;
        }
        break;
      case UniformType::kTexture:
      case UniformType::kInputAttachment:
        if (texture_unit < kMaxTextureUnits) {
          state_cache_.BindTexture2D(texture_unit, res.gl_name);
          if (pipeline_shader_info) {
            int32 loc =
                pipeline_shader_info->FindLocation(res.binding, res.type);
            if (loc >= 0) {
              glUniform1i(loc, static_cast<GLint>(texture_unit));
            }
          }
          ++texture_unit;
        }
        break;
      case UniformType::kUniformBuffer:
        state_cache_.BindUniformBuffer(res.binding, res.gl_name);
        break;
      case UniformType::kUniformBufferDynamic:
        state_cache_.BindUniformBufferRange(res.binding, res.gl_name,
                                            res.offset + p_dynamic_offset,
                                            res.range_size);
        break;
      default:
        break;
    }
  }
}

/**
 * @brief 绑定顶点缓冲区并设置顶点属性指针（GL 立即模式）
 *
 * @param[in] p_buffers 顶点缓冲区 ID 数组（下标 = binding 号）
 * @param[in] p_offsets 各缓冲区起始偏移（字节）数组；可为空
 * @param[in] p_count   p_buffers 元素个数
 *
 * @par 顶点数量推导（供 CommandDrawInstanced）
 *   以 binding 0 缓冲区为基准：`bound_vertex_count_ =
 *   (size - offset) / stride`，其中 stride 取管线顶点属性中 binding==0
 *   的属性步长，size 取自 `buffer_sizes_`。
 *
 * @par 属性指针设置
 *   遍历管线的 `vertex_attributes`，按 binding 取对应 VBO，把 `DataFormat`
 *   映射为 GL 的（分量数, 类型, 是否归一化），再以 `attr.offset + offset`
 *   作为指针偏移调用 `glVertexAttribPointer`。
 *
 * @note 实现映射：`state_cache_.BindVertexBuffer` + `glVertexAttribPointer`。
 * @see
 * IRenderingDeviceDriver::CommandBindVertexBuffers（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandBindVertexBuffers(
    const BufferID* p_buffers, const uint64* p_offsets, uint32 p_count) {
  // ── 从第一个绑定的顶点缓冲区大小推导顶点数量 ──
  bound_vertex_count_ = 0;
  if (p_count > 0 && p_buffers[0].IsValid() && current_pipeline_ != nullptr) {
    GLuint gl_buf = static_cast<GLuint>(p_buffers[0].GetId());
    const uint64* size_ptr = buffer_sizes_.GetPtr(static_cast<uint64>(gl_buf));
    if (size_ptr != nullptr && *size_ptr > 0) {
      // 使用管线顶点属性中第一个 binding 的 stride
      uint32 stride = 0;
      for (uint32 i = 0; i < current_pipeline_->vertex_attributes.Size(); ++i) {
        if (current_pipeline_->vertex_attributes[i].binding == 0) {
          stride = current_pipeline_->vertex_attributes[i].stride;
          break;
        }
      }
      if (stride > 0) {
        uint64 offset = (p_offsets != nullptr) ? p_offsets[0] : 0;
        uint64 remaining_size = (*size_ptr > offset) ? (*size_ptr - offset) : 0;
        bound_vertex_count_ = static_cast<uint32>(remaining_size / stride);
      }
    }
  }
  // ── 绑定 VBO 并设置顶点属性指针（含 offset） ──
  if (current_pipeline_ != nullptr &&
      !current_pipeline_->vertex_attributes.IsEmpty()) {
    for (uint32 i = 0; i < current_pipeline_->vertex_attributes.Size(); ++i) {
      const VertexAttribute& attr = current_pipeline_->vertex_attributes[i];
      if (attr.binding < p_count && p_buffers[attr.binding].IsValid()) {
        GLuint gl_buffer = static_cast<GLuint>(p_buffers[attr.binding].GetId());
        state_cache_.BindVertexBuffer(gl_buffer);
        GLint component_count = 4;
        uint32 gl_type = GL_FLOAT;
        GLboolean normalized = GL_FALSE;
        switch (attr.format) {
          case DataFormat::kR32G32B32A32Sfloat:
            component_count = 4;
            gl_type = GL_FLOAT;
            break;
          case DataFormat::kR32G32Sfloat:
            component_count = 2;
            gl_type = GL_FLOAT;
            break;
          case DataFormat::kR32Sfloat:
            component_count = 1;
            gl_type = GL_FLOAT;
            break;
          case DataFormat::kR16G16B16A16Sfloat:
            component_count = 4;
            gl_type = GL_HALF_FLOAT;
            break;
          case DataFormat::kR16G16Sfloat:
            component_count = 2;
            gl_type = GL_HALF_FLOAT;
            break;
          case DataFormat::kR16Sfloat:
            component_count = 1;
            gl_type = GL_HALF_FLOAT;
            break;
          case DataFormat::kR8G8B8A8Unorm:
          case DataFormat::kB8G8R8A8Unorm:
          case DataFormat::kR8G8B8A8Srgb:
            component_count = 4;
            gl_type = GL_UNSIGNED_BYTE;
            normalized = GL_TRUE;
            break;
          case DataFormat::kR8G8Unorm:
            component_count = 2;
            gl_type = GL_UNSIGNED_BYTE;
            normalized = GL_TRUE;
            break;
          case DataFormat::kR8Unorm:
            component_count = 1;
            gl_type = GL_UNSIGNED_BYTE;
            normalized = GL_TRUE;
            break;
          default:
            component_count = 4;
            gl_type = GL_FLOAT;
            break;
        }
        uint64 offset = p_offsets[attr.binding];
        glVertexAttribPointer(
            attr.location, component_count, gl_type, normalized,
            static_cast<GLsizei>(attr.stride),
            reinterpret_cast<const void*>(
                static_cast<uintptr_t>(attr.offset + offset)));
      }
    }
  }
}

/**
 * @brief 绑定索引缓冲区（GL 立即模式）
 *
 * @param[in] p_buffer 索引缓冲区 ID；无效时清零 `bound_index_count_` 并返回
 * @param[in] p_format 索引格式（uint16 / uint32）
 * @param[in] p_offset 起始偏移（GL 忽略——偏移在 draw 时以指针表达）
 *
 * @par 索引数量推导（供 CommandDrawIndexedInstanced）
 *   记录 `current_index_format_`，并按
 *   `bound_index_count_ = size / sizeof(index)` 推导索引数量，size 取自
 *   `buffer_sizes_`。
 *
 * @note 实现映射：`state_cache_.BindIndexBuffer`。
 * @see
 * IRenderingDeviceDriver::CommandBindIndexBuffer（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandBindIndexBuffer(BufferID p_buffer,
                                                     IndexBufferFormat p_format,
                                                     uint64 p_offset) {
  (void)p_offset;
  if (!p_buffer.IsValid()) {
    bound_index_count_ = 0;
    return;
  }
  current_index_format_ = p_format;
  GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
  state_cache_.BindIndexBuffer(gl_buffer);
  // ── 从索引缓冲区大小推导索引数量 ──
  bound_index_count_ = 0;
  const uint64* size_ptr = buffer_sizes_.GetPtr(static_cast<uint64>(gl_buffer));
  if (size_ptr != nullptr && *size_ptr > 0) {
    uint32 index_size = (p_format == IndexBufferFormat::kUint32)
                            ? sizeof(uint32)
                            : sizeof(uint16);
    bound_index_count_ = static_cast<uint32>(*size_ptr / index_size);
  }
}

/**
 * @brief 非索引绘制（GL 立即模式）
 *
 * @param[in] p_vertex_count   顶点数量
 * @param[in] p_instance_count 实例数量（<=1 走非实例化路径）
 * @param[in] p_base_vertex    起始顶点偏移
 * @param[in] p_first_instance 起始实例偏移（GL 忽略——需 ARB_base_instance）
 *
 * @note 固定图元 GL_TRIANGLES（本契约面仅三角形）；实现映射：
 *       `glDrawArrays` / `glDrawArraysInstanced`；结束后 `GL_CHECK_ERROR()`。
 * @see IRenderingDeviceDriver::CommandDraw（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandDraw(uint32 p_vertex_count,
                                          uint32 p_instance_count,
                                          uint32 p_base_vertex,
                                          uint32 p_first_instance) {
  (void)p_first_instance;
  if (p_instance_count <= 1) {
    glDrawArrays(GL_TRIANGLES, static_cast<GLint>(p_base_vertex),
                 static_cast<GLsizei>(p_vertex_count));
  } else {
    glDrawArraysInstanced(GL_TRIANGLES, static_cast<GLint>(p_base_vertex),
                          static_cast<GLsizei>(p_vertex_count),
                          static_cast<GLsizei>(p_instance_count));
  }
  GL_CHECK_ERROR();
}

/**
 * @brief 索引绘制（GL 立即模式）
 *
 * @param[in] p_index_count    索引数量
 * @param[in] p_instance_count 实例数量（<=1 走非实例化路径）
 * @param[in] p_first_index    起始索引偏移（换算为字节指针偏移）
 * @param[in] p_vertex_offset  顶点偏移（GL 忽略）
 * @param[in] p_first_instance 起始实例偏移（GL 忽略——需 ARB_base_instance）
 *
 * @par 索引类型
 *   依据 `CommandBindIndexBuffer` 记录的 `current_index_format_` 选择
 *   `GL_UNSIGNED_INT`（uint32）或 `GL_UNSIGNED_SHORT`（uint16）。
 *
 * @note 固定图元 GL_TRIANGLES；实现映射：`glDrawElements` /
 *       `glDrawElementsInstanced`；结束后 `GL_CHECK_ERROR()`。
 * @see IRenderingDeviceDriver::CommandDrawIndexed（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandDrawIndexed(uint32 p_index_count,
                                                 uint32 p_instance_count,
                                                 uint32 p_first_index,
                                                 int32 p_vertex_offset,
                                                 uint32 p_first_instance) {
  (void)p_first_instance;
  GLenum gl_type = GL_UNSIGNED_SHORT;
  uint32 index_size = sizeof(uint16);
  if (current_index_format_ == IndexBufferFormat::kUint32) {
    gl_type = GL_UNSIGNED_INT;
    index_size = sizeof(uint32);
  }
  const void* indices = reinterpret_cast<const void*>(
      static_cast<uintptr_t>(p_first_index * index_size));
  if (p_instance_count <= 1) {
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(p_index_count), gl_type,
                   indices);
  } else {
    glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(p_index_count),
                            gl_type, indices,
                            static_cast<GLsizei>(p_instance_count));
  }
  GL_CHECK_ERROR();
}

/**
 * @brief 非索引实例化绘制（GL 立即模式）
 *
 * 使用已绑定顶点缓冲区推导的 `bound_vertex_count_` 作为顶点数，图元取自
 * 当前管线的 `render_primitive`。
 *
 * @param[in] p_instance_count 实例数量
 * @param[in] p_base_instance  起始实例偏移（GL 忽略——需 ARB_base_instance）
 *
 * @note `bound_vertex_count_ == 0`（未绑定 / 未推导）时记 warning 并跳过绘制。
 * @note 实现映射：`glDrawArraysInstanced`；结束后 `GL_CHECK_ERROR()`。
 * @see
 * IRenderingDeviceDriver::CommandDrawInstanced（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandDrawInstanced(uint32 p_instance_count,
                                                   uint32 p_base_instance) {
  if (p_base_instance != 0) {
    ARHUD_LOG_WARN(
        "CommandDrawInstanced: p_base_instance=%u ignored "
        "(needs ARB_base_instance / GL 4.2+)",
        p_base_instance);
  }
  if (bound_vertex_count_ == 0) {
    ARHUD_LOG_WARN(
        "CommandDrawInstanced: bound_vertex_count_ is 0, skipping draw");
    return;
  }
  GLenum gl_mode = GL_TRIANGLES;
  if (current_pipeline_ != nullptr) {
    gl_mode = static_cast<GLenum>(
        RenderPrimitiveToGLMode(current_pipeline_->render_primitive));
  }
  glDrawArraysInstanced(gl_mode, 0, static_cast<GLsizei>(bound_vertex_count_),
                        static_cast<GLsizei>(p_instance_count));
  GL_CHECK_ERROR();
}

/**
 * @brief 索引实例化绘制（GL 立即模式）
 *
 * 使用已绑定索引缓冲区推导的 `bound_index_count_` 作为索引数，图元取自
 * 当前管线，索引类型取自 `current_index_format_`。
 *
 * @param[in] p_instance_count 实例数量
 * @param[in] p_base_instance  起始实例偏移（GL 忽略——需 ARB_base_instance）
 *
 * @note `bound_index_count_ == 0`（未绑定 / 未推导）时记 warning 并跳过绘制。
 * @note 实现映射：`glDrawElementsInstanced`（索引指针传 nullptr）；
 *       结束后 `GL_CHECK_ERROR()`。
 * @see
 * IRenderingDeviceDriver::CommandDrawIndexedInstanced（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandDrawIndexedInstanced(
    uint32 p_instance_count, uint32 p_base_instance) {
  if (p_base_instance != 0) {
    ARHUD_LOG_WARN(
        "CommandDrawIndexedInstanced: p_base_instance=%u ignored "
        "(needs ARB_base_instance / GL 4.2+)",
        p_base_instance);
  }
  if (bound_index_count_ == 0) {
    ARHUD_LOG_WARN(
        "CommandDrawIndexedInstanced: bound_index_count_ is 0, skipping draw");
    return;
  }
  GLenum gl_mode = GL_TRIANGLES;
  if (current_pipeline_ != nullptr) {
    gl_mode = static_cast<GLenum>(
        RenderPrimitiveToGLMode(current_pipeline_->render_primitive));
  }
  GLenum gl_type = GL_UNSIGNED_SHORT;
  if (current_index_format_ == IndexBufferFormat::kUint32) {
    gl_type = GL_UNSIGNED_INT;
  }
  glDrawElementsInstanced(gl_mode, static_cast<GLsizei>(bound_index_count_),
                          gl_type, nullptr,
                          static_cast<GLsizei>(p_instance_count));
  GL_CHECK_ERROR();
}

/**
 * @brief 派发计算着色器（GL 立即模式）
 *
 * @param[in] p_x_groups X 维度工作组数量
 * @param[in] p_y_groups Y 维度工作组数量
 * @param[in] p_z_groups Z 维度工作组数量
 *
 * @par 版本门禁
 *   先做运行时能力检查：桌面 OpenGL 需 >= 4.3，OpenGL ES 需 >= 3.1；
 *   不满足则记 error 并直接返回（不调用 GL）。
 *
 * @note 实现映射：`glDispatchCompute`；结束后 `GL_CHECK_ERROR()`。
 * @see IRenderingDeviceDriver::CommandDispatch（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandDispatch(uint32 p_x_groups,
                                              uint32 p_y_groups,
                                              uint32 p_z_groups) {
  // 运行时检查：桌面 OpenGL 4.3+ 或 OpenGL ES 3.1+
  const bool compute_supported =
      capabilities_.device_family == DeviceFamily::kOpenGLES
          ? (capabilities_.version_major > 3 ||
             (capabilities_.version_major == 3 &&
              capabilities_.version_minor >= 1))
          : (capabilities_.version_major > 4 ||
             (capabilities_.version_major == 4 &&
              capabilities_.version_minor >= 3));
  if (!compute_supported) {
    ARHUD_LOG_ERROR("kUnsupported",
                    "CommandDispatch requires OpenGL 4.3+ or OpenGL ES 3.1+, "
                    "current: %s %u.%u",
                    api_name_, capabilities_.version_major,
                    capabilities_.version_minor);
    return;
  }
  glDispatchCompute(static_cast<GLuint>(p_x_groups),
                    static_cast<GLuint>(p_y_groups),
                    static_cast<GLuint>(p_z_groups));
  GL_CHECK_ERROR();
}

/**
 * @brief 设置视口（GL 立即模式）
 *
 * @param[in] p_x      视口左下角 X
 * @param[in] p_y      视口左下角 Y
 * @param[in] p_width  视口宽度
 * @param[in] p_height 视口高度
 *
 * @note 经 `state_cache_.SetViewport` 缓存去重后下发 `glViewport`。
 * @see IRenderingDeviceDriver::CommandSetViewport（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandSetViewport(int32 p_x, int32 p_y,
                                                 uint32 p_width,
                                                 uint32 p_height) {
  state_cache_.SetViewport(p_x, p_y, p_width, p_height);
}

/**
 * @brief 设置裁剪矩形（GL 立即模式）
 *
 * @param[in] p_x      矩形左下角 X
 * @param[in] p_y      矩形左下角 Y
 * @param[in] p_width  矩形宽度
 * @param[in] p_height 矩形高度
 *
 * @note 先 `SetScissorEnable(true)` 再经 `state_cache_` 下发 `glScissor`
 *       （缓存去重）。
 * @see IRenderingDeviceDriver::CommandSetScissor（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandSetScissor(int32 p_x, int32 p_y,
                                                uint32 p_width,
                                                uint32 p_height) {
  state_cache_.SetScissorEnable(true);
  state_cache_.SetScissor(p_x, p_y, p_width, p_height);
}

/**
 * @brief 设置混合常量（GL 立即模式）
 *
 * 供 `GL_CONSTANT_COLOR` / `GL_CONSTANT_ALPHA` 等混合因子使用。
 *
 * @param[in] p_r 红色分量
 * @param[in] p_g 绿色分量
 * @param[in] p_b 蓝色分量
 * @param[in] p_a 透明度分量
 *
 * @note 直接调用 `glBlendColor`（不经状态缓存）。
 * @see
 * IRenderingDeviceDriver::CommandSetBlendConstants（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandSetBlendConstants(float p_r, float p_g,
                                                       float p_b, float p_a) {
  glBlendColor(p_r, p_g, p_b, p_a);
}

/**
 * @brief 清除缓冲区（GL 立即模式）
 *
 * 把 `[p_offset, p_offset + p_size)` 范围清零。
 *
 * @param[in] p_buffer 缓冲区 ID；无效时直接返回
 * @param[in] p_offset 起始偏移（字节）
 * @param[in] p_size   清除大小（字节）；为 0 时不做任何操作
 *
 * @par 实现方式
 *   GL 无对等的 `glClearNamedBufferData` 直接喂零路径，故在 CPU 侧分配并
 *   清零 `p_size` 字节缓冲，经 `glBufferSubData(GL_ARRAY_BUFFER, ...)` 写入后
 *   立即释放。
 *
 * @note 目标绑定到 GL_ARRAY_BUFFER（经 `state_cache_.BindVertexBuffer`）。
 * @see IRenderingDeviceDriver::CommandClearBuffer（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandClearBuffer(BufferID p_buffer,
                                                 uint64 p_offset,
                                                 uint64 p_size) {
  if (!p_buffer.IsValid()) {
    return;
  }

  GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
  state_cache_.BindVertexBuffer(gl_buffer);

  if (p_size > 0) {
    uint8* zero_data =
        static_cast<uint8*>(memory::Alloc(static_cast<uint64>(p_size)));
    if (zero_data) {
      for (uint64 i = 0; i < p_size; ++i) {
        zero_data[i] = 0;
      }
      glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(p_offset),
                      static_cast<GLsizeiptr>(p_size), zero_data);
      memory::Free(zero_data);
    }
  }
}

/**
 * @brief 复制缓冲区（GL 立即模式）
 *
 * @param[in] p_src_buffer 源缓冲区 ID
 * @param[in] p_dst_buffer 目标缓冲区 ID
 * @param[in] p_regions    复制区域数组（每项含 src/dst 偏移与字节数）
 *
 * @note 任一 ID 无效时直接返回。实现：把源 / 目标分别绑到
 *       `GL_COPY_READ_BUFFER` / `GL_COPY_WRITE_BUFFER`，逐区域调用
 *       `glCopyBufferSubData`，结束后解绑。
 * @see IRenderingDeviceDriver::CommandCopyBuffer（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandCopyBuffer(
    BufferID p_src_buffer, BufferID p_dst_buffer,
    VectorView<BufferCopyRegion> p_regions) {
  if (!p_src_buffer.IsValid() || !p_dst_buffer.IsValid()) {
    return;
  }

  GLuint src_gl = static_cast<GLuint>(p_src_buffer.GetId());
  GLuint dst_gl = static_cast<GLuint>(p_dst_buffer.GetId());

  glBindBuffer(GL_COPY_READ_BUFFER, src_gl);
  glBindBuffer(GL_COPY_WRITE_BUFFER, dst_gl);

  for (uint32 i = 0; i < p_regions.Size(); ++i) {
    const BufferCopyRegion& region = p_regions[i];

    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                        static_cast<GLintptr>(region.src_offset),
                        static_cast<GLintptr>(region.dst_offset),
                        static_cast<GLsizeiptr>(region.size));
  }

  glBindBuffer(GL_COPY_READ_BUFFER, 0);
  glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

/**
 * @brief 缓冲区数据拷贝到纹理（GL PBO 上传）
 *
 * @param[in] p_src_buffer  源缓冲区 ID（充当 PBO）
 * @param[in] p_dst_texture 目标纹理 ID（经 `texture_infos_` 查格式 / 目标）
 * @param[in] p_regions     拷贝区域数组（含子资源、纹理偏移、缓冲区偏移）
 *
 * @par PBO 上传流程
 *   把源 Buffer 绑到 `GL_PIXEL_UNPACK_BUFFER`，按目标纹理类型走
 *   `glTexSubImage2D`（GL_TEXTURE_2D / CUBE_MAP，cube 面按 `layer` 选
 *   POSITIVE_X 偏移）或 `glTexSubImage3D`（2D_ARRAY /
 * 3D）；`region.buffer_offset` 作为 PBO 内字节偏移；结束后解绑 PBO。
 *
 * @note 任一 ID 无效或纹理未登记时返回；像素格式 / 类型经
 *       `DataFormatToGLFormat/Type` 转换。
 * @see
 * IRenderingDeviceDriver::CommandCopyBufferToTexture（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandCopyBufferToTexture(
    BufferID p_src_buffer, TextureID p_dst_texture,
    VectorView<BufferTextureCopyRegion> p_regions) {
  if (!p_src_buffer.IsValid() || !p_dst_texture.IsValid()) {
    return;
  }

  GLTextureInfo* tex_info = nullptr;
  if (texture_infos_.Has(p_dst_texture.GetId())) {
    tex_info = &texture_infos_[p_dst_texture.GetId()];
  }
  if (tex_info == nullptr) {
    return;
  }

  GLuint src_gl = static_cast<GLuint>(p_src_buffer.GetId());
  GLuint dst_gl = static_cast<GLuint>(p_dst_texture.GetId());

  state_cache_.BindPixelUnpackBuffer(src_gl);

  state_cache_.BindTexture(tex_info->target, dst_gl);

  uint32 gl_format = DataFormatToGLFormat(tex_info->format);
  uint32 gl_type = DataFormatToGLType(tex_info->format);

  for (uint32 i = 0; i < p_regions.Size(); ++i) {
    const BufferTextureCopyRegion& region = p_regions[i];

    if (tex_info->target == GL_TEXTURE_2D ||
        tex_info->target == GL_TEXTURE_CUBE_MAP) {
      GLenum tex_target = tex_info->target;
      if (tex_info->target == GL_TEXTURE_CUBE_MAP) {
        tex_target =
            GL_TEXTURE_CUBE_MAP_POSITIVE_X + region.texture_subresource.layer;
      }

      glTexSubImage2D(tex_target,
                      static_cast<GLint>(region.texture_subresource.mipmap),
                      region.texture_offset_x, region.texture_offset_y,
                      region.texture_region_width, region.texture_region_height,
                      gl_format, gl_type,
                      reinterpret_cast<const void*>(
                          static_cast<uintptr_t>(region.buffer_offset)));
    } else if (tex_info->target == GL_TEXTURE_2D_ARRAY ||
               tex_info->target == GL_TEXTURE_3D) {
      glTexSubImage3D(tex_info->target,
                      static_cast<GLint>(region.texture_subresource.mipmap),
                      region.texture_offset_x, region.texture_offset_y,
                      region.texture_offset_z, region.texture_region_width,
                      region.texture_region_height, region.texture_region_depth,
                      gl_format, gl_type,
                      reinterpret_cast<const void*>(
                          static_cast<uintptr_t>(region.buffer_offset)));
    }
  }

  GL_CHECK_ERROR();

  state_cache_.BindPixelUnpackBuffer(0);
}

/**
 * @brief 清除颜色纹理（GL 立即模式）
 *
 * @param[in] p_texture      目标纹理 ID
 * @param[in] p_color_r      清除颜色 R
 * @param[in] p_color_g      清除颜色 G
 * @param[in] p_color_b      清除颜色 B
 * @param[in] p_color_a      清除颜色 A
 * @param[in] p_subresources 子资源范围（GL 忽略——仅清 level 0 单层）
 *
 * @par 实现方式
 *   临时创建 FBO，把纹理挂到 `GL_COLOR_ATTACHMENT0`，设置 draw buffer 与
 *   清除色后 `glClear(GL_COLOR_BUFFER_BIT)`，随后解挂并删除 FBO。
 *
 * @note FBO 创建失败时直接返回。
 * @see
 * IRenderingDeviceDriver::CommandClearColorTexture（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandClearColorTexture(
    TextureID p_texture, float p_color_r, float p_color_g, float p_color_b,
    float p_color_a, const TextureSubresourceRange& p_subresources) {
  (void)p_subresources;

  if (!p_texture.IsValid()) {
    return;
  }

  GLuint gl_texture = static_cast<GLuint>(p_texture.GetId());

  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  if (fbo == 0) {
    return;
  }

  state_cache_.BindFramebuffer(fbo);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         gl_texture, 0);

  GLenum draw_buf = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &draw_buf);

  glClearColor(p_color_r, p_color_g, p_color_b, p_color_a);

  glEnable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);

  glClear(GL_COLOR_BUFFER_BIT);

  GL_CHECK_ERROR();

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0,
                         0);

  glDeleteFramebuffers(1, &fbo);
}

/**
 * @brief 清除深度 / 模板纹理（GL 立即模式）
 *
 * @param[in] p_texture      目标纹理 ID
 * @param[in] p_depth        清除深度值
 * @param[in] p_stencil      清除模板值（GL 忽略——仅清深度位面）
 * @param[in] p_subresources 子资源范围（GL 忽略——仅清 level 0 单层）
 *
 * @par 实现方式
 *   临时创建 FBO，把纹理挂到 `GL_DEPTH_ATTACHMENT`，设置清除深度后
 *   `glClear(GL_DEPTH_BUFFER_BIT)`，随后解挂并删除 FBO。
 *
 * @note FBO 创建失败时直接返回。
 * @see
 * IRenderingDeviceDriver::CommandClearDepthStencilTexture（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandClearDepthStencilTexture(
    TextureID p_texture, float p_depth, uint32 p_stencil,
    const TextureSubresourceRange& p_subresources) {
  (void)p_stencil;
  (void)p_subresources;

  if (!p_texture.IsValid()) {
    return;
  }

  GLuint gl_texture = static_cast<GLuint>(p_texture.GetId());

  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  if (fbo == 0) {
    return;
  }

  state_cache_.BindFramebuffer(fbo);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                         gl_texture, 0);

  glClearDepth(static_cast<GLdouble>(p_depth));

  glEnable(GL_DEPTH_TEST);

  glClear(GL_DEPTH_BUFFER_BIT);

  GL_CHECK_ERROR();

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0,
                         0);

  glDeleteFramebuffers(1, &fbo);
}

/**
 * @brief 更新缓冲区数据（GL 立即模式，经 staging 中转）
 *
 * @param[in] p_dst_buffer 目标缓冲区 ID
 * @param[in] p_offset     写入偏移（字节）
 * @param[in] p_size       写入大小（字节）
 * @param[in] p_data       源数据指针（CPU 内存）
 *
 * @par 实现方式（staging 中转）
 *   新建临时 Buffer 绑到 `GL_COPY_READ_BUFFER`，`glBufferData` 预分配后
 *   `glMapBuffer` 映射、`memcpy` 写入源数据并解映射；再把目标 Buffer 绑到
 *   `GL_COPY_WRITE_BUFFER`，经 `glCopyBufferSubData` 拷到目标偏移；最后解绑并
 *   删除临时 Buffer。
 *
 * @note 任一入参无效（ID 非法 / p_data 为空 / p_size 为 0）时返回；
 *       `glMapBuffer` 失败记 error。
 * @see IRenderingDeviceDriver::CommandBufferUpdate（rendering_device_driver.h）
 */
void GLRenderingDeviceDriver::CommandBufferUpdate(BufferID p_dst_buffer,
                                                  uint32 p_offset,
                                                  uint32 p_size,
                                                  const void* p_data) {
  if (!p_dst_buffer.IsValid() || p_data == nullptr || p_size == 0) {
    return;
  }

  GLuint dst_gl = static_cast<GLuint>(p_dst_buffer.GetId());

  GLuint staging_buffer = 0;
  glGenBuffers(1, &staging_buffer);
  if (staging_buffer == 0) {
    return;
  }

  glBindBuffer(GL_COPY_READ_BUFFER, staging_buffer);
  glBufferData(GL_COPY_READ_BUFFER, static_cast<GLsizeiptr>(p_size), nullptr,
               GL_STREAM_DRAW);

  void* mapped = glMapBuffer(GL_COPY_READ_BUFFER, GL_WRITE_ONLY);
  if (mapped) {
    memcpy(mapped, p_data, static_cast<size_t>(p_size));
    glUnmapBuffer(GL_COPY_READ_BUFFER);
  } else {
    ARHUD_LOG_ERROR("kFailed",
                    "CommandBufferUpdate: glMapBuffer failed! dst=%u, size=%u",
                    static_cast<uint32>(p_dst_buffer.GetId()), p_size);
  }

  glBindBuffer(GL_COPY_WRITE_BUFFER, dst_gl);
  glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0,
                      static_cast<GLintptr>(p_offset),
                      static_cast<GLsizeiptr>(p_size));

  GL_CHECK_ERROR();

  glBindBuffer(GL_COPY_READ_BUFFER, 0);
  glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

  glDeleteBuffers(1, &staging_buffer);
}

}  // namespace arhud
