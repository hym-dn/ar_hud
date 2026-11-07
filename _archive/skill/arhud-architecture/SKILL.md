---
name: "arhud-architecture"
description: "AR HUD engine layered architecture design — 5-layer module structure, thread model, component responsibilities, dependency rules, current state after refactors (Camera→CameraMatrix, exceptions/RTTI enabled, CommandQueue SPSC). Invoke when reviewing architecture, adding modules, understanding layer boundaries, or onboarding to the codebase."
---

# AR HUD 分层架构设计（当前状态）

> 参考 Godot 4.6 四层模型（Scene → Servers → Drivers → Core），为 AR HUD 车规场景裁剪。
> 本文档描述**当前已实现**的架构状态，非目标状态。目标状态见 `arhud-render-thread-arch`
>（渲染搬线程）与 `arhud-multiscreen-arch`（多屏共享栈）skill。

---

## 〇、快速认知：三把钥匙

把本引擎读成「**一条流水线 + 三个线程域 + 一条依赖链**」，比读成「一堆并列的类」清楚得多。

**钥匙一 · 分层（谁依赖谁）**
`app → servers → drivers/platform → core`，严格单向。`platform/` 与 `drivers/` 均为
「契约在根、实现在子目录」的对称模式（详见 §一）。

**钥匙二 · 线程（三个域）**

- 主线程：只经 `RenderServer` 递命令，不碰渲染状态；
- 渲染线程（唯一）：独占 GL 与全部 Storage，消费命令 → 渲染 → readback；
- 呈现线程（每屏一条）：零 GL，只做上屏。

> 当前线程模型以 `arhud-render-thread-arch` / `arhud-multiscreen-arch` 为准（B3 已落地）。

**钥匙三 · 数据流（一帧的接力）**

```
应用 → RenderServer（打包小 delta 命令）══命令环══▶ 渲染线程
      Executor 改写 ItemStorage → RenderViewport 取景合成 → 渲染器绘制
      ══帧队列══▶ 呈现线程上屏
```
`dirty` 位门控：静态屏整段跳过（不渲染、不回读）。

> **三个常见阅读陷阱**：① `RenderServer` 的「直连/打包」双路径（B2 遗留）；
> ② `ItemStorage` 的「index 遍历 + RID 合成」双键；③ 部分注释仍停留在 B2/B3 之前。

---

## 一、五层结构

```
app/                        ← 应用层（demo）
  └─ 只用 ARHudEngine（PIMPL），不碰 GPU/平台/驱动
       │  依赖
servers/                    ← 渲染服务层（最大）
  ├─ ARHudEngine            多屏引擎（PIMPL，内部自建 DisplayServer/Window/RCD/Surface/Context/RenderViewport）
  ├─ RenderViewport           单屏管线（帧编排 + 渲染线程 + 后处理）
  ├─ RenderServer           主线程渲染 API 代理（app 唯一资源入口，序列化为命令，B2）
  ├─ ARHudCompositor        合成器（持有 5 个资源 Storage + renderer）
  ├─ renderer/              CanvasRenderer / SpatialRenderer
  ├─ storage/               两类容器：资源 Storage（GPU-backed，Compositor 持有）
  │                          与状态 Storage（ItemStorage，CPU-only，Engine 持有）
  ├─ RenderingDevice        RD（资源生命周期 + Staging + 帧循环 + 延迟销毁）
  ├─ CommandQueue           SPSC 序列化环形缓冲区（主→渲染线程命令传输，B1/B2）
  ├─ CommandExecutor        渲染线程命令执行器（反序列化 → 分发 Storage，B2）
  ├─ ItemStorage            状态 Storage（保留模式核心，B2-R）：CPU-only、零 GPU 依赖、
  │                         无 Initialize(rd)，**归 ARHudEngine 持有**（不在 Compositor 内）；
  │                         item 树层级见 ADR-0005
  └─ RCD/RDD 接口           抽象接口（定义在 servers，实现在 drivers）
       │  依赖
drivers/gl/                 ← GPU 驱动层
  ├─ GLRenderingContextDriver      RCD 的 GL 实现
  ├─ GLRenderingDeviceDriver       RDD 的 GL 实现
  └─ CommandBufferGL               CommandBuffer 的 GL 实现
       │  依赖
platform/                   ← 平台抽象层
  ├─ 接口（root）            window/screen/view/display_server/gl_context_manager
  └─ windows/               Win32 实现
       │  依赖
core/                       ← 基础层（零依赖）
  ├─ typedefs.h             基础类型、平台宏、Error 枚举
  ├─ template/              容器（LocalVector/HashMap/RID/PagedAllocator/Ref）
  ├─ os/                    内存/线程/同步（SpinLock/Mutex/Semaphore）
  ├─ io/                    日志（StdLogger/CompositeLogger/FileLogger）
  ├─ string/                ustring（UTF-32）
  └─ math/                  math_funcs（Vec/Mat/Color/Transform2D）、glm_types、camera_matrix
```

