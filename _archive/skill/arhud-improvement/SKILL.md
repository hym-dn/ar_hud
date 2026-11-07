---
name: "arhud-improvement"
description: "AR HUD next-step improvement plan — Route B remaining stages (B3 render-to-thread, B4 storage convergence, B5 cleanup; B1/B2 done), governance P1/P2/P3 items, priority order, effort estimates. Invoke when planning next work, deciding what to fix, or reviewing the improvement backlog."
---

# AR HUD 下一步改进计划

> 当前状态：Phase R + B1 + B2 + B2-R + B3-M0 + B3 + ADR-0005（item 树）
> + 测试基建 + 异常/RTTI + Camera 重构 已完成；Route B 剩余从 **B3-P** 起。
> 本文档列出剩余改进项，按优先级和依赖关系排序。
>
> **多屏决策**（2026-08-23 评审）：按 Godot 多屏策略实施（单共享栈 + 单渲染线程 +
> 轻量视口 + cull mask 可见性）。B3 扩展为 **B3-M0 / B3 / B3-P** 三阶段，
> 设计详见 `arhud-multiscreen-arch` skill。

---

## 一、已完成（基线）

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase R | demo 迁移新 ARHudEngine（PIMPL）+ 断链修复 | ✅ |
| B1 | CommandQueue SPSC 环形缓冲区 + SyncFlag P0 + 删 Push | ✅ |
| B2 | RenderServer + FrameScene + 资源命令化（详见下节） | ✅ |
| B2-R | RenderServer 保留模式重构（ItemStorage + 增量命令，详见下节） | ✅ |
| B3-M0 | 共享栈上提 + Config 拆分 + per-screen 相机/cull_mask + 离屏 FBO 导出 | ✅ |
| B3 | 渲染搬线程 + 每屏 present 线程 + FrameImageQueue 状态机 | ✅ |
| ADR-0005 | ItemStorage 树形管理（parent 链 + 有效状态合成 + z_as_relative） | ✅ |
| 测试 | GoogleTest + tests/ 模块化 + 25 用例全绿（command_queue 6 + item_storage 19） | ✅ |
| 编译 | 异常 + RTTI 启用（移除过度禁用） | ✅ |
| Camera | Camera3D→CameraMatrix（core/math/）、删 Camera2D、scene/ 清空 | ✅ |
| P0#1 | 延迟销毁锁 | 缓修 → B3 根本解决 |
| P0#2 | Fence 超时 | 缓修 → 实际不触发 |

---

## 二、Route B 剩余（主路径，结构性）

> 目标：渲染器搬到渲染线程，主线程只发高层命令，对齐 Godot。
> 设计详见 `arhud-render-thread-arch`（单屏线程模型）与 `arhud-multiscreen-arch`
>（多屏共享栈 + 呈现调度）skill。

### B2：RenderServer + FrameScene + 资源命令（~1 天）✅ 已完成（2026-08-22）

**内容**：
- 新增 `RenderServer`（主线程 API 代理）：资源创建/变更/释放方法，序列化为命令经 CommandQueue 到渲染线程
- 新增 `FrameScene`（帧场景双缓冲槽）：相机/灯光/item 列表，主写渲染读
- 命令词汇表：CommandType 枚举 + 命令结构体（TextureCreateCmd、MaterialCreateCmd 等）
- app 资源创建改走 RenderServer（demo 不再直接碰 Storage）

**验收**：
- `grep -rn "GetTextureStorage|GetMaterialStorage|GetMeshStorage|GetFontStorage" app/` 零命中
- demo 资源创建经 RenderServer → CommandQueue → 渲染线程执行 → RID 正确返回

**实际落地**：
- `render_commands.h`（已删除，ADR-0010 M3）
- `command_queue.h/.cpp` v3 重构：Entry 槽数组 → 1MB 序列化字节环形缓冲区（回绕 kPadding 填充 + 背压自旋）
- `command_executor.h/.cpp`：渲染线程命令反序列化 + 分发 Storage（含空 shader RID 自动取渲染器内置着色器）
- `frame_scene.h/.cpp`：FrameSceneBuffer 双缓冲（text_pool_ 深拷贝 + 指针重定向）；**B3 才接线消费**
- `render_server.h/.cpp`：CanBypassQueue 短路规则（无消费者/同线程/已关闭 → 直调 Storage）
- RenderViewport/ARHudEngine 接线（executor→queue→server，Finalize 逆序解绑）
- 4 个 demo 全部迁移；单测 6/6 绿；CommandQueue 须堆分配（1MB 内联缓冲区，栈实例溢出）

