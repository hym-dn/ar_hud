---
name: "arhud-design"
description: "ARHud 渲染引擎代码设计唯一事实源：六层分层与依赖规则、servers 层对象地图、Route B 线程模型、帧节拍与多屏上屏、Godot 4.6 对齐目标与偏离清单、冻结接口规格（§7.11）、绞杀者迁移阶段（§7.12）。改动架构/接口/线程模型前先读本文并同步更新 ✅/🎯 标记；编码风格另见 arhud-coding-style skill。"
---

# ARHud 引擎架构规范（统一分层 · 唯一设计事实源）

> 状态：**现状 + 目标**（2026-11-08 快照）。各层先如实描述当前代码的真实结构 / 契约 / 线程模型（与代码一致）；
> servers 章节在其后给出对齐 Godot 4.6 的**目标规范**与**偏离清单**。
> 本文是工程**唯一代码设计文档**（原 `docs/architecture.md`，2026-11-08 迁入本 skill 并合并
> 旧 `skill/arhud-architecture` / `arhud-render-thread-arch` / `arhud-multiscreen-arch` /
> `arhud-improvement` 四份设计 skill 的仍有效内容——旧文档体系已删除，历史见 git）。
> 参考源码：Godot 4.6（`servers/rendering` 等）。编码风格 = Google C++ Style 原教，
> 见 `docs/skill/arhud-coding-style/SKILL.md`（全库格式迁移已于 2026-11-07 完成）。

## 0a. 进展指针

> 进度不在本文维护。当前位置、短/中/长期计划与完成历史见 **`docs/skill/arhud-progress/SKILL.md`**
> （唯一动态账本）。本文 §7.12 只定义阶段内容与验收标准；两者冲突时，阶段定义以本文为准、
> 进度状态以 progress skill 为准。

---

## 0. 阅读方式

| 章 | 内容 | 读者 |
|---|---|---|
| §1 | 分层模型与依赖规则（先读） | 全体 |
| §2 | 构建系统与模块 | 集成 / 新增模块者 |
| §3~§8 | core / scene / platform / drivers / servers / app 各层 | 对应模块开发者 |
| §9 | 横切主题：RID、线程、帧与上屏、依赖规则 | 全体 |
| §10 | 命名与编码规范 | 全体 |

约定：`✅ 现状` = 当前代码已实现；`🎯 目标` = 对齐 Godot 的重设计方向（尚未落地，代码未改）。

---

## 1. 总览：分层模型与依赖规则

### 1.1 分层栈

自底向上六层，静态库逐层叠加，依赖**单向**（对齐 Godot 4.6）：

```
┌────────────────────────────────────────────────────────────┐
│  app/           演示程序（唯一用户入口，经 ARHudEngine）      │
├────────────────────────────────────────────────────────────┤
│  scene/         场景树 HudScene / HudNode / HudCanvasItem    │
├────────────────────────────────────────────────────────────┤
│  servers/       渲染引擎（RD / 资源 / 状态 / 视口 / 帧编排）  │
├────────────────────────────────────────────────────────────┤
│  drivers/       后端契约 RCD/RDD + GL 实现                   │
├────────────────────────────────────────────────────────────┤
│  platform/      窗口 / 屏幕 / 显示服务 / GL 上下文 / 上屏器   │
├────────────────────────────────────────────────────────────┤
│  core/          基础类型 / 容器 / 同步 / 数学 / 日志 / 字符串  │
└────────────────────────────────────────────────────────────┘
```

- 依赖单向：`core → platform → drivers → servers → scene → app`。
- `scene/` 位于 `servers/` **之上**，依赖 `scene → servers`：场景树消费 servers 下发渲染命令；`scene/` 当前为空壳，内容随 Phase 6 填充（详见 §4）。
- 渲染输入数据（原 `FrameScene` 之类）归 **servers / 共享 `storage/`**，不由 `scene` 反向提供。

### 1.2 库依赖链

```
arhud_core           (glm)                       ← 零内部依赖
    ↑
arhud_platform       (core + user32/gdi32/opengl32)
    ↑
arhud_drivers        (core + platform + glad)    ← 契约在根，GL 实现在 gl/
    ↑
arhud_servers        (core + drivers[PUBLIC] + platform[PRIVATE] + glm + freetype)
    ↑
arhud_scene          (core + servers)            ← 场景树，位于 servers 之上
    ↑
arhud_demo_*         (arhud_scene + arhud_servers + arhud_core + glad)
```

### 1.3 各层职责（一句话）

| 层 | 职责 | 不做什么 |
|---|---|---|
| core | 引擎无关的基座：类型、容器、同步、线程、数学、日志、UTF-32 字符串 | 不知道窗口、GL、渲染 |
| scene | 场景树（HudScene / HudNode / HudCanvasItem），位于 servers 之上；Phase 6 填充 | 不产出渲染输入数据（归 servers/storage） |
| platform | 窗口 / 屏幕 / 显示服务 / GL 上下文管理 / 平台上屏器 | 不做渲染算法 |
| drivers | 三阶 GPU 抽象的**契约**（RCD/RDD）+ 各后端实现 | 不链接 servers（禁止倒挂） |
| servers | 引擎全部：RD、资源 Storage、状态 Storage、Renderer、视口、命令、帧编排、引擎入口 | 不直接建窗口（经 platform 契约） |
| app | 演示程序，经 `ARHudEngine` 驱动 | 不碰 platform/drivers 类型（phase2 豁免） |

### 1.4 三条硬性依赖规则

1. **契约归位**：接口头住模块**根**，平台/后端实现在子目录——
   `drivers/*.h`（契约）↔ `drivers/gl/`（实现）；`platform/*.h`（契约）↔ `platform/windows/`（实现）。
   对称于 Godot 的 `servers/rendering`（契约）↔ `drivers/`（后端）。
2. **`renderer → storage` 单向**：`servers/rendering/storage/` 下的头**禁止** `#include` `renderer/`。
   共享的纯数据类型（如 `DirectionalLight`）下沉到 `storage/`，由 renderer 消费。
3. **drivers 不倒挂**：`arhud_drivers` 不得链接 `arhud_servers`。
   `arhud_servers` 以 `PUBLIC` 链 `arhud_drivers`（公共头消费契约 + 装配根注入 GL 实现），
   `PRIVATE` 链 `arhud_platform`（PIMPL 内部使用，平台类型不外泄给 app）。

---

## 2. 构建系统与模块

### 2.1 根 CMakeLists 加载顺序（不可调换）

```
project(arhud)                       # 3.16+ / C + CXX（C 供 glad 生成代码）
├── include(cmake/option.cmake)      # ① 全部 option 定义
├── include(cmake/build.cmake)       # ② C++ 标准 / 编译标志 / 全局宏
├── include(cmake/dir.cmake)         # ③ 全局 include + 输出目录
├── include(cmake/lib.cmake)         # ④ 第三方依赖查找
├── add_subdirectory(core platform drivers servers scene)   # 依赖方向顺序
├── if(任一 demo) add_subdirectory(app)
└── if(ARHUD_BUILD_TESTS) enable_testing() + add_subdirectory(tests)
```

`lib.cmake` 依赖 `option.cmake` 的 `ARHUD_API_OPENGL`，故顺序固定。

### 2.2 构建选项（`cmake/option.cmake`）

| 选项 | 默认 | 说明 |
|---|---|---|
| `ARHUD_API_OPENGL` | **ON** | 启用 OpenGL 后端（当前唯一可用） |
| `ARHUD_API_VULKAN` | OFF | 预留，未实现 |
| `ARHUD_BUILD_TESTS` | **ON** | 编译 `tests/` |
| `ARHUD_BUILD_TOOLS` | OFF | 开发者工具（`tools/` 尚不存在） |
| `ARHUD_BUILD_DEMO` | **ON** | 编译全部 demo |
| `ARHUD_DEMO_PHASE2 / PHASE4 / PHASE4_CANVAS / PHASE4_SPATIAL / PHASE4_COMPOSITOR / PHASE4_TEXT` | **ON** | 分项 demo 开关 |
| `ARHUD_ENABLE_CXX20` | OFF | 切 C++20（默认 C++17） |
| `BUILD_SHARED_LIBS` | OFF | 静态库 |

> **缓存陷阱**：`app/CMakeLists.txt` 用 `option(... OFF)` 重复声明 demo 开关，但 `option.cmake` 先加载且值已入缓存，**先声明者胜**——实际默认全部 **ON**。

### 2.3 全局编译设置（`cmake/build.cmake`）

- C++17（`CXX_STANDARD 17`，`EXTENSIONS OFF`）；`ARHUD_ENABLE_CXX20=ON` 时切 20。
- 未指定 `CMAKE_BUILD_TYPE` 时默认 `Debug`。
- 全局宏：`ARHUD_DEBUG=1/0`（按 config）、`ARHUD_API_OPENGL=1`、`ARHUD_API_VULKAN=1`。
- MSVC：`/W3 /utf-8 _CRT_SECURE_NO_WARNINGS`；GCC/Clang：`-Wall -Wextra -Wpedantic`。
- 异常 / RTTI **启用**（ADR-0004）；引擎代码本身不抛异常，错误以 `Error` 返回。
  （`build.cmake` 头注释"禁用异常/RTTI"为过时描述，实际未加对应标志。）

### 2.4 各模块 target / 依赖

