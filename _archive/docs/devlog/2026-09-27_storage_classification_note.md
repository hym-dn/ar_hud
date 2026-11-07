# 开发日志：补充 storage/ 分类说明（资源 Storage vs 状态 Storage）

- 日期：2026-09-27
- 范围：`skill/arhud-architecture/SKILL.md`、根 `AGENTS.md`
- 依据：代码评审提问「ItemStorage 和其他 storage 并不一致，放在一起合适吗？」；
  变更分级 **A 类**（纯文档/注释，零行为变化、零编译影响）

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 五层树：`storage/` 行标注两类容器；`ItemStorage` 行补「CPU-only / 无 Initialize(rd) / Engine 持有」 | `skill/arhud-architecture` §一 |
| §3.2 组件表：`ARHudCompositor`「持有所有 Storage」→「持有 5 个资源 Storage；不含 ItemStorage」 | `skill/arhud-architecture` §3.2 |
| §3.2 组件表：`Storage` 行拆为「资源 Storage（5）」+「状态 Storage（1）」 | `skill/arhud-architecture` §3.2 |
| §3.2 表后新增「`storage/` 分类说明」引用块（两类定义 + 唯一硬约束） | `skill/arhud-architecture` §3.2 |
| 目录树 `storage/` 段标注两类容器与各自持有者 | 根 `AGENTS.md` |

## 2. 关键实现决策

1. **投放到 architecture skill，而非新建 `storage/README`**：`docs/README.md`「职责边界」规定
   "架构现状 / 组件职责 / 线程模型"归 `skill/arhud-architecture`；且 `item_storage.h` 头注释
   已有完整「定位（区别于其他 Storage）」表（L11-L33），另有 devlog `2026-09-19_item_storage_placement_note.md`。
   新建 `storage/README` 只会与头注释重复，故只补**层级/目录层面**缺失的那一段。
2. **只补分类说明，不动 ownership / 不拆目录**：目录拆分或改名属 **C 类**（分层/契约变更），
   须方案先行（skill 章节 + ADR），不在本次范围。
3. **以代码为准修正既有失真**：原文档「ARHudCompositor 持有所有 Storage」与实现不符
   （`compositor.h` 无 ItemStorage；`ItemStorage` 为 `ARHudEngine::Impl` 值成员），
   按实现改为「持有 5 个资源 Storage」。§一 storage 行原漏列 ItemStorage，一并补齐。

## 3. 排障记录（可选，踩坑必留档）

- 本次为纯文档编辑（`SearchReplace`），未使用批量脚本，无 BOM 风险；仍按下文验收做首字节抽查。
  教训沿用：**任何中文文档写回后都抽查首字节**（`verification.md` 坑 #3）。

## 4. 验收结果

- 构建：未改任何源码，A 类文档变更，无需构建
- 单测：未改代码，无需复跑（基线 25/25）
- demo：不适用
- 编码抽查：`AGENTS.md`、`skill/arhud-architecture/SKILL.md` 首字节无 BOM，中文未乱码

## 5. 遗留（登记去向：skill/arhud-improvement）

- [ ] `skill/arhud-architecture` 仍残留旧命名：§一 platform 行含 `view`、§3.4 含 `IView`，
  且 §3.4 实现列表未含 `FramePresenterWin32`/`GLContextManagerWin32`（应为 `IFramePresenter`/
  `frame_presenter.h`）—— 本次新发现偏差，拟并入下一批 A 类文档修正
- [ ] （延续）demo `main.cpp` 乱码注释清理（P3 #365）
- [ ] （延续）`conanfile.py` `ConanInvalidConfiguration` 未导入（B6，代码缺陷）
- [ ] （新增候选）`storage/` 目录级拆分（资源 Storage 与状态 Storage 分目录）—— 仅当代码继续长大时，C 类，须 ADR