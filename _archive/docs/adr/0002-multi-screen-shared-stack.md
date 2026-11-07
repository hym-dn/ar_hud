# ADR-0002：多屏 —— Engine 级单共享栈 + 单渲染线程 + 轻量视口

| | |
|---|---|
| 状态 | 已接受（B3-M0.1~M0.4 2026-08-24~25 落地） |
| 日期 | 2026-08-23（评审决策） |
| 关联 | `skill/arhud-multiscreen-arch`（设计全量）、ADR-0001、ADR-0003 |

## 1. 背景与问题

B3-M0 前的结构：每个 RenderScreen 按值持有**一整套渲染栈**
（rd_ / command_queue_ / executor_ / item_storage_ / render_server_ / compositor_）+ 独立渲染线程。
N 屏 = N 套栈，带来：

1. 同一字体/图标 VRAM 上传 ×N（GL 上下文不共享，RID 不跨屏）
2. `GetRenderServer()` 只暴露屏 0，跨屏共享资源在 API 层不可能
3. 若先做 B3 渲染搬线程，搬移动作要对 N 套栈重复执行
4. 与 Godot 多窗口策略（一套全局渲染栈服务 N 个窗口）相悖

约束：HUD 负载轻（单屏 2~5ms），GPU 远未到瓶颈；车机无故障隔离硬需求（同进程同地址空间）。

## 2. 决策

**按 Godot 多窗口策略实施：一套全局渲染栈服务 N 个屏幕。**

1. **共享栈上提 Engine**：RD / CommandQueue / CommandExecutor / ItemStorage / RenderServer / Compositor
   全部 ×1，归 `ARHudEngine::Impl` 所有；RenderScreen 瘦身为轻量视口（收 `EngineStackRefs` 指针）
2. **RID 全局**：next_rid_ 全局分配，跨屏 RID 引用天然成立
3. **可见性 = cull mask**（对齐 Godot）：item `visibility_mask`（默认全 1）& viewport `cull_mask`（默认全 1）≠ 0 才渲染
4. **相机/灯光 per-screen**：ItemStorage 内 per-screen 槽（对齐 Godot 相机属 viewport），API 带 ScreenID 重载
5. **单渲染线程**：顺序驱动所有屏（引擎级循环），先上提后搬线程（避免 N 次重复重构）

## 3. 已否决方案

| 方案 | 否决理由 |
|---|---|
| 每屏独立渲染线程（进程内） | 无故障隔离收益（同地址空间一处崩溃全灭）+ 多线程同 GPU 提交引入驱动锁竞争；HUD 负载单线程足够 |
| 多进程渲染 | 当前无故障隔离硬需求；未来有安全需求再评估 |
| 保持每屏一套栈 | VRAM ×N、RID 不跨屏、搬线程动作重复 N 次（§1） |

## 4. 后果

- **正面**：VRAM 单份、RID 全局共享、B3 搬线程只做一次、与 Godot 结构同构便于对照维护
- **负面/代价**：命令路由需区分全局命令与 viewport 命令（相机/灯光/cull_mask 带 ScreenID）；
  RenderScreen 瘦身改造涉及全部 demo 迁移（已完成）
- **中性约束**：present 线程不是渲染线程（无 GL），与本决策"单渲染线程"不冲突（见 ADR-0003）

## 5. 验收

- 4 demo 单屏视觉不变（visibility_mask/cull_mask 默认全匹配、per-screen 相机默认 screen 0）✅
- 编译全绿 + 单测 6/6 ✅
- 双窗口 demo（cull mask 区分内容、共享 RID）待补（登记于 `skill/arhud-improvement`）
