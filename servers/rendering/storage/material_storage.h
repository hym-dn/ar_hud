/**
 * @file material_storage.h
 * @brief Material resource storage with Material/MaterialData separation
 *
 * 参考 Godot 4.6 GLES3 MaterialStorage 的 Material/MaterialData 分层设计，
 * 适配 ARHud 的 RD 抽象层，预留 3D Spatial 扩展接口。
 *
 * @par 核心分层（参考 Godot Material + MaterialData）：
 *   Material（业务层）
 *     ├── RID, shader_rid, name           ← 资源标识
 *     ├── uniform_dirty, texture_dirty     ← dirty 标记
 *     └── MaterialData* data              ← 多态 GPU 数据指针
 *
 *   MaterialData（GPU 数据层，多态基类）
 *     ├── ubo_data + uniform_buffer_id     ← CPU/GPU UBO
 *     ├── texture_rids + uniform_set_id    ← 纹理 + 绑定集
 *     └── 虚函数：SyncUniformBuffer / RecreateUniformSet
 *
 *   CanvasMaterialData : MaterialData      ← 2D Canvas（当前实现）
 *   SpatialMaterialData : MaterialData     ← 3D Spatial（未来扩展）
 *
 * @par 为什么需要 Material / MaterialData 分离：
 *   Godot 拆分的本质原因是**渲染模式多态**：
 *   - 2D Canvas：简单 uniform（transform + modulate），无光照，单 UniformSet
 *   - 3D Spatial：复杂 uniform（PBR + 光照 UBO + 阴影），多 UniformSet
 *   - 粒子/天空：Compute dispatch 参数 / 特殊绑定
 *   不同模式的 SyncUniformBuffer 和 RecreateUniformSet 逻辑完全不同，
 *   必须通过虚函数分发，否则 MaterialStorage 会充满 if-else 分支。
 *
 * @par 与 Godot 的对比：
 *   | 特性 | Godot GLES3 | ARHud RD |
 *   |------|-------------|----------|
 *   | Material 业务数据 | Material struct | Material struct |
 *   | MaterialData 基类 | MaterialData (GL) | MaterialData (RD) |
 *   | 2D 子类 | CanvasMaterialData | CanvasMaterialData |
 *   | 3D 子类 | SceneMaterialData | SpatialMaterialData（预留） |
 *   | UBO | GLuint + Vector<uint8> | RDBufferID + LocalVector<uint8> |
 *   | 纹理绑定 | glBindTexture | RD UniformSet |
 *   | 工厂函数 | MaterialDataRequestFunction | MaterialDataType 枚举 + switch |
 *
 * @par 数据分层（三层分离）：
 *   - Material / MaterialData（本文件）：业务数据 + GPU 数据簿记
 *   - RD 层：RDBufferID（GPU UBO）+ RDUniformSetID（绑定集）
 *   - RDD 层：GL UBO + GL texture units（驱动特有）
 *
 * @par 架构层次：
 *   MaterialStorage（本文件，材质资源存储）
 *     ├── ShaderStorage          ← 着色器元数据 + Uniform 反射
 *     ├── TextureStorage         ← 纹理元数据
 *     └── RenderingDevice        ← GPU 资源生命周期管理
 *           ├── RDBufferID       ← GPU UBO
 *           └── RDUniformSetID   ← 绑定集（Buffer + Texture 绑定）
 *
 * @see Godot 参考：drivers/gles3/storage/material_storage.h
 * MaterialData/Material
 * @see arch_skill.md §2.8 Compositor/Storage/Renderer
 *
 * @author yameng.he
 * @version 2.0
 * @date 2026-05-25
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_STORAGE_MATERIAL_STORAGE_H_
#define ARHUD_SERVERS_RENDERING_STORAGE_MATERIAL_STORAGE_H_

#include "io/logger.h"
#include "os/memory.h"
#include "rendering_device.h"
#include "string/ustring.h"
#include "template/hash_map.h"
#include "template/local_vector.h"

namespace arhud {
using UString = String;

class ShaderStorage;
class TextureStorage;
class MaterialStorage;

// ═══════════════════════════════════════════════════════════════════════
// MaterialDataType — 材质数据类型枚举
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 材质数据类型（参考 Godot RS::ShaderMode）
 *
 * 决定 MaterialData 的子类类型，影响 UBO 布局和同步策略。
 * 与 Godot RS::ShaderMode 的对应关系：
 *   - kCanvas  ↔ SHADER_CANVAS_ITEM（2D Canvas 材质）
 *   - kSpatial ↔ SHADER_SPATIAL（3D Spatial 材质，未来扩展）
 *   - kMax     ↔ SHADER_MAX（无效值，用于边界检查）
 */
