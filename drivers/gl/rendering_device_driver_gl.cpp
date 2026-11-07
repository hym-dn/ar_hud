/**
 * @file rendering_device_driver_gl.cpp
 * @brief OpenGL 渲染设备驱动实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-08
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "rendering_device_driver_gl.h"

#include <direct.h>
#include <glad/gl.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "io/logger.h"
#include "os/memory.h"

namespace arhud {
// ═══════════════════════════════════════════════════════════════════════
// GLStateCache — GL 状态去重缓存（线程本地）
// ═══════════════════════════════════════════════════════════════════════
// 设计动机：GL 是全局状态机，重复绑定（同 program/同纹理/同混合态）
// 每次都是一次驱动调用；批渲染每帧数百次 bind，去重后绝大多数变
// no-op。Vulkan 无此问题（PSO 打包状态），缓存是 GL 后端对齐
// Vulkan 性能形态的补偿层。
// 有效性边界：缓存只在"同一 GL 上下文"内成立 —— drawable 切换
// （同 context 换 surface）不破坏缓存；context 切换/重建必须
// Reset()（引擎当前单 context 模型下天然安全）。

/** @brief 全量复位为 GL 默认态（context 重建/首次使用时调用） */
void GLStateCache::Reset() {
  program_ = 0;
  vao_ = 0;
  fbo_ = 0;
  vertex_buffer_ = 0;
  index_buffer_ = 0;
  pixel_unpack_buffer_ = 0;
  active_texture_unit_ = 0;

  for (uint32 i = 0; i < 16; ++i) {
    bound_textures_[i] = 0;
    bound_ubos_[i] = 0;
    bound_ubo_offsets_[i] = 0;
    bound_ubo_sizes_[i] = 0;
    bound_samplers_[i] = 0;
  }

  depth_test_ = false;
  depth_write_ = true;
  depth_func_ = GL_LESS;
  blend_ = false;
  blend_src_rgb_ = GL_ONE;
  blend_dst_rgb_ = GL_ZERO;
  blend_src_alpha_ = GL_ONE;
  blend_dst_alpha_ = GL_ZERO;
  cull_face_ = false;
  cull_mode_ = GL_BACK;
  front_face_ = GL_CW;

  multisample_enable_ = false;
  sample_shading_enable_ = false;
  min_sample_shading_ = 0.0f;
  alpha_to_coverage_ = false;
  alpha_to_one_ = false;

  viewport_x_ = 0;
  viewport_y_ = 0;
  viewport_w_ = 0;
  viewport_h_ = 0;
  viewport_dirty_ = true;

  scissor_x_ = 0;
  scissor_y_ = 0;
  scissor_w_ = 0;
  scissor_h_ = 0;
  scissor_dirty_ = true;
  scissor_enable_ = false;
}

/**
 * @brief 绑定着色器程序（去重：同 program 不重复调 glUseProgram）
 */
void GLStateCache::UseProgram(uint32 p_program) {
  if (program_ != p_program) {
    program_ = p_program;
    glUseProgram(p_program);
  }
}

/**
 * @brief 绑定 VAO（去重 + 索引缓冲失效联动）
 *
 * @par index_buffer_ 联动清零（易错点）
 *   GL_ELEMENT_ARRAY_BUFFER 绑定是 **VAO 状态的一部分**（存进 VAO），
 *   换 VAO 后当前 element 绑定随 VAO 内容而变 —— 缓存值不再可信，
 *   必须清零迫使下次 BindIndexBuffer 真正下发。漏掉这行 = 换 VAO
 *   后索引缓冲"看似已绑"实际用旧 VAO 的绑定 → 画错几何。
 */
void GLStateCache::BindVertexArray(uint32 p_vao) {
  if (vao_ != p_vao) {
    vao_ = p_vao;
    glBindVertexArray(p_vao);
    index_buffer_ = 0;
  }
}

/** @brief 绑定 FBO（去重：GL_FRAMEBUFFER 目标） */
void GLStateCache::BindFramebuffer(uint32 p_fbo) {
  if (fbo_ != p_fbo) {
    fbo_ = p_fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, p_fbo);
  }
}

void GLStateCache::BindVertexBuffer(uint32 p_buffer) {
  if (vertex_buffer_ != p_buffer) {
    vertex_buffer_ = p_buffer;
    glBindBuffer(GL_ARRAY_BUFFER, p_buffer);
  }
}

/**
 * @brief 绑定索引缓冲（GL_ELEMENT_ARRAY_BUFFER 目标，去重）
 *
 * @par 与 BindVertexArray 的耦合
 *   element 绑定存于 VAO 内 —— 外部若绕过缓存直调
 *   glBindVertexArray，须调 InvalidateIndexBuffer() 同步缓存，
 *   否则本缓存会误判"已绑定"跳过真实绑定。
 */
void GLStateCache::BindIndexBuffer(uint32 p_buffer) {
  if (index_buffer_ != p_buffer) {
    index_buffer_ = p_buffer;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p_buffer);
  }
}

/**
 * @brief 绑定 UBO 到绑定点（glBindBufferBase 全缓冲绑定，去重）
 *
 * @param[in] p_index  绑定点索引（< 16，超界忽略 —— GL 3.3 保证
 *                     MAX_UNIFORM_BUFFER_BINDINGS >= 16）
 * @param[in] p_buffer GL 缓冲名
 *
 * @note 全量绑定使该点的 range 绑定信息失效（offset/size 清零）。
 */
void GLStateCache::BindUniformBuffer(uint32 p_index, uint32 p_buffer) {
  if (p_index < 16 && bound_ubos_[p_index] != p_buffer) {
    bound_ubos_[p_index] = p_buffer;
    bound_ubo_offsets_[p_index] = 0;
    bound_ubo_sizes_[p_index] = 0;
    glBindBufferBase(GL_UNIFORM_BUFFER, p_index, p_buffer);
  }
}

/**
 * @brief 绑定 UBO 区段到绑定点（glBindBufferRange，三元组去重）
 *
 * 与 BindUniformBuffer 的区别：只暴露缓冲的 [offset, offset+size)
 * 区段（std140 布局下多个 uniform block 共享一个大 UBO 的场景）。
 * 去重要三元组全等 —— buffer 相同但区间不同是合法的重新绑定。
 *
 * @param[in] p_index  绑定点索引（< 16）
 * @param[in] p_buffer GL 缓冲名
 * @param[in] p_offset 区段起始偏移（字节，须对齐
 * GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT）
 * @param[in] p_size   区段字节数
 */
void GLStateCache::BindUniformBufferRange(uint32 p_index, uint32 p_buffer,
                                          uint32 p_offset, uint32 p_size) {
  if (p_index >= 16) {
    return;
  }
  if (bound_ubos_[p_index] == p_buffer &&
      bound_ubo_offsets_[p_index] == p_offset &&
      bound_ubo_sizes_[p_index] == p_size) {
    return;
  }
  bound_ubos_[p_index] = p_buffer;
  bound_ubo_offsets_[p_index] = p_offset;
  bound_ubo_sizes_[p_index] = p_size;
  glBindBufferRange(GL_UNIFORM_BUFFER, p_index, p_buffer, p_offset, p_size);
}

/**
 * @brief 激活纹理单元（去重：glActiveTexture 是全局状态切换）
 */
void GLStateCache::ActiveTexture(uint32 p_unit) {
  if (active_texture_unit_ != p_unit) {
    active_texture_unit_ = p_unit;
    glActiveTexture(GL_TEXTURE0 + p_unit);
  }
}

/**
 * @brief 绑定 2D 纹理到指定单元（单元+纹理二元组去重）
 *
 * 内部先 ActiveTexture 切单元再 glBindTexture —— 单元选择与
 * 单元内绑定两级状态都经缓存，批渲染逐 draw 绑定零冗余调用。
 *
 * @param[in] p_unit    纹理单元（< 16，超界忽略）
 * @param[in] p_texture GL 纹理名
 */
void GLStateCache::BindTexture2D(uint32 p_unit, uint32 p_texture) {
  if (p_unit < 16 && bound_textures_[p_unit] != p_texture) {
    bound_textures_[p_unit] = p_texture;
    ActiveTexture(p_unit);
    glBindTexture(GL_TEXTURE_2D, p_texture);
  }
}

/**
 * @brief 绑定纹理到任意 target（无缓存直通）
 *
 * @par 为什么不判重
 *   同一纹理单元可同时绑不同 target（2D/1D/3D/CUBE 各占独立槽位），
 *   缓存按 target 维度展开的成本超过收益 —— 直通调用。
 *   渲染热路径（shader 采样）应走 BindTexture2D。
 */
void GLStateCache::BindTexture(uint32 p_target, uint32 p_texture) {
  glBindTexture(p_target, p_texture);
}

/** @brief 绑定采样器到纹理单元（去重；sampler object 分离采样参数于纹理） */
void GLStateCache::BindSampler(uint32 p_unit, uint32 p_sampler) {
  if (p_unit < 16 && bound_samplers_[p_unit] != p_sampler) {
    bound_samplers_[p_unit] = p_sampler;
    glBindSampler(p_unit, p_sampler);
  }
}

/**
 * @brief 绑定像素解包缓冲（GL_PIXEL_UNPACK_BUFFER，去重）
 *
 * 纹理上传路径专用 —— 绑定后 glTexSubImage2D 的像素指针参数
 * 被解释为 PBO 内偏移。上传完必须绑回 0，否则后续 CPU 指针
 * 上传会把偏移当地址。
 */
void GLStateCache::BindPixelUnpackBuffer(uint32 p_buffer) {
  if (pixel_unpack_buffer_ != p_buffer) {
    pixel_unpack_buffer_ = p_buffer;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, p_buffer);
  }
}

/** @brief 失效顶点缓冲缓存（外部直调 glBindBuffer(GL_ARRAY_BUFFER) 后同步用）
 */
void GLStateCache::InvalidateVertexBuffer() {
  vertex_buffer_ = 0;
}

/** @brief 失效索引缓冲缓存（外部直调 glBindBuffer(GL_ELEMENT_ARRAY_BUFFER)
 * 后同步用） */
void GLStateCache::InvalidateIndexBuffer() {
  index_buffer_ = 0;
}

void GLStateCache::SetDepthTest(bool p_enable) {
  if (depth_test_ != p_enable) {
    depth_test_ = p_enable;
    if (p_enable) {
      glEnable(GL_DEPTH_TEST);
    } else {
      glDisable(GL_DEPTH_TEST);
    }
  }
}
/** @brief 深度写入开关（glDepthMask，去重） — 透明物渲染时关写入的标准用法 */

void GLStateCache::SetDepthWrite(bool p_enable) {
  if (depth_write_ != p_enable) {
    depth_write_ = p_enable;
    glDepthMask(p_enable ? GL_TRUE : GL_FALSE);
  }
}

/** @brief 深度比较函数（glDepthFunc，去重；GL_LESS/GL_LEQUAL 等枚举直传） */
void GLStateCache::SetDepthFunc(uint32 p_func) {
  if (depth_func_ != p_func) {
    depth_func_ = p_func;
    glDepthFunc(p_func);
  }
}

/** @brief 混合开关（glEnable/glDisable(GL_BLEND)，去重） */
void GLStateCache::SetBlend(bool p_enable) {
  if (blend_ != p_enable) {
    blend_ = p_enable;
    if (p_enable) {
      glEnable(GL_BLEND);
    } else {
      glDisable(GL_BLEND);
    }
  }
}

