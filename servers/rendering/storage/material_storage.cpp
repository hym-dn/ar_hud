/**
 * @file material_storage.cpp
 * @brief MaterialStorage 实现
 *
 * 参考 Godot 4.6 GLES3 MaterialStorage 的核心逻辑：
 *   - MaterialData::update_uniform_buffer →
 * CanvasMaterialData::SyncUniformBuffer
 *   - MaterialData::bind_uniforms → CanvasMaterialData::RecreateUniformSet
 *   - MaterialData::update_parameters → MaterialStorage::MaterialGetUniformSet
 *   - MaterialDataRequestFunction → MaterialStorage::CreateMaterialData
 *
 * ARHud 适配：
 *   - MaterialData 子类通过 MaterialStorage* 访问
 * RD/ShaderStorage/TextureStorage
 *   - 通过 RD API 间接操作 GPU，不直接调用 GL
 *   - SpatialMaterialData 预留接口，当前仅实现 CanvasMaterialData
 */

#include "material_storage.h"

#include "shader_storage.h"
#include "texture_storage.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// CanvasMaterialData 实现
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 同步 CPU 端 Uniform 数据到 GPU UBO（2D Canvas 实现）
 *
 * 将 uniform_data 中的数据上传到 GPU 端的 uniform_buffer_id。
 * 上传成功后递增 uniform_set_version，触发 UniformSet 重建。
 *
 * @param[in] p_storage MaterialStorage 指针，用于访问 RenderingDevice
 *
 * @return Error::kOK     同步成功或无需同步（UBO 为空）
 * @return Error::kFailed BufferUpdate 失败
 *
 * @pre 必须在渲染线程调用
 * @pre uniform_buffer_id 已通过 MaterialCreate 创建
 *
 * @post uniform_set_version 递增
 *
 * @note 对应 Godot MaterialData::update_uniform_buffer
 */
Error CanvasMaterialData::SyncUniformBuffer(MaterialStorage* p_storage) {
  ARHUD_ASSERT_ON_RENDER_THREAD(p_storage->GetRD());

  if (uniform_buffer_size == 0 || !uniform_buffer_id.IsValid()) {
    return Error::kOK;
  }

  RenderingDevice* rd = p_storage->GetRD();
  Error err = rd->BufferUpdate(uniform_buffer_id, 0, uniform_buffer_size,
                               uniform_data.Data());

  if (err != Error::kOK) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasMaterialData::SyncUniformBuffer() BufferUpdate failed");
    return Error::kFailed;
  }

  uniform_set_version++;

  return Error::kOK;
}

/**
 * @brief 重建 UniformSet（2D Canvas 实现）
 *
 * 根据 Shader 的 Uniform 反射信息，构建 BoundUniform 数组，
 * 将 UBO 和纹理绑定到 UniformSet。
 *
 * @param[in] p_storage MaterialStorage 指针，用于访问
 * RD/ShaderStorage/TextureStorage
 *
 * @return 新创建的 UniformSet ID，失败时返回空 ID
 *
 * @pre 必须在渲染线程调用
 * @pre parent_material_rid 对应的 Material 存在且有效
 *
 * @post uniform_set_id 更新为新创建的 UniformSet
 *
 * @note 对应 Godot MaterialData::bind_uniforms
 * @note Canvas 材质只有单个 UniformSet（UBO + 纹理绑定在一起）
 */
