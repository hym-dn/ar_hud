# 开发主流程（Dev Workflow）

> 适用：本仓库的一切代码变更，AI agent 与人类执行同一流程。
> 流程深度由**变更分级**决定；验证与归档标准见 `verification.md` 与 `definition-of-done.md`。

---

## 0. 变更分级

| 级别 | 定义 | 例子 | 流程要求 |
|---|---|---|---|
| **A 类 小修** | 零行为变化 | 注释/Doxygen 补齐、日志、格式化、文档修正 | 免方案；构建 + 冒烟 + devlog |
| **B 类 修复/功能** | 行为变化，不动架构 | 修 bug、加 RenderServer API、加测试用例 | 文件清单 + 全量验证 + devlog |
| **C 类 结构/架构** | 触碰分层/线程模型/接口契约/依赖方向 | 渲染搬线程、契约归位、多屏共享栈 | **方案先行**（skill 章节 + ADR）+ 分阶段可编译可测 + 阶段验收 |

C 类铁律：
1. 先在相关 `skill/` 补设计章节（或新写 ADR），明确否决方案与理由
2. 每阶段结束必须可编译、既有测试全绿（不允许长期 编译不过 的中间态）
3. 分阶段推进（参照 B3-M0.1~M0.4 → B3 的拆法），每阶段独立验收

## 1. 任务受理

任务来源三类：
- **路线图**：`skill/arhud-improvement`（B3-P/B4/B5、治理 P1~P3）
- **缺陷/回归**：demo 或单测暴露的问题
- **用户指令**：临时需求

受理时明确三件事：
1. 目标（一句话）
2. 验收标准（可验证的表述：grep 零命中 / 单测绿 / demo 行为 / 数字指标）
3. 涉及层（core / platform / drivers / servers / app —— 判断是否越层）

## 2. 预读（动手前必做）

固定顺序：
1. 根 `AGENTS.md`（速览 + 规范 + 已知坑）
2. `docs/adr/` 相关决策（为什么是现状）
3. `skill/` 相关设计章节（目标形态与迁移计划）
4. `docs/devlog/` 相关历史（排障教训，尤其编码事故与超时误判）
5. **读源码**（读后再改；接口以代码为准，文档可能滞后）

产出：改动文件清单 + 预期验证方法。

## 3. 方案（B/C 类必做，A 类跳过）

- 明确接口变化：新增/修改/删除的公共方法，及调用方迁移清单
- 明确所有权与线程归属：新对象属于主线程 / 渲染线程 / present 线程哪一根（对照 `skill/arhud-architecture` §二 线程归属表）
- C 类：写 ADR（模板 `docs/templates/adr-template.md`）
- **最小改动原则**：不做任务外的顺手重构；发现的问题登记到 `skill/arhud-improvement`，不夹带
- VCS 现状：仓库当前未初始化 git；初始化后补充分支与提交规范（建议：`main` 保护 + 短命分支 `feat|fix|docs/<topic>`，提交信息 `<type>: <subject>`）

## 4. 实施

- 编码规范：根 `AGENTS.md`「Coding conventions」表为准（PascalCase 方法 / snake_case 变量 / 成员 `_` 后缀 / k 前缀常量 / 2 空格缩进 / 100 列）
- 新增源文件：手工加入对应模块 `CMakeLists.txt` **显式列表**（禁止 `file(GLOB ...)`）
- 公共 API 补全 Doxygen（`@brief` / `@param[in,out]` / `@return` / `@see`）；实现侧注释遵循 `@brief …（实现侧）` 分节惯例
- **中文文件批量编辑禁令**：禁用 PowerShell `Set-Content`/管道写 UTF-8 中文源码（GBK 二次编码事故，2026-09-03）；用 `[System.IO.File]::ReadAllText/WriteAllText` + 显式 UTF8，改完抽查中文注释
- 命令队列相关：1MB 内联缓冲区必须堆分配；渲染线程内不得发同步命令（`CanBypassQueue` 短路语义）

## 5. 验证

按 `docs/process/verification.md` 执行，顺序：
**配置 → 全量构建 → ctest → demo 冒烟 → 治理 grep**。

验收标准在任务受理时确定；demo 冒烟超时按 帧数 × 帧时长 计算并留余量。

## 6. 归档（DoD）

按 `docs/process/definition-of-done.md` 逐项过：
1. **devlog 必写**：模板 `docs/templates/devlog-template.md`，命名 `YYYY-MM-DD_主题.md`，含排障记录（踩坑必留档）
2. **ADR**：C 类/决策类变更补 ADR；已有 ADR 落地完成则更新状态标注
3. **skill 同步**：`arhud-improvement` 状态列更新；`arhud-architecture` 现状如有变化同步；遗留项登记（不静默丢弃）
4. **AGENTS.md 同步**：目录/构建选项/规范变化时更新对应段落

## 附：流程总图

```
受理 ──► 预读 ──► 分级判断
                   │
        A 类 ──────┼────── B/C 类
         │         │          │
         │         │      方案（B: 文件清单 / C: skill+ADR）
         │         │          │
         ▼         ▼          ▼
       实施 ◄───── 实施 ◄─── 实施
         │         │          │
         └────┬────┴──────────┘
              ▼
   验证（verification.md 四步）
              ▼
   归档（definition-of-done.md + devlog/ADR/skill/AGENTS 同步）
```
