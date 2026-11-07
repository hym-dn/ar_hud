# ADR-0010：命令绑定 (目标, 方法) + 渲染存储 RSG 式全局可达（对齐 Godot）

- **状态**：已接受（2026-10-07）
- **日期**：2026-10-02
- **关联**：ADR-0009（命令自派发，已接受）、ADR-0001（Route B：无动态分配偏好）、ADR-0007（视口场景状态）
- **来源**：Godot 4.6 本地源码 `D:\Work\godot-4.6`
  - `servers/rendering/rendering_server_globals.h`（RSG 平铺全局）
  - `core/templates/command_queue_mt.h`（命令 = 目标 + 方法 + 参数）
  - `servers/rendering/rendering_server_default.cpp`（`command_queue.push(RSG::xxx_storage, &Storage::method, args…)`）

---

## 1. 背景

### 1.1 现状（ADR-0009 之后）

命令是**类型标签 + POD 结构体 + 内联变长数据**，每条命令自带 `Execute()`（thunk 分派）；实现体在 `CommandExecutor` 内（33 个 `Exec*`），并通过它持有的 `compositor_`/`item_storage_`/`viewport_provider_`/`submit_signal_` 访问存储。**实现存在两层**：命令 `Execute` → `Exec*` → storage 方法（后者才是真正干活）。

### 1.2 Godot 的做法（源码事实）

**① 存储全局可达（平铺）**：`RenderingServerGlobals` 以 `static inline` 指针**逐个暴露**存储/渲染器：

```cpp
class RenderingServerGlobals {
    static inline RendererLightStorage     *light_storage     = nullptr;
    static inline RendererMaterialStorage  *material_storage  = nullptr;
    static inline RendererMeshStorage      *mesh_storage      = nullptr;
    static inline RendererTextureStorage   *texture_storage   = nullptr;
    static inline RendererCanvasRender     *canvas_render     = nullptr;
    static inline RendererCompositor       *rasterizer        = nullptr;
    static inline RendererCanvasCull       *canvas            = nullptr;
    static inline RendererViewport         *viewport          = nullptr;
    static inline RenderingMethod          *scene             = nullptr;
    // …（无 "RenderingServer*" 包装，直接平铺每个存储）
};
#define RSG RenderingServerGlobals
```

**② 命令 = (目标对象, 方法, 参数)**：`Command{ T *instance; M method; Tuple<Args> args; }`，`_flush()` 里 `(instance->*method)(…)`。RS 侧入队形如：

```cpp
command_queue.push(RSG::material_storage, &RendererMaterialStorage::shader_set_code, shader, p_code);
command_queue.push(this /*RenderingServerDefault*/, &RenderingServerDefault::_draw, p_present, frame_step);
```

⇒ **命令直接点名"调哪个存储的哪个方法"**，因此 Godot **没有执行器、没有 per-command Execute、没有重复的实现层**——**存储的既有方法本身就是命令体**。

### 1.3 本工程与 Godot 的差异点（唯一需要处理的）

| | Godot | 本工程 |
|---|---|---|
| 存储可达性 | RSG 平铺全局 | 无全局，靠注入 |
| 命令 | (目标, 方法, 参数) | (类型标签, POD 数据) |
| 变长数据（纹理像素/文字码点/字体路径） | 以**非平凡类型**按值入队（`Ref<Image>`/`String` 等）→ **动态分配** | **内联**到环形缓冲（零分配） |
| 传输 | 命令对象带 vtable，`cmd->call()` | POD + 函数指针 thunk |

**结论**：采 ① + ② 可消除执行器与重复实现层；但 **变长数据**不能照 Godot 用非平凡类型（会引入动态分配，违反 ADR-0001），需保留"内联 blob"通道。

---

## 2. 决策

### 2.1 引入 `RenderGlobals`：**平铺**暴露每个存储（RSG 式）

