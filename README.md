# ARHud Engine

**AR Head-Up Display Rendering Engine** — 专为车载 AR HUD（增强现实抬头显示）场景设计的高性能 C++ 渲染引擎。

![build](https://img.shields.io/badge/build-passing-brightgreen)
![c++](https://img.shields.io/badge/C%2B%2B-17%2F20-blue)
![platform](https://img.shields.io/badge/platform-Win32%20%7C%20Linux%20%7C%20Android%20%7C%20QNX-lightgrey)
![graphics](https://img.shields.io/badge/API-OpenGL%203.3%20Core-orange)

---

## 概述

ARHud Engine 参照 Godot 4.6 渲染引擎的四层 GPU 抽象架构，为车载 AR HUD 场景深度定制。引擎采用纯 C++ 编写（无异常、无 RTTI、无虚继承），满足车规级安全需求，同时为多平台（Windows/Linux/Android/QNX）和多图形 API（OpenGL/Vulkan）提供统一抽象。

### 设计目标

- **高性能** — 零开销抽象、PagedAllocator 热路径、缓存友好的数据结构
- **安全** — 无异常/无 RTTI/无 COW 容器、显式生命周期管理、车规安全约束
- **跨平台** — 统一抽象层设计，Platform 层隔离系统差异
- **多 API** — OpenGL 3.3 Core 先行，Vulkan 接口预留，API 切换零上层改动
- **多屏** — 单线程多 Surface / 多线程独立上下文，同一套接口

---

## 架构

### 四层 GPU 抽象

```
┌──────────────────────────────────────────────────────────┐
│                    RenderingDevice (RD)                   │  ← 设备层
│  资源生命周期 · Staging Buffer · 帧循环 · 依赖追踪        │
├──────────────────────────────────────────────────────────┤
│              IRenderingDeviceDriver (RDD)                 │  ← 驱动层
│  Buffer/Texture/Shader/Pipeline/Framebuffer 资源接口      │
├──────────────────────────────────────────────────────────┤
│             IRenderingContextDriver (RCD)                 │  ← 上下文层
│  GPU 实例化 · 设备枚举 · Surface 管理 · 驱动工厂          │
├──────────────────────────────────────────────────────────┤
│  IGLManager · IDisplayServer · IWindow · IScreen · IView │  ← 平台层
│  Win32 WGL 上下文/窗口/屏幕 · 预留 Linux/Android/QNX     │
└──────────────────────────────────────────────────────────┘
```

### 渲染模型

**单线程 + 多 Surface**（默认）：
```
主线程
  ├── Surface 0 (HUD 投影)   ── MakeCurrent → glDraw → SwapBuffers
  ├── Surface 1 (仪表盘)     ── MakeCurrent → glDraw → SwapBuffers
  └── Surface 2 (中控屏)     ── MakeCurrent → glDraw → SwapBuffers
```

**多线程 + 独立上下文**（可选）：
```
主线程 (RD 0)                   渲染线程 1 (RD 1)
  MainContext                       IndependentContext
  └── Surface 0                      └── Surface 1
```

两种模式使用完全相同的 API，区别仅在于 RenderingDevice 实例数量。

### 依赖方向

```
arhud_core (zero external deps)
    ↑
arhud_platform (core)
    ↑
arhud_servers (core + platform)
    ↑
arhud_drivers (core + servers + platform)
    ↑
arhud_demo     (all of the above)    ← 可选 demo 可执行文件
```

---

## 项目状态

| 模块 | 状态 | 说明 |
|------|------|------|
| `core/` | ✅ 完成 | 零依赖基础库，类型/容器/同步/日志/数学/字符串 |
| `servers/` 接口 | ✅ 完成 | RCD/RDD/RD/CommandBuffer 接口定义 |
| `servers/rendering_device` | ✅ 完成 | 资源生命周期、Staging Buffer、帧循环 |
| `drivers/gl/` | 🟡 骨架 | OpenGL 驱动声明 + 部分实现，需填充 GL 调用 |
| `platform/windows/` | ✅ 完成 | Win32 窗口/屏幕/GL 上下文/显示服务器/视图 |
| `app/demo` | 🟡 骨架 | 最小可运行 demo，端到端管线验证 |
| `platform/linux/` | ❌ 待实现 | — |
| `platform/android/` | ❌ 待实现 | — |
| `platform/qnx/` | ❌ 待实现 | — |
| `drivers/vulkan/` | ❌ 待实现 | 接口已预留 |
| 测试框架 | ❌ 待实现 | GoogleTest 集成 |

---

## 快速开始

### 环境要求

- **Visual Studio 2022**（"Desktop development with C++" 工作负载）
- **CMake** ≥ 3.16
- **Conan 2.x**（可选，推荐用于依赖管理）

### Windows 构建

从 **"Developer Command Prompt for VS 2022"** 或运行 `vcvarsall.bat x64` 后执行：

```bash
# 方式 1：一键构建（推荐）
build.bat              # Debug 构建
build.bat release      # Release 构建
build.bat clean        # 清理构建目录
build.bat conan        # Conan 依赖管理 + Debug 构建

# 方式 2：直接 CMake
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug

# 方式 3：构建 demo（需已安装 glad 或使用 Conan）
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DARHUD_BUILD_DEMO=ON
cmake --build build --config Debug
```

### 输出

```
build/
├── bin/Debug/       ← 可执行文件（启用 demo 时有 arhud_demo.exe）
└── lib/Debug/       ← 静态库
    ├── arhud_core.lib
    ├── arhud_platform.lib
    ├── arhud_servers.lib
    └── arhud_drivers.lib
```

### 运行 Demo

```bash
build\bin\Debug\arhud_demo.exe
```

Demo 会创建一个 1280×720 窗口，运行 120 帧的清屏循环后自动退出，验证 RCD→RDD→RD 端到端管线正常。

---

## 项目结构

```
ar_hud/
├── CMakeLists.txt          # 构建入口
├── conanfile.py            # Conan 配方
├── build.bat               # Windows 构建脚本
│
├── cmake/                  # CMake 模块
│   ├── option.cmake        # 构建选项
│   ├── build.cmake         # 编译器标志
│   ├── dir.cmake           # 输出目录
│   └── lib.cmake           # 第三方依赖
│
├── core/                   # 核心库（零外部依赖）
│   ├── typedefs.h          # 基础类型/平台宏/Error 枚举
│   ├── os/                 # 内存/线程/同步原语
│   ├── io/                 # 日志系统
│   ├── template/           # 模板容器
│   │   ├── paged_allocator.h   # O(1) 固定大小对象池
│   │   ├── local_vector.h      # 动态数组（无 COW）
│   │   ├── hash_map.h          # 开放寻址哈希表
│   │   ├── hash_set.h          # 哈希集合
│   │   ├── list.h              # 双向链表
│   │   ├── fixed_vector.h      # 栈分配定长数组
│   │   ├── ring_buffer.h       # 环形缓冲
│   │   ├── bitfield.h          # 类型安全位域
│   │   ├── vector_view.h       # 只读内存视图
│   │   ├── rid.h               # 不透明资源 ID (RID/RIDOwner)
│   │   └── safe_refcount.h     # 原子操作
│   ├── string/             # UTF-32 字符串
│   └── math/               # 数学库（Vec2/3/4/Mat4/Color/Transform2D）
│
├── platform/               # 平台抽象层
│   ├── window.h            # 窗口接口
│   ├── screen.h            # 屏幕接口
│   ├── view.h              # 视图接口（HUD 分层）
│   ├── display_server.h    # 显示服务器接口
│   ├── gl_manager.h        # GL 上下文管理器接口
│   └── windows/            # Win32 实现
│
├── servers/                # 服务器层
│   └── redering/           # 渲染接口与实现
│       ├── rendering_device_commons.h   # 共享枚举/类型
│       ├── rendering_context_driver.h   # RCD 接口
│       ├── rendering_device_driver.h    # RDD 接口
│       ├── rendering_device.h/.cpp      # RD 实现
│       ├── command_buffer.h             # 命令缓冲接口
│       └── resource_tracker.h/.cpp      # 资源追踪
│
├── drivers/                # 驱动层
│   └── gl/                 # OpenGL 驱动
│       ├── rendering_context_driver_gl.h/.cpp
│       ├── rendering_device_driver_gl.h/.cpp
│       └── command_buffer_gl.h/.cpp
│
└── app/                    # 应用层
    ├── CMakeLists.txt      # demo 构建（可选，需 -DARHUD_BUILD_DEMO=ON）
    └── main.cpp            # 最小可运行 demo
```

---

## 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `ARHUD_API_OPENGL` | ON | 启用 OpenGL 后端 |
| `ARHUD_API_VULKAN` | OFF | 启用 Vulkan 后端（预留） |
| `ARHUD_BUILD_TESTS` | OFF | 构建单元测试 |
| `ARHUD_BUILD_TOOLS` | OFF | 构建开发工具 |
| `ARHUD_BUILD_DEMO` | OFF | 构建 demo 可执行文件（需 glad） |
| `ARHUD_ENABLE_CXX20` | OFF | 使用 C++20 替代 C++17 |
| `BUILD_SHARED_LIBS` | OFF | 构建动态库 |

---

## 编码规范

| 元素 | 风格 | 示例 |
|------|------|------|
| 类型/类/枚举 | PascalCase | `SafeNumeric`, `LogLevel`, `Error` |
| 函数/方法 | PascalCase | `CreateTexture()`, `BufferMap()` |
| 局部/参数变量 | snake_case | `texture_id`, `buffer_size` |
| 成员变量 | snake_case + `_` | `texture_id_`, `buffer_size_` |
| 常量/枚举值 | k + PascalCase | `kMaxTextures`, `kOutOfMemory` |
| 宏 | UPPER_SNAKE_CASE | `ARHUD_PLATFORM_WINDOWS` |
| 命名空间 | snake_case | `arhud::render::opengl` |

详见 [skill/coding_standard_skill.md](skill/coding_standard_skill.md)。

---

## 第三方依赖

| 库 | 版本 | 用途 |
|----|------|------|
| [glm](https://github.com/g-truc/glm) | 1.0.1 | 数学库（向量/矩阵） |
| [glad](https://github.com/Dav1dde/glad) | 2.0.8 | OpenGL 函数加载器 |
| [freetype](https://freetype.org) | 2.13.3 | 字体渲染 |
| [stb](https://github.com/nothings/stb) | cci.20250126 | 图片加载 |
| [glfw](https://www.glfw.org) | 3.4 | 窗口管理（仅测试） |

`arhud_core` **零外部依赖**，渲染库依赖通过 Conan 管理，无 Conan 时需手动安装到系统路径。Demo 可执行文件需要 `glad`（系统安装或 Conan）。

---

## 设计参考

本项目在架构上深度参考 Godot 4.6 渲染引擎：

- **RenderingDevice** → `servers/rendering/rendering_device.h`
- **RenderingDeviceDriver** → `servers/rendering/rendering_device_driver.h`
- **RenderingContextDriver** → `servers/rendering/rendering_context_driver.h`
- **CommandBuffer** → `servers/rendering/rendering_device_graph.h`
- **GLManager** → `platform/windows/gl_manager_windows_native.cpp`
- **RID 系统** → `core/templates/rid.h`
- **容器** → `core/templates/` (LocalVector, HashMap, List, PagedAllocator)

详见 [skill/arch_skill.md](skill/arch_skill.md)。

---

## 路线图

- [x] 基础类型、容器、同步、日志
- [x] 数学库（Vec2/3/4, Mat4, Color, Transform2D）
- [x] RID 资源 ID 系统
- [x] Win32 平台层（窗口/屏幕/GL 上下文）
- [x] 渲染接口定义（RCD/RDD/RD/CommandBuffer）
- [x] 资源生命周期管理（Staging Buffer、帧循环）
- [x] 单线程多 Surface 渲染模型
- [x] 多线程独立上下文渲染模型
- [x] 最小可运行 Demo（端到端管线验证）
- [ ] OpenGL 设备驱动实现（Buffer/Texture/Shader/Pipeline）
- [ ] OpenGL 命令缓冲实现
- [ ] 测试框架（GoogleTest）
- [ ] Linux 平台层
- [ ] Android 平台层
- [ ] QNX 平台层
- [ ] Vulkan 驱动
- [ ] 2D HUD 渲染管线
- [ ] 字体渲染 / SDF 图集
- [ ] 纹理资源管理
- [ ] 场景管理 / 合成器

---

## 许可

MIT © 2025 yameng.he
