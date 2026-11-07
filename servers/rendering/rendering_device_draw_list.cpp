/**
 * @file rendering_device_draw_list.cpp
 * @brief RenderingDevice DrawList + ComputeList 实现
 *
 * @par 日志策略
 *   - INFO：会话边界（Begin/End）与关键 GPU 提交（Draw 系/Dispatch/Clear），
 *     均附 frame_number_ 便于按帧过滤
 *   - ERROR/WARN：所有失败路径（原有，不变）
 *   - 刻意不加：BindRenderPipeline/BindUniformSet/BindVertex(Index)Buffers
 *     等绑定类成功日志——每帧数十次高频调用会刷屏，且失败路径已有
 *     ERROR 覆盖；冗余绑定短路（bound_ 命中）亦不记录（属常态路径）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-06-14
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "os/memory.h"
#include "rendering_device.h"
#include "rendering_device_driver.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// DrawList（高层绘制 API）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 开始一个绘制列表（无 Framebuffer 绑定）
 *
 * 初始化绘制列表状态，重置已绑定管线和 Uniform Set 记录。
 * 不会启动 Render Pass，需后续手动绑定 Framebuffer 或使用其他 API。
 *
 * @return DrawListID 绘制列表句柄，失败时返回 kInvalidDrawListId
 *
 * @pre 设备已通过 Initialize() 初始化
 * @pre 当前没有活跃的 DrawList 或 ComputeList
 *
 * @note 同一时刻只允许一个活跃的 DrawList
 */
DrawListID RenderingDevice::DrawListBegin() {
  if (!initialized_) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::DrawListBegin() not initialized");
    return kInvalidDrawListId;
  }

  if (draw_list_.active) {
    ARHUD_LOG_ERROR(
        "kFailed", "RenderingDevice::DrawListBegin() draw list already active");
    return kInvalidDrawListId;
  }

  draw_list_.active = true;
  draw_list_.render_pass_active = false;
  draw_list_.bound_pipeline = RDPipelineID();
  draw_list_.bound_uniform_sets.Clear();

  ARHUD_LOG_INFO("DrawListBegin: bare session opened (frame=%llu)",
                 static_cast<unsigned long long>(frame_number_));

  return 0;
}

/**
 * @brief 开始绘制列表并绑定 Framebuffer，同时启动 Render Pass
 *
 * 根据 Framebuffer 关联的 RenderPass 自动构建清除值，
 * 对颜色/深度/模板附件按 load_op 为 kClear 时执行清除。
 * 若 Framebuffer 尺寸为 0 且存在 Swapchain Surface，则使用 Surface
 * 尺寸作为视口。
 *
 * @param[in] p_framebuffer      目标 Framebuffer 句柄
 * @param[in] p_clear_colors     是否清除颜色附件
 * @param[in] p_clear_depth      是否清除深度附件
 * @param[in] p_clear_stencil    是否清除模板附件
 * @param[in] p_clear_color      RGBA 清除值数组（4 个 float），可为 nullptr
 * @param[in] p_clear_depth_val  深度清除值
 * @param[in] p_clear_stencil_val 模板清除值
 *
 * @return DrawListID 绘制列表句柄，失败时返回 kInvalidDrawListId
 *
 * @pre 设备已通过 Initialize() 初始化
 * @pre 当前没有活跃的 DrawList
 * @pre p_framebuffer 有效且关联了合法的 RenderPass
 */
