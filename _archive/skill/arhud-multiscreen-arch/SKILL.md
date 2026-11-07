---
name: "arhud-multiscreen-arch"
description: "AR HUD multi-screen architecture aligned with Godot's multi-window strategy — Engine-level shared render stack (RD/CommandQueue/Compositor/ItemStorage/RenderServer), lightweight ScreenViewport per display, render/present split (single render thread with the only offscreen GL context rendering into per-screen shared buffers — DMA-BUF/EGLImage on QNX·Linux·Android, CPU-GDI on Windows dev — + N lightweight platform present threads as compositor clients, no GL context sharing), cull-mask item visibility, per-screen camera, render pacing driven by present-thread free-after-flip. Invoke when implementing multi-screen support, hoisting the shared stack out of RenderViewport, routing commands by ViewportID, splitting render/present across threads, or tuning multi-display pacing."
---

# AR HUD 多屏架构（对齐 Godot 多窗口策略）设计规范

> 参考 Godot 4.x 多窗口模型：**一套全局渲染栈服务 N 个窗口**，窗口本身是轻量视口
> （只持 Swapchain/相机/后处理配置），单渲染线程顺序驱动所有窗口。
> 目标：替代当前"每个 RenderViewport 一套完整渲染栈"的结构。

**前置阅读**：根 `AGENTS.md`（分层铁律）、`arhud-render-thread-arch`（B3 渲染搬线程单屏设计）。

**进度**：B3-M0 已完成（共享栈上提至 ARHudEngine + 离屏 GL context + FBO 渲染 +
FramebufferReadback + per-screen FrameImageQueue/PresentThread/GDI 上屏已落地，可构建运行）；
B3 ✅ 完成（2026-09-03）：ExecuteFrame record/replay 退役，改为 kSubmitScene 信号驱动的
渲染线程主循环（render_wake_ 三源唤醒 + buffer-token self-pacing + dirty bit 全部落地）。。

**决策记录**（2026-08-23 评审）：按 Godot 多屏策略实施（单共享栈 + 单渲染线程）。
已否决方案：进程内每屏独立渲染线程、多进程渲染（理由见 §0.2）。

**决策记录**（2026-08-24 评审）：**绘制/上屏分离**——渲染线程独占唯一离屏 GL
context，渲染到 FBO 导出 FrameImage，从不调 SwapBuffers；每屏一个轻量 present 线程
（无 GL）用平台 API 上屏并独立等各自 vblank。理由：GL SwapBuffers vsync 同步阻塞会
把单线程批量 swap 的多屏 vblank 等待串行化（§0.3）；present 线程无 GL 需求，
wglShareLists / VAO 跨上下文问题整体消失。已否决：单 context + drawable 切换 +
批量 swap；per-screen context + wglShareLists（§3.1）。

---

## 〇、为什么（动机与边界）

### 0.1 当前结构的问题（每屏一套完整栈）

| 问题 | 后果 |
|---|---|
| RenderViewport 按值持有 rd_/queue_/executor_/item_storage_/server_/compositor_ | N 屏 = N 套栈、N 渲染线程 |
| GL 上下文不共享（gl_context_manager_win32.h "所有 Context 不共享资源"） | 同一字体/图标 VRAM 上传 ×N；RID 不跨屏 |
| `GetRenderServer()` 只暴露屏 0 | 跨屏共享资源在 API 层不可能 |
| B3 若按现结构实施 | 渲染器搬线程动作重复 N 次 |

### 0.2 为什么单渲染线程（不是每屏一线程）

- HUD 负载轻（单屏 2~5ms），GPU 远未到瓶颈；多线程同 GPU 提交反引入驱动锁竞争
- 进程内多线程无故障隔离收益（同地址空间，一处崩溃全灭）；真隔离应多进程
- 行业惯例一致：Unity Display / UE per-viewport swapchain / SurfaceFlinger / QNX Screen
  均为单设备多 swapchain
- 多线程唯一真优势（各屏 vsync 等待重叠）由 present 线程达成（§0.3/§3.6）——present 线程只做平台上屏，不是渲染线程，无驱动锁竞争

**边界**：多屏不解决的问题——异相 vblank 漂移（§3.8）、驱动覆盖 vsync（既有教训）。
这些靠呈现调度与实测缓解，不靠线程数。

### 0.3 为什么绘制/上屏分离（2026-08-24 决策）

GL `SwapBuffers` vsync 开启时**同步阻塞调用线程直到 vblank**。单渲染线程批量 swap
（先渲染所有屏再连续交换）在非 genlock 屏间把 vblank 等待**串行化**：

```
双屏异相 vblank（相位差 8ms，均 60Hz）：
  批量 swap:  render 6ms → swap A 等 ~10ms → swap B 等 ~8ms = ~24ms（等待求和）
  独立等待:   两屏各自等各自 vblank，重叠 → wall time ≈ 16ms
```

解法不是退回"每屏一套栈"，而是**只把上屏拆出去**：

- 渲染线程独占唯一离屏 GL context，渲染到 FBO 导出 FrameImage，从不调 SwapBuffers
- 每屏一个 present 线程：无 GL context，仅用平台 API 把图片提交到窗口
  （Windows `StretchDIBits` / QNX `screen_post_window`），各自独立等各自 vblank——
  vblank 等待恢复并行，渲染线程与主线程零阻塞
- 无 wglShareLists：present 线程没有 GL 需求，纹理/VAO/FBO 跨上下文问题整体消失
- 渲染线程节拍由 present 线程的 buffer 释放节奏自然维持（§3.8），无需定时器

---

## 一、Godot 多屏策略要点 → 本工程映射

