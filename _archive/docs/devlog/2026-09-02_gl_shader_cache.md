# 开发日志：rendering_device_driver_gl_shader_cache.cpp 注释补齐

- 日期：2026-09-02
- 文件：`drivers/gl/rendering_device_driver_gl_shader_cache.cpp`
- 变更类型：文档（纯 Doxygen 注释，零行为改动）

## 1. 背景与目标

Shader 磁盘缓存分文件原有注释仅覆盖文件头（`@file` 概览 + 缓存文件格式），三个函数实现体内部为裸 `//` 行内注释，与 `rendering_device_driver_gl.cpp` / `rendering_device_driver_gl_format.cpp` 已确立的"实现侧注释规范"（`@brief …（实现侧）` + `@par` 分节 + `@note` + `@param`/`@return`/`@retval`/`@see`）不一致。本次对齐补齐。

## 2. 变更明细

| 函数 | 注释要点 |
|------|----------|
| `ComputeSourceHash` | FNV-1a 64 位参数（basis 0xcbf29ce484222325 / prime 0x100000001b3）；stage 枚举 4 字节逐字节参与、stage 顺序敏感；nullptr source 整体跳过 |
| `LoadShaderCache` | `@par` 四步加载流程（定位文件→头部 4 字段校验→`glProgramBinary` 直灌→`GL_LINK_STATUS` 自愈）；命中后两次反射遍历与编译路径对齐（UBO 补绑 / 采样器 location 表） |
| `SaveShaderCache` | `@par` 落盘流程（`GL_PROGRAM_BINARY_LENGTH` 探询→导出→`_mkdir` 幂等→写头+体）；静默失败语义与自愈闭环 |

文档化过程中如实记录的设计事实（均进 `@note`/`@par`，非新发现代码问题）：

1. **哈希键盲区**：缓存键只覆盖 stage 枚举 + 源码文本，uniform 声明表不参与——同源码不同绑定配置命中同一二进制。安全性依据：命中路径按本次 `p_uniforms` 重新反射/绑定，不依赖落盘配置。
2. **碰撞风险**：FNV-1a 非密码学哈希，理论碰撞 ~2^-64；碰撞表现为加载错误程序且链接校验无法检出，属已接受风险。
3. **`shader_objects` 恒空**：缓存二进制仅含 program 整体（stage 对象编译期已删），`ShaderFree` 据此只删 program。
4. **写侧防御缺口**：`fwrite` 返回值未检查，半截文件由读取侧短读校验兜底（自愈闭环）；程序二进制无驱动指纹记录，兼容性由 `GL_LINK_STATUS` 兜底剔除。

## 3. 附带技术结论（讨论记录）

- **`GLenum binary_format` 的类型选择**：该值是驱动私有枚举，本文件只做磁盘↔GL 透传，两端 API（`glGetProgramBinary` 出参 / `glProgramBinary` 入参）签名即 `GLenum`，零转换直传；GL 规范保证 `GLenum` 恒 32 位，文件头 "4B" 布局承诺稳定。副作用：跨驱动无意义——这正是链接校验 + 自愈存在的理由。若未来跨驱动复用缓存，应替换为引擎自定义指纹而非继续透传驱动枚举。
- **`TextureGetAllocationSize` 恒 0 的原因**（关联讨论，非本次变更）：RDD 纯虚契约要求 override；GL 无等价 `GL_BUFFER_SIZE` 的真实分配查询 API；servers 侧零调用方，实现无消费者。`GLTextureInfo` 已备齐估算所需字段，VRAM 预算功能落地时按 `width*height*bpp*mip 链*layers` 填充即可。

## 4. 验证

- 注释块配平检查：`/**` × 4（文件级 1 + 函数级 3），`*/` × 4，`@brief` × 4。
- 纯注释变更，无编译影响（未重编全量，格式与既有分文件一致）。

## 5. 后续待办（文档化过程中识别，未动代码）

