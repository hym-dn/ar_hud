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
 *   5. **CommandBuffer 工厂** — 创建和管理命令缓冲区实例
 *
 * @par 与 Godot RenderingDevice 的对比
 *   | 特性 | Godot RD | ARHud RD |
 *   |------|----------|----------|
 *   | RID 管理 | ✅ 完整 RIDOwner 体系 | ✅ 完整实现 |
 *   | Staging Buffer | ✅ 多 Block 环形 | ✅ 简化版（单 Block 足够 HUD） |
 *   | 帧循环 | ✅ Fence + SwapChain | ✅ 帧号 + 延迟销毁 |
 *   | 依赖追踪 | ✅ 双向 HashMap | ✅ 简化版 |
 *   | RDG (命令图) | ✅ 完整 DAG | ❌ 后续按需 |
 *   | SwapChain | ✅ acquire/present | ✅ RDD SwapChain（GL: MakeCurrent+SwapBuffers） |
 *   | Fence/Semaphore | ✅ 完整同步 | ✅ RDD 预留接口（OpenGL 空实现） |
 *   | Compute Pipeline | ✅ 支持 | ❌ HUD 不需要 |
 *   | Independent Device | ✅ LocalDevice（共享 RDD） | ✅ 独立 Context + 独立 RDD |
 *   | Timestamp Query | ✅ 支持 | ❌ 后续按需 |
 *   | 多 Surface | ❌ 单 SwapChain | ✅ 单 Context 多 Surface |
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
 *   4. 通过 CommandBuffer 提交 GPU 端拷贝命令
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

#pragma once

