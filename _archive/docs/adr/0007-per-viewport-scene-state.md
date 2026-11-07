# ADR-0007：per-viewport 场景状态归属（相机/灯光迁入 RenderViewport）

| | |
|---|---|
| 状态 | **已接受**（2026-10-01；采纳方案 C，与 ADR-0006 同窗口落地，见 §8 实施记录） |
| 日期 | 2026-10-01 |
| 关联 | ADR-0006（相机命令载荷参数化）§2.3 / §6、ADR-0002（多屏共享栈 / per-screen 相机）、ADR-0005（ItemStorage 树形管理）、`skill/arhud-architecture` §九、`skill/arhud-multiscreen-arch` §3.3、`docs/devlog/2026-09-27_camera_pipeline_per_viewport.md` |

## 1. 背景与问题

B3-M0.4 把 **per-screen 相机/灯光**以 `ItemStorage::ScreenState` 落库（`screen_states_`，按屏索引，
惰性 `EnsureScreen`）。功能可用，但**类型归属错位**：

- `ItemStorage` 的职责是「**绘制单元（item）状态账本**」（canvas/spatial/text 三表 + 层级 + dirty 门）；
  相机/灯光是「**视口 / 场景级状态**」，不是 item。
- 对照 Godot：`Viewport` / `Scenario`（相机 / 环境 / 灯光）与 `Instance`（item）是**不同的 owner**；
  ARHud 把两类状态揉进同一个类。
- 内部反证：per-screen 渲染配置（`cull_mask_` / `viewport_index_` / 后处理参数 / FBO）**已在
  `RenderViewport`**；唯独相机/灯光留在 `ItemStorage`，是那个"异类"。
- 后果：`ItemStorage` "四合一"（item 表 / 相机灯光 / 屏级 dirty 门 / 屏幕路由），是维护者认知负担的
  **结构根源**；也阻断扩展（多相机、环境、雾效无归宿）。

触点很小，使本决策成本可控：

| 侧 | 触点 | 位置 |
|---|---|---|
| 写 | **4 处** | `CommandExecutor` 的 `ExecCamera3DSet/2DSet/DirectionalLightSet/Clear` |
| 读 | **1 处** | `RenderViewport::RenderFromStorage` |
| 数据 | 1 结构 | `ItemStorage::ScreenState` + `screen_states_` + 相机灯光 API |

约束：**不改渲染结果**；`CameraMatrix` 惰性机制不动（归 ADR-0006）；命令仍走 SPSC；保持车规确定性
（不引入跨线程共享可变状态）；视口越界不得崩溃。

## 2. 决策

**把 per-screen 相机/灯光（`camera_3d` / `camera_2d` / `light` / `has_light`）从 `ItemStorage`
**迁入 `RenderViewport`**（每个视口自持）；`ItemStorage` 只保留 item 三表；屏级 dirty 门仍留
`ItemStorage`。**

要点：

- **落点**：`RenderViewport` 新增成员 `ViewportSceneState scene_state_`（`CameraMatrix` /
  `Transform2D` / `DirectionalLight` / `has_light`）及四个 setter + 三个 getter。选择视口作为归宿的
  理由：**per-screen 渲染配置本已在 `RenderViewport`**（`cull_mask_` / `viewport_index_` / 后处理），
  相机/灯光与之同属"本屏如何取景"，理应收在一处。
- **类型落点**：场景态纯数据类型（`DirectionalLight` 与 `ViewportSceneState`）移入新头
  `servers/rendering/storage/viewport_scene_state.h`——仍在 `storage/` 层、零 GPU 依赖，
  满足唯一硬约束 `renderer → storage`（单向）。
- **视口解析**：新增极简接口 `IViewportProvider`（`RenderViewport *GetViewport(uint32)`，
  见 `servers/rendering/viewport_provider.h`）。`ARHudEngine` 实现之（`GetScreen(id)`），
  注入 `CommandExecutor` 与 `RenderServer`（旁路路径用）。
- **写路径**：`CommandExecutor` 4 处 → `viewport->SetCamera*` + `item_storage_->MarkScreen(vp)`
  （标屏 dirty）。
- **读路径**：`RenderViewport::RenderFromStorage` 读自身 `scene_state_`。
- **越界防御**：`GetViewport(index)` 返回 nullptr 时 **WARN + 跳过**（视口在 `AddScreen` 时即创建，
  正常早于相机命令）。
- **不改** `kCamera3DSet` 等命令的**类型/语义**（载荷参数化属 ADR-0006）。

### 2.1 关键不变量

- `ItemStorage` 不再含任何相机/灯光成员、`ScreenState`、`screen_states_` 及相关 API。
- 屏级 dirty 仍是**唯一**渲染调度门；**item 变更与视图状态变更都置位**对应屏 dirty
  （视图状态写入由 `CommandExecutor` 调 `MarkScreen` 完成）。