| 模块 | target | 源 | 依赖 |
|---|---|---|---|
| core | `arhud_core` | memory/thread/logger/logger_file/ustring/camera_matrix.cpp | PUBLIC `glm::glm` |
| scene | `arhud_scene` | `scene_placeholder.cpp` | PUBLIC `arhud_core arhud_servers` |
| platform | `arhud_platform` | windows/*.cpp | PUBLIC `arhud_core`；WIN32 PUBLIC `user32 gdi32 opengl32` |
| drivers | `arhud_drivers` | gl/*.cpp（`ARHUD_API_OPENGL` 时） | PUBLIC `arhud_core arhud_platform`；OPENGL PUBLIC `glad::glad` |
| servers | `arhud_servers` | rendering/* + storage/* + renderer/* | PUBLIC `arhud_core arhud_drivers`；PRIVATE `arhud_platform`；OPENGL PUBLIC `glm::glm`；找到 Freetype PUBLIC `Freetype::Freetype` + `ARHUD_FREETYPE_ENABLED=1` |
| tests | `arhud_tests` | rendering/test_*.cpp | PRIVATE `arhud_servers arhud_core GTest::*` |
| app | `arhud_demo_*` ×5 | `<dir>/main.cpp` | PRIVATE `arhud_scene arhud_servers arhud_core`（phase2 额外 `arhud_drivers arhud_platform`）；OPENGL 加 `glad::glad` |

> **新增源文件**：在模块 `CMakeLists.txt` 的**显式列表**中手写追加，禁止 `file(GLOB)`。

### 2.5 产物布局

`cmake/dir.cmake`：`CMAKE_RUNTIME_OUTPUT_DIRECTORY=build/bin`，`LIBRARY/ARCHIVE=build/lib`；
VS 多配置追加 `/Debug`、`/Release`。产物：`build/bin/<cfg>/arhud_demo_*.exe`、`build/lib/<cfg>/arhud_*.lib`。

---

## 3. core 层

`arhud_core` 是引擎基座，**零内部依赖**，仅 PUBLIC 链 `glm`。

### 3.1 `typedefs.h` — 基础类型与宏

- **整型别名**：`uint8/16/32/64`、`int8/16/32/64`、`float32/float64`（禁裸 `int`/`unsigned` 于资源相关代码）。
- **平台/编译器探测宏**：`ARHUD_PLATFORM_WINDOWS` 等 + `ARHUD_ALWAYS_INLINE / NEVER_INLINE / LIKELY / UNLIKELY`、`ARHUD_ARRAY_SIZE / ALIGN_AS / CACHE_LINE_SIZE(64) / TOSTR`。
- **`enum class Error : int32`**：`kOK=0`、`kFailed`、`kUnavailable`、`kUnconfigured`、`kUnauthorized`、`kParameterRangeError`、`kOutOfMemory`、`kBusy`、`kLocked`、`kTimeout`、`kCantConnect`、`kAlreadyExists`、`kDoesNotExist`、`kInvalidParameter`——**引擎统一错误返回**。
- **内存宏**：`ARHUD_NEW / ARHUD_DELETE`、`ARHUD_MALLOC_ZEROED / ARHUD_FREE`（禁裸 `new`/`delete`/`malloc`/`free`）。
- **`ARHUD_ASSERT`**（转发 `arhud::AssertFailed`）、`ARHUD_DISABLE_COPY / MOVE / COPY_MOVE`、`ARHUD_MAIN()`（Windows 下展开为 `WinMain`）。

### 3.2 `os/` — 内存 / 线程 / 同步

- **`memory.h/.cpp`**：`PagedAllocator`（热路径定长分配，O(1) 无碎片）+ 泄漏检测（`memory::DumpLeaks()`，Debug 收尾用）。
- **`thread.h/.cpp`**：`Thread` 类（`Start / WaitToFinish`、`Priority` = `kPriorityLow/Normal/High`、`kMainThreadId`、`GetCallerId()`）；平台差异经 `PlatformFunctions` 注入。
- **`sync.h`**：`SpinLock`（CACHE_LINE 对齐，用户态自旋，含 `Guard`）、`Mutex`(递归)/`BinaryMutex`(非递归) + `MutexLock`(RAII，含 `TempUnlock/TempRelock`)、`Semaphore`（计数信号量，`Post/Wait/TryWait/WaitTimeout`——命令队列与帧同步用）、`RWLock` + `RWLockRead/RWLockWrite`。

### 3.3 `io/` — 日志

`logger.h/.cpp` + `logger_file.h/.cpp`：`LogLevel`(Verbose/Info/Warn/Error/Fatal)、抽象 `Logger`、`StdLogger`（stderr 默认后端）、`CompositeLogger`（多通道）、`FileLogger`；全局宏 `ARHUD_LOG_VERBOSE/INFO/WARN/ERROR/FATAL`。格式化用 `vsnprintf`（计算函数，规避"禁 printf"），Release 下 VERBOSE 编译为空；`LogV` 内部加锁保证线程安全。

### 3.4 `template/` — 容器与句柄

- **容器（禁 COW）**：`LocalVector`、`HashMap / HashSet`、`List`、`FixedVector`、`VectorView`（非拥有视图）、`BitField`、`PagedAllocator`。
- **`rid.h`**：`RID`（64 位不透明句柄）、`RIDOwner<ThreadSafe>`（代际索引簿记，防悬垂/复用）、`DEFINE_ID(T)` 宏——全引擎句柄基座（详见 §9.1）。
- **`safe_refcount.h`**：原子引用计数原语（零 OS 依赖）。
- **`ref.h / ref_counted.h / weak_ref.h`**：Object 层所有权（`Ref<T>`/`WeakRef<T>`），**为 Phase 6 预留，当前 0 使用者**。

### 3.5 `string/` — UTF-32 字符串

`ustring.h/.cpp` 定义 `String`（servers 的 storage 头以 `using UString = String;` 引用）：内部 `LocalVector<char32_t>`（UTF-32，O(1) 索引），末尾保 `U'\0'`；与 C API 交互转 UTF-8（`Utf8()/CStr()`）；`Format()` 静态格式化（不支持 printf 变参）。**不实现 Godot String 全量方法**，仅 HUD 所需核心。

### 3.6 `math/` — 数学

`math_funcs.h`（`Vec2/Vec3/Mat3/Mat4/Color/Transform2D` 等轻量数学）、`glm_types.h`（glm 适配）、`camera_matrix.h/.cpp`（`CameraMatrix`：投影/视图**惰性重算**，透视/正交 + `LookAt`；servers 相机命令经其意图参数重建，见 §7.6）。

### 3.7 core 规范要点

- 禁止：`iostream`、`printf/sprintf`、裸 `new/delete`、`malloc/free`、`goto`。
- 容器一律自研、非 COW；`using` 优于 `typedef`；`enum class` 唯一。
- 异常/RTTI 虽启用（ADR-0004），core 代码亦不抛异常。

---

## 4. scene 层

`arhud_scene` 位于 **`servers/` 之上**，依赖 `arhud_scene → arhud_servers`（+ `core`）——`arhud_servers` 不再链 `scene`，方向与 Godot 一致。

`scene/` 当前为**空壳**：`scene_placeholder.cpp` 仅 `namespace arhud {}`。Phase 6 填充 `HudScene` / `HudNode` / `HudCanvasItem`（场景树消费 servers 下发渲染命令）；渲染输入数据类型（原 `FrameScene` 之类）归 servers / 共享 `storage/`，由 `ItemStorage` 增量命令承载，scene 不反向提供。

---

## 5. platform 层

契约头在 `platform/` 根，Win32 实现在 `platform/windows/`。PUBLIC 链 `core` + 系统库。

### 5.1 `IDisplayServer`（`display_server.h`）

全平台门面，**单例**（`GetSingleton()`），持有屏幕 / 窗口 / GL 上下文管理器。

- 类型：`using ScreenID = IScreen::ScreenID`、`using WindowID = IWindow::WindowID`。
- 接口：`Initialize/Shutdown`、`GetScreenCount/GetScreen/GetPrimaryScreen`、
  `WindowCreate(WindowDesc)/WindowDestroy/GetWindow/GetWindowCount`、
  `ProcessEvents`、`SetVsyncMode/GetVsyncMode`、`GetGLContextManager`。

### 5.2 `IScreen`（`screen.h`）

`ScreenID = uint32`，`kInvalidScreenId = UINT32_MAX`。
`ScreenInfo{ id,x,y,width,height,dpi,refresh_rate,scale,is_primary,orientation }`、
`ScreenOrientation`、`kBaseDpi = 96.0f`；`GetNativeHandle()`（Win32 HMONITOR）。

### 5.3 `IWindow`（`window.h`）

`WindowID = uint32`，`kInvalidWindowId = UINT32_MAX`。

- `WindowMode`（kWindowed/kFullscreen/kBorderless/kExclusiveFullscreen）、`VSyncMode`（kDisabled/kEnabled/kAdaptive）。
- `WindowDesc{ width=1280,height=720,x=-1,y=-1,title,mode,vsync_mode,resizable,visible,screen_id=UINT32_MAX,native_window,external_window }`。
- `WindowCallbacks`：`OnResize / OnPaint / OnClose / OnFocus / OnKey / OnChar / OnMouse / OnMouseMove / OnScroll / OnTouch` + `userdata`。
  **`OnPaint` / `OnResize` 是外部重绘（WM_PAINT / WM_SIZE）的入口**——主线程收到后触发引擎强制全屏重渲（ADR-0008，见 §7.7）。

### 5.4 `IGLContextManager`（`gl_context_manager.h`）

平台 GL 底座，提供**统一 Context 模型**（无 master/slave、无 `wglShareLists`）。

- 类型：`ContextID/SurfaceID = uint32`，`kInvalidContextId / kInvalidSurfaceId`。
- 接口：`Initialize/Shutdown`、`CreateContext(SurfaceID)/DestroyContext/GetContextCount`、
  `CreateSurface(ScreenID,void*,w,h)/DestroySurface`、
  `MakeCurrent(ContextID,SurfaceID)`（独占绑定并记录 surface 归属）、`ReleaseCurrent()`、
  `SwapBuffers(SurfaceID)`（**per-surface，非 per-context**——为"零 GL 的上屏线程"铺路）、
  `SetVsyncMode/GetVsyncMode`、`GetDeviceContext/GetRenderContext/GetGLProcAddress/IsInitialized`。
- **两种模式**：Mode A（单线程多 surface 切换 drawable——**当前引擎采用**）；Mode B（多线程多 Context——**保留能力未用**）。
- WGL 要求像素格式索引一致；per-surface vsync 由实现重放。

### 5.5 `IFramePresenter`（`frame_presenter.h`）

**平台上屏边界**（零 GL）

- 接口：`Initialize(void* native_window,w,h)/Finalize/Resize`、
  `Present(pixels,w,h,stride)`（Win32 走 GDI `StretchDIBits`）、
  `SupportsSwapPresent()`（默认 false）、`SwapPresent()`（默认 `kUnavailable`）。
- **模式 R（kReadback）**：CPU 像素 → `Present()`。
- **模式 S（kSwap）**：`SwapBuffers + DwmFlush`，零回读零拷贝（Windows 方案 B）。

---

## 6. drivers 层

三阶 GPU 抽象：**RCD → RDD → RD**。契约头在 `drivers/` 根，GL 实现在 `drivers/gl/`。
`arhud_drivers` PUBLIC 链 `core + platform`（+ `glad`），**不链 servers**。

### 6.1 `rendering_device_commons.h` — 共享词表

- **RDD 句柄 ID 集**：`ARHUD_DEFINE_RDD_ID(...)` 生成 `RDBufferID/RDTextureID/RDSamplerID/RDShaderID/RDUniformSetID/RDVertexFormatID/RDRenderPassID/RDFramebufferID/RDPipelineID/RDCommandBufferID` 等。
- **`DataFormat`**（R8G8B8A8Unorm / D24UnormS8Uint / …）、**`TextureFormat`/`TextureView`**、`TextureUsageBits`、`TextureSamples`、`TextureLayout`、`AttachmentLoadOp/StoreOp`、`Attachment`、`Subpass`、`SubpassDependency`、shader/pipeline/renderpass/framebuffer 家族类型。

### 6.2 `IRenderingContextDriver`（RCD）— 上下文层

负责 GL 上下文与 surface 生命周期（驱动侧视图）。承载 Mode R / Mode S 两种呈现能力。
类型/方法与 `IGLContextManager` 对应（Surface/Context 生命周期），是 servers 装配根注入的实现契约。

### 6.3 `IRenderingDeviceDriver`（RDD）— 设备驱动层

API 无关的**具体后端接口**：资源创建/销毁、绘制命令、同步、shader 编译元数据（GL uniform locations / program）。
头内含 B3 状态表（活 / 退中 / 占位 / 偏差）。

### 6.4 命令录制模型（立即模式）

RD 的 `DrawList/ComputeList` 直接调用 `IRenderingDeviceDriver::CommandXxx()` 下发命令，
**无**多态 `ICommandBuffer` 对象层与录制-回放层。命令抽象缝位于
`IRenderingDeviceDriver` 抽象类（此处对齐 Godot `RenderingDeviceDriver`）。
本工程**无资源追踪 / DAG**（Godot 的 `ResourceTracker + DAG`）——依赖图 / RDG 的占位设计见 §7.13。

**后端分叉**：

| 后端 | 命令模型 | 说明 |
|---|---|---|
| GL（当前） | **立即提交** | `GLRenderingDeviceDriver::CommandXxx()` 直接调 GL，无录制层 |
| RD（Vulkan/D3D12，未来） | **录制 + RDG** | 命令缓冲由驱动**内部**持有（`VkCommandBuffer`）+ 依赖图（§7.13） |

> ⚠️ **偏离 Godot：本工程把 GL 也接进 RD（见 §7.10 D10）**
> Godot 的 OpenGL 驱动**完全不使用 RenderingDevice**（官方架构文档明确
> "The OpenGL driver does not use the RenderingDevice abstraction"）：只有
> Vulkan / Direct3D 12 / Metal(MoltenVK) 才有 RD 实现，GL/Compatibility 后端是独立的
> `drivers/gles3/`（`RasterizerCanvasGLES3` / `RasterizerSceneGLES3`），直接下 GL 调用。
> 本工程反之——**把 GL 接到 `IRenderingDeviceDriver` 这层 RD 缝里**：由 GL 驱动承担
> `CommandXxx()` 命令抽象，使 app/servers 不感知后端差异，并为未来 Vulkan 后端保留同一 RD 契约。
> 代价：GL 侧多一层接口间接，且 RD 的"录制 / 资源追踪"语义对 GL 当前为空壳。

> ⚠️ **对齐 Godot，但本工程 RD 契约更薄：Vulkan 接入需回补（见 §7.10 D11）**
> Vulkan 是 Godot 的 **RD 主后端**，故本工程接 Vulkan 属**对齐**方向——复用同一
> `IRenderingDeviceDriver` 契约，与 GL 后端并存。但须注意：Godot 的驱动层契约本身就含命令
> 缓冲 / 队列 / 同步原语——`RenderingDeviceDriver`（`servers/rendering/rendering_device_driver.h`）
> 定义了 `CommandBuffer` / `CommandPool` / `CommandQueue` / `Fence` / `Semaphore` 等不透明 ID。
> 本工程 RDD 契约当前按 GL 立即模式裁剪，已退役这些录制 / 提交原语（仅留 `CommandBufferID` /
> `CommandBufferType` 占位）。接 Vulkan 时需**回补**：驱动内部持有 `VkCommandBuffer`、命令池与
> 队列提交、帧同步（`Fence` / `Semaphore`）；上层 RDG / 依赖图见 D8（§7.13）。

> ✅ **已清理（Route A 遗留退役，2026-10-11）**：`ICommandBuffer` 多态对象层、
> `CommandType`（19 种）/ `CmdXxx` POD / `RecordedCommand` 录制-回放词汇表，
> 随 `command_buffer.h` / `command_buffer_gl.{h,cpp}` 一并移除。RD DrawList 直调
> `device_driver_->CommandXxx()`（契约缝保留在 `IRenderingDeviceDriver` 抽象类）。

---

## 7. servers 层

servers 是引擎主体：**上承 app，下接 platform/drivers**，拥有除平台/驱动外的全部运行时对象。
本章先给 **✅ 现状**（与代码一致），再给 **🎯 目标**（对齐 Godot）与 **偏离清单**。

### 7.1 ✅ 现状：对象与所有权

`ARHudEngine`（`arhud_engine.h/.cpp`）是顶层入口，内部经 **PIMPL** 持有平台/驱动对象，并拥有**共享渲染栈**（B3-M0.1 起从 RenderViewport 上提，多屏共享一套）：

```
ARHudEngine
├── Impl (PIMPL)                          ← 平台/驱动类型不外泄
│   ├── Win32DisplayServer / GLContextManagerWin32 / GLRenderingContextDriver
│   └── 共享渲染栈（×1，全局）
│       ├── RenderingDevice rd            ← 持有 RDD 驱动（命令直录）
│       ├── CommandQueue
│       ├── ARHudCompositor               ← 5 资源 Storage + 2 Renderer
│       ├── ItemStorage                   ← 全局 RID 的保留模式 item 账本
│       └── RenderServer                  ← app 唯一资源入口
├── RenderViewport[0]                     ← 轻量视口：offscreen surface/context + FBO + 后处理 + EngineStackRefs
├── RenderViewport[1] …
└── FrameImageQueue[i] + PresentThread[i] ← 每屏一套帧槽 + 上屏线程
```

`RenderViewport` 通过 `EngineStackRefs`（共享栈指针集合）引用上表对象，自身**不拥有**共享栈：
`{ rd, command_queue, item_storage, render_server, compositor, offscreen_surface_id, offscreen_context_id, presenter }`。

### 7.2 ✅ 现状：三阶抽象之上的装配

```
ARHudEngine ──BuildCompositor──► ARHudCompositor::Initialize(rd)   # 7 步顺序
   ShaderStorage → TextureStorage → MaterialStorage → MeshStorage → FontStorage
   → CanvasRenderer → SpatialRenderer                              # Finalize 逆序
RenderingDevice::Initialize(RCD*, frame_count=2, staging_size=4MB, context_id, surface)
   └── 内部创建/持有 RDD 并 MakeCurrent
```

`ARHudCompositor` ≈ Godot `RendererCompositor`：仅负责子系统生命周期装配与
`BeginFrame(double)/EndFrame()` 帧节拍；**不做帧编排**，不持 ItemStorage。

### 7.3 ✅ 现状：主线程 ↔ 渲染线程（Route B）

| 组件 | 线程 | 角色 |
|---|---|---|
| `RenderServer`（`render_server.h`） | 主线程 | app 可见的**唯一**资源/ item API；把调用序列化为命令经 `CommandQueue` 发往渲染线程；**不持 GPU 资源** |
| `CommandQueue`（`command_queue.h`） | 生产=主线程 / 消费=渲染线程 | v3 **SPSC 1MB 环形缓冲**；16B `CommandHeader`；ADR-0009 自派发 thunk；ADR-0010 绑定调用 `PushBound` |
| `RenderServer::Apply*`（S1 过渡桥） | 渲染线程 | 相机/灯光/提交的**命令目标方法**（`ResolveViewport` + 写入 + 置屏 dirty）；命令绑到 `RenderServer` 自身，S6 迁往 `RendererViewport` |

**ADR-0010 命令形态**（2026-10-07 起）：全部命令为绑定调用 `PushBound(存储/目标, 方法, POD 参数)`（变长数据走 `PushBoundBlob`）。
创建类用 **RID 分裂**（主线程先 `XxxAllocate` 立即拿回 RID，初始化异步入队渲染线程）；
变更/释放/帧命令 `PushBound` 发完即走，真并行（RD 延迟销毁保证 GPU 安全）。

`CommandQueue` 要点：`kCapacity=1<<20`、`kAlignment=16`、`kMaxCommandSize=0xFFFF`；
cache-line 对齐的 `write_pos_/read_pos_`；`notify_semaphore_` 唤醒渲染线程。队列已收敛为
**纯异步 SPSC**——ADR-0010 后 RD 遗留的同步函数指针代理路径（`kFunctionCall` / `PushAndSync`）
及其同步机器（`sync_sems_` / `WaitSync` / `GetSyncIdx` / `kMaxSyncThreads`）已全部移除。
v3.2 唯一的**阻塞出口**是整批屏障 `Sync()`（对齐 Godot `sync()`，见 §7.11）；
`Flush()` 另有 `thread_local` 再入保护 + "只能由执行者线程排空"的断言。
`RenderServer` 的**短路规则**（对齐 Godot 线程闸门：执行者直调前先排空）：

| 条件 | 行为 | 场景 |
|---|---|---|
| 队列无消费者 | 先 `FlushIfPending()` 再直调 Storage | 初始化期（渲染线程未起，主线程持 GL 上下文）/ 单测 |
| 调用者即渲染线程 | 先 `FlushIfPending()` 再直调 Storage | 渲染线程内部回调（批内调用被再入保护挡成空操作） |
| 队列已 shutdown | 直调 Storage（**不排空**残令） | 引擎关闭后清理 |
| 其余 | 走命令队列 | 常规运行期 |

### 7.4 ✅ 现状：命令词汇与全局可达点

- ⚠️ **`render_commands.h` 已删除**（ADR-0010 M3，2026-10-07）：`RenderCommandType` 枚举与 33 个 `*Cmd` POD 全部退场。
  命令统一为绑定调用 `PushBound(目标, 方法, POD 参数)`；变长数据（像素、名称、码点）经 `PushBoundBlob` 内联 blob 段传递。
  队列仅保留内部条目类型 `kCmdTypeNone/kPadding`（见 `command_queue.h`；RD 遗留的 `kFunctionCall` 已删除）。
- `render_globals.h`：`RenderGlobals`（ADR-0010，`static inline` 指针平铺，对齐 Godot `RSG`）——
  暴露 5 个资源 Storage、2 个 Renderer、ItemStorage、`IViewportProvider`、`submit_signal`。
  **唯一写入者 = ARHudEngine**（装配期填入，释放期 `Reset()`）。

### 7.5 ✅ 现状：状态 Storage 与 Item 层级（ADR-0005）

`ItemStorage`（`storage/item_storage.h`）是**保留模式 item 账本**（CPU-only、零 GPU、渲染线程独占）。

- 三张平表：`CanvasItemData` / `SpatialItemData` / `TextItemData`；`RIDPair<T>` 承载 RID→数据。
- **层级（ADR-0005）**：item 经 `parent` RID 组成森林（Canvas/Text 共享一套语义，Spatial 独立）。
  有效状态沿父链合成：**变换连乘、opacity 连乘、visible 取 AND、z_order 按 `z_as_relative` 累加**；
  提供 `*GetEffective*` 查询与 `CanvasItemEffectiveState/TextItemEffectiveState`。
- **脏位**：单一 `std::atomic<uint32> dirty_screen_mask_`（≤32 屏位图）——
  既表"是否有变化"(IsDirty) 又表"哪些屏脏"(IsScreenDirty)；`Mark*` 是**唯一**置位入口。
  消费为**逐屏制**（2026-11-08）：`RenderThreadFunc` 仅在某屏 `RenderAndSubmit()` 返回
  true（真正渲完+MarkReady）后 `ClearScreenDirty(i)`；背压跳过的屏保留脏位待下轮 submit
  重试——旧的全局 `ClearDirty` 会吞掉跳过屏的更新（首帧丢失竞态，已修）。
- item RID 一经分配**不复用**（无代际），线性 O(n) 查找对 HUD 规模足够。

### 7.6 ✅ 现状：视口与 per-viewport 场景状态（ADR-0006/0007）

- `RenderViewport`（`render_viewport.h`）＝每屏帧编排：`Config{width,height}`、
  `Render(...)` / `RenderAndSubmit()`（渲染线程入口：脏→HasInFlight→取槽→渲 FBO→回读→READY）、
  `SetFrameImageQueue`、`SetCullMask`、`SetViewportIndex`、`HandleResize`/`ApplyPendingResize`、`PostProcessParams`（gamma/distortion/premul_alpha）。
- `ViewportSceneState`（`storage/viewport_scene_state.h`）：per-viewport 的 `camera_3d / camera_2d / DirectionalLight light / has_light`
  ——ADR-0007 把相机/灯光从 ItemStorage 迁到**视口自持**（属"本屏如何取景"）。
- `IViewportProvider`（`viewport_provider.h`）：`GetViewport(uint32)` 按索引解析视口（相机/灯光命令路由），越界返回 nullptr。
  `ARHudEngine` 即其实现（`GetViewport(p_index) == GetScreen(p_index)`）。
- 相机命令只携带**意图参数**（fov/position/target/up…），渲染侧经 `CameraMatrix` 重建（ADR-0006），避免传输未重算矩阵。

### 7.7 ✅ 现状：帧节拍与上屏（多屏）

```
主线程                                   渲染线程                       每屏上屏线程
BeginFrame ─► ProcessEvents
  │ 经 RenderServer 发 Set*（异步）
  ├─ SubmitScene ──(Push)────────────────► Flush → 逐屏 RenderFromStorage
  │                                          └─ 脏屏：渲 FBO → glReadPixels → MarkReady
  │                                                                     │
  │                                          ◄────── frame slot（HasInFlight 背压，≤1 在飞）
EndFrame ─► (帧节拍)                                                   AcquireReady
                                                                        ├─ Mode R: Present(CPU 像素)
                                                                        ├─ Mode S: SwapPresent(零拷贝)
                                                                        └─ Release（唤醒渲染线程）
```

- `FrameImageQueue`（每屏双槽）：状态机 `FREE → WRITING → READY → COMMITTED → ON_SCREEN → FREE`；
  `HasInFlight()` 防渲染超前（每屏最多 1 帧在飞）；`Release()` 回槽并唤醒渲染线程（free-after-release 防死锁）。
- `PresentThread`（每屏，零 GL）：`AcquireReady → Present/SwapPresent → MarkOnScreen → Release`；
  vsync 等待各屏并行。`PresentMode`：`kReadback`（默认）/ `kSwap`（Windows 方案 B）。
- **强制全屏重渲（ADR-0008）**：窗口 `OnPaint`/`OnResize` → 主线程经 `WindowCallbacks` 调 `RequestFullRedraw()`，
  只置原子 `force_redraw_` 并唤醒渲染线程（**不触碰 ItemStorage**）；渲染线程消费标志时执行 `MarkAllScreens()`。
- **窗口 resize 重建（B3-M0.3）**：主线程 `HandleResize` 置原子 `resize_pending_ / new_width_ / new_height_` 并唤醒渲染线程；
  渲染线程每轮循环经 `ApplyPendingResize()` 消费——无在途帧时 `RenderBuffers::Recreate` + `FrameImageQueue::Resize`
  重建 FBO/帧队列（后处理 uniform set 按新颜色纹理重绑），有在途帧则保留 pending，待 present 释放槽位后下轮重试。

### 7.8 ✅ 现状：资源 Storage 与 Renderer

| 类别 | 成员 | 说明 |
|---|---|---|
| 资源 Storage（GPU-backed，归 Compositor） | `ShaderStorage / TextureStorage / MaterialStorage / MeshStorage / FontStorage` | RID → `RD*_ID` 簿记；持业务元数据（如 Shader 源码缓存、uniform 反射），GPU 资源归 RD |
| Renderer（归 Compositor） | `CanvasRenderer`（2D 批渲染）/ `SpatialRenderer`（3D 深度+光照） | 消费 Storage 与 `DirectionalLight`；不反向依赖 |
| 状态 Storage（CPU-only，归 Engine） | `ItemStorage` | §7.5 |
| 辅助 | `RenderBuffers`（Color+Depth+RenderPass+FBO 打包，≈ Godot `RenderBufferData`）、`DrawListScope/ComputeListScope`（RAII） | |

RD（`rendering_device.h` + `_frame/_draw_list/_staging` 分文件）：`kDefaultFrameCount=2`、
Staging Buffer 三级分配（≤256B 小块/32B 对齐；≤64KB 主块/256B 对齐；>64KB 临时）、
`DEFINE_ID` 资源族 9 个 `RIDOwner`（`buffer/texture/framebuffer/render_pass/pipeline/uniform_set` = 线程安全 `true`，共 6 个；`sampler/shader/vertex_format` = `false`）、`BeginFrame/EndFrame` 帧时延销毁、`DrawList/ComputeList`、Pipeline 缓存。

RD 是**渲染线程专用 API**：资源创建/更新方法进入即断言 `IsRenderThread() || command_queue_ == nullptr`（仅初始化期 `command_queue_` 未绑定时允许主线程直调）；跨线程一律经 `RenderServer` 的 RID 分裂路径路由（ADR-0010），RD 自身不再做单调用粒度的跨线程代理（原 `PushAndSync(lambda)` 代理分支已移除）。

### 7.9 🎯 目标：对齐 Godot 4.6 `servers/rendering`

现状三条主线偏离 Godot，目标即用 Godot 的机制替换：

1. **API 面**：`RenderServer` 是"窄代理 + CanBypassQueue + 手写 `PushBound` 分发"；
   Godot `RenderingServer` **本身就是全量 API 单例**，用 `FUNCn` 宏统一"本线程直调 / 异线程入队"。
2. **命令层**：✅ 已对齐（ADR-0010 M1~M3，2026-10-07）——`RenderCommandType` / 33 结构体 / `CommandExecutor` 已删除，
   统一为 `(目标, 方法, 参数)` 绑定调用（`PushBound`/`PushBoundBlob`），**无执行器、无类型枚举**；
   Godot 只有 `(instance, method, args)` **一种形态**，靠 `Command<T,M,…>::call()` 虚函数分发，**无执行器、无类型枚举**。
3. **状态层**：现状 `ItemStorage` 把 2D/3D/Text item 与层级、dirty 混成一个"状态 Storage"；
   Godot 拆为 `RendererCanvasCull`(2D) / `RendererSceneCull`+`RenderingMethod`+`RendererSceneRender`(3D) / `RendererViewport`，文本另立 `TextServer`。

**目标机制（Godot 六件套）**：

- **RSG 全局**：`RenderingServerGlobals`（`#define RSG RenderingServerGlobals`），`static inline` 持
  `rasterizer / texture_storage / canvas / scene / viewport / canvas_render / *_storage / text_server` + `threaded`。
  （取代本工程 `RenderGlobals`；命名归一到 S2。）
- **抽象 API + 默认实现**：`RenderingServer`（全量 virtual API 单例）+ `RenderingServerDefault`
  （持 `CommandQueue` + `server_thread`；`_init/_finish/_thread_loop/_draw/_free/_call_on_render_thread`；
  方法体由 `FUNCn` 宏展开）。
- **命令队列**：**保留 `CommandQueue`**（SPSC 环形缓冲）并扩展**绑定入队** `PushBound`——
  命令 =（目标, 方法, 显式 POD 参数）；thunk 调 `(instance->*method)(args…)`。
  > ADR-0010 裁决（2026-10-07）：**不采用** Godot 的 `CommandQueueMT`（`CommandBase{virtual call()}` + `Tuple`）——
  > 队列以 `memcpy` 搬运命令，vtable 对象 `memcpy` 属灰区（车规不友好），`std::tuple` 在 MSVC 下不保证 `trivially_copyable`。
  > 语义终点一致（命令=目标×方法、删执行器、平铺全局），机制取保守实现。
  >
  > 📌 Godot 侧实测校核（2026-10-13，`core/templates/command_queue_mt.h` @master）：Godot 确实**照做**了
  > 上述灰区——命令对象带 `virtual call()`，`memnew_placement` 进 `LocalVector<char> command_mem`，
  > 执行前再 `memcpy` 到 1KB 栈缓冲 `cmd_local_mem` 以规避 `resize` 引发的 realloc 竞态（GH-100509）。
  > 且它是 **BinaryMutex + ConditionVariable 的多生产者有锁队列**，缓冲区无上限增长（无背压）。
  > 本工程的"SPSC 无锁 + 1MB 有界 + 自旋背压 + POD thunk"是对实时性的**有意偏离**，非能力不足。
  - **整批屏障 `Sync()`**（2026-10-13 补齐，对齐 Godot `CommandQueueMT::sync()` =
    `push_and_sync(this, &_no_op)`）：谓词为"`read_pos_` 追平调用瞬间的 `write_pos_`"，
    唤醒 = 批末 `sync_waiting_` 命中则 `sync_semaphore_.Post()`，生产者每轮重敲
    `notify_semaphore_` 并以 `WaitTimeout` 兜底，故信号丢失/多余只影响延迟不影响正确性。
    **不**引回逐命令 `push_and_ret`（返回值回传）与超时机器——需要结果的一律走 RID 分裂
    或主线程镜像。`RenderingServerDefault::Sync()` 已从 S0 占位改为委托该屏障。
- **RID 分裂**：`FUNCRIDSPLIT(x)` = `x_allocate()`（任意线程，立即返回 RID）+ `x_initialize()`（服务线程直调或入队）。
  ✅ 已落地——`RenderServer` 各 `*Create*` 即此形态（`XxxAllocate()` 拿空壳 RID + `PushBound*` 异步入队初始化）。
  Godot 纹理族用 `FUNCRIDTEX*`，多一个 `|| rasterizer->can_create_resources_async()` 分支（Vulkan 可任意线程建资源，
  故连入队都省了）；本工程 GL 后端不具备该条件，恒走入队。
- **线程闸门**：`ASYNC_COND_PUSH = (Thread::get_caller_id() != server_thread)`；**本线程先 `flush_if_pending()` 再直调**。
  ✅ 已对齐（2026-10-13）——`RenderServer::CanBypassQueue()` 在"调用者即执行者"的两个分支
  （`IsConsumerThread()` / `!HasConsumer()`）里先 `queue_->FlushIfPending()` 再返回 true；
  `IsShuttingDown()` 分支**只直调不排空**（队列残令可能引用已释放资源）。
  配套两件事：① `CommandQueue::Flush()` 入口 `thread_local` 再入保护（批内 thunk 回调
  `FlushIfPending`/`Sync` 必须是空操作——`read_pos_` 只在批末发布，嵌套排空会用陈旧游标重复执行）；
  ② `Flush()` 断言执行者约束（注册了消费者时只能由该消费者排空）。
- **Storage 家族与 item 拆分**：

  | Godot（RSG 槽） | 本工程目标类 | 现状对应 | 职责 |
  |---|---|---|---|
  | `RendererCompositor`(`rasterizer`) | `RendererCompositor` | `ARHudCompositor` | 后端门面 + 持各 Storage |
  | `RendererTextureStorage` | 同名 | `TextureStorage` | 纹理 RID + GPU 资源 |
  | `RendererMaterialStorage` | 同名 | `MaterialStorage` + 部分 `ShaderStorage` | 材质 + 着色器 |
  | `RendererMeshStorage` | 同名 | `MeshStorage` | 网格 |
  | `RendererLightStorage` | 同名 | 现塞在 `ViewportSceneState` 的 light | 方向光 |
  | `RendererCanvasCull`(`canvas`) | 同名 | `ItemStorage` 2D 部分 | 2D item 树 / 属性 / 渲染列表 |
  | `RendererCanvasRender` | 同名 | `CanvasRenderer` | 2D 批渲染 |
  | `RenderingMethod`（抽象） | 同名 | —（新抽象层） | 场景渲染接口 |
  | `RendererSceneCull : RenderingMethod`(`scene`) | 同名 | `ItemStorage` 3D 部分 | instance 树 / 视锥剔除 |
  | `RendererSceneRender` | 同名 | `SpatialRenderer` | 3D 场景渲染算法 |
  | `RendererViewport`(`viewport`) | 同名 | `RenderViewport` | 视口 / 多屏 / 相机-attach / 绘制 |
  | `TextServer` | `TextServer/TextServerDefault` | `FontStorage` + `TextItemData` | 文本整形 / SDF 字形 / 字体 |

  **item 拆分粒度（对齐）**：`CanvasItemData` → `RendererCanvasCull`；`SpatialItemData` → `RendererSceneCull`
  （`RendererSceneRender` 由 `RendererCompositor` 提供、经 `RendererSceneCull::SetSceneRender()` 注入）；
  `TextItemData` + `FontStorage` → `TextServer`；层级 / 有效变换分别落在 canvas / scene 侧。
- **RID 回收**：`FreeRid(RID)` → `_free(RID)` **依次询问**各 Storage，谁认领谁返回 `true`。
- **帧**：`Draw(present, step)` 在渲染线程跑
  `rasterizer->begin_frame → scene->update → canvas->update → viewport->draw_viewports(present) → canvas_render->update → rasterizer->end_frame`；
  帧请求改用全局 `changes` 计数（`WRITE_ACTION`）替代现状 `submit_signal`。

**目标文件清单**（落地时）：

```
servers/rendering/
├─ rendering_server_globals.h               ← RSG（取代 render_globals.h）
├─ rendering_server.h / rendering_server_default.h/.cpp  ← 取代 render_server.*
├─ server_wrap_mt_common.h                  ← FUNCn 宏（可内联进 default.h）
├─ command_queue.*                         ← 保留并扩展 PushBound/PushBoundBlob（不新建 CommandQueueMT）
├─ renderer/renderer_compositor.*           ← 取代 compositor.*
├─ renderer/renderer_canvas_render.*        ← 取代 canvas_renderer.*
├─ storage/renderer_texture|material|mesh|light_storage.*
├─ canvas/renderer_canvas_cull.*            ← ItemStorage 2D 拆出
├─ scene/rendering_method.* + renderer_scene_cull.* + renderer_scene_render.*
├─ text/text_server.* + text_server_default.*
├─ viewport/renderer_viewport.*             ← 取代 render_viewport.*
└─ arhud_engine.*                           ← 降级为主循环驱动
删除：~~render_commands.h、command_executor.\*~~（✅ 已删 2026-10-07）、render_globals.h、
      render_server.*(旧)、viewport_provider.h、storage/font_storage.*
```

**目标重建顺序**：`CommandQueue::PushBound` 扩展 → RSG → `rendering_server.h` → `rendering_server_default`
→ `renderer_compositor` + 各 `Renderer*Storage` → `RendererCanvasCull` / `RenderingMethod`+`RendererSceneCull`+`RendererSceneRender`
→ `TextServer` → `RendererViewport` → `arhud_engine` 降级 + demo/test 适配 → 删旧文件。
> 该顺序为**目标依赖次序**；施工按 §7.12 的**渐进式阶段**推进（每阶段可编译/可测）。

> **命名决策**：目标**全面采用 Godot 原名**（`RenderingServer/Default`、`Renderer*Storage`、
> `RendererCanvasCull`、`RendererSceneCull`、`RendererViewport`、`TextServer`…）。

### 7.10 🎯 目标：必须的偏离（显式记录）

| # | 偏离 | 说明 |
|---|---|---|
| D1 | 多屏 | Godot 由 `DisplayServer` 多窗口承担；本工程在 `RenderingServer` 之上保留 `FrameImageQueue` + 每屏 present 线程 |
| D2 | 无 `Variant`/`StringName`/`Object` | API 用显式类型；命令参数允许集合 = `RID`、标量、enum、POD 值类型（`Transform2D`/`Color`…）、`Ref<T>` |
| D3 | 变长载荷用 `Ref<T>` | 用 `Ref<Buffer>`（像素/字体字节）、值类型 `UString`（名字）、`LocalVector<uint32>`（码点）；**禁止**把主线程裸指针（`const uint8*`/`const char*`）放进命令 |
| D4 | 文本 | 引入独立 `TextServer`/`TextServerDefault`（对齐 Godot），`RendererFontStorage` 并入其中 |
| D5 | per-screen 脏位 | Godot 无；本工程多屏扩展，落 `RendererViewport` |
| D6 | 参数打包 | ~~自研 `Tuple`~~ → **显式 POD 成员**（ADR-0010 裁决 2026-10-07）：不引入 `std::tuple`/自研 Tuple，规避 memcpy 与 `trivially_copyable` 风险 |
| D7 | 唤醒机制 | Godot 用 `WorkerThreadPool` yield；本工程自建渲染线程，用 `Semaphore`（`SetNotifySemaphore`） |
| D8 | RDG / 依赖图 | Godot 在 RD 后端有完整 RDG（`RenderGraph`：pass↔资源 DAG + barrier 编译）；本工程**当前无**，列为**后续按需**（占位设计见 §7.13） |
| D9 | GL 命令缓冲 | Godot GLES3 **无**录制层（立即提交）；本工程 GL 路径曾多一层 `GLCommandBuffer`，判为**冗余**，Route A 遗留退役后连多态 `ICommandBuffer` 层一并移除，DrawList 直调 `CommandXxx()`（§6.4） |
| D10 | OpenGL 接入 RD | Godot 的 OpenGL 驱动**不走 RD**（官方："The OpenGL driver does not use the RenderingDevice abstraction"），仅 Vulkan/D3D12/Metal(MoltenVK) 有 RD 实现；本工程**把 GL 接入 `IRenderingDeviceDriver`**，以同一 RD 契约统一后端并为 Vulkan 预留并存位；代价是 GL 多一层接口间接，RD 的录制/资源追踪语义对 GL 暂为空壳（§6.4） |
| D11 | RD 后端驱动契约（较 Godot 更薄） | Vulkan 是 Godot 的 RD 主后端（**对齐**方向）；但 Godot 驱动层 `RenderingDeviceDriver` 含 `CommandBuffer`/`CommandPool`/`CommandQueue`/`Fence`/`Semaphore` 等 ID 与命令缓冲 / 队列提交原语，本工程 RDD 契约按 GL 立即模式裁剪、已退掉这些原语（仅留 `CommandBufferID`/`CommandBufferType` 占位）。接 Vulkan 时需**回补**驱动内部 `VkCommandBuffer`、命令池与队列提交、帧同步（`Fence`/`Semaphore`）；上层 RDG / 依赖图见 D8（§6.4） |

### 7.11 🎯 目标落地规格（冻结）

> 本节把 §7.9 从"类名 / 职责"下沉到**可施工的契约**：接口清单、宏族、队列签名、
> 命令参数类型集、RID 分裂。**冻结 = 代码按此实现；如需改动先回改本文**（单一事实源）。
>
> **命令队列 / 参数打包机制已裁决**（ADR-0010 已接受，2026-10-07）：命令 =（目标, 方法, 参数）；
> **保留函数指针 thunk、不引入 vtable `CommandBase`；参数用显式 POD 成员（按元数展开），不引入 `Tuple`**。
> 原因：队列以 `memcpy` 搬运命令——vtable 对象 `memcpy` 属灰区（车规/MISRA 不友好），`std::tuple` 在 MSVC 下不保证 `trivially_copyable`。
> 故下方 **C / D** 的 `CommandQueueMT` + `Tuple` 规格**被 ADR-0010 取代**（语义终点相同：删执行器 + 平铺全局 + 命令=目标×方法）。

#### A. RenderingServer 抽象接口（全量虚 API 单例）

接口面 = 现状 `RenderServer` 全方法 + 视口 / 帧 / `FreeRid` 的一般化；item 动词沿用工程既有命名
（`SetParent/SetVisible/z_as_relative` 等，语义对齐 Godot）。按域分组：

```cpp
class RenderingServer
{
public:
    static RenderingServer *singleton;
    static RenderingServer *GetSingleton();
    static RenderingServer *Create();                 // 工厂：默认返回 RenderingServerDefault

    // ── 资源 ──
    virtual RID  TextureCreateFromData(const char *name, Ref<Buffer> data,
                                       uint32 w, uint32 h, DataFormat fmt, uint32 mips = 1) = 0; // RidSplit
    virtual void TextureFree(RID) = 0;
    virtual RID  MaterialCreate(const char *name, RID shader, MaterialDataType type) = 0;
    virtual void MaterialSetParam(RID, uint32 offset, const void *data, uint32 size) = 0; // Push 时同步拷入载荷
    virtual void MaterialSetTexture(RID, uint32 slot, RID texture) = 0;
    virtual void MaterialFree(RID) = 0;
    virtual RID  MeshCreateQuad/Box/Sphere(...) = 0;  // RidSplit
    virtual void MeshFree(RID) = 0;

    // ── 2D（RendererCanvasCull）──
    virtual RID  CanvasItemCreate(uint32 visibility_mask = 0xFFFFFFFF) = 0;  // RidSplit
    virtual void CanvasItemSetParent/Transform/Opacity/ZOrder/ZAsRelative/Visible/Material/Mesh/Layer(...) = 0;
    virtual void CanvasItemFree(RID) = 0;

    // ── 3D（RendererSceneCull；Godot instance_* 语义）──
    virtual RID  InstanceCreate(uint32 visibility_mask = 0xFFFFFFFF) = 0;    // RidSplit
    virtual void InstanceSetTransform/Material/Mesh/RenderPriority/DisableCull/Parent(...) = 0;
    virtual void InstanceFree(RID) = 0;

    // ── 文本（TextServer 持有）──
    virtual RID  FontCreateFromFreeType(const char *name, const char *path, float size,
                                        const uint32 *charcodes, uint32 count, float sdf_spread = 8.0f) = 0;
    virtual void FontFree(RID) = 0;
    virtual RID  TextItemCreate(uint32 visibility_mask = 0xFFFFFFFF) = 0;
    virtual void TextItemSetFont/Text/Position/Color/ZOrder/ZAsRelative/Visible/Parent(...) = 0;
    virtual void TextItemFree(RID) = 0;

    // ── 相机 / 灯光（per-viewport；ADR-0006/0007）──
    virtual void Camera3DSet(uint32 viewport, const CameraMatrix &) = 0;
    virtual void Camera2DSet(uint32 viewport, const Transform2D &) = 0;
    virtual void DirectionalLightSet(uint32 viewport, const DirectionalLight &) = 0;
    virtual void DirectionalLightClear(uint32 viewport) = 0;

    // ── 视口（RendererViewport）──
    virtual RID  ViewportCreate(uint32 width, uint32 height) = 0;
    virtual void ViewportDestroy(RID) = 0;
    virtual void ViewportSetSize(RID, uint32 width, uint32 height) = 0;
    virtual void ViewportSetActive(RID, bool active) = 0;

    // ── 帧 ──
    virtual void SubmitScene() = 0;                   // app 帧提交信号
    virtual void Draw(bool present, double step) = 0; // 渲染线程帧入口（≈ RenderingServerDefault::draw）

    // ── 生命周期 / 线程 ──
    virtual void Init() = 0;
    virtual void Finish() = 0;
    virtual void Sync() = 0;                          // create_thread ? queue.sync() : queue.flush_all()
    virtual bool HasChanged() const = 0;              // changes > 0
    virtual void FreeRid(RID) = 0;                    // 依次询问各 Storage，谁认领谁 true
    virtual bool IsOnRenderThread() = 0;
};
```

> **命名**：`Instance*` 取代现状 `SpatialItem*`（对齐 Godot `instance_*`，决策 A）；
> `MaterialSetParam` 取代实例层 `MaterialSetUniform`；`Ref<Buffer>` 承载像素 / 字体字节（D3）。
>
> **落地阶段（§7.12）**：
> - **S0（已落地）**：本接口由 `RenderingServer`（抽象）+ `RenderingServerDefault`（门面，内部委托旧 `RenderServer`）实现。
>   变长数据形参用**过渡签名**「裸指针 + size」（`const uint8*` / `const uint32*`，Push 时同步拷贝），
>   `Ref<Buffer>` 待 **S1** 引入 `Buffer`/`Ref` 参数体系后替换。
> - `Init/Finish/Draw/Sync/HasChanged/FreeRid` 于 **S1** 接管（S0 为 stage-gated 占位）；
>   `Viewport*` 于 **S6** 落地（S0 占位）。
> - 复杂创建签名（`TextureCreateFromData` / `FontCreateFromFreeType`）的**控膨胀改造**见 **H**（S1 落 param struct）。

#### B. FUNCn 宏族（裁剪版，无 Variant）

Godot `server_wrap_mt_common.h` 的 `FUNC0..FUNC15` 在本工程裁剪为 **0–4 参**；核心是
"本线程直调 / 异线程入队"的**唯一分发形态**（取代现状手写 `PushBound` 分发）：

```cpp
#define WRITE_ACTION   (++changes)                                   // 帧请求计数
#define ASYNC_PUSH     (Thread::GetCallerId() != server_thread)      // 线程闸门
#define FLUSH_AND_CALL (command_queue.FlushIfPending(), (instance->*method)(args...))
```

展开示例（`TextureCreateFromData`，含 `FUNCRIDSPLIT`）：

```cpp
RID TextureCreateFromData(const char *p_name, Ref<Buffer> p_data, uint32 p_w, uint32 p_h,
                          DataFormat p_fmt, uint32 p_mips) override
{
    RID ret = RSG::texture_storage->TextureAllocate();               // ① 任意线程即时分配
    if (ASYNC_PUSH)                                                  // ② 异线程 → 入队
        command_queue.Push(RSG::texture_storage, &RendererTextureStorage::TextureInitializeFromData,
                           ret, p_name, p_data, p_w, p_h, p_fmt, p_mips);
    else { command_queue.FlushIfPending();
           RSG::texture_storage->TextureInitializeFromData(ret, p_name, p_data, p_w, p_h, p_fmt, p_mips); }
    WRITE_ACTION;
    return ret;
}
```

#### C. 命令队列规格（**保留 `CommandQueue`** 并扩展绑定入队）

> ADR-0010 裁决：**不新建 `CommandQueueMT`**；沿用现有 `CommandQueue`（SPSC 1MB 环形缓冲）。语义对齐 Godot——命令直接点名"调哪个存储的哪个方法"。

```cpp
// 现有类 command_queue.h：保留环形缓冲 + 函数指针 thunk（ADR-0009 自派发）
// M1 已落地（0–3 元）：
template <typename S, typename M>                           void PushBound(S *p_instance, M p_method);
template <typename S, typename M, typename A0>              void PushBound(S *p_instance, M p_method, A0 p_a0);
template <typename S, typename M, typename A0, typename A1> void PushBound(S *, M, A0, A1);
template <typename S, typename M, typename A0, typename A1, typename A2> void PushBound(S *, M, A0, A1, A2);
// 变长内联 blob（M2 落地，沿用现有 CommandDataSpan）：
template <typename S, typename M, typename... Args>
void PushBoundBlob(M p_method, const void *p_blob, uint32 p_blob_size, Args... p_args);
```

- **目标存储来源**：**调用方显式传** `&Storage`（贴 Godot `push(RSG::material_storage, …)`），非队列隐式取；可测。
- **元数**：当前 0–3 元（M1）；复杂创建按 **H1** 改 `*CreateParams` param struct 收口，使元数保持 ≤3–4；超出则按需扩成员。

**搬运语义**：命令对象（含 `instance` / `method` / 显式 POD 参数）**全部 `trivially_copyable`**（`static_assert` 守门）；
入队内联构造一次 → flush 时 raw `memcpy` 到本线程栈缓冲 → thunk 调 `(instance->*method)(args…)`。**不引入 vtable**。

#### D. 参数打包：显式 POD 成员（取代 D6 的 `Tuple`）

> ADR-0010 裁决：**不引入 `Tuple` / `core/template/tuple.h`。**

- 命令参数以**显式 POD 成员**按元数展开（M1 落地 0–3 元）——保证 `trivially_copyable`，规避 MSVC 下 `std::tuple` 的不确定性。
- 复杂创建签名按 **H1** 改 `*CreateParams` param struct 收口，使命令元数保持在 ≤3–4。
- 变长载荷走**内联 blob 通道**（`PushBoundBlob`），不经非平凡类型按值传递。

#### E. 命令参数允许类型集合（D2 / D3 展开）

| 类别 | 允许 | 说明 |
|---|---|---|
| 句柄 | `RID` | 句柄，跨线程安全 |
| 标量 / 枚举 | 整 / 浮 / 布尔 / `enum class` | 定长 |
| POD 值类型 | `Transform2D`、`Color`、`Vec3`、`CameraMatrix`、`DirectionalLight` | 不可含指针 / 不可非平凡析构 |
| 引用计数对象 | `Ref<T>` | 引用计数保命，跨线程存活 |
| 变长载荷 | `Ref<Buffer>`（像素 / 字体字节）、`UString`（名字，值语义）、`LocalVector<uint32>`（码点） | 经 `Tuple` 值传递 |

**禁止**：把主线程裸指针（`const uint8*` / `const char*` / `const uint32*`）存进命令跨线程传递。
若必须传原始字节（如 `MaterialSetParam`），**在入队时同步 `memcpy` 进命令载荷**（`PushBoundBlob`），命令体内不存指针。
`PushBound` / `PushBoundBlob` 以 `static_assert(trivially_copyable)` 守门。

#### F. RID 分裂分配与 FreeRid

- **分裂**：`XxxAllocate()`（任意线程，立即返回空壳 RID）+ `XxxInitialize(...)`（服务线程直调或入队），
  由 `FUNCRIDSPLIT(x)` 宏封装（对齐 Godot）。
- **回收**：`RenderingServer::FreeRid(RID)` → 渲染线程 `_Free(RID)` **依次询问**
  `canvas / viewport / scene / texture_storage / material_storage / mesh_storage / …`，谁认领谁返回 `true`。
- **帧请求**：全局 `changes` 计数（`WRITE_ACTION`）替代现状 `submit_signal`；`HasChanged()` = `changes > 0`。

#### G. 多屏衔接（不丢现状能力）

- `RenderingServer::Draw(present, step)` 产出各屏帧 → 交现有 `FrameImageQueue` / `PresentThread`（保留 D1）。
- per-screen 脏位（D5）落 `RendererViewport`；强制重绘（ADR-0008）改经 `RenderingServer` 语义表达。

#### H. 接口控膨胀规范（对齐 Godot `rendering_server.h` 实测）

> 依据：Godot 4.4 `servers/rendering_server.h`（`class RenderingServer : public Object`，按域分节
> `/* TEXTURE API */`、`/* SHADER API */`、`/* COMMON MATERIAL API */`、`/* MESH API */`…，全部 `virtual … = 0`）。
> 结论：**接口"声明 + 路由"必然变大，但实现不膨胀**。膨胀必须被关在下列四条规范之内。

**H1. 复杂创建用 param struct，不堆参数表。** 加字段只改结构体、不改函数签名（无 API churn）。

```cpp
// 反例（现行 S0 过渡签名：参数会越来越长）
virtual RID TextureCreateFromData(const char *name, Ref<Buffer> data,
                                  uint32 w, uint32 h, DataFormat fmt, uint32 mips) = 0;
