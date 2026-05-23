---
name: "arhud-coding-standard"
description: "Enforce Google C++ Style Guide and Doxygen documentation standards across the AR HUD codebase. Invoke when user asks about code style, naming conventions, formatting, Doxygen comments, or code review standards."
---

# ARHud 编码规范指南（Google C++ Style + Doxygen）

本 Skill 定义 ARHud 项目的 C++ 编码规范，基于 **Google C++ Style Guide** 并针对 AR HUD 嵌入式/车规场景做针对性适配。所有代码必须满足 Doxygen 注释规范。

---

## 一、总体原则

| 原则 | 说明 |
|------|------|
| **可读性优先** | 代码是写给人读的，不要牺牲可读性换取微优化 |
| **一致性** | 整个代码库保持统一风格，比"哪种风格更好"更重要 |
| **零警告** | 所有代码必须零编译警告，W4 / -Wall -Wextra 级别 |
| **Doxygen 全覆盖** | 所有 public API、类、枚举、模板参数必须有 Doxygen 注释 |
| **车规安全** | 避免动态分配、RTTI、异常（配合 `-fno-exceptions`） |

---

## 二、命名规范（Google C++ Style）

### 2.1 通用规则

| 类别 | 风格 | 示例 |
|------|------|------|
| **类型/类/结构体/枚举** | PascalCase | `ARHudTexture`, `SurfaceType`, `BufferUsageBits` |
| **函数/方法** | PascalCase | `CreateTexture()`, `BufferMap()` |
| **变量（局部/参数/成员）** | snake_case | `texture_id`, `buffer_size` |
| **成员变量** | snake_case + `_` 后缀 | `texture_id_`, `buffer_size_` |
| **常量/enum 值** | k + PascalCase | `kMaxTextures`, `kTextureUsageSampling` |
| **宏** | UPPER_SNAKE_CASE | `ARHUD_MAX_TEXTURES`, `ARHUD_ASSERT()` |
| **命名空间** | snake_case | `arhud::render::opengl` |
| **文件/目录** | snake_case | `texture_storage.h`, `device_driver_gl.h` |
| **模板参数** | 大写字母或 PascalCase | `T`, `KeyT`, `ValueT`, `Allocator` |

### 2.2 类成员命名细则

```cpp
// 正确示例
class TextureStorage {
public:
    // 公有方法：PascalCase
    TextureID CreateTexture(const TextureCreateInfo& info);

    // 静态常量：k + PascalCase
    static constexpr uint32_t kMaxTextures = 256;

private:
    // 私有成员：snake_case + 后缀 _
    uint32_t texture_count_ = 0;
    ARHudVector<Texture*> textures_;
    ARHudMutex mutex_;
};
```

### 2.3 命名空间

```cpp
// 顶层命名空间：项目名称
namespace arhud {

// 子系统命名空间：平铺结构，不嵌套过深
namespace render {
namespace opengl {
// ...
}  // namespace opengl
}  // namespace render

// 内部实现细节：detail 子命名空间
namespace detail {
// 不对外暴露的实现细节
}  // namespace detail

}  // namespace arhud

// 使用：用完整限定名或 using 声明（禁止 using namespace）
using arhud::render::TextureID;
```

**禁止：**
- `using namespace std;`
- `using namespace arhud;`（头文件中）
- 匿名命名空间在头文件中使用

---

## 三、文件与目录规范

### 3.1 文件组织

```
每个类一个头文件（.h），实现放在对应 .cpp 中
文件名为 snake_case 的类名
```

| 文件类型 | 命名 | 示例 |
|----------|------|------|
| 头文件（公开接口） | `arhud_xxx.h` | `arhud_texture_storage.h` |
| 头文件（内部实现） | `xxx_private.h` | `texture_storage_private.h` |
| 实现文件 | `arhud_xxx.cpp` | `arhud_texture_storage.cpp` |
| 内联模板 | `arhud_xxx.inl.h` | `arhud_vector.inl.h` |

### 3.2 头文件保护

```cpp
// 格式：ARHUD_[SUBSYSTEM]_[FILENAME]_H
#ifndef ARHUD_RENDER_TEXTURE_STORAGE_H
#define ARHUD_RENDER_TEXTURE_STORAGE_H

// ...

#endif  // ARHUD_RENDER_TEXTURE_STORAGE_H
```

### 3.3 Include 顺序（Google Style）