enum class MaterialDataType : uint8 {
  kCanvas,
  kSpatial,
  kMax,
};

// ═══════════════════════════════════════════════════════════════════════
// MaterialData — GPU 数据层（多态基类）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 材质 GPU 数据基类（参考 Godot MaterialData）
 *
 * Godot 的 MaterialData 是所有材质 GPU 数据的多态基类，
 * 持有 UBO 数据 + 纹理缓存，通过虚函数分发不同渲染模式的同步逻辑。
 *
 * @par 基类职责（参考 Godot MaterialData）：
 *   - 持有 CPU 端 UBO 副本（ubo_data）和 GPU 端 UBO（uniform_buffer_id）
 *   - 持有纹理 RID 缓存（texture_rids）
 *   - 持有 UniformSet 缓存（uniform_set_id）
 *   - 定义虚函数接口供子类实现
 *
 * @par 子类差异：
 *   | 特性 | CanvasMaterialData | SpatialMaterialData（未来） |
 *   |------|-------------------|--------------------------|
 *   | UBO 布局 | transform + modulate | PBR + 光照 + 阴影 |
 *   | UniformSet 数量 | 1 | 2-3（材质 + 光照 + 阴影） |
 *   | 同步频率 | per-draw | per-frame + per-draw |
 *   | 光照 | 无 | 需要同步 Scene UBO |
 */
struct MaterialData {
  virtual ~MaterialData() = default;

  /**
   * @brief 同步 CPU 端 Uniform 数据到 GPU UBO
   *
   * 对应 Godot MaterialData::update_uniform_buffer。
   * Canvas 实现直接上传整个 UBO；
   * Spatial 实现需要额外合并光照/阴影 UBO。
   *
   * @param[in] p_storage MaterialStorage 指针（访问 RD 等共享资源）
   * @retval Error::kOK     同步成功
   * @retval Error::kFailed 同步失败
   */
  virtual Error SyncUniformBuffer(MaterialStorage* p_storage) = 0;

  /**
   * @brief 重建 UniformSet
   *
   * 对应 Godot MaterialData::bind_uniforms。
   * Canvas 实现构造单个 UniformSet（UBO + 纹理）；
   * Spatial 实现可能构造多个 UniformSet（材质 + 光照 + 阴影）。
   *
   * @param[in] p_storage MaterialStorage 指针
   * @return 创建的 UniformSet ID，失败时返回空 ID
   */
  virtual RDUniformSetID RecreateUniformSet(MaterialStorage* p_storage) = 0;

  /**
   * @brief 释放 GPU 资源
   *
   * 释放 UBO、UniformSet 等 GPU 资源。
   * 由 MaterialStorage::MaterialFree 调用。
   *
   * @param[in] p_storage MaterialStorage 指针
   */
  virtual void FreeResources(MaterialStorage* p_storage) = 0;

  /**
   * @brief 获取材质数据类型
   *
   * @return 材质数据类型枚举值
   */
  virtual MaterialDataType GetDataType() const = 0;

