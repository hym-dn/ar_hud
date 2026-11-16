---
name: "arhud-mode-x-validation"
description: "Mode X（共享显存零回读上屏）的多屏与混合显卡实测检查清单：验证开关、判定口径、已验项与证据、待验项操作步骤、以及新发现缺陷的复现方法。跑 Mode X/呈现路径相关改动前后按本清单过一遍。"
---

# Mode X 实测检查清单（多屏 / 混合显卡）

> 关联：design skill §5.5（三模式与拆卸硬约束）、§7.7（帧节拍）、
> AGENTS.md 坑 7/8/9；缺陷账目见 progress §5。
> 本文只回答"怎么测、测什么、算通过"，不重述架构。

## 0. 验证开关与判定口径

| 开关 | 作用 |
|---|---|
| `--screens=N`（canvas demo） | 进程内开 N 屏（1~4），每屏一个窗口/帧池/上屏线程 |
| `ARHUD_FORCE_READBACK=1` | 跳过帧池，强制 Mode R（A/B 基线） |
| `ARHUD_DEBUG_SAMPLE=1` | 每 300 帧读回一次上屏帧算平均亮度（画面自证） |

判定看 demo 结尾三类行（stderr）：

1. `AddScreen: screen i presenting via shared VRAM (zero readback)` —— 该屏走了 Mode X；
2. `PresentThread finalized (shared=…, readback=…, degraded=…, failed=…)` —— **每屏一行**，shared>0 且 readback=0 = 该屏全程零回读；`degraded>0` = 运行期掉回 GDI（**须归因，不算通过**）；
3. `RenderViewport finalized (present path: shared=… readback=… MB… ms)` —— GPU→CPU 搬运量直接可见。

辅助行（device 创建时每池打一次）：`adapter=<名> … outputs=N drives_target_window=M` 与 `GL_RENDERER=<名>` —— **二者同卡**是 Mode X 成立前提。

## 1. 清单 A：多屏（`--screens`）

本机 = Intel Iris Xe + NVIDIA MX450 双卡、双物理输出（DISPLAY1 1600×1000 主、DISPLAY2 2560×1440），GL 与帧池均落在 MX450。

| # | 用例 | 操作 | 通过判据 | 本机状态 |
|---|---|---|---|---|
| A1 | 双屏·同适配器 | `arhud_demo_phase4_canvas.exe --screens=2`（两窗口默认级联主显即可复现，无需拖到第二屏） | 两行 `presenting via shared`；两个 PresentThread 均 `shared>0 readback=0 degraded=0 failed=0`；两窗口均有画面 | ❌ **失败**（非确定性，见 §3 MX-MULTI-01：或 device-lost→屏 1 降级 GDI，或驱动内 AV 崩溃）。**收敛**：崩溃与"跨物理屏"无关，单适配器双 swapchain+interop 并行即触发 |

| A2 | 双屏·基线对照 | 同 A1 加 `ARHUD_FORCE_READBACK=1` | 两屏 readback 计数对等、`degraded=0`、退出干净 | ✅ 通过 |
| A3 | 双屏·退出顺序 | A1/A2 全程任一时刻关主窗口 | `exited cleanly`、无 `DumpLeaks` 输出（`presenters_` 的 null 槽跳过逻辑生效） | ✅ 通过（关窗即退属预期：ShouldClose 只看首屏窗口） |
| A4 | 三/四屏 | `--screens=3` / `4` | 每屏一行统计；`degraded` 计数与屏数一致性可解释 | ☐ 待执行 |
| A5 | 多屏 + resize | `--screens=2` 中分别拖拽两窗口尺寸 | 每屏各自 `resized to` 收敛；共享路径重建后 `degraded=0` | ☐ 待执行（依赖 §3 缺陷修复） |
| A6 | 撕裂/帧序观察 | `--screens=2` 长跑，观察高对比图案（canvas 棋盘）快速移动边缘 | 无水平撕裂；两屏动画相位差 ≤1 帧 | ☐ 待执行（人工观察项，A1 不稳时先只做单屏） |

## 2. 清单 C：混合显卡 / 跨适配器

| # | 用例 | 操作 | 通过判据 | 本机状态 |
|---|---|---|---|---|
| C1 | 适配器枚举与 LUID 判定 | 任意 demo 跑 5s，抓 `adapter=` / `GL_RENDERER=` / `output[` 行 | 帧池 adapter LUID == GL 渲染器所属卡；`drives_target_window=1` | ✅ MX450 双确认（luid=(0,61733)，outputs=2 覆盖两屏） |
| C2 | 无 interop 扩展回落 | 换无 NV 扩展的机器/驱动，或软件渲染环境 | 日志 `interop absent/unavailable`，画面走 Mode R，无崩溃 | ☐ 需异机（本机 MX450 有扩展） |
| C3 | 帧池建在核显上（强制跨卡） | Windows 设置 → 显示 → 图形 → 该 exe 指定"节能=Intel"，跑 `--screens=1` | 允许两种结果：Mode X 正常（Intel 有 interop）或**干净回落** `OpenDevice failed → readback`；**不允许**崩溃/黑屏/`degraded>0` | ☐ 待执行（需切换 OS 级 GPU 首选项并重启进程） |
| C4 | Intel-only 场景 | 设备管理器禁用独显后跑 demo | 同 C3 判据（`GL_RENDERER` 变 Intel UHD，Mode X 可用或回落） | ☐ 待执行（有损操作，可选） |
| C5 | 虚拟显示适配器共存 | 本机已含 OrayIdd 虚拟适配器 | 拓扑日志不把它计为输出即达标 | ✅ 天然覆盖（adapter 枚举未受干扰） |

