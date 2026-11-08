/**
 * @file render_viewport.cpp
 * @brief 单屏渲染管线实现（B3-M0.3：FBO 渲染 + GDI 上屏，无 SwapBuffers）
 *
 * B3-M0.3 渲染路径：GL 上下文走 offscreen surface（共享），渲染始终输出
 * 到 scene_buffers_.framebuffer（FBO）。InitializePostProcess 现在 ALWAYS
 * 创建 scene_buffers_（FBO 是回读必需品，即使无后处理效果）。渲染线程
 * 帧完成后 FramebufferReadback → CPU 缓冲 → IFramePresenter::Present。
 *
 * Initialize 只存 EngineStackRefs 指针 + 做 per-screen init（offscreen refs /
 * cpu_buffer / scene_buffers / presenter / post-process）。
 * Finalize 只停线程 + 后处理 + 释放 cpu_buffer，不碰共享栈（栈销毁归
 * ARHudEngine::Finalize）。全部方法通过指针访问共享栈成员。
 */

#include "render_viewport.h"

#include <cstring>

#include "draw_list_scope.h"
#include "io/logger.h"
#include "os/memory.h"
#include "renderer/shaders/hud_composite.h"
#include "rendering_context_driver.h"
#include "storage/shader_storage.h"

namespace arhud {

RenderViewport::~RenderViewport() {
  if (initialized_) {
    Finalize();
  }
}

Error RenderViewport::Initialize(IRenderingContextDriver* p_context_driver,
                                 const Config& p_config,
                                 const EngineStackRefs& p_stack_refs) {
  if (initialized_) {
    return Error::kAlreadyExists;
  }

  if (p_context_driver == nullptr) {
    return Error::kInvalidParameter;
  }

  rd_ = p_stack_refs.rd;
  command_queue_ = p_stack_refs.command_queue;
  item_storage_ = p_stack_refs.item_storage;
  rendering_server_ = p_stack_refs.rendering_server;
  compositor_ = p_stack_refs.compositor;

  width_ = p_config.width;
  height_ = p_config.height;
  context_driver_ = p_context_driver;

  // B3-M0.3: offscreen surface/context（共享，从 EngineStackRefs 注入）
  offscreen_surface_id_ = p_stack_refs.offscreen_surface_id;
  offscreen_context_id_ = p_stack_refs.offscreen_context_id;

  // B3-M0.3: per-screen GDI 上屏器（Engine 创建，RenderViewport 只持有指针）
  presenter_ = p_stack_refs.presenter;

  // B3-M0.3: 分配 CPU 回读缓冲（width * height * 4 bytes RGBA8）
  if (width_ > 0 && height_ > 0) {
    cpu_buffer_size_ = width_ * height_ * 4;
    cpu_buffer_ = static_cast<uint8*>(ARHUD_MALLOC_ZEROED(cpu_buffer_size_));
    if (cpu_buffer_ == nullptr) {
      ARHUD_LOG_ERROR("kOutOfMemory",
                      "RenderViewport::Initialize() cpu_buffer_ alloc failed");
      return Error::kOutOfMemory;
    }
  }

  Error err = InitializePostProcess();
  if (err != Error::kOK) {
    // B3-M0.3: scene_buffers_ 创建失败 = 无 FBO = 无法回读上屏 → 硬失败
    ARHUD_LOG_ERROR("kFailed",
                    "RenderViewport::Initialize() scene_buffers_ creation "
                    "failed, cannot render");
    if (cpu_buffer_ != nullptr) {
      ARHUD_FREE(cpu_buffer_);
      cpu_buffer_ = nullptr;
      cpu_buffer_size_ = 0;
    }
    return Error::kFailed;
  }

  initialized_ = true;
  ARHUD_LOG_INFO("RenderViewport initialized (%ux%u)", width_.load(),
                 height_.load());
  return Error::kOK;
}

void RenderViewport::Finalize() {
  if (!initialized_) {
    return;
  }

  FinalizePostProcess();

  // B3-M0.3: 释放 CPU 回读缓冲（presenter 由 ARHudEngine 拥有，此处不
  // finalize）
  if (cpu_buffer_ != nullptr) {
    ARHUD_FREE(cpu_buffer_);
    cpu_buffer_ = nullptr;
    cpu_buffer_size_ = 0;
  }
  presenter_ = nullptr;

  context_driver_ = nullptr;
  initialized_ = false;
  ARHUD_LOG_INFO("RenderViewport finalized");
}

bool RenderViewport::IsInitialized() const {
  return initialized_;
}

void RenderViewport::Render(const CameraMatrix& p_camera_3d,
                            const Transform2D& p_camera_2d,
                            const DirectionalLight* p_light,
                            const SpatialItem* p_spatial_items,
                            uint32 p_spatial_count,
                            const HUDCanvasLayer* p_canvas_layers,
                            uint32 p_layer_count) {
  if (!initialized_) {
    return;
  }

  SpatialRenderer* spatial_renderer = compositor_->GetSpatialRenderer();
  CanvasRenderer* canvas_renderer = compositor_->GetCanvasRenderer();

  Mat4 projection_2d =
      Orthographic(0.0f, static_cast<float32>(width_),
                   static_cast<float32>(height_), 0.0f, -1.0f, 1.0f);

  // B3-M0.3: 始终渲染到 scene_buffers_.framebuffer（FBO），不再用 default
  // framebuffer
  RDFramebufferID target_fb = scene_buffers_.framebuffer;

  if (p_spatial_count > 0 && p_spatial_items != nullptr) {
    spatial_renderer->SpatialItemsClear();
    for (uint32 i = 0; i < p_spatial_count; ++i) {
      spatial_renderer->SpatialItemPush(p_spatial_items[i]);
    }

    spatial_renderer->RenderSpatialItems(p_camera_3d, p_light, width_, height_,
                                         true, true, nullptr, target_fb);

    spatial_renderer->SpatialItemsClear();
  }

  if (p_layer_count > 0 && p_canvas_layers != nullptr) {
    LocalVector<uint32> sorted;
    sorted.Resize(p_layer_count);
    for (uint32 i = 0; i < p_layer_count; ++i) {
      sorted[i] = i;
    }

    if (p_layer_count > 1) {
      for (uint32 i = 0; i < p_layer_count - 1; ++i) {
        for (uint32 j = i + 1; j < p_layer_count; ++j) {
          if (p_canvas_layers[sorted[i]].layer >
              p_canvas_layers[sorted[j]].layer) {
            uint32 tmp = sorted[i];
            sorted[i] = sorted[j];
            sorted[j] = tmp;
          }
        }
      }
    }

    bool has_3d_background =
        (p_spatial_count > 0 && p_spatial_items != nullptr);

    for (uint32 i = 0; i < p_layer_count; ++i) {
      const HUDCanvasLayer& layer = p_canvas_layers[sorted[i]];

      bool layer_has_content =
          (!layer.canvas_items.IsEmpty() || !layer.text_items.IsEmpty());
      if (!layer_has_content) {
        continue;
      }

      if (!layer.canvas_items.IsEmpty()) {
        canvas_renderer->CanvasItemsClear();
        for (uint32 j = 0; j < layer.canvas_items.Size(); ++j) {
          canvas_renderer->CanvasItemPush(layer.canvas_items[j]);
        }

        bool should_clear = (!has_3d_background && i == 0);
        canvas_renderer->RenderCanvasItems(projection_2d.elements, width_,
                                           height_, should_clear, nullptr,
                                           target_fb);
        canvas_renderer->CanvasItemsClear();
      }

      {
        canvas_renderer->CanvasItemsClear();
        canvas_renderer->RenderCanvasItems(projection_2d.elements, width_,
                                           height_, true, nullptr, target_fb);
        canvas_renderer->CanvasItemsClear();
      }

      if (!layer.text_items.IsEmpty()) {
        canvas_renderer->TextItemsClear();
        for (uint32 j = 0; j < layer.text_items.Size(); ++j) {
          canvas_renderer->TextItemPush(layer.text_items[j]);
        }

        canvas_renderer->RenderTextItems(projection_2d.elements, width_,
                                         height_, target_fb);

        canvas_renderer->TextItemsClear();
      }
    }
  }

  if (post_process_enabled_) {
    RenderPostProcess(width_, height_);
  }
}

// ─────────────────────────────────────────────────────────────────────
//  保留模式渲染（B2-R：从 ItemStorage 收集场景状态）
// ─────────────────────────────────────────────────────────────────────

void RenderViewport::RenderFromStorage() {
  if (!initialized_) {
    return;
  }

  // B3-M0.4: cull_mask 过滤（visibility_mask & cull_mask_ 非零才渲染）
  // 1. Spatial items：cull_mask 过滤 + 全量收集
  //    ADR-0005: 世界矩阵沿父链连乘（无父时等价于自身矩阵）
  LocalVector<SpatialItem> spatial_items;
  for (uint32 i = 0; i < item_storage_->GetSpatialItemCount(); ++i) {
    const SpatialItemData& data = item_storage_->GetSpatialItemByIndex(i);
    if ((data.visibility_mask & cull_mask_) == 0) {
      continue;
    }

    SpatialItem item;
    item.material = data.material;
    item.mesh = data.mesh;
    if (!item_storage_->SpatialItemGetEffectiveTransform(
            item_storage_->GetSpatialItemRid(i), item.transform)) {
      continue;  // RID 无效（防御，理论不可达）
    }
    item.render_priority = data.render_priority;
    item.disable_cull = data.disable_cull;
    spatial_items.PushBack(item);
  }

  // 2. Canvas/Text items：树内可见性 + cull_mask 过滤 + 按 layer 分组
  //    ADR-0005: transform/opacity/z 经父链有效状态计算（无父时等价于自身状态，
  //    既有行为不变）；layer / visibility_mask 不继承（per-item）
  LocalVector<HUDCanvasLayer> layers;

  auto find_or_add_layer = [&](int32 p_layer) -> int32 {
    for (uint32 i = 0; i < layers.Size(); ++i) {
      if (layers[i].layer == p_layer) {
        return static_cast<int32>(i);
      }
    }
    HUDCanvasLayer layer;
    layer.layer = p_layer;
    layers.PushBack(layer);
    return static_cast<int32>(layers.Size() - 1);
  };

  for (uint32 i = 0; i < item_storage_->GetCanvasItemCount(); ++i) {
    const CanvasItemData& data = item_storage_->GetCanvasItemByIndex(i);
    if ((data.visibility_mask & cull_mask_) == 0) {
      continue;
    }

    ItemStorage::CanvasItemEffectiveState eff;
    if (!item_storage_->CanvasItemGetEffective(
            item_storage_->GetCanvasItemRid(i), eff)) {
      continue;  // RID 无效（防御，理论不可达）
    }
    if (!eff.visible) {
      continue;
    }  // 树内可见性（父隐藏 → 子孙全隐）

    int32 idx = find_or_add_layer(data.layer);

    CanvasItem item;
    item.material = data.material;
    item.mesh = data.mesh;
    memcpy(item.transform, eff.transform, sizeof(float) * 16);
    item.opacity = eff.opacity;
    item.z_order = eff.z_order;
    layers[idx].canvas_items.PushBack(item);
  }

  for (uint32 i = 0; i < item_storage_->GetTextItemCount(); ++i) {
    const TextItemData& data = item_storage_->GetTextItemByIndex(i);
    if ((data.visibility_mask & cull_mask_) == 0) {
      continue;
    }

    ItemStorage::TextItemEffectiveState eff;
    if (!item_storage_->TextItemGetEffective(item_storage_->GetTextItemRid(i),
                                             eff)) {
      continue;  // RID 无效（防御，理论不可达）
    }
    if (!eff.visible) {
      continue;
    }

    int32 idx = find_or_add_layer(data.layer);

    TextItem item;
    item.font = data.font;
    item.text = data.text.Data();
    item.length = data.text.Size();
    item.x = eff.x;
    item.y = eff.y;
    memcpy(item.color, data.color, sizeof(float) * 4);
    item.color[3] = eff.alpha;  // ADR-0005: alpha 沿父链 opacity 连乘
    item.z_order = eff.z_order;
    layers[idx].text_items.PushBack(item);
  }

  // 3. 复用立即模式渲染路径（ADR-0007: 相机/灯光为本视口自持状态）
  Render(GetCamera3D(), GetCamera2D(), GetDirectionalLight(),
         spatial_items.Data(), spatial_items.Size(), layers.Data(),
         layers.Size());
}

RenderingServer* RenderViewport::GetRenderServer() {
  return rendering_server_;
}

uint32 RenderViewport::GetWidth() const {
  return width_;
}

uint32 RenderViewport::GetHeight() const {
  return height_;
}

void RenderViewport::HandleResize(uint32 p_width, uint32 p_height) {
  if (!initialized_) {
    return;
  }

  if (p_width == 0 || p_height == 0) {
    return;
  }

  // B3-M0.3: 可见窗口无 GL surface，resize 改为 deferred（渲染线程内执行）
  new_width_.store(p_width, std::memory_order_relaxed);
  new_height_.store(p_height, std::memory_order_relaxed);
  resize_pending_.store(true, std::memory_order_release);

  // 唤醒渲染线程消费（静态场景无 SubmitScene 时也能及时重建）
  if (queue_ != nullptr) {
    queue_->WakeRender();
  }

  ARHUD_LOG_INFO("RenderViewport resize pending: %ux%u", p_width, p_height);
}

bool RenderViewport::ApplyPendingResize() {
  if (!initialized_ || !resize_pending_.load(std::memory_order_acquire)) {
    return false;
  }

  if (queue_ == nullptr) {
    return false;
  }

  // 必须无在途帧：present 线程可能正持有 queue 槽位，Resize 会释放其缓冲。
  // 保留 pending，待 present Release 唤醒本线程后下轮重试。
  if (queue_->HasInFlight()) {
    return false;
  }

  uint32 new_w = new_width_.load(std::memory_order_relaxed);
  uint32 new_h = new_height_.load(std::memory_order_relaxed);

  if (new_w == width_.load(std::memory_order_relaxed) &&
      new_h == height_.load(std::memory_order_relaxed)) {
    resize_pending_.store(false, std::memory_order_release);
    return false;
  }

  // 1. 重建 FBO（颜色/深度/RenderPass/Framebuffer）
  Error err = scene_buffers_.Recreate(rd_, new_w, new_h);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderViewport::ApplyPendingResize() Recreate failed (%ux%u)", new_w,
        new_h);
    resize_pending_.store(false, std::memory_order_release);
    return false;
  }

  // 1b. 后处理输出 FBO 必须跟随屏幕尺寸；失败降级为无后处理。
  if (post_buffers_.IsValid() &&
      post_buffers_.Recreate(rd_, new_w, new_h) != Error::kOK) {
    post_process_enabled_ = false;
    ARHUD_LOG_WARN(
        "RenderViewport post-process disabled after resize (post_buffers_ "
        "recreate failed)");
  }

  // 2. 重建帧图像队列（尺寸随屏；无在途帧，present 线程阻塞于
  // AcquireReady，安全）
  queue_->Resize(new_w, new_h);

  width_.store(new_w, std::memory_order_relaxed);
  height_.store(new_h, std::memory_order_relaxed);

  // 3. 后处理 uniform set 在初始化时绑定了旧 color_texture，须按新纹理重建
  if (post_process_enabled_) {
    if (CreateCompositeUniformSet() != Error::kOK) {
      post_process_enabled_ = false;
      ARHUD_LOG_WARN(
          "RenderViewport post-process disabled after resize (uniform set "
          "rebind failed)");
    }
  }

  resize_pending_.store(false, std::memory_order_release);
  ARHUD_LOG_INFO("RenderViewport resized to %ux%u", new_w, new_h);
  return true;
}

