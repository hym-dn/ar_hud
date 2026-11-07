/**
 * @file rendering_server_default.cpp
 * @brief RenderingServerDefault 实现（实现侧）— S0 全部转发给旧 RenderServer
 *
 * @par 内容
 *   - 装配：`Initialize` / `Finalize` 转发 RenderServer 的依赖注入 / 解绑；
 *   - 转发：资源 / 2D / 3D / 文本 / 相机灯光 /
 * 帧，逐方法原样转发（零行为变化）；
 *   - 占位：`Init/Finish/Draw/Sync/HasChanged/FreeRid`（S1）与
 * `Viewport*`（S6）， 在对应阶段落地前为空实现（详见 rendering_server_default.h
 * 的阶段表）。
 *
 * @par 命名映射（本类 → 被委托的旧名）
 *   - `Instance*`            → `RenderServer::SpatialItem*`
 *   - `MaterialSetParam`     → `RenderServer::MaterialSetUniform`
 *   - `FontCreateFromFreeType` → `RenderServer::FontLoadFromFreeType`
 *   其余方法名与旧 RenderServer 同名。
 *
 * @note 本文件各函数体均为**单行转发**，不引入任何逻辑；参数 / 返回值语义见
 *       rendering_server.h（公共 API Doxygen）。
 *
 * @see rendering_server_default.h  门面声明与阶段说明
 * @see render_server.h             被委托的旧实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-10-03
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "rendering_server_default.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
//  S0 装配
// ═══════════════════════════════════════════════════════════════════════

/// @brief 绑定依赖（实现侧：记录非拥有的 queue_ 供线程判定，并转发注入）
/// @copydoc RenderingServerDefault::Initialize
void RenderingServerDefault::Initialize(
    CommandQueue* p_queue, ARHudCompositor* p_compositor,
    ItemStorage* p_item_storage, IViewportProvider* p_viewport_provider) {
  queue_ = p_queue;
  render_server_.Initialize(p_queue, p_compositor, p_item_storage,
                            p_viewport_provider);
}

/// @brief 解绑依赖（实现侧：先转发解绑，再清空 queue_ 防悬垂）
/// @copydoc RenderingServerDefault::Finalize
void RenderingServerDefault::Finalize() {
  render_server_.Finalize();
  queue_ = nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
//  生命周期 / 帧 / 线程
// ═══════════════════════════════════════════════════════════════════════

/// @brief S0 占位：生命周期（命令队列 + 渲染线程创建）仍归 ARHudEngine 管理（S1
/// 落地）
void RenderingServerDefault::Init() {}

/// @brief S0 占位：生命周期仍归 ARHudEngine 管理（S1 落地）
void RenderingServerDefault::Finish() {}

/// @brief S0 占位：帧编排仍由 ARHudEngine::RenderThreadFunc 驱动（S1 落地）
void RenderingServerDefault::Draw(bool /*p_present*/, double /*p_step*/) {}

/// @brief 同步屏障：委托 CommandQueue::Sync()（对齐 Godot
///        RenderingServerDefault::sync() → CommandQueueMT::sync()）
void RenderingServerDefault::Sync() {
  if (queue_ != nullptr) {
    queue_->Sync();
  }
}

/// @brief S0 占位：changes 计数落地前恒为 false（S1 落地）
bool RenderingServerDefault::HasChanged() const {
  return false;
}

/// @brief 线程判定（实现侧：委托命令队列的消费者线程判定；queue_ 为空视为
/// false）
/// @copydoc RenderingServerDefault::IsOnRenderThread
bool RenderingServerDefault::IsOnRenderThread() {
  return queue_ != nullptr && queue_->IsConsumerThread();
}

// ═══════════════════════════════════════════════════════════════════════
//  资源
// ═══════════════════════════════════════════════════════════════════════

/// @brief 转发 → RenderServer::TextureCreateFromData
RID RenderingServerDefault::TextureCreateFromData(
    const char* p_name, const uint8* p_data, uint32 p_data_size, uint32 p_width,
    uint32 p_height, DataFormat p_format, uint32 p_mip_levels) {
  return render_server_.TextureCreateFromData(
      p_name, p_data, p_data_size, p_width, p_height, p_format, p_mip_levels);
}

