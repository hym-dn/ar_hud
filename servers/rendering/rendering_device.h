/**
 * @file rendering_device.h
 * @brief 高层渲染设备 — 资源生命周期管理、Staging Buffer、帧循环
 *
 * RenderingDevice (RD) 位于三层 GPU API 抽象的顶层：
 *   @code
 *   ┌─────────────────────────────────────────────────────┐
 *   │                  RenderingDevice (RD)  ← 本文件     │  ← 设备层
 *   ├─────────────────────────────────────────────────────┤
 *   │           RenderingDeviceDriver (RDD)                │  ← 驱动层
 *   ├─────────────────────────────────────────────────────┤
 *   │          RenderingContextDriver (RCD)                │  ← 上下文层
 *   └─────────────────────────────────────────────────────┘
 *   @endcode
 *
 * @par 核心职责
 *   1. **资源生命周期管理** — 通过 RIDOwner<T> 管理 RID → GPU 资源映射，
 *      支持延迟销毁（帧结束后回收），确保 GPU 不再使用后才释放
 *   2. **Staging Buffer** — CPU→GPU 数据传输的环形缓冲区，
 *      避免每帧频繁分配小 Buffer，提升上传性能
 *   3. **帧循环管理** — BeginFrame/EndFrame 编排帧内资源分配、
 *      回收和同步，维护帧号和帧资源池
 *   4. **依赖追踪** — 资源间的依赖关系（A 使用 B 的输出），
 *      确保销毁顺序正确，防止 use-after-free
 *
 * @par 与 Godot RenderingDevice 的对比
 *   | 特性 | Godot RD | ARHud RD |
 *   |------|----------|----------|
 *   | RID 管理 | ✅ 完整 RIDOwner 体系 | ✅ 完整实现 |
 *   | Staging Buffer | ✅ 多 Block 环形 | ✅ 简化版（单 Block 足够 HUD） |
 *   | 帧循环 | ✅ Fence + SwapChain | ✅ 帧号 + 延迟销毁 |
 *   | 依赖追踪 | ✅ 双向 HashMap | ✅ 简化版 |
 *   | RDG (命令图) | ✅ 完整 DAG | ❌ 后续按需 |
 *   | SwapChain | ✅ acquire/present | ✅ RDD 契约（GL 退化：GLS 路径不用，GDI
 * 上屏） | | Fence/Semaphore | ✅ 完整同步 | ✅ RDD 预留接口（OpenGL 空实现） |
 *   | Compute Pipeline | ✅ 支持 | ❌ HUD 不需要 |
 *   | Timestamp Query | ✅ 支持 | ❌ 后续按需 |
 *
 * @par 资源生命周期
 *   @code
 *   1. 创建：RD::BufferCreate() → RDD::BufferCreate() → RIDOwner::MakeRid()
 *   2. 使用：RD 返回 RID，上层通过 RID 查找簿记结构
 *   3. 销毁请求：RD::FreeResource(RID) → 加入当前帧的待释放列表
 *   4. 延迟销毁：EndFrame() 时检查帧号，GPU 不再使用后调用 RDD::XxxFree()
 *   @endcode
 *
 * @par Staging Buffer 工作流
 *   @code
 *   1. BeginFrame() → 重置当前帧的 Staging Buffer 偏移
 *   2. BufferUpdate()/TextureUpdate() → 从 Staging Buffer 分配空间
 *   3. memcpy 数据到 Staging Buffer 的映射区域
 *   4. 通过驱动命令直录提交 GPU 端拷贝命令
 *   5. EndFrame() → 标记当前 Block 为已使用
 *   @endcode
 *
 * @par 设计参考
 *   - Godot 4.6 RenderingDevice（servers/rendering/rendering_device.h）
 *   - arch_skill.md §1.1 三层 GPU API 抽象
 *   - arch_skill.md §Phase 2 第 6 项
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-10
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDERING_DEVICE_H_
#define ARHUD_SERVERS_RENDERING_RENDERING_DEVICE_H_

#include "command_queue.h"
#include "io/logger.h"
#include "os/sync.h"
#include "os/thread.h"
#include "rendering_context_driver.h"
#include "rendering_device_commons.h"
#include "rendering_device_driver.h"
#include "template/hash_map.h"
#include "template/hash_set.h"
#include "template/local_vector.h"
#include "template/paged_allocator.h"
#include "template/rid.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// 资源簿记结构
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief Buffer 资源簿记
 *
 * 存储 Buffer 的元信息，与 RDD 层的 BufferID 配合使用。
 * RDD 层持有 GPU 端对象（GLuint），RD 层持有簿记信息。
 */
struct Buffer {
  BufferID driver_id;               ///< 驱动层 Buffer ID
  uint64 size = 0;                  ///< 缓冲区大小（字节）
  BitField<BufferUsageBits> usage;  ///< 用途标志位
  MemoryAllocationType allocation_type =
      MemoryAllocationType::kGpu;  ///< 内存分配类型（CPU 可映射 / GPU 专用）
};

/**
 * @brief Texture 资源簿记
 *
 * 存储纹理的驱动层 ID 与格式描述。格式描述（format）供
 * TextureGetFormat() 查询，以及数据上传时推导像素布局/尺寸。
 * 延迟销毁由 Frame/FreeResourcesFromFrame 的帧槽列表承担。
 */
struct Texture {
  TextureID driver_id;   ///< 驱动层 Texture ID
  TextureFormat format;  ///< 纹理格式描述
};

/**
 * @brief Sampler 资源簿记
 *
 * 保存采样器状态（state）供状态查询，driver_id 供销毁时释放。
 * 采样状态在 RDD 层展开为 GL sampler object
 * （GL 3.3 将采样状态从纹理分离，对齐 Vulkan VkSampler）。
 */
struct Sampler {
  SamplerID driver_id;  ///< 驱动层 Sampler ID
  SamplerState state;   ///< 采样器状态
};

/**
 * @brief Shader 资源簿记
 *
 * 存储 API 无关的着色器编译元数据，对齐 Godot 的
 * Shader : ShaderReflection 设计。驱动特有数据
 * （如 GL uniform locations、VkDescriptorSetLayout）
 * 由 RDD 层的 GLShaderInfo / ShaderInfo 持有，
 * 通过 driver_id 指针即 ID 访问。
 *
 * @par 数据分层：
 *   - RDD 层：API 特有数据（GL uniform locations、VkPipelineLayout）
 *   - RD 层（本结构）：API 无关的编译元数据
 *   - ShaderStorage：业务数据（name、source 缓存）
 */
struct Shader {
  ShaderID driver_id;                   ///< 驱动层 Shader ID
  LocalVector<ShaderUniform> uniforms;  ///< Uniform 反射数据
  uint32 push_constant_size = 0;        ///< Push Constant 块大小（字节）
  bool is_valid = false;                ///< 着色器是否编译成功
  bool is_compute = false;              ///< 是否为计算着色器
  BitField<ShaderStage> stage_bits;     ///< 着色器阶段位掩码
};

/**
 * @brief UniformSet 资源簿记
 */
struct UniformSet {
  UniformSetID driver_id;  ///< 驱动层 UniformSet ID
  ShaderID shader_id;      ///< 关联的着色器
  uint32 set_index = 0;    ///< Set 索引
};

/**
 * @brief VertexFormat 资源簿记
 */
struct VertexFormat {
  VertexFormatID driver_id;  ///< 驱动层 VertexFormat ID
};

/**
 * @brief RenderPass 资源簿记
 */
struct RenderPass {
  RenderPassID driver_id;  ///< 驱动层 RenderPass ID
  LocalVector<Attachment>
      attachments;  ///< 附件格式列表（用于构建 clear_values）
};

/**
 * @brief Framebuffer 资源簿记
 */
struct Framebuffer {
  FramebufferID driver_id;  ///< 驱动层 Framebuffer ID
  RID render_pass;    ///< 关联的 RenderPass RID（用于 DrawListBegin 自动查找）
  uint32 width = 0;   ///< 帧缓冲宽度（像素）
  uint32 height = 0;  ///< 帧缓冲高度（像素）
};

/**
 * @brief Pipeline 缓存键
 *
 * 将 RenderPipelineCreate 的所有参数哈希为一个 uint64 键，
 * 用于查找相同参数的已有 Pipeline，避免重复创建 GLPipelineState。
 */
struct PipelineCacheKey {
  uint64 hash = 0;  ///< 管线参数哈希值（ComputeRenderPipelineCacheKey 计算）
  bool operator==(const PipelineCacheKey& p_other) const {
    return hash == p_other.hash;
  }
  bool operator!=(const PipelineCacheKey& p_other) const {
    return hash != p_other.hash;
  }
};

struct PipelineCacheKeyHasher {
  static uint32 Hash(const PipelineCacheKey& p_key) {
    return static_cast<uint32>(p_key.hash);
  }
};

/**
 * @brief Pipeline 资源簿记
 */
struct Pipeline {
  PipelineID driver_id;        ///< 驱动层 Pipeline ID
  RID shader_rid;              ///< 关联的 Shader RID
  PipelineCacheKey cache_key;  ///< 缓存键，用于 Pipeline 去重
  bool is_compute = false;     ///< 是否为计算管线
};

// ═══════════════════════════════════════════════════════════════════════
// 强类型 RID 定义（RD 层使用 RID 系统）
// ═══════════════════════════════════════════════════════════════════════

DEFINE_ID(RDBuffer);
DEFINE_ID(RDTexture);
DEFINE_ID(RDSampler);
DEFINE_ID(RDShader);
DEFINE_ID(RDUniformSet);
DEFINE_ID(RDVertexFormat);
DEFINE_ID(RDRenderPass);
DEFINE_ID(RDFramebuffer);
DEFINE_ID(RDPipeline);

