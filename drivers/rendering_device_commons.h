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

#ifndef ARHUD_DRIVERS_RENDERING_DEVICE_COMMONS_H_
#define ARHUD_DRIVERS_RENDERING_DEVICE_COMMONS_H_

#include "template/bitfield.h"
#include "template/local_vector.h"
#include "template/vector_view.h"
#include "typedefs.h"

namespace arhud {

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
 * 每个 ID 类型包装一个 uint64，提供类型安全的比较和有效性检查。
 * 可从原生指针构造（OpenGL 驱动用 GLuint 指针映射）。
 *
 * @param m_name ID 类型名称前缀
 */
#define ARHUD_DEFINE_RDD_ID(m_name)                                        \
  class m_name##ID {                                                       \
    uint64 id_ = 0;                                                        \
                                                                           \
   public:                                                                 \
    ARHUD_ALWAYS_INLINE m_name##ID() = default;                            \
    ARHUD_ALWAYS_INLINE explicit m_name##ID(uint64 p_id) : id_(p_id) {}    \
    ARHUD_ALWAYS_INLINE explicit m_name##ID(void* p_ptr)                   \
        : id_(reinterpret_cast<uint64>(p_ptr)) {}                          \
    ARHUD_ALWAYS_INLINE explicit operator bool() const {                   \
      return id_ != 0;                                                     \
    }                                                                      \
    ARHUD_ALWAYS_INLINE bool operator==(const m_name##ID& p_other) const { \
      return id_ == p_other.id_;                                           \
    }                                                                      \
    ARHUD_ALWAYS_INLINE bool operator!=(const m_name##ID& p_other) const { \
      return id_ != p_other.id_;                                           \
    }                                                                      \
    ARHUD_ALWAYS_INLINE bool operator<(const m_name##ID& p_other) const {  \
      return id_ < p_other.id_;                                            \
    }                                                                      \
    ARHUD_ALWAYS_INLINE uint64 GetId() const {                             \
      return id_;                                                          \
    }                                                                      \
    ARHUD_ALWAYS_INLINE bool IsValid() const {                             \
      return id_ != 0;                                                     \
    }                                                                      \
    ARHUD_ALWAYS_INLINE bool IsNull() const {                              \
      return id_ == 0;                                                     \
    }                                                                      \
  }

// ═══════════════════════════════════════════════════════════════════════
// 资源 ID 类型声明（前置声明，供后续结构体引用）
// ═══════════════════════════════════════════════════════════════════════
//
// 各 *ID 类由 ARHUD_DEFINE_RDD_ID 生成（强类型 uint64 包装），
// 编译期隔离不同资源 ID 的误传（BufferID 传不进 TextureID 形参）。
// 跨后端封装差异：
//   - GL：GLuint 对象名（Buffer/Texture/Sampler/Framebuffer），
//         或簿记结构指针（Shader/UniformSet/RenderPass/Pipeline
//         等 PSO 模拟类资源）
//   - Vulkan（规划）：驱动原生句柄（VkBuffer/VkImage/...）
// 通用契约：默认构造为空 ID；IsValid() 即 id != 0。

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

/** @brief 通用无效索引哨兵（-1） */
static constexpr int32 kInvalidId = -1;

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
enum class DataFormat : uint32 {
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
 * @par 消费方
 *   深度测试（PipelineDepthStencilState）、模板测试
 *   （StencilOperationState::compare）、采样器比较模式
 *   （SamplerState::enable_compare + compare_op，阴影贴图
 *   PCF 的 GL_TEXTURE_COMPARE_MODE 路径）。
 *
 * @par 后端映射
 *   枚举值对齐 Vulkan VkCompareOp；GL 经 CompareOperatorToGL()
 *   映射 GL_NEVER..GL_ALWAYS 八值一一对应。
 */
enum class CompareOperator : uint32 {
  /**
   * @brief 永不通过（恒 false）
   *
   * 无论比较双方取值如何，判定结果恒为"测试失败"。
   * @par GL 映射
   *   GL_NEVER
   * @par 典型用途
   *   临时屏蔽某测试（关闭深度/模板判定）而不必移除管线状态；
   *   调试用"完全禁用"开关。
   */
  kNever = 0,

  /**
   * @brief 小于时通过：新值 < 参考值
   *
   * 待测值（深度/模板/采样结果）严格小于参考值时测试通过。
   * @par GL 映射
   *   GL_LESS
   * @par 典型用途
   *   深度测试的默认"近者胜"——常规深度缓冲中深度值越小越近，
   *   小于即新片元更靠前、允许覆盖（min-z 语义）。
   */
  kLess = 1,

  /**
   * @brief 等于时通过：新值 == 参考值
   *
   * 待测值与参考值精确相等时测试通过。
   * @par GL 映射
   *   GL_EQUAL
   * @par 典型用途
   *   模板测试的等值判定（如"仅绘制被标记为 N 的像素"）；
   *   深度精确相等在浮点下罕见，一般配 compare_mask 做模板筛选。
   */
  kEqual = 2,

  /**
   * @brief 小于等于时通过：新值 <= 参考值
   *
   * 待测值小于或等于参考值时测试通过（含相等边界）。
   * @par GL 映射
   *   GL_LEQUAL
   * @par 典型用途
   *   深度预渲染后再叠加（描边/贴花）的"同深度可绘制"；
   *   阴影映射（shadow map）深度判定的常见配置。
   */
  kLessOrEqual = 3,

  /**
   * @brief 大于时通过：新值 > 参考值
   *
   * 待测值严格大于参考值时测试通过。
   * @par GL 映射
   *   GL_GREATER
   * @par 典型用途
   *   反转深度（reverse-Z）约定下的"近者胜"——大值表示更近时
   *   用它替代 kLess，提升远处深度精度。
   */
  kGreater = 4,

  /**
   * @brief 不等于时通过：新值 != 参考值
   *
   * 待测值与参考值不相等时测试通过（kEqual 的否定）。
   * @par GL 映射
   *   GL_NOTEQUAL
   * @par 典型用途
   *   模板通道筛选——跳过已被特定标记的像素（如已经在别处
   *   绘制的区域不再重叠渲染）。
   */
  kNotEqual = 5,

  /**
   * @brief 大于等于时通过：新值 >= 参考值
   *
   * 待测值大于或等于参考值时测试通过（含相等边界）。
   * @par GL 映射
   *   GL_GEQUAL
   * @par 典型用途
   *   反转深度（reverse-Z）下与 kGreater 配套的"同深度可绘制"。
   */
  kGreaterOrEqual = 6,

  /**
   * @brief 恒通过（恒 true）
   *
   * 无论比较双方取值如何，判定结果恒为"测试通过"。
   * @par GL 映射
   *   GL_ALWAYS
   * @par 典型用途
   *   默认值——SamplerState::compare_op 与
   *   StencilOperationState::compare 均以 kAlways 为缺省，
   *   表示未启用比较筛选；亦用于"始终写入"的模板覆盖操作。
   */
  kAlways = 7,

  /**
   * @brief 哨兵值：运算符数量上限
   *
   * 非合法运算符，仅用于数组定界与边界检查（如 GL 查找表
   * 尺寸、range-for 上界）。
   */
  kMax
};

// ═══════════════════════════════════════════════════════════════════════
// 纹理
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 纹理维度/形态类型
 *
 * @par 后端映射
 *   对齐 Vulkan VkImageViewType；GL 经 target 映射
 *   GL_TEXTURE_1D/2D/3D/CUBE/1D_ARRAY/2D_ARRAY/
 *   CUBE_MAP_ARRAY —— TextureCreate 按此选 target 并登记
 *   texture_infos_（MSAA 一律 2D_MULTISAMPLE 覆盖此选择）。
 *
 * @note HUD 场景以 k2D 绝对主导（画布/字体图集/深度）；
 *       数组与 Cube 族保留给图集分层与立方环境。
 */
enum class TextureType : uint32 {
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
 * @brief 纹理多重采样数（MSAA）
 *
 * @par 语义
 *   每像素采样数。k1 = 非 MSAA（常规纹理）；k2..k64 为
 *   MSAA 纹理 —— 须作 FBO 附件渲染，采样读取前需 resolve
 *   到 k1 纹理。枚举值对齐 VkSampleCountFlagBits 位值。
 *
 * @par GL 映射
 *   MSAA 强制 GL_TEXTURE_2D_MULTISAMPLE target
 *   （samples 参数透传 glTexImage2DMultisample）；
 *   实际支持上限由驱动定（GL_MAX_SAMPLES 查询）。
 */
enum class TextureSamples : uint32 {
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
enum class TextureUsageBits : uint32 {
  /**
   * @brief 可被着色器采样读取
   *
   * 纹理可作为采样源（sampler2D 等）在着色器中读取。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_SAMPLED_BIT；
   *   GL：常规 glBindTexture + 采样参数（无需额外用途声明）。
   * @par 典型用途
   *   HUD 图标、字体图集、已渲染完成待采样复用的中间结果。
   */
  kSampling = (1 << 0),

  /**
   * @brief 可作颜色附件（渲染目标）
   *
   * 纹理可挂载为 Framebuffer 的颜色附件被写入。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT；
   *   GL：glFramebufferTexture2D 挂到 GL_COLOR_ATTACHMENT0+N。
   * @par 典型用途
   *   离屏渲染目标（模式 R 回读路径的目标纹理）、合成输出。
   */
  kColorAttachment = (1 << 1),

  /**
   * @brief 可作深度/模板附件
   *
   * 纹理可挂载为 Framebuffer 的深度（或深度+模板）附件。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT；
   *   GL：挂到 GL_DEPTH_ATTACHMENT / GL_DEPTH_STENCIL_ATTACHMENT。
   * @par 典型用途
   *   HUD 深度缓冲（kD32Sfloat / kD24UnormS8Uint 格式）。
   */
  kDepthStencilAttachment = (1 << 2),

  /**
   * @brief 可作存储图像（读写）
   *
   * 纹理可作为 image 对象被着色器读写（image load/store）。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_STORAGE_BIT；
   *   GL：glBindImageTexture 绑定到 image unit（GL 4.2+）。
   * @par 典型用途
   *   计算着色器直接写纹理、图像后处理。
   */
  kStorage = (1 << 3),

  /**
   * @brief 存储图像支持原子操作
   *
   * 在 kStorage 基础上额外允许原子读写（imageAtomic* 族）。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_STORAGE_BIT + 格式的原子能力；
   *   GL：imageAtomic* 需驱动与格式支持原子。
   * @note 与 kStorage 配套使用；HUD 常规路径不启用。
   */
  kStorageAtomic = (1 << 4),

