/**
 * @file rendering_device_commons.h
 * @brief 渲染设备共享枚举、类型与结构体定义
 *
 * 参考 Godot 4.6 rendering_device_commons.h 设计，定义 RenderingDevice (RD)
 * 与 RenderingDeviceDriver (RDD) 共享的所有枚举、结构和类型。
 *
 * @par 设计原则（来自 Godot 源码注释）：
 *   1. 极少验证，仅在 Debug 构建中做
 *   2. 错误报告简单：返回 id=0 或 false
 *   3. 枚举/常量/结构体尽量对齐 Vulkan 值，使 Vulkan 驱动可直接 assert 兼容
 *   4. 热路径尽量零分配，使用 alloca()
 *   5. 使用 PagedAllocator 管理簿记结构
 *   6. 使用 VectorView 传递数组参数，避免拷贝
 *
 * @par 与 Godot 的差异：
 *   - 去掉 Godot Object/GDSOFTCLASS 继承，纯 C++ 枚举和结构体
 *   - 去掉不需要的格式（YUV/视频解码/ASTC HDR 等），HUD 场景用不到
 *   - 去掉 VRS/Fragment Density 等移动端特性
 *   - 去掉 MetalFX 相关特性和 Limit
 *   - 命名遵循 ARHud PascalCase + k 前缀约定
 *   - 使用 BitField<T> 替代 Godot 的 BitField<>
 *   - 使用 VectorView<T> 替代 Godot 的 VectorView / Span
 *
 * @par 架构层次：
 *   rendering_device_commons.h（本文件，共享定义）
 *     ├── rendering_context_driver.h  ← 上下文驱动接口（引用本文件类型）
 *     ├── rendering_device_driver.h   ← 设备驱动接口（继承本文件定义）
 *     └── rendering_device.h          ← 高层渲染设备（引用本文件定义）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#pragma once

#include "typedefs.h"
#include "template/bitfield.h"
#include "template/local_vector.h"
#include "template/vector_view.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // 强类型资源 ID 定义宏
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @def ARHUD_DEFINE_RDD_ID(m_name)
     * @brief 定义强类型渲染设备资源 ID
     *
     * 参考 Godot rendering_device_driver.h 中的 DEFINE_ID 宏。
     * 生成的类名为 `m_name##ID`，例如：
     *   @code
     *     ARHUD_DEFINE_RDD_ID(Buffer);    // 生成 BufferID 类
     *     ARHUD_DEFINE_RDD_ID(Texture);   // 生成 TextureID 类
     *   @endcode
     *
     * 每个 ID 类型包装一个 uint64_t，提供类型安全的比较和有效性检查。
     * 可从原生指针构造（OpenGL 驱动用 GLuint 指针映射）。
     *
     * @param m_name ID 类型名称前缀
     */
