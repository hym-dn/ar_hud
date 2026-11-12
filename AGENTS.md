# AGENTS.md — ARHud 渲染引擎（AI 上手指南）

> 单一事实源：`docs/skill/arhud-design/SKILL.md`（现状✅ + 目标🎯 + 冻结规格，唯一代码设计文档）；
> 进度唯一账本：`docs/skill/arhud-progress/SKILL.md`。本文只做浓缩与导航，不重复细节；
> 两者冲突以设计 skill 为准。
> **改代码涉及架构 / 接口冻结面时，必须先回改该文档再动代码**（§7.11 冻结门槛）。
> 旧文档体系（ADR/devlog/process/设计 skill 旧版）已删除；历史完整保留在 git（≤`c5ac51e` 前任意提交）。

## 0. 一句话定位

面向车载 AR-HUD 的 C++17 渲染引擎，架构对标 **Godot 4.6 `servers/rendering`**；
Windows + OpenGL（MSVC，静态库），Conan 2.x 管依赖。主线程发命令 / 渲染线程执行 /
每屏上屏线程"不发渲染命令"地呈现（Route B；呈现源可以是 CPU 像素，也可以是
显示侧分配的共享显存——后者才是生产形态）。

## 1. 工程现状

- **进度不在本文维护**——当前位置、计划与完成历史见 `docs/skill/arhud-progress/SKILL.md`（唯一动态账本）。
  一句话现状（2026-11-08）：S0 + S1 大部 ✅、全库风格迁移 ✅、五路 demo 端到端出图 ✅、
  **Mode X 共享显存零回读上屏 ✅**，处于 S1 收尾（`Ref<Buffer>` 参数体系）→ S2（RSG）交界。
- **Godot 对齐迁移（绞杀者计划 S0→S7，见设计 skill §7.12）进行中**；
  旧实现 `render_server.*` 与门面**并存**是绞杀者过渡态，勿提前删除；
  删除判定：`grep -r "render_server_\."` 无命中。
- 工作区常态应为 clean（迁移/风格/修复批次均已提交推送），动手前 `git status` 确认即可。

## 2. 分层与依赖规则

```
core → platform → drivers → servers → scene → app        （单向，静态库逐层叠加）
```

| 目录 | 库 target | 内容 | 关键文件 |
|---|---|---|---|
| `core/` | arhud_core | 类型/容器/同步/日志/UTF-32 String/数学，零内部依赖（+glm） | `typedefs.h`（`Error` 枚举、`ARHUD_NEW` 等）、`template/rid.h`、`os/sync.h`、`math/camera_matrix.h` |
| `platform/` | arhud_platform | 契约在根，Win32 实现在 `windows/` | `display_server.h`、`window.h`、`gl_context_manager.h`、`frame_presenter.h`（Mode R 回读 / Mode S Swap）、`shared_frame_pool.h`（Mode X 显示侧帧池）、`gpu_interop.h`（跨 API 显存别名） |
| `drivers/` | arhud_drivers | 三阶 GPU 抽象**契约**在根（RCD/RDD），GL 实现在 `gl/` | `rendering_context_driver.h`、`rendering_device_driver.h`、`rendering_device_commons.h` |
| `servers/` | arhud_servers | 引擎主体：RD / 命令 / Storage / Renderer / 视口 / 帧编排 / 引擎入口 | 见 §3 |
| `scene/` | arhud_scene | 空壳（Phase 6 场景树），位于 servers 之上 | `scene_placeholder.cpp` |
| `app/` | arhud_demo_* | 5 个 demo：phase2（越层豁免）+ phase4 canvas/spatial/compositor/text | 各 `<dir>/main.cpp` |
| `tests/` | arhud_tests | GoogleTest 3 个：command_queue / item_storage / camera_routing | `tests/CMakeLists.txt` |

**三条硬性规则**（设计 skill §1.4，评审必查）：
1. 契约归位：接口头住模块根，平台/后端实现在子目录（`drivers/*.h` ↔ `drivers/gl/`；`platform/*.h` ↔ `platform/windows/`）。
2. `renderer → storage` 单向：`servers/rendering/storage/` 下的头**禁止** include `renderer/`。
3. drivers 不倒挂 servers；servers PUBLIC 链 drivers、PRIVATE 链 platform（平台类型不外泄给 app）。

## 3. servers 层对象地图（改动最常落在这里）

