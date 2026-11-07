/**
 * @file shader_storage.h
 * @brief Shader resource storage with metadata and source caching
 *
 * ShaderStorage sits above RenderingDevice (RD) and manages shader resources
 * with metadata like uniform reflection and source caching.
 *
 * @par 设计原则：
 *   1. 在 RD 之上提供 Shader 资源的元数据管理
 *   2. 缓存 GLSL 源码，支持运行时重编译
 *   3. 通过 RIDOwner 管理 RID → ShaderInfo 映射
 *   4. 不持有 GPU 资源所有权，GPU 端资源由 RD 管理
 *
 * @par 与 Godot ShaderStorage 的对比：
 *   | 特性 | Godot ShaderStorage | ARHud ShaderStorage |
 *   |------|---------------------|---------------------|
 *   | RID 管理 | ✅ | ✅ |
 *   | 源码缓存 | ✅ | ✅ |
 *   | Uniform 反射 | ✅ | ✅ |
 *   | Push Constant | ✅ | ✅ |
 *   | 多阶段支持 | ✅ 全阶段 | ✅ Vertex + Fragment |
 *   | 线程安全 | ✅ | ✅ RIDOwner<true> |
 *
 * @par 架构层次：
 *   ShaderStorage（本文件，着色器资源存储）
 *     └── RenderingDevice          ← GPU 资源生命周期管理
 *           └── RenderingDeviceDriver   ← 驱动层接口
 *                 └── RenderingContextDriver ← 上下文层接口
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-23
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_STORAGE_SHADER_STORAGE_H_
#define ARHUD_SERVERS_RENDERING_STORAGE_SHADER_STORAGE_H_

#include "io/logger.h"
#include "os/memory.h"
#include "rendering_device.h"
#include "string/ustring.h"
#include "template/hash_map.h"
#include "template/local_vector.h"

namespace arhud {

using UString = String;

// ═══════════════════════════════════════════════════════════════════════
// Shader 资源簿记
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 着色器阶段源码缓存（拥有所有权）
 *
 * 与 ShaderStageSource 不同，本结构持有源码的副本（UString），
 * 用于 ShaderStorage 的源码缓存，支持运行时重编译。
 */
struct ShaderStageSourceCache {
  ShaderStage stage = ShaderStage::kMax;  ///< 着色器阶段（vertex / fragment）
  UString source;                         ///< 源码副本（深拷贝，拥有所有权）
};

/**
 * @brief 着色器资源簿记，仅存业务数据
 *
 * GPU 编译元数据（uniforms、push_constant_size、is_valid）
 * 由 RD 层的 Shader 结构持有，ShaderStorage 通过
 * RD 查询方法访问。本结构仅存储业务层数据：
 * 源码缓存（支持运行时重编译）和调试名称。
 *
 * @par 数据分层：
 *   - ShaderStorage（本结构）：业务数据（name、source 缓存）
 *   - RD Shader：API 无关的编译元数据（uniforms、push_constant_size）
 *   - RDD GLShaderInfo：API 特有数据（GL uniform locations、program）
 */
struct ShaderInfo {
  RID rid;           ///< 由 ShaderStorage 分配的资源 ID（MakeRid 后回填）
  RDShaderID rd_id;  ///< RD 层的着色器 ID（GPU 端 program 句柄的簿记索引）
  UString name;      ///< 着色器名称（用于调试/日志，如 "hud_text"）
  LocalVector<ShaderStageSourceCache>
      sources;  ///< 缓存的着色器阶段源码（支持运行时重编译）
  /**
   * @brief 每个变体的 Shader RID（索引 = ShaderVariant）
   *
   * kColorPass 槽位指向自身 RID（回填）；
   * kDepthPass 槽位指向独立编译的变体条目（编译失败时为空 RID，
   * 查询时退回基础 RID）。
   */
  LocalVector<RID> variant_shaders;
};

// ═══════════════════════════════════════════════════════════════════════
// Shader 编译缓存键
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 着色器源码缓存键
 *
 * 将所有着色器阶段的源码哈希为一个 uint64 键，
 * 用于查找相同源码的已编译着色器，避免重复编译。
 *
 * @note 哈希输入包含：各阶段枚举 + 源码内容（djb2）+ uniform
 *       反射四元组（type/binding/writable/length）+ push constant
 *       大小——任一变化即视为不同着色器
 */
struct ShaderSourceKey {
  uint64 hash = 0;  ///< 组合哈希值

  bool operator==(const ShaderSourceKey& p_other) const {
    return hash == p_other.hash;
  }
  bool operator!=(const ShaderSourceKey& p_other) const {
    return hash != p_other.hash;
  }
};

/**
 * @brief ShaderSourceKey 的哈希器（HashMap 配套）
 *
 * 取 hash 低 32 位作为桶索引。
 */
struct ShaderSourceKeyHasher {
  static uint32 Hash(const ShaderSourceKey& p_key) {
    return static_cast<uint32>(p_key.hash);
  }
};

