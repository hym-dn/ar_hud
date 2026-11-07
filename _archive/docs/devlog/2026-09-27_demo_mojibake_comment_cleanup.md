# 开发日志：demo main.cpp 乱码注释清理（P3 #365）

- 日期：2026-09-27
- 范围：`app/phase4_canvas/main.cpp`、`app/phase4_spatial/main.cpp`、`app/phase4_compositor/main.cpp`、`app/phase4_text/main.cpp`
- 依据：P3 #365「demo main.cpp 乱码注释清理」（登记于 `skill/arhud-improvement`）
- 变更分级 **A 类**：纯注释，零行为变化

## 1. 实际范围（修正登记口径）

登记描述为"4 个 demo 的 `?` 乱码注释"，治理基线记"注释命中 7 处"。**实测 mojibake 行数为 53 处**
（canvas 14 / spatial 9 / compositor 13 / text 17）——两个数字口径不同：治理 ① 数的是
"提及渲染内部名的注释行"，此处数的是"含 `?` 的损坏注释行"。本次处理后者（53 处）。

## 2. 关键前提：原文不可恢复，本次属"重建"

**本仓库没有 git**（`fatal: not a git repository`），故**没有历史版本可回滚**，原始中文**已永久丢失**。
因此本次不是"还原"，而是**按注释所标注代码的真实行为重写**。这是本次变更最重要的性质，
后续若要追溯"这行注释原本写了什么"，答案是：**无法追溯**。

## 3. 变更总览

| 文件 | 处数 | 备注 |
|---|---|---|
| `phase4_canvas/main.cpp` | 14 | 含文件头、RID 区块、CommandQueue/GL 说明、帧率控制 |
| `phase4_spatial/main.cpp` | 9 | 含文件头、60 帧动态创建、Sync 节流 |
| `phase4_compositor/main.cpp` | 13 | 含文件头、Godot 渲染流程对照、双阶段说明 |
| `phase4_text/main.cpp` | 17 | 含文件头、FreeType 字体查找、SDF 字号、UTF-32/ASCII 说明 |

## 4. 处理原则

1. **以代码为准**：每条重写都对照其下方/相邻代码的真实行为，不发明功能。
2. **保留 ASCII 技术 token**：`CommandQueue`/`RenderServer`/`RID`/`GL`/`FreeType`/`SDF`/`UTF-32`/
   `Consolas`/`PushAndSync`/`FPS`/`ms` 等原样保留（它们本就在原文中）。
3. **非 ASCII 符号按语义还原**：部分 `?` 原本不是汉字而是符号（破折号 `——`、管线箭头 `→`、乘号 `×`），
   按上下文还原，未强行改写为纯中文。
4. **只动含 `?` 的行**：不含 `?` 的注释（如 `// 保留模式：...（渲染走 RenderFromStorage）`、
   `// B3: render thread calls RenderFromStorage internally...`）一律不动。

## 5. 验收结果

- 全文 grep `\?`（`app/`）：**0 命中**
- 4 个文件首三字节均 `47,42,42`（`/**`）：**无 BOM**，中文正常
- 行数未变（canvas 284；其余同量级，未增删代码/空行）
- 构建：全量绿（5 lib + 5 exe + `arhud_tests`），退出码 0
- 单测：**25/25 通过**
- 治理 ① 注释计数 **15→15 不变**：命中的是 4 个 demo 对渲染内部名的**正常注释提及**（如 "渲染走
  RenderFromStorage"），本就合规；本次只消除 `?`，未新增/删除内部名提及

## 6. 附带订正（治理文档精度）

- `docs/process/verification.md`：判读规则与基线复核——① 命中为**正常注释提及（非乱码）**；
  并补记**唯一代码命中** = `phase2` 直调 `RenderingDevice`，属治理 §12.4 **备案豁免**（NOLINT）。
- `skill/arhud-improvement`：P3 #365 勾销；顺带把已陈旧的 `CmdBufferUpdate 深拷贝` 行勾销
  （实际走 payload 区 bump 分配）。

## 7. 收紧推断性括注（同日补做）

初版重写残留了若干**推断性括注**（对原文的猜测），已按两条规则收紧：

1. **能保住的 ASCII 原文一字不动**：4 个 demo 的文件头英文（如 `2D Canvas Rendering Validation`、
   `(2D layers)`、`(clear color+depth)`、`(cf. Godot RendererViewport::_draw_viewport)`）
   初版被译成中文，属多余改动，**已全部还原为英文原文**，只补原文中丢失的 `?`（破折号/箭头）。
2. **推断性括注一律删除**：共 8 处猜测性说明被删或改为代码可见事实——
   `（经 CommandQueue 创建/上传/异步创建）`、`（GL 调用在渲染线程执行）`、`（等待本帧渲染完成）`、
   `（材质/网格/item）`、`（避免 CPU 空转）`、`（CommandQueue 动态用例）`。

**保留的括注均有据可查**（非推断）：`（Hello AR HUD）` ↔ `kTextHello[]` 的字面内容；
`（32-126）` ↔ `for (c = 32; c < 127; ++c)`；`（动态创建纹理用）` ↔ frame-60 分支中
名为 `dyn_green` / `dynamic_green` 的纹理创建。其余剩余的括注均为**原本完好、未被乱码波及**的注释。

## 8. 同类扫描：非括注推断性填充（同日补做）

对全部 53 处重写行做同类扫描，再收紧 8 处"断言了代码之外事实"的填充：

| 处 | 问题 | 收紧后 |
|---|---|---|
| canvas:233 / spatial:156 / compositor:214 / text:215 | 四处的 GPU 同步描述互不一致，且**代码里只有帧计时 + sleep，并无 GPU 同步**（text 那处还凭空引入了 `VSync`） | 统一为 `// GPU 同步`（仅保留幸存 ASCII `ync`/`GPU` 的语义） |
| text:59 | 凭空写"Consolas、Arial 等常见字体" | 改为代码可见的 `kFontPaths` 实际内容：`consola / arial / msyh / tahoma` |
| text:65 | 凭空补"等宽字体"（且同组 `// Arial` / `// Tahoma` 均为裸名） | 改回裸名 `// Consolas`，与同组风格一致 |
| compositor:87 / text:112 | 凭空补"句柄" | `// 初始资源 RID` / `// 资源 RID` |

**判定为可保留（未改）**：canvas:109-111 的三行说明——其断言（`CommandQueue` 异步提交、
`BeginFrame` 唤醒渲染线程、`PushAndSync` 同步取 RID）均为**有据的架构事实**，且与幸存 ASCII
token 完全对应，非臆造。

### 行号核对（重要）

过程中发现 **Read 工具某次返回的行号整体偏移了 1 行**，一度误判"丢了行"。改用 PowerShell
直读行号核对后确认：4 个文件行数均未变（**284 / 196 / 261 / 263**），`// Sync 同步` 确在
canvas:233、`// 帧率控制` 确在 canvas:235，与原始乱码行号完全一致——**无任何代码或行丢失**。
（此后凡涉行号校验，一律以命令直读为准。）

### 验收

- 构建全量绿；单测 **25/25**；4 文件行数不变且无 BOM；`?` 仍为 0。

## 9. 遗留

- [ ] 建议：为仓库**建立 git**（或至少定期快照），否则任何一次编码事故都不可回滚——
      本次 53 处注释的永久丢失即是代价。