  LocalVector<uint8>
      uniform_data;  ///< CPU 端 UBO 数据副本（参考 Godot ubo_data）
  RDBufferID
      uniform_buffer_id;  ///< GPU 端 UBO（参考 Godot GLuint uniform_buffer）
  uint32 uniform_buffer_size = 0;  ///< UBO 大小（字节）
  LocalVector<RID> texture_rids;  ///< 纹理 RID 缓存（参考 Godot texture_cache）
  uint32 texture_count = 0;       ///< 纹理槽位数（由 Shader 反射决定）
  RDUniformSetID uniform_set_id;  ///< 缓存的 UniformSet ID
  uint32 uniform_set_version =
      0;  ///< Uniform 数据版本号（每次 SyncUniformBuffer 递增）
  uint32 last_synced_version = 0;  ///< 上次重建 UniformSet 时的版本号
  uint32 version =
      0;  ///< 材质参数版本号（MaterialSetUniform/MaterialSetTexture
          ///< 递增，用于 Renderer 缓存失效）
  RID parent_material_rid;  ///< 父级 Material 的 RID（参考 Godot self）
};

// ═══════════════════════════════════════════════════════════════════════
// MaterialUniformSetCacheKey — Renderer 层 UniformSet 缓存键
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 材质 UniformSet 缓存键（RID + 版本号）
 *
 * 用于 CanvasRenderer/SpatialRenderer 的 UniformSet 缓存。
 * 当材质参数或纹理变更时 version 递增，缓存键不同导致缓存未命中，
 * 从而触发 UniformSet 重建。
 */
struct MaterialUniformSetCacheKey {
  RID material_rid;    ///< 材质 RID
  uint32 version = 0;  ///< 材质参数版本号（对应 MaterialData::version）

  /**
   * @brief 计算哈希值（用于 HashMap 键）
   * @return 32 位哈希值
   */
  uint32 GetHash() const {
    uint32 h = material_rid.GetHash();
    h ^= version + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }

  /**
   * @brief 相等比较
   */
  bool operator==(const MaterialUniformSetCacheKey& p_other) const {
    return material_rid == p_other.material_rid && version == p_other.version;
  }
};

// ═══════════════════════════════════════════════════════════════════════
// CanvasMaterialData — 2D Canvas 材质数据
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 2D Canvas 材质数据（参考 Godot CanvasMaterialData）
 *
 * Canvas 材质的特点：
 *   - UBO 布局简单：transform + modulate + 自定义 uniform
 *   - 单个 UniformSet（UBO + 纹理绑定在一起）
 *   - 无光照计算
 *   - 同步策略：per-draw，直接上传整个 UBO
 *
 * @par 与 Godot CanvasMaterialData 的对比：
 *   | 特性 | Godot GLES3 | ARHud RD |
 *   |------|-------------|----------|
 *   | bind_uniforms | glBindTexture + glBindBufferBase | RD::UniformSetCreate |
 *   | update_parameters | 调用基类 update_uniform_buffer + update_textures |
 * SyncUniformBuffer + RecreateUniformSet |
 */
struct CanvasMaterialData : public MaterialData {
  /**
   * @brief 同步 CPU 端 Uniform 数据到 GPU UBO
   *
   * 对应 Godot MaterialData::update_uniform_buffer。
   * Canvas 实现直接上传整个 UBO；
   * Spatial 实现需要额外合并光照/阴影 UBO。
   *
   * @param[in] p_storage MaterialStorage 指针（访问 RD 等共享资源）
   * @retval Error::kOK     同步成功
   * @retval Error::kFailed 同步失败
   */
  Error SyncUniformBuffer(MaterialStorage* p_storage) override;

  /**
   * @brief 重建 UniformSet
   *
   * 对应 Godot MaterialData::bind_uniforms。
   * Canvas 实现构造单个 UniformSet（UBO + 纹理）；
   * Spatial 实现可能构造多个 UniformSet（材质 + 光照 + 阴影）。
   *
   * @param[in] p_storage MaterialStorage 指针
   * @return 创建的 UniformSet ID，失败时返回空 ID
   */
  RDUniformSetID RecreateUniformSet(MaterialStorage* p_storage) override;

  /**
   * @brief 释放 GPU 资源
   *
   * 释放 UBO、UniformSet 等 GPU 资源。
   * 由 MaterialStorage::MaterialFree 调用。
   *
   * @param[in] p_storage MaterialStorage 指针
   */
  void FreeResources(MaterialStorage* p_storage) override;

