---
name: "arhud-coding-style"
description: "ARHud 工程编码风格约束。以 Google C++ Style Guide 原教（google.github.io/styleguide/cppguide.html）为唯一基线，逐条裁决本工程执行口径与存量代码迁移策略；命名层原已对齐，格式层（80 列/2 空格/附尾大括号/不缩进 namespace/Type* p/include guard/无 p_ 前缀）按原教执行。撰写或修改任何 .h/.cpp、做代码评审、格式化为 clang-format 建档时调用本 skill。"
---

# ARHud 编码风格约束（Google C++ Style · 原教口径）

> **裁决记录（2026-11-07）**：本工程编码风格**全部采用 Google C++ Style Guide 原教条款**。
> 与 Google 条款不一致的存量代码（Godot 风味格式）**不再视为合法风格**，按 §1.3 迁移策略逐步重排。
> 本文件是原教的**本工程化誊本 + 裁决表**；两者冲突时以本文件为准，本文件未覆盖处以 Google 指南原文为准。
> **注释体系为明示修订**（2026-11-08）：采用 Doxygen 格式 + 中文（§7），原教注释章节属项目自定范围，
> 不构成违基线；其余格式条款仍严格原教。
> `docs/skill/arhud-design/SKILL.md` §10 保留浓缩表并指向本文件；§7.9/§7.11 历史代码样例中的 `p_` 前缀、Allman
> 大括号等属**历史起草风格**，不视为风格豁免——冻结面指参数的**类型/顺序/语义**，不含格式与参数名。

## 0. 何时使用本 skill

- 新建或修改任何 `.h` / `.cpp` 文件；
- 代码评审（style 层）；提交前自查；
- 配置 clang-format / IDE；
- 裁决"这行旧代码要不要顺手改齐"。

**优先级**：本 skill > `docs/skill/arhud-design/SKILL.md` §10 > Google 指南原文 > 周边存量代码。
唯一例外：设计 skill（`docs/skill/arhud-design/SKILL.md`）§7.11 **冻结接口规格**（虚表形状、`PushBound` 签名语义）——触及其内容先回改文档。

---

## 1. 与原教的差异：存量现状与迁移策略

### 1.1 存量代码的六大格式偏离（全部按原教重排，不视为合法风格）

| # | 项目 | Google 原教（合规目标） | 存量现状 | 迁移方式 |
|---|---|---|---|---|
| G1 | 缩进 | **2 空格**步进，只用空格 | 4 空格步进（约 97%） | clang-format 一键 |
| G2 | namespace 体 | **不缩进**，`} // namespace xxx` 收口注释 | 缩进 4 | clang-format 一键 |
| G3 | 大括号 | **一律附尾**（attach/K&R）：类、函数、namespace、`if/for` | 类型/函数/namespace 用 Allman | clang-format 一键 |
| G4 | 指针/引用 | `Type* p`、`const Type& r`（**符号紧贴类型，无空格**） | `Type *p`（占约 93%） | clang-format（`PointerAlignment: Left`） |
| G5 | 行宽 | **80 列** | 文档 100 列、代码混用 | 重排换行（人工确认语义断行处） |
| G6 | 头文件保护 | `#ifndef <相对路径大写>_H_` **include guard**；`#pragma once` 属非标准扩展，**禁用** | `#pragma once` | 逐文件替换（格式化提交中批量做） |

另有两处**写法层**偏离：

| # | 项目 | Google 原教 | 存量现状 | 迁移方式 |
|---|---|---|---|---|
| W1 | 形参命名 | `snake_case`，**无前缀** | `p_` 前缀（Godot 惯例） | 改动到的函数顺手改名；参数名不参与重载决议，无 ABI/接口影响 |
| W2 | 注释体系 | ~~Google `//` 自由式~~ → **本工程裁决（2026-11-08）：Doxygen 格式 + 中文**（§7） | 存量中文 Doxygen 头 ✓ 合规；迁移期英文 `//` 注释随改动转 Doxygen 中文 | 不批量强改，滚动收敛 |