- [x] `SaveShaderCache` 的 `fwrite` 返回值检查 + 写失败清理 —— 已于同日补齐（见第 6 节）。
- [ ] 缓存文件头可增加驱动/设备指纹字段，把"驱动变更失效"从运行期链接校验前移到头部校验（省一次 `glProgramBinary` 尝试）。
- [ ] `kShaderCacheDir` 相对工作目录，进程 CWD 不同则缓存失联——多 Demo 场景可考虑可执行目录锚定。

## 6. 追加变更（同日）：关键日志补齐 + 写失败防御

文件同上，变更类型：日志（行为微调一处）。

### 6.1 新增日志（异常路径全覆盖）

| 函数 | 路径 | 级别 | 内容 |
|------|------|------|------|
| Load | 头部短读 | WARN | 文件截断，重编译 |
| Load | 魔数/版本不符 | WARN | 格式不符（含 magic/version 值） |
| Load | Alloc 失败 | ERROR kOutOfMemory | 字节数 |
| Load | 载荷短读 | WARN | 二进制截断，重编译 |
| Load | glCreateProgram 失败 | ERROR kFailed | — |
| Save | 无程序二进制 | INFO | 驱动能力缺失，跳过落盘 |
| Save | Alloc 失败 | ERROR kOutOfMemory | 字节数 |
| Save | 导出空载荷 | WARN | glGetProgramBinary 空返回 |
| Save | fopen 失败 | WARN | 含路径 |
| Save | 载荷写不完整 | WARN | 实写/应写字节 + 路径 |

日志策略：文件不存在（冷启动未命中）为常态，刻意不打日志避免逐 shader 噪音；既有 hit/invalid/saved 三条 INFO 保留。ERROR 走 `ARHUD_LOG_ERROR(code, fmt)` 双参形式，与 drivers 内 `kFailed`/`kOutOfMemory` code 惯例一致。

### 6.2 行为微调（写失败防御，原待办 #1）

`SaveShaderCache` 载荷 `fwrite` 按实写字节数校验：写失败（磁盘满/权限）时删除半截文件 + WARN，成功 INFO 仅在真正落盘完整后输出。此前半截文件靠下次 Load 的短读校验兜底（一周期自愈），现当周期即清理。头部 16 字节 fwrite 仍不查——失败时载荷写必同步失败，由载荷校验统一兜底（函数 `@note` 已注明）。

### 6.3 验证

`cmake --build build --config Debug --target arhud_drivers` 全绿（三个 GL 分文件重编通过）。

## 7. 追加变更（同日）：rendering_device_driver_gl.cpp 关键日志

先经子代理全量审计 2371 行约 100 个函数的静默失败路径，按"异常必录、guard 静默"策略补 17 处（B 类无效 ID guard 刻意保持静默防刷屏）：

| 函数 | 分支 | 级别 |
|------|------|------|
| Initialize | glGetString(GL_VERSION) 返回 null（误判桌面 GL） | WARN |
| BufferCreate / TextureCreate / SamplerCreate / VertexFormatCreate / FramebufferCreate | glGenXxx 返回 0 | ERROR kFailed（各带尺寸/索引上下文） |
| BufferMap | glMapBuffer 返回 null（此前为 commands 文件同场景的未记录孪生） | ERROR kFailed |
| BufferUnmap | glUnmapBuffer 返回 GL_FALSE（写入丢失）—— 由"忽略返回值"改为检查+WARN，@note 同步更新 | WARN |
| CommandBufferCreate | PagedAllocator 耗尽 | ERROR kOutOfMemory |
| ShaderCreateFromGLSL | 空 stage 入参 / null source 丢 stage / 未知 stage 枚举 / 全部被丢后零可编译项 | WARN ×4 |
| FramebufferCreate | 附件引用未知纹理（外部/已释放 ID，深度纹理会被误挂 COLOR0） | WARN |
| FramebufferReadback | glReadPixels 后补 GL_CHECK_ERROR（Mode-R 呈现回读路径，此前零检查） | DEBUG 错误日志 |
| CompileShader / LinkProgram | info log 为空或其 Alloc 失败时的兜底消息 | ERROR kFailed |