// 正例（参数打包进结构体 / 资源对象）
struct TextureCreateParams {
    uint32 width = 0, height = 0;
    DataFormat format = kFormatR8G8B8A8Unorm;
    uint32 mip_levels = 1;
    Ref<Buffer> data;              // 像素字节（D3）
    const char *name = nullptr;
};
virtual RID TextureCreateFrom(const TextureCreateParams &p_params) = 0;
```

> Godot 实据：`mesh_create_from_surfaces(const Vector<SurfaceData> &p_surfaces, int p_blend_shape_count = 0)`
> 中 `struct SurfaceData { PrimitiveType primitive; uint64_t format; Vector<uint8_t> vertex_data; … }` 就定义在接口内；
> `texture_2d_create(const Ref<Image> &p_image)` 把宽 / 高 / 格式 / 像素数据全塞进 `Image`。
> **本工程落地**：§7.11 A 的 `TextureCreateFromData` / `FontCreateFromFreeType` 裸参数为 S0 过渡，
> 按 H1 于 **S1** 与 `Ref<Buffer>` 一并改 param struct。

**H2. RID 统一生命周期。** 所有资源同构：`Create→RID`、`Set*/Get*`、统一 `FreeRid(RID)`（不按类型分 free）。
新增一种资源 = 一套**可预测**的方法，不打乱结构。

> Godot 实据：TEXTURE 节 `texture_2d_create / texture_2d_update / texture_2d_get / texture_replace /
> texture_set_size_override / texture_get_format / texture_debug_usage` + 统一 `free_rid`。

**H3. 领域词汇内聚在接口。** 枚举 / 结构体作为 `RenderingServer` 的**嵌套类型**声明，领域词汇不出接口。
本工程对应：`DataFormat`、`MaterialDataType`、`CameraMatrix`、`DirectionalLight` 已按此置于 `servers/rendering`。

**H4. 逻辑归 Storage，接口只做路由。** `RenderingServerDefault` 每个方法体只做
「RidSplit + 线程闸门 + 直调/入队 + `WRITE_ACTION`」（见 B / C / F）；**严禁**把 Storage 逻辑写进接口。
逻辑一律落 `Renderer*Storage`（经 `RSG::*` 访问）。

> Godot 实据：宏 `FUNCRIDSPLIT` / `WRITE_ACTION`（形如 `WRITE_ACTION (++changes)`）+
> `command_queue.push(this, &类::方法, 参数…)`；实现在 `RendererTextureStorage` / `RendererMeshStorage` …

**反例 → 治理对照**

| 膨胀方向 | 治理规范 |
|---|---|
| 方法数 ↑ | H4：接口只路由（每方法体 1~3 行） |
| 参数列表 ↑ | H1：param struct / 资源对象吸收 |
| 命令载荷结构体 ↑ | B / C：绑定调用 `push(obj,&Class::method,args…)`，不每方法一个 POD |
| 领域逻辑渗入接口 | H4：一律落 Storage |
| 领域词汇散落 | H3：嵌套类型内聚 |

> 一句话：危险的从不是"方法多"，而是"① 签名无限长 ② 逻辑写进接口"——H1、H4 分别治这两点。

### 7.12 🎯 渐进式（绞杀者）迁移计划

原则：**每阶段都保持可编译、测试可跑、demo 可出图**；新机制旁路接入，旧实现按子系统逐个下线。
每阶段收尾更新本文 ✅ / 🎯 标记与阶段状态。

| 阶段 | 动作 | 新增 | 下线 | 验收 |
|---|---|---|---|---|
| **S0 门面** ✅ | 立 `RenderingServer`（抽象）+ `RenderingServerDefault`（门面，内部**委托旧** `RenderServer`）；`ARHudEngine` 值持门面并装配 `RenderingServer::singleton`；app/demo 经 `GetRenderServer()` / `GetSingleton()`；3D item 名对齐 `Instance*`、`MaterialSetParam`、`FontCreateFromFreeType` | `rendering_server.h/.cpp`、`rendering_server_default.h/.cpp` | — | 5 demo 出图不变；测试全绿 ✅（2026-11-08 编译+34 用例+五路出图验证） |
| **S1 队列** ⏳（M1~M3 ✅、S1-GL ✅ 2026-10-11、**H1 param struct ✅ 2026-11-08 实测确认**；余 `Ref<Buffer>` 参数体系） | 扩展 `CommandQueue`（`PushBound`/`PushBoundBlob`，M1 已落地）并**全量改写调用点**（M2）；删 `CommandExecutor`/33 命令结构体/`RenderCommandType`/`Execute`（M3）；`RenderingServerDefault` 改用绑定命令；**资源创建按 H1 改 `*CreateParams` param struct**（已落地：`TextureCreateParams`/`MeshCreateQuad/Box/SphereParams` 等随 `PushBoundBlob` 走线）；`Ref<Buffer>` 参数体系**未落地**（纹理/字体像素仍为裸指针+size 过渡签名，跨线程由 blob 同步拷贝保证安全）；**子步 S1-GL：GL 命令缓冲退化为直通（§6.4）；2026-10-11 整层退役（连多态 `ICommandBuffer` 与 `command_buffer.*` 文件一并删除）** | `command_queue.h` 扩展（`PushBound*`） | `command_executor.*`、`render_commands.h`；`command_buffer_gl` 录制机制（`CommandNode`/union/`kReplayTable`）；`command_buffer.h` / `command_buffer_gl.{h,cpp}`（2026-10-11） | 搬运 / 同步单测；`test_command_executor` 改「装配 `RenderGlobals` → `PushBound` → `Flush` 断言」；demo 出图 ✅（2026-11-08 五路验证通过） |
| **S2 全局** | `RenderingServerGlobals`（RSG）替换 `RenderGlobals`；命令实现体经 RSG 直达存储 | `rendering_server_globals.h` | `render_globals.h` | grep 无 `RenderGlobals` |
| **S3 资源 Storage** | `Renderer*Storage` 对齐重命名（Texture/Material/Mesh/Light）；`light` 从 `ViewportSceneState` 迁入 `RendererLightStorage` | `storage/renderer_*_storage.*` | 旧 storage 名 | 单测 + demo |
| **S4 item 拆分** | `ItemStorage` 拆为 `RendererCanvasCull`（2D）/ `RenderingMethod`+`RendererSceneCull`+`RendererSceneRender`（3D）；层级 / 脏位随迁 | `canvas/`、`scene/` | `item_storage.*` | item_storage 单测迁移 + demo（含 ADR-0005 树形） |
| **S5 文本** | 独立 `TextServer`/`TextServerDefault`；`FontStorage` + `TextItemData` 并入 | `text/` | `storage/font_storage.*` | text demo 出图 |
| **S6 视口** | `RendererViewport` 改名对齐；`viewport_provider` 并入 `RSG::viewport` | `viewport/renderer_viewport.*` | `render_viewport.*`、`viewport_provider.h` | 多屏 demo |
| **S7 收口** | `arhud_engine` 降级为主循环驱动；demo/test 适配；删净旧文件 | — | 全部废弃件 | 全量构建 + `ctest` 绿 |

**退场台账：`RenderServer` 职责去向**（绞杀进度的量尺——每阶段末尾 `RenderServer` 应只剩"未搬走"的行）

| `RenderServer` 现有职责 | 目标归属 | 阶段 |
|---|---|---|
| `Initialize/Finalize` 依赖装配 | `RenderingServerDefault::Init/Finish`（+ RSG） | S1 |
| 命令序列化（`Push`/`PushAndSync`/`CanBypassQueue`）、`render_commands.h`、`CommandExecutor` | `CommandQueue`（`PushBound`）+ `FUNCRIDSPLIT`/`WRITE_ACTION` | S1 ✅ |
| 资源 create/set/free（texture/material/mesh/font） | `RSG::*_storage`（内部名 `MaterialSetUniform→MaterialSetParam`） | S3 |
| 2D item（`CanvasItem*`） | `RendererCanvasCull` + canvas storage | S4 |
| 3D item（`SpatialItem*`） | `RendererSceneCull` + instance storage（内部名转 `Instance*`） | S4 |
| Text item（`TextItem*`） | `TextServer` | S5 |
| `Camera*` / `DirectionalLight*` | `RendererViewport` / `ViewportSceneState` | S6 |
| `ResolveViewport`（ADR-0007） | `RSG::viewport` 视口解析 | S6 |
| `SubmitScene` | `RenderingServerDefault` 帧驱动（`Draw`） | S1 / S6 |

**绞杀机制**：过渡期门面 `RenderingServerDefault::X` 由"转发 `render_server_.X`"逐方法替换为"直接用新基础设施"；
每搬走一个域，`RenderServer` 就少若干方法（S0 最厚 → S6 空壳 → S7 删文件）。
**删除判定**：`grep -r "render_server_\."` 无命中即可删；同步清理 `render_commands.h`、`command_executor.*`（S1 即失效）、
`EngineStackRefs` 旧栈字段、`@see render_server.h` 等引用。

> **冻结门槛**：S1 之前冻结的接口（`RenderingServer` 虚表、`CommandQueue::PushBound` 签名）为**施工基线**；
> 后续阶段如需变更，先回改 §7.11 再动代码。

### 7.13 🎯 目标：RDG / RenderGraph（依赖图，占位设计，后续按需）

**定位**：`servers/rendering/render_graph/`（对齐 Godot `servers/rendering/render_graph/`），
夹在 **`RendererSceneRender`（高层渲染）↔ `RenderingDevice`（底层命令直录）** 之间。
**不**属 RSG（RSG 只提供子系统指针）、**不**属 `CommandQueue`（只做跨线程传输）。

**现状**：本工程**无** DAG。后端为 GL（Mode R/S）立即模式、HUD 为单 pass 合成，无跨 pass 资源别名 / barrier 调度需求；
Godot 也**仅在 RD 后端（Vulkan/D3D12/Metal）启用 RDG**，GLES3 / Compatibility 后端同样没有。
故列为**后续按需**，已在 §7.10 D8、`rendering_device.h`（`RDG ❌ 后续按需`）记录。

**后端分叉**：GL＝**立即提交**（无录制层 / 无 RDG，见 §6.4）；RD＝**录制 + RDG**（本节）。

**占位实现设计**（契约冻结；占位与完整实现**共用同一 API**，将来只替换实现）：

```cpp
// render_graph.h —— 契约（占位期即可编译，无实际调度）
using RGTextureHandle = uint32;   // 0 = invalid
using RGBufferHandle  = uint32;