| Godot 概念 | 本工程对应 | 说明 |
|---|---|---|
| RenderingServer（全局单例） | RenderServer（Engine 级，全局 RID 空间） | 主线程 API 代理，命令序列化 |
| RendererCompositor（单例，全部 Storage/Renderer） | ARHudCompositor（Engine 级 ×1） | 共享栈核心 |
| RenderingDevice + 渲染线程 ×1 | RenderingDevice + 单渲染线程（Engine 级，离屏 context） | 渲染到 FBO，不 SwapBuffers |
| —（Godot 渲染线程内 present） | PresentThread ×N（无 GL，平台 API 上屏） | 绘制/上屏分离（§0.3/§3.6） |
| Window（继承 Viewport，轻量） | ScreenViewport（原 RenderViewport 瘦身） | 仅上屏窗口/相机/后处理/尺寸/渲染调度 |
| DisplayServer 多窗口 + 共享 GL 对象空间 | 唯一离屏 GL context + FBO 导出 + 平台上屏 | 一套对象空间，零上下文共享（§3.1） |
| Viewport.canvas_cull_mask + CanvasItem.visibility_layer | item visibility_mask + viewport cull_mask | item 多屏可见性 |
| World2D/3D（多窗口共享世界） | 全局 ItemStorage（item 全局，mask 决定归属） | 一份状态多屏消费 |
| 相机属于 viewport（节点树拓扑） | 相机/灯光 per-screen，且**由 `RenderViewport` 自持**（`ViewportSceneState`，ADR-0007） | Camera3DSet(ViewportID, …) |
| draw() 每帧渲染全部视口 | kSubmitScene 全局信号 → 遍历 due screens | + dirty 跳过（§3.7）；渲染节奏由 buffer 令牌 self-pacing（§3.8） |
| sync()（主线程等渲染完成，最多超前 1 帧） | RD frame slot 背压 + frame_done 信号量 | 帧节拍两相结构（§3.8） |

---

## 二、目标架构

```
ARHudEngine（Engine 级共享栈，PIMPL）
  ├─ RenderingDevice rd_               ×1 全局（资源生命周期/帧循环/延迟销毁）
  ├─ CommandQueue + CommandExecutor    ×1（全局命令 + viewport 命令路由）
  ├─ ARHudCompositor                   ×1（全部 Storage + Canvas/SpatialRenderer）
  ├─ ItemStorage                       ×1（**纯 item 账本**：全局 RID + per-screen dirty 门；相机/灯光已按 ADR-0007 迁出）
  ├─ RenderServer                      ×1（app 唯一入口）
  ├─ 渲染线程 ×1（唯一 GL context，离屏）——渲染进共享 buffer（DMA-BUF/pixmap/CPU），不 SwapBuffers
  ├─ FrameImageQueue[N]（双缓冲槽状态机）  ← FREE→WRITING→READY→COMMITTED→ON_SCREEN→FREE
  ├─ IFramePipeline[N]（平台帧收发，每屏一个）← Windows CPU-GDI / QNX Screen / Linux DMA-BUF / Android AHardwareBuffer
  ├─ PresentThread[N]（无 GL，compositor client）← commit + 等 frame callback，free-after-flip（§3.6）
  └─ ScreenViewport[N]（原 RenderViewport 瘦身）
       ├─ 上屏窗口（纯平台窗口，无 GL 像素格式）
       ├─ 尺寸 / resize 状态
       ├─ PostProcessParams + RenderBuffers（FBO 本就 per-screen，无共享冲突）
       ├─ ViewportSceneState（相机/灯光；ADR-0007 自 ItemStorage 迁入）
       └─ （呈现调度由 buffer 令牌 self-pacing 承担，见 §3.8；无 decimation/clock 配置）
```

**所有权迁移表**：

| 组件 | 现归属（RenderViewport 成员） | 目标归属 |
|---|---|---|
| rd_ / command_queue_ / command_executor_ | RenderViewport | ARHudEngine::Impl |
| compositor_（全部 Storage + Renderer） | RenderViewport | ARHudEngine::Impl |
| item_storage_ / render_server_ | RenderViewport | ARHudEngine::Impl |
| render_thread_ + 同步原语 | RenderViewport | ARHudEngine::Impl（渲染线程不 SwapBuffers） |
| surface/context（per-screen GL） | RenderViewport | **废弃**——唯一离屏 context 归 Engine；窗口退化为纯上屏窗口 |
| present 线程 + FrameImageQueue 槽 | （无，新增） | ARHudEngine::Impl |
| IFramePipeline（平台帧收发） | （无，新增） | ARHudEngine::Impl 持有 N 个（每屏一个，见 §3.9） |
| 上屏窗口/尺寸/后处理/渲染调度 | RenderViewport | ScreenViewport（保留） |

**线程归属**（多屏语义）：

| 组件 | 主线程 | 渲染线程（全局唯一） | Present 线程（每屏一个） |
|---|---|---|---|
| ARHudEngine / RenderServer | ✅ 调用入口 | ❌ | ❌ |
| CommandQueue push 端 | ✅ 写 | ❌ | ❌ |
| CommandQueue flush / Executor | ❌ | ✅（全局命令 + viewport 命令） | ❌ |
| Storage / Renderer / RD / ItemStorage | ❌ | ✅ 独占 | ❌ |
| GL context（唯一，离屏） | ❌ | ✅ 独占 | ❌（无 GL） |
| FrameImage 写（FBO 导出） | ❌ | ✅ | ❌ |
| FrameImage 读 + 平台上屏 | ❌ | ❌ | ✅ |
| ScreenViewport 配置（cull_mask/schedule/resize） | ✅ 写（经命令或加锁） | ✅ 读 | ✅ 读（schedule 经渲染侧传导） |

---

