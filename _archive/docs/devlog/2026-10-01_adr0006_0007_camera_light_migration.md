# 开发日志：相机命令参数化 + 相机/灯光迁入 RenderViewport（ADR-0006 + ADR-0007 同窗口落地）

- 日期：2026-10-01
- 范围：servers 渲染链（`render_commands.h` / `render_server.*` / `command_executor.*` / `render_viewport.*` / `arhud_engine.*` / `storage/*`）+ `core/math/camera_matrix.h` + 新增单测 + `servers/CMakeLists.txt`
- 变更分级 **C 类**（契约变更 + 类型归属迁移；方案先行：ADR-0006 + ADR-0007）
- 依据：`docs/adr/0006-camera-command-params.md`、`docs/adr/0007-per-viewport-scene-state.md`、`skill/arhud-architecture` §九、`docs/devlog/2026-09-27_camera_pipeline_per_viewport.md` §4（本篇即其遗留的收口）

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 相机命令载荷从「矩阵」改为「意图参数」 | `render_commands.h`：`Camera3DSetCmd` 重写 |
| 发送侧抽参数 + manual-view 防御 | `render_server.cpp`：`Camera3DSet` 重写；`camera_matrix.h` 补 3 个 getter |
| 执行侧重建 `CameraMatrix` | `command_executor.cpp`：`ExecCamera3DSet` 改用 `Set*` + `LookAt` |
| 相机/灯光状态迁出 item 账本 | 新增 `storage/viewport_scene_state.h`；`item_storage.*` 删 `ScreenState`/`screen_states_`/`EnsureScreen`/相机灯光 API |
| 每视口自持场景态 | `render_viewport.*`：`ViewportSceneState scene_state_` + 4 setter + 3 getter；`RenderFromStorage` 读自身 |
| 视口解析接口 | 新增 `viewport_provider.h`（`IViewportProvider`）；`arhud_engine.*` 实现并注入 |
| 旁路路径改走视口 | `render_server.cpp`：`ResolveViewport` 解析后直写目标视口 |
| 单测改写 | `tests/rendering/test_command_executor.cpp`：`FakeViewportProvider` + 真实 `RenderViewport` |
| 头文件登记 | `servers/CMakeLists.txt`：`SERVERS_HEADERS` 增两个新头 |

## 2. 关键实现决策

1. **相机命令传「意图参数」而非「结果矩阵」**：`Camera3DSetCmd` 携带 `projection_type / keep_aspect / ortho_mode / fov_y / aspect / near_z / far_z / ortho_size / ortho_rect / position / target / up`，执行侧用 `SetPerspective` / `SetOrthographic` + `LookAt` 重建。理由：`CameraMatrix` 惰性重算（`Set*` 只置 dirty、`Get*Matrix()` 不重算）决定了传矩阵必然送 stale 值；传参数让 `SpatialRenderer::UpdateMatrices()` 按**正确参数**重算，无需改动惰性机制（ADR-0006 §2）。
2. **`CameraMatrix` 只补 getter、不动语义**：新增 `IsUsingManualView()` / `IsUsingManualOrthoRect()` / `GetOrthoRect(...)`；`SetViewTransform`（manual view）为全仓零调用方的死路径，命令链**不支持**——发送侧遇 `IsUsingManualView()` 则 WARN 拒绝（不静默丢弃）。
3. **场景态落到 `RenderViewport`（方案 C，ADR-0007）**：per-screen 渲染配置（`cull_mask_` / `viewport_index_` / 后处理 / FBO）本已在视口，相机/灯光与之同属「本屏如何取景」，收拢一处才自洽；`ItemStorage` 回归纯 item 账本。
4. **`IViewportProvider` 极简接口 + 独立头**：`RenderViewport *GetViewport(uint32)`，避免命令层依赖具体渲染对象；`ARHudEngine` 实现（转 `GetScreen`）并注入 `CommandExecutor` / `RenderServer`。
5. **dirty 门仍留 `ItemStorage`（横切调度信号）**：视图状态写入后由 `CommandExecutor` / `RenderServer` 调 `MarkScreen(vp)` + `SetDirty()` 标屏，不做 `ViewportSceneState → ItemStorage` 反向依赖。
6. **越界防御替代 `EnsureScreen`**：`GetViewport` 越界返回 `nullptr` → WARN + 跳过；视口在 `AddScreen` 即创建，正常序列不触发。

## 3. 排障记录（收口 2026-09-27 §4）

- **现象**：3D 相机设置端到端不生效——app 的 `SetPerspective` / `LookAt` 被丢弃，渲染恒用默认相机。
  排查：2026-09-27 的失败单测实证「两个参数完全不同的相机读出逐字节相同的投影矩阵」。
  → 根因：`Set*` 惰性置 dirty 但 `Get*Matrix()` 不重算 ⇒ 发送侧读到 stale 默认矩阵；执行侧 `memcpy` 进 `matrices_dirty_=true` 的新对象，又被 `spatial_renderer.cpp` 的 `UpdateMatrices()` 按默认参数覆盖。
  修复：改为传意图参数、执行侧重建（决策 1）。教训：**跨线程传输应传「意图」而非「计算结果」**，尤其对带惰性语义的对象。
- **迁移触点**：`ItemStorage` 的相机/灯光读写仅 4 写 1 读，触点小使方案 C 成本可控；`ViewportID` 即 `screens_` 下标，天然按索引解析。

## 4. 验收结果

- 构建：**未执行**（按用户要求，编译交由用户）——待用户在 `.\build.bat conan` 后复核。
- 单测：预期 **29**（`test_command_executor` 4 用例改为断言 `RenderViewport::GetCamera3D()` 参数 + 新增越界丢弃用例）。
- demo：照片视觉冒烟（`phase4_spatial` / `phase4_compositor` 相机位置/朝向生效）为**人工项**，待确认。
- 治理 grep：`ItemStorage` 头/实现中**已不含** `camera_3d` / `camera_2d` / `DirectionalLight` / `ScreenState` / `screen_states_` / `EnsureScreen`（已 grep 核对）。

## 5. 遗留（登记去向：skill/arhud-improvement）

- [ ] **编译 + 单测复核** —— 用户侧执行（本窗口未编译）
- [ ] **demo 视觉冒烟（人眼）** —— `phase4_spatial` / `phase4_compositor` 相机生效判据
- [ ] **manual-view（`SetViewTransform`）命令链传输** —— 首个调用方出现时再补；当前 WARN 拒绝
- [ ] **屏级 dirty 门是否独立成对象** —— ADR-0007 §3 明确列为后续可选，未纳入本次