  /**
   * @brief  获取材质数据类型
   * @return 材质数据类型枚举值
   */
  MaterialDataType GetDataType() const override {
    return MaterialDataType::kCanvas;
  }
};

// ═══════════════════════════════════════════════════════════════════════
// SpatialMaterialData — 3D Spatial 材质数据
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 3D Spatial 材质数据（参考 Godot SceneMaterialData）
 *
 * Spatial 材质的特点：
 *   - UBO 布局复杂：PBR 参数（albedo/metallic/roughness/emission）+ 自定义
 * uniform
 *   - 单个 UniformSet（材质 UBO + 纹理），光照/阴影由 Renderer 层管理
 *   - 同步策略：per-draw，直接上传整个 UBO（与 Canvas 相同）
 *   - 渲染排序：priority + next_pass 支持多 pass 渲染
 *
 * @par 为什么光照不在 MaterialData 层处理：
 *   Godot 的光照/阴影集成在 Renderer 层（SceneRasterizerGLES3），
 *   不在 MaterialStorage 层。Renderer 在绘制时：
 *     1. 绑定材质的 UniformSet（UBO + 纹理）← MaterialData 负责
 *     2. 绑定场景级 UniformSet（光照 UBO + 阴影参数）← Renderer 负责
 *   因此 MaterialData 只需处理材质自身的 UBO 和纹理绑定。
 *
 * @par 与 Godot SceneMaterialData 的对比：
 *   | 特性 | Godot GLES3 | ARHud RD |
 *   |------|-------------|----------|
 *   | update_parameters | update_parameters_internal(..., true) |
 * SyncUniformBuffer + RecreateUniformSet | | bind_uniforms | glBindBufferBase +
 * bind_uniforms_generic | RD::UniformSetCreate | | priority/next_pass | int8
 * priority + RID next_pass | uint8 priority + RID next_pass | | last_pass/index
 * | 渲染帧追踪 | uint64 last_pass + uint32 index |
 *
 * @par 未来扩展点（当 3D Renderer 实现时）：
 *   - 光照 UBO：Renderer 在绘制时额外绑定 Scene UniformSet，不需要修改
 * MaterialData
 *   - 阴影参数：同上，Renderer 管理
 *   - 多 Pass 渲染：通过 next_pass 链式遍历，priority 控制排序
 */
struct SpatialMaterialData : public MaterialData {
  /**
   * @brief 同步 CPU 端 Uniform 数据到 GPU UBO
   *
   * 对应 Godot MaterialData::update_uniform_buffer。
   * Canvas 实现直接上传整个 UBO；
   * Spatial 实现需要额外合并光照/阴影 UBO。
   *
   * @param[in] p_storage MaterialStorage 指针（访问 RD 等共享资源）
   * @retval Error::kOK     同步成功
   * @retval Error::kFailed 同步失败
   */
  Error SyncUniformBuffer(MaterialStorage* p_storage) override;

  /**
   * @brief 重建 UniformSet
   *
   * 对应 Godot MaterialData::bind_uniforms。
   * Canvas 实现构造单个 UniformSet（UBO + 纹理）；
   * Spatial 实现可能构造多个 UniformSet（材质 + 光照 + 阴影）。
   *
   * @param[in] p_storage MaterialStorage 指针
   * @return 创建的 UniformSet ID，失败时返回空 ID
   */
  RDUniformSetID RecreateUniformSet(MaterialStorage* p_storage) override;

  /**
   * @brief 释放 GPU 资源
   *
   * 释放 UBO、UniformSet 等 GPU 资源。
   * 由 MaterialStorage::MaterialFree 调用。
   *
   * @param[in] p_storage MaterialStorage 指针
   */
  void FreeResources(MaterialStorage* p_storage) override;

  /**
   * @brief  获取材质数据类型
   * @return 材质数据类型枚举值
   */
  MaterialDataType GetDataType() const override {
    return MaterialDataType::kSpatial;
  }