class RenderGraphPass {
public:
    virtual const char *GetName() const = 0;
    virtual void Run(RenderingDevice *p_rd) = 0;      // 占位：直接调命令
};

class RenderGraph {
public:
    RGTextureHandle AddTexture(const RGTextureDesc &p_desc);   // 仅登记描述，不建真实 GPU 资源
    RGBufferHandle  AddBuffer(const RGBufferDesc &p_desc);
    template <typename PassT> PassT *AddPass(const char *p_name);
    void AddPassDependency(RenderGraphPass *p_from, RenderGraphPass *p_to); // 记录边，不排序
    void Compile();                                   // 占位：空操作 + debug 无环校验
    void Execute(RenderingDevice *p_rd);               // 占位：按登记顺序 Run
    void Reset();
    // 预留钩子（声明不实现，将来填充）
    void SetInvalidate(RenderGraphPass *p, const char *p_what);
    void SetAlias(RGTextureHandle p_a, RGTextureHandle p_b);
    void AddSubpassDependency(const SubpassDependency &p_dep);
};
```

**占位语义**（与完整实现的差别，必须显式；避免误用）：

| 环节 | 占位期 | 完整实现（将来） |
|---|---|---|
| 资源句柄 | 自增**索引**，不创建真实 GPU 资源 | 映射 RD 资源，支持别名 / 复用 |
| `AddPassDependency` | 记录边 | 参与拓扑排序 |
| `Compile()` | 空操作 + debug 无环校验 | 拓扑排序 + 生命周期 + barrier 计算 |
| `Execute()` | 按**登记顺序** `Run` | 按排序结果派发 |
| 消费者 | 无（纯契约头） | `RendererSceneRender` 构图 |

**填充路径**：只替换 `Compile()` / `Execute()` 的**实现**，调用方不改；
挂钩已预留语义：`ResourceUsageBits`（`rendering_device_commons.h`）、`SubpassDependency`（renderpass 类型）、`RenderingDevice`。

---

## 8. app 层

app 只链 `arhud_servers + arhud_core`（+ `glad`），**不暴露 platform/drivers 类型**。

### 8.1 统一入口 `ARHudEngine`

- `Config{ vsync=true, render_thread_priority=kPriorityHigh }`（仅引擎级）。
- `ScreenConfig{ title, width=1280, height=720 }`（窗口参数移出 Config，AddScreen 传入）。
- 生命周期：`Initialize(Config)` →（首次 `AddScreen` 延迟建 RCD + 共享栈）→ `BeginFrame/EndFrame` 或 `Run(IAppLoop&)` → `Finalize`。
  `~ARHudEngine` 若仍初始化会自动 `Finalize`。`ARHUD_DISABLE_COPY_MOVE`。

### 8.2 两种帧循环

- **方式 A（手动，demo 采用）**：`while(!ShouldClose()){ BeginFrame(); /* 经 RenderServer 发命令 */ rs->SubmitScene(); EndFrame(); }`
- **方式 B（`IAppLoop`）**：实现 `OnUpdate(dt)/OnRender()`，`engine.Run(app)`，引擎驱动循环。

### 8.3 演示程序

| 目标 | 验证点 | 依赖 |
|---|---|---|
| `arhud_demo_phase2` | RCD→RDD→RD 端到端（直连 drivers+platform，治理豁免，不经 ARHudEngine） | 越层 |
| `arhud_demo_phase4_canvas` | 2D Canvas（含 ADR-0005 树形层级/父链传播） | 标准 |
| `arhud_demo_phase4_spatial` | 3D Spatial（相机 + 平行光） | 标准 |
| `arhud_demo_phase4_compositor` | 2D/3D 混合 | 标准 |
| `arhud_demo_phase4_text` | SDF 文本（FreeType） | + `Freetype::Freetype` |

共同点：开头 `InitializeLogger()`、结尾 `ShutdownLogger()`；除 phase2 均经 `ARHudEngine`，循环内 `SubmitScene`；
Debug 收尾 `memory::DumpLeaks()`。

---

## 9. 横切主题

### 9.1 RID 与句柄

`core/template/rid.h` 提供 `RID`（64 位不透明）+ `RIDOwner`（代际索引）+ `DEFINE_ID(T)`。
servers 的每个 Storage 各自持 `RIDOwner`，`MakeRid` 回填→资源结构；DNS 级 `RD*_ID` 由 `ARHUD_DEFINE_RDD_ID` 定义。
item RID 无代际、不复用（§7.5）；GPU 资源 RID 走代际防悬垂。

### 9.2 线程模型（汇总）

| 线程 | 数量 | 拥有 | 职责 |
|---|---|---|---|
| 主线程 | 1 | 无 GL 资源所有权（初始化期例外） | 输入、发命令、`SubmitScene` |
| 渲染线程 | 1 | GL 上下文 + 全部 Storage + Compositor | Flush 命令、逐屏渲染、回读 |
| 上屏线程 | 每屏 1 | 零 GL | `Present`/`SwapPresent`，独立等 vsync |

跨线程通道：`CommandQueue`（主→渲染）、`FrameImageQueue`（渲染→上屏），均由 `Semaphore` 唤醒。

### 9.3 帧与上屏路径

现状保留 **Mode R（kReadback）** 与 **Mode S（kSwap）**（§5.5 / §7.7）。
生产环境（QNX/Linux/Android）规划走 EGLImage/DMA-BUF + release-event 模型（对应 `IFramePresenter::SwapPresent`）。

### 9.4 依赖与契约规则（复核清单）

- drivers 不链 servers；契约在 `drivers/` 根，GL 在 `drivers/gl/`。
- platform 契约在 `platform/` 根，Win32 在 `platform/windows/`。
- `storage/` 头禁 `#include` `renderer/`（`renderer → storage` 单向）。
- app 只链 `servers`+`core`（phase2 越层豁免）。
- `scene`：`scene → servers`（场景树位于 servers 之上，已落地）；`arhud_servers` 不再链 `scene`。
- 全局 include（`cmake/dir.cmake`）仅给 `core/*` 与 `platform/*`；新模块用 `target_include_directories()`。

