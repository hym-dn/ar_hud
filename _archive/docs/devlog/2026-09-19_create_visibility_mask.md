# 开发日志：ItemCreate 参数化 visibility_mask（创建即带归属）

- 日期：2026-09-19
- 范围：servers/rendering/storage/item_storage + render_commands + command_executor +
  render_server + tests/rendering/test_item_storage
- 依据：变更分级 B 类（行为兼容的功能增强）；源于用户对 dirty 路由的追问——
  `CanvasItemCreate` 标全屏是否合理的分析（见对话记录），评审决策"现在就实现参数化"

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 三个 Create 加默认参数 `uint32 p_visibility_mask = 0xFFFFFFFF` | `item_storage.h/.cpp`、`render_server.h/.cpp` |
| 创建即带归属：`data.visibility_mask = p_mask` + `MarkScreens(p_mask)`（原为固定 `kAllScreensDirty`） | `item_storage.cpp` ×3 |
| 三个 `XxxItemCreateCmd` 加 `uint32 visibility_mask = 0xFFFFFFFF` 字段 | `render_commands.h` |
| `ExecXxxCreate` 透传 `p_cmd.visibility_mask` | `command_executor.cpp` ×3 |
| Create 窄掩码路由回归测试 ×3 | `tests/rendering/test_item_storage.cpp` |

## 2. 关键实现决策

1. **默认参数保兼容**：无参调用（4 demo 全部调用点）行为完全不变（全屏归属 = 全屏标脏）；
   窄掩码创建是纯增量能力。
2. **为什么 Create 参数化才能省干净**：时序推演证明"创建后马上 SetVisibilityMask 收窄"
   救不回来——收窄动作会把**旧掩码（全屏）**标脏（"从全屏离开"也是全屏变更）。
   唯一不触发全屏标记的路径就是创建时直接带窄掩码。
3. **保守语义不变**：无参路径仍标全屏（新 item 默认全屏可见）——"宁可多渲不漏帧"
   哲学不受影响；参数化只是给窄屏场景一条精确路由的通道。

## 3. 排障记录

（无——一次通过。调用点扫描先行确认了改动范围：所有 app/tests 调用均为无参形式。）

## 4. 验收结果

- 全量构建：零 error / 零 warning
- 单测：**25/25 全绿**（+3：CreateWithNarrowMaskRoutesOnlyThoseScreens /
  CreateDefaultMaskKeepsAllScreensDirty（向后兼容回归）/ CreateNarrowMaskSpatialAndText）
- demo 冒烟：phase4_text exit=0 / 零 ERROR / 干净退出（Create→PushAndSync→Executor
  全链路经过修改的命令结构体）
- 治理 grep：①②③ 全部零代码命中

## 5. 遗留（登记去向）

- [x] 无新增遗留。测试计数 22 → 25 已同步 verification.md / definition-of-done.md / AGENTS.md
