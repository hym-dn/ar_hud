/**
 * @file rendering_device_driver_gl_format.cpp
 * @brief GLRenderingDeviceDriver 格式转换实现
 *
 * 包含所有 DataFormat 到 OpenGL 格式的转换函数：
 * - DataFormatToGLInternalFormat() — 数据格式 → GL 内部格式
 * - DataFormatToGLType() — 数据格式 → GL 数据类型
 * - DataFormatToGLFormat() — 数据格式 → GL 像素格式
 * - RenderPrimitiveToGLMode() — 渲染图元 → GL 绘制模式
 * - BlendFactorToGL() — 混合因子 → GL 混合因子
 * - CompareOperatorToGL() — 比较操作 → GL 比较函数
 * - SamplerFilterToGL() — 采样器过滤 → GL 过滤模式
 * - SamplerRepeatModeToGL() — 采样器重复 → GL 包裹模式
 * - DataFormatGetPixelSize() — 获取像素大小
 * - DataFormatIsDepthStencil() — 深度/模板格式族统一判定
 * - DataFormatHasStencil() — 模板通道判定
 * - CheckGLError() — GL 错误检查
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-06-14
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include <glad/gl.h>

#include "io/logger.h"
#include "rendering_device_driver_gl.h"

namespace arhud {
/**
 * @brief 数据格式 → GL 内部格式（实现侧；契约语义见 RDD 接口）
 *
 * @par 覆盖范围
 *   单/双/四通道 8bit 无符号、四通道 sRGB、16F/32F 浮点族、
 *   深度与深度模板组合 —— 与当前 Demo 阶段实际使用的格式子集
 *   对齐，未覆盖格式按需增补 case。新增格式必须同步维护四张
 *   映射表（本函数 + DataFormatToGLType + DataFormatToGLFormat +
 *   DataFormatGetPixelSize），任何一张缺项都会造成内部存储与
 *   传输布局错配。
 *
 * @par 已知映射偏差
 *   kB8G8R8A8Unorm → GL_RGBA8：GL 核心无 BGRA 内部格式
 *   （GL_BGRA 仅是传输布局，见 DataFormatToGLFormat），存储侧
 *   通道序仍为 RGBA —— 上传时靠 GL_BGRA 传输格式换序，采样与
 *   回读时通道序为 RGBA，消费方需自行换序对齐。
 *
 * @param[in] p_format 引擎侧数据格式
 *
 * @return GL 内部格式枚举值；未覆盖格式回退 GL_RGBA8
 */
uint32 GLRenderingDeviceDriver::DataFormatToGLInternalFormat(
    DataFormat p_format) {
  switch (p_format) {
    case DataFormat::kR8Unorm:
      return GL_R8;
    case DataFormat::kR8G8Unorm:
      return GL_RG8;
    case DataFormat::kR8G8B8A8Unorm:
      return GL_RGBA8;
    case DataFormat::kR8G8B8A8Srgb:
      return GL_SRGB8_ALPHA8;
    case DataFormat::kB8G8R8A8Unorm:
      return GL_RGBA8;
    case DataFormat::kR16Sfloat:
      return GL_R16F;
    case DataFormat::kR16G16Sfloat:
      return GL_RG16F;
    case DataFormat::kR16G16B16A16Sfloat:
      return GL_RGBA16F;
    case DataFormat::kR32Sfloat:
      return GL_R32F;
    case DataFormat::kR32G32Sfloat:
      return GL_RG32F;
    case DataFormat::kR32G32B32A32Sfloat:
      return GL_RGBA32F;
    case DataFormat::kD16Unorm:
      return GL_DEPTH_COMPONENT16;
    case DataFormat::kD32Sfloat:
      return GL_DEPTH_COMPONENT32F;
    case DataFormat::kD24UnormS8Uint:
      return GL_DEPTH24_STENCIL8;
    case DataFormat::kD32SfloatS8Uint:
      return GL_DEPTH32F_STENCIL8;
    case DataFormat::kD16UnormS8Uint:
      return GL_DEPTH24_STENCIL8;  // GL 无 DEPTH16_STENCIL8 —— 就近降级 D24S8
    case DataFormat::kS8Uint:
      return GL_STENCIL_INDEX8;
    default:
      return GL_RGBA8;
  }
}