/**
 * @brief 分离混合方程（glBlendFuncSeparate，四元组去重）
 *
 * RGB 与 Alpha 通道各自独立的 src/dst 因子 —— 预乘 alpha
 * 材质（src_rgb=GL_ONE）与普通混合（src_rgb=GL_SRC_ALPHA）
 * 的差异正体现在这四个参数的组合上。
 */
void GLStateCache::SetBlendFunc(uint32 p_src_rgb, uint32 p_dst_rgb,
                                uint32 p_src_alpha, uint32 p_dst_alpha) {
  if (blend_src_rgb_ != p_src_rgb || blend_dst_rgb_ != p_dst_rgb ||
      blend_src_alpha_ != p_src_alpha || blend_dst_alpha_ != p_dst_alpha) {
    blend_src_rgb_ = p_src_rgb;
    blend_dst_rgb_ = p_dst_rgb;
    blend_src_alpha_ = p_src_alpha;
    blend_dst_alpha_ = p_dst_alpha;
    glBlendFuncSeparate(p_src_rgb, p_dst_rgb, p_src_alpha, p_dst_alpha);
  }
}

/** @brief 背面剔除开关（GL_CULL_FACE，去重） */
void GLStateCache::SetCullFace(bool p_enable) {
  if (cull_face_ != p_enable) {
    cull_face_ = p_enable;
    if (p_enable) {
      glEnable(GL_CULL_FACE);
    } else {
      glDisable(GL_CULL_FACE);
    }
  }
}

/** @brief 剔除面（glCullFace：GL_BACK/GL_FRONT，去重） */
void GLStateCache::SetCullMode(uint32 p_mode) {
  if (cull_mode_ != p_mode) {
    cull_mode_ = p_mode;
    glCullFace(p_mode);
  }
}

/** @brief 正面判定绕向（glFrontFace：GL_CW/GL_CCW，去重） */
void GLStateCache::SetFrontFace(uint32 p_face) {
  if (front_face_ != p_face) {
    front_face_ = p_face;
    glFrontFace(p_face);
  }
}

/**
 * @brief 设置视口（glViewport，矩形四元组去重 + dirty 位）
 *
 * dirty 位专治 FBO 切换：glBindFramebuffer 会将视口重置为该
 * FBO 尺寸 —— 缓存值失真。BindFramebuffer 置 dirty 强制下次
 * 真实下发（dirty || 四元组不等 才调 glViewport）。
 */
void GLStateCache::SetViewport(int32 p_x, int32 p_y, uint32 p_width,
                               uint32 p_height) {
  if (viewport_dirty_ || viewport_x_ != p_x || viewport_y_ != p_y ||
      viewport_w_ != p_width || viewport_h_ != p_height) {
    viewport_x_ = p_x;
    viewport_y_ = p_y;
    viewport_w_ = p_width;
    viewport_h_ = p_height;
    viewport_dirty_ = false;
    glViewport(p_x, p_y, static_cast<GLsizei>(p_width),
               static_cast<GLsizei>(p_height));
  }
}

/**
 * @brief 设置裁剪矩形（glScissor，四元组去重 + dirty 位）
 *
 * dirty 语义同 SetViewport（FBO 切换重置裁剪态）。
 */
void GLStateCache::SetScissor(int32 p_x, int32 p_y, uint32 p_width,
                              uint32 p_height) {
  if (scissor_dirty_ || scissor_x_ != p_x || scissor_y_ != p_y ||
      scissor_w_ != p_width || scissor_h_ != p_height) {
    scissor_x_ = p_x;
    scissor_y_ = p_y;
    scissor_w_ = p_width;
    scissor_h_ = p_height;
    scissor_dirty_ = false;
    glScissor(p_x, p_y, static_cast<GLsizei>(p_width),
              static_cast<GLsizei>(p_height));
  }
}

/** @brief 裁剪测试开关（GL_SCISSOR_TEST，去重） */
void GLStateCache::SetScissorEnable(bool p_enable) {
  if (scissor_enable_ != p_enable) {
    scissor_enable_ = p_enable;
    if (p_enable) {
      glEnable(GL_SCISSOR_TEST);
    } else {
      glDisable(GL_SCISSOR_TEST);
    }
  }
}

/** @brief 多重采样开关（GL_MULTISAMPLE，去重） */
void GLStateCache::SetMultisampleEnable(bool p_enable) {
  if (multisample_enable_ != p_enable) {
    multisample_enable_ = p_enable;
    if (p_enable) {
      glEnable(GL_MULTISAMPLE);
    } else {
      glDisable(GL_MULTISAMPLE);
    }
  }
}

/** @brief 逐采样着色开关（GL_SAMPLE_SHADING，去重；MSAA 下提升着色质量） */
void GLStateCache::SetSampleShadingEnable(bool p_enable) {
  if (sample_shading_enable_ != p_enable) {
    sample_shading_enable_ = p_enable;
    if (p_enable) {
      glEnable(GL_SAMPLE_SHADING);
    } else {
      glDisable(GL_SAMPLE_SHADING);
    }
  }
}

/** @brief 最小采样着色率（glMinSampleShading，去重） */
void GLStateCache::SetMinSampleShading(float p_value) {
  if (min_sample_shading_ != p_value) {
    min_sample_shading_ = p_value;
    glMinSampleShading(p_value);
  }
}

/** @brief Alpha-to-Coverage
 * 开关（GL_SAMPLE_ALPHA_TO_COVERAGE，去重；几何抗锯齿手段） */
void GLStateCache::SetAlphaToCoverage(bool p_enable) {
  if (alpha_to_coverage_ != p_enable) {
    alpha_to_coverage_ = p_enable;
    if (p_enable) {
      glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    } else {
      glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    }
  }
}

/** @brief Alpha-to-One 开关（GL_SAMPLE_ALPHA_TO_ONE，去重；配合 A2C 把 alpha
 * 修正回 1） */
void GLStateCache::SetAlphaToOne(bool p_enable) {
  if (alpha_to_one_ != p_enable) {
    alpha_to_one_ = p_enable;
    if (p_enable) {
      glEnable(GL_SAMPLE_ALPHA_TO_ONE);
    } else {
      glDisable(GL_SAMPLE_ALPHA_TO_ONE);
    }
  }
}

/**
 * @brief 查 uniform 绑定点对应的 GL location（线性查表）
 *
 * @param[in] p_binding 引擎侧绑定点（ShaderUniform::binding，
 *                      与 GLSL 里 layout(binding=N) 对齐）
 * @param[in] p_type    uniform 类型（区分同绑定点上不同类槽位，
 *                      如 sampler 与 UBO 可同 binding 值）
 *
 * @return GL location（uniform/UBO 位置），未登记返回 -1
 *
 * @par 实现细节
 *   GL 3.3 Core 下 sampler uniform 无 location 直查 API（无
 *   program introspection 快捷路径的保守实现），编译期由
 *   ShaderCreateFromSource 反射（glGetUniformLocation）填充
 *   uniform_locations 表，本方法运行期查表。HUD 着色器
 *   uniform 数量级 < 16，线性扫描即 L1 常驻，无哈希必要。
 */
int32 GLShaderInfo::FindLocation(uint32 p_binding, UniformType p_type) const {
  for (uint32 i = 0; i < uniform_locations.Size(); ++i) {
    if (uniform_locations[i].binding == p_binding &&
        uniform_locations[i].type == p_type) {
      return uniform_locations[i].location;
    }
  }
  return -1;
}

// ─── 生命周期 ───────────────────────────────────────────────────────

/**
 * @brief 构造（注入依赖，零副作用）
 *
 * @param[in] p_context_driver 所属 RCD（能力查询/表面访问）
 * @param[in] p_gl_manager     平台 GL 底座（MakeCurrent/SwapBuffers/
 *                             函数加载入口 —— SwapChain 路径使用）
 */
GLRenderingDeviceDriver::GLRenderingDeviceDriver(
    IRenderingContextDriver* p_context_driver, IGLContextManager* p_gl_manager)
    : context_driver_(p_context_driver), gl_manager_(p_gl_manager) {}

/**
 * @brief 析构：若仍初始化则兜底 Shutdown
 *
 * @note 正常流程应由 RD 显式 Shutdown（GL 资源须在 context
 *       存活期释放）；析构兜底仅为防漏。context 已亡时 Reset(true)
 *       的 GL 调用是空转 —— allocator 内无 GL 交互，安全。
 */
GLRenderingDeviceDriver::~GLRenderingDeviceDriver() {
  if (initialized_) {
    Shutdown();
  }
}

/**
 * @brief 初始化 OpenGL 渲染设备驱动（实现侧；契约见 RDD 接口）
 *
 * @par 实现路径
 *   1. gladLoadGL（经 IGLContextManager 的组合 loader：wgl 优先 +
 *      opengl32.dll 回退）加载全部 GL 函数指针
 *   2. QueryLimits() 填 capabilities_（纹理尺寸上限等）
 *   3. GL_VERSION 字符串嗅探区分桌面 GL / OpenGL ES（同一代码
 *      路径服务桌面 dev 与未来 GLES，见 capabilities_.device_family）
 *   4. 版本号记入 capabilities_ + api_name_（日志用）
 *
 * @param[in] p_device_index  设备索引（GL 忽略 —— 单设备模型，
 *                            RCD 枚举已隐含选择；保留为契约参数）
 * @param[in] p_frame_count   帧缓冲数量（GL 忽略 —— GL 无 swapchain
 *                            image 池；保留为契约参数）
 *
 * @retval Error::kOK 初始化成功（重复调用幂等）
 * @retval Error::kFailed GL 函数加载失败
 *
 * @pre GL 上下文已 MakeCurrent（gladLoadGL 需要活跃上下文）
 */
Error GLRenderingDeviceDriver::Initialize(uint32 p_device_index,
                                          uint32 p_frame_count) {
  (void)p_device_index;
  (void)p_frame_count;

  if (initialized_) {
    return Error::kOK;
  }

  int gl_version = gladLoadGL(
      reinterpret_cast<GLADloadfunc>(gl_manager_->GetGLProcAddress()));
  if (gl_version == 0) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "GLRenderingDeviceDriver: Failed to load GL functions via glad");
    return Error::kFailed;
  }

  QueryLimits();

  // 检测 OpenGL ES（版本字符串以 "OpenGL ES" 开头）
  const char* version_str =
      reinterpret_cast<const char*>(glGetString(GL_VERSION));
  bool is_es =
      version_str != nullptr && strstr(version_str, "OpenGL ES") != nullptr;
  if (version_str == nullptr) {
    ARHUD_LOG_WARN(
        "GLRenderingDeviceDriver: glGetString(GL_VERSION) returned null, "
        "assuming desktop GL");
  }
  capabilities_.device_family =
      is_es ? DeviceFamily::kOpenGLES : DeviceFamily::kOpenGL;

  GLint major = 3, minor = 3;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  capabilities_.version_major = static_cast<uint32>(major);
  capabilities_.version_minor = static_cast<uint32>(minor);

  snprintf(api_name_, sizeof(api_name_), "%s %u.%u",
           is_es ? "OpenGL ES" : "OpenGL", capabilities_.version_major,
           capabilities_.version_minor);

  snprintf(pipeline_cache_uuid_, sizeof(pipeline_cache_uuid_),
           "00000000-0000-0000-0000-000000000000");

  initialized_ = true;
  ARHUD_LOG_INFO("GLRenderingDeviceDriver: Initialized (%s)", api_name_);
  return Error::kOK;
}