### 1.2 命名层现状 = 原教，零迁移

以下**已经合规**，写新代码时保持即可：类型/函数 PascalCase、变量 snake_case、类成员 `snake_case_`、
结构体成员无后缀、常量与枚举值 `kXxx`（含 `enum class`）、宏 `ARHUD_UPPER_SNAKE`（原教要求全大写+工程前缀，
例 `MYPROJECT_ROUND(x)`）、namespace snake_case、文件名全小写加下划线、include 顺序五段式、整型别名映射 `<cstdint>`。

### 1.3 迁移纪律（绞杀式重排，防格式化海啸）

1. **新/新建文件**：严格原教，零例外（模板见附录 A）。
2. **存量文件**：
   - 改动某函数/某段 → 顺手把**所触动的行**改齐；
   - 单文件改动行数 ≥ 30% → **整文件重排**（clang-format + guard/pragma 替换）；
   - 禁止纯格式化顺手扩散到无关文件；
   - 全库级重排按模块分目录做**独立提交**（提交前缀 `Style`，不与行为变更混提），保证 diff 可复核。
3. `.clang-format`（仓库根）是格式层的**机器裁决**；人审只兜工具覆盖不了的条款（§4.3、§5、§6、§7）。
4. Google 指南自身原则 *"Be consistent with existing code"* 在本工程**让位于**本裁决表——这是明示决定，不是疏漏。

---

## 2. 文件、头与包含

- **文件配对**：每个类一组 `xxx.h` / `xxx.cpp`（本工程保留 `.cpp` 扩展名——原教允许"跟随工程既有构建约定"，不作批量改名）。
- **文件名**：全小写 + 下划线（`command_queue.h`）；可含 `-`，但本工程统一 `_`。
- **自包含**：头文件必须可独立编译；`#include` 你所用到的，不依赖传递包含。
- **include guard**（G6 裁决，替代 `#pragma once`）：

  ```cpp
  #ifndef ARHUD_SERVERS_RENDERING_COMMAND_QUEUE_H_   // 相对工程根的大写路径
  #define ARHUD_SERVERS_RENDERING_COMMAND_QUEUE_H_
  ...
  #endif  // ARHUD_SERVERS_RENDERING_COMMAND_QUEUE_H_
  ```

- **包含顺序**（五段，段间空行）：
  ① 本文件关联头 → ② C 系统头（`<stdlib.h>` 类，少用）→ ③ C++ 标准库（`<atomic>`…）→
  ④ 第三方（`<glm/glm.hpp>`、`glad/…`…）→ ⑤ 本工程头。
  工程内一律写**相对模块根的路径**（如 `"template/rid.h"`、`"storage/item_storage.h"`），禁止 `./`、`../` 前缀
  （与 `cmake/dir.cmake` 全局 include 机制一致）。
  > **工具裁决（2026-11-07 实测）**：clang-format 的 `IncludeBlocks: Regroup` + 排序会按字典序重排段内
  > 头文件，曾把 `<psapi.h>` 排到 `<windows.h>` 之前导致 SDK 编译失败。本工程 `.clang-format` 已设
  > `SortIncludes: false` + `IncludeBlocks: Preserve`——**五段顺序与段内次序由作者维护**，评审人工把关；
  > 有先后依赖的 SDK 组合（windows.h 先行）必须在旁加一行英文注释声明。原教对 include 排序无硬数字要求
  > （仅规定分组），此裁决不违基线。
- **`.cpp` 内部实现实体**用匿名 namespace 或 `static` 给内部链接；`.h` 中**禁止**。收口注释：

  ```cpp
  namespace {
  ...
  }  // namespace
  ```