RDUniformSetID CanvasMaterialData::RecreateUniformSet(
    MaterialStorage* p_storage) {
  ARHUD_ASSERT_ON_RENDER_THREAD(p_storage->GetRD());

  RenderingDevice* rd = p_storage->GetRD();
  ShaderStorage* shader_storage = p_storage->GetShaderStorage();
  TextureStorage* texture_storage = p_storage->GetTextureStorage();

  if (uniform_set_id.IsValid()) {
    rd->UniformSetFree(uniform_set_id);
    uniform_set_id = RDUniformSetID();
  }

  const Material* mat = p_storage->MaterialGetInfo(parent_material_rid);

  if (mat == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasMaterialData::RecreateUniformSet() parent Material not found");
    return RDUniformSetID();
  }

  const LocalVector<ShaderUniform>& uniforms =
      shader_storage->ShaderGetUniforms(mat->shader_rid);
  RDShaderID shader_rd_id = shader_storage->ShaderGetRDId(mat->shader_rid);

  LocalVector<BoundUniform> bound_uniforms;
  uint32 tex_index = 0;

  for (uint32 i = 0; i < uniforms.Size(); ++i) {
    BoundUniform bound;
    bound.binding = uniforms[i].binding;
    bound.immutable_sampler = false;

    switch (uniforms[i].type) {
      case UniformType::kSamplerWithTexture:
      case UniformType::kSamplerWithTextureBuffer:
      case UniformType::kTexture:
      case UniformType::kTextureBuffer:
      case UniformType::kImage:
      case UniformType::kImageBuffer: {
        bound.type = uniforms[i].type;

        // Texture ID only (no custom sampler, use texture's own parameters)
        if (tex_index < texture_count && texture_rids[tex_index].IsValid()) {
          RDTextureID tex_rd_id =
              texture_storage->TextureGetRDId(texture_rids[tex_index]);
          if (tex_rd_id.IsValid()) {
            TextureID tex_driver_id = rd->TextureGetDriverId(tex_rd_id);
            bound.ids.PushBack(BufferID(tex_driver_id.GetId()));
          }
        }
        tex_index++;
        break;
      }
      case UniformType::kUniformBuffer:
      case UniformType::kUniformBufferDynamic: {
        bound.type = uniforms[i].type;
        if (uniform_buffer_id.IsValid()) {
          BufferID buf_driver_id = rd->BufferGetDriverId(uniform_buffer_id);
          bound.ids.PushBack(buf_driver_id);
        }
        break;
      }
      case UniformType::kStorageBuffer:
      case UniformType::kStorageBufferDynamic: {
        bound.type = uniforms[i].type;
        break;
      }
      default:
        continue;
    }

    bound_uniforms.PushBack(bound);
  }

  RDUniformSetID new_set = rd->UniformSetCreate(
      VectorView<BoundUniform>(bound_uniforms.Data(), bound_uniforms.Size()),
      shader_rd_id, 0);

  if (!new_set.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasMaterialData::RecreateUniformSet() UniformSetCreate failed");
    return RDUniformSetID();
  }

  uniform_set_id = new_set;
  return new_set;
}

/**
 * @brief 释放 GPU 资源（2D Canvas 实现）
 *
 * 释放 UniformSet 和 UBO，清空 CPU 端数据缓冲区。
 *
 * @param[in] p_storage MaterialStorage 指针，用于访问 RenderingDevice
 *
 * @post uniform_set_id 和 uniform_buffer_id 失效
 * @post uniform_data 和 texture_rids 被清空
 * @post uniform_buffer_size 和 texture_count 置零
 *
 * @note 由 MaterialStorage::MaterialFree 调用
 */
void CanvasMaterialData::FreeResources(MaterialStorage* p_storage) {
  RenderingDevice* rd = p_storage->GetRD();

  if (uniform_set_id.IsValid()) {
    rd->UniformSetFree(uniform_set_id);
    uniform_set_id = RDUniformSetID();
  }

  if (uniform_buffer_id.IsValid()) {
    rd->BufferFree(uniform_buffer_id);
    uniform_buffer_id = RDBufferID();
  }

  uniform_data.Clear();
  texture_rids.Clear();
  uniform_buffer_size = 0;
  texture_count = 0;
}

// ═══════════════════════════════════════════════════════════════════════
// SpatialMaterialData 实现
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 同步 CPU 端 Uniform 数据到 GPU UBO（3D Spatial 实现）
 *
 * 将 uniform_data 中的数据上传到 GPU 端的 uniform_buffer_id。
 * 当前实现与 CanvasMaterialData 相同，未来可扩展光照/阴影 UBO 合并。
 *
 * @param[in] p_storage MaterialStorage 指针，用于访问 RenderingDevice
 *
 * @return Error::kOK     同步成功或无需同步（UBO 为空）
 * @return Error::kFailed BufferUpdate 失败
 *
 * @pre 必须在渲染线程调用
 * @pre uniform_buffer_id 已通过 MaterialCreate 创建
 *
 * @post uniform_set_version 递增
 *
 * @note 对应 Godot MaterialData::update_uniform_buffer
 * @note 未来扩展：Spatial 材质可能需要合并光照 UBO
 */
Error SpatialMaterialData::SyncUniformBuffer(MaterialStorage* p_storage) {
  ARHUD_ASSERT_ON_RENDER_THREAD(p_storage->GetRD());

  if (uniform_buffer_size == 0 || !uniform_buffer_id.IsValid()) {
    return Error::kOK;
  }

  RenderingDevice* rd = p_storage->GetRD();
  Error err = rd->BufferUpdate(uniform_buffer_id, 0, uniform_buffer_size,
                               uniform_data.Data());

  if (err != Error::kOK) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "SpatialMaterialData::SyncUniformBuffer() BufferUpdate failed");
    return Error::kFailed;
  }

  uniform_set_version++;

  return Error::kOK;
}

