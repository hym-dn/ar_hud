/**
 * @file item_storage.h
 * @brief ItemStorage -- 渲染线程保留模式的绘制单元状态存储（retained mode）
 *
 * 维护 Canvas/Spatial/Text 三类绘制单元（Item）的 per-item 状态。与
 * TextureStorage / MaterialStorage / MeshStorage 等"GPU 资源存储"不同，
 * ItemStorage **不管理任何 GPU 资源**——它只持有 CPU 侧的纯状态数据
 * （RID 列表 + 状态结构体），GPU 资源的创建/释放仍由各资源 Storage 负责。
 * per-screen 相机/灯光已按 **ADR-0007** 迁出，改由 ViewportSceneState
 * （随 RenderViewport 自持）；本类退化为纯 item 账本 + dirty 门。
 *
 * @par 定位（区别于其他 Storage）
 *   | Storage          | 管理对象           | 是否创建 GPU 资源 | 持有者 |
 *   |------------------|--------------------|-------------------|---------------------|
 *   | TextureStorage   | 纹理 RID           | ✅（RD Texture）  |
 * ARHudCompositor     | | MaterialStorage  | 材质 RID           | ✅（UBO/Set）
 * | ARHudCompositor     | | MeshStorage      | 网格 RID           |
 * ✅（VBO/IBO）     | ARHudCompositor     | | ShaderStorage    | 着色器 RID |
 * ✅（program）     | ARHudCompositor     | | FontStorage      | 字体 RID |
 * ✅（SDF 图集）    | ARHudCompositor     | | **ItemStorage**  |
 * **Item（相机/灯光已迁出→ViewportSceneState）** | **❌ 纯状态** |
 * **ARHudEngine** |
 *
 * @par 为什么不放进 ARHudCompositor（与五个资源 Storage 保持一致的表面诱惑）
 *   1. **装配依赖分野**：Compositor::Initialize(&rd) 七步装配链全员依赖 RD
 *      （各 Storage 创建 GPU 对象）；ItemStorage 纯 CPU 状态，零 RD 依赖、
 *      无 Initialize——合并即制造假依赖
 *   2. **可测性**：tests/ 裸构造 `ItemStorage storage;` 即可单测（无需 GL
 * 上下文）； 进 Compositor 则需先建 RCD→RD→GL 上下文，单测退化成集成测试
 *   3. **消费方跨越渲染子系统**：RenderServer（RS 前端）、绑定命令 thunk
 *      （命令链）、RenderViewport（渲染收集）三方读写；Compositor 是被
 *      RenderViewport 调用的渲染子系统——账本不属于它
 *   4. **dirty 门控是调度概念**：单一屏位掩码（非零 = 有变更）服务 Engine 级
 *      渲染调度（哪些屏跳过管线），发生在渲染子系统被调用之前
 *   Godot 同构：RendererCompositor 持 renderer 侧 storages，场景状态在 RS 层。
 *   代价仅为 EngineStackRefs 多一个 item_storage 指针。
 *
 * @par 数据流（主线程 → 命令队列 → 渲染线程 → 渲染器）
 *   @code
 *   主线程（RenderServer 前端）
 *     │  CanvasItemSetTransform / TextItemSetText / ...
 *     ▼
 *   CommandQueue ──PushBound（ADR-0010）──► 绑定 thunk
 *                                                        │  调
 * ItemStorage::SetXxx 渲染线程（B3 起与消费同线程）                        ▼
 *   RenderViewport::RenderFromStorage() ──►
 * ItemStorage::GetXxxCount/GetXxxByIndex │ ▼ CanvasRenderer /
 * SpatialRenderer（构建 render list、渲染）
 *   @endcode
 *
 * @par 线程模型（B3 渲染线程化之后）
 *   - ItemStorage **渲染线程独占**：绑定命令 thunk（写入）与
 *     RenderViewport（读取）均运行在渲染线程，读写同线程，无锁。
 *   - RenderServer 的"短路路径"（CanBypassQueue）绕过命令队列直写：
 *     触发条件为 未绑定队列/合成器（单线程、单测形态）、队列无消费者
 *     （渲染线程未启动）、调用者即渲染线程、或队列关停中。
 *   - B2 过渡期使用 PushAndSync（同步命令），保证主线程读取时
 *     渲染线程已执行完毕（无竞态）；B3 起读写同线程，item 命令
 *     改为异步 Push + SubmitScene 统一信号。
 *
 * @par RID 方案（相对 RIDOwner 的简化）
 *   未使用 core 层 RIDOwner<T>，而是自维护单调递增的 next_rid_：
 *   - Item 的 RID 是自增 uint64（AllocateRID），**永不复用**，
 *     故无需世代（generation）校验——旧 RID 在线性表中查不到即失效；
 *   - 数据量级 HUD 数十个 item，线性表 + 线性查找（O(n)）足够，
 *     避免 HashMap 迭代器复杂度，换取缓存友好与按索引遍历能力。
 *
 * @par dirty 位（内容更新率控制；单一屏位掩码，方案 C）
 *   只有一个状态 dirty_screen_mask_，兼任"是否有变更"（非零即有）与
 *   "哪些屏要重渲"（各屏位）：任意一次状态变更都路由屏位，mask≠0 即
 *   IsDirty()==true。**非传播性变更**（material/mesh/layer/visibility_mask
 *   等仅影响自身的字段）按 item 自身的 visibility_mask 置位；**传播性变更**
 *   （transform/opacity/visible/z/parent——ADR-0005 起影响整棵子树的有效状态）
 *   按**子树掩码并集**置位（子孙可能落在父项不可见的屏上）。相机/灯光变更
 *   （ADR-0007 起由 RenderViewport 自持，经 MarkScreen 路由）同样落屏位。
 *   **Mark* 是唯一置位入口**（无独立全局门、无提交步）：任何状态变更路径
 *   都必须至少 Mark* 一个屏位，否则该变更不会被渲染（mask 保持 0 → 全屏跳过）。
 *   RenderThreadFunc 在逐屏渲染完成后统一 ClearDirty（清零掩码）。
 *   各屏经 IsScreenDirty 查询自己的位——mask==0 时
 *   全部屏跳过，静态 HUD 每帧 GPU/CPU 成本≈0；mask≠0 但本屏位未置的屏同样
 *   跳过（屏 A 的 item 动了，屏 B 不再陪跑）。
 *   atomic 仅为防御性安全（实际读写同线程）。
 *
 * @author yameng.he
 * @version 1.2
 * @date 2026-08-22
 * @date 2026-10-02 注释对齐 ADR-0005/0006/0007；dirty 合并为单一屏位掩码（删
 * dirty_ 与 SetDirty）
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_STORAGE_ITEM_STORAGE_H_
#define ARHUD_SERVERS_RENDERING_STORAGE_ITEM_STORAGE_H_

#include <atomic>

#include "math/camera_matrix.h"
#include "math/math_funcs.h"
#include "template/local_vector.h"
#include "template/rid.h"
#include "typedefs.h"

namespace arhud {

// DirectionalLight 已迁出（ADR-0007）——见 storage/viewport_scene_state.h：
// per-viewport 场景级状态（相机/灯光）随 RenderViewport 自持。

/**
 * @brief 2D Canvas 绘制单元的服务端状态（保留模式）
 *
 * 描述一个 2D 四边形/图元如何被绘制：材质（UBO+纹理）、网格、
 * 变换、透明度、绘制顺序与可见性。渲染时由 CanvasRenderer 收集
 * 成 HUDCanvasLayer 批次。
 *
 * @note 不持有 GPU 资源；材质/网格/纹理均为 RID 引用。
 */