- **`#include` <前置声明>**：优先 include；仅在打破循环依赖或明显编译耗时场景前置声明，且注释写明理由。
- 头文件中不写 `using namespace`（原教永久禁止 using-directive）；头里的公开 `using` 别名视同公共 API（受别名规则约束，见 §6.7）。

## 3. 格式规则（工具可裁决部分）

- 行宽 **80 列**（G5）。URL/长字符串字面量等不可断者例外（原教明列例外场景）。
- **只用空格**，2 空格步进（G1）。
- 大括号**一律附尾**（G3），控制流语句即使单行也必须带括号：

  ```cpp
  if (condition) {
    DoSomething();
  } else {
    Other();
  }
  ```

- namespace **不缩进**（G2），结尾加 `} // namespace arhud`。
- 访问标号相对类体**缩进 1 空格**，成员正常 2 空格（原教 Class Format "Note the 1 space indent!"）：

  ```cpp
  class MyClass : public OtherClass {
   public:
    MyClass() {}
    explicit MyClass(int var);
    ~MyClass() {}

    void SomeFunction();

   private:
    int some_var_ = 0;
  };
  ```

- 指针/引用符号**紧贴类型、右侧留一个空格**（G4）：`char* text;` `const std::string& str;` `int* GetPointer();`
  禁止 `char * text`、`char *text`。
- **函数声明/定义**：返回类型与函数名同行；一行放不下则全部参数换到下一行 4 空格缩进，或每参数一行并对齐：

  ```cpp
  Error RenderServer::TextureInitialize(RID rid, const char* name, const uint8_t* data,
                                        uint32 width, uint32 height) {
    ...
  }
  ```

- **构造函数初始化列表**：能一行则一行；放不下就**在冒号前换行、4 空格缩进、成员逐行对齐**：

  ```cpp
  MyClass::MyClass(int var)
      : some_var_(var),
        some_other_var_(var + 1) {
    DoSomething();
  }
  ```

- **switch**：非枚举条件必须带 `default`；枚举覆盖全值（编译器告警兜底），default 不应到达时按错误处理。
  case 体缩进 2、用 `{}` 包裹、`break` 置底；跨 case 贯穿必须 `[[fallthrough]];` 标注。
- 预处理器指令**永远顶格**（即使在缩进代码块内）——`#if/#endif` 不缩进。
- `return result;` 不加多余括号；`return (cond_a && cond_b);` 这类为可读性加括号是允许的。
- lambda：参数/体格式同普通函数；引用捕获**不留空格** `[&x]`；短 lambda 可作实参内联。
- 花括号初始化列表按"函数调用"格式换行对齐。
- 水平空白：关键字与 `(` 之间一个空格（`if (`），`(` 内侧不留；逗号后一个空格；行尾禁止空白。
- 宏定义优先转 `constexpr` 常量或内联函数（§6）；确需多行宏时，续行反斜杠在 80 列体系内对齐。

## 4. 命名规则（原教全表 + 工程映射）

### 4.1 一览表（✅ = 原教与工程现状一致；🔁 = 存量迁移项）