/**
 * @brief 重建 UniformSet（3D Spatial 实现）
 *
 * 根据 Shader 的 Uniform 反射信息，构建 BoundUniform 数组，
 * 将 UBO 和纹理绑定到 UniformSet。
 * 当前实现与 CanvasMaterialData 相同，未来可扩展多 UniformSet（材质 + 光照 +
 * 阴影）。
 *
 * @param[in] p_storage MaterialStorage 指针，用于访问
 * RD/ShaderStorage/TextureStorage
 *
 * @return 新创建的 UniformSet ID，失败时返回空 ID
 *
 * @pre 必须在渲染线程调用
 * @pre parent_material_rid 对应的 Material 存在且有效
 *
 * @post uniform_set_id 更新为新创建的 UniformSet
 *
 * @note 对应 Godot MaterialData::bind_uniforms
 * @note 未来扩展：Spatial 材质可能需要多个 UniformSet（材质 + 光照 + 阴影）
 */
RDUniformSetID SpatialMaterialData::RecreateUniformSet(
    MaterialStorage* p_storage) {
  ARHUD_ASSERT_ON_RENDER_THREAD(p_storage->GetRD());

  RenderingDevice* rd = p_storage->GetRD();
  ShaderStorage* shader_storage = p_storage->GetShaderStorage();
  TextureStorage* texture_storage = p_storage->GetTextureStorage();

  if (uniform_set_id.IsValid()) {
    rd->UniformSetFree(uniform_set_id);
    uniform_set_id = RDUniformSetID();
  }

  const Material* mat = p_storage->MaterialGetInfo(parent_material_rid);
  if (mat == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "SpatialMaterialData::RecreateUniformSet() parent Material not found");
    return RDUniformSetID();
  }

  const LocalVector<ShaderUniform>& uniforms =
      shader_storage->ShaderGetUniforms(mat->shader_rid);
  RDShaderID shader_rd_id = shader_storage->ShaderGetRDId(mat->shader_rid);

  LocalVector<BoundUniform> bound_uniforms;
  uint32 tex_index = 0;

  for (uint32 i = 0; i < uniforms.Size(); ++i) {
    BoundUniform bound;
    bound.binding = uniforms[i].binding;
    bound.immutable_sampler = false;

    switch (uniforms[i].type) {
      case UniformType::kSamplerWithTexture:
      case UniformType::kSamplerWithTextureBuffer:
      case UniformType::kTexture:
      case UniformType::kTextureBuffer:
      case UniformType::kImage:
      case UniformType::kImageBuffer: {
        bound.type = uniforms[i].type;

        // Texture ID only (no custom sampler, use texture's own parameters)
        if (tex_index < texture_count && texture_rids[tex_index].IsValid()) {
          RDTextureID tex_rd_id =
              texture_storage->TextureGetRDId(texture_rids[tex_index]);
          if (tex_rd_id.IsValid()) {
            TextureID tex_driver_id = rd->TextureGetDriverId(tex_rd_id);
            bound.ids.PushBack(BufferID(tex_driver_id.GetId()));
          }
        }
        tex_index++;
        break;
      }

      case UniformType::kUniformBuffer:
      case UniformType::kUniformBufferDynamic: {
        bound.type = uniforms[i].type;
        if (uniform_buffer_id.IsValid()) {
          BufferID buf_driver_id = rd->BufferGetDriverId(uniform_buffer_id);
          bound.ids.PushBack(buf_driver_id);
        }
        break;
      }

      case UniformType::kStorageBuffer:
      case UniformType::kStorageBufferDynamic: {
        bound.type = uniforms[i].type;
        break;
      }

      default:
        continue;
    }

    bound_uniforms.PushBack(bound);
  }

  RDUniformSetID new_set = rd->UniformSetCreate(
      VectorView<BoundUniform>(bound_uniforms.Data(), bound_uniforms.Size()),
      shader_rd_id, 0);

  if (!new_set.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "SpatialMaterialData::RecreateUniformSet() UniformSetCreate failed");
    return RDUniformSetID();
  }

  uniform_set_id = new_set;
  return new_set;
}

