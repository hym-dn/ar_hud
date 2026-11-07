/**
 * @file rendering_server_default.h
 * @brief RenderingServer 默认实现 — S0 绞杀者门面（内部委托旧 RenderServer）
 *
 * @par 职责
 *   实现冻结接口 RenderingServer，并把每个调用**原样转发**给旧 RenderServer，
 *   使 app 先稳定在目标接口上，而行为与 S0 之前完全一致（零行为变化）。
 *
 * @par 阶段（S0：绞杀者门面）
 *   本类是 architecture.md §7.12 S0
 * 的产物。目标不是实现功能，而是**钉住接缝**： app → RenderingServer（抽象）→
 * RenderingServerDefault（门面）→ RenderServer（旧）。 后续各阶段（S1 队列 / S2
 * RSG / S3~S6 拆分）再逐个替换门面内部实现。
 *
 * @par 转发约定
 *   除下述"占位"方法外，其余方法均为纯转发，且需做两处命名映射：
 *   | 本类（目标名） | 转发目标（旧名） |
 *   | --- | --- |
 *   | `Instance*` | `RenderServer::SpatialItem*` |
 *   | `MaterialSetParam` | `RenderServer::MaterialSetUniform` |
 *   | `FontCreateFromFreeType` | `RenderServer::FontLoadFromFreeType` |
 *   其余方法名与旧 RenderServer 同名，逐一对转发。
 *
 * @par 尚未落地的接口（stage-gated 占位）
 *   | 方法 | 阶段 | 说明 |
 *   | --- | --- | --- |
 *   | `Init` / `Finish` | S1 | 生命周期仍由 ARHudEngine 管理 |
 *   | `Draw` | S1 | 帧编排仍由 ARHudEngine::RenderThreadFunc 驱动 |
 *   | `Sync` / `HasChanged` / `FreeRid` | S1 | 队列同步 / changes 计数 /
 * Storage 认领链 | | `ViewportCreate/Destroy/SetSize/SetActive` | S6 | 视口归
 * RendererViewport 后落地 |
 *
 * @par 装配与所有权
 *   - 由 ARHudEngine **以值持有**（非堆分配），生命周期绑定引擎；
 *   - 装配：`Initialize()` 注入依赖并转发给 RenderServer::Initialize；
 *     销毁：`Finalize()` 转发给 RenderServer::Finalize（防悬垂）；
 *   - `queue_` 为**非拥有**指针，仅供 `IsOnRenderThread()` 查询。
 *
 * @see rendering_server.h          冻结接口（公共 API Doxygen）
 * @see render_server.h             S0 被委托的旧实现
 * @see arhud_engine.cpp            装配点（值持有 + 设置 singleton）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-10-03
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDERING_SERVER_DEFAULT_H_
#define ARHUD_SERVERS_RENDERING_RENDERING_SERVER_DEFAULT_H_

#include "command_queue.h"
#include "render_server.h"
#include "rendering_server.h"

namespace arhud {

/**
 * @brief RenderingServer 的默认实现（S0：委托旧 RenderServer）
 *
 * 门面类：把 RenderingServer 的全部虚函数实现为对内部 `render_server_` 的转发，
 * 从而在不改变行为的前提下，把 app 依赖从具体类 RenderServer 解耦到抽象接口。
 *
 * @note 各 override 的完整参数 / 返回值语义沿用 RenderingServer 的文档
 *       （多数以 `@copydoc RenderingServer::<方法>` 继承）。
 * @see RenderingServer
 */
class RenderingServerDefault : public RenderingServer {
  ARHUD_DISABLE_COPY_MOVE(RenderingServerDefault);

 public:
  RenderingServerDefault() = default;
  ~RenderingServerDefault() override = default;

