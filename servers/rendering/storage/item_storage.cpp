/**
 * @file item_storage.cpp
 * @brief ItemStorage 实现
 *
 * 实现约定：
 *   - **线性扫描**：所有 FindItem 均为 O(n)。HUD 场景 item 数十级，
 *     线性表比 HashMap 简单且缓存友好，规模不敏感。
 *   - **无效 RID 静默容错**：Set* 与 Free 找不到目标 item 时不报错、
 *     不中断；Set* 仍会路由屏位（防丢帧），Free 则直接返回。
 *   - **无序删除**：Free 使用 RemoveAtUnordered（尾部元素补位），
 *     不保持列表顺序；列表语义是"集合"而非"序列"，渲染读取按索引
 *     遍历与顺序无关（渲染顺序由 item 的 z_order / layer / priority 决定）。
 *   - **dirty 门控（单一屏位掩码）**：除个别只改 CPU 拷贝的路径
 *     （见方法注释）外，任何状态变更都会路由屏级掩码
 *     （item 变更按 visibility_mask；相机/灯光变更已随 ADR-0007 迁至视口，
 *     由调用方经 MarkScreen 按屏索引路由）；mask≠0 即"有变更"（IsDirty），
 *     渲染循环逐屏查询 IsScreenDirty（未触及的屏跳过），全部屏消费完后统一
 *     ClearDirty（清零掩码）。
 *
 * 调用方约定：
 *   - 写入（Create 与 Set 与 Free）与读取（GetByIndex）都在渲染线程；
 *   - 主线程仅经 RenderServer（短路或命令队列）间接调用。
 *
 * @author yameng.he
 * @version 1.2
 * @date 2026-08-22
 * @date 2026-10-02 注释对齐 ADR-0007；dirty 合并为单一屏位掩码（删 dirty_ 与
 * SetDirty）
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "item_storage.h"

#include <type_traits>

#include "logger.h"

namespace arhud {

// -- Per-screen dirty routing (B3-M0.5) --

/**
 * @brief 将 p_screen_mask 覆盖的屏标记为脏
 *
 * @param[in] p_screen_mask 屏位掩码（item 的 visibility_mask 或单屏位）
 * @note 无独立全局门、无 SetDirty——本函数即置 dirty（mask≠0）。
 *       p_screen_mask==0 时不置位。
 */
void ItemStorage::MarkScreens(uint32 p_screen_mask) {
  dirty_screen_mask_.fetch_or(p_screen_mask, std::memory_order_release);
}

/**
 * @brief 将单个屏标记为脏
 *
 * @param[in] p_screen 屏索引（per-viewport 状态变更/命令路由的目标屏）
 * @note p_screen ≥ 32（超出 uint32 位宽）时退化为全屏置位——HUD 实际
 *       屏数 1~3，此分支仅为防御。
 */
void ItemStorage::MarkScreen(uint32 p_screen) {
  if (p_screen >= 32) {
    MarkScreens(kAllScreensDirty);
    return;
  }
  MarkScreens(1u << p_screen);
}

// ADR-0008: 外部重绘请求 → 强制全屏重渲（渲染线程消费引擎 force_redraw_
// 时调用）
void ItemStorage::MarkAllScreens() {
  MarkScreens(kAllScreensDirty);
}

// -- 私有模板 helper（声明在 item_storage.h；定义于本 TU——
//    调用点全部在本 cpp 内，隐式实例化即可，无需跨 TU 可见）--

template <typename T>
T* ItemStorage::FindItem(LocalVector<RIDPair<T>>& p_list, RID p_rid) {
  for (uint32 i = 0; i < p_list.Size(); ++i) {
    if (p_list[i].first == p_rid)
      return &p_list[i].second;
  }
  return nullptr;
}

template <typename T>
const T* ItemStorage::FindItem(const LocalVector<RIDPair<T>>& p_list,
                               RID p_rid) const {
  for (uint32 i = 0; i < p_list.Size(); ++i) {
    if (p_list[i].first == p_rid)
      return &p_list[i].second;
  }
  return nullptr;
}

template <typename T>
T* ItemStorage::FindItemAndMark(LocalVector<RIDPair<T>>& p_list, RID p_rid) {
  T* item = FindItem(p_list, p_rid);
  MarkScreens(item != nullptr ? item->visibility_mask : kAllScreensDirty);
  return item;
}

// -- Canvas items --

/**
 * @brief 创建 2D item
 *
 * 分配一个新 RID，向 canvas_items_ 尾部追加零值状态条目。
 *
 * @return 新 item 的唯一 RID
 * @post canvas_items_ 增加一项；dirty 置位
 */