| 元素 | 原教规则 | 示例 | 状态 |
|---|---|---|---|
| 类/结构体/枚举/类型别名 | PascalCase，无下划线 | `CommandQueue`、`DataFormat` | ✅ |
| 函数/方法 | PascalCase | `FlushIfPending()`、`TextureCreateFrom()` | ✅ |
| 访问器/修改器 | 可用变量式 | `count()` / `set_count()`（工程 `GetXxx/SetXxx` 亦合规） | ✅ |
| 局部变量/参数 | snake_case，**无前缀** | `frame_count`、`width` | 🔁（废除 `p_`，见 W1） |
| 类数据成员 | snake_case + 尾下划线 | `queue_`、`write_pos_` | ✅ |
| 结构体数据成员 | snake_case，**无**尾下划线 | `CommandDataSpan.data` | ✅ |
| 常量（含 `constexpr`、静态常量成员、模板非类型参数量） | `k` + 混合大小写 | `kMaxCommandSize`、`kCapacity` | ✅ |
| 枚举值（scoped/unscoped 皆然） | 按常量命名 `kXxx`，**禁宏式全大写** | `Error::kOutOfMemory` | ✅ |
| namespace | snake_case，全局唯一、以工程名起头 | `arhud`、`arhud::rendering` | ✅ |
| 宏 | **全大写 + 下划线 + 工程前缀** | `ARHUD_ASSERT(x)` | ✅ |
| 模板参数 | 类型参数按类型名（PascalCase）；非类型参数按变量/常量名 | `template <typename StorageType, typename MethodType, typename Arg0>` | 🔁（存量 `S/M/A0` 随手改） |
| 文件 | 全小写 + `_` | `render_server_default.cpp` | ✅ |
| include guard | 工程相对路径全大写 + `_H_` | `ARHUD_CORE_TEMPLATE_RID_H_` | 🔁（随 G6） |
| 测试 | 用例/夹具 PascalCase | `TEST(CommandQueueTest, PushBoundRoundtrip)` | ✅ |

### 4.2 例外（原教明文允许，工程沿用）

- `#include <...>` 的第三方符号、C API 符号不强改；
- 单字母循环变量 `i/j/k` 在短作用域合法；
- 缩写与专名（GL、SDF、RID）按可读性定，全库一致即可。

### 4.3 评审必查的三条语义命名

1. **布尔名说事实**：`HasConsumer()`、`is_ready_`（不取 `flag/status` 类含糊名）。
2. 单位后缀强制：`width_px`、`size_bytes`、`timeout_ms`（车规可读性，工程惯例延续）。
3. RID 动词语义固定：`XxxAllocate`（同步出壳）/ `XxxInitialize`（渲染线程填壳）/ `XxxFree`，不造新动词。

## 5. 类、函数与 OO 设计

- **struct 仅用于被动数据**（无不变量、成员全公开）；带行为或不变量的一律 `class`。
- 类数据成员 `private`（常量除外）；访问标号顺序 `public → protected → private`，空的省略。
- **类内声明顺序**（原教 Declaration Order）：类型/别名 → 静态常量 → 工厂函数 → 构造/赋值 → 析构 →
  其余函数 → 其余数据成员。
- **单参构造与转换运算符一律 `explicit`**（除非拷贝构造语义）。
- 禁拷贝/移动用 public 区 `= delete`（工程宏 `ARHUD_DISABLE_COPY_MOVE` 等价可用，二选一保持文件内一致）。
- **不在构造函数里做可失败工作**：原教禁构造抛错且本引擎禁异常 → 统一 `Initialize()/Init()` 返回 `Error`
  （本工程的合规形态即"工厂 + Init"，`RenderGlobals`/`RenderingDevice` 现状已合规）。
- 继承：**优先组合**；只用 public 继承；实现继承至多一个基类，其余必须是**纯接口**
  （无状态、全 public 纯虚 + `virtual ~接口() = default` 语义）；抽象基禁止多态析构以外的实现。
- **虚函数上的重载签名必须一致**（含默认实参禁止——虚函数不得带默认实参，原教明文）；
  非虚函数默认实参仅当"任何调用点取到的默认值都相同且语义自然"。
- 函数宜短：**超过 40 行即应考虑拆分**（原教无硬上限，给出 40 行提示线）。
- 头内联函数：仅 trivial/性能关键且**足够短**（≤10 行量级）才在头里定义；类体内不放大方法体。
- 输出优先走**返回值**；确需输出参数：非可选输出/输入输出用**引用**，可选输出用非 const 指针；
  可选按值输入用 `std::optional`，可选输入对象用 `const T*`（可空才用指针——指针即"可为 null"信号）。