## 三、核心设计

### 3.1 渲染/上屏分离与 GL 上下文策略（2026-08-24 决策）

**决策：离屏单 GL context（渲染线程独占）+ 渲染进共享 buffer + 平台 compositor 上屏**

- 渲染线程持有**唯一** GL context，绑定离屏 surface（pbuffer / 隐藏 1×1 窗口），
  全部渲染输出到 per-screen FBO；**从不调 SwapBuffers**——RD swapchain 机制随之退役
- 渲染产物 = per-screen 共享 buffer，经 **IFramePipeline** 平台抽象收发（§3.9）：
  - **QNX/Linux 生产**：`DMA-BUF`/Screen pixmap——渲染侧 EGLImage 导入作 FBO 颜色附件
    **GPU 直写零拷贝**；v1 readback-into-pixmap 兜底（统一内存 SoC 开销小）
  - **Android**：`AHardwareBuffer`（gralloc）+ `EGL_ANDROID_image_native_buffer` 导入
  - **Windows 开发**：`glReadPixels` 回读到 CPU 缓冲（~1-2ms/屏，仅开发期可接受）
- present 线程（每屏一个，**无 GL context**）是 **compositor client**，不是 DRM master：
  把 buffer（DMA-BUF fd / pixmap / CPU 像素）交给平台 compositor——
  QNX `screen_post_window` / Linux `wl_surface.commit`（`zwp_linux_dmabuf_v1`）/
  Android `ANativeWindow_queueBuffer` / Windows `StretchDIBits`
- **探测到的模型纠正**：`drmModePageFlip` 等直通 DRM 只在"引擎自己就是 compositor"
  （裸 metal）时成立；Weston/Screen/SurfaceFlinger 场景下引擎是 client，`screen_post_window`
  和 `wl_surface.commit` 本质都是"client 向 compositor 交 buffer"，DRM master 在 compositor 手里
- **Windows 实现约束**：上屏窗口**不得设置 GL 像素格式**（GDI 与双缓冲 GL 像素格式
  混用行为未定义）；窗口是纯 DWM 合成窗口，DWM 保证不撕裂；GL context 只绑离屏 surface
- VAO/FBO 跨上下文问题**整体消失**（只有一个 context，且 present 线程无 GL）——
  零 RDD 改动；`gl_context_manager_win32.h` 的"Context 不共享资源"限制不再相关

**已否决（2026-08-24）**：

| 方案 | 否决理由 |
|------|--------|
| 单 master context + per-surface drawable 切换 + 批量 swap | `SwapBuffers` vsync 同步阻塞，非 genlock 屏间 vblank 等待串行化（§0.3）；drawable 切换代码随本决策成为死路径 |
| per-screen context + wglShareLists | VAO/FBO 不跨上下文需 RDD 键控改造；present 线程引入后本无 GL 需求，共享失去意义 |
| 每屏独立渲染线程 | 无故障隔离收益（同进程同地址空间）+ 驱动锁竞争；HUD 负载轻，单线程足够（2026-08-23） |

### 3.2 ScreenViewport（RenderViewport 瘦身）

- **移除**：rd_ / command_queue_ / command_executor_ / item_storage_ / render_server_ /
  compositor_ / render_thread_*（全部上提）；surface_id / context_id（GL 解耦——
  上屏不再依赖 GL surface）
- **保留**：上屏窗口句柄（纯平台窗口，无 GL 像素格式）、宽高、PostProcess 全套
  （RenderBuffers/合成管线——FBO 本就 per-screen）；渲染节奏由 buffer 令牌 self-pacing
  承担（§3.8），无 decimation/clock 配置
- **API**：`Initialize(EngineStackRefs, Config)` / `Finalize()` / `HandleResize()` /
  `GetPostProcessParams()`
- `RenderFromStorage()` 在 B3-M0 过渡期保留（主线程渲染到 FBO + 内联导出/上屏），
  B3 移入渲染线程后转私有；`GetCompositor()` / `GetRenderingDevice()` 公有入口已于
  2026-09-27 删除（app 只见 RenderServer）

### 3.3 ItemStorage 全局化与可见性

- **RID 全局**：next_rid_ 全局分配，跨屏 RID 引用天然成立；item 命令**不加 ViewportID**
- **可见性 = cull mask（对齐 Godot）**：
  - item 增加 `visibility_mask`（uint32，默认 0xFFFFFFFF）；
    API：`CanvasItemSetVisibilityMask(RID, uint32)`（Spatial/Text 同理）
  - viewport 增加 `cull_mask`（默认 0xFFFFFFFF）；API：`ScreenSetCullMask(ViewportID, uint32)`
  - 渲染过滤：item 对某屏可见 ⇔ `(item.visibility_mask & screen.cull_mask) != 0`
  - 单屏现状零行为变化（全匹配）
- **相机/灯光 per-screen 且由视口自持**：`RenderViewport::scene_state_`（`ViewportSceneState`：
  `camera_3d` / `camera_2d` / `light` / `has_light`）——对齐 Godot 相机属 viewport；
  **已按 ADR-0007 自 `ItemStorage` 迁出**（视口按索引经 `IViewportProvider` 解析）；API 见 §3.4

### 3.4 命令路由

| 命令类别 | ViewportID | 例子 |
|---|---|---|
| 资源 CRUD | 无（全局） | kTextureCreateFromData / kMaterialFree … |
| item CRUD/Set* | 无（全局 RID） | kCanvasItemSetTransform / kTextItemSetText … |
| 帧信号 | 无（全局，对齐 draw()） | kSubmitScene |
| viewport 状态 | **有** | kCamera3DSet / kScreenSetCullMask / kResize |

