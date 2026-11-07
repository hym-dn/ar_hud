# 开发日志：建立正规文档体系与开发流程（docs/ 目录）

- 日期：2026-09-19
- 范围：docs/（新建 README/process/templates/adr）+ 根 AGENTS.md + skill/arhud-improvement（登记项）
- 依据：用户指令「整理 agent 开发所需文档目录，建立正规流程」；变更分级 A 类（零代码行为变化，纯文档/流程）

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 文档地图（职责边界 + 同步义务 + 阅读顺序） | `docs/README.md`（新建） |
| 开发主流程（变更分级 A/B/C + 任务生命周期 6 阶段 + 流程图） | `docs/process/dev-workflow.md`（新建） |
| 验证手册（构建/测试/冒烟/治理 grep 命令 + 7 条已知坑） | `docs/process/verification.md`（新建） |
| 完成定义 DoD（A 代码/B 行为/C 治理/D 文档同步/E 产物卫生） | `docs/process/definition-of-done.md`（新建） |
| devlog 模板 + ADR 模板 | `docs/templates/`（新建 ×2） |
| ADR 体系：索引 + 历史决策追溯表 + 4 个种子 ADR | `docs/adr/`（新建） |
| AGENTS.md 修正：Project state 更新（tests 6/6、B3 落地）、scene 描述、删 drivers 重复段、servers 组件表补齐、docs/ 入口、ARHUD_BUILD_TESTS 默认值、异常/RTTI 描述对齐 ADR-0004、失效引用 `skill/SKILL.md`、Conan 构建路径 `build/Debug`→`build` | 根 `AGENTS.md` |
| P3 登记新增：skill/arhud-architecture 滞后项、demo 乱码注释清理项 | `skill/arhud-improvement` |

## 2. 关键实现决策

1. **职责分离**：skill/ 记"现在是什么样"（持续更新），docs/adr/ 记"为什么这么做"（只增不改），
   devlog 记"这次改了什么"（一次性）。ADR 建立后新决策不再散落 skill 正文。
2. **种子 ADR 选取**：只正式化 4 个最重大决策（Route B 渲染搬线程/多屏共享栈/渲染上屏分离/异常 RTTI），
   其余历史决策以追溯表形式指向权威记录，避免大规模文档迁移造成失真。
3. **治理 grep 以 PowerShell 原生 `Select-String` 为主**：本机无 `rg`；判读规则明确
   "代码行命中=违规、注释提及可接受"，并记录 2026-09-19 基线（①7 处乱码注释 / ②③ 零）。
4. **AGENTS.md 保持速读定位**：新增「Documentation & process」节作为入口，不把流程细节搬进 AGENTS.md。

## 3. 排障记录

- **治理 grep 初版误报**：verification.md 初稿用 `rg` 且未过滤注释，实测 app/ 命中 7 处
  ——全部为注释（含历史乱码注释提及 RenderFromStorage），material_storage.h 1 处亦为 Doxygen。
  修正：改 Select-String + 整行注释过滤 + 判读规则 + 基线记录。
  教训：治理命令必须先在本机实跑校准，再写入手册。

## 4. 验收结果

- 文档树：docs/ 共 15 文件（README 1 / process 3 / templates 2 / adr 5 + 索引 / devlog 3 含本篇）
- AGENTS.md 通读一致（ADR-0004 编号、verification gotcha #1 引用均有效）
- 治理 grep 实测：①注释 7 处可接受 / ②FrameScene 零 / ③storage→renderer 零 / cmake 无禁用标志
- 代码零改动：本篇归档后需重跑全量构建确认（文档变更，构建预期不受影响）

## 5. 遗留（登记去向：skill/arhud-improvement P3）

- [x] README 重写 —— 已登记（未实施）
- [x] skill/arhud-architecture 滞后修正 —— 已登记（未实施）
- [x] demo main.cpp 乱码注释清理 —— 已登记（未实施）
- [ ] git 初始化与分支/提交规范 —— dev-workflow §3 已留占位说明，待用户决策
