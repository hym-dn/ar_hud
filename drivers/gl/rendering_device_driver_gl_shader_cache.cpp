/**
 * @file rendering_device_driver_gl_shader_cache.cpp
 * @brief GLRenderingDeviceDriver Shader 编译缓存实现
 *
 * 包含 Shader 编译缓存相关方法：
 * - ComputeSourceHash() — 计算 Shader 源码哈希（FNV-1a）
 * - LoadShaderCache() — 从磁盘加载缓存的 Shader 二进制
 * - SaveShaderCache() — 将编译后的 Shader 二进制保存到磁盘
 *
 * 缓存文件格式：
 *   [4B 魔数 0x41524853] [4B 版本号 1] [4B GLenum binary_format]
 *   [4B binary_size] [NB 二进制数据]
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-06-14
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include <direct.h>
#include <glad/gl.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "io/logger.h"
#include "os/memory.h"
#include "rendering_device_driver_gl.h"

namespace arhud {
/**
 * @brief 计算着色器源码哈希（实现侧；缓存键生成）
 *
 * @par 算法
 *   FNV-1a 64 位：初值 14695981039346656037（offset basis，
 *   0xcbf29ce484222325），乘子 1099511628211（prime，
 *   0x100000001b3）。逐 stage 先哈希 4 字节 stage 枚举值
 *   （小端逐字节），再哈希源码字符串至 '\0'（不含终止符），
 *   stage 顺序参与哈希 —— 同一组 stage 顺序不同即不同键。
 *   source 为 nullptr 的 stage 整体跳过（含其 stage 枚举）。
 *
 * @par 哈希的覆盖面与盲区
 *   键只覆盖 stage 枚举 + 源码文本；uniform 声明表
 *   （p_uniforms）不参与 —— 同源码不同绑定配置会命中同一份
 *   缓存二进制，安全：UBO 绑定与采样器 location 在
 *   LoadShaderCache 命中时按本次 p_uniforms 重新反射/绑定，
 *   不依赖落盘时的配置。
 *
 * @note FNV-1a 非密码学哈希，理论碰撞存在（概率 ~2^-64 量级，
 *       实际源码规模下可忽略）；碰撞表现为加载到错误程序，
 *       链接校验（GL_LINK_STATUS）无法检出，属已知接受风险。
 *
 * @param[in] p_stage_sources 全部 stage 的源码与类型
 *
 * @return 64 位源码哈希，用作磁盘缓存文件名主体
 *
 * @see ShaderCreateFromSource （唯一调用方，缓存流程入口）
 */
uint64 GLRenderingDeviceDriver::ComputeSourceHash(
    VectorView<ShaderStageSource> p_stage_sources) const {
  // FNV-1a hash
  uint64 hash = 14695981039346656037ULL;
  for (uint32 i = 0; i < p_stage_sources.Size(); ++i) {
    const char* src = p_stage_sources[i].source;
    if (src == nullptr)
      continue;
    // Hash stage type
    uint32 stage = static_cast<uint32>(p_stage_sources[i].stage);
    for (int b = 0; b < 4; ++b) {
      hash ^= static_cast<uint64>((stage >> (b * 8)) & 0xFF);
      hash *= 1099511628211ULL;
    }
    // Hash source string
    while (*src != '\0') {
      hash ^= static_cast<uint64>(static_cast<uint8>(*src));
      hash *= 1099511628211ULL;
      ++src;
    }
  }
  return hash;
}

/**
 * @brief 从磁盘加载缓存的预编译程序（实现侧；缓存命中路径）
 *
 * @par 加载流程（任一步失败即返回 false，走源码重编译；
 *       异常路径记日志，文件不存在=冷启动常态不打日志）
 *   1. 定位文件 kShaderCacheDir/&lt;hash 十六进制 16 位&gt;.bin，
 *      打不开（不存在/权限）→ 未命中
 *   2. 顺序读 4 字段文件头：魔数 kCacheMagic("ARHS"，
 *      0x41524853) / 版本号 / GL binary_format / 二进制字节数；
 *      短读（文件截断，记 WARN）或魔数版本不符（格式演进/异
 *      产物，记 WARN）→ 弃用
 *   3. 按头部长度读二进制体 → glProgramBinary 直灌程序对象，
 *      免编译免链接
 *   4. 校验 GL_LINK_STATUS：程序二进制为驱动私有格式，驱动/
 *      显卡/版本变更后常不可用 —— 校验失败即自愈：删程序、
 *      删缓存文件、记 INFO 日志，下次走源码重编译并重新落盘
 *
 * @par 命中后的两次反射遍历（与编译路径完全对齐）
 *   - 遍历一（UBO）：UBO/SSBO 类 uniform 按名取
 *     glGetUniformBlockIndex 后 glUniformBlockBinding 绑到
 *     uniform.binding —— GL 3.3 GLSL 无 layout(binding=N)，
 *     运行期补绑（详见 ShaderCreateFromSource 的反射遍历一）
 *   - 遍历二（采样器）：采样器/纹理/图像类 uniform 的
 *     glGetUniformLocation 填 r_info.uniform_locations
 *     （binding+type → location 表）；其余类型 location 留 -1
 *
 * @note r_info.shader_objects 恒为空且属正常：缓存二进制只有
 *       program 整体，源码路径的各 stage shader 对象在链接后
 *       即删；ShaderFree 据此只删 program 不碰 shader objects。
 *
 * @param[in]  p_hash     源码哈希（ComputeSourceHash 产物，
 *                        决定缓存文件名）
 * @param[in]  p_uniforms uniform 声明表（驱动两次反射遍历的
 *                        输入，语义同编译路径）
 * @param[out] r_info     命中时填充 program 与 uniform_locations；
 *                        未命中时内容不确定，调用方应忽略
 *
 * @return 是否命中并成功装配
 * @retval true  r_info 就绪，调用方免编译直接使用
 * @retval false 未命中 / 文件损坏 / 格式不符 / 驱动不认 ——
 *               均为可重编译的软失败
 *
 * @see ComputeSourceHash （键来源）
 * @see SaveShaderCache   （落盘对侧）
 */