```cpp
// 1. 关联头文件（本类的 .h，必须第一个）
#include "arhud/render/texture_storage.h"

// 2. C 标准库
#include <cstdint>
#include <cstring>

// 3. C++ 标准库
#include <memory>
#include <string>

// 4. 其他库头文件
#include "glad/gl.h"
#include "glm/mat4x4.hpp"

// 5. 本项目其他头文件
#include "arhud/core/arhud_rid.h"
#include "arhud/render/arhud_rendering_device_driver.h"
```

### 3.4 前置声明优先

能用前置声明就不要 `#include`，减少编译依赖：

```cpp
// 好的做法：前置声明
class ARHudTexture;
class ARHudBuffer;

class TextureStorage {
    ARHudTexture* GetTexture(TextureID id);
};

// 不好的做法：不必要的 include
#include "arhud/render/arhud_texture.h"
```

---

## 四、格式化规则（Google C++ Style）

### 4.1 缩进与空格

```cpp
// 2 空格缩进（不使用 Tab）
// 行宽：100 字符
// 作用域大括号换行（Allman 风格）

class TextureStorage
{
public:
    TextureID CreateTexture(const TextureCreateInfo& info);
    
    void UpdateTexture(TextureID id, const void* data, size_t data_size);

private:
    uint32_t texture_count_ = 0;
};

// 控制流语句大括号不换行（K&R 风格）
if (texture_count_ >= kMaxTextures) {
    ARHUD_LOG_ERROR("Texture limit exceeded");
    return kInvalidTextureID;
}

for (uint32_t i = 0; i < count; ++i) {
    ProcessTexture(textures_[i]);
}

while (pending_) {
    ProcessNext();
}

// 空循环体用大括号
while (*ptr != '\0') {}

// 函数/方法实现：返回类型单独一行（如果太长）
bool TextureStorage::CreateTexture(
    const TextureCreateInfo& info,
    TextureID* out_id)
{
    // ...
}
```

### 4.2 指针与引用

```cpp
// * 和 & 贴近类型（Google Style）
void* ptr;
const std::string& name;
TextureID* out_id;

// 空格规则
int* a;        // 正确
int *a;        // 错误
int &a = b;    // 错误
int& a = b;    // 正确
```

### 4.3 函数声明/定义

```cpp
// 参数过多时换行（4 空格缩进）
Error CreateTexture(
    const TextureCreateInfo& info,
    TextureID* out_id,
    const char* debug_name);

// 如果返回类型和函数名放一行太长，返回类型单独一行
// 这是 Google Style 允许的例外
ARHudRenderingDeviceDriver::TextureID
ARHudRenderingDeviceDriver::texture_create(
    const TextureFormat& format,
    const TextureView& view)
{
    // ...
}
```

### 4.4 类声明顺序

```cpp
class TextureStorage
{
public:
    // 1. 类型别名/嵌套类型
    using CreateCallback = std::function<void(TextureID)>;

public:
    // 2. 构造/析构
    TextureStorage();
    ~TextureStorage();

public:
    // 3. 静态方法
    static TextureStorage* GetSingleton();

public:
    // 4. 公有接口方法（按功能分组）
    TextureID CreateTexture(const TextureCreateInfo& info);
    void FreeTexture(TextureID id);
    Texture* GetOrNull(TextureID id);

public:
    // 5. 常量
    static constexpr uint32_t kMaxTextures = 256;

protected:
    // 6. 保护接口

private:
    // 7. 私有类型
    struct TextureSlot
    {
        Texture* texture = nullptr;
        bool occupied = false;
    };

    // 8. 私有方法
    uint32_t FindFreeSlot();

    // 9. 成员变量（最后）
    ARHudVector<TextureSlot> slots_;
    uint32_t active_count_ = 0;
};
```

---

## 五、C++ 语言特性使用规范

### 5.1 允许的特性（Google Style + ARHud 扩展）

| 特性 | 允许 | 说明 |
|------|------|------|
| `constexpr` / `constinit` / `consteval` | ✅ | 鼓励使用编译期求值 |
| `if constexpr` | ✅ | 模板条件编译 |
| `auto` | ✅ | 类型推导（非必须时慎用） |
| `auto&` / `const auto&` | ✅ | 避免拷贝 |
| Range-based for `for (auto& x : vec)` | ✅ | |
| `std::array` / `std::span` | ✅ | 取代 C 数组 |
| `std::optional` | ✅ | 可选值 |
| `std::variant` | ✅ | 类型安全联合体 |
| Lambda 表达式 | ✅ | 简短 lambda 允许 auto 参数 |
| `override` / `final` | ✅ | **必须**使用 |
| `nullptr` | ✅ | **禁止**使用 `NULL` 或 `0` |
| `using` / `typedef` | ✅ using 优先 | `using TextureID = uint64_t;` |
| `enum class` | ✅ | **禁止**使用裸 `enum` |
| `template` | ✅ | |
| `thread_local` | ✅ | |