**B2 过渡决策**：MaterialSetUniform/SetTexture 保持主线程直调（渲染仍在主线程，避免数据竞态），B3 渲染搬线程后切异步命令。

### B2-R：RenderServer 保留模式重构（~2 天）✅ 已完成（2026-08-23）

> **设计变更**：B2 用了立即模式（每帧 BeginScene 重建 FrameScene 提交）。对齐 Godot RenderingServer 的**保留模式**：item 状态存服务端 ItemStorage（渲染线程），增量更新，帧提交仅信号。

**为什么改**：
1. **增量更新效率**：HUD 静态元素（背景/图标）不变化时不重复发送；车速表只在车速变时发 `TextItemSetText`
2. **Phase 6 天然衔接**：Scene Tree 节点改变 → 直接发增量命令，不需要 CollectItems 全量重建
3. **命令队列更高效**：保留模式传小 delta（`set_transform` ~32B），立即模式每帧传整个 FrameScene（几 KB）
4. **架构一致性**：已按 Godot 对齐 RCD/RDD/RD/CommandQueue/Compositor/Storage，RenderServer 这层不该例外

**核心设计**：

```
主线程（app）                    渲染线程
  │                               │
  rs->CanvasItemCreate()          │  ← 主线程预分配 RID（不等返回）
  rs->CanvasItemSetTransform()    │  ← Push(delta) → ItemStorage 更新 RID→ItemData
  rs->CanvasItemSetMaterial()     │  ← Push(delta) → ItemStorage 更新
  rs->SubmitScene()               │  ← Push(渲染信号) → 读 ItemStorage 状态渲染
```

**新增组件**：

| 文件 | 职责 |
|---|---|
| `storage/item_storage.h/.cpp` | **渲染线程 per-item 状态存储**：RID→ItemData（CanvasItem/SpatialItem/TextItem/Camera/Light），增量更新。渲染线程独占，无锁 |

**RenderServer API（保留模式）**：

```cpp
// 资源创建（不变，已是保留模式，同步返回 RID）
RID TextureCreateFromData(...);  RID MaterialCreate(...);  RID MeshCreateQuad(...);

// item 管理（新增，异步 Push，RID 主线程预分配）
RID CanvasItemCreate();
void CanvasItemSetTransform(RID item, const float xf[16]);
void CanvasItemSetMaterial(RID item, RID material);
void CanvasItemSetMesh(RID item, RID mesh);
void CanvasItemSetOpacity(RID item, float opacity);
void CanvasItemSetZOrder(RID item, int32_t z);
void CanvasItemSetLayer(RID item, uint32_t layer);
void CanvasItemSetVisible(RID item, bool visible);
void CanvasItemFree(RID item);
// ... SpatialItem* / TextItem* 同理

// 相机/灯光（服务端存储，设置一次，变化才更新）
void Camera3DSet(const CameraMatrix &cam);
void Camera2DSet(const Transform2D &cam);
void DirectionalLightSet(const DirectionalLight *light);

// 帧提交（简化为渲染信号）
void SubmitScene();  // 异步 Push，渲染线程读 ItemStorage 渲染
```

**关键设计决策**：
- **item RID 预分配**：`CanvasItemCreate()` 主线程直接从 RIDOwner 预分配 RID 返回（不走 CommandQueue，不阻塞）。渲染线程的 `ItemStorage::CanvasItemCreate(rid)` 按 RID 填状态。
- **增量更新**：只有属性变化才发命令（`SetTransform`/`SetColor`/`SetText` 等），静态元素零开销。
- **FrameScene 简化**：item 数据不再进 FrameScene（存服务端 ItemStorage）。相机/灯光也存服务端。FrameScene 简化为仅"帧提交信号"。

**demo 使用方式变化**：
```cpp
// 初始化（创建一次）：
RID item = rs->SpatialItemCreate();
rs->SpatialItemSetMaterial(item, material);
rs->SpatialItemSetMesh(item, mesh);
rs->SpatialItemSetTransform(item, transform);
rs->Camera3DSet(camera);

// 渲染循环（只发渲染信号，有变化才更新）：
while (...) {
    if (speed_changed) rs->TextItemSetText(speed_item, new_text);
    rs->SubmitScene();
}
```

**验收**：
- `grep -rn "GetTextureStorage|GetMaterialStorage|GetMeshStorage|GetFontStorage" app/` 零命中
- `grep -rn "BeginScene\|\.spatial_items\.PushBack\|\.canvas_layers\.PushBack" app/` 零命中（不再每帧重建场景）
- demo item 创建一次 + 增量更新（不再每帧 PushBack）
- 静态 HUD 元素零命令开销（无变化不发命令）

