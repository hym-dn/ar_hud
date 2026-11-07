# 2026-10-02 — ADR-0009 命令自派发（POD thunk）M1 落地

| | |
|---|---|
| 日期 | 2026-10-02 |
| 分级 | **C 类（分层/契约）** — 命令分派机制 |
| 关联 | `docs/adr/0009-command-self-dispatch.md`、`skill/arhud-render-thread-arch` §3.3、ADR-0001、ADR-0006 |
| 编译 | **未编译**（用户自行编译验证） |

## 背景

命令分派集中在 `CommandExecutor::ExecuteCommand` 的 `switch`（59 `case` / 42 `Exec*`）。新增命令需改动
`{render_commands.h, command_executor.h, command_executor.cpp, render_server.cpp}` 四文件，且 `switch` 为并行编辑热点。

方案先行：`docs/adr/0009-command-self-dispatch.md` 定"**POD 自派发**"——命令头携带函数指针 thunk，`Flush` 直接调用；
命令结构体自带 `Execute()`。本窗口落地 **M1**：机制 + 视口域（相机/灯光 4 条）首批迁移。

## 改动清单

| 文件 | 改动 |
|---|---|
| `servers/rendering/command_queue.h` | 增 `CommandExecuteThunk` + `command_detail::HasExecute`/`CommandThunk`；`CommandHeader` 8→16B（+`exec`）；`kAlignment` 8→16；`Push<T>`/`PushAndSync<T>` 用 `if constexpr` 绑定 thunk；`WriteCommand` 增 `p_thunk` |
| `servers/rendering/command_queue.cpp` | `WriteCommand` 写 `header.exec`；`Flush` 优先 thunk（已迁移命令）否则回退 switch；`kFunctionCall` 传 null thunk；回绕注释同步 |
| `servers/rendering/render_commands.h` | `Camera3DSetCmd`/`Camera2DSetCmd`/`DirectionalLightSetCmd`/`DirectionalLightClearCmd` 增 `void Execute(ICommandExecutor&) const` |
| `servers/rendering/command_executor.h` | `ResolveViewport` 提为 **public**；增 `GetItemStorage()`；删 4 个 `ExecCamera*/ExecDirectional*` 声明 |
| `servers/rendering/command_executor.cpp` | 删 4 个 `switch case`；4 个 `Exec*` 体**移入**对应命令的 `Execute()` 定义 |
| `tests/rendering/test_command_executor.cpp` | `Dispatch()` 由 `ExecuteCommand(...)` 改为 `cmd.Execute(executor)` |

## 关键决策 / 不变量

1. **命令头 8B → 16B，连带 `kAlignment` 8 → 16**。原因：回绕填充区 = `kCapacity − offset`，原不变量
   "offset 为 8 倍数 ⇒ pad∈{0}∪[8,∞)" 仅够容 8B 头；头 16B 后需 offset 为 16 倍数 ⇒ pad∈{0}∪[16,∞)，
   否则小 pad 无法容纳填充头（写越界）。命令头字段：`type/sync_idx/size/reserved/exec`。
2. **命令仍 POD**：`Execute` 为非虚成员函数，不影响 `trivially_copyable`，全部 `static_assert` 保留。
3. **`Execute` 取 `ICommandExecutor&`**（而非具体类）→ `render_commands.h` 不依赖 `CommandExecutor`；
   thunk 实体与 `Execute` 定义处做一次受控 downcast（thunk 只会收到 `CommandExecutor`）。
4. **迁移期双路径**：`Push<T>` 经 `HasExecute<T>` 检测决定是否绑定 thunk；未迁移命令 thunk 为 null →
   `Flush` 回退 `ExecuteCommand` switch。**行为等价**（thunk 体与 `Exec*` 体逐一对应）。
5. **发射侧零改动**：`RenderServer` 未改一行（`Push<T>` 编译期已知 T，自动绑定）。

## 行为等价性

`Flush` 对已迁移命令：`header.exec(payload, size, *executor_)` → `cmd.Execute(*executor_)` → 原 `Exec*` 体；
未迁移命令：thunk==null → 原 `switch` 路径。`kPadding`/`kFunctionCall` 仍走 `switch`。功能语义不变。

## 未完成（M2 / M3）

- **M2**：迁移 Item 域（~28）与资源域（~13）命令，其 `Exec*` 体移入各自 `Execute()`。
- **M3**：switch 清空后删除 `ExecuteCommand` 与 `command_detail::HasExecute`（改为无条件绑定 thunk），
  并评估 `ICommandExecutor` 是否收窄。
- **M1 期间** "新增命令不动执行器" 尚未完全成立（未迁移命令体仍在 `CommandExecutor`）——M3 后才成立。

## 诚实修正记录