/**
 * @brief 数据格式 → GL 数据类型（实现侧）
 *
 * 供纹理/缓冲传输路径（glTexImage*、glTexSubImage* 等）的 type
 * 参数，与 DataFormatToGLFormat 的通道布线联合描述一个 texel
 * 的 CPU 侧内存布局：
 *   - 8bit 无符号族 → GL_UNSIGNED_BYTE；带符号族 → GL_BYTE
 *   - 整型族（Uint/Sint）→ GL_UNSIGNED_INT / GL_INT
 *   - 16bit 浮点族 → GL_HALF_FLOAT；32bit 浮点族 → GL_FLOAT
 *   - 深度族 → GL_UNSIGNED_SHORT / GL_FLOAT / GL_UNSIGNED_INT_24_8
 *     （D24S8 紧凑打包布局）
 *
 * @note 8bit 整型格式（kR8Uint/kR8Sint 等）映射到 32 位 GL 类型，
 *       与 DataFormatGetPixelSize 的 1 字节/像素口径不一致 ——
 *       意味着 CPU 侧上传数据需按 32 位宽布局。整型格式当前
 *       仅作为附件/不可读格式使用，若需整型数据上传须修表。
 * @note kD32SfloatS8Uint 返回 GL_FLOAT：GL 精确对应物为
 *       GL_FLOAT_32_UNSIGNED_INT_24_8_REV，此处未接入，该格式的
 *       CPU 上传路径不可用（GPU 侧分配不受影响）。
 *
 * @param[in] p_format 引擎侧数据格式
 *
 * @return GL 数据类型枚举值；未覆盖格式回退 GL_UNSIGNED_BYTE
 */
uint32 GLRenderingDeviceDriver::DataFormatToGLType(DataFormat p_format) {
  switch (p_format) {
    case DataFormat::kR8Unorm:
    case DataFormat::kR8G8Unorm:
    case DataFormat::kR8G8B8A8Unorm:
    case DataFormat::kB8G8R8A8Unorm:
    case DataFormat::kR8G8B8A8Srgb:
      return GL_UNSIGNED_BYTE;
    case DataFormat::kR8Snorm:
    case DataFormat::kR8G8Snorm:
    case DataFormat::kR8G8B8A8Snorm:
      return GL_BYTE;
    case DataFormat::kR8Uint:
    case DataFormat::kR8G8Uint:
    case DataFormat::kR8G8B8A8Uint:
      return GL_UNSIGNED_INT;
    case DataFormat::kR8Sint:
    case DataFormat::kR8G8Sint:
    case DataFormat::kR8G8B8A8Sint:
      return GL_INT;
    case DataFormat::kR16Sfloat:
    case DataFormat::kR16G16Sfloat:
    case DataFormat::kR16G16B16A16Sfloat:
      return GL_HALF_FLOAT;
    case DataFormat::kR32Sfloat:
    case DataFormat::kR32G32Sfloat:
    case DataFormat::kR32G32B32A32Sfloat:
      return GL_FLOAT;
    case DataFormat::kD16Unorm:
      return GL_UNSIGNED_SHORT;
    case DataFormat::kD32Sfloat:
      return GL_FLOAT;
    case DataFormat::kD24UnormS8Uint:
      return GL_UNSIGNED_INT_24_8;
    case DataFormat::kD32SfloatS8Uint:
      return GL_FLOAT;
    case DataFormat::kS8Uint:
      return GL_UNSIGNED_BYTE;
    default:
      return GL_UNSIGNED_BYTE;
  }
}

/**
 * @brief 数据格式 → GL 像素传输格式（实现侧）
 *
 * 决定 CPU↔GPU 传输（glTexImage* / glReadPixels / glClearTexImage）
 * 的通道布线，按格式族聚合：
 *   - 单通道族（R 系全变体）→ GL_RED
 *   - 双通道族（RG 系全变体）→ GL_RG
 *   - 四通道族（RGBA 系无符号/带符号/sRGB/浮点）→ GL_RGBA
 *   - kB8G8R8A8Unorm → GL_BGRA：GL 侧唯一原生 BGRA 出口，仅作
 *     传输换序（配合 DataFormatToGLType 的 GL_UNSIGNED_BYTE），
 *     不影响内部存储通道序
 *   - 纯深度格式 → GL_DEPTH_COMPONENT；深度模板组合 →
 *     GL_DEPTH_STENCIL
 *
 * @param[in] p_format 引擎侧数据格式
 *
 * @return GL 传输格式枚举值；未覆盖格式回退 GL_RGBA
 */