### 5.2 禁止的特性

| 特性 | 原因 |
|------|------|
| **异常（`throw`/`try`/`catch`）** | 车规禁用，`-fno-exceptions` |
| **RTTI（`typeid`/`dynamic_cast`）** | 性能开销，车规禁用 |
| **`std::iostream`** | 启动开销大，嵌入式场景不宜，改用 `ARHudLogger` |
| **`std::regex`** | 编译期开销巨大 |
| **C 风格强制转型** | 不安全，用 `static_cast`/`reinterpret_cast` |
| **`malloc`/`free`** | 用 `ARHudMemory::alloc`/`ARHudMemory::free` |
| **`printf`/`sprintf`** | 用 `ARHudLogger` 或 `snprintf` |
| **全局/静态非 POD 对象** | 初始化顺序未定义 |
| **`goto`** | 破坏控制流可读性 |
| **`new`/`delete`（裸调用）** | 用 `ARHUD_NEW`/`ARHUD_DELETE` 宏 |

### 5.3 智能指针使用

```cpp
// 允许 std::unique_ptr（独占所有权）
std::unique_ptr<Texture> texture = std::make_unique<Texture>();

// 允许 std::shared_ptr（共享所有权），但谨慎使用
auto shared = std::make_shared<Buffer>();

// 裸指针用于非所有权传递（观察者模式）
void ProcessTexture(Texture* texture);  // texture 生命周期由调用方保证

// 引用用于非空参数
void UpdateTexture(Texture& texture, const void* data);
```

### 5.4 `auto` 使用规范

```cpp
// ✅ 推荐：类型明显或迭代器
auto texture = CreateTexture(info);
for (const auto& [key, value] : hash_map) { ... }

// ✅ 推荐：简化复杂类型
auto result = std::static_pointer_cast<Texture>(ptr);

// ❌ 避免：降低可读性
auto x = 42;                         // 不清晰，用 int
auto result = SomeFunction();        // 返回类型不明确
```

### 5.5 Lambda 规范

```cpp
// 简短 lambda：捕获列表明确，用 auto 参数
auto sort_by_id = [](const auto& a, const auto& b) {
    return a.GetId() < b.GetId();
};

// 避免使用默认捕获
[&] { ... }     // ❌ 隐式捕获所有引用，容易悬垂
[=] { ... }     // ❌ 隐式捕获所有拷贝，性能开销
[this] { ... }  // ✅ 显式捕获 this
[&tex] { ... }  // ✅ 显式捕获需要的变量

// 可变 lambda 需显式标记
auto counter = [count = 0]() mutable { return ++count; };
```

---

## 六、注释规范（Doxygen）

### 6.1 Doxygen 风格选择

使用 **Javadoc 风格**（`/** ... */`），禁用 Qt 风格（`/*! ... */`）和单行 `///`：

```cpp
/**
 * @brief 简短描述（一行，以句号结尾）
 *
 * 详细描述（可选），可以有多段文字。
 * 描述类的功能、使用方式、线程安全等注意事项。
 *
 * @note 注意事项
 * @warning 警告信息
 * @see RelatedClass
 */
class TextureStorage
{
    // ...
};
```

### 6.2 文件头注释

每个 `.h` / `.cpp` 文件必须有文件头注释：

```cpp
/**
 * @file arhud_texture_storage.h
 * @brief 纹理存储管理，提供纹理的创建、销毁、查询和生命周期管理
 * @author Yameng.He
 * @version 1.0
 * @date 2026-03-23
 * @copyright Copyright (c) 2024 3D HUD Project
 * @ingroup render_storage
 * @see ARHudTexture, ARHudRenderingDevice
 */
```

**字段说明：**

| 字段 | 要求 |
|------|------|
| `@file` | 文件名，自动或手动填写 |
| `@brief` | 一句话描述该文件职责 |
| `@author` | **必须**填写 `yameng.he` |
| `@version` | 文件版本，初版都按照1.0算 |
| `@date` |文件创建日期|
| `@copyright` | 自己发挥吧，越正规越好|
| `@ingroup` | 所属模块分组（可选） |
| `@see` | 相关类或文件（可选） |

### 6.3 类注释

