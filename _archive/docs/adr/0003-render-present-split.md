# ADR-0003：渲染/上屏分离 —— 离屏单 GL context + FBO 导出 + present 线程

| | |
|---|---|
| 状态 | 已接受（B3-M0.3 离屏/GDI 路径 2026-08-25 落地；B3 present 线程 + self-pacing + dirty bit 2026-09-03 落地） |
| 日期 | 2026-08-24（评审决策，替代原"批量 swap"设计） |
| 关联 | `skill/arhud-multiscreen-arch` §0.3/§3.1/§3.5/§3.6/§3.8/§3.9、`docs/devlog/2026-09-03_B3_render_thread.md`、ADR-0002 |

## 1. 背景与问题

GL `SwapBuffers` 在 vsync 开启时**同步阻塞调用线程直到 vblank**。若单渲染线程渲染完所有屏后
批量 swap，非 genlock 屏间（独立时钟域、相位任意漂移）的 vblank 等待被**串行化**：
双屏异相 8ms 时 render 6ms + swap A 等 ~10ms + swap B 等 ~8ms ≈ 24ms，而理论 wall time 16ms。
备选方案 per-screen context + wglShareLists 又引入 VAO/FBO 跨上下文问题，需 RDD 键控改造。

## 2. 决策

**绘制与上屏彻底分离：渲染线程永不调 SwapBuffers；上屏由每屏一个无 GL 的 present 线程执行。**

1. **离屏单 GL context**：渲染线程独占唯一 context（绑离屏 surface），全部渲染输出 per-screen FBO；
   上屏窗口**不设 GL 像素格式**（纯 DWM/GDI 窗口——GDI 与双缓冲 GL 像素格式混用行为未定义）
2. **IFramePipeline 平台帧收发抽象（每屏一个）**：渲染侧 `ExportFBO`（Windows：glReadPixels ~1-2ms 仅
   dev 路径；QNX/Linux/Android 生产路径：EGLImage 直写 DMA-BUF/AHardwareBuffer 零拷贝）+
   present 侧 `Commit`（`StretchDIBits` / `screen_post_window` / `wl_surface.commit` / `ANativeWindow`）；
   fence（sync_file fd）随 Commit 交 compositor，防扫到半写 buffer
3. **FrameImageQueue 状态机**：`FREE→WRITING→READY→COMMITTED→ON_SCREEN→FREE`；
   **release 事件才置 FREE**（frame callback 只是"帧翻入正在扫出"，绝不可用于释放——P0-1 铁律）
4. **buffer 令牌 self-pacing**：FREE 只在 compositor release 后出现 → 每屏 buffer 每显示周期释放一个
   → 渲染线程每显示周期恰好拿到一次渲染机会；`HasInFlight()` 防渲染超前（每屏最多一帧在途 = 精确 1 帧延迟）。
   **无需计时器/decimation/clock_screen 配置**——C=90Hz/B=60Hz/A=30Hz 各屏自动按显示频率渲染
5. **唤醒三源（P0-2 防死锁）**：present release post / CommandQueue push + SubmitScene post / 启动 pre-post
6. **dirty bit（内容更新率）**：ItemStorage 变更点置位、渲染消费清零；静态 HUD 渲染成本 ≈ 0。
   面板刷新率 / 渲染节奏 / 内容更新率三个频率彻底解耦

## 3. 已否决方案

| 方案 | 否决理由 |
|---|---|
| 单 context + drawable 切换 + 批量 swap | SwapBuffers vsync 同步阻塞串行化多屏 vblank 等待（§1 计算） |
| per-screen context + wglShareLists | VAO/FBO 不跨上下文需 RDD 键控改造；present 线程本无 GL 需求，共享失去意义 |
| frame callback 释放 buffer | 会覆写扫出中的画面（规则性撕裂）；释放只听 release 事件 |
| decimation/clock_screen 渲染调度 | pull 模型下冗余且引入统计抖动；self-pacing 天然按各自显示频率保序 |
| 每帧重建 EGLImage | 打不动 90Hz；init 时一次性建 EGLImage+纹理缓存，每帧只切 FBO 附件 |

## 4. 后果

- **正面**：各屏 vblank 等待并行吸收（渲染线程/主线程零阻塞）；上下文共享问题整体消失（零 RDD 改动）；
  渲染节拍自然、静态内容零成本；genlock 从"必需"降为"锦上添花"
- **负面/代价**：Windows dev 路径 glReadPixels CPU 开销 ~1-2ms/屏（生产 DMA-BUF 零拷贝，PBO 异步列 backlog）；
  状态机 + fence 传递复杂度（状态机单测覆盖；fd 关闭权属需清晰防双 close/漏 close）
- **中性约束**：format/modifier 协商（不匹配 = 无声花屏）；EGLImage 禁止每帧重建；
  compositior scanout 条件（全屏 surface + 格式匹配 + 全屏 damage）

## 5. 验收

- 4 demo 干净退出、零 ERROR、呈现正确（GDI 路径）✅（2026-09-03）
- dirty 语义验证：内容不变时 presented 停在 1（静态 HUD 零成本）✅
- 唤醒拓扑无死锁（首帧 pre-post + 命令 push 唤醒）✅
- 待办：60+30 双屏 present 计数断言、DMA-BUF 零拷贝目标硬件实测（B3-P，登记 `skill/arhud-improvement`）
