---
name: "arhud-progress"
description: "ARHud 工程进展与路线图的唯一动态账本：当前位置、短/中/长期计划、已完成历史（append-only）、已知残留与 backlog。凡提交里程碑、开始/结束任务、调整计划时，在同一提交内更新本文。阶段定义与验收标准以 arhud-design §7.12 为准，本文只记'走到哪了'。"
---

# ARHud 工程进展与路线图（实时账本）

> **定位**：本文回答"现在在哪、接下来做什么、做过什么"。
> 结构与契约问题看 `arhud-design`；风格问题看 `arhud-coding-style`；本文只管**进度**。
> 最后更新：**2026-11-08**。

## 0. 更新协议（本 skill 的"实时"约定）

| 触发点 | 动作 |
|---|---|
| 完成一个里程碑/修复批次 | §2 当前状态改行 + §3 历史追加一条（带日期与提交号） |
| 开始/接手一项任务 | §2 标 ⏳（写一句"当前任务"） |
| 计划调整（增删阶段、改优先级） | §2/§3 同步 + 若涉及阶段**定义**变化，先改 design skill §7.12 再改本文 |
| 被阻塞 | §2 标 ⛔ 并写原因与解阻条件 |

标记：✅（带完成日期）/ ⏳（进行中）/ ☐（未开始）/ ⛔（阻塞）。
规则：§3 **append-only 不改写**；§2 允许覆盖刷新；冲突时——阶段定义以 design skill 为准，进度状态以本文为准。

## 1. 一句话现状（2026-11-08）

Godot 对齐绞杀者迁移 **S0 + S1 大部完成**；全库 Google 原教风格迁移完成；
渲染链路五路 demo **端到端出图全部验证通过**；工程处于 **S1 收尾（Ref\<Buffer\> 参数体系）→ S2（RSG 全局）** 的交界点。

## 2. 当前状态表

### 2.1 绞杀者迁移阶段（定义见 design skill §7.12）

| 阶段 | 状态 | 备注 |
|---|---|---|
| S0 门面 | ✅ 2026-10 | `RenderingServer` 抽象 + `RenderingServerDefault` 委托门面；生命周期方法仍 stage-gated 占位，S1 接管 |
| S1 队列 | ⏳ **当前任务** | M1~M3 ✅、S1-GL 录制层退役 ✅、H1 `*CreateParams` ✅；**余：`Ref<Buffer>` 参数体系**（替换纹理/字体像素的裸指针+size 过渡签名；纯类型美化，跨线程安全已由 blob 同步拷贝保证） |
| S2 全局（RSG） | ☐ 下一步 | `RenderingServerGlobals` 替换 `RenderGlobals`；顺带把 S0 门面占位的 Init/Draw/Sync 接管做实 |
| S3 资源 Storage 更名 | ☐ | `Renderer*Storage`；light 迁入 `RendererLightStorage` |
| S4 item 拆分 | ☐ | ItemStorage → CanvasCull / SceneCull / SceneRender；层级与脏位随迁 |
| S5 TextServer | ☐ | FontStorage + TextItem 并入；顺带根治 shader 变体系统（VARIANT_DEPTH_PASS 类问题） |
| S6 RendererViewport | ☐ | 改名 + `viewport_provider` 并入 RSG::viewport |
| S7 收口 | ☐ | 删旧 `RenderServer`（判定：grep 无命中）；`arhud_engine` 降级为主循环；demo/test 适配 |

### 2.2 非迁移线工程状态

| 事项 | 状态 |
|---|---|
| 编码风格（Google 原教裁决） | ✅ 2026-11-07 全库 G1~G6 落地（110 文件，clang-format 幂等零违例）；W1 `p_` 前缀随改动收敛；**注释体系 2026-11-08 修订为 Doxygen 中文**（§7），存量英文注释滚动收敛 |
| demo 端到端出图 | ✅ 2026-11-08 五路全验（phase2/canvas/spatial/compositor/text，DPI-aware 截图 + 引擎像素 dump 双确认）；同日 Mode X 落地后四路复验通过（拆卸干净、零回读） |
| 呈现路径 | ✅ 2026-11-08 **Mode X 共享显存零回读**（Win32：D3D11 帧环 + DXGI flip-model + WGL_NV_DX_interop2）；Mode R 保留为兜底；`ARHUD_FORCE_READBACK` / `ARHUD_DEBUG_SAMPLE` 可 A/B |
| 测试 | ✅ ctest 42/42 绿（command_queue / item_storage / camera_routing / **rid_owner**） |
| 构建自洽 | ✅ clone 即 `build.bat conan/tests` 可全量构建 |
| 文档体系 | ✅ 2026-11-08 收敛为 4 份：AGENTS.md（导航）+ design + coding-style + progress（本文）；旧体系删除，历史在 git |
| 远端同步 | ✅ origin/main 已推送至 `bd3b4c1` |