/**
 * @brief 关闭驱动（实现侧；契约见 RDD 接口）
 *
 * @par 实现路径
 *   1. 六个 PagedAllocator Reset(true)（释放页内存；GL 对象
 *      本体的 glDeleteXxx 已在各资源 Free 时执行，allocator
 *      只管簿记结构的内存）
 *   2. state_cache_ 复位 + current_pipeline_ 置空
 *
 * @note 不执行 context 释放（归 RCD/GLContextManager）；
 *       幂等（未初始化直接返回）。
 */
void GLRenderingDeviceDriver::Shutdown() {
  if (!initialized_) {
    return;
  }

  shader_allocator_.Reset(true);
  pipeline_allocator_.Reset(true);
  framebuffer_allocator_.Reset(true);
  render_pass_allocator_.Reset(true);
  uniform_set_allocator_.Reset(true);
  vertex_format_allocator_.Reset(true);

  state_cache_.Reset();
  current_pipeline_ = nullptr;

  initialized_ = false;
  ARHUD_LOG_INFO("GLRenderingDeviceDriver: Shutdown complete");
}

/**
 * @brief 创建 GPU 缓冲（实现侧；契约语义见 RDD 接口）
 *
 * @par 实现路径
 *   1. glGenBuffers 生成 GL 名字（==0 视为失败）
 *   2. 经 state_cache_ 绑定到 GL_ARRAY_BUFFER（去重 + 保持缓存一致）
 *   3. glBufferData 以 usage 提示分配存储（数据传 nullptr —— 只
 *      分配不填充，内容经 BufferUpdate/BufferMap 写入）
 *   4. 尺寸入 buffer_sizes_ 表（GetAllocationSize 查询用）
 *
 * @par GL 特有语义
 *   - p_usage（顶点/索引/UBO/...）被忽略 —— GL 缓冲无 usage 绑定
 *     约束，同一缓冲可随时重绑到任意 target（Vulkan 需精确 usage）
 *   - p_allocation_type 仅映射为 GL_STATIC_DRAW / GL_DYNAMIC_DRAW
 *     性能提示（kVram→静态，kCpu→动态；GL 不强制，驱动只做放置策略）
 *
 * @param[in] p_size            缓冲字节数
 * @param[in] p_usage           用途位（GL 忽略，契约保留）
 * @param[in] p_allocation_type 内存类型（映射 usage 提示）
 *
 * @return 新 BufferID（封装 GL 缓冲名）
 * @retval BufferID() 生成失败
 */
BufferID GLRenderingDeviceDriver::BufferCreate(
    uint64 p_size, BitField<BufferUsageBits> p_usage,
    MemoryAllocationType p_allocation_type) {
  (void)p_usage;
  GLuint gl_buffer = 0;
  glGenBuffers(1, &gl_buffer);
  if (gl_buffer == 0) {
    ARHUD_LOG_ERROR("kFailed",
                    "BufferCreate: glGenBuffers returned 0 (size %llu)",
                    static_cast<unsigned long long>(p_size));
    return BufferID();
  }
  state_cache_.BindVertexBuffer(gl_buffer);
  GLenum gl_usage = GL_STATIC_DRAW;
  if (p_allocation_type == MemoryAllocationType::kCpu) {
    gl_usage = GL_DYNAMIC_DRAW;
  }
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(p_size), nullptr,
               gl_usage);
  GL_CHECK_ERROR();
  buffer_sizes_.Insert(static_cast<uint64>(gl_buffer), p_size);
  return BufferID(static_cast<uint64>(gl_buffer));
}

/**
 * @brief 释放缓冲（实现侧）
 *
 * 先摘簿记（buffer_sizes_）再 glDeleteBuffers —— 顺序保证
 * Free 后 GetAllocationSize 对该 ID 返回 0 而非悬垂读。
 * 无效 ID 静默返回。注意 GL 的延迟删除语义：缓冲若仍被
 * 在途 draw 引用，驱动延迟到不使用时才真正回收（调方无需
 * 像 Vulkan 那样等 fence —— 但 RD 层的帧延迟销毁仍保留作
 * 簿记一致性保护）。
 */
void GLRenderingDeviceDriver::BufferFree(BufferID p_buffer) {
  if (!p_buffer.IsValid()) {
    return;
  }
  GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
  buffer_sizes_.Erase(static_cast<uint64>(gl_buffer));
  glDeleteBuffers(1, &gl_buffer);
}

/**
 * @brief 映射缓冲到 CPU 可写内存（实现侧）
 *
 * @par 实现细节（三明治模式）
 *   glGetIntegerv 取当前绑定 → glBindBuffer 换目标 → glMapBuffer
 *   映射（GL_READ_WRITE，读写双向）→ **恢复原绑定**。
 *   恢复步骤不可省：绕过 state_cache_ 直调 glBindBuffer 会使
 *   缓存的 vertex_buffer_ 失真（见 BindIndexBuffer 注释的同类
 *   耦合问题）—— 三明治保证 GL 全局状态零扰动。
 *
 * @return CPU 指针，失败/无效 ID 返回 nullptr
 *
 * @note GL 3.3 无 persistent mapping（glMapBufferRange 的
 *       GL_MAP_PERSISTENT_BIT 是 4.4）—— 每次映射都是
 *       瞬时映射，Unmap 前驱动可能阻塞（GPU 正在读取时）。
 *       RD 的 Staging Buffer 池正是为摊销此成本而设。
 */
uint8* GLRenderingDeviceDriver::BufferMap(BufferID p_buffer) {
  if (!p_buffer.IsValid()) {
    return nullptr;
  }
  GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
  GLint prev_buffer = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, gl_buffer);
  uint8* ptr = static_cast<uint8*>(glMapBuffer(GL_ARRAY_BUFFER, GL_READ_WRITE));
  glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_buffer));
  if (ptr == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "BufferMap: glMapBuffer returned null (buffer %u)",
                    gl_buffer);
  }
  return ptr;
}

/**
 * @brief 解除映射（实现侧；三明治模式同 BufferMap）
 *
 * @note glUnmapBuffer 返回 GL_FALSE 时映射期间 CPU 写入可能
 *       已丢失（驱动内部故障，极少见）—— 记 WARN。
 */
void GLRenderingDeviceDriver::BufferUnmap(BufferID p_buffer) {
  if (!p_buffer.IsValid()) {
    return;
  }
  GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
  GLint prev_buffer = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, gl_buffer);
  if (!glUnmapBuffer(GL_ARRAY_BUFFER)) {
    ARHUD_LOG_WARN(
        "BufferUnmap: glUnmapBuffer failed, mapped writes may be lost (buffer "
        "%u)",
        gl_buffer);
  }
  glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_buffer));
}

/**
 * @brief 查询缓冲分配尺寸（实现侧）
 *
 * @par 两条路径
 *   首选 buffer_sizes_ 簿记表（创建时登记，零 GL 调用）；
 *   未命中（外部创建的缓冲等）才走 GL 查询路径（三明治 +
 *   glGetIntegerv(GL_BUFFER_SIZE)）。当前所有缓冲都经
 *   BufferCreate 走簿记，GL 路径为防御性兜底。
 *
 * @return 字节数；无效 ID 返回 0
 */
uint64 GLRenderingDeviceDriver::BufferGetAllocationSize(BufferID p_buffer) {
  if (!p_buffer.IsValid()) {
    return 0;
  }
  GLuint gl_buffer = static_cast<GLuint>(p_buffer.GetId());
  GLint prev_buffer = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, gl_buffer);
  GLint size = 0;
  glGetIntegerv(GL_BUFFER_SIZE, &size);
  glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_buffer));
  return static_cast<uint64>(size);
}

/**
 * @brief 创建纹理（实现侧；契约语义见 RDD 接口）
 *
 * @par 实现路径（按维度/版本分四个分支树）
 *   1. target 选择：MSAA 强制 GL_TEXTURE_2D_MULTISAMPLE；否则按
 *      texture_type 映射 2D/2D_ARRAY/3D/CUBE
 *   2. 经 state_cache_.BindTexture 绑定（此处走无缓存直通 ——
 *      同名可绑多 target，见 GLStateCache::BindTexture 注释）
 *   3. 存储分配，按能力分两路：
 *      - GL >= 4.2：不可变存储（glTexStorage2D/3D/2DMultisample）
 *        —— 一次性分配全部 mip 层，驱动可做放置优化；之后
 *        仅能 glTexSubImage 更新内容，不能重定义尺寸
 *      - GL 3.3：逐层 glTexImage2D/3D（数据 nullptr 只分配），
 *        cube 按 6 面循环；同时补默认过滤/环绕参数
 *        （glTexStorage 路径无此需要 —— 不可变存储隐含完备 mip 链）
 *   4. 簿记 GLTextureInfo（target/format/尺寸/mip 数/is_immutable）
 *      入 texture_infos_ 表 —— 后续 TextureGenerateMipmaps /
 *      上传路径按此复现 target
 *
 * @par 契约参数的 GL 忽略项
 *   p_view（is_depth/层范围）忽略 —— GL 3.3 无纹理视图（无
 *   glTextureView，4.3 才有）；深度纹理由 format 自带。
 *
 * @param[in] p_format 纹理格式与维度（详见 TextureFormat）
 * @param[in] p_view   视图描述（GL 忽略，契约保留）
 *
 * @return 新 TextureID（封装 GL 纹理名）
 * @retval TextureID() 生成失败
 */