  uint8 priority = 0;  ///< 渲染优先级（参考 Godot SceneMaterialData::priority）
  RID next_pass;       ///< 下一个 pass 的材质 RID（参考 Godot
                       ///< SceneMaterialData::next_pass）
  uint64 last_pass =
      0;  ///< 上次渲染的 pass ID（参考 Godot SceneMaterialData::last_pass）
  uint32 index =
      0;  ///< 渲染列表中的索引（参考 Godot SceneMaterialData::index）
};

// ═══════════════════════════════════════════════════════════════════════
// Material — 业务层
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 材质业务数据（参考 Godot Material）
 *
 * Material 持有业务数据（RID、Shader 关联、dirty 标记），
 * 通过 MaterialData* 指针委托 GPU 数据操作给子类。
 *
 * @par 与 Godot Material 的对应关系：
 *   | Godot | ARHud | 说明 |
 *   |-------|-------|------|
 *   | RID self | RID rid | 资源 ID |
 *   | MaterialData* data | MaterialData* data | GPU 数据多态指针 |
 *   | Shader* shader | RID shader_rid | Shader 关联 |
 *   | bool uniform_dirty | bool uniform_dirty | UBO dirty 标记 |
 *   | bool texture_dirty | bool texture_dirty | 纹理 dirty 标记 |
 *   | RS::ShaderMode shader_mode | MaterialDataType data_type | 渲染模式 |
 */
struct Material {
  RID rid;         ///< 由 MaterialStorage 分配的资源 ID
  RID shader_rid;  ///< 关联的着色器 RID
  UString name;    ///< 材质名称（用于调试/日志）
  MaterialDataType data_type = MaterialDataType::kCanvas;  ///< 材质数据类型

  MaterialData* data =
      nullptr;  ///< GPU 数据多态指针（由 MaterialStorage 创建/销毁）

  bool uniform_dirty = true;  ///< Uniform 数据是否需要同步到 GPU
  bool texture_dirty = true;  ///< 纹理绑定是否需要更新
  bool is_valid = false;      ///< 材质是否有效
};

// ═══════════════════════════════════════════════════════════════════════
// MaterialStorage
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 材质资源存储（参考 Godot GLES3::MaterialStorage）
 *
 * 在 RenderingDevice 层之上管理材质的业务数据和 GPU 资源。
 * 通过 RD 的公共 API 创建/更新/释放 UBO 和 UniformSet。
 *
 * @par 核心职责：
 *   1. **资源创建** — 创建 Material + MaterialData 子类，分配 UBO
 *   2. **Uniform 更新** — 写入 CPU 端副本，标记 uniform_dirty
 *   3. **纹理绑定** — 更新纹理 RID 缓存，标记 texture_dirty
 *   4. **GPU 同步** — 委托 MaterialData 子类按需同步
 *   5. **资源释放** — 释放 Material + MaterialData，委托 RD 释放 GPU 资源
 *
 * @par 线程安全约束（参考 Godot RenderingServer 线程模型）：
 *   Godot 的 RenderingServer 采用"主线程提交 → 渲染线程执行"模型：
 *   - 主线程调用 RenderingServer API（如 material_set_param）
 *   - RenderingServer 将命令入队到渲染线程
 *   - 渲染线程执行实际的 GPU 操作
 *
 *   ARHud 当前采用**单渲染线程模型**（与 Godot GLES3 一致），
 *   所有 GPU 操作必须在渲染线程调用：
 *
 *   | 方法 | GPU 操作 | 线程约束 |
 *   |------|---------|---------|
 *   | Initialize/Finalize | RD::BufferCreate/Free | 🔴 渲染线程 |
 *   | MaterialCreate | RD::BufferCreate | 🔴 渲染线程 |
 *   | MaterialSetUniform | 仅写 CPU 副本 | 🟢 任意线程* |
 *   | MaterialSetTexture | 仅写 CPU 副本 | 🟢 任意线程* |
 *   | MaterialSetRenderPriority | 仅写 CPU 字段 | 🟢 任意线程 |
 *   | MaterialSetNextPass | 仅写 CPU 字段 | 🟢 任意线程 |
 *   | MaterialFree | RD::BufferFree/UniformSetFree | 🔴 渲染线程 |
 *   | MaterialGetUniformSet | RD::BufferUpdate/UniformSetCreate | 🔴 渲染线程 |
 *   | MaterialGetInfo | 仅读 CPU 数据 | 🟢 任意线程 |
 *   | MaterialIsValid | 仅读 CPU 数据 | 🟢 任意线程 |
 *
 *   *注：MaterialSetUniform/SetTexture 虽然只写 CPU 副本，
 *   但 RIDOwner 是线程安全的（模板参数 true = 互斥锁保护），
 *   所以理论上可在任意线程调用。但建议统一在渲染线程调用，
 *   避免与 MaterialGetUniformSet 的 GPU 同步产生竞态。
 *
 * @par 典型使用流程：
 *   @code
 *   MaterialStorage storage;
 *   storage.Initialize(rd, &shader_storage, &texture_storage);
 *
 *   RID mat = storage.MaterialCreate("hud_icon", shader_rid);
 *   storage.MaterialSetUniform(mat, 0, mvp, sizeof(mvp));
 *   storage.MaterialSetTexture(mat, 0, texture_rid);
 *
 *   // 绘制时获取 UniformSet（自动同步 dirty 数据）
 *   RDUniformSetID set = storage.MaterialGetUniformSet(mat);
 *
 *   storage.MaterialFree(mat);
 *   storage.Finalize();
 *   @endcode
 *
 * @par GPU 同步时机（参考 Godot update_parameters）：
 *   Godot 在 material_set_param 时标记 dirty，在下次渲染时调用
 * update_parameters。 ARHud 在 MaterialGetUniformSet 时检查 dirty 并委托
 * MaterialData 同步，语义等价。
 *
 * @see RenderingDevice
 * @see Material
 * @see MaterialData
 * @see CanvasMaterialData
 * @see ShaderStorage
 * @see TextureStorage
 */