RID ItemStorage::ItemAllocate() {
  // 任意线程：原子自增 RID，仅分配标识不登记状态（RID 分裂，§7.11 F）
  return AllocateRID();
}

void ItemStorage::CanvasItemSetTransformBlob(RID p_item,
                                             const uint8* p_transform_blob) {
  CanvasItemSetTransform(p_item,
                         reinterpret_cast<const float*>(p_transform_blob));
}

void ItemStorage::SpatialItemSetTransformBlob(RID p_item,
                                              const uint8* p_transform_blob) {
  SpatialItemSetTransform(p_item,
                          reinterpret_cast<const float*>(p_transform_blob));
}

void ItemStorage::TextItemSetColorBlob(RID p_item, const uint8* p_color_blob) {
  TextItemSetColor(p_item, reinterpret_cast<const float*>(p_color_blob));
}

void ItemStorage::TextItemSetTextBlob(RID p_item, uint32 p_count,
                                      const uint8* p_text_blob) {
  TextItemSetText(p_item, reinterpret_cast<const uint32*>(p_text_blob),
                  p_count);
}

void ItemStorage::CanvasItemInitialize(RID p_item, uint32 p_visibility_mask) {
  CanvasItemData data{};
  data.visibility_mask =
      p_visibility_mask;  // 创建即带归属（默认全屏，行为兼容）
  canvas_items_.PushBack({p_item, data});
  MarkScreens(
      p_visibility_mask);  // 只标该 item 实际归属的屏（窄掩码创建不再标全屏）
}

RID ItemStorage::CanvasItemCreate(uint32 p_visibility_mask) {
  RID rid = AllocateRID();
  CanvasItemInitialize(rid, p_visibility_mask);
  return rid;
}

/**
 * @brief 查询 Canvas item 是否登记在表
 *
 * @param[in] p_rid item RID
 * @return true 存在；false 不存在（未创建或已 Free）
 */
bool ItemStorage::CanvasItemIsValid(RID p_rid) const {
  return FindItem(canvas_items_, p_rid) != nullptr;
}

/**
 * @brief 绑定材质 RID
 *
 * @param[in] p_item item RID
 * @param[in] p_material 材质 RID（MaterialStorage，kCanvas 类型）
 * @note 无效 RID 静默忽略；仍会置位 dirty 防丢帧
 */
void ItemStorage::CanvasItemSetMaterial(RID p_item, RID p_material) {
  if (auto* d = FindItemAndMark(canvas_items_, p_item)) {
    d->material = p_material;
  }
}

/**
 * @brief 绑定网格 RID
 *
 * @param[in] p_item item RID
 * @param[in] p_mesh 网格 RID（MeshStorage，通常为 quad）
 */
void ItemStorage::CanvasItemSetMesh(RID p_item, RID p_mesh) {
  if (auto* d = FindItemAndMark(canvas_items_, p_item)) {
    d->mesh = p_mesh;
  }
}

/**
 * @brief 设置 4x4 列主序变换
 *
 * @param[in] p_item item RID
 * @param[in] p_transform 16 个 float 列主序矩阵（先缩放/旋转后平移）
 */
void ItemStorage::CanvasItemSetTransform(RID p_item,
                                         const float p_transform[16]) {
  if (auto* d = FindItemAndMark(canvas_items_, p_item)) {
    for (int i = 0; i < 16; ++i) {
      d->transform[i] = p_transform[i];
    }
    MarkCanvasSubtree(p_item);  // ADR-0005: 传播性变更——子孙所在屏一并标脏
  }
}

/**
 * @brief 设置整体透明度
 *
 * @param[in] p_item item RID
 * @param[in] p_opacity 0.0 ~ 1.0（经顶点色 alpha 传入着色器）
 */
void ItemStorage::CanvasItemSetOpacity(RID p_item, float p_opacity) {
  if (auto* d = FindItemAndMark(canvas_items_, p_item)) {
    d->opacity = p_opacity;
    MarkCanvasSubtree(p_item);  // ADR-0005: opacity 沿链连乘——子树屏一并标脏
  }
}

/**
 * @brief 设置同 layer 内绘制次序
 *
 * @param[in] p_item item RID
 * @param[in] p_z_order 越大越靠上（后绘制）
 */