/**
 * @brief 释放 GPU 资源（3D Spatial 实现）
 *
 * 释放 UniformSet 和 UBO，清空 CPU 端数据缓冲区，
 * 并重置 Spatial 特有字段（priority、next_pass 等）。
 *
 * @param[in] p_storage MaterialStorage 指针，用于访问 RenderingDevice
 *
 * @post uniform_set_id 和 uniform_buffer_id 失效
 * @post uniform_data 和 texture_rids 被清空
 * @post priority、next_pass、last_pass、index 重置
 *
 * @note 由 MaterialStorage::MaterialFree 调用
 */
void SpatialMaterialData::FreeResources(MaterialStorage* p_storage) {
  RenderingDevice* rd = p_storage->GetRD();

  if (uniform_set_id.IsValid()) {
    rd->UniformSetFree(uniform_set_id);
    uniform_set_id = RDUniformSetID();
  }

  if (uniform_buffer_id.IsValid()) {
    rd->BufferFree(uniform_buffer_id);
    uniform_buffer_id = RDBufferID();
  }

  uniform_data.Clear();
  texture_rids.Clear();
  uniform_buffer_size = 0;
  texture_count = 0;
  priority = 0;
  next_pass = RID();
  last_pass = 0;
  index = 0;
}

// ═══════════════════════════════════════════════════════════════════════
// MaterialStorage Initialize / Finalize
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 初始化材质存储系统
 *
 * 保存 RenderingDevice、ShaderStorage、TextureStorage 的指针引用。
 * MaterialStorage 不拥有这些对象的生命周期，仅持有非拥有指针。
 *
 * @param[in] p_rd              RenderingDevice 指针（不拥有所有权）
 * @param[in] p_shader_storage  ShaderStorage 指针（不拥有所有权）
 * @param[in] p_texture_storage TextureStorage 指针（不拥有所有权）
 *
 * @retval Error::kOK            初始化成功
 * @retval Error::kAlreadyExists 已经初始化
 * @retval Error::kFailed        参数为空
 *
 * @pre p_rd、p_shader_storage、p_texture_storage 均非空
 * @pre 必须在渲染线程调用
 *
 * @post rd_、shader_storage_、texture_storage_ 指向传入的实例
 */
Error MaterialStorage::Initialize(RenderingDevice* p_rd,
                                  ShaderStorage* p_shader_storage,
                                  TextureStorage* p_texture_storage) {
  if (rd_ != nullptr) {
    ARHUD_LOG_ERROR("kAlreadyExists",
                    "MaterialStorage::Initialize() already initialized");
    return Error::kAlreadyExists;
  }

  if (p_rd == nullptr || p_shader_storage == nullptr ||
      p_texture_storage == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "MaterialStorage::Initialize() invalid parameters");
    return Error::kFailed;
  }

  rd_ = p_rd;
  shader_storage_ = p_shader_storage;
  texture_storage_ = p_texture_storage;

  return Error::kOK;
}

/**
 * @brief 终止材质存储系统，释放所有资源
 *
 * 遍历所有已注册的 Material，依次释放其 GPU 资源（UBO、UniformSet），
 * 然后释放 Material 本身。最后清空外部依赖指针。
 *
 * @pre 必须在 RenderingDevice 终止之前调用
 * @pre 必须在渲染线程调用
 *
 * @post 所有 Material 已释放
 * @post rd_、shader_storage_、texture_storage_ 置空
 *
 * @note 若未初始化则直接返回（安全操作）
 */
