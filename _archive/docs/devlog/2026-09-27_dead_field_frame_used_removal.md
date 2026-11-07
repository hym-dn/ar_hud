# 开发日志：清理死字段 `Buffer::frame_used` / `Texture::frame_used`

- 日期：2026-09-27
- 范围：`servers/rendering/rendering_device.h`、`rendering_device.cpp`、`rendering_device_frame.cpp`
- 依据：`docs/devlog/2026-09-27_frame_slot_single_command_buffer.md` §5 遗留项
- 变更分级 **A 类**：死字段清理，零行为变化

## 1. 核实过程（先证"死"，再删）

全仓 grep `frame_used`（排除 `build/`），逐个判定读写：

| 位置 | 性质 |
|---|---|
| `rendering_device.h:104` `Buffer::frame_used` 声明 | 声明 |
| `rendering_device.h:119` `Texture::frame_used` 声明 | 声明 |
| `rendering_device.cpp:357` `buf->frame_used = frame_number_` | **写**（无对应读） |
| `rendering_device.cpp:582` `tex->frame_used = frame_number_` | **写**（无对应读） |
| `rendering_device.h:112` / `:303` 文档提及 | 文档（陈旧） |
| `rendering_device_frame.cpp:177` 注释 | 自认过时（"EndFrame 不再需要单独标记 frame_used"） |

**读取点：0**。延迟销毁实际由 `frames_[frame_index]` 帧槽列表 + Fence 承担，
Staging 回收则由 `StagingBufferBlock::frame_references` 承担——`frame_used` 既非入参也非判定依据，
确为**只写不读**的死字段。（注：此前 B5 那步删的是 `CommandBuffer::frame_used`，与这两个不同。）

## 2. 变更总览

| 项 | 落点 |
|---|---|
| 删 `Buffer::frame_used` 字段 | `rendering_device.h` |
| 删 `Texture::frame_used` 字段 + 订正类文档（延迟销毁归 Frame 帧槽列表） | `rendering_device.h` |
| 订正 `StagingBufferBlock` 工作流文档（第 4 步改为"记录帧引用"） | `rendering_device.h` |
| 删 `buf->frame_used = frame_number_;` | `rendering_device.cpp` |
| 删 `tex->frame_used = frame_number_;` | `rendering_device.cpp` |
| 订正帧引用追踪注释（去掉"不再需要标记 frame_used"的负向表述） | `rendering_device_frame.cpp` |

## 3. 验收结果

- 构建：全量绿（5 lib + 5 exe + `arhud_tests`），退出码 0，无 error/warning
- 单测：**25/25 通过**（`ctest --test-dir build -C Debug`）
- **未做 demo 冒烟**（说明）：本次仅删除"无读取点的写入"与陈旧注释，**行为可证等价**
  （删除一个无读者的字段写入，不改变任何可观测状态），故不构成帧循环/渲染路径风险，
  build + 单测已足够。

## 4. 说明（关于历史文档）

`2026-09-27_frame_slot_single_command_buffer.md` §5 曾把本项登记为遗留；devlog 为**按日期的历史快照**，
故不回头改写该文件，本日志即其完成记录。

## 5. 遗留（登记去向：skill/arhud-improvement）

- [ ] B5 最后一项：后处理独立子系统（从 `RenderScreen` 拆 `ARHudCompositeRenderer`）——改动面最大，
      建议先出方案（C 类）再动