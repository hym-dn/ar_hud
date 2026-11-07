# 开发日志：B5 清理（批 1）— 公有入口收敛与闲置 API 删除

- 日期：2026-09-27
- 范围：`servers/rendering/arhud_engine.h/.cpp`、`servers/rendering/render_screen.h/.cpp`、
  `skill/arhud-improvement`、`skill/arhud-multiscreen-arch`
- 依据：`skill/arhud-improvement` §B5（清理与优化）+ 上批 devlog 登记的 B5 遗留
- 变更分级 **A 类**：删除项全部零调用方，零行为变化

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 删 `ARHudEngine::GetCompositor()` / `GetRenderingDevice()`（公有访问器 + Doxygen） | `arhud_engine.h/.cpp` |
| 删 `RenderScreen::GetCompositor()` / `GetRenderingDevice()`（+ 定义） | `render_screen.h/.cpp` |
| 删 `RenderScreen::BeginFrame()` / `EndFrame()`（+ 定义） | `render_screen.h/.cpp` |
| 顺手修正 `render_screen.h` 公有段丢缩进（乱源） | `render_screen.h` |
| 修正 `ARHudEngine::BeginFrame/EndFrame` 的陈旧 Doxygen（"遍历 RenderScreen 调 BeginFrame"） | `arhud_engine.h` |
| 纠正两条陈旧登记 + 勾销 B5 项 | `arhud-improvement`、`arhud-multiscreen-arch` |

## 2. 关键决策

1. **B5 拆两批**：本批只做"零调用方、零行为变化"的收敛；**框架性改动留批 2**——
   后处理独立子系统（从 RenderScreen 拆 `ARHudCompositeRenderer`，P2 ~1 天）、
   `ShaderCreateFromGLSL` → `ShaderCreateFromSource` + `ShaderSourceFormat`（P1 契约改动，~0.5 天）、
   RD 跨线程 frame slot 简化（线程语义）、`CmdBufferUpdate` 深拷贝优化（性能）。
   这些动到契约/线程/性能，需单独评审，不混入本批以免降低可回滚性。
2. **删除前逐项确认调用方**：`GetCompositor`/`GetRenderingDevice` 在 `app/` 与引擎内部均**零代码命中**
   （`verification.md` 治理 grep 早已作为基线）；`RenderScreen::BeginFrame/EndFrame` 零调用方，
   且其函数体直接调 `rd_->BeginFrame()`，与渲染线程 `RenderThreadFunc` 的 `rd.BeginFrame()`
   **重复驱动**——不只闲置，属**误导性 API**，一并删除。
3. **语义更正**：`RenderScreen::BeginFrame/EndFrame` 的删除使 `ARHudEngine::BeginFrame/EndFrame`
   的 Doxygen（"遍历所有 RenderScreen，调用 BeginFrame()"）变成假陈述，同步改述为
   "首次启动渲染线程 / 仅推进 Compositor 帧节拍"。

## 3. 排障记录（陈旧登记纠错）

审计发现 `arhud-improvement` 里的两条**"待删"登记本身是陈旧的**：
- `RenderingDevice::ExecuteFrame/OnFrameCompleted` 函数体 —— devlog `2026-09-03_B3_render_thread.md`
  记录**当日已删**；全仓 grep 零命中（仅剩 `rendering_device_frame.cpp:169` 一条
  "原 deferred 分支已随 ExecuteFrame 退役" 注释）。
- `SetDeferredExecution` / `deferred_execution_` —— 同样 2026-09-03 随 B3 移除，代码零命中。

即"滞留的不是代码，而是文档"。教训：**roadmap 的"待办"也要 grep 复核**，否则会拿着已完成的事
反复排期（与上批"把 CanBypassQueue 误判为死代码"是同一类错误的镜像）。

## 4. 未执行（如实说明）

`canvas_renderer.h::clip_rect` 死字段本轮**仍删除不了**：该结构体第 24–32 行**行尾带大量空白**
（且 `nine_patch_margin`/`};` 丢缩进），工具做精确文本替换不可靠，强行编辑有写坏文件风险。
建议随一次 **clang-format 全仓格式化**一并处理（可同时解决 `render_screen.h`、`canvas_renderer.h`
的丢缩进），而非手工逐行删。

## 5. 验收结果

- 构建：全量绿（5 lib + 5 exe + `arhud_tests`），`cmake --build build --config Debug` 退出码 0
- 单测：**25/25 通过**（`ctest --test-dir build -C Debug`）
- 残留校验：`GetCompositor|GetRenderingDevice|RenderScreen::BeginFrame|RenderScreen::EndFrame`
  在 `*.h/*.cpp` **零命中**
- demo：未单独冒烟——删除项均零调用方（零行为变化），编译已覆盖

## 6. 遗留（B5 批 2，需单独评审）

- [ ] 后处理独立子系统：从 RenderScreen 拆出 `ARHudCompositeRenderer`（P2，~1 天）
- [ ] `ShaderCreateFromGLSL` → `ShaderCreateFromSource` + `ShaderSourceFormat` 枚举（P1 契约，~0.5 天）
- [ ] RD 跨线程 frame slot 机制简化（保留 frame_done 帧节拍）
- [ ] `CmdBufferUpdate` 深拷贝 → 引用 Staging Buffer 指针（性能）
- [ ] `clip_rect` 死字段 + 多处丢缩进 → 随 clang-format 全仓格式化