void MaterialStorage::Finalize() {
  if (rd_ == nullptr) {
    return;
  }

  for (uint32 i = 0; i < material_owner_.Size(); ++i) {
    Material* mat = material_owner_.GetOrNull(RID(i));
    if (mat == nullptr) {
      continue;
    }

    if (mat->data != nullptr) {
      mat->data->FreeResources(this);
      ARHUD_DELETE(mat->data);
      mat->data = nullptr;
    }

    material_owner_.Free(mat->rid);
  }

  rd_ = nullptr;
  shader_storage_ = nullptr;
  texture_storage_ = nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
// CreateMaterialData（参考 Godot MaterialDataRequestFunction）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 MaterialData 子类实例（工厂方法）
 *
 * 根据 MaterialDataType 创建对应的 MaterialData 子类：
 *   - kCanvas  → CanvasMaterialData（2D Canvas 材质）
 *   - kSpatial → SpatialMaterialData（3D Spatial 材质）
 *
 * @param[in] p_type 材质数据类型
 *
 * @return MaterialData 子类指针，失败时返回 nullptr
 *
 * @note 对应 Godot MaterialDataRequestFunction 工厂函数模式
 * @note 调用者负责释放返回的对象（通过 ARHUD_DELETE）
 */
MaterialData* MaterialStorage::CreateMaterialData(MaterialDataType p_type) {
  switch (p_type) {
    case MaterialDataType::kCanvas:
      return ARHUD_NEW(CanvasMaterialData);
    case MaterialDataType::kSpatial:
      return ARHUD_NEW(SpatialMaterialData);
    default:
      ARHUD_LOG_ERROR("kFailed",
                      "MaterialStorage::CreateMaterialData() unknown type %u",
                      static_cast<uint32>(p_type));
      return nullptr;
  }
}

// ═══════════════════════════════════════════════════════════════════════
// MaterialCreate
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建材质资源
 *
 * 创建流程：
 *   1. 校验 Shader RID 有效性
 *   2. 创建 MaterialData 子类实例
 *   3. 根据 Shader Uniform 反射信息计算 UBO 大小和纹理数量
 *   4. 预分配 CPU 端 uniform_data 缓冲区
 *   5. 创建 GPU 端 UBO（RD::BufferCreate）
 *   6. 预分配纹理 RID 缓存
 *   7. 标记 uniform_dirty 和 texture_dirty 为 true
 *
 * @param[in] p_name   材质名称（用于调试/日志，可为 nullptr）
 * @param[in] p_shader 关联的着色器 RID
 * @param[in] p_type   材质数据类型（默认 kCanvas）
 *
 * @return 材质 RID，失败时返回空 RID
 *
 * @pre 必须在渲染线程调用
 * @pre p_shader 必须是有效的 Shader RID
 *
 * @post 返回的 RID 有效时，Material 和 MaterialData 已创建
 * @post UBO 和纹理缓存已预分配
 * @post uniform_dirty 和 texture_dirty 为 true
 */
RID MaterialStorage::MaterialAllocate() {
  // 任意线程：仅分配线程安全的 RID 槽位（空壳），不做 GPU 操作
  auto* mat = ARHUD_NEW(Material);
  RID rid = material_owner_.MakeRid(mat);
  mat->rid = rid;
  mat->is_valid = false;
  return rid;
}

Error MaterialStorage::MaterialInitializeRid(RID p_material, const char* p_name,
                                             RID p_shader,
                                             MaterialDataType p_type) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  if (rd_ == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "MaterialStorage::MaterialInitializeRid() not initialized");
    return Error::kFailed;
  }

  Material* mat = material_owner_.GetOrNull(p_material);
  if (mat == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "MaterialStorage::MaterialInitializeRid() invalid RID");
    return Error::kFailed;
  }

  if (!p_shader.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MaterialStorage::MaterialInitializeRid() invalid shader RID for '%s'",
        p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  const ShaderInfo* shader_info = shader_storage_->ShaderGetInfo(p_shader);
  if (shader_info == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MaterialStorage::MaterialInitializeRid() shader not found for '%s'",
        p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  MaterialData* data = CreateMaterialData(p_type);
  if (data == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "MaterialStorage::MaterialInitializeRid() "
                    "CreateMaterialData failed for '%s'",
                    p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  mat->shader_rid = p_shader;
  mat->name = p_name ? UString(p_name) : UString();
  mat->data_type = p_type;
  mat->data = data;
  mat->is_valid = true;
  data->parent_material_rid = p_material;

  const LocalVector<ShaderUniform>& uniforms =
      shader_storage_->ShaderGetUniforms(p_shader);

  uint32 ubo_size = 0;
  uint32 tex_count = 0;
  for (uint32 i = 0; i < uniforms.Size(); ++i) {
    switch (uniforms[i].type) {
      case UniformType::kSamplerWithTexture:
      case UniformType::kSamplerWithTextureBuffer:
      case UniformType::kTexture:
      case UniformType::kTextureBuffer:
      case UniformType::kImage:
      case UniformType::kImageBuffer:
        tex_count++;
        break;
      case UniformType::kUniformBuffer:
      case UniformType::kUniformBufferDynamic:
        ubo_size += uniforms[i].size;
        break;
      default:
        break;
    }
  }

  if (ubo_size > 0) {
    data->uniform_data.Resize(ubo_size);
    for (uint32 i = 0; i < ubo_size; ++i) {
      data->uniform_data[i] = 0;
    }

    data->uniform_buffer_id = rd_->BufferCreate(
        ubo_size,
        BitField<BufferUsageBits>(BufferUsageBits::kUniform) |
            BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
        MemoryAllocationType::kCpu);
    data->uniform_buffer_size = ubo_size;
  }

  if (tex_count > 0) {
    data->texture_rids.Resize(tex_count);
    for (uint32 i = 0; i < tex_count; ++i) {
      data->texture_rids[i] = RID();
    }
  }
  data->texture_count = tex_count;

  mat->uniform_dirty = true;
  mat->texture_dirty = true;

  return Error::kOK;
}

void MaterialStorage::MaterialInitialize(RID p_material,
                                         const MaterialCreateParams& p_params,
                                         const uint8* p_blob) {
  const char* name = reinterpret_cast<const char*>(p_blob);
  if (MaterialInitializeRid(p_material, name, p_params.shader,
                            p_params.data_type) != Error::kOK) {
    MaterialFree(p_material);
  }
}

RID MaterialStorage::MaterialCreate(const char* p_name, RID p_shader,
                                    MaterialDataType p_type) {
  RID rid = MaterialAllocate();
  if (!rid.IsValid()) {
    return RID();
  }
  if (MaterialInitializeRid(rid, p_name, p_shader, p_type) != Error::kOK) {
    MaterialFree(rid);
    return RID();
  }
  return rid;
}

// ═══════════════════════════════════════════════════════════════════════
// MaterialSetUniform
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 设置材质的 Uniform 数据
 *
 * 将数据写入材质的 CPU 端 Uniform 缓冲区指定偏移位置。
 * 调用后标记 uniform_dirty，下次 MaterialGetUniformSet 时同步到 GPU。
 *
 * @param[in] p_material 材质 RID
 * @param[in] p_offset   Uniform 数据在 UBO 中的偏移量（字节，std140 布局）
 * @param[in] p_data     数据指针
 * @param[in] p_size     数据大小（字节）
 *
 * @retval Error::kOK     设置成功
 * @retval Error::kFailed 无效 RID、数据为空或越界
 *
 * @pre 必须在渲染线程调用
 * @pre p_material 必须是有效的材质 RID
 * @pre p_offset + p_size 不超过 UBO 大小
 *
 * @post uniform_data 已更新
 * @post uniform_dirty 置为 true
 * @post uniform_set_version 和 version 递增
 *
 * @note 对应 Godot material_set_param
 * @note 数据不会立即同步到 GPU，延迟到 MaterialGetUniformSet 时按需同步
 */
Error MaterialStorage::MaterialSetUniform(RID p_material, uint32 p_offset,
                                          const void* p_data, uint32 p_size) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  Material* mat = material_owner_.GetOrNull(p_material);
  if (mat == nullptr || !mat->is_valid || mat->data == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MaterialStorage::MaterialSetUniform() invalid material RID");
    return Error::kFailed;
  }

  if (p_data == nullptr || p_size == 0) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MaterialStorage::MaterialSetUniform() invalid data for '%s'",
        mat->name.CStr());
    return Error::kFailed;
  }

  if (p_offset + p_size > mat->data->uniform_buffer_size) {
    ARHUD_LOG_ERROR("kFailed",
                    "MaterialStorage::MaterialSetUniform() out of bounds: "
                    "offset=%u size=%u buffer=%u for '%s'",
                    p_offset, p_size, mat->data->uniform_buffer_size,
                    mat->name.CStr());
    return Error::kFailed;
  }

  uint8* dst = mat->data->uniform_data.Data() + p_offset;
  const uint8* src = static_cast<const uint8*>(p_data);
  for (uint32 i = 0; i < p_size; ++i) {
    dst[i] = src[i];
  }

  mat->uniform_dirty = true;
  mat->data->uniform_set_version++;  // uniform 变更需要重建 UniformSet
  mat->data->version++;              // Renderer 缓存失效

  return Error::kOK;
}