### 2.3 近期修复线（渲染链路，2026-11-08 一天内闭环）

全部 ✅，明细见 §3。当前无 ⛔ 项。

## 3. 已完成历史（append-only，新条目加在最上面）

- **2026-11-08 窗口 resize 破坏 RD 对象的缺陷修复（三处根因，含 RID 地基修正）**：
  上条遗留的 ⛔ 已解除。三处独立根因逐一坐实并修复：
  ① **尺寸双源** —— `BeginFrame` 轮询"首屏窗口宽高"与 `WM_SIZE` 回调并行投递
    resize，且轮询只认 `screens_[0]`；删轮询，`WM_SIZE` 成为唯一权威（客户区），
    新增 `screen_window_ids_` 按窗口路由到对应屏；`AddScreen` 以**客户区实测值**
    一次性给池/上屏器/视口/队列四个对象定尺寸（`WS_OVERLAPPEDWINDOW` 下请求值≠客户区）。
  ② **RID 跨表不唯一** —— `MakeRid` 只编 (表内世代, **本地槽号**)，于是
    `buffer_owner_` 与 `texture_owner_` 会发出**同值** RID；`FreeDriverResourceByRid`
    逐表 `Owns()` 探测据此跨表误杀活对象。改为高 32 位 = `(表标识<<20)|世代`
    （`rid_detail::MakeTag`，inline 函数局部 static 保证进程内唯一）。
    顺带修掉 `MakeRid` 复用空闲槽的 **off-by-one**（free_list 存 1-based，
    旧代码写 `entries_[index]` 改写邻居槽）与 `Size()` 里 `entries_[i].valid_`
    的错字段名（非 Debug 分支不可编译，一直未被触发）。
    **新增 `tests/core/test_rid_owner.cpp`（8 用例）**钉住跨表唯一性与邻居安全；
    反向验证过：临时去掉表标识 → 3 个用例立刻变红。
  ③ **render_pass 随尺寸被误重建** —— `RenderBuffers::Recreate` 原为
    `Destroy + Create`，销毁 render_pass 会经 `DependencyAdd(render_pass→pipeline)`
    级联误杀 composite pipeline。改为只重建尺寸相关的纹理 + framebuffer、
    **保留 render_pass**（render_pass 本就不编码尺寸）；构造拆成
    `AllocAttachments/AllocRenderPass/AllocFramebuffer` 私有原语供 Create/Recreate 共用。
  **验证**：resize 期间 `invalid pipeline RID` 3858→0、`invalid buffer RID` 608→0、
  `invalid render pass` 454→0、`GLAD: ERROR`→0、`resize pending` 重试 259→3（三次真实
  变化全部应用，不收敛消失）；Mode X 连经 1000×600→1500×900→1280×720 三次重建，
  共享路径每次都重新就绪（`degraded=0`，未永久降级），自然跑完 6000 帧
  `shared=5833 / readback=0`；基线（`ARHUD_FORCE_READBACK=1`）同样归零。ctest 42/42 绿。
- **2026-11-08 Mode X 共享显存零回读上屏（Windows/GL）落地**：
  新增 `platform/shared_frame_pool.h`（`ISharedFramePool`，帧缓冲归显示侧）、
  `platform/gpu_interop.h`（`IGPUInterop`）+ Win32 实现
  `dxgi_shared_frame_pool_win32.*`（D3D11 帧环 3 + DXGI flip-model）、
  `gpu_interop_win32.*`（WGL_NV_DX_interop2 装载/别名/加解锁）；
  RDD/RD 增 `TextureCreateExternal` + `TextureExternalBegin/EndWrite` +
  `TextureFreeImmediate`（外部导入不能用帧槽延迟释放）；
  `RenderBuffers::CreateExternal`（外部颜色附件 + 无深度）、
  `FrameImageQueue` 增 `frame_token`、`PresentMode::kShared`、
  `RenderViewport` 双路提交 + `composite_override_`、Engine 装配与拆卸顺序。
  **量化验证**（1280×720 canvas demo 6000 帧）：Mode X `shared=5812 /
  readback=0 / 0 MB / 0.0 ms`；Mode R `readback=5424 / 19,068 MB / 26,865 ms`
  （≈4.95 ms/帧）。两路上屏帧平均亮度同分布（5–9/255）。ctest 34/34 绿。
  **踩实两个硬坑（已写进 design §5.5）**：① `Present1` 的
  `pPresentParameters` 传 `nullptr` 会在 `dxgi.dll` 内解引用 `NULL+0x10`
  直接 AV（头里是 `_In_` 不是 `_In_opt_`，文档说法不可信），必须传零值结构体；
  ② interop 纹理必须**先加锁再挂 FBO**，否则 `GL_FRAMEBUFFER_UNSUPPORTED`；
  另加 ③ 拆卸顺序：解除别名(screens) → `wglDXOpenDevice` 关闭(rd.Finalize)
  → 池释放，反序即 AV。
  遗留：resize 期间共享目标重建尚未验证通过（受下述既有缺陷阻塞）。