**实际落地**（2026-08-23）：
- `storage/item_storage.h/.cpp`：Canvas/Spatial/Text item 数据 + camera/light 全局状态，按 RID 线性表存储
- `render_commands.h`（已删除，ADR-0010 M3）：item CRUD + camera/light set 命令结构体（Spatial/Text Set* 复用 Canvas 同布局命令，靠 type 区分）
- `render_server.h/.cpp`：保留模式 API（CanvasItemCreate/Set*/Free 等）；item 命令全部 `PushAndSync`（B2 过渡：渲染仍在主线程 `RenderFromStorage` 读 ItemStorage，同步保证读一致；B3 搬线程后切异步 Push）
- `render_viewport.h/.cpp`：新增 `RenderFromStorage()`--从 ItemStorage 收集 SpatialItem 全量 + Canvas/Text 按 visible 过滤并按 layer 分组成 HUDCanvasLayer，复用立即模式 `Render()`
- `command_executor.h/.cpp`：分发 item 命令到 ItemStorage 对应方法
- 4 个 demo 全部迁移：item 创建一次（init），渲染循环只调 `RenderFromStorage()`（不再每帧 PushBack）；含动态 item 创建/释放
- **修复执行器 2 处 bug**：
  1. `ExecMaterialCreate` 空 shader RID 时未按 `data_type` 取渲染器内置着色器 -> 补 `GetCanvasRenderer/SpatialRenderer::GetShaderRID()` 解析
  2. `ExecuteCommand` 分发错误：`kSpatialItemSetMaterial/SetMesh`、`kTextItemSetFont/SetZOrder/SetLayer/SetVisible` 误调 Canvas 版执行器（写到 CanvasItem 而非对应 item）-> 补 6 个 Exec 方法，分发到 ItemStorage 对应方法
- 验证：4 demo 全部保留模式渲染正常，材质/纹理/网格 RID 全 valid，无 invalid material 警告，6000/600 帧干净退出

**B2 过渡决策**：渲染仍在主线程（`RenderFromStorage` 主线程读 ItemStorage），故 item 命令走 `PushAndSync` 保证读一致（异步命令当帧未执行会丢 item）。B3 渲染搬线程后读写同线程，切回异步 Push（真并行关键）。

**与 B3 的关系**：B2-R 的 ItemStorage 和增量命令是 B3 渲染搬线程的**前置**——B3 的 `kSubmitScene` handler 读 ItemStorage 状态渲染，而非读 FrameScene 槽。建议 B2-R 在 B3 之前完成。

### B3-M0：多屏共享栈上提 + 离屏渲染/导出管线（~3 天）

> 设计详见 `arhud-multiscreen-arch` skill。**先上提、后搬线程**：渲染器搬线程只做
> 一次，而非每屏一次；上提是纯结构移动（渲染仍主线程，PushAndSync 过渡约束不变）。
> 离屏 context/导出管线同在本阶段做——渲染路径改动在主线程即可独立验证，B3 只做
> 线程迁移，无 throwaway 的 drawable 切换工作。

#### B3-M0.1：共享栈上提 ✅ 已完成（2026-08-24）

**改动文件**：
| 文件 | 改动 |
|---|---|
| `render_viewport.h` | 新增 `EngineStackRefs` 结构体（7 指针：rd/command_queue/command_executor/frame_scene_buffer/item_storage/render_server/compositor）；`Initialize` 签名加第三参数 `const EngineStackRefs &`；7 个共享栈成员从值改指针 |
| `render_viewport.cpp` | `Initialize` 不再创建栈（只存指针 + per-screen init：surface/context/post-process）；`Finalize` 不再销毁栈（只停线程 + 后处理）；全部方法 `.`→`->`（7 组 replaceAll）；`&member_`→`member_`（15 处取址消除） |
| `arhud_engine.cpp` | `Impl` 加共享栈成员 + `GetStackRefs()`；`Initialize` 在 RenderViewport 前创建栈（rd.Initialize→MakeCurrent→compositor.Initialize→executor.Initialize→queue.SetExecutor→render_server.Initialize→rd.SetDeferredExecution→SurfaceSetVsyncMode）；`Finalize` 在 RenderViewport 后销毁栈（逆序）；`GetCompositor/GetRenderServer/GetRenderingDevice` 直接委派 `impl_` |

**验证**：编译 4 lib + 5 exe + 1 test 全绿；6/6 单测通过；行为不变（纯所有权迁移，渲染路径未改）

#### B3-M0.2：Config 拆分 + Initialize/AddScreen 分离 ✅ 已完成（2026-08-25）

