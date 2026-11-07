# 开发日志：ItemStorage 树形管理落地（ADR-0005）

- 日期：2026-09-19
- 范围：servers/rendering/storage/item_storage + render_commands + command_executor +
  render_server + render_screen(RenderFromStorage) + tests/rendering/test_item_storage（新增）+
  app/phase4_canvas（树形用例）
- 依据：ADR-0005（2026-09-19 草案→评审→已接受）；C 类变更，方案先行（ADR §2）+ 评审三决策
  （三类全量 / v1 含 z_as_relative / demo 加用例）

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 三类 ItemData 增 `parent` 字段（Canvas/Text 另增 `z_as_relative=true`） | `storage/item_storage.h` |
| SetParent ×3（环/自挂/跨类型/无效父四重防御）+ SetZAsRelative ×2 | `item_storage.h/.cpp`（SetParentImpl 模板共用） |
| 有效状态计算：CollectCanvasChain（向上收集，断链/超深防御）+ ComposeCanvasChain（自根向下合成 transform 连乘/opacity 连乘/z 累加/visible AND） | `item_storage.cpp` |
| Text 有效状态：位置经父链矩阵变换、alpha 沿链连乘 | `item_storage.cpp` TextItemGetEffective |
| Free 不级联：Canvas 释放摘 Canvas 子 + Text 子；Spatial 释放摘 Spatial 子（WARN） | `item_storage.cpp` Detach*Children |
| 命令：ItemSetParentCmd / ItemSetZRelativeCmd 共用布局（三类型按 type 路由） | `render_commands.h` |
| Executor 分发：ExecItemSetParent / ExecItemSetZRelative | `command_executor.h/.cpp` |
| RenderServer API ×5（异步 Push，CanBypassQueue 短路） | `render_server.h/.cpp` |
| 渲染收集改用有效状态 getter（无父时等价自身状态——渲染器算法零改动） | `render_screen.cpp` RenderFromStorage |
| ItemStorageTree 单测 12 例 | `tests/rendering/test_item_storage.cpp`（新增，注册 CMakeLists） |
| 树形用例：panel→child icon（动画移动+呼吸淡入淡出+Free 摘除验证） | `app/phase4_canvas/main.cpp` |

## 2. 关键实现决策

1. **链计算放收集时、不做缓存**：HUD 数十 item × 深度 ~3，每帧链计算可忽略；
   Godot 式 dirty 失效的 global_transform_cache 列为规模增长后的优化项（ADR §2.3）。
2. **不维护 children 数组**：单一数据源（parent 字段），Free 摘除用 O(n) 扫描——
   消除 child list 与 parent 双写一致性负担（ADR §2.1 有意差异）。
3. **SetParentImpl 按类型特化**：Canvas/Spatial 同表校验 + 环检测；Text 父恒为 Canvas
   （调用方预校验）且 Text 是叶子、父链只指向 Canvas 表，结构上不可能成环——
   `if constexpr (std::is_same_v<T, TextItemData>)` 跳过同表校验。
   **这是单测抓出的第一版 bug**：初版 SetParentImpl 在 text_items_ 里找 canvas 父，
   误判"父不存在"→ 两例 Text 测试失败。教训：跨类型挂接的校验归属要在设计时想清楚。
4. **z 累加语义**：z_eff(c) = c.z_as_relative ? z_eff(parent) + c.z : c.z，
   自根向下逐级合成（与 Godot canvas_item_set_z_as_relative 对齐）；layer/visibility_mask
   不继承（per-item，ADR §2.3）。
5. **RID 日志访问**：RID 的 id_ 为私有，日志用 GetId()——第一版误用 `.id`，
   且 replaceAll `.id))` → `.GetId())` 吞掉一层结构括号引发 C1057（见排障记录）。

## 3. 排障记录

- **C1057 宏扩展意外文件结束**：replaceAll 把 `static_cast<...>(x.id));` 中的 `.id))`
  替换为 `.GetId())`——GetId() 自带一对括号，外层 WARN 调用括号被吞。
  修复：逐模式补回外层 `)`（`.GetId());` → `.GetId()));`）。
  教训：**带配对括号的 API 迁移禁用盲替换**；替换前先数清括号语义，替换后全量 grep 校验。
- **`.id` 私有成员**：第一版直接 `.id` 访问 RID——不编译；改 `GetId()`。
  教训：写日志前先查类型接口（rid.h 的 GetId()），别凭直觉猜成员名。

