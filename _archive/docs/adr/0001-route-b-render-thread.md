# ADR-0001：Route B —— 渲染搬线程 + 保留模式 RenderServer + SPSC CommandQueue

| | |
|---|---|
| 状态 | 已接受（B1/B2/B2-R 2026-08-22~23 落地；B3 2026-09-03 落地完成） |
| 日期 | 2026-08-22（决策）/ 2026-09-03（B3 完成） |
| 关联 | `skill/arhud-render-thread-arch`（设计全量）、`docs/devlog/2026-09-03_B3_render_thread.md`、ADR-0002、ADR-0003 |

## 1. 背景与问题

路线 A 下：主线程做渲染"前半段"（剔除/排序/批处理 + 录制 CommandBuffer），渲染线程只是回放器——
同一份信息被序列化两次（高层意图 → GPU 命令 → 字节流回放），主线程与渲染线程**不是真并行**。
此外存在三个结构性缺陷：

1. **SyncFlag 栈分配悬垂（P0）**：同步命令的完成标志放栈上，命令在队列里但 flag 已析构
2. **`MaterialData::version` 跨线程竞态**：主线程直调变更、渲染线程读
3. **立即模式低效**：每帧 BeginScene 重建整个 FrameScene 提交（几 KB/帧），HUD 静态元素重复发送，Phase 6 场景树无法增量衔接

约束：对齐 Godot 4.x RenderingServer + CommandQueueMT 线程模型；车规无动态分配偏好（热路径稳态零分配）。

## 2. 决策

**主线程只发高层语义命令，渲染线程执行全部渲染工作（Storage/Renderer/RD/GL 渲染线程独占）。**

1. **RenderServer = 主线程 API 代理（序列化器）**：app 唯一资源入口，自身不持有 GPU 资源；
   `CanBypassQueue` 短路（无消费者/调用者即渲染线程/已关闭 → 直调 Storage）
2. **CommandQueue 重构为 SPSC 序列化环形缓冲**：定长 1MB（**必须堆分配**）、读写指针原子化、
   变长数据经 span 内联、背压自旋（禁止静默丢弃）；**每线程信号量替代栈 SyncFlag**（P0 根治）
3. **CommandExecutor 独立组件**：CommandQueue 只做传输，Executor 反序列化分发（职责分离，便于单测 mock）
4. **保留模式（B2-R）**：item 状态存服务端 ItemStorage（渲染线程独占），增量 Set* 命令（~32B delta），
   `SubmitScene()` 纯信号；立即模式 FrameScene 双缓冲废弃（B3-M0.4 删除）
5. **同步策略**：资源创建类命令 `PushAndSync`（主线程需立即拿 RID）；item 命令 B2 过渡期同步
   （渲染在主线程）、B3 后切异步 Push（读写同线程）；`MaterialSetUniform/SetTexture` B3 命令化（version 竞态根治）

## 3. 已否决方案

| 方案 | 否决理由 |
|---|---|
| 保持路线 A（渲染线程仅回放） | 双路径序列化空转、非真并行、P0 需另行修 |
| 加锁修延迟销毁/SyncFlag | 生命周期悬垂是结构问题，锁只是推高门槛；信号量根治 |
| 立即模式 + FrameScene 双缓冲长期保留 | 每帧全量重建；Scene Tree 需增量衔接；带宽浪费 |
| item RID 每次创建走命令往返 | 创建集中 init 期可接受，但异步 Push 是真并行关键；RID 预分配列 backlog |

## 4. 后果

- **正面**：逻辑 ∥ 渲染真并行；P0/竞态根治；静态 HUD 零命令开销；Phase 6 天然衔接
- **负面/代价**：创建类命令同步阻塞主线程（init 期集中，可接受）；过渡期（B2~B3）PushAndSync 语义复杂（已消除）
- **中性约束**：线程归属铁律——主线程零直触 storage/renderer；app 层 grep 治理制度化（`verification.md` §5）

## 5. 验收

- `grep -rn "GetStorage|GetRenderer|RenderFromStorage" app/` 零命中 ✅（2026-09-03 验证）
- 主线程 `SubmitScene()` 立即返回（纯异步 Push）✅
- 单测 Shutdown P0 回归绿（6/6）✅；4 demo 干净退出 ✅