uint32 GLRenderingDeviceDriver::DataFormatToGLFormat(DataFormat p_format) {
  switch (p_format) {
    case DataFormat::kR8Unorm:
    case DataFormat::kR8Snorm:
    case DataFormat::kR8Uint:
    case DataFormat::kR8Sint:
    case DataFormat::kR16Sfloat:
    case DataFormat::kR32Sfloat:
      return GL_RED;
    case DataFormat::kR8G8Unorm:
    case DataFormat::kR8G8Snorm:
    case DataFormat::kR8G8Uint:
    case DataFormat::kR8G8Sint:
    case DataFormat::kR16G16Sfloat:
    case DataFormat::kR32G32Sfloat:
      return GL_RG;
    case DataFormat::kR8G8B8A8Unorm:
    case DataFormat::kR8G8B8A8Snorm:
    case DataFormat::kR8G8B8A8Uint:
    case DataFormat::kR8G8B8A8Sint:
    case DataFormat::kR8G8B8A8Srgb:
    case DataFormat::kR16G16B16A16Sfloat:
    case DataFormat::kR32G32B32A32Sfloat:
      return GL_RGBA;
    case DataFormat::kB8G8R8A8Unorm:
      return GL_BGRA;
    case DataFormat::kD16Unorm:
    case DataFormat::kD32Sfloat:
      return GL_DEPTH_COMPONENT;
    case DataFormat::kD24UnormS8Uint:
    case DataFormat::kD32SfloatS8Uint:
      return GL_DEPTH_STENCIL;
    case DataFormat::kS8Uint:
      return GL_STENCIL_INDEX;
    default:
      return GL_RGBA;
  }
}

/**
 * @brief 渲染图元 → GL 绘制模式（实现侧）
 *
 * Draw 提交路径消费（rendering_device_driver_gl_commands.cpp 的
 * glDrawArrays/glDrawElements 分支），图元枚举与 GL 枚举一一同构，
 * 含四个邻接图元变体（*WithAdjacency，需 geometry shader 消费；
 * 普通管线按顶点冗余处理）。
 *
 * @note 契约枚举刻意不含 kTriangleFans：GL_TRIANGLE_FAN 在索引化
 *       合批（与 strip/quad 混批）下无法统一，契约层已裁剪该图元。
 *
 * @param[in] p_primitive 引擎侧渲染图元
 *
 * @return GL 绘制模式枚举值；未覆盖值回退 GL_TRIANGLES
 */
uint32 GLRenderingDeviceDriver::RenderPrimitiveToGLMode(
    RenderPrimitive p_primitive) {
  switch (p_primitive) {
    case RenderPrimitive::kPoints:
      return GL_POINTS;
    case RenderPrimitive::kLines:
      return GL_LINES;
    case RenderPrimitive::kLinesWithAdjacency:
      return GL_LINES_ADJACENCY;
    case RenderPrimitive::kLineStrips:
      return GL_LINE_STRIP;
    case RenderPrimitive::kLinesStripsWithAdjacency:
      return GL_LINE_STRIP_ADJACENCY;
    case RenderPrimitive::kTriangles:
      return GL_TRIANGLES;
    case RenderPrimitive::kTrianglesWithAdjacency:
      return GL_TRIANGLES_ADJACENCY;
    case RenderPrimitive::kTriangleStrips:
      return GL_TRIANGLE_STRIP;
    case RenderPrimitive::kTriangleStripsWithAdjacency:
      return GL_TRIANGLE_STRIP_ADJACENCY;
    default:
      return GL_TRIANGLES;
  }
}

/**
 * @brief 混合因子 → GL 混合因子（实现侧）
 *
 * 管线装配路径消费（rendering_device_driver_gl.cpp 的
 * Pipeline creation 处填充 blend_src/dst 四元组）。覆盖 14 个
 * 基础因子：Zero/One、Src/Dst 的 Color/Alpha 及其 OneMinus 变体、
 * 常量色/常量 Alpha 及其 OneMinus 变体、SrcAlphaSaturate。
 *
 * @note 常量因子（ConstantColor/ConstantAlpha）依赖
 *       glBlendColor 设置的常量值，由状态缓存路径按需下发；
 *       双源因子（SRC1_COLOR 系列）未进契约，无需映射。
 *
 * @param[in] p_factor 引擎侧混合因子
 *
 * @return GL 混合因子枚举值；未覆盖值回退 GL_ZERO
 *         （回退语义安全：该因子不贡献颜色，混合公式退化为
 *         可预测形式而非未定义行为）
 */