- 参数顺序：**输入在前、输出在后**（原教 Function Parameter Ordering）。
- 运算符重载节制：只做与本类型语义一致的（`==`、`<`、`=`、流输出仅"值类型"）；禁止用户字面量。
- `friend` 允许但克制，与被Friend类同文件为佳。
- 静态成员函数包不造（"非成员自由函数放 namespace 即可"）；除非有 policy/trait 理由。
- 右值引用只用于：移动构造/移动赋值、转发引用模板、按值形参转移所有权；其余场景禁 `&&` 形参。

## 6. 语言特性与库使用

- **整型**：内建只用 `int`；需要定宽用 `<cstdint>` 的 `int16_t/uint32_t/int64_t`。
  工程 `typedefs.h` 的 `uint8/16/32/64、float32/64` 即其别名层，资源相关代码**禁裸 `int/unsigned/long`**
  （工程加严，原教亦禁 `short/long/long long`）；`unsigned` 仅在位运算/模数语义处使用。
- **空值**：指针用 `nullptr`，字符用 `'\0'`；整型零用 `0`。禁 `NULL`。
- **类型推导（auto）**：仅在"让读者更明白或更安全"时使用；禁为省字而 `auto`；
  函数返回类型推导仅用于 lambda 与完美转发模板；结构化绑定只在字段名即文档时用。
- **转型**：`static_cast<T>(v)` 或算术转换用 `T{v}`；禁 C 风格 `(int)x`；
  `const_cast` 只为穿过老 API，注释理由；`dynamic_cast`/`typeid` 属 RTTI，**引擎禁用**（原教 Avoid RTTI；
  ADR-0004 仅放开编译开关，见 §8.2 冲突登记）。
- **异常**：原教 "We do not use C++ exceptions" → **引擎代码禁 `throw`/`try`/`catch`**，错误一律 `Error` 返回；
  构造失败走 Init。与编译开关（ADR-0004 启用异常/RTTI）的张力登记于 §8.2。
  `noexcept`：移动操作/析构/swap 标 `noexcept`。
- **所有权与智能指针**：单一所有权用 `std::unique_ptr` + `std::make_unique`；
  共享所有权允许但须注释理由；裸指针 = **非拥有**观察者。
  工程 `Ref<T>`（侵入式计数，Godot 机制）是**项目补充件**，等价 Google 的 shared 语义，
  用于 S1 `Ref<Buffer>` 参数体系（设计 skill D3），不视为违反原教。
- **全局/静态存储期对象**：必须**平凡可析构**（原教硬规）；经验式"该声明单独看能写 constexpr"即合规。
  `RenderGlobals` 的 `static inline Xxx* x = nullptr;` 全为指针——**合规范例**；
  动态初始化仅限受限场景并注释；函数内 static 允许（线程安全初始化）。
- **`sizeof`**：优先 `sizeof(varname)` 而非 `sizeof(Type)`。
- **前置自增** `++i`；除非需要后置语义。
- **lambda**：可逃逸当前作用域的必须**显式捕获**（禁 `[&]`/`[=]` 默认捕获跨线程/跨回调）；
  线程池/上屏线程里的捕获尤其照此执行。
- **`using` 优于 `typedef`**（原教明文，新代码）；头中公开别名受"别名规则"约束：须有存在理由并文档化。
- **禁 using-directive**（`using namespace foo;` 永久禁止）；禁 inline namespace。
- **流与 printf**：Google 允许 streams、不喜 printf；本工程**两者皆禁**（`iostream`/`printf` 在禁止清单，
  格式化统一 `String::Format`），属工程加严，见 §8.1。
- **C++20 特性**：默认 C++17（`ARHUD_ENABLE_CXX20` OFF）；coroutines/modules/concepts 属原教按需条款，
  本工程未启用即不引入。
- **编译器扩展**（`__declspec/__attribute__`）：只允许出现在 `typedefs.h` 的宏包装层内
  （`ARHUD_ALIGN_AS/ALWAYS_INLINE/...`），业务代码不裸写。

