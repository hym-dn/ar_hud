# 2026-10-02 ADR-0010 M1：渲染全局可达（RenderGlobals）+ 绑定命令入队

- **变更分级**：C 类（架构）——方案见 [ADR-0010](../adr/0010-render-globals-singleton.md)
- **范围**：M1（引入机制，与既有机制**并存**，可回滚）
- **状态**：已落地，**未编译**（按协作约定由用户编译）

## 背景

ADR-0009 落地后，`CommandExecutor` 只剩"实现体 + 共享状态宿主"两角色。用户裁定**对齐 Godot**：
存储以 RSG 式**平铺全局**暴露，命令直接绑定 **(目标, 方法, 参数)**，从而消除执行器与重复实现层。
M1 只引入机制，不改现有调用。

## 改动

| 文件 | 改动 |
|---|---|
| `servers/rendering/render_globals.h`（新增） | `RenderGlobals`：10 个 `static inline` 指针 + `Reset()`；**平铺**（不做 compositor 包装） |
| `servers/CMakeLists.txt` | `SERVERS_HEADERS` 增 `rendering/render_globals.h` |
| `servers/rendering/arhud_engine.cpp` | `Initialize` 装配（compositor 访问器 + `item_storage`/`this`(provider)/`submit_pending_`）；栈释放前 `Reset()` |
| `servers/rendering/command_queue.h` | `command_detail::BoundCall0~3` + `BoundThunk0~3`；public `PushBound`（0~3 元）；private `EnqueueBound` 尾部 |

**关键设计取舍**（详见 ADR-0010 §2 与 §8）：

1. **实参用显式 POD 成员，不用 `std::tuple`**：本队列 `memcpy` 搬运命令，要求 `trivially_copyable`；MSVC 的 `std::tuple` 不保证此性质。故按 0~3 元展开。
2. **入队 API 名 `PushBound`**：与既有类型标签 `Push(const T&)` 重载会在 0 元命令上歧义（`T` = 成员函数指针）。
3. **保留函数指针 thunk（不引入 vtable）**：`memcpy` 多态对象属灰区，对功能安全不友好。
4. **`RenderCommandType::kNone` 作绑定命令的类型位**：Flush 只对 `kPadding`/`kFunctionCall` 特判，`kNone` 安全且表意"无类型标签"。

## 风险 / 待验证

- **未编译**：`render_globals.h` 的前置声明与定义一致性已人工核对（全部为 `class`）；`arhud_engine.cpp` 的装配类型匹配需编译确认。
- **`PushBound` 目前无调用点**（M2 才迁移），属"机制先立、用例后接"；模板实例化仅在 M2 被触发。
- M1 期间 `RenderGlobals` 与 `CommandExecutor` **并存**，行为等价、可随时回滚（移除头文件与装配即回到 ADR-0009 状态）。

## 附：同步的构建修复（A 类）

`servers/rendering/storage/item_storage.h` 丢失了成员声明 `text_items_`（`canvas_items_`/`spatial_items_` 之外缺失），导致 `.h` 内联访问器与 `.cpp` 26 处引用全部报 `C2065`。已补回该成员。属此前编辑事故残留。

## 后续

- **M2**：分域把 `RenderServer` 调用点改为 `PushBound(&Storage::Method, …)`（视口 → Item → 资源），随首个变长命令补 `PushBoundBlob`。
- **M3**：删 `CommandExecutor`、33 命令结构体、`RenderCommandType`、per-command `Execute`；单测改造。