/// @brief 转发 → RenderServer::MaterialCreate
RID RenderingServerDefault::MaterialCreate(const char* p_name, RID p_shader,
                                           MaterialDataType p_type) {
  return render_server_.MaterialCreate(p_name, p_shader, p_type);
}

/// @brief 转发 → RenderServer::MeshCreateQuad
RID RenderingServerDefault::MeshCreateQuad(const char* p_name, float p_width,
                                           float p_height) {
  return render_server_.MeshCreateQuad(p_name, p_width, p_height);
}

/// @brief 转发 → RenderServer::MeshCreateBox
RID RenderingServerDefault::MeshCreateBox(const char* p_name, float p_width,
                                          float p_height, float p_depth) {
  return render_server_.MeshCreateBox(p_name, p_width, p_height, p_depth);
}

/// @brief 转发 → RenderServer::MeshCreateSphere
RID RenderingServerDefault::MeshCreateSphere(const char* p_name, float p_radius,
                                             uint32 p_sectors, uint32 p_rings) {
  return render_server_.MeshCreateSphere(p_name, p_radius, p_sectors, p_rings);
}

/// @brief 转发 →
/// RenderServer::FontLoadFromFreeType（命名映射：FontCreateFromFreeType）
RID RenderingServerDefault::FontCreateFromFreeType(
    const char* p_name, const char* p_font_path, float p_font_size,
    const uint32* p_charcodes, uint32 p_charcode_count, float p_sdf_spread) {
  return render_server_.FontLoadFromFreeType(p_name, p_font_path, p_font_size,
                                             p_charcodes, p_charcode_count,
                                             p_sdf_spread);
}

/// @brief 转发 → RenderServer::MaterialSetUniform（命名映射：MaterialSetParam）
Error RenderingServerDefault::MaterialSetParam(RID p_material, uint32 p_offset,
                                               const void* p_data,
                                               uint32 p_size) {
  return render_server_.MaterialSetUniform(p_material, p_offset, p_data,
                                           p_size);
}

/// @brief 转发 → RenderServer::MaterialSetTexture
Error RenderingServerDefault::MaterialSetTexture(RID p_material, uint32 p_slot,
                                                 RID p_texture) {
  return render_server_.MaterialSetTexture(p_material, p_slot, p_texture);
}

/// @brief 转发 → RenderServer::TextureFree
void RenderingServerDefault::TextureFree(RID p_texture) {
  render_server_.TextureFree(p_texture);
}

/// @brief 转发 → RenderServer::MaterialFree
void RenderingServerDefault::MaterialFree(RID p_material) {
  render_server_.MaterialFree(p_material);
}

/// @brief 转发 → RenderServer::MeshFree
void RenderingServerDefault::MeshFree(RID p_mesh) {
  render_server_.MeshFree(p_mesh);
}

/// @brief 转发 → RenderServer::FontFree
void RenderingServerDefault::FontFree(RID p_font) {
  render_server_.FontFree(p_font);
}

/// @brief S0 占位：统一回收链（依次询问各 Storage）落地前为空操作（S1 落地）
void RenderingServerDefault::FreeRid(RID /*p_rid*/) {}

// ═══════════════════════════════════════════════════════════════════════
//  2D item
// ═══════════════════════════════════════════════════════════════════════

/// @brief 转发 → RenderServer::CanvasItemCreate
RID RenderingServerDefault::CanvasItemCreate(uint32 p_visibility_mask) {
  return render_server_.CanvasItemCreate(p_visibility_mask);
}

/// @brief 转发 → RenderServer::CanvasItemSetMaterial
void RenderingServerDefault::CanvasItemSetMaterial(RID p_item, RID p_material) {
  render_server_.CanvasItemSetMaterial(p_item, p_material);
}

/// @brief 转发 → RenderServer::CanvasItemSetMesh
void RenderingServerDefault::CanvasItemSetMesh(RID p_item, RID p_mesh) {
  render_server_.CanvasItemSetMesh(p_item, p_mesh);
}

/// @brief 转发 → RenderServer::CanvasItemSetTransform
void RenderingServerDefault::CanvasItemSetTransform(
    RID p_item, const float p_transform[16]) {
  render_server_.CanvasItemSetTransform(p_item, p_transform);
}

/// @brief 转发 → RenderServer::CanvasItemSetOpacity
void RenderingServerDefault::CanvasItemSetOpacity(RID p_item, float p_opacity) {
  render_server_.CanvasItemSetOpacity(p_item, p_opacity);
}