- 惰性语义：视图状态的越界**不崩溃**（`IViewportProvider` 越界返回 nullptr → WARN 跳过），
  取代原 `EnsureScreen` 的"自动扩容"。

### 2.2 选项裁决（采纳 C）

| 方案 | 职责对齐 | 扩展性 | 成本 | 风险 | 结论 |
|---|---|---|---|---|---|
| **A** 不动，留 `ItemStorage` | ✗ | ✗ | 0 | 0 | 否决 |
| **B** 兄弟对象（Engine 持有、按屏索引） | ~（仅"换名"，仍在 Engine 的表里） | ✓ | 低 | 低 | 否决（见 §3） |
| **C** 迁入 `RenderViewport`（每视口自持） | ✓✓（与已有 per-screen 配置同处，贴合 Godot `Viewport`） | ✓✓ | 中 | 中（可控，见 §3） | **采纳** |

**裁决说明**：B 与 C 概念上都把相机/灯光移出 item 账本，但 B 只是"换个类型名、仍待在 Engine 的
按屏表"，与"**per-screen 配置已在 `RenderViewport`**"这一既有事实不一致；C 让"本屏取景"这一整类状态
真正收拢到视口，是唯一自洽的归宿。C 的成本被两点稀释：`ViewportID` 即 `screens_` 下标（天然按索引），
且新增机制只有一个 `IViewportProvider` 接口。

## 3. 已否决方案

| 方案 | 理由 |
|---|---|
| **A. 不动，留在 `ItemStorage`** | SRP 违背；与 Godot 不同源；阻断扩展；"四合一"认知负担持续 |
| **B. 兄弟对象 `ViewportSceneState`（Engine 持有）** | 仅"换名"——状态仍在 Engine 的按屏表，未落到视口；与"per-screen 配置已在 `RenderViewport`"不一致；相较 C 无独立价值 |
| **只搬相机、留灯光** | 二者同属场景级状态，无理由拆开；徒增两套归属 |
| **顺带抽出屏级 dirty 为独立对象** | 范围更大，超出本决策；保持最小改动，列为后续可选 |

> 对"方案 C 生命周期风险"（相机命令可能早于视口创建）的处置：**越界 WARN + 跳过**；因视口在
> `AddScreen` 即创建，正常调用序列下不触发。此为本决策接受的权衡。

## 4. 后果

- **正面**：`ItemStorage` 回归"item 账本"单一职责；相机/灯光与既有 per-screen 配置在
  `RenderViewport` 收拢；与 Godot `Viewport` 概念对齐；为后续多相机/环境/雾效提供归宿；
  显著降低维护者认知负担。
- **负面 / 代价**：新增 1 个头（`viewport_scene_state.h`）+ 1 个接口（`IViewportProvider`）+
  `Engine` 实现接口并注入；`CommandExecutor`（4 处）与 `RenderViewport`（读 1 处、新增 setter/getter）
  改动；**必须与 ADR-0006 同窗口实现**（同一片相机代码）。
- **中性约束**：
  - 屏级 dirty 继续由 `ItemStorage` 承载——文档须明确其身份为"**横切调度信号**"，非"item 专属"；
  - `RenderViewport` **不得**反向依赖 `ItemStorage` 的相机语义（只通过 `IViewportProvider` 被外部解析）。

## 5. 验收

- **治理 grep**：`ItemStorage` 头/实现中**不再出现** `camera_3d` / `camera_2d` / `DirectionalLight` /
  `ScreenState` / `screen_states_` / `EnsureScreen`。
- `CommandExecutor` 4 处、`RenderViewport` 读 1 处均指向 `RenderViewport::scene_state_`。
- **单测**：既有用例全绿（现为 **29**；相机/灯光路由用例改为断言 `RenderViewport` 内的状态）；
  跨屏不串扰 + 视图变更置对应屏 dirty。
- **demo 冒烟**：`phase4_spatial` / `phase4_compositor` 相机位置/朝向生效（**与 ADR-0006 同一判据**，人眼确认）。
- 全量构建绿 + 治理 grep（`docs/process/verification.md` §5）全绿。

## 6. 分阶段实施（C 类要求：每阶段可编译可测）

