# Unit Tests

## 目录结构

按引擎模块组织，与 Godot `tests/` 一致：

```
tests/
├── CMakeLists.txt              ← 测试基础设施 + 目标定义
├── core/                       ← arhud_core 测试（sync/containers/math）
├── rendering/                  ← arhud_servers 测试（CommandQueue/RD/Storage）
└── integration/                ← 跨模块集成测试（需 NullDriver 或真实 GL）
```

## 构建 & 运行

### 方式一：一键脚本

```bat
build.bat tests
```

自动完成：conan install（`-o build_tests=True`）→ cmake 配置 → 编译 → ctest。

### 方式二：手动

```bash
# 1. Conan 安装依赖（含 gtest）
conan install . --build=missing -s build_type=Debug -o build_tests=True

# 2. CMake 配置
cmake --preset conan-default

# 3. 编译
cmake --build build --config Debug --parallel

# 4. 运行测试
ctest --test-dir build --build-config Debug --output-on-failure
```

## 添加新测试

1. 在对应模块子目录下新建 `test_xxx.cpp`
2. 在 `tests/CMakeLists.txt` 的 `TEST_SOURCES` 列表追加路径
3. 编译运行

## 约束

- 测试目标单独启用 `/EHsc /GR`（gtest 需异常 + RTTI），不影响引擎库的 `/EHs- /GR-`
- 测试链接 `arhud_servers` + `arhud_core`，不直接链接 `arhud_drivers`/`arhud_platform`（PRIVATE 隔离）
- 需要真实 GL 上下文的测试放 `integration/`，用 NullDriver 的测试也放那里