```cpp
/**
 * @brief GPU 纹理存储管理器
 *
 * 管理所有 GPU 纹理资源的生命周期，包含纹理的创建、销毁、
 * 更新和查询功能。内部使用 ARHudRID 统一标识纹理资源。
 *
 * 线程安全：所有公有方法支持多线程调用（内部使用读写锁）。
 * 性能：创建/销毁操作涉及 GPU 同步，建议在帧头/帧尾批量处理。
 *
 * @ingroup render_storage
 *
 * @tparam T 纹理数据类型（ColorFormat 枚举）
 *
 * Usage example:
 * @code{.cpp}
 * auto storage = TextureStorage::GetSingleton();
 * TextureID id = storage->CreateTexture(info);
 * @endcode
 */
template <typename T>
class TextureStorage
{
    // ...
};
```

### 6.4 函数/方法注释

```cpp
/**
 * @brief 创建 GPU 纹理
 *
 * 根据传入的 TextureCreateInfo 在 GPU 上分配纹理资源。
 * 返回的 TextureID 可用于后续的纹理更新、绑定和销毁操作。
 *
 * @param[in]  info      纹理创建参数（宽度、高度、格式、用途等）
 * @param[out] out_id    输出参数，成功时写入新纹理的 ID
 * @param[in]  debug_name 可选调试名称（在 GPU 调试工具中可见）
 *
 * @return Error 错误码
 * @retval Error::kOk         创建成功
 * @retval Error::kOutOfMemory GPU 显存不足
 * @retval Error::kInvalidParam 参数校验失败（如尺寸超过限制）
 *
 * @pre info.width > 0 && info.height > 0
 * @pre info.format != DataFormat::kUndefined
 * @post out_id != nullptr 时 *out_id 为有效 ID 或 kInvalidTextureID
 *
 * @note 此函数可能触发 GPU 内存分配，不要在热路径中频繁调用
 * @warning 必须在渲染线程中调用
 *
 * @see FreeTexture, UpdateTexture
 */
Error CreateTexture(
    const TextureCreateInfo& info,
    TextureID* out_id,
    const char* debug_name = nullptr);


/**
 * @brief 更新纹理数据
 *
 * 将 CPU 内存中的数据上传到已存在的 GPU 纹理对象。
 * 支持部分区域更新（通过 offset/size 参数）。
 *
 * @param[in] id         目标纹理 ID
 * @param[in] offset     数据偏移（字节）
 * @param[in] size       数据大小（字节）
 * @param[in] data       源数据指针，不能为 nullptr
 *
 * @pre IsTextureValid(id)
 * @pre data != nullptr
 * @pre offset + size <= GetTextureSize(id)
 */
void UpdateTexture(
    TextureID id,
    size_t offset,
    size_t size,
    const void* data);
```

### 6.5 参数文档规范

使用 `[in]` / `[out]` / `[in,out]` 标记参数方向：

```cpp
/**
 * @brief 将数据从 CPU 传输到 GPU 缓冲区
 *
 * @param[in]     buffer_id  目标 GPU 缓冲区 ID
 * @param[in]     offset     缓冲区写入偏移量
 * @param[in]     size       写入数据大小
 * @param[in]     data       CPU 源数据指针
 * @param[out]    bytes_written 实际写入的字节数（可为 nullptr）
 * @param[in,out] state      缓冲区状态（函数内部会更新状态标记）
 */
```

### 6.6 枚举和常量注释

```cpp
/**
 * @brief 缓冲区用途标志位（BitField）
 *
 * 定义缓冲区在 GPU 管线中的使用方式。
 * 多个标志位可用 | 组合。
 */
enum class BufferUsageBits : uint32_t
{
    kTransferFrom  = (1 << 0),  ///< 可作为 DMA 读取源
    kTransferTo    = (1 << 1),  ///< 可作为 DMA 写入目标
    kUniform       = (1 << 4),  ///< Uniform Buffer
    kStorage       = (1 << 5),  ///< Storage Buffer（SSBO）
    kIndex         = (1 << 6),  ///< Index Buffer
    kVertex        = (1 << 7),  ///< Vertex Buffer
};

/**
 * @brief 纹理像素格式
 *
 * 与 Vulkan VkFormat 枚举值对齐，方便驱动层直接映射。
 */
enum class DataFormat : uint32_t
{
    kR8Unorm          = 9,    ///< 单通道 8-bit 归一化
    kR8G8Unorm        = 16,   ///< 双通道 8-bit 归一化
    kR8G8B8A8Unorm    = 37,   ///< RGBA 8-bit 归一化（最常用）
    kB8G8R8A8Unorm    = 44,   ///< BGRA 8-bit 归一化（Windows 兼容）
    kD32Sfloat        = 126,  ///< 32-bit 浮点深度缓冲
};
```