- **2026-11-08 注释裁决修订**：编码风格 §7 改为 **Doxygen 格式 + 中文**（工程对原教的明示修订，
  原教注释章节属项目自定范围）；文件头/类/函数/成员标签规范落地，AGENTS/design/账本同步；
  存量英文 `//` 注释（迁移期产物）随改动滚动收敛，不批量强改。
- **2026-11-08 `bd3b4c1`** Docs 设计文档收敛：architecture.md → `docs/skill/arhud-design/SKILL.md`；删除 `_archive/` 旧体系 50 件；引用全库同步。
- **2026-11-08 `c5ac51e`** Fix 首帧 dirty 竞态：`RenderAndSubmit` 返回 bool，引擎逐屏消费（`ClearScreenDirty(i)` 仅在真正渲完后），替换全局 `ClearDirty`；box 首渲恢复稳定 frame 2。
- **2026-11-08 `6dc8047`** Fix composite 致场景零像素：六操作二分定位到 composite pipeline bind 泄漏 `GL_DEPTH_WRITEMASK=FALSE` → 次帧 depth clear 被吞 → 深度滞留 0。双修：BeginSegment 不再每帧 Reset 状态缓存（同 context 缓存须如实）；`CommandBeginRenderPass` 清深度时临时强制 mask。
- **2026-11-08 `37ef5ca`** Fix spatial 三实修：UBO 元数据 `name=nullptr` 致 block 全塌 binding0（model 读成投影矩阵）；box/sphere 绕序反向（外视 CW 被全剔）；主 pass 深度 LESS→LEQUAL（prepass 等深冲突）。
- **2026-11-08 `effb5fb`** Fix 出图链路：FrameImageQueue `ready_index_` 消费后不清 → MarkReady 自覆盖 → present 饿死（黑屏主因）；后处理补 `post_buffers_` 输出 FBO，回读按开关选源。
- **2026-11-08 `910fa14`** Fix `MeshInitialize*` is_valid 时序自相矛盾（内部 MeshAddSurface 被自身 guard 拒绝，内置网格无 surface）。
- **2026-11-08 五路 demo 出图验证**：建立 DPI-aware 窗口截图 + 引擎像素 dump 冒烟方法（教训：窗口级验证必须 DPI-aware，引擎像素是 ground truth）。
- **2026-11-07 风格工程**：`75919c6` skill+`.clang-format` 落地 → `9277542` 72 untracked 文件批量迁移 → `e8517f5`/`235559a` tracked 存量重排 → 全库审计零违例；`e29225e` include 自动排序禁用裁决（psapi/windows.h 实案）。
- **2026-11-07 仓库清扫**：`ed86104` `.gitignore`（build/.conan/shader_cache/生成预设）→ `9a1e52d` 构建系统落库 → `60661b8` `.vscode` → `61eb290` 旧文档归档（后于 11-08 删除收敛）。
- **2026-10-11** S1-GL：`ICommandBuffer` 多态层与 GL 录制机制整层退役（DrawList 直调 `CommandXxx`）。
- **2026-10-07** S1-M1~M3：绑定命令 `PushBound/PushBoundBlob` 全量改写调用点；`CommandExecutor`/`RenderCommandType`/33 命令结构体删除（ADR-0010）。
- **2026-10** S0：`RenderingServer` 抽象门面落地，委托旧 `RenderServer`。

## 4. 未来计划

### 4.1 短期（下一批工作，按序）