// ═══════════════════════════════════════════════════════════════════════
// Shader 变体
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief Shader 变体类型（参考 Godot ShaderGLES3）
 *
 * 通过 #ifdef 宏注入切换 shader 行为：
 * 变体编译时在 #version 行后注入 `#define VARIANT_DEPTH_PASS 1`，
 * GLSL 源码内以 `#ifdef VARIANT_DEPTH_PASS` 分支裁剪代码路径。
 *
 * @note 变体参与度探测：仅源码声明了 VARIANT_DEPTH_PASS 分支的
 *       shader 才编译 kDepthPass 变体（如 hud_spatial）；未声明
 *       分支的 shader（hud_canvas/hud_text/hud_composite）跳过
 *       变体编译，kDepthPass 槽位置空，查询时退回基础 RID
 * @note Phase 4 仅支持 2 个变体；新增变体需同步扩展
 *       ShaderCreateFromSource 的注入逻辑与 kMax 计数
 */
enum class ShaderVariant : uint8 {
  kColorPass = 0,  ///< 颜色渲染 pass（默认，无宏注入，即基础着色器）
  kDepthPass =
      1,  ///< 深度 pre-pass（仅写深度，不写颜色，注入 VARIANT_DEPTH_PASS）
  kMax    ///< 变体计数（越界防护）
};

// ═══════════════════════════════════════════════════════════════════════
// ShaderStorage
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 着色器资源存储，仅管理业务数据和源码缓存
 *
 * 在 RenderingDevice 层之上管理着色器的业务数据：
 * 源码缓存（支持运行时重编译）和调试名称。
 * GPU 编译元数据（uniforms、push_constant_size、is_valid）
 * 由 RD 层的 Shader 结构持有，本类通过 RD 查询方法访问。
 *
 * @par 核心职责：
 *   1. **资源创建** — 从 GLSL 源码创建 Shader，委托 RD 编译 GPU 端程序
 *   2. **源码缓存** — 缓存 GLSL 源码，支持运行时重编译
 *   3. **资源查询** — 通过 RID 提供 O(1) 的 ShaderInfo 查找
 *   4. **元数据代理** — 通过 RD 查询方法代理 GPU 元数据访问
 *   5. **资源释放** — 释放 ShaderInfo 并委托 RD 释放 GPU 端资源
 *
 * @par 典型使用流程：
 *   @code
 *   ShaderStorage storage;
 *   storage.Initialize(rd);
 *
 *   RID shader = storage.ShaderCreateFromSource(
 *       "hud_text", vert_src, frag_src, uniforms, 64);
 *
 *   // 查询业务数据（本地）
 *   const ShaderInfo* info = storage.ShaderGetInfo(shader);
 *
 *   // 查询 GPU 元数据（代理到 RD）
 *   uint32 push_size = storage.ShaderGetPushConstantSize(shader);
 *
 *   // 释放
 *   storage.ShaderFree(shader);
 *   storage.Finalize();
 *   @endcode
 *
 * @par 线程安全：
 *   - RIDOwner<ShaderInfo, true> 提供线程安全的 RID → 对象映射
 *     （防御性加锁，为未来跨线程查询预留）
 *   - Initialize/Finalize 应在渲染线程调用
 *   - 创建/释放（ShaderCreateFromSource/ShaderFree）必须在渲染线程
 *     调用（GL 上下文亲和，内部 ARHUD_ASSERT 校验）
 *   - 查询方法（ShaderGet*）为 const 只读，可在任意线程调用；
 *     返回的引用/指针生命周期由渲染线程的创建/释放节奏保证，
 *     不应跨帧长期持有
 *
 * @see RenderingDevice
 * @see ShaderInfo
 */
class ShaderStorage {
 public:
  /**
   * @brief 初始化着色器存储
   *
   * 将存储绑定到 RenderingDevice 实例，用于 GPU 着色器
   * 编译和资源管理。
   *
   * @param[in] p_rd RenderingDevice 指针（不拥有所有权，调用者管理生命周期）
   *
   * @retval Error::kOK 初始化成功
   * @retval Error::kAlreadyExists 已经初始化
   * @retval Error::kFailed 无效的 RenderingDevice
   *
   * @pre p_rd 已初始化（IsInitialized() == true）
   */
  Error Initialize(RenderingDevice* p_rd);

  /**
   * @brief 结束着色器存储并释放所有资源
   *
   * 遍历释放所有 ShaderInfo 条目（含变体条目）并委托
   * RenderingDevice 释放 GPU 资源，清空编译缓存。
   * 必须在 RenderingDevice 结束之前调用。
   *
   * @pre 所有外部着色器引用已不再使用
   * @post shader_owner_ / shader_cache_ 清空，rd_ 置空，
   *       可重新 Initialize
   */
  void Finalize();