DrawListID RenderingDevice::DrawListBegin(
    RDFramebufferID p_framebuffer, bool p_clear_colors, bool p_clear_depth,
    bool p_clear_stencil, const float p_clear_color[4], float p_clear_depth_val,
    uint32 p_clear_stencil_val) {
  if (!initialized_) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::DrawListBegin(FBO) not initialized");
    return kInvalidDrawListId;
  }

  if (draw_list_.active) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderingDevice::DrawListBegin(FBO) draw list already active");
    return kInvalidDrawListId;
  }

  if (p_framebuffer.IsNull()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::DrawListBegin(FBO) null framebuffer");
    return kInvalidDrawListId;
  }

  const Framebuffer* fb = framebuffer_owner_.GetOrNull(p_framebuffer.GetRid());
  if (fb == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderingDevice::DrawListBegin(FBO) invalid framebuffer RID");
    return kInvalidDrawListId;
  }

  RenderPassID rp_driver_id =
      RenderPassGetDriverId(RDRenderPassID(fb->render_pass));
  if (!rp_driver_id.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::DrawListBegin(FBO) invalid render pass "
                    "for framebuffer");
    return kInvalidDrawListId;
  }

  const RenderPass* rp = render_pass_owner_.GetOrNull(fb->render_pass);
  uint32 attachment_count = (rp != nullptr) ? rp->attachments.Size() : 0;

  LocalVector<RenderPassClearValue> clear_values;
  clear_values.Resize(attachment_count);

  uint32 clear_mask = 0;

  for (uint32 i = 0; i < attachment_count; ++i) {
    const Attachment& att = rp->attachments[i];
    bool is_depth = (att.format == DataFormat::kD16Unorm ||
                     att.format == DataFormat::kD32Sfloat ||
                     att.format == DataFormat::kS8Uint ||
                     att.format == DataFormat::kD16UnormS8Uint ||
                     att.format == DataFormat::kD24UnormS8Uint ||
                     att.format == DataFormat::kD32SfloatS8Uint);

    if (!is_depth) {
      if (p_clear_colors && att.load_op == AttachmentLoadOp::kClear) {
        clear_values[i].color[0] =
            p_clear_color != nullptr ? p_clear_color[0] : 0.0f;
        clear_values[i].color[1] =
            p_clear_color != nullptr ? p_clear_color[1] : 0.0f;
        clear_values[i].color[2] =
            p_clear_color != nullptr ? p_clear_color[2] : 0.0f;
        clear_values[i].color[3] =
            p_clear_color != nullptr ? p_clear_color[3] : 1.0f;
        clear_mask |= (1u << i);
      }
    } else {
      if (p_clear_depth && att.load_op == AttachmentLoadOp::kClear) {
        clear_values[i].depth = p_clear_depth_val;
        clear_mask |= (1u << i);
      }
      if (p_clear_stencil && att.stencil_load_op == AttachmentLoadOp::kClear) {
        clear_values[i].stencil = p_clear_stencil_val;
        clear_mask |= (1u << i);
      }
    }
  }

  uint32 viewport_w = fb->width;
  uint32 viewport_h = fb->height;

  device_driver_->CommandBeginRenderPass(
      rp_driver_id, fb->driver_id,
      VectorView<RenderPassClearValue>(clear_values.Data(),
                                       clear_values.Size()),
      clear_mask, 0, 0, viewport_w, viewport_h);

  draw_list_.active = true;
  draw_list_.render_pass_active = true;
  draw_list_.bound_pipeline = RDPipelineID();
  draw_list_.bound_uniform_sets.Clear();

  ARHUD_LOG_INFO(
      "DrawListBegin: framebuffer %llu, viewport %ux%u, clear_mask=0x%x (c=%d "
      "d=%d s=%d), attachments=%u, frame=%llu",
      static_cast<unsigned long long>(fb->driver_id.GetId()), viewport_w,
      viewport_h, clear_mask, p_clear_colors ? 1 : 0, p_clear_depth ? 1 : 0,
      p_clear_stencil ? 1 : 0, attachment_count,
      static_cast<unsigned long long>(frame_number_));

  return 0;
}

/**
 * @brief 设置绘制列表的视口矩形
 *
 * @param[in] p_list   DrawListBegin() 返回的句柄
 * @param[in] p_x      视口左下角 X（像素）
 * @param[in] p_y      视口左下角 Y（像素）
 * @param[in] p_width  视口宽度（像素）
 * @param[in] p_height 视口高度（像素）
 *
 * @pre p_list 有效且有活跃的 DrawList
 *
 * @par OpenGL 等效
 *   glViewport(x, y, width, height)
 */