**改动**：
- `ARHudEngine::Config` 拆为 `Config`（引擎级：vsync + thread_priority）+ `ScreenConfig`（per-screen：title + width + height）
- `Initialize` 只做 DisplayServer + GLContextManager（不创建窗口）
- `AddScreen` 内部创建窗口 + surface + context + 首屏延迟初始化共享栈（RCD+RD+Compositor+Queue+Server）+ RenderViewport
- 4 个 demo 迁移到新 API（`Initialize(engine_cfg)` + `AddScreen(screen_cfg)`）
- `GetWidth/Height` 委派 `screens_[0]`；`ShouldClose` 加无屏 guard；`Finalize` 加 window guard
- **离屏 context + 上屏窗口去 GL 像素格式移到 B3-M0.3**（与 IFramePipeline 上屏路径耦合，不可单独拆）

**验证**：编译全绿 + 6/6 单测通过

#### B3-M0.3：离屏 GL context + FBO 导出 + IFramePresenter ✅ 已完成（随 B3 一并落地，2026-09-03）

- GL context 改离屏（pbuffer/隐藏窗口，从不 SwapBuffers）
- 上屏窗口不设 GL 像素格式（纯 DWM 窗口）
- 渲染路径统一 scene→FBO→(post-process)→导出 FrameImage
- 新增 `IFramePresenter` 平台接口（`platform/frame_presenter.h`）+ Windows GDI 实现
  （`frame_presenter_win32`：`glReadPixels` + `StretchDIBits`）
- **实际落地**：接口定名 `IFramePresenter`（原拟名 IFramePipeline）；上屏由 `PresentThread`
  （B3）调用，非主线程内联；`RenderViewport::RenderAndSubmit` 六步内含 `FramebufferReadback`
- 备选 `kSwap` 路径（渲染直画窗口 backbuffer + `SwapBuffers` + `DwmFlush`）以
  `PresentMode` 预留（`present_thread.h`），现状默认 `kReadback`

#### B3-M0.4：ItemStorage 全局化 + cull_mask + per-screen 相机 ✅ 已完成（2026-08-25）

**改动**：
- `CanvasItemData`/`SpatialItemData`/`TextItemData` 加 `visibility_mask`（默认 0xFFFFFFFF）
- `ItemStorage`：相机/灯光从单一全局改为 per-screen `LocalVector<ScreenState>`（ViewportID 索引）；便捷重载默认 screen 0（demo 零改动）（**后续 ADR-0007 将相机/灯光迁出至 `RenderViewport::ViewportSceneState`**）
- `RenderViewport`：加 `cull_mask_`（默认全匹配）+ `viewport_index_`；`RenderFromStorage` 按 `visibility_mask & cull_mask` 过滤 + 用 per-screen 相机
- `SetVisibilityMask` / `SetCullMask` / `SetViewportIndex` 方法
- 删除 FrameScene 死路径：`FrameSceneBuffer` 从 EngineStackRefs/Impl/RenderViewport 移除；`RenderServer::Initialize` 去掉 scene_buffer 参数；`BeginScene`/`SubmitScene` 删除；`frame_scene.h/.cpp` 从 CMakeLists 移除（**孤儿文件本体于 2026-09-27 删除**）

**验证**：编译全绿 + 6/6 单测通过

**B3-M0 整体验收**：
- ✅ 4 demo 单屏编译通过（视觉行为不变——visibility_mask 默认全匹配、cull_mask 默认全匹配、per-screen 相机默认 screen 0）
- ✅ `grep -rn "BeginScene|FrameScene" app/` 零命中（死路径已删）
- 双窗口 demo（待 B3 阶段添加）

**B3-M0 全部完成** ✅

### B3：渲染搬线程 + present 线程（~3 天，核心）✅ 已完成（2026-09-03）

> B3-M0 之后按 **Engine 级单渲染线程 + 每屏 present 线程**实施（渲染线程归
> ARHudEngine，渲染到 FBO 导出 FrameImage；present 线程无 GL，是 compositor client，
> 通过 IFramePipeline 交 buffer + 等 release 事件），主循环详见
> `arhud-multiscreen-arch` §3.5/§3.6/§3.9。

**实际落地（2026-09-03）**：
- **kSubmitScene 全局信号**：`SubmitSceneCmd`（纯信号 16B）+ `RenderServer::SubmitScene()`
  （异步 Push）+ Executor handler（置位 Engine 注入的 `submit_pending_` atomic）
- **渲染线程主循环改形**（`RenderThreadFunc`）：`render_wake_` 三路唤醒
  （present release / CommandQueue push / 启动 pre-post）→ Flush → 消费 submit →
  `rd.BeginFrame()`（**RD 帧簿记搬入渲染线程**）→ 逐屏 `RenderAndSubmit()` →
  `rd.EndFrame()`（**立即执行模式**，`SetDeferredExecution(false)`——同线程录放）
