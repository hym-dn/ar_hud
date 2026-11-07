# ADR-0006：相机命令载荷参数化（对齐 CameraMatrix 惰性重算）

| | |
|---|---|
| 状态 | **已接受**（2026-10-01；与 ADR-0007 同窗口落地，见 §7 实施记录） |
| 日期 | 2026-09-27 |
| 关联 | `skill/arhud-render-thread-arch` §3.4、`docs/devlog/2026-09-27_camera_pipeline_per_viewport.md`、`core/math/camera_matrix.h`、ADR-0001（命令序列化）、ADR-0002（多屏共享栈 / per-screen 相机）、**配套决策：ADR-0007（已接受，见 `0007-per-viewport-scene-state.md`）** |

## 1. 背景与问题

B3-M0.4 已把 **per-screen 相机**存入 `ItemStorage`（`ScreenState`），并把 `RenderServer` /
命令链的视口路由补齐（2026-09-27，见 devlog）。但随即发现**更深一层缺陷**：
**3D 相机设置端到端不生效**——app 的 `SetPerspective` / `LookAt` 被丢弃，渲染恒用默认相机
（fov 45 / aspect 16:9 / eye 原点 / target -Z）。

根因是「**惰性重算**」与「**矩阵化传输**」的组合失效，证据链：

| # | 事实 | 位置 |
|---|---|---|
| 1 | `SetPerspective/SetOrthographic/LookAt` 只置 `matrices_dirty_ = true`，**不重算** | `camera_matrix.cpp:16-62` |
| 2 | `GetProjectionMatrix()/GetViewMatrix()` **直接返回成员、不调用 `UpdateMatrices()`** | `camera_matrix.h:70-72` |
| 3 | ⇒ `RenderServer::Camera3DSet` 读到的载荷是**未重算的默认矩阵** | `render_server.cpp` |
| 4 | `ExecCamera3DSet` 新建 `CameraMatrix`（`matrices_dirty_` 默认 **true**），memcpy 进矩阵后存库，**参数仍是默认值** | `command_executor.cpp` |
| 5 | `SpatialRenderer::RenderSpatialItems` 调 `UpdateMatrices()` → 因 dirty=true **按默认参数重算，覆盖** memcpy 的矩阵 | `spatial_renderer.cpp:523` |

- 第 1-2 点已由**失败单测实证**（两个 `SetPerspective` 参数完全不同的相机，读出的投影逐字节相同）；
  3-5 点由代码推证。
- 全仓唯一调用 `UpdateMatrices()` 之处即 `spatial_renderer.cpp:523`。
- **不变量**：`CameraMatrix` 的惰性设计本身是合理的（避免重复计算），问题在于**跨线程传输的是
  "计算结果"而非"意图参数"**，导致接收端无从重建。

约束：命令走 SPSC 环形缓冲（≤64KB / 单命令），相机命令为低频（每次变更一条）；不得引入
跨线程共享可变状态；不得暴露 `CameraMatrix` 内部布局。

## 2. 决策

**`Camera3DSetCmd` 携带「相机意图参数」，渲染侧用 `Set*` 重建 `CameraMatrix`——与惰性重算协同，
而非对抗。**

### 2.1 命令结构（`render_commands.h`）

```cpp
struct Camera3DSetCmd
{
    static constexpr RenderCommandType kType = RenderCommandType::kCamera3DSet;
    uint32 viewport_index;            ///< 目标视口索引
    uint8  projection_type;           ///< kPerspective / kOrthographic
    uint8  keep_aspect;               ///< kKeepHeight / kKeepWidth
    uint8  ortho_mode;                ///< kSize / kManualRect
    uint8  reserved;                  ///< 对齐保留（0）
    float  fov_y, aspect, near_z, far_z;
    float  ortho_size;
    float  ortho_left, ortho_right, ortho_bottom, ortho_top;
    float  position[3], target[3], up[3];
};
```

- 仍满足 `static_assert(std::is_trivially_copyable_v<...>)`；尺寸 ~80B（原两矩阵为 128B）。
- **不使用**变长 payload 区（相机命令低频、字段有界；固定结构体让 Executor 无分支）。

### 2.2 发送侧 / 执行侧