---

## 10. 命名与编码规范

| 元素 | 风格 | 示例 |
|---|---|---|
| 类型/类/枚举 | PascalCase | `SafeNumeric`、`LogLevel`、`Error` |
| 函数/方法 | PascalCase | `CreateTexture()`、`BufferMap()` |
| 局部/参数变量 | snake_case | `texture_id`、`buffer_size` |
| 类成员变量 | snake_case + `_` | `texture_id_`、`buffer_size_` |
| 结构体成员 | snake_case（无后缀） | `object`、`generation`、`width` |
| 常量/枚举值 | k + PascalCase | `kMaxTextures`、`kOutOfMemory` |
| 宏 | UPPER_SNAKE_CASE | `ARHUD_PLATFORM_WINDOWS` |
| 命名空间 | snake_case | `arhud::render::opengl` |
| 文件/目录 | snake_case | `texture_storage.h` |

- `enum class` 唯一；`using` 优于 `typedef`；`override`/`final` 必写。
- include 顺序：关联头 → C stdlib → C++ stdlib → 第三方 → 工程头。
- **禁止**：`iostream`、`malloc/free`、`printf/sprintf`、裸 `new/delete`、`goto`。
- 异常/RTTI 编译开关启用（ADR-0004），但引擎代码**禁 `throw/try/catch` 与 `dynamic_cast/typeid`**
  （按 Google 原教条款），错误返回 `Error`；开关与原教的张力登记于风格 skill §8.2。