  /**
   * @brief CPU 可读取
   *
   * 纹理内容可被 CPU 侧读取（回读）。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_HOST_TRANSFER_BIT 族；
   *   GL：glGetTexImage / glReadPixels（到 PBO 或映射）。
   * @par 典型用途
   *   模式 R 回读呈现路径——渲染结果回读到 CPU 后交 IFramePresenter。
   * @warning 回读有 CPU-GPU 同步开销，属 dev 过渡路径。
   */
  kCpuRead = (1 << 5),

  /**
   * @brief 可被 CPU 侧更新
   *
   * 允许通过上传接口改写纹理内容。
   * @par 后端映射
   *   GL：glTexSubImage2D / BufferUpdate 路径上传；
   *   Vulkan：VK_IMAGE_USAGE_TRANSFER_DST_BIT。
   * @par 典型用途
   *   动态纹理内容更新（每帧变化的 HUD 数据纹理）。
   */
  kCanUpdate = (1 << 6),

  /**
   * @brief 可作拷贝源
   *
   * 纹理内容可被拷贝到其它纹理 / 缓冲区。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_TRANSFER_SRC_BIT；
   *   GL：glCopyImageSubData / glGetTexImage 的源侧。
   * @par 典型用途
   *   采样纹理（见文件头组合 kSampling | kCanCopyFrom）。
   */
  kCanCopyFrom = (1 << 7),

  /**
   * @brief 可作拷贝目标
   *
   * 纹理可接收来自其它纹理 / 缓冲区的拷贝写入。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_TRANSFER_DST_BIT；
   *   GL：glCopyImageSubData / glTexSubImage* 的目的侧。
   * @par 典型用途
   *   Staging 上传目标（见文件头组合 kCanCopyTo | kCpuRead）。
   */
  kCanCopyTo = (1 << 8),

  /**
   * @brief 可作输入附件
   *
   * 纹理可在同一 RenderPass 的后续子 Pass 中作为只读输入附件引用
   * （G-buffer 延迟渲染的读回通道）。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT；
   *   GL：无子 Pass 概念，退化为普通纹理采样。
   */
  kInputAttachment = (1 << 9),

  // 位 10（1 << 10）未使用：Vulkan 对应视频解码用途
  // （VK_IMAGE_USAGE_VIDEO_DECODE_*），HUD 场景不需要，故留空。

  /**
   * @brief 瞬态附件
   *
   * 内容无需跨 Pass 保留，驱动可据此优化（tile 架构省写回带宽）。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT；
   *   GL：无对应概念，忽略。
   * @par 典型用途
   *   仅被后续 Pass 覆写的中间缓冲。
   */
  kTransient = (1 << 11),

  /**
   * @brief 可作深度解析目标附件
   *
   * 承接 MSAA 深度附件的 resolve 输出（多重采样深度解析）。
   * @par 后端映射
   *   Vulkan：VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT 的
   *   resolve 形态（配合 Attachment::depth_resolve_reference）；
   *   GL：需 ARB_framebuffer_depth_resolve 类扩展。
   * @par 典型用途
   *   MSAA 渲染后的单采样深度读取。
   */
  kDepthResolveAttachment = (1 << 12),

  /**
   * @brief 哨兵值：最高有效位
   *
   * 非用途标志，等于本枚举中最大的合法位（kDepthResolveAttachment），
   * 供位掩码遍历 / 边界检查使用。
   */
  kMaxBit = kDepthResolveAttachment,
};

/**
 * @brief 纹理格式描述
 */
struct TextureFormat {
  DataFormat format =
      DataFormat::kR8Unorm;  ///< 像素数据格式（如
                             ///< kR8Unorm、kRGBA8Unorm、kDepth24Stencil8）
  uint32 width = 1;          ///< 纹理宽度（像素）
  uint32 height = 1;         ///< 纹理高度（像素）
  uint32 depth = 1;          ///< 纹理深度（仅 3D 纹理有效，层数）
  uint32 array_layers = 1;   ///< 数组层数（仅数组纹理 / CubeMap 有效）
  uint32 mipmaps = 1;        ///< Mipmap 级数（1 = 仅基础层）
  TextureType texture_type =
      TextureType::k2D;  ///< 纹理类型（1D/2D/3D/Array/CubeMap）
  TextureSamples samples =
      TextureSamples::k1;  ///< 多重采样数（1/2/4/8/16/32/64）
  BitField<TextureUsageBits>
      usage;                       ///< 使用方式位掩码（采样、渲染目标、存储等）
  bool is_resolve_buffer = false;  ///< 是否为多重采样解析缓冲区
  bool is_discardable =
      false;  ///< 是否可丢弃（内容无需跨帧保留，允许驱动优化）
};

/**
 * @brief 纹理通道重映射（Swizzle）
 *
 * @par 语义
 *   采样输出前对 RGBA 四通道重布线：kZero/kOne 写常量，
 *   kR/kG/kB/kA 把指定通道复制到本位置（kIdentity 原样）。
 *   典型用途：单通道灰度按 (r,r,r,1) 读、BGRA 存储反转成
 *   RGBA 输出。
 *
 * @par 后端映射
 *   对齐 Vulkan VkComponentSwizzle；GL 无独立视图对象，
 *   经 glTexParameter(GL_TEXTURE_SWIZZLE_RGBA) 在纹理对象
 *   上直接生效。
 */
enum class TextureSwizzle : uint32 {
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
 * @brief 纹理视图描述（同一纹理的"另一种读法"）
 *
 * @par 语义
 *   对既有纹理声明替代读法：format 重解读（如把 R16G16
 *   当两个 R8 读）+ 四通道 Swizzle。视图不复制数据，同一
 *   底层纹理可挂多个视图。
 *
 * @par 后端映射
 *   Vulkan：真视图对象（VkImageView，格式+Swizzle 完整
 *   支持）；GL：无独立视图 —— TextureView 参数当前被
 *   TextureCreate 忽略，Swizzle 能力经纹理对象参数近似。
 *
 * @note format = kMax（默认）表示沿用纹理自身格式。
 */
struct TextureView {
  DataFormat format = DataFormat::kMax;  ///< 视图格式（可不同于纹理格式）
  TextureSwizzle swizzle_r = TextureSwizzle::kR;  ///< R 通道重映射
  TextureSwizzle swizzle_g = TextureSwizzle::kG;  ///< G 通道重映射
  TextureSwizzle swizzle_b = TextureSwizzle::kB;  ///< B 通道重映射
  TextureSwizzle swizzle_a = TextureSwizzle::kA;  ///< A 通道重映射
};

/**
 * @brief 纹理内存布局（Vulkan 布局转换专用）
 *
 * @par 语义
 *   纹理在各使用阶段的内存排布（颜色/深度附件专属、采样
 *   只读、拷贝源/目的等）。Vulkan 驱动在屏障与 RenderPass
 *   边界按 initial/final 布局做隐式转换。对齐
 *   VkImageLayout。
 *
 * @par GL 策略
 *   GL 无布局概念（纹理全程通用排布）—— Attachment 的
 *   initial/final_layout 与 AttachmentReference::layout
 *   在 GL 后端全部忽略，纯 Vulkan 契约保留。
 */
enum class TextureLayout : uint32 {
  /**
   * @brief 内容未定义
   *
   * 纹理既有内容不确定/不关心。Vulkan 在转换时允许丢弃旧内容，
   * 是最廉价的布局转换。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_UNDEFINED
   * @par 典型用途
   *   首次作为附件写入（无历史内容需保留）时的 initial_layout。
   */
  kUndefined,

  /**
   * @brief 通用布局
   *
   * 支持任意读写用途（采样、读写、附件均可），但无针对性优化。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_GENERAL
   * @par 典型用途
   *   同一时刻承担多重角色、无法归入单一最优布局的纹理。
   */
  kGeneral,

  /**
   * @brief 存储图像最优布局
   *
   * 供 image load/store 读写的最优排布，语义上等同"通用"，
   * 单独列出以便 GL 后端原样透传。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_GENERAL
   * @par 典型用途
   *   计算着色器直接读写的存储图像（kStorage 用途）。
   */
  kStorageOptimal,

  /**
   * @brief 颜色附件最优布局
   *
   * 作为颜色渲染目标被写入时的最优排布。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
   * @par 典型用途
   *   离屏颜色目标在 RenderPass 内的布局（final_layout 或
   *   AttachmentReference::layout）。
   */
  kColorAttachmentOptimal,

  /**
   * @brief 深度/模板附件最优布局
   *
   * 作为深度（或深度+模板）附件可读写时的最优排布。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
   * @par 典型用途
   *   深度测试同时读写深度缓冲的 RenderPass。
   */
  kDepthStencilAttachmentOptimal,

  /**
   * @brief 深度/模板只读最优布局
   *
   * 深度/模板仅被读取（不再写入）时的最优排布。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
   * @par 典型用途
   *   深度预渲染完成后，后续 Pass 只采样不写入深度。
   */
  kDepthStencilReadOnlyOptimal,

  /**
   * @brief 着色器只读最优布局
   *
   * 供着色器采样读取的最优排布——绝大多数纹理最终停放于此。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
   * @par 典型用途
   *   HUD 图标/字体图集、已渲染结果转采样输入。
   */
  kShaderReadOnlyOptimal,

  /**
   * @brief 拷贝源最优布局
   *
   * 作为 transfer 拷贝源时的最优排布。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
   * @par 典型用途
   *   TextureCopy 的源纹理（如回读前的目标纹理）。
   */
  kCopySrcOptimal,

  /**
   * @brief 拷贝目标最优布局
   *
   * 作为 transfer 拷贝目的时的最优排布。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
   * @par 典型用途
   *   TextureCopy 的目的纹理、Staging 上传落点。
   */
  kCopyDstOptimal,

  /**
   * @brief 解析源最优布局
   *
   * MSAA 附件作为 resolve 源时的排布（pseudo-layout，
   * 底层复用传输布局）。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
   * @par 典型用途
   *   多重采样附件被 resolve 读取。
   */
  kResolveSrcOptimal,

  /**
   * @brief 解析目标最优布局
   *
   * MSAA 附件作为 resolve 目的时的排布（pseudo-layout，
   * 底层复用传输布局）。
   * @par Vulkan 布局
   *   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
   * @par 典型用途
   *   多重采样附件 resolve 输出的单采样纹理。
   */
  kResolveDstOptimal,