- RenderServer API 变化：
  ```cpp
  // 相机/灯光加 ViewportID（单屏便捷重载默认 screen 0，现有 demo 零改动）
  void Camera3DSet(ViewportID p_screen, const CameraMatrix &p_camera);
  void Camera3DSet(const CameraMatrix &p_camera);   // = screen 0
  void ScreenSetCullMask(ViewportID p_screen, uint32_t p_mask);
  ```
- **kPresent 移除** ✅（2026-09-27 完成）：上屏由 present 线程执行（§3.6），从来不是命令；枚举值已从 `RenderCommandType` 删除
- FrameScene 立即模式路径（FrameSceneBuffer / BeginScene / SubmitScene-帧场景变体）
  在 B3-M0 **直接删除**——demo 已全部保留模式，属死代码，不再上提（✅ 2026-09-27 孤儿文件 `frame_scene.h/.cpp` 已删除）

### 3.5 渲染线程主循环（B3，Engine 级）

```cpp
// arhud_engine.cpp 渲染线程（B3 后）
void ARHudEngine::RenderThreadFunc()
{
    context_driver_->MakeCurrent(offscreen_context_id_, offscreen_surface_id_);
    rd_.MakeRenderThread();
    render_thread_ready_.Post();

    // 启动种子：初始双 buffer 全 FREE，但尚无任何 release 事件——若不预唤醒，
    // 首帧永远画不出来（P0）。按槽数 pre-post 或直接先跑一轮再进 wait。
    render_wake_.Post(kSlotsPerScreen);

    while (render_thread_running_.load())
    {
        render_wake_.Wait();           // 唤醒源：buffer 释放 + 命令 push（见下）
        if (!render_thread_running_.load()) { break; }

        command_queue_.Flush();        // 先 flush：SubmitScene/PushAndSync/Create 在此生效

        // 逐屏：有 free buffer 且无在途帧才渲染（deadline 优先，无 decimation）
        for (ViewportID i = 0; i < screens_.Size(); ++i)
        {
            if (!screens_[i]->IsDirty()) { continue; }            // 静态跳过（§3.8）
            if (queues_[i]->HasInFlight()) { continue; }          // 防渲染超前（§3.8）
            FrameSlot *slot = queues_[i]->TryAcquireFree();       // FREE→WRITING；无 free 则跳过
            if (slot == nullptr) { continue; }

            screens_[i]->HandleResizeIfNeeded();
            RenderViewportToFBO(*screens_[i]);               // 全局 ItemStorage × cull_mask → FBO
            pipelines_[i]->ExportFBO(fbo, slot->index);   // 生产：EGLImage 直写；Windows：glReadPixels
            slot->fence_fd = pipelines_[i]->ExportFenceFd(); // sync_file fd
            queues_[i]->MarkReady(slot);                     // WRITING→READY；post present 唤醒
        }

        frame_done_.Post();           // 帧节拍：唤醒主线程（§3.8）
    }
    command_queue_.Flush();
    context_driver_->ReleaseCurrent();
}
```

- 渲染线程**从不调 SwapBuffers**——渲染频率由 present 线程的 buffer 释放驱动（§3.8），
  无需计时器/计数器/decimation：C=90Hz、B=60Hz、A=30Hz 各屏按各自显示周期自动保序
- **唤醒源（P0-2 止血）**：`render_wake_` 除 present 释放外，还有两个 post 源——
  ① 启动按槽数 pre-post；② CommandQueue push / SubmitScene 时 post（否则全静态内容下
  渲染线程睡死，app 推了内容也无人唤醒去 flush）
- **HasInFlight() 防渲染超前**：该屏已有 COMMITTED/ON_SCREEN 帧在途则跳过渲染——
  每屏最多一帧在途，渲染严格 1:1 跟随显示，精确 1 帧延迟
- `ExecuteFrame` 双路径（预录 CommandBuffer 跨线程回放）移除——B3 原有内容
- 渲染顺序：for 循环按 C→B→A 排（deadline 优先，§3.8）
- demo 迁移：`render_viewport->RenderFromStorage()` → `render_server->SubmitScene()`

### 3.6 Present 线程（每屏一个，无 GL，compositor client）

```cpp
// 每屏一个 present 线程（B3 后）
// 正确性铁律：buffer 只能由 compositor 的 release 事件释放，frame callback 只是
// "帧翻入了"，不代表 buffer 可复用（P0：frame callback 就 FREE 会每帧覆写扫出中的画面）
void ARHudEngine::PresentThreadFunc(ViewportID p_screen)
{
    const FrameSlot *on_screen = nullptr;   // 当前正在扫出的 buffer（未获 release）
    while (present_running_[p_screen].load())
    {
        // 1. 取下一帧（就绪帧；无新帧则 hold，禁止忙等）
        const FrameSlot *slot = queue_[p_screen]->AcquireReadySlot();  // READY→COMMITTED
        if (slot == nullptr) { break; }  // shutdown

        // 2. commit：buffer 交给 compositor，排到下一 vblank（非阻塞）
        //    Windows: StretchDIBits / QNX: screen_post_window /
        //    Linux: wl_surface.attach+commit / Android: ANativeWindow_queueBuffer
        IFramePipeline *pipe = pipelines_[p_screen];
        pipe->Commit(slot->index, slot->fence_fd);                   // COMMITTED→（pending→）ON_SCREEN

        // 3. 等 compositor 归还"上一帧"的 buffer（release 事件）——不是等本帧 frame callback
        //    Wayland wl_buffer.release / QNX Screen release / Android dequeueBuffer
        //    release 语义：继任帧翻入后，旧 buffer 才真正空闲可复用
        pipe->WaitBufferRelease();

        // 4. 释放上一帧（它此刻已 ON_SCREEN→被替换→FREE）
        if (on_screen != nullptr)
        {
            queue_[p_screen]->ReleaseSlot(on_screen);   // ON_SCREEN→FREE + post render_wake_
        }
        on_screen = slot;
    }
    // shutdown：末帧 buffer 直接归还（无继任帧会释放它）
    if (on_screen != nullptr) { queue_[p_screen]->ReleaseSlot(on_screen); }
}
```