// ═══════════════════════════════════════════════════════════════════════
// DrawList
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief DrawList 不透明句柄
 *
 * 由 DrawListBegin() 返回，用于后续 DrawList 操作。
 * 当前实现只支持一个活跃 DrawList（kInvalidDrawListId + 0）。
 *
 * @see DrawListBegin()
 * @see DrawListEnd()
 */
using DrawListID = uint32;

static constexpr DrawListID kInvalidDrawListId = UINT32_MAX;

// ═══════════════════════════════════════════════════════════════════════
// ComputeList
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief ComputeList 不透明句柄
 *
 * 由 ComputeListBegin() 返回，用于后续 ComputeList 操作。
 * 当前实现只支持一个活跃 ComputeList（kInvalidComputeListId + 0）。
 *
 * @see ComputeListBegin()
 * @see ComputeListEnd()
 */
using ComputeListID = uint32;

static constexpr ComputeListID kInvalidComputeListId = UINT32_MAX;

// ═══════════════════════════════════════════════════════════════════════
// Staging Buffer
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief Staging Buffer 块
 *
 * CPU→GPU 数据传输的环形缓冲区块。
 * 每帧使用一块，帧结束后标记为已使用，等待 GPU 完成后回收。
 *
 * @par 工作流
 *   1. 初始化时分配一块大的 Buffer（如 4MB）
 *   2. 每帧从 offset=0 开始线性分配
 *   3. memcpy 数据到映射区域
 *   4. 分配时记录帧引用（frame_references）
 *   5. 下一帧检查旧 Block 是否可回收（GPU 已完成）
 */
struct StagingBufferBlock {
  BufferID driver_id;         ///< 驱动层 Buffer ID
  uint8* data_ptr = nullptr;  ///< mmap 指针（CPU 端）
  /**
   * @brief 引用该 Block 数据的帧号列表（升序、无重复、单调追加）
   *
   * @note 回收判定依赖帧号无符号序比较（> p_completed_frame），
   *       前提是帧号单调不减——见 RenderingDevice::frame_number_
   *       的 uint64 契约注释（严禁窄化为 uint32）。
   */
  LocalVector<uint64> frame_references;
  uint32 fill_amount = 0;     ///< 当前已填充的字节数
  uint64 size = 0;            ///< 块字节大小（延迟释放时扣减总量用）
  bool is_temporary = false;  ///< true = 档 3 动态创建的临时块（用后延迟释放）

  /**
   * @brief 检查 Block 是否可安全回收
   *
   * 当所有引用帧都已完成（帧号 <= p_completed_frame）时，
   * GPU 已完成对该 Block 数据的读取，可以安全覆写。
   *
   * @param[in] p_completed_frame 已安全完成的最大帧号
   * @return true 所有引用帧都已完成，Block 可回收
   */
  bool IsReclaimable(uint64 p_completed_frame) const {
    for (uint32 i = 0; i < frame_references.Size(); ++i) {
      if (frame_references[i] > p_completed_frame) {
        return false;
      }
    }
    return true;
  }

  /**
   * @brief 移除已完成的帧引用
   *
   * @param[in] p_completed_frame 已安全完成的最大帧号
   */
  void RemoveCompletedReferences(uint64 p_completed_frame) {
    uint32 write = 0;
    for (uint32 read = 0; read < frame_references.Size(); ++read) {
      if (frame_references[read] > p_completed_frame) {
        frame_references[write++] = frame_references[read];
      }
    }
    // Resize to remove trailing elements (no destructor for uint64)
    while (frame_references.Size() > write) {
      frame_references.PopBack();
    }
  }
};

/**
 * @brief Staging Buffer 分配结果
 *
 * 由 StagingBufferAllocate() 返回，描述一次 Staging 分配的位置信息。
 * 所有分配均来自持久映射的 Staging Block，无需手动 BufferUnmap。
 *
 * @par 使用方式
 *   @code
 *   StagingBufferAllocation alloc;
 *   StagingBufferAllocate(size, alloc);
 *   memcpy(alloc.data_ptr, data, size);
 *   device_driver_->CommandCopyBuffer(alloc.driver_id, dst_id, regions);
 *   @endcode
 */
struct StagingBufferAllocation {
  uint8* data_ptr = nullptr;  ///< CPU 端写入指针
  BufferID driver_id;         ///< 驱动层 Buffer ID
  uint64 offset = 0;          ///< 在 Buffer 中的字节偏移
};

// ═══════════════════════════════════════════════════════════════════════
// 帧资源池
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 帧资源池
 *
 * 每帧持有待释放资源列表，在帧结束时延迟销毁。
 * 延迟帧数由 frame_count_ 决定（默认 2，即双缓冲）。
 */
struct Frame {
  IRenderingDeviceDriver::FenceID fence =
      IRenderingDeviceDriver::kInvalidFenceId;  ///< 帧完成 Fence（GPU
                                                ///< 完成信号）

  LocalVector<RID> buffers_to_free;         ///< 待释放 Buffer RID 列表
  LocalVector<RID> textures_to_free;        ///< 待释放 Texture RID 列表
  LocalVector<RID> samplers_to_free;        ///< 待释放 Sampler RID 列表
  LocalVector<RID> shaders_to_free;         ///< 待释放 Shader RID 列表
  LocalVector<RID> uniform_sets_to_free;    ///< 待释放 UniformSet RID 列表
  LocalVector<RID> vertex_formats_to_free;  ///< 待释放 VertexFormat RID 列表
  LocalVector<RID> render_passes_to_free;   ///< 待释放 RenderPass RID 列表
  LocalVector<RID> framebuffers_to_free;    ///< 待释放 Framebuffer RID 列表
  LocalVector<RID> pipelines_to_free;       ///< 待释放 Pipeline RID 列表

  void Clear() {
    buffers_to_free.Clear();
    textures_to_free.Clear();
    samplers_to_free.Clear();
    shaders_to_free.Clear();
    uniform_sets_to_free.Clear();
    vertex_formats_to_free.Clear();
    render_passes_to_free.Clear();
    framebuffers_to_free.Clear();
    pipelines_to_free.Clear();
  }
};

// ═══════════════════════════════════════════════════════════════════════
// RenderingDevice
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 高层渲染设备
 *
 * 在 RDD 之上提供资源生命周期管理、Staging Buffer 和帧循环管理。
 * 是 Storage 层和 Renderer 层的唯一 GPU 资源入口。
 *
 * @par 线程安全
 *   - Initialize/Finalize 必须在拥有 GL 上下文的渲染线程上调用
 *   - 资源创建/销毁、帧循环应在渲染线程
 *   - 内部不做线程同步，依赖外部调用约定
 *
 * @par 典型使用流程
 *   @code
 *   // 初始化
 *   auto* rd = new RenderingDevice();
 *   rd->Initialize(context_driver, device_driver, 2);
 *
 *   // 资源创建
 *   RDBufferID buf = rd->BufferCreate(1024, BufferUsageBits::kVertex,
 * MemoryAllocationType::kGpu); RDTextureID tex = rd->TextureCreate(fmt, view);
 *
 *   // 帧循环
 *   while (running) {
 *       rd->BeginFrame();
 *
 *       // ... 使用资源渲染 ...
 *
 *       rd->EndFrame();
 *   }
 *
 *   // 清理
 *   rd->Finalize();
 *   @endcode
 *
 * @see IRenderingDeviceDriver   驱动层接口
 * @see IRenderingContextDriver  上下文层接口
 */
class RenderingDevice {
  ARHUD_DISABLE_COPY_MOVE(RenderingDevice);

 public:
  static constexpr uint32 kDefaultFrameCount = 2;
  static constexpr uint32 kDefaultStagingBufferSize = 4 * 1024 * 1024;

  /**
   * @brief Staging Buffer 三级分配阈值
   *
   * | 数据大小              | 分配策略                          | 对齐   |
   * |----------------------|----------------------------------|--------|
   * | ≤ kSmallUploadMax    | 小 Block 紧凑打包（减少 cache miss）| 32B   |
   * | ≤ kLargeUploadThreshold | 主 Block 线性子分配             | 256B  |
   * | > kLargeUploadThreshold | 独立临时 Buffer（避免挤占主 Block）| N/A   |
   */
  static constexpr uint32 kSmallUploadMax = 256;
  static constexpr uint32 kSmallStagingBlockSize = 64 * 1024;
  static constexpr uint32 kLargeUploadThreshold = 64 * 1024;
  static constexpr uint64 kMaxStagingTotalSize = 256 * 1024 * 1024;

  // ═══════════════════════════════════════════════════════════════════
  // 生命周期
  // ═══════════════════════════════════════════════════════════════════

  RenderingDevice() = default;
  ~RenderingDevice();