  /**
   * @brief 哨兵值：布局数量上限
   *
   * 非合法布局，仅用于数组定界与边界检查。
   */
  kMax
};

/**
 * @brief 纹理切面（颜色/深度/模板三选一）
 *
 * 深度+模板组合格式含 D 与 S 两个独立位面：拷贝/屏障/清除
 * 须指明操作哪个面；颜色格式恒为 kColor。单值形态对齐
 * VkImageAspectFlagBits 的常用子集。
 */
enum class TextureAspect : uint32 {
  kColor = 0,
  kDepth = 1,
  kStencil = 2,
  kMax
};

/**
 * @brief 纹理切面标志位（可组合）
 *
 * TextureSubresource/Layers/Range 的 aspect 字段承载类型：
 * BitField 组合（如 kDepthBit|kStencilBit 同指深度与模板
 * 面，对应清除/屏障的联合操作）。对齐 VkImageAspectFlags。
 */
enum class TextureAspectBits : uint32 {
  kColorBit = (1 << 0),
  kDepthBit = (1 << 1),
  kStencilBit = (1 << 2),
};

/**
 * @brief 纹理子资源定位（单 layer + 单 mip + 单 aspect）
 *
 * 最细粒度定位 —— 唯一确定一个 mip 级/数组层/位面，
 * 供指定层级上传/回读类操作使用。
 */
struct TextureSubresource {
  TextureAspect aspect = TextureAspect::kColor;  ///< 颜色/深度/模板切面
  uint32 layer = 0;                              ///< 纹理数组层索引
  uint32 mipmap = 0;                             ///< mipmap 层级索引
};

/**
 * @brief 纹理子资源层组（多 layer + 单 mip + aspect 组合）
 *
 * 拷贝命令的描述形态（TextureCopyRegion 的 src/dst_
 * subresources）：一次搬运覆盖 [base_layer, base_layer +
 * layer_count) 的连续数组层。对齐 VkImageSubresourceLayers。
 */
struct TextureSubresourceLayers {
  BitField<TextureAspectBits> aspect;  ///< 颜色/深度/模板切面标志
  uint32 mipmap = 0;                   ///< mipmap 层级索引
  uint32 base_layer = 0;               ///< 起始数组层索引
  uint32 layer_count = 0;              ///< 数组层数量
};

/**
 * @brief 纹理子资源范围（多 mip × 多 layer × aspect 组合）
 *
 * 屏障（TextureBarrier::subresources）与布局转换的描述
 * 形态：mip 与 layer 各自带起点+数量，覆盖一个矩形子域。
 * 对齐 VkImageSubresourceRange。
 */
struct TextureSubresourceRange {
  BitField<TextureAspectBits> aspect;  ///< 颜色/深度/模板切面标志
  uint32 base_mipmap = 0;              ///< 起始 mipmap 层级索引
  uint32 mipmap_count = 0;             ///< mipmap 层级数量
  uint32 base_layer = 0;               ///< 起始数组层索引
  uint32 layer_count = 0;              ///< 数组层数量
};

/**
 * @brief 纹理切片族判定
 *
 * 拷贝/传输路径判定源或目标纹理的切片形态（普通 2D /
 * Cubemap 面 / 3D 切片 / 数组层）—— 驱动据此组织 buffer
 * 侧数据的内存布局。对齐 Godot RD 的 TextureSliceType。
 */
enum class TextureSliceType : uint32 { k2D, kCubemap, k3D, k2DArray, kMax };

/**
 * @brief 纹理拷贝布局查询结果
 *
 * CPU↔GPU 搬运前查询目标子资源的内存需求：
 *   - size：该子资源总字节数（分配 staging 的依据）
 *   - row_pitch：单行字节数（GPU 行可能有对齐 padding，
 *     与 CPU 紧凑布局的差额由调用方垫齐）
 */
struct TextureCopyableLayout {
  uint64 size = 0;       ///< 子资源数据总大小（字节）
  uint64 row_pitch = 0;  ///< 单行字节数
};

// ═══════════════════════════════════════════════════════════════════════
// 采样器
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 采样器过滤模式（单级）
 *
 * kNearest：最近邻（像素风/精确取值）；kLinear：线性插值。
 * mag/min/mip 三维度各自独立（见 SamplerState）；min 维度
 * 与 mip 维度由 SamplerCreate 拼装出 GL 的
 * *_MIPMAP_* 复合过滤模式。
 */
enum class SamplerFilter : uint32 {
  /**
   * @brief 最近邻过滤
   *
   * 取与采样坐标最接近的单个 texel，不做任何插值。
   * @par GL 映射
   *   GL_NEAREST
   * @par 典型用途
   *   像素对齐的 HUD 图标/位图字体（保持锐利、避免糊边）；
   *   需要精确取值的查找表类纹理。
   */
  kNearest,

  /**
   * @brief 线性过滤
   *
   * 在相邻 texel 间做线性插值（2D 即双线性）。
   * @par GL 映射
   *   GL_LINEAR
   * @par 典型用途
   *   纹理缩放/旋转时的平滑显示、渐变类内容。
   */
  kLinear,
};

/**
 * @brief 采样器寻址模式（UVW 越界处理）
 *
 * 经 SamplerRepeatModeToGL 映射 GL_WRAP 族；U/V/W 三轴向
 * 可各自独立指定（见 SamplerState）。
 *
 * @note kMirrorClampToEdge（GL_MIRROR_CLAMP_TO_EDGE）为
 *       GL 4.4 核心特性，GL 3.3 需
 *       ARB_texture_mirror_clamp_to_edge 扩展；
 *       kClampToBorder 需另行下发边框色
 *       （SamplerState::border_color）。
 */
enum class SamplerRepeatMode : uint32 {
  /**
   * @brief 平铺重复
   *
   * 采样坐标超出 [0,1) 时取整数部分循环，纹理无缝平铺。
   * @par GL 映射
   *   GL_REPEAT
   * @par 典型用途
   *   需要周期平铺的图案（网格、噪声、地面纹理）。
   */
  kRepeat,

  /**
   * @brief 镜像重复
   *
   * 每跨越一个整数边界就翻转一次，平铺时相邻块镜像相接。
   * @par GL 映射
   *   GL_MIRRORED_REPEAT
   * @par 典型用途
   *   平铺处希望接缝连续、不产生突变时。
   */
  kMirroredRepeat,

  /**
   * @brief 钳制到边缘
   *
   * 越界坐标被钳到最近的边缘 texel，边缘内容向外拉伸。
   * @par GL 映射
   *   GL_CLAMP_TO_EDGE
   * @par 典型用途
   *   HUD 默认寻址——单张纹理不做平铺，避免采到重复内容。
   */
  kClampToEdge,

  /**
   * @brief 钳制到边框色
   *
   * 越界采样返回固定边框颜色（由 SamplerState::border_color 指定）。
   * @par GL 映射
   *   GL_CLAMP_TO_BORDER（需另下发 GL_TEXTURE_BORDER_COLOR）
   * @par 典型用途
   *   阴影贴图——越界区域返回约定值（如"完全被照亮"）。
   */
  kClampToBorder,

  /**
   * @brief 镜像后钳制到边缘
   *
   * 先对坐标镜像一次，再钳到边缘（仅在跨越处镜像一次）。
   * @par GL 映射
   *   GL_MIRROR_CLAMP_TO_EDGE（GL 4.4 核心特性；
   *   GL 3.3 需 ARB_texture_mirror_clamp_to_edge 扩展）
   * @par 典型用途
   *   需要单次镜像过渡、又不想周期平铺的边界处理。
   */
  kMirrorClampToEdge,

  /**
   * @brief 哨兵值：寻址模式数量上限
   *
   * 非合法寻址模式，仅用于数组定界与边界检查。
   */
  kMax
};

/**
 * @brief 采样器边框颜色（kClampToBorder 越界填充色）
 *
 * 六预设对齐 Vulkan VkBorderColor；GL 侧经
 * glSamplerParameterfv(GL_TEXTURE_BORDER_COLOR) 设置
 * （int/float 变体按附件格式族选择）。缺省透明黑。
 */
enum class SamplerBorderColor : uint32 {
  /**
   * @brief 浮点·透明黑 (0,0,0,0)
   *
   * 供浮点格式族使用（GL 经 glSamplerParameterfv 下发）。
   * @par 典型用途
   *   缺省边框色——越界处视作"无内容"。
   */
  kFloatTransparentBlack,

  /**
   * @brief 整数·透明黑
   *
   * 供整数格式族使用（GL 经 glSamplerParameterIiv 下发），
   * 语义与 kFloatTransparentBlack 对应的整数值一致。
   * @par 典型用途
   *   整数纹理（如 ID/R32UI）越界返回透明黑。
   */
  kIntTransparentBlack,

  /**
   * @brief 浮点·不透明黑 (0,0,0,1)
   *
   * 供浮点格式族使用。alpha 为 1，越界处不透明。
   * @par 典型用途
   *   需要越界表现为实心黑边界的场景。
   */
  kFloatOpaqueBlack,

  /**
   * @brief 整数·不透明黑
   *
   * 供整数格式族使用，语义与 kFloatOpaqueBlack 对应的整数值一致。
   */
  kIntOpaqueBlack,

  /**
   * @brief 浮点·不透明白 (1,1,1,1)
   *
   * 供浮点格式族使用。
   * @par 典型用途
   *   阴影贴图越界返回"完全被照亮"的约定值。
   */
  kFloatOpaqueWhite,

  /**
   * @brief 整数·不透明白
   *
   * 供整数格式族使用，语义与 kFloatOpaqueWhite 对应的整数值一致。
   */
  kIntOpaqueWhite,