- **提交顺序铁律**：`commit` 在 `等 release` **之前**；present 线程持有"前任"槽，当前帧
  的 release/翻入到达时才释放前任——commit K 释放的是 K-1，不是 K
- **buffer 生命周期铁律（P0-1 修正）**：`frame callback` = 帧刚翻入、正在扫出；
  `release 事件`（`wl_buffer.release`/Screen release/dequeueBuffer）才是 buffer 可复用。
  二者不可混。frame callback 降级为时间戳用途（§3.8 实测手段），**不用于释放**
- **状态机**：`FREE→WRITING→READY→COMMITTED→ON_SCREEN→FREE`。`ON_SCREEN` 是把
  "已提交 compositor" 与"正在扫描"区分开的独立态——release 事件触发 ON_SCREEN→FREE，
  这是 free-after-flip 的正确语义
- **fence 传递**：渲染完成信号（`glFenceSync` → sync_file fd）随 `Commit` 交
  compositor（Linux `wp_linux_explicit_synchronization` / QNX Screen fence），compositor
  等 fence 后才扫 buffer——防扫到 GPU 还在写的帧
- vblank 等待**并行**：各 present 线程独立阻塞于各自屏的 release 事件，渲染线程
  与主线程均不被拖慢——这就是 §0.3 的动机落地

### 3.7 内容更新率控制（静态 HUD 零成本）

> 注意：混合刷新率（60Hz 仪表 + 30Hz HUD）**不由 decimation 控制**——§3.8 的 buffer
> 令牌 pull 模型已让每屏按各自显示频率自动渲染。本节只解决"内容不变就不该重复渲染"
> 的功耗问题。

- **dirty 门（内容更新率）**：`ItemStorage` 单一屏位掩码 `dirty_screen_mask_`
  （ADR-0007 起相机/灯光迁出；单一掩码取代原全局 `dirty_`）——`Mark*` 是唯一置位
  入口（CommandExecutor 的 Set*/Create 路由、相机灯光经 `MarkScreen`）；渲染线程
  逐屏 `IsScreenDirty`，全部屏消费完后统一 `ClearDirty`。未置位的屏跳过渲染
  （无新图片 → present 线程 hold，前缓冲持续扫出）。静态 HUD GPU/CPU 成本 ≈ 0
- **dirty 门的两个来源（ADR-0008）**：
  1. **内容变更**：app 发命令 → 渲染线程 `Mark*`（常规路径）；
  2. **外部重绘请求**：`WM_PAINT` / `WM_SIZE` → 窗口 `OnPaint` / `OnResize` →
     引擎 `RequestFullRedraw()`（主线程只置原子 `force_redraw_` + 唤醒渲染线程）
     → 渲染线程 `MarkAllScreens()` **且强制一次渲染**（dirty 单独置位不足以触发
     渲染，渲染由 `submit_pending_` 门控）。动机：跳过渲染依赖上屏表面驻留，
     系统要求重绘（遮挡揭开/最小化恢复/DWM 重置/尺寸变化）时必须能自行唤醒，
     否则露背景。
- **三个频率彻底解耦**：
  | 频率 | 控制方式 | 控制方 |
  |---|---|---|
  | 面板刷新率（60/30/90Hz） | 显示硬件 | compositor 自己 |
  | 渲染节奏 | buffer 令牌 self-pacing（§3.8） | present 释放节奏 |
  | 内容更新率 | dirty 门（内容变更 ∪ 外部重绘） | app（变化才发命令）/ 平台重绘事件 |
  三者互不干扰，引擎只感知前两者，内容更新率由 app 与平台事件共同驱动
- **无 decimation/clock_screen 配置**：旧模型的 `RenderSchedule{decimation, phase, clock_screen}`
  已退役——decimation 在 pull 模型下冗余且会引入统计抖动（tick 是聚合释放速率，非稳定基频）；
  时钟屏的"恒渲染恒显示"由它的显示频率天然保证，无需标记

### 3.8 vblank 相位与帧节拍

- **相位事实**：无 genlock 时两屏独立时钟域，相位任意且漂移（±50ppm → 分钟级绕圈；
  59.94Hz vs 30.000Hz → 约 33s 绕一圈）；"30Hz vblank 落在 60Hz 周期 1/2 处"不成立，
  genlock 时也是**边沿重合**而非中点。新架构下异相 vblank 由 present 线程**并行
  吸收**，不再串行拖累渲染线程——genlock 从"必需"降为"锦上添花"（车规 DPU 仍建议
  选型确认）
