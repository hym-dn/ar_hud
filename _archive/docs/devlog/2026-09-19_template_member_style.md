# 开发日志：ItemStorage 私有模板成员风格统一（定义集中至 cpp）

- 日期：2026-09-19
- 范围：servers/rendering/storage/item_storage.h/.cpp
- 依据：变更分级 A 类（纯代码搬移，零行为变化）；源于代码带读中"模板实现能否放 cpp"的讨论

## 1. 变更总览

| 项 | 落点 |
|---|---|
| `FindItem`（可变/常量）、`FindItemAndMark` 三个模板成员：完整定义从头文件移至 cpp（头文件保留声明 + Doxygen） | `item_storage.h`（−60 行）/ `item_storage.cpp`（+45 行） |

## 2. 关键实现决策

1. **合法性前提**：模板定义放 cpp 要求"定义位于本 TU 全部调用点之前"且"无跨 TU 实例化需求"。
   `FindItem/FindItemAndMark/SetParentImpl/ChainContains` 均为 private helper，调用点
   全部在 item_storage.cpp 内，隐式实例化即满足链接；外部误用会得到链接错误——恰是
   私有实现细节想要的封装效果。
2. **统一后全类风格**：私有模板成员一律"声明在头文件 + 定义在 cpp"（SetParentImpl /
   ChainContains 昨日新增时即为此风格）；非模板私有方法本就在 cpp。
3. **收益**：ItemStorage.h 减少 ~60 行模板体；改查找/环检测逻辑不再触发 servers 全模块重编。

## 3. 验收结果

- 全量构建零 error / 零 warning
- 单测 25/25 全绿（零行为变化）
- phase4_text 冒烟 exit=0 / 零 ERROR / 干净退出

## 4. 遗留

- [x] 无。