```cpp
// servers/rendering/render_globals.h
struct RenderGlobals
{
    static inline ShaderStorage    *shader_storage    = nullptr;
    static inline TextureStorage   *texture_storage   = nullptr;
    static inline MaterialStorage  *material_storage  = nullptr;
    static inline MeshStorage      *mesh_storage      = nullptr;
    static inline FontStorage      *font_storage      = nullptr;
    static inline CanvasRenderer   *canvas_renderer   = nullptr;
    static inline SpatialRenderer  *spatial_renderer  = nullptr;
    static inline ItemStorage      *item_storage      = nullptr;   // 保留模式账本（非 GPU 存储）
    static inline IViewportProvider *viewport_provider = nullptr;  // ADR-0007
    static inline std::atomic<bool> *submit_signal    = nullptr;  // kSubmitScene 信号（引擎 submit_pending_）
};
```

- **平铺**：与 RSG 一致，**不做 `compositor` 包装**（本工程此前版本正是包装，已否决，见 §3）。
- **装配**：`ARHudEngine::Initialize` 从 `compositor` 的各访问器逐个填入；`Finalize` 逐个清空。
- 只含指针 → 零分配，不动 ADR-0001。

### 2.2 命令改为绑定 **(目标存储, 方法, POD 参数)**

`CommandQueue` 增泛型入队（编译期绑定，发射侧零样板）：

```cpp
template <typename S, typename M, typename... Args>
void Push(M p_method, Args... p_args);          // 目标由 p_method 的所属类推导，或显式传 S*
```

生成一条"绑定命令"：内部存 `S *instance; M method; Tuple<Args…> args;`（均 POD/平凡可拷贝），thunk 调 `(instance->*method)(args…)`。

- **本工程的重载**：目标存储从 `RenderGlobals` 取（如 `RenderGlobals::material_storage`），或由调用方显式给出指针——**在评审时二选一**（§7）。
- `RenderServer` 调用点由 `Push(MaterialSetShaderCmd{…})` 变为
  `Push(&MaterialStorage::SetShader, item, shader)` 形态。

### 2.3 变长数据：保留"内联 blob"通道（**偏离 Godot 的唯一一处，且有据**）

变长命令（纹理像素 / 文字码点 / 字体路径）**不**改为按值非平凡类型（那会引入动态分配）。做法：

```cpp
template <typename S, typename M, typename... Args>
void PushBlob(M p_method, const void *p_blob, uint32 p_blob_size, Args... p_args);
```

blob 紧跟在绑定命令对象之后（与现有 `p_var_data` 机制相同）；方法签名末参收 `const uint8 *`。既有内联机制**原样复用**。

### 2.4 删除

- **删除 `CommandExecutor` 类**（含 `Initialize/Finalize`、33 个 `Exec*`、`ResolveViewport/GetItemStorage/SignalSubmit`）；
- **删除 33 个命令结构体与 `RenderCommandType` 枚举**（含 `Camera3DSetCmd` 等）——命令词汇表由"存储方法"承载；
- **删除 per-command `Execute()`**（实现层归零：命令体 = 存储方法本身）；
- `EngineStackRefs` 去掉 `command_executor`；`CommandQueue` 去掉 `executor_`/`SetExecutor`。
- 帧控制（`kSubmitScene`/`kResize`/`kShutdown`）与队列内部条目（`kPadding`/`kFunctionCall`）保留为**极小的内部通道**（或同样改为绑定 `ARHudEngine` 的方法）。

### 2.5 传输机制：**保留函数指针 thunk，不引入 vtable**（有意的偏离）

Godot 的 `CommandBase` 带虚函数（vtable）并在 `_flush` 里 `memcpy` 后调用；本项目**保留现有的 thunk**（`CommandHeader.exec`，函数指针）。理由：函数指针是平凡可拷贝，`memcpy` 语义无争议；vtable 对象 `memcpy` 属灰区，对功能安全/MISRA 不友好。**行为等价，表达更保守。**