void RenderingDevice::DrawListSetViewport(DrawListID p_list, int32 p_x,
                                          int32 p_y, uint32 p_width,
                                          uint32 p_height) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  device_driver_->CommandSetViewport(p_x, p_y, p_width, p_height);
}

/**
 * @brief 设置绘制列表的裁剪矩形
 *
 * @param[in] p_list   DrawListBegin() 返回的句柄
 * @param[in] p_x      裁剪矩形左下角 X（像素）
 * @param[in] p_y      裁剪矩形左下角 Y（像素）
 * @param[in] p_width  裁剪矩形宽度（像素）
 * @param[in] p_height 裁剪矩形高度（像素）
 *
 * @pre p_list 有效且有活跃的 DrawList
 *
 * @par OpenGL 等效
 *   glScissor(x, y, width, height)
 */
void RenderingDevice::DrawListSetScissor(DrawListID p_list, int32 p_x,
                                         int32 p_y, uint32 p_width,
                                         uint32 p_height) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  device_driver_->CommandSetScissor(p_x, p_y, p_width, p_height);
}

/**
 * @brief 设置绘制列表的混合常量颜色
 *
 * @param[in] p_list DrawListBegin() 返回的句柄
 * @param[in] p_r    红色分量 [0.0, 1.0]
 * @param[in] p_g    绿色分量 [0.0, 1.0]
 * @param[in] p_b    蓝色分量 [0.0, 1.0]
 * @param[in] p_a    Alpha 分量 [0.0, 1.0]
 *
 * @pre p_list 有效且有活跃的 DrawList
 *
 * @par OpenGL 等效
 *   glBlendColor(r, g, b, a)
 */
void RenderingDevice::DrawListSetBlendConstants(DrawListID p_list, float p_r,
                                                float p_g, float p_b,
                                                float p_a) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  device_driver_->CommandSetBlendConstants(p_r, p_g, p_b, p_a);
}

/**
 * @brief 在绘制列表中绑定渲染管线
 *
 * 若与上次绑定的管线相同则跳过（冗余绑定优化）。
 *
 * @param[in] p_list     DrawListBegin() 返回的句柄
 * @param[in] p_pipeline 渲染管线句柄
 *
 * @pre p_list 有效且有活跃的 DrawList
 * @pre p_pipeline 为有效的渲染管线 RID
 */
void RenderingDevice::DrawListBindRenderPipeline(DrawListID p_list,
                                                 RDPipelineID p_pipeline) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  // 跳过冗余绑定
  if (draw_list_.bound_pipeline == p_pipeline) {
    return;
  }
  Pipeline* pipeline = pipeline_owner_.GetOrNull(p_pipeline.GetRid());
  if (pipeline == nullptr) {
    ARHUD_LOG_ERROR("kInvalidParameter",
                    "DrawListBindRenderPipeline: invalid pipeline RID");
    return;
  }
  draw_list_.bound_pipeline = p_pipeline;
  device_driver_->CommandBindRenderPipeline(pipeline->driver_id);
}

/**
 * @brief 在绘制列表中绑定 Uniform Set
 *
 * 若指定 set_index 已绑定相同 UniformSet 则跳过（冗余绑定优化）。
 * 兼容性由开发者保证，不做运行时 Shader 布局校验。
 *
 * @param[in] p_list        DrawListBegin() 返回的句柄
 * @param[in] p_uniform_set Uniform Set 句柄
 * @param[in] p_set_index   绑定点索引（对应 Shader 中的 set 编号）
 *
 * @pre p_list 有效且有活跃的 DrawList
 * @pre p_uniform_set 为有效的 UniformSet RID
 */
