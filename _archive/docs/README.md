# ARHud Engine 文档体系（文档地图）

> 本目录是工程的文档中心，AI agent 与人类开发者共用同一套文档。
> 入口顺序：`AGENTS.md`（速览）→ `docs/process/`（怎么干活）→ `skill/`（设计规范）→ `docs/adr/`（为什么这么做）。

---

## 目录结构

```
docs/
├── README.md                 ← 本文件：文档地图
├── process/                  ← 正规开发流程（动手前必读）
│   ├── dev-workflow.md       ← 开发主流程：变更分级 + 任务生命周期 6 阶段
│   ├── verification.md       ← 验证手册：构建/测试/冒烟/治理 grep 命令 + 已知坑
│   └── definition-of-done.md ← 完成定义（DoD）：归档前逐项 checklist
├── templates/                ← 文档模板
│   ├── devlog-template.md    ← 开发日志模板（docs/devlog/ 使用）
│   └── adr-template.md       ← 架构决策记录模板（docs/adr/ 使用）
├── adr/                      ← 架构决策记录（Architecture Decision Records）
│   ├── README.md             ← ADR 索引 + 历史决策追溯表
│   └── 0001~0007-*.md        ← 编号递增，只增不改（取代除外）
└── devlog/                   ← 开发日志（按日期归档）
    └── YYYY-MM-DD_主题.md
```

`docs/` 之外的配套文档：

| 位置 | 职责 |
|---|---|
| `AGENTS.md`（根） | agent 入口速览：目录/构建/规范/已知坑 |
| `README.md`（根） | 面向人的项目总览（⚠️ 内容滞后，待重写，见 skill/arhud-improvement P3） |
| `tests/README.md` | 测试构建/运行/新增方法 |
| `skill/`（4 份） | 持续状态的设计规范：`arhud-architecture`（现状）/ `arhud-render-thread-arch`（Route B）/ `arhud-multiscreen-arch`（多屏）/ `arhud-improvement`（路线图） |

---

## 职责边界（什么内容写到哪里）

| 内容类型 | 去处 | 说明 |
|---|---|---|
| "现在是什么样"（架构现状、组件职责、线程模型） | `skill/arhud-architecture` | 随代码演进**持续更新** |
| "目标怎么设计"（路线 B、多屏等方案细节） | `skill/arhud-*-arch` | 阶段完成后更新进度标记 |
| "为什么这么做"（一次性决策 + 否决方案） | `docs/adr/` | **只增不改**，被取代时改状态指向新 ADR |
| "这次改了什么"（变更记录、排障教训） | `docs/devlog/` | 每次代码变更一篇，用模板 |
| "接下来做什么"（backlog、优先级） | `skill/arhud-improvement` | 遗留项必须登记，不静默丢弃 |
| "怎么干活/怎么验证"（流程、命令、坑） | `docs/process/` | 流程变更时更新 |

---

## 阅读顺序（agent 新会话 onboarding）

1. `AGENTS.md` — 工程速览（2 分钟）
2. `docs/process/dev-workflow.md` — 按流程干活（变更分级决定流程深度）
3. 与任务相关的 `skill/` 章节 + `docs/adr/` 对应决策
4. 动手前查 `docs/process/verification.md`（命令与已知坑）
5. 相关模块最近一篇 `docs/devlog/`（避免重复踩坑）

## 同步义务（改代码必须同步文档）

| 变更类型 | 必须更新 |
|---|---|
| 任何代码变更 | `docs/devlog/` 新增一篇（用模板，含排障记录） |
| 架构/接口契约决策 | `docs/adr/` 新增 ADR + 相关 skill 段落 |
| 阶段完成（如 B3-P → B4） | `skill/arhud-improvement` 状态列 + `skill/arhud-architecture` |
| 目录/构建选项/编码规范变化 | 根 `AGENTS.md` 对应段落 |
| 新增遗留问题 | `skill/arhud-improvement` 治理清单 |