  /**
   * @brief 从 GLSL 源码创建着色器
   *
   * 通过 RenderingDevice 编译着色器阶段，
   * 在 ShaderInfo 条目中缓存源码。
   *
   * @par 流程
   *   1. 计算源码缓存键（源码 + uniform 反射 + push constant 哈希）
   *   2. 命中缓存且 GPU 端仍有效 → 直接返回已有 RID（零编译）
   *   3. 未命中 → 编译基础变体（kColorPass）；
   *      变体参与度探测通过（源码含 VARIANT_DEPTH_PASS 子串）时
   *      额外注入宏编译深度变体（kDepthPass），为变体建立独立
   *      ShaderInfo 条目；未声明分支的 shader 跳过变体编译
   *   4. 登记缓存并返回基础 RID
   *
   * @param[in] p_name                着色器名称（用于调试/日志，可为 nullptr）
   * @param[in] p_stage_sources       着色器阶段源码数组
   * @param[in] p_uniforms            Uniform 反射数据
   * @param[in] p_push_constant_size  Push Constant 块大小（字节，默认 0）
   *
   * @return 基础着色器（kColorPass）的 RID，失败时返回空 RID
   *
   * @pre Initialize() 已成功调用
   * @pre 必须在渲染线程调用（GL 编译归渲染线程）
   *
   * @note 深度变体编译失败不阻塞创建——仅 WARN，变体查询时退回基础 RID
   * @note 源码未声明 VARIANT_DEPTH_PASS 分支时不编译深度变体（探测跳过，
   *       INFO 日志可见），kDepthPass 槽位置空 RID
   * @note 相同源码重复创建走缓存，返回同一 RID（引用不计数，先 Free 后
   *       Create 会得到新 RID 且旧缓存条目已失效清除）
   */
  RID ShaderCreateFromSource(
      const char* p_name, const LocalVector<ShaderStageSource>& p_stage_sources,
      const LocalVector<ShaderUniform>& p_uniforms,
      uint32 p_push_constant_size = 0);

  /**
   * @brief 释放着色器资源
   *
   * 释放 ShaderInfo 条目并委托 RenderingDevice 释放 GPU 资源
   * （延迟销毁）。变体（kDepthPass 独立条目）一并级联释放。
   *
   * @param[in] p_shader 着色器 RID
   *
   * @pre 必须在渲染线程调用
   *
   * @note 传入无效/空 RID 时安全返回（no-op）
   * @note 基础与变体 ShaderInfo 均即时释放（簿记层），
   *       GPU program 走 RD 延迟销毁（fc 帧后回收）
   */
  void ShaderFree(RID p_shader);

  /**
   * @brief 获取完整的着色器信息结构
   *
   * @param[in] p_shader 着色器 RID
   *
   * @return ShaderInfo 指针，RID 无效时返回 nullptr
   */
  const ShaderInfo* ShaderGetInfo(RID p_shader) const;

  /**
   * @brief 获取 RD 层的着色器 ID
   *
   * @param[in] p_shader 着色器 RID
   *
   * @return RDShaderID，RID 无效时返回空 ID
   */
  RDShaderID ShaderGetRDId(RID p_shader) const;

  /**
   * @brief 获取 uniform 反射数据（代理到 RD 层）
   *
   * @param[in] p_shader 着色器 RID
   *
   * @return uniform 列表的常量引用，RID 无效时返回空列表
   */
  const LocalVector<ShaderUniform>& ShaderGetUniforms(RID p_shader) const;

  /**
   * @brief 获取 push constant 块大小（代理到 RD 层）
   *
   * @param[in] p_shader 着色器 RID
   *
   * @return push constant 大小（字节），RID 无效时返回 0
   */
  uint32 ShaderGetPushConstantSize(RID p_shader) const;

  /**
   * @brief 检查着色器是否有效（代理到 RD 层）
   *
   * @param[in] p_shader 着色器 RID
   *
   * @return 着色器编译成功返回 true，否则返回 false
   */
  bool ShaderIsValid(RID p_shader) const;

  /**
   * @brief 获取指定变体的 Shader RID
   *
   * @param[in] p_shader 基础着色器 RID
   * @param[in] p_variant 变体类型
   *
   * @return 变体 Shader RID；变体未编译（槽位空）或 RID 无效时
   *         返回基础 Shader RID（kColorPass 退化为自身）
   *
   * @note kColorPass 恒返回 p_shader 自身（槽位回填设计）
   */
  RID ShaderGetVariant(RID p_shader, ShaderVariant p_variant) const;

 private:
  RenderingDevice* rd_ = nullptr;  ///< RenderingDevice（不拥有所有权）
  RIDOwner<ShaderInfo, true>
      shader_owner_;  ///< 线程安全的 RID → ShaderInfo 映射
  HashMap<ShaderSourceKey, RID, ShaderSourceKeyHasher>
      shader_cache_;  ///< 源码哈希 → Shader RID 缓存

  /**
   * @brief 计算着色器源码缓存键
   *
   * 将所有着色器阶段的源码内容哈希为一个 uint64 键。
   *
   * @param[in] p_stage_sources 着色器阶段源码数组
   * @param[in] p_uniforms      Uniform 描述数组
   * @param[in] p_push_constant_size Push Constant 大小
   *
   * @return 缓存键
   */
  ShaderSourceKey ComputeShaderSourceKey(
      const LocalVector<ShaderStageSource>& p_stage_sources,
      const LocalVector<ShaderUniform>& p_uniforms,
      uint32 p_push_constant_size) const;
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_STORAGE_SHADER_STORAGE_H_
