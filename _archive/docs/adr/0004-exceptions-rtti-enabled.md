# ADR-0004：启用异常与 RTTI（移除过度禁用）

| | |
|---|---|
| 状态 | 已接受（已决策**不回退**，见 `skill/arhud-improvement` §四） |
| 日期 | 2026-08（追溯记录；具体日期未留档，晚于初版 /EHs- /GR- 约束、早于 B2 2026-08-22） |
| 关联 | `skill/arhud-architecture` §四「异常/RTTI 启用」、`cmake/build.cmake`、`tests/CMakeLists.txt` |

## 1. 背景与问题

初版按车规硬实时假设全面禁用异常与 RTTI：
MSVC `/EHs- /GR-`、GCC/Clang `-fno-exceptions -fno-rtti`。实际暴露两个问题：

1. **过度设计**：AR HUD 非硬实时系统，异常/RTTI 的最坏情形开销在此场景不可感知；
   禁用带来的全是工程摩擦（第三方库适配、错误传播繁琐）
2. **测试基建阻塞**：GoogleTest 需要异常 + RTTI，`ARHUD_BUILD_TESTS` 无法落地（B1 兜底策略依赖测试先行）

## 2. 决策

**启用异常与 RTTI，移除全部禁用标志。**

- `cmake/build.cmake` 仅保留 `/W3`、`/utf-8`、`_CRT_SECURE_NO_WARNINGS`（MSVC）与
  `-Wall -Wextra -Wpedantic`（GCC/Clang）
- 测试目标按需显式启用 `/EHsc /GR`（gtest 要求），引擎库不再反向禁用
- **其余车规约束全部保留**：无 iostream、无 `malloc/free`、无 `printf/sprintf`、无裸 `new/delete`、
  无 `goto`、无 COW 容器、显式生命周期管理、错误经 `Error` 枚举返回

## 3. 已否决方案

| 方案 | 否决理由 |
|---|---|
| 维持禁用 | 过度设计 + 阻塞 gtest；HUD 非硬实时 |
| 折中：引擎禁用、测试启用（双轨编译标志） | 同一编译单元两套异常语义易出错；维护成本 > 收益 |
| `/EHs-` 改 `/EHs-c-` | 行为无差异，不值得改（已决策不做） |

## 4. 后果

- **正面**：GoogleTest 落地（当前 6/6）；第三方库集成顺畅；错误处理统一走 `Error` 枚举不受影响
- **负面/代价**：理论上的异常展开开销（实际场景不可感知）；失去编译期"零 throw"保证
- **中性约束**：**引擎代码约定零 throw**（不主动抛异常），由代码评审维持；根 `AGENTS.md` 的 Forbidden
  列表相应更新（2026-09-19 同步：移除 exceptions/RTTI 禁令，保留其余禁令）

## 5. 验收

- `cmake/build.cmake` 无 `/EHs-`、`/GR-`、`-fno-exceptions`、`-fno-rtti` ✅
- `ARHUD_BUILD_TESTS=ON` 构建通过、ctest 6/6 绿 ✅