---

## 3. 已否决

| 方案 | 结论 |
|---|---|
| **`RenderGlobals` 包装 `compositor` 指针**（本 ADR 上一版） | 否决——非 Godot 形态；Godot 平铺暴露每个存储 |
| 保留 `CommandExecutor` | 否决——多一层重复实现（`Execute → Exec* → storage`） |
| 保留类型标签 + per-command `Execute` | 否决——与"命令 = (目标, 方法)"重复；枚举/结构体需手工维护 |
| 变长数据改用非平凡类型按值入队（纯 Godot 做法） | 否决——引入动态分配，违反 ADR-0001 |
| 命令内联 `CommandContext*` 指针 | 否决——既有 RSG 平铺即可，无需再包一层 |

---

## 4. 后果

### 4.1 正面

- **实现层归零**：删掉 33 个 `Exec*` + 33 个命令结构体 + 1 个枚举；命令体就是存储方法，**无重复**。
- **概念最少**：无执行器、无 per-command `Execute`、无类型枚举；与 Godot 对齐（平铺 RSG + 绑定命令）。
- 新增 API 只写一次：在 Storage 上加一个方法 + 在 `RenderServer` 加一行 `Push(&Storage::Method, …)`。

### 4.2 负面 / 风险（须评审确认）

- **引入全局可变状态**：隐藏依赖复发；**单测隔离**需在用例内装配 `RenderGlobals`（或提供 reset）。
- **多实例不可行**：多个 `ARHudEngine` 争用同一组全局指针。当前单实例，可接受，须文档明示。
- **时序**：先 `Initialize` 装配再 `Flush`；`Finalize` 后不得 `Flush`；建议 debug 断言。
- **模板实例化按 (S, M, Args…) 组合展开**：命令种类越多，二进制越大；需 `MAX_COMMAND_SIZE` 类上限约束。
- **`RenderServer` 调用点全量改写**（~41 处）：`Push(Cmd{…})` → `Push(&Storage::Method, …)`。
- **ADR-0009 大部分被取代**：thunk 机制保留，但"命令结构体 + `Execute` + 执行器"三者删除，需在 ADR-0009 §8 追加说明。
- **车规**：无分配不受影响；但"全局可变状态 + 成员函数指针间接调用"需在安全评审中论证。

---

## 5. 验收标准

- **编译**：无 `CommandExecutor`、无 `RenderCommandType` 枚举、无 33 个命令结构体、无 per-command `Execute`；`Push<S>(&S::Method, …)` 全量替换。
- **单测**：`test_command_executor` 改造为"装配 `RenderGlobals` → `Push(&ItemStorage::CanvasItemCreate, …)` → `Flush` → 断言状态"；`test_item_storage`(19)/`test_command_queue`(6) 语义不变。
- **冒烟**：`phase4_*` 的 item/相机/灯光/资源命令端到端生效；静态帧不重渲（ADR-0008 dirty 门不受影响）。
- **治理 grep**：`CommandExecutor`/`ExecCamera3DSet` 等仅存于历史 ADR/devlog。

---

## 6. 迁移计划（分批，可回滚）

| 阶段 | 内容 | 回滚 |
|---|---|---|
| **M1** | 引入 `RenderGlobals`（平铺）+ `ARHudEngine` 装配/清空；`CommandQueue` 增泛型 `Push<S>`/`PushBlob`（与现有机制**并存**） | 移除头文件即回退 |
| **M2** | 分域把 `RenderServer` 调用点改为 `Push(&Storage::Method, …)`（视口 → Item → 资源），逐批编译验证 | 每批独立回退 |
| **M3** | 删 `CommandExecutor`、33 命令结构体、`RenderCommandType`、`Execute`；`EngineStackRefs`/`CommandQueue` 清理；单测改造 | 单向门 |

---

## 7. 开放点（**已裁决 2026-10-07**）