### 6.7 成员变量注释

```cpp
private:
    ARHudVector<TextureSlot> slots_;    ///< 纹理槽位数组，空闲链表管理
    uint32_t active_count_ = 0;         ///< 当前活跃纹理数量
    uint32_t next_id_ = 1;              ///< 下一个分配的纹理 ID（0 保留为无效）
    ARHudMutex mutex_;                  ///< 线程安全锁（读写锁）
    
    // 调试统计（仅在 Debug 构建中启用）
    #ifndef NDEBUG
    uint64_t total_allocs_ = 0;         ///< 累计分配次数
    uint64_t total_frees_ = 0;          ///< 累计释放次数
    #endif
};
```

### 6.8 分组标签（@defgroup / @ingroup）

```cpp
/**
 * @defgroup render_storage 渲染资源存储
 * @brief 所有 GPU 资源存储器的基类和接口
 *
 * 包含纹理、材质、网格、字体、着色器等资源的存储管理层。
 * 每个 Storage 类负责对应资源的创建、销毁、查询和生命周期管理。
 */
 
/**
 * @ingroup render_storage
 * @brief 纹理存储
 */
class TextureStorage { ... };

/**
 * @ingroup render_storage
 * @brief 材质存储
 */
class MaterialStorage { ... };
```

### 6.9 模板参数注释

```cpp
/**
 * @brief 分页内存分配器
 *
 * @tparam T           分配的对象类型
 * @tparam kThreadSafe 是否线程安全（true 时内部使用 SpinLock）
 * @tparam kPageSize   每页包含的槽位数量（必须是 2 的幂）
 */
template <typename T, bool kThreadSafe = false, uint32_t kPageSize = 256>
class ARHudPagedAllocator
{
    // ...
};
```

### 6.10 命名空间注释

```cpp
/**
 * @namespace arhud::render::opengl
 * @brief OpenGL API 驱动实现
 *
 * 包含 OpenGL 上下文驱动、设备驱动、着色器编译等实现。
 * 仅 Windows 平台初始支持，后续扩展 Linux/QNX。
 */
namespace arhud {
namespace render {
namespace opengl {
// ...
}  // namespace opengl
}  // namespace render
}  // namespace arhud
```

### 6.11 TODO / FIXME 注释

```cpp
// TODO(username): 实现纹理流式加载（优先级 P2，目标 v1.2）
//  当前实现一次性加载全部纹理到 GPU，大纹理场景会卡顿。
//  需要改为分块流式加载 + LRU 缓存。

// FIXME(issue#142): 部分 Intel GPU 上纹理压缩格式 B5G6R5 显示异常
//  临时方案：在 Intel GPU 上回退到 R8G8B8A8 格式。
```

---

## 七、代码组织与最佳实践

### 7.1 函数长度

```cpp
// 一个函数尽量控制在 40 行以内
// 超过 100 行必须拆分

// ✅ 好的做法：单一职责，层次清晰
Error CreateTexture(const TextureCreateInfo& info, TextureID* out_id)
{
    if (!ValidateCreateInfo(info)) {
        return Error::kInvalidParam;
    }

    TextureSlot slot = AllocateSlot();
    if (!slot.IsValid()) {
        return Error::kOutOfMemory;
    }

    Error err = driver_->CreateGpuTexture(info, &slot.driver_id);
    if (err != Error::kOk) {
        FreeSlot(slot);
        return err;
    }

    *out_id = slot.id;
    return Error::kOk;
}
```

### 7.2 尽早返回（Early Return）

```cpp
// ✅ 好的做法：先处理错误/边界情况
Error ProcessData(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0) {
        return Error::kInvalidParam;
    }
    if (size > kMaxDataSize) {
        return Error::kOutOfRange;
    }

    // 正常处理逻辑
    // ...
    return Error::kOk;
}

// ❌ 避免：深层嵌套
Error ProcessData(const uint8_t* data, size_t size)
{
    if (data != nullptr && size > 0) {
        if (size <= kMaxDataSize) {
            // 正常处理逻辑
            // ...
            return Error::kOk;
        }
    }
    return Error::kInvalidParam;
}
```

### 7.3 const 正确性

