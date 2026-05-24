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

#pragma once

#include "rendering_device.h"
#include "template/local_vector.h"
#include "template/hash_map.h"
#include "string/ustring.h"
#include "os/memory.h"
#include "io/logger.h"

namespace arhud
{

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
struct ShaderStageSourceCache
{
    ShaderStage stage = ShaderStage::kMax;
    UString source;
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
struct ShaderInfo
{
    RID rid;                                      ///< 由 ShaderStorage 分配的资源 ID
    RDShaderID rd_id;                             ///< RD 层的着色器 ID
    UString name;                                 ///< 着色器名称（用于调试/日志）
    LocalVector<ShaderStageSourceCache> sources;   ///< 缓存的着色器阶段源码
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
 *   uint32_t push_size = storage.ShaderGetPushConstantSize(shader);
 *
 *   // 释放
 *   storage.ShaderFree(shader);
 *   storage.Finalize();
 *   @endcode
 *
 * @par 线程安全：
 *   - RIDOwner<ShaderInfo, true> 提供线程安全的 RID → 对象映射
 *   - Initialize/Finalize 应在渲染线程调用
 *   - 资源创建/查询/释放可在任意线程（RIDOwner 内部加锁）
 *
 * @see RenderingDevice
 * @see ShaderInfo
 */
class ShaderStorage
{
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
    Error Initialize(RenderingDevice *p_rd);

    /**
     * @brief 结束着色器存储并释放所有资源
     *
     * 释放所有 ShaderInfo 条目并委托 RenderingDevice 释放 GPU 资源。
     * 必须在 RenderingDevice 结束之前调用。
     *
     * @pre 所有外部着色器引用已不再使用
     */
    void Finalize();

    /**
     * @brief 从 GLSL 源码创建着色器
     *
     * 通过 RenderingDevice 编译着色器阶段，
     * 在 ShaderInfo 条目中缓存源码。
     *
     * @param[in] p_name                着色器名称（用于调试/日志）
     * @param[in] p_stage_sources       着色器阶段源码数组
     * @param[in] p_uniforms            Uniform 反射数据
     * @param[in] p_push_constant_size  Push Constant 块大小（字节，默认 0）
     *
     * @return 着色器 RID，失败时返回空 RID
     *
     * @pre Initialize() 已成功调用
     */
    RID ShaderCreateFromSource(const char *p_name,
                               const LocalVector<ShaderStageSource> &p_stage_sources,
                               const LocalVector<ShaderUniform> &p_uniforms,
                               uint32_t p_push_constant_size = 0);

    /**
     * @brief 释放着色器资源
     *
     * 释放 ShaderInfo 条目并委托 RenderingDevice 释放 GPU 资源
     * （延迟销毁）。
     *
     * @param[in] p_shader 着色器 RID
     */
    void ShaderFree(RID p_shader);

    /**
     * @brief 获取完整的着色器信息结构
     *
     * @param[in] p_shader 着色器 RID
     *
     * @return ShaderInfo 指针，RID 无效时返回 nullptr
     */
    const ShaderInfo *ShaderGetInfo(RID p_shader) const;

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
    const LocalVector<ShaderUniform> &ShaderGetUniforms(RID p_shader) const;

    /**
     * @brief 获取 push constant 块大小（代理到 RD 层）
     *
     * @param[in] p_shader 着色器 RID
     *
     * @return push constant 大小（字节），RID 无效时返回 0
     */
    uint32_t ShaderGetPushConstantSize(RID p_shader) const;

    /**
     * @brief 检查着色器是否有效（代理到 RD 层）
     *
     * @param[in] p_shader 着色器 RID
     *
     * @return 着色器编译成功返回 true，否则返回 false
     */
    bool ShaderIsValid(RID p_shader) const;

private:
    RenderingDevice *rd_ = nullptr;              ///< RenderingDevice（不拥有所有权）
    RIDOwner<ShaderInfo, true> shader_owner_;    ///< 线程安全的 RID → ShaderInfo 映射
};

}