TextureID GLRenderingDeviceDriver::TextureCreate(const TextureFormat& p_format,
                                                 const TextureView& p_view) {
  (void)p_view;
  GLuint gl_texture = 0;
  glGenTextures(1, &gl_texture);
  if (gl_texture == 0) {
    ARHUD_LOG_ERROR("kFailed",
                    "TextureCreate: glGenTextures returned 0 (%ux%u)",
                    p_format.width, p_format.height);
    return TextureID();
  }
  // MSAA 纹理使用 GL_TEXTURE_2D_MULTISAMPLE
  bool is_msaa = (p_format.samples != TextureSamples::k1);
  GLenum target = GL_TEXTURE_2D;
  if (is_msaa) {
    target = GL_TEXTURE_2D_MULTISAMPLE;
  } else {
    switch (p_format.texture_type) {
      case TextureType::k2D:
        target = GL_TEXTURE_2D;
        break;
      case TextureType::k2DArray:
        target = GL_TEXTURE_2D_ARRAY;
        break;
      case TextureType::k3D:
        target = GL_TEXTURE_3D;
        break;
      case TextureType::kCube:
        target = GL_TEXTURE_CUBE_MAP;
        break;
      default:
        target = GL_TEXTURE_2D;
        break;
    }
  }
  state_cache_.BindTexture(target, gl_texture);
  uint32 internal_format = DataFormatToGLInternalFormat(p_format.format);
  uint32 gl_format = DataFormatToGLFormat(p_format.format);
  uint32 gl_type = DataFormatToGLType(p_format.format);
  // TextureSamples → GL sample count
  GLint sample_count = 1;
  switch (p_format.samples) {
    case TextureSamples::k2:
      sample_count = 2;
      break;
    case TextureSamples::k4:
      sample_count = 4;
      break;
    case TextureSamples::k8:
      sample_count = 8;
      break;
    case TextureSamples::k16:
      sample_count = 16;
      break;
    case TextureSamples::k32:
      sample_count = 32;
      break;
    case TextureSamples::k64:
      sample_count = 64;
      break;
    default:
      sample_count = 1;
      break;
  }
  if (is_msaa) {
    // MSAA 纹理只能使用 glTexStorage2DMultisample 或 glTexImage2DMultisample
    bool msaa_immutable =
        (capabilities_.version_major > 4 || (capabilities_.version_major == 4 &&
                                             capabilities_.version_minor >= 2));
    if (msaa_immutable) {
      glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, sample_count,
                                static_cast<GLenum>(internal_format),
                                static_cast<GLsizei>(p_format.width),
                                static_cast<GLsizei>(p_format.height),
                                GL_TRUE);  // fixedsamplelocations
    } else {
      glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, sample_count,
                              static_cast<GLenum>(internal_format),
                              static_cast<GLsizei>(p_format.width),
                              static_cast<GLsizei>(p_format.height), GL_TRUE);
    }
  } else {
    bool use_immutable =
        (capabilities_.version_major > 4 || (capabilities_.version_major == 4 &&
                                             capabilities_.version_minor >= 2));
    if (use_immutable) {
      if (p_format.texture_type == TextureType::k2D ||
          p_format.texture_type == TextureType::kCube) {
        glTexStorage2D(target, static_cast<GLsizei>(p_format.mipmaps),
                       static_cast<GLenum>(internal_format),
                       static_cast<GLsizei>(p_format.width),
                       static_cast<GLsizei>(p_format.height));
      } else if (p_format.texture_type == TextureType::k3D ||
                 p_format.texture_type == TextureType::k2DArray) {
        glTexStorage3D(target, static_cast<GLsizei>(p_format.mipmaps),
                       static_cast<GLenum>(internal_format),
                       static_cast<GLsizei>(p_format.width),
                       static_cast<GLsizei>(p_format.height),
                       static_cast<GLsizei>(p_format.depth));
      }
    } else {
      if (p_format.texture_type == TextureType::k2D) {
        for (uint32 level = 0; level < p_format.mipmaps; ++level) {
          uint32 w = p_format.width >> level;
          uint32 h = p_format.height >> level;
          if (w == 0)
            w = 1;
          if (h == 0)
            h = 1;
          glTexImage2D(target, static_cast<GLint>(level),
                       static_cast<GLint>(internal_format),
                       static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                       gl_format, gl_type, nullptr);
        }
      } else if (p_format.texture_type == TextureType::k3D ||
                 p_format.texture_type == TextureType::k2DArray) {
        for (uint32 level = 0; level < p_format.mipmaps; ++level) {
          uint32 w = p_format.width >> level;
          uint32 h = p_format.height >> level;
          uint32 d = p_format.depth >> level;
          if (w == 0)
            w = 1;
          if (h == 0)
            h = 1;
          if (d == 0)
            d = 1;
          glTexImage3D(target, static_cast<GLint>(level),
                       static_cast<GLint>(internal_format),
                       static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                       static_cast<GLsizei>(d), 0, gl_format, gl_type, nullptr);
        }
      } else if (p_format.texture_type == TextureType::kCube) {
        for (uint32 level = 0; level < p_format.mipmaps; ++level) {
          uint32 w = p_format.width >> level;
          uint32 h = p_format.height >> level;
          if (w == 0)
            w = 1;
          if (h == 0)
            h = 1;
          for (uint32 face = 0; face < 6; ++face) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                         static_cast<GLint>(level),
                         static_cast<GLint>(internal_format),
                         static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                         gl_format, gl_type, nullptr);
          }
        }
      }
      if (p_format.mipmaps > 1) {
        glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL,
                        static_cast<GLint>(p_format.mipmaps - 1));
      } else {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }
      glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
  }
  GL_CHECK_ERROR();
  TextureID tex_id(static_cast<uint64>(gl_texture));
  GLTextureInfo info;
  info.target = target;
  info.format = p_format.format;
  info.width = p_format.width;
  info.height = p_format.height;
  info.depth = p_format.depth;
  info.mipmaps = p_format.mipmaps;
  info.layers = p_format.array_layers;
  info.is_immutable =
      is_msaa ||
      (capabilities_.version_major > 4 ||
       (capabilities_.version_major == 4 && capabilities_.version_minor >= 2));
  texture_infos_.Insert(tex_id.GetId(), info);
  return tex_id;
}

/**
 * @brief 释放纹理（实现侧）
 *
 * glDeleteTextures（延迟删除语义同 BufferFree）+ 摘 texture_infos_
 * 簿记。无效 ID 静默返回。
 */
void GLRenderingDeviceDriver::TextureFree(TextureID p_texture) {
  if (!p_texture.IsValid()) {
    return;
  }
  GLuint gl_texture = static_cast<GLuint>(p_texture.GetId());
  glDeleteTextures(1, &gl_texture);
  texture_infos_.Erase(p_texture.GetId());
}

/**
 * @brief 生成 mipmap 链（实现侧）
 *
 * 经簿记表取回 target 后 BindTexture（直通）+ glGenerateMipmap。
 * 簿记未登记的 ID 静默返回 —— 未命中即外部纹理或已释放，
 * 盲跑会破坏当前绑定。
 *
 * @note 前提：纹理以可 mipmap 的过滤模式创建（GL 3.3 路径在
 *       TextureCreate 补过 MIN_FILTER）；不可变存储路径由
 *       glTexStorage 的 mip 层参数保证空间已备。
 */
void GLRenderingDeviceDriver::TextureGenerateMipmaps(TextureID p_texture) {
  if (!p_texture.IsValid()) {
    return;
  }
  if (!texture_infos_.Has(p_texture.GetId())) {
    return;
  }
  const GLTextureInfo& tex_info = texture_infos_[p_texture.GetId()];
  GLuint gl_texture = static_cast<GLuint>(p_texture.GetId());
  state_cache_.BindTexture(tex_info.target, gl_texture);
  glGenerateMipmap(tex_info.target);
  GL_CHECK_ERROR();
}

/**
 * @brief 查询纹理分配尺寸（实现侧——GL 路径未实现）
 *
 * @return 恒 0（VRAM 估算未覆盖 GL 后端；对比 BufferGetAllocationSize
 *         有簿记路径。若需 VRAM 预算功能，可按
 *         width*height*mips*每像素字节 估算登记入 texture_infos_）
 */
uint64 GLRenderingDeviceDriver::TextureGetAllocationSize(TextureID p_texture) {
  (void)p_texture;
  return 0;
}

/**
 * @brief 查询格式支持的用途位（实现侧；契约语义见 RDD 接口）
 *
 * @par GL 的宽松策略
 *   GL 3.3 无 Vulkan 式格式-用途支持矩阵（vkGetPhysicalDevice
 *   FormatProperties 的对应物缺失），本实现按保守集合恒返回：
 *   采样 + 双向拷贝 + 可更新恒置；颜色 vs 深度模板按格式族
 *   二选一；CPU 读按参数透传。即"GL 能创建的纹理都支持这些
 *   用途"——对引擎实际使用的格式子集（RGBA8/深度/压缩）成立。
 *
 * @param[in] p_format       数据格式
 * @param[in] p_cpu_readable 调用方声明的 CPU 可读需求（透传置位）
 */
BitField<TextureUsageBits>
GLRenderingDeviceDriver::TextureGetUsagesSupportedByFormat(
    DataFormat p_format, bool p_cpu_readable) {
  // 深度/模板格式（统一判定，唯一权威清单见 _format.cpp）
  const bool is_depth_stencil = DataFormatIsDepthStencil(p_format);
  BitField<TextureUsageBits> usages;
  usages.SetFlag(TextureUsageBits::kSampling);
  usages.SetFlag(TextureUsageBits::kCanCopyFrom);
  usages.SetFlag(TextureUsageBits::kCanCopyTo);
  if (is_depth_stencil) {
    usages.SetFlag(TextureUsageBits::kDepthStencilAttachment);
  } else {
    usages.SetFlag(TextureUsageBits::kColorAttachment);
  }
  // 颜色格式通过 glTexSubImage 更新，压缩格式通过 glCompressedTexSubImage 更新
  usages.SetFlag(TextureUsageBits::kCanUpdate);
  if (p_cpu_readable) {
    usages.SetFlag(TextureUsageBits::kCpuRead);
  }
  return usages;
}

/**
 * @brief 创建采样器（实现侧；契约语义见 RDD 接口）
 *
 * @par 实现路径
 *   glGenSamplers 后逐参数下发（glSamplerParameter*）—— GL 3.3 的
 *   sampler object 将采样状态从纹理分离（对齐 Vulkan VkSampler），
 *   绑定于纹理单元（BindSampler），纹理本体保持"纯数据"。
 *
 * @par 过滤器组合展开
 *   GL 的 MIN_FILTER 是 min 与 mip 两维的复合枚举（4 选 1），
 *   契约的 min_filter/mip_filter 两字段在此展开为：
 *   NEAREST_MIPMAP_NEAREST / LINEAR_MIPMAP_NEAREST /
 *   NEAREST_MIPMAP_LINEAR / LINEAR_MIPMAP_LINEAR。
 *   MAG_FILTER 无 mip 维度，直映射。
 *
 * @par 可选项按需下发
 *   各向异性（需 GL_EXT_texture_filter_anisotropic）、比较模式
 *   （shadow sampler：比较结果代替采样值）仅在启用时设置。
 *
 * @return 新 SamplerID（封装 GL 采样器名）
 * @retval SamplerID() 生成失败
 */
SamplerID GLRenderingDeviceDriver::SamplerCreate(const SamplerState& p_state) {
  GLuint gl_sampler = 0;
  glGenSamplers(1, &gl_sampler);
  if (gl_sampler == 0) {
    ARHUD_LOG_ERROR("kFailed", "SamplerCreate: glGenSamplers returned 0");
    return SamplerID();
  }
  uint32 min_filter = GL_NEAREST_MIPMAP_NEAREST;
  if (p_state.mip_filter == SamplerFilter::kLinear) {
    if (p_state.min_filter == SamplerFilter::kLinear) {
      min_filter = GL_LINEAR_MIPMAP_LINEAR;
    } else {
      min_filter = GL_NEAREST_MIPMAP_LINEAR;
    }
  } else {
    if (p_state.min_filter == SamplerFilter::kLinear) {
      min_filter = GL_LINEAR_MIPMAP_NEAREST;
    }
  }
  uint32 mag_filter = SamplerFilterToGL(p_state.mag_filter);
  glSamplerParameteri(gl_sampler, GL_TEXTURE_MIN_FILTER,
                      static_cast<GLint>(min_filter));
  glSamplerParameteri(gl_sampler, GL_TEXTURE_MAG_FILTER,
                      static_cast<GLint>(mag_filter));
  glSamplerParameteri(
      gl_sampler, GL_TEXTURE_WRAP_S,
      static_cast<GLint>(SamplerRepeatModeToGL(p_state.repeat_u)));
  glSamplerParameteri(
      gl_sampler, GL_TEXTURE_WRAP_T,
      static_cast<GLint>(SamplerRepeatModeToGL(p_state.repeat_v)));
  glSamplerParameteri(
      gl_sampler, GL_TEXTURE_WRAP_R,
      static_cast<GLint>(SamplerRepeatModeToGL(p_state.repeat_w)));
  if (p_state.use_anisotropy) {
    glSamplerParameterf(gl_sampler, GL_MAX_TEXTURE_MAX_ANISOTROPY,
                        p_state.anisotropy_max);
  }
  if (p_state.enable_compare) {
    glSamplerParameteri(gl_sampler, GL_TEXTURE_COMPARE_MODE, GL_TRUE);
    glSamplerParameteri(
        gl_sampler, GL_TEXTURE_COMPARE_FUNC,
        static_cast<GLint>(CompareOperatorToGL(p_state.compare_op)));
  }
  glSamplerParameterf(gl_sampler, GL_TEXTURE_LOD_BIAS, p_state.lod_bias);
  glSamplerParameterf(gl_sampler, GL_TEXTURE_MIN_LOD, p_state.min_lod);
  glSamplerParameterf(gl_sampler, GL_TEXTURE_MAX_LOD, p_state.max_lod);
  return SamplerID(static_cast<uint64>(gl_sampler));
}