```cpp
// ✅ 能用 const 就用 const
const Texture* GetTexture(TextureID id) const;
void ProcessBuffer(const uint8_t* data, size_t size);
constexpr uint32_t kMaxTextures = 256;

// ✅ const 成员函数
uint32_t GetActiveCount() const { return active_count_; }

// ✅ const 引用传递大对象
void UpdateFrom(const TextureCreateInfo& info);
```

### 7.4 错误处理

```cpp
// 使用 Error 枚举统一错误处理（禁用异常）
enum class Error
{
    kOk,
    kInvalidParam,
    kOutOfMemory,
    kNotInitialized,
    kGpuError,
};

// 所有可能失败的函数返回 Error
// 输出参数用指针传递
Error CreateBuffer(size_t size, BufferID* out_id);

// 调用方必须检查返回值
Error err = CreateBuffer(size, &id);
if (err != Error::kOk) {
    ARHUD_LOG_ERROR("Failed to create buffer: {}", static_cast<int>(err));
    return err;
}
```

### 7.5 断言使用

```cpp
// Debug 断言：条件不满足时终止
#define ARHUD_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            ARHUD_LOG_FATAL("Assertion failed: {} - {}", #cond, msg); \
            std::abort(); \
        } \
    } while (0)

// 使用场景
void UpdateTexture(TextureID id, const void* data)
{
    ARHUD_ASSERT(IsTextureValid(id), "Invalid texture ID");
    ARHUD_ASSERT(data != nullptr, "Data pointer is null");
    // ...
}
```

### 7.6 头文件自包含

```cpp
// 每个 .h 必须能独立编译（自包含）
// 即 #include "arhud/render/arhud_texture_storage.h" 应可直接编译
// 不可以依赖调用方事先 include 其他头文件
```

---

## 八、Doxygen 配置

项目根目录放置 `Doxyfile`：

```doxygen
# Doxyfile 关键配置
PROJECT_NAME           = "ARHud Engine"
PROJECT_BRIEF          = "AR Head-Up Display Rendering Engine"
OUTPUT_DIRECTORY       = docs/doxygen
INPUT                  = src
RECURSIVE              = YES
EXTRACT_ALL            = YES
EXTRACT_PRIVATE        = NO
EXTRACT_STATIC         = NO
GENERATE_HTML          = YES
GENERATE_LATEX         = NO
WARN_IF_UNDOCUMENTED   = YES
WARN_IF_DOC_ERROR      = YES
WARN_NO_PARAMDOC       = YES
ENABLE_PREPROCESSING   = YES
MACRO_EXPANSION        = YES
EXPAND_ONLY_PREDEF     = YES
PREDEFINED             = "ARHUD_DEFINE_ID(x)=struct x##ID { uint64_t id; };"
TAB_SIZE               = 2
OPTIMIZE_OUTPUT_FOR_C  = NO
```

---

## 九、附录：快速检查清单

### 9.1 提交前自检清单

- [ ] 遵循 Google C++ Naming Convention（PascalCase 类/函数，snake_case 变量 + `_` 后缀）
- [ ] 所有 public API 有完整 Doxygen 注释（`@brief`、`@param`、`@return` 齐全）
- [ ] 无编译警告（W4 / -Wall -Wextra）
- [ ] 头文件自包含，最小化 include 依赖
- [ ] const 正确性检查
- [ ] Early return 替代深层嵌套
- [ ] 函数不超过 40 行（热路径例外需注释说明）
- [ ] 无异常、无 RTTI、无裸 `new`/`delete`
- [ ] `enum class` 替代裸 `enum`
- [ ] 智能指针或 `ARHUD_NEW`/`ARHUD_DELETE` 替代裸动态分配
- [ ] `override` / `final` 关键字完整

### 9.2 常见命名对照速查

| 代码元素 | Google Style | 错误示例 |
|----------|-------------|----------|
| 类 | `TextureStorage` | `texture_storage`, `textureStorage` |
| 函数 | `CreateTexture()` | `create_texture()`, `createTexture()` |
| 局部变量 | `texture_id` | `textureID`, `mTextureId` |
| 成员变量 | `texture_id_` | `mTextureId`, `texture_id` |
| 常量 | `kMaxTextures` | `MAX_TEXTURES`, `max_textures_` |
| 宏 | `ARHUD_MAX_TEXTURES` | `MAX_TEXTURES` |
| enum class | `kOptionA` | `OPTION_A` |
| 命名空间 | `arhud::render` | `ARHud::Render` |
| 文件 | `texture_storage.h` | `TextureStorage.h` |