  /**
   * @brief 初始化渲染设备
   *
   * 接收 RCD 并内部创建 RDD（对齐 Godot RenderingDevice::initialize）。
   * RDD 通过 RCD::CreateDeviceDriver() 创建，RD 拥有 RDD 的所有权。
   *
   * @param[in] p_context_driver
   * 上下文驱动（不拥有所有权，由调用者管理生命周期）
   * @param[in] p_frame_count    帧缓冲数量（默认 2，双缓冲）
   * @param[in] p_staging_buffer_size Staging Buffer 每块大小（字节，默认 4MB）
   * @param[in] p_context_id     可选的 RCD Context ID，用于在 RDD 初始化前
   * MakeCurrent
   * @param[in] p_surface        可选的 RCD Surface ID，用于在 RDD 初始化前
   * MakeCurrent
   *
   * @retval Error::kOK 初始化成功
   * @retval Error::kAlreadyExists 已初始化
   * @retval Error::kFailed 初始化失败（RDD 创建/初始化失败，或 Staging Buffer
   * 创建失败）
   *
   * @pre p_context_driver 已成功初始化（IsInitialized() == true）
   * @pre 必须在拥有 GL 上下文的渲染线程上调用
   * @pre 如果提供 p_context_id 和 p_surface，GL 上下文将在此方法内 MakeCurrent
   *
   * @par 所有权模型（对齐 Godot）
   *   - RD 拥有 RDD 的所有权（通过 RCD::CreateDeviceDriver 创建）
   *   - RD 不拥有 RCD 的所有权（由外部管理，通常是 Engine 或 Compositor）
   *   - Finalize() 释放 RDD（通过 RCD::DriverFree）和所有 RID 资源
   *   - RCD 的生命周期由调用者负责
   *
   * @par 典型初始化顺序
   *   @code
   *   // 1. 外部创建 RCD
   *   GLRenderingContextDriver context_driver(&gl_manager);
   *   context_driver.Initialize(native_handle);
   *
   *   // 2. 创建 Surface + Context
   *   auto surface = context_driver.SurfaceCreate(screen_id, hwnd, w, h);
   *   auto context = context_driver.ContextCreate(surface);
   *
   *   // 3. 创建 RD 并初始化（传入 context/surface，内部 MakeCurrent）
   *   RenderingDevice rd;
   *   rd.Initialize(&context_driver, 2, 4*1024*1024, context, surface);
   *   @endcode
   *
   * @par 典型销毁顺序
   *   @code
   *   rd.Finalize();                     // 1. 释放 RID 资源 + RDD
   *   context_driver.Shutdown();          // 2. 销毁 RCD
   *   @endcode
   */
  Error Initialize(IRenderingContextDriver* p_context_driver,
                   uint32 p_frame_count = kDefaultFrameCount,
                   uint32 p_staging_buffer_size = kDefaultStagingBufferSize,
                   IRenderingContextDriver::ContextID p_context_id = 0,
                   IRenderingContextDriver::SurfaceID p_surface = 0);

  /**
   * @brief 终止渲染设备，释放所有资源
   *
   * 释放所有 RIDOwner 管理的资源，销毁 Staging Buffer。
   * 不销毁 RCD 和 RDD（由调用者管理）。
   * 调用后不应再使用任何 RD 方法。
   *
   * @pre 所有外部引用已释放
   * @pre 必须在拥有 GL 上下文的渲染线程上调用
   *
   * @par 线程安全
   *   Finalize() 内部会调用 GPU 资源释放（如 glDeleteFramebuffers），
   *   这些操作要求 GL 上下文已绑定到当前线程。
   *   调用者必须确保在正确的渲染线程上调用此方法。
   *
   * @par 典型销毁顺序
   *   @code
   *   // 1. 渲染线程：Finalize RD
   *   rd->Finalize();
   *
   *   // 2. 主线程：销毁 RDD、RCD
   *   context_driver->DriverFree(rdd);
   *   delete context_driver;
   *   @endcode
   */
  void Finalize();

  /**
   * @brief 检查是否已初始化
   * @return true 已初始化
   */
  bool IsInitialized() const {
    return initialized_;
  }

  /**
   * @brief 将当前线程注册为渲染线程
   *
   * 调用后，当前线程 ID 被记录为该 RenderingDevice 的渲染线程。
   * 同时同步到 CommandQueue，启用同线程短路优化。
   * 后续所有需要渲染线程的操作（Initialize/Finalize/资源创建等）
   * 将断言检查调用线程是否匹配。
   *
   * @par 使用场景
   *   - Initialize 之前调用，或 Initialize 自动记录
   *
   * @see IsRenderThread()
   * @see CommandQueue::SetRenderThreadId()
   */
  void MakeRenderThread() {
    render_thread_id_ = Thread::GetCallerId();
    if (command_queue_ != nullptr) {
      command_queue_->SetRenderThreadId(render_thread_id_);
    }
  }

  /**
   * @brief 检查当前线程是否为渲染线程
   *
   * @return true 当前线程是注册的渲染线程
   *
   * @see MakeRenderThread()
   */
  bool IsRenderThread() const {
    return Thread::GetCallerId() == render_thread_id_;
  }

  // ═══════════════════════════════════════════════════════════════════
  // 渲染线程断言宏
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @def ARHUD_ASSERT_ON_RENDER_THREAD
   * @brief 断言当前线程是渲染线程
   *
   * 在 Debug 构建中检查当前线程是否为注册的渲染线程，
   * 如果不是则触发断言失败。Release 构建中为空操作。
   *
   * @par 使用场景：
   *   Storage 层的 Create/Free/Update 等修改 GPU 资源的方法
   *   必须在渲染线程调用（经 RenderServer 的 RID 分裂路径路由，或直接调用）。
   *   此宏提供防御性检查，帮助在开发阶段捕获线程安全违规。
   *
   * @par 用法：
   *   @code
   *   void TextureStorage::TextureCreate(...) {
   *       ARHUD_ASSERT_ON_RENDER_THREAD(rd_);
   *       // ... 创建纹理 ...
   *   }
   *   @endcode
   *
   * @param[in] p_rd RenderingDevice 指针，用于调用 IsRenderThread()
   */
#ifndef NDEBUG
#define ARHUD_ASSERT_ON_RENDER_THREAD(p_rd)                   \
  ARHUD_ASSERT((p_rd) != nullptr && (p_rd)->IsRenderThread(), \
               "This method must be called on the render thread")
#else
#define ARHUD_ASSERT_ON_RENDER_THREAD(p_rd) ((void)0)
#endif

  // ═══════════════════════════════════════════════════════════════════
  // 帧循环
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 开始新的一帧
   *
   * 递增帧号，等待并回收 frame_count_ 帧前槽位的延迟销毁资源
   * （Fence 确认 GPU 完成后才释放），重置 Staging Buffer 偏移。
   *
   * @pre IsInitialized() == true
   */
  void BeginFrame();

  /**
   * @brief 结束当前帧
   *
   * 结束命令录制（关闭活跃的 DrawList/ComputeList），插入本帧 GPU Fence。
   * 延迟销毁回收移至下一 BeginFrame（等待该 Fence 时执行）。
   *
   * @pre BeginFrame() 已调用
   */
  void EndFrame();

  /**
   * @brief 获取当前帧号
   * @return 帧号（从 0 开始递增）
   */
  uint64 GetFrameNumber() const {
    return frame_number_;
  }

  // ═══════════════════════════════════════════════════════════════════
  // Buffer 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建 Buffer 资源
   *
   * @param[in] p_size            缓冲区大小（字节）
   * @param[in] p_usage           用途标志位
   * @param[in] p_allocation_type 内存分配类型
   *
   * @return Buffer RID，失败返回空 RID
   */
  RDBufferID BufferCreate(uint64 p_size, BitField<BufferUsageBits> p_usage,
                          MemoryAllocationType p_allocation_type);

  /**
   * @brief 释放 Buffer 资源（延迟销毁）
   *
   * 资源不会立即释放，而是在帧结束时加入待释放列表，
   * 等待 GPU 不再使用后才真正销毁。
   *
   * @param[in] p_buffer Buffer RID
   */
  void BufferFree(RDBufferID p_buffer);

  /**
   * @brief 获取 Buffer 的驱动层 ID
   *
   * @param[in] p_buffer Buffer RID
   *
   * @return 驱动层 BufferID，无效 RID 返回空 ID
   */
  BufferID BufferGetDriverId(RDBufferID p_buffer) const;

  /**
   * @brief 获取 Buffer 的大小
   *
   * @param[in] p_buffer Buffer RID
   *
   * @return 缓冲区大小（字节），无效 RID 返回 0
   */
  uint64 BufferGetSize(RDBufferID p_buffer) const;

  /**
   * @brief 映射 Buffer 到 CPU 内存
   *
   * @param[in] p_buffer Buffer RID
   *
   * @return CPU 内存指针，失败返回 nullptr
   */
  uint8* BufferMap(RDBufferID p_buffer);

  /**
   * @brief 取消映射 Buffer
   *
   * @param[in] p_buffer Buffer RID
   */
  void BufferUnmap(RDBufferID p_buffer);

  /**
   * @brief 通过 Staging Buffer 更新 GPU Buffer 数据
   *
   * 将 CPU 数据通过 Staging Buffer 传输到 GPU Buffer。
   * 内部使用多级分配策略：
   *   - 小数据（≤ Staging 剩余空间）：子分配当前帧的 Staging Block
   *   - 大数据（> Staging 剩余空间）：创建独立临时 Buffer
   *
   * @param[in] p_buffer 目标 Buffer RID
   * @param[in] p_offset 目标 Buffer 中的字节偏移
   * @param[in] p_size   数据大小（字节）
   * @param[in] p_data   CPU 端数据指针
   *
   * @retval Error::kOK 更新成功
   * @retval Error::kFailed Staging 分配失败或目标 Buffer 无效
   *
   * @pre Initialize() 已调用
   * @pre p_buffer 为有效的 GPU 侧 Buffer（非 kCpu 分配）
   *
   * @par 典型使用
   *   @code
   *   float vertices[] = { ... };
   *   rd.BufferUpdate(vbo_rid, 0, sizeof(vertices), vertices);
   *   @endcode
   */
  Error BufferUpdate(RDBufferID p_buffer, uint32 p_offset, uint32 p_size,
                     const void* p_data);

  // ═══════════════════════════════════════════════════════════════════
  // Texture 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建 Texture 资源
   *
   * @param[in] p_format 纹理格式描述
   * @param[in] p_view   纹理视图描述
   *
   * @return Texture RID，失败返回空 RID
   */
  RDTextureID TextureCreate(const TextureFormat& p_format,
                            const TextureView& p_view);

  /**
   * @brief 释放 Texture 资源（延迟销毁）
   *
   * @param[in] p_texture Texture RID
   */
  void TextureFree(RDTextureID p_texture);

