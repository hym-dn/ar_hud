# ADR-0008：外部重绘请求 → 强制全屏重渲（dirty 门的第二来源）

| | |
|---|---|
| 状态 | 已接受 |
| 日期 | 2026-10-02 |
| 关联 | skill/arhud-multiscreen-arch §3.7、skill/arhud-architecture §3.2、ADR-0002、ADR-0003、docs/devlog/2026-10-02_external_repaint_force_redraw.md |

## 1. 背景与问题

- B3「静态 HUD 每帧成本 ≈ 0」依赖 dirty 门：无内容变更 → 各屏 `IsScreenDirty` 为假 → 跳过渲染与上屏（`RenderViewport::RenderAndSubmit` 直接 return）。
- 该优化的**隐含前提**是"上屏表面稳定驻留"：GDI `StretchDIBits` 把位图拷到窗口表面、或 `SwapBuffers` 后 front buffer 驻留；停止拷贝/翻转则上一帧留在屏上。
- **缺口**：当**系统要求重绘**时（`WM_PAINT`：遮挡后揭开、最小化恢复、DWM 重置、显示模式切换）或**窗口尺寸变化**时（`WM_SIZE`），无人应答 → 未被重绘的无效区可能露出背景（`hbrBackground = COLOR_WINDOW`），而非最新画面。
- 另：`window_win32.h` 文档写有「`WM_PAINT` → 触发 `OnPaint`」，但 `WindowCallbacks` **实际没有 `OnPaint`** 字段、`HandleMessage` 也**未处理 `WM_PAINT`**（文档漂移）。
- 硬约束：窗口消息在**主线程**分发（`ARHudEngine::BeginFrame → DisplayServer::ProcessEvents → WndProc`）；而 `ItemStorage` 为**渲染线程独占**（同线程读写、无锁，见 ADR-0002 / `skill/arhud-architecture` §二）。因此"置屏位"**不能从窗口回调直写**。

## 2. 决策

**为 dirty 门增加第二来源：「外部重绘请求」。** 平台窗口把 `WM_PAINT` / `WM_SIZE` 作为"请求全量重绘"的信号上报，引擎据此令**渲染线程**强制全屏重渲一次。

机制（选定方案 A：原子标志 + 唤醒，不改命令层）：

1. **平台契约**：`WindowCallbacks` 增 `OnPaint`（`void (*)(uint32 id, void *userdata)`）；`win32` 新增 `case WM_PAINT:` → `BeginPaint/EndPaint` 验证无效区（避免 paint 循环）+ 触发 `OnPaint`。`WM_SIZE` 已有 `OnResize`。
2. **引擎（ARHudEngine）**：给可见窗口注册回调；`OnPaint` / `OnResize` → `RequestFullRedraw()`：置 `std::atomic<bool> force_redraw_` 并 `render_wake_.Post()`。主线程**只碰 atomic + semaphore**，线程安全。
3. **渲染线程循环**：`force_redraw_.exchange(false)` 为真 → `item_storage.MarkAllScreens()` 且令 `should_render = true`。
   **关键点**：渲染由 `submit_pending_` 门控，**单独置 dirty 不足以触发渲染**——外部重绘必须同时置 `should_render`。
4. **ItemStorage**：增 public `MarkAllScreens()`（= `MarkScreens(kAllScreensDirty)`），不改其余 dirty 语义。

关键不变量：
- **置屏位仍只发生在渲染线程**（`ItemStorage` 独占契约不破）——回调只置原子标志，真正 `MarkAllScreens()` 在渲染线程执行。
- dirty 门语义不变：`Mark*` 仍是唯一置位入口；外部重绘只是**新增一个在渲染线程调用 `MarkAllScreens()` 的来源**。

「全更新」粒度：任一窗口重绘请求 → 标记**所有屏**（HUD 1~3 屏；实现最简、最稳）。将来可细化为"仅该窗口对应的屏"。

## 3. 已否决方案

| 方案 | 否决理由 |
|---|---|
| 主线程回调里直接 `ItemStorage::MarkScreens(...)` | 违反「ItemStorage 渲染线程独占」；跨线程直写数据竞争 |
| 新增 `kForceRedraw` 渲染命令 + `RenderServer::ForceRedraw`（走命令链） | 可行且语义统一，但需新增命令枚举 + 结构体 + executor 分支 + 新 API，改动面更大；且外部重绘是**控制信号**（无数据、无返回值），用引擎级唤醒通道（`render_wake_`）更贴切 |
| 仅靠 app 每帧 `SubmitScene` 自然重渲 | 静态场景下 app 可能空闲/低帧率；外部重绘是**异步事件**，必须能自行唤醒渲染线程 |
| 不处理 `WM_PAINT`（纯依赖 DWM 驻留） | 未验证无效区会引发 paint 循环；系统丢弃/要求重绘时无法恢复 |
| 复用 `OnResize` 兼作"重绘"信号 | 语义错误（重绘 ≠ 改尺寸）；`OnResize` 已承载尺寸语义 |

## 4. 后果

- **正面**：外部重绘（遮挡/恢复/尺寸变化/DWM 事件）自动触发一次全屏重渲，消除"静态帧跳过后露背景"的风险；同时补全 `WindowCallbacks::OnPaint` 的文档漂移。
- **负面/代价**：`WindowCallbacks` 新增字段（源兼容的加法）；引擎新增 `force_redraw_` 原子 + 窗口回调注册；`WM_PAINT` 高频时可能触发多余全屏重渲（可接受——重绘本就该发生）。
- **中性约束**：`ItemStorage` 增 public `MarkAllScreens()`；文档中"dirty 门仅由内容变更置位"须改为"内容变更 ∪ 外部重绘请求"。
- **未覆盖（另行登记）**：真正的**尺寸重建**（FBO / 帧队列 resize）仍是 `resize_pending_` stub，本 ADR 不涉及；`Free` 后不置 dirty 亦不涉及。

## 5. 验收

- **grep**：`window_win32.cpp` 含 `case WM_PAINT`；`window.h` 的 `WindowCallbacks` 含 `OnPaint`；`arhud_engine.cpp` 含 `force_redraw_` 消费与 `MarkAllScreens()`。
- **单测**：既有 29 用例全绿（不改 dirty 语义）。
- **demo 行为**（人眼）：`phase4_spatial` / `phase4_compositor` 静态画面——遮挡再揭开、拖动改尺寸 → 画面正确恢复（不再露背景）；静止时不再持续重渲。

## 6. 实施记录（本窗口落地）

| 文件 | 改动 |
|---|---|
| `platform/window.h` | `WindowCallbacks` 增 `OnPaint` 回调 |
| `platform/windows/window_win32.cpp` | 新增 `case WM_PAINT:`（`BeginPaint/EndPaint` + 触发 `OnPaint`） |
| `servers/rendering/storage/item_storage.h/.cpp` | 增 public `MarkAllScreens()` |
| `servers/rendering/arhud_engine.h/.cpp` | 增 `force_redraw_` 原子 + `RequestFullRedraw()`；注册窗口回调；渲染循环消费标志 |
| `docs/devlog/2026-10-02_external_repaint_force_redraw.md` | 变更日志 |