  // ─────────────────────────────────────────────────────────────────
  //  S0 装配（非抽象接口）：注入旧 RenderServer 的依赖，并转发
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 绑定依赖（装配期调用一次；转发 RenderServer::Initialize）
   *
   * @param[in] p_queue             命令队列（主线程 push / 渲染线程 flush）
   * @param[in] p_compositor        Compositor（短路路径直接调 Storage）
   * @param[in] p_item_storage     保留模式 item 存储（短路路径）
   * @param[in] p_viewport_provider 视口解析器（ADR-0007；可空 = 忽略）
   *
   * @note 同时记录 `queue_` 供 `IsOnRenderThread()` 查询（非拥有）。
   * @see Finalize
   */
  void Initialize(CommandQueue* p_queue, ARHudCompositor* p_compositor,
                  ItemStorage* p_item_storage,
                  IViewportProvider* p_viewport_provider = nullptr);

  /**
   * @brief 解绑依赖（销毁前调用；转发 RenderServer::Finalize）
   * @note 会清空 `queue_`，避免销毁后 `IsOnRenderThread()` 悬垂访问。
   * @see Initialize
   */
  void Finalize();

  // ─────────────────────────────────────────────────────────────────
  //  RenderingServer — 生命周期 / 帧 / 线程
  // ─────────────────────────────────────────────────────────────────

  /// @brief 初始化渲染服务（S1 落地；S0 空实现）
  /// @copydoc RenderingServer::Init
  void Init() override;

  /// @brief 终止渲染服务（S1 落地；S0 空实现）
  /// @copydoc RenderingServer::Finish
  void Finish() override;

  /// @brief 渲染线程帧入口（S1 落地；S0 空实现）
  /// @copydoc RenderingServer::Draw
  void Draw(bool p_present, double p_step) override;

  /// @brief 同步命令队列（S0 已实现：委托 CommandQueue::Sync() 整批屏障）
  /// @copydoc RenderingServer::Sync
  void Sync() override;

  /// @brief 是否存在待处理变更（S1 落地；S0 恒 false）
  /// @copydoc RenderingServer::HasChanged
  bool HasChanged() const override;

  /// @brief 当前是否在渲染线程（S0 已实现：委托 queue_->IsConsumerThread()）
  /// @copydoc RenderingServer::IsOnRenderThread
  bool IsOnRenderThread() override;

  // ─────────────────────────────────────────────────────────────────
  //  RenderingServer — 资源
  // ─────────────────────────────────────────────────────────────────

  /// @brief 从像素数据创建纹理（转发 → RenderServer::TextureCreateFromData）
  /// @copydoc RenderingServer::TextureCreateFromData
  RID TextureCreateFromData(const char* p_name, const uint8* p_data,
                            uint32 p_data_size, uint32 p_width, uint32 p_height,
                            DataFormat p_format,
                            uint32 p_mip_levels = 1) override;

  /// @brief 创建材质（转发 → RenderServer::MaterialCreate）
  /// @copydoc RenderingServer::MaterialCreate
  RID MaterialCreate(
      const char* p_name, RID p_shader,
      MaterialDataType p_type = MaterialDataType::kCanvas) override;

  /// @brief 创建 2D 四边形网格（转发 → RenderServer::MeshCreateQuad）
  /// @copydoc RenderingServer::MeshCreateQuad
  RID MeshCreateQuad(const char* p_name, float p_width,
                     float p_height) override;

  /// @brief 创建 3D 长方体网格（转发 → RenderServer::MeshCreateBox）
  /// @copydoc RenderingServer::MeshCreateBox
  RID MeshCreateBox(const char* p_name, float p_width, float p_height,
                    float p_depth) override;

  /// @brief 创建 3D 球体网格（转发 → RenderServer::MeshCreateSphere）
  /// @copydoc RenderingServer::MeshCreateSphere
  RID MeshCreateSphere(const char* p_name, float p_radius, uint32 p_sectors,
                       uint32 p_rings) override;

  /// @brief 从 FreeType 加载 SDF 字体（转发 →
  /// RenderServer::FontLoadFromFreeType）
  /// @copydoc RenderingServer::FontCreateFromFreeType
  RID FontCreateFromFreeType(const char* p_name, const char* p_font_path,
                             float p_font_size, const uint32* p_charcodes,
                             uint32 p_charcode_count,
                             float p_sdf_spread = 8.0f) override;