bool GLRenderingDeviceDriver::LoadShaderCache(
    uint64 p_hash, VectorView<ShaderUniform> p_uniforms, GLShaderInfo& r_info) {
  // 构建缓存文件路径: shader_cache/<hash>.bin
  char path[256] = {};
  std::snprintf(path, sizeof(path), "%s/%016llx.bin", kShaderCacheDir,
                static_cast<unsigned long long>(p_hash));
  std::FILE* fp = std::fopen(path, "rb");
  if (fp == nullptr) {
    return false;  // 缓存文件不存在
  }
  // 读取文件头：魔数 + 版本 + 二进制格式 + 二进制大小
  uint32 magic = 0;
  uint32 version = 0;
  GLenum binary_format = 0;
  uint32 binary_size = 0;
  if (std::fread(&magic, sizeof(uint32), 1, fp) != 1 ||
      std::fread(&version, sizeof(uint32), 1, fp) != 1 ||
      std::fread(&binary_format, sizeof(GLenum), 1, fp) != 1 ||
      std::fread(&binary_size, sizeof(uint32), 1, fp) != 1) {
    ARHUD_LOG_WARN(
        "Shader cache header truncated (corrupted?), recompiling: %016llx",
        static_cast<unsigned long long>(p_hash));
    std::fclose(fp);
    return false;
  }
  // 校验魔数和版本
  constexpr uint32 kCacheMagic = 0x41524853;  // "ARHS" (AR Hud Shader)
  constexpr uint32 kCacheVersion = 1;
  if (magic != kCacheMagic || version != kCacheVersion) {
    ARHUD_LOG_WARN(
        "Shader cache format mismatch (magic 0x%08x, version %u): %016llx",
        magic, version, static_cast<unsigned long long>(p_hash));
    std::fclose(fp);
    return false;
  }
  // 读取二进制数据
  void* binary_data = memory::Alloc(binary_size);
  if (binary_data == nullptr) {
    ARHUD_LOG_ERROR("kOutOfMemory", "LoadShaderCache: alloc %u bytes failed",
                    binary_size);
    std::fclose(fp);
    return false;
  }
  if (std::fread(binary_data, 1, binary_size, fp) != binary_size) {
    ARHUD_LOG_WARN("Shader cache binary truncated, recompiling: %016llx",
                   static_cast<unsigned long long>(p_hash));
    memory::Free(binary_data);
    std::fclose(fp);
    return false;
  }
  std::fclose(fp);
  // 使用 glProgramBinary 加载预编译程序
  GLuint program = glCreateProgram();
  if (program == 0) {
    ARHUD_LOG_ERROR("kFailed", "LoadShaderCache: glCreateProgram failed");
    memory::Free(binary_data);
    return false;
  }
  glProgramBinary(program, binary_format, binary_data,
                  static_cast<GLsizei>(binary_size));
  memory::Free(binary_data);
  // 验证链接状态
  GLint link_status = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &link_status);
  if (link_status == 0) {
    // 缓存无效（驱动更新等原因），删除程序和缓存文件
    glDeleteProgram(program);
    std::remove(path);
    ARHUD_LOG_INFO(
        "Shader cache invalid (driver change?), recompiling: %016llx",
        static_cast<unsigned long long>(p_hash));
    return false;
  }
  // 加载成功，绑定 UBO 和查询 uniform 位置
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
    r_info.uniform_locations.PushBack(loc);
  }
  r_info.program = program;
  // 从缓存加载的程序没有 shader objects（它们在编译时已被删除）
  // shader_objects 为空是正常的，ShaderFree 时只删除 program
  ARHUD_LOG_INFO("Shader cache hit: %016llx",
                 static_cast<unsigned long long>(p_hash));
  return true;
}