/**
 * @brief B3: 渲染线程单屏渲染入口（原 RecordRender + ReadbackAndMarkReady
 * 合并）
 *
 * @par 调用方（RenderThreadFunc，本屏归属渲染线程）
 *   1. dirty 检查（ItemStorage 全局 dirty；false = 静态内容零成本跳过）
 *   2. HasInFlight 防渲染超前（§3.8：每屏最多 1 帧在途 = 精确 1 帧延迟）
 *   3. TryAcquireFree（FREE→WRITING；无 free 槽跳过）
 *   4. RenderFromStorage（本线程读 ItemStorage —— B3 起读写同线程）
 *   5. FramebufferReadback（FBO → CPU 槽）
 *   6. MarkReady（READY；唤醒 present 线程）
 */
bool RenderViewport::RenderAndSubmit() {
  if (!initialized_ || queue_ == nullptr || item_storage_ == nullptr) {
    return false;
  }

  // 1. dirty（§3.7 内容更新率：静态 HUD 跳过）
  //    B3-M0.5: per-screen 路由——只查本屏位（viewport_index_）；变更
  //    未触及本屏时本屏跳过，其他屏照常渲染。只读不消费——本屏脏位
  //    由 RenderThreadFunc 在本函数返回 true（真正渲完）后经
  //    ClearScreenDirty 清除；返回 false 时脏位保留，下一轮 submit
  //    重试——防止背压跳过被全局清零吞掉（首帧丢失竞态的根因）。
  if (!item_storage_->IsScreenDirty(viewport_index_)) {
    return false;
  }

  // 2. Backpressure: skip if frame in flight (anti render-ahead, §3.8)
  if (queue_->HasInFlight()) {
    return false;
  }

  // 3. Acquire slot (FREE→WRITING)
  uint32 slot = queue_->TryAcquireFree();
  if (slot == UINT32_MAX) {
    return false;
  }

  // 4. Render scene to FBO (same-thread read of ItemStorage)
  RenderFromStorage();

  // 5. Readback FBO → CPU slot (post-process enabled: composite output;
  // otherwise read the scene FBO directly).
  const RDFramebufferID readback_fb =
      (post_process_enabled_ && post_buffers_.IsValid())
          ? post_buffers_.framebuffer
          : scene_buffers_.framebuffer;
  rd_->FramebufferReadback(readback_fb, 0, 0, width_, height_,
                           queue_->GetPixels(slot));

  // 6. Mark READY (wakes present thread)
  queue_->MarkReady(slot);
  return true;
}

