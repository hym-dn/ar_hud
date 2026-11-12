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

#include <chrono>
#include <cstring>

#include "draw_list_scope.h"
#include "io/logger.h"
#include "os/memory.h"
#include "renderer/shaders/hud_composite.h"
#include "rendering_context_driver.h"
#include "storage/shader_storage.h"

namespace arhud {
namespace {

/** @brief 单调时钟微秒读数（呈现路径统计用） */
uint64 NowUs() {
  return static_cast<uint64>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

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

  // Mode X: per-screen 共享显存帧池（Engine 创建；null = 走回读路径）
  shared_pool_ = p_stack_refs.shared_pool;

  Error err = InitializePostProcess();
  if (err != Error::kOK) {
    // B3-M0.3: scene_buffers_ 创建失败 = 无 FBO = 无法回读上屏 → 硬失败
    ARHUD_LOG_ERROR("kFailed",
                    "RenderViewport::Initialize() scene_buffers_ creation "
                    "failed, cannot render");
    return Error::kFailed;
  }

  // Mode X: 帧池可用 + composite 就绪 → 导入每帧共享目标；失败静默回落
  // 回读形态（CPU 像素归 FrameImageQueue 槽，本类不再自持回读缓冲）
  if (InitializeSharedPath()) {
    ARHUD_LOG_INFO(
        "RenderViewport: shared-VRAM present path active (frames=%u)",
        shared_frame_count_);
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

  // Mode X 先撤：导入解除后，平台帧池才可以安全释放其显存
  // （Engine 的拆卸顺序保证 pool->Finalize 晚于本函数）
  FinalizeSharedPath();

  FinalizePostProcess();

  // presenter / 帧池均由 ARHudEngine 拥有，此处只断引用不 finalize
  presenter_ = nullptr;
  shared_pool_ = nullptr;

  context_driver_ = nullptr;
  initialized_ = false;
  ARHUD_LOG_INFO(
      "RenderViewport finalized (present path: shared=%u frames zero-copy, "
      "readback=%u frames/%llu MB/%.1f ms GPU->CPU)",
      shared_frames_, static_cast<uint32>(readback_frames_),
      static_cast<unsigned long long>(readback_bytes_ / (1024 * 1024)),
      static_cast<float64>(readback_us_) / 1000.0);
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

  // 0. Mode X：先撤销共享目标（解除 interop 别名），平台侧才能换显存
  //    顺序：解除导入 → pool.Resize → 重新导入
  const bool was_shared = shared_path_active_;
  if (was_shared) {
    FinalizeSharedPath();
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

  // 4. Mode X：显存换新尺寸后重新导入（失败即此后走回读——
  //    CPU 槽一直在，降级不需要重建队列）
  if (was_shared) {
    if (shared_pool_ != nullptr) {
      shared_pool_->Resize(new_w, new_h);
    }
    if (!InitializeSharedPath()) {
      ARHUD_LOG_WARN(
          "RenderViewport: shared present disabled after resize — "
          "readback path takes over (%ux%u)",
          new_w, new_h);
    }
  }

  resize_pending_.store(false, std::memory_order_release);
  ARHUD_LOG_INFO("RenderViewport resized to %ux%u", new_w, new_h);
  return true;
}

/**
 * @brief 渲染线程单屏渲染入口（两条呈现路径共用）
 *
 * @par Mode X（共享显存，零回读）
 *   1. dirty 检查（静态内容零成本跳过）
 *   2. HasInFlight 背压（§3.8：每屏最多 1 帧在途）
 *   3. BeginWrite 取本帧共享显存的绘制权——失败即本轮跳过（显示侧
 *      仍在读，脏位保留下轮重试；这是池环与队列背压之外的第三道闸）
 *   4. TryAcquireFree（无空闲槽则交还绘制权后跳过）
 *   5. RenderFromStorage：场景 pass 渲进 scene_buffers_，composite
 *      经 composite_override_ 直写共享显存（distortion/gamma 不丢）
 *   6. EndWrite 交还绘制权 → MarkReady(slot, frame_index)
 *   —— 全帧零 CPU 拷贝；本函数内没有任何回读调用。
 *
 * @par Mode R（回读，回落路径）
 *   步骤同上但跳过 3/6，第 5 步 composite 写 post_buffers_，
 *   随后 FramebufferReadback 把 FBO 像素写进队列 CPU 槽，
 *   MarkReady(slot)（token = kInvalidToken，上屏线程据此走 Present）。
 *
 * @return true 本屏本轮真正完成"渲染 + 提交"（引擎可清本屏脏位）；
 *         false 未脏或背压跳过（脏位保留，下一轮 submit 重试）
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

  // 2. 背压：有在途帧则跳过（防渲染超前，§3.8）
  if (queue_->HasInFlight()) {
    return false;
  }

  // 3. Mode X：取本帧共享显存的绘制权
  //    池在运行期失效（设备丢失等）时这里不进入共享分支，自动
  //    落到下面的回读路径——CPU 槽始终存在，降级不需要重分配。
  RenderBuffers* shared_target = nullptr;
  uint32 shared_index = ISharedFramePool::kInvalidFrame;
  if (shared_path_active_ && shared_pool_ != nullptr &&
      shared_pool_->IsAvailable()) {
    shared_index = shared_write_cursor_;
    shared_write_cursor_ = (shared_write_cursor_ + 1) % shared_frame_count_;
    if (rd_->TextureExternalBeginWrite(
            shared_targets_[shared_index].color_texture)) {
      shared_target = &shared_targets_[shared_index];
    } else {
      // 锁定失败 = 显示侧仍占用该帧；游标已推进，下轮自然轮到别的帧
      return false;
    }
  }

  // 4. 取空闲槽（FREE→WRITING）
  uint32 slot = queue_->TryAcquireFree();
  if (slot == UINT32_MAX) {
    if (shared_target != nullptr) {
      rd_->TextureExternalEndWrite(shared_target->color_texture);
    }
    return false;
  }

  // 5. 渲场景；composite 输出目标由呈现路径决定
  composite_override_ =
      shared_target != nullptr ? shared_target->framebuffer : RDFramebufferID();
  RenderFromStorage();
  composite_override_ = RDFramebufferID();

  if (shared_target != nullptr) {
    // 6a. Mode X：交还绘制权（GPU 侧写完可见）+ 提交帧 index
    //     token = 平台帧池的 index，上屏线程据此 CommitFrame；
    //     "显示侧读完了这块显存"的保证由下一次 BeginWrite 的跨驱动
    //     同步点兑现（见 platform/windows/dxgi_shared_frame_pool_win32）
    rd_->TextureExternalEndWrite(shared_target->color_texture);
    queue_->MarkReady(slot, shared_index);
    ++shared_frames_;
    return true;
  }

  // 6b. Mode R：回读 FBO → CPU 槽（开后处理时取 composite 输出，
  //     否则直接取场景 FBO）
  const RDFramebufferID readback_fb =
      (post_process_enabled_ && post_buffers_.IsValid())
          ? post_buffers_.framebuffer
          : scene_buffers_.framebuffer;
  const uint64 t0 = NowUs();
  rd_->FramebufferReadback(readback_fb, 0, 0, width_, height_,
                           queue_->GetPixels(slot));
  readback_us_ += NowUs() - t0;
  ++readback_frames_;
  readback_bytes_ += static_cast<uint64>(width_) * height_ * 4;

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

/**
 * @brief 建立共享显存呈现路径（导入帧池每一帧为可写渲染目标）
 *
 * @par 成立条件（任一不满足 → 返回 false，Mode R 原样生效）
 *   1. 平台给了帧池且 `IsAvailable()`；
 *   2. composite pass 可用——共享形态必须有一次全屏 pass 把场景
 *      写进共享显存（distortion/gamma 也因此不丢）；
 *   3. 后端能把外部显存导入成 GL 纹理（本调用即真实探测：
 *      WGL_NV_DX_interop2 缺失、跨适配器、驱动不支持 DX11 资源
 *      都在这里暴露）；
 *   4. 每帧的目标 FBO 建成（FBO 完整性失败会在 RD 侧告警）。
 *
 * @par 全有或全无
 *   中途失败即整体撤销（FinalizeSharedPath），不留半共享状态——
 *   否则上屏线程按 token 提交会对着不存在的目标等下去。
 *
 * @pre 已 InitializePostProcess（scene/post buffers 与 composite pipeline 就绪）
 * @pre 调用线程持有 current GL 上下文（初始化期主线程即满足）
 *
 * @return true 共享路径就绪
 */
bool RenderViewport::InitializeSharedPath() {
  shared_path_active_ = false;
  shared_frame_count_ = 0;
  shared_write_cursor_ = 0;

  if (shared_pool_ == nullptr || !shared_pool_->IsAvailable()) {
    return false;
  }
  if (!post_process_enabled_ || !post_buffers_.IsValid()) {
    ARHUD_LOG_INFO(
        "RenderViewport: shared present skipped (composite unavailable) — "
        "readback path");
    return false;
  }

  const uint32 frames = shared_pool_->GetFrameCount();
  if (frames == 0 || frames > kMaxSharedFrames) {
    ARHUD_LOG_WARN("RenderViewport: frame pool count %u unsupported (max %u)",
                   frames, kMaxSharedFrames);
    return false;
  }

  const uint32 pool_width = shared_pool_->GetFrame(0).width;
  const uint32 pool_height = shared_pool_->GetFrame(0).height;
  if (pool_width != width_ || pool_height != height_) {
    // 尺寸不一致 = 装配错误（帧池与视口不同步），必须回退而不是画歪
    ARHUD_LOG_WARN(
        "RenderViewport: frame pool size %ux%u != viewport %ux%u — "
        "readback fallback",
        pool_width, pool_height, width_.load(), height_.load());
    return false;
  }

  for (uint32 i = 0; i < frames; ++i) {
    const ISharedFramePool::Frame frame = shared_pool_->GetFrame(i);
    if (!frame.IsValid()) {
      ARHUD_LOG_WARN("RenderViewport: frame %u invalid — readback fallback", i);
      FinalizeSharedPath();
      return false;
    }

    ExternalTextureSource source;
    source.native_device = frame.native_device;
    source.native_resource = frame.native_resource;
    source.width = frame.width;
    source.height = frame.height;
    source.writable = true;
    // 格式翻译只在这一处发生（platform 词表 → DataFormat）；
    // 后端不支持该组合格式时 TextureCreateExternal 返回空 RID。
    source.format = (frame.format == SharedFrameFormat::kRgba8Unorm)
                        ? DataFormat::kR8G8B8A8Unorm
                        : DataFormat::kMax;

    RDTextureID color = rd_->TextureCreateExternal(source);
    if (color.IsNull()) {
      // 探测失败（扩展缺失/跨适配器/驱动不支持）：整体回退
      FinalizeSharedPath();
      ARHUD_LOG_INFO(
          "RenderViewport: shared present unavailable on this driver — "
          "readback path");
      return false;
    }

    // interop 规定"关联 DX 资源的 GL 对象，使用前必须先加锁"——
    // 未锁时该 GL 纹理没有可视化的 data store，挂进 FBO 会得到
    // GL_FRAMEBUFFER_UNSUPPORTED。故附件建立也在锁内完成。
    const bool locked = rd_->TextureExternalBeginWrite(color);
    if (!locked) {
      ARHUD_LOG_WARN(
          "RenderViewport: shared frame %u could not be locked for FBO "
          "assembly — readback fallback",
          i);
      FinalizeSharedPath();
      return false;
    }

    const Error create_err = shared_targets_[i].CreateExternal(
        rd_, color, frame.width, frame.height, source.format);
    rd_->TextureExternalEndWrite(color);

    if (create_err != Error::kOK) {
      FinalizeSharedPath();
      ARHUD_LOG_WARN(
          "RenderViewport: shared target %u create failed — readback fallback",
          i);
      return false;
    }
  }

  shared_frame_count_ = frames;
  shared_path_active_ = true;
  ARHUD_LOG_INFO("RenderViewport: shared-VRAM present path ready (%ux%u, "
                 "frames=%u, pool=%s)",
                 width_.load(), height_.load(), frames,
                 shared_pool_->GetBackendName());
  return true;
}

/**
 * @brief 撤销共享显存呈现路径
 *
 * @par 释放顺序（不可交换）
 *   先 Destroy 目标（内部走 TextureFreeImmediate → 解除 interop 别名），
 *   平台侧此后才允许释放/重建那批显存。引擎拆卸顺序：
 *   本函数（screens）→ rd.Finalize（内含 wglDXCloseDeviceNV）→
 *   pool.Finalize。反序会让 interop 关闭时面对已销毁的 D3D device。
 */
void RenderViewport::FinalizeSharedPath() {
  // Destroy 对空 ID 安全，故无条件遍历：半途失败的目标也要清干净
  // （非拥有的颜色附件走 TextureFreeImmediate，即时解除 interop 别名）
  if (rd_ != nullptr) {
    for (uint32 i = 0; i < kMaxSharedFrames; ++i) {
      shared_targets_[i].Destroy(rd_);
    }
  }
  shared_frame_count_ = 0;
  shared_write_cursor_ = 0;
  shared_path_active_ = false;
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
  // Mode X：composite 输出改投共享显存目标（由 RenderAndSubmit 每帧设置）
  const RDFramebufferID target = composite_override_.IsNull()
                                     ? post_buffers_.framebuffer
                                     : composite_override_;
  RenderPostProcessTo(p_screen_width, p_screen_height, target);
}

/**
 * @brief 后处理 composite pass 输出到指定 FBO
 *
 * @par 目标选择的唯一差别
 *   Mode R 投 post_buffers_.framebuffer（随后被回读）；Mode X 投
 *   共享显存目标的 FBO。输入的 uniform set（场景颜色纹理 + 参数 UBO）
 *   与 pipeline 完全共用——共享形态只是换了画布，不改画面。
 *
 * @param[in] p_screen_width  视口宽
 * @param[in] p_screen_height 视口高
 * @param[in] p_target        输出 FBO
 */
void RenderViewport::RenderPostProcessTo(uint32 p_screen_width,
                                         uint32 p_screen_height,
                                         RDFramebufferID p_target) {
  float params[5] = {
      post_process_params_.gamma,
      post_process_params_.enable_distortion ? 1.0f : 0.0f,
      post_process_params_.distortion_k1,
      post_process_params_.distortion_k2,
      post_process_params_.enable_premul_alpha ? 1.0f : 0.0f,
  };
  rd_->BufferUpdate(composite_params_ubo_, 0, sizeof(params), params);

  float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  // Composite output goes to the target FBO. The default framebuffer is
  // the hidden 100x100 offscreen window surface and nobody reads it back —
  // drawing there silently discards the result.
  if (p_target.IsNull()) {
    return;
  }
  DrawListScope scope(rd_, p_target, true, false, false, clear_color);
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