// ═══════════════════════════════════════════════════════════════════════
// MaterialSetTexture
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 设置材质的纹理绑定
 *
 * 将纹理绑定到材质的指定纹理槽位。
 * 调用后标记 texture_dirty，下次 MaterialGetUniformSet 时重建 UniformSet。
 *
 * @param[in] p_material 材质 RID
 * @param[in] p_slot     纹理槽位索引（由 Shader 反射决定）
 * @param[in] p_texture  纹理 RID（TextureStorage 的 RID）
 *
 * @retval Error::kOK     设置成功
 * @retval Error::kFailed 无效 RID 或槽位越界
 *
 * @pre 必须在渲染线程调用
 * @pre p_material 必须是有效的材质 RID
 * @pre p_slot 小于材质的纹理槽位数
 *
 * @post texture_rids[p_slot] 已更新
 * @post texture_dirty 置为 true
 * @post uniform_set_version 和 version 递增
 *
 * @note 对应 Godot material_set_param（纹理类型参数）
 */
Error MaterialStorage::MaterialSetTexture(RID p_material, uint32 p_slot,
                                          RID p_texture) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  Material* mat = material_owner_.GetOrNull(p_material);
  if (mat == nullptr || !mat->is_valid || mat->data == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MaterialStorage::MaterialSetTexture() invalid material RID");
    return Error::kFailed;
  }

  if (p_slot >= mat->data->texture_count) {
    ARHUD_LOG_ERROR("kFailed",
                    "MaterialStorage::MaterialSetTexture() slot %u out of "
                    "range (max %u) for '%s'",
                    p_slot, mat->data->texture_count, mat->name.CStr());
    return Error::kFailed;
  }

  mat->data->texture_rids[p_slot] = p_texture;
  mat->texture_dirty = true;
  mat->data->uniform_set_version++;  // 纹理变更需要重建 UniformSet
  mat->data->version++;              // Renderer 缓存失效

  return Error::kOK;
}

