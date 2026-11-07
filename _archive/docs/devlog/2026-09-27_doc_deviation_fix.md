# 开发日志：修复评审暴露的文档偏差（文档/代码漂移批次修正）

- 日期：2026-09-27
- 范围：根 `AGENTS.md`、`README.md`、`docs/adr/0005-item-storage-tree.md`、
  `skill/arhud-improvement`、`skill/arhud-architecture`、`skill/arhud-render-thread-arch`、
  `skill/arhud-multiscreen-arch`、`core/CMakeLists.txt`、`drivers/command_buffer.h`、
  `drivers/gl/rendering_device_driver_gl.h`、`drivers/rendering_device_driver.h`、
  `servers/rendering/arhud_engine.h`
- 依据：代码评审「给出全部设计细节」过程中暴露的文档偏差；变更分级 **A 类**（零行为变化，纯注释/文档）

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 幻影文件 `view.h`/`view_win32` 删除，补 `frame_presenter.h`/`frame_presenter_win32` | `AGENTS.md`、`README.md` |
| 基础类型名 `u8/u32/f32` → `uint8/uint32/float32` | `AGENTS.md`、`skill/arhud-architecture`、`core/CMakeLists.txt` |
| platform 依赖补 `gdi32` | `AGENTS.md` |
| README 目录树重写（platform/drivers/servers/app）；合同头归位 drivers 根；`redering` 拼写 | `README.md` |
| 构建选项默认值 `ARHUD_BUILD_TESTS`→ON、`ARHUD_BUILD_DEMO`→ON（对齐 `cmake/option.cmake`） | `README.md` |
| roadmap 状态回填：B3-M0.3 ✅、头部状态行、基线表新增 B3-M0/B3/ADR-0005、测试 6→25 | `skill/arhud-improvement` |
| ADR-0005 `z_as_relative` 自相矛盾修正（删除线 + 注记，保留原始否决记录） | `docs/adr/0005-item-storage-tree.md` |
| arhud-architecture 滞后：§3.2 删 FrameScene 行、§五 测试 6→25 | `skill/arhud-architecture` |
| `arhud-arch-governance` 幻影 skill 引用 → 指向根 `AGENTS.md` + `docs/process` | 4 份 skill |
| CommandBuffer 线程契约：`内部 Mutex` → `单线程独占 + atomic state_`（对齐 GL 实现，含 18 处逐方法 tag） | `drivers/command_buffer.h`、`drivers/gl/rendering_device_driver_gl.h` |
| `IFramePipeline` 家族 → `IFramePresenter`（与同文件他处一致） | `drivers/rendering_device_driver.h` |
| `arhud_engine.h` 类体文档：退役的 `SwapBuffers`/`Compositor->CanvasItemPush()` 改述 | `servers/rendering/arhud_engine.h` |

## 2. 关键实现决策

1. **ADR 只增不改**：`docs/adr/0005` 的 `z_as_relative` 矛盾不删原文，改用删除线 + 评审决策注记，
   保留"原始否决记录"的历史可追溯性（符合 `docs/adr/README.md` 只增不改规则）。
2. **设计期名 vs 实现名**：多屏 §3.9 的 `IFramePipeline` 为设计期命名，实现定名 `IFramePresenter`。
   采用"加命名对齐注记"而非全文改名，避免大范围文档改动引入新失真。
3. **契约注释对齐实现而非反之**：CommandBuffer 契约原称"内部 Mutex"，GL 实现实为
   `atomic<CommandBufferState>` + 单占用。以代码为准修正文档（契约摘要表 + 18 处逐方法 `@thread_safety`）。
4. **幻影引用收敛到真实权威**：治理铁律实际散落在根 `AGENTS.md` 与 `docs/process/`，
   无独立 governance skill，故 4 份 skill 的引用统一改为指向真实位置。

## 3. 排障记录（踩坑留档）

- **批量替换误加 UTF-8 BOM**：用 `[System.IO.File]::WriteAllText` + `New-Object UTF8Encoding($true)`
  批量替换 `command_buffer.h` 的 18 处注释后，文件凭空多了 BOM（`EF BB BF`），而本工程
  惯例是**无 BOM**（`format-hex` 抽查 `command_buffer.h` 原为 `47,42,42`、`command_buffer_gl.h`
  等均为 `47,42,42`，`AGENTS.md`/`README.md` 为 `35,32,65`）。
  → 根因：`WriteAllText` 的 `UTF8Encoding($true)` 显式带 BOM，代码未先探测源文件是否含 BOM。
  → 修复：改用 `New-Object System.Text.UTF8Encoding($false)` 重写，BOM 还原为 `47,42,42`。
  教训：**批量文本替换前先 `format-hex` 探测 BOM 与编码**，写回时用与源文件一致的
  `UTF8Encoding($有/无BOM)`；这正是 `verification.md` 坑 #3「编码事故」的延伸——
  用 `[System.IO.File]` 还不够，还需匹配 BOM 策略。改完必须抽查中文注释与首字节。

## 4. 验收结果

- 构建：全量绿（5 lib + 5 exe + `arhud_tests`），`cmake --build build --config Debug` 退出码 0
- 单测：**25/25 通过**（`ctest --test-dir build -C Debug`，含 `ItemStorageTree` 14 例）
- demo：未改渲染路径，本次未单独冒烟（A 类注释变更，构建已覆盖编译正确性）
- 治理 grep：`view.h`/`view_win32`/`u8/u32/f32`/`gl_manager.h`/`redering` 仅剩真实文件
  `vector_view.h`（非违规）；`command_buffer.h` 内 `内部 Mutex` 已零命中；`command_buffer.h`
  首字节 `47,42,42`（无 BOM），中文注释抽查完好

## 5. 遗留（登记去向：skill/arhud-improvement）

- [x] README 重写（P3 #360）—— 本次完成（目录树 + 构建选项默认值）
- [x] skill/arhud-architecture 滞后修正（P3 #363）—— 本次完成
- [x] arhud_engine.h 类体文档滞后（P3 #364）—— 本次完成
- [ ] demo main.cpp 乱码注释清理（P3 #365）—— 编码事故遗留，按"触碰时顺手清理"不做主动修改
- [ ] `conanfile.py` `validate()` 使用未导入的 `ConanInvalidConfiguration`（B6，**代码缺陷**非文档）—— 待 B5 批次
- [ ] 文字完整变换（旋转/缩放）缺失（评审 A1/C2）—— C 类，须方案先行（skill + ADR）
- [ ] `canvas_renderer` 的 `clip_rect`/`nine_patch` 死能力（评审 C3）—— 补 API 或删除，B5 批次