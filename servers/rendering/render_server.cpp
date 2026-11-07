/**
 * @file render_server.cpp
 * @brief RenderServer 实现 — 主线程渲染 API 代理（Route B / B2~B3）
 *
 * @par 架构角色（B3 后）
 *   主线程只经本类发命令（资源创建走 RID 分裂 + 异步 PushBound / item 变更
 *   异步 PushBound / SubmitScene
 * 帧信号），渲染决策全部在渲染线程（RenderAndSubmit 同线程读
 * ItemStorage）。真并行：主线程发完即走。
 *
 * @par 帧节拍（三层背压）
 *   CommandQueue 容量背压 + RD frame slot（frame_count_=2）+
 *   FrameImageQueue HasInFlight（每屏 1 帧在途）。
 *
 * @see ADR-0010（绑定命令 (目标, 方法, 参数)）
 * @see command_queue.h  传输层
 *
 * @author yameng.he
 * @version 3.0
 * @date 2026-09-03
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "render_server.h"

#include <cstring>

#include "command_queue.h"
#include "io/logger.h"
#include "render_globals.h"
#include "render_viewport.h"
#include "renderer/canvas_renderer.h"
#include "renderer/spatial_renderer.h"
#include "storage/font_storage.h"
#include "storage/material_storage.h"
#include "storage/mesh_storage.h"
#include "storage/texture_storage.h"
#include "viewport_provider.h"

namespace arhud {

namespace {

uint32 NameSize(const char* p_name) {
  return (p_name != nullptr) ? static_cast<uint32>(strlen(p_name)) + 1 : 0;
}

CommandDataSpan MakeSpan(const void* p_data, uint32 p_size) {
  CommandDataSpan span;
  span.data = p_data;
  span.size = p_size;
  return span;
}
}  // namespace

void RenderServer::Initialize(CommandQueue* p_queue,
                              ARHudCompositor* p_compositor,
                              ItemStorage* p_item_storage,
                              IViewportProvider* p_viewport_provider) {
  queue_ = p_queue;
  compositor_ = p_compositor;
  item_storage_ = p_item_storage;
  viewport_provider_ = p_viewport_provider;
}

void RenderServer::Finalize() {
  queue_ = nullptr;
  compositor_ = nullptr;
  item_storage_ = nullptr;
  viewport_provider_ = nullptr;
}

bool RenderServer::CanBypassQueue() const {
  if (queue_ == nullptr || compositor_ == nullptr) {
    return true;  // 未绑定队列/合成器：直连（单线程/测试形态）
  }

  // 关闭中：直调但不排空——队列里剩下的命令可能引用已释放的资源
  if (queue_->IsShuttingDown()) {
    return true;
  }

  // 对齐 Godot 线程闸门（ASYNC_COND_PUSH 分支中的 flush_if_pending）：
  // 当调用者就是"事实上的执行者"时，直调 Storage 前必须先排空更早入队的命令，
  // 否则本次直调会插到它们前面 → 破坏程序顺序。
  //   - IsConsumerThread()：渲染线程在两轮 Flush 之间回调本类；
  //   - !HasConsumer()：单线程/测试形态，调用方即执行者。
  // 批处理内部（thunk 里）的再入请求被 CommandQueue 的 thread_local
  // 标志挡成空操作。
  if (queue_->IsConsumerThread() || !queue_->HasConsumer()) {
    queue_->FlushIfPending();
    return true;
  }

  return false;
}

RID RenderServer::TextureCreateFromData(const char* p_name, const uint8* p_data,
                                        uint32 p_data_size, uint32 p_width,
                                        uint32 p_height, DataFormat p_format,
                                        uint32 p_mip_levels) {
  TextureStorage* ts = compositor_->GetTextureStorage();

  if (CanBypassQueue()) {
    // 单线程/初始化期：同步创建（无渲染线程，不走命令队列）
    return ts->TextureCreateFromData((p_name != nullptr) ? p_name : "", p_data,
                                     p_data_size, p_width, p_height, p_format,
                                     p_mip_levels);
  }

  // RID 分裂（ADR-0010 §7.11 F / Godot FUNCRIDSPLIT）：
  //   主线程立即分配空壳 RID（线程安全 RIDOwner），初始化异步入队渲染线程。
  RID rid = ts->TextureAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  TextureCreateParams params;
  params.width = p_width;
  params.height = p_height;
  params.format = p_format;
  params.mip_levels = p_mip_levels;
  params.data_size = (p_data != nullptr) ? p_data_size : 0;
  params.name_size = NameSize(p_name);

  // blob 布局 [像素][名称]，与 TextureInitializeFromData 的切片约定一致
  const char* name = (p_name != nullptr) ? p_name : "";
  CommandDataSpan spans[2];
  uint32 span_count = 0;
  if (params.data_size > 0) {
    spans[span_count++] = MakeSpan(p_data, params.data_size);
  }
  spans[span_count++] = MakeSpan(name, params.name_size);

  queue_->PushBoundBlob(ts, &TextureStorage::TextureInitializeFromData, rid,
                        params, spans, span_count);
  return rid;
}

RID RenderServer::MaterialCreate(const char* p_name, RID p_shader,
                                 MaterialDataType p_type) {
  MaterialStorage* ms = compositor_->GetMaterialStorage();

  // 空 shader → 按类型取渲染器内置着色器（对齐原 ExecMaterialCreate 语义）
  RID shader = p_shader;
  if (!shader.IsValid()) {
    shader = (p_type == MaterialDataType::kSpatial)
                 ? compositor_->GetSpatialRenderer()->GetShaderRID()
                 : compositor_->GetCanvasRenderer()->GetShaderRID();
  }

  if (CanBypassQueue()) {
    return ms->MaterialCreate((p_name != nullptr) ? p_name : "", shader,
                              p_type);
  }

  // RID 分裂：主线程分配空壳，初始化异步入队（名称走 blob）
  RID rid = ms->MaterialAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  MaterialCreateParams params;
  params.shader = shader;
  params.data_type = p_type;
  params.name_size = NameSize(p_name);

  const char* name = (p_name != nullptr) ? p_name : "";
  CommandDataSpan span = MakeSpan(name, params.name_size);

  queue_->PushBoundBlob(ms, &MaterialStorage::MaterialInitialize, rid, params,
                        &span, 1);
  return rid;
}

RID RenderServer::MeshCreateQuad(const char* p_name, float p_width,
                                 float p_height) {
  MeshStorage* ms = compositor_->GetMeshStorage();
  if (CanBypassQueue()) {
    return ms->CreateQuad((p_name != nullptr) ? p_name : "", p_width, p_height);
  }

  RID rid = ms->MeshAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  MeshCreateQuadParams params;
  params.width = p_width;
  params.height = p_height;

  const char* name = (p_name != nullptr) ? p_name : "";
  CommandDataSpan span = MakeSpan(name, NameSize(p_name));

  queue_->PushBoundBlob(ms, &MeshStorage::MeshInitializeQuad, rid, params,
                        &span, 1);
  return rid;
}

RID RenderServer::MeshCreateBox(const char* p_name, float p_width,
                                float p_height, float p_depth) {
  MeshStorage* ms = compositor_->GetMeshStorage();
  if (CanBypassQueue()) {
    return ms->CreateBox((p_name != nullptr) ? p_name : "", p_width, p_height,
                         p_depth);
  }

  RID rid = ms->MeshAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  MeshCreateBoxParams params;
  params.width = p_width;
  params.height = p_height;
  params.depth = p_depth;

  const char* name = (p_name != nullptr) ? p_name : "";
  CommandDataSpan span = MakeSpan(name, NameSize(p_name));

  queue_->PushBoundBlob(ms, &MeshStorage::MeshInitializeBox, rid, params, &span,
                        1);
  return rid;
}

RID RenderServer::MeshCreateSphere(const char* p_name, float p_radius,
                                   uint32 p_sectors, uint32 p_rings) {
  MeshStorage* ms = compositor_->GetMeshStorage();
  if (CanBypassQueue()) {
    return ms->CreateSphere((p_name != nullptr) ? p_name : "", p_radius,
                            p_sectors, p_rings);
  }

  RID rid = ms->MeshAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  MeshCreateSphereParams params;
  params.radius = p_radius;
  params.radial_segments = p_sectors;
  params.rings = p_rings;

  const char* name = (p_name != nullptr) ? p_name : "";
  CommandDataSpan span = MakeSpan(name, NameSize(p_name));

  queue_->PushBoundBlob(ms, &MeshStorage::MeshInitializeSphere, rid, params,
                        &span, 1);
  return rid;
}

RID RenderServer::FontLoadFromFreeType(
    const char* p_name, const char* p_font_path, float p_font_size,
    const uint32* p_charcodes, uint32 p_charcode_count, float p_sdf_spread) {
  FontStorage* fs = compositor_->GetFontStorage();
  if (CanBypassQueue()) {
    return fs->FontLoadFromFreeType((p_name != nullptr) ? p_name : "",
                                    (p_font_path != nullptr) ? p_font_path : "",
                                    p_font_size, p_charcodes, p_charcode_count,
                                    p_sdf_spread);
  }

  // RID 分裂：主线程分配空壳，SDF 图集生成异步入队（名称/路径/码点走 blob）
  RID rid = fs->FontAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  const char* name = (p_name != nullptr) ? p_name : "";
  const char* path = (p_font_path != nullptr) ? p_font_path : "";

  FontCreateParams params;
  params.font_size = p_font_size;
  params.sdf_spread = p_sdf_spread;
  params.charcode_count = (p_charcodes != nullptr) ? p_charcode_count : 0;
  params.name_size = NameSize(name);
  params.path_size = NameSize(path);

  // blob 布局 [名称][路径][码点]，与 FontInitializeFromFreeType 的切片约定一致
  CommandDataSpan spans[3];
  uint32 span_count = 0;
  spans[span_count++] = MakeSpan(name, params.name_size);
  spans[span_count++] = MakeSpan(path, params.path_size);
  if (params.charcode_count > 0) {
    spans[span_count++] =
        MakeSpan(p_charcodes, params.charcode_count * sizeof(uint32));
  }

  queue_->PushBoundBlob(fs, &FontStorage::FontInitializeFromFreeType, rid,
                        params, spans, span_count);
  return rid;
}

Error RenderServer::MaterialSetUniform(RID p_material, uint32 p_offset,
                                       const void* p_data, uint32 p_size) {
  // B3：材质变更命令化（MaterialData::version 竞态根治——变更仅渲染线程触碰）
  if (CanBypassQueue()) {
    return compositor_->GetMaterialStorage()->MaterialSetUniform(
        p_material, p_offset, p_data, p_size);
  }
  MaterialSetUniformParams params;
  params.material = p_material;
  params.offset = p_offset;
  params.size = (p_data != nullptr) ? p_size : 0;

  CommandDataSpan span = MakeSpan(p_data, params.size);
  queue_->PushBoundBlob(compositor_->GetMaterialStorage(),
                        &MaterialStorage::MaterialSetUniformBlob, params, &span,
                        1);
  return Error::kOK;
}

Error RenderServer::MaterialSetTexture(RID p_material, uint32 p_slot,
                                       RID p_texture) {
  // B3：同上，命令化
  if (CanBypassQueue()) {
    return compositor_->GetMaterialStorage()->MaterialSetTexture(
        p_material, p_slot, p_texture);
  }
  queue_->PushBound(compositor_->GetMaterialStorage(),
                    &MaterialStorage::MaterialSetTexture, p_material, p_slot,
                    p_texture);
  return Error::kOK;
}

void RenderServer::TextureFree(RID p_texture) {
  if (CanBypassQueue()) {
    compositor_->GetTextureStorage()->TextureFree(p_texture);
    return;
  }

  queue_->PushBound(compositor_->GetTextureStorage(),
                    &TextureStorage::TextureFree, p_texture);
}

void RenderServer::MaterialFree(RID p_material) {
  if (CanBypassQueue()) {
    compositor_->GetMaterialStorage()->MaterialFree(p_material);
    return;
  }

  queue_->PushBound(compositor_->GetMaterialStorage(),
                    &MaterialStorage::MaterialFree, p_material);
}

void RenderServer::MeshFree(RID p_mesh) {
  if (CanBypassQueue()) {
    compositor_->GetMeshStorage()->MeshFree(p_mesh);
    return;
  }

  queue_->PushBound(compositor_->GetMeshStorage(), &MeshStorage::MeshFree,
                    p_mesh);
}

void RenderServer::FontFree(RID p_font) {
  if (CanBypassQueue()) {
    compositor_->GetFontStorage()->FontFree(p_font);
    return;
  }

  queue_->PushBound(compositor_->GetFontStorage(), &FontStorage::FontFree,
                    p_font);
}

RID RenderServer::CanvasItemCreate(uint32 p_visibility_mask) {
  if (CanBypassQueue()) {
    return item_storage_->CanvasItemCreate(p_visibility_mask);
  }
  // RID 分裂：主线程原子分配 RID，状态登记异步入队渲染线程
  RID rid = item_storage_->ItemAllocate();
  queue_->PushBound(item_storage_, &ItemStorage::CanvasItemInitialize, rid,
                    p_visibility_mask);
  return rid;
}

void RenderServer::CanvasItemSetMaterial(RID p_item, RID p_material) {
  if (CanBypassQueue()) {
    item_storage_->CanvasItemSetMaterial(p_item, p_material);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::CanvasItemSetMaterial, p_item,
                    p_material);
}

void RenderServer::CanvasItemSetMesh(RID p_item, RID p_mesh) {
  if (CanBypassQueue()) {
    item_storage_->CanvasItemSetMesh(p_item, p_mesh);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::CanvasItemSetMesh, p_item,
                    p_mesh);
}

void RenderServer::CanvasItemSetTransform(RID p_item,
                                          const float p_transform[16]) {
  if (CanBypassQueue()) {
    item_storage_->CanvasItemSetTransform(p_item, p_transform);
    return;
  }
  CommandDataSpan span = MakeSpan(p_transform, sizeof(float) * 16);
  queue_->PushBoundBlob(item_storage_, &ItemStorage::CanvasItemSetTransformBlob,
                        p_item, &span, 1);
}

void RenderServer::CanvasItemSetOpacity(RID p_item, float p_opacity) {
  if (CanBypassQueue()) {
    item_storage_->CanvasItemSetOpacity(p_item, p_opacity);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::CanvasItemSetOpacity, p_item,
                    p_opacity);
}

void RenderServer::CanvasItemSetZOrder(RID p_item, int32 p_z_order) {
  if (CanBypassQueue()) {
    item_storage_->CanvasItemSetZOrder(p_item, p_z_order);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::CanvasItemSetZOrder, p_item,
                    p_z_order);
}

void RenderServer::CanvasItemSetLayer(RID p_item, int32 p_layer) {
  if (CanBypassQueue()) {
    item_storage_->CanvasItemSetLayer(p_item, p_layer);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::CanvasItemSetLayer, p_item,
                    p_layer);
}

void RenderServer::CanvasItemSetVisible(RID p_item, bool p_visible) {
  if (CanBypassQueue()) {
    item_storage_->CanvasItemSetVisible(p_item, p_visible);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::CanvasItemSetVisible, p_item,
                    p_visible);
}

void RenderServer::CanvasItemSetParent(RID p_item, RID p_parent) {
  if (CanBypassQueue()) {
    item_storage_->CanvasItemSetParent(p_item, p_parent);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::CanvasItemSetParent, p_item,
                    p_parent);
}

void RenderServer::CanvasItemSetZAsRelative(RID p_item, bool p_relative) {
  if (CanBypassQueue()) {
    item_storage_->CanvasItemSetZAsRelative(p_item, p_relative);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::CanvasItemSetZAsRelative,
                    p_item, p_relative);
}

void RenderServer::CanvasItemFree(RID p_item) {
  if (CanBypassQueue()) {
    item_storage_->CanvasItemFree(p_item);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::CanvasItemFree, p_item);
}

RID RenderServer::SpatialItemCreate(uint32 p_visibility_mask) {
  if (CanBypassQueue()) {
    return item_storage_->SpatialItemCreate(p_visibility_mask);
  }
  RID rid = item_storage_->ItemAllocate();
  queue_->PushBound(item_storage_, &ItemStorage::SpatialItemInitialize, rid,
                    p_visibility_mask);
  return rid;
}

void RenderServer::SpatialItemSetMaterial(RID p_item, RID p_material) {
  if (CanBypassQueue()) {
    item_storage_->SpatialItemSetMaterial(p_item, p_material);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::SpatialItemSetMaterial, p_item,
                    p_material);
}

void RenderServer::SpatialItemSetMesh(RID p_item, RID p_mesh) {
  if (CanBypassQueue()) {
    item_storage_->SpatialItemSetMesh(p_item, p_mesh);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::SpatialItemSetMesh, p_item,
                    p_mesh);
}

void RenderServer::SpatialItemSetTransform(RID p_item,
                                           const float p_transform[16]) {
  if (CanBypassQueue()) {
    item_storage_->SpatialItemSetTransform(p_item, p_transform);
    return;
  }
  CommandDataSpan span = MakeSpan(p_transform, sizeof(float) * 16);
  queue_->PushBoundBlob(item_storage_,
                        &ItemStorage::SpatialItemSetTransformBlob, p_item,
                        &span, 1);
}

void RenderServer::SpatialItemSetRenderPriority(RID p_item, uint8 p_priority) {
  if (CanBypassQueue()) {
    item_storage_->SpatialItemSetRenderPriority(p_item, p_priority);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::SpatialItemSetRenderPriority,
                    p_item, p_priority);
}

void RenderServer::SpatialItemSetDisableCull(RID p_item, bool p_disable) {
  if (CanBypassQueue()) {
    item_storage_->SpatialItemSetDisableCull(p_item, p_disable);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::SpatialItemSetDisableCull,
                    p_item, p_disable);
}

void RenderServer::SpatialItemSetParent(RID p_item, RID p_parent) {
  if (CanBypassQueue()) {
    item_storage_->SpatialItemSetParent(p_item, p_parent);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::SpatialItemSetParent, p_item,
                    p_parent);
}

void RenderServer::SpatialItemFree(RID p_item) {
  if (CanBypassQueue()) {
    item_storage_->SpatialItemFree(p_item);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::SpatialItemFree, p_item);
}

RID RenderServer::TextItemCreate(uint32 p_visibility_mask) {
  if (CanBypassQueue()) {
    return item_storage_->TextItemCreate(p_visibility_mask);
  }
  RID rid = item_storage_->ItemAllocate();
  queue_->PushBound(item_storage_, &ItemStorage::TextItemInitialize, rid,
                    p_visibility_mask);
  return rid;
}

void RenderServer::TextItemSetFont(RID p_item, RID p_font) {
  if (CanBypassQueue()) {
    item_storage_->TextItemSetFont(p_item, p_font);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::TextItemSetFont, p_item,
                    p_font);
}

void RenderServer::TextItemSetText(RID p_item, const uint32* p_text,
                                   uint32 p_count) {
  if (CanBypassQueue()) {
    item_storage_->TextItemSetText(p_item, p_text, p_count);
    return;
  }
  CommandDataSpan span = MakeSpan(p_text, p_count * sizeof(uint32));
  queue_->PushBoundBlob(item_storage_, &ItemStorage::TextItemSetTextBlob,
                        p_item, p_count, &span, 1);
}

void RenderServer::TextItemSetPosition(RID p_item, float p_x, float p_y) {
  if (CanBypassQueue()) {
    item_storage_->TextItemSetPosition(p_item, p_x, p_y);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::TextItemSetPosition, p_item,
                    p_x, p_y);
}

void RenderServer::TextItemSetColor(RID p_item, const float p_color[4]) {
  if (CanBypassQueue()) {
    item_storage_->TextItemSetColor(p_item, p_color);
    return;
  }
  CommandDataSpan span = MakeSpan(p_color, sizeof(float) * 4);
  queue_->PushBoundBlob(item_storage_, &ItemStorage::TextItemSetColorBlob,
                        p_item, &span, 1);
}

void RenderServer::TextItemSetZOrder(RID p_item, int32 p_z_order) {
  if (CanBypassQueue()) {
    item_storage_->TextItemSetZOrder(p_item, p_z_order);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::TextItemSetZOrder, p_item,
                    p_z_order);
}

void RenderServer::TextItemSetLayer(RID p_item, int32 p_layer) {
  if (CanBypassQueue()) {
    item_storage_->TextItemSetLayer(p_item, p_layer);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::TextItemSetLayer, p_item,
                    p_layer);
}

void RenderServer::TextItemSetVisible(RID p_item, bool p_visible) {
  if (CanBypassQueue()) {
    item_storage_->TextItemSetVisible(p_item, p_visible);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::TextItemSetVisible, p_item,
                    p_visible);
}

void RenderServer::TextItemSetParent(RID p_item, RID p_parent) {
  if (CanBypassQueue()) {
    item_storage_->TextItemSetParent(p_item, p_parent);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::TextItemSetParent, p_item,
                    p_parent);
}

void RenderServer::TextItemSetZAsRelative(RID p_item, bool p_relative) {
  if (CanBypassQueue()) {
    item_storage_->TextItemSetZAsRelative(p_item, p_relative);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::TextItemSetZAsRelative, p_item,
                    p_relative);
}

void RenderServer::TextItemFree(RID p_item) {
  if (CanBypassQueue()) {
    item_storage_->TextItemFree(p_item);
    return;
  }
  queue_->PushBound(item_storage_, &ItemStorage::TextItemFree, p_item);
}

void RenderServer::Camera3DSet(uint32 p_viewport,
                               const CameraMatrix& p_camera) {
  // ADR-0006: 命令链只携带相机**意图参数**，不传矩阵——CameraMatrix 惰性重算，
  // 未 UpdateMatrices() 时 Get*Matrix()
  // 返回陈旧/默认值，传矩阵会致相机端到端不生效。
  if (p_camera.IsUsingManualView()) {
    ARHUD_LOG_WARN(
        "RenderServer::Camera3DSet: manual view matrix (SetViewTransform) "
        "is not supported over the command queue; ignored (viewport=%u)",
        p_viewport);
    return;
  }

  if (CanBypassQueue()) {
    RenderViewport* viewport = ResolveViewport(p_viewport);
    if (viewport == nullptr) {
      return;
    }
    viewport->Camera3DSet(p_camera);
    if (item_storage_ != nullptr) {
      item_storage_->MarkScreen(p_viewport);
    }
    return;
  }

  Camera3DIntent intent;
  intent.projection_type = static_cast<uint8>(p_camera.GetProjectionType());
  intent.keep_aspect = static_cast<uint8>(p_camera.GetKeepAspect());
  intent.ortho_mode = p_camera.IsUsingManualOrthoRect() ? 1 : 0;
  intent.fov_y = p_camera.GetFOV();
  intent.aspect = p_camera.GetAspect();
  intent.near_z = p_camera.GetNearZ();
  intent.far_z = p_camera.GetFarZ();
  intent.ortho_size = p_camera.GetOrthoSize();
  if (intent.ortho_mode == 1) {
    p_camera.GetOrthoRect(intent.ortho_rect[0], intent.ortho_rect[1],
                          intent.ortho_rect[2], intent.ortho_rect[3]);
  }
  intent.position[0] = p_camera.GetPosition().x;
  intent.position[1] = p_camera.GetPosition().y;
  intent.position[2] = p_camera.GetPosition().z;
  intent.target[0] = p_camera.GetTarget().x;
  intent.target[1] = p_camera.GetTarget().y;
  intent.target[2] = p_camera.GetTarget().z;
  intent.up[0] = p_camera.GetUp().x;
  intent.up[1] = p_camera.GetUp().y;
  intent.up[2] = p_camera.GetUp().z;
  queue_->PushBound(this, &RenderServer::ApplyCamera3D, p_viewport, intent);
}

void RenderServer::ApplyCamera3D(uint32 p_viewport,
                                 const Camera3DIntent& p_intent) {
  CameraMatrix cam;
  const ProjectionType proj =
      static_cast<ProjectionType>(p_intent.projection_type);
  if (proj == ProjectionType::kPerspective) {
    cam.SetPerspective(p_intent.fov_y, p_intent.aspect, p_intent.near_z,
                       p_intent.far_z);
  } else if (p_intent.ortho_mode == 1) {
    cam.SetOrthographic(p_intent.ortho_rect[0], p_intent.ortho_rect[1],
                        p_intent.ortho_rect[2], p_intent.ortho_rect[3],
                        p_intent.near_z, p_intent.far_z);
  } else {
    cam.SetOrthographic(p_intent.ortho_size, p_intent.aspect, p_intent.near_z,
                        p_intent.far_z);
  }
  cam.SetKeepAspect(static_cast<KeepAspect>(p_intent.keep_aspect));
  cam.LookAt(
      Vec3(p_intent.position[0], p_intent.position[1], p_intent.position[2]),
      Vec3(p_intent.target[0], p_intent.target[1], p_intent.target[2]),
      Vec3(p_intent.up[0], p_intent.up[1], p_intent.up[2]));

  RenderViewport* viewport = ResolveViewport(p_viewport);
  if (viewport == nullptr) {
    return;
  }
  viewport->Camera3DSet(cam);
  if (item_storage_ != nullptr) {
    item_storage_->MarkScreen(p_viewport);
  }
}

void RenderServer::Camera2DSet(uint32 p_viewport, const Transform2D& p_camera) {
  if (CanBypassQueue()) {
    RenderViewport* viewport = ResolveViewport(p_viewport);
    if (viewport == nullptr) {
      return;
    }
    viewport->Camera2DSet(p_camera);
    if (item_storage_ != nullptr) {
      item_storage_->MarkScreen(p_viewport);
    }
    return;
  }
  Camera2DIntent intent;
  for (int i = 0; i < 3; ++i) {
    intent.columns[i * 2] = p_camera.columns[i].x;
    intent.columns[i * 2 + 1] = p_camera.columns[i].y;
  }
  queue_->PushBound(this, &RenderServer::ApplyCamera2D, p_viewport, intent);
}

void RenderServer::ApplyCamera2D(uint32 p_viewport,
                                 const Camera2DIntent& p_intent) {
  Transform2D cam;
  for (int i = 0; i < 3; ++i) {
    cam.columns[i].x = p_intent.columns[i * 2];
    cam.columns[i].y = p_intent.columns[i * 2 + 1];
  }

  RenderViewport* viewport = ResolveViewport(p_viewport);
  if (viewport == nullptr) {
    return;
  }
  viewport->Camera2DSet(cam);
  if (item_storage_ != nullptr) {
    item_storage_->MarkScreen(p_viewport);
  }
}

void RenderServer::DirectionalLightSet(uint32 p_viewport,
                                       const DirectionalLight& p_light) {
  if (CanBypassQueue()) {
    RenderViewport* viewport = ResolveViewport(p_viewport);
    if (viewport == nullptr) {
      return;
    }
    viewport->DirectionalLightSet(p_light);
    if (item_storage_ != nullptr) {
      item_storage_->MarkScreen(p_viewport);
    }
    return;
  }
  queue_->PushBound(this, &RenderServer::ApplyDirectionalLight, p_viewport,
                    p_light);
}

void RenderServer::ApplyDirectionalLight(uint32 p_viewport,
                                         const DirectionalLight& p_light) {
  RenderViewport* viewport = ResolveViewport(p_viewport);
  if (viewport == nullptr) {
    return;
  }
  viewport->DirectionalLightSet(p_light);
  if (item_storage_ != nullptr) {
    item_storage_->MarkScreen(p_viewport);
  }
}

void RenderServer::DirectionalLightClear(uint32 p_viewport) {
  if (CanBypassQueue()) {
    RenderViewport* viewport = ResolveViewport(p_viewport);
    if (viewport == nullptr) {
      return;
    }
    viewport->DirectionalLightClear();
    if (item_storage_ != nullptr) {
      item_storage_->MarkScreen(p_viewport);
    }
    return;
  }
  queue_->PushBound(this, &RenderServer::ApplyDirectionalLightClear,
                    p_viewport);
}

void RenderServer::ApplyDirectionalLightClear(uint32 p_viewport) {
  RenderViewport* viewport = ResolveViewport(p_viewport);
  if (viewport == nullptr) {
    return;
  }
  viewport->DirectionalLightClear();
  if (item_storage_ != nullptr) {
    item_storage_->MarkScreen(p_viewport);
  }
}

/**
 * @brief 按视口索引解析 RenderViewport（ADR-0007，短路路径）
 *
 * 经 Engine 注入的 IViewportProvider；未注入或越界返回 nullptr（WARN）。
 */
RenderViewport* RenderServer::ResolveViewport(uint32 p_index) const {
  if (viewport_provider_ == nullptr) {
    ARHUD_LOG_WARN(
        "RenderServer: viewport provider not set, camera/light call dropped "
        "(index=%u)",
        p_index);
    return nullptr;
  }
  RenderViewport* viewport = viewport_provider_->GetViewport(p_index);
  if (viewport == nullptr) {
    ARHUD_LOG_WARN(
        "RenderServer: viewport index %u out of range, camera/light call "
        "dropped",
        p_index);
  }
  return viewport;
}

void RenderServer::SubmitScene() {
  queue_->PushBound(this, &RenderServer::ApplySubmitScene);
}

void RenderServer::ApplySubmitScene() {
  // 帧提交信号（Engine 注入 via RenderGlobals）；渲染线程置位，主线程循环消费
  if (RenderGlobals::submit_signal != nullptr) {
    RenderGlobals::submit_signal->store(true, std::memory_order_release);
  }
}

}  // namespace arhud