| # | 议题 | 裁决 |
|---|---|---|
| 1 | 目标存储来源 | **显式传** `&Storage`（最贴 Godot `push(RSG::material_storage, …)`，可测）——M1 `PushBound(instance, method, …)` 已如此 |
| 2 | 帧控制命令（`kSubmitScene`/`kResize`/`kShutdown`） | **保留为极小内部通道**（不改为绑定 `ARHudEngine` 方法）——M1 保留 |
| 3 | 全局形态 | **`static inline` 散指针（Godot 式）+ `Reset()`** 供测试——M1 已如此 |
| 4 | debug 断言 | **加**：未装配 / 未绑定访问 → `ARHUD_ASSERT`——M1 `PushBound` 已有 |
| 5 | 命名 | **暂定 `RenderGlobals` / `render_globals.h`**；S2 归一到 `RSG` / `RenderingServerGlobals` |

---

## 8. 实施记录

### 评审裁决（2026-10-07，已接受）

对照 `docs/architecture.md §7.11 C/D`（`CommandQueueMT` + `Tuple`）：**本 ADR 采纳为 S1 施工基线**，§7.11 C/D 已回改为「保留 `CommandQueue` + `PushBound` + 显式 POD 参数」。理由：语义终点一致（命令=目标×方法、删执行器、平铺全局）；vtable `memcpy` 属灰区（车规/MISRA）、`std::tuple` 在 MSVC 下不保证 `trivially_copyable`；且 M1 已落地并通过构建。开放点 1–5 裁决见 §7。

### M1（2026-10-02 落地；2026-10-07 编译验证通过）

| 文件 | 改动 |
|---|---|
| [render_globals.h](../servers/rendering/render_globals.h)（新增） | `RenderGlobals`：10 个 `static inline` 指针（5 资源 Storage + 2 Renderer + `ItemStorage` + `viewport_provider` + `submit_signal`）+ `Reset()`；**平铺**，不做 compositor 包装 |
| [servers/CMakeLists.txt](../servers/CMakeLists.txt) | `SERVERS_HEADERS` 增 `rendering/render_globals.h` |
| [arhud_engine.cpp](../servers/rendering/arhud_engine.cpp) | `Initialize` 装配（从 compositor 访问器逐个填入 + `item_storage`/`this`/`submit_pending_`）；栈释放前 `RenderGlobals::Reset()` |
| [command_queue.h](../servers/rendering/command_queue.h) | 增 `command_detail::BoundCall0~3` + `BoundThunk0~3`；public `PushBound` 重载（0~3 元）；private `EnqueueBound` 公共尾部 |

**与 ADR 决策的偏差（已记录，待评审确认）**：

1. **入队名为 `PushBound` 而非重载 `Push`**：与既有类型标签 `Push(const T &)` 混合重载会在 0 元命令上产生歧义（`T` 意为成员函数指针）；命名统一留待 M2。
2. **实参为显式 POD 成员、按 0~3 元展开，而非变参**：`std::tuple` 在 MSVC 下不保证 `trivially_copyable`，而本队列以 `memcpy` 搬运命令；显式 POD 成员可保证 `static_assert(is_trivially_copyable)`。>3 元命令（若有）需扩至 4 元。
3. **`PushBoundBlob` 未在 M1 落地**：变长命令在 M2 迁移时随首个实际用例加入（内联 blob 机制沿用 `CommandDataSpan`）。
4. **§2.1 示意订正**：`submit_signal` 实际类型为 `std::atomic<bool> *`（引擎 `submit_pending_`），非接口类型。
5. **命名（§7.5）暂定**：类型 `RenderGlobals`、文件 `render_globals.h`。

### M2（进行中，2026-10-07）

**切片一（已落地并验证）**：迁移「无指针变长 + ≤3 参 + 异步」命令 **27 处** → `PushBound`。