void ItemStorage::CanvasItemSetZOrder(RID p_item, int32 p_z_order) {
  if (auto* d = FindItemAndMark(canvas_items_, p_item)) {
    d->z_order = p_z_order;
    MarkCanvasSubtree(
        p_item);  // ADR-0005: z_as_relative 子孙有效 z 随之变——子树屏标脏
  }
}

/**
 * @brief 设置图层 ID
 *
 * @param[in] p_item item RID
 * @param[in] p_layer 图层编号（RenderViewport 按 layer 分组分批）
 */
void ItemStorage::CanvasItemSetLayer(RID p_item, int32 p_layer) {
  if (auto* d = FindItemAndMark(canvas_items_, p_item)) {
    d->layer = p_layer;
  }
}

/**
 * @brief 显示或隐藏
 *
 * @param[in] p_item item RID
 * @param[in] p_visible false 时渲染器跳过该 item
 */
void ItemStorage::CanvasItemSetVisible(RID p_item, bool p_visible) {
  if (auto* d = FindItemAndMark(canvas_items_, p_item)) {
    d->visible = p_visible;
    MarkCanvasSubtree(p_item);  // ADR-0005: 可见性沿链 AND——子树屏一并标脏
  }
}

/**
 * @brief 释放 Canvas item
 *
 * @param[in] p_item item RID
 * @post 该项被 RemoveAtUnordered 移除；后续同一 RID 查询失效
 * @note 不置 dirty：渲染列表按当前表重建，删除无需触发重渲
 * @note ADR-0005: Free 不级联——Canvas 子与 Text 子自动摘到根（WARN）
 */
void ItemStorage::CanvasItemFree(RID p_item) {
  for (uint32 i = 0; i < canvas_items_.Size(); ++i) {
    if (canvas_items_[i].first == p_item) {
      canvas_items_.RemoveAtUnordered(i);
      DetachCanvasChildren(p_item);
      return;
    }
  }
}

// -- Spatial items --

/**
 * @brief 创建 3D item
 *
 * @return 新 item 的唯一 RID
 * @post spatial_items_ 增加一项；dirty 置位
 */
void ItemStorage::SpatialItemInitialize(RID p_item, uint32 p_visibility_mask) {
  SpatialItemData data{};
  data.visibility_mask =
      p_visibility_mask;  // 创建即带归属（默认全屏，行为兼容）
  spatial_items_.PushBack({p_item, data});
  MarkScreens(p_visibility_mask);
}

RID ItemStorage::SpatialItemCreate(uint32 p_visibility_mask) {
  RID rid = AllocateRID();
  SpatialItemInitialize(rid, p_visibility_mask);
  return rid;
}

/**
 * @brief 查询 Spatial item 是否登记在表
 *
 * @param[in] p_rid item RID
 * @return true 存在；false 不存在
 */
bool ItemStorage::SpatialItemIsValid(RID p_rid) const {
  return FindItem(spatial_items_, p_rid) != nullptr;
}

/**
 * @brief 绑定材质 RID
 *
 * @param[in] p_item item RID
 * @param[in] p_material 材质 RID（必须为 MaterialDataType::kSpatial）
 */
void ItemStorage::SpatialItemSetMaterial(RID p_item, RID p_material) {
  if (auto* d = FindItemAndMark(spatial_items_, p_item)) {
    d->material = p_material;
  }
}

/**
 * @brief 绑定网格 RID
 *
 * @param[in] p_item item RID
 * @param[in] p_mesh 网格 RID（MeshStorage）
 */
void ItemStorage::SpatialItemSetMesh(RID p_item, RID p_mesh) {
  if (auto* d = FindItemAndMark(spatial_items_, p_item)) {
    d->mesh = p_mesh;
  }
}

/**
 * @brief 设置 4x4 世界矩阵
 *
 * @param[in] p_item item RID
 * @param[in] p_transform 列主序世界矩阵
 */
void ItemStorage::SpatialItemSetTransform(RID p_item,
                                          const float p_transform[16]) {
  if (auto* d = FindItemAndMark(spatial_items_, p_item)) {
    for (int i = 0; i < 16; ++i) {
      d->transform[i] = p_transform[i];
    }
    MarkSpatialSubtree(p_item);  // ADR-0005: 传播性变更——子孙所在屏一并标脏
  }
}

/**
 * @brief 设置渲染优先级
 *
 * @param[in] p_item item RID
 * @param[in] p_priority 越小越先画（配合材质 priority 参与排序）
 */
void ItemStorage::SpatialItemSetRenderPriority(RID p_item, uint8 p_priority) {
  if (auto* d = FindItemAndMark(spatial_items_, p_item)) {
    d->render_priority = p_priority;
  }
}

