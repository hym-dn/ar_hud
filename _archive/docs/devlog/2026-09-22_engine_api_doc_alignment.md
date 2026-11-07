# 开发日志：引擎 API 文档对齐（RenderFromStorage 转私有 + ARHudEngine 示例更新）

- 日期：2026-09-22
- 范围：`servers/rendering/render_screen.h`、`servers/rendering/arhud_engine.h`
- 依据：`skill/arhud-improvement` B3（L267/L321 声称「RenderFromStorage 已转私有」，实际代码仍为 public）；`docs/process/dev-workflow.md`

## 1. 变更总览

| 项 | 落点 |
|---|---|
| #1 `RenderFromStorage()` 由 public 转 private（补 B3 未做项） | `render_screen.h` |
| #3 `ARHudEngine` 文件头「方式 A / 方式 B」示例对齐 B3 实际 API | `arhud_engine.h` |

## 2. 关键实现决策

1. **`RenderFromStorage()` 转私有**：全仓唯一调用点为 `render_screen.cpp` 内部 `RenderAndSubmit()`（渲染线程内），`app/` 零代码命中（治理 grep ① 仅 7 处整行注释）。无外部调用者 → 无需 `friend`，编译通过即为访问性收紧的证明。理由：`arhud-improvement` B3 交付物明确要求转私有，且其 Doxygen 早已声明「渲染线程独占调用、主线程直调已退役」，public 属遗漏。
2. **文件头示例改写**：`Config` 自 B3-M0.2 起已无 `title/width/height`（移入 `ScreenConfig`），原示例 `cfg.title/width/height` 无法编译；`GetCompositor()->Render(...)` 亦非 app 路径。改为 `Initialize(Config) → AddScreen(ScreenConfig) → GetRenderServer() → 循环 { BeginFrame → rs->SubmitScene → EndFrame }`，与 4 个 demo 实际用法一致。
3. **「方式 B：IAppLoop（推荐）」标签改写**：`IAppLoop` / `engine.Run()` 全仓**零调用者**（4 demo 均手动帧循环）。`Run()` 内部为 `BeginFrame → OnUpdate → OnRender → EndFrame`，**不**自动提交帧，故示例在 `OnRender` 显式调 `SubmitScene()`；标签改为「可选」，方式 A 标注「demo 采用」。

## 3. 排障记录（踩坑留档）

- **现象**：`phase4_canvas` 冒烟命令返回退出码 1，但 demo 自身日志显示完整跑完并 clean exit。
  排查：日志尾部为 `Render loop exited after 6000 frames` + `=== Phase 4 Canvas Demo exited cleanly ===`；命令日志中另有 `TRAE Sandbox Error: hit restricted ... NVIDIA\GLCache\...`。
  根因：受限沙箱拦截了 GL 驱动写 NVIDIA shader cache 目录（`%LOCALAPPDATA%\NVIDIA\GLCache`），在进程收尾阶段返回非零码，**非程序错误**。
  教训：Windows GL demo 在受限沙箱下会因驱动缓存写入被 deny 而带非零退出码；判读成败应以程序自身日志（`Render loop exited after N frames` / `exited cleanly` / 零 ERROR）为准，不看外层 shell 退出码。

## 4. 验收结果

- **构建**：全量绿（5 lib：core/scene/platform/drivers/servers + 5 exe），exit 0，无新增 warning
- **单测**：25/25 通过（CommandQueue 6 + ItemStorageTree 19），总耗时 1.79s
- **demo 冒烟**：`phase4_canvas` 6000 帧 @60Hz（≈100s）干净退出；日志零 ERROR；teardown 完整（窗口销毁 ×2 → GLContextManager Shutdown ×2 → `ARHudEngine finalized`）
- **治理 grep**：① 注释命中 7 处（全部整行注释，与 2026-09-19 基线一致，**零代码命中**）/ ② 0 / ③ 0

## 5. 遗留（登记去向：skill/arhud-improvement）

- [ ] `arhud_engine.h` 类体其余过时描述：`Run()` / `BeginFrame()` / `EndFrame()` 的 Doxygen 仍称「SwapBuffers」（B3-M0.3 已退役）；`IAppLoop` 类文档示例/`@note` 仍提 `Compositor->CanvasItemPush()`（B2-R 已退役）。本次未夹带修改，已登记 P3 —— 文档治理
- [ ] 根目录残留 `canvas_out.log`（0 字节）/ `canvas_err.log`（511 KB）—— 2026-08-23 B2-R 时期手工重定向产物，非本次产生；按 `verification.md` §4 属验证后清理项