**依赖方向**：`app → servers → drivers/platform → core`，严格单向，禁止反向。

**注意**：`scene/` 目录已清空（Camera2D 删除、Camera3D 移走），当前是空库占位，Phase 6 放场景树。

---

## 二、线程模型（当前 Route A）

```
主线程（app + 渲染器 + 录制）              渲染线程（回放 + GL）
  │                                        │
  ├─ BeginFrame                            │
  ├─ 游戏逻辑（填充 item 列表）             │
  ├─ CanvasRenderer::RenderCanvasItems()   │
  │   └─ RD::DrawListBegin/Draw/End        │   ← GPU 命令录进 CommandBuffer
  ├─ EndFrame ────────────────────────────►├─ Wait 返回
  │                                        ├─ CommandQueue.Flush()
  │                                        ├─ ExecuteFrame（回放 CommandBuffer）
  │                                        ├─ SwapBuffers
  │                                        └─ loop 回 Wait
```

**当前模型（Route A + B2 资源命令化 + B2-R 保留模式）**：渲染器跑主线程（`RenderFromStorage`
读 ItemStorage），GPU 命令录进 CommandBuffer，渲染线程回放；资源/item 命令已经
RenderServer → CommandQueue → CommandExecutor 命令化（B2/B2-R 完成，item 命令 B2 过渡
期走 PushAndSync）。
**目标模型（Route B）**：渲染器搬渲染线程，主线程只发高层命令，渲染线程直接发 GPU 命令。
迁移计划见 `arhud-render-thread-arch` skill（B3-M0/B3 起）。
**多屏目标**：当前每个 RenderViewport 持一套完整栈（rd_/queue_/compositor_/item_storage_/
render_server_ + 独立渲染线程）；已决策按 Godot 多屏策略改为 Engine 级单共享栈 + 单渲染
线程 + 轻量视口（B3-M0 起实施），详见 `arhud-multiscreen-arch` skill。

---

## 三、各层组件职责

### 3.1 core/（基础层，零依赖）

| 模块 | 文件 | 职责 |
|---|---|---|
| typedefs | `typedefs.h` | uint8/uint32/float32 类型、ARHUD_* 宏、Error 枚举、ARHUD_ASSERT |
| 容器 | `template/` | LocalVector、HashMap、HashSet、RID、PagedAllocator、Ref、WeakRef、SafeNumeric、SafeFlag（ref/weak_ref 为 Object 层预留件，**0 使用属设计预期**，见 §八） |
| 同步 | `os/sync.h` | SpinLock、BinaryMutex、MutexLock、Semaphore（含 WaitTimeout）、RWLock |
| 线程 | `os/thread.h` | Thread（Start/WaitToFinish/GetCallerId）、PlatformFunctions 注入点 |
| 内存 | `os/memory.h` | ARHUD_NEW/DELETE、PagedAllocator 集成、Debug 泄漏追踪 |
| 日志 | `io/logger.h` | ARHUD_LOG_*、StdLogger、CompositeLogger、FileLogger（异步） |
| 数学 | `math/math_funcs.h` | Vec2/3/4、Mat4、Rect2、Color、Transform2D、Perspective/Orthographic/LookAt |
| GLM | `math/glm_types.h` | glm 集成（SIMD 数学） |
| 相机矩阵 | `math/camera_matrix.h` | CameraMatrix（3D 投影+视图矩阵，ex-Camera3D） |

### 3.2 servers/（渲染服务层）