struct CanvasItemData {
  RID material;  ///< 材质 RID（MaterialStorage，决定着色/纹理）
  RID mesh;      ///< 网格 RID（MeshStorage，决定几何，通常为 quad）
  float transform[16] = {
      1, 0, 0, 0, 0, 1, 0, 0,
      0, 0, 1, 0, 0, 0, 0, 1};  ///< 4x4 列主序变换（先缩放旋转后平移）
  float opacity = 1.0f;         ///< 整体透明度（经顶点色 alpha 通道传入着色器）
  int32 z_order = 0;            ///< 同 layer 内的绘制次序（越大越靠上）
  int32 layer = 0;              ///< 图层 ID（分层绘制，不同 layer 间深度隔离）
  bool visible = true;          ///< 是否参与渲染
  uint32 visibility_mask =
      0xFFFFFFFF;  ///< B3-M0.4: 屏可见性掩码（与 RenderViewport cull_mask AND）
  RID parent = RID();  ///< ADR-0005: 父 Canvas item（kInvalid =
                       ///< 根；变换/可见性/透明度沿树传播）
  bool z_as_relative =
      true;  ///< ADR-0005: z_order 是否相对父链累加（对齐 Godot z_as_relative）
};

/**
 * @brief 3D Spatial 绘制单元的服务端状态（保留模式）
 *
 * 描述一个 3D 物体如何被绘制：材质、网格、世界变换、渲染优先级
 * 与剔除开关。渲染时由 SpatialRenderer 收集进 render list 并排序。
 *
 * @note 3D 材质可携带 priority / next_pass（SpatialMaterialData），
 *       item 层的 render_priority 是 item 自身的排序权重。
 */
struct SpatialItemData {
  RID material;  ///< 材质 RID（必须为 MaterialDataType::kSpatial）
  RID mesh;      ///< 网格 RID（MeshStorage，box/sphere 等）
  float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                         0, 0, 1, 0, 0, 0, 0, 1};  ///< 4x4 列主序世界矩阵
  uint8 render_priority =
      0;  ///< 渲染优先级（越小越先画；front-to-back/back-to-front 排序）
  bool disable_cull = false;            ///< 关闭视锥剔除（HUD 元素常用）
  uint32 visibility_mask = 0xFFFFFFFF;  ///< B3-M0.4: 屏可见性掩码
  RID parent =
      RID();  ///< ADR-0005: 父 Spatial item（kInvalid = 根；世界矩阵沿树连乘）
};