| 域 | 覆盖 |
|---|---|
| 资源 Free（4） | `&TextureStorage::TextureFree` / `&MaterialStorage::MaterialFree` / `&MeshStorage::MeshFree` / `&FontStorage::FontFree` |
| Item Set*/Free（23） | Canvas ×9、Spatial ×6、Text ×8，一律 `&ItemStorage::Xxx`，**按绑定方法显式路由**（取代原 `ItemFreeCmd`/`ItemSetParentCmd`/`ItemSetZRelativeCmd` 的 `p_type` switch） |

- 目标来源：`compositor_->GetXxxStorage()` / `item_storage_`（**显式传指针**，最稳健；`RenderGlobals` 待命令体需跨存储时启用）。
- 验证：`cmake --build build --config Debug` 0 error；`ctest` **29/29 通过**。
- 行为等价：绑定方法即 Storage 既有入口（与既有 bypass 分支同一调用），dirty 传播仍在 Storage 侧。

**切片二（已落地并验证，2026-10-07）**：**Godot RID 分裂 + blob 通道**（首个 create 用例）

- `CommandQueue`：新增 `PushBoundBlob`（1/2 元实参 + 内联 blob；被绑定方法**末参**收 `const uint8*` blob 基址）+ `BoundCallBlob1/2`/`BoundThunkBlob1/2`/`EnqueueBoundBlob`。
- `TextureStorage`：新增 `TextureAllocate()`（任意线程，线程安全 RIDOwner 分配空壳）+ `TextureInitializeFromData(RID, TextureCreateParams, blob)`（渲染线程）+ `TextureCreateParams`（H1）；`TextureCreate` 重构复用私有 `TextureInitializeRid`。
- `RenderServer::TextureCreateFromData`：**RID 分裂**——主线程 `TextureAllocate` 立即返回 RID，`TextureInitializeFromData` 经 `PushBoundBlob` 异步入队；blob 布局 `[像素][名称(含NUL)]`；bypass（单线程）路径仍走同步 `TextureCreateFromData`。
- 验证：`cmake --build build --config Debug` 0 error；`ctest` **29/29**。
- 语义说明：分裂后主线程**不可立即读**纹理元数据（空壳未初始化，与 Godot 同语义）；已核对 5 个 demo 无此依赖。
- 同批扩展（2026-10-07）：`MaterialSetUniform` 改走 blob 通道（新增 `MaterialSetUniformParams` + `MaterialSetUniformBlob`）；`MaterialSetTexture` 直接 `PushBound`（3 POD 元）。验证 `ctest` 29/29。

**切片三（已完成，2026-10-07）**：把同一 RID 分裂范式复制到各 create。

- ✅ `MaterialCreate`：`MaterialAllocate()` + `MaterialInitialize(RID, MaterialCreateParams, blob)`（私有 `MaterialInitializeRid` 供同步/分裂两路复用）；内置 shader 回退**上移到主线程**解析（顺带修正 bypass 与队列两路语义不一致）。验证 `ctest` 29/29。
- ✅ `MeshCreateQuad/Box/Sphere`：`MeshAllocate()` + `MeshInitializeQuad/Box/Sphere(RID, params, blob)`（几何构建抽出为 Initialize，`Create*` 变薄；`kVertex2D/3DStride` 复用）。
- ✅ `CanvasItemCreate`/`SpatialItemCreate`/`TextItemCreate`：`ItemAllocate()`（`next_rid_` 原子化）+ `Canvas/Spatial/TextItemInitialize(RID, mask)`。
- ✅ `FontLoadFromFreeType`（专项）：**两函数连体拆分**——`FontAllocate()` + 私有 `FontBuildFromFreeType(RID, name, path, size, chars, count, spread)`（450 行体，改返回 `Error`）+ `FontInitializeFromSdfAtlas(RID, ...)`；同步入口 `FontLoadFromFreeType`/`FontLoadFromSdfAtlas` 退化为「Allocate + Initialize」薄包装。命令 blob 布局 `[名称][路径][码点]`（注意 `NameSize(nullptr)==0`，故按非空兜底指针计段长）。验证 `ctest` 29/29。
- `MaterialSetUniform` 走 blob、`MaterialSetTexture` 直接绑定已随切片二完成。

