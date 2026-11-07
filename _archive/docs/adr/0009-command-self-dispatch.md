# ADR-0009：命令分派改为 POD 自派发（替代集中 switch）

| | |
|---|---|
| 状态 | 已接受（M1 落地：thunk 机制 + 视口域 4 条命令；M2/M3 待续） |
| 日期 | 2026-10-02 |
| 关联 | skill/arhud-render-thread-arch §3.3、skill/arhud-architecture §3.2、ADR-0001、ADR-0006、docs/adr/README.md |

## 1. 背景与问题

- 现状：命令分派**集中**在 `CommandExecutor::ExecuteCommand` 的大 `switch`（59 个 `case` / 42 个 `Exec*`；`command_executor.cpp` 840 行）。
- 痛点：
  1. **开放-封闭性差（OCP）**：新增一条命令须同时改 `render_commands.h`（枚举 + 结构体）、`command_executor.h/.cpp`（`Exec` 声明 + 定义 + 中心 `case`）、`render_server.cpp`（发射方法）——**每次都要打开执行器这个文件**；
  2. **热点文件**：多人并行加命令时冲突集中在同一 `switch`；
  3. **规模膨胀**：命令数 ≈ RenderServer API 数，中心 `switch` 随 API 线性变长（`enum : uint8` 上限 255，当前 ~50）。