uint32 GLRenderingDeviceDriver::BlendFactorToGL(BlendFactor p_factor) {
  switch (p_factor) {
    case BlendFactor::kZero:
      return GL_ZERO;
    case BlendFactor::kOne:
      return GL_ONE;
    case BlendFactor::kSrcColor:
      return GL_SRC_COLOR;
    case BlendFactor::kOneMinusSrcColor:
      return GL_ONE_MINUS_SRC_COLOR;
    case BlendFactor::kDstColor:
      return GL_DST_COLOR;
    case BlendFactor::kOneMinusDstColor:
      return GL_ONE_MINUS_DST_COLOR;
    case BlendFactor::kSrcAlpha:
      return GL_SRC_ALPHA;
    case BlendFactor::kOneMinusSrcAlpha:
      return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::kDstAlpha:
      return GL_DST_ALPHA;
    case BlendFactor::kOneMinusDstAlpha:
      return GL_ONE_MINUS_DST_ALPHA;
    case BlendFactor::kConstantColor:
      return GL_CONSTANT_COLOR;
    case BlendFactor::kOneMinusConstantColor:
      return GL_ONE_MINUS_CONSTANT_COLOR;
    case BlendFactor::kConstantAlpha:
      return GL_CONSTANT_ALPHA;
    case BlendFactor::kOneMinusConstantAlpha:
      return GL_ONE_MINUS_CONSTANT_ALPHA;
    case BlendFactor::kSrcAlphaSaturate:
      return GL_SRC_ALPHA_SATURATE;
    default:
      return GL_ZERO;
  }
}

/**
 * @brief 比较操作 → GL 比较函数（实现侧）
 *
 * 八值全量一一映射，两处消费：管线深度测试（Pipeline 创建处
 * 填充 depth_func）与采样器比较（glSamplerParameteri 的
 * GL_TEXTURE_COMPARE_FUNC）。
 *
 * @param[in] p_op 引擎侧比较操作
 *
 * @return GL 比较函数枚举值；未覆盖值回退 GL_ALWAYS
 *         （回退语义：测试恒通过，与深度测试默认行为一致）
 */
uint32 GLRenderingDeviceDriver::CompareOperatorToGL(CompareOperator p_op) {
  switch (p_op) {
    case CompareOperator::kNever:
      return GL_NEVER;
    case CompareOperator::kLess:
      return GL_LESS;
    case CompareOperator::kEqual:
      return GL_EQUAL;
    case CompareOperator::kLessOrEqual:
      return GL_LEQUAL;
    case CompareOperator::kGreater:
      return GL_GREATER;
    case CompareOperator::kNotEqual:
      return GL_NOTEQUAL;
    case CompareOperator::kGreaterOrEqual:
      return GL_GEQUAL;
    case CompareOperator::kAlways:
      return GL_ALWAYS;
    default:
      return GL_ALWAYS;
  }
}

/**
 * @brief 采样器过滤 → GL 过滤模式（实现侧）
 *
 * 基础两级映射（kNearest/kLinear），采样器创建路径消费。
 * 仅描述单级（non-mip）放大/缩小基元；mipmap 维度的过滤组合
 * （GL_NEAREST_MIPMAP_LINEAR 等）由调用方按纹理 mipmaps>0
 * 自行拼装 MIN_FILTER，本函数不掺和。
 *
 * @param[in] p_filter 引擎侧采样过滤
 *
 * @return GL 过滤模式枚举值；未覆盖值回退 GL_NEAREST
 */
uint32 GLRenderingDeviceDriver::SamplerFilterToGL(SamplerFilter p_filter) {
  switch (p_filter) {
    case SamplerFilter::kNearest:
      return GL_NEAREST;
    case SamplerFilter::kLinear:
      return GL_LINEAR;
    default:
      return GL_NEAREST;
  }
}