| 侧 | 动作 |
|---|---|
| 发送（主线程 `RenderServer`） | 用 getter 抽参数：`GetProjectionType/GetKeepAspect/GetFOV/GetAspect/GetNearZ/GetFarZ/GetOrthoSize/GetPosition/GetTarget/GetUp`（均已存在） |
| 执行（渲染线程 `CommandExecutor`） | 按 `projection_type` 调 `SetPerspective` / `SetOrthographic`，调 `LookAt` 重建 → 存库 |

关键收益：重建后的 `CameraMatrix` 参数正确，`SpatialRenderer` 的 `UpdateMatrices()`
**按正确参数**重算 ⇒ 端到端正确。**无需改动 `CameraMatrix` 的惰性机制。**

### 2.3 边界（明确不做的事）

- **不传** `use_manual_view_`（`SetViewTransform`）：全仓**零调用方**，属死路径。新增
  `CameraMatrix::IsUsingManualView()` 后，`RenderServer::Camera3DSet` 在该模式激活时
  **WARN 并拒绝**（不静默丢弃），登记 backlog（首个调用方出现时再补传输）。
- `Camera2DSetCmd`（`Transform2D` 平铺，无惰性）/ `DirectionalLightSetCmd`（纯 float）**不受影响**，
  且已具备 `viewport_index`。
- 不顺手重构相机以外任何东西（最小改动原则）。
- **实现窗口（交叉引用）**：本决策与「per-viewport 场景状态归属」决策——即 `ScreenState` 承载的
  相机/灯光由 `ItemStorage` 迁出为独立类型 `ViewportSceneState`（**ADR-0007** 草案，
  见 `0007-per-viewport-scene-state.md`）——
  **同窗口实现**。理由：两者改的是**同一片代码**（`Camera3DSetCmd` 载荷 + `CommandExecutor` 的
  相机/灯光路径），分两次做会重复触碰、重复回归、重复改同一批单测。**但决策分开记录**
  （一 ADR 一决策，便于各自否决）；0007 落地后，此处转为正式**双向**引用。
  **解耦保证**：若 0007 未获通过，本决策仍可**独立实现**（见 §6），0007 不构成本决策的前置条件。

## 3. 已否决方案

| 方案 | 否决理由 |
|---|---|
| **B：矩阵传输 + 新增 `CameraMatrix::SetMatrices(proj, view)`（写入并清 dirty）** | 传"结果"而非"意图"；需为绕过惰性专开一条旁路通道；后续任何 `CameraMatrix` 参数变更都须记得同步清 dirty（易漏）；`UpdateMatrices()` 重算能力被架空 |
| **C：仅在 `RenderServer::Camera3DSet` 读前调 `UpdateMatrices()`** | 只修第 1-3 点；第 4-5 点仍在（Executor 侧仍被默认参数覆盖）⇒ 相机依旧不生效 |
| 让 `Get*Matrix()` 自动重算，仍传输矩阵 | 同样只修一半（3-5 点未解）；且把「读」变成有副作用（隐式重算），语义更隐晦 |
| 整体 `memcpy` 传输 `CameraMatrix`（params + matrices + dirty 标志） | 暴露内部布局、跨版本/跨后端 ABI 脆弱；把"对象状态"当"消息"传，违反命令序列化的值语义约定 |
| 保留矩阵传输 + 在 `ExecCamera3DSet` 里反解参数 | 从投影/视图矩阵**反解** fov/位置等不可靠（正交/退化情形），且不可逆（`LookAt` 的 up 丢失） |

## 4. 后果

- **正面**：3D 相机端到端生效；传"意图"使命令**小而稳定**，便于后续扩展（exposure / dof / 投影矩阵自定义
  等只需加字段）；与惰性重算天然协同；对 Vulkan/多后端无影响（后端只消费 `CameraMatrix`）。
- **负面/代价**：
  - `Camera3DSetCmd` 字段与 `CameraMatrix` 的参数集**必须保持同步**（新增投影参数时两处同改）——
    以 Doxygen 交叉引用 `@see CameraMatrix` 约束；
  - 需为 `CameraMatrix` 补 2 个 getter（`IsUsingManualView` / `IsUsingManualOrthoRect`）；
  - manual-view 模式在命令链上**暂不支持**（WARN），属已知限制。