  /// @brief 写材质 Uniform（转发 → RenderServer::MaterialSetUniform）
  /// @copydoc RenderingServer::MaterialSetParam
  Error MaterialSetParam(RID p_material, uint32 p_offset, const void* p_data,
                         uint32 p_size) override;

  /// @brief 绑定材质纹理槽位（转发 → RenderServer::MaterialSetTexture）
  /// @copydoc RenderingServer::MaterialSetTexture
  Error MaterialSetTexture(RID p_material, uint32 p_slot,
                           RID p_texture) override;

  /// @brief 释放纹理（转发 → RenderServer::TextureFree）
  /// @copydoc RenderingServer::TextureFree
  void TextureFree(RID p_texture) override;

  /// @brief 释放材质（转发 → RenderServer::MaterialFree）
  /// @copydoc RenderingServer::MaterialFree
  void MaterialFree(RID p_material) override;

  /// @brief 释放网格（转发 → RenderServer::MeshFree）
  /// @copydoc RenderingServer::MeshFree
  void MeshFree(RID p_mesh) override;

  /// @brief 释放字体（转发 → RenderServer::FontFree）
  /// @copydoc RenderingServer::FontFree
  void FontFree(RID p_font) override;

  /// @brief 统一回收任意 RID（S1 落地；S0 空实现）
  /// @copydoc RenderingServer::FreeRid
  void FreeRid(RID p_rid) override;

  // ─────────────────────────────────────────────────────────────────
  //  RenderingServer — 2D item（转发 → RenderServer::CanvasItem*）
  // ─────────────────────────────────────────────────────────────────

  /// @brief 创建 2D Canvas item（转发 → RenderServer::CanvasItemCreate）
  /// @copydoc RenderingServer::CanvasItemCreate
  RID CanvasItemCreate(uint32 p_visibility_mask = 0xFFFFFFFF) override;

  /// @brief 设置 item 材质（转发 → RenderServer::CanvasItemSetMaterial）
  /// @copydoc RenderingServer::CanvasItemSetMaterial
  void CanvasItemSetMaterial(RID p_item, RID p_material) override;

  /// @brief 设置 item 网格（转发 → RenderServer::CanvasItemSetMesh）
  /// @copydoc RenderingServer::CanvasItemSetMesh
  void CanvasItemSetMesh(RID p_item, RID p_mesh) override;

  /// @brief 设置 item 局部变换（转发 → RenderServer::CanvasItemSetTransform）
  /// @copydoc RenderingServer::CanvasItemSetTransform
  void CanvasItemSetTransform(RID p_item, const float p_transform[16]) override;

  /// @brief 设置 item 不透明度（转发 → RenderServer::CanvasItemSetOpacity）
  /// @copydoc RenderingServer::CanvasItemSetOpacity
  void CanvasItemSetOpacity(RID p_item, float p_opacity) override;

  /// @brief 设置 item 绘制序（转发 → RenderServer::CanvasItemSetZOrder）
  /// @copydoc RenderingServer::CanvasItemSetZOrder
  void CanvasItemSetZOrder(RID p_item, int32 p_z_order) override;

  /// @brief 设置 item 图层（转发 → RenderServer::CanvasItemSetLayer）
  /// @copydoc RenderingServer::CanvasItemSetLayer
  void CanvasItemSetLayer(RID p_item, int32 p_layer) override;

  /// @brief 设置 item 局部可见性（转发 → RenderServer::CanvasItemSetVisible）
  /// @copydoc RenderingServer::CanvasItemSetVisible
  void CanvasItemSetVisible(RID p_item, bool p_visible) override;

  /// @brief 设置 item 父节点（转发 → RenderServer::CanvasItemSetParent）
  /// @copydoc RenderingServer::CanvasItemSetParent
  void CanvasItemSetParent(RID p_item, RID p_parent) override;