  /**
   * @brief 以显示侧显存导入 Texture（Mode X 零拷贝上屏，不分配存储）
   *
   * @par 与 TextureCreate 的差别
   *   显存归 platform::ISharedFramePool 拥有，本方法只建立后端别名，
   *   因此必须用 TextureFreeImmediate 释放（帧槽延迟释放对"外部资源"
   *   不安全：平台侧可能在别名仍存在时重建该显存，例如 resize）。
   *   统计口径上**仍计入 vram_usage_**——显存确实被本后端使用，
   *   只是所有权不在 RD。
   *
   * @param[in] p_source 外部资源描述（须 IsValid()）
   *
   * @return Texture RID，可作 FBO 颜色附件；不支持或失败返回空 RID
   *
   * @pre 调用线程持有 current GL 上下文（初始化期主线程或渲染线程）
   */
  RDTextureID TextureCreateExternal(const ExternalTextureSource& p_source);

  /**
   * @brief 立即释放 Texture（跳过帧槽延迟）
   *
   * @par 存在理由
   *   外部导入纹理的后端别名必须在平台侧归还显存之前解除，而默认
   *   TextureFree 要等 frame_count 帧才真正落到驱动。本方法仅在
   *   "该资源无在途 GPU 引用"时可用（共享显存路径由
   *   FrameImageQueue::HasInFlight == false 保证）。
   *
   * @param[in] p_texture Texture RID（空 RID 静默忽略）
   */
  void TextureFreeImmediate(RDTextureID p_texture);

  /**
   * @brief 开始写外部导入纹理（跨 API 锁定，渲染线程）
   *
   * @param[in] p_texture TextureCreateExternal 的产物
   * @return true 已取得绘制权
   */
  bool TextureExternalBeginWrite(RDTextureID p_texture);

  /**
   * @brief 结束写外部导入纹理（交还显示侧，渲染线程）
   *
   * @param[in] p_texture TextureCreateExternal 的产物
   */
  void TextureExternalEndWrite(RDTextureID p_texture);

  /**
   * @brief 后端是否已确认可导入外部显存
   *
   * @return true 探测成功；false 尚未探测或不可用（以
   *         TextureCreateExternal 的实际结果为准）
   */
  bool SupportsExternalTexture() const;

  /**
   * @brief 获取 Texture 的驱动层 ID
   *
   * @param[in] p_texture Texture RID
   *
   * @return 驱动层 TextureID，无效 RID 返回空 ID
   */
  TextureID TextureGetDriverId(RDTextureID p_texture) const;

  /**
   * @brief 获取 Texture 的格式描述
   *
   * @param[in] p_texture Texture RID
   *
   * @return 纹理格式描述的常引用，无效 RID 行为未定义
   */
  const TextureFormat& TextureGetFormat(RDTextureID p_texture) const;

  /**
   * @brief 通过 Staging Buffer 更新 GPU Texture 数据
   *
   * 将 CPU 数据通过 Staging Buffer 传输到 GPU Texture。
   * 内部使用 Buffer→Texture 拷贝命令（OpenGL: PBO 方式）。
   *
   * @param[in] p_texture  目标 Texture RID
   * @param[in] p_layer    目标数组层（默认 0）
   * @param[in] p_mipmap   目标 Mipmap 层级（默认 0）
   * @param[in] p_data     CPU 端数据指针
   * @param[in] p_data_size 数据大小（字节）
   *
   * @retval Error::kOK 更新成功
   * @retval Error::kFailed Staging 分配失败或目标 Texture 无效
   *
   * @pre Initialize() 已调用
   * @pre p_texture 为有效的 GPU 侧 Texture
   *
   * @par 典型使用
   *   @code
   *   uint8 pixels[256*256*4] = { ... };
   *   rd.TextureUpdate(tex_rid, 0, 0, pixels, sizeof(pixels));
   *   @endcode
   */
  Error TextureUpdate(RDTextureID p_texture, uint32 p_layer, uint32 p_mipmap,
                      const void* p_data, uint32 p_data_size);

  /**
   * @brief 生成纹理 Mipmap 链
   *
   * 从基础层级自动生成完整 Mipmap 链。
   * 纹理创建时必须指定 mip_levels > 1。
   *
   * @param[in] p_texture 目标 Texture RID
   *
   * @retval Error::kOK 生成成功
   * @retval Error::kFailed 生成失败
   */
  Error TextureGenerateMipmaps(RDTextureID p_texture);

  // ═══════════════════════════════════════════════════════════════════
  // Sampler 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建 Sampler 资源
   *
   * @param[in] p_state 采样器状态描述
   *
   * @return Sampler RID，失败返回空 RID
   */
  RDSamplerID SamplerCreate(const SamplerState& p_state);

  /**
   * @brief 释放 Sampler 资源（延迟销毁）
   *
   * @param[in] p_sampler Sampler RID
   */
  void SamplerFree(RDSamplerID p_sampler);

  /**
   * @brief 获取 Sampler 的驱动层 ID
   *
   * @param[in] p_sampler Sampler RID
   *
   * @return 驱动层 SamplerID，无效 RID 返回空 ID
   */
  SamplerID SamplerGetDriverId(RDSamplerID p_sampler) const;

  // ═══════════════════════════════════════════════════════════════════
  // Shader 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 从源码创建 Shader 资源
   *
   * @param[in] p_stage_sources   着色器阶段源码数组
   * @param[in] p_source_format   源码格式（OpenGL 仅支持 kGLSL）
   * @param[in] p_uniforms        Uniform 描述数组
   * @param[in] p_push_constant_size Push Constant 大小（字节）
   *
   * @return Shader RID，失败返回空 RID
   */
  RDShaderID ShaderCreateFromSource(
      VectorView<ShaderStageSource> p_stage_sources,
      ShaderSourceFormat p_source_format, VectorView<ShaderUniform> p_uniforms,
      uint32 p_push_constant_size);

  /**
   * @brief 释放 Shader 资源（延迟销毁）
   *
   * @param[in] p_shader Shader RID
   */
  void ShaderFree(RDShaderID p_shader);

  /**
   * @brief 获取 Shader 的驱动层 ID
   *
   * @param[in] p_shader Shader RID
   *
   * @return 驱动层 ShaderID，无效 RID 返回空 ID
   */
  ShaderID ShaderGetDriverId(RDShaderID p_shader) const;

  /**
   * @brief 获取 Shader 的 Uniform 反射数据
   *
   * @param[in] p_shader Shader RID
   *
   * @return Uniform 列表的常量引用，无效 RID 返回空列表
   */
  const LocalVector<ShaderUniform>& ShaderGetUniforms(
      RDShaderID p_shader) const;

  /**
   * @brief 获取 Shader 的 Push Constant 块大小
   *
   * @param[in] p_shader Shader RID
   *
   * @return Push Constant 大小（字节），无效 RID 返回 0
   */
  uint32 ShaderGetPushConstantSize(RDShaderID p_shader) const;

  /**
   * @brief 检查 Shader 是否有效（编译成功）
   *
   * @param[in] p_shader Shader RID
   *
   * @return 编译成功返回 true，否则返回 false
   */
  bool ShaderIsValid(RDShaderID p_shader) const;

  /**
   * @brief 检查 Shader 是否为计算着色器
   *
   * @param[in] p_shader Shader RID
   *
   * @return 计算着色器返回 true，否则返回 false
   */
  bool ShaderIsCompute(RDShaderID p_shader) const;

  // ═══════════════════════════════════════════════════════════════════
  // UniformSet 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建 UniformSet 资源
   *
   * @param[in] p_uniforms  绑定 Uniform 数组
   * @param[in] p_shader    关联的 Shader RID
   * @param[in] p_set_index Set 索引
   *
   * @return UniformSet RID，失败返回空 RID
   */
  RDUniformSetID UniformSetCreate(VectorView<BoundUniform> p_uniforms,
                                  RDShaderID p_shader, uint32 p_set_index);

  /**
   * @brief 释放 UniformSet 资源（延迟销毁）
   *
   * @param[in] p_uniform_set UniformSet RID
   */
  void UniformSetFree(RDUniformSetID p_uniform_set);

  /**
   * @brief 获取 UniformSet 的驱动层 ID
   *
   * @param[in] p_uniform_set UniformSet RID
   *
   * @return 驱动层 UniformSetID，无效 RID 返回空 ID
   */
  UniformSetID UniformSetGetDriverId(RDUniformSetID p_uniform_set) const;

  // ═══════════════════════════════════════════════════════════════════
  // VertexFormat 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建 VertexFormat 资源
   *
   * @param[in] p_vertex_attribs 顶点属性数组
   *
   * @return VertexFormat RID，失败返回空 RID
   */
  RDVertexFormatID VertexFormatCreate(
      VectorView<VertexAttribute> p_vertex_attribs);

  /**
   * @brief 释放 VertexFormat 资源（延迟销毁）
   *
   * @param[in] p_vertex_format VertexFormat RID
   */
  void VertexFormatFree(RDVertexFormatID p_vertex_format);

  /**
   * @brief 获取 VertexFormat 的驱动层 ID
   *
   * @param[in] p_vertex_format VertexFormat RID
   *
   * @return 驱动层 VertexFormatID，无效 RID 返回空 ID
   */
  VertexFormatID VertexFormatGetDriverId(
      RDVertexFormatID p_vertex_format) const;

  // ═══════════════════════════════════════════════════════════════════
  // RenderPass 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建 RenderPass 资源
   *
   * @param[in] p_attachments       附件描述数组
   * @param[in] p_subpasses         子 Pass 描述数组
   * @param[in] p_subpass_dependencies 子 Pass 依赖数组
   *
   * @return RenderPass RID，失败返回空 RID
   */
  RDRenderPassID RenderPassCreate(
      VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses,
      VectorView<SubpassDependency> p_subpass_dependencies);

  /**
   * @brief 释放 RenderPass 资源（延迟销毁）
   *
   * @param[in] p_render_pass RenderPass RID
   */
  void RenderPassFree(RDRenderPassID p_render_pass);