```
servers/rendering/
├─ arhud_engine.*            顶层入口（PIMPL；装配共享栈；实现 IViewportProvider）
├─ rendering_server*.h/.cpp  🎯新门面（S0，委托 ↓ 旧 RenderServer）
├─ render_server.*           ✅旧 API 代理（主线程唯一资源入口，RID 分裂 + PushBound）
├─ command_queue.*           SPSC 1MB 环形队列；PushBound/PushBoundBlob；Sync() 整批屏障
├─ render_globals.h          RSG 前身：平铺全局（5 Storage+2 Renderer+ItemStorage+submit_signal），唯一写入者=Engine
├─ rendering_device.*(+_frame/_draw_list/_staging)  RD：资源生命周期/Staging/帧时延销毁（渲染线程专用 API）
├─ render_viewport.*         每屏帧编排（FBO+后处理+ViewportSceneState 相机/灯光，ADR-0006/0007）
├─ viewport_provider.h       GetViewport(index) 路由（S6 并入 RSG::viewport）
├─ frame_image_queue.h       每屏双槽状态机 FREE→WRITING→READY→COMMITTED→ON_SCREEN（背压≤1 在飞；槽可载 CPU 像素或帧 token）
├─ present_thread.h          每屏上屏线程（不发渲染命令；Mode X/R/S 逐帧按 token 选出路）
├─ render_buffers.h / draw_list_scope.h  FBO 打包 / RAII DrawList
├─ storage/  shader|texture|material|mesh|font_storage.*   GPU-backed 资源 Storage（归 Compositor）
│            item_storage.*    CPU-only item 账本+层级+脏位（ADR-0005，归 Engine）
│            viewport_scene_state.h  per-viewport 相机/灯光
└─ renderer/ compositor.*    子系统生命周期装配（≈RendererCompositor）
             canvas_renderer.* / spatial_renderer.*  2D 批渲染 / 3D 深度光照
             shaders/*.h      内联 GLSL 源码
```

## 4. 线程模型速记

| 线程 | 拥有 | 职责 |
|---|---|---|
| 主线程 ×1 | 无 GL（初始化期例外） | `BeginFrame`→经 RenderServer 发命令→`SubmitScene`→`EndFrame` |
| 渲染线程 ×1 | GL 上下文 + 全部 Storage | `Flush` 命令→脏屏渲 FBO→呈现（Mode X 直写共享显存 / Mode R `glReadPixels`）→MarkReady |
| 上屏线程 每屏×1 | 不发渲染命令（Mode X 下只调 DXGI） | `AcquireReady`→`CommitFrame`(X)/`Present`(R)/`SwapPresent`(S)→`Release` |

跨线程通道：`CommandQueue`（主→渲染）、`FrameImageQueue`（渲染→上屏），`Semaphore` 唤醒。
命令形态一律 `PushBound(目标, 方法, POD 参数)`；创建类走 **RID 分裂**（`XxxAllocate` 同步拿 RID，初始化异步入队）；
参数必须 `trivially_copyable`（static_assert 守门），变长数据经 `PushBoundBlob` 内联 blob，**禁止裸指针跨线程**。
线程闸门：执行者线程/无消费者时先 `FlushIfPending()` 再直调；shutdown 后只直调不排空。

## 5. 构建与验证

```bat
build.bat                :: Debug（需 VS2022 环境：dev prompt 或 vcvarsall x64）
build.bat release        :: Release
build.bat conan          :: conan install + Debug（推荐）
build.bat clean

cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --config Debug   :: 直接 CMake 亦可
ctest --test-dir build -C Debug            :: 3 个测试须全绿
```

产物：`build/bin/<cfg>/arhud_demo_*.exe`、`build/lib/<cfg>/arhud_*.lib`。
**每次改完的最低验证**：编译通过 + `ctest` 绿 + 相关 demo 能出图（demo 是冒烟测试）。

呈现路径的自证开关（退出时打日志，不靠截图猜）：

```bat
set ARHUD_FORCE_READBACK=1   :: 强制 Mode R（A/B 基线）
set ARHUD_DEBUG_SAMPLE=1     :: 每 300 帧采样一次"即将上屏帧"的平均亮度
```

看 demo 结尾两行即可判定：`PresentThread finalized (shared=… readback=… degraded=… failed=…)`
与 `RenderViewport finalized (present path: shared=… readback=… MB… ms GPU->CPU)`。
`shared>0 且 readback=0/0 MB` = 零回读成立；`degraded>0` = 发生过运行期降级，须查因。

## 6. 编码规范（摘要；全量裁决与存量迁移见 `docs/skill/arhud-coding-style/SKILL.md`）

- **风格基线 = Google C++ Style 原教**（2026-11-07 裁决）：80 列、2 空格缩进、namespace 体**不缩进**、
  大括号**一律附尾**、`Type* p` 符号贴类型、include guard `#ifndef ARHUD_<相对路径>_H_`（**禁 `#pragma once`**）。
- 命名：类型/方法 PascalCase；变量/形参 snake_case（**弃 `p_` 前缀**，存量随改动迁移）；类成员带尾 `_`
  （struct 成员不带）；常量/枚举值 `kXxx`；宏 UPPER_SNAKE 带 `ARHUD_` 前缀；文件 snake_case。
- 注释：**Doxygen 格式 + 中文**（2026-11-08 裁决）——文件头 `@file/@brief/@author/@version/@date/@copyright/@ingroup`；
  公开 API 必配 `@brief/@param/@return`；成员与枚举值行尾 `///<`；代码本体仍纯 ASCII、文件 UTF-8；
  `TODO(责任标识):` 中文说明。细则见风格 skill §7。
