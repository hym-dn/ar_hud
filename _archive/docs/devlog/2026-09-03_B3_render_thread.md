# 开发日志：B3 阶段完成 —— 渲染搬线程 + present 线程

- 日期：2026-09-03
- 范围：servers/rendering（engine/server/executor/screen/queue/storage）+ app/ 4 demo
- 依据：skill/arhud-render-thread-arch §2.6 + skill/arhud-multiscreen-arch §3.5/3.7/3.8

## 1. 变更总览

B3-M0 已就位的基础设施（FrameImageQueue/PresentThread/离屏 ctx）之上，完成
"渲染决策搬线程"的六项核心工作：

| 项 | 落点 |
|---|---|
| kSubmitScene 全局信号 | SubmitSceneCmd（16B 纯信号）→ RenderServer::SubmitScene()（异步 Push）→ Executor 置位 submit_pending_ |
| 渲染线程主循环改形 | RenderThreadFunc：render_wake_ 三源唤醒 → Flush → 消费 submit → rd.BeginFrame → 逐屏 RenderAndSubmit → rd.EndFrame |
| RD 帧簿记搬线程 | Begin/EndFrame 从主线程挪到渲染线程；SetDeferredExecution(false)（同线程录+立即执行） |
| ExecuteFrame 退役 | 渲染线程不再回放预录 CommandBuffer；main_to_render_ 信号量删除 |
| dirty bit（§3.7） | ItemStorage 29 个变更点置位；RenderAndSubmit 消费清零；静态内容零成本跳过 |
| item 命令异步化 | 35 处 PushAndSync → Push（Create 保留同步）；MaterialSetUniform/SetTexture 命令化（version 竞态根治） |

## 2. 关键实现决策

1. **submit 信号注入链**：Engine Impl 持 atomic submit_pending_，构造时注入
   CommandExecutor（Initialize 第三参数）——kSubmitScene handler 置位、
   渲染循环 exchange 消费。flush 顺序天然保证"本命令前的全部 Set*/Create
   已执行"（ItemStorage 状态就绪才渲染）。
2. **唤醒拓扑修正**：FrameImageQueue 的 PostRenderWake/Release 原本只 post
   本地 free_sem_，渲染线程改等 Engine 级 render_wake_ 后存在唤醒断链
   （首帧死锁）。修复：Queue 增 SetWakeSemaphore，释放/启动事件双 post
   （free_sem_ 保持槽位计数语义，wake_sem_ 是唯一线程唤醒通道）。
3. **RD 立即模式**：原 deferred_execution=true 下 frame_driver_cmd_ 由主线程
   Begin/EndFrame 持有，渲染线程 DrawListBegin 直接报"no frame command
   buffer"。切立即模式 + 帧簿记随循环搬入渲染线程后消解——这正是
   render-thread-arch §2.6 预言的"GL 路径：渲染线程录 + 立即执行"。
4. **MaterialSetUniform/Texture 命令化**：原直调路径是 MaterialStorage 跨线程
   写（version 竞态/潜在锁死锁——B3 计划内"顺带修"项），改走既有
   kMaterialSetUniform/kMaterialSetTexture 命令（Exec 已存在）。

## 3. 排障记录（值得留档的两坑）

- **批量正则改编码事故**：PowerShell Set-Content 对 UTF-8 中文文件二次编码
  产生 GBK mojibake（render_server.cpp 全文件注释损坏）。恢复：UTF8→GBK
  round-trip 反解 + 注释块按行重建（代码行无损，45 方法/130 对花括号完好）。
  教训：中文工程文件批量替换用 [System.IO.File]::WriteAllText + 显式 UTF8。
- **"卡死"误判**：canvas demo 6000 帧 × 16ms ≈ 96s，测试超时 30-40s 判 STUCK。
  B3DBG 插桩（write/read 指针、唤醒计数、心跳）证明主循环/渲染线程/present
  全部健康，presented=1 是 dirty bit 正确语义（内容静态=不重渲，§3.7 兑现）。
  115s 完整运行干净退出。教训：验收超时须 ≥ 帧数 × 帧时长。

## 4. 验收结果

- 全量构建绿（5 lib + 5 exe + tests）；单测 6/6
- 4 demo 干净退出、零 ERROR、呈现正确（dirty 语义：内容不变时 presented 停在 1）
- grep app/：GetStorage/GetRenderer/RenderFromStorage/GetCompositor 零代码命中
- SubmitScene 立即返回（纯异步 Push，无等待）

## 5. 遗留（B5 清理清单新增）

- ~~RD::ExecuteFrame/OnFrameCompleted 无调用方（函数体待删）~~ ✅ 当日已删
- ~~RD BeginFrame 的 deferred 等待路径失效（frame_slot_cv_ 可删）~~ ✅ 当日已删
- RenderScreen::current_slot_ 闲置、BeginFrame/EndFrame 公有方法待审
- GLCommandBuffer 的跨线程 state_ atomic 可降级（B3 后同线程录放——
  前次讨论的"三件套"整体退役项）

## 6. 追加变更（同日）：第一档死代码删除

B3 遗留清单前两项当场清理（"跨线程交接协议"三件套）：

- **删除**：`RenderingDevice::ExecuteFrame()/OnFrameCompleted()` 函数体与声明
  （零调用方）；`SetDeferredExecution()` + `deferred_execution_` 成员；
  `frame_slot_mutex_` + `frame_slot_cv_`（唯一 wait/notify 均在死分支）；
  `CommandBuffer::ready` atomic 字段
- **顺带**：EndFrame 文档更新为"B3 起渲染线程同线程录+立即执行"（原双模式
  叙述删除）；engine 中 `SetDeferredExecution(false)` 调用点移除
- **保留**：`pool_state`/`frame_used`（BeginFrame 断言 + 池簿记仍在用）；
  GLCommandBuffer 内 22 处 atomic（第二档——GetState 公共契约 + Finalize
  窗口防御，B5 统一降级）
- 验证：全量构建绿 + 单测 6/6 + 4 demo + phase2 全部干净退出零 ERROR
- 调试经验：`?` 乱码注释在 4 个 demo main.cpp 中遗留（历史编码事故产物，
  建议下次触碰时顺手清理）