## 3. 实测新发现：多屏 Mode X device-lost（MX-MULTI-01）

**状态：未修复，修复前 A1/A4/A5 无法标 ✅。** 已录入 progress §5。

- **定性**：`hr=0x887A0005` 经查 SDK = **DXGI_ERROR_DEVICE_REMOVED**（不是 HUNG/RESET）。
  这是 WDDM 监护（TDR）类摘除的典型呈现：GPU 侧某个操作超时/出错，驱动把设备
  重置摘走，进程内两个池 + interop + swapchain 全部连带失效——一次事故能解释
  两种结局（降级继续跑 vs 驱动内 AV，取决于摘除瞬间谁在驱动里）。
- **现象（非确定性，三种观测到的结局）**：
  1. ~1280 帧（≈20s）后屏 1 报 `device lost hr=0x887A0005`，其 interop `Lock`
     连锁失败（770 次，`GetLastError=0`），逐帧降级门生效 → 屏 1 以 GDI 回读
     跑完（`readback=768 degraded=1 failed=1`），屏 0 全程零回读，退出干净；
  2. ~704 帧时**直接 AV**：Windows 事件日志（Application Error）指向
     `nvwgf2umx.dll+0x442737`（NVIDIA 驱动内），异常码 0xc0000005；
  3. 50s 内无异常（复现率非 100%，高压时序相关）。
- **已排除**：
  - ❌ 窗口落到非合成/无 DWM 输出——两输出均挂 MX450 且 drives_target_window 正常；
  - ❌ 运行期改过 `Present1` 参数（该修复在 `793f0e2` 内且未再动）；
  - ❌ 单池问题——`--screens=1` 6000 帧长跑 `degraded=0` 无恙（resize 也扛过）；
  - ❌ 尺寸双源/旧 RID 误杀——A2 基线双屏回读全程无错。
- **嫌疑排序**（修复时从这查，全部指向"双池并行 + interop 命令流"的 GPU 侧挂死）：
  1. **两 present 线程共用 `g_dx_context`（immediate context）做 `CopyResource`**，
     叠加 multithread-protect 锁与 GL 线程 interop 命令流的交叉——驱动内部
     序列化失败/自等待 → 超 TDR 时限摘设备；
  2. Optimus 混合模式下 flip-model 双 swapchain 的 present 路径（copy engine /
     rescale 子系统等共享资源）本就是 TDR 高发面；单池串行遮蔽，双池并行暴露；
  3. `wglDXLockObjectsNV` 返回 `GetLastError()=0`：NV 驱动该入口疑不设置
     last-error——错误采集不可信，须补 `FAILED(HRESULT)` 语义 + `glGetError`
     + `GetDeviceRemovedReason` 三路交叉定位。
- **最小复现**：`arhud_demo_phase4_canvas.exe --screens=2`（两窗口保持默认级联
  位置即可，**无需跨物理屏**——已实测 `drives_target_window` 恒为 output[0]
  仍复现，问题面从"多屏"收敛到"单适配器多池并行"）；观察 stderr 至 ~20s，
  并同步看事件查看器 TDR 事件与 Application Error 事件对照。
- **定位辅助**（按序做）：
  1. device-lost 分支打 `device_->GetDeviceRemovedReason()` 与当时帧计数；
  2. interop Lock 失败路径补 `glGetError()`（区分设备已移除 vs 句柄失效）；
  3. 二分并行度：临时把屏 1 的 `CommitFrame` 挪回渲染线程串行执行（同进程
     一次只有一条 present 流）——若不复现，坐实嫌疑 1/2；
  4. 仍复现 → 上 D3D11 debug layer / PIX capture 看 GPU 侧挂死命令。

## 4. 清单 D：改动回归门禁（每次触碰呈现链必跑）

1. `cmake --build build --config Debug` 零错误 + `ctest` 全绿（42 用例含 rid_owner 地基组）；
2. 单屏：`--screens=1` 自然退出，三行统计符合 §0 判据、`exited cleanly`；
3. 基线：`ARHUD_FORCE_READBACK=1` 同跑法错误计数为 0；
4. resize：拖拽窗口 ≥3 次不同尺寸，`invalid pipeline RID / invalid buffer RID / invalid render pass = 0`、`resized to` 次数 == 尺寸变化次数；
5. 多屏：A1~A3（MX-MULTI-01 修复后恢复为门禁项）；
6. 日志关键字全文扫描必须为空：`ERROR`、`invalid`、`GLAD: ERROR`、`degraded to`、`device lost`。

## 5. 工具残留说明

- `--screens` 参数**只加在 canvas demo**（多屏验证专用入口）；其余 demo 保持单屏，避免冒烟面扩大。
- 本清单不含代码开关 `ARHUD_D3D_ADAPTER` 之类"代码内强制选卡"——强制选卡走 **OS 级图形设置**（C3），代码里不养测试专用分支。