#define ARHUD_DEFINE_RDD_ID(m_name)                                                                         \
    class m_name##ID                                                                                        \
    {                                                                                                       \
        uint64_t id_ = 0;                                                                                   \
                                                                                                            \
    public:                                                                                                 \
        ARHUD_ALWAYS_INLINE m_name##ID() = default;                                                         \
        ARHUD_ALWAYS_INLINE explicit m_name##ID(uint64_t p_id) : id_(p_id) {}                               \
        ARHUD_ALWAYS_INLINE explicit m_name##ID(void *p_ptr) : id_(reinterpret_cast<uint64_t>(p_ptr)) {}    \
        ARHUD_ALWAYS_INLINE explicit operator bool() const { return id_ != 0; }                             \
        ARHUD_ALWAYS_INLINE bool operator==(const m_name##ID &p_other) const { return id_ == p_other.id_; } \
        ARHUD_ALWAYS_INLINE bool operator!=(const m_name##ID &p_other) const { return id_ != p_other.id_; } \
        ARHUD_ALWAYS_INLINE bool operator<(const m_name##ID &p_other) const { return id_ < p_other.id_; }   \
        ARHUD_ALWAYS_INLINE uint64_t GetId() const { return id_; }                                          \
        ARHUD_ALWAYS_INLINE bool IsValid() const { return id_ != 0; }                                       \
        ARHUD_ALWAYS_INLINE bool IsNull() const { return id_ == 0; }                                        \
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 资源 ID 类型声明（前置声明，供后续结构体引用）
    // ═══════════════════════════════════════════════════════════════════════

    ARHUD_DEFINE_RDD_ID(Buffer);
    ARHUD_DEFINE_RDD_ID(Texture);
    ARHUD_DEFINE_RDD_ID(Sampler);
    ARHUD_DEFINE_RDD_ID(VertexFormat);
    ARHUD_DEFINE_RDD_ID(CommandQueueFamily);
    ARHUD_DEFINE_RDD_ID(CommandQueue);
    ARHUD_DEFINE_RDD_ID(CommandPool);
    ARHUD_DEFINE_RDD_ID(CommandBuffer);
    ARHUD_DEFINE_RDD_ID(SwapChain);
    ARHUD_DEFINE_RDD_ID(Framebuffer);
    ARHUD_DEFINE_RDD_ID(Shader);
    ARHUD_DEFINE_RDD_ID(UniformSet);
    ARHUD_DEFINE_RDD_ID(Pipeline);
    ARHUD_DEFINE_RDD_ID(RenderPass);
    ARHUD_DEFINE_RDD_ID(QueryPool);
    ARHUD_DEFINE_RDD_ID(Fence);
    ARHUD_DEFINE_RDD_ID(Semaphore);

    // ═══════════════════════════════════════════════════════════════════════
    // 通用常量
    // ═══════════════════════════════════════════════════════════════════════

    static constexpr int32_t kInvalidId = -1;

    // ═══════════════════════════════════════════════════════════════════════
    // 数据格式（对齐 Vulkan VkFormat 枚举值）
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief GPU 数据格式
     *
     * 枚举值对齐 Vulkan VkFormat，使 Vulkan 驱动可直接 static_assert 兼容。
     * OpenGL 驱动通过查表映射到 GL 内部格式。
     *
     * @par AR HUD 常用格式：
     *   - kR8G8B8A8Unorm: 4 通道 8 位 RGBA（HUD 图标/纹理）
     *   - kB8G8R8A8Unorm: Windows 位图标准格式（SwapChain 输出）
     *   - kR32Sfloat: 单精度浮点（Uniform Buffer 数据）
     *   - kD24UnormS8Uint: 深度+模板附件（HUD 不常用模板）
     *   - kD32Sfloat: 纯深度附件
     *   - kR16Sfloat: 半精度浮点（HDR 中间缓冲）
     *
     * @par 精简说明：
     *   相比 Godot 的 200+ 格式，仅保留 HUD 渲染所需的子集。
     *   去掉了 YUV/视频解码格式、ASTC HDR 变体、
     *   10/12 位打包格式等 HUD 场景不需要的格式。
     */
    enum class DataFormat : uint32_t
    {
        kR4G4UnormPack8 = 0,
        kR4G4B4A4UnormPack16 = 1,
        kB4G4R4A4UnormPack16 = 2,
        kR5G6B5UnormPack16 = 3,
        kB5G6R5UnormPack16 = 4,
        kR5G5B5A1UnormPack16 = 5,
        kB5G5R5A1UnormPack16 = 6,
        kA1R5G5B5UnormPack16 = 7,

        kR8Unorm = 8,
        kR8Snorm = 9,
        kR8Uint = 12,
        kR8Sint = 13,
        kR8Srgb = 14,

        kR8G8Unorm = 15,
        kR8G8Snorm = 16,
        kR8G8Uint = 19,
        kR8G8Sint = 20,
        kR8G8Srgb = 21,

        kR8G8B8Unorm = 22,
        kR8G8B8Snorm = 23,
        kR8G8B8Uint = 26,
        kR8G8B8Sint = 27,
        kR8G8B8Srgb = 28,

        kB8G8R8Unorm = 29,
        kB8G8R8Snorm = 30,
        kB8G8R8Uint = 33,
        kB8G8R8Sint = 34,
        kB8G8R8Srgb = 35,

        kR8G8B8A8Unorm = 36,
        kR8G8B8A8Snorm = 37,
        kR8G8B8A8Uint = 40,
        kR8G8B8A8Sint = 41,
        kR8G8B8A8Srgb = 42,

        kB8G8R8A8Unorm = 43,
        kB8G8R8A8Snorm = 44,
        kB8G8R8A8Uint = 47,
        kB8G8R8A8Sint = 48,
        kB8G8R8A8Srgb = 49,

        kA2R10G10B10UnormPack32 = 56,
        kA2B10G10R10UnormPack32 = 62,

        kR16Unorm = 69,
        kR16Snorm = 70,
        kR16Uint = 73,
        kR16Sint = 74,
        kR16Sfloat = 75,

        kR16G16Unorm = 76,
        kR16G16Snorm = 77,
        kR16G16Uint = 80,
        kR16G16Sint = 81,
        kR16G16Sfloat = 82,

        kR16G16B16A16Unorm = 91,
        kR16G16B16A16Snorm = 92,
        kR16G16B16A16Uint = 95,
        kR16G16B16A16Sint = 96,
        kR16G16B16A16Sfloat = 97,

        kR32Uint = 98,
        kR32Sint = 99,
        kR32Sfloat = 100,

        kR32G32Uint = 101,
        kR32G32Sint = 102,
        kR32G32Sfloat = 103,

        kR32G32B32Uint = 104,
        kR32G32B32Sint = 105,
        kR32G32B32Sfloat = 106,

        kR32G32B32A32Uint = 107,
        kR32G32B32A32Sint = 108,
        kR32G32B32A32Sfloat = 109,

        kB10G11R11UfloatPack32 = 121,
        kE5B9G9R9UfloatPack32 = 122,

        kD16Unorm = 123,
        kD32Sfloat = 124,
        kS8Uint = 125,
        kD16UnormS8Uint = 126,
        kD24UnormS8Uint = 127,
        kD32SfloatS8Uint = 128,

        kBc1RgbUnormBlock = 129,
        kBc1RgbSrgbBlock = 130,
        kBc1RgbaUnormBlock = 131,
        kBc1RgbaSrgbBlock = 132,
        kBc2UnormBlock = 133,
        kBc2SrgbBlock = 134,
        kBc3UnormBlock = 135,
        kBc3SrgbBlock = 136,
        kBc4UnormBlock = 137,
        kBc4SnormBlock = 138,
        kBc5UnormBlock = 139,
        kBc5SnormBlock = 140,
        kBc6HUfloatBlock = 141,
        kBc6HSfloatBlock = 142,
        kBc7UnormBlock = 143,
        kBc7SrgbBlock = 144,

        kEtc2R8G8B8UnormBlock = 145,
        kEtc2R8G8B8SrgbBlock = 146,
        kEtc2R8G8B8A1UnormBlock = 147,
        kEtc2R8G8B8A1SrgbBlock = 148,
        kEtc2R8G8B8A8UnormBlock = 149,
        kEtc2R8G8B8A8SrgbBlock = 150,

        kAstc4x4UnormBlock = 159,
        kAstc4x4SrgbBlock = 160,
        kAstc5x5UnormBlock = 165,
        kAstc5x5SrgbBlock = 166,
        kAstc6x6UnormBlock = 169,
        kAstc6x6SrgbBlock = 170,
        kAstc8x8UnormBlock = 175,
        kAstc8x8SrgbBlock = 176,

        kMax,
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 比较运算符
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 深度/模板比较运算符
     *
     * 对齐 Vulkan VkCompareOp 枚举值。
     * 用于深度测试、模板测试和采样器比较模式。
     */
    enum class CompareOperator : uint32_t
    {
        kNever = 0,
        kLess = 1,
        kEqual = 2,
        kLessOrEqual = 3,
        kGreater = 4,
        kNotEqual = 5,
        kGreaterOrEqual = 6,
        kAlways = 7,
        kMax
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 纹理
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 纹理类型
     *
     * 对齐 Vulkan VkImageViewType。
     */
    enum class TextureType : uint32_t
    {
        k1D,
        k2D,
        k3D,
        kCube,
        k1DArray,
        k2DArray,
        kCubeArray,
        kMax,
    };

    /**
     * @brief 纹理采样数（多重采样抗锯齿）
     *
     * 对齐 Vulkan VkSampleCountFlagBits。
     */
    enum class TextureSamples : uint32_t
    {
        k1 = 0,
        k2 = 1,
        k4 = 2,
        k8 = 3,
        k16 = 4,
        k32 = 5,
        k64 = 6,
        kMax,
    };

    /**
     * @brief 纹理用途标志位
     *
     * 对齐 Vulkan VkImageUsageFlagBits。
     * 使用 BitField<TextureUsageBits> 组合多个标志。
     *
     * @par AR HUD 典型组合：
     *   - 渲染目标：kColorAttachment | kSampling
     *   - 深度缓冲：kDepthStencilAttachment
     *   - 采样纹理：kSampling | kCanCopyFrom
     *   - Staging：kCanCopyTo | kCpuRead
     */
    enum class TextureUsageBits : uint32_t
    {
        kSampling = (1 << 0),
        kColorAttachment = (1 << 1),
        kDepthStencilAttachment = (1 << 2),
        kStorage = (1 << 3),
        kStorageAtomic = (1 << 4),
        kCpuRead = (1 << 5),
        kCanUpdate = (1 << 6),
        kCanCopyFrom = (1 << 7),
        kCanCopyTo = (1 << 8),
        kInputAttachment = (1 << 9),
        kTransient = (1 << 11),
        kDepthResolveAttachment = (1 << 12),
        kMaxBit = kDepthResolveAttachment,
    };

    /**
     * @brief 纹理格式描述
     */
    struct TextureFormat
    {
        DataFormat format = DataFormat::kR8Unorm;
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t depth = 1;
        uint32_t array_layers = 1;
        uint32_t mipmaps = 1;
        TextureType texture_type = TextureType::k2D;
        TextureSamples samples = TextureSamples::k1;
        BitField<TextureUsageBits> usage;
        bool is_resolve_buffer = false;
        bool is_discardable = false;
    };

    /**
     * @brief 纹理通道重映射（Swizzle）
     *
     * 对齐 Vulkan VkComponentSwizzle。
     */
    enum class TextureSwizzle : uint32_t
    {
        kIdentity,
        kZero,
        kOne,
        kR,
        kG,
        kB,
        kA,
        kMax
    };

    /**
     * @brief 纹理视图描述
     *
     * 创建纹理视图时可指定格式转换和通道重映射。
     */
    struct TextureView
    {
        DataFormat format = DataFormat::kMax;
        TextureSwizzle swizzle_r = TextureSwizzle::kR;
        TextureSwizzle swizzle_g = TextureSwizzle::kG;
        TextureSwizzle swizzle_b = TextureSwizzle::kB;
        TextureSwizzle swizzle_a = TextureSwizzle::kA;
    };

    /**
     * @brief 纹理布局（Vulkan 专用，OpenGL 可忽略）
     *
     * 对齐 Vulkan VkImageLayout。
     * OpenGL 驱动不需要布局转换，但为接口统一性保留。
     */
    enum class TextureLayout : uint32_t
    {
        kUndefined,
        kGeneral,
        kStorageOptimal,
        kColorAttachmentOptimal,
        kDepthStencilAttachmentOptimal,
        kDepthStencilReadOnlyOptimal,
        kShaderReadOnlyOptimal,
        kCopySrcOptimal,
        kCopyDstOptimal,
        kResolveSrcOptimal,
        kResolveDstOptimal,
        kMax
    };

    /**
     * @brief 纹理切面（颜色/深度/模板）
     */
    enum class TextureAspect : uint32_t
    {
        kColor = 0,
        kDepth = 1,
        kStencil = 2,
        kMax
    };

    /**
     * @brief 纹理切面标志位
     */
    enum class TextureAspectBits : uint32_t
    {
        kColorBit = (1 << 0),
        kDepthBit = (1 << 1),
        kStencilBit = (1 << 2),
    };

    /**
     * @brief 纹理子资源定位（单个 mip + layer + aspect）
     */
    struct TextureSubresource
    {
        TextureAspect aspect = TextureAspect::kColor;
        uint32_t layer = 0;
        uint32_t mipmap = 0;
    };

    /**
     * @brief 纹理子资源层（多个 layer + 单个 mip + aspect）
     */
    struct TextureSubresourceLayers
    {
        BitField<TextureAspectBits> aspect;
        uint32_t mipmap = 0;
        uint32_t base_layer = 0;
        uint32_t layer_count = 0;
    };

    /**
     * @brief 纹理子资源范围（多个 mip + 多个 layer + aspect）
     */
    struct TextureSubresourceRange
    {
        BitField<TextureAspectBits> aspect;
        uint32_t base_mipmap = 0;
        uint32_t mipmap_count = 0;
        uint32_t base_layer = 0;
        uint32_t layer_count = 0;
    };

    /**
     * @brief 纹理切片类型
     */
    enum class TextureSliceType : uint32_t
    {
        k2D,
        kCubemap,
        k3D,
        k2DArray,
        kMax
    };

    /**
     * @brief 纹理拷贝布局信息
     */
    struct TextureCopyableLayout
    {
        uint64_t size = 0;
        uint64_t row_pitch = 0;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 采样器
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 采样器过滤模式
     */
    enum class SamplerFilter : uint32_t
    {
        kNearest,
        kLinear,
    };

    /**
     * @brief 采样器寻址模式
     */
    enum class SamplerRepeatMode : uint32_t
    {
        kRepeat,
        kMirroredRepeat,
        kClampToEdge,
        kClampToBorder,
        kMirrorClampToEdge,
        kMax
    };

    /**
     * @brief 采样器边框颜色
     */
    enum class SamplerBorderColor : uint32_t
    {
        kFloatTransparentBlack,
        kIntTransparentBlack,
        kFloatOpaqueBlack,
        kIntOpaqueBlack,
        kFloatOpaqueWhite,
        kIntOpaqueWhite,
        kMax
    };

    /**
     * @brief 采样器状态描述
     */
    struct SamplerState
    {
        SamplerFilter mag_filter = SamplerFilter::kNearest;
        SamplerFilter min_filter = SamplerFilter::kNearest;
        SamplerFilter mip_filter = SamplerFilter::kNearest;
        SamplerRepeatMode repeat_u = SamplerRepeatMode::kClampToEdge;
        SamplerRepeatMode repeat_v = SamplerRepeatMode::kClampToEdge;
        SamplerRepeatMode repeat_w = SamplerRepeatMode::kClampToEdge;
        float lod_bias = 0.0f;
        bool use_anisotropy = false;
        float anisotropy_max = 1.0f;
        bool enable_compare = false;
        CompareOperator compare_op = CompareOperator::kAlways;
        float min_lod = 0.0f;
        float max_lod = 1e20f;
        SamplerBorderColor border_color = SamplerBorderColor::kFloatOpaqueBlack;
        bool unnormalized_uvw = false;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 顶点数组
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 索引缓冲区格式
     */
    enum class IndexBufferFormat : uint32_t
    {
        kUint16,
        kUint32,
    };

    /**
     * @brief 顶点属性频率
     */
    enum class VertexFrequency : uint32_t
    {
        kVertex,
        kInstance,
    };

    /**
     * @brief 顶点属性描述
     */
    struct VertexAttribute
    {
        uint32_t binding = UINT32_MAX;
        uint32_t location = 0;
        uint32_t offset = 0;
        DataFormat format = DataFormat::kMax;
        uint32_t stride = 0;
        VertexFrequency frequency = VertexFrequency::kVertex;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 帧缓冲
    // ═══════════════════════════════════════════════════════════════════════

    static constexpr int32_t kAttachmentUnused = -1;

    // ═══════════════════════════════════════════════════════════════════════
    // 着色器
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 着色器阶段
     *
     * 对齐 Vulkan VkShaderStageFlagBits 的位值，
     * 可作为 BitField 使用。
     */
    enum class ShaderStage : uint32_t
    {
        kVertex = 0,
        kFragment = 1,
        kTessellationControl = 2,
        kTessellationEvaluation = 3,
        kCompute = 4,
        kMax,
    };

    /**
     * @brief 着色器阶段源码对
     *
     * 描述一个着色器阶段及其对应的 GLSL 源码。
     * 用于 ShaderCreateFromGLSL 的多阶段输入，
     * 替代硬编码的 vertex_source + fragment_source 参数。
     *
     * @par 使用示例：
     *   @code
     *   LocalVector<ShaderStageSource> stages;
     *   stages.PushBack({ShaderStage::kVertex, kVertSrc});
     *   stages.PushBack({ShaderStage::kFragment, kFragSrc});
     *   rd->ShaderCreateFromGLSL(stages, uniforms, 64);
     *   @endcode
     */
    struct ShaderStageSource
    {
        ShaderStage stage = ShaderStage::kMax;
        const char *source = nullptr;
    };

    /**
     * @brief Uniform 类型
     *
     * 对齐 Godot UniformType 枚举。
     */
    enum class UniformType : uint32_t
    {
        kSampler,
        kSamplerWithTexture,
        kTexture,
        kImage,
        kTextureBuffer,
        kSamplerWithTextureBuffer,
        kImageBuffer,
        kUniformBuffer,
        kStorageBuffer,
        kInputAttachment,
        kUniformBufferDynamic,
        kStorageBufferDynamic,
        kMax
    };

    /**
     * @brief 着色器 Uniform 描述
     */
    struct ShaderUniform
    {
        UniformType type = UniformType::kMax;
        bool writable = false;
        uint32_t binding = 0;
        BitField<ShaderStage> stages;
        uint32_t length = 0;
    };

    /**
     * @brief 特化常量类型
     */
    enum class PipelineSpecializationConstantType : uint32_t
    {
        kBool,
        kInt,
        kFloat,
    };

    /**
     * @brief 管线特化常量
     */
    struct PipelineSpecializationConstant
    {
        PipelineSpecializationConstantType type = PipelineSpecializationConstantType::kBool;
        uint32_t constant_id = UINT32_MAX;
        union
        {
            uint32_t int_value = 0;
            float float_value;
            bool bool_value;
        };
    };

    /**
     * @brief 绑定 Uniform
     */
    struct BoundUniform
    {
        UniformType type = UniformType::kMax;
        uint32_t binding = UINT32_MAX;
        LocalVector<BufferID> ids;
        bool immutable_sampler = false;

        bool IsDynamic() const
        {
            return type == UniformType::kStorageBufferDynamic ||
                   type == UniformType::kUniformBufferDynamic;
        }
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 渲染管线
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 渲染图元类型
     */
    enum class RenderPrimitive : uint32_t
    {
        kPoints,
        kLines,
        kLinesWithAdjacency,
        kLineStrips,
        kLinesStripsWithAdjacency,
        kTriangles,
        kTrianglesWithAdjacency,
        kTriangleStrips,
        kTriangleStripsWithAdjacency,
        kTriangleStripsWithRestartIndex,
        kTessellationPatch,
        kMax
    };

    /**
     * @brief 多边形剔除模式
     */
    enum class PolygonCullMode : uint32_t
    {
        kDisabled,
        kFront,
        kBack,
        kMax
    };

    /**
     * @brief 多边形正面方向
     */
    enum class PolygonFrontFace : uint32_t
    {
        kClockwise,
        kCounterClockwise,
    };

    /**
     * @brief 模板操作
     */
    enum class StencilOperation : uint32_t
    {
        kKeep,
        kZero,
        kReplace,
        kIncrementAndClamp,
        kDecrementAndClamp,
        kInvert,
        kIncrementAndWrap,
        kDecrementAndWrap,
        kMax
    };

    /**
     * @brief 逻辑操作
     */
    enum class LogicOperation : uint32_t
    {
        kClear,
        kAnd,
        kAndReverse,
        kCopy,
        kAndInverted,
        kNoOp,
        kXor,
        kOr,
        kNor,
        kEquivalent,
        kInvert,
        kOrReverse,
        kCopyInverted,
        kOrInverted,
        kNand,
        kSet,
        kMax
    };

    /**
     * @brief 混合因子
     */
    enum class BlendFactor : uint32_t
    {
        kZero,
        kOne,
        kSrcColor,
        kOneMinusSrcColor,
        kDstColor,
        kOneMinusDstColor,
        kSrcAlpha,
        kOneMinusSrcAlpha,
        kDstAlpha,
        kOneMinusDstAlpha,
        kConstantColor,
        kOneMinusConstantColor,
        kConstantAlpha,
        kOneMinusConstantAlpha,
        kSrcAlphaSaturate,
        kSrc1Color,
        kOneMinusSrc1Color,
        kSrc1Alpha,
        kOneMinusSrc1Alpha,
        kMax
    };

    /**
     * @brief 混合操作
     */
    enum class BlendOperation : uint32_t
    {
        kAdd,
        kSubtract,
        kReverseSubtract,
        kMinimum,
        kMaximum,
        kMax
    };

    /**
     * @brief 管线光栅化状态
     */
    struct PipelineRasterizationState
    {
        bool enable_depth_clamp = false;
        bool discard_primitives = false;
        bool wireframe = false;
        PolygonCullMode cull_mode = PolygonCullMode::kDisabled;
        PolygonFrontFace front_face = PolygonFrontFace::kClockwise;
        bool depth_bias_enabled = false;
        float depth_bias_constant_factor = 0.0f;
        float depth_bias_clamp = 0.0f;
        float depth_bias_slope_factor = 0.0f;
        float line_width = 1.0f;
        uint32_t patch_control_points = 1;
    };

    /**
     * @brief 管线多重采样状态
     */
    struct PipelineMultisampleState
    {
        TextureSamples sample_count = TextureSamples::k1;
        bool enable_sample_shading = false;
        float min_sample_shading = 0.0f;
        bool enable_alpha_to_coverage = false;
        bool enable_alpha_to_one = false;
    };

    /**
     * @brief 模板操作状态
     */
    struct StencilOperationState
    {
        StencilOperation fail = StencilOperation::kZero;
        StencilOperation pass = StencilOperation::kZero;
        StencilOperation depth_fail = StencilOperation::kZero;
        CompareOperator compare = CompareOperator::kAlways;
        uint32_t compare_mask = 0;
        uint32_t write_mask = 0;
        uint32_t reference = 0;
    };

    /**
     * @brief 管线深度/模板状态
     */
    struct PipelineDepthStencilState
    {
        bool enable_depth_test = false;
        bool enable_depth_write = false;
        CompareOperator depth_compare_operator = CompareOperator::kAlways;
        bool enable_depth_range = false;
        float depth_range_min = 0.0f;
        float depth_range_max = 0.0f;
        bool enable_stencil = false;
        StencilOperationState front_op;
        StencilOperationState back_op;
    };

    /**
     * @brief 管线颜色混合附件状态
     */
    struct PipelineColorBlendAttachment
    {
        bool enable_blend = false;
        BlendFactor src_color_blend_factor = BlendFactor::kZero;
        BlendFactor dst_color_blend_factor = BlendFactor::kZero;
        BlendOperation color_blend_op = BlendOperation::kAdd;
        BlendFactor src_alpha_blend_factor = BlendFactor::kZero;
        BlendFactor dst_alpha_blend_factor = BlendFactor::kZero;
        BlendOperation alpha_blend_op = BlendOperation::kAdd;
        bool write_r = true;
        bool write_g = true;
        bool write_b = true;
        bool write_a = true;
    };

    /**
     * @brief 管线颜色混合状态
     */
    struct PipelineColorBlendState
    {
        bool enable_logic_op = false;
        LogicOperation logic_op = LogicOperation::kClear;
        LocalVector<PipelineColorBlendAttachment> attachments;
        float blend_constant_r = 0.0f;
        float blend_constant_g = 0.0f;
        float blend_constant_b = 0.0f;
        float blend_constant_a = 0.0f;

        static PipelineColorBlendState CreateDisabled(uint32_t p_attachments = 1)
        {
            PipelineColorBlendState state;
            for (uint32_t i = 0; i < p_attachments; ++i)
            {
                state.attachments.PushBack(PipelineColorBlendAttachment());
            }
            return state;
        }

        static PipelineColorBlendState CreateBlend(uint32_t p_attachments = 1)
        {
            PipelineColorBlendState state;
            for (uint32_t i = 0; i < p_attachments; ++i)
            {
                PipelineColorBlendAttachment att;
                att.enable_blend = true;
                att.src_color_blend_factor = BlendFactor::kSrcAlpha;
                att.dst_color_blend_factor = BlendFactor::kOneMinusSrcAlpha;
                att.src_alpha_blend_factor = BlendFactor::kSrcAlpha;
                att.dst_alpha_blend_factor = BlendFactor::kOneMinusSrcAlpha;
                state.attachments.PushBack(att);
            }
            return state;
        }
    };

    /**
     * @brief 管线动态状态标志位
     */
    enum class PipelineDynamicStateFlags : uint32_t
    {
        kLineWidth = (1 << 0),
        kDepthBias = (1 << 1),
        kBlendConstants = (1 << 2),
        kDepthBounds = (1 << 3),
        kStencilCompareMask = (1 << 4),
        kStencilWriteMask = (1 << 5),
        kStencilReference = (1 << 6),
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 渲染 Pass / 附件
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 附件加载操作
     */
    enum class AttachmentLoadOp : uint32_t
    {
        kLoad = 0,
        kClear = 1,
        kDontCare = 2,
    };

    /**
     * @brief 附件存储操作
     */
    enum class AttachmentStoreOp : uint32_t
    {
        kStore = 0,
        kDontCare = 1,
    };

    /**
     * @brief 附件描述
     */
    struct Attachment
    {
        DataFormat format = DataFormat::kMax;
        TextureSamples samples = TextureSamples::kMax;
        AttachmentLoadOp load_op = AttachmentLoadOp::kDontCare;
        AttachmentStoreOp store_op = AttachmentStoreOp::kDontCare;
        AttachmentLoadOp stencil_load_op = AttachmentLoadOp::kDontCare;
        AttachmentStoreOp stencil_store_op = AttachmentStoreOp::kDontCare;
        TextureLayout initial_layout = TextureLayout::kUndefined;
        TextureLayout final_layout = TextureLayout::kUndefined;
    };

    /**
     * @brief 附件引用
     */
    struct AttachmentReference
    {
        static constexpr uint32_t kUnused = 0xffffffff;
        uint32_t attachment = kUnused;
        TextureLayout layout = TextureLayout::kUndefined;
        BitField<TextureAspectBits> aspect;
    };

    /**
     * @brief 子 Pass 描述
     */
    struct Subpass
    {
        LocalVector<AttachmentReference> input_references;
        LocalVector<AttachmentReference> color_references;
        AttachmentReference depth_stencil_reference;
        AttachmentReference depth_resolve_reference;
        LocalVector<AttachmentReference> resolve_references;
        LocalVector<uint32_t> preserve_attachments;
    };

    /**
     * @brief 管线阶段标志位
     *
     * 对齐 Vulkan VkPipelineStageFlagBits。
     * 用于屏障命令指定源/目标管线阶段。
     */
    enum class PipelineStageBits : uint32_t
    {
        kTopOfPipe = (1 << 0),
        kDrawIndirect = (1 << 1),
        kVertexInput = (1 << 2),
        kVertexShader = (1 << 3),
        kTessellationControlShader = (1 << 4),
        kTessellationEvaluationShader = (1 << 5),
        kGeometryShader = (1 << 6),
        kFragmentShader = (1 << 7),
        kEarlyFragmentTests = (1 << 8),
        kLateFragmentTests = (1 << 9),
        kColorAttachmentOutput = (1 << 10),
        kComputeShader = (1 << 11),
        kCopy = (1 << 12),
        kBottomOfPipe = (1 << 13),
        kResolve = (1 << 14),
        kAllGraphics = (1 << 15),
        kAllCommands = (1 << 16),
        kClearStorage = (1 << 17),
    };

    /**
     * @brief 屏障访问标志位
     *
     * 对齐 Vulkan VkAccessFlagBits。
     * 用于屏障命令指定内存访问类型。
     */
    enum class BarrierAccessBits : uint32_t
    {
        kIndirectCommandRead = (1 << 0),
        kIndexRead = (1 << 1),
        kVertexAttributeRead = (1 << 2),
        kUniformRead = (1 << 3),
        kInputAttachmentRead = (1 << 4),
        kShaderRead = (1 << 5),
        kShaderWrite = (1 << 6),
        kColorAttachmentRead = (1 << 7),
        kColorAttachmentWrite = (1 << 8),
        kDepthStencilAttachmentRead = (1 << 9),
        kDepthStencilAttachmentWrite = (1 << 10),
        kCopyRead = (1 << 11),
        kCopyWrite = (1 << 12),
        kHostRead = (1 << 13),
        kHostWrite = (1 << 14),
        kMemoryRead = (1 << 15),
        kMemoryWrite = (1 << 16),
        kResolveRead = (1 << 25),
        kResolveWrite = (1 << 26),
        kStorageClear = (1 << 27),
    };

    /**
     * @brief 子 Pass 依赖
     */
    struct SubpassDependency
    {
        uint32_t src_subpass = 0xffffffff;
        uint32_t dst_subpass = 0xffffffff;
        BitField<PipelineStageBits> src_stages;
        BitField<PipelineStageBits> dst_stages;
        BitField<BarrierAccessBits> src_access;
        BitField<BarrierAccessBits> dst_access;
    };

    /**
     * @brief 渲染 Pass 清除值
     */
    union RenderPassClearValue
    {
        float color[4] = {};
        struct
        {
            float depth;
            uint32_t stencil;
        };

        RenderPassClearValue() {}
    };

    /**
     * @brief 附件清除操作
     */
    struct AttachmentClear
    {
        BitField<TextureAspectBits> aspect;
        uint32_t color_attachment = 0xffffffff;
        RenderPassClearValue value;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 屏障（Barrier）
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 全局内存屏障
     */
    struct MemoryAccessBarrier
    {
        BitField<BarrierAccessBits> src_access;
        BitField<BarrierAccessBits> dst_access;
    };

    /**
     * @brief 缓冲区屏障
     */
    struct BufferBarrier
    {
        BufferID buffer;
        BitField<BarrierAccessBits> src_access;
        BitField<BarrierAccessBits> dst_access;
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    /**
     * @brief 纹理屏障
     */
    struct TextureBarrier
    {
        TextureID texture;
        BitField<BarrierAccessBits> src_access;
        BitField<BarrierAccessBits> dst_access;
        TextureLayout prev_layout = TextureLayout::kUndefined;
        TextureLayout next_layout = TextureLayout::kUndefined;
        TextureSubresourceRange subresources;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 缓冲区
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 缓冲区用途标志位
     *
     * 对齐 Vulkan VkBufferUsageFlagBits。
     */
    enum class BufferUsageBits : uint32_t
    {
        kTransferFrom = (1 << 0),
        kTransferTo = (1 << 1),
        kTexel = (1 << 2),
        kUniform = (1 << 4),
        kStorage = (1 << 5),
        kIndex = (1 << 6),
        kVertex = (1 << 7),
        kIndirect = (1 << 8),
        kDeviceAddress = (1 << 17),
        kDynamicPersistent = (1u << 31),
    };

    static constexpr uint64_t kBufferWholeSize = ~0ULL;

    /**
     * @brief 内存分配类型
     */
    enum class MemoryAllocationType : uint32_t
    {
        kCpu,
        kGpu,
    };

    /**
     * @brief 缓冲区拷贝区域
     */
    struct BufferCopyRegion
    {
        uint64_t src_offset = 0;
        uint64_t dst_offset = 0;
        uint64_t size = 0;
    };

    /**
     * @brief 缓冲区-纹理拷贝区域
     */
    struct BufferTextureCopyRegion
    {
        uint64_t buffer_offset = 0;
        uint64_t row_pitch = 0;
        TextureSubresource texture_subresource;
        int32_t texture_offset_x = 0;
        int32_t texture_offset_y = 0;
        int32_t texture_offset_z = 0;
        uint32_t texture_region_width = 0;
        uint32_t texture_region_height = 0;
        uint32_t texture_region_depth = 0;
    };

    /**
     * @brief 纹理拷贝区域
     */
    struct TextureCopyRegion
    {
        TextureSubresourceLayers src_subresources;
        int32_t src_offset_x = 0;
        int32_t src_offset_y = 0;
        int32_t src_offset_z = 0;
        TextureSubresourceLayers dst_subresources;
        int32_t dst_offset_x = 0;
        int32_t dst_offset_y = 0;
        int32_t dst_offset_z = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 命令缓冲区
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 命令队列族标志位
     */
    enum class CommandQueueFamilyBits : uint32_t
    {
        kGraphics = 0x1,
        kCompute = 0x2,
        kTransfer = 0x4
    };

    /**
     * @brief 命令缓冲区类型
     */
    enum class CommandBufferType : uint32_t
    {
        kPrimary,
        kSecondary,
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 设备信息
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief GPU 设备类型
     *
     * 对齐 Vulkan VkPhysicalDeviceType。
     */
    enum class DeviceType : uint32_t
    {
        kOther,
        kIntegratedGpu,
        kDiscreteGpu,
        kVirtualGpu,
        kCpu,
        kMax
    };

    /**
     * @brief 驱动资源类型
     */
    enum class DriverResource : uint32_t
    {
        kLogicalDevice,
        kPhysicalDevice,
        kTopmostObject,
        kCommandQueue,
        kQueueFamily,
        kTexture,
        kTextureView,
        kTextureDataFormat,
        kSampler,
        kUniformSet,
        kBuffer,
        kComputePipeline,
        kRenderPipeline,
    };

    /**
     * @brief 设备限制查询项
     *
     * 精简版，仅保留 HUD 渲染所需的限制项。
     */
    enum class Limit : uint32_t
    {
        kMaxBoundUniformSets,
        kMaxFramebufferColorAttachments,
        kMaxTexturesPerUniformSet,
        kMaxSamplersPerUniformSet,
        kMaxStorageBuffersPerUniformSet,
        kMaxStorageImagesPerUniformSet,
        kMaxUniformBuffersPerUniformSet,
        kMaxDrawIndexedIndex,
        kMaxFramebufferHeight,
        kMaxFramebufferWidth,
        kMaxTextureArrayLayers,
        kMaxTextureSize2D,
        kMaxTextureSizeCube,
        kMaxTexturesPerShaderStage,
        kMaxSamplersPerShaderStage,
        kMaxStorageBuffersPerShaderStage,
        kMaxStorageImagesPerShaderStage,
        kMaxUniformBuffersPerShaderStage,
        kMaxPushConstantSize,
        kMaxUniformBufferSize,
        kMaxVertexInputAttributeOffset,
        kMaxVertexInputAttributes,
        kMaxVertexInputBindings,
        kMaxVertexInputBindingStride,
        kMinUniformBufferOffsetAlignment,
        kMaxComputeWorkgroupCountX,
        kMaxComputeWorkgroupCountY,
        kMaxComputeWorkgroupCountZ,
        kMaxComputeWorkgroupInvocations,
        kMaxComputeWorkgroupSizeX,
        kMaxComputeWorkgroupSizeY,
        kMaxComputeWorkgroupSizeZ,
        kMaxViewportDimensionsX,
        kMaxViewportDimensionsY,
        kMaxShaderVaryings,
        kMax
    };

    /**
     * @brief 设备特性查询项
     */
    enum class Features : uint32_t
    {
        kSupportsMultiview,
        kSupportsHalfFloat,
        kSupportsFragmentShaderWithOnlySideEffects,
        kSupportsBufferDeviceAddress,
        kSupportsFramebufferDepthResolve,
        kSupportsPointSize,
        kMax
    };

    /**
     * @brief API 特性查询项
     */
    enum class ApiTrait : uint32_t
    {
        kHonorsPipelineBarriers,
        kShaderChangeInvalidation,
        kTextureTransferAlignment,
        kTextureDataRowPitchStep,
        kSecondaryViewportScissor,
        kClearsWithCopyEngine,
        kUseGeneralInCopyQueues,
        kBuffersRequireTransitions,
        kTextureOutputsRequireClears,
    };

    /**
     * @brief 着色器变更失效模式
     */
    enum class ShaderChangeInvalidation : uint32_t
    {
        kAllBoundUniformSets,
        kIncompatibleSetsPlusCascade,
        kAllOrNoneAccordingToLayoutHash,
    };

    /**
     * @brief 设备族
     */
    enum class DeviceFamily : uint32_t
    {
        kUnknown,
        kOpenGL,
        kVulkan,
        kDirectX,
        kMetal,
    };

    /**
     * @brief 驱动能力描述
     */
    struct Capabilities
    {
        DeviceFamily device_family = DeviceFamily::kUnknown;
        uint32_t version_major = 1;
        uint32_t version_minor = 0;
    };

    /**
     * @brief 多视图能力
     */
    struct MultiviewCapabilities
    {
        bool is_supported = false;
        bool geometry_shader_is_supported = false;
        bool tessellation_shader_is_supported = false;
        uint32_t max_view_count = 0;
        uint32_t max_instance_count = 0;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 资源用途（RDG 用）
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 资源使用方式（用于渲染命令图 RDG 的依赖追踪）
     *
     * 对齐 Godot rendering_device_graph.h 中的 ResourceUsage 枚举。
     */
    enum class ResourceUsage : uint32_t
    {
        kNone,
        kCopyFrom,
        kCopyTo,
        kTextureSample,
        kAttachmentColorReadWrite,
        kAttachmentDepthStencilReadWrite,
        kStorageImageReadWrite,
        kMax
    };

} // namespace arhud