/** @brief 释放采样器（glDeleteSamplers，延迟删除语义；无簿记表 —— sampler
 * 无查询需求） */
void GLRenderingDeviceDriver::SamplerFree(SamplerID p_sampler) {
  if (!p_sampler.IsValid()) {
    return;
  }
  GLuint gl_sampler = static_cast<GLuint>(p_sampler.GetId());
  glDeleteSamplers(1, &gl_sampler);
}

/**
 * @brief 创建顶点格式（实现侧；契约语义见 RDD 接口）
 *
 * @par GL 语义转换：VertexFormat ⇒ VAO（本后端最大的一次语义偷换）
 *   Vulkan 里顶点格式（输入布局描述）与顶点缓冲绑定是**两件事**
 *   （ViState 描述 + vkCmdBindVertexBuffers 时给 stride）；
 *   GL 里 glVertexAttribPointer 的 stride/offset 烘焙进 VAO ——
 *   于是"格式"顺带固化了布局。副作用：同一格式配不同 stride
 *   的缓冲需要多个 VertexFormat（RD 层的 PipelineCacheKey 已
 *   将 stride 纳入属性描述，规避了此坑）。
 *
 * @par 属性格式映射
 *   DataFormat → (component_count, gl_type, normalized) 三元组：
 *   分量数按通道数（RGBA=4/RG=2/R=1）、类型查 DataFormatToGLType、
 *   Unorm/Srgb 系置 normalized（整数 → [0,1] 浮点解释）。
 *
 * @par 实例化除数
 *   frequency == kInstance 时 glVertexAttribDivisor(location, 1)
 *   —— 实例绘制下每实例前进一次的 instanced attribute（HUD
 *   场景暂未使用，接口能力保留）。
 *
 * @param[in] p_vertex_attribs 属性描述数组（location/format/
 *                             stride/offset/frequency）
 *
 * @return 新 VertexFormatID（**注意：封装的是 GLVertexFormatInfo
 *         簿记指针而非 VAO 名** —— 绘定时经簿记取 vao + 属性布局）
 * @retval VertexFormatID() VAO 生成失败
 */
VertexFormatID GLRenderingDeviceDriver::VertexFormatCreate(
    VectorView<VertexAttribute> p_vertex_attribs) {
  GLuint vao = 0;
  glGenVertexArrays(1, &vao);
  if (vao == 0) {
    ARHUD_LOG_ERROR("kFailed",
                    "VertexFormatCreate: glGenVertexArrays returned 0");
    return VertexFormatID();
  }
  state_cache_.BindVertexArray(vao);
  for (uint32 i = 0; i < p_vertex_attribs.Size(); ++i) {
    const VertexAttribute& attr = p_vertex_attribs[i];
    glEnableVertexAttribArray(attr.location);
    GLint component_count = 4;
    uint32 gl_type = GL_FLOAT;
    GLboolean normalized = GL_FALSE;
    switch (attr.format) {
      case DataFormat::kR8G8B8A8Unorm:
      case DataFormat::kR8G8B8A8Snorm:
      case DataFormat::kB8G8R8A8Unorm:
      case DataFormat::kR8G8B8A8Srgb:
      case DataFormat::kR8G8B8A8Uint:
      case DataFormat::kR8G8B8A8Sint:
        component_count = 4;
        gl_type = DataFormatToGLType(attr.format);
        break;
      case DataFormat::kR8G8Unorm:
      case DataFormat::kR8G8Snorm:
      case DataFormat::kR8G8Uint:
      case DataFormat::kR8G8Sint:
        component_count = 2;
        gl_type = DataFormatToGLType(attr.format);
        break;
      case DataFormat::kR8Unorm:
      case DataFormat::kR8Snorm:
      case DataFormat::kR8Uint:
      case DataFormat::kR8Sint:
        component_count = 1;
        gl_type = DataFormatToGLType(attr.format);
        break;
      case DataFormat::kR32G32B32A32Sfloat:
        component_count = 4;
        gl_type = GL_FLOAT;
        break;
      case DataFormat::kR32G32Sfloat:
        component_count = 2;
        gl_type = GL_FLOAT;
        break;
      case DataFormat::kR32Sfloat:
        component_count = 1;
        gl_type = GL_FLOAT;
        break;
      case DataFormat::kR16G16B16A16Sfloat:
        component_count = 4;
        gl_type = GL_HALF_FLOAT;
        break;
      case DataFormat::kR16G16Sfloat:
        component_count = 2;
        gl_type = GL_HALF_FLOAT;
        break;
      case DataFormat::kR16Sfloat:
        component_count = 1;
        gl_type = GL_HALF_FLOAT;
        break;
      default:
        component_count = 4;
        gl_type = GL_FLOAT;
        break;
    }
    if (attr.format == DataFormat::kR8G8B8A8Unorm ||
        attr.format == DataFormat::kR8G8Unorm ||
        attr.format == DataFormat::kR8Unorm ||
        attr.format == DataFormat::kB8G8R8A8Unorm ||
        attr.format == DataFormat::kR8G8B8A8Srgb) {
      normalized = GL_TRUE;
    }
    glVertexAttribPointer(
        attr.location, component_count, gl_type, normalized,
        static_cast<GLsizei>(attr.stride),
        reinterpret_cast<const void*>(static_cast<uintptr_t>(attr.offset)));
    if (attr.frequency == VertexFrequency::kInstance) {
      glVertexAttribDivisor(attr.location, 1);
    }
  }
  GL_CHECK_ERROR();
  GLVertexFormatInfo* info = vertex_format_allocator_.Alloc();
  info->vao = vao;
  for (uint32 i = 0; i < p_vertex_attribs.Size(); ++i) {
    info->attributes.PushBack(p_vertex_attribs[i]);
  }
  return VertexFormatID(reinterpret_cast<uint64>(info));
}

/**
 * @brief 释放顶点格式（实现侧）
 *
 * 经簿记指针取回 VAO 删除 + 归还 PagedAllocator 槽位。
 * 延迟删除语义：VAO 若仍被在途 draw 引用，驱动延迟回收。
 */
void GLRenderingDeviceDriver::VertexFormatFree(VertexFormatID p_vertex_format) {
  if (!p_vertex_format.IsValid()) {
    return;
  }
  GLVertexFormatInfo* info =
      reinterpret_cast<GLVertexFormatInfo*>(p_vertex_format.GetId());
  if (!info) {
    return;
  }
  if (info->vao != 0) {
    glDeleteVertexArrays(1, &info->vao);
  }
  vertex_format_allocator_.Free(info);
}

/**
 * @brief 从 GLSL 源码创建着色器（实现侧；契约语义见 RDD 接口）
 *
 * @par 完整路径（缓存优先）
 *   1. ComputeSourceHash 对全部 stage 源码做哈希
 *   2. LoadShaderCache 按哈希查磁盘缓存（shader_cache/ 目录）——
 *      命中直接复用 program（GL program 二进制经
 *      glProgramBinary 存取），跳过编译链接
 *   3. 未命中：逐 stage CompileShader（含 info-log 错误输出，
 *      任一失败则清理已编对象返回空 ID）→ LinkProgram
 *   4. 编译成功后的两次反射遍历（见下）→ SaveShaderCache 落盘
 *
 * @par 反射遍历一：UBO block 绑定（GLSL 330 的历史坑）
 *   GL 3.3 的 GLSL **无 layout(binding=N)**（4.2+ 才有）——
 *   uniform block 与绑定点须运行期 glUniformBlockBinding 关联。
 *   遍历用户声明的 UBO 类 uniform，按 block 名取
 *   glGetUniformBlockIndex 后绑到 uniform.binding。引擎因此
 *   能以"绑定点"（Vulkan 语义）而非"block 名"（GL 老语义）
 *   组织 uniform set。
 *
 * @par 反射遍历二：采样器 location 表
 *   采样器类 uniform 的 glGetUniformLocation 结果填
 *   uniform_locations 表（binding+type → location 映射）——
 *   UniformSet 绑定纹理时查表（GLShaderInfo::FindLocation）
 *   下发 glUniform1i 设置采样器指向的纹理单元。
 *
 * @par 契约参数的 GL 忽略项
 *   p_push_constant_size 忽略 —— GL 无 push constant（Vulkan
 *   专有），引擎侧等价能力走 UBO。
 *
 * @return 新 ShaderID（封装 GLShaderInfo 簿记指针）
 * @retval ShaderID() 无 stage / 编译 / 链接失败
 *
 * @see CompileShader/LinkProgram        （gl_shader_cache 分文件）
 * @see LoadShaderCache/SaveShaderCache  （磁盘缓存读写）
 */
