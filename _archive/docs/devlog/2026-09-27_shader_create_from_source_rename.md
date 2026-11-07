# 开发日志：治理 P1 — `ShaderCreateFromGLSL` 改名 + `ShaderSourceFormat`

- 日期：2026-09-27
- 范围：`drivers/rendering_device_driver.h`、`drivers/rendering_device_commons.h`、
  `drivers/gl/rendering_device_driver_gl.h/.cpp`、`drivers/gl/rendering_device_driver_gl_shader_cache.cpp`、
  `servers/rendering/rendering_device.h/.cpp`、`servers/rendering/storage/shader_storage.cpp`、
  `skill/arhud-architecture`、`skill/arhud-render-thread-arch`、`skill/arhud-improvement`
- 依据：治理 **P1**（`skill/arhud-improvement` §三 P1 表）——"`ShaderCreateFromGLSL` 命名泄漏"
- 变更分级 **B 类**：契约方法改名 + 增参，贯穿 RDD/RD/GL 三层（无行为变化——GL 后端仍只跑 GLSL）

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 新增 `enum class ShaderSourceFormat { kGLSL, kSPIRV, kMax }` | `rendering_device_commons.h`（紧邻 `ShaderStageSource`） |
| RDD 契约：`ShaderCreateFromGLSL` → `ShaderCreateFromSource` + 增 `ShaderSourceFormat p_source_format` | `rendering_device_driver.h` |
| GL 实现：override 同步改名/增参 + 非 `kGLSL` 返回无效 ID + WARN | `rendering_device_driver_gl.h/.cpp` |
| RD 层：声明/定义/跨线程 `Args`/递归调用/驱动转发 全部增 format | `rendering_device.h/.cpp` |
| Storage：两处调用传 `ShaderSourceFormat::kGLSL` | `shader_storage.cpp` |
| 文档：P1 项勾销（3 份 skill） | architecture / render-thread-arch / improvement |

## 2. 关键实现决策

1. **参数位置**：`p_source_format` 紧随 `p_stage_sources`（格式描述的是"这些源码"），
   即 `ShaderCreateFromSource(stage_sources, source_format, uniforms, push_constant_size)`。
2. **GL 后端的格式校验**：新增分支——非 `kGLSL` 直接 `WARN + 返回空 ShaderID`。
   保持"GL 只跑 GLSL"的既有事实**显式化**，为 Vulkan 分支留下明确接管点。
3. **跨线程代理同步增参**：RD 的 `PushAndSync` 代理路径中 `Args` 结构体加 `source_format`
   字段并透传，保证主线程 → 渲染线程的跨线程调用与同线程调用语义一致。
4. **Storage 层未改签名**：审计发现 `ShaderStorage::ShaderCreateFromSource` **早已是合规命名**
   （[shader_storage.cpp:162](../../servers/rendering/storage/shader_storage.cpp#L162)），
   命名泄漏**只存在于 RDD/RD/GL**。故本轮只对泄漏层改名，Storage 维持原签名、内部传 `kGLSL`。
   若将来 Vulkan 需从 Storage 请求 SPIR-V，再把 format 上提（并须计入 `ShaderSourceKey` 缓存键）。
5. **改名方式**：7 个代码文件的 `ShaderCreateFromGLSL` 用脚本做 token 级替换（一并覆盖
   **Doxygen 注释与日志串**，避免残留"文档引用不存在的方法"），随后手工增参——
   纯人工逐处改易漏注释，纯脚本又改不了签名，两者结合最稳。

## 3. 排障记录（编码防护）

- 脚本改文件用 `New-Object System.Text.UTF8Encoding($false)`（**no BOM**）读改写，并逐文件校验
  首三字节——6 个文件均 `47,42,42`（`/**`），**无 BOM**，中文未乱码。此流程沿用 2026-09-27
  文档偏差批的教训（该次曾误写 BOM）。

## 4. 验收结果

- 构建：全量绿（5 lib + 5 exe + `arhud_tests`），退出码 0，无 error/warning
- 单测：**25/25 通过**（`ctest --test-dir build -C Debug`）
- demo：`phase2_rdc_rdd_rd` 直接调用 Storage 层（签名未变），5 个 demo 全部重链成功
- 残留校验：`ShaderCreateFromGLSL` 在 `*.h/*.cpp` **零命中**（仅历史 devlog 保留记录）
- 治理：`ShaderSourceFormat` 参数已贯穿 RDD → RD → GL → Storage 调用点

## 5. 遗留（登记去向：skill/arhud-improvement）

- [ ] Storage 层若要支持 SPIR-V：`ShaderStorage::ShaderCreateFromSource` 增 `ShaderSourceFormat`
  参数并计入 `ShaderSourceKey`（缓存键需含格式，否则格式不同的同源 shader 会键碰撞）
- [ ] Vulkan 后端落地时：`ShaderSourceFormat::kSPIRV` 分支接入（当前 GL 后端对非 GLSL 显式拒绝）
- [ ] B5 批 2 其余项：后处理独立子系统、RD frame slot 简化、`CmdBufferUpdate` 深拷贝优化