Error RenderViewport::InitializePostProcess() {
  if (width_ == 0 || height_ == 0) {
    ARHUD_LOG_WARN("RenderViewport::InitializePostProcess() invalid size %ux%u",
                   width_.load(), height_.load());
    return Error::kFailed;
  }

  // B3-M0.3: 始终创建 scene_buffers_（FBO 是回读必需品，即使无后处理效果）
  Error err = scene_buffers_.Create(rd_, width_, height_);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderViewport::InitializePostProcess() RenderBuffers::Create failed");
    return Error::kFailed;
  }

  // Post-process output FBO (composite target). Failure only downgrades to
  // "no post-process": readback falls back to the scene FBO.
  if (post_buffers_.Create(rd_, width_, height_) != Error::kOK) {
    ARHUD_LOG_WARN(
        "RenderViewport::InitializePostProcess() post_buffers_ create failed; "
        "post-process disabled");
    post_process_enabled_ = false;
    return Error::kOK;
  }

  // ── 以下为 composite pipeline（可选，失败不影响 scene_buffers_）──
  ShaderStorage* shader_storage = compositor_->GetShaderStorage();

  LocalVector<ShaderStageSource> stages;
  stages.Resize(2);
  stages[0].stage = ShaderStage::kVertex;
  stages[0].source = kHudCompositeVert;
  stages[1].stage = ShaderStage::kFragment;
  stages[1].source = kHudCompositeFrag;

  LocalVector<ShaderUniform> uniforms;
  uniforms.Resize(2);

  uniforms[0].type = UniformType::kSamplerWithTexture;
  uniforms[0].writable = false;
  uniforms[0].binding = 0;
  uniforms[0].stages = BitField<ShaderStage>(ShaderStage::kFragment);
  uniforms[0].length = 0;
  uniforms[0].size = 0;
  uniforms[0].name = "u_scene_color";

  uniforms[1].type = UniformType::kUniformBuffer;
  uniforms[1].writable = false;
  uniforms[1].binding = 1;
  uniforms[1].stages = BitField<ShaderStage>(ShaderStage::kFragment);
  uniforms[1].length = 0;
  uniforms[1].size = 20;
  uniforms[1].name = "CompositeParams";

  composite_shader_rid_ = shader_storage->ShaderCreateFromSource(
      "hud_composite", stages, uniforms, 0);

  if (!composite_shader_rid_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "RenderViewport::InitializePostProcess() "
                    "ShaderCreateFromSource failed");
    // B3-M0.3: scene_buffers_ 保留（FBO 仍用于回读），仅 composite 不可用
    post_process_enabled_ = false;
    ARHUD_LOG_WARN(
        "RenderViewport post-process disabled (composite shader creation "
        "failed), scene FBO still active (%ux%u)",
        width_.load(), height_.load());
    return Error::kOK;
  }

  composite_shader_rd_id_ =
      shader_storage->ShaderGetRDId(composite_shader_rid_);
  if (!composite_shader_rd_id_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderViewport::InitializePostProcess() shader compilation failed");
    shader_storage->ShaderFree(composite_shader_rid_);
    composite_shader_rid_ = RID();
    post_process_enabled_ = false;
    ARHUD_LOG_WARN(
        "RenderViewport post-process disabled (composite shader compilation "
        "failed), scene FBO still active (%ux%u)",
        width_.load(), height_.load());
    return Error::kOK;
  }

  LocalVector<VertexAttribute> attribs;
  attribs.Resize(2);
  attribs[0].binding = 0;
  attribs[0].location = 0;
  attribs[0].offset = 0;
  attribs[0].format = DataFormat::kR32G32Sfloat;
  attribs[0].stride = 16;
  attribs[0].frequency = VertexFrequency::kVertex;

  attribs[1].binding = 0;
  attribs[1].location = 1;
  attribs[1].offset = 8;
  attribs[1].format = DataFormat::kR32G32Sfloat;
  attribs[1].stride = 16;
  attribs[1].frequency = VertexFrequency::kVertex;

  composite_vertex_format_id_ = rd_->VertexFormatCreate(
      VectorView<VertexAttribute>(attribs.Data(), attribs.Size()));
  if (!composite_vertex_format_id_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderViewport::InitializePostProcess() VertexFormatCreate failed");
    goto fail_pipeline;
  }

  {
    PipelineRasterizationState raster;
    raster.cull_mode = PolygonCullMode::kDisabled;
    raster.front_face = PolygonFrontFace::kCounterClockwise;

    PipelineMultisampleState multisample;
    multisample.sample_count = TextureSamples::k1;

    PipelineDepthStencilState depth_stencil;
    depth_stencil.enable_depth_test = false;
    depth_stencil.enable_depth_write = false;

    PipelineColorBlendState blend = PipelineColorBlendState::CreateBlend(1);

    composite_pipeline_id_ = rd_->RenderPipelineCreate(
        composite_shader_rd_id_, composite_vertex_format_id_,
        RenderPrimitive::kTriangles, raster, multisample, depth_stencil, blend,
        BitField<PipelineDynamicStateFlags>(), post_buffers_.render_pass);
  }

  if (!composite_pipeline_id_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "RenderViewport::InitializePostProcess() RenderPipelineCreate failed");
    goto fail_pipeline;
  }

  {
    float vertices[] = {

        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,  -1.0f, 1.0f, 0.0f,
        1.0f,  1.0f,  1.0f, 1.0f, -1.0f, 1.0f,  0.0f, 1.0f,
    };
    uint32 indices[] = {0, 1, 2, 0, 2, 3};

    quad_vbo_ = rd_->BufferCreate(
        sizeof(vertices), BitField<BufferUsageBits>(BufferUsageBits::kVertex),
        MemoryAllocationType::kCpu);
    if (!quad_vbo_.IsValid()) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "RenderViewport::InitializePostProcess() quad VBO create failed");
      goto fail_pipeline;
    }
    rd_->BufferUpdate(quad_vbo_, 0, sizeof(vertices), vertices);

    quad_ibo_ = rd_->BufferCreate(
        sizeof(indices), BitField<BufferUsageBits>(BufferUsageBits::kIndex),
        MemoryAllocationType::kCpu);
    if (!quad_ibo_.IsValid()) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "RenderViewport::InitializePostProcess() quad IBO create failed");
      goto fail_pipeline;
    }
    rd_->BufferUpdate(quad_ibo_, 0, sizeof(indices), indices);
  }

  {
    static constexpr uint32 kParamsUBOSize = 256;
    composite_params_ubo_ = rd_->BufferCreate(
        kParamsUBOSize, BitField<BufferUsageBits>(BufferUsageBits::kUniform),
        MemoryAllocationType::kCpu);
    if (!composite_params_ubo_.IsValid()) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "RenderViewport::InitializePostProcess() params UBO create failed");
      goto fail_pipeline;
    }

    float params[5] = {
        post_process_params_.gamma,
        post_process_params_.enable_distortion ? 1.0f : 0.0f,
        post_process_params_.distortion_k1,
        post_process_params_.distortion_k2,
        post_process_params_.enable_premul_alpha ? 1.0f : 0.0f,
    };
    rd_->BufferUpdate(composite_params_ubo_, 0, sizeof(params), params);

    if (CreateCompositeUniformSet() != Error::kOK) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "RenderViewport::InitializePostProcess() UniformSetCreate failed");
      goto fail_pipeline;
    }
  }

  post_process_enabled_ = true;
  ARHUD_LOG_INFO("RenderViewport post-process enabled (%ux%u)", width_.load(),
                 height_.load());
  return Error::kOK;

