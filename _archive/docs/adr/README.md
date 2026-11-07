# 架构决策记录（ADR）索引

> ADR 记录"**为什么这么做**"的一次性决策（含否决方案）；`skill/` 记录"**现在是什么样**"的持续状态。二者互链，不重复正文。

---

## 使用规则

1. **何时写 ADR**：接口契约变更、线程模型调整、分层边界与依赖方向、资源生命周期策略、平台/后端策略、影响重大的否决决定。A/B 类日常变更不写。
2. **编号**：三位递增（0001 起），只增不改；被取代时旧 ADR 状态改"被 ADR-MMMM 取代"并指向新文件。
3. **状态机**：`草案 → 已接受 →（已废弃 | 被 ADR-MMMM 取代）`
4. **模板**：`docs/templates/adr-template.md`

## 索引

| 编号 | 标题 | 状态 | 日期 |
|---|---|---|---|
| [0001](0001-route-b-render-thread.md) | Route B：渲染搬线程 + 保留模式 RenderServer + SPSC CommandQueue | 已接受 | 2026-08-22 决策 / 2026-09-03 落地 |
| [0002](0002-multi-screen-shared-stack.md) | 多屏：Engine 级单共享栈 + 单渲染线程 + 轻量视口 | 已接受 | 2026-08-23（B3-M0 2026-08-24~25 落地） |
| [0003](0003-render-present-split.md) | 渲染/上屏分离：离屏单 GL context + FBO 导出 + present 线程 | 已接受 | 2026-08-24（B3-M0.3/B3 落地） |
| [0004](0004-exceptions-rtti-enabled.md) | 启用异常与 RTTI（移除过度禁用） | 已接受 | 2026-08（追溯记录） |
| [0005](0005-item-storage-tree.md) | ItemStorage 树形管理（canvas/spatial/text item 层级 + z_as_relative） | 已接受 | 2026-09-19 当日落地 |
| [0006](0006-camera-command-params.md) | 相机命令载荷参数化（对齐 CameraMatrix 惰性重算） | 已接受 | 2026-09-27 决策 / 2026-10-01 落地（与 0007 同窗口） |
| [0007](0007-per-viewport-scene-state.md) | per-viewport 场景状态归属（相机/灯光迁出 ItemStorage） | 已接受 | 2026-10-01 决策并落地（方案 C） |
| [0008](0008-external-repaint-force-redraw.md) | 外部重绘请求 → 强制全屏重渲（dirty 门的第二来源） | 已接受 | 2026-10-02 决策并落地 |
| [0009](0009-command-self-dispatch.md) | 命令分派改为 POD 自派发（替代集中 switch） | 已接受 | 2026-10-02 决策；M1（机制+视口域）当日落地 |
| [0010](0010-render-globals-singleton.md) | 命令绑定 (目标, 方法) + 渲染存储 RSG 式全局可达（对齐 Godot） | 已接受 | 2026-10-02 决策 / 2026-10-07 裁决接受（M1 落地，M2~M3 待实施） |

## 历史决策追溯表（ADR 制度建立前，权威记录在 skill/AGENTS.md）

| 决策 | 日期 | 权威记录 |
|---|---|---|
| 契约归位：RCD/RDD/CommandBuffer 契约头住 `drivers/` 根 | 2026-08-30 | 根 `AGENTS.md`「契约归位」 |
| 保留模式（ItemStorage 增量命令）替代立即模式（FrameScene 双缓冲） | 2026-08-23 | ADR-0001 §2 |
| Camera3D→CameraMatrix 重构（core/math/）、删 Camera2D（用 Transform2D）、2D 投影内联 | 2026-08 | `skill/arhud-architecture` §四 |
| dirty bit 内容更新率 + buffer 令牌 self-pacing（退役 decimation/clock_screen） | 2026-09-03 | ADR-0003 §2 / `skill/arhud-multiscreen-arch` §3.7-3.8 |
| CommandExecutor 独立于 CommandQueue（职责分离，便于单测 mock） | 2026-08-22 | `skill/arhud-render-thread-arch` §3.3 |
| CMake 显式源文件列表，禁 `file(GLOB)` | 初版即定 | 根 `AGENTS.md`「Adding new source files」 |

> **自本索引建立起，新决策一律走 `docs/adr/`，不再散落 skill 正文。**