  /**
   * @brief 哨兵值：边框色数量上限
   *
   * 非合法边框色，仅用于数组定界与边界检查。
   */
  kMax
};

/**
 * @brief 采样器完整状态描述
 *
 * @par 组成
 *   过滤三维度（mag/min/mip）+ 寻址三轴向（repeat_u/v/w）
 *   + LOD 区间与偏移 + 各向异性 + 比较模式 + 边框色 +
 *   非归一化坐标开关。
 *
 * @par 典型组合
 *   - HUD 画布/图标：Linear/Linear/Nearest + ClampToEdge
 *   - 阴影贴图：Linear + enable_compare + kLess
 *   - 像素风：全 Nearest + ClampToEdge
 *
 * @note unnormalized_uvw = true 时采样坐标为像素数（非
 *       [0,1] 归一化），GL 语义为 TEXTURE_RECTANGLE 路径。
 */
struct SamplerState {
  SamplerFilter mag_filter = SamplerFilter::kNearest;  ///< 放大过滤模式
  SamplerFilter min_filter = SamplerFilter::kNearest;  ///< 缩小过滤模式
  SamplerFilter mip_filter = SamplerFilter::kNearest;  ///< mipmap 过滤模式
  SamplerRepeatMode repeat_u =
      SamplerRepeatMode::kClampToEdge;  ///< U 方向重复模式
  SamplerRepeatMode repeat_v =
      SamplerRepeatMode::kClampToEdge;  ///< V 方向重复模式
  SamplerRepeatMode repeat_w =
      SamplerRepeatMode::kClampToEdge;  ///< W 方向重复模式
  float lod_bias = 0.0f;                ///< LOD 偏移
  bool use_anisotropy = false;          ///< 是否启用各向异性过滤
  float anisotropy_max = 1.0f;          ///< 最大各向异性倍数
  bool enable_compare = false;          ///< 是否启用比较操作
  CompareOperator compare_op = CompareOperator::kAlways;  ///< 比较操作符
  float min_lod = 0.0f;                                   ///< 最小 LOD
  float max_lod = 1e20f;                                  ///< 最大 LOD
  SamplerBorderColor border_color =
      SamplerBorderColor::kFloatOpaqueBlack;  ///< 边界颜色
  bool unnormalized_uvw = false;              ///< 是否使用非归一化坐标
};

// ═══════════════════════════════════════════════════════════════════════
// 顶点数组
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 索引缓冲区位宽
 *
 * kUint16（GL_UNSIGNED_SHORT，顶点数 ≤ 65535，索引内存
 * 减半）；kUint32（GL_UNSIGNED_INT，大 mesh）。DrawIndexed
 * 路径记录 current_index_format_，图元重启魔数（0xFFFF vs
 * 0xFFFFFFFF）随之选择。
 */
enum class IndexBufferFormat : uint32 {
  kUint16,
  kUint32,
};

/**
 * @brief 顶点属性频率（属性推进粒度）
 *
 * kVertex：逐顶点推进（常规位置/UV）；kInstance：逐实例
 * 推进（每实例一份，如实例变换/颜色——合批绘制的键），
 * GL 经 glVertexAttribDivisor 实现。
 */
enum class VertexFrequency : uint32 {
  kVertex,
  kInstance,
};

/**
 * @brief 顶点属性描述（顶点着色器单输入的取数方式）
 *
 * 对齐 Vulkan VkVertexInputAttributeDescription（本引擎把
 * 绑定槽的 stride 概念并入每属性，GL 侧 VertexFormatCreate
 * 据此 glVertexAttribPointer 逐项下发进 VAO）。
 *
 * @par 字段语义
 *   binding：取数自哪个顶点缓冲槽（CommandBindVertexBuffers
 *   的数组索引）；location：GLSL layout(location=N)；
 *   offset：槽内字节偏移；format：元素数据格式；
 *   stride：同槽相邻元素间距；frequency：顶点/实例推进。
 */
struct VertexAttribute {
  uint32 binding = UINT32_MAX;                           ///< 绑定索引
  uint32 location = 0;                                   ///< 属性位置
  uint32 offset = 0;                                     ///< 字节偏移
  DataFormat format = DataFormat::kMax;                  ///< 数据格式
  uint32 stride = 0;                                     ///< 步长
  VertexFrequency frequency = VertexFrequency::kVertex;  ///< 属性频率
};

// ═══════════════════════════════════════════════════════════════════════
// 帧缓冲
// ═══════════════════════════════════════════════════════════════════════

/** @brief 附件未使用标记（对齐 Vulkan VK_ATTACHMENT_UNUSED） */
static constexpr int32 kAttachmentUnused = -1;

// ═══════════════════════════════════════════════════════════════════════
// 着色器
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 着色器阶段
 *
 * 对齐 Vulkan VkShaderStageFlagBits 的位值，
 * 可作为 BitField 使用。
 */
enum class ShaderStage : uint32 {
  kVertex = 0,
  kFragment = 1,
  kTessellationControl = 2,
  kTessellationEvaluation = 3,
  kCompute = 4,
  kMax,
};

/**
 * @brief 着色器源码格式
 *
 * 标识 ShaderStageSource::source 的字节格式，供多后端分派：
 * OpenGL 仅接受 kGLSL（GLSL 文本）；Vulkan 接受 kSPIRV（SPIR-V
 * 字节码，尚未实现）。
 */
enum class ShaderSourceFormat : uint32 {
  kGLSL = 0,  ///< GLSL 文本源码（OpenGL）
  kSPIRV,     ///< SPIR-V 字节码（Vulkan，尚未实现）
  kMax,
};

/**
 * @brief 着色器阶段源码对
 *
 * 描述一个着色器阶段及其对应的 GLSL 源码。
 * 用于 ShaderCreateFromSource 的多阶段输入，
 * 替代硬编码的 vertex_source + fragment_source 参数。
 *
 * @par 使用示例：
 *   @code
 *   LocalVector<ShaderStageSource> stages;
 *   stages.PushBack({ShaderStage::kVertex, kVertSrc});
 *   stages.PushBack({ShaderStage::kFragment, kFragSrc});
 *   rd->ShaderCreateFromSource(stages, ShaderSourceFormat::kGLSL, uniforms,
 * 64);
 *   @endcode
 */
struct ShaderStageSource {
  ShaderStage stage = ShaderStage::kMax;  ///< 着色器阶段
  const char* source = nullptr;           ///< GLSL 源码
};

/**
 * @brief Uniform 类型（UniformSet 资源槽的形态）
 *
 * 对齐 Godot UniformType。类型选择映射两后端能力差异：
 * Vulkan 采样器与纹理分离（kSampler 独立绑定）；GL 老式
 * 绑定要求采样状态与纹理同指一个纹理单元 ——
 * kSamplerWithTexture 组合对即该兼容形态。
 *
 * @par 动态变体（*Dynamic）
 *   UBO/SSBO 的动态偏移形态：UniformSetCreate 记录
 *   range_size（单元素字节数），draw 时以 p_dynamic_offset
 *   换算 glBindBufferRange 区间 —— 一套 set 走多实例
 *   偏移的经典合批手法。
 */
enum class UniformType : uint32 {
  kSampler,             ///< 独立采样器（GL_SAMPLER，Vulkan sampler）
  kSamplerWithTexture,  ///< 采样器+纹理对（ids=[SamplerID, TextureID]，GL
                        ///< 老式绑定）
  kTexture,             ///< 纹理（GL_SAMPLER_2D，仅采样读）
  kImage,          ///< 图像（GL_IMAGE_2D，读写存储，对应 GL image load/store）
  kTextureBuffer,  ///< 纹理缓冲区（GL_SAMPLER_BUFFER，Buffer 当纹理采样）
  kSamplerWithTextureBuffer,  ///< 采样器+纹理缓冲区对（ids=[SamplerID,
                              ///< TextureBufferID]）
  kImageBuffer,               ///< 图像缓冲区（GL_IMAGE_BUFFER，读写存储）
  kUniformBuffer,  ///< Uniform Buffer（UBO，GL_UNIFORM_BUFFER，只读常量数据）
  kStorageBuffer,  ///< Storage
                   ///< Buffer（SSBO，GL_SHADER_STORAGE_BUFFER，读写结构化数据）
  kInputAttachment,  ///< 输入附件（渲染目标子资源，Vulkan input attachment，GL
                     ///< 退化为纹理）
  kUniformBufferDynamic,  ///< 动态 UBO（偏移在 draw 时提供，range_size
                          ///< 记录单元素大小）
  kStorageBufferDynamic,  ///< 动态 SSBO（偏移在 draw 时提供，range_size
                          ///< 记录单元素大小）
  kMax
};

/**
 * @brief 着色器 Uniform 描述
 *
 * @par binding 字段的含义因 UniformType 而异：
 *   - UBO (kUniformBuffer/kUniformBufferDynamic): binding = GL UBO binding
 * point， 对应 GLSL 中的 layout(binding=X)
 *   - Sampler (kSamplerWithTexture 等): binding = UniformSet 内的逻辑绑定索引，
 *     用于 RD 层绑定管理；GLSL 中的变量名通过 name 字段指定
 *   - Storage Buffer: binding = GL shader storage buffer binding point
 *
 * @par name 字段：
 *   - 对于 Sampler/Image 类型：指定 GLSL 中的变量名（如 "u_texture"），
 *     GL driver 通过此名称调用 glGetUniformLocation
 *   - 对于 UBO 类型：不需要 name（GL 通过 binding point 绑定，不通过变量名）
 *   - 对于 Storage Buffer 类型：不需要 name（同 UBO，通过 binding point 绑定）
 */
struct ShaderUniform {
  UniformType type = UniformType::kMax;  ///< Uniform 类型
  bool writable = false;                 ///< 是否可写
  uint32 binding = 0;                    ///< 绑定索引
  BitField<ShaderStage> stages;          ///< 可见着色器阶段
  uint32 length = 0;                     ///< 数组长度
  uint32 size = 0;                       ///< 字节大小
  const char* name = nullptr;            ///< GLSL 变量名
};

/**
 * @brief 特化常量值类型
 *
 * 特化常量在管线创建时为 shader 编译期常量注入运行期值
 * （SPIR-V specialization 机制；GLSL 源码路径无原生对应）。
 * union 三成员共享存储，读法由本枚举决定。
 */
enum class PipelineSpecializationConstantType : uint32 {
  kBool,
  kInt,
  kFloat,
};

/**
 * @brief 管线特化常量（单个）
 *
 * constant_id 对应 GLSL/SPIR-V 的
 * layout(constant_id = X) 声明 —— 同一 shader 按不同常量
 * 值可编译出不同优化路径（如光源数展开）。
 *
 * @note GL 后端当前忽略（GLSL 源码路径无机制）；
 *       Vulkan 后端接 SPIR-V 后生效。
 */
struct PipelineSpecializationConstant {
  PipelineSpecializationConstantType type =
      PipelineSpecializationConstantType::kBool;  ///< 特化常量类型
  uint32 constant_id =
      UINT32_MAX;  ///< 常量 ID（对应 GLSL layout(constant_id = X)）
  union {
    uint32 int_value = 0;  ///< 整型值
    float float_value;     ///< 浮点值
    bool bool_value;       ///< 布尔值
  };
};

/**
 * @brief 已绑定的 Uniform 槽（UniformSetCreate 的单条输入）
 *
 * @par ids 语义随 UniformType 变化
 *   - kSamplerWithTexture / kSamplerWithTextureBuffer：
 *     成对 [SamplerID, TextureID]（单元素视为无独立采样器）
 *   - kTexture/kImage/kTextureBuffer/kImageBuffer/
 *     kInputAttachment：[TextureID]
 *   - kUniformBuffer/kStorageBuffer 及动态变体：[BufferID]
 *
 * @par 动态偏移
 *   range_size 仅动态 UBO/SSBO 有效（单元素字节数），
 *   IsDynamic() 为类型判定辅助。
 */
struct BoundUniform {
  UniformType type = UniformType::kMax;  ///< Uniform 类型
  uint32 binding = UINT32_MAX;           ///< 绑定索引
  LocalVector<BufferID> ids;             ///< 资源 ID 列表
  bool immutable_sampler = false;        ///< 是否不可变采样器
  uint32 range_size = 0;                 ///< kUniformBufferDynamic 的单元素大小
  bool IsDynamic() const {
    return type == UniformType::kStorageBufferDynamic ||
           type == UniformType::kUniformBufferDynamic;
  }
};

// ═══════════════════════════════════════════════════════════════════════
// 渲染管线
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 渲染图元类型（draw 调用的图元装配模式）
 *
 * @par 语义与消费方
 *   RenderPipelineCreate 随管线烘焙，draw 路径经
 *   RenderPrimitiveToGLMode() 映射为 GL 绘制模式
 *   （GL_POINTS/GL_LINES/GL_TRIANGLES 族）。
 *
 * @par 命名辨析（Line 系列）
 *   复数 Lines = 独立线段对（每 2 顶点一条线）；
 *   单数 LineStrip = 首尾相连折线（n 顶点 n-1 段）。
 *   Triangles 族同理：kTriangles 独立三角形（每 3 顶点），
 *   kTriangleStrips 共享边的三角带。
 *
 * @par 邻接变体（*WithAdjacency）
 *   每图元附带邻接顶点（线段 2→4 顶点、三角形 3→6 顶点），
 *   仅 geometry shader 可消费邻接数据做无限线/轮廓扩展等
 *   效果；普通管线邻接顶点冗余但无害。HUD 渲染不用。
 *
 * @par 与 Godot/Vulkan 的差异
 *   - 无 kTriangleFans：GL_TRIANGLE_FAN 在索引化合批
 *     （与 strip/quad 混批）下无法统一，契约层刻意裁剪
 *   - kTriangleStripsWithRestartIndex：用索引 0xFFFFFFFF
 *     （uint32 索引）/ 0xFFFF（uint16 索引）作为图元重启
 *     标记，一条 draw 拼多条 strip；GL 端依赖
 *     primitive-restart 机制启用
 *   - kTessellationPatch：面片图元（GL_PATCHES），配合
 *     tess control/eval shader 与管线的
 *     patch_control_points 使用，顶点数由后者而非固定值决定
 */
enum class RenderPrimitive : uint32 {
  kPoints,              ///< 点（GL_POINTS）：每顶点一个像素级点精灵
  kLines,               ///< 独立线段（GL_LINES）：每 2 顶点一条线段
  kLinesWithAdjacency,  ///< 独立线段+邻接（GL_LINES_ADJACENCY）：每 4 顶点
                        ///< [邻接,首,尾,邻接]
  kLineStrips,          ///< 折线（GL_LINE_STRIP）：n 顶点 n-1 段首尾相连
  kLinesStripsWithAdjacency,  ///< 折线+邻接（GL_LINE_STRIP_ADJACENCY）：首尾各带
                              ///< 1 邻接顶点
  kTriangles,               ///< 独立三角形（GL_TRIANGLES）：每 3 顶点一个三角形
  kTrianglesWithAdjacency,  ///< 独立三角形+邻接（GL_TRIANGLES_ADJACENCY）：每 6
                            ///< 顶点 [3 邻接, 3 本体]
  kTriangleStrips,  ///< 三角带（GL_TRIANGLE_STRIP）：n 顶点 n-2 个共享边三角形
  kTriangleStripsWithAdjacency,  ///< 三角带+邻接（GL_TRIANGLE_STRIP_ADJACENCY）
  kTriangleStripsWithRestartIndex,  ///< 带重启索引的三角带（GL_TRIANGLE_STRIP +
                                    ///< primitive restart）：0xFFFF/0xFFFFFFFF
                                    ///< 分隔多带
  kTessellationPatch,               ///< 面片（GL_PATCHES）：顶点数由管线
                                    ///< patch_control_points 决定
  kMax                              ///< 哨兵值（数量上限，非合法图元）
};

/**
 * @brief 多边形背面剔除模式
 *
 * kDisabled：不剔除（半透明 HUD 常态）；kFront/kBack：剔除
 * 正/背面。经 glCullFace 下发（管线烘焙 cull_mode），实际
 * 剔除哪一面与 PolygonFrontFace 的正面定义联合决定。
 */
enum class PolygonCullMode : uint32 { kDisabled, kFront, kBack, kMax };

/**
 * @brief 多边形正面判定（屏幕投影的顶点绕序）
 *
 * kClockwise：顺时针为正面；kCounterClockwise：逆时针为
 * 正面。经 glFrontFace 下发。剔除方向与模板正/背面独立
 * 操作均以此判定为基准。
 */
enum class PolygonFrontFace : uint32 {
  kClockwise,
  kCounterClockwise,
};

/**
 * @brief 模板位面操作（八种）
 *
 * 测试三情形（fail/pass/depth_fail）各指定其一，对齐
 * VkStencilOp：kKeep/kZero/kReplace（写参考值——阴影体
 * 经典手法）/kInvert 及增减变体（Clamp 封顶防回绕，
 * Wrap 允许 8bit 回绕计数）。
 */
enum class StencilOperation : uint32 {
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
 * @brief 逐位逻辑操作（混合的可选路径）
 *
 * 16 种布尔运算对齐 VkLogicOp（kCopy 即常规直写）。启用时
 * 替代数值混合：输出 = src OP dst 按位运算。典型用途：
 * 遮罩擦除（kAnd/kAndInverted）、XOR 重绘。
 *
 * @note GL 经 glLogicOp 支持但仅对无符号整型帧缓冲合法；
 *       HUD 现路径未启用（enable_logic_op = false）。
 */
enum class LogicOperation : uint32 {
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
 * @brief 混合公式因子（src/dst 权重来源）
 *
 * 混合输出 = op(src_factor × src, dst_factor × dst)。18 因子
 * 对齐 VkBlendFactor：常量 Zero/One、源/目标的 Color/Alpha
 * 及 OneMinus 反相、混合常量色（PipelineColorBlendState::
 * blend_constant 经 glBlendColor 下发）、SrcAlphaSaturate、
 * 双源 Src1 系列（dual-source blending，HUD 未用）。
 *
 * @par 典型组合
 *   预乘 alpha：kOne / kOneMinusSrcAlpha；
 *   直乘 alpha：kSrcAlpha / kOneMinusSrcAlpha。
 */
enum class BlendFactor : uint32 {
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
 * @brief 混合公式操作符
 *
 * kAdd（透明叠加常规）/kSubtract/kReverseSubtract/
 * kMinimum/kMaximum，对齐 VkBlendOp。Min/Max 的典型用途：
 * 软粒子 max 混合、深度驱动可视化。GL 经
 * glBlendEquationSeparate 支持（颜色/alpha 可分离）。
 */
enum class BlendOperation : uint32 {
  kAdd,
  kSubtract,
  kReverseSubtract,
  kMinimum,
  kMaximum,
  kMax
};

/**
 * @brief 管线光栅化状态
 *
 * @par GL 映射（管线烘焙，bind 时经 state_cache_ 下发）
 *   cull_mode/front_face → glCullFace/glFrontFace；
 *   wireframe → glPolygonMode（GL 3.3 core 前后面只能同设）；
 *   depth_bias 族 → glPolygonOffset；line_width →
 *   glLineWidth；discard_primitives → rasterizer discard；
 *   patch_control_points → GL_PATCH_VERTICES（tess 用）。
 */
struct PipelineRasterizationState {
  bool enable_depth_clamp = false;  ///< 深度截断：允许片段深度越过 [0,1]
                                    ///< 不被近/远平面裁剪（GL_DEPTH_CLAMP）
  bool discard_primitives =
      false;  ///< 丢弃图元：光栅化器不产生片元（仅跑顶点阶段的开关）
  bool wireframe =
      false;  ///< 线框模式：以线绘制图元（GL 3.3 core 前后面只能同设），调试用
  PolygonCullMode cull_mode =
      PolygonCullMode::kDisabled;  ///< 面剔除：kDisabled/kFront/kBack，对应
                                   ///< glCullFace
  PolygonFrontFace front_face =
      PolygonFrontFace::kClockwise;  ///< 正面绕序：决定哪一面为"正面"，对应
                                     ///< glFrontFace
  bool depth_bias_enabled =
      false;  ///< 深度偏移开关：启用后按常量/斜率因子偏移深度
  float depth_bias_constant_factor =
      0.0f;                       ///< 深度偏移常量因子：与深度无关的固定偏移量
  float depth_bias_clamp = 0.0f;  ///< 深度偏移上限：限制偏移量的最大值（钳位）
  float depth_bias_slope_factor =
      0.0f;                 ///< 深度偏移斜率因子：随深度斜率缩放的偏移量
  float line_width = 1.0f;  ///< 线宽：线图元的绘制宽度（glLineWidth）
  uint32 patch_control_points =
      1;  ///< 面片控制点数：曲面细分每面片的控制点数（GL_PATCH_VERTICES）
};

/**
 * @brief 管线多重采样状态
 *
 * sample_count 须与渲染目标的 TextureSamples 一致
 * （resolve 之后的 pass 用 k1）。sample_shading +
 * min_sample_shading（每采样点独立着色的比例）用于
 * alpha-to-coverage 画质补偿；alpha_to_coverage 把
 * alpha 映射成采样掩码（无深度排序透明的廉价近似）；
 * alpha_to_one 强制输出 alpha = 1。
 */
struct PipelineMultisampleState {
  TextureSamples sample_count =
      TextureSamples::k1;  ///< 采样数：须与渲染目标 TextureSamples
                           ///< 一致（resolve 后的 pass 用 k1）
  bool enable_sample_shading =
      false;  ///< 采样着色：强制每采样点独立执行片元着色器（GL_SAMPLE_SHADING）
  float min_sample_shading =
      0.0f;  ///< 最小采样着色率：参与独立着色的采样点比例下限（1.0=全部）
  bool enable_alpha_to_coverage =
      false;  ///< alpha 转覆盖率：把 alpha
              ///< 映射为采样掩码，无深度排序透明的廉价近似
  bool enable_alpha_to_one =
      false;  ///< alpha 归一：强制片元输出 alpha=1，配合 alpha-to-coverage 使用
};

/**
 * @brief 模板操作状态（单面）
 *
 * 三情形（fail/pass/depth_fail）各配一个 StencilOperation +
 * 比较配置（compare/compare_mask/reference/write_mask）。
 * PipelineDepthStencilState 内正/背面各持一份独立配置，
 * GL 对应 glStencilFuncSeparate/glStencilOpSeparate。
 */
struct StencilOperationState {
  StencilOperation fail =
      StencilOperation::kZero;  ///< 模板测试失败时对模板缓冲执行的操作
  StencilOperation pass =
      StencilOperation::kZero;  ///< 模板测试通过时对模板缓冲执行的操作
  StencilOperation depth_fail =
      StencilOperation::kZero;  ///< 模板通过但深度测试失败时执行的操作
  CompareOperator compare =
      CompareOperator::kAlways;  ///< 模板比较算子（两侧先按 compare_mask
                                 ///< 再比较）
  uint32 compare_mask =
      0;  ///< 比较掩码：参与比较的位（参考值与模板值均先与此按位与）
  uint32 write_mask = 0;  ///< 写入掩码：模板操作结果写回缓冲时保留的位
  uint32 reference = 0;   ///< 参考值：与模板缓冲值比较的基准值
};

/**
 * @brief 管线深度/模板测试状态
 *
 * @par 深度
 *   enable_depth_test/enable_depth_write + 比较算子（典型
 *   kLess 近者胜）。enable_depth_range 为深度边界测试
 *   （片段深度须落在 [min,max] 内才通过，Vulkan depth
 *   bounds 语义；GL 需扩展支持，本引擎 GL 路径未下发）。
 *
 * @par 模板
 *   enable_stencil 开启后正/背面各按 StencilOperationState
 *   执行（阴影体/模板扣洞用法）。
 */
struct PipelineDepthStencilState {
  bool enable_depth_test = false;   ///< 启用深度测试
  bool enable_depth_write = false;  ///< 启用深度写入
  CompareOperator depth_compare_operator =
      CompareOperator::kAlways;  ///< 深度比较算子（典型 kLess 近者胜）
  bool enable_depth_range =
      false;  ///< 深度边界测试开关：片段深度须落 [min,max]（GL 路径未下发）
  float depth_range_min = 0.0f;  ///< 深度边界测试下界
  float depth_range_max = 0.0f;  ///< 深度边界测试上界
  bool enable_stencil = false;   ///< 启用模板测试
  StencilOperationState
      front_op;  ///< 正面模板操作状态（glStencil*Separate 正面参数）
  StencilOperationState
      back_op;  ///< 背面模板操作状态（glStencil*Separate 背面参数）
};

/**
 * @brief 单颜色附件混合状态
 *
 * 六因子（src/dst × color/alpha）+ 双操作符（color/alpha
 * 各自独立）+ RGBA 写掩码（关通道即保护）。对齐
 * VkPipelineColorBlendAttachmentState；GL 经
 * glBlendFuncSeparate/glBlendEquationSeparate +
 * glColorMask 模拟。
 *
 * @note GL 实现当前仅取首附件（GLPipelineState 烘焙单组，
 *       多渲染目标混合需扩展）。
 */
struct PipelineColorBlendAttachment {
  bool enable_blend = false;  ///< 该附件是否启用混合（关闭=直接覆写）
  BlendFactor src_color_blend_factor =
      BlendFactor::kZero;  ///< 颜色通道源混合因子
  BlendFactor dst_color_blend_factor =
      BlendFactor::kZero;  ///< 颜色通道目标混合因子
  BlendOperation color_blend_op =
      BlendOperation::kAdd;  ///< 颜色通道混合操作（加/减/取极值等）
  BlendFactor src_alpha_blend_factor =
      BlendFactor::kZero;  ///< alpha 通道源混合因子
  BlendFactor dst_alpha_blend_factor =
      BlendFactor::kZero;  ///< alpha 通道目标混合因子
  BlendOperation alpha_blend_op = BlendOperation::kAdd;  ///< alpha 通道混合操作
  bool write_r = true;  ///< R 通道写掩码（关闭即保护该通道不被写入）
  bool write_g = true;  ///< G 通道写掩码（关闭即保护该通道不被写入）
  bool write_b = true;  ///< B 通道写掩码（关闭即保护该通道不被写入）
  bool write_a = true;  ///< A 通道写掩码（关闭即保护该通道不被写入）
};

/**
 * @brief 管线颜色混合状态（逐附件表 + 全局项）
 *
 * @par 组成
 *   attachments 长度应对齐管线颜色附件数；逻辑操作开关
 *   （替代数值混合的按位路径）；混合常量色
 *   （kConstantColor/kConstantAlpha 因子的引用值，
 *   glBlendColor 下发）。
 *
 * @par 工厂方法
 *   CreateDisabled：N 附件全直写；CreateBlend：N 附件
 *   预设 alpha 混合（SrcAlpha/OneMinusSrcAlpha）—— HUD
 *   透明图层即此配置。
 */
struct PipelineColorBlendState {
  bool enable_logic_op = false;  ///< 启用逻辑操作：用按位运算替代数值混合
  LogicOperation logic_op =
      LogicOperation::kClear;  ///< 逻辑操作类型（如 kCopy/kXor）
  LocalVector<PipelineColorBlendAttachment>
      attachments;  ///< 逐附件混合状态表：长度须对齐管线颜色附件数
  float blend_constant_r =
      0.0f;  ///< 混合常量色 R：kConstantColor/kConstantAlpha 因子引用值
  float blend_constant_g = 0.0f;  ///< 混合常量色 G
  float blend_constant_b = 0.0f;  ///< 混合常量色 B
  float blend_constant_a = 0.0f;  ///< 混合常量色 A