fail_pipeline:
  // B3-M0.3: composite pipeline 失败不销毁 scene_buffers_（FBO 仍用于回读）
  // 仅 post-process 效果不可用，渲染管线照常工作
  if (composite_pipeline_id_.IsValid()) {
    rd_->PipelineFree(composite_pipeline_id_);
    composite_pipeline_id_ = RDPipelineID();
  }
  if (composite_vertex_format_id_.IsValid()) {
    rd_->VertexFormatFree(composite_vertex_format_id_);
    composite_vertex_format_id_ = RDVertexFormatID();
  }
  if (composite_shader_rid_.IsValid() && shader_storage) {
    shader_storage->ShaderFree(composite_shader_rid_);
    composite_shader_rid_ = RID();
    composite_shader_rd_id_ = RDShaderID();
  }
  if (quad_vbo_.IsValid()) {
    rd_->BufferFree(quad_vbo_);
    quad_vbo_ = RDBufferID();
  }
  if (quad_ibo_.IsValid()) {
    rd_->BufferFree(quad_ibo_);
    quad_ibo_ = RDBufferID();
  }
  if (composite_params_ubo_.IsValid()) {
    rd_->BufferFree(composite_params_ubo_);
    composite_params_ubo_ = RDBufferID();
  }
  if (composite_set_.IsValid()) {
    rd_->UniformSetFree(composite_set_);
    composite_set_ = RDUniformSetID();
  }
  post_buffers_.Destroy(rd_);
  post_process_enabled_ = false;
  ARHUD_LOG_WARN(
      "RenderViewport post-process disabled (composite pipeline failed), scene "
      "FBO still active (%ux%u)",
      width_.load(), height_.load());
  return Error::kOK;
}