  /**
   * @brief 获取 RenderPass 的驱动层 ID
   *
   * @param[in] p_render_pass RenderPass RID
   *
   * @return 驱动层 RenderPassID，无效 RID 返回空 ID
   */
  RenderPassID RenderPassGetDriverId(RDRenderPassID p_render_pass) const;

  // ═══════════════════════════════════════════════════════════════════
  // Framebuffer 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建 Framebuffer 资源
   *
   * @param[in] p_render_pass RenderPass RID
   * @param[in] p_attachments 附件 Texture RID 数组
   * @param[in] p_width       帧缓冲宽度
   * @param[in] p_height      帧缓冲高度
   *
   * @return Framebuffer RID，失败返回空 RID
   */
  RDFramebufferID FramebufferCreate(RDRenderPassID p_render_pass,
                                    VectorView<RDTextureID> p_attachments,
                                    uint32 p_width, uint32 p_height);

  /**
   * @brief 释放 Framebuffer 资源（延迟销毁）
   *
   * @param[in] p_framebuffer Framebuffer RID
   */
  void FramebufferFree(RDFramebufferID p_framebuffer);

  /**
   * @brief 获取 Framebuffer 的驱动层 ID
   *
   * @param[in] p_framebuffer Framebuffer RID
   *
   * @return 驱动层 FramebufferID，无效 RID 返回空 ID
   */
  FramebufferID FramebufferGetDriverId(RDFramebufferID p_framebuffer) const;

  /**
   * @brief 回读 Framebuffer 像素到 CPU 缓冲（B3-M0.3）
   *
   * 将指定区域从 GPU 帧缓冲读出到 CPU 内存（GL: glReadPixels）。
   * 用于「渲染/呈现分离」链路：渲染线程渲染到离屏 FBO 后，
   * 读回像素交给呈现线程用平台 API（GDI）上屏。
   *
   * @param[in] p_framebuffer Framebuffer RID（必须为可读附件）
   * @param[in] p_x 读取区域左上角 X（像素）
   * @param[in] p_y 读取区域左上角 Y（像素）
   * @param[in] p_width 读取区域宽度（像素）
   * @param[in] p_height 读取区域高度（像素）
   * @param[out] p_dst 目标 CPU 缓冲（大小须 ≥ 4 * width * height）
   *
   * @note 阻塞式读取（glReadPixels 同步等待 GPU 完成）
   * @see RenderViewport
   */
  void FramebufferReadback(RDFramebufferID p_framebuffer, uint32 p_x,
                           uint32 p_y, uint32 p_width, uint32 p_height,
                           void* p_dst);

  // ═══════════════════════════════════════════════════════════════════
  // Pipeline 管理
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 创建渲染 Pipeline 资源
   *
   * @param[in] p_shader               Shader RID
   * @param[in] p_vertex_format        VertexFormat RID
   * @param[in] p_render_primitive     渲染图元
   * @param[in] p_rasterization_state  光栅化状态
   * @param[in] p_multisample_state    多重采样状态
   * @param[in] p_depth_stencil_state  深度/模板状态
   * @param[in] p_blend_state          颜色混合状态
   * @param[in] p_dynamic_state        动态状态标志位
   * @param[in] p_render_pass          RenderPass RID
   *
   * @return Pipeline RID，失败返回空 RID
   */
  RDPipelineID RenderPipelineCreate(
      RDShaderID p_shader, RDVertexFormatID p_vertex_format,
      RenderPrimitive p_render_primitive,
      const PipelineRasterizationState& p_rasterization_state,
      const PipelineMultisampleState& p_multisample_state,
      const PipelineDepthStencilState& p_depth_stencil_state,
      const PipelineColorBlendState& p_blend_state,
      BitField<PipelineDynamicStateFlags> p_dynamic_state,
      RDRenderPassID p_render_pass);

  /**
   * @brief 创建计算 Pipeline 资源
   *
   * @param[in] p_shader 计算着色器 RID（必须包含 kCompute 阶段）
   *
   * @return Pipeline RID，失败返回空 RID
   *
   * @note 需要 OpenGL 4.3+ 支持
   */
  RDPipelineID ComputePipelineCreate(RDShaderID p_shader);

  /**
   * @brief 释放 Pipeline 资源（延迟销毁）
   *
   * @param[in] p_pipeline Pipeline RID
   */
  void PipelineFree(RDPipelineID p_pipeline);

  /**
   * @brief 获取 Pipeline 的驱动层 ID
   *
   * @param[in] p_pipeline Pipeline RID
   *
   * @return 驱动层 PipelineID，无效 RID 返回空 ID
   */
  PipelineID PipelineGetDriverId(RDPipelineID p_pipeline) const;

  // ═══════════════════════════════════════════════════════════════════
  // DrawList（高层绘制 API，对齐 Godot RenderingDevice::draw_list_xxx）
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 开始一个绘制列表
   *
   * 内部激活帧命令录制上下文（命令直录，对齐 Godot draw_list_begin）。
   * 返回的 DrawListID 用于后续 DrawList 操作。
   *
   * @return DrawListID 绘制列表句柄，失败返回 kInvalidDrawListId
   *
   * @pre IsInitialized() == true
   * @pre 当前没有活跃的 DrawList（同一时间只允许一个）
   *
   * @par 典型使用
   *   @code
   *   DrawListID dl = rd.DrawListBegin();
   *   rd.DrawListSetViewport(dl, 0, 0, width, height);
   *   rd.DrawListSetScissor(dl, 0, 0, width, height);
   *   rd.DrawListBindRenderPipeline(dl, pipeline_rid);
   *   rd.DrawListDraw(dl, vertex_count, 1);
   *   rd.DrawListEnd();
   *   @endcode
   *
   * @par 与 Godot 的对应
   *   Godot: draw_list_begin(p_framebuffer, p_draw_flags, p_clear_colors, ...)
   *   ARHud:  DrawListBegin() — 直接渲染到屏幕（使用默认 Framebuffer）
   *           DrawListBegin(p_framebuffer) — 渲染到指定 Framebuffer
   */
  DrawListID DrawListBegin();

  /**
   * @brief 开始一个绘制列表（绑定指定 Framebuffer）
   *
   * 与 DrawListBegin() 类似，但绑定指定的 Framebuffer。
   * 内部在激活命令录制上下文后调用 BeginRenderPass
   * 绑定 FBO 并设置渲染区域。
   *
   * @param[in] p_framebuffer Framebuffer RID（由 FramebufferCreate() 创建，
   *                           或 GetDefaultFramebuffer() 获取默认 FBO）
   * @param[in] p_clear_colors 是否清除颜色附件
   * @param[in] p_clear_depth 是否清除深度附件
   * @param[in] p_clear_stencil 是否清除模板附件
   * @param[in] p_clear_color  清除颜色值（RGBA，仅 p_clear_colors 时有效）
   * @param[in] p_clear_depth_val 清除深度值（仅 p_clear_depth 时有效）
   * @param[in] p_clear_stencil_val 清除模板值（仅 p_clear_stencil 时有效）
   *
   * @return DrawListID 绘制列表句柄，失败返回 kInvalidDrawListId
   *
   * @pre IsInitialized() == true
   * @pre 当前没有活跃的 DrawList
   * @pre p_framebuffer 有效
   *
   * @par 与 Godot 的对应
   *   Godot: draw_list_begin(p_framebuffer, p_draw_flags, p_clear_colors, ...)
   *   ARHud:  DrawListBegin(p_framebuffer, clear_flags, clear_values)
   */
  DrawListID DrawListBegin(RDFramebufferID p_framebuffer,
                           bool p_clear_colors = false,
                           bool p_clear_depth = false,
                           bool p_clear_stencil = false,
                           const float p_clear_color[4] = nullptr,
                           float p_clear_depth_val = 1.0f,
                           uint32 p_clear_stencil_val = 0);

  /**
   * @brief 清除 Framebuffer（不开始绘制列表）
   *
   * 仅清除指定 Framebuffer 的附件，不进入绘制模式。
   * 内部使用 BeginRenderPass + EndRenderPass 实现。
   *
   * @param[in] p_framebuffer Framebuffer RID
   * @param[in] p_clear_colors 是否清除颜色附件
   * @param[in] p_clear_depth 是否清除深度附件
   * @param[in] p_clear_stencil 是否清除模板附件
   * @param[in] p_clear_color  清除颜色值（RGBA）
   * @param[in] p_clear_depth_val 清除深度值
   * @param[in] p_clear_stencil_val 清除模板值
   *
   * @pre IsInitialized() == true
   * @pre 当前没有活跃的 DrawList
   */
  void DrawListClear(RDFramebufferID p_framebuffer, bool p_clear_colors = true,
                     bool p_clear_depth = true, bool p_clear_stencil = false,
                     const float p_clear_color[4] = nullptr,
                     float p_clear_depth_val = 1.0f,
                     uint32 p_clear_stencil_val = 0);

  /**
   * @brief 设置绘制列表的视口
   *
   * @param[in] p_list  DrawListBegin() 返回的句柄
   * @param[in] p_x     视口左下角 X（像素）
   * @param[in] p_y     视口左下角 Y（像素）
   * @param[in] p_width 视口宽度（像素）
   * @param[in] p_height 视口高度（像素）
   *
   * @pre p_list 有效且有活跃的 DrawList
   *
   * @par OpenGL 等效
   *   glViewport(x, y, width, height)
   */
  void DrawListSetViewport(DrawListID p_list, int32 p_x, int32 p_y,
                           uint32 p_width, uint32 p_height);