/**
 * @brief SDF 文字绘制单元的服务端状态（保留模式）
 *
 * 描述一行文字如何被绘制：字体（SDF 图集）、UTF-32 码点串、
 * 基线位置、颜色与绘制顺序。渲染时由 CanvasRenderer 的文本管线
 * 收集成 TextDrawInfo 批次。
 *
 * @note 码点以 uint32 存储（UTF-32，对齐 ustring），由
 *       TextItemSetText 从外部拷贝进入。
 */
struct TextItemData {
  RID font;                  ///< 字体 RID（FontStorage，SDF 图集 + glyph 度量）
  LocalVector<uint32> text;  ///< UTF-32 码点序列（无终止符）
  float x = 0.0f;            ///< 基线起点 X（视口像素坐标，屏幕空间）
  float y = 0.0f;            ///< 基线起点 Y（视口像素坐标，屏幕空间）
  float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  ///< 文字颜色（RGBA）
  int32 z_order = 0;                          ///< 同 layer 内的绘制次序
  int32 layer = 0;                            ///< 图层 ID
  bool visible = true;                        ///< 是否参与渲染
  uint32 visibility_mask = 0xFFFFFFFF;        ///< B3-M0.4: 屏可见性掩码
  RID parent =
      RID();  ///< ADR-0005: 父 Canvas item（文字叶子挂 2D 面板；kInvalid = 根）
  bool z_as_relative = true;  ///< ADR-0005: z_order 是否相对父链累加
};

/**
 * @brief 线性存储节点：RID → 状态数据 的平铺对
 *
 * ItemStorage 内部三个成员列表共用此节点类型：
 *   - first  = 外部可见的 item RID（AllocateRID 分配）
 *   - second = 对应类型的状态结构体
 * 使用 LocalVector 平铺存储，缓存友好且支持按索引顺序遍历。
 *
 * @tparam T 状态数据类型（CanvasItemData / SpatialItemData / TextItemData）
 */
template <typename T>
struct RIDPair {
  RID first;  ///< item 标识（RID）
  T second;   ///< item 状态（POD/值语义）
};

/**
 * @brief 保留模式的绘制单元（Item）状态存储
 *
 * ItemStorage 是 "渲染线程保留模式" 的核心状态库：它把主线程
 * 一帧一帧提交的绘制意图（item 创建 → Set 参数），沉淀成服务端
 * 持续存活的 item 状态；渲染时渲染器从这里收集当前帧要画的全部
 * 内容。应用代码（demo）只与 RenderServer 交互，不直接触碰本类。
 *
 * @par 核心职责
 *   1. **Item 生命周期** — Create/Free 三类 item，分配/回收 RID
 *   2. **Item 状态维护** — Set* 系列逐个字段更新（材质/网格/变换/顺序/可见性）
 *   3. **屏级 dirty 路由** — MarkScreen / Mark*Subtree
 * 向调度层报告"哪些屏需重渲" （相机/灯光状态已按 ADR-0007 迁至
 * ViewportSceneState，其变更仍经此门）
 *   4. **渲染输入** — 向渲染器提供 count + by-index 迭代读取
 *   5. **dirty 门控** — 状态变更置位 + 屏级路由，静态帧/未触及屏零成本跳过
 *
 * @par 装配关系（不拥有，指针注入）
 *   | 消费方             | 持有方式                           | 访问时机      |
 *   |--------------------|------------------------------------|---------------|
 *   | ARHudEngine        | **值语义持有**（arhud_engine.cpp）  | 生命周期 | |
 * RenderServer       | 裸指针（短路路径主线程直写）        | 主线程 API    | |
 * 绑定命令 thunk     | 裸指针（命令执行，正规写入路径）     | 渲染线程      |
 *   | RenderViewport       | 裸指针（RenderFromStorage 读取）     | 渲染线程 |
 *
 * @par 线程安全约束（B3 之后）
 *   | 方法组           | 操作       | 线程约束                 |
 *   |------------------|------------|--------------------------|
 *   | Create/Set/Free  | 修改列表   | 🔴 渲染线程（命令执行）  |
 *   | MarkScreen       | 路由屏位   | 🔴 渲染线程              |
 *   | Get* / Count*      | 只读       | 🔴 渲染线程（同线程）    |
 *   读写同线程 ⇒ 无需锁；dirty 的 atomic 仅为防御。
 *
 * @par 与 Godot 的对应
 *   Godot 4.6 中 "服务端保存场景状态、渲染线程消费" 的形态由
 *   RenderingServer 的 retained 数据与 Rasterizer 的 InstanceData
 *   承担；ARHud 因裁剪掉场景树（Phase 6 预留），把"要画什么"
 *   浓缩为三类 item 的扁平表（相机/灯光已随 ADR-0007 移至视口），即本类的定位。
 *
 * @par 典型使用流程（demo 视角）
 *   @code
 *   // 主线程（RenderServer 前端）
 *   RID item = render_server->CanvasItemCreate();
 *   render_server->CanvasItemSetMaterial(item, mat_rid);
 *   render_server->CanvasItemSetMesh(item, quad_rid);
 *   render_server->CanvasItemSetTransform(item, identity);
 *   render_server->SubmitScene();   // 命令队列冲刷 + 渲染信号
 *
 *   // 渲染线程（内部）
 *   绑定 thunk → ItemStorage::CanvasItemSetMaterial(...)  // 写入
 *   RenderViewport::RenderFromStorage() → GetCanvasItemByIndex(i) // 读取
 *   @endcode
 *
 * @see RenderServer（主线程前端 API）
 * @see 绑定命令（渲染线程执行）
 * @see RenderViewport（渲染编排，RenderFromStorage）
 * @see TypeErasedStorage 系列（resource_storage.h 的 GPU 资源存储）
 */