Error RenderViewport::CreateCompositeUniformSet() {
  // 重建前先释放旧 set（resize 重绑场景下旧 set 指向已销毁的纹理）
  if (composite_set_.IsValid()) {
    rd_->UniformSetFree(composite_set_);
    composite_set_ = RDUniformSetID();
  }

  TextureID color_tex_driver_id =
      rd_->TextureGetDriverId(scene_buffers_.color_texture);

  LocalVector<BoundUniform> bound_uniforms;
  bound_uniforms.Resize(2);

  bound_uniforms[0].type = UniformType::kSamplerWithTexture;
  bound_uniforms[0].binding = 0;
  bound_uniforms[0].immutable_sampler = false;
  bound_uniforms[0].ids.PushBack(BufferID(color_tex_driver_id.GetId()));

  bound_uniforms[1].type = UniformType::kUniformBuffer;
  bound_uniforms[1].binding = 1;
  bound_uniforms[1].immutable_sampler = false;
  bound_uniforms[1].ids.PushBack(BufferID(composite_params_ubo_.GetId()));

  composite_set_ = rd_->UniformSetCreate(
      VectorView<BoundUniform>(bound_uniforms.Data(), bound_uniforms.Size()),
      composite_shader_rd_id_, 0);

  if (!composite_set_.IsValid()) {
    return Error::kFailed;
  }

  return Error::kOK;
}

