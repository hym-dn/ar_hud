# 开发日志：清理死代码与迁移残骸（frame_scene / ring_buffer / current_slot_ / kPresent）

- 日期：2026-09-27
- 范围：`servers/rendering/frame_scene.h/.cpp`（删除）、`core/template/ring_buffer.h`（删除）、
  `core/CMakeLists.txt`、`servers/rendering/render_screen.h`、`servers/rendering/render_commands.h`、
  `servers/rendering/command_executor.cpp`、根 `AGENTS.md`、`README.md`、
  `skill/arhud-architecture`、`skill/arhud-render-thread-arch`、`skill/arhud-multiscreen-arch`、
  `skill/arhud-improvement`
- 依据：代码评审「架构设计的乱七八糟的，怎么回事」——诊断后的清死代码行动；
  变更分级 **A 类**（删除项均无调用点，零行为变化）

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 删除孤儿文件 `frame_scene.h/.cpp`（B3-M0.4 后无 include、未在 CMake） | `servers/rendering/` |
| 删除 `core/template/ring_buffer.h`（无任何源文件 include）+ 构建列表同步 | `core/`、`core/CMakeLists.txt` |
| 删除 `RenderScreen::current_slot_`（B3 起闲置成员） | `render_screen.h` |
| 删除退役命令 `RenderCommandType::kPresent` + Executor 分支 + 类型注释 | `render_commands.h`、`command_executor.cpp` |
| 文档同步：断链修正 + 假陈述纠正 + roadmap 勾销 | `AGENTS.md`、`README.md`、4 份 skill |

## 2. 关键实现决策

1. **先审计、再删除**：审计纠正了两处**上轮误判**——
   - `RenderServer::CanBypassQueue()` **不是死代码**：40+ 处在用，且被 `verification.md` /
     `dev-workflow.md` / `ADR-0001` 明文规定为"渲染线程内防死锁短路"（新代码不得在渲染线程发
     PushAndSync）。**保留**。
   - `RenderScreen` 的 7 个共享栈指针（rd/command_queue/command_executor/item_storage/
     render_server/compositor/context_driver）**不是残骸**：成员上方注释即写明
     「B3-M0.1：从值改为指针，所有权归 ARHudEngine::Impl」，是"Engine 级单共享栈 + 轻量视口"
     的设计本体。**保留**。
2. **只删"零引用"项**，逐项以 grep 证据为准：frame_scene（CMake 与 include 均零命中）、
   ring_buffer（无 include）、current_slot_（仅声明）、kPresent（无 Push 点，仅 Executor 分支）。
3. **文档按"是否变成假陈述/断链"同步**：`arhud-render-thread-arch` 指向 `frame_scene.h` 的
   链接会断 → 改述为"已删除"；"kPresent 退役为 WARN"、"current_slot_ 闲置待 B5 删" 等假陈述 → 更新；
   `arhud-multiscreen-arch` 实施步骤 4（删 FrameScene）→ 勾销。纯历史设计叙述（render-thread-arch §2.2）
   按"保留历史"原则不动，仅补删除标记。

## 3. 排障记录（踩坑留档）

- **"看起来是残骸、其实不是"**：`RenderScreen` 的 7 个指针成员名字像"重构残留"（一个视口却持有
  RD/Compositor/ItemStorage 等），但注释与调用点证明它们是 B3-M0 有意为之的共享栈引用。
  若凭直觉删除 → 直接编译失败。
  教训：**判定"残骸"必须看成员上方注释 + 调用点**，不能按名字或"像遗留"的直觉；
  `CanBypassQueue` 同理（名字像"临时旁路"，实为治理规则明文要求的短路语义）。

## 4. 验收结果

- 构建：全量绿（5 lib + 5 exe + `arhud_tests`），`cmake --build build --config Debug` 退出码 0
- 单测：**25/25 通过**（`ctest --test-dir build -C Debug`）
- demo：未单独冒烟——删除项均无调用点（零行为变化），编译已覆盖正确性
- 治理 grep：`frame_scene` / `FrameSceneBuffer` 在 `app/`、`servers/` 源码零命中；
  `RingBuffer` / `ring_buffer.h` 零 include 命中；`current_slot_` 零命中；`kPresent` 代码零命中
  （仅文档保留"已删除"记录）

## 5. 遗留（登记去向：skill/arhud-improvement）

- [ ] `canvas_renderer.h::clip_rect` 死字段 + 该结构体的丢缩进（行尾空白致工具精确替换不可靠）
  —— B5 批次
- [ ] RD 中 `ExecuteFrame/OnFrameCompleted` 函数体仍在（无调用方）—— B5 删
- [ ] RD BeginFrame 的 deferred 等待路径失效（可删）—— B5
- [ ] `RenderScreen::BeginFrame/EndFrame` 公有方法闲置待审 —— B5
- [ ] 命名收敛：`PipelineStageBits` 同名不同义（commons 18 值 vs RDD 4 值）、三套 ID 体系
  （RID/RDBufferID/BufferID）—— C 类/P2，须方案先行
- [ ] 空 `scene/` 库占位（Phase 6 场景树用）—— 保留，非死代码