class ItemStorage {
 public:
  ItemStorage() = default;
  ~ItemStorage() = default;

  ARHUD_DISABLE_COPY_MOVE(ItemStorage);

  // ─────────────────────────────────────────────────────────────
  //  Canvas items（2D 绘制单元，见 CanvasItemData）
  // ─────────────────────────────────────────────────────────────

  /// @brief 创建 2D item，返回新分配的唯一 RID
  /// @param[in] p_visibility_mask
  /// 创建即带的屏可见性掩码（默认全屏，向后兼容）。
  ///            窄屏创建时直接传掩码，避免"全屏创建→收窄旧掩码触发全屏标脏"的绕路
  RID CanvasItemCreate(uint32 p_visibility_mask = 0xFFFFFFFF);
  /// @brief 查询 RID 是否存在且有效
  bool CanvasItemIsValid(RID p_rid) const;
  /// @brief 绑定材质 RID（决定着色器/UBO/纹理）
  void CanvasItemSetMaterial(RID p_item, RID p_material);
  /// @brief 绑定网格 RID（决定几何形状）
  void CanvasItemSetMesh(RID p_item, RID p_mesh);
  /// @brief 设置 4x4 列主序变换矩阵
  void CanvasItemSetTransform(RID p_item, const float p_transform[16]);
  /// @brief 设置整体透明度（0~1，顶点色 alpha）
  void CanvasItemSetOpacity(RID p_item, float p_opacity);
  /// @brief 设置同 layer 内绘制次序（越大越靠上）
  void CanvasItemSetZOrder(RID p_item, int32 p_z_order);
  /// @brief 设置图层 ID（分层绘制）
  void CanvasItemSetLayer(RID p_item, int32 p_layer);
  /// @brief 显示/隐藏
  void CanvasItemSetVisible(RID p_item, bool p_visible);
  /// @brief B3-M0.4: 设置屏可见性掩码（与屏 cull_mask 做 AND 过滤）
  void CanvasItemSetVisibilityMask(RID p_item, uint32 p_mask);
  /// @brief 释放 item（线性表无序删除）
  void CanvasItemFree(RID p_item);

  // ─────────────────────────────────────────────────────────────
  //  Spatial items（3D 绘制单元，见 SpatialItemData）
  // ─────────────────────────────────────────────────────────────

  /// @brief 创建 3D item，返回新分配的唯一 RID
  /// @param[in] p_visibility_mask 创建即带的屏可见性掩码（默认全屏）
  RID SpatialItemCreate(uint32 p_visibility_mask = 0xFFFFFFFF);
  /// @brief 查询 RID 是否存在且有效
  bool SpatialItemIsValid(RID p_rid) const;
  /// @brief 绑定材质 RID（必须为 kSpatial 类型）
  void SpatialItemSetMaterial(RID p_item, RID p_material);
  /// @brief 绑定网格 RID
  void SpatialItemSetMesh(RID p_item, RID p_mesh);
  /// @brief 设置 4x4 世界矩阵
  void SpatialItemSetTransform(RID p_item, const float p_transform[16]);
  /// @brief 设置渲染优先级（越小越先画，排序用）
  void SpatialItemSetRenderPriority(RID p_item, uint8 p_priority);
  /// @brief 关闭视锥剔除（HUD 元素）
  void SpatialItemSetDisableCull(RID p_item, bool p_disable);
  /// @brief B3-M0.4: 设置屏可见性掩码
  void SpatialItemSetVisibilityMask(RID p_item, uint32 p_mask);
  /// @brief 释放 item（线性表无序删除）
  void SpatialItemFree(RID p_item);

  // ─────────────────────────────────────────────────────────────
  //  Text items（SDF 文字，见 TextItemData）
  // ─────────────────────────────────────────────────────────────

  /// @brief 创建文字 item，返回新分配的唯一 RID
  /// @param[in] p_visibility_mask 创建即带的屏可见性掩码（默认全屏）
  RID TextItemCreate(uint32 p_visibility_mask = 0xFFFFFFFF);

  /**
   * @brief RID 分裂分配（任意线程）：原子自增一个不复用的 item RID，不登记状态
   * @note 与 CanvasItemInitialize / SpatialItemInitialize / TextItemInitialize
   * 配对
   */
  RID ItemAllocate();

  /**
   * @brief RID 分裂初始化：渲染线程登记 Canvas item 状态并置脏
   * @param[in] p_item           ItemAllocate() 返回的 RID
   * @param[in] p_visibility_mask 归属屏掩码
   */
  void CanvasItemInitialize(RID p_item, uint32 p_visibility_mask);