/**
 * @brief 开关视锥剔除
 *
 * @param[in] p_item item RID
 * @param[in] p_disable true = 不剔除（HUD 叠加元素常用）
 */
void ItemStorage::SpatialItemSetDisableCull(RID p_item, bool p_disable) {
  if (auto* d = FindItemAndMark(spatial_items_, p_item)) {
    d->disable_cull = p_disable;
  }
}

/**
 * @brief 释放 Spatial item
 *
 * @param[in] p_item item RID
 * @note 不置 dirty（同 CanvasItemFree 理由）
 * @note ADR-0005: Free 不级联——Spatial 子自动摘到根（WARN）
 */
void ItemStorage::SpatialItemFree(RID p_item) {
  for (uint32 i = 0; i < spatial_items_.Size(); ++i) {
    if (spatial_items_[i].first == p_item) {
      spatial_items_.RemoveAtUnordered(i);
      DetachSpatialChildren(p_item);
      return;
    }
  }
}

// -- Text items --

/**
 * @brief 创建文字 item
 *
 * @return 新 item 的唯一 RID
 * @post text_items_ 增加一项；dirty 置位
 */
void ItemStorage::TextItemInitialize(RID p_item, uint32 p_visibility_mask) {
  TextItemData data{};
  data.visibility_mask =
      p_visibility_mask;  // 创建即带归属（默认全屏，行为兼容）
  text_items_.PushBack({p_item, data});
  MarkScreens(p_visibility_mask);
}

RID ItemStorage::TextItemCreate(uint32 p_visibility_mask) {
  RID rid = AllocateRID();
  TextItemInitialize(rid, p_visibility_mask);
  return rid;
}

/**
 * @brief 查询 Text item 是否登记在表
 *
 * @param[in] p_rid item RID
 * @return true 存在；false 不存在
 */
bool ItemStorage::TextItemIsValid(RID p_rid) const {
  return FindItem(text_items_, p_rid) != nullptr;
}

/**
 * @brief 绑定字体 RID
 *
 * @param[in] p_item item RID
 * @param[in] p_font 字体 RID（FontStorage，SDF 图集）
 */
void ItemStorage::TextItemSetFont(RID p_item, RID p_font) {
  if (auto* d = FindItemAndMark(text_items_, p_item)) {
    d->font = p_font;
  }
}

/**
 * @brief 设置文本内容（UTF-32 码点）
 *
 * 将 p_text 深拷贝进 item 的 LocalVector<uint32>。
 *
 * @param[in] p_item item RID
 * @param[in] p_text UTF-32 码点缓冲区（长度 p_count）
 * @param[in] p_count 码点个数（不含终止符）
 * @pre p_text 指向至少 p_count 个 uint32
 * @note 仅改 CPU 侧码点缓存，未置 dirty——渲染时按当前 text
 * 重建字形批次，无需行数门控触发
 */
void ItemStorage::TextItemSetText(RID p_item, const uint32* p_text,
                                  uint32 p_count) {
  if (auto* d = FindItem(text_items_, p_item)) {
    d->text.Clear();
    d->text.Reserve(p_count);
    for (uint32 i = 0; i < p_count; ++i) {
      d->text.PushBack(p_text[i]);
    }
  }
}

/**
 * @brief 设置基线位置
 *
 * @param[in] p_item item RID
 * @param[in] p_x 基线起点 X（视口像素坐标）
 * @param[in] p_y 基线起点 Y（视口像素坐标）
 */
void ItemStorage::TextItemSetPosition(RID p_item, float p_x, float p_y) {
  if (auto* d = FindItemAndMark(text_items_, p_item)) {
    d->x = p_x;
    d->y = p_y;
  }
}

/**
 * @brief 设置文字颜色
 *
 * @param[in] p_item item RID
 * @param[in] p_color RGBA 四元组
 */
void ItemStorage::TextItemSetColor(RID p_item, const float p_color[4]) {
  if (auto* d = FindItemAndMark(text_items_, p_item)) {
    for (int i = 0; i < 4; ++i) {
      d->color[i] = p_color[i];
    }
  }
}

/**
 * @brief 设置同 layer 内绘制次序
 *
 * @param[in] p_item item RID
 * @param[in] p_z_order 越大越靠上
 */
void ItemStorage::TextItemSetZOrder(RID p_item, int32 p_z_order) {
  if (auto* d = FindItemAndMark(text_items_, p_item)) {
    d->z_order = p_z_order;
  }
}