void RenderViewport::FinalizePostProcess() {
  // B3-M0.3: composite pipeline 可选，仅 post_process_enabled_ 时存在
  if (post_process_enabled_) {
    ShaderStorage* shader_storage = compositor_->GetShaderStorage();

    if (composite_set_.IsValid()) {
      rd_->UniformSetFree(composite_set_);
      composite_set_ = RDUniformSetID();
    }
    if (composite_params_ubo_.IsValid()) {
      rd_->BufferFree(composite_params_ubo_);
      composite_params_ubo_ = RDBufferID();
    }
    if (quad_ibo_.IsValid()) {
      rd_->BufferFree(quad_ibo_);
      quad_ibo_ = RDBufferID();
    }
    if (quad_vbo_.IsValid()) {
      rd_->BufferFree(quad_vbo_);
      quad_vbo_ = RDBufferID();
    }
    if (composite_pipeline_id_.IsValid()) {
      rd_->PipelineFree(composite_pipeline_id_);
      composite_pipeline_id_ = RDPipelineID();
    }
    if (composite_vertex_format_id_.IsValid()) {
      rd_->VertexFormatFree(composite_vertex_format_id_);
      composite_vertex_format_id_ = RDVertexFormatID();
    }
    if (composite_shader_rid_.IsValid() && shader_storage) {
      shader_storage->ShaderFree(composite_shader_rid_);
      composite_shader_rid_ = RID();
      composite_shader_rd_id_ = RDShaderID();
    }
    post_process_enabled_ = false;
  }

  // B3-M0.3: scene_buffers_ 始终创建（回读必需），此处始终销毁
  scene_buffers_.Destroy(rd_);
  post_buffers_.Destroy(rd_);
}