**切片四（已完成，2026-10-07）**：值类型 / blob / 相机灯光过渡桥。

- ✅ `CanvasItemSetTransform`/`SpatialItemSetTransform`/`TextItemSetColor`/`TextItemSetText` → 走 **blob 通道**（新增与指针版等价的 `*Blob` 包装，不改动既有指针签名）。
- ✅ 相机/灯光/提交 → **S1 过渡桥**：命令目标绑到 `RenderServer` 自身的 `ApplyCamera3D/2D`、`ApplyDirectionalLight/Clear`（渲染线程 `ResolveViewport` + 写入 + 置屏 dirty）。相机仍按 ADR-0006 只传**意图 POD**（`Camera3DIntent`/`Camera2DIntent`），`DirectionalLight` 本身是 POD 故直接值传。S6 引入 `RSG::viewport` 后目标改 `RendererViewport`。
- ✅ `SubmitScene` → `ApplySubmitScene`（经 `RenderGlobals::submit_signal` 置位；该全局此前已装配但无人读，现被启用）。
- 至此 `render_server.cpp` **零**类型化 `Push`/`RenderCommandType`。验证 `ctest` 29/29。

### M3（已完成，2026-10-07）

- ✅ 删除 [render_commands.h](file:///d:/Work/ar_hud/servers/rendering/render_commands.h)（33 命令结构体 + `RenderCommandType` + `FunctionCallCmd`）、`command_executor.h/.cpp`、各 `*Cmd::Execute`。
- ✅ `CommandQueue` 去掉 `CommandExecutor` 依赖：thunk 签名简化为 `void(*)(const uint8*, uint32)`；`SetExecutor`/`executor_` 移除；类型标签 `Push<T>`/`PushAndSync<T&>` 删除（**保留** RD 用的 `PushAndSync(ExecuteFunc, void*)` 函数指针路径）。
- ✅ 队列内部条目类型由 `RenderCommandType` 枚举改为 `kCmdTypeNone/kPadding/kFunctionCall` 常量；`CommandDataSpan`/`ExecuteFunc`/`FunctionCallPayload` 迁入 `command_queue.h`。
- ✅ `EngineStackRefs` / `RenderViewport` 移除 `command_executor` 字段；`ARHudEngine` 移除 `CommandExecutor` 成员与装配。
- ✅ `test_command_executor.cpp` → [test_camera_routing.cpp](file:///d:/Work/ar_hud/tests/rendering/test_camera_routing.cpp)（直接驱动 `RenderServer::ApplyCamera3D/2D`、`ApplyDirectionalLight/Clear`）。
- 验证：构建 0 error、`ctest` 29/29。

---

## 附：Godot 对照表

| Godot 4.6 | 本工程（本 ADR 后） |
|---|---|
| `RenderingServerGlobals`（`static inline` 平铺） | `RenderGlobals`（`static inline` 平铺） |
| `Command{ T *instance; M method; Tuple<Args> args; }` | `CommandQueue::Push<S>(&S::Method, args…)` 生成的绑定命令 |
| `command_queue.push(RSG::material_storage, &Storage::method, …)` | `Push(&MaterialStorage::SetShader, …)` |
| `RenderingServerDefault`（RS 实现 + 队列） | `RenderServer`（主线程代理）+ `ARHudEngine`（装配/线程） |
| `CommandBase::call()`（vtable） | thunk（函数指针，**有意偏离**） |
| 变长数据按值（`Ref<Image>` 等，动态分配） | 内联 blob（`PushBlob`，**有意偏离**，保零分配） |