/// @brief 转发 → RenderServer::CanvasItemSetZOrder
void RenderingServerDefault::CanvasItemSetZOrder(RID p_item, int32 p_z_order) {
  render_server_.CanvasItemSetZOrder(p_item, p_z_order);
}

/// @brief 转发 → RenderServer::CanvasItemSetLayer
void RenderingServerDefault::CanvasItemSetLayer(RID p_item, int32 p_layer) {
  render_server_.CanvasItemSetLayer(p_item, p_layer);
}

/// @brief 转发 → RenderServer::CanvasItemSetVisible
void RenderingServerDefault::CanvasItemSetVisible(RID p_item, bool p_visible) {
  render_server_.CanvasItemSetVisible(p_item, p_visible);
}

/// @brief 转发 → RenderServer::CanvasItemSetParent
void RenderingServerDefault::CanvasItemSetParent(RID p_item, RID p_parent) {
  render_server_.CanvasItemSetParent(p_item, p_parent);
}

/// @brief 转发 → RenderServer::CanvasItemSetZAsRelative
void RenderingServerDefault::CanvasItemSetZAsRelative(RID p_item,
                                                      bool p_relative) {
  render_server_.CanvasItemSetZAsRelative(p_item, p_relative);
}

/// @brief 转发 → RenderServer::CanvasItemFree
void RenderingServerDefault::CanvasItemFree(RID p_item) {
  render_server_.CanvasItemFree(p_item);
}

// ═══════════════════════════════════════════════════════════════════════
//  3D item（Instance* → 旧 SpatialItem*）
// ═══════════════════════════════════════════════════════════════════════

/// @brief 转发 → RenderServer::SpatialItemCreate（命名映射：Instance*）
RID RenderingServerDefault::InstanceCreate(uint32 p_visibility_mask) {
  return render_server_.SpatialItemCreate(p_visibility_mask);
}

/// @brief 转发 → RenderServer::SpatialItemSetMaterial
void RenderingServerDefault::InstanceSetMaterial(RID p_item, RID p_material) {
  render_server_.SpatialItemSetMaterial(p_item, p_material);
}

/// @brief 转发 → RenderServer::SpatialItemSetMesh
void RenderingServerDefault::InstanceSetMesh(RID p_item, RID p_mesh) {
  render_server_.SpatialItemSetMesh(p_item, p_mesh);
}

/// @brief 转发 → RenderServer::SpatialItemSetTransform
void RenderingServerDefault::InstanceSetTransform(RID p_item,
                                                  const float p_transform[16]) {
  render_server_.SpatialItemSetTransform(p_item, p_transform);
}

/// @brief 转发 → RenderServer::SpatialItemSetRenderPriority
void RenderingServerDefault::InstanceSetRenderPriority(RID p_item,
                                                       uint8 p_priority) {
  render_server_.SpatialItemSetRenderPriority(p_item, p_priority);
}

/// @brief 转发 → RenderServer::SpatialItemSetDisableCull
void RenderingServerDefault::InstanceSetDisableCull(RID p_item,
                                                    bool p_disable) {
  render_server_.SpatialItemSetDisableCull(p_item, p_disable);
}

/// @brief 转发 → RenderServer::SpatialItemSetParent
void RenderingServerDefault::InstanceSetParent(RID p_item, RID p_parent) {
  render_server_.SpatialItemSetParent(p_item, p_parent);
}

/// @brief 转发 → RenderServer::SpatialItemFree
void RenderingServerDefault::InstanceFree(RID p_item) {
  render_server_.SpatialItemFree(p_item);
}

// ═══════════════════════════════════════════════════════════════════════
//  文本 item
// ═══════════════════════════════════════════════════════════════════════

/// @brief 转发 → RenderServer::TextItemCreate
RID RenderingServerDefault::TextItemCreate(uint32 p_visibility_mask) {
  return render_server_.TextItemCreate(p_visibility_mask);
}

/// @brief 转发 → RenderServer::TextItemSetFont
void RenderingServerDefault::TextItemSetFont(RID p_item, RID p_font) {
  render_server_.TextItemSetFont(p_item, p_font);
}