| 组件 | 文件 | 职责 |
|---|---|---|
| ARHudEngine | `arhud_engine.h/.cpp` | 多屏引擎 PIMPL，app 唯一入口 |
| RenderViewport | `render_viewport.h/.cpp` | 单屏帧管线 + 渲染线程管理 + 后处理 |
| RenderServer | `render_server.h/.cpp` | **主线程渲染 API 代理（保留模式，对齐 Godot RenderingServer）**：资源 CRUD + item 管理 + 帧提交信号 |
| ItemStorage | `storage/item_storage.h` | **状态 Storage（保留模式核心，非资源 Storage）**：渲染线程 per-item 状态（RID→ItemData）+ dirty 门控；**CPU-only、零 GPU 依赖、无 `Initialize(rd)`**，由 **ARHudEngine** 持有（不在 Compositor 内）；item 层级树见 ADR-0005（parent 链，渲染收集时合成 effective transform/opacity/visibility/z_as_relative）。相机/灯光已于 ADR-0007 迁出（见 ViewportSceneState 行） |
| ViewportSceneState | `storage/viewport_scene_state.h` | **per-viewport 场景级状态**（`camera_3d`/`camera_2d`/`light`/`has_light`，ADR-0007）：纯数据、零 GPU 依赖，由 `RenderViewport` 自持（与 `cull_mask`/后处理同处）；写入经 `CommandExecutor`（命令侧）/`IViewportProvider` 解析视口，读取由 `RenderViewport::RenderFromStorage` |
| CommandExecutor | `command_executor.h/.cpp` | 渲染线程命令执行器（实现 ICommandExecutor，调 Storage） |
| ARHudCompositor | `renderer/compositor.h/.cpp` | 持有 **5 个资源 Storage + Renderer**；**不含 ItemStorage**（归 ARHudEngine） |
| CanvasRenderer | `renderer/canvas_renderer.h/.cpp` | 2D 批渲染（CanvasItem + TextItem） |
| SpatialRenderer | `renderer/spatial_renderer.h/.cpp` | 3D 深度+光照渲染 |
| RenderingDevice | `rendering_device*.h/.cpp` | RD（资源生命周期 + Staging + 帧循环 + 延迟销毁 + DrawList） |
| CommandQueue | `command_queue.h/.cpp` | SPSC 环形缓冲区（主→渲染线程代理） |
| 资源 Storage（5） | `storage/` | ShaderStorage/TextureStorage/MaterialStorage/MeshStorage/FontStorage —— **GPU 资源容器**：`RID → RD*_ID`，Create/Update/Free GPU 对象；Compositor 持有、纳入 `Initialize(rd)` 装配链 |
| 状态 Storage（1） | `storage/item_storage.h` | ItemStorage —— **CPU 状态容器**：item 数据 + item 树 + dirty；零 GPU 依赖；ARHudEngine 持有 |
| RCD 接口 | `rendering_context_driver.h` | 渲染上下文驱动抽象（Surface/Context/SwapChain/VSync） |
| RDD 接口 | `rendering_device_driver.h` | 渲染设备驱动抽象（Buffer/Texture/Shader/Pipeline/Fence） |

> **`storage/` 分类说明**：该目录并存两类容器，命名同源（都叫 Storage）但语义不同——
> 1. **资源 Storage（5 个）**：`RID → GPU 资源`，调用 `RenderingDevice`/RDD 创建与释放 GPU 对象，
>    由 `ARHudCompositor` 持有并纳入 `Initialize(rd)` 拓扑装配链。
> 2. **状态 Storage（ItemStorage）**：保留模式场景状态（item 数据 / item 树 / dirty 门控），
>    **零 GPU 依赖、无 `Initialize(rd)`**，由 `ARHudEngine` 持有。
>
> 两者共同满足唯一硬约束 `renderer → storage`（单向）；差异仅在两处——**是否创建 GPU 资源** 与 **持有者**。
> 详见 `item_storage.h` 头注释的「定位（区别于其他 Storage）」表。
>
> **注**：ItemStorage 曾含 "per-screen 相机/灯光"；该职责已按 **ADR-0007** 迁出为独立
> `ViewportSceneState`（由 `RenderViewport` 自持）——见 §九。

### 3.3 drivers/gl/（GPU 驱动层）

| 组件 | 职责 |
|---|---|
| GLRenderingContextDriver | RCD 实现：WGL 上下文、Surface、SwapChain、VSync、GL 函数加载 |
| GLRenderingDeviceDriver | RDD 实现：glCreateBuffer/glTexture/glShader/glDraw 等 |
| CommandBufferGL | CommandBuffer 实现：命令录制 → GL 调用回放 |

### 3.4 platform/（平台抽象层）

| 组件 | 职责 |
|---|---|
| IWindow/IScreen/IView/IDisplayServer | 平台抽象接口（窗口/屏幕/视图/显示服务） |
| IGLContextManager | GL 上下文管理接口（WGL 封装） |
| Win32DisplayServer/Win32Window/Win32Screen | Win32 实现 |