void RenderViewport::RenderPostProcess(uint32 p_screen_width,
                                       uint32 p_screen_height) {
  float params[5] = {
      post_process_params_.gamma,
      post_process_params_.enable_distortion ? 1.0f : 0.0f,
      post_process_params_.distortion_k1,
      post_process_params_.distortion_k2,
      post_process_params_.enable_premul_alpha ? 1.0f : 0.0f,
  };
  rd_->BufferUpdate(composite_params_ubo_, 0, sizeof(params), params);

  float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  // Composite output goes to the post-process FBO. The default framebuffer is
  // the hidden offscreen window surface (100x100) and nobody reads it back
  // in Mode R — drawing there silently discards the result.
  if (!post_buffers_.IsValid()) {
    return;
  }
  DrawListScope scope(rd_, post_buffers_.framebuffer, true, false, false,
                      clear_color);
  DrawListID dl = scope.Get();

  if (!scope.IsActive()) {
    return;
  }

  rd_->DrawListSetViewport(dl, 0, 0, p_screen_width, p_screen_height);
  rd_->DrawListBindRenderPipeline(dl, composite_pipeline_id_);
  rd_->DrawListBindUniformSet(dl, composite_set_, 0);

  uint64 offset = 0;
  rd_->DrawListBindVertexBuffers(dl, &quad_vbo_, &offset, 1);
  rd_->DrawListBindIndexBuffer(dl, quad_ibo_, IndexBufferFormat::kUint32, 0);

  rd_->DrawListDrawIndexed(dl, 6);
}

RenderViewport::PostProcessParams& RenderViewport::GetPostProcessParams() {
  return post_process_params_;
}

bool RenderViewport::IsPostProcessEnabled() const {
  return post_process_enabled_;
}

}  // namespace arhud