// ═══════════════════════════════════════════════════════════════════════
// MaterialSetRenderPriority / MaterialSetNextPass
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 设置材质的渲染优先级
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
 * @retval Error::kFailed 无效 RID 或优先级超出范围
 *
 * @pre 必须在渲染线程调用
 *
 * @note 对应 Godot material_set_render_priority
 * @note 仅 SpatialMaterialData 生效，CanvasMaterialData 忽略
 */
Error MaterialStorage::MaterialSetUniformBlob(
    const MaterialSetUniformParams& p_params, const uint8* p_blob) {
  return MaterialSetUniform(p_params.material, p_params.offset, p_blob,
                            p_params.size);
}

Error MaterialStorage::MaterialSetRenderPriority(RID p_material,
                                                 int32 p_priority) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  Material* mat = material_owner_.GetOrNull(p_material);
  if (mat == nullptr || !mat->is_valid || mat->data == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MaterialStorage::MaterialSetRenderPriority() invalid material RID");
    return Error::kFailed;
  }

  if (p_priority < -128 || p_priority > 127) {
    ARHUD_LOG_ERROR("kFailed",
                    "MaterialStorage::MaterialSetRenderPriority() priority %d "
                    "out of range [-128, 127] for '%s'",
                    p_priority, mat->name.CStr());
    return Error::kFailed;
  }

  if (mat->data_type == MaterialDataType::kSpatial) {
    auto* spatial_data = static_cast<SpatialMaterialData*>(mat->data);
    spatial_data->priority = static_cast<uint8>(p_priority + 128);
  }

  return Error::kOK;
}

/**
 * @brief 设置下一个 pass 的材质
 *
 * 仅对 SpatialMaterialData 有效（3D 多 pass 渲染）。
 * CanvasMaterialData 忽略此设置。
 *
 * 多 pass 渲染用于描边、辉光等效果：
 *   Renderer 绘制完当前材质后，检查 next_pass，
 *   如果非空则用同一几何体再次绘制 next_pass 材质。
 *
 * @param[in] p_material       材质 RID
 * @param[in] p_next_material  下一个 pass 的材质 RID（空 RID 表示无 next pass）
 *
 * @retval Error::kOK     设置成功
 * @retval Error::kFailed 无效 RID
 *
 * @pre 必须在渲染线程调用
 *
 * @note 对应 Godot material_set_next_pass
 * @note 仅 SpatialMaterialData 生效，CanvasMaterialData 忽略
 */
Error MaterialStorage::MaterialSetNextPass(RID p_material,
                                           RID p_next_material) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  Material* mat = material_owner_.GetOrNull(p_material);
  if (mat == nullptr || !mat->is_valid || mat->data == nullptr) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MaterialStorage::MaterialSetNextPass() invalid material RID");
    return Error::kFailed;
  }

  if (mat->data_type == MaterialDataType::kSpatial) {
    auto* spatial_data = static_cast<SpatialMaterialData*>(mat->data);
    spatial_data->next_pass = p_next_material;
  }

  return Error::kOK;
}