- `enum class` only；`using` 优于 `typedef`；`override`/`final` 必写；include 顺序：关联头→C→C++→第三方→工程。
- **禁止**：`iostream`、`printf/sprintf`、裸 `new/delete`、`malloc/free`、`goto`、`file(GLOB)`；
  引擎代码禁 `throw/try/catch` 与 `dynamic_cast`（编译开关启用属 ADR-0004 例外，错误一律返回 `Error`）。
- 单参构造 `explicit`；虚函数禁默认实参；全局对象须平凡可析构；跨线程 lambda 必须显式捕获。
- 格式落地用仓库根 `.clang-format`；纯格式化独立提交（`Style` 前缀），细则见 SKILL §1.3。
- 新增源文件：手工追加到模块 `CMakeLists.txt` 显式列表。

## 7. 已知坑（改前先读）

1. **option 缓存陷阱**：`app/CMakeLists.txt` 重复声明 demo `option(... OFF)`，但 `cmake/option.cmake` 先加载且先声明者胜——实际默认全 ON。
2. **全局 include**：`cmake/dir.cmake` 只全局加 `core/*`；新模块必须用 `target_include_directories()`。
3. `shader_cache/` 是 demo 运行时产物，可整体删除，勿手工编辑、勿提交新增。
4. RD 是渲染线程专用 API：断言 `IsRenderThread() || command_queue_ == nullptr`；跨线程一律走 RenderServer RID 分裂。
5. 多屏共享栈：`RenderViewport` 经 `EngineStackRefs` 引用共享对象，自身不拥有——勿在视口里 new 共享资源。
6. MSVC `/utf-8` 已开；**注释规范 = Doxygen 中文**（风格 skill §7，2026-11-08 裁决），文件必须 UTF-8。
7. **`Present1` 第三参不可为 `nullptr`**：SDK 头里它是 `_In_`（非 `_In_opt_`），传空指针会在
   `dxgi.dll` 内解引用 `NULL+0x10` 直接 AV（与 swap effect／线程／窗口类样式无关）。
   必须传零值 `DXGI_PRESENT_PARAMETERS`。文档说法不可信，已用最小复现程序对照验证。
8. **interop 纹理必须先加锁再挂 FBO**：`wglDXRegisterObjectNV` 之后未 `Lock` 就
   `glFramebufferTexture2D`，`glCheckFramebufferStatus` 报 `GL_FRAMEBUFFER_UNSUPPORTED`。
   见 `RenderViewport::InitializeSharedPath`。
9. **共享显存的拆卸顺序是硬约束**：解除 interop 别名（screens Finalize）→
   `wglDXCloseDeviceNV`（RD Shutdown）→ 才允许释放 D3D11 device/帧池；反序会在驱动内 AV。
   另外外部导入纹理**不能走 RD 的帧槽延迟释放**（故有 `TextureFreeImmediate`）。
10. **窗口 resize 的三条硬规则**（2026-11-08 修复；此前会破坏 RD 对象，基线回读同样复现）：
    - **尺寸唯一权威 = `WM_SIZE` 回调**（客户区），经 `screen_window_ids_` 路由到对应屏；
      绝不再用"BeginFrame 轮询首屏窗口宽高"这类第二源头（双源互相拉扯 → 不收敛）。
    - **render_pass 不编码尺寸**（附件只有格式/采样/加载存储/布局）。`RenderBuffers::Recreate`
      只重建尺寸相关的纹理+framebuffer，**必须保留 render_pass**——销毁它会经
      `DependencyAdd(render_pass→pipeline)` 级联误杀 composite pipeline。
    - **RID 的 index 是每表本地槽号**，高 32 位带表标识（`rid_detail::MakeTag`）保证跨表唯一；
      `FreeDriverResourceByRid` 逐表 `Owns()` 探测依赖此性质，破坏它会跨表误杀（见 tests/core/test_rid_owner.cpp）。

## 8. 文档与决策索引

| 想知道 | 去哪看 |
|---|---|
| 架构现状/目标/偏离/阶段定义 | `docs/skill/arhud-design/SKILL.md`（§1 分层、§7 servers、§7.12 阶段表、§7.11 冻结规格） |
| **当前进展 / 下一步 / 完成历史 / 计划** | `docs/skill/arhud-progress/SKILL.md`（唯一动态账本） |
| 编码风格约束（Google 原教裁决、存量迁移、clang-format） | `docs/skill/arhud-coding-style/SKILL.md` + 根 `.clang-format` |
| 历史 ADR（0001 Route B ~ 0010）/ devlog / 流程 / 旧设计 skill | git 历史（提交 `61eb290` 的 `_archive/`，其后已删除；`git show 61eb290:_archive/docs/adr/README.md` 等按需取阅） |

**变更义务**：代码改动若影响设计 skill 描述的结构/契约/阶段状态，同一提交内更新该文档的 ✅/🎯 标记；
完成里程碑/开始任务/调整计划时，同一提交内更新 progress skill（其 §0 更新协议）；
触及 §7.11 冻结接口（RenderingServer 虚表、`PushBound` 签名）时，先改文档、经确认后再改代码。