ShaderID GLRenderingDeviceDriver::ShaderCreateFromSource(
    VectorView<ShaderStageSource> p_stage_sources,
    ShaderSourceFormat p_source_format, VectorView<ShaderUniform> p_uniforms,
    uint32 p_push_constant_size) {
  (void)p_push_constant_size;
  if (p_source_format != ShaderSourceFormat::kGLSL) {
    ARHUD_LOG_WARN(
        "ShaderCreateFromSource: unsupported source format %u (GL backend "
        "accepts kGLSL only)",
        static_cast<uint32>(p_source_format));
    return ShaderID();
  }
  if (p_stage_sources.Size() == 0) {
    ARHUD_LOG_WARN(
        "ShaderCreateFromSource: no stage sources, returning invalid ShaderID");
    return ShaderID();
  }
  // ── 尝试从磁盘缓存加载 ──
  uint64 source_hash = ComputeSourceHash(p_stage_sources);
  GLShaderInfo cached_info;
  if (LoadShaderCache(source_hash, p_uniforms, cached_info)) {
    GLShaderInfo* info = shader_allocator_.Alloc();
    info->program = cached_info.program;
    info->shader_objects = cached_info.shader_objects;
    info->uniform_locations = cached_info.uniform_locations;
    return ShaderID(reinterpret_cast<uint64>(info));
  }
  // ── 缓存未命中，从源码编译 ──
  LocalVector<uint32> compiled_shaders;
  for (uint32 i = 0; i < p_stage_sources.Size(); ++i) {
    const ShaderStageSource& stage_src = p_stage_sources[i];
    if (stage_src.source == nullptr) {
      ARHUD_LOG_WARN(
          "ShaderCreateFromSource: stage %u has null source, dropped",
          static_cast<uint32>(stage_src.stage));
      continue;
    }
    uint32 gl_type = 0;
    switch (stage_src.stage) {
      case ShaderStage::kVertex:
        gl_type = GL_VERTEX_SHADER;
        break;
      case ShaderStage::kFragment:
        gl_type = GL_FRAGMENT_SHADER;
        break;
      case ShaderStage::kTessellationControl:
        gl_type = GL_TESS_CONTROL_SHADER;
        break;
      case ShaderStage::kTessellationEvaluation:
        gl_type = GL_TESS_EVALUATION_SHADER;
        break;
      case ShaderStage::kCompute:
        gl_type = GL_COMPUTE_SHADER;
        break;
      default:
        ARHUD_LOG_WARN("ShaderCreateFromSource: unknown stage enum %u, dropped",
                       static_cast<uint32>(stage_src.stage));
        continue;
    }
    uint32 shader_obj = CompileShader(stage_src.source, gl_type);
    if (shader_obj == 0) {
      for (uint32 j = 0; j < compiled_shaders.Size(); ++j) {
        glDeleteShader(compiled_shaders[j]);
      }
      return ShaderID();
    }
    compiled_shaders.PushBack(shader_obj);
  }
  if (compiled_shaders.Size() == 0) {
    ARHUD_LOG_WARN(
        "ShaderCreateFromSource: no stages compiled, returning invalid "
        "ShaderID");
    return ShaderID();
  }
  uint32 program = LinkProgram(compiled_shaders);
  if (program == 0) {
    for (uint32 i = 0; i < compiled_shaders.Size(); ++i) {
      glDeleteShader(compiled_shaders[i]);
    }
    return ShaderID();
  }
  GL_CHECK_ERROR();
  GLShaderInfo* info = shader_allocator_.Alloc();
  info->program = program;
  info->shader_objects = compiled_shaders;
  // 绑定 UBO uniform block 到 binding point（GLSL 330 没有 layout(binding=)，
  // 必须通过 glUniformBlockBinding 手动关联）
  // 遍历用户声明的 uniform，对 UBO 类型按名称查找 block index 并绑定
  for (uint32 i = 0; i < p_uniforms.Size(); ++i) {
    const ShaderUniform& uniform = p_uniforms[i];
    if ((uniform.type == UniformType::kUniformBuffer ||
         uniform.type == UniformType::kUniformBufferDynamic ||
         uniform.type == UniformType::kStorageBuffer ||
         uniform.type == UniformType::kStorageBufferDynamic) &&
        uniform.name != nullptr) {
      GLuint block_index = glGetUniformBlockIndex(program, uniform.name);
      if (block_index != GL_INVALID_INDEX) {
        glUniformBlockBinding(program, block_index, uniform.binding);
      }
    }
  }
  for (uint32 i = 0; i < p_uniforms.Size(); ++i) {
    const ShaderUniform& uniform = p_uniforms[i];
    GLShaderInfo::UniformLocation loc;
    loc.binding = uniform.binding;
    loc.type = uniform.type;
    loc.location = -1;
    switch (uniform.type) {
      case UniformType::kSamplerWithTexture:
      case UniformType::kSamplerWithTextureBuffer:
      case UniformType::kTexture:
      case UniformType::kTextureBuffer:
      case UniformType::kImage:
      case UniformType::kImageBuffer:
      case UniformType::kInputAttachment:
        if (uniform.name != nullptr) {
          loc.location = glGetUniformLocation(program, uniform.name);
        }
        break;
      default:
        break;
    }
    info->uniform_locations.PushBack(loc);
  }
  // ── 保存到磁盘缓存 ──
  SaveShaderCache(source_hash, *info);
  return ShaderID(reinterpret_cast<uint64>(info));
}

/**
 * @brief 释放着色器（实现侧）
 *
 * glDeleteProgram + 逐个 glDeleteShader（program 已链，shader
 * 对象本可创建后即删——保留是缓存路径复用的需要）+
 * 归还 allocator 槽位。无效 ID 静默返回。
 */
void GLRenderingDeviceDriver::ShaderFree(ShaderID p_shader) {
  if (!p_shader.IsValid()) {
    return;
  }
  GLShaderInfo* info = reinterpret_cast<GLShaderInfo*>(p_shader.GetId());
  if (!info) {
    return;
  }
  if (info->program != 0) {
    glDeleteProgram(info->program);
  }
  for (uint32 i = 0; i < info->shader_objects.Size(); ++i) {
    glDeleteShader(info->shader_objects[i]);
  }
  shader_allocator_.Free(info);
}

/**
 * @brief 创建 uniform set（实现侧；契约语义见 RDD 接口）
 *
 * @par GL 语义转换：描述符集 ⇒ 绑定资源记录表
 *   GL 无描述符集（descriptor set 是 Vulkan 概念），本实现把
 *   BoundUniform 列表翻译为"待绑定资源"记录（GLUniformSetInfo），
 *   **创建时零 GL 调用** —— 真正的 glBindSampler/glBindBuffer/
 *   glUniform1i 推迟到 CommandBindUniformSet（绘制时）逐条下发。
 *   这保留了 Vulkan 的"set 一次创建、多次绑定"使用形态。
 *
 * @par 资源记录的字段抽取
 *   - 采样器类：ids 按序对 [SamplerID, TextureID] 拆两个字段
 *     （单元素退化为无 sampler，绑定时用 0 号默认采样器）
 *   - 其余类型：取首 ID 为 gl_name
 *   - Dynamic UBO/SSBO：额外记录 range_size（draw 时走
 *     glBindBufferRange 的动态偏移语义）
 *
 * @param[in] p_uniforms  绑定资源描述数组
 * @param[in] p_shader    所属着色器（运行期校验用）
 * @param[in] p_set_index set 序号（GL 忽略 —— 无 set 槽概念，
 *                        契约保留供 Vulkan）
 *
 * @return 新 UniformSetID（封装 GLUniformSetInfo 簿记指针）
 */
UniformSetID GLRenderingDeviceDriver::UniformSetCreate(
    VectorView<BoundUniform> p_uniforms, ShaderID p_shader,
    uint32 p_set_index) {
  GLUniformSetInfo* info = uniform_set_allocator_.Alloc();
  info->shader = p_shader;
  info->set_index = p_set_index;
  for (uint32 i = 0; i < p_uniforms.Size(); ++i) {
    const BoundUniform& uniform = p_uniforms[i];
    GLUniformSetInfo::BoundResource res;
    res.type = uniform.type;
    res.binding = uniform.binding;
    switch (uniform.type) {
      case UniformType::kSamplerWithTexture:
      case UniformType::kSamplerWithTextureBuffer:
        // IDs come in pairs: [SamplerID, TextureID]
        if (uniform.ids.Size() >= 2) {
          res.gl_sampler_name = static_cast<uint32>(uniform.ids[0].GetId());
          res.gl_name = static_cast<uint32>(uniform.ids[1].GetId());
        } else if (uniform.ids.Size() == 1) {
          // 只有 texture，没有 sampler（使用默认 sampler 0）
          res.gl_name = static_cast<uint32>(uniform.ids[0].GetId());
        }
        break;
      default:
        if (uniform.ids.Size() > 0) {
          res.gl_name = static_cast<uint32>(uniform.ids[0].GetId());
        }
        break;
    }
    // 动态 UBO/SSBO 存储 range_size，draw 时用 glBindBufferRange
    if (uniform.type == UniformType::kUniformBufferDynamic ||
        uniform.type == UniformType::kStorageBufferDynamic) {
      res.range_size = uniform.range_size;
    }
    info->resources.PushBack(res);
  }
  return UniformSetID(reinterpret_cast<uint64>(info));
}

/**
 * @brief 释放 uniform set（实现侧）
 *
 * 仅归还 allocator 槽位 —— set 创建时零 GL 对象（纯簿记，
 * 见 UniformSetCreate），无需 GL 层释放。
 */
void GLRenderingDeviceDriver::UniformSetFree(UniformSetID p_uniform_set) {
  if (!p_uniform_set.IsValid()) {
    return;
  }
  GLUniformSetInfo* info =
      reinterpret_cast<GLUniformSetInfo*>(p_uniform_set.GetId());
  if (!info) {
    return;
  }
  uniform_set_allocator_.Free(info);
}

/**
 * @brief 创建 render pass（实现侧；契约语义见 RDD 接口）
 *
 * @par GL 语义转换：纯簿记（GL 无 render pass 对象）
 *   Vulkan 的 VkRenderPass（附件布局转换/子 pass 依赖/屏障）
 *   在 GL 无对应物 —— GL 的 FBO 附件布局全程通用，屏障隐式。
 *   本实现仅把附件描述存入 GLRenderPassInfo 供后续参考
 *   （FramebufferCreate 的附件兼容性推断等）。
 *
 * @par 契约参数的 GL 忽略项
 *   p_subpasses / p_subpass_dependencies 忽略 —— GL 无子 pass
 *   概念（Vulkan 的 mobile 渲染优化点），单 pass 语义由
 *   CommandBeginRenderPass/EndRenderPass 对承担。
 *
 * @return 新 RenderPassID（封装簿记指针，非 GL 对象）
 */
RenderPassID GLRenderingDeviceDriver::RenderPassCreate(
    VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses,
    VectorView<SubpassDependency> p_subpass_dependencies) {
  (void)p_subpasses;
  (void)p_subpass_dependencies;
  GLRenderPassInfo* info = render_pass_allocator_.Alloc();
  for (uint32 i = 0; i < p_attachments.Size(); ++i) {
    info->attachments.PushBack(p_attachments[i]);
  }
  return RenderPassID(reinterpret_cast<uint64>(info));
}

/** @brief 释放 render pass（纯簿记归还，无 GL 对象 —— 见 RenderPassCreate） */
void GLRenderingDeviceDriver::RenderPassFree(RenderPassID p_render_pass) {
  if (!p_render_pass.IsValid()) {
    return;
  }
  GLRenderPassInfo* info =
      reinterpret_cast<GLRenderPassInfo*>(p_render_pass.GetId());
  if (!info) {
    return;
  }
  render_pass_allocator_.Free(info);
}

/**
 * @brief 创建帧缓冲（实现侧；契约语义见 RDD 接口）
 *
 * @par 实现路径
 *   1. glGenFramebuffers + 经 state_cache_ 绑定（保持缓存一致）
 *   2. 逐附件挂接：查 texture_infos_ 簿记判格式族 →
 *      深度/模板挂 GL_DEPTH(_STENCIL)_ATTACHMENT、颜色按
 *      到达序挂 COLOR_ATTACHMENT0+N（契约的附件数组顺序即
 *      颜色槽序 —— 与 RenderPass 附件描述对齐）
 *   3. glDrawBuffers 声明绘制目标集（仅深度 FBO 则
 *      GL_NONE + glReadBuffer(GL_NONE) —— 阴影贴图等纯深度场景）
 *   4. glCheckFramebufferStatus 完整性校验，失败仅 ERROR 日志
 *      （**不返回失败** —— 调用方可继续创建流程，draw 时驱动
 *      自然报错；宽容策略换初始化期不因单附件中断）
 *
 * @par 契约参数的 GL 忽略项
 *   p_render_pass 忽略 —— GL 的 FBO 兼容性由驱动隐式校验
 *   （Vulkan 需 pass 与 framebuffer 附件描述匹配）。
 *
 * @param[in] p_render_pass  所属 render pass（忽略，契约保留）
 * @param[in] p_attachments  纹理附件数组（颜色/深度顺序无关，
 *                           按格式自动分流）
 * @param[in] p_width/p_height 尺寸簿记
 *
 * @return 新 FramebufferID（封装 GLFramebufferInfo 簿记指针）
 * @retval FramebufferID() FBO 生成失败
 */