---

## 四、关键设计决策（已落地）

| 决策 | 内容 | 理由 |
|---|---|---|
| **PIMPL 隔离** | app 只 include `arhud_engine.h`，servers PRIVATE 链接 drivers/platform | 后端可插拔（Vulkan 时 app 零改动） |
| **CommandQueue SPSC** | 定长环形数组 + 原子读写指针 + 每线程 Semaphore | 无锁快路径、零动态分配、修复 SyncFlag P0 |
| **保留模式 RenderServer（B2 目标）** | item 状态存服务端 ItemStorage（渲染线程），增量更新；帧提交仅信号 | 对齐 Godot RenderingServer；增量更新高效；Phase 6 Scene Tree 天然衔接 |
| **Item 树形层级（ADR-0005）** | 三类 item parent 链（Text 挂 Canvas）；渲染收集时合成 effective transform/opacity/visibility/z_as_relative；Free 不级联摘到根 | 对齐 Godot RS canvas item 层级；面板/标签成组变换显隐淡入淡出；Phase 6 场景树承接面 |
| **所有权双体系** | Server 世界 = RID + RIDOwner + 显式 Free（在位，~1400 处使用）；Object 世界 = Ref/RefCounted/WeakRef（Phase 6 预留，0 使用属设计预期） | 服务器资源释放需帧边界同步 + 跨线程有序，引用计数语义不适配；Godot servers 内部同样不用 Ref（详见 §八） |
| **异常/RTTI 启用** | 移除 `/EHs-` + `-fno-exceptions` + `/GR-` + `-fno-rtti` | HUD 非硬实时，禁用是过度设计；gtest 需要 |
| **Camera 重构** | `Camera3D`→`CameraMatrix`（`core/math/`）、删 `Camera2D`（用 `Transform2D`） | 对齐 Godot core/math 分层；2D 投影 RenderViewport 内联算 |
| **2D 投影内联** | RenderViewport 内联 `Orthographic(0,w,h,0,-1,1)` | 对齐 Godot Viewport 内联算 2D 投影，无需 Camera2D 类 |
| **CanvasRenderer 解耦** | 签名从 `const Camera2D&` → `const float* projection` | 渲染器不依赖相机类型，只收预计算矩阵 |
| **测试基建** | GoogleTest + tests/ 模块化 + `build.bat tests` | B1 兜底 + 后续重构的保险 |

---

## 五、测试基础设施

```
tests/
├── CMakeLists.txt           ← 显式 TEST_SOURCES 列表（禁 file(GLOB)）
├── README.md                ← 构建/运行/添加测试文档
├── core/                    ← arhud_core 测试（预留）
├── rendering/               ← arhud_servers 测试
│   ├── test_command_queue.cpp  ← 6 用例（PushAndSync/多次/同线程/Shutdown/空队列/查询）
│   └── test_item_storage.cpp   ← 19 用例（item CRUD、ADR-0005 树层级/有效状态、dirty 屏级路由）
└── integration/             ← 集成测试（需 NullDriver，预留）
```

**构建**：`build.bat tests`（一键：conan `-o build_tests=True` → cmake → build → ctest）
**当前覆盖**：25 用例全绿——CommandQueue 6（PushAndSync 往返、多次连续、同线程短路、Shutdown、Flush 空队列、IsShuttingDown）+ ItemStorage 19（item CRUD、ADR-0005 树层级/有效状态合成、dirty 屏级路由）

---

## 六、已知问题（治理清单跟踪）

| 优先级 | 项 | 状态 |
|---|---|---|
| P0#1 | 延迟销毁队列竞态 | 缓修——B3 Free 搬渲染线程后自然修复 |
| P0#2 | Fence 超时检查 | 缓修——1s 超时 vs 16ms 帧，实际不触发 |
| P1 | ~~`ShaderCreateFromGLSL` 命名泄漏~~ ✅ 已完成（2026-09-27） | 已改名 `ShaderCreateFromSource` + 加 `ShaderSourceFormat` 参数（RDD/RD/GL）；Storage 早已合规命名 |
| P1 | `MaterialData::version` 竞态 | B3 后自然修复 |
| P2 | 三套 ID 体系（RID/RDBufferID/BufferID） | 待收敛 |
| P2 | 后处理硬编码进 RenderViewport | 待独立子系统 |
| P2 | `opengl32` PUBLIC 链接 / 命名空间扁平 / README 滞后 | 待修 |

---

## 七、与其他 Skill 的关系