- **渲染频率 = 显示频率（present 驱动渲染）**：buffer 令牌生命周期
  `FREE → WRITING → READY → COMMITTED → ON_SCREEN → FREE`，其中 **FREE 只在 compositor
  release 事件之后发生**（§3.6）→ 每屏 buffer 每显示周期释放一个 → 渲染线程每显示周期
  恰好拿到一次渲染机会。于是 C=90Hz、B=60Hz、A=30Hz 各屏自动以其显示频率渲染，
  **无需计时器/计数器/decimation**：
  - **防渲染超前**：渲染某屏前检查 `HasInFlight()`（该屏已有 COMMITTED/ON_SCREEN 帧
    在途则跳过）——初始化瞬间双 buffer 都 free 时，没有此 guard 会一口气画 2 帧
    （= +1 帧延迟且浪费）；每屏最多一帧在途 = 精确 1 帧延迟
  - **防空转 + 防死锁（P0-2）**：渲染线程阻塞在 `render_wake_`，唤醒源有三——
    present 释放 post、CommandQueue push/SubmitScene post、启动 pre-post（§3.5）；
    有活才醒，且全静态内容时命令 push 仍能唤醒去 flush
  - **deadline 优先**：for 循环按 C→B→A（刷新率降序）排，最快屏 deadline 优先
  - **latest-wins 降级为防御断言**：有 `HasInFlight()` 后渲染线程不可能连产两帧
    （前一帧在途就跳过），"后帧覆盖前帧"永不触发；`MarkReady` 里若发现已有 READY 帧
    属违反不变量，ASSERT 而非静默覆盖
- **主线程节拍** = RD frame slot（现有 frame_count_=2，最多超前渲染 2 帧）；命令 flush
  在等 buffer 之前执行，PushAndSync 最多等一帧，无死锁。全链路背压即节拍，无定时器、
  无显式 sync；B5 简化 RD 跨线程 frame slot 后背压链必须保留
- **渲染预算（P2 注意）**：最坏一轮串行渲 A+B+C（每屏 5ms 则 ~15ms）可能超 C 屏 11.1ms
  预算 → C 偶发掉到 ~45Hz 等效。缓解：C→B→A 顺序已让 C 优先；实测若 C 掉帧，给 C 屏升
  3 缓冲（其余保持 2）即可吸收抖动，不影响延迟链
- **实测手段**：Debug 日志输出每 present 线程 frame callback 时间戳（Windows 调试期
  DwmGetCompositionTimingInfo，车机读 DPU 计数器），长时间采样判断锁定/漂移

### 3.9 IFramePipeline：平台帧收发抽象（每屏一个）

> **命名对齐（2026-09-03 落地）**：实现中本抽象定名为 `IFramePresenter`
> （`platform/frame_presenter.h`），上屏由 `PresentThread` 调用。本文档沿用
> 设计期名 IFramePipeline，正文其余处同义。

渲染侧"生产帧"与上屏侧"上屏"偶联成一对，每平台一个实现。present 线程与渲染线程**都
不感知平台差异**。这是多平台（Windows/QNX/Linux/Android）的单一抽象面。

**职责边界（P2 澄清）**：槽状态机（FREE/WRITING/READY/...）归 `FrameImageQueue`；buffer
的**平台分配**在 init 时一次性完成归 pipeline（GBM allocate / `new` CPU / `AHardwareBuffer`），
运行时"哪个槽空着可用"归 queue。pipeline 只做**纯平台动作**，不分 buffer 获取。

```cpp
// platform/frame_pipeline.h
class IFramePipeline
{
public:
    virtual ~IFramePipeline() = default;

    // ── init（主线程，每屏一次）：分配双 buffer，返回可写入句柄挂到槽上 ──
    virtual Error CreateBuffers(uint32_t p_w, uint32_t p_h) = 0;

    // ── 渲染线程侧 ──
    /** @brief FBO → buffer（生产：EGLImage 直写零拷贝；Windows：glReadPixels） */
    virtual void ExportFBO(RDFramebufferID p_fbo, uint32_t p_slot) = 0;
    /** @brief 导出渲染完成 fence（sync_file fd）供 compositor 等待 */
    virtual int ExportFenceFd() = 0;

    // ── present 线程侧 ──
    /** @brief commit：buffer 交给 compositor（带 fence fd），排到下一 vblank */
    virtual void Commit(uint32_t p_slot, int p_fence_fd) = 0;
    /** @brief 等 compositor release 事件——旧 buffer 真正空闲可复用 */
    virtual void WaitBufferRelease() = 0;
};
```

**实现矩阵**：

| 平台 | 状态 | CreateBuffers | ExportFBO | Commit | WaitBufferRelease |
|---|---|---|---|---|---|
| **Windows** | dev | CPU 缓冲 | `glReadPixels`（~2ms，dev 可接受） | `StretchDIBits` → DWM | 同步返回（StretchDIBits 即拷完） |
| **QNX** | 生产 | Screen pixmap | EGLImage 直写（v2）/ readback（v1） | `screen_post_window` | Screen buffer-release event |
| **Linux** | 生产 | DMA-BUF（GBM/gralloc） | EGLImage 直写 | `wl_surface.commit` + `zwp_linux_dmabuf_v1` | `wl_buffer.release` |
| **Android** | 生产 | `AHardwareBuffer` | EGLImage（`EGL_ANDROID_image_native_buffer`） | `ANativeWindow_queueBuffer` | `ANativeWindow` dequeue/release |

**关键差异与约束**：

- **两个"导入"不可混**：DMA-BUF 在**渲染侧**是 EGLImage→GL 纹理→FBO 附件（GPU 写进去）；
  在**上屏侧**是 compositor 的 framebuffer（`zwp_linux_dmabuf_v1` / Screen pixmap），
  **不是** GL 纹理。上屏侧无 GL
- **buffer 可复用 = release 事件，不是 frame callback**（P0-1 铁律）：`WaitBufferRelease`
  等的是 `wl_buffer.release`/Screen release/dequeueBuffer，compositor 在继任帧翻入后才
  归还旧 buffer；`wl_surface.frame`（frame callback）只用于时间戳观测（§3.8 实测手段），
  **绝不可用于释放**
- **fence 传递**（生产）：`glFenceSync` → `ExportFenceFd` 导出 sync_file fd，随 `Commit`
  交 compositor；`wp_linux_explicit_synchronization` / QNX Screen fence 让 compositor 等
  GPU 写完再扫——防扫到半写 buffer（规则性撕裂，难调试）