FramebufferID GLRenderingDeviceDriver::FramebufferCreate(
    RenderPassID p_render_pass, VectorView<TextureID> p_attachments,
    uint32 p_width, uint32 p_height) {
  (void)p_render_pass;
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  if (fbo == 0) {
    ARHUD_LOG_ERROR("kFailed",
                    "FramebufferCreate: glGenFramebuffers returned 0");
    return FramebufferID();
  }
  state_cache_.BindFramebuffer(fbo);
  GLFramebufferInfo* info = framebuffer_allocator_.Alloc();
  info->fbo = fbo;
  info->width = p_width;
  info->height = p_height;
  uint32 color_index = 0;
  for (uint32 i = 0; i < p_attachments.Size(); ++i) {
    GLuint tex = static_cast<GLuint>(p_attachments[i].GetId());
    if (tex == 0) {
      continue;
    }
    // 查找纹理格式和 target 以判断附件类型
    DataFormat tex_format = DataFormat::kR8G8B8A8Unorm;
    GLenum tex_target = GL_TEXTURE_2D;
    const GLTextureInfo* tex_info =
        texture_infos_.GetPtr(static_cast<uint64>(tex));
    if (tex_info != nullptr) {
      tex_format = tex_info->format;
      tex_target = tex_info->target;
    } else {
      // 未知纹理（外部/已释放/陈旧 ID）：按 RGBA8/2D 兜底 ——
      // 深度纹理会被误挂成颜色附件，直接改变渲染结果
      ARHUD_LOG_WARN(
          "FramebufferCreate: attachment %u references unknown texture %u, "
          "assuming RGBA8/2D",
          i, tex);
    }
    // 统一判定深度/模板格式族（唯一权威判定见 _format.cpp）
    if (DataFormatIsDepthStencil(tex_format)) {
      GLenum attachment = GL_DEPTH_ATTACHMENT;
      if (DataFormatHasStencil(tex_format) &&
          tex_format != DataFormat::kS8Uint) {
        // 深度+模板组合格式：共享挂点
        attachment = GL_DEPTH_STENCIL_ATTACHMENT;
      } else if (tex_format == DataFormat::kS8Uint) {
        // 纯模板：独立 STENCIL 挂点（此前误走 DEPTH 挂点，
        // glCheckFramebufferStatus 必报 incomplete）
        attachment = GL_STENCIL_ATTACHMENT;
      }
      glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, tex_target, tex, 0);
      info->depth_stencil_attachment = tex;
    } else {
      GLenum attachment = GL_COLOR_ATTACHMENT0 + color_index;
      glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, tex_target, tex, 0);
      info->color_attachments.PushBack(tex);
      ++color_index;
    }
  }
  if (color_index > 0) {
    GLenum* draw_buffers =
        static_cast<GLenum*>(memory::Alloc(sizeof(GLenum) * color_index));
    for (uint32 i = 0; i < color_index; ++i) {
      draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
    }
    glDrawBuffers(static_cast<GLsizei>(color_index), draw_buffers);
    memory::Free(draw_buffers);
  } else {
    // 仅深度附件的 FBO，不需要绘制颜色缓冲区
    GLenum draw_buffer = GL_NONE;
    glDrawBuffers(1, &draw_buffer);
    glReadBuffer(GL_NONE);
  }
  // 检查帧缓冲完整性
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    const char* status_str = "UNKNOWN";
    switch (status) {
      case GL_FRAMEBUFFER_UNDEFINED:
        status_str = "UNDEFINED";
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
        status_str = "INCOMPLETE_ATTACHMENT";
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
        status_str = "MISSING_ATTACHMENT";
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
        status_str = "INCOMPLETE_DRAW_BUFFER";
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
        status_str = "INCOMPLETE_READ_BUFFER";
        break;
      case GL_FRAMEBUFFER_UNSUPPORTED:
        status_str = "UNSUPPORTED";
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
        status_str = "INCOMPLETE_MULTISAMPLE";
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
        status_str = "INCOMPLETE_LAYER_TARGETS";
        break;
      default:
        break;
    }
    ARHUD_LOG_ERROR("kFailed",
                    "FramebufferCreate() framebuffer incomplete: %s (0x%04X), "
                    "color_attachments=%u, depth_stencil=%u, size=%ux%u",
                    status_str, static_cast<uint32>(status), color_index,
                    info->depth_stencil_attachment != 0 ? 1u : 0u, p_width,
                    p_height);
  }
  GL_CHECK_ERROR();
  return FramebufferID(reinterpret_cast<uint64>(info));
}

/** @brief 释放帧缓冲（glDeleteFramebuffers + 簿记归还；延迟删除语义） */
void GLRenderingDeviceDriver::FramebufferFree(FramebufferID p_framebuffer) {
  if (!p_framebuffer.IsValid()) {
    return;
  }
  GLFramebufferInfo* info =
      reinterpret_cast<GLFramebufferInfo*>(p_framebuffer.GetId());
  if (!info) {
    return;
  }
  if (info->fbo != 0) {
    glDeleteFramebuffers(1, &info->fbo);
  }
  framebuffer_allocator_.Free(info);
}

/**
 * @brief 回读帧缓冲像素（实现侧）
 *
 * @par 状态表身份：**模式 R（回读呈现）专用口子**
 *   渲染线程把 FBO 内容拉回 CPU 供 PresentThread 的 GDI 上屏。
 *   该路径在 RDD 契约状态表中标记为"偏差"—— 生产零拷贝路径
 *   （EGLImage 导出）不用回读，B3 后应移出契约（见
 *   rendering_device_driver.h 文件头状态表）。
 *
 * @par 实现细节
 *   - 绑 GL_READ_FRAMEBUFFER（直调 glBindFramebuffer —— 刻意
 *     不走 state_cache_：READ 绑定与 DRAW 绑定是不同槽位，
 *     污染 draw 缓存反而破坏状态一致性）
 *   - GL_BGRA + GL_UNSIGNED_BYTE 输出格式 —— 与 Win32
 *     BITMAPINFO 的 BGRA 字节序直接匹配（StretchDIBits 零转换）
 *   - 同步回读（无 PBO 异步）—— ~1-2ms/屏 的已知 dev 税，
 *     PBO 优化在 backlog
 *
 * @param[in] p_framebuffer 目标 FBO
 * @param[in] p_x/p_y/p_width/p_height 回读矩形（GL 坐标系，原点左下）
 * @param[out] p_dst 目标 CPU 缓冲（调用方保证容量）
 */
void GLRenderingDeviceDriver::FramebufferReadback(FramebufferID p_framebuffer,
                                                  uint32 p_x, uint32 p_y,
                                                  uint32 p_width,
                                                  uint32 p_height,
                                                  void* p_dst) {
  if (!p_framebuffer.IsValid()) {
    return;
  }
  GLFramebufferInfo* info =
      reinterpret_cast<GLFramebufferInfo*>(p_framebuffer.GetId());
  if (info == nullptr || info->fbo == 0) {
    return;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, info->fbo);
  glReadPixels(p_x, p_y, p_width, p_height, GL_BGRA, GL_UNSIGNED_BYTE, p_dst);
  GL_CHECK_ERROR();
}

/**
 * @brief 创建渲染管线（实现侧；契约语义见 RDD 接口）
 *
 * @par GL 语义转换：PSO ⇒ GLPipelineState 结构体（模拟）
 *   GL 无 Pipeline State Object —— 本实现把全部管线状态
 *   （shader program 引用/VAO/图元/剔除/深度/混合/线框）烘焙
 *   进 GLPipelineState 结构；CommandBindRenderPipeline 时经
 *   state_cache_ 逐项下发（见文件头"OpenGL 特殊策略"）。
 *   创建期零 GL 调用（纯簿记）。
 *
 * @par 契约参数的 GL 忽略项
 *   p_render_pass 忽略 —— GL 管线兼容性由驱动 draw 时校验
 *   （Vulkan 需 pass 兼容的 pipeline）。p_dynamic_state 忽略
 *   —— GL 全部状态本就是动态设置。
 *
 * @return 新 PipelineID（封装 GLPipelineState 簿记指针）
 * @retval PipelineID() 参数无效
 */
PipelineID GLRenderingDeviceDriver::RenderPipelineCreate(
    ShaderID p_shader, VertexFormatID p_vertex_format,
    RenderPrimitive p_render_primitive,
    const PipelineRasterizationState& p_rasterization_state,
    const PipelineMultisampleState& p_multisample_state,
    const PipelineDepthStencilState& p_depth_stencil_state,
    const PipelineColorBlendState& p_blend_state,
    BitField<PipelineDynamicStateFlags> p_dynamic_state,
    RenderPassID p_render_pass) {
  (void)p_render_pass;
  GLPipelineState* pipeline = pipeline_allocator_.Alloc();
  pipeline->shader_id = p_shader;
  GLShaderInfo* shader_info = reinterpret_cast<GLShaderInfo*>(p_shader.GetId());
  pipeline->shader_program = shader_info ? shader_info->program : 0;
  if (p_vertex_format.IsValid()) {
    GLVertexFormatInfo* vf_info =
        reinterpret_cast<GLVertexFormatInfo*>(p_vertex_format.GetId());
    if (vf_info) {
      pipeline->vao = vf_info->vao;
      pipeline->vertex_attributes = vf_info->attributes;
    }
  }
  pipeline->render_primitive = p_render_primitive;
  pipeline->cull_enable =
      (p_rasterization_state.cull_mode != PolygonCullMode::kDisabled);
  switch (p_rasterization_state.cull_mode) {
    case PolygonCullMode::kFront:
      pipeline->cull_mode = GL_FRONT;
      break;
    case PolygonCullMode::kBack:
      pipeline->cull_mode = GL_BACK;
      break;
    default:
      pipeline->cull_mode = GL_BACK;
      break;
  }
  pipeline->front_face =
      (p_rasterization_state.front_face == PolygonFrontFace::kClockwise)
          ? GL_CW
          : GL_CCW;
  pipeline->depth_test_enable = p_depth_stencil_state.enable_depth_test;
  pipeline->depth_write_enable = p_depth_stencil_state.enable_depth_write;
  pipeline->depth_func =
      CompareOperatorToGL(p_depth_stencil_state.depth_compare_operator);
  pipeline->blend_enable = false;
  if (p_blend_state.attachments.Size() > 0) {
    const PipelineColorBlendAttachment& blend_att =
        p_blend_state.attachments[0];
    pipeline->blend_enable = blend_att.enable_blend;
    pipeline->blend_src_rgb = BlendFactorToGL(blend_att.src_color_blend_factor);
    pipeline->blend_dst_rgb = BlendFactorToGL(blend_att.dst_color_blend_factor);
    pipeline->blend_src_alpha =
        BlendFactorToGL(blend_att.src_alpha_blend_factor);
    pipeline->blend_dst_alpha =
        BlendFactorToGL(blend_att.dst_alpha_blend_factor);
  }
  // MSAA 状态
  pipeline->multisample_enable =
      (p_multisample_state.sample_count != TextureSamples::k1);
  pipeline->sample_shading_enable = p_multisample_state.enable_sample_shading;
  pipeline->min_sample_shading = p_multisample_state.min_sample_shading;
  pipeline->alpha_to_coverage_enable =
      p_multisample_state.enable_alpha_to_coverage;
  pipeline->alpha_to_one_enable = p_multisample_state.enable_alpha_to_one;
  pipeline->wireframe = p_rasterization_state.wireframe;
  pipeline->dynamic_state = p_dynamic_state;
  return PipelineID(reinterpret_cast<uint64>(pipeline));
}

