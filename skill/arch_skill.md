---
name: "arhud-render-engine"
description: "Design and implement an AR HUD rendering engine with multi-API (OpenGL/Vulkan) and multi-platform support. Invoke when user asks about ARHud engine design, rendering architecture, or GPU abstraction layer."
---

# ARHud 渲染引擎设计指南

本 Skill 基于 Godot 4.6 渲染架构的深度分析，为 AR HUD 场景定制一个轻量、高性能、跨平台跨 API 的渲染引擎设计蓝图。当前阶段优先对接 **OpenGL + Windows** 平台。

---

## 一、Godot 渲染架构核心梳理

### 1.1 三层 GPU API 抽象（核心设计模式）

Godot 4.x 采用了经典的三层抽象架构，这是整个渲染引擎的骨架：

```
┌─────────────────────────────────────────────────────┐
│                  RenderingServer                     │  ← 业务层：场景管理、资源调度
├─────────────────────────────────────────────────────┤
│                  RenderingDevice (RD)                │  ← 设备层：资源管理、Staging Buffer、命令图
├─────────────────────────────────────────────────────┤
│           RenderingDeviceDriver (RDD)                │  ← 驱动层：GPU API 的直接封装
├─────────────────────────────────────────────────────┤
│          RenderingContextDriver (RCD)                │  ← 上下文层：平台/API 实例化、Surface 管理
└─────────────────────────────────────────────────────┘
```

**关键源码参考：**
- [rendering_context_driver.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_context_driver.h) — 上下文驱动接口
- [rendering_device_driver.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_device_driver.h) — 设备驱动接口
- [rendering_device.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_device.h) — 高层渲染设备
- [rendering_device_commons.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_device_commons.h) — 共享枚举与类型

#### RenderingContextDriver（上下文驱动层）

职责：管理 GPU API 的实例化、设备枚举、Surface（窗口渲染表面）创建。

```cpp
// 核心接口（参考 Godot rendering_context_driver.h）
class RenderingContextDriver {
public:
    virtual Error initialize() = 0;                                    // 初始化 API 实例
    virtual const Device &device_get(uint32_t p_device_index) const = 0; // 枚举物理设备
    virtual uint32_t device_get_count() const = 0;
    virtual RenderingDeviceDriver *driver_create() = 0;                // 创建设备驱动
    virtual SurfaceID surface_create(const void *p_platform_data) = 0; // 创建渲染表面
    virtual void surface_set_size(SurfaceID, uint32_t w, uint32_t h) = 0;
    virtual void surface_set_vsync_mode(SurfaceID, VSyncMode) = 0;
    // ...
};
```