/**
 * @brief 采样器重复模式 → GL 包裹模式（实现侧）
 *
 * 采样器创建路径消费（repeat_u/v/w 三个轴向各一次）。五值映射：
 * kRepeat / kMirroredRepeat / kClampToEdge / kClampToBorder /
 * kMirrorClampToEdge。
 *
 * @note kClampToBorder 需调用方另行下发边框色
 *       （GL_TEXTURE_BORDER_COLOR，缺省为全零透明黑）。
 * @note kMirrorClampToEdge 对应的 GL_MIRROR_CLAMP_TO_EDGE 在
 *       GL 4.4 才入核心；GL 3.3 上下文依赖
 *       ARB_texture_mirror_clamp_to_edge 扩展（主流桌面驱动普遍
 *       可用，若目标环境缺失应在能力探测处降级到
 *       GL_CLAMP_TO_EDGE）。
 *
 * @param[in] p_mode 引擎侧重复模式
 *
 * @return GL 包裹模式枚举值；未覆盖值回退 GL_CLAMP_TO_EDGE
 */
uint32 GLRenderingDeviceDriver::SamplerRepeatModeToGL(
    SamplerRepeatMode p_mode) {
  switch (p_mode) {
    case SamplerRepeatMode::kRepeat:
      return GL_REPEAT;
    case SamplerRepeatMode::kMirroredRepeat:
      return GL_MIRRORED_REPEAT;
    case SamplerRepeatMode::kClampToEdge:
      return GL_CLAMP_TO_EDGE;
    case SamplerRepeatMode::kClampToBorder:
      return GL_CLAMP_TO_BORDER;
    case SamplerRepeatMode::kMirrorClampToEdge:
      return GL_MIRROR_CLAMP_TO_EDGE;
    default:
      return GL_CLAMP_TO_EDGE;
  }
}

/**
 * @brief 获取像素字节大小（实现侧）
 *
 * CPU 侧内存布局口径：width × height × 本值 = 单层 mip 的字节量，
 * 供纹理上传 stride 计算与字节量预估使用。数组层/立方体面数由
 * 调用方乘 layers；mip 链总字节量按每级 1/4 递减累加。
 *
 * @note 本值是引擎侧逻辑布局，不含驱动内部对齐/padding（GL 侧
 *       GL_UNPACK_ALIGNMENT 行尾对齐由上传路径另行处理）。
 * @note kD32SfloatS8Uint 按实现约定返回 8（D32F 4 + S8 按 4 slot
 *       对齐的口径），与部分驱动的紧凑 5 字节物理布局不同 ——
 *       仅用于估算，勿据此做 GPU 回读反序列化。
 * @note 8bit 整型族（kR8Uint/kR8Sint 等）未列入表内：见
 *       DataFormatToGLType 的 @note，整型族当前不走 CPU 传输。
 *
 * @param[in] p_format 引擎侧数据格式
 *
 * @return 每像素字节数；未覆盖格式回退 4
 */
uint32 GLRenderingDeviceDriver::DataFormatGetPixelSize(DataFormat p_format) {
  switch (p_format) {
    case DataFormat::kR8Unorm:
      return 1;
    case DataFormat::kR8G8Unorm:
      return 2;
    case DataFormat::kR8G8B8A8Unorm:
      return 4;
    case DataFormat::kR8G8B8A8Snorm:
      return 4;
    case DataFormat::kB8G8R8A8Unorm:
      return 4;
    case DataFormat::kR8G8B8A8Srgb:
      return 4;
    case DataFormat::kR16Sfloat:
      return 2;
    case DataFormat::kR16G16Sfloat:
      return 4;
    case DataFormat::kR16G16B16A16Sfloat:
      return 8;
    case DataFormat::kR32Sfloat:
      return 4;
    case DataFormat::kR32G32Sfloat:
      return 8;
    case DataFormat::kR32G32B32A32Sfloat:
      return 16;
    case DataFormat::kD16Unorm:
      return 2;
    case DataFormat::kD32Sfloat:
      return 4;
    case DataFormat::kD24UnormS8Uint:
      return 4;
    case DataFormat::kD32SfloatS8Uint:
      return 8;
    case DataFormat::kD16UnormS8Uint:
      return 4;  // 降级 D24S8 口径（GL 实际分配）
    case DataFormat::kS8Uint:
      return 1;
    default:
      return 4;
  }
}

