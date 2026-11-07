# 开发日志：B5 — `CmdBufferUpdate` 去深拷贝（命令 payload 区 bump 分配）

- 日期：2026-09-27
- 范围：`drivers/command_buffer.h`、`drivers/gl/command_buffer_gl.h/.cpp`、
  `skill/arhud-render-thread-arch`、`skill/arhud-improvement`
- 依据：B5「清理与优化」之 `CmdBufferUpdate` 深拷贝优化项
- 变更分级 **B 类**：性能优化，零行为变化（GPU 写入结果不变）

## 1. 变更总览

| 项 | 落点 |
|---|---|
| `CmdBufferUpdate::data`：`LocalVector<uint8>` → `const uint8 *`（平凡类型） | `command_buffer.h` |
| 新增命令载荷区 `payload_arena_` + bump 指针 `payload_used_` + `AllocPayload()` | `command_buffer_gl.h/.cpp` |
| `BufferUpdate()`：`Resize+memcpy` 堆拷贝 → `AllocPayload()` bump 拷贝 | `command_buffer_gl.cpp` |
| 回放：`c.data.Data()` → `c.data` | `command_buffer_gl.cpp` |
| 移除 `DestroyNodeData` 的 `kBufferUpdate` 析构分支（已平凡） | `command_buffer_gl.cpp` |
| `Begin()`/`Reset()`：归还载荷区（`payload_used_ = 0`，容量保留） | `command_buffer_gl.cpp` |
| Doxygen 全面改述（去"深拷贝必要性/Route A 契约"） | `command_buffer.h`、`command_buffer_gl.h/.cpp` |

## 2. 关键实现决策

1. **未采纳字面"引用 Staging Buffer 指针"（重要）**：审计发现该做法**不安全**——
   `RenderingDevice::BufferUpdate` 的帧录制路径（`frame_driver_cmd_ != nullptr`）把**原始用户指针**
   传入命令缓冲，而这些指针常是**调用方短命局部**：
   [spatial_renderer.cpp:530-573](../../servers/rendering/renderer/spatial_renderer.cpp#L530-L573)
   传入 `light_data` / `no_light` / `&opacity` / `&default_opacity`。`RenderSpatialItems` 返回后
   `RenderThreadFunc` 才调 `rd.EndFrame()` 触发回放 → 存裸指针必成**悬垂指针 UB**。
   （且把每帧全部 UBO 更新灌进有容量上限的 frame staging，另有耗尽风险。）
2. **等效且安全的替代——命令 payload bump 区**：把载荷从"每条命令一次堆分配"改为拷入
   CommandBuffer 内建 `payload_arena_`。**memcpy 次数不变**（数据必须落到稳定内存），
   但**消除逐条堆分配/释放 + 逐条析构**，改为一次 bump 递增、整体归还。
3. **`CmdBufferUpdate` 变平凡类型**：`data` 改指针后析构平凡 → `DestroyNodeData` 移除该分支，
   节点销毁路径更简单（少一处"漏补即泄漏"的纪律点）。
4. **载荷区跨帧复用**：`Begin()`/`Reset()` 只把 `payload_used_` 归零、**保留容量**，避免每帧 realloc
   （与 `command_allocator_.Rewind()` 的"保留页内存"策略一致）。
5. **增长正确性已核实**：`LocalVector::Resize` 增长时仅初始化新元素、保留既有内容
   （`ResizeInternal` → `Reserve`），故 bump 增长不会丢已写载荷。

## 3. 排障记录（编辑失误留档）

- 改 `DestroyNodeData` 时，`old_str/new_str` 写得不完整，**误删了 switch 的收尾大括号**（保留 case、
  删掉 `}`），会造成语法错误。读回文件确认后立即修正：正确做法是 `old_str` 含 case + `default` + `}`，
  `new_str` 只留 `default` + `}` + 函数 `}`。
  教训：**删除一个 switch/函数分支时，old/new 必须成对覆盖到结构边界**，否则容易只删半边。

## 4. 验收结果

- 构建：全量绿（5 lib + 5 exe + `arhud_tests`），退出码 0，无 error/warning
- 单测：**25/25 通过**（`ctest --test-dir build -C Debug`）
- demo：5 个 demo 全部重链成功（渲染路径行为不变——仍 memcpy 到稳定内存，回放仍 glMapBuffer 写入）
- 语义等价性：录制→回放的数据内容、目标区间、空数据 no-op 行为均保持不变

## 5. 遗留（登记去向：skill/arhud-improvement）

- [ ] 真正的"零拷贝"（不 memcpy）需引入**数据生命周期契约**（源数据须存活至 `EndFrame()`）——
      当前保留一次 memcpy 是安全边界，不建议在无契约前提下强推
- [ ] B5 剩余：RD 跨线程 frame slot 机制简化；后处理独立子系统（拆出 `ARHudCompositeRenderer`）