起草/讨论过程中曾把一段"Godot 295 个 Rasterizer 方法 / `DEFINE_COMMAND_0/1/2` / 车规不推荐"当作**本仓 skill 的引文**。
经核实，`skill/` 中**并无这段文字**（`grep` 类型擦除/车规/ Rasterizer 均无命中）。已更正 ADR-0009 §1 为**不引用该"原话"**，
并改为准确表述（本仓此前只有集中 `switch`；自派发可用 POD 实现，与车规不冲突）。此后对"引文"一律先 `grep` 核实再引用。

## M2（同日追加，Item 域）

- **签名扩展**：`CommandExecuteThunk` / `Execute()` 增 `const uint8 *p_var_data` + `RenderCommandType p_type`
  （前者供 Text/资源命令取变长数据，后者供共用布局命令分流）。
- **迁移范围**：Item 域 **19 个结构体 / 28 个命令**（Canvas 8 + ItemFree/Parent/ZRelative 3 + Spatial 11 + Text 6 覆盖的 28 个 `case`）。
- **方式**：**执行体保留**在 `CommandExecutor`（`Exec*` 组由 private 提为 public），命令 `Execute` 仅做转发；
  共用布局结构体（Material 3 式 / Mesh 2 / ZOrder 2 / Layer 2 / Visible 2）的 `Execute` 内嵌 `switch(p_type)`；
  `ItemFree/ItemSetParent/ItemSetZRelative` 直接透传 `p_type`（执行器内部本就有分支）。
- **关键约束（踩过的坑）**：thunk 按**结构体 T** 绑定、不按命令类型 → 共用布局必须**整组一起迁移**，
  否则该结构体其余类型的中心 `case` 会被 thunk 绕过而不执行。
- **仍走 switch**：资源域（~13）+ `kSubmitScene`（M2b）。

## M2b（同日追加，资源域 + SubmitScene）

- **迁移范围**：资源域 9 结构体（Texture/Material/Mesh×3/Font Create + Material Uniform/Texture + ResourceFree 4 共用）+ `SubmitSceneCmd`。
- **方式**：与 M2 一致——`Exec*` 体保留（public），`Execute` 转发；`ResourceFreeCmd` 按 `p_type` 内含 4 路小 switch；
  `SubmitSceneCmd::Execute` 调新增的 `CommandExecutor::SignalSubmit()`。
- **结果**：**全部业务命令已自派发**（33 结构体 / 41 命令）。主 `switch` 仅剩 `kFunctionCall`/`kPadding`
  （队列内部）+ `kResize`/`kShutdown`（错误占位）+ `default`。
- **M3 待办**：`Flush` 显式处理 `kPadding`/`kFunctionCall`，删 `ExecuteCommand` 该虚方法，删 `HasExecute` 改无条件绑定 thunk。
- **⚠️ 事故记录**：编辑过程中 `command_executor.cpp` 一度被**截断**（尾部停在 `CanvasItemSetOpacityCmd::Execute`
  中途、丢失收尾 `} // namespace arhud`）。经 `grep` 发现后已按完整内容**补齐修复**并复核（33 个 `*Cmd::Execute`
  定义齐全、命名空间闭合、关键锚点均在）。请编译时一并确认。

## M3（同日追加，收尾：方案 b）

- **删除**：`ICommandExecutor` 接口、`CommandExecutor::ExecuteCommand`（中央 switch）、`command_detail::HasExecute`。
- **改**：33 个 `Execute` 参数 `ICommandExecutor&` → `CommandExecutor&`（去掉 downcast）；`CommandQueue` 直接持 `CommandExecutor*`；
  `Push<T>`/`PushAndSync<T>` 无条件绑定 thunk。
- **`Flush`**：删除 `switch`，仅就地处理 `kPadding`（跳过，且顺带修正原"kPadding 会误 Post sync_sems_[0]"）与
  `kFunctionCall`，其余直接 `header.exec(...)`。
- **结果**：中央 switch 彻底消失；方案 b 落地。
- **⚠️ 事故记录 2**：本轮 `command_executor.cpp` 曾因工具**截断**（尾部停在 `CanvasItemSetOpacityCmd::Execute`
  中途并丢失 `} // namespace arhud`），已 `grep` 发现并补齐，复核 33 个 `*Cmd::Execute` 定义齐全、命名空间闭合。

## 交接（用户侧）

1. 编译（`.\build.bat conan`）——重点看：`CommandHeader` 16B `static_assert`、`HasExecute` SFINAE、thunk 绑定；
2. `test_command_queue`（6）+ `test_command_executor`（4）+ `test_item_storage`（19）；
3. demo 视觉冒烟：`phase4_spatial`/`phase4_compositor` 相机/灯光（相机位置朝向、光照）应生效。