  /// @brief 设置 z_order 是否相对父（转发 →
  /// RenderServer::CanvasItemSetZAsRelative）
  /// @copydoc RenderingServer::CanvasItemSetZAsRelative
  void CanvasItemSetZAsRelative(RID p_item, bool p_relative) override;

  /// @brief 释放 2D item（转发 → RenderServer::CanvasItemFree）
  /// @copydoc RenderingServer::CanvasItemFree
  void CanvasItemFree(RID p_item) override;

  // ─────────────────────────────────────────────────────────────────
  //  RenderingServer — 3D item（转发 → RenderServer::SpatialItem*）
  // ─────────────────────────────────────────────────────────────────

  /// @brief 创建 3D Instance item（转发 → RenderServer::SpatialItemCreate）
  /// @copydoc RenderingServer::InstanceCreate
  RID InstanceCreate(uint32 p_visibility_mask = 0xFFFFFFFF) override;

  /// @brief 设置 Instance 材质（转发 → RenderServer::SpatialItemSetMaterial）
  /// @copydoc RenderingServer::InstanceSetMaterial
  void InstanceSetMaterial(RID p_item, RID p_material) override;

  /// @brief 设置 Instance 网格（转发 → RenderServer::SpatialItemSetMesh）
  /// @copydoc RenderingServer::InstanceSetMesh
  void InstanceSetMesh(RID p_item, RID p_mesh) override;

  /// @brief 设置 Instance 局部变换（转发 →
  /// RenderServer::SpatialItemSetTransform）
  /// @copydoc RenderingServer::InstanceSetTransform
  void InstanceSetTransform(RID p_item, const float p_transform[16]) override;

  /// @brief 设置渲染优先级（转发 → RenderServer::SpatialItemSetRenderPriority）
  /// @copydoc RenderingServer::InstanceSetRenderPriority
  void InstanceSetRenderPriority(RID p_item, uint8 p_priority) override;

  /// @brief 设置是否禁用视锥剔除（转发 →
  /// RenderServer::SpatialItemSetDisableCull）
  /// @copydoc RenderingServer::InstanceSetDisableCull
  void InstanceSetDisableCull(RID p_item, bool p_disable) override;

  /// @brief 设置实例父节点（转发 → RenderServer::SpatialItemSetParent）
  /// @copydoc RenderingServer::InstanceSetParent
  void InstanceSetParent(RID p_item, RID p_parent) override;

  /// @brief 释放 3D Instance（转发 → RenderServer::SpatialItemFree）
  /// @copydoc RenderingServer::InstanceFree
  void InstanceFree(RID p_item) override;

  // ─────────────────────────────────────────────────────────────────
  //  RenderingServer — 文本 item（转发 → RenderServer::TextItem*）
  // ─────────────────────────────────────────────────────────────────

  /// @brief 创建文本 item（转发 → RenderServer::TextItemCreate）
  /// @copydoc RenderingServer::TextItemCreate
  RID TextItemCreate(uint32 p_visibility_mask = 0xFFFFFFFF) override;

  /// @brief 设置文本字体（转发 → RenderServer::TextItemSetFont）
  /// @copydoc RenderingServer::TextItemSetFont
  void TextItemSetFont(RID p_item, RID p_font) override;

  /// @brief 设置文本内容（转发 → RenderServer::TextItemSetText）
  /// @copydoc RenderingServer::TextItemSetText
  void TextItemSetText(RID p_item, const uint32* p_text,
                       uint32 p_count) override;

  /// @brief 设置文本锚点位置（转发 → RenderServer::TextItemSetPosition）
  /// @copydoc RenderingServer::TextItemSetPosition
  void TextItemSetPosition(RID p_item, float p_x, float p_y) override;

  /// @brief 设置文本颜色（转发 → RenderServer::TextItemSetColor）
  /// @copydoc RenderingServer::TextItemSetColor
  void TextItemSetColor(RID p_item, const float p_color[4]) override;

  /// @brief 设置文本绘制序（转发 → RenderServer::TextItemSetZOrder）
  /// @copydoc RenderingServer::TextItemSetZOrder
  void TextItemSetZOrder(RID p_item, int32 p_z_order) override;

