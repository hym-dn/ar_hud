# 完成定义（Definition of Done）

> 任何代码变更在宣告"完成"前逐项打勾。任何一项不满足 → 回到对应流程阶段修复，不得带病归档。
> 分级标准见 `dev-workflow.md` §0（A 小修 / B 修复功能 / C 结构架构）；标注 [A] 的项对 A 类小修可豁免。

---

## A. 代码

- [ ] 全量构建零 error，/W3 零新增 warning
- [ ] 新增/删除/更名源文件已同步对应 `CMakeLists.txt` **显式列表**（无 `file(GLOB)`）
- [ ] 遵循根 `AGENTS.md` 编码规范（命名/缩进/花括号/include 顺序/`override`）
- [ ] 公共 API 有 Doxygen（`@brief` / `@param[in,out]` / `@return`）；实现侧注释遵循 `@brief …（实现侧）` 分节惯例
- [ ] [B] 最小改动：无任务外顺手重构；发现的问题已登记 `skill/arhud-improvement`

## B. 行为

- [ ] ctest 全绿（当前 25/25）
- [ ] 相关 demo 冒烟干净退出、日志零 ERROR（超时按 帧数 × 帧时长 计算）
- [ ] [B] 行为变化可观测/可验证（新增测试，或 devlog 中明确记录验证步骤与结果数字）

## C. 治理

- [ ] `verification.md` §5 治理 grep 全部零命中
- [ ] [B] 线程归属检查：新增对象已标注所属线程（主 / 渲染 / present），主线程零直触 storage/renderer
- [ ] [B] 资源生命周期：新资源有 Free 路径且走 RD 延迟销毁语义；无裸 `new`/`delete`（容器与 RAII 除外）
- [ ] 中文注释抽查无乱码（编码事故回归检查）

## D. 文档同步

- [ ] devlog 已写（模板 `docs/templates/devlog-template.md`；**踩坑必留档**排障记录）
- [ ] [C] 架构/接口决策已写 ADR（`docs/adr/`，模板 `docs/templates/adr-template.md`）
- [ ] [B] `skill/arhud-improvement` 状态列已更新；遗留项已登记（不静默丢弃）
- [ ] [B] 目录/构建选项/编码规范变化已同步根 `AGENTS.md`
- [ ] [C] `skill/arhud-architecture` 现状描述与代码一致

## E. 产物卫生

- [ ] `shader_cache/` 新产物未入库；根目录无遗留 `*.log` / 临时文件