void RenderingDevice::DrawListBindUniformSet(DrawListID p_list,
                                             RDUniformSetID p_uniform_set,
                                             uint32 p_set_index) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  UniformSet* uniform_set =
      uniform_set_owner_.GetOrNull(p_uniform_set.GetRid());
  if (uniform_set == nullptr) {
    ARHUD_LOG_ERROR("kInvalidParameter",
                    "DrawListBindUniformSet: invalid uniform set RID");
    return;
  }
  // 验证 UniformSet 与当前 Pipeline 的 Shader 兼容性
  // 不同 shader 的同名 uniform block 布局可能完全兼容（如 ProjectionBlock），
  // 因此不强制要求 uniform_set->shader_id == bound_shader->driver_id。
  // 兼容性由开发者保证，此处不做运行时限制。
  // 跳过冗余绑定
  if (p_set_index < draw_list_.bound_uniform_sets.Size() &&
      draw_list_.bound_uniform_sets[p_set_index] == p_uniform_set) {
    return;
  }
  // 确保 vector 足够大
  while (draw_list_.bound_uniform_sets.Size() <= p_set_index) {
    draw_list_.bound_uniform_sets.PushBack(RDUniformSetID());
  }
  draw_list_.bound_uniform_sets[p_set_index] = p_uniform_set;
  device_driver_->CommandBindUniformSet(uniform_set->driver_id, p_set_index, 0);
}

/**
 * @brief 在绘制列表中绑定 Uniform Set（带动态偏移）
 *
 * 与 DrawListBindUniformSet() 不同，即使同一 UniformSet 以相同索引绑定，
 * 也不会跳过冗余绑定——因为动态偏移可能不同。
 *
 * @param[in] p_list           DrawListBegin() 返回的句柄
 * @param[in] p_uniform_set    Uniform Set 句柄
 * @param[in] p_set_index      绑定点索引（对应 Shader 中的 set 编号）
 * @param[in] p_dynamic_offset 动态偏移量（字节）
 *
 * @pre p_list 有效且有活跃的 DrawList
 * @pre p_uniform_set 为有效的 UniformSet RID
 */
void RenderingDevice::DrawListBindUniformSetWithDynamicOffset(
    DrawListID p_list, RDUniformSetID p_uniform_set, uint32 p_set_index,
    uint32 p_dynamic_offset) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  UniformSet* uniform_set =
      uniform_set_owner_.GetOrNull(p_uniform_set.GetRid());
  if (uniform_set == nullptr) {
    return;
  }
  // 不跳过冗余绑定 — 同一 UniformSet 可能以不同动态偏移多次绑定
  while (draw_list_.bound_uniform_sets.Size() <= p_set_index) {
    draw_list_.bound_uniform_sets.PushBack(RDUniformSetID());
  }
  draw_list_.bound_uniform_sets[p_set_index] = p_uniform_set;
  device_driver_->CommandBindUniformSet(uniform_set->driver_id, p_set_index,
                                        p_dynamic_offset);
}

/**
 * @brief 在绘制列表中绑定顶点缓冲区数组
 *
 * 将多个顶点缓冲区及其偏移批量绑定到连续的 vertex binding 点。
 * 实际绑定数量不超过栈数组容量 kMaxVertexBindings。
 *
 * @param[in] p_list    DrawListBegin() 返回的句柄
 * @param[in] p_buffers 顶点缓冲区句柄数组
 * @param[in] p_offsets 各缓冲区起始偏移数组（字节），可为 nullptr（默认全 0）
 * @param[in] p_count   缓冲区数量
 *
 * @pre p_list 有效且有活跃的 DrawList
 * @pre p_buffers 非空且包含至少 p_count 个有效 Buffer RID
 */
void RenderingDevice::DrawListBindVertexBuffers(DrawListID p_list,
                                                const RDBufferID* p_buffers,
                                                const uint64* p_offsets,
                                                uint32 p_count) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  constexpr uint32 kMaxVertexBindings = 16;
  BufferID driver_ids[kMaxVertexBindings] = {};
  uint64 driver_offsets[kMaxVertexBindings] = {};
  uint32 actual_count =
      p_count > kMaxVertexBindings ? kMaxVertexBindings : p_count;
  for (uint32 i = 0; i < actual_count; ++i) {
    Buffer* buf = buffer_owner_.GetOrNull(p_buffers[i].GetRid());
    driver_ids[i] = buf != nullptr ? buf->driver_id : BufferID();
    driver_offsets[i] = p_offsets != nullptr ? p_offsets[i] : 0;
  }
  device_driver_->CommandBindVertexBuffers(driver_ids, driver_offsets,
                                           actual_count);
}