/**
 * @brief 将已链接程序落盘为缓存（实现侧；缓存回填路径）
 *
 * @par 落盘流程（全程静默失败，不影响编译成功事实）
 *   1. glGetProgramiv(GL_PROGRAM_BINARY_LENGTH) 探询二进制
 *      长度 —— 驱动不支持二进制检索时返回 0/负值，记 INFO 后
 *      放弃（部分核心格式/实现无 glGetProgramBinary 能力）
 *   2. glGetProgramBinary 导出程序二进制（驱动私有格式，
 *      binary_format 出参随数据一并取回）—— actual_length
 *      为 0 视为导出失败，记 WARN 返回
 *   3. _mkdir(kShaderCacheDir) 幂等建目录（已存在/失败均忽略，
 *      目录缺失最终表现为 fopen 失败并记 WARN）
 *   4. 写文件：四字段头（魔数 "ARHS" / 版本 1 /
 *      binary_format / data_size）+ 二进制体；载荷 fwrite 按实写
 *      字节数校验，写失败（磁盘满/权限）删除半截文件并记 WARN。
 *      全程成功才记 INFO（含哈希与字节数）
 *
 * @note 头部四字段 fwrite 未检查：仅 16 字节且失败时载荷写必然
 *       同步失败，由载荷校验统一兜底。
 * @note 程序二进制跨驱动/版本不保证兼容 —— 本函数不做指纹
 *       记录，兼容性由 LoadShaderCache 的 GL_LINK_STATUS
 *       校验兜底剔除。
 * @note 时机上仅在源码编译+链接全成功后调用一次（见
 *       ShaderCreateFromSource 尾部）；缓存键不含 uniform 配置
 *       （见 ComputeSourceHash），落盘内容也仅程序二进制，
 *       两侧口径一致。
 *
 * @param[in] p_hash 源码哈希（ComputeSourceHash 产物，
 *                   决定缓存文件名）
 * @param[in] p_info 已完成链接的着色器簿记（只读 program，
 *                   uniform_locations 不落盘）
 *
 * @see LoadShaderCache （读取对侧，含失效自愈）
 */
void GLRenderingDeviceDriver::SaveShaderCache(uint64 p_hash,
                                              const GLShaderInfo& p_info) {
  // 查询程序二进制大小
  GLint binary_length = 0;
  glGetProgramiv(p_info.program, GL_PROGRAM_BINARY_LENGTH, &binary_length);
  if (binary_length <= 0) {
    ARHUD_LOG_INFO(
        "Driver exposes no program binary, cache save skipped: %016llx",
        static_cast<unsigned long long>(p_hash));
    return;  // 不支持二进制检索
  }
  // 分配缓冲区并获取二进制
  void* binary_data = memory::Alloc(static_cast<uint64>(binary_length));
  if (binary_data == nullptr) {
    ARHUD_LOG_ERROR("kOutOfMemory", "SaveShaderCache: alloc %d bytes failed",
                    binary_length);
    return;
  }
  GLenum binary_format = 0;
  GLsizei actual_length = 0;
  glGetProgramBinary(p_info.program, binary_length, &actual_length,
                     &binary_format, binary_data);
  if (actual_length == 0) {
    ARHUD_LOG_WARN(
        "glGetProgramBinary returned empty payload, cache save skipped");
    memory::Free(binary_data);
    return;
  }
  // 确保缓存目录存在
  _mkdir(kShaderCacheDir);  // Windows: _mkdir, POSIX: mkdir. _mkdir
                            // 在已存在时返回 0
  // 写入缓存文件
  char path[256] = {};
  std::snprintf(path, sizeof(path), "%s/%016llx.bin", kShaderCacheDir,
                static_cast<unsigned long long>(p_hash));
  std::FILE* fp = std::fopen(path, "wb");
  if (fp == nullptr) {
    ARHUD_LOG_WARN("Shader cache write open failed: %s", path);
    memory::Free(binary_data);
    return;
  }
  // 文件头：魔数 + 版本 + 二进制格式 + 二进制大小
  constexpr uint32 kCacheMagic = 0x41524853;  // "ARHS"
  constexpr uint32 kCacheVersion = 1;
  std::fwrite(&kCacheMagic, sizeof(uint32), 1, fp);
  std::fwrite(&kCacheVersion, sizeof(uint32), 1, fp);
  std::fwrite(&binary_format, sizeof(GLenum), 1, fp);
  uint32 data_size = static_cast<uint32>(actual_length);
  std::fwrite(&data_size, sizeof(uint32), 1, fp);
  size_t written =
      std::fwrite(binary_data, 1, static_cast<size_t>(actual_length), fp);
  std::fclose(fp);
  memory::Free(binary_data);
  if (written != static_cast<size_t>(actual_length)) {
    // 写失败（磁盘满/权限等）：删半截文件，免下次加载走损坏分支
    std::remove(path);
    ARHUD_LOG_WARN("Shader cache write incomplete (%zu/%u bytes), removed: %s",
                   written, data_size, path);
    return;
  }
  ARHUD_LOG_INFO("Shader cache saved: %016llx (%u bytes)",
                 static_cast<unsigned long long>(p_hash), data_size);
}

}  // namespace arhud