/**
 * @brief 设置图层 ID
 *
 * @param[in] p_item item RID
 * @param[in] p_layer 图层编号
 */
void ItemStorage::TextItemSetLayer(RID p_item, int32 p_layer) {
  if (auto* d = FindItemAndMark(text_items_, p_item)) {
    d->layer = p_layer;
  }
}

/**
 * @brief 显示或隐藏
 *
 * @param[in] p_item item RID
 * @param[in] p_visible false 时渲染器跳过
 */
void ItemStorage::TextItemSetVisible(RID p_item, bool p_visible) {
  if (auto* d = FindItemAndMark(text_items_, p_item)) {
    d->visible = p_visible;
  }
}

/**
 * @brief 释放 Text item
 *
 * @param[in] p_item item RID
 * @note 不置 dirty（同前）
 */
void ItemStorage::TextItemFree(RID p_item) {
  for (uint32 i = 0; i < text_items_.Size(); ++i) {
    if (text_items_[i].first == p_item) {
      text_items_.RemoveAtUnordered(i);
      return;
    }
  }
}

// -- Camera / Light 已迁出（ADR-0007）--
// per-viewport 相机/灯光现由 RenderViewport 自持（ViewportSceneState）；
// 相关命令由渲染线程经绑定 thunk 执行，并经 ItemStorage::MarkScreen 置屏
// dirty。

// -- Visibility mask (B3-M0.4) --

/**
 * @brief 设置 Canvas item 的屏可见性掩码
 *
 * @param[in] p_item item RID
 * @param[in] p_mask 掩码（渲染时与 RenderViewport cull_mask 做 AND，非零才画）
 */
void ItemStorage::CanvasItemSetVisibilityMask(RID p_item, uint32 p_mask) {
  // 旧掩码屏由 FindItemAndMark 路由（旧屏失去该 item）；
  // 命中后补路由新掩码屏（新屏获得该 item）——两端都要重渲
  if (CanvasItemData* data = FindItemAndMark(canvas_items_, p_item)) {
    MarkScreens(p_mask);
    data->visibility_mask = p_mask;
  }
}

/**
 * @brief 设置 Spatial item 的屏可见性掩码
 *
 * @param[in] p_item item RID
 * @param[in] p_mask 掩码
 */
void ItemStorage::SpatialItemSetVisibilityMask(RID p_item, uint32 p_mask) {
  // 旧掩码屏由 FindItemAndMark 路由（旧屏失去该 item）；
  // 命中后补路由新掩码屏（新屏获得该 item）——两端都要重渲
  if (SpatialItemData* data = FindItemAndMark(spatial_items_, p_item)) {
    MarkScreens(p_mask);
    data->visibility_mask = p_mask;
  }
}

/**
 * @brief 设置 Text item 的屏可见性掩码
 *
 * @param[in] p_item item RID
 * @param[in] p_mask 掩码
 */
void ItemStorage::TextItemSetVisibilityMask(RID p_item, uint32 p_mask) {
  // 旧掩码屏由 FindItemAndMark 路由（旧屏失去该 item）；
  // 命中后补路由新掩码屏（新屏获得该 item）——两端都要重渲
  if (TextItemData* data = FindItemAndMark(text_items_, p_item)) {
    MarkScreens(p_mask);
    data->visibility_mask = p_mask;
  }
}

// -- Item hierarchy (ADR-0005: 树形管理) --

/**
 * @brief 沿 Canvas 父链向上收集（自链底到链顶）
 *
 * 防御语义：父 RID 在表中不存在（断链）→ 停止收集，链截断于该处
 * （该子树视断链点之下的部分为根）；深度达 kMaxTreeDepth → 停止
 * （SetParent 已拒绝环，此为兜底，不死循环）。
 */
uint32 ItemStorage::CollectCanvasChain(RID p_item,
                                       const CanvasItemData** p_chain) const {
  const CanvasItemData* cur = FindItem(canvas_items_, p_item);
  if (cur == nullptr) {
    return 0;
  }
  uint32 depth = 0;
  p_chain[depth++] = cur;
  while (depth < kMaxTreeDepth && cur->parent.IsValid()) {
    const CanvasItemData* parent = FindItem(canvas_items_, cur->parent);
    if (parent == nullptr) {
      break;  // 断链防御：视当前节点为根
    }
    p_chain[depth++] = parent;
    cur = parent;
  }
  return depth;
}