  /**
   * @brief RID 分裂初始化：渲染线程登记 Spatial item 状态并置脏
   */
  void SpatialItemInitialize(RID p_item, uint32 p_visibility_mask);

  /**
   * @brief RID 分裂初始化：渲染线程登记 Text item 状态并置脏
   */
  void TextItemInitialize(RID p_item, uint32 p_visibility_mask);
  /// @brief 查询 RID 是否存在且有效
  bool TextItemIsValid(RID p_rid) const;
  /// @brief 绑定字体 RID（FontStorage）
  void TextItemSetFont(RID p_item, RID p_font);
  /// @brief 设置 UTF-32 码点文本（从 p_text 深拷贝）
  void TextItemSetText(RID p_item, const uint32* p_text, uint32 p_count);
  /// @brief 设置基线位置（视口像素坐标）
  void TextItemSetPosition(RID p_item, float p_x, float p_y);
  /// @brief 设置文字颜色（RGBA）
  void TextItemSetColor(RID p_item, const float p_color[4]);

  /**
   * @brief 命令队列变体：变换/颜色/文本经内联 blob 跨线程传递（blob-last，§7.11
   * E）
   *
   * 与对应指针版语义等价，供 `CommandQueue::PushBoundBlob` 绑定调用；
   * blob 内为原始字节（16×float / 4×float / count×uint32）。
   */
  void CanvasItemSetTransformBlob(RID p_item, const uint8* p_transform_blob);
  void SpatialItemSetTransformBlob(RID p_item, const uint8* p_transform_blob);
  void TextItemSetColorBlob(RID p_item, const uint8* p_color_blob);
  void TextItemSetTextBlob(RID p_item, uint32 p_count,
                           const uint8* p_text_blob);
  /// @brief 设置同 layer 内绘制次序
  void TextItemSetZOrder(RID p_item, int32 p_z_order);
  /// @brief 设置图层 ID
  void TextItemSetLayer(RID p_item, int32 p_layer);
  /// @brief 显示/隐藏
  void TextItemSetVisible(RID p_item, bool p_visible);
  /// @brief B3-M0.4: 设置屏可见性掩码
  void TextItemSetVisibilityMask(RID p_item, uint32 p_mask);
  /// @brief 释放 item（线性表无序删除；子 item 自动摘到根，ADR-0005）
  void TextItemFree(RID p_item);

  // ─────────────────────────────────────────────────────────────
  //  Item 层级（树形管理，ADR-0005：对齐 Godot RS canvas item 层级）
  //
  //  三类 item 经 parent 字段构成森林；渲染收集时逐 item 沿链计算
  //  有效状态（transform 连乘 / visible AND / opacity 连乘 / z 累加）。
  //  约定：
  //   - CanvasItem 父 = CanvasItem；TextItem 父 = CanvasItem（文字叶子）；
  //     SpatialItem 父 = SpatialItem；跨类型挂接 → WARN + 视为根
  //   - SetParent 环检测（沿目标祖先链走，遇自身拒绝）
  //   - Free 不级联：子 item 自动摘到根（对齐 RS 语义）
  //   - layer / visibility_mask 不继承（per-item）
  // ─────────────────────────────────────────────────────────────

  /// @brief Canvas item 的有效状态（沿父链计算后的渲染输入）
  struct CanvasItemEffectiveState {
    float transform[16] = {
        1, 0, 0, 0, 0, 1, 0, 0,
        0, 0, 1, 0, 0, 0, 0, 1};  ///< 全局变换（祖先链连乘 × 本地）
    float opacity = 1.0f;         ///< 有效透明度（祖先链连乘 × 本地）
    int32 z_order = 0;            ///< 有效 z（z_as_relative 时沿链累加）
    bool visible = true;          ///< 树内可见性（自身 AND 全部祖先）
  };

  /// @brief Text item 的有效状态（父链为 Canvas，位置经链矩阵变换）
  struct TextItemEffectiveState {
    float x = 0.0f;       ///< 基线 X（父链矩阵作用于本地 (x,y) 后）
    float y = 0.0f;       ///< 基线 Y
    float alpha = 1.0f;   ///< 有效 alpha（父链 opacity 连乘 × 本地 color[3]）
    int32 z_order = 0;    ///< 有效 z（z_as_relative 时沿链累加）
    bool visible = true;  ///< 树内可见性（自身 AND 全部祖先）
  };

  /// @brief 挂接 Canvas item 到父 Canvas item（kInvalid = 摘到根）
  /// @note 环/无效父 RID 防御：拒绝时保持旧父并
  /// WARN，不返回错误（异步命令无返回通道）
  void CanvasItemSetParent(RID p_item, RID p_parent);
  /// @brief 设置 Canvas item 的 z 是否相对父链累加（对齐 Godot
  /// canvas_item_set_z_as_relative）
  void CanvasItemSetZAsRelative(RID p_item, bool p_relative);
  /// @brief 计算 Canvas item 的有效状态（沿父链向上收集、自根向下合成）
  /// @param[in] p_item item RID
  /// @param[out] p_out 有效状态（transform/opacity/z/visible）
  /// @return false = item RID 无效（防御；p_out 内容未定义）
  bool CanvasItemGetEffective(RID p_item,
                              CanvasItemEffectiveState& p_out) const;

