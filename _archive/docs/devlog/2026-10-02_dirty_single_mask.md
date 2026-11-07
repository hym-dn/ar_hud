# 开发日志：dirty 合并为单一屏位掩码（删 `dirty_`，再无 `SetDirty`）

- 日期：2026-10-02
- 范围：`servers/rendering/storage/item_storage.h`、`item_storage.cpp`（删 29 处 `SetDirty()`）+ `render_server.cpp`（4）+ `command_executor.cpp`（4）；注释同步 `arhud_engine.cpp`、`tests/rendering/test_item_storage.cpp`
- 变更分级 **B 类**（内部数据结构简化；无契约 / 线程 / 分层变化，无方案先行义务）
- 依据：用户评审「`dirty_` & `dirty_screen_mask_` 保留一个不行么」→（间经「方案 C 换名重构」）→ 最终裁定「**方案 B：删 `SetDirty` 与断言，追求极简**」；`item_storage.h` §dirty 路由注释；`skill/arhud-multiscreen-arch` §3.7

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 删除 `std::atomic<bool> dirty_` | `item_storage.h` |
| `IsDirty()` 改为派生（`mask != 0`） | `item_storage.h` |
| **删除 `SetDirty()`**（含 debug 断言） | `item_storage.h` |
| `ClearDirty()` 只清掩码 | `item_storage.h` |
| `IsScreenDirty()` 去掉全局门分支 | `item_storage.h` |
| 删 37 处 `SetDirty()` 调用点 | `item_storage.cpp`(29) / `render_server.cpp`(4) / `command_executor.cpp`(4) |
| 注释同步 | `item_storage.h/.cpp`、`arhud_engine.cpp`、`test_item_storage.cpp`、`skill` |

## 2. 关键实现决策

1. **合并的等价性依据**：代码里**每一处 `SetDirty()` 都伴生一次 `Mark*`**（37 处逐一核对），故 `mask != 0 ⟺ 旧 dirty_`。删除 `dirty_` 不改变既有行为。
2. **`dirty_` 删除**：单一 `dirty_screen_mask_` 兼任"本轮是否要渲"（`mask != 0`）与"哪些屏要渲"（各屏位）；并**移除跨字段内存序约定**（旧方案需"先 `Mark*`(release) 后 `SetDirty`(release)"才能让读者的 acquire 顺带看到掩码）。
3. **`SetDirty()` 彻底删除（最终方案 B）**：`Mark*` 成为**唯一置位入口**。此前一版（方案 C）曾将其降级为"提交兜底 + debug 断言"，经权衡改为删除，理由：
   - 该兜底在 30+ 处调用中 **100% 是空操作**（Mark 已置位）；
   - 断言检测能力有限——同帧内"首个变更已置位、第二个漏标"检不到；且对零掩码 item（`Create(0)`）**误报**；
   - 兜底的"宁可多渲"实为掩盖 bug 的成本，而非修复。
   → 改为以**契约**约束：见 `item_storage.h` dirty 段——"任何状态变更路径**必须**至少 `Mark*` 一个屏位"。
4. **代价（显式登记）**：失去"漏标 → 保守全屏重渲"的 **fail-safe**；此后漏标表现为**静默漏渲**。当前所有路径均已核对为 Mark，风险以契约 + 代码评审控制。

## 3. 实施要点

- 批量删除调用行采用 `[System.IO.File]::ReadAllText/WriteAllText` + **显式 UTF-8**（规避 `docs/process/verification.md` 已知坑 #3「中文编码事故」）；保留原换行与编码（三文件均无 BOM，改动前后一致）。

## 4. 验收结果

- 构建：**未执行**（按用户要求，编译交由用户）。
- 单测：预期 **19/19**（`test_item_storage`）——`IsDirty` / `IsScreenDirty` / `ClearDirty` 语义不变（`IsDirty` 现为推导值）。
- 治理 grep：全仓已无 `SetDirty()` 调用（仅个别历史文档提及）。
- demo：不涉及。

## 5. 遗留（登记去向：skill/arhud-improvement）

- [ ] **编译 + 单测复核** —— 用户侧执行（本窗口未编译）
- [ ] `Free` 后不置 dirty（潜在陈旧帧）—— 既有登记，本次未变