/**
 * @brief 自链顶（根）向链底（自身）合成有效状态
 *
 * - transform：world = parent_world × local（列主序，先父后子右乘）
 * - opacity：全员连乘（对齐 Godot modulate）
 * - z：自顶向下逐级 z_eff(c) = c.z_as_relative ? z_eff(parent) + c.z : c.z
 * - visible：全员 AND
 */
void ItemStorage::ComposeCanvasChain(const CanvasItemData* const* p_chain,
                                     uint32 p_depth,
                                     CanvasItemEffectiveState& p_out) {
  const CanvasItemData* root = p_chain[p_depth - 1];
  Mat4 world;
  for (int i = 0; i < 16; ++i) {
    world.elements[i] = root->transform[i];
  }
  float opacity = root->opacity;
  int32 z = root->z_order;
  bool visible = root->visible;

  for (uint32 i = p_depth - 1; i-- > 0;) {
    const CanvasItemData& c = *p_chain[i];
    Mat4 local;
    for (int e = 0; e < 16; ++e) {
      local.elements[e] = c.transform[e];
    }
    world = world * local;
    opacity *= c.opacity;
    z = c.z_as_relative ? z + c.z_order : c.z_order;
    visible = visible && c.visible;
  }

  for (int e = 0; e < 16; ++e) {
    p_out.transform[e] = world.elements[e];
  }
  p_out.opacity = opacity;
  p_out.z_order = z;
  p_out.visible = visible;
}

/**
 * @brief 判断 p_target 是否在 p_start 的祖先链上（含 p_start 自身）
 *
 * 自 p_start 向上走 parent 链；断链（查无此 RID）即 false；
 * 迭代上限 kMaxTreeDepth（环兜底）。
 */
template <typename T>
bool ItemStorage::ChainContains(const LocalVector<RIDPair<T>>& p_list,
                                RID p_start, RID p_target) const {
  RID cur = p_start;
  uint32 guard = 0;
  while (cur.IsValid() && guard++ < kMaxTreeDepth) {
    if (cur == p_target) {
      return true;
    }
    const T* node = FindItem(p_list, cur);
    if (node == nullptr) {
      return false;  // 断链防御
    }
    cur = node->parent;
  }
  return false;
}

/**
 * @brief 收集 p_root 的 Canvas 子树 visibility_mask 并集（含自身与 Text 后代）
 *
 * 传播性变更的路由依据：子孙（含挂 Canvas 的 Text）可能落在父项不可见
 * 的屏上，仅标记父项自身 mask 会漏屏（陈旧帧）。
 */
uint32 ItemStorage::CollectCanvasSubtreeMask(RID p_root) const {
  uint32 mask = 0;
  for (uint32 i = 0; i < canvas_items_.Size(); ++i) {
    if (ChainContains(canvas_items_, canvas_items_[i].first, p_root)) {
      mask |= canvas_items_[i].second.visibility_mask;
    }
  }
  // Text 后代：父链（Canvas 侧）经过 p_root 即属于该子树
  for (uint32 i = 0; i < text_items_.Size(); ++i) {
    if (text_items_[i].second.parent.IsValid() &&
        ChainContains(canvas_items_, text_items_[i].second.parent, p_root)) {
      mask |= text_items_[i].second.visibility_mask;
    }
  }
  return mask;
}

uint32 ItemStorage::CollectSpatialSubtreeMask(RID p_root) const {
  uint32 mask = 0;
  for (uint32 i = 0; i < spatial_items_.Size(); ++i) {
    if (ChainContains(spatial_items_, spatial_items_[i].first, p_root)) {
      mask |= spatial_items_[i].second.visibility_mask;
    }
  }
  return mask;
}

void ItemStorage::MarkCanvasSubtree(RID p_root) {
  MarkScreens(CollectCanvasSubtreeMask(p_root));
}

void ItemStorage::MarkSpatialSubtree(RID p_root) {
  MarkScreens(CollectSpatialSubtreeMask(p_root));
}

/**
 * @brief SetParent 公共实现（模板：三类 item 共用逻辑）
 *
 * 防御序：① item 无效 → 静默容错 + 全屏置位（Set* 惯例：防丢帧）；
 * ② parent != kInvalid 但表中查不到 → WARN + 视为根；③ 自挂接/环
 * （沿 p_parent 祖先链走，遇 p_item）→ WARN + 保持旧父。
 */