- **format/modifier 协商**：GBM 分配时用 `EGL_EXT_image_dma_buf_import_modifiers` 拿
  modifier，EGLImage 创建带一模一样的 modifier；不匹配 = 红蓝翻转/tiling 花屏（无声）
- **EGLImage 禁止每帧重建**：CreateBuffers 时把每屏双 buffer 一次性建成 EGLImage+GL
  纹理缓存，每帧只 `glFramebufferTexture2D` 切换；每帧 `eglCreateImageKHR` 打不动 90Hz
- **Windows GLES 换后端**：Android 需 GLES3+EGL（当前是 GL 3.3 Core+WGL），是后端级
  前置改动，不是薄层
- **compositor 扫描条件**：全屏 surface + 格式匹配 + damage 全屏时 compositor 才走
  scanout（零拷贝）；否则 GPU 合成（compositor 的代价，非引擎代价）——AR HUD 全屏场景
  满足 scanout 条件

---

## 四、迁移计划（每阶段可编译、可测）

| 阶段 | 内容 | 验收 | 状态 |
|---|---|---|---|
| **B3-M0：共享栈上提 + 离屏渲染/导出管线**（渲染仍在主线程，~3 天） | M0.1 ✅ 共享栈 RenderViewport→Engine（2026-08-24）；M0.2 离屏 context + 上屏窗口去 GL 像素格式；M0.3 渲染路径统一 scene→FBO→导出 + IFramePipeline 接口 + Windows GDI 实现（§3.9），上屏暂由主线程内联执行；M0.4 ItemStorage 全局化 + cull mask + per-screen 相机；删 FrameScene 死路径 | 4 demo 单屏视觉不变；双窗口 demo（cull mask 区分内容、共享 RID）；grep app/ 无跨层访问 | M0.1 ✅ / M0.2~.4 ← 下一步 |
| **B3：渲染搬线程 + present 线程**（Engine 级，~3 天） | §3.5 渲染循环搬线程；PresentThread[N]（§3.6，无 GL，compositor client，free-after-flip）；FrameImageQueue 双缓冲状态机 + 背压节拍；kSubmitScene 全局信号；item 命令 PushAndSync → 异步 Push；移除 ExecuteFrame；RD 帧完成语义改为导出完成（fence），swapchain 退役；demo → SubmitScene()；顺带修 P0#1 + MaterialData::version 竞态 | demo 渲染结果一致；SubmitScene 立即返回；双窗口 demo 双屏并行上屏 | 未开始 |
| **B3-P：呈现调度**（~1 天） | dirty bit 内容更新率（§3.7）+ buffer 令牌 self-pacing 验证（§3.8）；present 时序 Debug 日志；QNX/Linux DMA-BUF 零拷贝（EGLImage + release 事件 + fence，目标硬件验证） | 60+30 双屏 demo：HUD present 计数 = 仪表一半；静态 HUD 渲染计数不增 | 未开始 |

**B3-M0 详细清单**：

1. ✅ 共享栈成员迁移 RenderViewport → ARHudEngine::Impl（B3-M0.1 完成 2026-08-24）；
   `RenderViewport::Initialize` 改收 `EngineStackRefs` 指针；`ARHudEngine::Initialize`
   建栈一次（rd→MakeCurrent→compositor→executor→queue→render_server）；`Finalize`
   逆序销毁；编译全绿 + 6/6 单测通过
2. GL context 改**离屏单 context**（pbuffer / 隐藏 1×1 窗口）；上屏窗口创建时**不设
   GL 像素格式**（纯 DWM/GDI 窗口，§3.1 约束）；`ARHudEngine::Initialize` 建栈一次；
   `AddScreen` 内聚创建上屏窗口，app 只传窗口参数（title/width/height）——对齐
   Godot Window 创建体验，Config 不再有 surface_id/context_id
3. 渲染路径统一为 scene→FBO→(post-process)→**导出 FrameImage**；新增 **IFramePipeline**
   平台接口 + Windows GDI 实现（`glReadPixels` + `StretchDIBits`，§3.9）；B3-M0 上屏由
   主线程渲染后**内联调用**（单线程呈现，B3 拆 present 线程）
4. ✅ 删除 FrameSceneData/FrameSceneBuffer + RenderServer::BeginScene/SubmitScene（帧场景
   变体）+ render_viewport.h 的 frame_scene_buffer_ 成员；RenderServer::Initialize 签名
   去掉 scene_buffer 参数（孤儿文件 `frame_scene.h/.cpp` 于 2026-09-27 删除）
5. ItemStorage：next_rid_ 语义不变（全局化自动成立）；相机/灯光曾作 per-screen 数组承载，
   **后续按 ADR-0007 迁入 `RenderViewport::scene_state_`**；
   CanvasItemData/SpatialItemData/TextItemData 加 visibility_mask
6. RenderServer：相机 API 加 ViewportID 重载；新增 SetVisibilityMask / ScreenSetCullMask
   （decimation/clock 配置已退役，见 §3.7）
7. CommandExecutor：viewport 命令分发到 per-screen 槽

**为什么 B3-M0 在 B3 之前**：上提是纯结构移动（渲染仍主线程，PushAndSync 过渡约束
不变）；离屏 context/导出管线同在 B3-M0 做——渲染路径改动在主线程即可独立验证，
B3 只做线程迁移（渲染循环 + present 线程），不做渲染路径改动，也无 throwaway 的
drawable 切换工作。若先做 B3，渲染器搬线程要对 N 个 per-screen 栈重复执行，且搬完
再上提等于二次重构。

---

## 五、与 Godot 的差异（裁剪说明，均为有意决策）

