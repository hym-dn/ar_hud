# ADR-0005：ItemStorage 树形管理（canvas/spatial/text item 层级）

| | |
|---|---|
| 状态 | **已接受**（评审通过，2026-09-19 当日落地；评审决策：三类全量 + z_as_relative 纳入 v1 + demo 树形用例） |
| 日期 | 2026-09-19 |
| 关联 | `skill/arhud-architecture` §3.2（ItemStorage 职责）、ADR-0001（保留模式）、`servers/rendering/storage/item_storage.h`、Godot `servers/rendering_server.cpp` canvas_item_set_parent |

## 1. 背景与问题

当前 ItemStorage（B2-R 引入）是三类 item 的**扁平线性表**：RIDPair 平铺 + 按索引遍历，
item 之间没有任何父子关系。对照 Godot 4.6 RenderingServer 的 RS 层 canvas item 层级
（`canvas_item_set_parent` + Item::child_items + 收集时递归计算全局变换/modulate/可见性），
本工程缺了保留模式的关键一环：

1. **变换不成组**：HUD 的典型结构是"面板 + 挂在面板上的图标/标签"——移动面板必须逐个
   改每个子 item 的 transform，app 层重复维护组合数学
2. **可见性不成组**：隐藏一个面板需要逐个 SetVisible 子 item
3. **透明度不成组**：整组淡入淡出无法一次完成
4. **Phase 6 场景树断档**：SceneTree 节点的 parent/children 语义在 RS 层没有承接面，
   Phase 6 届时只能在 app 层自行算全局变换——违背"渲染状态收进服务端"的保留模式原则

## 2. 决策

**在 ItemStorage 内为三类 item 引入 parent 链（森林），渲染收集时逐 item 沿链计算
有效状态（transform/visibility/opacity），渲染器算法零改动。**

### 2.1 数据结构

- `CanvasItemData` / `SpatialItemData` / `TextItemData` 各增 `RID parent`（kInvalid = 根）
- **不维护 children 数组**（与 Godot 的有意差异）：HUD 数十 item 规模，收集/校验时按需
  从扁平表推导即可——单一数据源（parent 字段），消除 child list 与 parent 字段的双写
  一致性负担；规模超出后再引入 child_items（列 backlog）

### 2.2 挂接规则

| item 类型 | 可挂父类型 | 说明 |
|---|---|---|
| CanvasItem | CanvasItem | 同类型 |
| TextItem | **CanvasItem** | 文字叶子挂 2D 面板（AR HUD 典型场景） |
| SpatialItem | SpatialItem | 同类型 |

- 传 kInvalidRID = 摘到根；跨类型挂接 → WARN + 视为根（防御，不返回错误——命令为异步 Push）

### 2.3 传播语义（收集时计算，无缓存）

| 属性 | 语义 | 对齐 Godot |
|---|---|---|
| transform | 全局 = 祖先链 mat4 连乘 × 本地 | ✅ global_transform |
| visible | AND 沿链（父隐藏 → 子孙全隐） | ✅ visible_in_tree |
| opacity | 祖先链连乘 × 本地 | ✅ modulate alpha |
| Text 位置 | (x,y) 点经父链 mat4 变换 | ✅（Godot 文字是 canvas item 命令，天然继承） |
| layer / z_order / visibility_mask | **不继承**（per-item） | Godot 无 layer 概念；visibility_mask 对应 visibility_layer（本就 per-item） |

计算时机：`RenderFromStorage` 收集阶段逐 item 沿 parent 链向上（HUD 数十 item × 深度 ~3，
每帧 ~百次 mat4 乘法，可忽略）。Godot 用 dirty 失效的 `global_transform_cache`——**本工程
规模下不引入缓存**（避免失效传播复杂度），列为规模增长后的优化项。

### 2.4 健壮性规则

1. **环检测**：SetParent 时沿目标 parent 的祖先链走，遇 item 自身 → 拒绝（WARN，保持旧父）
2. **自挂接**：item == parent → 同上拒绝
3. **Free 摘除**：释放 item 时扫描同表内 parent == 该 RID 的 item，摘到根（WARN）——
   对齐 RS 语义：free 不级联、子 item 存活
4. **链断裂防御**：parent 链上有失效 RID（理论上被 Free 摘除后不应出现）→ 视为根，不崩溃

### 2.5 命令与 API（对齐 B3 item 命令风格：异步 Push）

```cpp
// render_server.h（新增 3 个方法，全异步 Push）
void CanvasItemSetParent(RID p_item, RID p_parent);    // kInvalid = 摘到根
void TextItemSetParent(RID p_item, RID p_parent);      // 只能挂 CanvasItem
void SpatialItemSetParent(RID p_item, RID p_parent);   // 只能挂 SpatialItem
```

- `render_commands.h`：3 个命令结构体（item RID + parent RID，16B 级小命令）
- `command_executor`：3 个 Exec 分发到 ItemStorage 对应方法
- `item_storage`：SetParent（含环/类型校验）+ Free 摘除 + effective getters：
  `CanvasItemGetEffective`（transform+opacity）/ `CanvasItemIsEffectivelyVisible` /
  `TextItemGetEffectivePosition` / `SpatialItemGetEffectiveTransform`