/** @brief 释放管线（纯簿记归还 —— PSO 是结构体模拟，无 GL 对象可删） */
void GLRenderingDeviceDriver::PipelineFree(PipelineID p_pipeline) {
  if (!p_pipeline.IsValid()) {
    return;
  }
  GLPipelineState* pipeline =
      reinterpret_cast<GLPipelineState*>(p_pipeline.GetId());
  if (!pipeline) {
    return;
  }
  pipeline_allocator_.Free(pipeline);
}

/**
 * @brief 创建计算管线（实现侧）
 *
 * @par 运行时能力门槛
 *   计算 shader 需桌面 GL 4.3+ 或 GLES 3.1+ —— 引擎基线 3.3
 *   通常不满足，本方法按能力探测返回空 ID（HUD 场景无计算
 *   需求，接口保留为契约完整性）。管线状态全取"无渲染"默认。
 *
 * @return 新 PipelineID；版本不支持返回 PipelineID()
 */
PipelineID GLRenderingDeviceDriver::ComputePipelineCreate(ShaderID p_shader) {
  // 运行时检查：桌面 OpenGL 4.3+ 或 OpenGL ES 3.1+
  const bool compute_supported =
      capabilities_.device_family == DeviceFamily::kOpenGLES
          ? (capabilities_.version_major > 3 ||
             (capabilities_.version_major == 3 &&
              capabilities_.version_minor >= 1))
          : (capabilities_.version_major > 4 ||
             (capabilities_.version_major == 4 &&
              capabilities_.version_minor >= 3));
  if (!compute_supported) {
    ARHUD_LOG_ERROR("kUnsupported",
                    "ComputePipelineCreate requires OpenGL 4.3+ or OpenGL ES "
                    "3.1+, current: %s %u.%u",
                    api_name_, capabilities_.version_major,
                    capabilities_.version_minor);
    return PipelineID();
  }
  GLPipelineState* pipeline = pipeline_allocator_.Alloc();
  pipeline->shader_id = p_shader;
  GLShaderInfo* shader_info = reinterpret_cast<GLShaderInfo*>(p_shader.GetId());
  pipeline->shader_program = shader_info ? shader_info->program : 0;
  // 计算管线不需要 VAO 和渲染状态
  pipeline->vao = 0;
  pipeline->render_primitive = RenderPrimitive::kTriangles;
  pipeline->cull_enable = false;
  pipeline->depth_test_enable = false;
  pipeline->depth_write_enable = false;
  pipeline->blend_enable = false;
  pipeline->wireframe = false;
  return PipelineID(reinterpret_cast<uint64>(pipeline));
}

/**
 * @brief 开始帧段（Godot RDD 侧概念：帧的驱动级初始化）
 *
 * 记录帧索引/已绘帧数 + **state_cache_ 全量复位** —— 段边界
 * 即缓存失效边界：新段的第一批绑定全部真实下发，杜绝任何
 * 跨段状态泄漏假设。
 *
 * @param[in] p_frame_index 帧槽索引（帧循环簿记）
 * @param[in] p_frames_drawn 已绘帧计数
 */
void GLRenderingDeviceDriver::BeginSegment(uint32 p_frame_index,
                                           uint32 p_frames_drawn) {
  frame_index_ = p_frame_index;
  frames_drawn_ = p_frames_drawn;
  state_cache_.Reset();
}

/** @brief 结束帧段（当前无驱动级收尾动作；预留对齐 Godot RDD） */
void GLRenderingDeviceDriver::EndSegment() {}

/** @brief 查询设备限制（limits_ 数组直读；Initialize 时 QueryLimits 填充） */
uint64 GLRenderingDeviceDriver::LimitGet(Limit p_limit) {
  uint32 idx = static_cast<uint32>(p_limit);
  if (idx < static_cast<uint32>(Limit::kMax)) {
    return limits_[idx];
  }
  return 0;
}

/**
 * @brief 查询特性支持（实现侧——恒 false）
 *
 * @return 恒 false（Features 枚举当前对应的能力在 GL 3.3 基线
 *         均不可用或引擎未启用；占位对齐 Godot RDD 接口）
 */
bool GLRenderingDeviceDriver::HasFeature(Features p_feature) {
  (void)p_feature;
  return false;
}

/** @brief 取能力快照（Initialize 时嗅探的版本/设备族等） */
const Capabilities& GLRenderingDeviceDriver::GetCapabilities() const {
  return capabilities_;
}

/** @brief 取 API 显示名（如 "OpenGL 4.6"/"OpenGL ES 3.2"） */
const char* GLRenderingDeviceDriver::GetApiName() const {
  return api_name_;
}

/**
 * @brief 取管线缓存 UUID（实现侧——占位全零）
 *
 * @return 恒 "00000000-..."（GL 无 PSO 管线缓存概念，Vulkan
 *         下对应 VkPhysicalDeviceProperties.pipelineCacheUUID；
 *         引擎的 shader 磁盘缓存（LoadShaderCache）独立于此）
 */
const char* GLRenderingDeviceDriver::GetPipelineCacheUuid() const {
  return pipeline_cache_uuid_;
}

/**
 * @brief 查询 GL 限制填充 limits_（Initialize 时调用一次）
 *
 * @par 两类来源
 *   - 查询型：glGetIntegerv 实测（纹理尺寸上限/UBO 对齐/
 *     顶点属性数等 —— 驱动真实值）
 *   - 保守型：GL 3.3 规范下限直接写死（uniform set 容量/索引
 *     上限/视口上限等 —— 保证低于任何驱动的安全值）
 */
void GLRenderingDeviceDriver::QueryLimits() {
  GLint val = 0;

  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &val);
  limits_[static_cast<uint32>(Limit::kMaxTextureSize2D)] =
      static_cast<uint64>(val);

  glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &val);
  limits_[static_cast<uint32>(Limit::kMaxTextureSizeCube)] =
      static_cast<uint64>(val);

  glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &val);
  limits_[static_cast<uint32>(Limit::kMaxTexturesPerShaderStage)] =
      static_cast<uint64>(val);
  limits_[static_cast<uint32>(Limit::kMaxSamplersPerShaderStage)] =
      static_cast<uint64>(val);

  glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &val);
  limits_[static_cast<uint32>(Limit::kMaxUniformBufferSize)] =
      static_cast<uint64>(val);

  glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &val);
  limits_[static_cast<uint32>(Limit::kMaxVertexInputAttributes)] =
      static_cast<uint64>(val);
  limits_[static_cast<uint32>(Limit::kMaxVertexInputBindings)] =
      static_cast<uint64>(val);

  glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &val);
  limits_[static_cast<uint32>(Limit::kMaxTextureArrayLayers)] =
      static_cast<uint64>(val);

  glGetIntegerv(GL_MAX_FRAMEBUFFER_WIDTH, &val);
  limits_[static_cast<uint32>(Limit::kMaxFramebufferWidth)] =
      static_cast<uint64>(val);

  glGetIntegerv(GL_MAX_FRAMEBUFFER_HEIGHT, &val);
  limits_[static_cast<uint32>(Limit::kMaxFramebufferHeight)] =
      static_cast<uint64>(val);

  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &val);
  limits_[static_cast<uint32>(Limit::kMinUniformBufferOffsetAlignment)] =
      static_cast<uint64>(val);

  limits_[static_cast<uint32>(Limit::kMaxBoundUniformSets)] = 4;
  limits_[static_cast<uint32>(Limit::kMaxFramebufferColorAttachments)] = 4;
  limits_[static_cast<uint32>(Limit::kMaxTexturesPerUniformSet)] = 16;
  limits_[static_cast<uint32>(Limit::kMaxSamplersPerUniformSet)] = 16;
  limits_[static_cast<uint32>(Limit::kMaxUniformBuffersPerUniformSet)] = 12;
  limits_[static_cast<uint32>(Limit::kMaxStorageBuffersPerUniformSet)] = 8;
  limits_[static_cast<uint32>(Limit::kMaxStorageImagesPerUniformSet)] = 8;
  limits_[static_cast<uint32>(Limit::kMaxDrawIndexedIndex)] = 0x00FFFFFF;
  limits_[static_cast<uint32>(Limit::kMaxPushConstantSize)] = 128;
  limits_[static_cast<uint32>(Limit::kMaxVertexInputAttributeOffset)] = 2048;
  limits_[static_cast<uint32>(Limit::kMaxVertexInputBindingStride)] = 2048;
  limits_[static_cast<uint32>(Limit::kMaxViewportDimensionsX)] = 4096;
  limits_[static_cast<uint32>(Limit::kMaxViewportDimensionsY)] = 4096;
  limits_[static_cast<uint32>(Limit::kMaxShaderVaryings)] = 60;
}

uint32 GLRenderingDeviceDriver::CompileShader(const char* p_source,
                                              uint32 p_type) {
  if (!p_source) {
    return 0;
  }

  GLuint shader = glCreateShader(p_type);
  if (shader == 0) {
    ARHUD_LOG_ERROR("kFailed",
                    "GLRenderingDeviceDriver: Failed to create shader object");
    return 0;
  }

  glShaderSource(shader, 1, &p_source, nullptr);

  glCompileShader(shader);

  GL_CHECK_ERROR();

  GLint compile_status = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);

  if (compile_status == 0) {
    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

    if (log_length > 0) {
      char* log = static_cast<char*>(
          memory::Alloc(static_cast<uint64>(log_length + 1)));
      if (log) {
        glGetShaderInfoLog(shader, log_length, nullptr, log);
        ARHUD_LOG_ERROR("GLRenderingDeviceDriver: Shader compile error: %s",
                        log);
        memory::Free(log);
      } else {
        ARHUD_LOG_ERROR("kFailed",
                        "GLRenderingDeviceDriver: Shader compile failed (info "
                        "log %d bytes, alloc failed)",
                        log_length);
      }
    } else {
      ARHUD_LOG_ERROR(
          "kFailed",
          "GLRenderingDeviceDriver: Shader compile failed (no info log)");
    }

    glDeleteShader(shader);
    return 0;
  }

  return shader;
}

uint32 GLRenderingDeviceDriver::LinkProgram(
    const LocalVector<uint32>& p_shader_objects) {
  GLuint program = glCreateProgram();
  if (program == 0) {
    ARHUD_LOG_ERROR("kFailed",
                    "GLRenderingDeviceDriver: Failed to create program object");
    return 0;
  }

  for (uint32 i = 0; i < p_shader_objects.Size(); ++i) {
    glAttachShader(program, p_shader_objects[i]);
  }

  glLinkProgram(program);

  GL_CHECK_ERROR();

  GLint link_status = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &link_status);

  if (link_status == 0) {
    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);

    if (log_length > 0) {
      char* log = static_cast<char*>(
          memory::Alloc(static_cast<uint64>(log_length + 1)));
      if (log) {
        glGetProgramInfoLog(program, log_length, nullptr, log);
        ARHUD_LOG_ERROR("GLRenderingDeviceDriver: Program link error: %s", log);
        memory::Free(log);
      } else {
        ARHUD_LOG_ERROR("kFailed",
                        "GLRenderingDeviceDriver: Program link failed (info "
                        "log %d bytes, alloc failed)",
                        log_length);
      }
    } else {
      ARHUD_LOG_ERROR(
          "kFailed",
          "GLRenderingDeviceDriver: Program link failed (no info log)");
    }

    glDeleteProgram(program);
    return 0;
  }

  return program;
}

}  // namespace arhud