/**
 * @brief 判定深度/模板附件格式族（实现侧；全引擎唯一权威判定）
 *
 * 覆盖 common 层全部 6 个深度/模板格式（kD16Unorm /
 * kD32Sfloat / kD16UnormS8Uint / kD24UnormS8Uint /
 * kD32SfloatS8Uint / kS8Uint）。
 *
 * @par 消费方（禁止再手写格式清单）
 *   - FramebufferCreate：附件挂点选择（COLOR / DEPTH /
 *     DEPTH_STENCIL / STENCIL）
 *   - CommandBeginRenderPass：clear 值颜色/深度通道分拣
 *
 * @par 历史教训
 *   此前两处消费方各自硬编码枚举清单且已经漂移（FBO 侧漏
 *   kD16UnormS8Uint、clear 侧漏 kS8Uint —— 后者导致纯模板
 *   附件的 clear 值被 glClearColor 误清颜色）。
 *
 * @param[in] p_format 数据格式
 *
 * @return 是否深度/模板格式（含纯模板）
 */
bool GLRenderingDeviceDriver::DataFormatIsDepthStencil(DataFormat p_format) {
  switch (p_format) {
    case DataFormat::kD16Unorm:
    case DataFormat::kD32Sfloat:
    case DataFormat::kD16UnormS8Uint:
    case DataFormat::kD24UnormS8Uint:
    case DataFormat::kD32SfloatS8Uint:
    case DataFormat::kS8Uint:
      return true;
    default:
      return false;
  }
}

/**
 * @brief 判定格式是否含模板通道（实现侧）
 *
 * 含模板位的 4 个格式（kD16UnormS8Uint / kD24UnormS8Uint /
 * kD32SfloatS8Uint / kS8Uint）返回 true —— clear 逻辑据此
 * 追加 GL_STENCIL_BUFFER_BIT（纯模板附件走 glClearStencil）。
 *
 * @param[in] p_format 数据格式
 *
 * @return 是否含模板位面
 */
bool GLRenderingDeviceDriver::DataFormatHasStencil(DataFormat p_format) {
  switch (p_format) {
    case DataFormat::kD16UnormS8Uint:
    case DataFormat::kD24UnormS8Uint:
    case DataFormat::kD32SfloatS8Uint:
    case DataFormat::kS8Uint:
      return true;
    default:
      return false;
  }
}

/**
 * @brief GL 错误检查（实现侧；GL_CHECK_ERROR 宏的展开目标）
 *
 * @par 行为
 *   仅 ARHUD_DEBUG 生效：拉取并清空 glGetError 错误位，把错误码
 *   翻译为字符串后经 ARHUD_LOG_ERROR 输出（"GL_ERROR" 域 +
 *   调用点函数名与行号）。Release 构建下整个函数体编译为空，
 *   参数以 (void) 转型压制未使用告警。
 *
 * @par 覆盖的错误码
 *   GL_INVALID_ENUM / GL_INVALID_VALUE / GL_INVALID_OPERATION /
 *   GL_OUT_OF_MEMORY / GL_INVALID_FRAMEBUFFER_OPERATION；
 *   其余未枚举码打印 "UNKNOWN"。
 *
 * @note 只消费错误位不做控制流决策：错误已发生即记录，是否致命
 *       由调用方（或 Demo 层）判断。glGetError 本身会清空错误位，
 *       因此宏调用点应紧跟待检查的 GL 调用，避免吞掉别处错误。
 *
 * @param[in] p_function 调用点函数名（GL_CHECK_ERROR 宏注入
 *                      __FUNCTION__）
 * @param[in] p_line     调用点行号（GL_CHECK_ERROR 宏注入 __LINE__）
 */
void GLRenderingDeviceDriver::CheckGLError(const char* p_function, int p_line) {
#if ARHUD_DEBUG
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    const char* error_str = "UNKNOWN";
    switch (err) {
      case GL_INVALID_ENUM:
        error_str = "GL_INVALID_ENUM";
        break;
      case GL_INVALID_VALUE:
        error_str = "GL_INVALID_VALUE";
        break;
      case GL_INVALID_OPERATION:
        error_str = "GL_INVALID_OPERATION";
        break;
      case GL_OUT_OF_MEMORY:
        error_str = "GL_OUT_OF_MEMORY";
        break;
      case GL_INVALID_FRAMEBUFFER_OPERATION:
        error_str = "GL_INVALID_FRAMEBUFFER_OPERATION";
        break;
      default:
        break;
    }
    ARHUD_LOG_ERROR("GL_ERROR", "GL error %s (0x%x) at %s:%d", error_str, err,
                    p_function, p_line);
  }
#endif
  (void)p_function;
  (void)p_line;
}

}  // namespace arhud