| Skill | 关系 |
|---|---|
| 分层与依赖铁律 | 见根 `AGENTS.md`「Architecture」+ `docs/process/dev-workflow.md`（治理规则散落于此，无独立 governance skill）——本架构的**规则** |
| `arhud-render-thread-arch` | Route B 设计（渲染搬线程、RenderServer、CommandQueue 序列化、保留模式）——本架构的**目标演进**（B1/B2/B2-R 已落地，B3-M0 起） |
| `arhud-multiscreen-arch` | 多屏设计（Engine 级单共享栈 + 单渲染线程 + 轻量视口 + cull mask + 呈现调度）——本架构的**多屏目标演进**（B3-M0 起实施） |
| `arhud-improvement` | 下一步改进计划（B3-M0/B3/B3-P/B4/B5 + 治理项）——本架构的**行动项** |

---

## 八、所有权体系（RID 世界 vs Ref 世界）

> 两套**并行**的所有权体系，各自服务不同的层。Server 世界已在位并全面使用；
> Ref 世界随 Phase 6（Scene 层）启用。`core/template/ref.h` / `ref_counted.h` /
> `weak_ref.h` 当前 **0 使用属设计预期（0 users by design）**——勿当死代码删除。

### 8.1 分工

| | Server 世界（已在位） | Object 世界（Phase 6 预留） |
|---|---|---|
| 所有权机制 | `RID` + `RIDOwner` + 显式 `Free()` | `Ref<T>` / `RefCounted` / `WeakRef`（引用计数） |
| 释放时机 | 命令队列 → 渲染线程执行 + RD 延迟销毁（**帧边界同步**） | 最后一个强引用消失的瞬间（时机由引用图决定） |
| 现状使用量 | ~1400 处（servers/platform/drivers/app 全部资源） | 0 处（仅定义文件自身） |
| Godot 对应 | servers 内部同样 `RID_Owner` + explicit free——**Godot 服务器内部也不用 Ref 管资源** | Object→RefCounted→Resource（scene/editor 层，用户世界） |

### 8.2 为什么 Server 层禁用 Ref

1. **帧边界约束**：帧在途的 command buffer / present 槽仍引用 GPU 资源，释放必须等 RD 延迟销毁语义放行
2. **线程有序约束**：主线程发 Free 命令 → 渲染线程执行（ADR-0001），释放不能发生在任意线程的任意时刻
3. 引用计数的本质是"释放时机由引用图决定"，与上述两约束结构性冲突
4. **结论**：同一资源上两套体系是**替代关系**——Server 世界选了 RID，Ref 无用户是分层未到的必然状态，不是疏漏

### 8.3 有意偏离 #4：外部控制块 vs Godot 内部计数

- Godot 的 `RefCounted`：引用计数在**对象内部**（成员变量）
- 本工程的 `RefCounted`：**外部控制块**（`RefCountBlock` 独立分配，仿 std::shared_ptr/weak_ptr；
  `ref_counted.h` 开篇有大段论证——对象删除后控制块仍存活，消除 WeakRef 悬垂竞态）
- 启用后将沿用 **shared_ptr 语义**，不是 Godot 的内部计数语义——Phase 6 设计时需正式裁决（登记 `arhud-improvement` P2）
- 与既有三个有意偏离并列：#1 GL 走 RD 抽象（Godot GL 直连）、#2 无单例注入（Godot 全局单例）、#3 渲染/上屏分离（ADR-0003）

### 8.4 预期启用点

1. **Phase 6 Scene 层**——Node/Resource 若走引用计数
2. **Facade API**——HudTexture/HudMaterial 等 RAII 句柄叠加在 RenderServer 之上（`arhud-render-thread-arch` §九），句柄共享持有时 Ref 是现成件
3. **信号/回调系统**（若做）——WeakRef 防悬垂回调

> ⚠️ **启用前置检查**：ref/weak_ref 的 CAS 循环（`TryAddStrongRef`）、隐式弱引用时序、
> 跨线程 `Lock()` 从未被真实编译验证（0 使用 = 0 验证）——启用前必须补 core 单测
> （登记 `arhud-improvement` P3）。

---

## 九、ItemStorage 职责边界（ADR-0006 / ADR-0007 — 已落地）

> 状态：**已落地**（2026-10-01，ADR-0006 与 ADR-0007 同窗口实现）。相机/灯光已迁出 `ItemStorage`；
> 相机命令改为传"意图参数"。本节保留迁移的**决策脉络与落点**，作为 §四「已落地」的补充。