- 既有事实（决定本方案的可行性）：
  1. 全部命令已 `static_assert(std::is_trivially_copyable_v<...>)`；命令头 [CommandHeader](file:///d:/Work/ar_hud/servers/rendering/command_queue.h#L106-L113) 8B 已含 `type` 字段；
  2. 队列**已存在函数指针路径** `kFunctionCall`（[command_queue.cpp L245](file:///d:/Work/ar_hud/servers/rendering/command_queue.cpp#L241-L266)）——自派发并非新范式，而是把该路径**由特例普适化**；
  3. `Push<T>` / `PushAndSync<T>` 在**编译期已知命令类型 T**（[command_queue.h L220-L289](file:///d:/Work/ar_hud/servers/rendering/command_queue.h#L220-L289)），可在推送时自动绑定执行体 → 发射侧零改动；
  4. `ICommandExecutor` 目前**唯一**实现者是 `CommandExecutor`，**无 mock 实现**；单测直接用具体 `CommandExecutor`（[test_command_executor.cpp L61-L66](file:///d:/Work/ar_hud/tests/rendering/test_command_executor.cpp#L60-L66)）。
- **认知澄清（承接讨论）**：本仓此前只有一种分派形态——**集中 `switch`**（`skill/arhud-render-thread-arch` §3.3 与 `CommandExecutor::ExecuteCommand` 一致，该节即以 `switch` 为设计）。讨论中一度出现"自派发必须依赖类型擦除/动态分配、因而不合车规"的推断，**该推断不成立**：
  - **自派发可用 POD 实现**（非虚成员函数 + 函数指针 thunk），**不引入动态分配、不破坏 `trivially_copyable`**；
  - 故"自派发"与车规硬约束（无裸 `new`、可 `memcpy`、可测）**并不冲突**。Godot 的实际实现（`CommandBase` + 虚 `execute()` + 堆分配 + 类型擦除）只是其一种选择，**非机制必然**；本决策不照搬该实现，只采纳"自派发"这一**模式**。
- 硬约束：① 命令保持 POD（可 `memcpy` 进定长环）；② 零动态分配；③ 不引入晦涩机制（可静态分析/可追溯）；④ 单测可裸构造执行器。

## 2. 决策

**采纳「POD 自派发」：为每条命令绑定函数指针 thunk，`Push<T>` 时自动绑定，`Flush` 直接调用 thunk，取消中心 `switch`。**

机制：

1. **命令自带执行体**：每个 `*Cmd` 增**非虚** const 成员
   ```cpp
   void Execute(CommandExecutor &p_exec) const;   // 声明于 render_commands.h（class CommandExecutor; 前向声明）
   ```
   定义放在实现侧 `.cpp`。**非虚成员函数不影响 `trivially_copyable`** → 既有 `static_assert` 全部保留。
2. **thunk 绑定**：`CommandQueue::Push<T>` / `PushAndSync<T>` 已有 `T`，自动写入模板 thunk 地址
   ```cpp
   template <typename T>
   void CommandThunk(const uint8 *p, uint32, CommandExecutor &ex)
   { reinterpret_cast<const T *>(p)->Execute(ex); }
   ```
   → **发射侧（`RenderServer`）调用点零改动**；改动集中在 `CommandQueue` 内部与命令定义。
3. **命令头扩展**：`CommandHeader` 8B → **16B**，新增 8B thunk 指针（`type` 保留，供调试/日志/`kPadding` 判定）。代价：+8B/命令（约 50 条/帧 ≈ 400B/帧），可接受。
   - **连带：环缓冲区对齐 `kAlignment` 8 → 16**。原因：回绕填充区大小 = `kCapacity − offset`，原不变量"offset 为 8 的倍数 ⇒ pad 为 0 或 ≥8"仅够容纳 8B 头；头变 16B 后必须把对齐提到 16，才能保证 pad 为 0 或 ≥16（= 头大小）。
4. **`Flush` 归一**：`kPadding` 跳过；其余一律 `header.exec(payload, size, *executor_)`。`kFunctionCall` 与普通命令**归一**（由"遗留特例"降为"通用形态"）。
5. **执行体保留、分派退场**：42 个 `Exec*` **保留为执行体**（由各命令的 `Execute()` 调用），仅**删除中央 `switch`**；`ExecuteCommand(type,…)` 或删除、或保留为**测试兼容 shim**（当前单测经 `Dispatch()` 调用它）。
6. **接口（开放点，评审时定）**：thunk 签名取具体 `CommandExecutor&` 会收紧 `ICommandExecutor` 的"接口解耦"（skill §3.3 的 mock 好处）。因当前**无 mock 实现**，代价低。二选一：
   - **(a)** 保留 `ICommandExecutor`（供 `kFunctionCall` 遗留路径），thunk 取具体类（含一处受控 downcast）；
   - **(b)** 收窄/移除 `ICommandExecutor`，`CommandQueue` 直接持 `CommandExecutor*`。

**关键不变量（务必记录，避免误解）**：
- 命令仍 **POD + 定长环 + 零动态分配**（车规不破）；变长数据、sync 写回、`kPadding` 语义**不变**。
- **分派点未消失，只是"由集中改为分散"**——每条命令仍有一处"类型 → 执行体"的映射，只是从中心 `switch` **搬进命令自身的 `Execute()`**。真正收益是 **OCP**（新增命令不改执行器），不是"免除映射"。

## 3. 已否决方案

| 方案 | 否决理由 |
|---|---|
| 维持集中 `switch`（现状） | OCP 差：新增命令必改中心文件；执行器为热点文件；随 API 线性膨胀——本决策正要解决它 |
| Godot 原实现（`CommandBase` + 虚 `execute()` + 类型擦除 + 堆分配） | 动态分配 + 间接调用 + 晦涩机制，破坏 POD/车规；不可照搬 |
| X-macro 生成（枚举 + 结构体 + `case`） | 仍保留中心 `switch`，OCP 未改善；宏不可读/不可 grep，违背本仓"显式代码"风格 |
| 函数指针分派表 `ExecFn kTable[256]`（按 enum 索引） | 分派点仍在中心一处（只换形态），OCP 未改善；另需表初始化与越界防御 |
| 命令 `Execute` 取**虚**函数 | 引入 vptr，破坏 `trivially_copyable`，与 POD 约束冲突 |

## 4. 后果

- **正面**：新增命令**不动执行器**（OCP）；消除巨型 `switch` 源码；命令自带执行语义，与"命令即对象"一致；`kFunctionCall` 归一。
- **负面/代价**：
  1. 命令头 8→16B（每命令 +8B）；
  2. 命令从"**纯数据**"变为"**数据 + 行为**"，`render_commands.h` 需前向声明 `CommandExecutor`（耦合↑）；
  3. **失去"单点枚举全部命令 / 单点插桩"**：grep 与审计能力下降；
  4. 函数指针**间接调用** + 每命令**模板实例化**（代码膨胀、调试略难）；
  5. **认证层面**：需为"间接调用可追溯"补论证——**非红线，是成本**。
- **中性约束**：新增命令须同时定义其 `Execute()`；`CommandExecutor` 须在 thunk 实例化处可见（include 关系调整）；文档中"命令分派 = 中央 `switch`"须改写（含本仓 skill §3.3/§九、`AGENTS.md`）。
- **未覆盖**：不改命令语义、线程模型与队列结构（仅头布局与分派路径）。

## 5. 验收

- **grep**：`command_executor.cpp` 的 `ExecuteCommand` 内**不再有渲染命令 `switch`**；每个 `*Cmd` 声明 `Execute(`；`sizeof(CommandHeader) == 16`；`Flush` 经 thunk 调用。
- **单测**：既有 29 用例全绿（`test_command_executor` 的 4 例改用 thunk 或经保留的 shim）；全部 `static_assert(trivially_copyable)` 保留。
- **规模指标（本决策的核心收益）**：新增一条命令的**改动文件集**由
  `{render_commands.h, command_executor.h, command_executor.cpp, render_server.cpp}`
  缩减为 `{render_commands.h, 该命令 Execute 的定义处}`；**执行器文件不再随命令增长**。

## 6. 迁移计划与回滚（评审通过后执行）

分阶段、每阶段独立可编译可测：

| 阶段 | 内容 | 回滚点 |
|---|---|---|
| **M1** | 头 8→16B + `Push<T>` 绑定 thunk；`Flush` **优先用 thunk，thunk==null 时回退旧 switch** | ✅ 还原头与 `Flush`（零行为变化，旧 switch 尚在） |
| **M2** | 逐命令迁移 `Execute()`（建议分批：先 item/相机灯光，再资源），每迁一条即删对应 `case` | ✅ 每批独立；期间双路径共存 |
| **M3** | `switch` 清空 → 删除 `ExecuteCommand`（或降 shim）+ 收紧接口（§2.6） | ⚠️ 单向门：M3 前打 tag |

## 7. 待评审开放点

1. §2.6 接口取舍（a/b）——M1 采用 (a)：`ICommandExecutor&` + 命令内受控 downcast，接口保留；
2. 是否保留 `ExecuteCommand` 作为测试 shim——M1 保留（未迁移命令仍走它），M3 再定；
3. 迁移批次划分——M1 = 视口域（相机/灯光 4 条），后续批次待定。

## 8. 实施记录

### M1（2026-10-02 本窗口落地）

| 文件 | 改动 |
|---|---|
| `servers/rendering/command_queue.h` | 增 `CommandExecuteThunk` + `command_detail::HasExecute`/`CommandThunk`（迁移期检测）；`CommandHeader` 8→16B（+`exec`）；`kAlignment` 8→16；`Push<T>`/`PushAndSync<T>` 按 `if constexpr` 绑定 thunk；`WriteCommand` 增 `p_thunk` 参数 |
| `servers/rendering/command_queue.cpp` | `WriteCommand` 写 `header.exec`；`Flush` 优先调 thunk（已迁移命令），null 时回退 switch；`kFunctionCall` 传 null thunk |
| `servers/rendering/render_commands.h` | 视口域 4 条命令（`Camera3DSetCmd`/`Camera2DSetCmd`/`DirectionalLightSetCmd`/`DirectionalLightClearCmd`）增 `void Execute(ICommandExecutor&) const` |
| `servers/rendering/command_executor.h` | `ResolveViewport` 提为 public；增 `GetItemStorage()`；删 4 个 `ExecCamera*/ExecDirectional*` 声明 |
| `servers/rendering/command_executor.cpp` | 删 4 个 switch `case`；将 4 个 `Exec*` 体**移入**对应命令的 `Execute()` 定义 |
| `tests/rendering/test_command_executor.cpp` | `Dispatch()` 改为 `cmd.Execute(executor)`（自派发） |

**当前状态**：机制就位；4 条视口命令走 thunk，其余 45 条走 `ExecuteCommand` switch 兜底。**行为等价**（thunk 体与 `Exec*` 体逐一对应）。

### M2（2026-10-02 同日，Item 域）

**签名扩展**：`CommandExecuteThunk` 与 `Execute()` 各增两参——`const uint8 *p_var_data`（变长数据指针，Text/资源命令需要）、`RenderCommandType p_type`（共用布局命令分流）。

| 文件 | 改动 |
|---|---|
| `servers/rendering/command_queue.h` | thunk 签名 + `HasExecute` 检测 + `CommandThunk` 适配新签名 |
| `servers/rendering/command_queue.cpp` | `Flush` 传 `header.type` 给 thunk |
| `servers/rendering/render_commands.h` | **Item 域 19 个结构体**增 `Execute` 声明（视口域 4 个改新签名） |
| `servers/rendering/command_executor.h` | `Exec*` 组由 private 提为 **public**（供命令 `Execute` 调用） |
| `servers/rendering/command_executor.cpp` | 删 **Item 域 28 个 `case`**；新增 **19 个 `*Cmd::Execute` 定义**（转发；共用布局按 `p_type` 分流） |
| `tests/rendering/test_command_executor.cpp` | `Dispatch()` 传 `p_var_data` + `p_type` |

**✅ 已迁移**：视口域（相机/灯光 4）+ Item 域（19 结构体 / 28 命令）。**仍走 switch**：资源域（~13）+ `kSubmitScene`。

**共用布局的处理（实证）**：`CanvasItemSetMaterialCmd`(3 式)、`CanvasItemSetMeshCmd`(2)、`CanvasItemSetZOrderCmd`(2)、`CanvasItemSetLayerCmd`(2)、`CanvasItemSetVisibleCmd`(2) 的 `Execute` 内嵌 `switch(p_type)`（共 6 处局部小 switch，~11 case）；`ItemFreeCmd`/`ItemSetParentCmd`/`ItemSetZRelativeCmd` 直接转发（执行器内部本就有 p_type 分支，无需小 switch）。此即"中心 switch → 局部小 switch"的落地形态。

### M2b（2026-10-02 同日，资源域 + SubmitScene）

| 文件 | 改动 |
|---|---|
| `servers/rendering/render_commands.h` | **资源域 9 结构体 + `SubmitSceneCmd`** 增 `Execute` 声明（累计 **33** 个命令结构体全部具备） |
| `servers/rendering/command_executor.h` | 增 `SignalSubmit()`（`SubmitSceneCmd::Execute` 用） |
| `servers/rendering/command_executor.cpp` | 删资源域 12 个 `case` + `kSubmitScene` case；新增 **10** 个 `*Cmd::Execute` 定义 |

**✅ 全部业务命令已迁移（33 结构体 / 41 命令）**。主 `switch` 现仅剩：`kFunctionCall`（队列内部函数指针路径）、`kPadding`（回绕填充）、`kResize`/`kShutdown`（错误占位）、`default`。

### M3（2026-10-02 同日，收尾：方案 b）

| 文件 | 改动 |
|---|---|
| `render_commands.h` | 删除 `ICommandExecutor` 接口 → 前置声明 `class CommandExecutor;`；33 个 `Execute` 参数改 `CommandExecutor&` |
| `command_queue.h` | `CommandExecuteThunk`/`CommandThunk` 参数改 `CommandExecutor&`；**删 `HasExecute`**，`Push<T>`/`PushAndSync<T>` **无条件绑定 thunk**；`executor_`/`SetExecutor` 改 `CommandExecutor*` |
| `command_queue.cpp` | `Flush` **删除 `switch`**：显式就地处理 `kPadding`/`kFunctionCall`，其余直接调 thunk（含 executor 空值兜底） |
| `command_executor.h` / `.cpp` | 去除 `: public ICommandExecutor`；删 `ExecuteCommand` 声明与定义（**中央 switch 消失**）；33 个 `Execute` 定义改签名并去掉 `static_cast` downcast |

**结果**：**中央 switch 彻底消失**；`ICommandExecutor`、`ExecuteCommand`、`HasExecute` 全部删除。方案 b 落地。

**最终形态（诚实评估）**：净代码量**增加**（33 个 `Execute` 约 150+ 行），换来"无中央 switch、新增非共用命令不动执行器"；6 个共用布局结构体各含局部 `switch(p_type)`。中心热点（原 59-case）已消除。

**注意（对留档的诚实修正）**：本 ADR §2/§5 曾称"新增命令不动执行器"。在**分批迁移期间**仍未完全成立——未迁移命令的实现体仍在 `CommandExecutor`。只有 M3（全部迁移）后，"新增命令只改 `render_commands.h` + 其 `Execute` 定义"才真正成立。