## 7. 注释（Doxygen · 中文）★ 本工程裁决修订

> **裁决（2026-11-08）**：注释体系采用 **Doxygen 格式 + 中文**——这是工程对 Google 原教的
> 明示修订（原教注释章节本就允许"跟随项目约定"，且其 `//` 自由式非硬规）。
> 本节取代此前"新代码 `//` 英文"条款；原教其余条款不受影响。

### 7.1 语言与编码

- **注释一律中文**（与存量代码统一；术语保留英文原文，如 render pass、staging、backpressure、
  RID 分裂，不硬译）。
- **代码本体（标识符、字符串字面量）仍纯 ASCII**；中文只允许出现在注释中。
- 文件编码 **UTF-8**（MSVC `/utf-8` 已全局开启）；禁止 GBK/BOM 混入。
- 存量英文 `//` 注释（2026-11-07 迁移期产物）：改动到即顺手转 Doxygen 中文，不批量强改
  （纪律同 W1/W2 滚动收敛）。

### 7.2 文件头（每个 .h/.cpp 必配）

```cpp
/**
 * @file mesh_storage.cpp
 * @brief 网格资源 Storage——RID→Surface 账本与 GPU 缓冲生命周期实现。
 *
 * @par 职责边界
 *   - 只做簿记与 RD 资源转发，不含渲染逻辑（renderer → storage 单向）。
 *   - RID 分裂契约（design skill §7.11 F）：Allocate 同步出壳，Initialize 渲染线程填壳。
 *
 * @author yameng.he
 * @version 1.1
 * @date 2026-11-08
 * @copyright Copyright (c) 2026 ARHud Project
 * @ingroup rendering
 */
```

- 必填：`@file`、`@brief`（一句话职责）、`@copyright`；`@par` 段按需（设计要点/契约/状态表）。
- `@author/@version/@date`：本工程**保留**（车规追溯要求，覆盖原教"新文件不加 author line"条款）；
  重大改动时更新 `@version/@date`，可加多个 `@date` 行记录修订。
- `@ingroup` 取模块名：`core / platform / drivers / rendering / scene / app / tests`。

### 7.3 类 / 结构体

```cpp
/**
 * @brief 每屏双槽帧图像队列——渲染线程→上屏线程的唯一交接通道（零拷贝语义）。
 *
 * @par 状态机
 *   FREE → WRITING → READY → COMMITTED → ON_SCREEN → FREE
 *
 * @par 线程模型
 *   生产=渲染线程，消费=上屏线程；背压保证每屏最多 1 帧在飞。
 */
class FrameImageQueue {
```

- 类注释必含 `@brief`；**不变量、线程约束、所有权语义为必写项**（车规评审重点），
  用 `@par` 分段。跨线程成员注明"哪个线程读写、发布次序"。

### 7.4 函数

```cpp
  /**
   * @brief 尝试领取一个空闲槽位进入写入态（FREE→WRITING，非阻塞）。
   * @return 槽位索引；无空槽时返回 UINT32_MAX。
   * @retval UINT32_MAX 两槽均被占用（背压生效，调用方应跳过本轮）。
   * @pre 仅渲染线程调用（与 MarkReady 同线程）。
   * @note 不产生阻塞——与 free_sem_ 的计数语义解耦，见 §3.8 背压设计。
   */
  uint32 TryAcquireFree();
```

- **声明处注释说用途**：`@brief` + `@param[in]/[out]/[in,out]` + `@return`/`@retval` +
  `@pre/@note/@see`——非显然才写，但公开 API 的 `@brief/@param/@return` 三件套必配。
- **定义处注释说做法**（算法要点、坑、偏离），无新信息则不重复声明注释。
- 简单访问器/一行函数：行尾 `///<` 即可，不强制块注释。

### 7.5 成员 / 枚举 / 常量