  /// @brief 设置文本图层（转发 → RenderServer::TextItemSetLayer）
  /// @copydoc RenderingServer::TextItemSetLayer
  void TextItemSetLayer(RID p_item, int32 p_layer) override;

  /// @brief 设置文本局部可见性（转发 → RenderServer::TextItemSetVisible）
  /// @copydoc RenderingServer::TextItemSetVisible
  void TextItemSetVisible(RID p_item, bool p_visible) override;

  /// @brief 设置文本父节点（转发 → RenderServer::TextItemSetParent）
  /// @copydoc RenderingServer::TextItemSetParent
  void TextItemSetParent(RID p_item, RID p_parent) override;

  /// @brief 设置 z_order 是否相对父（转发 →
  /// RenderServer::TextItemSetZAsRelative）
  /// @copydoc RenderingServer::TextItemSetZAsRelative
  void TextItemSetZAsRelative(RID p_item, bool p_relative) override;

  /// @brief 释放文本 item（转发 → RenderServer::TextItemFree）
  /// @copydoc RenderingServer::TextItemFree
  void TextItemFree(RID p_item) override;

  // ─────────────────────────────────────────────────────────────────
  //  RenderingServer — 相机 / 灯光（转发 →
  //  RenderServer::Camera*/DirectionalLight*）
  // ─────────────────────────────────────────────────────────────────

  /// @brief 设置 3D 相机（转发 → RenderServer::Camera3DSet）
  /// @copydoc RenderingServer::Camera3DSet
  void Camera3DSet(uint32 p_viewport, const CameraMatrix& p_camera) override;

  /// @brief 设置 2D 相机（转发 → RenderServer::Camera2DSet）
  /// @copydoc RenderingServer::Camera2DSet
  void Camera2DSet(uint32 p_viewport, const Transform2D& p_camera) override;

  /// @brief 设置方向光（转发 → RenderServer::DirectionalLightSet）
  /// @copydoc RenderingServer::DirectionalLightSet
  void DirectionalLightSet(uint32 p_viewport,
                           const DirectionalLight& p_light) override;

  /// @brief 清除方向光（转发 → RenderServer::DirectionalLightClear）
  /// @copydoc RenderingServer::DirectionalLightClear
  void DirectionalLightClear(uint32 p_viewport) override;

  // ─────────────────────────────────────────────────────────────────
  //  RenderingServer — 视口（S6 落地；S0 空实现）
  // ─────────────────────────────────────────────────────────────────

  /// @brief 创建视口（S6 落地；S0 返回空 RID）
  /// @copydoc RenderingServer::ViewportCreate
  RID ViewportCreate(uint32 p_width, uint32 p_height) override;

  /// @brief 销毁视口（S6 落地；S0 空实现）
  /// @copydoc RenderingServer::ViewportDestroy
  void ViewportDestroy(RID p_viewport) override;

  /// @brief 调整视口尺寸（S6 落地；S0 空实现）
  /// @copydoc RenderingServer::ViewportSetSize
  void ViewportSetSize(RID p_viewport, uint32 p_width,
                       uint32 p_height) override;

  /// @brief 设置视口是否参与渲染（S6 落地；S0 空实现）
  /// @copydoc RenderingServer::ViewportSetActive
  void ViewportSetActive(RID p_viewport, bool p_active) override;

  // ─────────────────────────────────────────────────────────────────
  //  RenderingServer — 帧
  // ─────────────────────────────────────────────────────────────────

  /// @brief 帧提交信号（转发 → RenderServer::SubmitScene）
  /// @copydoc RenderingServer::SubmitScene
  void SubmitScene() override;

 private:
  RenderServer render_server_;  ///< S0 被委托的旧实现（S1+ 逐步替换）
  CommandQueue* queue_ =
      nullptr;  ///< 命令队列（不拥有；供 IsOnRenderThread 查询）
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDERING_SERVER_DEFAULT_H_