/**
 * @brief 在绘制列表中绑定索引缓冲区
 *
 * @param[in] p_list   DrawListBegin() 返回的句柄
 * @param[in] p_buffer 索引缓冲区句柄
 * @param[in] p_format 索引数据格式（uint16 / uint32）
 * @param[in] p_offset 缓冲区起始偏移（字节）
 *
 * @pre p_list 有效且有活跃的 DrawList
 * @pre p_buffer 为有效的 Buffer RID
 *
 * @par OpenGL 等效
 *   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ...); 配合 glDrawElements
 */
void RenderingDevice::DrawListBindIndexBuffer(DrawListID p_list,
                                              RDBufferID p_buffer,
                                              IndexBufferFormat p_format,
                                              uint64 p_offset) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  Buffer* buf = buffer_owner_.GetOrNull(p_buffer.GetRid());
  if (buf == nullptr) {
    ARHUD_LOG_ERROR("kInvalidParameter",
                    "DrawListBindIndexBuffer: invalid buffer RID");
    return;
  }
  device_driver_->CommandBindIndexBuffer(buf->driver_id, p_format, p_offset);
}

/**
 * @brief 执行非索引绘制
 *
 * @param[in] p_list          DrawListBegin() 返回的句柄
 * @param[in] p_vertex_count  顶点数量
 * @param[in] p_instance_count 实例数量（≥1）
 *
 * @pre p_list 有效且有活跃的 DrawList
 * @pre 已绑定渲染管线和顶点缓冲区
 *
 * @par OpenGL 等效
 *   glDrawArraysInstanced(GL_TRIANGLES, 0, vertex_count, instance_count)
 */
void RenderingDevice::DrawListDraw(DrawListID p_list, uint32 p_vertex_count,
                                   uint32 p_instance_count) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  device_driver_->CommandDraw(p_vertex_count, p_instance_count, 0, 0);
  ARHUD_LOG_INFO("DrawListDraw: vertices=%u instances=%u (frame=%llu)",
                 p_vertex_count, p_instance_count,
                 static_cast<unsigned long long>(frame_number_));
}

/**
 * @brief 执行索引绘制
 *
 * @param[in] p_list          DrawListBegin() 返回的句柄
 * @param[in] p_index_count   索引数量
 * @param[in] p_instance_count 实例数量（≥1）
 *
 * @pre p_list 有效且有活跃的 DrawList
 * @pre 已绑定渲染管线、顶点缓冲区和索引缓冲区
 *
 * @par OpenGL 等效
 *   glDrawElementsInstanced(GL_TRIANGLES, index_count, type, 0, instance_count)
 */
void RenderingDevice::DrawListDrawIndexed(DrawListID p_list,
                                          uint32 p_index_count,
                                          uint32 p_instance_count) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  device_driver_->CommandDrawIndexed(p_index_count, p_instance_count, 0, 0, 0);
  ARHUD_LOG_INFO("DrawListDrawIndexed: indices=%u instances=%u (frame=%llu)",
                 p_index_count, p_instance_count,
                 static_cast<unsigned long long>(frame_number_));
}

/**
 * @brief 执行非索引实例化绘制
 *
 * 与 DrawListDraw() 不同，此方法支持指定 base_instance，
 * 用于实例化着色器中通过 gl_InstanceID + base_instance 获取实例数据。
 *
 * @param[in] p_list          DrawListBegin() 返回的句柄
 * @param[in] p_instance_count 实例数量
 * @param[in] p_base_instance  起始实例索引
 *
 * @pre p_list 有效且有活跃的 DrawList
 * @pre 已绑定渲染管线和顶点缓冲区
 *
 * @par OpenGL 等效
 *   glDrawArraysInstancedBaseInstance(...)
 */
void RenderingDevice::DrawListDrawInstanced(DrawListID p_list,
                                            uint32 p_instance_count,
                                            uint32 p_base_instance) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  device_driver_->CommandDrawInstanced(p_instance_count, p_base_instance);
  ARHUD_LOG_INFO(
      "DrawListDrawInstanced: instances=%u base_instance=%u (frame=%llu)",
      p_instance_count, p_base_instance,
      static_cast<unsigned long long>(frame_number_));
}