```cpp
  std::atomic<uint32> dirty_screen_mask_{0};  ///< ≤32 屏脏位图；Mark* 是唯一置位入口
  enum class PresentMode : uint8 {
    kReadback = 0,  ///< 回读上屏：CPU 像素 → GDI/EGL 呈现
    kSwap = 1,      ///< Swap 上屏：零回读零拷贝（Windows 方案 B / 生产形态）
  };
```

- 数据成员、枚举值一律行尾 `///<`（一句话语义；语义复杂者升级块注释）。

### 7.6 标签与格式纪律

- 白名单标签：`@brief @param @return @retval @note @pre @post @warning @see @par
  @deprecated @author @version @date @copyright @ingroup @code @endcode @b @c @ref`。
  不使用原教没有、Doxygen 生僻的标签（`@implements/@class` 之类）。
- 块注释统一 `/** ` 起、` * ` 续行、` */` 收尾；行内注释 `//` 后空一格。
- 缩进随所在层级；`@brief` 不换行写完一句。
- 图/表用 `@code/@endcode` 包裹 ASCII 图（clang-format 不重排块注释，安全）。
- **TODO**：`// TODO(责任标识): 中文说明`——写 bug ID、人名或文档链接；无主 TODO 不许入库。
- 禁止注释掉的死代码入库（git 是历史）；临时调试注释提交前清除。

### 7.7 与 clang-format 的配合

clang-format（本工程配置）**不改注释内容**，但会保留 `/** */` 块原样——Doxygen 块的
排版（对齐、续行 ` * `）由作者负责；评审用 `git diff` 检查注释与代码同步性：
**改行为必改注释，注释与代码矛盾按缺陷处理**。

## 8. 项目补充条款（Google 之外的工程硬约束，评审同查）

### 8.1 加严清单（与 Google 不冲突，仅收紧）

- 禁 `iostream`、`printf/sprintf`、裸 `new/delete`（用 `ARHUD_NEW/DELETE` 或智能指针）、`malloc/free`、`goto`；
- CMake 禁 `file(GLOB)`；新源文件手工进模块 `CMakeLists.txt` 显式列表；
- 错误一律 `Error` 枚举返回；
- 分层三硬规（契约归位 / `renderer→storage` 单向 / drivers 不倒挂）见 `docs/skill/arhud-design/SKILL.md` §1.4；
- 跨线程数据：禁裸指针入命令；`PushBound/PushBoundBlob` 参数 `static_assert(trivially_copyable)` 守门；
  变长载荷走 blob 或 `Ref<Buffer>`（S1 后）。

### 8.2 与原教的正式冲突登记（保留，勿在评审中误伤）

| 冲突 | 原教 | 本工程裁决 | 依据 |
|---|---|---|---|
| 异常/RTTI **编译开关** | 禁用（-fno-exceptions、不写 dynamic_cast） | MSVC 保持启用，**引擎代码仍不使用** | ADR-0004（集成第三方/工具链现实）；行为层仍按 §6 |
| `.cc` 扩展名 | 推荐 | 维持 `.cpp` | File Names 条款自留"follow project convention"空间 |
| Doxygen + 中文注释 | 注释体系无此规定（`//` 自由式，跟随项目） | **本工程裁决**：Doxygen 格式 + 中文（§7，2026-11-08） | 项目自定范围，不违基线 |
| `p_` 前缀、`Type *p`、Allman、`#pragma once`、100 列、namespace 缩进 | — | **均判违规，按 §1.3 迁移**，非豁免 | 本次裁决 G1–G6/W1 |

## 9. 工具与落地

- **`.clang-format`（仓库根，已提供）**：`BasedOnStyle: Google` + 逐条钉死 G1–G4/80 列/指针左贴/namespace 零缩进。
  用法：`clang-format -i path/to/file.cpp`（VS 扩展 / clangd 自动读取；CI 可 `--dry-run -Werror`）。
  clang-format 管不了：include 段序（排序已禁用，见 §2）、`#pragma once`→guard（G6）、`p_`（W1）、注释文体（§7）、声明顺序（§5）——人审兜底。
