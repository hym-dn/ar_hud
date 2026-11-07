# 开发日志：RenderScreen → RenderViewport 重命名（术语对齐 Godot）

- 日期：2026-09-27
- 范围：servers 层类名/文件名/类型/成员 + 现行文档（AGENTS/README/4 份 skill）
- 变更分级 **B 类**：机械重命名，零行为变化

## 1. 动机：一个词背了两个概念

本工程把 Godot 的 **Viewport**（渲染目标）叫成了 **Screen**，与 Godot 语义下的 **screen**（显示器）
撞名。后果是同名不同物、可读性差：

| 概念 | 层 | 代表（改名前 / 改名后） |
|---|---|---|
| 显示器（几何/DPI/HMONITOR） | platform | `IScreen` / `ScreenInfo` / `Win32Screen`（**不变**，对齐 Godot `DisplayServer.screen_*`） |
| 渲染视口（FBO/cull/camera/present） | servers | `RenderScreen` → **`RenderViewport`**（对齐 Godot `Viewport`） |

## 2. 变更总览

| token | 改前 | 改后 |
|---|---|---|
| 类名 | `RenderScreen` | `RenderViewport` |
| 文件 | `render_screen.h/.cpp` | `render_viewport.h/.cpp`（**连带改**，否则文件名≠类名） |
| 成员 | `screen_id_` | `viewport_index_` |
| 方法 | `SetScreenID()` | `SetViewportIndex()` |
| 类型（**仅 servers**） | `ScreenID` | `ViewportID` |

命中：223 处 / 33 文件 → 实际改写 24 个在范围内文件 + 2 个文件改名。

## 3. 三个关键判断

1. **类名与文件名一起改**：只改类名会造出"`render_screen.h` 里定义 `RenderViewport`"——正是我们刚在
   `ustring.h`↔`String` 上批评过的"文件名≠类名"。故一并改名，并同步 `servers/CMakeLists.txt`。
2. **`ScreenID` 是二义的，禁止盲替**：servers 里有两种 `ScreenID`——
   - `arhud_engine.h` 的 `using ScreenID = RenderScreen::ID`（**视口 id → 改**）
   - `arhud_engine.cpp:197` 的 `IScreen::ScreenID`（**显示器 id → 不动**）
   采用**保护哨兵**：先把 `IScreen::ScreenID` 换成占位符 → 全局替 `ScreenID`→`ViewportID` → 再还原。
   复核：servers/app/tests 内 `ScreenID` 残留**恰好只剩 2 处 `IScreen::ScreenID`**（platform 语义），正确。
3. **历史记录不改**：`docs/devlog/**`（约 35 处）与 `docs/adr/0002`（3 处）是历史（"B3-M0 前每个
   RenderScreen 按值持有一整套渲染栈"当时是事实）——改了即篡改历史，且 ADR「只增不改」。
   另：仓库根的游离日志 `canvas_err.log` 未动。

## 4. 刻意保留（不属"视口"，指"屏幕/窗口"概念）

`GetScreen()` / `AddScreen()` / `GetScreenCount()` / `ScreenConfig` / `GetPrimaryScreen()` /
`IScreen` / `ScreenInfo` / `kInvalidScreenId` —— 这些仍指"屏幕（窗口/显示器）"，语义正确，不动。

## 5. 验收

- 构建全量绿：5 lib + 5 exe + `arhud_tests`；**ctest 25/25（100%）**
- servers/app 内 `render_screen|RenderScreen|screen_id_|SetScreenID` 残留 **0**
- 24 个改写文件编码抽查：无 BOM（首字节 `47,42,42` / `2F,2A,2A` 等）

## 6. 附带发现（本次未修，独立报告）

**per-screen 相机/灯光经命令链不可达** —— 本重命名过程中核对相机命令时发现：

- `RenderServer` 只暴露 `Camera3DSet(const CameraMatrix&)` / `Camera2DSet(const Transform2D&)` /
  `DirectionalLightSet(const DirectionalLight&)`——**没有带屏参数的版本**；
- 命令结构体 `Camera3DSetCmd` / `Camera2DSetCmd` / `DirectionalLightSetCmd` **不含 screen 字段**；
- `CommandExecutor::ExecCamera3DSet` 落到 `item_storage_->Camera3DSet(cam)`——即**screen 0 便捷重载**。

即：B3-M0.4 已把 per-screen 相机/灯光的**存储**做好（`ScreenState` + `GetCamera3D(viewport_index_)`），
但**管道缺失**——screen≠0 永远拿不到被设置的相机（读零值 `ScreenState{}`）。多屏 demo 会出现
"两屏同一相机 / 第二屏相机异常"。

`arhud-multiscreen-arch` 的设计稿本就有 `Camera3DSet(ScreenID, …)` 重载，**未落地**。

建议独立修复（B 类）：命令结构体加 `uint32 viewport_index`；`RenderServer` 加 `(ViewportID, …)` 重载；
便捷重载转调 0；补 per-screen 相机单测。

## 7. 遗留

- [ ] 上述 per-screen 相机/灯光管道缺失（建议尽快，影响多屏正确性）
- [ ] `skill/arhud-architecture` §一 platform 行仍含幻影 `view`（应为 `frame_presenter`）——历史遗留文档偏差，未在本次范围
- [ ] 建议：为仓库**建立 git**（无版本控制使此类大范围重命名无法回滚）