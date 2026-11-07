# 开发日志：command_executor.h/cpp 注释完善 + 生命周期描述对齐 Engine 装配现状

- 日期：2026-09-19
- 范围：servers/rendering/command_executor.h/.cpp（纯注释，零行为变化）
- 依据：变更分级 A 类（用户指令，延续 render_commands.h 注释完善；配合 Stage B 带读）

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 头文件生命周期段**过期修正**：`RenderScreen::Initialize 时构造` → `Engine::Impl 首次 AddScreen 延迟初始化共享栈`（B3-M0 现状，engine.cpp:244 实证）；Finalize 归属同步修正 | command_executor.h 类 doc |
| 头文件装配示例代码更新：`command_executor_.Initialize(&compositor_)`（旧双参签名 + RenderScreen 语境）→ Engine 三参现状 | 类 doc @code |
| 头文件 35 个私有 Exec 声明逐个补单行 `/// @brief`（含变长/共用布局/复用布局三类特殊标注） | 私有声明区 |
| cpp 文件头补 `@par B3 更新`（submit 信号注入 / item Create 掩码 / kPresent 退役）+ @version 2.1 | 文件头 |
| cpp Initialize/Finalize 补 Doxygen（此前裸函数）| 生命周期区 |
| ExecuteCommand 补 `@note 分发契约`（零 throw、单条失败不阻断、同步 Post 由 Flush 保证） | 主分发 |
| cpp 35 个 Exec 实现补 @brief；三个共用布局方法补 `@param p_type` 路由说明；Camera/Light 补字段重组说明 | 各分区 |
| Item 区头扩充"通用模式"四条（Create 写回 / Set* 转发+dirty 在 Storage 侧 / 共用布局路由 / 复用布局字段名） | Item 管理区 banner |

## 2. 关键实现决策

1. **过期修正优先于补缺**：生命周期段描述的是 B2 时代（RenderScreen 持有栈）的装配，
   与 B3-M0 现状（Engine::Impl 持有 + submit_signal 注入）不符——带读者会被误导。
2. **通用模式进区头、方法只留一行**：40 个 Exec 中约 30 个是"一行转发"，
   逐个写长注释是噪音；共性（dirty 路由在 Storage 侧、共用布局 p_type 路由、
   复用布局字段名语义）集中在区头，特殊点（变长拆包、空 shader 内置着色器
   契约、Camera 字段重组）才在方法级展开。
3. **复用布局的"字段名错位"显式标注**：`ExecTextItemSetFont` 用
   `p_cmd.material` 携带字体 RID——这是共用布局最反直觉的一点，注释点明。

## 3. 排障记录

- **编辑手滑自纠**：为 ExecSpatialItemCreate 补 @brief 时误把一行旧声明粘进
  newString（cpp 内出现非法的类外重声明），紧随其后的编辑立即移除。
  教训：批量机械补注释时，newString 粘贴后先回读一眼再提交。

## 4. 验收结果

- 全量构建零 error / 零 warning；单测 25/25
- phase4_text 冒烟 exit=0 / 零 ERROR / 干净退出

## 5. 遗留

- [x] 无。
