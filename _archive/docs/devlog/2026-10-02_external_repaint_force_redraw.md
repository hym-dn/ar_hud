# 开发日志：外部重绘请求 → 强制全屏重渲（dirty 门的第二来源）

- 日期：2026-10-02
- 范围：`platform/window.h`、`platform/windows/window_win32.{h,cpp}`、`servers/rendering/arhud_engine.{h,cpp}`、`servers/rendering/storage/item_storage.{h,cpp}`
- 依据：**ADR-0008**（本窗口决策并落地）、`skill/arhud-multiscreen-arch` §3.7

## 1. 变更总览

| 项 | 落点 |
|---|---|
| `WindowCallbacks` 增 `OnPaint` | `platform/window.h` |
| `WM_PAINT` 处理（`BeginPaint/EndPaint` + 触发 `OnPaint`） | `platform/windows/window_win32.cpp` |
| `force_redraw_` 原子 + `RequestFullRedraw()` + 两个静态桥 | `servers/rendering/arhud_engine.h/.cpp` |
| 窗口回调注册（`OnPaint`/`OnResize`） | `arhud_engine.cpp` `AddScreen` |
| 渲染循环消费标志（`MarkAllScreens()` + `should_render`） | `arhud_engine.cpp` `RenderThreadFunc` |
| `ItemStorage::MarkAllScreens()`（public） | `storage/item_storage.h/.cpp` |
| 文档 | `docs/adr/0008`、`docs/adr/README.md`、`skill/arhud-multiscreen-arch` §3.7 |

## 2. 关键实现决策

1. **通道选「原子标志 + 唤醒」（ADR-0008 方案 A），不新增渲染命令**：外部重绘是**控制信号**（无数据、无返回值），无需造命令枚举 / 结构体 / executor 分支；引擎已有 `render_wake_` 唤醒通道，直接复用。
2. **主线程绝不触碰 `ItemStorage`**：窗口回调在主线程（`BeginFrame → ProcessEvents → WndProc`），而 `ItemStorage` 为渲染线程独占。回调只做 `force_redraw_.store(...)` + `render_wake_.Post()`；真正的 `MarkAllScreens()` 在渲染线程消费标志时执行。**顺序**：先 `store(release)` 后 `Post`。
3. **必须同时令 `should_render = true`**：渲染由 `submit_pending_` 门控，**单独置 dirty 不触发渲染**。这是最容易漏的一步——静态场景下只 `MarkAllScreens()` 而不置 `should_render`，重绘请求不会产生任何渲帧。
4. **`WM_PAINT` 必须 `BeginPaint/EndPaint`**：否则无效区不被验证 → `WM_PAINT` 反复投递（paint 循环）。本处理只"验证无效区 + 通知"，实际画面仍由 present 线程写入窗口表面。
5. **「全更新」= `MarkAllScreens()`（所有屏）**：按需求取最简最稳；窗口→屏索引映射的精细化留待后续（已登记）。

## 3. 排障记录

- **文档漂移（本次一并补全）**：`window_win32.h` 声称「`WM_PAINT` → 触发 `OnPaint`」，但 `WindowCallbacks` **实际无 `OnPaint` 字段**、`HandleMessage` **无该 case**。现已补上字段 + `case WM_PAINT`。
- **相关发现（未改，另行登记）**：`RenderViewport::HandleResize` 只置 `resize_pending_`/`new_width_`，**全仓无消费点** → 尺寸重建（FBO/帧队列 `Resize`）仍是 stub。本次只做「置屏位」，尺寸重建不在范围。

## 4. 验收结果

- 构建：**未执行**（按用户要求，编译交由用户）。
- 单测：预期 **29/29**——本变更不改 dirty 语义（仅新增 `MarkAllScreens()` 与外部触发通道）。
- demo：`phase4_spatial` / `phase4_compositor` 静态画面——遮挡后揭开、拖动改尺寸 → 画面正确恢复；静止时不再持续重渲（人眼验证，用户侧）。
- 治理 grep：`window_win32.cpp` 含 `case WM_PAINT`；`window.h` 的 `WindowCallbacks` 含 `OnPaint`；`arhud_engine.cpp` 含 `force_redraw_` 消费与 `MarkAllScreens()` ✔

## 5. 遗留（登记去向：skill/arhud-improvement）

- [ ] **编译 + 单测 + demo 人眼冒烟** —— 用户侧（本窗口未编译）
- [ ] **尺寸重建**：消费 `resize_pending_`（FBO / 帧队列 `Resize`）—— 既有 stub，另行登记
- [ ] **外部重绘精细化**：窗口 → 屏索引映射，仅标该屏（当前全屏）—— 登记
- [ ] `Free` 后不置 dirty（潜在陈旧帧）—— 既有登记，本次未变