### 9.1 问题：ItemStorage 曾"四合一"

`ItemStorage` 曾同时承载四类职责，是维护者认知负担的**结构根源**：

1. **item 三表 + 层级树**（canvas/spatial/text；parent 链，ADR-0005）——**保留**；
2. **per-screen 相机/灯光**（`ScreenState`：`camera_3d`/`camera_2d`/`light`/`has_light`）——**已迁出**（ADR-0007）；
3. **屏级 dirty 门控**——**保留**（横切调度信号）；
4. **item → 屏幕路由**（`visibility_mask`）——**保留**。

第 2 类属"**视口 / 场景级状态**"，不是 item——与 Godot（`Viewport`/`Scenario` 与 `Instance`
分属不同 owner）不同源；且 per-screen 渲染配置（`cull_mask`/`viewport_index`/后处理/FBO）**本已属
`RenderViewport`**，相机/灯光是那个"异类"。

### 9.2 落地方案（ADR-0007 采纳 C：迁入 RenderViewport）

把 `camera_3d` / `camera_2d` / `light` / `has_light` 剥离为独立类型
**`ViewportSceneState`**（`storage/viewport_scene_state.h`）——**由 `RenderViewport` 每个视口自持一份**
（与 `cull_mask_`/`viewport_index_`/后处理同处，即"本屏如何取景"整类状态收拢于视口）；`ItemStorage`
只留 item 三表 + dirty 门。

- 新类型 CPU-only、零 GPU 依赖，定义在 `storage/` 层满足唯一硬约束 `renderer → storage`（单向）。
- **视口解析**：新增极简接口 `IViewportProvider`（`viewport_provider.h`，`RenderViewport *GetViewport(uint32)`）；
  **`ARHudEngine` 实现**（复用 `GetScreen(id)`），注入 `CommandExecutor` 与 `RenderServer`（旁路路径用）。
- **写**：`CommandExecutor` 4 处 → `viewport->Camera3DSet/Camera2DSet/DirectionalLightSet/DirectionalLightClear`
  + `item_storage_->MarkScreen(vp)`（置屏位）。
- **读**：`RenderViewport::RenderFromStorage` 1 处读自身 `scene_state_`。
- **dirty 门仍留 `ItemStorage`**（定位为"横切调度信号"）：视图状态写入后由 Executor 标屏 dirty，
  **不引入 `ViewportSceneState → ItemStorage` 反向引用**（两者平级）。
- **越界防御**：`GetViewport` 返回 nullptr → WARN + 跳过；取代原 `EnsureScreen` 的"自动扩容"。

### 9.3 已否决（ADR-0007 §3）

| 方案 | 结论 |
|---|---|
| **A. 留在 `ItemStorage`** | 否决——SRP 违背、与 Godot 不同源、阻断扩展 |
| **B. 兄弟对象（`ARHudEngine` 按屏持有）** | 否决——仅"换名"，状态仍未落到视口；与"per-screen 配置已在 `RenderViewport`"不一致 |
| 只搬相机、留灯光 | 否决——同属场景级状态，无理由拆开 |
| 顺带抽出屏级 dirty 为独立对象 | 暂缓——超出范围，保持最小改动 |

### 9.4 与 ADR-0006 的协同（相机命令参数化）

`Camera3DSetCmd` 改为携带**意图参数**（`projection_type`/`fov`/`position`/`target`/`up`/正交模式…），
**不再传矩阵**——`CameraMatrix` 是惰性重算的（`Set*`/`LookAt` 只置 `matrices_dirty_`，不立即重算；
`Get*Matrix()` 不触发重算），传"结果矩阵"会把**未重算的默认值**送达，致相机端到端不生效。
渲染侧 `Camera3DSetCmd::Execute`（原 `ExecCamera3DSet`，ADR-0009 起命令自派发）用
`CameraMatrix::SetPerspective/SetOrthographic` + `LookAt` 重建，
随后 `SpatialRenderer` 的 `UpdateMatrices()` 按**正确参数**重算。手工视图（`SetViewTransform`）
在命令链上**暂不支持**（`RenderServer::Camera3DSet` 检测 `IsUsingManualView()` → WARN + 忽略）。
详见 ADR-0006 §2。

### 9.5 已知滞后（另行同步）

- §二 标题仍为"线程模型（当前 Route A）"，正文停留在 B3 之前；
- §五 用例计数：当前实为 **29**（`test_command_queue` 6 + `test_command_executor` 4 + `test_item_storage` 19）。