| Godot | 本工程 | 理由 |
|---|---|---|
| 每帧渲染全部窗口 | dirty 跳过 + buffer 令牌 self-pacing（§3.7/§3.8） | 车载功耗预算；每屏按其显示频率自动渲染 |
| embedded subwindows | 不做 | 无 GUI 嵌入需求 |
| World2D/3D 节点树 + CanvasItem 节点 | retained ItemStorage + RID API | 无场景树（Phase 6 前）；Phase 6 衔接时 CollectItems → 增量命令 |
| 节点 visibility_layer | 显式 SetVisibilityMask API | 同上 |
| 渲染线程内 present（draw + swap 同线程） | 渲染/上屏分离：离屏 context 渲 FBO 导出，present 线程平台 API 上屏 | GL SwapBuffers vsync 同步阻塞会串行化多屏 vblank 等待（§0.3）；present 线程无 GL，上下文共享问题消失 |
| Vulkan per-display present 队列 | present 线程 ×N（平台 API） | Phase 9 时 present 线程内直接换 vkQueuePresentKHR；present 异步后可再评估收敛 |

---

## 六、测试策略

| 测试 | 内容 | 阶段 |
|---|---|---|
| 双窗口 demo | 两窗口渲染不同 item 集（cull mask 区分），共享同一纹理/字体 RID | B3-M0 |
| ItemStorage 单测 | cull mask 过滤、per-screen 相机隔离、visibility_mask 默认全匹配 | B3-M0 |
| 导出/上屏冒烟 | Windows GDI 窗口无 GL 像素格式；FBO→映像→上屏内容正确 | B3-M0 |
| 命令路由单测（NullDriver） | viewport 命令 ViewportID 分发、全局命令无路由 | B3-M0/B3 |
| 异步 item 命令回归 | 当帧创建当帧渲染可见（B3 切异步后） | B3 |
| FrameImageQueue 状态机单测 | 完整态机 FREE→WRITING→READY→COMMITTED→ON_SCREEN→FREE 流转；release 事件才置 FREE；HasInFlight 防超前；启动 pre-post + 命令 push 唤醒（P0-2 回归）；shutdown 不阻塞 | B3 |
| fence 传递 | 渲染完成 fence → present 不早于 fence 提交；无扫到半写 buffer（目标硬件） | B3 |
| present 计数断言 | HUD present 计数 = 仪表 1/2；静态帧渲染计数不增 | B3-P |
| 时序长采样 | present frame callback 时间戳日志，判断 vblank 锁定/漂移 | B3-P/实车 |
| 回归 | 单屏 4 demo + CommandQueue 6 用例全绿 | 每阶段 |

---

## 七、风险与缓解

| 风险 | 缓解 |
|---|---|
| fence 传递错误 → 扫到半写 buffer（规则性撕裂，难调试） | `glFenceSync` 包住全部渲染命令；sync_file fd 随 Commit 交 compositor（explicit sync）；fd 关闭权属清晰（防双 close/漏 close） |
| format/modifier 不匹配 → 红蓝翻转 / tiling 花屏（无声） | `EGL_EXT_image_dma_buf_import_modifiers` 拿 modifier，EGLImage 用一致 modifier；B3-M0 目标硬件冒烟 |
| EGLImage 每帧重建 → 90Hz 打不动 | init 一次性建好 EGLImage+GL 纹理缓存，每帧只 `glFramebufferTexture2D` 切换 |
| release 事件与 frame callback 混淆 → 覆写扫出中画面（规则性撕裂） | P0-1 铁律：释放只听 release 事件（`wl_buffer.release`/Screen release/dequeueBuffer），frame callback 仅时间戳；§六 状态机单测覆盖 |
| Windows GDI/GL 像素格式混用（行为未定义） | 上屏窗口不设 GL 像素格式（纯 DWM 窗口）；GL context 仅绑离屏 surface；B3-M0 早期单屏 demo 冒烟 |
| Windows 回读 + BitBlt CPU 开销（~2-3ms/屏） | 仅开发期路径；生产走 DMA-BUF/EGLImage 零拷贝；PBO 双缓冲异步化列 backlog |
| QNX/Linux 零拷贝依赖驱动 EGLImage + modifier 支持 | v1 readback 过渡；v2 按驱动能力启用，目标 SoC 实测 |
| present 线程停转 → 渲染线程槽背压死锁 | watchdog：槽超时未消费告警 + 强制释放；B3 期 present 心跳日志 |
| 驱动覆盖 vsync（NVIDIA 教训） | 抽稀不依赖 interval；present 计数日志断言 |
| 上提后 RenderViewport 遗留引用悬垂 | B3-M0 用 grep 验证 RenderViewport 不再被外部直接触碰栈成员；Finalize 逆序解绑（沿 B2 模式） |
| ItemStorage 线性表多屏增长 | HUD 数十项量级 O(n) 可接受；超预期再换 RID→索引哈希 |
| Android 后端：GL 3.3 Core+WGL → GLES3+EGL | 后端级前置改动，非薄层；Android 实现排 Linux 之后 |

---

## 八、与其他 Skill 的关系

| Skill | 关系 |
|---|---|
| `arhud-architecture` | 现状（每屏一套栈）；本规范是其多屏演进目标 |
| `arhud-render-thread-arch` | B3 渲染搬线程的单屏设计；本规范把 B3 扩展为 Engine 级多屏循环 + 渲染/上屏分离（B3-M0 前置） |
| `arhud-improvement` | B3-M0 / B3 / B3-P 行动清单与执行顺序 |
| 分层与依赖铁律 | 见根 `AGENTS.md`（本规范不违反：共享栈仍在 servers 层内部重组，app 依赖面反而收窄） |