**Godot 实现：**
- Vulkan: [rendering_context_driver_vulkan.h](file:///d:/Work/godot-4.6/drivers/vulkan/rendering_context_driver_vulkan.h)
- D3D12: [rendering_context_driver_d3d12.h](file:///d:/Work/godot-4.6/drivers/d3d12/rendering_context_driver_d3d12.h)
- Windows 平台 Vulkan: [rendering_context_driver_vulkan_windows.h](file:///d:/Work/godot-4.6/platform/windows/rendering_context_driver_vulkan_windows.h)

#### RenderingDeviceDriver（设备驱动层）

职责：封装所有 GPU 资源操作，是 API 抽象的核心。使用强类型 ID 替代裸指针。

```cpp
// 资源 ID 定义（参考 Godot rendering_device_driver.h）
DEFINE_ID(Buffer);       // GPU 缓冲区
DEFINE_ID(Texture);      // 纹理
DEFINE_ID(Sampler);      // 采样器
DEFINE_ID(Shader);       // 着色器
DEFINE_ID(Pipeline);     // 渲染管线
DEFINE_ID(RenderPass);   // 渲染 Pass
DEFINE_ID(Framebuffer);  // 帧缓冲
DEFINE_ID(UniformSet);   // Uniform 集合
DEFINE_ID(CommandBuffer);// 命令缓冲
DEFINE_ID(SwapChain);    // 交换链
DEFINE_ID(Fence);        // 栅栏
DEFINE_ID(Semaphore);    // 信号量

// 核心操作接口
class RenderingDeviceDriver {
    // 缓冲区
    virtual BufferID buffer_create(uint64_t size, BitField<BufferUsageBits> usage, ...) = 0;
    virtual void buffer_free(BufferID) = 0;
    virtual uint8_t *buffer_map(BufferID) = 0;
    virtual void buffer_unmap(BufferID) = 0;

    // 纹理
    virtual TextureID texture_create(const TextureFormat &, const TextureView &) = 0;
    virtual void texture_free(TextureID) = 0;

    // 着色器
    virtual ShaderID shader_create_from_container(const Ref<RenderingShaderContainer> &, ...) = 0;
    virtual void shader_free(ShaderID) = 0;

    // 管线
    virtual PipelineID render_pipeline_create(...) = 0;
    virtual PipelineID compute_pipeline_create(ShaderID) = 0;
    virtual void pipeline_free(PipelineID) = 0;

    // 命令录制
    virtual void command_bind_render_pipeline(CommandBufferID, PipelineID) = 0;
    virtual void command_bind_uniform_set(CommandBufferID, UniformSetID, ShaderID, uint32_t) = 0;
    virtual void command_draw_array(CommandBufferID, uint32_t, uint32_t) = 0;
    virtual void command_draw_indexed(CommandBufferID, uint32_t, uint32_t) = 0;
    // ...
};
```

**设计原则（来自 Godot 源码注释）：**
1. 极少验证，仅在 Debug 构建中做
2. 错误报告简单：返回 id=0 或 false
3. 枚举/常量/结构体尽量对齐 Vulkan 值，使 Vulkan 驱动可直接 assert 兼容
4. 热路径尽量零分配，使用 `alloca()`
5. 使用 `PagedAllocator` 管理簿记结构
6. 使用 `VectorView` 传递数组参数，避免拷贝

#### RenderingDevice（设备层）

职责：在 RDD 之上提供资源生命周期管理、Staging Buffer、命令图（RDG）、依赖追踪。

```cpp
// 关键结构（参考 Godot rendering_device.h）
class RenderingDevice {
    RenderingContextDriver *context;
    RenderingDeviceDriver *driver;

    // Staging Buffer 管理（CPU→GPU 数据传输）
    struct StagingBufferBlock {
        RDD::BufferID driver_id;
        uint64_t frame_used;
        uint32_t fill_amount;
        uint8_t *data_ptr;       // mmap 指针
    };

    // 资源 RID 管理
    RID_Owner<Buffer, true> uniform_buffer_owner;
    RID_Owner<Buffer, true> storage_buffer_owner;
    RID_Owner<Texture, true> texture_owner;
    // ...

    // 依赖追踪
    HashMap<RID, HashSet<RID>> dependency_map;
    HashMap<RID, HashSet<RID>> reverse_dependency_map;
};
```

### 1.2 渲染器组合器模式（RendererCompositor）

Godot 使用组合器模式统一不同渲染后端：

```
┌──────────────────────────────────────────┐
│           RendererCompositor             │  ← 统一接口
├──────────────────┬───────────────────────┤
│ RendererComposit │   RasterizerGLES3     │  ← 两个实现
│     orRD         │   (OpenGL 后端)        │
│ (Vulkan/D3D12/   │                       │
│  Metal 后端)      │                       │
├──────────────────┴───────────────────────┤
│  Storage 层（Texture/Material/Mesh/      │
│  Light/Particles/Utilities）             │
├──────────────────────────────────────────┤
│  Scene Renderer / Canvas Renderer        │
└──────────────────────────────────────────┘
```

**关键源码参考：**
- [renderer_compositor.h](file:///d:/Work/godot-4.6/servers/rendering/renderer_compositor.h) — 组合器接口
- [renderer_compositor_rd.h](file:///d:/Work/godot-4.6/servers/rendering/renderer_rd/renderer_compositor_rd.h) — RD 后端实现
- [rasterizer_gles3.h](file:///d:/Work/godot-4.6/drivers/gles3/rasterizer_gles3.h) — GLES3 后端实现

组合器持有所有 Storage 子系统：

```cpp
class RendererCompositor {
    virtual RendererUtilities *get_utilities() = 0;
    virtual RendererLightStorage *get_light_storage() = 0;
    virtual RendererMaterialStorage *get_material_storage() = 0;
    virtual RendererMeshStorage *get_mesh_storage() = 0;
    virtual RendererParticlesStorage *get_particles_storage() = 0;
    virtual RendererTextureStorage *get_texture_storage() = 0;
    virtual RendererCanvasRender *get_canvas() = 0;
    virtual RendererSceneRender *get_scene() = 0;
};
```

### 1.3 Storage 模式（资源管理）

每种 GPU 资源类型都有独立的 Storage 类，接口定义在 `servers/rendering/storage/`，实现在各后端目录。

**关键接口：**
- [texture_storage.h](file:///d:/Work/godot-4.6/servers/rendering/storage/texture_storage.h) — 纹理存储接口
- [material_storage.h](file:///d:/Work/godot-4.6/servers/rendering/storage/material_storage.h) — 材质存储接口
- [mesh_storage.h](file:///d:/Work/godot-4.6/servers/rendering/storage/mesh_storage.h) — 网格存储接口
- [light_storage.h](file:///d:/Work/godot-4.6/servers/rendering/storage/light_storage.h) — 光照存储接口

GLES3 实现示例：
- [gles3/storage/texture_storage.h](file:///d:/Work/godot-4.6/drivers/gles3/storage/texture_storage.h)
- [gles3/storage/material_storage.h](file:///d:/Work/godot-4.6/drivers/gles3/storage/material_storage.h)
- [gles3/storage/mesh_storage.h](file:///d:/Work/godot-4.6/drivers/gles3/storage/mesh_storage.h)

### 1.4 着色器系统

Godot 有两套着色器管线：

**RD 路径（Vulkan/D3D12/Metal）：**
```
GLSL 源码 → glslang 编译 → SPIRV → RenderingShaderContainer → RDD::shader_create_from_container()
```
参考：[rendering_shader_container.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_shader_container.h)

**GLES3 路径（OpenGL）：**
```
GLSL 源码 → ShaderGLES3（运行时编译链接）→ GLuint program
```
参考：[shader_gles3.h](file:///d:/Work/godot-4.6/drivers/gles3/shader_gles3.h)

ShaderGLES3 的关键设计：
- **Version**：用户着色器代码 = 内置定义 + 用户代码
- **Variant**：通过 `#ifdef` 切换行为（如深度预pass vs 颜色pass）
- **Specialization**：通过 `#ifdef` 切换性能特征（如启用/禁用高级特性）

### 1.5 平台抽象层

**DisplayServer** — 窗口管理与输入：
- [display_server.h](file:///d:/Work/godot-4.6/servers/display/display_server.h)
- Windows: [display_server_windows.h](file:///d:/Work/godot-4.6/platform/windows/display_server_windows.h)

**OpenGL 上下文管理**（Windows 平台）：
- [gl_manager_windows_native.h](file:///d:/Work/godot-4.6/platform/windows/gl_manager_windows_native.h) — 原生 WGL + GL 上下文
- [gl_manager_windows_angle.h](file:///d:/Work/godot-4.6/platform/windows/gl_manager_windows_angle.h) — ANGLE (OpenGL ES → D3D11)

**Vulkan 上下文管理**（Windows 平台）：
- [rendering_context_driver_vulkan_windows.h](file:///d:/Work/godot-4.6/platform/windows/rendering_context_driver_vulkan_windows.h)

**Android 平台**：
- [display_server_android.h](file:///d:/Work/godot-4.6/platform/android/display_server_android.h)
- [GodotVulkanRenderView.kt](file:///d:/Work/godot-4.6/platform/android/java/lib/src/main/java/org/godotengine/godot/GodotVulkanRenderView.kt)
- [GodotGLRenderView.kt](file:///d:/Work/godot-4.6/platform/android/java/lib/src/main/java/org/godotengine/godot/GodotGLRenderView.kt)

### 1.6 渲染命令图（RenderingDeviceGraph）

Godot 4.x 引入了渲染图（RDG）来优化命令提交，这是整个渲染管线中最复杂的子系统之一。

参考：[rendering_device_graph.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_device_graph.h)

#### 1.6.1 核心设计思想

RDG 的核心目标是**延迟命令执行**——将所有 GPU 命令先记录为数据，在帧末统一编译、排序、插入屏障后提交。这使得：

1. **自动屏障推导**：通过资源追踪器（ResourceTracker）自动计算 Memory/Buffer/Texture Barrier
2. **命令重排序**：无依赖的命令可并行执行，提升 GPU 利用率
3. **屏障合并**：相邻命令的屏障可合并，减少管线停滞
4. **跨 API 兼容**：Vulkan 需要精确屏障，OpenGL 可忽略屏障直接执行

```
┌──────────────────────────────────────────────────────────────────┐
│                    RenderingDeviceGraph 工作流                    │
│                                                                  │
│  1. begin()          — 重置命令缓冲、资源追踪器                   │
│  2. add_xxx_command  — 记录命令到指令流（序列化字节）             │
│     ├── add_draw_list_begin/end     — 绘制命令                   │
│     ├── add_compute_list_begin/end  — 计算命令                   │
│     ├── add_buffer_copy/clear       — 缓冲区操作                 │
│     ├── add_texture_copy/update     — 纹理操作                   │
│     └── add_capture_timestamp       — 时间戳                     │
│  3. end()            — 编译图、排序、插入屏障、录制到 CmdBuffer   │
│     ├── _add_command_to_graph       — 建立资源依赖图             │
│     ├── _group_barriers_for_render_commands — 合并屏障           │
│     └── _run_render_commands        — 回放命令到 Vulkan CmdBuf   │
└──────────────────────────────────────────────────────────────────┘
```

#### 1.6.2 命令类型体系

```cpp
struct RecordedCommand {
    enum Type {
        TYPE_BUFFER_CLEAR,          // 清除缓冲区
        TYPE_BUFFER_COPY,           // 缓冲区拷贝
        TYPE_BUFFER_GET_DATA,       // GPU→CPU 回读
        TYPE_BUFFER_UPDATE,         // CPU→GPU 更新
        TYPE_COMPUTE_LIST,          // 计算列表（含多条指令）
        TYPE_DRAW_LIST,             // 绘制列表（含多条指令）
        TYPE_TEXTURE_CLEAR_COLOR,   // 清除颜色纹理
        TYPE_TEXTURE_CLEAR_DEPTH_STENCIL, // 清除深度/模板
        TYPE_TEXTURE_COPY,          // 纹理拷贝
        TYPE_TEXTURE_GET_DATA,      // 纹理回读
        TYPE_TEXTURE_RESOLVE,       // 多采样解析
        TYPE_TEXTURE_UPDATE,        // 纹理上传
        TYPE_CAPTURE_TIMESTAMP,     // 捕获时间戳
        TYPE_DRIVER_CALLBACK,       // 驱动回调
    };

    // 依赖追踪信息
    RDD::MemoryAccessBarrier memory_barrier;
    int32_t normalization_barrier_index;
    int32_t transition_barrier_index;
    int32_t buffer_barrier_index;
    BitField<RDD::PipelineStageBits> previous_stages;
    BitField<RDD::PipelineStageBits> next_stages;
    BitField<RDD::PipelineStageBits> self_stages;
};
```

**绘制/计算指令采用序列化字节流存储**（而非多态对象），减少内存碎片和分配开销：

```cpp
// 指令序列化示例：DrawListBindPipelineInstruction
struct DrawListBindPipelineInstruction : DrawListInstruction {
    RDD::PipelineID pipeline;       // 8 bytes
    // type 字段在基类 DrawListInstruction 中（4 bytes）
};
// 总大小 = sizeof(DrawListInstruction) + sizeof(RDD::PipelineID)
// 直接 memcpy 到连续字节流中
```

#### 1.6.3 ResourceTracker — 资源追踪器

ResourceTracker 是 RDG 的核心数据结构，追踪每个 GPU 资源在当前帧中的使用情况：

```cpp
struct ResourceTracker {
    uint32_t reference_count = 0;
    int64_t command_frame = -1;         // 当前帧号（用于跨帧重置检测）

    // 管线阶段追踪
    BitField<RDD::PipelineStageBits> previous_frame_stages;
    BitField<RDD::PipelineStageBits> current_frame_stages;

    // 读写依赖链（链表结构）
    int32_t read_full_command_list_index = -1;    // 完整读依赖链头
    int32_t read_slice_command_list_index = -1;   // 子资源读依赖链头
    int32_t write_command_or_list_index = -1;     // 写依赖链头

    // Draw/Compute 列表中的使用
    int32_t draw_list_index = -1;
    ResourceUsage draw_list_usage = RESOURCE_USAGE_NONE;
    int32_t compute_list_index = -1;
    ResourceUsage compute_list_usage = RESOURCE_USAGE_NONE;

    // 资源标识
    RDD::BufferID buffer_driver_id;
    RDD::TextureID texture_driver_id;
    RDD::TextureSubresourceRange texture_subresources;
    uint32_t texture_usage = 0;

    // 层级关系（纹理切片的父子追踪）
    ResourceTracker *parent = nullptr;
    ResourceTracker *dirty_shared_list = nullptr;
    ResourceTracker *next_shared = nullptr;

    // 状态标记
    bool write_command_list_enabled = false;
    bool is_discardable = false;         // 可丢弃（无需 loadOp）

    void reset_if_outdated(int64_t new_command_frame) {
        if (new_command_frame != command_frame) {
            command_frame = new_command_frame;
            previous_frame_stages = current_frame_stages;
            current_frame_stages.clear();
            // 重置所有索引...
        }
    }
};
```

**关键设计：**
- **链表结构**：读写依赖使用链表（`RecordedCommandListNode` / `RecordedSliceListNode`）而非数组，避免频繁 realloc
- **子资源追踪**：纹理可以按 mip/layer 切片追踪，支持部分屏障（如只 barrier 某个 mip level）
- **可丢弃标记**：如果一个 attachment 在当前帧首次使用时是 clear 操作，则标记为 discardable，避免不必要的 loadOp
- **跨帧检测**：`command_frame` 确保每帧开始时自动重置追踪状态

#### 1.6.4 屏障推导算法

```
当新命令 N 使用资源 R 时：

1. 检查 R 的写依赖链：
   - 如果之前有命令 W 写入了 R → 需要 W→N 的屏障
   - 屏障类型由 W 的 usage 和 N 的 usage 决定

2. 检查 R 的读依赖链：
   - 如果之前有命令 R1 读取了 R，且 N 要写入 R → 需要 R1→N 的屏障
   - 如果 N 只是读取 R → 无需屏障（读-读无冲突）

3. 屏障参数推导：
   - src_stages = 前序命令的 pipeline stages
   - dst_stages = N 的 pipeline stages
   - src_access = _usage_to_access_bits(prev_usage)
   - dst_access = _usage_to_access_bits(curr_usage)
   - image_layout = _usage_to_image_layout(curr_usage)  // Vulkan 纹理布局转换

4. 屏障合并：
   - 相邻命令的屏障合并到 BarrierGroup 中
   - normalization_barriers: 纹理布局转换
   - transition_barriers: 纹理访问权限转换
   - buffer_barriers: 缓冲区访问权限转换（可选，默认关闭）
   - memory_barrier: 全局内存屏障（兜底）
```

**ResourceUsage → Vulkan 映射：**

| ResourceUsage | Image Layout | Access Bits |
|---------------|-------------|-------------|
| COPY_FROM | COPY_SRC_OPTIMAL | COPY_READ_BIT |
| COPY_TO | COPY_DST_OPTIMAL | COPY_WRITE_BIT |
| TEXTURE_SAMPLE | SHADER_READ_ONLY_OPTIMAL | SHADER_READ_BIT |
| ATTACHMENT_COLOR_READ_WRITE | COLOR_ATTACHMENT_OPTIMAL | COLOR_ATTACHMENT_READ/WRITE_BIT |
| ATTACHMENT_DEPTH_STENCIL_READ_WRITE | DEPTH_STENCIL_ATTACHMENT_OPTIMAL | DEPTH_STENCIL_READ/WRITE_BIT |
| STORAGE_IMAGE_READ_WRITE | STORAGE_OPTIMAL | SHADER_READ/WRITE_BIT |

#### 1.6.5 帧循环集成

```cpp
// RenderingDevice 帧循环
void RenderingDevice::_begin_frame() {
    driver->begin_segment(frame, frames_drawn++);
    driver->command_buffer_begin(frames[frame].command_buffer);

    // 重置 RDG
    draw_graph.begin();

    // 释放待回收资源
    _free_pending_resources(frame);

    // 推进 Staging Buffer 环形索引
    // ...
}

void RenderingDevice::_end_frame() {
    // 编译 RDG → 录制到 CommandBuffer
    draw_graph.end(
        RENDER_GRAPH_REORDER,     // 是否重排序命令
        RENDER_GRAPH_FULL_BARRIERS, // 是否使用全内存屏障
        command_buffer,
        frames[frame].command_buffer_pool
    );

    driver->command_buffer_end(command_buffer);
    driver->end_segment();
}
```

**RENDER_GRAPH_REORDER**：启用时，无依赖的命令会按优先级排序（如将 Compute 命令提前，与 Draw 命令并行）。

**RENDER_GRAPH_FULL_BARRIERS**：启用时，在每对相邻命令间插入全内存屏障（`MEMORY_READ | MEMORY_WRITE`），用于调试。

#### 1.6.6 OpenGL vs Vulkan 的 RDG 差异

| 特性 | Vulkan | OpenGL |
|------|--------|--------|
| 命令录制 | 延迟到 `end()` 时回放到 VkCommandBuffer | 立即模式，`add_xxx` 直接调用 GL 函数 |
| 屏障 | 必须精确指定 `vkCmdPipelineBarrier` | `glMemoryBarrier` 足矣，甚至可忽略 |
| 命令重排序 | 有效（GPU 可并行执行独立 CmdBuffer） | 无意义（GL 是顺序执行） |
| 纹理布局转换 | 必须通过 `vkCmdPipelineBarrier` 转换 layout | 不需要（GL 无 layout 概念） |
| 二级命令缓冲 | 支持（多线程录制） | 不支持（GL 上下文单线程） |

**结论**：OpenGL 后端可以使用 RDG 的**记录接口**，但**跳过屏障推导和命令重排序**，直接回放命令。这为 ARHud 的 RDG Lite 设计提供了方向。

### 1.7 内存管理系统

Godot 的内存管理是整个引擎的基础设施，为所有子系统提供统一的分配/释放接口，并支持内存统计与泄漏检测。

**关键源码参考：**
- [memory.h](file:///d:/Work/godot-4.6/core/os/memory.h) — 全局内存分配器
- [paged_allocator.h](file:///d:/Work/godot-4.6/core/templates/paged_allocator.h) — 分页内存分配器
- [safe_refcount.h](file:///d:/Work/godot-4.6/core/templates/safe_refcount.h) — 原子引用计数
- [typedefs.h](file:///d:/Work/godot-4.6/core/typedefs.h) — 基础类型与编译器宏

#### 1.7.1 全局内存分配器（Memory 命名空间）

Godot 不使用 C 标准库的 malloc/free，而是通过 `Memory` 命名空间提供统一接口，所有分配都经过全局钩子，支持内存统计：

```cpp
namespace Memory {
    // 基础分配（p_pad_align=true 时在头部记录元素数量和大小）
    template <bool p_ensure_zero = false>
    void *alloc_static(size_t p_bytes, bool p_pad_align = false);
    void *realloc_static(void *p_memory, size_t p_bytes, bool p_pad_align = false);
    void free_static(void *p_ptr, bool p_pad_align = false);

    // 对齐分配（用于 SIMD / GPU 对齐场景）
    // 内部布局：[padding][uint32_t offset][p_bytes][padding]
    // offset 记录了返回指针相对于真实分配起始的偏移
    void *alloc_aligned_static(size_t p_bytes, size_t p_alignment);
    void *realloc_aligned_static(void *p_memory, size_t p_bytes, size_t p_prev_bytes, size_t p_alignment);
    void free_aligned_static(void *p_memory);

    // 内存统计
    uint64_t get_mem_available();
    uint64_t get_mem_usage();       // 当前使用量
    uint64_t get_mem_max_usage();   // 峰值使用量
};
```

**p_pad_align 模式下的内存布局：**
```
Alignment:  ↓ max_align_t        ↓ uint64_t          ↓ MAX_ALIGN
            ┌─────────────────┬──┬────────────────┬──┬───────────...
            │ uint64_t        │░░│ uint64_t       │░░│ T[]
            │ alloc size      │░░│ element count  │░░│ data
            └─────────────────┴──┴────────────────┴──┴───────────...
Offset:     ↑ SIZE_OFFSET        ↑ ELEMENT_OFFSET    ↑ DATA_OFFSET
```

#### 1.7.2 对象构造/析构宏

Godot 通过宏封装了 `operator new` + 构造函数调用，确保所有对象分配都经过 Memory 系统：

```cpp
// 单对象分配（调用 operator new(size_t, description) 重载）
#define memnew(m_class) _post_initialize(::new ("") m_class)

// 带自定义分配器的对象分配
#define memnew_allocator(m_class, m_allocator) _post_initialize(::new (m_allocator::alloc) m_class)

// 就地构造（placement new）
#define memnew_placement(m_placement, m_class) _post_initialize(::new (m_placement) m_class)

// 数组分配（记录元素数量，支持非平凡析构）
#define memnew_arr(m_class, m_count) memnew_arr_template<m_class>(m_count)

// 对象析构 + 释放
template <typename T>
void memdelete(T *p_class) {
    if (!predelete_handler(p_class)) return;
    if constexpr (!std::is_trivially_destructible_v<T>) {
        p_class->~T();
    }
    Memory::free_static(p_class, false);
}

// C 风格分配（不调用构造函数）
#define memalloc(m_size) Memory::alloc_static(m_size)
#define memrealloc(m_mem, m_size) Memory::realloc_static(m_mem, m_size)
#define memfree(m_mem) Memory::free_static(m_mem)
```

**设计要点：**
- `_post_initialize` 在构造后调用 `postinitialize_handler`，Godot 用它来设置引用计数等
- `memdelete` 使用 `if constexpr` 避免对平凡析构类型调用空析构函数
- `memnew_arr` 不使用 `operator new[]`，因为 C++ 标准不保证 `new[]` 返回的指针与分配起始一致

#### 1.7.3 PagedAllocator（分页内存分配器）

Godot 渲染系统中大量使用 `PagedAllocator` 来管理固定大小的小对象（如渲染命令、资源簿记等），避免频繁调用全局分配器：

```cpp
template <typename T, bool thread_safe = false, uint32_t DEFAULT_PAGE_SIZE = 4096>
class PagedAllocator {
    T **page_pool = nullptr;        // 已分配的内存页
    T ***available_pool = nullptr;  // 可用槽位指针
    uint32_t pages_allocated = 0;
    uint32_t allocs_available = 0;
    SpinLock spin_lock;             // 可选的线程安全锁

    // 分配一个 T 对象（从可用池中取出一个槽位，就地构造）
    template <typename... Args>
    T *alloc(Args &&...p_args);

    // 释放一个 T 对象（析构后归还到可用池）
    void free(T *p_mem);
};
```

**工作原理：**
1. 按 `page_size`（2 的幂次）为单位批量分配内存页
2. 每页包含 `page_size` 个 T 对象槽位
3. `available_pool` 维护一个可用槽位栈（LIFO），alloc 从栈顶取，free 归还到栈顶
4. `page_shift` 和 `page_mask` 用于快速定位槽位所在的页和页内偏移
5. `thread_safe=true` 时使用 SpinLock 保护

**在 Godot 渲染系统中的使用：**
- `RenderingDeviceGraph` 中分配 Draw/Compute/Copy 等渲染命令节点
- `RenderingDevice` 中分配各种资源的簿记结构
- 渲染线程与主线程共享时使用 `thread_safe=true`

#### 1.7.4 SafeNumeric（原子操作）

Godot 提供了 `SafeNumeric<T>` 封装 `std::atomic<T>`，使用 acquire-release 语义：

```cpp
template <typename T>
class SafeNumeric {
    std::atomic<T> value;
    static_assert(std::atomic<T>::is_always_lock_free);

    void set(T p_value);                    // memory_order_release
    T get() const;                          // memory_order_acquire
    T increment();                          // fetch_add + 1
    T decrement();                          // fetch_sub - 1
    T add(T p_value);                       // fetch_add + p_value
    bool conditional_increment();            // CAS 循环
};

class SafeFlag {
    std::atomic<bool> flag;
    void set();                             // memory_order_release
    bool is_set() const;                    // memory_order_acquire
    void clear();                           // memory_order_release
    bool is_set_clear();                    // memory_order_acq_rel (test_and_set)
};
```

**设计原则：**
- 不提供自动转换或算术运算符，保持原子操作显式化
- 统一使用 acquire-release 语义（而非 relaxed），确保多线程同步正确性
- `static_assert` 保证无锁（lock-free）

### 1.8 OS 抽象层（线程与同步）

Godot 将所有操作系统相关的功能抽象为独立模块，确保上层代码不依赖特定平台 API。

**关键源码参考：**
- [thread.h](file:///d:/Work/godot-4.6/core/os/thread.h) — 线程抽象
- [mutex.h](file:///d:/Work/godot-4.6/core/os/mutex.h) — 互斥锁
- [semaphore.h](file:///d:/Work/godot-4.6/core/os/semaphore.h) — 信号量
- [rw_lock.h](file:///d:/Work/godot-4.6/core/os/rw_lock.h) — 读写锁
- [spin_lock.h](file:///d:/Work/godot-4.6/core/os/spin_lock.h) — 自旋锁
- [time.h](file:///d:/Work/godot-4.6/core/os/time.h) — 时间管理
- [os.h](file:///d:/Work/godot-4.6/core/os/os.h) — 操作系统抽象

#### 1.8.1 Thread（线程抽象）

```cpp
class Thread {
public:
    typedef void (*Callback)(void *p_userdata);
    typedef uint64_t ID;

    enum : ID { UNASSIGNED_ID = 0, MAIN_ID = 1 };
    enum Priority { PRIORITY_LOW, PRIORITY_NORMAL, PRIORITY_HIGH };

    struct Settings {
        Priority priority = PRIORITY_NORMAL;
    };

    struct PlatformFunctions {
        Error (*set_name)(const String &) = nullptr;
        void (*set_priority)(Thread::Priority) = nullptr;
        void (*init)() = nullptr;
        void (*wrapper)(Thread::Callback, void *) = nullptr;
        void (*term)() = nullptr;
    };

    static constexpr size_t CACHE_LINE_BYTES = 128;  // 避免伪共享

    ID start(Thread::Callback p_callback, void *p_user, const Settings &p_settings = Settings());
    bool is_started() const;
    void wait_to_finish();

    static ID get_caller_id();            // 获取调用者线程 ID
    static ID get_main_id();              // 主线程 ID
    static bool is_main_thread();         // 是否在主线程
    static void yield();                  // 让出时间片
};
```

**关键设计：**
- **PlatformFunctions**：平台可注入自定义的线程包装函数，用于 QNX 等特殊平台的线程初始化/清理
- **CACHE_LINE_BYTES**：用于内存对齐，避免伪共享（false sharing）
- **THREADS_ENABLED 宏**：当禁用线程时，所有方法变为空操作，实现零成本抽象
- **MinGW 兼容**：使用 `mingw-std-threads` 第三方库替代 Windows 原生不完整的 C++ 线程实现

#### 1.8.2 Mutex / BinaryMutex（互斥锁）

```cpp
// 递归互斥锁（通用场景）
using Mutex = MutexImpl<THREADING_NAMESPACE::recursive_mutex>;

// 非递归互斥锁（高性能场景，需小心使用）
using BinaryMutex = MutexImpl<THREADING_NAMESPACE::mutex>;

// RAII 锁守卫
template <typename MutexT>
class MutexLock {
    THREADING_NAMESPACE::unique_lock<typename MutexT::StdMutexType> lock;
    void temp_unlock();   // 临时解锁（用于条件等待）
    void temp_relock();   // 重新加锁
};
```

**设计要点：**
- `Mutex`（递归）用于一般场景，允许同一线程多次加锁
- `BinaryMutex`（非递归）用于性能敏感路径，与 `std::condition_variable` 配合使用
- `MutexLock` 的 `temp_unlock/relock` 用于条件变量等待模式
- 无线程模式（`THREADS_ENABLED=false`）下所有方法为空操作

#### 1.8.3 Semaphore（信号量）

```cpp
class Semaphore {
    mutable THREADING_NAMESPACE::mutex mutex;
    mutable THREADING_NAMESPACE::condition_variable condition;
    mutable uint32_t count = 0;

    void post(uint32_t p_count = 1) const;  // 释放信号量
    void wait() const;                       // 等待信号量
    bool try_wait() const;                   // 尝试等待
};
```

**Godot 渲染系统中的使用：**
- 主线程与渲染线程之间的帧同步
- `RenderingDevice::swap_buffers` 中等待前一帧 GPU 完成
- 生产者-消费者模式：主线程提交命令，渲染线程消费执行

#### 1.8.4 RWLock（读写锁）

```cpp
class RWLock {
    mutable THREADING_NAMESPACE::shared_timed_mutex mutex;

    void read_lock() const;     // 共享读锁
    void read_unlock() const;
    bool read_try_lock() const;
    void write_lock();          // 独占写锁
    void write_unlock();
    bool write_try_lock();
};

// RAII 守卫
class RWLockRead { ... };   // 构造时读锁，析构时解锁
class RWLockWrite { ... };  // 构造时写锁，析构时解锁
```

**使用场景：** 渲染资源（纹理/材质/网格）的读多写少场景——渲染线程频繁读取，主线程偶尔更新。

#### 1.8.5 SpinLock（自旋锁）

```cpp
class SpinLock {
    union {
        mutable std::atomic_flag _flag = ATOMIC_FLAG_INIT;
        char aligner[Thread::CACHE_LINE_BYTES];  // 避免伪共享
    };

    void lock() const;    // 自旋等待（_cpu_pause + test_and_set）
    void unlock() const;  // clear
};
```

**设计要点：**
- 使用 `CACHE_LINE_BYTES` 对齐，避免伪共享
- 平台特定的 `_cpu_pause()`：x86 用 `_mm_pause()`，ARM 用 `__yield()`
- 用于极短临界区（如 PagedAllocator 的分配/释放）
- 不使用 OS 级锁，纯用户态自旋

#### 1.8.6 Time（时间管理）

```cpp
class Time : public Object {
    static Time *singleton;

    // 高精度计时（渲染帧计时核心）
    uint64_t get_ticks_msec() const;   // 毫秒
    uint64_t get_ticks_usec() const;   // 微秒

    // 系统时间
    double get_unix_time_from_system() const;
    Dictionary get_datetime_dict_from_system(bool p_utc = false) const;
};
```

**OS 类中的时间方法：**
```cpp
class OS {
    virtual uint64_t get_ticks_usec() const = 0;  // 平台实现的高精度计时
    uint64_t get_ticks_msec() const;               // 基于 get_ticks_usec
    virtual void delay_usec(uint32_t p_usec) const = 0;  // 精确延时
    virtual double get_unix_time() const;           // Unix 时间戳
};
```

### 1.9 平台抽象层（文件/目录/日志/系统信息）

#### 1.9.1 FileAccess（文件访问）

```cpp
class FileAccess : public RefCounted {
public:
    enum AccessType {
        ACCESS_RESOURCES,    // 只读资源目录
        ACCESS_USERDATA,     // 用户数据目录（可写）
        ACCESS_FILESYSTEM,   // 完整文件系统
        ACCESS_PIPE,         // 管道
    };

    enum ModeFlags {
        READ = 1, WRITE = 2, READ_WRITE = 3, WRITE_READ = 7,
        SKIP_PACK = 16,
    };

    // 工厂方法
    static Ref<FileAccess> open(const String &p_path, int p_mode_flags, ...);
    static Ref<FileAccess> create_for_path(const String &p_path);

    // 核心操作
    virtual Error open_internal(const String &p_path, int p_mode_flags) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    virtual uint64_t get_length() const = 0;
    virtual uint64_t get_position() const = 0;
    virtual size_t get_buffer(uint8_t *p_dst, size_t p_length) const = 0;

    // 读写基础类型
    uint8_t get_8() const;
    uint16_t get_16() const;
    uint32_t get_32() const;
    uint64_t get_64() const;
    float get_float() const;
    double get_double() const;

    // 平台实现
    virtual uint64_t _get_modified_time(const String &p_file) = 0;
    virtual BitField<UnixPermissionFlags> _get_unix_permissions(const String &p_file) = 0;
    virtual bool _get_hidden_attribute(const String &p_file) = 0;
};
```

**平台实现：**
- Windows: [file_access_windows.h](file:///d:/Work/godot-4.6/platform/windows/file_access_windows.h)
- Linux: [file_access_unix.h](file:///d:/Work/godot-4.6/platform/linuxbsd/file_access_unix.h)
- Android: 使用 Unix 实现 + AAssetManager for APK 内资源

**关键设计：**
- `AccessType` 区分不同访问域，`fix_path()` 自动映射到平台路径
- `RefCounted` 引用计数管理生命周期
- `last_file_open_error` 线程局部存储，避免多线程冲突

#### 1.9.2 DirAccess（目录访问）

```cpp
class DirAccess : public RefCounted {
public:
    enum AccessType { ACCESS_RESOURCES, ACCESS_USERDATA, ACCESS_FILESYSTEM, ACCESS_MAX };

    virtual Error list_dir_begin() = 0;    // 开始遍历
    virtual String get_next() = 0;          // 获取下一个条目
    virtual bool current_is_dir() const = 0;
    virtual bool current_is_hidden() const = 0;
    virtual void list_dir_end() = 0;        // 结束遍历

    virtual Error change_dir(String p_dir) = 0;
    virtual String get_current_dir(bool p_include_drive = true) const = 0;
    virtual Error make_dir(String p_dir) = 0;
    virtual Error make_dir_recursive(const String &p_dir);

    virtual bool file_exists(String p_file) = 0;
    virtual bool dir_exists(String p_dir) = 0;
    virtual Error copy(const String &p_from, const String &p_to, int p_chmod_flags = -1);
    virtual Error rename(String p_from, String p_to) = 0;
    virtual Error remove(String p_name) = 0;

    virtual bool is_link(String p_file) = 0;
    virtual String read_link(String p_file) = 0;
    virtual Error create_link(String p_source, String p_target) = 0;
};
```

#### 1.9.3 Logger（日志系统）

```cpp
class Logger {
public:
    enum ErrorType { ERR_ERROR, ERR_WARNING, ERR_SCRIPT, ERR_SHADER };

    virtual void logv(const char *p_format, va_list p_list, bool p_err) = 0;
    virtual void log_error(const char *p_function, const char *p_file, int p_line,
                           const char *p_code, const char *p_rationale,
                           bool p_editor_notify = false,
                           ErrorType p_type = ERR_ERROR, ...);
};

// 标准输出日志
class StdLogger : public Logger { ... };

// 轮转文件日志（自动备份，最大文件数可配）
class RotatedFileLogger : public Logger {
    String base_path;
    int max_files;
    Ref<FileAccess> file;
    void clear_old_backups();
    void rotate_file();
};

// 组合日志（同时输出到多个 Logger）
class CompositeLogger : public Logger {
    Vector<Logger *> loggers;
};
```

**OS 类中的日志集成：**
```cpp
class OS {
    CompositeLogger *_logger = nullptr;
    void add_logger(Logger *p_logger);
    void print_error(const char *p_function, const char *p_file, int p_line,
                     const char *p_code, const char *p_rationale, ...);
    void print(const char *p_format, ...);
    void printerr(const char *p_format, ...);
};
```

#### 1.9.4 OS（操作系统抽象）

OS 是 Godot 最顶层的平台抽象，提供所有与操作系统交互的接口：

```cpp
class OS {
    static OS *singleton;

    // 生命周期
    virtual void initialize() = 0;
    virtual void finalize() = 0;
    virtual void finalize_core() = 0;

    // 进程管理
    virtual Error execute(const String &p_path, const List<String> &p_args, ...) = 0;
    virtual Error create_process(const String &p_path, const List<String> &p_args, ...) = 0;
    virtual Error kill(const ProcessID &p_pid) = 0;
    virtual int get_process_id() const;

    // 环境变量
    virtual bool has_environment(const String &p_var) const = 0;
    virtual String get_environment(const String &p_var) const = 0;
    virtual void set_environment(const String &p_var, const String &p_value) const = 0;

    // 系统信息
    virtual String get_name() const = 0;          // "Windows", "Linux", "Android", etc.
    virtual int get_processor_count() const;
    virtual String get_processor_name() const;

    // 动态库加载
    virtual Error open_dynamic_library(const String &p_path, void *&p_library_handle, ...) = 0;
    virtual Error close_dynamic_library(void *p_library_handle) = 0;
    virtual Error get_dynamic_library_symbol_handle(void *p_library_handle, const String &p_name, ...) = 0;

    // 内存统计
    virtual uint64_t get_static_memory_usage() const;
    virtual uint64_t get_static_memory_peak_usage() const;

    // 路径
    virtual String get_data_path() const;
    virtual String get_config_path() const;
    virtual String get_cache_path() const;
    virtual String get_user_data_dir() const;
    virtual String get_resource_dir() const;

    // 渲染线程模式
    enum RenderThreadMode {
        RENDER_THREAD_UNSAFE,      // 单线程渲染
        RENDER_THREAD_SAFE,        // 多线程安全
        RENDER_SEPARATE_THREAD,    // 独立渲染线程
    };
};
```

### 1.10 容器与模板

**关键源码参考：**
- [vector.h](file:///d:/Work/godot-4.6/core/templates/vector.h) — 写时复制动态数组
- [local_vector.h](file:///d:/Work/godot-4.6/core/templates/local_vector.h) — 本地动态数组
- [rid.h](file:///d:/Work/godot-4.6/core/templates/rid.h) — 资源 ID 系统
- [rb_map.h](file:///d:/Work/godot-4.6/core/templates/rb_map.h) — 红黑树映射
- [hash_map.h](file:///d:/Work/godot-4.6/core/templates/hash_map.h) — 哈希映射
- [list.h](file:///d:/Work/godot-4.6/core/templates/list.h) — 链表

#### 1.10.1 Vector（写时复制动态数组）

```cpp
template <typename T>
class Vector {
    // 写时复制（Copy-on-Write）语义
    // 多个 Vector 共享同一底层数据，仅在修改时才复制
    Vector write;  // 触发 COW
    const T *ptr() const;
    T *ptrw();     // 可写指针，触发 COW

    void push_back(const T &p_elem);
    void remove_at(int p_index);
    void resize(int p_size);
    void reserve(int p_size);
    int size() const;
    bool is_empty() const;
};
```

**COW 机制：** 适用于共享只读数据的场景（如资源引用），但在频繁修改时性能较差。

#### 1.10.2 LocalVector（本地动态数组）

```cpp
template <typename T, typename U = uint32_t, bool force_trivial = false, bool tight = false>
class LocalVector {
    U count = 0;
    U capacity = 0;
    T *data = nullptr;

    void push_back(T p_elem);
    void remove_at(U p_index);
    void remove_at_unordered(U p_index);  // O(1) 删除（不保序）
    U find(const T &p_val) const;
    void sort();
    void resize(U p_size);
    void reserve(U p_size);
    Span<T> span() const;  // C++20 span 视图
};
```

**与 Vector 的区别：**
- **无 COW**：直接持有数据指针，修改不需要复制
- **更轻量**：无引用计数开销
- **tight 模式**：严格按需增长（非 2 倍扩容）
- **Godot 渲染系统内部大量使用 LocalVector**（如命令缓冲、顶点数组等）

#### 1.10.3 RID / RID_Owner（资源 ID 系统）

```cpp
class RID {
    uint64_t _id = 0;
public:
    bool is_valid() const { return _id != 0; }
    bool is_null() const { return _id == 0; }
    uint32_t get_local_index() const { return _id & 0xFFFFFFFF; }
    uint64_t get_id() const { return _id; }
    uint32_t hash() const;
};

// RID 到对象的映射管理器
template <typename T, bool thread_safe = false>
class RID_Owner {
    // 提供 RID → T* 的双向映射
    RID make_rid(T *p_object);
    T *get_or_null(const RID &p_rid);
    bool owns(const RID &p_rid);
    void free(const RID &p_rid);
};
```

**设计要点：**
- RID 是 64 位不透明 ID，隐藏了内部指针，保证 API 安全性
- `RID_Owner` 管理 RID 到对象的映射，支持线程安全模式
- `get_local_index()` 提取低 32 位作为数组索引，实现 O(1) 查找
- 渲染系统中所有 GPU 资源（Buffer/Texture/Shader/Pipeline）都通过 RID 引用

---

## 二、ARHud 渲染引擎架构设计

### 2.1 设计目标

| 目标 | 说明 |
|------|------|
| **轻量** | AR HUD 场景以 2D 叠加为主，不需要完整 3D 引擎 |
| **低延迟** | 60fps 稳定输出，单帧 < 8ms |
| **跨 API** | OpenGL (优先) → Vulkan → 未来扩展 |
| **跨平台** | Windows (优先) → Linux → Android → QNX |
| **可裁剪** | 模块化设计，按需编译 |
| **安全** | 适合车规级场景（QNX + OpenGL ES） |

### 2.2 整体架构

```
┌──────────────────────────────────────────────────────────────────┐
│                        ARHud Application Layer                   │
│  (HUD Widget / Navigation / ADAS Overlay / Speed / Warning)      │
├──────────────────────────────────────────────────────────────────┤
│                        ARHud Rendering Server                    │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────────────────┐ │
│  │ Scene Graph  │ │  Material    │ │   HUD Layout Engine      │ │
│  │  (2D Tree)   │ │  System      │ │   (Z-Order / Layer)      │ │
│  └──────────────┘ └──────────────┘ └──────────────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│                        ARHud Renderer Compositor                 │
│  ┌──────────────────────┐ ┌──────────────────────────────────┐  │
│  │  HUD Scene Renderer  │ │  HUD Canvas Renderer (2D)        │  │
│  │  (3D 叠加/透视)       │ │  (文字/图标/线条/矩形)           │  │
│  └──────────────────────┘ └──────────────────────────────────┘  │
├──────────────────────────────────────────────────────────────────┤
│                     ARHud Resource Storage                       │
│  ┌─────────┐ ┌──────────┐ ┌────────┐ ┌─────────┐ ┌──────────┐ │
│  │Texture  │ │ Material │ │  Mesh  │ │  Font   │ │  Shader  │ │
│  │Storage  │ │ Storage  │ │Storage │ │ Storage │ │ Storage  │ │
│  └─────────┘ └──────────┘ └────────┘ └─────────┘ └──────────┘ │
├──────────────────────────────────────────────────────────────────┤
│                     ARHud Rendering Device                       │
│  ┌──────────────────┐ ┌──────────────────┐ ┌─────────────────┐ │
│  │  Resource Mgr    │ │  Staging Buffer  │ │  Command Graph  │ │
│  │  (RID Owner)     │ │  (CPU↔GPU)       │ │  (RDG Lite)     │ │
│  └──────────────────┘ └──────────────────┘ └─────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│                   ARHud Rendering Device Driver                  │
│  ┌──────────────────┐ ┌──────────────────┐ ┌─────────────────┐ │
│  │  GLDriver        │ │  VulkanDriver    │ │  FutureDriver   │ │
│  │  (OpenGL/GLES)   │ │  (Vulkan 1.x)    │ │  (Metal/...)    │ │
│  └──────────────────┘ └──────────────────┘ └─────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│                   ARHud Rendering Context Driver                 │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────────┐  │
│  │ Windows  │ │  Linux   │ │ Android  │ │      QNX         │  │
│  │ (WGL/    │ │ (GLX/    │ │ (EGL/    │ │  (EGL/           │  │
│  │  EGL)    │ │  EGL)    │ │  Vulkan) │ │   OpenGL ES)     │  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────────────┘  │
├──────────────────────────────────────────────────────────────────┤
│                     ARHud Platform Layer                         │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────────┐  │
│  │ Window   │ │  Input   │ │  Thread  │ │  File System     │  │
│  │ Manager  │ │  System  │ │  System  │ │  (Asset Load)    │  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

### 2.3 目录结构设计

```
arhud_engine/
├── CMakeLists.txt                          # 顶层构建
├── core/                                   # arhud_core 静态库（零外部依赖）
│   ├── typedefs.h                          # 基础类型、平台宏、Error 枚举
│   ├── os/                                 # 内存、线程、同步原语
│   │   ├── memory.h                        # 全局内存分配器
│   │   ├── thread.h                        # 线程抽象
│   │   ├── mutex.h                         # 互斥锁（Mutex/MutexLock）
│   │   ├── rw_lock.h                       # 读写锁
│   │   ├── semaphore.h                     # 信号量
│   │   └── spin_lock.h                     # 自旋锁
│   ├── io/                                 # 日志系统
│   │   ├── logger.h                        # Logger 接口
│   │   ├── std_logger.h                    # 控制台日志
│   │   ├── composite_logger.h              # 组合日志
│   │   └── file_logger.h                   # 文件日志（带轮转）
│   ├── template/                           # 容器与模板
│   │   ├── safe_refcount.h                 # SafeNumeric<T> 原子计数
│   │   ├── paged_allocator.h               # 分页分配器
│   │   ├── local_vector.h                  # 动态数组（无 COW）
│   │   ├── hash_map.h                      # 哈希映射
│   │   ├── rid.h                           # RID + RIDOwner
│   │   └── ring_buffer.h                   # 环形缓冲区
│   ├── string/                             # 字符串
│   │   └── ustring.h                       # UTF-32 字符串（LocalVector-based）
│   └── math/                               # 数学库
│       └── math_funcs.h                    # 数学工具函数
│
├── servers/redering/                       # arhud_servers 静态库（注意目录名拼写）
│   ├── rendering_device_commons.h          # 共享类型、DataFormat、强类型 ID
│   ├── rendering_context_driver.h          # IRenderingContextDriver 接口
│   ├── rendering_device_driver.h           # IRenderingDeviceDriver 接口
│   ├── rendering_device.h/.cpp             # RenderingDevice（资源生命周期/Staging Buffer/帧循环）
│   ├── command_buffer.h                    # ICommandBuffer 接口、CommandType 枚举
│   └── resource_tracker.h/.cpp             # ResourceTracker（调试资源冲突检测）
│
├── drivers/gl/                             # arhud_drivers 静态库
│   ├── rendering_context_driver_gl.h/.cpp  # GLRenderingContextDriver
│   ├── rendering_device_driver_gl.h/.cpp   # GLRenderingDeviceDriver（含 GLStateCache）
│   └── command_buffer_gl.h/.cpp            # GLCommandBuffer（录制/回放）
│
├── platform/                               # arhud_platform 静态库
│   ├── gl_manager.h                        # IGLManager 接口（统一上下文模型）
│   ├── display_server.h                    # IDisplayServer 接口
│   ├── screen.h                            # IScreen 接口
│   ├── window.h                            # IWindow 接口
│   ├── view.h                              # IView 接口
│   └── windows/                            # Windows 实现
│       ├── gl_manager_win32.h/.cpp         # Win32GLManager（WGL 实现）
│       ├── display_server_win32.h/.cpp     # Win32DisplayServer
│       ├── screen_win32.h/.cpp             # Win32Screen
│       ├── window_win32.h/.cpp             # Win32Window
│       └── view_win32.h/.cpp               # Win32View
│
├── storage/                                # 资源存储层（未来）
│   ├── texture_storage.h
│   ├── material_storage.h
│   ├── mesh_storage.h
│   ├── font_storage.h
│   ├── shader_storage.h
│   └── gl/                                 # OpenGL 实现
│       └── ...
│
├── renderer/                               # 渲染器（未来）
│   ├── compositor.h                        # 渲染组合器
│   ├── canvas_renderer.h                   # 2D 画布渲染
│   ├── hud_renderer.h                      # HUD 专用渲染器
│   └── gl/                                 # OpenGL 实现
│       └── ...
│
├── shader/                                 # 着色器（未来）
│   └── glsl/                               # GLSL 着色器源码
│       ├── hud_canvas.vert
│       ├── hud_canvas.frag
│       ├── hud_text.vert
│       ├── hud_text.frag
│       ├── hud_icon.vert
│       ├── hud_icon.frag
│       ├── hud_line.vert
│       ├── hud_line.frag
│       └── hud_composite.frag
│
├── scene/                                  # 场景管理（未来）
│   └── ...
│
├── app/                                    # 应用入口（未来）
│   └── ...
│
├── tests/                                  # 测试
│   ├── test_driver_gl.cpp
│   ├── test_canvas_renderer.cpp
│   └── test_hud_renderer.cpp
│
└── cmake/                                  # 构建系统
    ├── option.cmake
    ├── build.cmake
    ├── dir.cmake
    └── lib.cmake
```

### 2.4 核心类设计

#### 2.4.1 资源 ID 系统（参考 Godot RID）

```cpp
// core/template/rid.h
class RID {
    uint64_t id = 0;

public:
    RID() = default;
    explicit RID(uint64_t p_id) : id(p_id) {}
    bool IsValid() const { return id != 0; }
    bool operator==(const RID &p_other) const { return id == p_other.id; }
    bool operator<(const RID &p_other) const { return id < p_other.id; }
    uint64_t GetId() const { return id; }
};

// RID Owner（参考 Godot RID_Owner）
template <typename T, bool ThreadSafe = false>
class RIDOwner {
    // 使用 PagedAllocator + HashMap 管理
    // Allocate() → 返回 RID
    // GetOrNull(RID) → 返回 T*
    // Free(RID) → 释放
};
```

#### 2.4.2 上下文驱动接口

```cpp
// servers/redering/rendering_context_driver.h
class IRenderingContextDriver {
public:
    typedef uint64_t SurfaceID;

    struct DeviceInfo {
        std::string name;
        uint32_t vendor_id;
        enum Type { INTEGRATED_GPU, DISCRETE_GPU, OTHER } type;
    };

    virtual ~IRenderingContextDriver() = default;

    virtual Error Initialize(IScreen::ScreenID p_screen_id,
                             void *p_native_window,
                             uint32_t p_width, uint32_t p_height) = 0;
    virtual uint32_t GetDeviceCount() const = 0;
    virtual const DeviceInfo &GetDevice(uint32_t index) const = 0;
    virtual IRenderingDeviceDriver *CreateDeviceDriver() = 0;
    virtual void DriverFree(IRenderingDeviceDriver *driver) = 0;

    virtual SurfaceID SurfaceCreate(IScreen::ScreenID p_screen_id,
                                    void *p_native_window,
                                    uint32_t p_width, uint32_t p_height) = 0;
    virtual void SurfaceSetSize(SurfaceID, uint32_t w, uint32_t h) = 0;
    virtual void SurfaceSetVsyncMode(SurfaceID, bool enabled) = 0;
    virtual void SurfaceDestroy(SurfaceID) = 0;

    virtual ContextID ContextCreate(SurfaceID p_surface) = 0;
    virtual void ContextDestroy(ContextID p_context_id) = 0;
};
```

#### 2.4.3 设备驱动接口

```cpp
// servers/redering/rendering_device_driver.h
class IRenderingDeviceDriver {
public:
    // 强类型资源 ID（参考 Godot DEFINE_ID 模式）
    #define ARHUD_DEFINE_RDD_ID(m_name) \
        struct m_name##ID { \
            uint64_t id = 0; \
            explicit operator bool() const { return id != 0; } \
            bool operator==(const m_name##ID &o) const { return id == o.id; } \
            bool operator!=(const m_name##ID &o) const { return id != o.id; } \
            m_name##ID() = default; \
            explicit m_name##ID(uint64_t i) : id(i) {} \
            explicit m_name##ID(void *p) : id((uint64_t)p) {} \
        };

    ARHUD_DEFINE_RDD_ID(Buffer)
    ARHUD_DEFINE_RDD_ID(Texture)
    ARHUD_DEFINE_RDD_ID(Sampler)
    ARHUD_DEFINE_RDD_ID(Shader)
    ARHUD_DEFINE_RDD_ID(Pipeline)
    ARHUD_DEFINE_RDD_ID(RenderPass)
    ARHUD_DEFINE_RDD_ID(Framebuffer)
    ARHUD_DEFINE_RDD_ID(UniformSet)
    ARHUD_DEFINE_RDD_ID(CommandBuffer)
    ARHUD_DEFINE_RDD_ID(SwapChain)
    ARHUD_DEFINE_RDD_ID(Fence)

    // 数据格式（对齐 Vulkan 枚举值，参考 Godot rendering_device_commons.h）
    enum DataFormat {
        DATA_FORMAT_R8_UNORM = 9,
        DATA_FORMAT_R8G8_UNORM = 16,
        DATA_FORMAT_R8G8B8A8_UNORM = 37,
        DATA_FORMAT_B8G8R8A8_UNORM = 44,
        DATA_FORMAT_R16_SFLOAT = 76,
        DATA_FORMAT_R16G16_SFLOAT = 83,
        DATA_FORMAT_R16G16B16A16_SFLOAT = 97,
        DATA_FORMAT_R32_SFLOAT = 100,
        DATA_FORMAT_R32G32_SFLOAT = 103,
        DATA_FORMAT_R32G32B32_SFLOAT = 106,
        DATA_FORMAT_R32G32B32A32_SFLOAT = 109,
        DATA_FORMAT_D16_UNORM = 124,
        DATA_FORMAT_D24_UNORM_S8_UINT = 129,
        DATA_FORMAT_D32_SFLOAT = 126,
        DATA_FORMAT_S8_UINT = 127,
    };

    // 缓冲区用途
    enum BufferUsageBits {
        BUFFER_USAGE_TRANSFER_FROM = (1 << 0),
        BUFFER_USAGE_TRANSFER_TO = (1 << 1),
        BUFFER_USAGE_UNIFORM = (1 << 4),
        BUFFER_USAGE_STORAGE = (1 << 5),
        BUFFER_USAGE_INDEX = (1 << 6),
        BUFFER_USAGE_VERTEX = (1 << 7),
    };

    // 纹理用途
    enum TextureUsageBits {
        TEXTURE_USAGE_SAMPLING_BIT = (1 << 0),
        TEXTURE_USAGE_COLOR_ATTACHMENT_BIT = (1 << 1),
        TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT = (1 << 2),
        TEXTURE_USAGE_STORAGE_BIT = (1 << 3),
        TEXTURE_USAGE_CAN_COPY_FROM_BIT = (1 << 4),
        TEXTURE_USAGE_CAN_COPY_TO_BIT = (1 << 5),
        TEXTURE_USAGE_CPU_READ_BIT = (1 << 6),
    };

    // 纹理类型
    enum TextureType {
        TEXTURE_TYPE_2D,
        TEXTURE_TYPE_2D_ARRAY,
        TEXTURE_TYPE_3D,
        TEXTURE_TYPE_CUBE,
    };

    // 采样器过滤
    enum SamplerFilter {
        SAMPLER_FILTER_NEAREST,
        SAMPLER_FILTER_LINEAR,
    };

    // 采样器寻址模式
    enum SamplerRepeatMode {
        SAMPLER_REPEAT_MODE_REPEAT,
        SAMPLER_REPEAT_MODE_MIRRORED_REPEAT,
        SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE,
        SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER,
    };

    // 核心接口
    virtual Error Initialize(uint32_t device_index, uint32_t frame_count) = 0;

    // --- 缓冲区 ---
    virtual BufferID BufferCreate(uint64_t size, uint32_t usage, bool cpu_accessible) = 0;
    virtual void BufferFree(BufferID) = 0;
    virtual uint8_t *BufferMap(BufferID) = 0;
    virtual void BufferUnmap(BufferID) = 0;
    virtual void BufferUpdate(BufferID, uint32_t offset, uint32_t size, const void *data) = 0;

    // --- 纹理 ---
    struct TextureFormat {
        DataFormat format = DATA_FORMAT_R8G8B8A8_UNORM;
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t depth = 1;
        uint32_t array_layers = 1;
        uint32_t mipmaps = 1;
        TextureType type = TEXTURE_TYPE_2D;
        uint32_t usage = TEXTURE_USAGE_SAMPLING_BIT;
        uint32_t samples = 1;
    };

    virtual TextureID TextureCreate(const TextureFormat &format) = 0;
    virtual void TextureFree(TextureID) = 0;
    virtual void TextureUpdate(TextureID, uint32_t layer, const void *data, uint32_t data_size) = 0;

    // --- 采样器 ---
    struct SamplerState {
        SamplerFilter mag_filter = SAMPLER_FILTER_LINEAR;
        SamplerFilter min_filter = SAMPLER_FILTER_LINEAR;
        SamplerFilter mip_filter = SAMPLER_FILTER_LINEAR;
        SamplerRepeatMode repeat_u = SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
        SamplerRepeatMode repeat_v = SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
        SamplerRepeatMode repeat_w = SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
        float max_lod = 0.0f;
    };

    virtual SamplerID SamplerCreate(const SamplerState &state) = 0;
    virtual void SamplerFree(SamplerID) = 0;

    // --- 着色器 ---
    struct ShaderStage {
        enum Type { VERTEX, FRAGMENT, COMPUTE } type;
        std::string source;          // GLSL 源码
        std::string entry_point;     // "main"
    };

    virtual ShaderID ShaderCreateFromGLSL(const ShaderStage *stages, uint32_t stage_count) = 0;
    virtual void ShaderFree(ShaderID) = 0;

    // --- Uniform Set ---
    enum UniformType {
        UNIFORM_TYPE_SAMPLER,
        UNIFORM_TYPE_SAMPLER_WITH_TEXTURE,
        UNIFORM_TYPE_UNIFORM_BUFFER,
        UNIFORM_TYPE_STORAGE_BUFFER,
        UNIFORM_TYPE_TEXTURE,
    };

    struct BoundUniform {
        UniformType type;
        uint32_t binding;
        LocalVector<ID> ids;
    };

    virtual UniformSetID UniformSetCreate(const BoundUniform *uniforms,
                                             uint32_t uniform_count,
                                             ShaderID shader,
                                             uint32_t set_index) = 0;
    virtual void UniformSetFree(UniformSetID) = 0;

    // --- 渲染管线 ---
    struct RenderPipelineCreateInfo {
        ShaderID shader;
        DataFormat color_format;
        DataFormat depth_format;
        // Vertex input
        // Blend state
        // Rasterization state
        // Depth stencil state
        // Multisample state
    };

    virtual PipelineID RenderPipelineCreate(const RenderPipelineCreateInfo &info) = 0;
    virtual void PipelineFree(PipelineID) = 0;

    // --- 帧缓冲 ---
    virtual FramebufferID FramebufferCreate(RenderPassID render_pass,
                                              const TextureID *attachments,
                                              uint32_t attachment_count,
                                              uint32_t width, uint32_t height) = 0;
    virtual void FramebufferFree(FramebufferID) = 0;

    // --- 交换链 ---
    // SwapChain = Context + Surface 绑定记录
    // OpenGL: MakeCurrent + SwapBuffers wrapper
    // Vulkan: VkSwapchainKHR (future)
    virtual SwapChainID SwapChainCreate(SurfaceID p_surface, ContextID p_context) { return SwapChainID(); }
    virtual void SwapChainDestroy(SwapChainID p_swapchain) { (void)p_swapchain; }
    virtual Error SwapChainAcquire(SwapChainID p_swapchain) { (void)p_swapchain; return Error::kOK; }
    virtual void SwapChainPresent(SwapChainID p_swapchain) { (void)p_swapchain; }

    // --- 命令录制 ---
    virtual CommandBufferID CommandBufferCreate() = 0;
    virtual void CommandBufferBegin(CommandBufferID) = 0;
    virtual void CommandBufferEnd(CommandBufferID) = 0;

    virtual void CommandBindRenderPipeline(CommandBufferID, PipelineID) = 0;
    virtual void CommandBindUniformSet(CommandBufferID, UniformSetID, uint32_t set_index) = 0;
    virtual void CommandBindVertexBuffers(CommandBufferID, const BufferID *buffers, uint32_t count) = 0;
    virtual void CommandBindIndexBuffer(CommandBufferID, BufferID, DataFormat index_format) = 0;
    virtual void CommandDraw(CommandBufferID, uint32_t vertex_count, uint32_t instance_count) = 0;
    virtual void CommandDrawIndexed(CommandBufferID, uint32_t index_count, uint32_t instance_count) = 0;
    virtual void CommandSetViewport(CommandBufferID, int32_t x, int32_t y, uint32_t w, uint32_t h) = 0;
    virtual void CommandSetScissor(CommandBufferID, int32_t x, int32_t y, uint32_t w, uint32_t h) = 0;

    // --- 同步 ---
    virtual FenceID FenceCreate() = 0;
    virtual Error FenceWait(FenceID) = 0;
    virtual void FenceFree(FenceID) = 0;

    // --- 帧提交 ---
    virtual void Submit(CommandBufferID cmd_buffer, SwapChainID swap_chain) = 0;
    virtual void Present(SwapChainID swap_chain) = 0;
};
```

#### 2.4.4 OpenGL 驱动实现要点

```cpp
// drivers/gl/rendering_device_driver_gl.h
class GLRenderingDeviceDriver : public IRenderingDeviceDriver {
    GLuint vao = 0;  // 全局 VAO（OpenGL 3.3+ 必需）

    // 资源簿记（参考 Godot 的 PagedAllocator + VersatileResourceTemplate）
    template <typename... Types>
    using VersatileResource = ...;

    // Buffer → GLuint
    // Texture → GLuint
    // Shader → GLuint program
    // Pipeline → 自定义结构体（OpenGL 没有原生 PSO）
    // Framebuffer → GLuint FBO
    // RenderPass → 自定义结构体（记录 attachment 描述）
    // UniformSet → 自定义结构体（记录 binding 信息）

    Error Initialize(uint32_t device_index, uint32_t frame_count) override;

    // OpenGL 特殊处理：
    // 1. Pipeline 不是原生概念，需要记录 blend/depth/raster 状态
    //    在 bind_pipeline 时逐个设置 GL 状态
    // 2. Uniform Set 在 bind 时逐个 glUniform / glBindTexture
    // 3. RenderPass 在 begin 时设置 glClear / loadOp / storeOp
    // 4. SwapChain 对应默认 FBO (FBO 0)
    // 5. CommandBuffer 采用录制/回放模式（GLCommandBuffer）
    //    Begin() 开始录制，End() 结束录制，Execute() 回放执行
    //    客户端通过 DrawList API 间接使用，不直接操作 ICommandBuffer
};
```

**OpenGL 驱动的关键映射：**

| RDD 概念 | OpenGL 映射 |
|----------|-------------|
| BufferID | GLuint (VBO/IBO/UBO) |
| TextureID | GLuint (纹理对象) |
| SamplerID | GLuint (采样器对象, GL 3.3+) |
| ShaderID | GLuint (Program Object) |
| PipelineID | 自定义结构体（blend/depth/raster 状态） |
| RenderPassID | 自定义结构体（attachment 描述） |
| FramebufferID | GLuint (FBO) |
| UniformSetID | 自定义结构体（binding 映射表） |
| CommandBufferID | GLCommandBuffer（录制/回放，非立即模式） |
| SwapChainID | 默认 FBO (FBO 0) |
| FenceID | GLsync (glFenceSync) |

#### 2.4.5 OpenGL 上下文驱动实现

```cpp
// drivers/gl/rendering_context_driver_gl.h
class GLRenderingContextDriver : public IRenderingContextDriver {
    // Windows: 使用 WGL
    // Linux: 使用 GLX/EGL
    // Android: 使用 EGL
    // QNX: 使用 EGL

    // 参考 Godot 的 GLManagerNative_Windows
    // - 创建 OpenGL 上下文（3.3 Core / 4.5 Core / GLES 3.2）
    // - 管理多个窗口的 GL 上下文共享
    // - VSync 控制（wglSwapIntervalEXT / glXSwapIntervalEXT）
};
```

**Windows 平台 WGL 初始化流程（参考 [gl_manager_windows_native.cpp](file:///d:/Work/godot-4.6/platform/windows/gl_manager_windows_native.cpp)）：**

```
1. 注册窗口类 → CreateWindowEx
2. 获取临时 DC → 选择临时像素格式 → 创建临时 GL 上下文
3. wglCreateContextAttribsARB 创建正式 GL 3.3+ Core 上下文
4. 删除临时上下文
5. 加载 GL 函数指针（GLAD）
6. 设置 VSync（wglSwapIntervalEXT）
```

### 2.5 HUD 专用渲染管线

AR HUD 的渲染管线与通用引擎不同，需要专门优化：

```
┌──────────────────────────────────────────────────────────────┐
│                    ARHud Frame Pipeline                      │
│                                                              │
│  1. BeginFrame                                               │
│     ├── 更新 HUD 数据（车速/导航/ADAS）                       │
│     ├── 更新 Uniform Buffer（Transform/Color/Params）         │
│     └── 更新动态纹理（导航地图/摄像头画面）                    │
│                                                              │
│  2. Clear (Color + Depth)                                    │
│                                                              │
│  3. Render HUD Layers (Z-Order, back to front)               │
│     ├── Layer 0: Background (地图/摄像头叠加)                 │
│     │   └── Full-screen quad + texture                       │
│     ├── Layer 1: ADAS Overlay (车道线/障碍物)                 │
│     │   └── Lines + Polygons + Alpha blend                   │
│     ├── Layer 2: Navigation (箭头/路径)                      │
│     │   └── Lines + Triangles + Alpha blend                  │
│     ├── Layer 3: Dashboard (速度/转速/油量)                   │
│     │   └── Text + Arc + Icon                                │
│     └── Layer 4: Warning (碰撞预警/限速)                     │
│         └── Text + Icon + Flash animation                    │
│                                                              │
│  4. Post-Process                                             │
│     ├── Alpha pre-multiply correction                        │
│     ├── Gamma correction (sRGB → Linear → sRGB)              │
│     └── Distortion correction (AR 投影畸变校正)               │
│                                                              │
│  5. Present (SwapBuffers)                                    │
└──────────────────────────────────────────────────────────────┘
```

### 2.6 HUD 专用着色器设计

#### Canvas 顶点着色器（2D 元素通用）

```glsl
// hud_canvas.vert
#version 330 core

layout(location = 0) in vec2 a_position;    // 顶点位置 (NDC or Pixel)
layout(location = 1) in vec2 a_uv;          // 纹理坐标
layout(location = 2) in vec4 a_color;       // 顶点颜色
layout(location = 3) in float a_flags;      // 标志位

uniform mat4 u_projection;   // 正交投影矩阵
uniform mat4 u_model;        // 模型矩阵

out vec2 v_uv;
out vec4 v_color;

void main() {
    vec4 world_pos = u_model * vec4(a_position, 0.0, 1.0);
    gl_Position = u_projection * world_pos;
    v_uv = a_uv;
    v_color = a_color;
}
```

#### Canvas 片段着色器

```glsl
// hud_canvas.frag
#version 330 core

in vec2 v_uv;
in vec4 v_color;

uniform sampler2D u_texture;
uniform float u_opacity;

out vec4 frag_color;

void main() {
    vec4 tex_color = texture(u_texture, v_uv);
    frag_color = tex_color * v_color * u_opacity;
}
```

#### 文字渲染着色器（SDF）

```glsl
// hud_text.frag
#version 330 core

in vec2 v_uv;
in vec4 v_color;

uniform sampler2D u_font_sdf;   // SDF 纹理
uniform float u_smoothing;      // 1.0 / spread / viewport_size

out vec4 frag_color;

void main() {
    float dist = texture(u_font_sdf, v_uv).r;
    float alpha = smoothstep(0.5 - u_smoothing, 0.5 + u_smoothing, dist);
    frag_color = vec4(v_color.rgb, v_color.a * alpha);
}
```

#### 线条渲染着色器（抗锯齿）

```glsl
// hud_line.vert
#version 330 core

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_normal;      // 法线方向（用于展开线宽）
layout(location = 2) in vec4 a_color;

uniform mat4 u_projection;
uniform float u_line_width;

out vec4 v_color;
out float v_edge_distance;

void main() {
    vec2 expanded = a_position + a_normal * u_line_width * 0.5;
    gl_Position = u_projection * vec4(expanded, 0.0, 1.0);
    v_color = a_color;
    v_edge_distance = a_normal.x; // 用于抗锯齿
}
```

#### 畸变校正着色器（AR 投影）

```glsl
// hud_distortion.frag
#version 330 core

in vec2 v_uv;

uniform sampler2D u_scene;
uniform vec2 u_lens_center;
uniform float u_k1;
uniform float u_k2;
uniform float u_scale;

out vec4 frag_color;

void main() {
    vec2 uv = v_uv * 2.0 - 1.0;
    vec2 centered = (uv - u_lens_center) * u_scale;
    float r2 = dot(centered, centered);
    vec2 distorted = centered * (1.0 + u_k1 * r2 + u_k2 * r2 * r2);
    vec2 final_uv = (distorted + u_lens_center) * 0.5 + 0.5;

    if (final_uv.x < 0.0 || final_uv.x > 1.0 ||
        final_uv.y < 0.0 || final_uv.y > 1.0) {
        frag_color = vec4(0.0);
    } else {
        frag_color = texture(u_scene, final_uv);
    }
}
```

### 2.7 HUD 场景节点设计

```cpp
// hud_element.h — HUD 元素基类
class ARHudElement : public ARHudNode2D {
    float opacity = 1.0f;
    int z_order = 0;
    bool visible = true;
    RID material;

    virtual void render(ARHudCanvasRenderer *renderer) = 0;
};

// text_element.h — 文字元素
class ARHudTextElement : public ARHudElement {
    std::string text;
    RID font;           // Font RID
    float font_size = 24.0f;
    Color color;
    enum Alignment { LEFT, CENTER, RIGHT } alignment;

    void render(ARHudCanvasRenderer *renderer) override;
};

// icon_element.h — 图标元素
class ARHudIconElement : public ARHudElement {
    RID texture;        // Texture RID
    Rect2 source_rect;       // 图集区域
    Vec2 size;               // 显示大小
    Color tint;

    void render(ARHudCanvasRenderer *renderer) override;
};

// line_element.h — 线条元素
class ARHudLineElement : public ARHudElement {
    LocalVector<Vec2> points;
    float width = 2.0f;
    Color color;
    bool antialiased = true;

    void render(ARHudCanvasRenderer *renderer) override;
};

// rect_element.h — 矩形元素
class ARHudRectElement : public ARHudElement {
    Rect2 rect;
    Color fill_color;
    Color border_color;
    float border_width = 0.0f;
    float corner_radius = 0.0f;

    void render(ARHudCanvasRenderer *renderer) override;
};
```

### 2.8 渲染组合器实现

```cpp
// compositor.h
class ARHudCompositor {
    RenderingDevice *device = nullptr;

    // Storage 子系统
    ARHudTextureStorage *texture_storage = nullptr;
    ARHudMaterialStorage *material_storage = nullptr;
    ARHudMeshStorage *mesh_storage = nullptr;
    ARHudFontStorage *font_storage = nullptr;
    ARHudShaderStorage *shader_storage = nullptr;

    // 渲染器
    ARHudCanvasRenderer *canvas_renderer = nullptr;
    ARHudHUDRenderer *hud_renderer = nullptr;

    // 帧状态
    uint64_t frame_number = 0;
    double delta_time = 0.0;

public:
    // Initialize 接收 RCD，RD 内部通过 RCD::CreateDeviceDriver() 创建 RDD
    // RDD 所有权归 RD，Finalize() 时通过 RCD::DriverFree() 释放
    // RCD 所有权归外部（Engine/调用者），生命周期由外部管理
    void Initialize(IRenderingContextDriver *context_driver);
    void BeginFrame(double frame_step);
    void Render(ARHudSceneTree *scene);
    void EndFrame(bool present);
    void Finalize();

    ARHudTextureStorage *GetTextureStorage() { return texture_storage; }
    ARHudMaterialStorage *GetMaterialStorage() { return material_storage; }
    ARHudMeshStorage *GetMeshStorage() { return mesh_storage; }
    ARHudFontStorage *GetFontStorage() { return font_storage; }
};
```

### 2.9 引擎主类

```cpp
// engine.h
class ARHudEngine {
    static ARHudEngine *singleton;

    IRenderingContextDriver *context_driver = nullptr;
    RenderingDevice *rendering_device = nullptr;
    ARHudCompositor *compositor = nullptr;
    ARHudSceneTree *scene_tree = nullptr;

    // 平台
    IWindow *main_window = nullptr;

    // 配置
    struct Config {
        uint32_t window_width = 1920;
        uint32_t window_height = 720;
        bool vsync = true;
        enum class GraphicsAPI { OPENGL, VULKAN } graphics_api = GraphicsAPI::OPENGL;
        enum class Platform { WINDOWS, LINUX, ANDROID, QNX } platform = Platform::WINDOWS;
    } config;

public:
    static ARHudEngine *GetSingleton() { return singleton; }

    Error Initialize(const Config &config);
    void Run();
    void Finalize();

    ARHudCompositor *GetCompositor() { return compositor; }
    ARHudSceneTree *GetSceneTree() { return scene_tree; }
    RenderingDevice *GetRenderingDevice() { return rendering_device; }

    // ─── 初始化顺序（对齐 Godot RenderingDevice::initialize） ───
    // 1. DisplayServer::Initialize()
    // 2. DisplayServer::WindowCreate() → 获取 native_window
    // 3. IGLManager::Initialize()
    // 4. RCD::Initialize(screen_id, native_window, width, height)
    //    → 内部创建引导 Surface/Context + 加载 GL 函数
    // 5. RD::Initialize(RCD) → 内部创建 RDD (RCD::CreateDeviceDriver)
    // 6. RD::MakeCurrent() → 绑定 SwapChain
    //
    // ─── 销毁顺序 ───
    // 1. RD::Finalize() → 释放 RID 资源 + RDD (RCD::DriverFree)
    // 2. RCD::Shutdown() → 销毁所有 Surface/Context
    // 3. IGLManager::Shutdown()
    // 4. DisplayServer::WindowDestroy()
    // 5. DisplayServer::Finalize()
};
```

### 2.10 内存管理系统设计

ARHud 的内存管理参考 Godot 的分层设计，但针对嵌入式/车规场景做了简化与强化。

#### 2.10.1 设计原则

| 原则 | 说明 |
|------|------|
| **全局统一分配** | 所有堆分配经过统一接口，支持内存统计与泄漏检测 |
| **渲染路径零分配** | 热路径（帧循环）不调用 malloc/new，使用预分配池 |
| **对齐保证** | GPU 缓冲区/SIMD 数据保证 16/256 字节对齐 |
| **内存预算** | 为不同子系统设定内存预算上限，防止 OOM |
| **车规安全** | QNX 平台禁用动态分配，所有内存池在启动时预分配 |

#### 2.10.2 全局内存分配器

```cpp
// arhud_memory.h
namespace ARHudMemory {
    // 基础分配
    void *alloc(size_t p_size);
    void *alloc_zeroed(size_t p_size);
    void *realloc(void *p_ptr, size_t p_size);
    void free(void *p_ptr);

    // 对齐分配（GPU/SIMD 场景）
    void *alloc_aligned(size_t p_size, size_t p_alignment);
    void free_aligned(void *p_ptr);

    // 内存统计
    struct MemoryStats {
        size_t current_usage;
        size_t peak_usage;
        size_t allocation_count;
    };
    MemoryStats get_stats();
    void reset_peak();

    // 内存预算
    void set_budget(size_t p_total_bytes);
    bool is_budget_exceeded();
};

// 对象构造/析构宏（参考 Godot memnew/memdelete）
#define ARHUD_NEW(m_class) ::new (ARHudMemory::alloc(sizeof(m_class))) m_class
#define ARHUD_DELETE(m_ptr) \
    do { \
        if (m_ptr) { \
            (m_ptr)->~decltype(*m_ptr)(); \
            ARHudMemory::free(m_ptr); \
        } \
    } while(0)

#define ARHUD_NEW_ARR(m_class, m_count) arhud_new_arr<m_class>(m_count)
#define ARHUD_DELETE_ARR(m_ptr, m_count) arhud_delete_arr(m_ptr, m_count)

// C 风格
#define ARHUD_MALLOC(m_size) ARHudMemory::alloc(m_size)
#define ARHUD_FREE(m_ptr) ARHudMemory::free(m_ptr)
```

**与 Godot 的差异：**
- 去掉了 `p_pad_align` 参数，统一使用对齐分配
- 增加了内存预算机制，适合嵌入式场景
- 简化了宏命名，避免与 Godot 宏冲突
- `ARHUD_DELETE` 使用 `decltype` 自动推导类型

#### 2.10.3 PagedAllocator（分页分配器）

```cpp
// core/template/paged_allocator.h
template <typename T, bool thread_safe = false, uint32_t PAGE_SIZE = 256>
class PagedAllocator {
    T **page_pool = nullptr;
    T ***available_pool = nullptr;
    uint32_t pages_allocated = 0;
    uint32_t allocs_available = 0;
    uint32_t page_shift = 0;
    uint32_t page_mask = 0;

    // 线程安全锁（可选）
    typename std::conditional<thread_safe,
        SpinLock,
        NoOpLock>::type lock;

public:
    void configure(uint32_t p_page_size) {
        page_size = nearest_pow2(p_page_size);
        page_mask = page_size - 1;
        page_shift = get_shift_from_pow2(page_size);
    }

    template <typename... Args>
    T *alloc(Args &&...args) {
        if constexpr (thread_safe) lock.lock();
        if (allocs_available == 0) {
            grow();  // 分配新页
        }
        allocs_available--;
        T *ptr = available_pool[allocs_available >> page_shift]
                                 [allocs_available & page_mask];
        if constexpr (thread_safe) lock.unlock();
        return new (ptr) T(std::forward<Args>(args)...);
    }

    void free(T *p_ptr) {
        if constexpr (thread_safe) lock.lock();
        p_ptr->~T();
        available_pool[allocs_available >> page_shift]
                      [allocs_available & page_mask] = p_ptr;
        allocs_available++;
        if constexpr (thread_safe) lock.unlock();
    }

    // 车规模式：启动时预分配所有页
    void preallocate(uint32_t p_max_count);
};
```

**ARHud 特有增强：**
- `preallocate()` 方法：车规模式下在启动时预分配所有内存，运行时不再调用 malloc
- 默认 `PAGE_SIZE=256`（比 Godot 的 4096 小，HUD 场景对象数量有限）
- 使用 `std::conditional` 选择锁类型，避免模板特化

#### 2.10.4 渲染路径内存策略

```
┌─────────────────────────────────────────────────────────────┐
│                    ARHud 帧内存策略                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  启动阶段（允许分配）：                                      │
│  ├── GPU 资源池（Texture/Buffer/Shader 预创建）              │
│  ├── 命令池（PagedAllocator<DrawCmd> 预分配）               │
│  ├── 顶点缓冲池（动态 VBO 预分配）                          │
│  └── 字形缓存池（SDF Atlas 预分配）                         │
│                                                             │
│  帧循环（零分配）：                                          │
│  ├── 命令录制 → 从命令池 alloc（无 malloc）                 │
│  ├── 顶点写入 → 环形缓冲区（无 malloc）                     │
│  ├── Uniform 更新 → 预分配 UBO（无 malloc）                 │
│  └── 纹理更新 → PBO 环形缓冲（无 malloc）                   │
│                                                             │
│  帧结束（回收）：                                            │
│  ├── 命令池 reset（归还所有命令到可用池）                    │
│  ├── 环形缓冲区前进指针                                     │
│  └── 丢弃上一帧临时数据                                     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

#### 2.10.5 内存预算分配

```cpp
// memory_budget.h
struct MemoryBudget {
    // 总预算（默认 256MB，车规场景可配置）
    size_t total_budget = 256 * 1024 * 1024;

    // 子系统预算
    struct {
        size_t textures = 128 * 1024 * 1024;    // 纹理（导航地图/图标/字体图集）
        size_t buffers = 32 * 1024 * 1024;      // GPU 缓冲区（VBO/UBO/SSBO）
        size_t shaders = 8 * 1024 * 1024;       // 着色器代码
        size_t fonts = 16 * 1024 * 1024;        // 字形缓存
        size_t scene = 16 * 1024 * 1024;        // 场景数据
        size_t staging = 32 * 1024 * 1024;      // Staging Buffer
        size_t system = 24 * 1024 * 1024;       // 系统开销
    } allocation;

    bool validate() const;  // 检查子预算之和不超过总预算
};
```

### 2.11 OS 抽象层设计

ARHud 参考 Godot 的 OS 抽象层，但针对 HUD 场景做了精简，去掉了不需要的功能（如脚本错误、编辑器通知等）。

#### 2.11.1 整体结构

```
┌──────────────────────────────────────────────────────────┐
│                    ARHud OS 抽象层                        │
├──────────────────────────────────────────────────────────┤
│  OS                — 操作系统抽象（单例）                  │
│  Thread            — 线程抽象                              │
│  Mutex             — 互斥锁（递归/非递归）                 │
│  BinaryMutex       — 非递归互斥锁                         │
│  Semaphore         — 信号量                                │
│  RWLock            — 读写锁                                │
│  SpinLock          — 自旋锁                                │
│  SafeNumeric<T>    — 原子操作                              │
│  Time              — 时间管理                              │
├──────────────────────────────────────────────────────────┤
│  FileAccess       — 文件访问                              │
│  DirAccess        — 目录访问                              │
│  Logger           — 日志系统                              │
├──────────────────────────────────────────────────────────┤
│  平台实现：                                               │
│  ├── OSWindows          — Windows 实现                    │
│  ├── OSLinux            — Linux 实现                      │
│  ├── OSAndroid          — Android 实现                    │
│  └── OSQNX              — QNX 实现                        │
└──────────────────────────────────────────────────────────┘
```

#### 2.11.2 Thread（线程抽象）

```cpp
// core/os/thread.h
class Thread {
public:
    using Callback = void (*)(void *p_userdata);
    using ID = uint64_t;

    enum class Priority { LOW, NORMAL, HIGH, REALTIME };

    struct Settings {
        Priority priority = Priority::NORMAL;
        size_t stack_size = 0;  // 0 = 平台默认
    };

    ID start(Callback p_callback, void *p_userdata, const Settings &p_settings = {});
    bool is_started() const;
    void wait_to_finish();
    void detach();

    static ID get_caller_id();
    static ID get_main_id();
    static bool is_main_thread();
    static void yield();
    static void sleep_usec(uint32_t p_usec);
    static uint32_t get_processor_count();

private:
    struct Impl;  // 平台特定实现（PIMPL）
    std::unique_ptr<Impl> impl;
    static thread_local ID caller_id;
    static ID main_id;
};
```

**与 Godot 的差异：**
- 增加 `detach()` 支持（Godot 不支持分离线程）
- 增加 `stack_size` 配置（QNX 实时线程需要自定义栈大小）
- 增加 `REALTIME` 优先级（车规场景需要）
- 使用 PIMPL 模式隐藏平台实现细节
- 去掉了 `PlatformFunctions` 注入机制，改为编译时选择实现

#### 2.11.3 Mutex / BinaryMutex

```cpp
// core/os/mutex.h
class Mutex {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    void lock();
    void unlock();
    bool try_lock();
};

class BinaryMutex {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    void lock();
    void unlock();
    bool try_lock();
};

// RAII 守卫
class MutexLock {
    Mutex &mutex;
public:
    MutexLock(Mutex &m) : mutex(m) { mutex.lock(); }
    ~MutexLock() { mutex.unlock(); }
};

class BinaryMutexLock {
    BinaryMutex &mutex;
public:
    BinaryMutexLock(BinaryMutex &m) : mutex(m) { mutex.lock(); }
    ~BinaryMutexLock() { mutex.unlock(); }
};
```

**PIMPL 的好处：**
- 头文件不需要 include `<mutex>`，减少编译依赖
- 平台特定实现（如 QNX 的 `pthread_mutex_t`）完全隐藏
- ABI 稳定，更换实现不需要重编译

#### 2.11.4 Semaphore

```cpp
// core/os/semaphore.h
class Semaphore {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    void post(uint32_t p_count = 1);
    void wait();
    bool try_wait();
};
```

#### 2.11.5 RWLock

```cpp
// core/os/rw_lock.h
class RWLock {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    void read_lock();
    void read_unlock();
    void write_lock();
    void write_unlock();
};

class ReadGuard {
    RWLock &lock;
public:
    ReadGuard(RWLock &l) : lock(l) { lock.read_lock(); }
    ~ReadGuard() { lock.read_unlock(); }
};

class WriteGuard {
    RWLock &lock;
public:
    WriteGuard(RWLock &l) : lock(l) { lock.write_lock(); }
    ~WriteGuard() { lock.write_unlock(); }
};
```

#### 2.11.6 SpinLock

```cpp
// core/os/spin_lock.h
class SpinLock {
    alignas(128) mutable std::atomic_flag flag = ATOMIC_FLAG_INIT;
public:
    void lock() const {
        while (flag.test_and_set(std::memory_order_acquire)) {
            cpu_pause();
        }
    }
    void unlock() const {
        flag.clear(std::memory_order_release);
    }

private:
    static void cpu_pause() {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
#elif defined(_M_ARM) || defined(__aarch64__)
        __yield();
#else
        // fallback: std::this_thread::yield()
#endif
    }
};
```

#### 2.11.7 SafeNumeric（原子操作）

```cpp
// core/template/safe_refcount.h
template <typename T>
class SafeNumeric {
    static_assert(std::atomic<T>::is_always_lock_free);
    std::atomic<T> value;

public:
    void Set(T p_val) { value.store(p_val, std::memory_order_release); }
    T Get() const { return value.load(std::memory_order_acquire); }
    T Increment() { return value.fetch_add(1, std::memory_order_acq_rel) + 1; }
    T Decrement() { return value.fetch_sub(1, std::memory_order_acq_rel) - 1; }
    bool CompareExchange(T &p_expected, T p_desired) {
        return value.compare_exchange_weak(p_expected, p_desired,
                                           std::memory_order_acq_rel);
    }
};

using SafeNumericInt32 = SafeNumeric<int32_t>;
using SafeNumericUint32 = SafeNumeric<uint32_t>;
using SafeNumericBool = SafeNumeric<bool>;
```

#### 2.11.8 Time（时间管理）

```cpp
// core/os/time.h
class Time {
public:
    static uint64_t get_ticks_usec();    // 微秒精度计时
    static uint64_t get_ticks_msec();    // 毫秒精度计时
    static double get_ticks_sec();       // 秒精度（浮点）
    static void sleep_usec(uint32_t p_usec);
    static void sleep_msec(uint32_t p_msec);

    // 平台实现
    #if defined(_WIN32)
        static uint64_t get_ticks_usec_windows();
    #elif defined(__linux__) || defined(__QNX__)
        static uint64_t get_ticks_usec_posix();
    #elif defined(__ANDROID__)
        static uint64_t get_ticks_usec_android();
    #endif
};
```

**平台实现：**
- Windows: `QueryPerformanceCounter` / `QueryPerformanceFrequency`
- Linux/QNX: `clock_gettime(CLOCK_MONOTONIC, ...)`
- Android: 同 Linux 实现

### 2.12 平台抽象层设计

#### 2.12.1 IDisplayServer（操作系统抽象）

```cpp
// platform/display_server.h
class IDisplayServer {
    static IDisplayServer *singleton;

public:
    static IDisplayServer *GetSingleton() { return singleton; }

    // 生命周期
    virtual Error Initialize() = 0;
    virtual void Finalize() = 0;

    // 系统信息
    virtual const char *GetName() const = 0;
    virtual uint32_t GetProcessorCount() const = 0;
    virtual size_t GetSystemMemory() const = 0;

    // 环境变量
    virtual bool HasEnv(const char *p_var) const = 0;
    virtual const char *GetEnv(const char *p_var) const = 0;
    virtual void SetEnv(const char *p_var, const char *p_value) const = 0;

    // 动态库
    virtual void *LoadLibrary(const char *p_path) = 0;
    virtual void UnloadLibrary(void *p_handle) = 0;
    virtual void *GetSymbol(void *p_handle, const char *p_name) = 0;

    // 路径
    virtual const char *GetResourcePath() const = 0;
    virtual const char *GetUserDataPath() const = 0;
    virtual const char *GetCachePath() const = 0;
    virtual const char *GetTempPath() const = 0;

    // 时间
    virtual uint64_t GetTicksUsec() const = 0;
    virtual void DelayUsec(uint32_t p_usec) const = 0;

    // 渲染线程模式
    enum class RenderThreadMode {
        SINGLE_THREAD,       // 单线程（逻辑+渲染同线程）
        SEPARATE_THREAD,     // 独立渲染线程
    };
    virtual RenderThreadMode GetRenderThreadMode() const = 0;

    // 日志
    virtual void Print(const char *p_format, ...) = 0;
    virtual void PrintError(const char *p_function, const char *p_file,
                             int p_line, const char *p_msg) = 0;
    virtual void PrintWarning(const char *p_function, const char *p_file,
                               int p_line, const char *p_msg) = 0;
};
```

**平台实现：**

```cpp
// platform/windows/display_server_win32.h
class Win32DisplayServer : public IDisplayServer {
    HINSTANCE h_instance = nullptr;
public:
    Error Initialize() override;
    void Finalize() override;
    const char *GetName() const override { return "Windows"; }
    uint32_t GetProcessorCount() const override;
    void *LoadLibrary(const char *p_path) override { return (void*)LoadLibraryA(p_path); }
    void UnloadLibrary(void *p_handle) override { FreeLibrary((HMODULE)p_handle); }
    void *GetSymbol(void *p_handle, const char *p_name) override {
        return (void*)GetProcAddress((HMODULE)p_handle, p_name);
    }
    uint64_t GetTicksUsec() const override;
    // ...
};

// platform/qnx/display_server_qnx.h
class QNXDisplayServer : public IDisplayServer {
public:
    const char *GetName() const override { return "QNX"; }
    // QNX 特有：Screen API 初始化、实时线程配置
    Error ScreenInit();
    Error SetupRealtimeThread(Thread::Priority p_priority);
    // ...
};
```

#### 2.12.2 FileAccess（文件访问）

```cpp
// core/io/file_access.h
class FileAccess {
public:
    enum class Mode { READ, WRITE, READ_WRITE };

    static FileAccess *Open(const char *p_path, Mode p_mode);
    static void Close(FileAccess *p_fa);

    virtual size_t read(void *p_buffer, size_t p_bytes) = 0;
    virtual size_t write(const void *p_buffer, size_t p_bytes) = 0;
    virtual bool seek(size_t p_position) = 0;
    virtual size_t get_position() const = 0;
    virtual size_t get_length() const = 0;
    virtual bool is_open() const = 0;
    virtual void close() = 0;

    // 便捷方法
    virtual uint8_t read_8();
    virtual uint16_t read_16();
    virtual uint32_t read_32();
    virtual uint64_t read_64();
    virtual float read_float();
    virtual double read_double();

    // 工具
    static bool file_exists(const char *p_path);
    static size_t get_file_size(const char *p_path);
    static bool read_all(const char *p_path, void **r_data, size_t *r_size);

protected:
    bool big_endian = false;
};
```

**平台实现：**
- Windows: 使用 `CreateFile` / `ReadFile` / `WriteFile`
- Linux/QNX: 使用 POSIX `open` / `read` / `write`
- Android: 使用 AAssetManager 读取 APK 内资源 + POSIX 读写外部文件

#### 2.12.3 DirAccess（目录访问）

```cpp
// core/io/dir_access.h
class DirAccess {
    static DirAccess *Create();
    static void Destroy(DirAccess *p_da);

    virtual bool list_dir_begin() = 0;
    virtual const char *get_next() = 0;
    virtual bool current_is_dir() const = 0;
    virtual bool current_is_hidden() const = 0;
    virtual void list_dir_end() = 0;

    virtual bool change_dir(const char *p_dir) = 0;
    virtual bool make_dir(const char *p_dir) = 0;
    virtual bool file_exists(const char *p_file) = 0;
    virtual bool dir_exists(const char *p_dir) = 0;
    virtual bool remove(const char *p_name) = 0;
    virtual bool rename(const char *p_from, const char *p_to) = 0;
};
```

#### 2.12.4 Logger（日志系统）

```cpp
// core/io/logger.h
class Logger {
public:
    enum class Level { TRACE, DEBUG, INFO, WARN, ERROR, FATAL };

    virtual void log(Level p_level, const char *p_file, int p_line,
                     const char *p_format, ...) = 0;
    virtual void flush() = 0;
    virtual void set_level(Level p_level) = 0;
};

// 控制台日志
class StdLogger : public Logger { ... };

// 文件日志（带轮转）
class FileLogger : public Logger {
    FileAccess *file = nullptr;
    size_t max_file_size = 10 * 1024 * 1024;  // 10MB
    int max_backup_count = 5;
    void RotateIfNeeded();
};

// 组合日志
class CompositeLogger : public Logger {
    LocalVector<Logger *> loggers;
};

// 日志宏
#define ARHUD_LOG_TRACE(...) Logger::GetSingleton()->Log(Logger::Level::TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define ARHUD_LOG_DEBUG(...) Logger::GetSingleton()->Log(Logger::Level::DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define ARHUD_LOG_INFO(...)  Logger::GetSingleton()->Log(Logger::Level::INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define ARHUD_LOG_WARN(...)  Logger::GetSingleton()->Log(Logger::Level::WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define ARHUD_LOG_ERROR(...) Logger::GetSingleton()->Log(Logger::Level::ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define ARHUD_LOG_FATAL(...) Logger::GetSingleton()->Log(Logger::Level::FATAL, __FILE__, __LINE__, __VA_ARGS__)
```

**与 Godot 的差异：**
- 增加了 `TRACE` 和 `FATAL` 级别
- 增加了 `set_level()` 过滤（Release 构建可关闭 TRACE/DEBUG）
- 去掉了 `ERR_SCRIPT` 和 `ERR_SHADER` 类型（HUD 引擎不需要脚本引擎）
- 文件日志带大小轮转（Godot 的 RotatedFileLogger 按启动次数轮转）

### 2.13 容器与模板设计

ARHud 参考 Godot 的容器设计，但去掉 COW 语义，全部使用无共享的本地容器，更适合渲染引擎的零分配需求。

#### 2.13.1 LocalVector（动态数组）

```cpp
// core/template/local_vector.h
template <typename T>
class LocalVector {
    T *data = nullptr;
    uint32_t count = 0;
    uint32_t capacity = 0;

public:
    LocalVector() = default;
    ~LocalVector() { Clear(); }

    void PushBack(const T &p_val);
    void PushBack(T &&p_val);
    void PopBack();
    void RemoveAt(uint32_t p_index);
    void RemoveAtUnordered(uint32_t p_index);  // O(1) 不保序删除

    T *Ptr() { return data; }
    const T *Ptr() const { return data; }
    uint32_t Size() const { return count; }
    bool IsEmpty() const { return count == 0; }

    T &operator[](uint32_t p_index) { return data[p_index]; }
    const T &operator[](uint32_t p_index) const { return data[p_index]; }

    void Reserve(uint32_t p_capacity);
    void Resize(uint32_t p_size);
    void ResizeUninitialized(uint32_t p_size);  // 不调用构造函数
    void Clear();
    void ShrinkToFit();

    // 查找
    int32_t Find(const T &p_val) const;
    bool Has(const T &p_val) const { return Find(p_val) >= 0; }

    // 排序
    void Sort();
};
```

**与 Godot Vector 的区别：**
- **无 COW**：直接持有数据，无引用计数开销
- **无共享**：拷贝构造执行深拷贝
- **resize_uninitialized**：用于 POD 类型的快速扩容
- **remove_at_unordered**：O(1) 删除，适合不需要保序的场景

#### 2.13.2 HashMap（哈希映射）

```cpp
// core/template/hash_map.h
template <typename K, typename V, typename Hasher = DefaultHasher<K>>
class HashMap {
    struct Slot {
        K key;
        V value;
        uint32_t hash;
        bool occupied = false;
    };

    Slot *slots = nullptr;
    uint32_t capacity = 0;
    uint32_t count = 0;
    uint32_t deleted_count = 0;

public:
    V *get_or_null(const K &p_key);
    const V *get_or_null(const K &p_key) const;
    void set(const K &p_key, const V &p_value);
    bool has(const K &p_key) const;
    bool erase(const K &p_key);
    void clear();
    void reserve(uint32_t p_capacity);

    uint32_t size() const { return count; }
    bool is_empty() const { return count == 0; }

    // 迭代器
    struct Iterator { ... };
    Iterator begin();
    Iterator end();
};
```

#### 2.13.3 RID / RIDOwner（资源 ID 系统）

```cpp
// core/template/rid.h
class RID {
    uint64_t id = 0;
public:
    RID() = default;
    explicit RID(uint64_t p_id) : id(p_id) {}

    bool IsValid() const { return id != 0; }
    bool IsNull() const { return id == 0; }
    uint64_t GetId() const { return id; }
    uint32_t GetIndex() const { return static_cast<uint32_t>(id & 0xFFFFFFFF); }
    uint32_t Hash() const { return static_cast<uint32_t>(id ^ (id >> 32)); }

    bool operator==(const RID &p_other) const { return id == p_other.id; }
    bool operator!=(const RID &p_other) const { return id != p_other.id; }
    bool operator<(const RID &p_other) const { return id < p_other.id; }
};

template <typename T, bool thread_safe = false>
class RIDOwner {
    LocalVector<T *> objects;
    LocalVector<uint32_t> free_list;
    uint32_t count = 0;

    typename std::conditional<thread_safe, SpinLock, NoOpLock>::type lock;

public:
    RID MakeRid(T *p_object) {
        if constexpr (thread_safe) lock.lock();
        uint32_t index;
        if (!free_list.is_empty()) {
            index = free_list.back();
            free_list.pop_back();
            objects[index] = p_object;
        } else {
            index = objects.size();
            objects.push_back(p_object);
        }
        count++;
        if constexpr (thread_safe) lock.unlock();
        return RID((static_cast<uint64_t>(count) << 32) | index);
    }

    T *GetOrNull(const RID &p_rid) const {
        if (!p_rid.IsValid()) return nullptr;
        uint32_t index = p_rid.GetIndex();
        if (index >= objects.size()) return nullptr;
        return objects[index];
    }

    void Free(const RID &p_rid) {
        if constexpr (thread_safe) lock.lock();
        uint32_t index = p_rid.GetIndex();
        objects[index] = nullptr;
        free_list.push_back(index);
        if constexpr (thread_safe) lock.unlock();
    }

    bool Owns(const RID &p_rid) const;
};
```

**与 Godot RID 的差异：**
- 高 32 位存储版本号（防止 ABA 问题），低 32 位存储索引
- 使用 `free_list` 回收槽位，避免频繁 realloc
- `thread_safe` 模式使用 SpinLock（而非 Godot 的全局锁）

#### 2.13.4 FixedVector（固定容量向量）

```cpp
// core/template/fixed_vector.h
// 用于渲染路径中避免堆分配的固定容量向量
template <typename T, uint32_t N>
class FixedVector {
    alignas(T) uint8_t storage[sizeof(T) * N];
    uint32_t count = 0;

public:
    void PushBack(const T &p_val) {
        if (count < N) {
            new (reinterpret_cast<T*>(storage) + count) T(p_val);
            count++;
        }
    }

    T *Data() { return reinterpret_cast<T*>(storage); }
    uint32_t Size() const { return count; }
    T &operator[](uint32_t i) { return Data()[i]; }
};
```

**使用场景：** 每帧的绘制命令列表（HUD 元素数量有限，通常 < 256）。

#### 2.13.5 RingBuffer（环形缓冲区）

```cpp
// core/template/ring_buffer.h
template <typename T>
class RingBuffer {
    LocalVector<T> buffer;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t capacity = 0;

public:
    void reserve(uint32_t p_capacity) {
        capacity = p_capacity;
        buffer.resize(p_capacity);
    }

    bool push(const T &p_val) {
        uint32_t next = (head + 1) % capacity;
        if (next == tail) return false;  // 满
        buffer[head] = p_val;
        head = next;
        return true;
    }

    bool pop(T &r_val) {
        if (tail == head) return false;  // 空
        r_val = buffer[tail];
        tail = (tail + 1) % capacity;
        return true;
    }

    uint32_t size() const;
    bool is_empty() const { return head == tail; }
    bool is_full() const { return ((head + 1) % capacity) == tail; }
};
```

**使用场景：**
- 主线程→渲染线程的命令队列
- PBO 双缓冲/三缓冲
- Staging Buffer 的帧循环管理

### 2.14 基础类型与编译器抽象

```cpp
// arhud_defs.h
#include <cstdint>
#include <cstddef>

// 基础类型别名
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using float32 = float;
using float64 = double;

// 错误码
enum class Error {
    OK = 0,
    FAILED,
    INVALID_PARAMETER,
    OUT_OF_MEMORY,
    FILE_NOT_FOUND,
    FILE_CORRUPT,
    DRIVER_ERROR,
    UNSUPPORTED,
};

// 编译器宏
#if defined(__GNUC__) || defined(__clang__)
    #define ARHUD_ALWAYS_INLINE __attribute__((always_inline)) inline
    #define ARHUD_NO_INLINE __attribute__((noinline))
    #define ARHUD_LIKELY(x) __builtin_expect(!!(x), 1)
    #define ARHUD_UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER)
    #define ARHUD_ALWAYS_INLINE __forceinline
    #define ARHUD_NO_INLINE __declspec(noinline)
    #define ARHUD_LIKELY(x) (x)
    #define ARHUD_UNLIKELY(x) (x)
#endif

// 平台检测
#if defined(_WIN32)
    #define ARHUD_PLATFORM_WINDOWS 1
#elif defined(__ANDROID__)
    #define ARHUD_PLATFORM_ANDROID 1
#elif defined(__QNX__)
    #define ARHUD_PLATFORM_QNX 1
#elif defined(__linux__)
    #define ARHUD_PLATFORM_LINUX 1
#endif

// API 检测
#define ARHUD_API_OPENGL 1
// #define ARHUD_API_VULKAN 1

// Debug 宏
#ifdef NDEBUG
    #define ARHUD_DEBUG 0
#else
    #define ARHUD_DEBUG 1
#endif

#define ARHUD_ASSERT(cond) \
    do { if (ARHUD_DEBUG && !(cond)) { ARHUD_LOG_FATAL("Assertion failed: %s", #cond); abort(); } } while(0)

#define ARHUD_ASSERT_MSG(cond, msg) \
    do { if (ARHUD_DEBUG && !(cond)) { ARHUD_LOG_FATAL("Assertion failed: %s - %s", #cond, msg); abort(); } } while(0)

// 禁用拷贝
#define ARHUD_DISABLE_COPY(classname) \
    classname(const classname &) = delete; \
    classname &operator=(const classname &) = delete;

// 禁用拷贝和移动
#define ARHUD_DISABLE_MOVE(classname) \
    classname(classname &&) = delete; \
    classname &operator=(classname &&) = delete;
```

### 2.15 多屏幕/多窗口/多视图架构

AR HUD 场景通常涉及多块物理屏幕（仪表盘、HUD 投影、中控屏），每块屏幕可能需要独立帧率和渲染线程；同一屏幕上可能有多个窗口（如导航窗口、告警窗口），每个窗口需要独立的窗口线程和事件处理；单个窗口内可能包含多个视图（左/右眼立体视图、2D 叠加层/3D 场景层）。

#### 2.15.1 Godot 多窗口/多屏幕架构参考

Godot 通过以下层次实现多窗口支持：

| 层次 | 类 | 职责 |
|------|------|------|
| 窗口管理 | `DisplayServer` | 枚举屏幕 (`screen_get_count/screen_get_size`)、创建/销毁窗口 (`window_create/window_destroy`)、VSync 控制 |
| 渲染表面 | `RenderingContextDriver` | 维护 `HashMap<WindowID, SurfaceID>` 映射，每个窗口对应一个渲染表面 (Surface) |
| 交换链 | `RenderingDevice` | 维护 `HashMap<WindowID, SwapChainID>`，每个窗口/屏幕拥有独立 SwapChain |
| 独立设备 | `RenderingDevice::CreateIndependentDevice()` | 创建独立渲染栈（独立 RDD + 独立上下文），用于多线程渲染 |
| 视口 | `RendererViewport` | 每个视口拥有独立 RenderTarget，可渲染到屏幕或纹理，支持 XR 多视图 |

关键代码参考：
```cpp
// RenderingContextDriver: 窗口 -> Surface 映射
class RenderingContextDriver {
    HashMap<DisplayServer::WindowID, SurfaceID> window_surface_map;
    SurfaceID surface_get_from_window(WindowID p_window) const;
    Error window_create(WindowID p_window, const void *p_platform_data);
    void window_destroy(WindowID p_window);
};

// RenderingDevice: 屏幕 -> SwapChain 映射
class RenderingDevice {
    HashMap<WindowID, SwapChainID> screen_swap_chains;
    HashMap<WindowID, FramebufferID> screen_framebuffers;
    Error screen_create(WindowID p_screen);
    Error screen_free(WindowID p_screen);
    RenderingDevice *CreateIndependentDevice(IScreen::ScreenID p_screen_id,
                                              void *native_window,
                                              uint32_t width, uint32_t height);
};
```

#### 2.15.2 ARHud 多屏幕/多窗口/多视图架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                     IDisplayServer (平台抽象)                    │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │
│  │  Screen 0    │  │  Screen 1    │  │  Screen 2    │  ...         │
│  │  (仪表盘)    │  │  (HUD投影)   │  │  (中控屏)    │              │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘              │
│         │                 │                 │                       │
│  ┌──────▼───────┐  ┌──────▼───────┐  ┌──────▼───────┐              │
│  │ RenderThread0│  │ RenderThread1│  │ RenderThread2│  (每屏一线程) │
│  │  ┌────────┐  │  │  ┌────────┐  │  │  ┌────────┐  │              │
│  │  │Window 0│  │  │  │Window 0│  │  │  │Window 0│  │              │
│  │  │┌──────┐│  │  │  │┌──────┐│  │  │  │┌──────┐│  │              │
│  │  ││View 0││  │  │  ││View 0││  │  │  ││View 0││  │              │
│  │  ││View 1││  │  │  ││View 1││  │  │  ││View 1││  │              │
│  │  │└──────┘│  │  │  │└──────┘│  │  │  │└──────┘│  │              │
│  │  └────────┘  │  │  └────────┘  │  │  └────────┘  │              │
│  │  ┌────────┐  │  │              │  │  ┌────────┐  │              │
│  │  │Window 1│  │  │              │  │  │Window 1│  │              │
│  │  └────────┘  │  │              │  │  └────────┘  │              │
│  └──────────────┘  └──────────────┘  └──────────────┘              │
└─────────────────────────────────────────────────────────────────────┘
```

#### 2.15.3 核心类设计

##### IScreen — 屏幕抽象

```cpp
class IScreen {
public:
    using ScreenID = uint32_t;

    struct Info {
        ScreenID id;
        int32_t x, y;
        int32_t width, height;
        float dpi;
        float refresh_rate;
        float scale;
        bool is_primary;
    };

    virtual ~IScreen() = default;
    virtual const Info &get_info() const = 0;
    virtual int32_t get_width() const = 0;
    virtual int32_t get_height() const = 0;
    virtual float get_refresh_rate() const = 0;
    virtual float get_dpi() const = 0;
    virtual bool is_primary() const = 0;
};
```

##### IWindow — 窗口抽象

```cpp
class IWindow {
    ARHUD_NON_COPYABLE(IWindow)

public:
    using WindowID = uint32_t;
    static constexpr WindowID INVALID_WINDOW_ID = UINT32_MAX;

    enum class Mode {
        WINDOWED,
        FULLSCREEN,
        EXCLUSIVE_FULLSCREEN,
        BORDERLESS
    };

    struct CreateParams {
        IScreen::ScreenID screen_id;
        int32_t x = 0, y = 0;
        int32_t width = 1920, height = 720;
        Mode mode = Mode::WINDOWED;
        bool resizable = true;
        bool visible = true;
        const char *title = "ARHud";
        void *native_handle = nullptr;
    };

    struct Callbacks {
        void (*on_resize)(WindowID, int32_t w, int32_t h, void *) = nullptr;
        void (*on_close)(WindowID, void *) = nullptr;
        void (*on_focus)(WindowID, bool focused, void *) = nullptr;
        void (*on_key)(WindowID, int key, int scancode, bool pressed, void *) = nullptr;
        void *userdata = nullptr;
    };

    virtual ~IWindow() = default;

    virtual WindowID get_id() const = 0;
    virtual IScreen::ScreenID get_screen_id() const = 0;
    virtual int32_t get_width() const = 0;
    virtual int32_t get_height() const = 0;
    virtual void set_size(int32_t w, int32_t h) = 0;
    virtual void *get_native_handle() const = 0;
    virtual bool is_visible() const = 0;
    virtual void set_visible(bool p_visible) = 0;

    virtual void set_callbacks(const Callbacks &p_callbacks) = 0;
};
```

##### IView — 视图抽象

```cpp
class IView {
    ARHUD_NON_COPYABLE(IView)

public:
    using ViewID = uint32_t;

    enum class Type {
        SCENE_3D,
        OVERLAY_2D,
        STEREO_LEFT,
        STEREO_RIGHT,
        CUSTOM
    };

    struct Config {
        ViewID id;
        Type type = Type::SCENE_3D;
        int32_t x = 0, y = 0;
        int32_t width = 0, height = 0;
        float z_order = 0.0f;
        bool clear_color = true;
        bool clear_depth = true;
        Color clear_value = {0, 0, 0, 0};
    };

    virtual ~IView() = default;

    virtual ViewID get_id() const = 0;
    virtual Type get_type() const = 0;
    virtual int32_t get_width() const = 0;
    virtual int32_t get_height() const = 0;
    virtual void set_viewport(int32_t x, int32_t y, int32_t w, int32_t h) = 0;
    virtual float get_z_order() const = 0;
    virtual void set_z_order(float p_order) = 0;

    virtual RID get_render_target() const = 0;
};
```

##### IDisplayServer — 显示服务器（核心管理器）

```cpp
class IDisplayServer {
    ARHUD_NON_COPYABLE(IDisplayServer)

public:
    using ScreenID = IScreen::ScreenID;
    using WindowID = IWindow::WindowID;
    using ViewID = IView::ViewID;

    struct ScreenRenderThread {
        ScreenID screen_id;
        Thread *thread = nullptr;
        Mutex *frame_mutex = nullptr;
        Semaphore *frame_semaphore = nullptr;
        SafeNumeric<bool> running{false};
        SafeNumeric<bool> frame_ready{false};
        SafeNumeric<int> frame_count{0};
    };

    struct WindowContext {
        WindowID window_id;
        ScreenID screen_id;
        IWindow *window = nullptr;
        Thread *window_thread = nullptr;
        Mutex *event_mutex = nullptr;
        SafeNumeric<bool> running{false};
        LocalVector<IView *> views;
        void *render_surface = nullptr;
    };

    static IDisplayServer *GetSingleton();

    virtual ~IDisplayServer() = default;

    // ─── 屏幕管理 ───
    virtual uint32_t get_screen_count() const = 0;
    virtual IScreen *get_screen(ScreenID p_id) const = 0;
    virtual ScreenID get_primary_screen() const = 0;

    // ─── 窗口管理 ───
    virtual WindowID window_create(const IWindow::CreateParams &p_params) = 0;
    virtual void window_destroy(WindowID p_id) = 0;
    virtual IWindow *get_window(WindowID p_id) const = 0;
    virtual uint32_t get_window_count() const = 0;
    virtual LocalVector<WindowID> get_window_list() const = 0;
    virtual LocalVector<WindowID> get_windows_on_screen(ScreenID p_screen) const = 0;

    // ─── 视图管理 ───
    virtual ViewID view_create(WindowID p_window, const IView::Config &p_config) = 0;
    virtual void view_destroy(ViewID p_id) = 0;
    virtual IView *get_view(ViewID p_id) const = 0;
    virtual LocalVector<IView *> get_views_of_window(WindowID p_window) const = 0;

    // ─── 渲染线程管理 ───
    virtual bool screen_render_thread_start(ScreenID p_screen) = 0;
    virtual void screen_render_thread_stop(ScreenID p_screen) = 0;
    virtual bool is_screen_render_thread_running(ScreenID p_screen) const = 0;
    virtual Thread *get_screen_render_thread(ScreenID p_screen) const = 0;

    // ─── 窗口线程管理 ───
    virtual bool window_thread_start(WindowID p_window) = 0;
    virtual void window_thread_stop(WindowID p_window) = 0;
    virtual bool is_window_thread_running(WindowID p_window) const = 0;

    // ─── 事件处理 ───
    virtual void process_events() = 0;
    virtual void swap_buffers(WindowID p_window) = 0;

protected:
    static IDisplayServer *singleton;

    LocalVector<IScreen *> screens;
    HashMap<WindowID, WindowContext> windows;
    HashMap<ScreenID, ScreenRenderThread> screen_render_threads;
    HashMap<ViewID, IView *> views;

    WindowID next_window_id = 1;
    ViewID next_view_id = 1;
};
```

#### 2.15.4 渲染线程模型

AR HUD 采用 **每屏幕一渲染线程 + 每窗口一事件线程** 的双线程模型：

```
┌──────────────────────────────────────────────────────────────────┐
│                        主线程 (Main Thread)                       │
│  - ARHudEngine 初始化/销毁                                        │
│  - 场景更新 (逻辑 Tick)                                           │
│  - 资源加载/卸载调度                                               │
│  - 向各渲染线程提交渲染命令                                        │
└──────────┬──────────────────┬──────────────────┬─────────────────┘
           │                  │                  │
     ┌─────▼─────┐     ┌─────▼─────┐     ┌─────▼─────┐
     │Screen 0   │     │Screen 1   │     │Screen 2   │
     │RenderThread│    │RenderThread│    │RenderThread│
     │            │     │            │     │            │
     │ ┌────────┐ │     │ ┌────────┐ │     │ ┌────────┐ │
     │ │Window 0│ │     │ │Window 0│ │     │ │Window 0│ │
     │ │ Render │ │     │ │ Render │ │     │ │ Render │ │
     │ └────────┘ │     │ └────────┘ │     │ └────────┘ │
     │ ┌────────┐ │     │            │     │ ┌────────┐ │
     │ │Window 1│ │     │            │     │ │Window 1│ │
     │ │ Render │ │     │            │     │ │ Render │ │
     │ └────────┘ │     │            │     │ └────────┘ │
     └────────────┘     └────────────┘     └────────────┘

     ┌────────────┐     ┌────────────┐     ┌────────────┐
     │Window 0    │     │Window 0    │     │Window 0    │
     │EventThread │     │EventThread │     │EventThread │
     │(输入/resize│     │(输入/resize│     │(输入/resize│
     │ /平台事件) │     │ /平台事件) │     │ /平台事件) │
     └────────────┘     └────────────┘     └────────────┘
```

**渲染线程职责：**
- 执行 GPU 渲染命令（绑定 FBO、Draw Call、SwapBuffers）
- 管理该屏幕上所有窗口的渲染顺序
- 独立帧率控制（与屏幕刷新率匹配）
- 帧同步：等待主线程的场景数据就绪信号

**窗口线程职责：**
- 处理平台窗口事件（键盘、鼠标、触摸、resize）
- 管理窗口生命周期
- 将事件转发给主线程或渲染线程

**线程间通信：**

```cpp
struct FrameSync {
    Semaphore *main_to_render;   // 主线程 -> 渲染线程：场景数据就绪
    Semaphore *render_to_main;   // 渲染线程 -> 主线程：渲染完成
    SafeNumeric<bool> scene_ready{false};
    SafeNumeric<bool> render_done{false};
    SafeNumeric<bool> quit{false};
};

// 主线程：提交渲染
void main_thread_tick() {
    update_scene();
    for (auto &screen_rt : screen_render_threads) {
        screen_rt.frame_sync.scene_ready.store(true);
        screen_rt.frame_sync.main_to_render->post();
    }
    // 等待所有渲染线程完成
    for (auto &screen_rt : screen_render_threads) {
        screen_rt.frame_sync.render_to_main->wait();
        screen_rt.frame_sync.render_done.store(false);
    }
}

// 渲染线程：执行渲染
void render_thread_func(ScreenID screen_id) {
    auto &sync = screen_render_threads[screen_id].frame_sync;
    while (!sync.quit.load()) {
        sync.main_to_render->wait();
        if (sync.quit.load()) break;
        if (sync.scene_ready.load()) {
            render_all_windows_on_screen(screen_id);
            swap_buffers_for_screen(screen_id);
            sync.scene_ready.store(false);
            sync.render_done.store(true);
            sync.render_to_main->post();
        }
    }
}
```

#### 2.15.5 多视图合成

单个窗口内可能包含多个视图（如 3D 场景层 + 2D 叠加层），视图按 `z_order` 排序合成：

```cpp
class ViewCompositor {
public:
    struct CompositeLayer {
        IView *view;
        RID render_target;
        float z_order;
        bool alpha_blend;
        BlendMode blend_mode;
    };

    void AddLayer(const CompositeLayer &p_layer);
    void RemoveLayer(IView::ViewID p_view_id);
    void SortLayers();

    void Composite(IRenderingDeviceDriver *p_driver,
                   RID p_target_framebuffer,
                   const Rect &p_viewport);

private:
    LocalVector<CompositeLayer> layers;
    bool layers_sorted = false;
};
```

**合成流程：**

```
1. 清除窗口帧缓冲 (color + depth)
2. 按 z_order 升序遍历所有 Layer:
   a. 绑定 Layer 的 render_target 纹理
   b. 设置混合模式 (Alpha Blend / Additive / None)
   c. 绘制全屏四边形 (textured quad)
3. 可选：后处理 (色调映射、锐化)
4. 呈现 (SwapBuffers / Present)
```

**AR 立体视图特殊处理：**

```cpp
struct StereoConfig {
    enum class StereoMode {
        NONE,
        SIDE_BY_SIDE,
        TOP_BOTTOM,
        QUAD_BUFFER,
        MULTI_VIEW
    };

    StereoMode mode = StereoMode::NONE;
    float ipd = 0.064f;           // 瞳距 (米)
    float convergence = 10.0f;    // 汇聚距离 (米)
    Mat4 left_eye_projection;
    Mat4 right_eye_projection;
    Mat4 left_eye_view;
    Mat4 right_eye_view;
};

// 立体渲染流程
void RenderStereoView(IView *p_view, const StereoConfig &p_stereo) {
    switch (p_stereo.mode) {
    case StereoConfig::StereoMode::SIDE_BY_SIDE:
        RenderEye(p_view, p_stereo.left_eye_projection, p_stereo.left_eye_view,
                   Rect(0, 0, width / 2, height));
        RenderEye(p_view, p_stereo.right_eye_projection, p_stereo.right_eye_view,
                   Rect(width / 2, 0, width / 2, height));
        break;
    case StereoConfig::StereoMode::QUAD_BUFFER:
        // GL_STEREO + GL_BACK_LEFT / GL_BACK_RIGHT
        glDrawBuffer(GL_BACK_LEFT);
        RenderEye(p_view, p_stereo.left_eye_projection, p_stereo.left_eye_view,
                   Rect(0, 0, width, height));
        glDrawBuffer(GL_BACK_RIGHT);
        RenderEye(p_view, p_stereo.right_eye_projection, p_stereo.right_eye_view,
                   Rect(0, 0, width, height));
        glDrawBuffer(GL_BACK);
        break;
    case StereoConfig::StereoMode::MULTI_VIEW:
        // GL_OVR_multiview / VK_multiview — 一次绘制多视图
        RenderMultiview(p_view, p_stereo, 2);
        break;
    default:
        break;
    }
}
```

#### 2.15.6 OpenGL 多窗口实现要点

OpenGL 的多窗口/多上下文实现需要注意以下关键点：

```cpp
// ARHud 采用统一上下文模型，所有 Context 地位平等，无主从区分
class IGLManager {
public:
    using ContextID = uint32_t;
    using SurfaceID = uint32_t;
    static constexpr ContextID kInvalidContextId = UINT32_MAX;
    static constexpr SurfaceID kInvalidSurfaceId = UINT32_MAX;

    virtual Error Initialize() = 0;
    virtual void Shutdown() = 0;

    // 统一上下文模型 - 所有 Context 地位平等，无主从区分
    virtual ContextID CreateContext(SurfaceID p_surface) = 0;
    virtual void DestroyContext(ContextID p_context_id) = 0;
    virtual SurfaceID CreateSurface(IScreen::ScreenID p_screen_id, void *p_native_window,
                                    uint32_t p_width, uint32_t p_height) = 0;
    virtual void DestroySurface(SurfaceID p_surface_id) = 0;

    virtual Error MakeCurrent(ContextID p_context_id, SurfaceID p_surface_id) = 0;
    virtual void ReleaseCurrent() = 0;
    virtual void SwapBuffers(SurfaceID p_surface_id) = 0;
    virtual void SetVsyncMode(SurfaceID p_surface_id, VSyncMode p_mode) = 0;
};

// ─── 单线程多 Surface 模式 ──────────────────────────────────────────
// 1 Context + N Surface，MakeCurrent 切换渲染目标
void single_thread_render_loop() {
    for (auto &surface_id : surfaces) {
        rd->MakeCurrent(surface_id);
        DrawListID dl = rd->DrawListBegin();
        rd->DrawListSetViewport(dl, 0, 0, width, height);
        // ... 绘制命令 ...
        rd->DrawListEnd();
        rd->SwapBuffers(surface_id);
    }
}

// ─── 多线程独立上下文模式 ──────────────────────────────────────────
// N Context + N Surface，每线程独立渲染栈
void multi_thread_render_thread(RenderingDevice *indep_rd,
                                 IGLManager::ContextID ctx_id,
                                 IRenderingContextDriver::SurfaceID surf_id) {
    indep_rd->GetGLManager()->MakeCurrent(ctx_id, surf_id);
    while (!quit) {
        DrawListID dl = indep_rd->DrawListBegin();
        indep_rd->DrawListSetViewport(dl, 0, 0, width, height);
        // ... 绘制命令 ...
        indep_rd->DrawListEnd();
        indep_rd->GetGLManager()->SwapBuffers(surf_id);
    }
    indep_rd->GetGLManager()->ReleaseCurrent();
}
```

**ARHud 多上下文资源归属规则（无共享上下文）：**

| 资源类型 | 跨独立上下文共享 | 备注 |
|----------|:---:|------|
| 纹理 (Texture) | ❌ | 各上下文独立创建，不使用 wglShareLists |
| 缓冲区 (Buffer) | ❌ | 各上下文独立创建 |
| 着色器程序 (Program) | ❌ | 各上下文独立编译链接 |
| 着色器对象 (Shader) | ❌ | 不可共享，需在各上下文编译 |
| FBO | ❌ | 各上下文独立创建 |
| VAO | ❌ | 不可共享 (GL 3.3+)，各上下文独立创建 |
| 同步对象 (Fence) | ✅ | `glFenceSync` 可跨上下文等待 |

**设计决策：为什么不使用共享上下文（wglShareLists）？**
1. wglShareLists 有严格的时序要求和平台差异，容易引发隐蔽 bug
2. 共享上下文下，资源创建/销毁需要跨线程同步，增加复杂度
3. AR HUD 场景资源量有限（HUD 纹理/着色器），重复创建开销可接受
4. Vulkan 后端天然线程安全，无需共享上下文机制
5. 独立上下文模型更简单、更安全，与 Vulkan 模型一致

#### 2.15.7 Vulkan 多屏幕实现要点

Vulkan 天然支持多窗口/多线程，其设计比 OpenGL 更自然：

```cpp
class VulkanSurfaceManager {
public:
    struct SurfaceContext {
        WindowID window_id;
        VkSurfaceKHR surface;
        VkSwapchainKHR swapchain;
        VkFormat surface_format;
        VkColorSpaceKHR color_space;
        VkPresentModeKHR present_mode;
        LocalVector<VkImage> swapchain_images;
        LocalVector<VkImageView> swapchain_image_views;
        LocalVector<VkFramebuffer> framebuffers;
        uint32_t current_image_index = 0;
        VkExtent2D extent;
    };

    // 为窗口创建 Vulkan Surface
    SurfaceContext CreateSurface(WindowID p_window,
                                  VkInstance p_instance,
                                  VkPhysicalDevice p_phys_device,
                                  VkDevice p_device);

    void DestroySurface(SurfaceContext &p_ctx, VkInstance p_instance, VkDevice p_device);

    // 重建 Swapchain（窗口 resize 时）
    void RecreateSwapchain(SurfaceContext &p_ctx,
                            VkInstance p_instance,
                            VkPhysicalDevice p_phys_device,
                            VkDevice p_device);

private:
    HashMap<WindowID, SurfaceContext> surfaces;
};

// Vulkan 渲染线程（无需上下文切换，天然多线程）
void vk_render_thread_func(ScreenID screen_id) {
    auto &screen_rt = display_server->get_screen_render_thread(screen_id);
    auto windows = display_server->get_windows_on_screen(screen_id);

    // Vulkan: 每个线程可独立录制命令缓冲，无需上下文绑定
    VkCommandPool cmd_pool = create_command_pool(vk_device, queue_family_index);

    while (!screen_rt.frame_sync.quit.load()) {
        screen_rt.frame_sync.main_to_render->wait();
        if (screen_rt.frame_sync.quit.load()) break;

        for (WindowID wid : windows) {
            auto &surf_ctx = surface_mgr->get_surface(wid);

            // 获取下一个 Swapchain 图像
            vkAcquireNextImageKHR(vk_device, surf_ctx.swapchain,
                                  UINT64_MAX, image_available_semaphore,
                                  VK_NULL_HANDLE, &surf_ctx.current_image_index);

            // 录制命令
            VkCommandBuffer cmd = begin_single_time_commands(cmd_pool);
            render_window_to_command_buffer(cmd, wid);
            end_single_time_commands(cmd);

            // 提交并呈现
            VkSubmitInfo submit = { ... };
            vkQueueSubmit(graphics_queue, 1, &submit, render_finished_semaphore);

            VkPresentInfoKHR present = { ... };
            vkQueuePresentKHR(present_queue, &present);
        }

        screen_rt.frame_sync.render_to_main->post();
    }

    vkDestroyCommandPool(vk_device, cmd_pool, nullptr);
}
```

#### 2.15.8 目录结构更新

```
platform/               ← arhud_platform 静态库
├── gl_manager.h                  # IGLManager 接口（统一上下文模型）
├── display_server.h              # IDisplayServer 接口
├── screen.h                      # IScreen 接口
├── window.h                      # IWindow 接口
├── view.h                        # IView 接口
├── view_compositor.h             # 视图合成器
├── stereo_config.h               # 立体渲染配置
│
└── windows/                      # Windows 实现
    ├── gl_manager_win32.h/.cpp           # Win32GLManager（WGL 实现）
    ├── display_server_win32.h/.cpp       # Win32DisplayServer
    ├── screen_win32.h/.cpp               # Win32Screen
    ├── window_win32.h/.cpp               # Win32Window
    └── view_win32.h/.cpp                 # Win32View
```

#### 2.15.9 多屏幕/多窗口/多视图使用示例

```cpp
// 初始化 AR HUD 引擎，配置多屏幕
void init_arhud_engine() {
    ARHudEngine *engine = ARHudEngine::GetSingleton();
    IDisplayServer *ds = IDisplayServer::GetSingleton();

    // 枚举屏幕
    uint32_t screen_count = ds->get_screen_count();
    for (uint32_t i = 0; i < screen_count; i++) {
        auto *screen = ds->get_screen(i);
        ARHUD_LOG_INFO("Screen %d: %dx%d @ %.1fHz DPI=%.0f",
                       i, screen->get_width(), screen->get_height(),
                       screen->get_refresh_rate(), screen->get_dpi());
    }

    // Screen 0: 仪表盘 — 全屏
    IWindow::CreateParams cluster_params;
    cluster_params.screen_id = 0;
    cluster_params.mode = IWindow::Mode::EXCLUSIVE_FULLSCREEN;
    cluster_params.width = 1920;
    cluster_params.height = 720;
    IWindow::WindowID cluster_wid = ds->window_create(cluster_params);

    // 仪表盘窗口：3D 场景视图 + 2D 叠加层
    IView::Config scene_cfg;
    scene_cfg.type = IView::Type::SCENE_3D;
    scene_cfg.width = 1920;
    scene_cfg.height = 720;
    scene_cfg.z_order = 0.0f;
    scene_cfg.clear_color = true;
    scene_cfg.clear_depth = true;
    ds->view_create(cluster_wid, scene_cfg);

    IView::Config overlay_cfg;
    overlay_cfg.type = IView::Type::OVERLAY_2D;
    overlay_cfg.width = 1920;
    overlay_cfg.height = 720;
    overlay_cfg.z_order = 1.0f;
    overlay_cfg.clear_color = false;
    overlay_cfg.clear_depth = false;
    ds->view_create(cluster_wid, overlay_cfg);

    // Screen 1: HUD 投影 — 立体视图
    IWindow::CreateParams hud_params;
    hud_params.screen_id = 1;
    hud_params.mode = IWindow::Mode::EXCLUSIVE_FULLSCREEN;
    hud_params.width = 1920;
    hud_params.height = 1080;
    IWindow::WindowID hud_wid = ds->window_create(hud_params);

    IView::Config stereo_left;
    stereo_left.type = IView::Type::STEREO_LEFT;
    stereo_left.width = 1920;
    stereo_left.height = 1080;
    stereo_left.z_order = 0.0f;
    ds->view_create(hud_wid, stereo_left);

    IView::Config stereo_right;
    stereo_right.type = IView::Type::STEREO_RIGHT;
    stereo_right.width = 1920;
    stereo_right.height = 1080;
    stereo_right.z_order = 1.0f;
    ds->view_create(hud_wid, stereo_right);

    // Screen 2: 中控屏 — 多窗口
    IWindow::CreateParams nav_params;
    nav_params.screen_id = 2;
    nav_params.x = 0;
    nav_params.y = 0;
    nav_params.width = 800;
    nav_params.height = 480;
    IWindow::WindowID nav_wid = ds->window_create(nav_params);

    IWindow::CreateParams alert_params;
    alert_params.screen_id = 2;
    alert_params.x = 800;
    alert_params.y = 0;
    alert_params.width = 480;
    alert_params.height = 480;
    IWindow::WindowID alert_wid = ds->window_create(alert_params);

    // 启动每屏幕渲染线程
    for (uint32_t i = 0; i < screen_count; i++) {
        ds->screen_render_thread_start(i);
    }

    // 启动每窗口事件线程
    for (auto wid : ds->get_window_list()) {
        ds->window_thread_start(wid);
    }
}
```

#### 2.15.10 线程安全与资源所有权

多线程渲染的关键约束（独立上下文模型）：

| 资源 | 所有权 | 跨线程访问 | 同步方式 |
|------|--------|:---:|------|
| GPU 纹理/缓冲区 | 各渲染线程独立 | ❌ (不共享上下文) | 各线程独立创建 |
| 场景数据 (只读) | 主线程 -> 渲染线程 | ✅ (只读) | FrameSync |
| 场景数据 (读写) | 主线程 | ❌ | Mutex |
| 窗口事件 | 窗口线程 -> 主线程 | ✅ (单向) | 事件队列 + Mutex |
| 渲染命令 | 主线程 -> 渲染线程 | ✅ (单向) | 命令队列 + Semaphore |
| FBO/VAO (GL) | 渲染线程 | ❌ | 各线程独立创建 |

```cpp
// 线程安全的场景数据快照
class SceneSnapshot {
public:
    void UpdateFromScene(const Scene &p_scene) {
        MutexLock lock(write_mutex);
        // 深拷贝场景数据到双缓冲的后端
        back_data.objects = p_scene.GetRenderObjects();
        back_data.cameras = p_scene.GetCameras();
        back_data.lights = p_scene.GetLights();
        swap_requested.Set(true);
    }

    bool TrySwap() {
        if (swap_requested.Get()) {
            MutexLock lock(write_mutex);
            Swap(front_data, back_data);
            swap_requested.Set(false);
            return true;
        }
        return false;
    }

    const SceneData &GetCurrent() const { return front_data; }

private:
    SceneData front_data;
    SceneData back_data;
    Mutex write_mutex;
    SafeNumeric<bool> swap_requested{false};
};
```

### 2.16 渲染命令图（RDG Lite）设计

Godot 的 RenderingDeviceGraph 是一个功能完整但极其复杂的系统（约 3000+ 行），包含命令重排序、子资源追踪、二级命令缓冲等高级特性。AR HUD 场景不需要这些复杂功能，因此设计一个精简版 RDG Lite。

#### 2.16.1 设计原则

| 原则 | 说明 |
|------|------|
| **API 一致** | OpenGL 和 Vulkan 使用相同的 RDG 记录接口 |
| **执行策略可配** | OpenGL 用立即模式，Vulkan 用延迟编译模式 |
| **最小屏障** | HUD 场景资源依赖简单，只需粗粒度屏障 |
| **零帧内分配** | 所有命令节点从 PagedAllocator 分配 |
| **可扩展** | 预留 Compute List 接口，未来支持 GPU 畸变校正 |

#### 2.16.2 RDG Lite vs Godot RDG 对比

| 特性 | Godot RDG | ARHud RDG Lite（当前实现） |
|------|-----------|---------------------------|
| 命令类型 | 14 种 | 6 种（Draw/Compute/BufferCopy/TextureCopy/Clear/Timestamp） |
| 资源追踪 | 子资源级（mip/layer） | 资源级（整纹理/整缓冲区） |
| 屏障推导 | 精确（per-resource barrier） | 粗粒度（全内存屏障 + 可选纹理屏障） |
| 命令重排序 | 支持（拓扑排序 + 优先级） | 不支持（HUD 命令量小，无需重排序） |
| 二级命令缓冲 | 支持（多线程录制） | 不支持（OpenGL 限制） |
| 指令存储 | 序列化字节流 | ICommandBuffer 内部命令列表（非字节流） |
| 内存分配 | PagedAllocator | PagedAllocator（同 Godot） |
| DrawList 执行时机 | 帧末 RDG::end() 统一编译+回放 | DrawListEnd() 立即执行（每个 DrawList 独立） |
| 多 DrawList 并发 | ✅ 可同时存在多个 | ❌ 同一时间只允许一个 |
| ICommandBuffer 抽象 | ❌ 无，直接序列化字节流 | ✅ 额外抽象层（录制/回放） |
| 跨 DrawList 优化 | ✅ 屏障合并、命令重排序 | ❌ 每个 DrawList 独立执行 |

**设计决策说明：**

当前 RDG Lite 是简化实现，公共 API（DrawListBegin/End、DrawListBindPipeline 等）与 Godot 对齐，
但内部实现使用 ICommandBuffer 录制/回放而非 Godot 的序列化字节流 + 帧末统一编译。
这是有意为之的简化：

1. **HUD 场景不需要帧级优化**：命令量小（< 100 DrawCall），屏障合并和重排序收益有限
2. **ICommandBuffer 提供调试便利**：可录制/回放/检查命令流
3. **API 层面对齐**：未来 RDG 完整实现时，DrawList API 保持不变，内部替换为字节流 + 延迟编译
4. **逐步演进**：先跑通基本流程，再按需引入 Godot 风格的序列化字节流和帧末编译

#### 2.16.3 核心类设计

**当前实现采用 DrawList API 作为公共绘制接口（对齐 Godot RenderingDevice::draw_list_xxx），CommandBuffer 作为内部实现细节。**

```cpp
// ═══════════════════════════════════════════════════════════════════
// DrawList — 公共绘制 API（RenderingDevice 的公共方法）
// ═══════════════════════════════════════════════════════════════════

using DrawListID = uint32_t;
static constexpr DrawListID kInvalidDrawListId = UINT32_MAX;

class RenderingDevice {
public:
    // ─── DrawList 公共 API ───
    // 客户端通过这些方法录制绘制命令，不直接操作 ICommandBuffer

    DrawListID DrawListBegin();
    void DrawListSetViewport(DrawListID p_list,
                             int32_t p_x, int32_t p_y,
                             uint32_t p_width, uint32_t p_height);
    void DrawListSetScissor(DrawListID p_list,
                            int32_t p_x, int32_t p_y,
                            uint32_t p_width, uint32_t p_height);
    void DrawListSetBlendConstants(DrawListID p_list,
                                   float p_r, float p_g,
                                   float p_b, float p_a);
    void DrawListBindRenderPipeline(DrawListID p_list, RDPipelineID p_pipeline);
    void DrawListBindUniformSet(DrawListID p_list,
                                RDUniformSetID p_uniform_set,
                                uint32_t p_set_index);
    void DrawListBindVertexBuffers(DrawListID p_list,
                                   const RDBufferID *p_buffers,
                                   const uint64_t *p_offsets,
                                   uint32_t p_count);
    void DrawListBindIndexBuffer(DrawListID p_list,
                                 RDBufferID p_buffer,
                                 IndexBufferFormat p_format,
                                 uint64_t p_offset);
    void DrawListDraw(DrawListID p_list,
                      uint32_t p_vertex_count,
                      uint32_t p_instance_count = 1);
    void DrawListDrawIndexed(DrawListID p_list,
                             uint32_t p_index_count,
                             uint32_t p_instance_count = 1);
    void DrawListEnd();

private:
    // ─── CommandBuffer 池管理（内部方法，供 DrawList 使用）───
    RDCommandBufferID CommandBufferAcquire();
    void CommandBufferRelease(RDCommandBufferID p_command_buffer);
    ICommandBuffer *CommandBufferGetDriverCmd(RDCommandBufferID p_command_buffer) const;

    // ─── DrawList 内部状态 ───
    struct DrawListState
    {
        bool active = false;
        RDCommandBufferID command_buffer;
        ICommandBuffer *driver_cmd = nullptr;
    } draw_list_;
};
```

**DrawList 工作流（对齐 Godot draw_list_begin/end）：**

```
DrawListBegin()
  ├── CommandBufferAcquire() → 从池中获取空闲 CommandBuffer
  ├── CommandBufferGetDriverCmd() → 获取 ICommandBuffer 指针
  ├── driver_cmd->Begin() → 开始录制
  └── draw_list_.active = true

DrawListBindRenderPipeline(dl, pipeline_rid)
  ├── pipeline_owner_.GetOrNull(pipeline_rid) → RID → 簿记结构
  └── driver_cmd->BindRenderPipeline(pipeline->driver_id) → RID→DriverID 自动转换

DrawListDraw(dl, vertex_count, instance_count)
  └── driver_cmd->Draw(vertex_count, instance_count)

DrawListEnd()
  ├── driver_cmd->End() → 结束录制
  ├── driver_cmd->Execute() → 回放执行
  ├── CommandBufferRelease(command_buffer) → 归还池
  └── draw_list_.active = false
```

**关键设计决策：为什么 DrawList 是公共 API 而 CommandBuffer 是内部细节？**

1. **对齐 Godot**：Godot 4.6 的 `RenderingDevice::draw_list_begin/end` 是公共 API，
   `CommandBufferID` 仅在 RDG 内部使用
2. **层次隔离**：客户端不需要知道 CommandBuffer 的存在，降低 API 复杂度
3. **自动 RID→DriverID 转换**：DrawList 方法接收 RD 层的 RID（如 RDPipelineID），
   内部自动查找簿记结构获取 DriverID，客户端无需手动转换
4. **池化管理**：CommandBuffer 的 Acquire/Release 由 DrawList 自动管理，
   客户端无需关心池状态

**与 Godot RDG 的关系：**

当前 DrawList 是 RDG Lite 的简化实现（无序列化字节流、无屏障推导）。
未来 RDG 完整实现时，DrawList API 保持不变，内部替换为序列化指令流 + 延迟编译。

#### 2.16.4 当前 DrawList 实现（基于 ICommandBuffer 录制/回放）

当前 DrawList 通过 ICommandBuffer 的录制/回放机制实现，而非序列化字节流：

```cpp
DrawListID RenderingDevice::DrawListBegin()
{
    // 从 CommandBuffer 池获取空闲 CommandBuffer
    RDCommandBufferID cmd_id = CommandBufferAcquire();
    ICommandBuffer *driver_cmd = CommandBufferGetDriverCmd(cmd_id);

    // 开始录制
    driver_cmd->Begin();

    // 记录 DrawList 状态
    draw_list_.active = true;
    draw_list_.command_buffer = cmd_id;
    draw_list_.driver_cmd = driver_cmd;
    return 0;
}

void RenderingDevice::DrawListBindRenderPipeline(DrawListID p_list, RDPipelineID p_pipeline)
{
    // RID → 簿记结构 → DriverID 自动转换
    Pipeline *pipeline = pipeline_owner_.GetOrNull(p_pipeline.GetRid());
    draw_list_.driver_cmd->BindRenderPipeline(pipeline->driver_id);
}

void RenderingDevice::DrawListEnd()
{
    // 结束录制
    if (draw_list_.driver_cmd != nullptr)
    {
        draw_list_.driver_cmd->End();
        // 回放执行
        draw_list_.driver_cmd->Execute();
    }
    // 归还 CommandBuffer 到池
    if (!draw_list_.command_buffer.IsNull())
    {
        CommandBufferRelease(draw_list_.command_buffer);
    }
    draw_list_.active = false;
    draw_list_.command_buffer = RDCommandBufferID();
    draw_list_.driver_cmd = nullptr;
}
```

**GLCommandBuffer 的录制/回放机制：**

```cpp
// GLCommandBuffer 将命令录制到内部指令流，Execute() 时回放
void GLCommandBuffer::Begin()
{
    // 重置指令流
    command_stream_.Clear();
    state_ = CommandBufferState::kRecording;
}

void GLCommandBuffer::BindRenderPipeline(PipelineID p_pipeline)
{
    // 录制指令到指令流
    RecordCommand(CommandType::kBindPipeline, p_pipeline);
}

void GLCommandBuffer::End()
{
    state_ = CommandBufferState::kReady;
}

void GLCommandBuffer::Execute()
{
    // 回放指令流，调用实际 GL 函数
    for (auto &cmd : command_stream_)
    {
        switch (cmd.type)
        {
        case CommandType::kBindPipeline:
            // 通过 GLStateCache 应用 PSO 状态
            state_cache_.ApplyPipelineState(cmd.pipeline_id);
            break;
        case CommandType::kDraw:
            glDrawArraysInstanced(GL_TRIANGLES, 0, cmd.vertex_count, cmd.instance_count);
            break;
        // ... 其他命令类型
        }
    }
    state_ = CommandBufferState::kExecuted;
}
```

**关键点：** 即使是 OpenGL 后端，也通过录制/回放间接执行。好处：
1. 统一了 OpenGL 和 Vulkan 的上层调用代码（DrawList API 完全相同）
2. 可以在指令流中插入调试/性能追踪
3. 未来切换到 RDG 完整实现时，只需替换 Execute() 的内部逻辑
4. CommandBuffer 池化复用，避免每帧创建/销毁

#### 2.16.5 Vulkan 延迟模式实现（未来设计）

> **注意：** 以下为未来 RDG 完整实现的设计，当前 RDG Lite 使用 ICommandBuffer 录制/回放，
> 不包含屏障推导和帧末统一编译。

Vulkan 后端在 `end()` 时编译屏障并录制到 CommandBuffer：

```cpp
void RDG::CompileBarriers() {
    // 简化版屏障推导：遍历命令序列，检查资源使用变化
    for (int32_t i = 0; i < (int32_t)command_order.size(); i++) {
        int32_t cmd_idx = command_order[i];
        auto *cmd = get_command(cmd_idx);

        for (uint32_t t = 0; t < cmd->tracker_count; t++) {
            auto &tracker = get_tracker_for_command(cmd_idx, t);
            auto prev_usage = tracker.last_usage;
            auto curr_usage = get_usage_for_command(cmd_idx, t);

            if (prev_usage != ResourceUsage::NONE && prev_usage != curr_usage) {
                // 需要屏障
                VkMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARrier;
                barrier.srcAccessMask = usage_to_vk_access(prev_usage);
                barrier.dstAccessMask = usage_to_vk_access(curr_usage);

                VkPipelineStageFlags src_stage = usage_to_vk_stage(prev_usage);
                VkPipelineStageFlags dst_stage = usage_to_vk_stage(curr_usage);

                vkCmdPipelineBarrier(cmd_buffer,
                    src_stage, dst_stage, 0,
                    1, &barrier, 0, nullptr, 0, nullptr);
            }

            tracker.last_usage = curr_usage;
            tracker.last_command_index = i;
        }
    }
}
```

#### 2.16.6 HUD 场景的屏障简化

HUD 渲染的典型资源依赖模式非常简单：

```
帧开始 → Clear → Draw Layer0 → Draw Layer1 → ... → Draw LayerN → PostProcess → Present
```

**关键观察：**
1. **无 Compute→Graphics 依赖**：HUD 不使用 Compute Shader（未来畸变校正除外）
2. **无纹理 RW 冲突**：所有纹理要么只读（采样），要么只写（渲染目标）
3. **顺序依赖明确**：Layer 必须按 z_order 顺序渲染

因此，RDG Lite 的屏障策略可以极度简化：

```cpp
// HUD 专用屏障策略
void RDG::InsertHudBarriers() {
    if (execution_mode == ExecutionMode::IMMEDIATE) {
        // OpenGL: glMemoryBarrier 足矣
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        return;
    }

    // Vulkan: 只需在以下位置插入屏障
    // 1. Clear → Draw: COLOR_ATTACHMENT → FRAGMENT_SHADER (如果后续采样)
    // 2. Draw → PostProcess: COLOR_ATTACHMENT → FRAGMENT_SHADER
    // 3. PostProcess → Present: COLOR_ATTACHMENT → BOTTOM_OF_PIPE

    // 全内存屏障（最安全，HUD 命令量小，开销可忽略）
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_buffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
}
```

#### 2.16.7 RDG Lite 帧循环

```cpp
void ARHudCompositor::BeginFrame(double frame_step) {
    frame_number++;
    delta_time = frame_step;

    // 更新场景快照
    scene_snapshot.try_swap();
}

void ARHudCompositor::Render(ARHudSceneTree *scene) {
    auto &snapshot = scene_snapshot.get_current();

    // 遍历 HUD 图层，按 z_order 排序
    for (auto &layer : snapshot.layers) {
        // 开始绘制列表
        DrawListID dl = device->DrawListBegin();

        // 设置视口和裁剪
        device->DrawListSetViewport(dl, layer.viewport.x, layer.viewport.y,
                                     layer.viewport.width, layer.viewport.height);
        device->DrawListSetScissor(dl, layer.viewport.x, layer.viewport.y,
                                    layer.viewport.width, layer.viewport.height);

        // 绑定管线和资源
        device->DrawListBindRenderPipeline(dl, layer.pipeline);
        device->DrawListBindUniformSet(dl, layer.uniform_set, 0);

        // 绘制元素
        for (auto &element : layer.elements) {
            element.render(this, dl);
        }

        device->DrawListEnd();
    }

    // 后处理（畸变校正）
    if (stereo_config.mode != ARHudStereoConfig::StereoMode::NONE) {
        DrawListID dl = device->DrawListBegin();
        device->DrawListSetViewport(dl, 0, 0, width, height);
        device->DrawListBindRenderPipeline(dl, distortion_pipeline);
        device->DrawListBindUniformSet(dl, distortion_uniform_set, 0);
        device->DrawListDraw(dl, 3, 1);  // 全屏三角形
        device->DrawListEnd();
    }
}

void ARHudCompositor::EndFrame(bool present) {
    if (present) {
        device->SwapBuffers(main_surface);
    }
}
```

#### 2.16.8 OpenGL 延迟录制模式（主线程录制 / 渲染线程播放）

> **注意：** 以下为未来多线程渲染架构设计，当前仅支持单线程 DrawList 模式。
> DrawList API 在单线程和多线程模式下保持一致，内部实现将随 RDG 完整实现而替换。

**动机：** 虽然 OpenGL 传统上是立即模式 API，但 ARHud 引擎期望统一使用"主线程录制命令，渲染线程回放执行"的模式，以实现：
1. **API 一致性**：OpenGL 和 Vulkan 使用完全相同的上层调用模式
2. **帧率解耦**：主线程逻辑更新频率可与渲染线程解耦
3. **多屏并行**：MULTI_THREAD 模式下，主线程可向多个渲染线程分发命令
4. **调试友好**：可录制/回放/重放命令流用于调试

**核心架构：**

```
┌──────────────────┐         ┌──────────────────────────────┐
│    主线程         │         │        渲染线程               │
│                  │         │                              │
│  RDG.begin()     │         │  等待 main_to_render 信号量   │
│    ↓             │         │           ↓                  │
│  录制 DrawList   │──指令流─→│  从 CommandQueue 消费指令     │
│  到 instruction_ │  (Ring   │           ↓                  │
│  stream          │   Buffer)│  _execute_draw_list_immediate│
│    ↓             │         │  (在共享 GL 上下文中执行)      │
│  RDG.end()       │         │           ↓                  │
│    ↓             │         │  swap_buffers                │
│  提交到队列 +    │         │           ↓                  │
│  post 信号量     │         │  post render_to_main 信号量   │
└──────────────────┘         └──────────────────────────────┘
```

##### 2.16.8.1 双缓冲命令队列设计（未来设计）

```cpp
class ARHudCommandQueue {
public:
    struct FrameCommandBuffer {
        ARHudVector<uint8_t> instruction_stream;   // 序列化的 RDG 指令
        uint64_t frame_number = 0;
        uint32_t command_count = 0;
        bool ready = false;                        // 主线程标记为就绪
        bool consumed = false;                     // 渲染线程标记为已消费
    };

private:
    static constexpr uint32_t QUEUE_DEPTH = 3;     // 三缓冲，允许 2 帧延迟

    FrameCommandBuffer buffers[QUEUE_DEPTH];
    ARHudAtomic<uint32_t> produce_index{0};        // 主线程写入位置
    ARHudAtomic<uint32_t> consume_index{0};        // 渲染线程读取位置
    ARHudSpinLock queue_lock;

public:
    // 主线程调用：获取可写入的命令缓冲区
    FrameCommandBuffer *acquire_produce_buffer() {
        uint32_t next = (produce_index.load() + 1) % QUEUE_DEPTH;
        if (next == consume_index.load()) {
            return nullptr;  // 队列满，渲染线程太慢
        }
        auto &buf = buffers[produce_index.load()];
        buf.instruction_stream.clear();
        buf.command_count = 0;
        buf.ready = false;
        buf.consumed = false;
        return &buf;
    }

    // 主线程调用：提交已录制的命令
    void commit_produce_buffer(uint64_t p_frame_number) {
        auto &buf = buffers[produce_index.load()];
        buf.frame_number = p_frame_number;
        buf.ready = true;
        produce_index.store((produce_index.load() + 1) % QUEUE_DEPTH);
    }

    // 渲染线程调用：获取待执行的命令缓冲区
    FrameCommandBuffer *acquire_consume_buffer() {
        if (consume_index.load() == produce_index.load()) {
            return nullptr;  // 队列空，无新命令
        }
        auto &buf = buffers[consume_index.load()];
        if (!buf.ready) {
            return nullptr;  // 尚未就绪
        }
        return &buf;
    }

    // 渲染线程调用：标记消费完成
    void release_consume_buffer() {
        auto &buf = buffers[consume_index.load()];
        buf.consumed = true;
        consume_index.store((consume_index.load() + 1) % QUEUE_DEPTH);
    }

    bool is_empty() const { return consume_index.load() == produce_index.load(); }
    bool is_full() const {
        uint32_t next = (produce_index.load() + 1) % QUEUE_DEPTH;
        return next == consume_index.load();
    }
};
```

##### 2.16.8.2 RDG Lite 延迟模式接口扩展（未来设计）

> **注意：** 当前 DrawList 使用 ICommandBuffer 录制/回放，以下 ARHudRDG 延迟模式
> 为未来 RDG 完整实现时的设计，届时 DrawList API 保持不变，内部替换为序列化字节流。

```cpp
class ARHudRDG {
public:
    enum class RecordMode {
        IMMEDIATE_EXECUTE,    // §2.16.4: 调用即执行（单线程模式）
        DEFERRED_RECORD,      // 本节: 录制到指令流（多线程模式）
    };

    // 设置录制目标队列（多线程模式时调用）
    void set_command_queue(ARHudCommandQueue *p_queue);
    void set_record_mode(RecordMode p_mode);

    // begin/end 行为根据 mode 变化：
    // - IMMEDIATE_EXECUTE: begin() 无操作，end() 无操作
    // - DEFERRED_RECORD:   begin() acquire produce buffer, end() commit buffer
    void begin() override;
    void end() override;

private:
    RecordMode record_mode = RecordMode::IMMEDIATE_EXECUTE;
    ARHudCommandQueue *command_queue = nullptr;
    FrameCommandBuffer *current_recording_buffer = nullptr;

    // 延迟录制：将指令序列化到 current_recording_buffer
    void _record_instruction(const void *p_data, uint32_t p_size);

    // 重定向 draw_list_xxx 的实现
    void _draw_list_bind_pipeline_deferred(DrawListID p_id, ARHudRID p_pipeline);
    void _draw_list_draw_deferred(DrawListID p_id, uint32_t p_vertex_count,
                                   uint32_t p_instance_count);
};
```

**关键实现细节 — 指令序列化：**

```cpp
void ARHudRDG::_record_instruction(const void *p_data, uint32_t p_size) {
    ARHUD_ASSERT(record_mode == RecordMode::DEFERRED_RECORD);
    ARHUD_ASSERT(current_recording_buffer != nullptr);

    auto &stream = current_recording_buffer->instruction_stream;
    uint32_t offset = (uint32_t)stream.size();
    stream.resize(offset + p_size);
    memcpy(stream.data() + offset, p_data, p_size);
    current_recording_buffer->command_count++;
}

// 示例：draw_list_bind_pipeline 的延迟版本
void ARHudRDG::draw_list_bind_pipeline(DrawListID p_id, ARHudRID p_pipeline) {
    if (record_mode == RecordMode::IMMEDIATE_EXECUTE) {
        _execute_bind_pipeline_immediate(p_id, p_pipeline);  // 直接调 GL
        return;
    }

    // 延迟模式：序列化到指令流
    DrawListBindPipelineInstr instr{};
    instr.type = DrawListInstruction::TYPE_BIND_PIPELINE;
    instr.draw_list_id = p_id;
    instr.pipeline_rid = p_pipeline;
    _record_instruction(&instr, sizeof(instr));
}
```

##### 2.16.8.3 主线程录制流程

```cpp
// 主线程帧循环（MULTI_THREAD 模式）
// 注意：此为未来设计，当前仅支持单线程 DrawList 模式
void ARHudCompositor::main_thread_tick_deferred(double frame_step) {
    // 1. 更新场景数据
    update_scene(frame_step);

    for (auto &screen : screens) {
        auto &queue = screen.command_queue;

        // 2. 获取命令缓冲区
        auto *cmd_buf = queue->acquire_produce_buffer();
        if (!cmd_buf) {
            ARHUD_LOG_WARN("Screen %d command queue full, skipping frame",
                           screen.id);
            continue;
        }

        // 3. 录制 DrawList 命令到命令缓冲区
        //    未来 RDG 完整实现时，DrawList 将序列化到字节流
        DrawListID dl = device->DrawListBegin();
        device->DrawListSetViewport(dl, 0, 0, screen.width, screen.height);
        // ... 绘制命令 ...
        device->DrawListEnd();

        // 4. 提交命令缓冲区
        queue->commit_produce_buffer();

        // 5. 通知渲染线程
        screen.frame_sync.main_to_render->post();
    }
}

// 关键优势：render_scene() 代码完全复用
void ARHudCompositor::Render(ARHudSceneTree *scene) {
    auto &snapshot = scene_snapshot.get_current();
    for (auto &layer : snapshot.layers) {
        DrawListID dl = device->DrawListBegin();
        device->DrawListSetViewport(dl, layer.viewport.x, layer.viewport.y,
                                     layer.viewport.width, layer.viewport.height);
        device->DrawListSetScissor(dl, layer.viewport.x, layer.viewport.y,
                                    layer.viewport.width, layer.viewport.height);
        device->DrawListBindRenderPipeline(dl, layer.pipeline);
        device->DrawListBindUniformSet(dl, layer.uniform_set, 0);

        for (auto &element : layer.elements) {
            element.render(this, dl);
        }

        device->DrawListEnd();
    }
}
```

##### 2.16.8.4 渲染线程播放流程（未来设计）

```cpp
// 渲染线程入口函数
void render_thread_func(ScreenID screen_id) {
    auto &screen = screens[screen_id];
    auto &sync = screen.frame_sync;
    auto &queue = *screen.command_queue;

    // 绑定独立 GL 上下文到此线程
    gl_manager->MakeCurrent(screen.gl_context, screen.gl_surface);

    // 创建此线程私有的 VAO/FBO/状态缓存
    thread_local_resources.initialize_for_thread();

    while (!sync.quit.load()) {
        // 1. 等待主线程信号
        sync.main_to_render->wait();
        if (sync.quit.load()) break;

        // 2. 从队列取出命令
        auto *cmd_buf = queue.acquire_consume_buffer();
        if (!cmd_buf) {
            sync.render_to_main->post();
            continue;
        }

        // 3. 执行指令流（复用 §2.16.4 的立即模式执行器）
        execute_command_buffer(cmd_buf);

        // 4. Present
        device->SwapBuffers(screen.surface_id);

        // 5. 标记消费完成
        queue.release_consume_buffer();

        // 6. 通知主线程
        sync.render_to_main->post();
    }
}

// 执行单个命令缓冲区
void ARHudCompositor::execute_command_buffer(
    ARHudCommandQueue::FrameCommandBuffer *p_cmd_buf) {

    const auto &stream = p_cmd_buf->instruction_stream;
    const uint8_t *ptr = stream.data();
    const uint8_t *end = ptr + stream.size();

    // 解析顶层命令头
    while (ptr < end) {
        auto *header = reinterpret_cast<const CommandHeader *>(ptr);
        ptr += sizeof(CommandHeader);

        switch (header->type) {
        case CommandHeader::TYPE_DRAW_LIST:
            _execute_draw_list_immediate(ptr, header->instruction_size);
            break;
        case CommandHeader::TYPE_COMPUTE_LIST:
            _execute_compute_list_immediate(ptr, header->instruction_size);
            break;
        case CommandHeader::TYPE_BUFFER_COPY:
            _execute_buffer_copy_immediate(ptr, header->instruction_size);
            break;
        case CommandHeader::TYPE_TEXTURE_COPY:
            _execute_texture_copy_immediate(ptr, header->instruction_size);
            break;
        case CommandHeader::TYPE_CLEAR:
            _execute_clear_immediate(ptr, header->instruction_size);
            break;
        default:
            ARHUD_ASSERT_MSG(false, "Unknown command type in deferred playback");
        }

        ptr += header->instruction_size;
    }
}
```

##### 2.16.8.5 资源生命周期管理（未来设计）

**原则：独立上下文模型下，每个渲染线程拥有独立的 RDD 和资源空间，资源在各线程独立创建/销毁。**

```
资源类型          创建线程        销毁线程        共享方式
─────────────────────────────────────────────────────────
Texture           各渲染线程      各渲染线程      ❌ 不共享，各上下文独立创建
Buffer (VBO/IBO)  各渲染线程      各渲染线程      ❌ 同上
Shader Program    各渲染线程      各渲染线程      ❌ 同上
Pipeline State    各渲染线程      各渲染线程      ✅ 纯 CPU 数据结构
VAO               各渲染线程      各渲染线程      ❌ 不可共享，每线程创建
FBO               各渲染线程      各渲染线程      ❌ 不可共享，每线程创建
GL State Cache    各渲染线程      各渲染线程      ❌ 每线程独立实例
```

**线程安全资源访问协议：**

```cpp
class GLRenderingDeviceDriver {
    // 资源注册表
    RIDOwner<GLTexture, true> texture_owner;
    RIDOwner<GLBuffer, true> buffer_owner;
    RIDOwner<GLShaderProgram, true> shader_owner;

    // 渲染线程本地存储（TLS）
    struct ThreadLocalGLState {
        GLuint global_vao = 0;              // 全局 VAO（Core Profile 要求）
        HashMap<WindowID, GLuint> fbos; // 按窗口缓存的 FBO
        GLStateCache state_cache;       // GL 状态缓存（减少冗余调用）

        void ensure_vao() {
            if (!global_vao) glGenVertexArrays(1, &global_vao);
        }
    };

    static thread_local ThreadLocalGLState tls_gl_state;

public:
    // 主线程调用：创建纹理（生成 GL 名称，各上下文独立创建，不共享）
    RID TextureCreate(const TextureFormat &p_fmt) {
        ARHUD_ASSERT(Thread::is_main_thread() || IsRenderThread());

        GLuint tex_name;
        glGenTextures(1, &tex_name);
        glBindTexture(GL_TEXTURE_2D, tex_name);
        glTexImage2D(GL_TEXTURE_2D, 0, p_fmt.internal_format,
                      p_fmt.width, p_fmt.height, 0,
                      p_fmt.format, p_fmt.type, nullptr);

        GLTexture tex{tex_name, p_fmt};
        return texture_owner.MakeRid(tex);
    }

    // 渲染线程调用：通过 RID 获取 GL 名称并使用
    GLuint get_gl_texture_name(RID p_rid) {
        auto *tex = texture_owner.GetOrNull(p_rid);
        ARHUD_ASSERT(tex);
        return tex->gl_name;  // 独立上下文模型：此名称仅在创建它的上下文中有效
    }

    // 渲染线程调用：获取/创建当前线程的 FBO
    GLuint get_or_create_fbo(WindowID p_window, const FBOSpec &p_spec) {
        auto &tls = tls_gl_state;
        auto it = tls.fbos.find(p_window);
        if (it != tls.fbos.end()) return it->value;

        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        // ... 附加纹理附件 ...
        tls.fbos[p_window] = fbo;
        return fbo;
    }
};
```

##### 2.16.8.6 状态缓存一致性（未来设计）

**问题：** 主线程录制时不维护 GL 状态，渲染线程播放时需要完整的状态追踪。

**解决方案：** 渲染线程持有独立的 `GLStateCache`，指令流中的每个 bind 调用都通过状态缓存过滤冗余 GL 调用：

```cpp
class GLStateCache {
    GLuint current_program = 0;
    GLuint current_vao = 0;
    GLuint current_fbo = 0;
    GLuint current_textures[16] = {0};
    GLuint current_buffers[GL_ARRAY_BUFFER + 1] = {0};
    Rect current_viewport;
    Rect current_scissor;
    bool blend_enabled = false;
    // ...

public:
    void bind_program(GLuint p_program) {
        if (current_program != p_program) {
            glUseProgram(p_program);
            current_program = p_program;
        }
    }

    void bind_vao(GLuint p_vao) {
        if (current_vao != p_vao) {
            glBindVertexArray(p_vao);
            current_vao = p_vao;
        }
    }

    void bind_texture(uint32_t p_unit, GLenum p_target, GLuint p_texture) {
        if (current_textures[p_unit] != p_texture) {
            glActiveTexture(GL_TEXTURE0 + p_unit);
            glBindTexture(p_target, p_texture);
            current_textures[p_unit] = p_texture;
        }
    }

    void SetViewport(const Rect &p_rect) {
        if (memcmp(&current_viewport, &p_rect, sizeof(p_rect)) != 0) {
            glViewport(p_rect.x, p_rect.y, p_rect.width, p_rect.height);
            current_viewport = p_rect;
        }
    }

    void invalidate_all() {
        memset(current_textures, 0, sizeof(current_textures));
        memset(current_buffers, 0, sizeof(current_buffers));
        current_program = 0;
        current_vao = 0;
        current_fbo = 0;
    }
};
```

##### 2.16.8.7 模式切换与兼容性矩阵（未来设计）

> **注意：** 当前仅支持 SINGLE_IMMEDIATE 模式（DrawList + ICommandBuffer 录制/回放）。

```cpp
enum class ExecutionConfig {
    SINGLE_IMMEDIATE,     // 单线程 + 立即执行（默认，最简配置）
    SINGLE_DEFERRED,      // 单线程 + 延迟录制+立即回放（调试用途）
    MULTI_DEFERRED,       // 多线程 + 延迟录制+渲染线程回放（生产配置）
};

// 配置选择指南
// ┌─────────────────────┬───────────────┬──────────────────────────────┐
// │ 场景                 │ 推荐配置      │ 说明                         │
// ├─────────────────────┼───────────────┼──────────────────────────────┤
// │ 开发/调试            │ SINGLE_IMMEDIATE│ 快速迭代，无线程复杂性       │
// │ 单屏仪表盘           │ SINGLE_IMMEDIATE│ HUD 负载低，无需多线程       │
// │ 多屏 HUD (2~4 屏)    │ MULTI_DEFERRED │ 并行渲染，避免木桶效应        │
// │ 性能剖析             │ SINGLE_DEFERRED │ 可录制/重放命令流            │
// │ 回归测试             │ SINGLE_DEFERRED │ 可对比录制结果与实时结果      │
// └─────────────────────┴───────────────┴──────────────────────────────┘
```

##### 2.16.8.8 与 Vulkan 模式的统一视图（未来设计）

**最终效果：OpenGL 和 Vulkan 在上层代码中完全一致：**

```
┌─────────────────────────────────────────────────────────┐
│                   应用层代码（无 API 差异）               │
│                                                         │
│   compositor->BeginFrame(dt)                            │
│   dl = device->DrawListBegin()                          │
│   device->DrawListSetViewport(dl, ...)                  │
│   device->DrawListBindRenderPipeline(dl, pipeline)      │
│   device->DrawListBindUniformSet(dl, ubo, 0)            │
│   device->DrawListDraw(dl, vertex_count, 1)             │
│   device->DrawListEnd()                                 │
│   compositor->EndFrame(true)                            │
├─────────────────────┬───────────────────────────────────┤
│   OpenGL 后端        │   Vulkan 后端                     │
│                     │                                   │
│  当前 (RDG Lite):   │  未来 (RDG 完整实现):             │
│  → ICommandBuffer   │  → 序列化指令流                   │
│    录制/回放        │  → 编译 VkPipelineBarrier         │
│  → DrawListEnd()    │  → 录制到 VkCommandBuffer         │
│    立即执行         │  → vkQueueSubmit                 │
│                     │                                   │
│  未来 (RDG 完整):   │                                   │
│  → 序列化指令流      │                                   │
│  → 帧末统一编译      │                                   │
│  → 渲染线程回放      │                                   │
└─────────────────────┴───────────────────────────────────┘
```

### 2.17 关键问题与解决方案

本节记录 ARHud 渲染引擎设计中识别出的关键问题及其解决方案，作为设计决策的依据和实现时的参考。

#### 2.17.1 线程模型：避免过度设计

**问题：** 当前设计为每屏一渲染线程 + 每窗口一事件线程，但 AR HUD 场景渲染负载极低（2D 叠加为主，DrawCall < 100），多线程的复杂度收益比可能为负。

**分析：**

| 场景 | 线程数 | 风险 |
|------|--------|------|
| 仪表盘(1屏1窗) | 主线程+渲染线程+事件线程 = 3 | 中等 |
| 仪表盘+HUD投影+中控屏(3屏4窗) | 1主+3渲染+4事件 = **8** | 高 |
| QNX 车规(资源受限) | 同上但栈/内存受限 | **危险** |

多渲染线程引入的问题：
- 上下文切换开销
- 独立上下文资源重复创建开销
- 帧同步延迟增加（木桶效应）
- 内存占用翻倍（每线程独立 VAO/FBO/纹理/着色器）
- 调试难度指数级上升

**解决方案：统一架构，两种使用模式**

ARHud 采用 Context/Surface 分离模型，统一支持单线程多 Surface 和多线程独立上下文。
两种模式使用完全相同的 API，区别仅在于 RenderingDevice 实例数量和线程分配。

```cpp
enum class ThreadModel {
    SINGLE_THREAD,       // 1 RD + 1 Context + N Surface（默认）
    MULTI_THREAD,        // N RD + N Context + N Surface（每线程独立渲染栈）
};
```

**SINGLE_THREAD 模式（默认）：**
```
┌──────────────────────────────────────────────────────────────┐
│                     主线程 (Main Thread)                      │
│  RenderingDevice (主 RD)                                     │
│  └── MainContext (ContextID=0)                               │
│      ├── Surface 0: MakeCurrent → Render → SwapBuffers       │
│      ├── Surface 1: MakeCurrent → Render → SwapBuffers       │
│      └── Surface 2: MakeCurrent → Render → SwapBuffers       │
│                                                              │
│  rd->BeginFrame();                                           │
│  for (auto surf : surfaces) {                                │
│      rd->MakeCurrent(surf);                                  │
│      DrawListID dl = rd->DrawListBegin();                    │
│      rd->DrawListSetViewport(dl, ...);                       │
│      // ... 绘制命令 ...                                      │
│      rd->DrawListEnd();                                      │
│      rd->SwapBuffers(surf);                                  │
│  }                                                           │
│  rd->EndFrame();                                             │
└──────────────────────────────────────────────────────────────┘
```

- 优点：零同步开销、零死锁风险、调试简单
- 缺点：某屏幕渲染慢会拖慢其他屏幕
- 适用：90% 的 HUD 场景（3 屏总渲染 < 5ms）

**MULTI_THREAD 模式（可选）：**
```
┌──────────────────────┐  ┌──────────────────────┐
│ Thread 0 (主 RD)      │  │ Thread 1 (独立 RD)    │
│ MainContext (HGLRC_0) │  │ IndepContext(HGLRC_1) │
│ └── Surface 0         │  │ └── Surface 1         │
│                       │  │                       │
│ rd->BeginFrame();     │  │ indep_rd->BeginFrame();│
│ rd->MakeCurrent(s0);  │  │ indep_rd->MakeCurrent │
│ dl = DrawListBegin(); │  │   OnContext(ctx1, s1); │
│ DrawListEnd();        │  │ dl = DrawListBegin(); │
│ rd->SwapBuffers(s0);  │  │ DrawListEnd();        │
│ rd->EndFrame();       │  │ indep_rd->SwapBuffers();│
│                       │  │ indep_rd->EndFrame();  │
└──────────────────────┘  └──────────────────────┘
  资源不共享，各线程          资源不共享，各线程
  独立创建 GPU 资源          独立创建 GPU 资源
```

- 仅当某屏幕确实需要独立帧率时启用
- 每个线程拥有独立的 RenderingDevice + RDD + GL Context
- 资源不共享（不使用 wglShareLists），各线程独立创建 GPU 资源
- 通过 RenderingDevice::CreateIndependentDevice() 创建独立渲染栈

#### 2.17.2 帧同步：避免木桶效应和死锁

**问题：** 当前帧同步模型中，主线程等待所有屏幕完成渲染，导致木桶效应（最慢屏幕拖慢整体），且渲染线程 crash 会导致主线程死锁。

**解决方案：异步帧同步 + 超时保护**

```cpp
struct FrameSync {
    Semaphore *main_to_render;
    Semaphore *render_to_main;
    SafeNumeric<bool> quit{false};
    SafeNumeric<bool> frame_in_flight{false};

    static constexpr uint32_t SYNC_TIMEOUT_MS = 100;  // 100ms 超时
};

// 主线程：异步提交 + 超时等待
void main_thread_tick_multi() {
    update_scene();

    // 向所有渲染线程发送渲染信号
    for (auto &rt : screen_render_threads) {
        rt.frame_sync.frame_in_flight.store(true);
        rt.frame_sync.main_to_render->post();
    }

    // 等待各屏幕完成（带超时）
    for (auto &rt : screen_render_threads) {
        bool completed = wait_with_timeout(rt.frame_sync.render_to_main,
                                            FrameSync::SYNC_TIMEOUT_MS);
        if (!completed) {
            ARHUD_LOG_WARN("Screen %d render thread timeout, skipping frame",
                           rt.screen_id);
            rt.health.consecutive_timeouts++;
            if (rt.health.consecutive_timeouts > 3) {
                ARHUD_LOG_ERROR("Screen %d render thread unresponsive, disabling",
                                rt.screen_id);
                rt.health.state = ScreenHealth::FAULTED;
            }
        } else {
            rt.health.consecutive_timeouts = 0;
            rt.health.state = ScreenHealth::HEALTHY;
        }
        rt.frame_sync.frame_in_flight.store(false);
    }
}

// 渲染线程
void render_thread_func(ScreenID screen_id) {
    auto &sync = screen_render_threads[screen_id].frame_sync;
    while (!sync.quit.load()) {
        sync.main_to_render->wait();
        if (sync.quit.load()) break;

        render_all_windows_on_screen(screen_id);
        swap_buffers_for_screen(screen_id);

        sync.render_to_main->post();
    }
}
```

**SINGLE_THREAD 模式下无需帧同步**，直接串行执行即可。

#### 2.17.3 OpenGL 独立上下文：多线程渲染模型

**设计决策：不使用共享上下文（wglShareLists），采用独立上下文模型。**

ARHud 采用 Context/Surface 分离模型，每个独立渲染线程拥有完全独立的渲染栈
（GL Context + RDD + RD），资源不共享，各线程独立创建 GPU 资源。

**为什么不使用共享上下文？**
1. `wglShareLists` 有严格的时序要求和平台差异，容易引发隐蔽 bug
2. 共享上下文下，资源创建/销毁需要跨线程同步，增加复杂度
3. AR HUD 场景资源量有限（HUD 纹理/着色器），重复创建开销可接受
4. 独立上下文模型更简单、更安全，与 Vulkan 模型一致

```cpp
// Phase 1: 主线程 - 平台资源创建（GetDC, SetPixelFormat, wglCreateContext）
// 无 GL 命令，安全在主线程调用
RenderingDevice *indep_rd = main_rd->CreateIndependentDevice(
    screen_id, native_window, width, height);

// Phase 2: 渲染线程 - GL 初始化（wglMakeCurrent, RDD 创建等）
// 必须在渲染线程调用
Error err = main_rd->InitializeIndependentDevice(indep_rd);

// CreateIndependentDevice 内部流程（Phase 1，主线程）：
// 1. RCD::SurfaceCreate(screen_id, native_window, w, h) → SurfaceID
// 2. RCD::ContextCreate(surf_id) → ContextID
// 3. new RenderingDevice → 创建新 RD
// 注意：不调用任何 GL 命令，不创建 RDD

// InitializeIndependentDevice 内部流程（Phase 2，渲染线程）：
// 1. IGLManager::MakeCurrent(ctx_id, surf_id) → 绑定上下文到渲染线程
// 2. RD::Initialize(RCD) → 内部创建 RDD：
//    a. RCD::CreateDeviceDriver() → 创建新 RDD
//    b. RDD::Initialize() → 初始化新 RDD（GL 状态初始化）
//    c. 创建 Staging Buffer、CommandBuffer 池等

// 独立渲染线程使用模式
void render_thread_func(RenderingDevice *indep_rd,
                         IGLManager::ContextID ctx_id,
                         IRenderingContextDriver::SurfaceID surf_id) {
    // 绑定独立上下文到当前线程
    indep_rd->GetGLManager()->MakeCurrent(ctx_id, surf_id);

    while (!quit) {
        // 帧循环（与主线程完全相同的 DrawList API）
        DrawListID dl = indep_rd->DrawListBegin();
        indep_rd->DrawListSetViewport(dl, 0, 0, width, height);
        // ... 绘制命令 ...
        indep_rd->DrawListEnd();
        indep_rd->GetGLManager()->SwapBuffers(surf_id);
    }

    indep_rd->GetGLManager()->ReleaseCurrent();
}

// 销毁独立渲染设备（在主线程调用）
// RD::Finalize() 内部释放 RDD（RCD::DriverFree）
main_rd->DestroyIndependentDevice(indep_rd);
```

**独立上下文资源归属规则：**

| 资源 | 归属 | 跨线程共享 | 说明 |
|------|------|:---:|------|
| GPU 纹理/缓冲区 | 各线程独立 | ❌ | 各上下文独立创建 |
| 着色器程序 | 各线程独立 | ❌ | 各上下文独立编译链接 |
| VAO/FBO | 各线程独立 | ❌ | GL 3.3+ 不可共享 |
| 场景数据 (只读) | 主线程 → 渲染线程 | ✅ | FrameSync 快照 |
| 同步对象 (Fence) | 跨线程 | ✅ | `glFenceSync` 可跨上下文等待 |

**跨驱动行为差异处理：**

```cpp
struct GLDriverQuirks {
    bool amd_context_create_slow;           // AMD: 独立上下文创建较慢
    bool nvidia_concurrent_context_ok;       // NVIDIA: 多上下文并发执行良好
    bool intel_fence_sync_reliable;          // Intel: glFenceSync 是否可靠

    static GLDriverQuirks detect() {
        GLDriverQuirks q{};
        const char *vendor = (const char *)glGetString(GL_VENDOR);
        if (strstr(vendor, "ATI") || strstr(vendor, "AMD")) {
            q.amd_context_create_slow = true;
        }
        if (strstr(vendor, "NVIDIA")) {
            q.nvidia_concurrent_context_ok = true;
        }
        return q;
    }
};
```

#### 2.17.4 窗口 Resize 竞态：双缓冲尺寸

**问题：** 事件线程修改窗口尺寸时，渲染线程可能正在使用旧尺寸的 FBO/viewport，导致渲染异常。

**解决方案：帧边界尺寸切换**

```cpp
class ARHudWindowBase : public ARHudWindow {
protected:
    // 双缓冲尺寸
    struct SizeState {
        int32_t width;
        int32_t height;
        ARHudAtomic<bool> pending_resize{false};
    };

    SizeState current_size;     // 渲染线程使用
    SizeState requested_size;   // 事件线程写入

    // 事件线程调用
    void on_platform_resize(int32_t w, int32_t h) override {
        requested_size.width = w;
        requested_size.height = h;
        requested_size.pending_resize.store(true);
        // 不直接修改 current_size！
    }

public:
    // 渲染线程在帧开始时调用
    bool apply_pending_resize() {
        if (requested_size.pending_resize.load()) {
            current_size.width = requested_size.width;
            current_size.height = requested_size.height;
            requested_size.pending_resize.store(false);
            return true;  // 尺寸已变化，需要重建 FBO
        }
        return false;
    }

    int32_t get_width() const override { return current_size.width; }
    int32_t get_height() const override { return current_size.height; }
};
```

**渲染线程帧循环中的使用：**

```cpp
void render_frame_for_screen(ScreenID screen_id) {
    auto windows = display_server->get_windows_on_screen(screen_id);

    for (WindowID wid : windows) {
        auto *window = static_cast<ARHudWindowBase *>(display_server->get_window(wid));

        // 在帧边界应用 resize
        if (window->apply_pending_resize()) {
            rebuild_framebuffer_for_window(wid);
        }

        // 安全渲染：current_size 不会在本帧内变化
        render_window(wid, window->get_width(), window->get_height());
    }
}
```

#### 2.17.5 DisplayServer 职责拆分

**问题：** 当前 `ARHudDisplayServer` 是一个巨大的单例类，违反单一职责原则，导致平台实现必须一次性实现所有接口。

**解决方案：组合模式拆分**

```cpp
class IDisplayServer {
public:
    static IDisplayServer *GetSingleton();

    // 子管理器（组合关系，非继承）
    ScreenManager &get_screen_manager() { return screen_mgr; }
    WindowManager &get_window_manager() { return window_mgr; }
    ViewManager &get_view_manager() { return view_mgr; }
    InputManager &get_input_manager() { return input_mgr; }
    RenderScheduler &get_render_scheduler() { return render_sched; }

    // 便捷方法（委托给子管理器）
    uint32_t get_screen_count() const { return screen_mgr.get_count(); }
    WindowID window_create(const IWindow::CreateParams &p) {
        return window_mgr.create(p);
    }
    ViewID view_create(WindowID w, const IView::Config &c) {
        return view_mgr.create(w, c);
    }

private:
    ScreenManager screen_mgr;
    WindowManager window_mgr;
    ViewManager view_mgr;
    InputManager input_mgr;
    RenderScheduler render_sched;
};

// 屏幕管理器
class ScreenManager {
public:
    virtual uint32_t get_count() const = 0;
    virtual IScreen *get(ScreenID p_id) const = 0;
    virtual ScreenID get_primary() const = 0;
    virtual IScreen::Info get_info(ScreenID p_id) const = 0;

    // 热插拔回调
    using ScreenChangeCallback = void (*)(ScreenID, bool connected, void *);
    void set_screen_change_callback(ScreenChangeCallback p_cb, void *p_userdata);
};

// 窗口管理器
class WindowManager {
public:
    virtual WindowID create(const IWindow::CreateParams &p_params) = 0;
    virtual void destroy(WindowID p_id) = 0;
    virtual IWindow *get(WindowID p_id) const = 0;
    virtual LocalVector<WindowID> get_windows_on_screen(ScreenID p_screen) const = 0;
};

// 视图管理器
class ViewManager {
public:
    virtual ViewID create(WindowID p_window, const IView::Config &p_config) = 0;
    virtual void destroy(ViewID p_id) = 0;
    virtual IView *get(ViewID p_id) const = 0;
    virtual LocalVector<IView *> get_views_of_window(WindowID p_window) const = 0;
    virtual void sort_views_by_z_order(WindowID p_window) = 0;
};

// 输入管理器
class InputManager {
public:
    struct InputEvent {
        enum Type { KEY, TOUCH, MOUSE } type;
        WindowID window_id;
        union {
            struct { int key; bool pressed; } key;
            struct { float x, y; bool pressed; } touch;
            struct { float x, y; int buttons; } mouse;
        };
    };

    virtual void poll_events() = 0;
    uint32_t get_event_count() const;
    const InputEvent &get_event(uint32_t p_index) const;
    void clear_events();

    using InputCallback = void (*)(const InputEvent &, void *);
    void set_input_callback(InputCallback p_cb, void *p_userdata);

private:
    RingBuffer<InputEvent> event_queue;
};

// 渲染调度器
class RenderScheduler {
public:
    virtual bool start_screen_render_thread(ScreenID p_screen) = 0;
    virtual void stop_screen_render_thread(ScreenID p_screen) = 0;
    virtual bool start_window_thread(WindowID p_window) = 0;
    virtual void stop_window_thread(WindowID p_window) = 0;

    void set_thread_model(ThreadModel p_model);
    ThreadModel get_thread_model() const;

    // 帧调度
    void begin_frame();
    void submit_frame();
    void wait_frame(uint32_t timeout_ms = 100);

private:
    ThreadModel thread_model = ThreadModel::SINGLE_THREAD;
    HashMap<ScreenID, ScreenRenderThread> screen_render_threads;
    HashMap<WindowID, WindowEventThread> window_event_threads;
};
```

**平台实现只需实现需要的子管理器：**

```cpp
// Windows 平台
class ScreenManagerWin32 : public ScreenManager { ... };
class WindowManagerWin32 : public WindowManager { ... };
class InputManagerWin32 : public InputManager { ... };

class Win32DisplayServer : public IDisplayServer {
    Win32DisplayServer() {
        screen_mgr = ARHUD_NEW(ScreenManagerWin32);
        window_mgr = ARHUD_NEW(WindowManagerWin32);
        input_mgr = ARHUD_NEW(InputManagerWin32);
        view_mgr = ARHUD_NEW(ViewManager);       // 通用实现
        render_sched = ARHUD_NEW(RenderScheduler); // 通用实现
    }
};
```

#### 2.17.6 立体渲染性能优化：Instanced Stereo

**问题：** 当前立体渲染设计将所有 DrawCall × 2，对于未来 3D 叠加（ADAS 点云、车道线透视）可能导致帧时间超预算。

**解决方案：预留 Instanced Stereo 扩展点**

```cpp
// 立体渲染策略枚举
enum class StereoRenderStrategy {
    SEQUENTIAL,          // 顺序渲染：左眼→右眼（当前实现）
    INSTANCED_STEREO,    // 实例化立体：一次 DrawCall 渲染双眼
    MULTIVIEW,           // OVR_multiview：GPU 自动复制（OpenGL 扩展）
};

class ARHudStereoRenderer {
public:
    void set_strategy(StereoRenderStrategy p_strategy) {
        strategy = p_strategy;
    }

    void render(ARHudView *p_view, const ARHudStereoConfig &p_config) {
        switch (strategy) {
        case StereoRenderStrategy::SEQUENTIAL:
            render_sequential(p_view, p_config);
            break;
        case StereoRenderStrategy::INSTANCED_STEREO:
            render_instanced_stereo(p_view, p_config);
            break;
        case StereoRenderStrategy::MULTIVIEW:
            render_multiview(p_view, p_config);
            break;
        }
    }

private:
    StereoRenderStrategy strategy = StereoRenderStrategy::SEQUENTIAL;

    // Instanced Stereo 实现
    void render_instanced_stereo(ARHudView *p_view,
                                  const ARHudStereoConfig &p_config) {
        // 使用 gl_ViewportIndex + gl_InstanceID 区分左右眼
        // 顶点着色器中：
        //   if (gl_ViewportIndex == 0) gl_Position = left_proj * left_view * pos;
        //   else gl_Position = right_proj * right_view * pos;

        // 需要 GL_ARB_viewport_array 扩展
        glEnable(GL_VIEWPORT_INDEX_PROVOKING_VERTEX_NV);  // 或 AMD 变体

        glViewportIndexedf(0, 0, 0, width / 2.0f, height);
        glViewportIndexedf(1, width / 2.0f, 0, width / 2.0f, height);

        // 将左右眼 VP 矩阵打包到 UBO
        float vp_data[2 * 16];  // 2 个 Mat4
        memcpy(vp_data, p_config.left_eye_projection.elements, 64);
        memcpy(vp_data + 16, p_config.left_eye_view.elements, 64);
        memcpy(vp_data + 32, p_config.right_eye_projection.elements, 64);
        memcpy(vp_data + 48, p_config.right_eye_view.elements, 64);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(vp_data), vp_data);

        // 一次 DrawCall，instance_count = 2
        glDrawArraysInstanced(GL_TRIANGLES, 0, vertex_count, 2);
    }

    // Multiview 实现（需要 GL_OVR_multiview2 扩展）
    void render_multiview(ARHudView *p_view,
                           const ARHudStereoConfig &p_config) {
        // 创建 2-layer 纹理数组作为渲染目标
        // glFramebufferTextureMultiviewOVR(GL_FRAMEBUFFER,
        //     GL_COLOR_ATTACHMENT0, texture, 0, 0, 2);

        // 着色器中使用 gl_ViewID_OVR 区分左右眼
        // layout(num_views = 2) in;
        // if (gl_ViewID_OVR == 0) { left eye }
        // else { right eye }

        // 一次 DrawCall，GPU 自动渲染到两个 layer
        glDrawArrays(GL_TRIANGLES, 0, vertex_count);
    }
};
```

**Instanced Stereo 着色器示例：**

```glsl
// hud_stereo_instanced.vert
#version 330 core
#extension GL_ARB_viewport_array : require

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

// 双眼 VP 矩阵（UBO）
layout(std140) uniform StereoVP {
    mat4 u_left_proj;
    mat4 u_left_view;
    mat4 u_right_proj;
    mat4 u_right_view;
};

out vec2 v_uv;
out vec4 v_color;

void main() {
    mat4 proj, view;
    if (gl_InstanceID == 0) {
        proj = u_left_proj;
        view = u_left_view;
        gl_ViewportIndex = 0;
    } else {
        proj = u_right_proj;
        view = u_right_view;
        gl_ViewportIndex = 1;
    }

    vec4 world_pos = view * vec4(a_position, 0.0, 1.0);
    gl_Position = proj * world_pos;
    v_uv = a_uv;
    v_color = a_color;
}
```

#### 2.17.7 热插拔与容错设计

**问题：** 车规 HUD 场景中屏幕可能热插拔，GPU 可能 TDR，渲染线程 crash 不应导致整个引擎挂掉。

**解决方案：分层容错**

```cpp
// 屏幕健康状态
struct ScreenHealth {
    enum State {
        HEALTHY,
        DEGRADED,       // 偶尔超时
        FAULTED,        // 连续超时，已禁用
        DISCONNECTED,   // 物理断开
    };

    State state = HEALTHY;
    uint32_t consecutive_timeouts = 0;
    uint32_t total_timeouts = 0;
    uint32_t frames_since_last_ok = 0;
};

// GPU 上下文丢失检测与恢复
class ARHudGPUWatchdog {
public:
    void check_gpu_status() {
        // OpenGL: 检查上下文丢失
        GLenum err = glGetError();
        if (err == GL_CONTEXT_LOST || err == GL_INVALID_OPERATION) {
            ARHUD_LOG_ERROR("GPU context lost detected: 0x%x", err);
            on_gpu_context_lost();
        }

        // Vulkan: 检查 VkResult
        // vkDeviceWaitIdle(device);
        // 如果返回 VK_ERROR_DEVICE_LOST → on_gpu_context_lost()
    }

    void on_gpu_context_lost() {
        ARHUD_LOG_FATAL("GPU device lost! Attempting recovery...");

        // 1. 停止所有渲染线程
        for (auto &rt : render_scheduler->get_screen_threads()) {
            rt.frame_sync.quit.store(true);
            rt.frame_sync.main_to_render->post();
        }

        // 2. 等待渲染线程退出
        for (auto &rt : render_scheduler->get_screen_threads()) {
            if (rt.thread) rt.thread->wait_to_finish();
        }

        // 3. 重建 GL 上下文 / 重置 Vulkan 设备
        if (graphics_api == GraphicsAPI::OPENGL) {
            gl_context_mgr->destroy_all_contexts();
            gl_context_mgr->create_primary_context(main_window);
        } else {
            // Vulkan: vkDestroyDevice + vkCreateDevice
        }

        // 4. 重建所有 GPU 资源
        compositor->rebuild_all_resources();

        // 5. 重启渲染线程
        for (uint32_t i = 0; i < display_server->get_screen_count(); i++) {
            render_scheduler->start_screen_render_thread(i);
        }

        ARHUD_LOG_INFO("GPU recovery completed");
    }
};

// 屏幕热插拔处理
class ARHudScreenHotplugHandler {
public:
    using ScreenChangeCallback = void (*)(ScreenID, bool connected, void *);

    void set_callback(ScreenChangeCallback p_cb, void *p_userdata) {
        callback = p_cb;
        userdata = p_userdata;
    }

    // 平台层调用（如 Windows 的 WM_DEVICECHANGE）
    void on_screen_connected(ScreenID p_id) {
        ARHUD_LOG_INFO("Screen %d connected", p_id);
        if (callback) callback(p_id, true, userdata);

        // 自动启动渲染线程（如果 MULTI_THREAD 模式）
        if (render_sched->get_thread_model() == ThreadModel::MULTI_THREAD) {
            render_sched->start_screen_render_thread(p_id);
        }
    }

    void on_screen_disconnected(ScreenID p_id) {
        ARHUD_LOG_WARN("Screen %d disconnected", p_id);

        // 停止该屏幕的渲染线程
        render_sched->stop_screen_render_thread(p_id);

        // 标记该屏幕的窗口为不可见
        auto windows = display_server->get_window_manager().get_windows_on_screen(p_id);
        for (auto wid : windows) {
            display_server->get_window_manager().get(wid)->set_visible(false);
        }

        if (callback) callback(p_id, false, userdata);
    }

private:
    ScreenChangeCallback callback = nullptr;
    void *userdata = nullptr;
    ARHudRenderScheduler *render_sched = nullptr;
    ARHudDisplayServer *display_server = nullptr;
};
```

**QNX 平台特殊容错：**

```cpp
// QNX Screen API 的窗口断开检测
class ARHudScreenManagerQNX : public ARHudScreenManager {
    screen_context_t screen_ctx;

    void poll_screen_events() {
        screen_event_t event;
        while (screen_get_event(screen_ctx, event, 0) == 0) {
            int type;
            screen_get_event_property_iv(event, SCREEN_PROPERTY_TYPE, &type);

            if (type == SCREEN_EVENT_DISPLAY) {
                int display_id;
                screen_get_event_property_iv(event, SCREEN_PROPERTY_DISPLAY,
                                              &display_id);

                // 检查是连接还是断开
                screen_display_t display;
                if (screen_get_display(screen_ctx, display_id, &display) == 0) {
                    on_screen_connected(display_id);
                } else {
                    on_screen_disconnected(display_id);
                }
            }
        }
    }
};
```

#### 2.17.8 目录结构职责划分

**问题：** `platform/` 和 `display/platform/` 的职责重叠，维护混乱。

**解决方案：明确划分**

```
platform/               — 纯粹的平台 API 封装（薄封装，无业务逻辑）
├── gl_manager.h                  # IGLManager 接口（统一上下文模型）
├── display_server.h              # IDisplayServer 接口
├── screen.h                      # IScreen 接口
├── window.h                      # IWindow 接口
├── view.h                        # IView 接口
├── windows/
│   ├── gl_manager_win32.h/.cpp           # WGL 上下文创建/封装
│   ├── display_server_win32.h/.cpp       # Win32DisplayServer
│   ├── screen_win32.h/.cpp               # Win32Screen
│   ├── window_win32.h/.cpp               # Win32Window
│   └── view_win32.h/.cpp                 # Win32View
├── linux/
│   └── ...
└── qnx/
    └── ...
```

**职责边界：**

| 目录 | 职责 | 依赖方向 | 示例 |
|------|------|----------|------|
| `platform/` | 封装 OS API 调用 | display → platform | `arhud_window_win32.h` 只封装 `CreateWindowEx` |
| `platform/` | 封装 OS API 调用 + 显示管理接口 | 无外部依赖 | `gl_manager.h` 定义 IGLManager 接口 |

---

### 2.18 实现状态（2026-05 更新）

#### 已完成
- ✅ Layer 0: 平台层 (Win32) — IScreen/IWindow/IView/IDisplayServer/IGLManager + Win32 实现
- ✅ Layer 1: ContextDriver (RCD) — IRenderingContextDriver + GLRenderingContextDriver
- ✅ Layer 2: DeviceDriver (RDD) — IRenderingDeviceDriver + GLRenderingDeviceDriver (含 GLStateCache)
- ✅ Layer 3: RenderingDevice (RD) — RID 管理/延迟销毁/Staging Buffer/帧循环/CommandBuffer 池/独立设备/依赖追踪
- ✅ CommandBuffer — ICommandBuffer + GLCommandBuffer (录制/回放/ResourceTracker)
- ✅ DrawList API — DrawListBegin/End + 11 个绘制方法，CommandBuffer 作为内部实现细节
- ✅ RDD 所有权模型 — RD 内部创建/释放 RDD（对齐 Godot RenderingDevice::initialize）
- ✅ RCD::Initialize 窗口参数 — 接受 screen_id/native_window/width/height，内部创建引导 Surface/Context
- ✅ Core 库 — typedefs/os/io/template/string/math

#### 关键设计决策（与本文档早期版本的区别）
1. **统一上下文模型**：移除主从上下文区分，所有 Context 地位平等
2. **SwapChain 简化**：Vulkan 风格 SwapChain API 简化为 Context+Surface 绑定记录
3. **两阶段独立设备创建**：CreateIndependentDevice (主线程) + InitializeIndependentDevice (渲染线程)
4. **不使用 wglShareLists**：每个独立设备拥有完整独立渲染栈，资源不共享
5. **渲染线程守卫**：Initialize 中记录 render_thread_id_，关键方法断言 IsRenderThread()
6. **HDC 长期持有**：GetDC 后不调 ReleaseDC，依赖窗口销毁自动清理（对齐 Godot）
7. **GL 状态缓存**：所有 glBind* 调用统一走 GLStateCache，消除冗余 GL 调用
8. **CreateContext 复用 Surface HDC**：先 CreateSurface 再 CreateContext(surf_id)，避免跨线程 GetDC
9. **RD 拥有 RDD**：RD::Initialize 内部通过 RCD::CreateDeviceDriver() 创建 RDD，Finalize 时通过 RCD::DriverFree() 释放（对齐 Godot RenderingDevice::initialize(context)）
10. **RCD 外部拥有**：RCD 生命周期由外部（Engine/调用者）管理，RD 不拥有 RCD
11. **DrawList 是公共 API**：客户端通过 DrawListBegin/End 录制绘制命令，不直接操作 ICommandBuffer
12. **CommandBuffer 是内部细节**：CommandBufferAcquire/Release/GetDriverCmd 为 private 方法，仅 DrawList 内部使用
13. **RCD::Initialize 接受窗口参数**：内部创建引导 Surface/Context + 加载 GL 函数，消除冗余 Surface/Context 对
14. **RDG Lite 简化实现**：当前使用 ICommandBuffer 录制/回放，非 Godot 的序列化字节流 + 帧末统一编译。公共 API 与 Godot 对齐，内部实现简化
15. **RDG 完整实现时机**：Phase 7（Vulkan 后端时同步引入），触发条件详见 [phase3_plan.md](file:///d:/Work/ar_hud/skill/phase3_plan.md) §2.2

#### 未实现
- ⬜ Layer 4: 资源存储层 (ShaderStorage/TextureStorage/MeshStorage/MaterialStorage/FontStorage) — 详见 [phase3_plan.md](file:///d:/Work/ar_hud/skill/phase3_plan.md) §3
- ⬜ Layer 5: 渲染组合器 (Compositor/CanvasRenderer/HUDRenderer) — 详见 [phase3_plan.md](file:///d:/Work/ar_hud/skill/phase3_plan.md) §4
- ⬜ Layer 6: 渲染服务器 (Scene Tree/HUD Layout Engine) — 详见 [phase3_plan.md](file:///d:/Work/ar_hud/skill/phase3_plan.md) §5
- ⬜ Layer 7: 应用层 (ARHudEngine 主循环) — 详见 [phase3_plan.md](file:///d:/Work/ar_hud/skill/phase3_plan.md) §5
- ⬜ RDG 完整实现（序列化字节流 + 帧末统一编译 + 屏障推导）— 详见 [phase3_plan.md](file:///d:/Work/ar_hud/skill/phase3_plan.md) §7
- ⬜ Vulkan 后端 — 详见 [phase3_plan.md](file:///d:/Work/ar_hud/skill/phase3_plan.md) §7
- ⬜ 非 Windows 平台

---

## 三、OpenGL + Windows 实现路线图

### Phase 1：核心框架（2-3 周）

1. **基础类型系统**
   - `core/typedefs.h` — 基础类型、平台宏、Error 枚举
   - `core/math/math_funcs.h` — 数学工具函数
   - `core/template/rid.h` — RID + RIDOwner
   - `core/template/local_vector.h` — 动态数组
   - `core/template/hash_map.h` — 哈希表
   - `core/template/paged_allocator.h` — 分页内存分配器
   - `core/template/safe_refcount.h` — SafeNumeric 原子计数
   - `core/template/ring_buffer.h` — 环形缓冲区

2. **OS 抽象层**
   - `core/os/thread.h` — 线程抽象
   - `core/os/mutex.h` — 互斥锁（Mutex/MutexLock）
   - `core/os/rw_lock.h` — 读写锁
   - `core/os/semaphore.h` — 信号量
   - `core/os/spin_lock.h` — 自旋锁
   - `core/os/memory.h` — 全局内存分配器

3. **日志系统**
   - `core/io/logger.h` — Logger 接口
   - `core/io/std_logger.h` — 控制台日志
   - `core/io/composite_logger.h` — 组合日志
   - `core/io/file_logger.h` — 文件日志

4. **字符串**
   - `core/string/ustring.h` — UTF-32 字符串

5. **CMake 构建系统**
   - 顶层 CMakeLists.txt
   - GLAD 集成（OpenGL 3.3 Core + WGL 扩展）
   - 编译选项（Debug/Release）

### Phase 2：渲染驱动层（3-4 周）

6. **平台层（Win32）**
   - `platform/gl_manager.h` — IGLManager 接口（统一上下文模型）
   - `platform/display_server.h` — IDisplayServer 接口
   - `platform/screen.h` — IScreen 接口
   - `platform/window.h` — IWindow 接口
   - `platform/view.h` — IView 接口
   - `platform/windows/gl_manager_win32.h/.cpp` — Win32GLManager（WGL 实现）
   - `platform/windows/display_server_win32.h/.cpp` — Win32DisplayServer
   - `platform/windows/screen_win32.h/.cpp` — Win32Screen
   - `platform/windows/window_win32.h/.cpp` — Win32Window
   - `platform/windows/view_win32.h/.cpp` — Win32View

7. **RenderingContextDriver GL (Windows/WGL)**
   - `servers/redering/rendering_context_driver.h` — IRenderingContextDriver 接口
   - `servers/redering/rendering_device_commons.h` — 共享类型、DataFormat、强类型 ID
   - `drivers/gl/rendering_context_driver_gl.h/.cpp` — GLRenderingContextDriver
   - WGL 上下文创建（参考 [gl_manager_windows_native.cpp](file:///d:/Work/godot-4.6/platform/windows/gl_manager_windows_native.cpp)）
   - Surface 管理（HWND → HDC → HGLRC）
   - VSync 控制（wglSwapIntervalEXT）
   - 多窗口支持

8. **RenderingDeviceDriver GL**
   - `servers/redering/rendering_device_driver.h` — IRenderingDeviceDriver 接口
   - `drivers/gl/rendering_device_driver_gl.h/.cpp` — GLRenderingDeviceDriver（含 GLStateCache）
   - Buffer 管理（glGenBuffers / glBindBuffer / glBufferData）
   - Texture 管理（glGenTextures / glTexImage2D）
   - Sampler 管理（glGenSamplers, GL 3.3+）
   - Shader 编译链接（glCreateShader / glCompileShader / glLinkProgram）
   - Pipeline 状态模拟（blend/depth/raster 状态结构体）
   - Framebuffer 管理（glGenFramebuffers）
   - Uniform Set 绑定（glUniform + glBindTexture）
   - SwapChain（MakeCurrent + SwapBuffers wrapper）

9. **RenderingDevice**
   - `servers/redering/rendering_device.h/.cpp` — RenderingDevice
   - 资源生命周期管理（RID → 对象映射）
   - Staging Buffer（CPU→GPU 数据传输）
   - 帧同步（Fence + SwapChain acquire/present）
   - CommandBuffer 池管理

10. **CommandBuffer**
    - `servers/redering/command_buffer.h` — ICommandBuffer 接口、CommandType 枚举
    - `drivers/gl/command_buffer_gl.h/.cpp` — GLCommandBuffer（录制/回放）
    - `servers/redering/resource_tracker.h/.cpp` — ResourceTracker（调试资源冲突检测）

### Phase 3：资源存储层（2-3 周）

7. **TextureStorage GL**
   - 2D 纹理创建/更新/释放
   - 纹理格式转换（RGBA8/RGB565/...）
   - Mipmap 自动生成

8. **MaterialStorage GL**
   - 材质定义（Shader + Uniform 参数）
   - 材质排序与批处理

9. **MeshStorage GL**
   - 2D 网格（Quad/Line/Rect/Polygon）
   - VBO/IBO 管理
   - 顶点格式定义

10. **FontStorage GL**
    - FreeType 集成
    - SDF 字体纹理生成
    - 字形缓存

11. **ShaderStorage GL**
    - GLSL 着色器加载/编译
    - Shader 变体管理（参考 Godot ShaderGLES3 的 Version/Variant 模式）
    - Uniform 位置缓存

### Phase 4：渲染器层（2-3 周）

12. **CanvasRenderer GL**
    - 2D 画布渲染
    - 批处理（同材质合并绘制）
    - 裁剪与变换

13. **HUDRenderer GL**
    - HUD 图层管理（Z-Order 排序）
    - 文字渲染（SDF）
    - 图标渲染（纹理四边形）
    - 线条渲染（抗锯齿）
    - 矩形渲染（圆角）

14. **Compositor GL**
    - 帧流程编排
    - 后处理（畸变校正、Gamma 校正）
    - Present

### Phase 5：场景与集成（2 周）

15. **Scene Tree**
    - 节点基类
    - 2D 节点
    - HUD 元素节点

16. **Engine 主循环**
    - 初始化 → 帧循环 → 清理
    - 数据更新 → 渲染 → Present

17. **Demo 应用**
    - 仪表盘 Demo（速度/转速/油量）
    - 导航箭头 Demo
    - ADAS 车道线 Demo

---

## 四、关键设计决策与权衡

### 4.1 为什么选择三层抽象而非两层？

| 方案 | 优点 | 缺点 |
|------|------|------|
| **两层**（ContextDriver + DeviceDriver） | 简单 | Staging Buffer/资源管理需在每个驱动中重复实现 |
| **三层**（Context + Driver + Device）✅ | 资源管理逻辑只写一次；驱动只关注 API 映射 | 多一层间接调用 |

ARHud 选择三层：Device 层的 Staging Buffer、RID 管理、依赖追踪是通用逻辑，不应在 OpenGL 和 Vulkan 驱动中各写一遍。

### 4.2 OpenGL Pipeline 模拟策略

OpenGL 没有原生的 Pipeline State Object (PSO)。两种方案：

**方案 A：记录状态，在 bind_pipeline 时逐个设置 GL 状态** ✅
- 优点：接口与 Vulkan 对齐，未来迁移容易
- 缺点：每次 bind 有冗余状态设置

**方案 B：直接暴露 GL 状态设置 API**
- 优点：性能最优
- 缺点：API 不统一，无法迁移到 Vulkan

ARHud 选择方案 A，但增加状态缓存（State Cache）避免冗余 GL 调用：

```cpp
struct GLPipelineState {
    bool blend_enabled;
    GLenum blend_src, blend_dst;
    bool depth_test_enabled;
    bool depth_write_enabled;
    GLenum depth_func;
    bool cull_enabled;
    GLenum cull_mode;
    GLenum front_face;
    // ...
};

// 状态缓存：只在状态变化时调用 GL
class GLStateCache {
    GLPipelineState current;
    void apply(const GLPipelineState &desired);
};
```

### 4.3 OpenGL Uniform Set 映射

Vulkan 的 Descriptor Set 概念在 OpenGL 中没有直接对应。映射策略：

```cpp
struct GLUniformSet {
    struct Binding {
        UniformType type;
        uint32_t binding;
        GLuint buffer_id;      // UBO/SSBO
        GLuint texture_id;     // 纹理
        GLuint sampler_id;     // 采样器
        uint32_t texture_unit; // 纹理单元索引
    };
    LocalVector<Binding> bindings;
};

// bind_uniform_set 实现：
void bind_uniform_set(GLUniformSet *set, GLuint program) {
    for (auto &binding : set->bindings) {
        switch (binding.type) {
            case UNIFORM_TYPE_UNIFORM_BUFFER:
                glBindBufferBase(GL_UNIFORM_BUFFER, binding.binding, binding.buffer_id);
                break;
            case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE:
                glActiveTexture(GL_TEXTURE0 + binding.texture_unit);
                glBindTexture(GL_TEXTURE_2D, binding.texture_id);
                glBindSampler(binding.texture_unit, binding.sampler_id);
                glUniform1i(glGetUniformLocation(program, ...), binding.texture_unit);
                break;
            // ...
        }
    }
}
```

### 4.4 多线程渲染

ARHud 采用与 Godot 类似的多线程模型：

```
┌──────────────┐     CommandQueue      ┌──────────────┐
│  Main Thread │ ──────────────────→   │ Render Thread │
│  (Logic)     │                       │ (GPU)         │
│              │                       │               │
│ - 更新数据    │                       │ - 执行渲染命令 │
│ - 场景遍历    │                       │ - 提交 GPU    │
│ - 提交命令    │                       │ - Present     │
└──────────────┘                       └──────────────┘
```

OpenGL 的多线程限制：OpenGL 上下文只能在创建它的线程上使用。解决方案：
- 渲染线程独占 GL 上下文
- 主线程通过 CommandQueue 提交渲染命令
- 数据通过 Staging Buffer 传递

### 4.5 QNX 平台特殊考虑

QNX 是 POSIX 兼容的实时操作系统，用于车规级 HUD：
- 使用 OpenGL ES 3.2（非桌面 GL）
- 使用 EGL 创建上下文（Screen API）
- 需要关注实时性保证（避免 GC、避免动态内存分配在渲染路径上）
- 需要安全认证（ISO 26262）

---

## 五、性能优化策略

### 5.1 批处理（Batching）

HUD 场景的特点是大量小绘制调用。批处理策略：

1. **材质排序**：相同 Shader + 相同参数的元素合并为一个 Draw Call
2. **纹理图集**：图标/符号打包到图集，减少纹理切换
3. **动态 VBO**：将多个小 Quad 的顶点数据写入同一 VBO
4. **实例化渲染**：相同几何体不同位置（如多个图标）

### 5.2 纹理流式更新

导航地图/摄像头画面需要频繁更新纹理：
- 使用 PBO (Pixel Buffer Object) 异步上传
- 双缓冲 PBO 避免管线停滞
- 只更新变化区域（脏矩形）

### 5.3 字形缓存

SDF 字体渲染的缓存策略：
- 按需渲染字形到 SDF 图集
- LRU 淘汰策略
- 预加载常用字符（ASCII + 中文常用字）

### 5.4 帧预算

60fps = 16.67ms/帧，AR HUD 建议分配：
- 数据更新：1-2ms
- 渲染：4-6ms
- Present + 等待：8-10ms
- 预留余量：2-3ms

---

## 六、测试策略

### 6.1 驱动层测试

```cpp
// test_driver_gl.cpp
void test_buffer_create_free() {
    auto *driver = create_gl_driver();
    auto buf = driver->buffer_create(1024, BUFFER_USAGE_VERTEX, true);
    ASSERT_TRUE(buf);
    driver->buffer_free(buf);
}

void test_texture_create_update() {
    auto *driver = create_gl_driver();
    ARHudRenderingDeviceDriver::TextureFormat fmt;
    fmt.width = 256; fmt.height = 256;
    fmt.format = DATA_FORMAT_R8G8B8A8_UNORM;
    auto tex = driver->texture_create(fmt);
    ASSERT_TRUE(tex);
    uint8_t data[256*256*4] = {};
    driver->texture_update(tex, 0, data, sizeof(data));
    driver->texture_free(tex);
}
```

### 6.2 渲染器测试

```cpp
// test_canvas_renderer.cpp
void test_draw_rect() {
    // 创建红色矩形
    // 渲染到 FBO
    // 读回像素验证颜色
}

void test_draw_text() {
    // 创建 "Hello" 文字
    // 渲染到 FBO
    // 验证像素非全透明
}
```

### 6.3 性能基准

- 1000 个图标的渲染时间
- 100 个文字元素的渲染时间
- 全屏导航地图纹理更新延迟
- 单帧端到端延迟

---

## 七、扩展路线

### 7.1 Vulkan 后端（Phase 6）

- 实现 `ARHudContextDriverVulkan`（参考 [rendering_context_driver_vulkan.h](file:///d:/Work/godot-4.6/drivers/vulkan/rendering_context_driver_vulkan.h)）
- 实现 `ARHudDeviceDriverVulkan`（参考 [rendering_device_driver_vulkan.h](file:///d:/Work/godot-4.6/drivers/vulkan/rendering_device_driver_vulkan.h)）
- SPIRV 着色器编译管线
- RenderingDeviceGraph 完整实现（延迟命令提交 + 自动屏障推导）

### 7.2 Linux 平台（Phase 7）

- GLX/EGL 上下文创建
- X11/Wayland 窗口管理

### 7.3 Android 平台（Phase 8）

- EGL 上下文创建
- ANativeWindow 窗口管理
- OpenGL ES 3.2 适配

### 7.4 QNX 平台（Phase 9）

- Screen API + EGL
- OpenGL ES 3.2
- 实时性优化
- 安全认证支持

---

## 八、参考源码索引

| 模块 | Godot 源码路径 | 说明 |
|------|---------------|------|
| 上下文驱动接口 | [rendering_context_driver.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_context_driver.h) | API 实例化、Surface 管理 |
| 设备驱动接口 | [rendering_device_driver.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_device_driver.h) | GPU 资源操作抽象 |
| 渲染设备 | [rendering_device.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_device.h) | 资源管理、Staging Buffer |
| 渲染命令图 | [rendering_device_graph.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_device_graph.h) | 延迟命令提交 |
| 共享枚举 | [rendering_device_commons.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_device_commons.h) | 跨层共享类型 |
| Vulkan 驱动 | [rendering_device_driver_vulkan.h](file:///d:/Work/godot-4.6/drivers/vulkan/rendering_device_driver_vulkan.h) | Vulkan RDD 实现 |
| D3D12 驱动 | [rendering_device_driver_d3d12.h](file:///d:/Work/godot-4.6/drivers/d3d12/rendering_device_driver_d3d12.h) | D3D12 RDD 实现 |
| GLES3 光栅化器 | [rasterizer_gles3.h](file:///d:/Work/godot-4.6/drivers/gles3/rasterizer_gles3.h) | OpenGL 后端组合器 |
| GLES3 着色器 | [shader_gles3.h](file:///d:/Work/godot-4.6/drivers/gles3/shader_gles3.h) | GLSL 编译管理 |
| 渲染组合器 | [renderer_compositor.h](file:///d:/Work/godot-4.6/servers/rendering/renderer_compositor.h) | 后端统一接口 |
| RD 组合器 | [renderer_compositor_rd.h](file:///d:/Work/godot-4.6/servers/rendering/renderer_rd/renderer_compositor_rd.h) | Vulkan/D3D12 后端 |
| 场景渲染器 | [renderer_scene_render.h](file:///d:/Work/godot-4.6/servers/rendering/renderer_scene_render.h) | 3D 场景渲染接口 |
| 纹理存储 | [texture_storage.h](file:///d:/Work/godot-4.6/servers/rendering/storage/texture_storage.h) | 纹理资源接口 |
| 材质存储 | [material_storage.h](file:///d:/Work/godot-4.6/servers/rendering/storage/material_storage.h) | 材质资源接口 |
| 网格存储 | [mesh_storage.h](file:///d:/Work/godot-4.6/servers/rendering/storage/mesh_storage.h) | 网格资源接口 |
| 着色器容器 | [rendering_shader_container.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_shader_container.h) | SPIRV 着色器容器 |
| Windows GL 管理 | [gl_manager_windows_native.h](file:///d:/Work/godot-4.6/platform/windows/gl_manager_windows_native.h) | WGL 上下文管理 |
| Windows Vulkan | [rendering_context_driver_vulkan_windows.h](file:///d:/Work/godot-4.6/platform/windows/rendering_context_driver_vulkan_windows.h) | Windows Vulkan 上下文 |
| 渲染服务器 | [rendering_server.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_server.h) | 顶层渲染 API |
| 渲染方法 | [rendering_method.h](file:///d:/Work/godot-4.6/servers/rendering/rendering_method.h) | 相机/实例/场景管理 |