  /**
   * @brief 设置绘制列表的裁剪矩形
   *
   * @param[in] p_list   DrawListBegin() 返回的句柄
   * @param[in] p_x      裁剪矩形左下角 X（像素）
   * @param[in] p_y      裁剪矩形左下角 Y（像素）
   * @param[in] p_width  裁剪矩形宽度（像素）
   * @param[in] p_height 裁剪矩形高度（像素）
   *
   * @pre p_list 有效且有活跃的 DrawList
   *
   * @par OpenGL 等效
   *   glScissor(x, y, width, height)
   */
  void DrawListSetScissor(DrawListID p_list, int32 p_x, int32 p_y,
                          uint32 p_width, uint32 p_height);

  /**
   * @brief 设置绘制列表的混合常量颜色
   *
   * @param[in] p_list DrawListBegin() 返回的句柄
   * @param[in] p_r    R 通道 [0, 1]
   * @param[in] p_g    G 通道 [0, 1]
   * @param[in] p_b    B 通道 [0, 1]
   * @param[in] p_a    A 通道 [0, 1]
   *
   * @pre p_list 有效且有活跃的 DrawList
   *
   * @par OpenGL 等效
   *   glBlendColor(r, g, b, a)
   */
  void DrawListSetBlendConstants(DrawListID p_list, float p_r, float p_g,
                                 float p_b, float p_a);

  /**
   * @brief 绑定渲染管线到绘制列表
   *
   * @param[in] p_list      DrawListBegin() 返回的句柄
   * @param[in] p_pipeline  渲染管线 RID
   *
   * @pre p_list 有效且有活跃的 DrawList
   * @pre p_pipeline 有效（由 RenderPipelineCreate() 创建）
   *
   * @par OpenGL 等效
   *   glUseProgram(program); // + PSO 状态应用
   */
  void DrawListBindRenderPipeline(DrawListID p_list, RDPipelineID p_pipeline);

  /**
   * @brief 绑定 Uniform Set 到绘制列表
   *
   * @param[in] p_list        DrawListBegin() 返回的句柄
   * @param[in] p_uniform_set Uniform Set RID
   * @param[in] p_set_index   Set 索引
   *
   * @pre p_list 有效且有活跃的 DrawList
   */
  void DrawListBindUniformSet(DrawListID p_list, RDUniformSetID p_uniform_set,
                              uint32 p_set_index);

  /**
   * @brief 绑定 Uniform Set 到绘制列表（带动态偏移）
   *
   * 与 DrawListBindUniformSet 类似，但支持动态偏移。
   * 用于 kUniformBufferDynamic 类型的 UBO，在绑定时指定偏移量。
   * 不会跳过冗余绑定（因为同一 UniformSet 可能以不同偏移多次绑定）。
   *
   * @param[in] p_list           DrawListBegin() 返回的句柄
   * @param[in] p_uniform_set    Uniform Set RID
   * @param[in] p_set_index      Set 索引
   * @param[in] p_dynamic_offset 动态偏移（字节，必须对齐到 UBO offset
   * alignment）
   *
   * @pre p_list 有效且有活跃的 DrawList
   */
  void DrawListBindUniformSetWithDynamicOffset(DrawListID p_list,
                                               RDUniformSetID p_uniform_set,
                                               uint32 p_set_index,
                                               uint32 p_dynamic_offset);

  /**
   * @brief 绑定顶点缓冲区数组到绘制列表
   *
   * @param[in] p_list     DrawListBegin() 返回的句柄
   * @param[in] p_buffers  顶点缓冲区 RID 数组
   * @param[in] p_offsets  每个缓冲区的字节偏移数组
   * @param[in] p_count    缓冲区数量
   *
   * @pre p_list 有效且有活跃的 DrawList
   */
  void DrawListBindVertexBuffers(DrawListID p_list, const RDBufferID* p_buffers,
                                 const uint64* p_offsets, uint32 p_count);

  /**
   * @brief 绑定索引缓冲区到绘制列表
   *
   * @param[in] p_list    DrawListBegin() 返回的句柄
   * @param[in] p_buffer  索引缓冲区 RID
   * @param[in] p_format  索引格式
   * @param[in] p_offset  字节偏移
   *
   * @pre p_list 有效且有活跃的 DrawList
   */
  void DrawListBindIndexBuffer(DrawListID p_list, RDBufferID p_buffer,
                               IndexBufferFormat p_format, uint64 p_offset);

  /**
   * @brief 非索引绘制
   *
   * @param[in] p_list           DrawListBegin() 返回的句柄
   * @param[in] p_vertex_count   顶点数量
   * @param[in] p_instance_count 实例化数量（默认 1）
   *
   * @pre p_list 有效且有活跃的 DrawList
   * @pre 已绑定有效的管线和顶点缓冲区
   */
  void DrawListDraw(DrawListID p_list, uint32 p_vertex_count,
                    uint32 p_instance_count = 1);

  /**
   * @brief 索引绘制
   *
   * @param[in] p_list           DrawListBegin() 返回的句柄
   * @param[in] p_index_count    索引数量
   * @param[in] p_instance_count 实例化数量（默认 1）
   *
   * @pre p_list 有效且有活跃的 DrawList
   * @pre 已绑定有效的管线、顶点缓冲区和索引缓冲区
   */
  void DrawListDrawIndexed(DrawListID p_list, uint32 p_index_count,
                           uint32 p_instance_count = 1);

  /**
   * @brief 非索引实例化绘制
   *
   * 使用已绑定的实例缓冲区进行实例化绘制。
   * 顶点数量从绑定的顶点缓冲区元数据中推导。
   *
   * @param[in] p_list           DrawListBegin() 返回的句柄
   * @param[in] p_instance_count 实例化数量
   * @param[in] p_base_instance  起始实例偏移（默认 0）
   *
   * @pre p_list 有效且有活跃的 DrawList
   * @pre 已绑定有效的管线、顶点缓冲区（含实例缓冲区）
   *
   * @par OpenGL 等效操作
   *   glDrawArraysInstanced(mode, first, count, instance_count)
   */
  void DrawListDrawInstanced(DrawListID p_list, uint32 p_instance_count,
                             uint32 p_base_instance = 0);

  /**
   * @brief 索引实例化绘制
   *
   * 使用已绑定的实例缓冲区和索引缓冲区进行实例化绘制。
   * 索引数量从绑定的索引缓冲区元数据中推导。
   *
   * @param[in] p_list           DrawListBegin() 返回的句柄
   * @param[in] p_instance_count 实例化数量
   * @param[in] p_base_instance  起始实例偏移（默认 0）
   *
   * @pre p_list 有效且有活跃的 DrawList
   * @pre 已绑定有效的管线、顶点缓冲区（含实例缓冲区）和索引缓冲区
   *
   * @par OpenGL 等效操作
   *   glDrawElementsInstanced(mode, count, type, indices, instance_count)
   */
  void DrawListDrawIndexedInstanced(DrawListID p_list, uint32 p_instance_count,
                                    uint32 p_base_instance = 0);

  /**
   * @brief 结束绘制列表
   *
   * 结束命令录制（关闭 render pass）。
   *
   * @pre DrawListBegin() 已成功调用
   * @post DrawList 不再活跃，不可再调用 DrawListXxx()
   *
   * @par 与 Godot 的对应
   *   Godot: draw_list_end()
   *   ARHud:  DrawListEnd() — 内部完成 End + Execute + Release
   */
  void DrawListEnd();

  // ═══════════════════════════════════════════════════════════════════
  // ComputeList（高层计算 API，对齐 Godot RenderingDevice::compute_list_xxx）
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 开始一个计算列表
   *
   * 开始录制计算着色器命令。返回的 ComputeListID 用于后续操作。
   *
   * @return ComputeListID 计算列表句柄，失败返回 kInvalidComputeListId
   *
   * @pre IsInitialized() == true
   * @pre 当前没有活跃的 DrawList 或 ComputeList
   *
   * @par 典型使用
   *   @code
   *   ComputeListID cl = rd.ComputeListBegin();
   *   rd.ComputeListBindPipeline(cl, compute_pipeline_rid);
   *   rd.ComputeListBindUniformSet(cl, uniform_set_rid, 0);
   *   rd.ComputeListDispatch(cl, x_groups, y_groups, z_groups);
   *   rd.ComputeListEnd();
   *   @endcode
   *
   * @par 与 Godot 的对应
   *   Godot: compute_list_begin()
   *   ARHud:  ComputeListBegin()
   */
  ComputeListID ComputeListBegin();

  /**
   * @brief 绑定计算管线到计算列表
   *
   * @param[in] p_list      ComputeListBegin() 返回的句柄
   * @param[in] p_pipeline  计算管线 RID（由 ComputePipelineCreate() 创建）
   *
   * @pre p_list 有效且有活跃的 ComputeList
   * @pre p_pipeline 为计算管线（is_compute == true）
   */
  void ComputeListBindPipeline(ComputeListID p_list, RDPipelineID p_pipeline);

  /**
   * @brief 绑定 Uniform Set 到计算列表
   *
   * @param[in] p_list        ComputeListBegin() 返回的句柄
   * @param[in] p_uniform_set Uniform Set RID
   * @param[in] p_set_index   Set 索引
   *
   * @pre p_list 有效且有活跃的 ComputeList
   */
  void ComputeListBindUniformSet(ComputeListID p_list,
                                 RDUniformSetID p_uniform_set,
                                 uint32 p_set_index);

  /**
   * @brief 派发计算着色器
   *
   * @param[in] p_list      ComputeListBegin() 返回的句柄
   * @param[in] p_x_groups  X 维度工作组数量
   * @param[in] p_y_groups  Y 维度工作组数量
   * @param[in] p_z_groups  Z 维度工作组数量
   *
   * @pre p_list 有效且有活跃的 ComputeList
   * @pre 已绑定有效的计算管线
   *
   * @par OpenGL 等效
   *   glDispatchCompute(x_groups, y_groups, z_groups)
   */
  void ComputeListDispatch(ComputeListID p_list, uint32 p_x_groups,
                           uint32 p_y_groups, uint32 p_z_groups);