// ═══════════════════════════════════════════════════════════════════════
// MaterialFree
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 释放材质资源
 *
 * 释放流程：
 *   1. 委托 MaterialData::FreeResources 释放 GPU 资源（UBO、UniformSet）
 *   2. 释放 MaterialData 对象
 *   3. 标记 Material 为无效
 *   4. 从 RIDOwner 中释放 RID 槽位
 *
 * @param[in] p_material 要释放的材质 RID
 *
 * @pre 必须在渲染线程调用
 *
 * @post p_material 对应的 RID 槽位已释放，可被后续分配复用
 * @post 关联的 GPU 资源（UBO、UniformSet）已释放
 *
 * @note 对无效 RID 安全：p_material 未注册或已释放时直接返回
 */
void MaterialStorage::MaterialFree(RID p_material) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  Material* mat = material_owner_.GetOrNull(p_material);
  if (mat == nullptr) {
    return;
  }

  if (mat->data != nullptr) {
    mat->data->FreeResources(this);
    ARHUD_DELETE(mat->data);
    mat->data = nullptr;
  }

  mat->is_valid = false;
  material_owner_.Free(p_material);
}

// ═══════════════════════════════════════════════════════════════════════
// MaterialGetInfo / MaterialIsValid
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 获取材质信息
 *
 * 返回包含材质业务数据（RID、Shader 关联、dirty 标记等）的 Material 指针。
 * 此方法为纯只读操作，不涉及任何 GPU 调用，可在任意线程安全调用。
 *
 * @param[in] p_material 材质 RID
 *
 * @return Material 指针（成功），nullptr（RID 无效或材质已标记为无效）
 *
 * @note 返回的指针在材质被释放前有效。调用者不应持有所返回的指针跨帧。
 */
const Material* MaterialStorage::MaterialGetInfo(RID p_material) const {
  const Material* mat = material_owner_.GetOrNull(p_material);
  if (mat == nullptr || !mat->is_valid) {
    return nullptr;
  }
  return mat;
}

/**
 * @brief 检查材质 RID 是否有效
 *
 * 有效条件：
 *   1. RID 已注册到 material_owner_
 *   2. Material::is_valid 标志位为 true（未被 MaterialFree 释放）
 *
 * @param[in] p_material 材质 RID
 *
 * @return true  材质有效，可安全用于查询方法
 * @return false RID 未注册或已释放
 *
 * @note 此方法为纯只读操作，可在任意线程安全调用
 */
bool MaterialStorage::MaterialIsValid(RID p_material) const {
  const Material* mat = material_owner_.GetOrNull(p_material);
  return mat != nullptr && mat->is_valid;
}

// ═══════════════════════════════════════════════════════════════════════
// MaterialGetUniformSet（参考 Godot update_parameters）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 获取或创建材质的 UniformSet
 *
 * 检查 dirty 标记并委托 MaterialData 子类执行 GPU 同步：
 *   1. uniform_dirty → MaterialData::SyncUniformBuffer（上传 UBO 到 GPU）
 *   2. texture_dirty 或版本号变化 → MaterialData::RecreateUniformSet（重建
 * UniformSet）
 *
 * @param[in] p_material 材质 RID
 *
 * @return UniformSet ID，失败时返回空 ID
 *
 * @pre 必须在渲染线程调用
 * @pre p_material 必须是有效的材质 RID
 *
 * @post uniform_dirty 置为 false（若之前为 true）
 * @post texture_dirty 置为 false（若之前为 true）
 * @post last_synced_version 更新为 uniform_set_version
 *
 * @note 对应 Godot update_parameters
 * @note 延迟同步设计：SetUniform/SetTexture 只写 CPU 副本，
 *       此方法在需要时才同步到 GPU，减少驱动开销
 */
RDUniformSetID MaterialStorage::MaterialGetUniformSet(RID p_material) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  Material* mat = material_owner_.GetOrNull(p_material);
  if (mat == nullptr || !mat->is_valid || mat->data == nullptr) {
    return RDUniformSetID();
  }

  if (mat->uniform_dirty) {
    mat->data->SyncUniformBuffer(this);
    mat->uniform_dirty = false;
  }

  // Recreate UniformSet if textures changed OR if uniform version changed
  // (version change means UBO data was updated, and for safety we rebuild)
  if (mat->texture_dirty ||
      mat->data->last_synced_version != mat->data->uniform_set_version) {
    mat->data->RecreateUniformSet(this);
    mat->texture_dirty = false;
    mat->data->last_synced_version = mat->data->uniform_set_version;
  }

  return mat->data->uniform_set_id;
}

}  // namespace arhud