## 4. 验收结果

- 全量构建：零 error / 零 warning（5 lib + 5 exe + tests）
- 单测：**18/18 全绿**（原 CommandQueue 6 + 新 ItemStorageTree 12）
- demo 冒烟：
  - phase4_canvas：树形用例创建（panel→child icon）、动画移动+淡入淡出正常、
    **Free 摘除实测生效**（"canvas child 4 detached to root (parent 3 freed)"）、
    32294 行日志零 ERROR、干净退出
  - phase4_spatial / phase4_compositor / phase4_text：exit=0、零 ERROR、干净退出
  - phase2：1 条 ERROR 为既有故意负向测试（帧外 DrawListBegin 验证防御路径），
    与本变更无关（phase2 不触碰 ItemStorage）
- 治理 grep（verification.md §5）：①②③ 全部零代码命中；app 仅经 RenderServer 调新 API

## 5. 遗留（登记去向）

- [x] 全局变换缓存 + dirty 失效传播 —— ADR-0005 已列 backlog（规模增长后再引入）
- [x] z_as_relative 的 `z_index` 相对模式在 Spatial 的对应物（render_priority 相对化）—— 暂无需求，不引入
- [x] demo main.cpp 乱码注释清理 —— 既有 P3 登记项，本变更仅触碰 canvas demo 未顺手扩面

## 6. 追加变更（同日）：落地后代码审视，发现并修复 2 个问题

对当日落地代码做严格审视（用户要求"帮忙审视下修改"），发现：

### 6.1 BUG A：传播性变更的屏级 dirty 路由不完整（真 BUG，已修）

- **现象**：`FindItemAndMark` 只标记被改 item 自身的 `visibility_mask`。树形传播后，
  父项的 SetTransform/SetOpacity/SetVisible/SetZOrder/SetZAsRelative/SetParent 会改变
  **全部子孙**（含 Text 子孙）的有效状态——若子项掩码与父项不同（子挂屏 1、父挂屏 0），
  子所在屏不会被标脏 → 不重渲 → **陈旧帧**
- **修复**：新增 `ChainContains` / `CollectCanvasSubtreeMask`（含 Text 后代）/
  `CollectSpatialSubtreeMask` / `MarkCanvasSubtree` / `MarkSpatialSubtree`；
  8 个传播性 setter 全部改为**子树掩码并集路由**（Canvas ×6 + Spatial ×2；
  Text 是叶子无后代，不需改）
- **验证**：新增 3 个 DirtyRouting 回归测试（掩码分屏场景下父变更必须标脏子屏）——
  修复前必失败（旧代码只标父 mask），修复后全绿；单测基线 18 → **22/22**

### 6.2 BUG B：SetParentImpl 无效 RID 早退不置 dirty（约定不一致，已修）

- 文件头明文约定"Set* 找不到目标仍 SetDirty（防丢帧）"，SetParentImpl 早退漏了
  MarkScreens(全屏) + SetDirty。已对齐，新增 SetParentInvalidItemStillDirties 测试

### 6.3 审视确认无问题的项（推理留档）

- 矩阵合成方向：world = root × … × parent × local（列主序，(P·L)·v = P·(L·v)）✅
- z 累加公式：z_eff(c) = c.z_as_relative ? z_eff(parent) + c.z : c.z，自根向下逐级 ✅
- 环检测：沿 p_parent 祖先链找 p_item，A→B→A 场景推演正确 ✅
- 命令枚举中插移位：CommandQueue 环形缓冲为内存内序列化，无跨版本持久化，安全 ✅
- ComposeCanvasChain 循环边界（`i-- > 0`）：depth=2/3 逐步推演正确 ✅
- Free 摘除顺序：RemoveAtUnordered 后全表扫描，无自匹配 ✅
- O(n×depth) 链计算/子树扫描：HUD 数十 item 规模文档化可接受 ✅

### 6.4 预存在问题登记（非本次引入，不修——保持审视范围克制）

- **Free 全族不置 dirty**：`CanvasItemFree/SpatialItemFree/TextItemFree` 删除（及
  树形摘除）后若无其他变更，屏上残留旧画面直到下一次 dirty——为既有设计
  （item_storage.cpp 注释明示"删除无需触发重渲"），登记 `skill/arhud-improvement`
  治理项评估是否改为置位

验证：全量构建零 error 零 warning；单测 **22/22**；phase4_text 冒烟 exit=0 / 零 ERROR。
