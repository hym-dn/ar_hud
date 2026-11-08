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
| 编码风格（Google 原教裁决） | ✅ 2026-11-07 全库 G1~G6 落地（110 文件，clang-format 幂等零违例）；W1 `p_` 前缀 / W2 中文注释**随改动收敛**，不批量强改 |
| demo 端到端出图 | ✅ 2026-11-08 五路全验（phase2/canvas/spatial/compositor/text，DPI-aware 截图 + 引擎像素 dump 双确认） |
| 测试 | ✅ ctest 34/34 绿（command_queue / item_storage / camera_routing） |
| 构建自洽 | ✅ clone 即 `build.bat conan/tests` 可全量构建 |
| 文档体系 | ✅ 2026-11-08 收敛为 4 份：AGENTS.md（导航）+ design + coding-style + progress（本文）；旧体系删除，历史在 git |
| 远端同步 | ✅ origin/main 已推送至 `bd3b4c1` |

### 2.3 近期修复线（渲染链路，2026-11-08 一天内闭环）

全部 ✅，明细见 §3。当前无 ⛔ 项。

## 3. 已完成历史（append-only，新条目加在最上面）

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
- **生产平台呈现**：QNX/Linux/Android 走 EGLImage/DMA-BUF + release-event（Mode S 生产形态，替换 Windows GDI 回读）。
- **性能**：PBO 异步回读（现同步回读 ~1-2ms/屏 dev 税）；canvas 批处理合并优化；HUD 静态场景零重渲验证。
- **风格残留收敛**：W1 `p_` 前缀、W2 中文注释——随日常改动自然消化（coding-style §1.3）。
- **测试扩展**：RD/Storage 层单测（现仅 command_queue/item_storage/camera_routing 3 文件 34 用例）；NullDriver 集成测试（tests/CMakeLists 注释预留）。

## 5. 已知风险与注意

- 门面与旧 `RenderServer` **并存**是刻意过渡态——任何人"顺手清理旧代码"前先查本文 §2.1 阶段状态。
- `Ref<Buffer>` 改造触及 §7.11 冻结签名（E/D3 条目）——按冻结门槛：先改 design skill，经确认再动代码。
- 窗口级验证务必 DPI-aware（200% 缩放下非感知坐标会产生位置伪影，本会话曾三次误判）。