template <typename T>
void ItemStorage::SetParentImpl(LocalVector<RIDPair<T>>& p_list, RID p_item,
                                RID p_parent) {
  T* item = FindItem(p_list, p_item);
  if (item == nullptr) {
    // 惯例对齐（文件头）：Set* 无效 RID 仍置 dirty 防丢帧
    MarkScreens(kAllScreensDirty);
    return;
  }
  if (!p_parent.IsValid()) {
    item->parent = RID();  // 摘到根
  } else if constexpr (std::is_same_v<T, TextItemData>) {
    // Text 父恒为 Canvas item（TextItemSetParent 已校验存在性）；
    // Text 是叶子且父链只指向 Canvas 表，结构上不可能成环 → 直接挂接
    item->parent = p_parent;
  } else {
    T* parent = FindItem(p_list, p_parent);
    if (parent == nullptr) {
      ARHUD_LOG_WARN(
          "ItemStorage::SetParent: parent RID %llu not found, treat as root",
          static_cast<unsigned long long>(p_parent.GetId()));
      item->parent = RID();
    } else if (p_parent == p_item) {
      ARHUD_LOG_WARN("ItemStorage::SetParent: self-parent rejected (RID %llu)",
                     static_cast<unsigned long long>(p_item.GetId()));
    } else {
      // 环检测：沿 p_parent 祖先链走，遇 p_item 即成环
      bool cyclic = false;
      RID ancestor = p_parent;
      while (ancestor.IsValid() && !cyclic) {
        const T* node = FindItem(p_list, ancestor);
        if (node == nullptr) {
          break;  // 断链，不可能成环
        }
        if (ancestor == p_item) {
          cyclic = true;
          break;
        }
        ancestor = node->parent;
      }
      if (cyclic) {
        ARHUD_LOG_WARN(
            "ItemStorage::SetParent: cycle rejected (item %llu under parent "
            "%llu)",
            static_cast<unsigned long long>(p_item.GetId()),
            static_cast<unsigned long long>(p_parent.GetId()));
      } else {
        item->parent = p_parent;
      }
    }
  }
  MarkScreens(item->visibility_mask);
}

void ItemStorage::CanvasItemSetParent(RID p_item, RID p_parent) {
  SetParentImpl(canvas_items_, p_item, p_parent);
  MarkCanvasSubtree(
      p_item);  // ADR-0005: 挂接/摘除改变整棵子树有效状态——子树屏标脏
}

void ItemStorage::TextItemSetParent(RID p_item, RID p_parent) {
  // 跨类型校验：Text 的父必须是 Canvas item（文字叶子挂 2D 面板）
  if (p_parent.IsValid() && FindItem(canvas_items_, p_parent) == nullptr) {
    ARHUD_LOG_WARN(
        "ItemStorage::TextItemSetParent: parent RID %llu is not a Canvas item, "
        "treat as root",
        static_cast<unsigned long long>(p_parent.GetId()));
    p_parent = RID();
  }
  SetParentImpl(text_items_, p_item, p_parent);
}

void ItemStorage::SpatialItemSetParent(RID p_item, RID p_parent) {
  SetParentImpl(spatial_items_, p_item, p_parent);
  MarkSpatialSubtree(
      p_item);  // ADR-0005: 挂接/摘除改变整棵子树有效状态——子树屏标脏
}

void ItemStorage::CanvasItemSetZAsRelative(RID p_item, bool p_relative) {
  if (auto* d = FindItemAndMark(canvas_items_, p_item)) {
    d->z_as_relative = p_relative;
    MarkCanvasSubtree(
        p_item);  // ADR-0005: z_as_relative 改变自身及子孙有效 z——子树屏标脏
  }
}

void ItemStorage::TextItemSetZAsRelative(RID p_item, bool p_relative) {
  if (auto* d = FindItemAndMark(text_items_, p_item)) {
    d->z_as_relative = p_relative;
  }
}

bool ItemStorage::CanvasItemGetEffective(
    RID p_item, CanvasItemEffectiveState& p_out) const {
  const CanvasItemData* chain[kMaxTreeDepth];
  const uint32 depth = CollectCanvasChain(p_item, chain);
  if (depth == 0) {
    return false;
  }
  ComposeCanvasChain(chain, depth, p_out);
  return true;
}