/**
 * @brief MaterialSetUniformBlob 的参数包（标量部分；变长数据走 blob 通道）
 *
 * 对齐 §7.11 H1/E：标量入 param struct，字节经命令内联 blob 传递。
 */
struct MaterialSetUniformParams {
  RID material;       ///< 材质 RID
  uint32 offset = 0;  ///< Uniform 数据偏移（字节，std140 布局）
  uint32 size = 0;    ///< 数据字节数
};

/**
 * @brief MaterialInitialize（RID 分裂初始化）的参数包（标量部分；名称走 blob）
 */
struct MaterialCreateParams {
  RID shader;  ///< 关联着色器 RID（调用方已解析内置回退）
  MaterialDataType data_type = MaterialDataType::kCanvas;  ///< 材质数据类型
  uint32 name_size = 0;  ///< blob 第 1 段：名称字节数（含 NUL）
};

class MaterialStorage {
 public:
  /**
   * @brief 初始化材质存储
   *
   * @param[in] p_rd              RenderingDevice 指针（不拥有所有权）
   * @param[in] p_shader_storage  ShaderStorage 指针（不拥有所有权）
   * @param[in] p_texture_storage TextureStorage 指针（不拥有所有权）
   *
   * @retval Error::kOK           初始化成功
   * @retval Error::kAlreadyExists 已经初始化
   * @retval Error::kFailed       无效参数
   */
  Error Initialize(RenderingDevice* p_rd, ShaderStorage* p_shader_storage,
                   TextureStorage* p_texture_storage);

  /**
   * @brief 结束材质存储并释放所有资源
   *
   * 释放所有 Material 条目并委托 RenderingDevice 释放 GPU 资源。
   * 必须在 RenderingDevice 结束之前调用。
   */
  void Finalize();

  /**
   * @brief 创建材质（参考 Godot material_allocate + material_set_shader）
   *
   * 创建与指定着色器关联的材质。根据 MaterialDataType 创建对应的
   * MaterialData 子类，根据 Shader 的 Uniform 反射信息：
   *   1. 预分配 CPU 端 uniform_data 缓冲区
   *   2. 创建 GPU 端 UBO（RD::BufferCreate）
   *   3. 预分配纹理 RID 缓存
   *
   * @param[in] p_name     材质名称（用于调试/日志）
   * @param[in] p_shader   关联的着色器 RID
   * @param[in] p_type     材质数据类型（默认 kCanvas）
   *
   * @return 材质 RID，失败时返回空 RID
   */
  RID MaterialCreate(const char* p_name, RID p_shader,
                     MaterialDataType p_type = MaterialDataType::kCanvas);