- `auto` 仅在提升可读性/安全性时使用，不得遮蔽类型可读性。

> **裁决更新（2026-11-07）**：本节上方表格保持有效（命名层与 Google 原教一致）；
> 格式与惯例细则改按 **Google C++ Style 原教**执行——80 列、2 空格缩进、namespace 体不缩进、
> 大括号一律附尾、`Type* p` 符号贴类型、include guard `#ifndef ARHUD_<相对路径>_H_`（禁 `#pragma once`）、
> 形参 `snake_case`（弃 `p_` 前缀）；**注释体系为工程修订：Doxygen 格式 + 中文**（2026-11-08 裁决，
> 文件头/类/函数按标签规范，代码本体仍纯 ASCII）。
> 完整裁决表、存量代码迁移纪律与 clang-format 落地见 **`docs/skill/arhud-coding-style/SKILL.md`**；
> §7.9/§7.11 历史样例中的 `p_`/Allman 等为起草遗留，不构成风格豁免。
> **迁移完成度（2026-11-08）**：G1~G6 格式层已全库落地（110 tracked 源文件，clang-format 审计零违例，
> 构建+34 测试全绿）；W1（`p_` 前缀清除）/W2（注释转英文）按该 skill §1.3 随改动收敛，不批量强改。

---

## 11. 术语

| 术语 | 含义 |
|---|---|
| RCD / RDD / RD | RenderingContextDriver / RenderingDeviceDriver / RenderingDevice（三阶 GPU 抽象） |
| RSG | RenderingServerGlobals（Godot 全局可达点；本工程现状为 `RenderGlobals`） |
| Route B | 主线程发命令 / 渲染线程执行的线程模型 |
| Mode R / Mode S | 回读上屏 / Swap 上屏（`kReadback` / `kSwap`） |
| Storage | 资源 Storage（GPU-backed，归 Compositor）与状态 Storage（CPU-only，如 ItemStorage） |
| 有效状态 | item 沿父链合成后的 transform/opacity/visibility/z（ADR-0005） |
| 强制重绘 | 外部 WM_PAINT/WM_SIZE 触发的全屏重渲（ADR-0008） |