- **cpplint（可选）**：`pip install cpplint; cpplint --filter=...`——对 `.cpp` 扩展、工程宏前缀有已知误报，
  与本 skill 冲突处**以本 skill 为准**。
- IDE：缩进空格 2/80 列标尺；禁止"制表符"；格式化提交一律独立（§1.3-2）。

## 10. 提交前自查清单

1. `clang-format --dry-run --Werror <改动文件>` 零告警？
2. 新文件：guard 齐全、Doxygen 中文文件头（@file/@brief/@copyright/@author/@date/@ingroup）？
   公开 API 三件套（@brief/@param/@return）齐？不变量/线程/所有权注释写了？
3. 有无残留 `p_` / `Type *p` / 缩进 namespace 出现在**你改动过的行**？
4. 头文件是否自包含、include 五段有序、无 `using namespace`？
5. 新单参构造 `explicit`？虚函数无默认实参？构造无重活？
6. 全局对象平凡可析构？跨线程 lambda 显式捕获？命令参数 trivially_copyable？
7. TODO 带责任标识？
8. 纯格式化改动是否已拆独立提交？

---

## 附录 A · 新文件模板（.h）

```cpp
/**
 * @file foo_bar.h
 * @brief FooBar——示例：GPU 侧宽度簿记与初始化契约。
 *
 * @par 线程模型
 *   仅主线程调用；Initialize 在渲染线程执行（RID 分裂）。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-11-08
 * @copyright Copyright (c) 2026 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_FOO_BAR_H_
#define ARHUD_SERVERS_RENDERING_FOO_BAR_H_

#include <cstdint>

#include "typedefs.h"

namespace arhud {

/**
 * @brief 示例类：宽度资源壳（被动状态 + 两阶段初始化）。
 *
 * @par 不变量
 *   width_px_ 恒 > 0（Initialize 成功后）。
 */
class FooBar final {
 public:
  explicit FooBar(uint32 width_px);
  FooBar(const FooBar&) = delete;
  FooBar& operator=(const FooBar&) = delete;
  ~FooBar();

  /**
   * @brief 初始化 GPU 侧状态（渲染线程执行）。
   * @return kOK 成功；kInvalidParameter 当 width_px == 0。
   */
  Error Initialize();

  uint32 width_px() const { return width_px_; }  ///< 当前宽度（像素）

 private:
  uint32 width_px_ = 0;  ///< 宽度（像素）；构造后只读
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_FOO_BAR_H_
```

## 附录 B · 存量 → 原教迁移示例（同一文件的前后对照）

改造前（Godot 风味，现状约 93% 文件）：

```cpp
#pragma once
namespace arhud
{
    struct CommandDataSpan
    {
        const void *data = nullptr; ///< 段数据指针
        uint32 size = 0;            ///< 段字节数
    };
    void RenderServer::Initialize(CommandQueue *p_queue, ARHudCompositor *p_compositor)
    {
        queue_ = p_queue;
    }
}
```

改造后（原教口径，新代码一律如此）：

```cpp
#ifndef ARHUD_SERVERS_RENDERING_COMMAND_QUEUE_H_
#define ARHUD_SERVERS_RENDERING_COMMAND_QUEUE_H_

namespace arhud {

// Describes one variable-length segment inlined in a command's blob.
struct CommandDataSpan {
  const void* data = nullptr;  // Segment bytes; must not be a cross-thread raw
                                // handle owned elsewhere (see PushBound rules).
  uint32 size = 0;              // Segment byte count.
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_COMMAND_QUEUE_H_
```

```cpp
void RenderServer::Initialize(CommandQueue* queue, ARHudCompositor* compositor) {
  queue_ = queue;
  compositor_ = compositor;
}
```