  /**
   * @brief 结束计算列表
   *
   * 结束计算命令录制。
   *
   * @pre ComputeListBegin() 已成功调用
   * @post ComputeList 不再活跃
   *
   * @par 与 Godot 的对应
   *   Godot: compute_list_end()
   *   ARHud:  ComputeListEnd()
   */
  void ComputeListEnd();

  // ═══════════════════════════════════════════════════════════════════════
  // GPU 时间戳查询（性能分析）
  // ═══════════════════════════════════════════════════════════════════════

  /**
   * @brief 创建 GPU 时间戳查询对象
   *
   * @return 查询对象 ID，失败返回空 ID
   */
  IRenderingDeviceDriver::TimestampQueryID TimestampQueryCreate();

  /**
   * @brief 销毁 GPU 时间戳查询对象
   *
   * @param[in] p_query 查询对象 ID
   */
  void TimestampQueryDestroy(IRenderingDeviceDriver::TimestampQueryID p_query);

  /**
   * @brief 记录当前 GPU 时间戳
   *
   * @param[in] p_query 查询对象 ID
   */
  void TimestampQueryRecord(IRenderingDeviceDriver::TimestampQueryID p_query);

  /**
   * @brief 获取时间戳查询结果（纳秒）
   *
   * @param[in] p_query 查询对象 ID
   *
   * @return GPU 时间戳（纳秒），0 表示不可用
   *
   * @note 此调用会阻塞等待 GPU 结果
   */
  uint64 TimestampQueryGetResult(
      IRenderingDeviceDriver::TimestampQueryID p_query);

  /**
   * @brief 检查时间戳查询结果是否可用
   *
   * @param[in] p_query 查询对象 ID
   *
   * @return true 结果已可用
   */
  bool TimestampQueryIsAvailable(
      IRenderingDeviceDriver::TimestampQueryID p_query);

  // ═══════════════════════════════════════════════════════════════════════
  // 依赖追踪
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 声明资源依赖关系
   *
   * 记录 p_dependent 依赖于 p_base。
   * 当 p_base 被释放时，p_dependent 也会被标记为待释放。
   *
   * @param[in] p_base      被依赖的资源 RID
   * @param[in] p_dependent 依赖方的资源 RID
   */
  void DependencyAdd(RID p_base, RID p_dependent);

  /**
   * @brief 移除资源依赖关系
   *
   * @param[in] p_base      被依赖的资源 RID
   * @param[in] p_dependent 依赖方的资源 RID
   */
  void DependencyRemove(RID p_base, RID p_dependent);

  /**
   * @brief 根据驱动层 BufferID 查找 RD 层 RID
   *
   * 遍历 buffer_owner_ 查找 driver_id 匹配的条目。
   * 用于 UniformSet 依赖追踪：当 Buffer 被释放时，
   * 引用该 Buffer 的 UniformSet 也需要被级联释放。
   *
   * @param[in] p_driver_id 驱动层 Buffer ID
   *
   * @return 匹配的 RID，未找到返回空 RID
   */
  RID FindBufferRidByDriverId(BufferID p_driver_id);

  /**
   * @brief 根据驱动层 TextureID 查找 RD 层 RID
   *
   * 遍历 texture_owner_ 查找 driver_id 匹配的条目。
   * 用于 UniformSet 依赖追踪：当 Texture 被释放时，
   * 引用该 Texture 的 UniformSet 也需要被级联释放。
   *
   * @param[in] p_driver_id 驱动层 Texture ID
   *
   * @return 匹配的 RID，未找到返回空 RID
   */
  RID FindTextureRidByDriverId(TextureID p_driver_id);

  /**
   * @brief 根据驱动层 SamplerID 查找 RD 层 RID
   *
   * 遍历 sampler_owner_ 查找 driver_id 匹配的条目。
   * 用于 UniformSet 依赖追踪：kSamplerWithTexture 成对 ID 中
   * 的 SamplerID 部分——当 Sampler 被释放时，引用该 Sampler 的
   * UniformSet 也需要被级联释放（对齐 Godot 对 uniform 内全部
   * 资源 ID 通用登记依赖边的模式）。
   *
   * @param[in] p_driver_id 驱动层 Sampler ID
   *
   * @return 匹配的 RID，未找到返回空 RID
   */
  RID FindSamplerRidByDriverId(SamplerID p_driver_id);

  // ═══════════════════════════════════════════════════════════════════
  // VRAM 预算跟踪
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 设置 VRAM 预算上限
   * @param[in] p_budget_bytes 预算字节数，0 表示无限制
   */
  void SetVramBudget(uint64 p_budget_bytes);

  /**
   * @brief 获取当前 VRAM 使用量
   * @return 使用量（字节）
   */
  uint64 GetVramUsage() const;

  /**
   * @brief 获取 VRAM 预算上限
   * @return 预算字节数，0 表示无限制
   */
  uint64 GetVramBudget() const;

  // ═══════════════════════════════════════════════════════════════════
  // 访问器
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 获取上下文驱动
   * @return 上下文驱动指针
   */
  IRenderingContextDriver* GetContextDriver() const {
    return context_driver_;
  }

  /**
   * @brief 获取设备驱动
   * @return 设备驱动指针
   */
  IRenderingDeviceDriver* GetDeviceDriver() const {
    return device_driver_;
  }

  /**
   * @brief 获取帧缓冲数量
   * @return 帧缓冲数量
   */
  uint32 GetFrameCount() const {
    return frame_count_;
  }

  /**
   * @brief 获取默认 Framebuffer（FBO 0，屏幕）
   *
   * 默认 Framebuffer 代表 OpenGL 的 FBO 0（屏幕缓冲区）。
   * 在 Initialize() 中自动创建，Finalize() 中自动释放。
   * 可用于 DrawListBegin() 和 DrawListClear()。
   *
   * @return 默认 Framebuffer RID
   */
  RDFramebufferID GetDefaultFramebuffer() const {
    return default_framebuffer_;
  }

  /**
   * @brief 获取默认 RenderPass
   *
   * 默认 RenderPass 描述了屏幕缓冲区的附件格式。
   * 与默认 Framebuffer 配合使用。
   *
   * @return 默认 RenderPass RID
   */
  RDRenderPassID GetDefaultRenderPass() const {
    return default_render_pass_;
  }

  /**
   * @brief 设置命令队列
   *
   * RenderingDevice 是渲染线程专用 API，其资源创建方法不自行代理跨线程
   * 调用（跨线程一律经 RenderServer 的 RID 分裂路径路由，ADR-0010）。
   * 此队列引用仅用于区分“初始化期（未绑定）”与“运行期”，供渲染线程
   * 不变量断言使用。
   *
   * @param[in] p_queue 命令队列指针（不拥有所有权）
   *
   * @see CommandQueue
   */
  void SetCommandQueue(CommandQueue* p_queue) {
    command_queue_ = p_queue;
    // 同步渲染线程 ID 到 CommandQueue（启用同线程短路优化）
    if (command_queue_ != nullptr &&
        render_thread_id_ != Thread::kUnassignedId) {
      command_queue_->SetRenderThreadId(render_thread_id_);
    }
  }

 private:
  // ═══════════════════════════════════════════════════════════════════
  // 内部方法
  // ═══════════════════════════════════════════════════════════════════

  /**
   * @brief 回收旧帧的延迟销毁资源
   *
   * 在 BeginFrame() 写入本帧槽位前调用：等待该槽位上一轮
   * （frame_count_ 帧前）EndFrame 插入的 GPU Fence，确认 GPU
   * 已完成后释放槽位中的待销毁资源。
   */
  void FreeResourcesFromPreviousFrames();

  /**
   * @brief 释放指定帧槽位中的所有待释放资源
   * @param[in] p_frame 帧资源引用
   */
  void FreeResourcesFromFrame(Frame& p_frame);

  /**
   * @brief 释放单个 Buffer 的驱动层资源
   * @param[in] p_rid Buffer RID
   */
  void FreeBufferDriverResource(RID p_rid);

  /**
   * @brief 释放单个 Texture 的驱动层资源
   * @param[in] p_rid Texture RID
   */
  void FreeTextureDriverResource(RID p_rid);

  /**
   * @brief 释放单个 Sampler 的驱动层资源
   * @param[in] p_rid Sampler RID
   */
  void FreeSamplerDriverResource(RID p_rid);

  /**
   * @brief 释放单个 Shader 的驱动层资源
   * @param[in] p_rid Shader RID
   */
  void FreeShaderDriverResource(RID p_rid);

  /**
   * @brief 释放单个 UniformSet 的驱动层资源
   * @param[in] p_rid UniformSet RID
   */
  void FreeUniformSetDriverResource(RID p_rid);

  /**
   * @brief 释放单个 VertexFormat 的驱动层资源
   * @param[in] p_rid VertexFormat RID
   */
  void FreeVertexFormatDriverResource(RID p_rid);

  /**
   * @brief 释放单个 RenderPass 的驱动层资源
   * @param[in] p_rid RenderPass RID
   */
  void FreeRenderPassDriverResource(RID p_rid);

  /**
   * @brief 释放单个 Framebuffer 的驱动层资源
   * @param[in] p_rid Framebuffer RID
   */
  void FreeFramebufferDriverResource(RID p_rid);

  /**
   * @brief 释放单个 Pipeline 的驱动层资源
   * @param[in] p_rid Pipeline RID
   */
  void FreePipelineDriverResource(RID p_rid);