### 2.6 渲染端（最小侵入）

`RenderFromStorage` 三处字段拷贝改为 effective getter（可见性过滤改为
IsEffectivelyVisible）；渲染器的扁平收集、layer 分组、z 排序、批处理**零改动**。

## 3. 已否决方案

| 方案 | 否决理由 |
|---|---|
| 维护 children 数组（Godot Item::child_items 式） | 双数据源（parent + child list）一致性负担（Free/reparent/乱序删除三处同步）；HUD 规模推导成本 O(n²) 不可感 |
| 全局变换缓存 + dirty 失效传播 | 缓存失效要沿树向上向下传播，复杂度高；HUD 数十 item × 深度 ~3 的链计算可忽略；规模增长后再引入（backlog） |
| z_order 沿树累加（z_as_relative） | 改动 z 排序语义（canvas/text 的 qsort 与层分组耦合），超出"树形管理"最小集；~~v2 扩展~~ **（评审决策改纳入 v1，见状态行；本行保留为原始否决记录）** |
| layer 沿树继承 | layer 是渲染批处理分组键，继承会让父子异 layer 场景语义模糊；per-item 更直白 |
| 树管理放 app 层（SceneTree 前置实现） | 违背保留模式原则（状态收服务端）；Phase 6 被迫重复实现 |
| SetParent 返回 Error | item 命令统一异步 Push 无返回通道；违规场景 WARN + 防御兜底即可 |

## 4. 后果

- **正面**：面板/图标/标签成组操作（变换/显隐/淡入淡出）；AR HUD 场景（标签贴 3D 标记）
  表达力质变；Phase 6 SceneTree 有 RS 层承接面；与 Godot RS 心智模型对齐
- **负面/代价**：收集阶段每 item 沿链计算（可忽略）；三类 item 语义分叉点
  （Text 只能挂 Canvas）需文档清晰
- **中性约束**：parent 字段进入三类 ItemData；Free 增加 O(n) 摘除扫描（量级可忽略）

## 5. 验收

- 新增 `tests/rendering/test_item_storage.cpp`：链变换正确性（两层/三层连乘）、可见性
  AND 传播、opacity 连乘、Text 点变换、环拒绝（A→B→A）、自挂接拒绝、Free 摘除到根、
  跨类型挂接拒绝、失效链防御
- phase4_canvas demo 增加树形用例（panel → icon + text 子项，整体移动/淡入淡出）
- 渲染回归：既有 4 demo 视觉不变（无 parent 时 effective = 自身状态，行为等价）
- 治理 grep 全绿（verification.md §5）

## 6. 验收结果（2026-09-19 落地）

- 单测 **22/22 全绿**（原 6 + ItemStorageTree 16：NoParent 基线/TransformChain/
  VisibilityPropagation/OpacityChain/ZRelative 三级累加/CycleAndSelfParent 拒绝/
  FreeDetaches/SetParentToInvalid 摘除/Text 位置+alpha/Text 跨类型拒绝/TextZRelative/
  SpatialTransformChain + 落地后审视回归 4 例：DirtyRouting 传播分屏路由 ×3 +
  SetParent 无效 RID 置 dirty）
- 全量构建零 error 零 warning（5 lib + 5 exe）
- demo 冒烟：phase4_canvas 树形用例（panel→child icon，动画移动+呼吸淡入淡出），
  **Free 摘除语义实测生效**（"canvas child 4 detached to root (parent 3 freed)"），
  零 ERROR 干净退出；phase4_spatial/compositor/text 零 ERROR；phase2 的 1 条 ERROR
  为既有故意负向测试（帧外 DrawListBegin 验证 API 防御路径，与本变更无关）
- 治理 grep：①②③ 全部零代码命中；app 侧仅经 RenderServer 调用新 API

## 7. 实现差异记录

- SetParentImpl 按类型特化：Text 的父恒为 Canvas（TextItemSetParent 预校验存在性），
  且 Text 是叶子、父链只指向 Canvas 表 → 结构上不可能成环，跳过同表校验与环检测
  （`if constexpr (std::is_same_v<T, TextItemData>)`）
- 渲染收集需同时取原始数据（visibility_mask/layer 等）与 RID → ItemStorage 增加
  `GetCanvasItemRid/GetSpatialItemRid/GetTextItemRid` 按索引访问器
- **传播性变更的屏级 dirty 路由按子树掩码并集**（落地后审视发现并修复）：父项
  SetTransform/SetOpacity/SetVisible/SetZOrder/SetZAsRelative/SetParent 影响整棵子树
  的有效状态（子孙掩码可能与父不同）——`CollectCanvasSubtreeMask`（含 Text 后代）/
  `CollectSpatialSubtreeMask` 收集并集标脏；非传播性字段（material/mesh/layer/
  visibility_mask）维持按 item 自身 mask 路由
- SetParentImpl 无效 RID 早退对齐文件约定：MarkScreens(全屏) + SetDirty（防丢帧）