/**
 * @brief 执行索引实例化绘制
 *
 * 与 DrawListDrawIndexed() 不同，此方法支持指定 base_instance，
 * 用于实例化着色器中通过 gl_InstanceID + base_instance 获取实例数据。
 *
 * @param[in] p_list          DrawListBegin() 返回的句柄
 * @param[in] p_instance_count 实例数量
 * @param[in] p_base_instance  起始实例索引
 *
 * @pre p_list 有效且有活跃的 DrawList
 * @pre 已绑定渲染管线、顶点缓冲区和索引缓冲区
 *
 * @par OpenGL 等效
 *   glDrawElementsInstancedBaseInstance(...)
 */
void RenderingDevice::DrawListDrawIndexedInstanced(DrawListID p_list,
                                                   uint32 p_instance_count,
                                                   uint32 p_base_instance) {
  (void)p_list;
  if (!draw_list_.active) {
    return;
  }
  device_driver_->CommandDrawIndexedInstanced(p_instance_count,
                                              p_base_instance);
  ARHUD_LOG_INFO(
      "DrawListDrawIndexedInstanced: instances=%u base_instance=%u "
      "(frame=%llu)",
      p_instance_count, p_base_instance,
      static_cast<unsigned long long>(frame_number_));
}

/**
 * @brief 结束当前绘制列表
 *
 * 若 Render Pass 仍处于活跃状态则自动结束。
 * 重置 DrawList 活跃标志，之后可重新调用 DrawListBegin()。
 *
 * @pre 当前有活跃的 DrawList（否则仅输出警告）
 */
void RenderingDevice::DrawListEnd() {
  if (!draw_list_.active) {
    ARHUD_LOG_WARN("RenderingDevice::DrawListEnd() no active draw list");
    return;
  }

  if (draw_list_.render_pass_active) {
    device_driver_->CommandEndRenderPass();
  }

  bool pass_was_active = draw_list_.render_pass_active;
  draw_list_.active = false;
  draw_list_.render_pass_active = false;

  ARHUD_LOG_INFO("DrawListEnd: session closed (pass_was_active=%d, frame=%llu)",
                 pass_was_active ? 1 : 0,
                 static_cast<unsigned long long>(frame_number_));
}

/**
 * @brief 仅清除 Framebuffer（不启动绘制列表）
 *
 * 临时开启并立即结束一个 Render Pass，仅用于清除指定 Framebuffer 的附件。
 * 调用时不得有活跃的 DrawList。
 *
 * @param[in] p_framebuffer       目标 Framebuffer 句柄
 * @param[in] p_clear_colors      是否清除颜色附件
 * @param[in] p_clear_depth       是否清除深度附件
 * @param[in] p_clear_stencil     是否清除模板附件
 * @param[in] p_clear_color       RGBA 清除值数组（4 个 float），可为 nullptr
 * @param[in] p_clear_depth_val   深度清除值
 * @param[in] p_clear_stencil_val 模板清除值
 *
 * @pre 设备已通过 Initialize() 初始化
 * @pre 当前没有活跃的 DrawList
 * @pre p_framebuffer 有效且关联了合法的 RenderPass
 */