- **ExecuteFrame 双路径退役**：渲染线程不再回放预录 CommandBuffer；
  `main_to_render_` 信号量删除；主线程 Begin/EndFrame 只剩 compositor 节拍回调
- **RenderViewport::RenderAndSubmit()**：合并原 RecordRender+ReadbackAndMarkReady，
  渲染线程独占（dirty → HasInFlight → TryAcquireFree → RenderFromStorage →
  FramebufferReadback → MarkReady 六步）
- **ItemStorage dirty bit**（§3.7 内容更新率）：29 个变更点置位（Set*/Free/
  Create/Camera/Light/ClearAll）+ 渲染消费清零。静态内容零成本渲染跳过已验证
  （demo presented=1 帧后不再渲染，内容不变）
- **item 命令 PushAndSync → 异步 Push**（35 处；Create 保留同步——主线程需立即
  拿 RID）；**MaterialSetUniform/SetTexture 命令化**（MaterialData::version 竞态
  根治——变更仅渲染线程触碰）
- **FrameImageQueue 唤醒接线**：`SetWakeSemaphore(render_wake_)`，Release/
  PostRenderWake 同时 post 槽位背压与线程唤醒（free_sem_ 保持计数语义不被借用）
- **demo 迁移**：4 个 demo 渲染循环加 `render_server->SubmitScene()`
- **kPresent 移除**（2026-09-27 完成）：枚举值已从 `RenderCommandType` 删除，Executor 分支一并移除（上屏归 PresentThread，从来不是命令）

**验证**：全量构建绿（含 5 exe + tests）；单测 6/6；4 demo 全部干净退出
（零 ERROR、正确呈现；canvas 6000 帧 96s 注意测试超时误判）；grep app/ 零跨层
访问（RenderFromStorage/GetStorage/GetRenderer 均无代码命中）。

**遗留（B5 清理）**：~~`current_slot_` 成员闲置~~ ✅ 已删（2026-09-27）；~~`ExecuteFrame/OnFrameCompleted`
函数体~~ ✅ 已于 2026-09-03 随 B3 删除（原登记为陈旧——代码中从未残留）；~~RD BeginFrame 的 deferred
等待路径~~ ✅ 已随 ExecuteFrame 退役（2026-09-03）；~~RenderViewport::BeginFrame/EndFrame~~ ✅ 已删
（2026-09-27，零调用方且会与渲染线程重复驱动 `rd`）。

**内容**：
- 渲染线程归 ARHudEngine；主循环 = `render_wake_` 唤醒 → flush → 逐屏渲染到 FBO →
  ExportFBO → ExportFenceFd → MarkReady → frame_done
- **PresentThread[N]**（每屏一个，无 GL context）：IFramePipeline 平台实现上屏
  （Windows `StretchDIBits` / QNX `screen_post_window` / Linux `wl_surface.commit` /
  Android `ANativeWindow_queueBuffer`）；commit K 后等 release 事件释放 K-1（free-after-flip
  正确语义，§3.6 P0-1）；**buffer 令牌 self-pacing**——每屏渲染频率 = 该屏显示频率，
  无 decimation/clock（§3.8）
- **FrameImageQueue 状态机**：`FREE→WRITING→READY→COMMITTED→ON_SCREEN→FREE`；
  release 事件才置 FREE（不是 frame callback）；HasInFlight() 防渲染超前
- 唤醒源三路：present release post / CommandQueue push+SubmitScene post / 启动 pre-post
  （§3.5 P0-2 防死锁）
- 新增 `kSubmitScene` 命令 handler（全局信号，对齐 Godot draw() 一次画全部视口）；
  kPresent 移除——上屏由 present 线程执行，从来不是命令
- `ExecuteFrame` 双路径移除；`RenderFromStorage()` 迁入渲染线程并转私有
- RD 帧完成语义改为导出完成（fence）；swapchain/SwapBuffers 路径退役（B5 清理）
- item 命令由 `PushAndSync` 切回异步 `Push`（读写同线程，真并行）
- demo 迁移：`render_viewport->RenderFromStorage()` → `render_server->SubmitScene()`
- **顺带修 P0#1**：Free 操作搬渲染线程 → 同线程 → 无锁
- **顺带修 MaterialData::version 竞态**：变更全走命令队列 → 仅渲染线程触碰

**验收**：
- demo 渲染结果与之前一致
- 主线程 `SubmitScene` 后立即返回（不阻塞渲染）
- 双窗口 demo 双屏并行上屏（present 线程各自等各自 release 事件）
- `grep -rn "GetStorage|GetRenderer|RenderFromStorage" app/` 零命中