| 阶段 | 内容 | 独立验收 |
|---|---|---|
| **1** | 新增 `viewport_scene_state.h`（迁 `DirectionalLight` + `ViewportSceneState`）与 `viewport_provider.h`（`IViewportProvider`）；`RenderViewport` 加 `scene_state_` + setter/getter | 编译绿；`ItemStorage` 仍保留旧相机/灯光 API（暂不删） |
| **2** | `ARHudEngine` 实现并注入 `IViewportProvider`；`CommandExecutor` 4 处改指视口 + 标 dirty；`RenderViewport::RenderFromStorage` 读自身；`RenderServer` 旁路改走视口 | 单测全绿；相机/灯光路由落到视口 |
| **3** | **删除** `ItemStorage::ScreenState` / `screen_states_` / `EnsureScreen` / 相机灯光 API | grep 满足 §5；28 用例全绿 |
| **4** | devlog + `skill/arhud-architecture` §九/§3.2 更新；ADR-0006 交叉引用转正式 | 文档同步 |

> **落地状态（2026-10-01）**：阶段 1-3 已**同窗口落地**（与 ADR-0006）：
> 新增 `viewport_scene_state.h`（迁 `DirectionalLight` + `ViewportSceneState`）与 `viewport_provider.h`
> （`IViewportProvider`）；`RenderViewport` 加 `scene_state_` + setter/getter；`ARHudEngine` 实现接口并
> 注入 `CommandExecutor` / `RenderServer`；`CommandExecutor` 4 处改指视口 + `MarkScreen`；`RenderViewport::
> RenderFromStorage` 读自身；`RenderServer` 旁路改走视口；`ItemStorage` 的 `ScreenState` / `screen_states_`
> / `EnsureScreen` / 相机灯光 API **已删除**。阶段 4（devlog / skill §六 等文档同步）见 §8。

**与 ADR-0006 的窗口协调**：上表与 ADR-0006 §6 共享同一实现窗口（同一片相机代码）。
**解耦保证**：若本 ADR 被否决，ADR-0006 仍可独立推进；若 ADR-0006 被否决，本 ADR 的搬迁仍可独立成立。

## 7. 待定（评审拍板结论）

- **`IViewportProvider` 的放置**：裁决为**独立头** `servers/rendering/viewport_provider.h`——极简接口
  与命令词汇表（`render_commands.h`）不同层级，独立头避免命令层依赖渲染对象概念。
- **`DirectionalLight` 是否一并迁出 `item_storage.h`**：裁决为**迁**——`DirectionalLight` 与
  `ViewportSceneState` 同置 `storage/viewport_scene_state.h`，`item_storage.h` 不再定义它
  （`render_server.h` 经包含新头获取该类型）。

## 8. 实施记录（2026-10-01）

采纳方案 C，与 ADR-0006 **同窗口落地**；实际改动清单：

| 文件 | 改动 |
|---|---|
| `servers/rendering/storage/viewport_scene_state.h` | **新增**：`DirectionalLight`（自 `item_storage.h` 迁入）+ `ViewportSceneState`（`camera_3d`/`camera_2d`/`light`/`has_light`） |
| `servers/rendering/viewport_provider.h` | **新增**：`IViewportProvider`（`RenderViewport *GetViewport(uint32)`） |
| `servers/rendering/storage/item_storage.{h,cpp}` | 删 `DirectionalLight` / `ScreenState` / `screen_states_` / `EnsureScreen` / 相机灯光 API；`MarkScreen` 提升为 public |
| `servers/rendering/render_viewport.{h,cpp}` | 加 `ViewportSceneState scene_state_` + 4 setter + 3 getter；`RenderFromStorage` 读自身 |
| `servers/rendering/command_executor.{h,cpp}` | 加 `viewport_provider_`；`ResolveViewport`；4 处相机/灯光执行器改指视口 + `MarkScreen` + `SetDirty`；新增 `ExecDirectionalLightClear` |
| `servers/rendering/render_server.{h,cpp}` | `Initialize` 增 `IViewportProvider*`；相机/灯光短路分支经 `ResolveViewport` 写视口；`Camera3DSet` 参数化（ADR-0006） |
| `servers/rendering/arhud_engine.{h,cpp}` | 实现 `IViewportProvider`（`GetViewport` → `GetScreen`）；注入 `CommandExecutor` / `RenderServer` |
| `servers/CMakeLists.txt` | `SERVERS_HEADERS` 增 `viewport_provider.h` / `viewport_scene_state.h` |
| `tests/rendering/test_command_executor.cpp` | 改为 `FakeViewportProvider` + 真实 `RenderViewport`，断言视口内状态；新增越界丢弃用例 |

待办（阶段 4 及人工项）：~~devlog 归档~~ ✅ `docs/devlog/2026-10-01_adr0006_0007_camera_light_migration.md`；
~~`docs/README.md` / `AGENTS.md` 目录图同步~~ ✅（并同步 `docs/adr/README.md` 索引状态、
`skill/arhud-multiscreen-arch` §一/§3.3/§3.5、`skill/arhud-improvement` B3-M0.4、`skill/arhud-render-thread-arch` §2.2）；
demo 视觉冒烟（人眼）**待办**。