bool ItemStorage::TextItemGetEffective(RID p_item,
                                       TextItemEffectiveState& p_out) const {
  const TextItemData* self = FindItem(text_items_, p_item);
  if (self == nullptr) {
    return false;
  }

  bool visible = self->visible;
  int32 z = self->z_order;
  float alpha = self->color[3];
  float x = self->x;
  float y = self->y;

  if (self->parent.IsValid()) {
    // 父链有效状态：位置经父链矩阵变换、alpha 连乘、z 累加、可见性 AND
    CanvasItemEffectiveState parent_eff;
    if (CanvasItemGetEffective(self->parent, parent_eff)) {
      const Vec4 local_pos(self->x, self->y, 0.0f, 1.0f);
      Mat4 parent_mat;
      for (int e = 0; e < 16; ++e) {
        parent_mat.elements[e] = parent_eff.transform[e];
      }
      const Vec4 world_pos = parent_mat * local_pos;
      x = world_pos.x;
      y = world_pos.y;
      alpha *= parent_eff.opacity;
      if (self->z_as_relative) {
        z = parent_eff.z_order + self->z_order;
      }
      visible = visible && parent_eff.visible;
    } else {
      ARHUD_LOG_WARN(
          "ItemStorage::TextItemGetEffective: parent chain broken (text %llu), "
          "use own state",
          static_cast<unsigned long long>(p_item.GetId()));
    }
  }

  p_out.x = x;
  p_out.y = y;
  p_out.alpha = alpha;
  p_out.z_order = z;
  p_out.visible = visible;
  return true;
}

bool ItemStorage::SpatialItemGetEffectiveTransform(RID p_item,
                                                   float p_out[16]) const {
  // Spatial 链收集 + 合成（仅 transform；Spatial 无 opacity/visible/z 字段）
  const SpatialItemData* cur = FindItem(spatial_items_, p_item);
  if (cur == nullptr) {
    return false;
  }
  const SpatialItemData* chain[kMaxTreeDepth];
  uint32 depth = 0;
  chain[depth++] = cur;
  while (depth < kMaxTreeDepth && cur->parent.IsValid()) {
    const SpatialItemData* parent = FindItem(spatial_items_, cur->parent);
    if (parent == nullptr) {
      break;  // 断链防御
    }
    chain[depth++] = parent;
    cur = parent;
  }

  Mat4 world;
  for (int e = 0; e < 16; ++e) {
    world.elements[e] = chain[depth - 1]->transform[e];
  }
  for (uint32 i = depth - 1; i-- > 0;) {
    Mat4 local;
    for (int e = 0; e < 16; ++e) {
      local.elements[e] = chain[i]->transform[e];
    }
    world = world * local;
  }
  for (int e = 0; e < 16; ++e) {
    p_out[e] = world.elements[e];
  }
  return true;
}

/**
 * @brief 摘除 Canvas 孤儿（Canvas 释放后调用）
 *
 * Canvas 子与 Text 子都摘到根（Text 可挂 Canvas）；WARN 记录
 * 每个被摘除的子 item。
 */
void ItemStorage::DetachCanvasChildren(RID p_freed) {
  for (uint32 i = 0; i < canvas_items_.Size(); ++i) {
    if (canvas_items_[i].second.parent == p_freed) {
      ARHUD_LOG_WARN(
          "ItemStorage: canvas child %llu detached to root (parent %llu freed)",
          static_cast<unsigned long long>(canvas_items_[i].first.GetId()),
          static_cast<unsigned long long>(p_freed.GetId()));
      canvas_items_[i].second.parent = RID();
    }
  }
  for (uint32 i = 0; i < text_items_.Size(); ++i) {
    if (text_items_[i].second.parent == p_freed) {
      ARHUD_LOG_WARN(
          "ItemStorage: text child %llu detached to root (canvas parent %llu "
          "freed)",
          static_cast<unsigned long long>(text_items_[i].first.GetId()),
          static_cast<unsigned long long>(p_freed.GetId()));
      text_items_[i].second.parent = RID();
    }
  }
}

void ItemStorage::DetachSpatialChildren(RID p_freed) {
  for (uint32 i = 0; i < spatial_items_.Size(); ++i) {
    if (spatial_items_[i].second.parent == p_freed) {
      ARHUD_LOG_WARN(
          "ItemStorage: spatial child %llu detached to root (parent %llu "
          "freed)",
          static_cast<unsigned long long>(spatial_items_[i].first.GetId()),
          static_cast<unsigned long long>(p_freed.GetId()));
      spatial_items_[i].second.parent = RID();
    }
  }
}

// -- Clear --

/**
 * @brief 清空所有 items
 *
 * @post 三类 item 表全部清空；dirty 置位
 * @note 调用后下一帧将渲染空场景；适用于场景整体复位或退出
 */
void ItemStorage::ClearAll() {
  MarkScreens(kAllScreensDirty);  // 整表清空：全屏重渲
  canvas_items_.Clear();
  spatial_items_.Clear();
  text_items_.Clear();
}

}  // namespace arhud