**前置**：B2 ✅ + B2-R ✅ + B3-M0（共享栈 + IFramePipeline 是 Engine 级循环的载体）

### B3-P：呈现调度（~1 天）

> 内容更新率控制（静态 HUD 零成本）+ buffer 令牌 self-pacing 验证 + 平台零拷贝落地，
> 详见 `arhud-multiscreen-arch` §3.7/§3.8。

**内容**：
- **dirty bit**（内容更新率）：ItemStorage 全局 dirty bit——绑定命令任意
  Set*/Free 置位，渲染清零；dirty=false 跳过渲染（无新图片 → present 线程 hold，
  前缓冲持续扫出）。静态 HUD GPU/CPU 成本 ≈ 0
- **buffer 令牌 self-pacing 验证**：每屏渲染频率 = 该屏显示频率（free-after-flip
  驱动），三屏 30/60/90Hz 各自保序；用 present 计数断言验证（不是 decimation）
- present 时序 Debug 日志（每 present 线程 release 事件时间戳；Windows 调试期
  DwmGetCompositionTimingInfo，车机读 DPU 计数器，长采样判断 vblank 锁定/漂移）
- QNX/Linux DMA-BUF 零拷贝落地（EGLImage 导入 + fence + modifier 协商，目标硬件实测）；
  v1 readback 过渡兜底

**验收**：
- 60+30 双屏 demo：HUD present 计数 = 仪表一半；静态 HUD 渲染计数不增
- 单屏 demo 不受影响（默认全 dirty，buffer 令牌按显示频率保序）

**前置**：B3

### B4：Storage 线程安全收敛（~1 天）

**内容**：
- 确认主线程零直接接触 storage（全部经 RenderServer 命令）
- `RIDOwner<true>`（带锁）→ 评估降 `<false>`（无锁，渲染线程独占）
- 静态检查：`grep` 验证主线程无 storage 直接调用

**前置**：B3

### B5：清理与优化（~1 天）

**内容**：
- ✅ 简化 RD 跨线程 frame slot 机制（2026-09-27 完成）：CommandBuffer 池从 `frame_count_` 槽
  退化为**单实例复用**，删除 `pool_state`/`frame_used` 轮转字段（录制/回放同线程，无需多槽）。
  **frame_done 帧节拍保留**——队列背压之外唯一的帧级调节器，见 `arhud-multiscreen-arch` §3.7
- ✅ `CmdBufferUpdate` 深拷贝优化（2026-09-27 完成）：命令改持 CommandBuffer 内建 payload 区指针，去逐条堆分配/析构
- 后处理独立子系统（从 RenderViewport 拆出）
- ✅ 公有入口收敛（2026-09-27 完成）：删除 `GetCompositor()` / `GetRenderingDevice()` 公有访问器
  （app 只见 RenderServer；`RenderFromStorage` 已在 B3 转私有）
- ✅ `ShaderCreateFromGLSL` → `ShaderCreateFromSource` + `ShaderSourceFormat`（治理 P1，2026-09-27 完成）

**前置**：B3/B4

---

## 三、治理独立项（B 类，不阻塞 Route B，可穿插）

> 与 Route B 无关的独立清理，可在 B2-B5 之间穿插做。

### P1（高优，接口定稿前必须做）

| 项 | 内容 | 预估 |
|---|---|---|
| ~~`ShaderCreateFromGLSL` 改名~~ ✅ 已完成（2026-09-27） | → `ShaderCreateFromSource` + `ShaderSourceFormat` 枚举（GLSL/SPIR-V）；RDD/RD/GL 已加参数，Storage 早已合规命名 | ~半天 |
| **3D 相机设置端到端不生效**（CameraMatrix 惰性 + 矩阵传输不匹配） | `Set*()` 只置 dirty 不重算、`Get*Matrix()` 不触发重算 ⇒ 命令载荷 stale；Executor 新建的 CameraMatrix dirty=true，又被 `SpatialRenderer` 的 `UpdateMatrices()` 按**默认参数**覆盖 ⇒ 相机恒为默认（fov 45/eye 原点/target -Z）。**需裁决「传参数 vs 传矩阵」**，见 devlog `2026-09-27_camera_pipeline_per_viewport.md` §4 | ~1 天 |

### P2（中优，结构清理）