  static PipelineColorBlendState CreateDisabled(uint32 p_attachments = 1) {
    PipelineColorBlendState state;
    for (uint32 i = 0; i < p_attachments; ++i) {
      state.attachments.PushBack(PipelineColorBlendAttachment());
    }
    return state;
  }

  static PipelineColorBlendState CreateBlend(uint32 p_attachments = 1) {
    PipelineColorBlendState state;
    for (uint32 i = 0; i < p_attachments; ++i) {
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
 * @brief 管线动态状态标志（Vulkan 契约项）
 *
 * 标记哪些状态留在命令缓冲运行期设置（不烘焙进 PSO），
 * 用于减少 Vulkan 的管线变体数。
 *
 * @par GL 策略
 *   GL 全部状态本就动态设置（无真 PSO），本标志整体
 *   忽略。
 */
enum class PipelineDynamicStateFlags : uint32 {
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
 * @brief 附件加载操作（pass 开始时的旧内容处理）
 *
 * kLoad：保留旧内容继续叠加；kClear：清成 clear 值
 * （CommandBeginRenderPass 的 clear 路径消费）；kDontCare：
 * 内容无意义，驱动可跳过加载（tile 架构省带宽优化点）。
 */
enum class AttachmentLoadOp : uint32 {
  kLoad = 0,
  kClear = 1,
  kDontCare = 2,
};

/**
 * @brief 附件存储操作（pass 结束时的结果处理）
 *
 * kStore：写回内存（后续采样/回读依赖）；kDontCare：结果
 * 可丢弃（只被后续 pass 覆写的中间缓冲，省 tile 写回带宽）。
 */
enum class AttachmentStoreOp : uint32 {
  kStore = 0,
  kDontCare = 1,
};

/**
 * @brief 附件描述（RenderPass 的"渲染目标说明书"）
 *
 * @par 语义
 *   RenderPassCreate 的单附件声明：格式 + MSAA 采样数 +
 *   颜色/模板通道各自的 load/store 策略 + Vulkan 布局转换
 *   端点。Framebuffer 依格式做附件挂点兼容选择。
 *
 * @par GL 侧实际消费
 *   format 参与两处判定（FBO 挂点选择、clear 通道分拣，
 *   统一走 DataFormatIsDepthStencil）；stencil_load_op ==
 *   kClear 时追加模板位清理；layout 项全忽略。
 */
struct Attachment {
  DataFormat format = DataFormat::kMax;  ///< 附件像素格式（颜色附件用
                                         ///< RGBA8，深度附件用 D32Sfloat 等）
  TextureSamples samples =
      TextureSamples::kMax;  ///< 多重采样数（MSAA），非 MSAA 用 k1
  AttachmentLoadOp load_op =
      AttachmentLoadOp::kDontCare;  ///< 颜色/深度通道在 RenderPass
                                    ///< 开始时的加载操作
  AttachmentStoreOp store_op =
      AttachmentStoreOp::kDontCare;  ///< 颜色/深度通道在 RenderPass
                                     ///< 结束时的存储操作
  AttachmentLoadOp stencil_load_op =
      AttachmentLoadOp::kDontCare;  ///< 模板通道在 RenderPass
                                    ///< 开始时的加载操作（仅深度+模板格式有效）
  AttachmentStoreOp stencil_store_op = AttachmentStoreOp::
      kDontCare;  ///< 模板通道在 RenderPass
                  ///< 结束时的存储操作（仅深度+模板格式有效）
  TextureLayout initial_layout =
      TextureLayout::kUndefined;  ///< RenderPass 开始前该附件的纹理布局（Vulkan
                                  ///< 布局转换用）
  TextureLayout final_layout =
      TextureLayout::kUndefined;  ///< RenderPass 结束后该附件的纹理布局（Vulkan
                                  ///< 布局转换用）
};

/**
 * @brief 附件引用（Subpass 内按索引指认附件）
 *
 * attachment 为 RenderPass 附件表的下标（非 ID）；
 * kUnused（0xFFFFFFFF）表示该槽不绑定。layout 为该子
 * pass 使用期的期望布局（Vulkan 契约）；aspect 指明深度
 * 模板组合附件参与的是哪个位面。
 */
struct AttachmentReference {
  static constexpr uint32 kUnused = 0xffffffff;
  uint32 attachment = kUnused;                       ///< 附件索引
  TextureLayout layout = TextureLayout::kUndefined;  ///< 纹理布局
  BitField<TextureAspectBits> aspect;                ///< 切面标志
};

/**
 * @brief 子 Pass 描述（RenderPass 的阶段编排）
 *
 * @par 字段语义
 *   color_references：本阶段写入的颜色附件；
 *   depth_stencil_reference：深度测试附件；
 *   input_references：**只读**引用上阶段输出（G-buffer
 *   延迟渲染的读回通道，Vulkan input attachment，GL 退化
 *   为普通采样）；resolve_references：MSAA 附件的 resolve
 *   目标；preserve_attachments：本阶段不碰但须维持内容
 *   有效的附件索引。
 *
 * @par GL 策略
 *   GL 无子 pass 概念 —— RenderPassCreate 对
 *   p_subpasses 整体忽略；单 pass 语义由 Begin/
 *   EndRenderPass 命令对承担。完整消费者是 Vulkan 后端
 *   （mobile tiler 的带宽优化核心）。
 */
struct Subpass {
  LocalVector<AttachmentReference> input_references;    ///< 输入附件引用列表
  LocalVector<AttachmentReference> color_references;    ///< 颜色附件引用列表
  AttachmentReference depth_stencil_reference;          ///< 深度/模板附件引用
  AttachmentReference depth_resolve_reference;          ///< 深度解析附件引用
  LocalVector<AttachmentReference> resolve_references;  ///< 解析附件引用列表
  LocalVector<uint32> preserve_attachments;             ///< 保留附件索引列表
};

/**
 * @brief 管线阶段标志位（屏障的执行序控制）
 *
 * 对齐 VkPipelineStageFlagBits。屏障命令以此指定"源阶段
 * 全部完成后才放行目标阶段"，把执行序同步（本枚举）与
 * 内存可见性（BarrierAccessBits）分开表达 —— Vulkan 核心
 * 机制。GL 后端忽略（驱动隐式同步）。
 */
enum class PipelineStageBits : uint32 {
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
 * @brief 屏障访问标志位（内存可见性控制）
 *
 * 对齐 VkAccessFlagBits。与 PipelineStageBits 配对：阶段
 * 定"何时"，访问类型定"何种内存操作"（读/写 × 附件/
 * 采样/拷贝/主机各族）。GL 后端忽略（隐式同步）。
 */
enum class BarrierAccessBits : uint32 {
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
 * @brief 子 Pass 依赖（阶段间的执行/内存同步声明）
 *
 * src_subpass 的 src_stages/src_access 完成并刷新后，
 * dst_subpass 的 dst_stages/dst_access 才可见 —— Vulkan
 * 依此插屏障并安排子 pass 重叠执行。src/dst =
 * 0xFFFFFFFF 分别指 pass 外的前序/后序（外部依赖）。
 * GL 后端忽略。
 */
struct SubpassDependency {
  uint32 src_subpass = 0xffffffff;         ///< 源子 Pass 索引
  uint32 dst_subpass = 0xffffffff;         ///< 目标子 Pass 索引
  BitField<PipelineStageBits> src_stages;  ///< 源管线阶段
  BitField<PipelineStageBits> dst_stages;  ///< 目标管线阶段
  BitField<BarrierAccessBits> src_access;  ///< 源访问标志
  BitField<BarrierAccessBits> dst_access;  ///< 目标访问标志
};

/**
 * @brief 渲染 Pass 清除值（联合体）
 *
 * 颜色附件读 color[4]（RGBA float）；深度/模板附件读
 * depth + stencil —— 读法由附件格式决定，
 * CommandBeginRenderPass 经 DataFormatIsDepthStencil
 * 分拣（纯模板附件读 stencil 成员）。空构造函数维持
 * 含非平凡成员联合体的合法性。
 */
union RenderPassClearValue {
  float color[4] = {};  ///< 颜色清除值 (R,G,B,A)
  struct {
    float depth;     ///< 深度清除值
    uint32 stencil;  ///< 模板清除值
  };

  RenderPassClearValue() {}
};

/**
 * @brief 附件清除操作（pass 内的细粒度清理）
 *
 * 相对整场 clear（RenderPassClearValue）的局部形态：
 * aspect 位面 + 颜色附件索引 + 清除值，用于 pass 进行中
 * 清理单面/单附件。GL 侧对应 glClearBuffer* 族按位清理。
 */
struct AttachmentClear {
  BitField<TextureAspectBits> aspect;    ///< 清除的切面标志
  uint32 color_attachment = 0xffffffff;  ///< 颜色附件索引
  RenderPassClearValue value;            ///< 清除值
};

// ═══════════════════════════════════════════════════════════════════════
// 屏障（Barrier）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 全局内存屏障（不绑定具体资源）
 *
 * src_access 的写入对 dst_access 可见 —— 最宽的屏障形态
 * （资源无关，全局 flush/invalidate）。资源精确同步用
 * BufferBarrier/TextureBarrier。GL 后端忽略。
 */
struct MemoryAccessBarrier {
  BitField<BarrierAccessBits> src_access;  ///< 源访问标志
  BitField<BarrierAccessBits> dst_access;  ///< 目标访问标志
};

/**
 * @brief 缓冲区屏障（资源精确内存同步）
 *
 * buffer 的 [offset, offset+size) 区间内 src_access 写入
 * 对 dst_access 可见；size = kBufferWholeSize 即全程。
 * 对应 VkBufferMemoryBarrier（典型：SSBO 写 → 顶点读的
 * fencing 点）。GL 后端忽略。
 */
struct BufferBarrier {
  BufferID buffer;                         ///< 缓冲区 ID
  BitField<BarrierAccessBits> src_access;  ///< 源访问标志
  BitField<BarrierAccessBits> dst_access;  ///< 目标访问标志
  uint64 offset = 0;                       ///< 字节偏移
  uint64 size = 0;                         ///< 字节大小
};

/**
 * @brief 纹理屏障（内存同步 + 布局转换）
 *
 * texture 的 subresources 范围：src→dst access 的可见性
 * 同步 + prev_layout→next_layout 的布局转换（Vulkan）。
 * 典型：渲染附件写完转采样读。GL 后端全忽略（无布局
 * 概念 + 隐式同步）。
 */
struct TextureBarrier {
  TextureID texture;                                      ///< 纹理 ID
  BitField<BarrierAccessBits> src_access;                 ///< 源访问标志
  BitField<BarrierAccessBits> dst_access;                 ///< 目标访问标志
  TextureLayout prev_layout = TextureLayout::kUndefined;  ///< 先前布局
  TextureLayout next_layout = TextureLayout::kUndefined;  ///< 目标布局
  TextureSubresourceRange subresources;                   ///< 子资源范围
};

// ═══════════════════════════════════════════════════════════════════════
// 缓冲区
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 缓冲区用途标志位
 *
 * 对齐 VkBufferUsageFlagBits（kDynamicPersistent 为引擎
 * 自增项：GL 4.4 persistent mapping 路径的标记）。
 *
 * @par 典型组合
 *   - 顶点缓冲：kTransferTo | kVertex
 *   - 索引缓冲：kTransferTo | kIndex
 *   - UBO：kTransferTo | kUniform
 *   - SSBO：kTransferTo | kStorage
 *   - Staging：kTransferFrom | kTransferTo | kCpuRead
 *
 * @note GL 后端当前忽略用途位（glBufferData 的 usage 仅按
 *       MemoryAllocationType 给 STATIC/DYNAMIC 提示）。
 */
enum class BufferUsageBits : uint32 {
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

/** @brief 全区间哨兵（barrier/copy 的 size 传此值 = 整个缓冲） */
static constexpr uint64 kBufferWholeSize = ~0ULL;

/**
 * @brief 内存分配类型（CPU 侧可达性策略）
 *
 * kCpu：CPU 可见内存（glMapBuffer 直映射路径）；
 * kGpu：设备本地内存（带宽最优，CPU 访问须经 staging
 * 转移）。GL 后端仅以此为 DYNAMIC/STATIC usage 提示
 * （真实堆策略归驱动）；Vulkan 后端将对应显式内存堆选择。
 */
enum class MemoryAllocationType : uint32 {
  kCpu,  ///< CPU 可见内存（映射直写路径）
  kGpu,  ///< 设备本地内存（staging 转移路径）
};

/**
 * @brief 缓冲区拷贝区域（单条 GPU 内复制）
 *
 * [src_offset, +size) → [dst_offset, +size)。CommandCopyBuffer
 * 按 region 循环展开 glCopyBufferSubData。
 */
struct BufferCopyRegion {
  uint64 src_offset = 0;  ///< 源缓冲区偏移
  uint64 dst_offset = 0;  ///< 目标缓冲区偏移
  uint64 size = 0;        ///< 拷贝字节数
};

/**
 * @brief 缓冲区↔纹理拷贝区域（单条格式化搬运）
 *
 * buffer 侧按 row_pitch 组织行（GPU 纹理行可能对齐 4 字节，
 * 与 CPU 紧凑布局的差额由调用方垫齐）；texture 侧经
 * texture_subresource 定位层/mip/位面，texture_offset_* +
 * texture_region_* 框出目标矩形。GL 路径
 * CommandCopyBufferToTexture 据此展开 glTexSubImage*。
 */
struct BufferTextureCopyRegion {
  uint64 buffer_offset = 0;                ///< 缓冲区偏移
  uint64 row_pitch = 0;                    ///< 行间距（字节）
  TextureSubresource texture_subresource;  ///< 纹理子资源
  int32 texture_offset_x = 0;              ///< 纹理 X 偏移
  int32 texture_offset_y = 0;              ///< 纹理 Y 偏移
  int32 texture_offset_z = 0;              ///< 纹理 Z 偏移
  uint32 texture_region_width = 0;         ///< 拷贝区域宽度
  uint32 texture_region_height = 0;        ///< 拷贝区域高度
  uint32 texture_region_depth = 0;         ///< 拷贝区域深度
};

/**
 * @brief 纹理↔纹理拷贝区域（单条 GPU 内搬运）
 *
 * src/dst 各自的子资源层组 + 三维偏移 + 拷贝尺寸，要求
 * 两侧格式兼容（同格式或同族 reinterpret）。
 *
 * @note GL 无直接纹理到纹理复制 API —— 需 FBO blit 或
 *       缓冲中转；GL 路径未实现该命令，Vulkan 契约保留。
 */
struct TextureCopyRegion {
  TextureSubresourceLayers src_subresources;  ///< 源子资源层
  int32 src_offset_x = 0;                     ///< 源 X 偏移
  int32 src_offset_y = 0;                     ///< 源 Y 偏移
  int32 src_offset_z = 0;                     ///< 源 Z 偏移
  TextureSubresourceLayers dst_subresources;  ///< 目标子资源层
  int32 dst_offset_x = 0;                     ///< 目标 X 偏移
  int32 dst_offset_y = 0;                     ///< 目标 Y 偏移
  int32 dst_offset_z = 0;                     ///< 目标 Z 偏移
  uint32 width = 0;                           ///< 拷贝宽度
  uint32 height = 0;                          ///< 拷贝高度
  uint32 depth = 0;                           ///< 拷贝深度
};

// ═══════════════════════════════════════════════════════════════════════
// 命令缓冲区
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 命令队列族标志位
 *
 * Graphics（渲染+呈现）/Compute/Transfer 三族。Vulkan 查询
 * 队列族能力后按族提交（Transfer 族可异步搬运、与呈现
 * 重叠）；GL 单一隐式队列（无族概念，后端忽略）。
 */
enum class CommandQueueFamilyBits : uint32 {
  kGraphics = 0x1,
  kCompute = 0x2,
  kTransfer = 0x4
};

/**
 * @brief 命令缓冲类型
 *
 * kPrimary：顶层缓冲，直接提交队列；kSecondary：嵌套于
 * primary 的子缓冲（RenderPass 内复用录制片段，Vulkan
 * 特性）。GL 后端无二级概念（Route A 主线程直录）。
 */
enum class CommandBufferType : uint32 {
  kPrimary,
  kSecondary,
};

// ═══════════════════════════════════════════════════════════════════════
// 设备信息
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief GPU 设备类型
 *
 * 对齐 VkPhysicalDeviceType：kDiscreteGpu（独显）/
 * kIntegratedGpu（核显，UMA 架构 staging 无收益）/
 * kVirtualGpu（虚拟化）/kCpu（软渲染）/kOther。
 * 影响内存堆选择与性能启发式；GL 无对应查询（仅能从
 * GL_RENDERER 字符串近似嗅探）。
 */
enum class DeviceType : uint32 {
  kOther,
  kIntegratedGpu,
  kDiscreteGpu,
  kVirtualGpu,
  kCpu,
  kMax
};

/**
 * @brief 驱动资源类别（预算统计口径）
 *
 * 资源类别用于预算统计口径，供预算上限校验使用
 * （QNX 安全约束的量化抓手）。kTopmostObject
 * 为顶层对象（设备级），其余为各资源族。
 */
enum class DriverResource : uint32 {
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
 * @brief 设备限制查询项（LimitGet 查询）
 *
 * GL 侧由 QueryLimits 经 glGetIntegerv 填充，未覆盖项取
 * 保守值。精简版：仅保留 HUD 渲染所需（对齐 Godot Limit
 * 子集裁剪）。
 *
 * @par 高频项
 *   kMaxTextureSize2D（画布尺寸上限）、kMaxUniformBufferSize
 *   （UBO 打包容量）、kMinUniformBufferOffsetAlignment
 *   （动态 UBO 偏移对齐 —— GL 恒 256，编排动态偏移时必须
 *   遵守）、kMaxTextureArrayLayers（图集分层容量）。
 */
enum class Limit : uint32 {
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
 * @brief 设备可选特性查询项（HasFeature 查询）
 *
 * 存在即可启用的能力开关（Vulkan 对应 feature 位）：
 * kSupportsHalfFloat（16F 附件渲染）、kSupportsBufferDevice
 * Address（SSBO 指针寻址）、kSupportsFramebufferDepthResolve
 * （MSAA 深度 resolve）、kSupportsFragmentShaderWithOnlySide
 * Effects（无输出纯副作用 FS）等。GL 后端当前恒 false
 * （契约保守默认，防未探测即启用）。
 */
enum class Features : uint32 {
  kSupportsMultiview,
  kSupportsHalfFloat,
  kSupportsFragmentShaderWithOnlySideEffects,
  kSupportsBufferDeviceAddress,
  kSupportsFramebufferDepthResolve,
  kSupportsPointSize,
  kMax
};

/**
 * @brief API 行为差异查询项（GetApiTrait 查询）
 *
 * 后端行为学差异的探测点（对齐 Godot RDDApiTrait 概念）：
 * 屏障是否被尊重（GL 隐式同步 = 否）、shader 切换是否
 * 失效已绑定 set、纹理传输对齐、行距步进粒度等 ——
 * 引擎侧据此选择降级路径，而非硬编码后端假设。
 */
enum class ApiTrait : uint32 {
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
 * @brief 着色器变更时的 UniformSet 失效策略
 *
 * ApiTrait::kShaderChangeInvalidation 的返回值：换绑
 * program 后哪些已绑定 set 须重绑 ——
 * kAllBoundUniformSets 最保守（GL 老硬件行为）；
 * kIncompatibleSetsPlusCascade 仅布局不兼容者及其级联；
 * kAllOrNoneAccordingToLayoutHash 按布局哈希整体判定。
 */
enum class ShaderChangeInvalidation : uint32 {
  kAllBoundUniformSets,
  kIncompatibleSetsPlusCascade,
  kAllOrNoneAccordingToLayoutHash,
};

/**
 * @brief 设备族（后端 API 归类）
 *
 * Initialize 时经 GL_VERSION 字符串嗅探填充（桌面 GL /
 * OpenGL ES）；Vulkan/DirectX/Metal 为后续后端预留。
 * 驱动行为分支按 family + version 组合判定（如
 * ComputePipelineCreate 的能力门槛、texStorage 不可变
 * 存储路径选择）。
 */
enum class DeviceFamily : uint32 {
  kUnknown,
  kOpenGL,
  kOpenGLES,
  kVulkan,
  kDirectX,
  kMetal,
};

/**
 * @brief 驱动能力描述（Initialize 时填充）
 *
 * device_family + 版本号（GL_MAJOR/MINOR_VERSION 实查）。
 * 组合判定示例：GL ≥ 4.2 → 不可变存储 glTexStorage 路径；
 * GL ≥ 4.3 或 ES ≥ 3.1 → compute 管线可用。api_name 由
 * 此拼装（日志展示用）。
 */
struct Capabilities {
  DeviceFamily device_family = DeviceFamily::kUnknown;  ///< 设备家族
  uint32 version_major = 1;                             ///< 主版本号
  uint32 version_minor = 0;                             ///< 次版本号
};

/**
 * @brief 多视图（立体渲染）能力
 *
 * 一次 draw 输出多视图（GL_OVR_multiview / Vulkan
 * multiview），双目 HUD 立体渲染的潜在依赖。当前 GL 3.3
 * 基线不支持（is_supported 恒 false），契约保留。
 */
struct MultiviewCapabilities {
  bool is_supported = false;                      ///< 是否支持多视图
  bool geometry_shader_is_supported = false;      ///< 是否支持几何着色器
  bool tessellation_shader_is_supported = false;  ///< 是否支持曲面细分着色器
  uint32 max_view_count = 0;                      ///< 最大视图数
  uint32 max_instance_count = 0;                  ///< 最大实例数
};

// ═══════════════════════════════════════════════════════════════════════
// 资源用途（RDG 用）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 资源使用方式（RDG 依赖追踪粒度）
 *
 * 对齐 Godot rendering_device_graph.h 的 ResourceUsage。
 * 渲染命令图据此做读写集追踪 → 自动屏障插入与 Pass 重排：
 * 读-写对（kCopyFrom vs kCopyTo 等）构成依赖边，
 * *ReadWrite 变体标记写侧（屏障 src 端候选）。
 */
enum class ResourceUsage : uint32 {
  kNone,
  kCopyFrom,
  kCopyTo,
  kTextureSample,
  kAttachmentColorReadWrite,
  kAttachmentDepthStencilReadWrite,
  kStorageImageReadWrite,
  kMax
};

/**
 * @brief 计算 mipmap 层级的尺寸
 * @param[in] p_base_size 基础层级（level 0）的尺寸
 * @param[in] p_level mipmap 层级索引
 * @return 该层级的尺寸，最小为 1
 */
inline uint32 MipmapSize(uint32 p_base_size, uint32 p_level) {
  return p_base_size >> p_level > 0 ? p_base_size >> p_level : 1;
}

/**
 * @brief 获取 DataFormat 的每像素字节数
 * @param[in] p_format 数据格式
 * @return 每像素字节数，未知格式返回 0
 */
inline uint32 DataFormatGetPixelSize(DataFormat p_format) {
  switch (p_format) {
    case DataFormat::kR8Unorm:
    case DataFormat::kR8Snorm:
    case DataFormat::kR8Uint:
    case DataFormat::kR8Sint:
    case DataFormat::kR8Srgb:
      return 1;
    case DataFormat::kR8G8Unorm:
    case DataFormat::kR8G8Snorm:
    case DataFormat::kR16Sfloat:
    case DataFormat::kR16Unorm:
      return 2;
    case DataFormat::kR8G8B8A8Unorm:
    case DataFormat::kR8G8B8A8Snorm:
    case DataFormat::kR8G8B8A8Uint:
    case DataFormat::kR8G8B8A8Sint:
    case DataFormat::kR8G8B8A8Srgb:
    case DataFormat::kB8G8R8A8Unorm:
    case DataFormat::kR16G16Sfloat:
    case DataFormat::kR16G16B16A16Unorm:
    case DataFormat::kD24UnormS8Uint:
      return 4;
    case DataFormat::kR16G16B16A16Sfloat:
      return 8;
    case DataFormat::kR32G32B32A32Sfloat:
      return 16;
    case DataFormat::kR32Sfloat:
      return 4;
    case DataFormat::kD32Sfloat:
      return 4;
    default:
      return 0;
  }
}

/**
 * @brief 描述单个 mipmap 层级的数据跨度
 *
 * 用于 TextureCreateFromData 等接口，描述连续 buffer 中
 * 各 mipmap 层级的偏移和大小。
 */
struct MipmapDataSpan {
  const void* data = nullptr;  ///< 该层级数据的指针
  uint32 size = 0;             ///< 该层级数据的大小（字节）
};

}  // namespace arhud

#endif  // ARHUD_DRIVERS_RENDERING_DEVICE_COMMONS_H_