void RenderingDevice::DrawListClear(RDFramebufferID p_framebuffer,
                                    bool p_clear_colors, bool p_clear_depth,
                                    bool p_clear_stencil,
                                    const float p_clear_color[4],
                                    float p_clear_depth_val,
                                    uint32 p_clear_stencil_val) {
  if (!initialized_) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::DrawListClear() not initialized");
    return;
  }

  if (draw_list_.active) {
    ARHUD_LOG_ERROR(
        "kFailed", "RenderingDevice::DrawListClear() draw list already active");
    return;
  }

  if (p_framebuffer.IsNull()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::DrawListClear() null framebuffer");
    return;
  }

  const Framebuffer* fb = framebuffer_owner_.GetOrNull(p_framebuffer.GetRid());
  if (fb == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::DrawListClear() invalid framebuffer RID");
    return;
  }

  RenderPassID rp_driver_id =
      RenderPassGetDriverId(RDRenderPassID(fb->render_pass));
  if (!rp_driver_id.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderingDevice::DrawListClear() invalid render pass for framebuffer");
    return;
  }

  const RenderPass* rp = render_pass_owner_.GetOrNull(fb->render_pass);
  uint32 attachment_count = (rp != nullptr) ? rp->attachments.Size() : 0;

  LocalVector<RenderPassClearValue> clear_values;
  clear_values.Resize(attachment_count);

  uint32 clear_mask = 0;

  for (uint32 i = 0; i < attachment_count; ++i) {
    const Attachment& att = rp->attachments[i];
    bool is_depth = (att.format == DataFormat::kD16Unorm ||
                     att.format == DataFormat::kD32Sfloat ||
                     att.format == DataFormat::kS8Uint ||
                     att.format == DataFormat::kD16UnormS8Uint ||
                     att.format == DataFormat::kD24UnormS8Uint ||
                     att.format == DataFormat::kD32SfloatS8Uint);

    if (!is_depth) {
      if (p_clear_colors && att.load_op == AttachmentLoadOp::kClear) {
        clear_values[i].color[0] =
            p_clear_color != nullptr ? p_clear_color[0] : 0.0f;
        clear_values[i].color[1] =
            p_clear_color != nullptr ? p_clear_color[1] : 0.0f;
        clear_values[i].color[2] =
            p_clear_color != nullptr ? p_clear_color[2] : 0.0f;
        clear_values[i].color[3] =
            p_clear_color != nullptr ? p_clear_color[3] : 1.0f;
        clear_mask |= (1u << i);
      }
    } else {
      if (p_clear_depth && att.load_op == AttachmentLoadOp::kClear) {
        clear_values[i].depth = p_clear_depth_val;
        clear_mask |= (1u << i);
      }
      if (p_clear_stencil && att.stencil_load_op == AttachmentLoadOp::kClear) {
        clear_values[i].stencil = p_clear_stencil_val;
        clear_mask |= (1u << i);
      }
    }
  }

  uint32 clear_vp_w = fb->width;
  uint32 clear_vp_h = fb->height;

  device_driver_->CommandBeginRenderPass(
      rp_driver_id, fb->driver_id,
      VectorView<RenderPassClearValue>(clear_values.Data(),
                                       clear_values.Size()),
      clear_mask, 0, 0, clear_vp_w, clear_vp_h);

  device_driver_->CommandEndRenderPass();

  ARHUD_LOG_INFO(
      "DrawListClear: framebuffer %llu cleared, clear_mask=0x%x, viewport "
      "%ux%u (frame=%llu)",
      static_cast<unsigned long long>(fb->driver_id.GetId()), clear_mask,
      clear_vp_w, clear_vp_h, static_cast<unsigned long long>(frame_number_));
}

// ═══════════════════════════════════════════════════════════════════════
// ComputeList
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 开始一个计算列表
 *
 * 初始化计算列表状态。计算列表与绘制列表互斥，
 * 同一时刻不能同时存在活跃的 DrawList 和 ComputeList。
 *
 * @return ComputeListID 计算列表句柄，失败时返回 kInvalidComputeListId
 *
 * @pre 设备已通过 Initialize() 初始化
 * @pre 当前没有活跃的 DrawList
 * @pre 当前没有活跃的 ComputeList
 */
ComputeListID RenderingDevice::ComputeListBegin() {
  if (!initialized_) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::ComputeListBegin() not initialized");
    return kInvalidComputeListId;
  }

  if (draw_list_.active) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderingDevice::ComputeListBegin() draw list is active");
    return kInvalidComputeListId;
  }

  if (compute_list_.active) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderingDevice::ComputeListBegin() compute list already active");
    return kInvalidComputeListId;
  }

  compute_list_.active = true;

  ARHUD_LOG_INFO("ComputeListBegin: session opened (frame=%llu)",
                 static_cast<unsigned long long>(frame_number_));

  return 0;
}