1. **S1 收尾**：`Ref<Buffer>` 参数体系——`Buffer`（引用计数字节载体）+ `Ref<T>` 接入
   `TextureCreateParams.data` / font 像素 / blob 路径；替换裸指针+size 签名。
   验收：ctest 绿 + canvas/text demo 出图 + `grep 'const uint8\* p_data'` 门面签名清零。
2. **S2 RSG**：`RenderingServerGlobals` 平铺全局替换 `RenderGlobals`（grep 无 `RenderGlobals`）；
   同时把门面 `Init/Finish/Draw/Sync/HasChanged/FreeRid` 从占位做实（渲染线程主循环迁入 `RenderingServerDefault`）。
3. **CI 门禁**：`clang-format --dry-run -Werror` + `ctest` 进构建脚本/CI；demo 冒烟脚本沉淀到 `tools/`
   （本轮 DPI-aware 截图法已验证可用，待入库）。

### 4.2 中期（S3~S7，顺序执行，每阶段独立可编译可测）

S3 Storage 更名 → S4 ItemStorage 拆分（canvas/scene cull）→ S5 TextServer（含 shader 变体系统根治）→
S6 RendererViewport → S7 收口（删 `render_server.*`，判定 `grep -r "render_server_\."` 无命中）。

### 4.3 长期 / backlog（按需触发）

- **Vulkan 后端**：RDD 契约回补（`CommandBuffer`/`CommandPool`/`Queue`/`Fence`/`Semaphore`，design D11）+ RDG/依赖图（D8，§7.13 占位设计）。
- **生产平台呈现**：QNX/Linux/Android 接 `ISharedFramePool` + `IGPUInterop`
  的同一契约（`screen_buffer`+release event / GBM bo+flip event /
  AHardwareBuffer+queueBuffer）——Windows Mode X 已把接口形状验证过一遍
  （2026-11-08），换平台只是换实现。
- **性能**：~~PBO 异步回读~~ —— Mode X 已让回读退出主路径，PBO 只在
  回读兜底态下才值得做（优先级下调）；canvas 批处理合并优化；
  HUD 静态场景零重渲验证（本轮已见：静态内容下 6000 循环仅 3 帧真渲染）。
- **Mode X 待办**：① ~~resize 期间共享目标重建~~ ✅ 2026-11-08 已验证（三次重建均
  重新就绪、`degraded=0`）；
  ② 多屏实测（本机仅单屏验证，需确认 3 屏各一条上屏线程共用同一 D3D11
  device 的行为）；③ `CommitFrame` 的 `p_timeout_ms` 目前未用（归还保证落在
  interop Lock 上）——若实测有撕裂，改为在 present 侧等 GPU 消费完成；
  ④ CPU 背衬的 2×w×h×4 常驻内存可改为降级时再分配（当前刻意保留，换降级安全）。
- **测试扩展**：RD/Storage 层单测（现 command_queue/item_storage/camera_routing/rid_owner 共 4 文件 42 用例）；NullDriver 集成测试（tests/CMakeLists 注释预留）。

## 5. 已知风险与注意

- **✅ 窗口 resize 破坏 RD 对象的缺陷（2026-11-08 修复，明细见 §3）**：三处根因
  （尺寸双源 / RID 跨表不唯一 / render_pass 随尺寸误重建）全部坐实并修复，
  Mode X 与基线回读 resize 期间错误计数归零、不收敛消失、共享路径重建后不降级。
  回归由 `tests/core/test_rid_owner.cpp`（8 用例，含反向验证）+ resize harness 钉住。
- 门面与旧 `RenderServer` **并存**是刻意过渡态——任何人"顺手清理旧代码"前先查本文 §2.1 阶段状态。
- `Ref<Buffer>` 改造触及 §7.11 冻结签名（E/D3 条目）——按冻结门槛：先改 design skill，经确认再动代码。
- 窗口级验证务必 DPI-aware（200% 缩放下非感知坐标会产生位置伪影，本会话曾三次误判）。
- **Mode X 的成立前提尚未在多 GPU 上验证**：D3D11 device、GL context、
  目标窗口三者必须同适配器。本机（NVIDIA MX450）实测同卡通过；
  Intel 核显 / 混合显卡未实测，探测失败会静默回落 Mode R（看
  `PresentThread finalized` 与 `present path: shared=` 两行统计即可判定）。
- 本机 Windows SDK（10.0.26100）的 `d3d11.h` **未声明** `ID3D11DeviceContext`
  的 query 接口、`IDXGIAdapter` 的 `GetOutputCount`——写 DX 代码时不要
  依赖这些名字（已改为不依赖）。