  /**
   * @brief 计算 RenderPipeline 缓存键
   *
   * @param[in] p_shader               Shader RID
   * @param[in] p_vertex_format        VertexFormat RID
   * @param[in] p_render_primitive     渲染图元
   * @param[in] p_rasterization_state  光栅化状态
   * @param[in] p_multisample_state    多重采样状态
   * @param[in] p_depth_stencil_state  深度/模板状态
   * @param[in] p_blend_state          颜色混合状态
   * @param[in] p_dynamic_state        动态状态标志位
   * @param[in] p_render_pass          RenderPass RID
   *
   * @return 缓存键
   */
  auto ComputeRenderPipelineCacheKey(
      RDShaderID p_shader, RDVertexFormatID p_vertex_format,
      RenderPrimitive p_render_primitive,
      const PipelineRasterizationState& p_rasterization_state,
      const PipelineMultisampleState& p_multisample_state,
      const PipelineDepthStencilState& p_depth_stencil_state,
      const PipelineColorBlendState& p_blend_state,
      BitField<PipelineDynamicStateFlags> p_dynamic_state,
      RDRenderPassID p_render_pass) const -> PipelineCacheKey;

  /**
   * @brief 估算纹理 VRAM 占用
   *
   * @param[in] p_w 宽度
   * @param[in] p_h 高度
   * @param[in] p_format 数据格式
   * @param[in] p_mipmaps Mipmap 层级数
   *
   * @return 估算的 VRAM 字节数
   */
  uint32 EstimateTextureVram(uint32 p_w, uint32 p_h, DataFormat p_format,
                             uint32 p_mipmaps);

  /**
   * @brief 根据 RID 自动识别资源类型并释放驱动层资源
   *
   * 遍历所有 RIDOwner 查找 RID 所属的资源类型，
   * 调用对应的 FreeXxxDriverResource 方法释放。
   * 仅第一个匹配的 Owner 会执行释放。
   *
   * @param[in] p_rid 要释放的资源 RID
   */
  void FreeDriverResourceByRid(RID p_rid);

  /**
   * @brief 递归释放依赖于指定资源的所有资源
   *
   * 查找 dependency_map_ 中依赖于 p_base_rid 的所有资源，
   * 递归释放它们的依赖，然后释放资源本身。
   * 确保销毁顺序正确：先释放依赖方，再释放被依赖方。
   *
   * @param[in] p_base_rid 被依赖的基础资源 RID
   */
  void FreeResourceDependencies(RID p_base_rid);

  /**
   * @brief 释放所有资源（Finalize 时调用）
   */
  void FreeAllResources();

  /**
   * @brief 初始化 Staging Buffer
   *
   * @retval Error::kOK 成功
   */
  Error InitializeStagingBuffer();

  /**
   * @brief 销毁 Staging Buffer
   */
  void DestroyStagingBuffer();

  /**
   * @brief 从 Staging Buffer 分配空间（多级策略）
   *
   * 根据请求大小选择不同的分配策略：
   *   - 数据 ≤ Staging Block 剩余空间：子分配当前帧的 Staging Block
   *   - 数据 > Staging Block 剩余空间：创建独立临时 Buffer
   *
   * @param[in] p_size 请求的数据大小（字节）
   * @param[out] r_allocation 分配结果
   *
   * @retval Error::kOK 分配成功
   * @retval Error::kFailed 分配失败
   *
   * @pre Initialize() 已调用
   * @pre 必须在 BeginFrame/EndFrame 之间调用
   */
  Error StagingBufferAllocate(uint32 p_size,
                              StagingBufferAllocation& r_allocation);

  /**
   * @brief 解除 Staging Buffer 的 CPU 映射
   *
   * 数据写入完成、提交 GPU 拷贝命令前调用，解除当前帧 Staging Block
   * 的 CPU 映射，确保驱动侧可见最新写入的数据。
   *
   * @param[in] p_driver_id 待解除映射的 Staging Block 驱动层 ID
   *
   * @note 与 StagingBufferRemap() 成对使用，形成
   *       Unmap → GPU Copy → Remap 序列
   */
  void StagingBufferUnmap(BufferID p_driver_id);

  /**
   * @brief 重新映射 Staging Buffer
   *
   * GPU 拷贝命令完成后调用，重新获取 Staging Block 的 CPU 映射指针，
   * 以便后续帧继续向该 Block 写入数据。自动匹配小 Staging Block
   * 或主 Staging Block 并更新其 data_ptr。
   *
   * @param[in] p_driver_id 待重新映射的 Staging Block 驱动层 ID
   *
   * @pre p_driver_id 对应一个已通过 StagingBufferUnmap() 解除映射的 Block
   * @note 若重新映射失败（返回 nullptr），记录错误日志但不中断流程
   */
  void StagingBufferRemap(BufferID p_driver_id);

 private:
  // ═══════════════════════════════════════════════════════════════════
  // 成员变量
  // ═══════════════════════════════════════════════════════════════════

  // 生命周期
  bool initialized_ =
      false;  ///< 是否已初始化（Initialize 置位，Finalize 清除）
  Thread::ID render_thread_id_ =
      Thread::kUnassignedId;  ///< 注册的渲染线程 ID（MakeRenderThread 记录）

  // 驱动层
  IRenderingContextDriver* context_driver_ =
      nullptr;  ///< 上下文驱动（不拥有所有权，外部管理）
  IRenderingDeviceDriver* device_driver_ =
      nullptr;  ///< 设备驱动（拥有所有权，Finalize 释放）

  uint32 frame_count_ =
      kDefaultFrameCount;  ///< 帧缓冲数量（默认双缓冲，延迟销毁深度）
  /**
   * @brief 当前帧号（BeginFrame 递增）
   *
   * @note 必须保持 uint64：帧槽位/Fence 生命周期按 N % frame_count_ 取模，
   *       回绕无感；但 StagingBufferBlock::frame_references 的回收判定
   *       依赖帧号单调不减的无符号序比较（refs > completed），
   *       回绕会使 pre-wrap 引用永久判"未完成"（Block 钉死 + 引用列表
   *       重新无界增长，静默泄漏）。
   *       uint64 回绕需 2^64 帧（60fps 下约 97 亿年）——不可达；
   *       严禁为省内存窄化为 uint32（60fps 下 828 天即回绕，
   *       长寿命车机为真实场景）。Godot frames_drawn 同为 uint64。
   */
  uint64 frame_number_ = 0;
  CommandQueue* command_queue_ =
      nullptr;  ///< 命令队列引用（不拥有；仅用于渲染线程不变量断言区分初始化期/运行期）

  // RID 资源管理
  // 线程安全的 Owner：主线程在 DrawList 录制期间通过 GetOrNull 读取，
  // 渲染线程在 FreeResourcesFromFrame 中写入，需要 Mutex 保护
  RIDOwner<Buffer, true> buffer_owner_;
  RIDOwner<Texture, true> texture_owner_;
  RIDOwner<Framebuffer, true> framebuffer_owner_;
  RIDOwner<RenderPass, true> render_pass_owner_;
  RIDOwner<Pipeline, true> pipeline_owner_;
  RIDOwner<UniformSet, true> uniform_set_owner_;
  // 非线程安全的 Owner：仅在渲染线程访问（RenderingDevice 为渲染线程专用
  // API，创建/销毁均直接在该线程进行）
  RIDOwner<Sampler, false> sampler_owner_;
  RIDOwner<Shader, false> shader_owner_;
  RIDOwner<VertexFormat, false> vertex_format_owner_;

  // 帧资源池（环形缓冲）
  static constexpr uint32 kMaxFrameCount = 4;
  Frame frames_[kMaxFrameCount];  ///< 每帧待释放资源列表（延迟销毁，深度 =
                                  ///< frame_count_）

  // 依赖追踪
  // 正向映射：p_base → 依赖它的资源集合；反向映射：p_dependent →
  // 它依赖的资源集合
  HashMap<RID, HashSet<RID>>
      dependency_map_;  ///< 正向依赖映射（被依赖 → 依赖方集合）
  HashMap<RID, HashSet<RID>>
      reverse_dependency_map_;  ///< 反向依赖映射（依赖方 → 被依赖集合）

  // Pipeline 缓存（避免重复创建相同参数的 GLPipelineState）
  HashMap<PipelineCacheKey, RID, PipelineCacheKeyHasher>
      render_pipeline_cache_;  ///< 参数哈希 → Pipeline RID

  // Staging Buffer
  LocalVector<StagingBufferBlock>
      staging_buffer_blocks_;  ///< 主 Staging Block 环形数组（每帧一块）
  StagingBufferBlock
      small_staging_block_;  ///< 小 Staging Block（微小上传紧凑打包）
  uint32 staging_buffer_block_index_ =
      0;  ///< 当前帧使用的主 Block 索引（帧轮转）
  uint32 staging_buffer_size_ =
      kDefaultStagingBufferSize;  ///< 主 Block 大小（字节）
  uint64 staging_buffer_total_size_ =
      0;  ///< 所有主 Block 总大小（帧数 × 块大小）

  // DrawList 状态
  struct DrawListState {
    bool active = false;              ///< 是否有活跃的 DrawList
    bool render_pass_active = false;  ///< RenderPass 是否已开始（绑定 FBO）
    RDPipelineID bound_pipeline;      ///< 当前绑定的 Pipeline RID
    LocalVector<RDUniformSetID>
        bound_uniform_sets;  ///< 当前绑定的 UniformSet RID
  };

  DrawListState draw_list_;

  // ComputeList 状态
  struct ComputeListState {
    bool active = false;  ///< 是否有活跃的 ComputeList
  };

  ComputeListState compute_list_;

  // 默认资源（屏幕 FBO + 对应 RenderPass）
  RDFramebufferID
      default_framebuffer_;  ///< 默认 Framebuffer（OpenGL FBO 0，屏幕）
  RDRenderPassID default_render_pass_;  ///< 默认 RenderPass（屏幕缓冲附件格式）

  // ─── VRAM 预算跟踪 ───
  uint64 vram_usage_ = 0;   ///< 当前 VRAM 使用量（字节）
  uint64 vram_budget_ = 0;  ///< VRAM 预算上限（字节），0 表示无限制
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDERING_DEVICE_H_
