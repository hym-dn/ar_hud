# 验证手册（Verification Handbook）

> 任何代码变更在归档前必须按本手册顺序完成验证。命令均在仓库根目录执行。
> 历史事故教训集中在 §6，动手前先读一遍。

---

## 1. 环境

| 组件 | 要求 | 备注 |
|---|---|---|
| Visual Studio | 2022 / 18（"Desktop development with C++"） | 先跑 `vcvarsall.bat x64` 或用 Developer Prompt |
| CMake | ≥ 3.16 | preset 需要 ≥ 3.23 |
| Conan | 2.x | `pip install conan` |

## 2. 标准构建流程（Conan，推荐）

```bat
build.bat conan          REM conan install + preset 配置 + 全量构建
build.bat tests          REM 上行 + -o build_tests=True + ctest
```

手动等价：

```bat
conan install . --build=missing -s build_type=Debug [-o build_tests=True]
cmake --preset conan-default
cmake --build build --config Debug --parallel
ctest --test-dir build --build-config Debug --output-on-failure
```

## 3. 验收顺序与预期结果

| # | 步骤 | 预期 |
|---|---|---|
| 1 | 配置成功 | 依赖状态 4 个全 1：`glm / glad / Freetype / stb_image` |
| 2 | 全量构建绿 | 5 lib（core/scene/platform/drivers/servers）+ 5 exe（phase2_rdc_rdd_rd + phase4_canvas/spatial/compositor/text） |
| 3 | ctest 全绿 | 当前 **25/25**（CommandQueue 6 + ItemStorageTree 19：树语义 + 传播性 dirty 路由 + Create 参数化掩码回归） |
| 4 | demo 冒烟 | 相关 demo 启动 → 渲染正确 → **干净退出，日志零 ERROR** |
| 5 | 治理 grep | §5 清单全部零命中 |

产物位置：`build/bin/Debug/*.exe`、`build/lib/Debug/*.lib`（VS 多配置自动追加 `/Debug`）。

## 4. demo 冒烟注意事项

- **超时 ≥ 帧数 × 帧时长 + 余量**：phase4_canvas 6000 帧 @60Hz ≈ 96s，30~40s 超时会误判 STUCK（2026-09-03 事故）
- demo 会弹出真实窗口（Win32 窗口 + GDI StretchDIBits 上屏）
- **dirty bit 语义**：内容静态时 presented 计数停在 1 属正常（不重渲），不是卡死
- 运行产物：`shader_cache/`（运行时生成，可整体删除，不入库）；根目录 `*.log` / `demo_output.txt` 为手工重定向残留，验证后清理

## 5. 治理 grep 清单（期望零代码命中）

```powershell
# ① app 层不得跨层触碰 storage/renderer/渲染内部（app 只见 ARHudEngine + RenderServer）
Get-ChildItem app -Recurse -Filter *.cpp | Select-String -Pattern "GetStorage|GetRenderer|RenderFromStorage|GetCompositor" | Where-Object { $_.Line -notmatch "^\s*//" }

# ② FrameScene 立即模式死路径（B3-M0.4 已删，回潮即回归）
Get-ChildItem app -Recurse -Filter *.cpp | Select-String -Pattern "BeginScene|FrameScene"

# ③ 分层规则：storage 头不得 include renderer（依赖 renderer → storage，单向）
Get-ChildItem servers\rendering\storage -Filter *.h | Select-String -Pattern "#include\s+.*renderer/"
```

判读规则：
- **代码行命中 = 违规**；纯注释行提及这些名字可接受（demo main.cpp 的注释会提及渲染内部名，属**正常注释**；历史 `?` 乱码注释已于 2026-09-27 清理）
- 命令 ① 的行内注释过滤只剔除整行注释（`^\s*//`），行尾注释需人工判读
- 环境（本机）无 `rg`；agent 用 Grep 工具，或装 ripgrep 后用 `rg -n` 等价执行
- 参考（2026-09-27 复核）：① 注释命中 7 处——**全为 4 个 demo 的正常注释提及渲染内部名，非乱码**（`?` 乱码已清零）/ ② 0 / ③ 0；**唯一代码命中** = `app/phase2_rdc_rdd_rd/main.cpp` 直调 `RenderingDevice`，属治理 §12.4 **备案豁免**（`// NOLINT(arch-layer-violation)`，该 demo 使命即直验 RCD→RDD→RD）

## 6. 已知坑（历史事故，务必先读）

| # | 坑 | 规避 |
|---|---|---|
| 1 | **Conan 缓存固化**：toolchain 变量（如 `ARHUD_BUILD_TESTS`）首次配置时写入 `CMakeCache.txt`，之后再 `conan install` 换选项**不生效** | 显式 `cmake -B build -DARHUD_BUILD_TESTS=ON` 强制覆盖，或删 `build/` 重配 |
| 2 | **无 Conan 直接 `cmake -B build` 失败**：`cmake/lib.cmake` 的 `find_package(glm CONFIG REQUIRED)` 找不到依赖 | 走 `build.bat conan`；或手动安装依赖并设 `CMAKE_PREFIX_PATH` |
| 3 | **中文文件编码事故**：PowerShell `Set-Content`/管道对 UTF-8 中文源码二次编码 → GBK 乱码（2026-09-03 render_server.cpp 全文件注释损坏事故） | 批量读写用 `[System.IO.File]::ReadAllText/WriteAllText` + 显式 UTF8；改完抽查中文注释 |
| 4 | **demo 超时误判卡死** | 见 §4，验证前先算 帧数 × 帧时长 |
| 5 | **CommandQueue 1MB 内联缓冲区** | 必须堆分配（`new`），栈实例溢出 Windows 默认 1MB 线程栈 |
| 6 | **渲染线程内发同步命令死锁** | RenderServer 的 `CanBypassQueue` 短路（无消费者/调用者即渲染线程/已关闭 → 直调 Storage）；新代码不得在渲染线程发起 PushAndSync |
| 7 | **CMake option 缓存陷阱**：`app/CMakeLists.txt` 重复声明 per-demo `option()`（默认 OFF），但 `cmake/option.cmake` 先加载，第一次 `option()` 调用生效 → 真实默认全 ON | 改默认值只改 `cmake/option.cmake` |

## 7. 清理

```bat
build.bat clean          REM 删整个 build/（含 Conan generators，需重跑 conan install）
```

`shader_cache/` 可随时整体删除（运行时重建）。