  /// @brief 挂接 Text item 到父 Canvas item（文字叶子；kInvalid = 摘到根）
  void TextItemSetParent(RID p_item, RID p_parent);
  /// @brief 设置 Text item 的 z 是否相对父链累加
  void TextItemSetZAsRelative(RID p_item, bool p_relative);
  /// @brief 计算 Text item 的有效状态（位置经父链矩阵变换、alpha 沿链连乘）
  bool TextItemGetEffective(RID p_item, TextItemEffectiveState& p_out) const;

  /// @brief 挂接 Spatial item 到父 Spatial item（kInvalid = 摘到根）
  void SpatialItemSetParent(RID p_item, RID p_parent);
  /// @brief 计算 Spatial item 的有效世界矩阵（祖先链连乘 × 本地）
  /// @return false = item RID 无效
  bool SpatialItemGetEffectiveTransform(RID p_item, float p_out[16]) const;

  // ─────────────────────────────────────────────────────────────
  //  屏级 dirty 路由（ADR-0007）
  //
  //  相机/灯光已迁入 RenderViewport（ViewportSceneState）；其变更由
  //  绑定命令在写入视口状态后调用 MarkScreen 置对应屏 dirty——
  //  dirty 门仍是唯一渲染调度门，item 变更与视图状态变更都经它。
  // ─────────────────────────────────────────────────────────────

  /**
   * @brief 将单个屏标记为脏（per-viewport 状态变更后用）
   *
   * @param[in] p_screen 屏索引；≥32 时退化为全屏置位（防御）
   * @note 只路由屏位；无独立全局门、无 SetDirty——mask≠0 即"有变更"。
   */
  void MarkScreen(uint32 p_screen);

  /**
   * @brief 标记所有屏为脏（外部重绘请求强制全屏重渲；ADR-0008）
   *
   * 等价 `MarkScreens(kAllScreensDirty)`。供渲染线程消费引擎级
   * `force_redraw_` 标志时调用（窗口 WM_PAINT/WM_SIZE 触发的重绘请求）。
   *
   * @note 仍需在渲染线程调用（ItemStorage 独占契约）；主线程只置标志。
   */
  void MarkAllScreens();

  // ─────────────────────────────────────────────────────────────
  //  Render-thread query（渲染器构建 render list 的只读接口）
  // ─────────────────────────────────────────────────────────────

  /// @brief 当前 Canvas item 数量（配合 GetCanvasItemByIndex 遍历）
  uint32 GetCanvasItemCount() const {
    return canvas_items_.Size();
  }
  /// @brief 按索引读取第 i 个 Canvas item（仅渲染线程）
  const CanvasItemData& GetCanvasItemByIndex(uint32 p_index) const {
    return canvas_items_[p_index].second;
  }
  /// @brief 当前 Spatial item 数量
  uint32 GetSpatialItemCount() const {
    return spatial_items_.Size();
  }
  /// @brief 按索引读取第 i 个 Spatial item（仅渲染线程）
  const SpatialItemData& GetSpatialItemByIndex(uint32 p_index) const {
    return spatial_items_[p_index].second;
  }
  /// @brief 当前 Text item 数量
  uint32 GetTextItemCount() const {
    return text_items_.Size();
  }
  /// @brief 按索引读取第 i 个 Text item（仅渲染线程）
  const TextItemData& GetTextItemByIndex(uint32 p_index) const {
    return text_items_[p_index].second;
  }

  // ADR-0005: 按索引取 RID（有效状态 getter 以 RID 为键，渲染收集同时需要
  // 原始数据[visibility_mask 等]与 RID，二者配合使用）
  /// @brief 按索引读取第 i 个 Canvas item 的 RID
  RID GetCanvasItemRid(uint32 p_index) const {
    return canvas_items_[p_index].first;
  }
  /// @brief 按索引读取第 i 个 Spatial item 的 RID
  RID GetSpatialItemRid(uint32 p_index) const {
    return spatial_items_[p_index].first;
  }
  /// @brief 按索引读取第 i 个 Text item 的 RID
  RID GetTextItemRid(uint32 p_index) const {
    return text_items_[p_index].first;
  }

  /// @brief 清空全部 items（置 dirty，触发下一帧重渲）
  void ClearAll();