| 项 | 内容 | 预估 |
|---|---|---|
| 三套 ID 体系收敛 | `RID`/`RDBufferID`/`BufferID` → 砍 RDXXXID 中间层，统一用 RID | ~1 天 |
| **Phase 6 Ref 语义裁决（前置检查项）** | Scene 层 Node/Resource 是否采用 Ref 体系；若采用，正式拍板**外部控制块**（本工程现状，仿 std::shared_ptr）vs Godot 内部计数语义——见 `arhud-architecture` §八（所有权体系）；Server 层维持 RID 禁用 Ref 不变 | ~0.5 天（随 Phase 6 方案） |
| 后处理独立子系统 | 从 RenderViewport 拆出（composite_shader/quad_vbo/scene_buffers 等 10+ 成员 → `ARHudCompositeRenderer`） | ~1 天 |
| `opengl32` PUBLIC → PRIVATE | `platform/CMakeLists.txt` 链接方式 | ~0.5 天 |
| 命名空间分层 | `arhud::render::opengl` / `arhud::platform::windows` 等 | ~1 天 |
| ~~窗口尺寸重建未完成（resize stub）~~ ✅ 已完成（2026-10-07） | 主线程 `HandleResize` 置原子 `resize_pending_/new_width_/new_height_` + 唤醒渲染线程；渲染线程每轮循环经 `ApplyPendingResize()` 消费：无在途帧时 `RenderBuffers::Recreate` + `FrameImageQueue::Resize` 重建 FBO/帧队列，并按新颜色纹理重绑后处理 uniform set；有在途帧则保留 pending，待 present 释放槽位后下轮重试（2026-10-02 登记 → 2026-10-07 修复） |

### P3（低优，工程打磨）

| 项 | 内容 |
|---|---|
| ~~README 重写~~ ✅ 已完成（2026-09-27） | 目录树（platform/drivers/servers/app）与构建选项默认值（`ARHUD_BUILD_TESTS`/`ARHUD_BUILD_DEMO`）已对齐 AGENTS.md |
| **ref/weak_ref 启用前补 core 单测** | `Ref/RefCountBlock/WeakRef` 的 CAS 循环（TryAddStrongRef）、隐式弱引用时序、跨线程 Lock 从未被编译验证（0 使用 = 0 验证）；Phase 6 / Facade 启用 Ref 前必须先补 tests/core 用例清零风险（2026-09-19 登记随所有权体系记录，见 `arhud-architecture` §八） |
| **Free 不置 dirty（潜在陈旧帧）** | `ItemStorage::CanvasItemFree/SpatialItemFree/TextItemFree` 删除（含 ADR-0005 树形摘除）后不置 dirty——若无其他变更，屏上残留被删内容直到下次 dirty。既有设计（注释明示"删除无需触发重渲"），但树形摘除会产生可见状态跳变（子项跳回根位置），运行期动态删 item 场景需评估改为 MarkScreens（置屏位）（2026-09-19 审视登记） |
| ~~skill/arhud-architecture 滞后修正~~ ✅ 已完成（2026-09-27） | §3.2 FrameScene 行已删；§五 测试描述改 25 用例（command_queue 6 + item_storage 19）；`arhud-arch-governance` 幻影引用已在 4 份 skill 改为指向根 `AGENTS.md` + `docs/process` |
| ~~arhud_engine.h 类体文档滞后~~ ✅ 已完成（2026-09-27） | `Run()`/`EndFrame()` Doxygen 的 SwapBuffers 已改述为 present 线程上屏（IFramePresenter）；`IAppLoop` 示例/`@note` 的 `Compositor->CanvasItemPush()` 已改述为经 RenderServer 发命令 |
| ~~demo main.cpp 乱码注释清理~~ ✅ 已完成（2026-09-27） | 4 个 demo 共 **53 处** `?` 乱码注释已按代码语义重写（原文因无 git 历史不可考，属**重建**）；治理 ① 注释计数不变（命中的是渲染内部名的**正常注释提及**，非乱码） |
| VRAM 估算改进 | 考虑 mipmap/压缩/MSAA 因子 |
| 缓存 key 碰撞校验 | `ShaderSourceKey`/`PipelineCacheKey` 存原始内容比对（不只 hash） |
| ~~`CmdBufferUpdate` 深拷贝~~ ✅ 已完成（2026-09-27） | 实际采用**命令 payload 区 bump 分配**（非字面"引用 Staging Buffer"——后者会引入悬垂指针，见 devlog）；去逐条堆分配/析构 |
| **外部重绘精细化：窗口→屏索引映射** | ADR-0008 落地为「任一窗口重绘请求 → `MarkAllScreens`（全屏）」；多屏下可细化为仅标该窗口对应的屏（2026-10-02 登记） |

> **流程基建（2026-09-19）**：正规文档体系已建立——`docs/README.md`（文档地图）+ `docs/process/`
>（dev-workflow 变更分级/生命周期、verification 验证手册、definition-of-done）+ `docs/adr/`
>（0001~0004 种子决策）+ `docs/templates/`。历史决策追溯表见 `docs/adr/README.md`；
> **新决策一律走 ADR，不再散落 skill**。治理 grep 基线：①注释命中 7（乱码注释）/②③ 零代码命中。