#include "rendering_device_commons.h"
#include "rendering_context_driver.h"
#include "rendering_device_driver.h"
#include "command_buffer.h"
#include "template/rid.h"
#include "template/local_vector.h"
#include "template/hash_map.h"
#include "template/hash_set.h"
#include "template/paged_allocator.h"
#include "os/thread.h"
#include "io/logger.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // 资源簿记结构
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Buffer 资源簿记
     *
     * 存储 Buffer 的元信息，与 RDD 层的 BufferID 配合使用。
     * RDD 层持有 GPU 端对象（GLuint），RD 层持有簿记信息。
     */
    struct Buffer
    {
        BufferID driver_id;              ///< 驱动层 Buffer ID
        uint64_t size = 0;               ///< 缓冲区大小（字节）
        BitField<BufferUsageBits> usage; ///< 用途标志位
        MemoryAllocationType allocation_type = MemoryAllocationType::kGpu;
        uint64_t frame_used = 0; ///< 最后使用帧号
    };

    /**
     * @brief Texture 资源簿记
     */
    struct Texture
    {
        TextureID driver_id;     ///< 驱动层 Texture ID
        TextureFormat format;    ///< 纹理格式描述
        uint64_t frame_used = 0; ///< 最后使用帧号
    };

    /**
     * @brief Sampler 资源簿记
     */
    struct Sampler
    {
        SamplerID driver_id; ///< 驱动层 Sampler ID
        SamplerState state;  ///< 采样器状态
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
    struct Shader
    {
        ShaderID driver_id;                     ///< 驱动层 Shader ID
        LocalVector<ShaderUniform> uniforms;    ///< Uniform 反射数据
        uint32_t push_constant_size = 0;        ///< Push Constant 块大小（字节）
        bool is_valid = false;                  ///< 着色器是否编译成功
        bool is_compute = false;                ///< 是否为计算着色器
        BitField<ShaderStage> stage_bits;       ///< 着色器阶段位掩码
    };

    /**
     * @brief UniformSet 资源簿记
     */
    struct UniformSet
    {
        UniformSetID driver_id; ///< 驱动层 UniformSet ID
        ShaderID shader_id;     ///< 关联的着色器
        uint32_t set_index = 0; ///< Set 索引
    };

    /**
     * @brief VertexFormat 资源簿记
     */
    struct VertexFormat
    {
        VertexFormatID driver_id; ///< 驱动层 VertexFormat ID
    };

    /**
     * @brief RenderPass 资源簿记
     */
    struct RenderPass
    {
        RenderPassID driver_id; ///< 驱动层 RenderPass ID
    };

    /**
     * @brief Framebuffer 资源簿记
     */
    struct Framebuffer
    {
        FramebufferID driver_id; ///< 驱动层 Framebuffer ID
        uint32_t width = 0;
        uint32_t height = 0;
    };

    /**
     * @brief Pipeline 资源簿记
     */
    struct Pipeline
    {
        PipelineID driver_id; ///< 驱动层 Pipeline ID
    };

    /**
     * @brief CommandBuffer 池内状态
     *
     * 标识 CommandBuffer 在池中的使用状态，用于池化分配和回收。
     */
    enum class CommandBufferPoolState : uint8_t
    {
        kFree,     ///< 空闲，可分配
        kAcquired, ///< 已分配，正在录制
        kReady,    ///< 录制完成，待执行
        kExecuted, ///< 已执行，待回收
    };

    /**
     * @brief CommandBuffer 资源簿记
     *
     * 存储 CommandBuffer 的元信息。与 GPU 资源不同，
     * CommandBuffer 是 CPU 端的命令录制容器，不直接对应 GPU 对象。
     *
     * @par 池化管理
     *   CommandBuffer 采用池化复用策略：
     *   - 初始化时预创建 frame_count_ 个 CommandBuffer
     *   - 每帧从池中获取空闲 CommandBuffer
     *   - Execute 后归还池中，下一帧复用
     *   - Finalize 时统一销毁
     *
     * @par 生命周期状态
     *   @code
     *   kFree → CommandBufferAcquire() → kAcquired
     *   kAcquired → Begin() 录制 → End() → kReady
     *   kReady → Execute() → kExecuted
     *   kExecuted → CommandBufferRelease() → kFree
     *   @endcode
     */
    struct CommandBuffer
    {
        ICommandBuffer *driver_cmd = nullptr;                              ///< 驱动层 CommandBuffer 指针
        CommandBufferPoolState pool_state = CommandBufferPoolState::kFree; ///< 池内状态
        uint64_t frame_used = 0;                                           ///< 最后使用帧号
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
    DEFINE_ID(RDCommandBuffer);

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
    using DrawListID = uint32_t;

    static constexpr DrawListID kInvalidDrawListId = UINT32_MAX;

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
     *   4. 帧结束后标记 frame_used
     *   5. 下一帧检查旧 Block 是否可回收（GPU 已完成）
     */
    struct StagingBufferBlock
    {
        BufferID driver_id;          ///< 驱动层 Buffer ID
        uint8_t *data_ptr = nullptr; ///< mmap 指针（CPU 端）
        uint64_t frame_used = 0;     ///< 该 Block 最后使用的帧号
        uint32_t fill_amount = 0;    ///< 当前已填充的字节数
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
    struct StagingBufferAllocation
    {
        uint8_t *data_ptr = nullptr; ///< CPU 端写入指针
        BufferID driver_id;          ///< 驱动层 Buffer ID
        uint64_t offset = 0;         ///< 在 Buffer 中的字节偏移
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
    struct Frame
    {
        LocalVector<RID> buffers_to_free;
        LocalVector<RID> textures_to_free;
        LocalVector<RID> samplers_to_free;
        LocalVector<RID> shaders_to_free;
        LocalVector<RID> uniform_sets_to_free;
        LocalVector<RID> vertex_formats_to_free;
        LocalVector<RID> render_passes_to_free;
        LocalVector<RID> framebuffers_to_free;
        LocalVector<RID> pipelines_to_free;
        LocalVector<RID> command_buffers_to_release;

        void Clear()
        {
            buffers_to_free.Clear();
            textures_to_free.Clear();
            samplers_to_free.Clear();
            shaders_to_free.Clear();
            uniform_sets_to_free.Clear();
            vertex_formats_to_free.Clear();
            render_passes_to_free.Clear();
            framebuffers_to_free.Clear();
            pipelines_to_free.Clear();
            command_buffers_to_release.Clear();
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
     *   RDBufferID buf = rd->BufferCreate(1024, BufferUsageBits::kVertex, MemoryAllocationType::kGpu);
     *   RDTextureID tex = rd->TextureCreate(fmt, view);
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
    class RenderingDevice
    {
        ARHUD_DISABLE_COPY_MOVE(RenderingDevice);
        
    public:
        static constexpr uint32_t kDefaultFrameCount = 2;
        static constexpr uint32_t kDefaultStagingBufferSize = 4 * 1024 * 1024;

        /**
         * @brief Staging Buffer 三级分配阈值
         *
         * | 数据大小              | 分配策略                          | 对齐   |
         * |----------------------|----------------------------------|--------|
         * | ≤ kSmallUploadMax    | 小 Block 紧凑打包（减少 cache miss）| 32B   |
         * | ≤ kLargeUploadThreshold | 主 Block 线性子分配             | 256B  |
         * | > kLargeUploadThreshold | 独立临时 Buffer（避免挤占主 Block）| N/A   |
         */
        static constexpr uint32_t kSmallUploadMax = 256;
        static constexpr uint32_t kSmallStagingBlockSize = 64 * 1024;
        static constexpr uint32_t kLargeUploadThreshold = 64 * 1024;
        static constexpr uint64_t kMaxStagingTotalSize = 256 * 1024 * 1024;

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
         * @param[in] p_context_driver 上下文驱动（不拥有所有权，由调用者管理生命周期）
         * @param[in] p_frame_count    帧缓冲数量（默认 2，双缓冲）
         * @param[in] p_staging_buffer_size Staging Buffer 每块大小（字节，默认 4MB）
         *
         * @retval Error::kOK 初始化成功
         * @retval Error::kAlreadyExists 已初始化
         * @retval Error::kFailed 初始化失败（RDD 创建/初始化失败，或 Staging Buffer/CommandBuffer 创建失败）
         *
         * @pre p_context_driver 已成功初始化（IsInitialized() == true）
         * @pre 必须在拥有 GL 上下文的渲染线程上调用
         * @pre GL 上下文已绑定到当前线程
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
         *   context_driver.Initialize(screen_id, native_window, width, height);
         *
         *   // 2. 创建 RD 并初始化（RD 内部创建 RDD）
         *   RenderingDevice rd;
         *   rd.Initialize(&context_driver);
         *
         *   // 3. 绑定 SwapChain
         *   rd.MakeCurrent(0, 0);
         *   @endcode
         *
         * @par 典型销毁顺序
         *   @code
         *   rd.Finalize();                     // 1. 释放 RID 资源 + RDD
         *   context_driver.Shutdown();          // 2. 销毁 RCD
         *   @endcode
         */
        Error Initialize(IRenderingContextDriver *p_context_driver,
                         uint32_t p_frame_count = kDefaultFrameCount,
                         uint32_t p_staging_buffer_size = kDefaultStagingBufferSize);

        /**
         * @brief 终止渲染设备，释放所有资源
         *
         * 释放所有 RIDOwner 管理的资源，销毁 Staging Buffer。
         * 对于主设备：不销毁 RCD 和 RDD（由调用者管理）。
         * 对于独立设备：由父 RD 的 Finalize/DestroyIndependentDevice 负责销毁 RDD。
         * 调用后不应再使用任何 RD 方法。
         *
         * @pre 所有外部引用已释放
         * @pre 必须在拥有 GL 上下文的渲染线程上调用
         * @pre 所有独立设备必须已在其各自的渲染线程上调用 Finalize()
         *
         * @par 线程安全
         *   Finalize() 内部会调用 GPU 资源释放（如 glDeleteFramebuffers），
         *   这些操作要求 GL 上下文已绑定到当前线程。
         *   调用者必须确保在正确的渲染线程上调用此方法。
         *
         * @par 典型销毁顺序
         *   @code
         *   // 1. 独立渲染线程：先 Finalize 自己的 RD
         *   indep_rd->Finalize();
         *
         *   // 2. 主渲染线程：Finalize 主 RD（会断言检查独立设备已 Finalize）
         *   main_rd->Finalize();
         *
         *   // 3. 主线程：销毁 RDD、RCD
         *   context_driver->DriverFree(rdd);
         *   delete context_driver;
         *   @endcode
         */
        void Finalize();

        /**
         * @brief 检查是否已初始化
         * @return true 已初始化
         */
        bool IsInitialized() const { return initialized_; }

        /**
         * @brief 查询是否为主设备
         *
         * 主设备由外部创建，可以调用 CreateIndependentDevice()。
         * 独立设备由 CreateIndependentDevice() 创建，不能再创建子设备。
         *
         * @return true 主设备
         */
        bool IsMainDevice() const { return is_main_device_; }

        /**
         * @brief 将当前线程注册为渲染线程
         *
         * 调用后，当前线程 ID 被记录为该 RenderingDevice 的渲染线程。
         * 后续所有需要渲染线程的操作（Initialize/Finalize/资源创建等）
         * 将断言检查调用线程是否匹配。
         *
         * @par 使用场景
         *   - 主设备：在 Initialize 之前调用，或 Initialize 自动记录
         *   - 独立设备：在独立渲染线程入口处调用
         *
         * @see IsRenderThread()
         */
        void MakeRenderThread() { render_thread_id_ = Thread::GetCallerId(); }

        /**
         * @brief 检查当前线程是否为渲染线程
         *
         * @return true 当前线程是注册的渲染线程
         *
         * @see MakeRenderThread()
         */
        bool IsRenderThread() const { return Thread::GetCallerId() == render_thread_id_; }

        // ═══════════════════════════════════════════════════════════════════
        // 独立渲染设备（多线程渲染）
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 创建独立渲染设备（主线程阶段）
         *
         * 创建独立渲染栈的平台资源：Surface（GetDC + SetPixelFormat）+
         * Context（wglCreateContext）+ RDD + RD 对象。
         * 不执行任何 GL 操作，可在主线程安全调用。
         *
         * 创建后必须调用 InitializeIndependentDevice() 在渲染线程上
         * 完成 GL 初始化（SwapChainAcquire + RD::Initialize）。
         *
         * @par 与 Godot LocalDevice 的区别
         *   Godot LocalDevice 共享 RDD（VkDevice 天然线程安全），
         *   ARHud IndependentDevice 拥有独立 RDD（OpenGL 上下文不共享）。
         *   这意味着每个独立设备需要独立创建 GPU 资源。
         *
         * @param[in] p_screen_id 关联的屏幕 ID
         * @param[in] p_native_window 原生窗口句柄
         * @param[in] p_width 窗口宽度
         * @param[in] p_height 窗口高度
         *
         * @return 新创建的独立渲染设备指针（未初始化）
         * @retval nullptr 创建失败
         *
         * @pre IsInitialized() == true
         * @pre IsMainDevice() == true（独立设备不能再创建子设备）
         *
         * @par 线程安全
         *   此方法在主线程调用，只执行平台窗口操作（GetDC/SetPixelFormat/
         *   wglCreateContext），不执行任何 GL 命令。
         *
         * @par 完整生命周期
         *   @code
         *   // === 阶段 1：主线程创建（平台资源，无 GL 操作）===
         *   RenderingDevice* indep_rd = main_rd->CreateIndependentDevice(
         *       screen_id, hwnd, width, height);
         *
         *   // === 阶段 2：渲染线程初始化 + 渲染循环 ===
         *   void RenderThread(RenderingDevice* rd, std::atomic<bool>& running) {
         *       rd->InitializeIndependentDevice();  // wglMakeCurrent + GL 资源创建
         *       while (running.load()) {
         *           rd->BeginFrame();
         *           // ... 渲染 ...
         *           rd->EndFrame();
         *       }
         *       rd->Finalize();  // 渲染线程退出前必须 Finalize
         *   }
         *
         *   // === 阶段 3：主线程销毁 ===
         *   running.store(false);                  // 通知渲染线程退出
         *   render_thread.WaitToFinish();           // 等待渲染线程结束
         *   main_rd->DestroyIndependentDevice(indep_rd);  // 清理平台资源
         *   @endcode
         *
         * @see InitializeIndependentDevice()
         * @see DestroyIndependentDevice()
         */
        RenderingDevice *CreateIndependentDevice(IScreen::ScreenID p_screen_id,
                                                 void *p_native_window,
                                                 uint32_t p_width,
                                                 uint32_t p_height);

        /**
         * @brief 初始化独立渲染设备（渲染线程阶段）
         *
         * 在渲染线程上完成独立设备的 GL 初始化：
         * SwapChainAcquire（wglMakeCurrent）+ RD::Initialize（GL 资源创建）。
         *
         * @param[in] p_device 由 CreateIndependentDevice() 创建的设备
         *
         * @retval Error::kOK 初始化成功
         * @retval Error::kFailed 初始化失败
         *
         * @pre p_device 由 CreateIndependentDevice() 创建且尚未初始化
         * @pre 必须在渲染线程上调用
         *
         * @par 线程安全
         *   此方法执行 GL 操作（wglMakeCurrent、BufferCreate 等），
         *   必须在拥有 GL 上下文的渲染线程上调用。
         */
        Error InitializeIndependentDevice(RenderingDevice *p_device);

        /**
         * @brief 销毁独立渲染设备
         *
         * 销毁由 CreateIndependentDevice 创建的独立渲染设备，
         * 释放其 RDD、Surface 和 GL 上下文。
         *
         * @param[in] p_device 独立渲染设备指针
         *
         * @pre p_device 由 CreateIndependentDevice() 创建
         * @pre p_device 已在其渲染线程上调用 Finalize()（否则断言失败）
         * @post p_device 已销毁，指针失效
         *
         * @note 此方法不调用 Finalize()，因为 GPU 资源释放必须在渲染线程进行。
         *       调用者应先在独立渲染线程上调用 p_device->Finalize()，
         *       然后在主线程上调用此方法清理剩余资源。
         */
        void DestroyIndependentDevice(RenderingDevice *p_device);

        // ═══════════════════════════════════════════════════════════════════
        // Surface 操作（委托到 RDD SwapChain）
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 将指定 GL 上下文绑定到指定 Surface
         *
         * 通过 RDD SwapChain 实现上下文绑定。
         * 首次对 (Context, Surface) 组合调用时自动创建 SwapChain。
         *
         * @param[in] p_context_id RCD Context ID
         * @param[in] p_surface RCD Surface ID
         *
         * @return Error::kOK 绑定成功
         *
         * @pre IsInitialized() == true
         *
         * @see SwapChainAcquire()
         */
        Error MakeCurrent(IRenderingContextDriver::ContextID p_context_id,
                          IRenderingContextDriver::SurfaceID p_surface);

        /**
         * @brief 交换指定 Surface 的前后缓冲区
         *
         * 通过 RDD SwapChain 实现缓冲区交换。
         *
         * @param[in] p_surface RCD Surface ID
         *
         * @pre IsInitialized() == true
         * @pre MakeCurrent() 已对该 Surface 调用过
         *
         * @see SwapChainPresent()
         */
        void SwapBuffers(IRenderingContextDriver::SurfaceID p_surface);

        // ═══════════════════════════════════════════════════════════════════
        // 帧循环
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 开始新的一帧
         *
         * 递增帧号，回收旧帧的延迟销毁资源，重置 Staging Buffer 偏移。
         *
         * @pre IsInitialized() == true
         */
        void BeginFrame();

        /**
         * @brief 结束当前帧
         *
         * 标记当前帧资源为待回收，准备下一帧。
         *
         * @pre BeginFrame() 已调用
         */
        void EndFrame();

        /**
         * @brief 获取当前帧号
         * @return 帧号（从 0 开始递增）
         */
        uint64_t GetFrameNumber() const { return frame_number_; }

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
        RDBufferID BufferCreate(uint64_t p_size,
                                BitField<BufferUsageBits> p_usage,
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
        uint64_t BufferGetSize(RDBufferID p_buffer) const;

        /**
         * @brief 映射 Buffer 到 CPU 内存
         *
         * @param[in] p_buffer Buffer RID
         *
         * @return CPU 内存指针，失败返回 nullptr
         */
        uint8_t *BufferMap(RDBufferID p_buffer);

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
        Error BufferUpdate(RDBufferID p_buffer, uint32_t p_offset,
                           uint32_t p_size, const void *p_data);

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
        RDTextureID TextureCreate(const TextureFormat &p_format,
                                  const TextureView &p_view);

        /**
         * @brief 释放 Texture 资源（延迟销毁）
         *
         * @param[in] p_texture Texture RID
         */
        void TextureFree(RDTextureID p_texture);

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
        const TextureFormat &TextureGetFormat(RDTextureID p_texture) const;

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
         *   uint8_t pixels[256*256*4] = { ... };
         *   rd.TextureUpdate(tex_rid, 0, 0, pixels, sizeof(pixels));
         *   @endcode
         */
        Error TextureUpdate(RDTextureID p_texture, uint32_t p_layer,
                            uint32_t p_mipmap, const void *p_data,
                            uint32_t p_data_size);

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
        RDSamplerID SamplerCreate(const SamplerState &p_state);

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
         * @brief 从 GLSL 源码创建 Shader 资源
         *
         * @param[in] p_stage_sources   着色器阶段源码数组
         * @param[in] p_uniforms        Uniform 描述数组
         * @param[in] p_push_constant_size Push Constant 大小（字节）
         *
         * @return Shader RID，失败返回空 RID
         */
        RDShaderID ShaderCreateFromGLSL(VectorView<ShaderStageSource> p_stage_sources,
                                        VectorView<ShaderUniform> p_uniforms,
                                        uint32_t p_push_constant_size);

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
        const LocalVector<ShaderUniform> &ShaderGetUniforms(RDShaderID p_shader) const;

        /**
         * @brief 获取 Shader 的 Push Constant 块大小
         *
         * @param[in] p_shader Shader RID
         *
         * @return Push Constant 大小（字节），无效 RID 返回 0
         */
        uint32_t ShaderGetPushConstantSize(RDShaderID p_shader) const;

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
                                        RDShaderID p_shader,
                                        uint32_t p_set_index);

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
        RDVertexFormatID VertexFormatCreate(VectorView<VertexAttribute> p_vertex_attribs);

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
        VertexFormatID VertexFormatGetDriverId(RDVertexFormatID p_vertex_format) const;

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
        RDRenderPassID RenderPassCreate(VectorView<Attachment> p_attachments,
                                        VectorView<Subpass> p_subpasses,
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
                                          uint32_t p_width,
                                          uint32_t p_height);

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
        RDPipelineID RenderPipelineCreate(RDShaderID p_shader,
                                          RDVertexFormatID p_vertex_format,
                                          RenderPrimitive p_render_primitive,
                                          const PipelineRasterizationState &p_rasterization_state,
                                          const PipelineMultisampleState &p_multisample_state,
                                          const PipelineDepthStencilState &p_depth_stencil_state,
                                          const PipelineColorBlendState &p_blend_state,
                                          BitField<PipelineDynamicStateFlags> p_dynamic_state,
                                          RDRenderPassID p_render_pass);

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
         * 内部从 CommandBuffer 池获取一个空闲 CommandBuffer 并开始录制。
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
         *   ARHud:  DrawListBegin() — 当前无 Framebuffer 参数（直接渲染到屏幕）
         *           未来添加 Framebuffer RID 参数以支持离屏渲染
         */
        DrawListID DrawListBegin();

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
        void DrawListSetViewport(DrawListID p_list,
                                 int32_t p_x, int32_t p_y,
                                 uint32_t p_width, uint32_t p_height);

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
        void DrawListSetScissor(DrawListID p_list,
                                int32_t p_x, int32_t p_y,
                                uint32_t p_width, uint32_t p_height);

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
        void DrawListSetBlendConstants(DrawListID p_list,
                                       float p_r, float p_g,
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
        void DrawListBindUniformSet(DrawListID p_list,
                                    RDUniformSetID p_uniform_set,
                                    uint32_t p_set_index);

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
        void DrawListBindVertexBuffers(DrawListID p_list,
                                       const RDBufferID *p_buffers,
                                       const uint64_t *p_offsets,
                                       uint32_t p_count);

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
        void DrawListBindIndexBuffer(DrawListID p_list,
                                     RDBufferID p_buffer,
                                     IndexBufferFormat p_format,
                                     uint64_t p_offset);

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
        void DrawListDraw(DrawListID p_list,
                          uint32_t p_vertex_count,
                          uint32_t p_instance_count = 1);

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
        void DrawListDrawIndexed(DrawListID p_list,
                                 uint32_t p_index_count,
                                 uint32_t p_instance_count = 1);

        /**
         * @brief 结束绘制列表
         *
         * 结束命令录制，执行回放，归还 CommandBuffer 到池。
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

        // ═══════════════════════════════════════════════════════════════════
        // CommandBuffer 池管理（内部方法，供 DrawList 使用）
        // ═══════════════════════════════════════════════════════════════════

        RDCommandBufferID CommandBufferAcquire();
        void CommandBufferRelease(RDCommandBufferID p_command_buffer);
        ICommandBuffer *CommandBufferGetDriverCmd(RDCommandBufferID p_command_buffer) const;

        // ═══════════════════════════════════════════════════════════════════
        // 访问器
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 获取上下文驱动
         * @return 上下文驱动指针
         */
        IRenderingContextDriver *GetContextDriver() const { return context_driver_; }

        /**
         * @brief 获取设备驱动
         * @return 设备驱动指针
         */
        IRenderingDeviceDriver *GetDeviceDriver() const { return device_driver_; }

        /**
         * @brief 获取帧缓冲数量
         * @return 帧缓冲数量
         */
        uint32_t GetFrameCount() const { return frame_count_; }

    private:
        // ═══════════════════════════════════════════════════════════════════
        // 内部方法
        // ═══════════════════════════════════════════════════════════════════

        /**
         * @brief 回收旧帧的延迟销毁资源
         *
         * 在 BeginFrame() 中调用，检查 frame_count_ 帧前的待释放列表，
         * 如果 GPU 已不再使用这些资源，则调用驱动层释放。
         */
        void FreeResourcesFromPreviousFrames();

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
         * @brief 释放单个 CommandBuffer 的驱动层资源
         * @param[in] p_rid CommandBuffer RID
         */
        void FreeCommandBufferDriverResource(RID p_rid);

        /**
         * @brief 初始化 CommandBuffer 池
         *
         * 预创建 frame_count_ 个 CommandBuffer 到池中。
         *
         * @retval Error::kOK 成功
         */
        Error InitializeCommandBufferPool();

        /**
         * @brief 销毁 CommandBuffer 池
         *
         * 释放池中所有 CommandBuffer 的驱动层资源。
         */
        void DestroyCommandBufferPool();

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
        Error StagingBufferAllocate(uint32_t p_size, StagingBufferAllocation &r_allocation);

        // ═══════════════════════════════════════════════════════════════════
        // 成员变量
        // ═══════════════════════════════════════════════════════════════════

        bool initialized_ = false;
        bool is_main_device_ = true;
        Thread::ID render_thread_id_ = Thread::kUnassignedId;

        IRenderingContextDriver *context_driver_ = nullptr;
        IRenderingDeviceDriver *device_driver_ = nullptr;

        LocalVector<RenderingDevice *> independent_devices_;

        /**
         * @brief 独立渲染设备附加信息
         *
         * 跟踪独立设备关联的 GL 上下文 ID 和 Surface ID，
         * 用于 DestroyIndependentDevice() 时的资源清理。
         */
        struct IndependentDeviceInfo
        {
            RenderingDevice *device = nullptr;
            IRenderingContextDriver::ContextID context_id = IRenderingContextDriver::kInvalidContextId;
            IRenderingContextDriver::SurfaceID surface_id = IRenderingContextDriver::kInvalidSurfaceId;
            IRenderingDeviceDriver::SwapChainID swapchain_id = IRenderingDeviceDriver::kInvalidSwapChainId;
        };
        LocalVector<IndependentDeviceInfo> independent_device_infos_;

        /**
         * @brief SwapChain 簿记
         *
         * 记录 RCD (Context, Surface) 到 RDD SwapChain 的映射。
         * MakeCurrent 首次对 (Context, Surface) 调用时自动创建。
         */
        struct SwapChainInfo
        {
            IRenderingContextDriver::ContextID context_id = IRenderingContextDriver::kInvalidContextId;
            IRenderingContextDriver::SurfaceID surface_id = IRenderingContextDriver::kInvalidSurfaceId;
            IRenderingDeviceDriver::SwapChainID swapchain_id = IRenderingDeviceDriver::kInvalidSwapChainId;
        };
        LocalVector<SwapChainInfo> swapchain_infos_;

        uint32_t frame_count_ = kDefaultFrameCount;
        uint64_t frame_number_ = 0;

        // RID 资源管理
        RIDOwner<Buffer> buffer_owner_;
        RIDOwner<Texture> texture_owner_;
        RIDOwner<Sampler> sampler_owner_;
        RIDOwner<Shader> shader_owner_;
        RIDOwner<UniformSet> uniform_set_owner_;
        RIDOwner<VertexFormat> vertex_format_owner_;
        RIDOwner<RenderPass> render_pass_owner_;
        RIDOwner<Framebuffer> framebuffer_owner_;
        RIDOwner<Pipeline> pipeline_owner_;
        RIDOwner<CommandBuffer> command_buffer_owner_;

        // 帧资源池（环形缓冲）
        static constexpr uint32_t kMaxFrameCount = 4;
        Frame frames_[kMaxFrameCount];

        // 依赖追踪
        HashMap<RID, HashSet<RID>> dependency_map_;
        HashMap<RID, HashSet<RID>> reverse_dependency_map_;

        // Staging Buffer
        LocalVector<StagingBufferBlock> staging_buffer_blocks_;
        StagingBufferBlock small_staging_block_;
        uint32_t staging_buffer_block_index_ = 0;
        uint32_t staging_buffer_size_ = kDefaultStagingBufferSize;
        uint64_t staging_buffer_total_size_ = 0;

        // DrawList 状态
        struct DrawListState
        {
            bool active = false;
            RDCommandBufferID command_buffer;
            ICommandBuffer *driver_cmd = nullptr;
        } draw_list_;
    };

} // namespace arhud