  /**
   * @brief RID 分裂分配（对齐 §7.11 F / Godot FUNCRIDSPLIT）
   *
   * **任意线程**：仅分配线程安全的空壳 Material 条目（is_valid=false），无 GPU
   * 操作。
   *
   * @return 空壳材质 RID
   */
  RID MaterialAllocate();

  /**
   * @brief RID 分裂初始化：渲染线程为已分配的空壳 RID 建 UBO/反射并填充
   *
   * 失败时释放该 RID。参数经 param struct + 内联 blob（名称）传递。
   *
   * @param[in] p_material MaterialAllocate() 返回的空壳 RID
   * @param[in] p_params   shader / data_type / name_size
   * @param[in] p_blob     名称 blob 基址（含 NUL）
   *
   * @pre 必须在渲染线程上调用
   */
  void MaterialInitialize(RID p_material, const MaterialCreateParams& p_params,
                          const uint8* p_blob);

  /**
   * @brief 设置 Uniform 数据（参考 Godot material_set_param）
   *
   * 将数据写入材质的 CPU 端 Uniform 缓冲区指定偏移位置。
   * 调用后标记 uniform_dirty，下次 MaterialGetUniformSet 时同步到 GPU。
   *
   * @param[in] p_material 材质 RID
   * @param[in] p_offset   Uniform 数据偏移量（字节，std140 布局）
   * @param[in] p_data     数据指针
   * @param[in] p_size     数据大小（字节）
   *
   * @retval Error::kOK     设置成功
   * @retval Error::kFailed 无效 RID 或越界
   */
  Error MaterialSetUniform(RID p_material, uint32 p_offset, const void* p_data,
                           uint32 p_size);

  /**
   * @brief 命令队列变体：Uniform 字节经内联 blob 跨线程传递（blob-last
   * 约定，§7.11 E）
   *
   * 与 MaterialSetUniform 语义等价，供 `CommandQueue::PushBoundBlob` 绑定调用：
   * 标量（material/offset/size）随 param struct 内联，字节走 blob 通道。
   *
   * @param[in] p_params 材质 RID / 偏移 / 字节数
   * @param[in] p_blob   数据 blob 基址（blob 载荷，执行期有效）
   */
  Error MaterialSetUniformBlob(const MaterialSetUniformParams& p_params,
                               const uint8* p_blob);

  /**
   * @brief 设置纹理绑定（参考 Godot material_set_param 对纹理类型）
   *
   * 将纹理绑定到材质的指定纹理槽位。
   * 调用后标记 texture_dirty，下次 MaterialGetUniformSet 时重建 UniformSet。
   *
   * @param[in] p_material 材质 RID
   * @param[in] p_slot     纹理槽位索引
   * @param[in] p_texture  纹理 RID（TextureStorage 的 RID）
   *
   * @retval Error::kOK     设置成功
   * @retval Error::kFailed 无效 RID
   */
  Error MaterialSetTexture(RID p_material, uint32 p_slot, RID p_texture);

  /**
   * @brief 设置渲染优先级（参考 Godot material_set_render_priority）
   *
   * 仅对 SpatialMaterialData 有效（3D 渲染排序）。
   * CanvasMaterialData 忽略此设置（2D 绘制顺序由 CanvasItem z-index 控制）。
   *
   * 优先级范围 [-128, 127]，值越小越先绘制。
   * 用于不透明物体 front-to-back 排序（early-Z 优化）和
   * 透明物体 back-to-front 排序。
   *
   * @param[in] p_material 材质 RID
   * @param[in] p_priority 渲染优先级（-128 ~ 127）
   *
   * @retval Error::kOK     设置成功
   * @retval Error::kFailed 无效 RID 或超出范围
   */
  Error MaterialSetRenderPriority(RID p_material, int32 p_priority);