坑：`ARHUD_LOG_ERROR` 为双参宏（code, fmt），单参调用编译失败——两处新调用已带 code。验证：`arhud_drivers` 全绿。

审计还识别出未处理的既有缺口（记录备查，未动代码）：多数 `Alloc()` 结果未判空（1384/1469/1530/1639/1726/1791/1994/2092 行附近）；`GL_CHECK_ERROR` 仅 Debug 生效，Release 下 GL 调用全静默；SamplerCreate 参数循环与 QueryLimits 无错误检查。

## 8. 追加变更（2026-09-03）：深度/模板格式统一判定 + S8 模板挂点修正

### 8.1 问题

`FramebufferCreate` 与 `CommandBeginRenderPass` 各自硬编码深度格式枚举清单且已漂移：前者含 `kS8Uint` 但挂点分支错（纯模板被挂到 `GL_DEPTH_ATTACHMENT`，FBO 必 incomplete），后者漏 `kS8Uint`（模板 clear 值被 `glClearColor` 误清颜色）；`TextureGetUsagesSupportedByFormat` 有第三份正确清单；`DataFormatToGLInternalFormat` 等四张映射表缺 `kD16UnormS8Uint`/`kS8Uint` 映射。

### 8.2 变更

1. **统一判定**（`rendering_device_driver_gl_format.cpp` + 头声明）：
   - `DataFormatIsDepthStencil()`：全 6 格式族判定（D16/D32/D16S8/D24S8/D32S8/S8）
   - `DataFormatHasStencil()`：4 个含模板位格式判定
2. **四消费点收敛**（禁手写清单）：FramebufferCreate 挂点选择、CommandBeginRenderPass clear 分拣、TextureGetUsagesSupportedByFormat 格式族归类。
3. **S8 挂点修正**：`GL_STENCIL_ATTACHMENT`（`GL_STENCIL_INDEX8` 内部格式）；clear 走 `glClearStencil` 仅清模板位。
4. **D16S8 降级**：GL 无 DEPTH16_STENCIL8 内部格式 → 映射 D24S8（注释注明降级口径），PixelSize 按 4 字节。
5. **顺带**：BeginRenderPass 移除死变量 `clear_index`，`rp_info` 解析提到循环外。

### 8.3 验证

全量 Debug 构建全绿（drivers/servers/五个 demo）；`arhud_demo_phase4_spatial.exe`（深度 pass 路径）600 帧运行读回像素正常、干净退出。日志中的 `glProgramBinary 1280` 为陈旧 shader 缓存自愈，与本变更无关。

## 9. 追加变更（2026-09-03）：rendering_device_commons.h 全量 Doxygen 补齐

契约头 1563 行、约 50 个类型从单行 brief 升级为完整 Doxygen（82 个文档块全配平，风格与 drivers 分文件一致：`@brief` + `@par` 语义/映射/消费方 + `@note`）。要点：

- **ID 块**：补跨后端封装差异说明（GL 对象名 vs 簿记指针 vs Vulkan 句柄）与强类型隔离动机
- **纹理族**：TextureType/Samples/Swizzle/View/Layout + 子资源三兄弟（Subresource/Layers/Range）各自的消费形态
- **管线状态族**：每个状态结构补 GL 映射表（cull/wireframe/depth_bias/stencil separate 等），如实记录 GL 缺口（MRT 混合仅首附件、depth bounds 未下发、logic op 需整型 FBO）
- **RenderPass 族**：Attachment/Subpass/Dependency 补"GL 忽略项 vs Vulkan 消费者"双向说明；附件引用下标语义
- **屏障族**：阶段位/访问位/三屏障的"执行序 vs 可见性"分离语义
- **查询族**：Limit 高频项（kMinUniformBufferOffsetAlignment 恒 256 等硬约束）、Features/ApiTrait/ShaderChangeInvalidation 各自的探测用途
- **修正**：MemoryAllocationType 成员注释（"主机可见显存"→CPU 可见内存）与误导性表述；kAttachmentUnused/kBufferWholeSize/kInvalidId 补单行文档

验证：全量 Debug 构建全绿（含 5 demo）。
