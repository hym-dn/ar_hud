# 开发日志：B5 — 简化 RD frame slot（命令缓冲退化为单实例）

- 日期：2026-09-27
- 范围：`servers/rendering/rendering_device.h`、`rendering_device.cpp`、`rendering_device_frame.cpp`、
  `skill/arhud-render-thread-arch`、`skill/arhud-improvement`
- 依据：B5「清理与优化」之"简化 RD 跨线程 frame slot 机制（frame_done 帧节拍保留）"
- 变更分级 **B 类**：帧循环内部结构简化，行为不变

## 1. 审计结论（先确认边界）

B3 **已经**删掉了跨线程回放路径：`EndFrame` 现为同线程 `Execute() + Reset()`，
`ExecuteFrame`/`deferred_execution_` 已消失（仅剩一条"原 deferred 分支已退役"注释）。
因此真正的"跨线程 frame slot 残留"是：**按 `frame_number_ % frame_count_` 轮转的
CommandBuffer 池**（`command_buffer_pool_[]`）+ 其 `pool_state`/`frame_used` 字段——
这套轮转是为"主线程录、渲染线程放"而设，同线程录+放后**已无必要**。

## 2. 变更总览

| 项 | 落点 |
|---|---|
| 删 `enum class CommandBufferPoolState` | `rendering_device.h` |
| `struct CommandBuffer` 删 `pool_state` / `frame_used`，仅留 `driver_cmd` | `rendering_device.h` |
| `command_buffer_pool_[kMaxFrameCount]` → 单实例 `frame_command_buffer_` | `rendering_device.h` |
| `InitializeCommandBufferPool`：创建 1 个而非 `frame_count_` 个 | `rendering_device.cpp` |
| `DestroyCommandBufferPool`：重置单实例 RID | `rendering_device.cpp` |
| `BeginFrame`：去槽轮转/池状态断言，直接取单实例 `Begin()` | `rendering_device_frame.cpp` |
| `EndFrame`：去池状态归还，`frame_index` 仅保留用于 `frames_[]` | `rendering_device_frame.cpp` |

## 3. 关键实现决策

1. **`frame_count_` 必须保留**：它同时是**延迟销毁深度**（`frames_[]` + 每槽 GPU Fence）与
   **Staging Buffer 帧引用深度**。这两者依赖"资源释放要等 GPU 完成 N 帧前那一帧"，
   与命令缓冲槽数无关。故仅解耦命令缓冲（→ 1），`frame_count_` 不动。
2. **`frame_index` 在 `EndFrame` 仍保留**：用于索引 `frames_[frame_index]` 的 Fence 槽；
   仅 `BeginFrame` 里那处（只为取命令缓冲而算）被删除。
3. **`frame_done` 帧节拍不受影响**：它是 Engine 级 `Semaphore`（`arhud_engine.cpp`），
   与 RD 帧槽无关，属"队列背压之外唯一的帧级调节器"，按 B5 要求原样保留。
4. **单实例是充分的**：渲染线程每帧严格 `BeginFrame → 录制 → EndFrame(Execute+Reset)`，
   帧与帧之间不重叠，命令缓冲在下一帧开始前已 Reset 空闲，故 1 个足够。

## 4. 验收结果（含未被单测覆盖项的冒烟）

- 构建：全量绿（5 lib + 5 exe + `arhud_tests`），退出码 0，无 error/warning
- 单测：**25/25 通过**（`ctest --test-dir build -C Debug`）
- **demo 冒烟（必需）**：RD 帧循环**不在单测覆盖范围内**，故实跑
  `arhud_demo_phase2`（直接驱动 RCD→RDD→RD 的 BeginFrame/EndFrame）：
  **跑满 120 帧**，日志 `Render loop exited after 120 frames` +
  `=== ARHud Engine Minimal Demo exited cleanly ===`，RCD/GL/DisplayServer 全部干净关闭，
  **无 assert、无 ERROR**。（命令 shell 退出码非 0 系沙箱拦截输入法日志写入所致，与 demo 无关。）

## 5. 遗留（登记去向：skill/arhud-improvement）

- [ ] `Buffer::frame_used` / `Texture::frame_used`（`rendering_device.h`）：**只写不读**，疑似死字段
      （延迟销毁实际走 `frames_[]` 列表，而非该字段）。下一步核实后清理。
- [ ] B5 最后一项：后处理独立子系统（从 `RenderScreen` 拆 `ARHudCompositeRenderer`）——改动面最大，
      建议先出方案再动。