---

## 四、已决策不做（避免重复纠结）

| 决策 | 结论 | 理由 |
|---|---|---|
| 加锁修延迟销毁（P0#1） | 不做 | B3 Free 搬渲染线程后自然修复，加帧数也解决不了 |
| frame_count_ 加帧数 | 不做 | 加帧数只是把撞上门槛推高，稳态总会达到 |
| 禁异常（/EHs-） | 已启用，不回退 | HUD 非硬实时，禁用是过度设计 |
| 禁 RTTI（/GR-） | 已启用，不回退 | 同异常 |
| `/EHs-` → `/EHs-c-` | 不改 | 引擎代码零 throw，行为无差异 |
| 合并 CameraMatrix + Transform2D | 不合 | 参数集不重叠，Godot 也不合并 |
| 多屏渲染：每屏独立渲染线程 | 不做 | 无隔离收益（同进程同地址空间）+ 驱动锁竞争；HUD 负载单线程足够（2026-08-23 评审）。注：present 线程不是渲染线程（无 GL），不冲突 |
| 多屏渲染：多进程 | 不做 | 当前无故障隔离硬需求；未来有安全需求时再评估 |
| 多屏结构：每屏一套完整栈 | 不做 | VRAM ×N、RID 不跨屏；按 Godot 单共享栈实施（`arhud-multiscreen-arch`） |
| 多屏 GL：单 context + drawable 切换 + 批量 swap | 不做 | SwapBuffers vsync 同步阻塞串行化多屏 vblank 等待（§0.3 of multiscreen）；渲染/上屏分离替代（2026-08-24） |
| 多屏 GL：per-screen context + wglShareLists | 不做 | VAO/FBO 跨上下文需 RDD 键控改造；present 线程无 GL 需求，共享失去意义（2026-08-24） |

---

## 五、建议执行顺序

```
B2（1天，立即模式 RenderServer+FrameScene）✅
  ↓
B2-R（2天，保留模式重构--ItemStorage + 增量命令）✅ 2026-08-23
  ↓
B3-M0.1（1天，共享栈上提 RenderViewport→Engine）✅ 2026-08-24
  ↓
B3-M0.2（1天，Config 拆分 + Initialize/AddScreen 分离）✅ 2026-08-25
  ↓
B3-M0.3~.4（2天，离屏 context + FBO 导出 + IFramePipeline + ItemStorage 全局化）✅ 2026-08-25
  ↓
B3（3天，渲染搬线程 + present 线程，Engine 级单渲染线程 + per-screen 平台上屏）✅ 2026-09-03
  ↓
B3-P（1天，呈现调度：dirty bit + buffer self-pacing 验证 + present 时序日志 + DMA-BUF 零拷贝落地）
  ↓
B4（1天，Storage 收敛）
  ↓
B5（1天，清理优化）
  │
  ├─ ✅ P1 ShaderCreateFromGLSL 改名（2026-09-27 完成，~0.5天）
  ├─ P2 各项（B5 之后集中，~3.5天）
  └─ P3 各项（最后，~1.5天）
```

**Route B 剩余**：~6 天（B3 3 + B3-P 1 + B4 1 + B5 1）
**治理 P1**：~0.5 天
**治理 P2**：~3.5 天
**治理 P3**：~1.5 天
**总计**：~11.5 天

---

## 六、下一步

**B3 ✅ 完成（2026-09-03）→ 下一步 B3-P（呈现调度）**。

- **B3-M0 ✅ 全部完成**（2026-08-24~25）：
  - M0.1 共享栈上提 / M0.2 Config 拆分 + Initialize/AddScreen 分离 /
    M0.3 离屏 context + FBO 导出 + GDI 上屏 / M0.4 ItemStorage 全局化 + cull_mask + 删 FrameScene
- **B3 ✅ 已完成**（2026-09-03）：渲染搬线程 + present 线程 + FrameImageQueue 状态机
  （free-after-flip + buffer 令牌 self-pacing + dirty bit）
- 验收：4 demo 单屏视觉不变 + 双窗口 demo（cull mask 区分内容、共享 RID）

之后 **B3**（渲染搬线程 + present 线程，Engine 级单渲染线程渲 FBO 导出 + per-screen
present 线程经 IFramePipeline 交 buffer 给 compositor，release 事件驱动 free-after-flip，
buffer 令牌 self-pacing）→ **B3-P**（dirty bit 内容更新率 + buffer self-pacing 验证 +
present 时序日志 + QNX/Linux DMA-BUF 零拷贝落地）。