/**
 * @brief 在计算列表中绑定计算管线
 *
 * @param[in] p_list     ComputeListBegin() 返回的句柄
 * @param[in] p_pipeline 计算管线句柄（必须为计算管线）
 *
 * @pre p_list 有效且有活跃的 ComputeList
 * @pre p_pipeline 为有效的计算管线 RID（is_compute == true）
 */
void RenderingDevice::ComputeListBindPipeline(ComputeListID p_list,
                                              RDPipelineID p_pipeline) {
  (void)p_list;
  if (!compute_list_.active) {
    return;
  }
  Pipeline* pipeline = pipeline_owner_.GetOrNull(p_pipeline.GetRid());
  if (pipeline == nullptr) {
    ARHUD_LOG_ERROR("kInvalidParameter",
                    "ComputeListBindPipeline: invalid pipeline RID");
    return;
  }
  if (!pipeline->is_compute) {
    ARHUD_LOG_ERROR(
        "kInvalidParameter",
        "ComputeListBindPipeline: pipeline is not a compute pipeline");
    return;
  }
  device_driver_->CommandBindRenderPipeline(pipeline->driver_id);
}

/**
 * @brief 在计算列表中绑定 Uniform Set
 *
 * @param[in] p_list        ComputeListBegin() 返回的句柄
 * @param[in] p_uniform_set Uniform Set 句柄
 * @param[in] p_set_index   绑定点索引（对应 Shader 中的 set 编号）
 *
 * @pre p_list 有效且有活跃的 ComputeList
 * @pre p_uniform_set 为有效的 UniformSet RID
 */
void RenderingDevice::ComputeListBindUniformSet(ComputeListID p_list,
                                                RDUniformSetID p_uniform_set,
                                                uint32 p_set_index) {
  (void)p_list;
  if (!compute_list_.active) {
    return;
  }
  UniformSet* uniform_set =
      uniform_set_owner_.GetOrNull(p_uniform_set.GetRid());
  if (uniform_set == nullptr) {
    ARHUD_LOG_ERROR("kInvalidParameter",
                    "ComputeListBindUniformSet: invalid uniform set RID");
    return;
  }
  device_driver_->CommandBindUniformSet(uniform_set->driver_id, p_set_index);
}

/**
 * @brief 派发计算着色器
 *
 * @param[in] p_list     ComputeListBegin() 返回的句柄
 * @param[in] p_x_groups X 方向工作组数量
 * @param[in] p_y_groups Y 方向工作组数量
 * @param[in] p_z_groups Z 方向工作组数量
 *
 * @pre p_list 有效且有活跃的 ComputeList
 * @pre 已绑定计算管线和所需 Uniform Set
 *
 * @par OpenGL 等效
 *   glDispatchCompute(x_groups, y_groups, z_groups)
 */
void RenderingDevice::ComputeListDispatch(ComputeListID p_list,
                                          uint32 p_x_groups, uint32 p_y_groups,
                                          uint32 p_z_groups) {
  (void)p_list;
  if (!compute_list_.active) {
    return;
  }
  device_driver_->CommandDispatch(p_x_groups, p_y_groups, p_z_groups);
  ARHUD_LOG_INFO("ComputeListDispatch: groups=%ux%ux%u (frame=%llu)",
                 p_x_groups, p_y_groups, p_z_groups,
                 static_cast<unsigned long long>(frame_number_));
}

/**
 * @brief 结束当前计算列表
 *
 * 重置 ComputeList 活跃标志，之后可重新调用 ComputeListBegin()
 * 或切换到 DrawList。
 *
 * @pre 当前有活跃的 ComputeList（否则仅输出警告）
 */
void RenderingDevice::ComputeListEnd() {
  if (!compute_list_.active) {
    ARHUD_LOG_WARN("RenderingDevice::ComputeListEnd() no active compute list");
    return;
  }

  compute_list_.active = false;

  ARHUD_LOG_INFO("ComputeListEnd: session closed (frame=%llu)",
                 static_cast<unsigned long long>(frame_number_));
}

}  // namespace arhud