  // ─────────────────────────────────────────────────────────────
  //  dirty 路由（内容更新率控制，§3.7 of multiscreen；单一屏位掩码）
  // ─────────────────────────────────────────────────────────────
  //   dirty_screen_mask_ 是唯一的 dirty 状态，兼任两职：
  //   - "本轮是否要渲"：mask != 0（IsDirty）；
  //   - "哪些屏要渲"：各屏位（IsScreenDirty）。
  //   **Mark* 是唯一的置位入口**（无独立全局门、无 SetDirty 提交步）：
  //   - item 变更按其 visibility_mask 置位（FindItemAndMark 共用入口，
  //     无效 RID 全屏置位防丢帧）；传播性变更按子树掩码并集；
  //   - 相机/灯光变更（ADR-0007 迁至视口后）由写入方调 MarkScreen 按
  //     屏索引置位。
  //   契约：任何状态变更路径**必须**至少 Mark* 一个屏位，否则该变更
  //   不会被渲染（mask 保持 0 → 全屏跳过）。
  //   RenderThreadFunc 逐屏消费：某屏 RenderAndSubmit() 真正渲完才
  //   ClearScreenDirty(i)；背压跳过的屏保留脏位待下轮 submit 重试
  //   （旧的全局 ClearDirty 会连跳过屏一起清——首帧更新偶发丢失）。
  //   mask==0 的帧全部屏跳过渲染——静态 HUD GPU/CPU 成本 ≈ 0；
  //   mask!=0 但本屏位未置的屏同样跳过——屏间不陪跑。
  //   atomic：置位在渲染线程（命令执行），读取在同一线程循环，无跨
  //   线程；atomic 仅为防御性安全。
  // ─────────────────────────────────────────────────────────────

  /// @brief 本轮是否有内容变更（false = 静态帧，可跳过渲染；即屏位掩码非零）
  bool IsDirty() const {
    return dirty_screen_mask_.load(std::memory_order_acquire) != 0;
  }

  /// @brief 清理屏级路由掩码（渲染循环每轮渲染完成后调用；清零即"无变更"）
  void ClearDirty() {
    dirty_screen_mask_.store(0, std::memory_order_release);
  }

  /// @brief 清理单屏脏位（渲染线程在该屏本轮真正渲完并提交后调用）
  /// @param[in] p_screen 屏索引（ViewportID，0-based；≥32 为 no-op）
  /// @note 逐屏消费：backpressure 跳过的屏保留本位，下一轮 submit 重试，
  ///       不会被"全局 ClearDirty"吞掉更新（旧全局清零曾致首帧丢失竞态）。
  void ClearScreenDirty(uint32 p_screen) {
    if (p_screen >= 32) {
      return;
    }
    dirty_screen_mask_.fetch_and(~(1u << p_screen), std::memory_order_release);
  }

  /// @brief 某屏本轮是否有内容变更（false = 该屏静态，可跳过渲染）
  /// @param[in] p_screen 屏索引（ViewportID，0-based）
  /// @return true 该屏有变更需渲染；false 可跳过
  /// @note mask==0 表示无变更（含被 ClearDirty 后），全部屏跳过；
  ///       p_screen ≥ 32（超出 uint32 位宽）保守返回 true。
  bool IsScreenDirty(uint32 p_screen) const {
    const uint32 mask = dirty_screen_mask_.load(std::memory_order_acquire);
    if (mask == 0) {
      return false;
    }
    if (p_screen >= 32) {
      return true;
    }
    return (mask & (1u << p_screen)) != 0;
  }

 private:
  /**
   * @brief 在 p_list 中按 RID 线性查找（可变版本，返回可写指针）
   *
   * @tparam T 状态数据类型
   * @param[in] p_list 目标线性表（canvas/spatial/text 之一）
   * @param[in] p_rid  要查找的 item RID
   * @return 命中的条目指针；未找到返回 nullptr
   * @note O(n) 线性扫描——HUD 规模（数十 item）下足够且无哈希开销。
   * @note 定义在 item_storage.cpp：私有 helper 仅本 TU 实例化（调用点全在
   * cpp）。
   */
  template <typename T>
  T* FindItem(LocalVector<RIDPair<T>>& p_list, RID p_rid);

  /**
   * @brief 在 p_list 中按 RID 线性查找（常量版本）
   *
   * @tparam T 状态数据类型
   * @param[in] p_list 目标线性表
   * @param[in] p_rid  要查找的 item RID
   * @return 命中的常量条目指针；未找到返回 nullptr
   */
  template <typename T>
  const T* FindItem(const LocalVector<RIDPair<T>>& p_list, RID p_rid) const;

  /**
   * @brief 查找 item 并路由屏级 dirty（Set* 系列共用写入口）
   *
   * 查找与路由合一，Set* 只需把 FindItem 换成本函数即获得
   * per-screen dirty 语义：命中时按 item 的 visibility_mask 置位
   * （变更只落在该 item 可见的屏），未命中时全屏置位（保持
   * "无效 RID 仍防丢帧"的既有语义）。
   *
   * @tparam T 状态数据类型（须含 visibility_mask 字段）
   * @param[in] p_list 目标线性表（canvas/spatial/text 之一）
   * @param[in] p_rid  要查找的 item RID
   * @return 命中的条目指针；未找到返回 nullptr
   */
  template <typename T>
  T* FindItemAndMark(LocalVector<RIDPair<T>>& p_list, RID p_rid);

  /**
   * @brief ADR-0005: 沿 Canvas 父链向上收集，返回自根到链底的数组
   *
   * 链底 = p_item 自身（p_chain[0]），链顶 = 最老祖先（p_chain[p_depth-1]）。
   * 环/断链/超深防御：遇失效父 RID 停止（视为根），深度上限 kMaxTreeDepth。
   *
   * @param[in] p_item 链底 item（必须已在表中）
   * @param[out] p_chain 输出数组（至少 kMaxTreeDepth 容量）
   * @return 链长度（≥1，含自身）
   */
  uint32 CollectCanvasChain(RID p_item, const CanvasItemData** p_chain) const;