- **中性约束**：`kCamera3DSet` 语义不变（仍是"设置某视口 3D 相机"）；`CanBypassQueue` 短路分支
  同样改用视口路径（ADR-0007：主线程内直调目标 `RenderViewport::Camera3DSet`，不经命令）。

## 5. 验收

- **单测**（扩展 `tests/rendering/test_command_executor.cpp`）：
  1. 用真实 `CameraMatrix`（`SetPerspective` + `LookAt`）经发送侧语义构造命令 → Executor 执行后，
     断言目标 `RenderViewport::GetCamera3D()` 的**参数**（`GetFOV/GetAspect/GetPosition/GetTarget`）与来源一致；
  2. 调 `GetCamera3D().UpdateMatrices()` 后，投影矩阵等于本地 `Perspective(fov, aspect, near, far)`；
  3. 两个视口设不同相机 → 互不串扰（保留既有路由用例）
- **demo 视觉冒烟**：`phase4_spatial` / `phase4_compositor` 的相机位置/朝向**生效**（需人眼确认，
  这是本缺陷的关键判据）
- 全量构建绿 + 既有用例全绿（现为 **29** 用例）；治理 grep（verification.md §5）全绿
- `SetViewTransform` 激活时应出现明确 WARN（防御路径可见）

## 6. 分阶段实施（C 类要求：每阶段可编译可测）

| 阶段 | 内容 | 独立验收 |
|---|---|---|
| **1** | `CameraMatrix` 补 `IsUsingManualView()` / `IsUsingManualOrthoRect()` getter | 编译绿；core 侧无行为变化 |
| **2** | `Camera3DSetCmd` 换参数结构体；`RenderServer` 抽参数 + manual-view WARN；`CommandExecutor` 重建；扩展单测 | 新单测全绿（含 §5 的三条断言）；既有用例不回归（29） |
| **3** | demo 视觉冒烟 + 归档（devlog / skill §3.4 状态转"已接受"） | 相机位置生效（人眼）；文档同步 |

> **落地状态（2026-10-01）**：阶段 1/2 已随 ADR-0007 **同窗口落地**（`CameraMatrix` 三 getter 就位；
> `Camera3DSetCmd` 参数化；`RenderServer::Camera3DSet` 抽参 + manual-view WARN；`ExecCamera3DSet`
> 重建并写入视口；`test_command_executor.cpp` 已改断视口内状态）。阶段 3 的 demo 视觉冒烟待人眼确认。

**本 ADR 只提方案，不含实现**；实现须在评审通过后按上表推进。

**与 ADR-0007 的窗口协调**：上表**阶段 1/2 与 ADR-0007 共享同一实现窗口**（同一片代码，见 §2.3）——
0007 通过时，其"按索引取视口"的注入改动并入**阶段 2**；0007 未通过时，阶段 1/2 按本表**独立推进**
（`CommandExecutor` 继续经 `item_storage_` 读写相机/灯光）。无论 0007 是否通过，阶段 3 的验收判据不变。

## 7. 实施记录（2026-10-01）

ADR-0007 已获采纳（方案 C），本 ADR 与其**同窗口落地**；实际改动清单：

| 文件 | 改动 |
|---|---|
| `core/math/camera_matrix.h` | 新增 `IsUsingManualView()` / `IsUsingManualOrthoRect()` / `GetOrthoRect()` |
| `servers/rendering/render_commands.h` | `Camera3DSetCmd` 换为**意图参数**结构体（投影类型 + fov/aspect/near/far + 正交模式 + position/target/up），删 `projection[16]`/`view[16]` |
| `servers/rendering/render_server.cpp` | `Camera3DSet` 抽参数（getter）+ `IsUsingManualView()` 时 WARN 并忽略；短路分支改走目标视口（ADR-0007） |
| `servers/rendering/command_executor.cpp` | `ExecCamera3DSet` 用 `SetPerspective/SetOrthographic` + `LookAt` **重建** `CameraMatrix` 后写入视口 |
| `tests/rendering/test_command_executor.cpp` | 改为断言目标视口内 `GetCamera3D()` 的参数；新增越界视口安全丢弃用例 |

> 说明：原 §6「0007 未通过时独立推进」的分支**未采用**——0007 采纳 C，故 `CommandExecutor` 相机/灯光
> 路径直写 `RenderViewport`（非 `item_storage_`）。