/// @brief 转发 → RenderServer::TextItemSetText
void RenderingServerDefault::TextItemSetText(RID p_item, const uint32* p_text,
                                             uint32 p_count) {
  render_server_.TextItemSetText(p_item, p_text, p_count);
}

/// @brief 转发 → RenderServer::TextItemSetPosition
void RenderingServerDefault::TextItemSetPosition(RID p_item, float p_x,
                                                 float p_y) {
  render_server_.TextItemSetPosition(p_item, p_x, p_y);
}

/// @brief 转发 → RenderServer::TextItemSetColor
void RenderingServerDefault::TextItemSetColor(RID p_item,
                                              const float p_color[4]) {
  render_server_.TextItemSetColor(p_item, p_color);
}

/// @brief 转发 → RenderServer::TextItemSetZOrder
void RenderingServerDefault::TextItemSetZOrder(RID p_item, int32 p_z_order) {
  render_server_.TextItemSetZOrder(p_item, p_z_order);
}

/// @brief 转发 → RenderServer::TextItemSetLayer
void RenderingServerDefault::TextItemSetLayer(RID p_item, int32 p_layer) {
  render_server_.TextItemSetLayer(p_item, p_layer);
}

/// @brief 转发 → RenderServer::TextItemSetVisible
void RenderingServerDefault::TextItemSetVisible(RID p_item, bool p_visible) {
  render_server_.TextItemSetVisible(p_item, p_visible);
}

/// @brief 转发 → RenderServer::TextItemSetParent
void RenderingServerDefault::TextItemSetParent(RID p_item, RID p_parent) {
  render_server_.TextItemSetParent(p_item, p_parent);
}

/// @brief 转发 → RenderServer::TextItemSetZAsRelative
void RenderingServerDefault::TextItemSetZAsRelative(RID p_item,
                                                    bool p_relative) {
  render_server_.TextItemSetZAsRelative(p_item, p_relative);
}

/// @brief 转发 → RenderServer::TextItemFree
void RenderingServerDefault::TextItemFree(RID p_item) {
  render_server_.TextItemFree(p_item);
}

// ═══════════════════════════════════════════════════════════════════════
//  相机 / 灯光
// ═══════════════════════════════════════════════════════════════════════

/// @brief 转发 → RenderServer::Camera3DSet
void RenderingServerDefault::Camera3DSet(uint32 p_viewport,
                                         const CameraMatrix& p_camera) {
  render_server_.Camera3DSet(p_viewport, p_camera);
}

/// @brief 转发 → RenderServer::Camera2DSet
void RenderingServerDefault::Camera2DSet(uint32 p_viewport,
                                         const Transform2D& p_camera) {
  render_server_.Camera2DSet(p_viewport, p_camera);
}

/// @brief 转发 → RenderServer::DirectionalLightSet
void RenderingServerDefault::DirectionalLightSet(
    uint32 p_viewport, const DirectionalLight& p_light) {
  render_server_.DirectionalLightSet(p_viewport, p_light);
}

/// @brief 转发 → RenderServer::DirectionalLightClear
void RenderingServerDefault::DirectionalLightClear(uint32 p_viewport) {
  render_server_.DirectionalLightClear(p_viewport);
}

// ═══════════════════════════════════════════════════════════════════════
//  视口（S6 落地前为占位）
// ═══════════════════════════════════════════════════════════════════════

/// @brief S0 占位：视口归 RendererViewport 后落地，暂返回空 RID（S6 落地）
RID RenderingServerDefault::ViewportCreate(uint32 /*p_width*/,
                                           uint32 /*p_height*/) {
  return RID();
}

/// @brief S0 占位（S6 落地）
void RenderingServerDefault::ViewportDestroy(RID /*p_viewport*/) {}

/// @brief S0 占位（S6 落地）
void RenderingServerDefault::ViewportSetSize(RID /*p_viewport*/,
                                             uint32 /*p_width*/,
                                             uint32 /*p_height*/) {}

/// @brief S0 占位（S6 落地）
void RenderingServerDefault::ViewportSetActive(RID /*p_viewport*/,
                                               bool /*p_active*/) {}

// ═══════════════════════════════════════════════════════════════════════
//  帧
// ═══════════════════════════════════════════════════════════════════════

/// @brief 转发 → RenderServer::SubmitScene
void RenderingServerDefault::SubmitScene() {
  render_server_.SubmitScene();
}

}  // namespace arhud