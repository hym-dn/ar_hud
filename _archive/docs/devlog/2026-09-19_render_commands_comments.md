# 开发日志：render_commands.h 注释完善 + 过期引用修正

- 日期：2026-09-19
- 范围：servers/rendering/render_commands.h（纯注释，零行为变化）
- 依据：变更分级 A 类（用户指令"render_commands 完善注释"，配合 Stage B 带读发现注释断层）

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 枚举值逐个补 `///<` 注释（item 全部 32 个 + 相机灯光 4 个 + 帧控制 4 个） | RenderCommandType 枚举 |
| 枚举分组头修正：item 段"异步 Push"→"Create 同步 PushAndSync 写回 RID，Set*/Free 异步 Push" | 枚举分组注释 |
| Item 管理命令区 17 个结构体补 @brief + 字段注释（原为裸结构体） | Canvas/Spatial/Text 命令结构体 |
| TextItemSetTextCmd 变长布局注释正式化（原 `// charcodes follow inline` 一行裸注释） | TextItemSetTextCmd |
| Camera3DSetCmd/Camera2DSetCmd/DirectionalLightSetCmd 补 @brief + 字段语义 | Camera/Light 区 |
| 区头注释正式化："-- Spatial items (reuse Set* patterns) --" 等英文裸注释 → 中文布局规律说明 | 三个区头 |
| ItemSetParentCmd 措辞修正："kInvalidRID"（不存在的名字）→ "kInvalid（默认构造的 RID()）" | ItemSetParentCmd |

## 2. 过期注释修正（带读中发现，比补缺更重要）

| 过时内容 | 现状 | 修正 |
|---|---|---|
| 文件头"大体积数据走 FrameScene 双缓冲槽（见 frame_scene.h）" | FrameScene 已于 B3-M0.4 删除，frame_scene.h 不存在 | 改为"状态由 ItemStorage 承载（增量小命令）+ 废弃记录指向" |
| 枚举注释"kSubmitScene/kPresent/kResize/kShutdown：B3 预留，当前无执行器" | B3 已落地：kSubmitScene 有执行器；kPresent 已退役 | 逐个标注真实状态（含"kPresent 收到即 WARN"） |
| kPresent 注释"B3：呈现（SwapBuffers）" | B3 渲染/上屏分离，SwapBuffers 路径退役 | "已退役：上屏归 present 线程" |
| 文件头同步/异步表缺 item 创建行、帧命令行写"kPresent" | item Create 实为同步 | 表补一行 + 帧命令行改 kSubmitScene |

## 3. 关键实现决策

1. **注释只写确定事实**：所有树传播语义（父链连乘/AND 传播/alpha 连乘）严格对应
   ItemStorage 实现（ADR-0005），不引入未验证的行为描述（如 SetMaterial(kInvalid)
   的语义未定义就不写）。
2. **枚举注释对齐资源段风格**：`///< ItemStorage::Xxx（关键语义）`——既有资源段
   就是"落点 + 括号补充"式，item 段补齐后全枚举风格统一。
3. **Item 区头写"布局规律"而非逐个重复**：共用布局（ItemFreeCmd 三类型一结构体）、
   同布局复用（kSpatialItemSetMaterial 复用 Canvas 结构体）是读者最容易迷路的两点，
   集中在区头讲清。

## 4. 验收结果

- 全量构建零 error / 零 warning（头文件被 servers/tests 全量 include）
- 中文注释抽查无乱码（Read 工具 UTF-8 解码验证）
- phase4_text 冒烟 exit=0 / 零 ERROR / 干净退出

## 5. 遗留

- [x] 无。