  /**
   * @brief 设置下一个 pass 的材质（参考 Godot material_set_next_pass）
   *
   * 仅对 SpatialMaterialData 有效（3D 多 pass 渲染）。
   * CanvasMaterialData 忽略此设置。
   *
   * 多 pass 渲染用于描边、辉光等效果：
   *   Renderer 绘制完当前材质后，检查 next_pass，
   *   如果非空则用同一几何体再次绘制 next_pass 材质。
   *
   * @param[in] p_material       材质 RID
   * @param[in] p_next_material  下一个 pass 的材质 RID（空 RID 表示无 next
   * pass）
   *
   * @retval Error::kOK     设置成功
   * @retval Error::kFailed 无效 RID
   */
  Error MaterialSetNextPass(RID p_material, RID p_next_material);

  /**
   * @brief 释放材质资源
   *
   * 释放 Material + MaterialData 条目并委托 RenderingDevice 释放 GPU 资源
   * （UBO + UniformSet，延迟销毁）。
   *
   * @param[in] p_material 材质 RID
   */
  void MaterialFree(RID p_material);

  /**
   * @brief 获取材质信息
   *
   * @param[in] p_material 材质 RID
   *
   * @return Material 指针，RID 无效时返回 nullptr
   */
  const Material* MaterialGetInfo(RID p_material) const;

  /**
   * @brief 获取或创建 UniformSet（参考 Godot update_parameters +
   * bind_uniforms）
   *
   * 检查 dirty 标记并委托 MaterialData 子类执行 GPU 同步：
   *   1. uniform_dirty → MaterialData::SyncUniformBuffer
   *   2. texture_dirty 或 uniform_dirty → MaterialData::RecreateUniformSet
   *
   * @param[in] p_material 材质 RID
   *
   * @return UniformSet ID，失败时返回空 ID
   */
  RDUniformSetID MaterialGetUniformSet(RID p_material);

  /**
   * @brief 检查材质是否有效
   *
   * @param[in] p_material 材质 RID
   *
   * @return 材质有效返回 true，否则返回 false
   */
  bool MaterialIsValid(RID p_material) const;

  /**
   * @brief 获取 RenderingDevice 指针（供 MaterialData 子类使用）
   * @return RenderingDevice 指针
   */
  RenderingDevice* GetRD() const {
    return rd_;
  }

  /**
   * @brief 获取 ShaderStorage 指针（供 MaterialData 子类使用）
   * @return ShaderStorage 指针
   */
  ShaderStorage* GetShaderStorage() const {
    return shader_storage_;
  }

  /**
   * @brief 获取 TextureStorage 指针（供 MaterialData 子类使用）
   * @return TextureStorage 指针
   */
  TextureStorage* GetTextureStorage() const {
    return texture_storage_;
  }

 private:
  /**
   * @brief 创建 MaterialData 子类实例（参考 Godot MaterialDataRequestFunction）
   *
   * 根据 MaterialDataType 创建对应的 MaterialData 子类。
   * 对应 Godot 的 MaterialDataRequestFunction 工厂函数模式。
   *
   * @param[in] p_type 材质数据类型
   *
   * @return MaterialData 指针，失败时返回 nullptr
   */
  MaterialData* CreateMaterialData(MaterialDataType p_type);

  /**
   * @brief RID 分裂内部：为已分配的空壳 RID 建 MaterialData（UBO/反射）并填充
   * @note 渲染线程；MaterialCreate（同步）与 MaterialInitialize（分裂）共用
   */
  Error MaterialInitializeRid(RID p_material, const char* p_name, RID p_shader,
                              MaterialDataType p_type);

 private:
  RenderingDevice* rd_ = nullptr;  ///< RenderingDevice（不拥有所有权）
  ShaderStorage* shader_storage_ = nullptr;  ///< ShaderStorage（不拥有所有权）
  TextureStorage* texture_storage_ =
      nullptr;                               ///< TextureStorage（不拥有所有权）
  RIDOwner<Material, true> material_owner_;  ///< 线程安全的 RID → Material 映射
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_STORAGE_MATERIAL_STORAGE_H_