  /**
   * @brief ADR-0005: 从链顶向链底合成有效状态（transform/opacity/z/visible）
   *
   * world(self) = root × … × parent × local；z 自顶向下按 z_as_relative
   * 逐级累加；opacity 全员连乘；visible 全员 AND。
   *
   * @param[in] p_chain 自根到链底的数组（CollectCanvasChain 输出）
   * @param[in] p_depth 链长度（≥1）
   * @param[out] p_out 合成结果
   */
  static void ComposeCanvasChain(const CanvasItemData* const* p_chain,
                                 uint32 p_depth,
                                 CanvasItemEffectiveState& p_out);

  /**
   * @brief ADR-0005: SetParent 公共实现（环检测 + 类型校验 + 摘接）
   *
   * @tparam T 状态类型（须含 parent 字段）
   * @param[in] p_list 目标线性表
   * @param[in] p_item 待挂接 item
   * @param[in] p_parent 父 RID（kInvalid = 摘到根；父必须同表存在）
   */
  template <typename T>
  void SetParentImpl(LocalVector<RIDPair<T>>& p_list, RID p_item, RID p_parent);

  /**
   * @brief ADR-0005: Free 后摘除子 item（parent == p_freed → kInvalid）
   *
   * Canvas 释放需同时摘 Canvas 子与 Text 子（Text 可挂 Canvas）。
   *
   * @param[in] p_freed 被释放的 item RID
   */
  void DetachCanvasChildren(RID p_freed);
  /// @brief ADR-0005: 摘除 Spatial 孤儿（Spatial 释放用）
  void DetachSpatialChildren(RID p_freed);

  /**
   * @brief ADR-0005: 判断 p_target 是否在 p_start 的祖先链上（含 p_start 自身）
   *
   * 防御：断链（查无此 RID）即返回 false；迭代上限 kMaxTreeDepth
   * （SetParent 已拒绝环，此为兜底）。
   *
   * @tparam T 状态类型（同表查找）
   */
  template <typename T>
  bool ChainContains(const LocalVector<RIDPair<T>>& p_list, RID p_start,
                     RID p_target) const;

  /**
   * @brief ADR-0005: 收集 p_root 的 Canvas 子树 visibility_mask 并集（含自身与
   * Text 后代）
   *
   * 传播性变更（transform/opacity/visible/z/parent）的路由依据：子孙可能
   * 落在父项不可见的屏上，仅标记父项自身的 mask 会漏掉这些屏（陈旧帧）。
   * O(n×depth) 扫描——HUD 数十 item 规模可忽略。
   */
  uint32 CollectCanvasSubtreeMask(RID p_root) const;

  /// @brief ADR-0005: 收集 p_root 的 Spatial 子树 visibility_mask
  /// 并集（含自身）
  uint32 CollectSpatialSubtreeMask(RID p_root) const;

  /// @brief ADR-0005: 标记 p_root 的 Canvas 子树覆盖的屏（传播性变更调用）
  void MarkCanvasSubtree(RID p_root);

  /// @brief ADR-0005: 标记 p_root 的 Spatial 子树覆盖的屏（传播性变更调用）
  void MarkSpatialSubtree(RID p_root);

  std::atomic<uint64> next_rid_{
      1};  ///< 单调递增的 RID 分配器计数（从 1 起，任意线程安全）
  RID AllocateRID() {
    return RID(next_rid_.fetch_add(1));
  }  ///< 分配一个不复用的唯一 RID

  /// @brief 全屏置位常量（uint32 位宽 → 屏级路由最多支持 32 屏）
  static constexpr uint32 kAllScreensDirty = 0xFFFFFFFF;

  /// @brief ADR-0005: 树深度上限（环/超深防御；SetParent 已拒绝环，此为兜底）
  static constexpr uint32 kMaxTreeDepth = 32;

  /// @brief B3-M0.5 / 方案 C: dirty 的**单一状态**（每屏一位，与 viewport_index
  /// 对应） 兼任"IsDirty"（非零 = 有变更）与"哪些屏要渲"；原独立 dirty_
  /// 已删除。 初始全屏置位，保证首帧各屏都渲染。
  std::atomic<uint32> dirty_screen_mask_{kAllScreensDirty};

  /**
   * @brief 将 p_screen_mask 覆盖的屏标记为脏
   *
   * @param[in] p_screen_mask 屏位掩码（通常为 item 的 visibility_mask）
   * @note 无独立全局门、无 SetDirty：本函数即"置 dirty"（非零即已置位）。
   *       p_screen_mask==0 时不置位。
   */
  void MarkScreens(uint32 p_screen_mask);

  LocalVector<RIDPair<CanvasItemData>>
      canvas_items_;  ///< 2D item 表（RID 序，无序删除）
  LocalVector<RIDPair<SpatialItemData>> spatial_items_;  ///< 3D item 表
  LocalVector<RIDPair<TextItemData>>
      text_items_;  ///< 文字 item 表（RID 序，无序删除）
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_STORAGE_ITEM_STORAGE_H_
