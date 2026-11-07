/**
 * @file mesh_storage.cpp
 * @brief MeshStorage 实现
 *
 * 参考 Godot 4.6 GLES3 MeshStorage 的核心逻辑：
 *   - mesh_create → MeshInfo（空容器）
 *   - mesh_add_surface → SurfaceInfo（VBO/IBO + 数据上传）
 *   - mesh_free → 释放所有 Surface + Mesh
 *
 * ARHud 适配：
 *   - MeshCreate 创建空容器，MeshAddSurface 添加 Surface
 *   - 通过 RD API 间接操作 GPU，不直接调用 GL
 *   - 内置图元工厂使用 CreateQuad/Rect/Line → 内部调用 MeshCreate +
 * MeshAddSurface
 */

#include "mesh_storage.h"

#include <cmath>

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// Initialize / Finalize
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 初始化网格存储
 *
 * 校验 RenderingDevice 有效性后建立绑定。本方法不创建任何网格，
 * 网格由调用方通过 MeshCreate 显式创建。
 *
 * @param[in] p_rd RenderingDevice 指针（不拥有所有权）
 *
 * @retval Error::kOK            初始化成功，rd_ 绑定到 p_rd
 * @retval Error::kAlreadyExists 重复初始化（rd_ 已绑定）
 * @retval Error::kFailed        p_rd 为空
 */
Error MeshStorage::Initialize(RenderingDevice* p_rd) {
  if (rd_ != nullptr) {
    ARHUD_LOG_ERROR("kAlreadyExists",
                    "MeshStorage::Initialize() already initialized");
    return Error::kAlreadyExists;
  }

  if (p_rd == nullptr) {
    ARHUD_LOG_ERROR("kFailed", "MeshStorage::Initialize() invalid parameters");
    return Error::kFailed;
  }

  rd_ = p_rd;

  return Error::kOK;
}

/**
 * @brief 结束网格存储并释放所有资源
 *
 * 遍历全部 MeshInfo 条目：对每个网格释放其全部 Surface
 * （FreeSurface 委托 RD 释放 VBO/IBO，随后 ARHUD_DELETE 簿记对象），
 * 清空 surfaces 列表并释放网格 RID。
 *
 * @pre 必须在 RenderingDevice 结束之前调用（GPU 资源需经 RD 释放）
 *
 * @post mesh_owner_ 已清空，rd_ == nullptr；未初始化时静默返回
 */
void MeshStorage::Finalize() {
  if (rd_ == nullptr) {
    return;
  }

  for (uint32 i = 0; i < mesh_owner_.Size(); ++i) {
    MeshInfo* info = mesh_owner_.GetOrNull(RID(i));
    if (info == nullptr) {
      continue;
    }

    for (uint32 j = 0; j < info->surfaces.Size(); ++j) {
      if (info->surfaces[j] != nullptr) {
        FreeSurface(info->surfaces[j]);
        ARHUD_DELETE(info->surfaces[j]);
      }
    }
    info->surfaces.Clear();

    mesh_owner_.Free(info->rid);
  }

  rd_ = nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
// FreeSurface
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 释放单个 Surface 的 GPU 缓冲（私有辅助）
 *
 * 按需释放索引缓冲（IBO）与顶点缓冲（VBO）——各以
 * IsValid() 判空保护，无效句柄直接跳过。仅释放 RD 端缓冲，
 * 不删除 SurfaceInfo 对象本身（由调用方负责）。
 *
 * @param[in] p_surface 目标 SurfaceInfo 指针（可空，空则静默返回）
 */
void MeshStorage::FreeSurface(SurfaceInfo* p_surface) {
  if (p_surface == nullptr) {
    return;
  }

  if (p_surface->index_buffer_id.IsValid()) {
    rd_->BufferFree(p_surface->index_buffer_id);
  }

  if (p_surface->vertex_buffer_id.IsValid()) {
    rd_->BufferFree(p_surface->vertex_buffer_id);
  }
}

// ═══════════════════════════════════════════════════════════════════════
// MeshCreate（参考 Godot mesh_create）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建空网格容器（参考 Godot mesh_create）
 *
 * 分配 MeshInfo 并登记 RID，不包含任何 Surface；
 * 随后由 MeshAddSurface 逐面片填充。
 *
 * @param[in] p_name 网格名称（用于调试/日志，可空）
 *
 * @return 网格 RID；未初始化时返回空 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 */
RID MeshStorage::MeshCreate(const char* p_name) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  if (rd_ == nullptr) {
    ARHUD_LOG_ERROR("kFailed", "MeshStorage::MeshCreate() not initialized");
    return RID();
  }

  auto* info = ARHUD_NEW(MeshInfo);
  RID rid = mesh_owner_.MakeRid(info);
  info->rid = rid;
  info->name = p_name ? UString(p_name) : UString();
  info->is_valid = true;

  return rid;
}

// ═══════════════════════════════════════════════════════════════════════
// RID 分裂：MeshAllocate + MeshInitialize*（对齐 §7.11 F / Godot FUNCRIDSPLIT）
// ═══════════════════════════════════════════════════════════════════════

RID MeshStorage::MeshAllocate() {
  // 任意线程：仅分配线程安全的 RID 槽位（空壳），不做 GPU 操作
  auto* info = ARHUD_NEW(MeshInfo);
  RID rid = mesh_owner_.MakeRid(info);
  info->rid = rid;
  info->is_valid = false;
  return rid;
}

Error MeshStorage::MeshInitializeQuad(RID p_mesh,
                                      const MeshCreateQuadParams& p_params,
                                      const uint8* p_name_blob) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);
  const char* p_name = reinterpret_cast<const char*>(p_name_blob);

  MeshInfo* info = mesh_owner_.GetOrNull(p_mesh);
  if (info == nullptr) {
    ARHUD_LOG_ERROR("kFailed", "MeshStorage::MeshInitializeQuad() invalid RID");
    return Error::kFailed;
  }

  if (p_params.width <= 0.0f || p_params.height <= 0.0f) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MeshStorage::MeshInitializeQuad() invalid dimensions for '%s'",
        p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  struct Vertex2D {
    float x, y;
    float u, v;
  };

  Vertex2D vertices[4] = {
      {0.0f, 0.0f, 0.0f, 0.0f},
      {p_params.width, 0.0f, 1.0f, 0.0f},
      {p_params.width, p_params.height, 1.0f, 1.0f},
      {0.0f, p_params.height, 0.0f, 1.0f},
  };

  uint16 indices[6] = {0, 1, 2, 0, 2, 3};

  info->name = p_name ? UString(p_name) : UString();
  uint32 surf_idx =
      MeshAddSurface(p_mesh, vertices, 4, kVertex2DStride, indices, 6,
                     IndexBufferFormat::kUint16, RenderPrimitive::kTriangles);

  if (surf_idx == kInvalidSurfaceIndex) {
    return Error::kFailed;
  }

  SurfaceInfo* surf = mesh_owner_.GetOrNull(p_mesh)->surfaces[surf_idx];
  surf->aabb = AABB(0.0f, 0.0f, 0.0f, p_params.width, p_params.height, 0.0f);
  info->aabb = AABB(0.0f, 0.0f, 0.0f, p_params.width, p_params.height, 0.0f);
  info->is_valid = true;
  return Error::kOK;
}

Error MeshStorage::MeshInitializeBox(RID p_mesh,
                                     const MeshCreateBoxParams& p_params,
                                     const uint8* p_name_blob) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);
  const char* p_name = reinterpret_cast<const char*>(p_name_blob);

  MeshInfo* info = mesh_owner_.GetOrNull(p_mesh);
  if (info == nullptr) {
    ARHUD_LOG_ERROR("kFailed", "MeshStorage::MeshInitializeBox() invalid RID");
    return Error::kFailed;
  }

  if (p_params.width <= 0.0f || p_params.height <= 0.0f ||
      p_params.depth <= 0.0f) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MeshStorage::MeshInitializeBox() invalid dimensions for '%s'",
        p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  struct Vertex3D {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
  };

  float hw = p_params.width * 0.5f;
  float hh = p_params.height * 0.5f;
  float hd = p_params.depth * 0.5f;

  Vertex3D vertices[24] = {
      {hw, -hh, hd, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
      {-hw, -hh, hd, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      {-hw, hh, hd, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
      {hw, hh, hd, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f},

      {-hw, -hh, -hd, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f},
      {hw, -hh, -hd, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f},
      {hw, hh, -hd, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f},
      {-hw, hh, -hd, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f},

      {hw, hh, hd, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
      {-hw, hh, hd, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
      {-hw, hh, -hd, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f},
      {hw, hh, -hd, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},

      {hw, -hh, -hd, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f},
      {-hw, -hh, -hd, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f},
      {-hw, -hh, hd, 0.0f, -1.0f, 0.0f, 1.0f, 1.0f},
      {hw, -hh, hd, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f},

      {-hw, -hh, hd, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
      {-hw, -hh, -hd, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f},
      {-hw, hh, -hd, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f},
      {-hw, hh, hd, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f},

      {hw, -hh, -hd, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
      {hw, -hh, hd, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f},
      {hw, hh, hd, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f},
      {hw, hh, -hd, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
  };

  uint16 indices[36] = {
      0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
      12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
  };

  info->name = p_name ? UString(p_name) : UString();
  uint32 surf_idx =
      MeshAddSurface(p_mesh, vertices, 24, kVertex3DStride, indices, 36,
                     IndexBufferFormat::kUint16, RenderPrimitive::kTriangles);

  if (surf_idx == kInvalidSurfaceIndex) {
    return Error::kFailed;
  }

  AABB box_aabb(-hw, -hh, -hd, p_params.width, p_params.height, p_params.depth);
  mesh_owner_.GetOrNull(p_mesh)->surfaces[surf_idx]->aabb = box_aabb;
  info->aabb = box_aabb;
  info->is_valid = true;
  return Error::kOK;
}

Error MeshStorage::MeshInitializeSphere(RID p_mesh,
                                        const MeshCreateSphereParams& p_params,
                                        const uint8* p_name_blob) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);
  const char* p_name = reinterpret_cast<const char*>(p_name_blob);

  MeshInfo* info = mesh_owner_.GetOrNull(p_mesh);
  if (info == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::MeshInitializeSphere() invalid RID");
    return Error::kFailed;
  }

  if (p_params.radius <= 0.0f || p_params.radial_segments < 3 ||
      p_params.rings < 1) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MeshStorage::MeshInitializeSphere() invalid params for '%s'",
        p_name ? p_name : "(null)");
    return Error::kFailed;
  }

  struct Vertex3D {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
  };

  uint32 vertex_count = (p_params.rings + 1) * (p_params.radial_segments + 1);
  uint32 index_count = p_params.rings * p_params.radial_segments * 6;

  auto* vertices = ARHUD_NEW_ARR(Vertex3D, vertex_count);
  auto* indices = ARHUD_NEW_ARR(uint16, index_count);

  uint32 vi = 0;
  uint32 ii = 0;

  for (uint32 r = 0; r <= p_params.rings; ++r) {
    float v = static_cast<float>(r) / static_cast<float>(p_params.rings);
    float phi = v * MathFuncs::kPi;

    float sin_phi = MathFuncs::Sin(phi);
    float cos_phi = MathFuncs::Cos(phi);

    for (uint32 s = 0; s <= p_params.radial_segments; ++s) {
      float u =
          static_cast<float>(s) / static_cast<float>(p_params.radial_segments);
      float theta = u * 2.0f * MathFuncs::kPi;

      float sin_theta = MathFuncs::Sin(theta);
      float cos_theta = MathFuncs::Cos(theta);

      float nx = sin_phi * cos_theta;
      float ny = cos_phi;
      float nz = sin_phi * sin_theta;

      vertices[vi].x = nx * p_params.radius;
      vertices[vi].y = ny * p_params.radius;
      vertices[vi].z = nz * p_params.radius;
      vertices[vi].nx = nx;
      vertices[vi].ny = ny;
      vertices[vi].nz = nz;
      vertices[vi].u = u;
      vertices[vi].v = v;
      ++vi;
    }
  }

  for (uint32 r = 0; r < p_params.rings; ++r) {
    for (uint32 s = 0; s < p_params.radial_segments; ++s) {
      uint32 a = r * (p_params.radial_segments + 1) + s;
      uint32 b = a + p_params.radial_segments + 1;

      indices[ii++] = static_cast<uint16>(a);
      indices[ii++] = static_cast<uint16>(b);
      indices[ii++] = static_cast<uint16>(a + 1);

      indices[ii++] = static_cast<uint16>(a + 1);
      indices[ii++] = static_cast<uint16>(b);
      indices[ii++] = static_cast<uint16>(b + 1);
    }
  }

  info->name = p_name ? UString(p_name) : UString();
  uint32 surf_idx = MeshAddSurface(
      p_mesh, vertices, vertex_count, kVertex3DStride, indices, index_count,
      IndexBufferFormat::kUint16, RenderPrimitive::kTriangles);

  ARHUD_DELETE_ARR(vertices, Vertex3D);
  ARHUD_DELETE_ARR(indices, uint16);

  if (surf_idx == kInvalidSurfaceIndex) {
    return Error::kFailed;
  }

  AABB sphere_aabb(-p_params.radius, -p_params.radius, -p_params.radius,
                   2.0f * p_params.radius, 2.0f * p_params.radius,
                   2.0f * p_params.radius);
  mesh_owner_.GetOrNull(p_mesh)->surfaces[surf_idx]->aabb = sphere_aabb;
  info->aabb = sphere_aabb;
  info->is_valid = true;
  return Error::kOK;
}

// ═══════════════════════════════════════════════════════════════════════
// MeshAddSurface（参考 Godot mesh_add_surface）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 向网格添加一个渲染 Surface（参考 Godot mesh_add_surface）
 *
 * 完整流程：
 *  1. 前置校验：RD 已初始化、网格 RID 有效、顶点数据合法
 *     （vertices/count/stride 均非零）、索引计数与指针一致性
 *     （p_index_count > 0 时 p_indices 必须非空）；
 *  2. 构建 SurfaceInfo 并登记元数据（count/stride/format/primitive/
 *     material）；
 *  3. 顶点数据深拷贝到 cpu_vertex_data（供 CPU 侧批量合并使用），
 *     创建 GPU VBO（kVertex | kTransferTo）并上传；
 *  4. p_index_count > 0 时按索引格式换算大小（u16/u32），创建 GPU
 *     IBO（kIndex | kTransferTo）并上传，同时深拷贝到
 *     cpu_index_data；
 *  5. 任一步失败回滚已创建资源（BufferFree + 删除 SurfaceInfo）
 *     并返回 kInvalidSurfaceIndex；成功后追加到 surfaces 列表。
 *
 * @param[in] p_mesh         目标网格 RID
 * @param[in] p_vertices     顶点数据指针（CPU 内存）
 * @param[in] p_vertex_count 顶点数量
 * @param[in] p_vertex_stride 单个顶点字节步幅（如 kVertex2DStride）
 * @param[in] p_indices      索引数据指针（可空，配合 p_index_count）
 * @param[in] p_index_count  索引数量（0 表示无索引缓冲）
 * @param[in] p_index_format 索引格式（u16 或 u32）
 * @param[in] p_primitive_type 图元类型
 * @param[in] p_material_rid 关联材质 RID（可空）
 *
 * @return Surface 索引（追加位置）；校验或创建失败时返回
 *         kInvalidSurfaceIndex
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 *
 * @note 顶点/索引均保留 CPU 副本，内存开销约为 GPU 侧两倍；
 *       这是 CPU 侧批处理（Batch）合并 DrawCall 的代价。
 */
uint32 MeshStorage::MeshAddSurface(RID p_mesh, const void* p_vertices,
                                   uint32 p_vertex_count,
                                   uint32 p_vertex_stride,
                                   const void* p_indices, uint32 p_index_count,
                                   IndexBufferFormat p_index_format,
                                   RenderPrimitive p_primitive_type,
                                   RID p_material_rid) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  if (rd_ == nullptr) {
    ARHUD_LOG_ERROR("kFailed", "MeshStorage::MeshAddSurface() not initialized");
    return kInvalidSurfaceIndex;
  }

  MeshInfo* info = mesh_owner_.GetOrNull(p_mesh);
  if (info == nullptr || !info->is_valid) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::MeshAddSurface() invalid mesh RID");
    return kInvalidSurfaceIndex;
  }

  if (p_vertices == nullptr || p_vertex_count == 0 || p_vertex_stride == 0) {
    ARHUD_LOG_ERROR(
        "kFailed", "MeshStorage::MeshAddSurface() invalid vertex data for '%s'",
        info->name.CStr());
    return kInvalidSurfaceIndex;
  }

  if (p_index_count > 0 && p_indices == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::MeshAddSurface() index count > 0 but null "
                    "indices for '%s'",
                    info->name.CStr());
    return kInvalidSurfaceIndex;
  }

  auto* surface = ARHUD_NEW(SurfaceInfo);

  surface->vertex_count = p_vertex_count;
  surface->vertex_stride = p_vertex_stride;
  surface->index_count = p_index_count;
  surface->index_format = p_index_format;
  surface->primitive_type = p_primitive_type;
  surface->material_rid = p_material_rid;

  uint32 vertex_data_size = p_vertex_count * p_vertex_stride;

  // Store CPU-side vertex data copy for batching
  surface->cpu_vertex_data.Resize(vertex_data_size);
  memcpy(surface->cpu_vertex_data.Data(), p_vertices, vertex_data_size);

  surface->vertex_buffer_id = rd_->BufferCreate(
      vertex_data_size,
      BitField<BufferUsageBits>(BufferUsageBits::kVertex) |
          BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
      MemoryAllocationType::kGpu);

  if (!surface->vertex_buffer_id.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed", "MeshStorage::MeshAddSurface() VBO creation failed for '%s'",
        info->name.CStr());
    ARHUD_DELETE(surface);
    return kInvalidSurfaceIndex;
  }

  Error err = rd_->BufferUpdate(surface->vertex_buffer_id, 0, vertex_data_size,
                                p_vertices);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::MeshAddSurface() VBO upload failed for '%s'",
                    info->name.CStr());
    rd_->BufferFree(surface->vertex_buffer_id);
    ARHUD_DELETE(surface);
    return kInvalidSurfaceIndex;
  }

  if (p_index_count > 0 && p_indices != nullptr) {
    uint32 index_size = (p_index_format == IndexBufferFormat::kUint32)
                            ? sizeof(uint32)
                            : sizeof(uint16);
    uint32 index_data_size = p_index_count * index_size;

    surface->index_buffer_id = rd_->BufferCreate(
        index_data_size,
        BitField<BufferUsageBits>(BufferUsageBits::kIndex) |
            BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
        MemoryAllocationType::kGpu);

    if (!surface->index_buffer_id.IsValid()) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "MeshStorage::MeshAddSurface() IBO creation failed for '%s'",
          info->name.CStr());
      rd_->BufferFree(surface->vertex_buffer_id);
      ARHUD_DELETE(surface);
      return kInvalidSurfaceIndex;
    }

    err = rd_->BufferUpdate(surface->index_buffer_id, 0, index_data_size,
                            p_indices);
    if (err != Error::kOK) {
      ARHUD_LOG_ERROR(
          "kFailed", "MeshStorage::MeshAddSurface() IBO upload failed for '%s'",
          info->name.CStr());
      rd_->BufferFree(surface->index_buffer_id);
      rd_->BufferFree(surface->vertex_buffer_id);
      ARHUD_DELETE(surface);
      return kInvalidSurfaceIndex;
    }

    // Store CPU-side index data copy for batching
    surface->cpu_index_data.Resize(index_data_size);
    memcpy(surface->cpu_index_data.Data(), p_indices, index_data_size);
  }

  uint32 surface_idx = info->surfaces.Size();
  info->surfaces.PushBack(surface);

  return surface_idx;
}

// ═══════════════════════════════════════════════════════════════════════
// MeshFree
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 释放网格资源
 *
 * 遍历释放全部 Surface（FreeSurface 释放 GPU 缓冲 + 删除簿记对象），
 * 清空 surfaces 列表，置 is_valid = false 后释放网格 RID。
 *
 * @param[in] p_mesh 网格 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 *
 * @note RID 无效时静默返回（幂等，可安全重复调用）
 */
void MeshStorage::MeshFree(RID p_mesh) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  MeshInfo* info = mesh_owner_.GetOrNull(p_mesh);
  if (info == nullptr) {
    return;
  }

  for (uint32 i = 0; i < info->surfaces.Size(); ++i) {
    if (info->surfaces[i] != nullptr) {
      FreeSurface(info->surfaces[i]);
      ARHUD_DELETE(info->surfaces[i]);
    }
  }
  info->surfaces.Clear();

  info->is_valid = false;
  mesh_owner_.Free(p_mesh);
}

// ═══════════════════════════════════════════════════════════════════════
// 2D 图元工厂（内部调用 MeshCreate + MeshAddSurface）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 2D 四边形网格（原点在左上角）
 *
 * 生成 4 顶点（XY + UV，kVertex2DStride=16B）与 6 索引
 * （两个三角形 {0,1,2} {0,2,3}），UV 覆盖 [0,1]×[0,1]。
 * 顶点位于 (0,0)~(p_width,p_height) 区域。成功后为 Surface
 * 与 Mesh 设置一致的 AABB。
 *
 * @param[in] p_name   网格名称（用于调试/日志，可空）
 * @param[in] p_width  宽度（必须 > 0）
 * @param[in] p_height 高度（必须 > 0）
 *
 * @return 网格 RID；尺寸非法或 MeshAddSurface 失败时返回空 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 *
 * @note 内部依次调用 MeshCreate + MeshAddSurface（无索引失败时
 *       自动回滚 MeshFree）
 */
RID MeshStorage::CreateQuad(const char* p_name, float p_width, float p_height) {
  RID rid = MeshAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  MeshCreateQuadParams params;
  params.width = p_width;
  params.height = p_height;

  if (MeshInitializeQuad(rid, params, reinterpret_cast<const uint8*>(p_name)) !=
      Error::kOK) {
    MeshFree(rid);
    return RID();
  }
  return rid;
}

/**
 * @brief 创建带偏移的 2D 矩形网格
 *
 * 与 CreateQuad 的差异在于显式指定左下角锚点 (p_x, p_y)，
 * 顶点分布于 (p_x,p_y)~(p_x+p_width, p_y+p_height)，UV 仍覆盖
 * [0,1]×[0,1]。成功后为 Surface 与 Mesh 设置一致的 AABB。
 *
 * @param[in] p_name   网格名称（用于调试/日志，可空）
 * @param[in] p_x      左下角 X（像素）
 * @param[in] p_y      左下角 Y（像素）
 * @param[in] p_width  宽度（必须 > 0）
 * @param[in] p_height 高度（必须 > 0）
 *
 * @return 网格 RID；尺寸非法或 MeshAddSurface 失败时返回空 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 */
RID MeshStorage::CreateRect(const char* p_name, float p_x, float p_y,
                            float p_width, float p_height) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  if (p_width <= 0.0f || p_height <= 0.0f) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreateRect() invalid dimensions for '%s'",
                    p_name ? p_name : "(null)");
    return RID();
  }

  struct Vertex2D {
    float x, y;
    float u, v;
  };

  float x1 = p_x + p_width;
  float y1 = p_y + p_height;

  Vertex2D vertices[4] = {
      {p_x, p_y, 0.0f, 0.0f},
      {x1, p_y, 1.0f, 0.0f},
      {x1, y1, 1.0f, 1.0f},
      {p_x, y1, 0.0f, 1.0f},
  };

  uint16 indices[6] = {0, 1, 2, 0, 2, 3};

  RID rid = MeshCreate(p_name);
  uint32 surf_idx =
      MeshAddSurface(rid, vertices, 4, kVertex2DStride, indices, 6,
                     IndexBufferFormat::kUint16, RenderPrimitive::kTriangles);

  if (surf_idx == kInvalidSurfaceIndex) {
    MeshFree(rid);
    return RID();
  }

  SurfaceInfo* surf = mesh_owner_.GetOrNull(rid)->surfaces[surf_idx];
  surf->aabb = AABB(p_x, p_y, 0.0f, p_width, p_height, 0.0f);

  MeshInfo* info = mesh_owner_.GetOrNull(rid);
  info->aabb = AABB(p_x, p_y, 0.0f, p_width, p_height, 0.0f);

  return rid;
}

/**
 * @brief 创建 2D 线段网格（按厚度膨胀为四边形）
 *
 * 计算线段方向 (dx,dy) 与单位长度，法线 (nx,ny)=(-dy,dx)/len，
 * 沿法线方向各偏移半厚度（p_thickness*0.5f）构造 4 顶点带。
 * UV 沿线段方向为 [0,1]（v 区分两侧 0/1）。
 * AABB 以两端点坐标 ± 半厚度外扩。
 *
 * @param[in] p_name      网格名称（用于调试/日志，可空）
 * @param[in] p_x0,p_y0  线段起点
 * @param[in] p_x1,p_y1  线段终点
 * @param[in] p_thickness 线宽（必须 > 0）
 *
 * @return 网格 RID；厚度非法、零长线段或 MeshAddSurface 失败时
 *         返回空 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 *
 * @note 零长线段（长度 < 1e-6）无法确定法线方向，直接拒绝
 */
RID MeshStorage::CreateLine(const char* p_name, float p_x0, float p_y0,
                            float p_x1, float p_y1, float p_thickness) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  if (p_thickness <= 0.0f) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreateLine() invalid thickness for '%s'",
                    p_name ? p_name : "(null)");
    return RID();
  }

  struct Vertex2D {
    float x, y;
    float u, v;
  };

  float dx = p_x1 - p_x0;
  float dy = p_y1 - p_y0;
  float length = std::sqrt(dx * dx + dy * dy);

  if (length < 1e-6f) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreateLine() zero-length line for '%s'",
                    p_name ? p_name : "(null)");
    return RID();
  }

  float nx = -dy / length;
  float ny = dx / length;
  float half_t = p_thickness * 0.5f;

  float offset_x = nx * half_t;
  float offset_y = ny * half_t;

  Vertex2D vertices[4] = {
      {p_x0 + offset_x, p_y0 + offset_y, 0.0f, 0.0f},
      {p_x1 + offset_x, p_y1 + offset_y, 1.0f, 0.0f},
      {p_x1 - offset_x, p_y1 - offset_y, 1.0f, 1.0f},
      {p_x0 - offset_x, p_y0 - offset_y, 0.0f, 1.0f},
  };

  uint16 indices[6] = {0, 1, 2, 0, 2, 3};

  RID rid = MeshCreate(p_name);
  uint32 surf_idx =
      MeshAddSurface(rid, vertices, 4, kVertex2DStride, indices, 6,
                     IndexBufferFormat::kUint16, RenderPrimitive::kTriangles);

  if (surf_idx == kInvalidSurfaceIndex) {
    MeshFree(rid);
    return RID();
  }

  SurfaceInfo* surf = mesh_owner_.GetOrNull(rid)->surfaces[surf_idx];
  float min_x = std::fmin(p_x0, p_x1) - half_t;
  float min_y = std::fmin(p_y0, p_y1) - half_t;
  float max_x = std::fmax(p_x0, p_x1) + half_t;
  float max_y = std::fmax(p_y0, p_y1) + half_t;
  surf->aabb = AABB(min_x, min_y, 0.0f, max_x - min_x, max_y - min_y, 0.0f);

  MeshInfo* info = mesh_owner_.GetOrNull(rid);
  info->aabb = AABB(min_x, min_y, 0.0f, max_x - min_x, max_y - min_y, 0.0f);

  return rid;
}

// ═══════════════════════════════════════════════════════════════════════
// 3D 图元工厂（参考 Godot BoxMesh/PlaneMesh/SphereMesh/CylinderMesh）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建立方体网格（6 面独立法线）
 *
 * 生成 24 顶点（位置 XYZ + 法线 + UV，kVertex3DStride=32B）——
 * 每个面 4 顶点带各自面法线（±X/±Y/±Z），36 索引
 * （每面 2 三角形）。中心位于原点，尺寸由 w/h/d 决定。
 * AABB 为 [-hw,-hh,-hd] × [hw,hh,hd]。
 *
 * @param[in] p_name   网格名称（用于调试/日志，可空）
 * @param[in] p_width  宽（X 方向，必须 > 0）
 * @param[in] p_height 高（Y 方向，必须 > 0）
 * @param[in] p_depth  深（Z 方向，必须 > 0）
 *
 * @return 网格 RID；尺寸非法或 MeshAddSurface 失败时返回空 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 *
 * @note 6 面分离顶点使法线硬边（无平滑着色），适合
 *       HUD 框体等需要棱角分明的 3D 图元
 */
RID MeshStorage::CreateBox(const char* p_name, float p_width, float p_height,
                           float p_depth) {
  RID rid = MeshAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  MeshCreateBoxParams params;
  params.width = p_width;
  params.height = p_height;
  params.depth = p_depth;

  if (MeshInitializeBox(rid, params, reinterpret_cast<const uint8*>(p_name)) !=
      Error::kOK) {
    MeshFree(rid);
    return RID();
  }
  return rid;
}

/**
 * @brief 创建 XZ 平面网格（Y=0，法线朝上）
 *
 * 生成 4 顶点（法线恒为 +Y）+ 6 索引的双三角形平面，
 * 顶点分布在 XZ 平面 [-hw,hd]×[hw,-hd] 区域。UV 覆盖
 * [0,1]×[0,1]。AABB 高度为 0（扁平包围盒）。
 *
 * @param[in] p_name  网格名称（用于调试/日志，可空）
 * @param[in] p_width X 方向宽度（必须 > 0）
 * @param[in] p_depth Z 方向深度（必须 > 0）
 *
 * @return 网格 RID；尺寸非法或 MeshAddSurface 失败时返回空 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 */
RID MeshStorage::CreatePlane(const char* p_name, float p_width, float p_depth) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  if (p_width <= 0.0f || p_depth <= 0.0f) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreatePlane() invalid dimensions for '%s'",
                    p_name ? p_name : "(null)");
    return RID();
  }

  struct Vertex3D {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
  };

  float hw = p_width * 0.5f;
  float hd = p_depth * 0.5f;

  Vertex3D vertices[4] = {
      {hw, 0.0f, -hd, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
      {-hw, 0.0f, -hd, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
      {-hw, 0.0f, hd, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
      {hw, 0.0f, hd, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f},
  };

  uint16 indices[6] = {0, 1, 2, 0, 2, 3};

  RID rid = MeshCreate(p_name);
  uint32 surf_idx =
      MeshAddSurface(rid, vertices, 4, kVertex3DStride, indices, 6,
                     IndexBufferFormat::kUint16, RenderPrimitive::kTriangles);

  if (surf_idx == kInvalidSurfaceIndex) {
    MeshFree(rid);
    return RID();
  }

  AABB plane_aabb(-hw, 0.0f, -hd, p_width, 0.0f, p_depth);
  mesh_owner_.GetOrNull(rid)->surfaces[surf_idx]->aabb = plane_aabb;
  mesh_owner_.GetOrNull(rid)->aabb = plane_aabb;

  return rid;
}

/**
 * @brief 创建经纬球网格（顶点复用平滑着色）
 *
 * 参数化生成 (rings+1)×(segments+1) 顶点网格：经度角 theta
 * （绕 Y 轴 0~2π）与纬度角 phi（0~π，极点方向为 +Y）。
 * 顶点位置 = 法线 × 半径（法线即归一化方向，天然平滑着色），
 * UV 为 (u,v)∈[0,1]²。索引为 (rings×segments×6) 个三角形。
 * 顶点/索引数组由 ARHUD_NEW_ARR 分配，MeshAddSurface 深拷贝后
 * 立即释放。AABB 为半径 2 倍的立方体。
 *
 * @param[in] p_name           网格名称（用于调试/日志，可空）
 * @param[in] p_radius         球半径（必须 > 0）
 * @param[in] p_radial_segments 经度分段数（必须 ≥ 3）
 * @param[in] p_rings          纬度环数（必须 ≥ 1）
 *
 * @return 网格 RID；参数非法或 MeshAddSurface 失败时返回空 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 */
RID MeshStorage::CreateSphere(const char* p_name, float p_radius,
                              uint32 p_radial_segments, uint32 p_rings) {
  RID rid = MeshAllocate();
  if (!rid.IsValid()) {
    return RID();
  }

  MeshCreateSphereParams params;
  params.radius = p_radius;
  params.radial_segments = p_radial_segments;
  params.rings = p_rings;

  if (MeshInitializeSphere(
          rid, params, reinterpret_cast<const uint8*>(p_name)) != Error::kOK) {
    MeshFree(rid);
    return RID();
  }
  return rid;
}

/**
 * @brief 创建圆台/圆锥网格（侧面 + 底面盖 + 可选顶盖）
 *
 * 三部分顶点构造：
 *  - 侧面：沿圆周上下两层顶点，法线为侧面真实朝向
 *    （含斜率分量 (bottom-top)/height，归一化后使光照正确），
 *    UV 沿圆周/高度方向铺展；
 *  - 底面盖：中心点 + 圆周顶点扇形（法线 -Y），UV 投影圆盘；
 *  - 顶盖：p_top_radius > 0 时生成（法线 +Y），否则不生成
 *    （此时即为圆锥）。
 * p_top_radius 允许为 0，p_bottom_radius 必须 > 0。
 * AABB 以 max(top,bottom) 为半径、高度为高的圆柱包围盒。
 *
 * @param[in] p_name           网格名称（用于调试/日志，可空）
 * @param[in] p_top_radius     顶面半径（≥ 0，0 = 圆锥）
 * @param[in] p_bottom_radius  底面半径（必须 > 0）
 * @param[in] p_height         高度（必须 > 0）
 * @param[in] p_radial_segments 圆周分段数（必须 ≥ 3）
 *
 * @return 网格 RID；参数非法或 MeshAddSurface 失败时返回空 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 */
RID MeshStorage::CreateCylinder(const char* p_name, float p_top_radius,
                                float p_bottom_radius, float p_height,
                                uint32 p_radial_segments) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  if (p_height <= 0.0f) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreateCylinder() invalid height for '%s'",
                    p_name ? p_name : "(null)");
    return RID();
  }

  if (p_top_radius < 0.0f || p_bottom_radius <= 0.0f) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreateCylinder() invalid radii for '%s'",
                    p_name ? p_name : "(null)");
    return RID();
  }

  if (p_radial_segments < 3) {
    ARHUD_LOG_ERROR(
        "kFailed", "MeshStorage::CreateCylinder() radial_segments < 3 for '%s'",
        p_name ? p_name : "(null)");
    return RID();
  }

  struct Vertex3D {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
  };

  float hh = p_height * 0.5f;
  float max_radius = MathFuncs::Max(p_top_radius, p_bottom_radius);

  uint32 side_verts = (p_radial_segments + 1) * 2;
  uint32 side_indices = p_radial_segments * 6;
  uint32 cap_verts = p_radial_segments + 1;
  uint32 cap_indices = p_radial_segments * 3;

  bool has_top = p_top_radius > 0.0f;
  uint32 vertex_count = side_verts + cap_verts + (has_top ? cap_verts : 0);
  uint32 index_count = side_indices + cap_indices + (has_top ? cap_indices : 0);

  auto* vertices = ARHUD_NEW_ARR(Vertex3D, vertex_count);
  auto* indices = ARHUD_NEW_ARR(uint16, index_count);

  uint32 vi = 0;
  uint32 ii = 0;

  for (uint32 s = 0; s <= p_radial_segments; ++s) {
    float u = static_cast<float>(s) / static_cast<float>(p_radial_segments);
    float theta = u * 2.0f * MathFuncs::kPi;

    float sin_theta = MathFuncs::Sin(theta);
    float cos_theta = MathFuncs::Cos(theta);

    float nx = cos_theta;
    float nz = sin_theta;
    float ny = (p_bottom_radius - p_top_radius) / p_height;

    float nl = MathFuncs::Sqrt(nx * nx + ny * ny + nz * nz);
    if (nl > 0.0f) {
      nx /= nl;
      ny /= nl;
      nz /= nl;
    }

    vertices[vi].x = cos_theta * p_bottom_radius;
    vertices[vi].y = -hh;
    vertices[vi].z = sin_theta * p_bottom_radius;
    vertices[vi].nx = nx;
    vertices[vi].ny = ny;
    vertices[vi].nz = nz;
    vertices[vi].u = u;
    vertices[vi].v = 1.0f;
    ++vi;

    vertices[vi].x = cos_theta * p_top_radius;
    vertices[vi].y = hh;
    vertices[vi].z = sin_theta * p_top_radius;
    vertices[vi].nx = nx;
    vertices[vi].ny = ny;
    vertices[vi].nz = nz;
    vertices[vi].u = u;
    vertices[vi].v = 0.0f;
    ++vi;
  }

  for (uint32 s = 0; s < p_radial_segments; ++s) {
    uint32 a = s * 2;
    uint32 b = a + 1;
    uint32 c = a + 2;
    uint32 d = a + 3;

    indices[ii++] = static_cast<uint16>(a);
    indices[ii++] = static_cast<uint16>(c);
    indices[ii++] = static_cast<uint16>(b);

    indices[ii++] = static_cast<uint16>(b);
    indices[ii++] = static_cast<uint16>(c);
    indices[ii++] = static_cast<uint16>(d);
  }

  uint32 bottom_center = vi;
  vertices[vi].x = 0.0f;
  vertices[vi].y = -hh;
  vertices[vi].z = 0.0f;
  vertices[vi].nx = 0.0f;
  vertices[vi].ny = -1.0f;
  vertices[vi].nz = 0.0f;
  vertices[vi].u = 0.5f;
  vertices[vi].v = 0.5f;
  ++vi;

  for (uint32 s = 0; s <= p_radial_segments; ++s) {
    float u = static_cast<float>(s) / static_cast<float>(p_radial_segments);
    float theta = u * 2.0f * MathFuncs::kPi;

    vertices[vi].x = MathFuncs::Cos(theta) * p_bottom_radius;
    vertices[vi].y = -hh;
    vertices[vi].z = MathFuncs::Sin(theta) * p_bottom_radius;
    vertices[vi].nx = 0.0f;
    vertices[vi].ny = -1.0f;
    vertices[vi].nz = 0.0f;
    vertices[vi].u = MathFuncs::Cos(theta) * 0.5f + 0.5f;
    vertices[vi].v = MathFuncs::Sin(theta) * 0.5f + 0.5f;
    ++vi;
  }

  for (uint32 s = 0; s < p_radial_segments; ++s) {
    indices[ii++] = static_cast<uint16>(bottom_center);
    indices[ii++] = static_cast<uint16>(bottom_center + s + 2);
    indices[ii++] = static_cast<uint16>(bottom_center + s + 1);
  }

  if (has_top) {
    uint32 top_center = vi;
    vertices[vi].x = 0.0f;
    vertices[vi].y = hh;
    vertices[vi].z = 0.0f;
    vertices[vi].nx = 0.0f;
    vertices[vi].ny = 1.0f;
    vertices[vi].nz = 0.0f;
    vertices[vi].u = 0.5f;
    vertices[vi].v = 0.5f;
    ++vi;

    for (uint32 s = 0; s <= p_radial_segments; ++s) {
      float u = static_cast<float>(s) / static_cast<float>(p_radial_segments);
      float theta = u * 2.0f * MathFuncs::kPi;

      vertices[vi].x = MathFuncs::Cos(theta) * p_top_radius;
      vertices[vi].y = hh;
      vertices[vi].z = MathFuncs::Sin(theta) * p_top_radius;
      vertices[vi].nx = 0.0f;
      vertices[vi].ny = 1.0f;
      vertices[vi].nz = 0.0f;
      vertices[vi].u = MathFuncs::Cos(theta) * 0.5f + 0.5f;
      vertices[vi].v = MathFuncs::Sin(theta) * 0.5f + 0.5f;
      ++vi;
    }

    for (uint32 s = 0; s < p_radial_segments; ++s) {
      indices[ii++] = static_cast<uint16>(top_center);
      indices[ii++] = static_cast<uint16>(top_center + s + 1);
      indices[ii++] = static_cast<uint16>(top_center + s + 2);
    }
  }

  RID rid = MeshCreate(p_name);
  uint32 surf_idx = MeshAddSurface(
      rid, vertices, vertex_count, kVertex3DStride, indices, index_count,
      IndexBufferFormat::kUint16, RenderPrimitive::kTriangles);

  ARHUD_DELETE_ARR(vertices, Vertex3D);
  ARHUD_DELETE_ARR(indices, uint16);

  if (surf_idx == kInvalidSurfaceIndex) {
    MeshFree(rid);
    return RID();
  }

  AABB cyl_aabb(-max_radius, -hh, -max_radius, 2.0f * max_radius, p_height,
                2.0f * max_radius);
  mesh_owner_.GetOrNull(rid)->surfaces[surf_idx]->aabb = cyl_aabb;
  mesh_owner_.GetOrNull(rid)->aabb = cyl_aabb;

  return rid;
}

// ═══════════════════════════════════════════════════════════════════════
// 3D 牵引线（Fillet Arc 圆角折线带）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建 3D 牵引线网格（圆角折线带，Fillet Arc）
 *
 * 将折线路径膨胀为带状网格，拐角处插入圆角圆弧：
 *  1. 预处理各线段方向/长度（SegInfo），零长段兜底为 +X 方向；
 *  2. 生成 PathPoint 序列（位置 + 切线）：拐角夹角 alpha ∈
 *     (0.01, π-0.01) 且 p_corner_radius > 0 时插入圆角弧——
 *     切点 T1/T2 沿入/出方向回退 d = r·cos(α/2)/sin(α/2)，
 *     弧心 = 角点 + (r/sin(α/2))·角平分线，用 Rodrigues 轴角
 *     旋转（轴 = start_dir × end_dir，扫角 sweep = π - α）
 *     插值 p_corner_segments 段；夹角过小/过大（≈直线/折返）
 *     或圆角半径受限时退化为直接拐点；
 *  3. 沿各点切线叉乘上方向求右向量，左右各偏移半宽度生成
 *     双行顶点（UV.u 为累计弧长归一化，v 区分两侧）；
 *  4. 条带索引（每段 2 三角形），AABB 由全部顶点 ExpandTo 计算。
 *
 * @param[in] p_name           网格名称（用于调试/日志，可空）
 * @param[in] p_points         折线控制点数组
 * @param[in] p_point_count    控制点数量（必须 ≥ 2）
 * @param[in] p_width          带宽（必须 > 0）
 * @param[in] p_corner_radius  拐角圆角半径（0 关闭圆角，必须 ≥ 0）
 * @param[in] p_corner_segments 圆角弧分段数（必须 ≥ 1）
 *
 * @return 网格 RID；参数非法、退化路径或 MeshAddSurface 失败时
 *         返回空 RID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 *
 * @note 圆角半径自动钳制为不超过相邻线段可容纳的最大值
 *       （min(prev_len,next_len)·tan(α/2)），防止弧超出线段范围
 */
RID MeshStorage::CreateRibbon3D(const char* p_name, const Vec3* p_points,
                                uint32 p_point_count, float p_width,
                                float p_corner_radius,
                                uint32 p_corner_segments) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  if (rd_ == nullptr) {
    ARHUD_LOG_ERROR("kFailed", "MeshStorage::CreateRibbon3D() not initialized");
    return RID();
  }

  if (p_points == nullptr || p_point_count < 2 || p_width <= 0.0f) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreateRibbon3D() invalid parameters for '%s'",
                    p_name ? p_name : "(null)");
    return RID();
  }

  if (p_corner_radius < 0.0f || p_corner_segments < 1) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MeshStorage::CreateRibbon3D() invalid corner params for '%s'",
        p_name ? p_name : "(null)");
    return RID();
  }

  struct PathPoint {
    Vec3 position;
    Vec3 tangent;
  };

  struct SegInfo {
    Vec3 dir;
    float len;
  };

  LocalVector<SegInfo> segs;
  segs.Reserve(p_point_count - 1);
  for (uint32 i = 0; i < p_point_count - 1; ++i) {
    Vec3 diff = p_points[i + 1] - p_points[i];
    float len = diff.Length();
    segs.PushBack(
        {len > 1e-6f ? diff * (1.0f / len) : Vec3(1.0f, 0.0f, 0.0f), len});
  }

  LocalVector<PathPoint> path;
  path.Reserve(p_point_count * (1 + p_corner_segments));

  Vec3 first_dir = segs[0].dir;
  path.PushBack({p_points[0], first_dir});

  for (uint32 i = 1; i < p_point_count - 1; ++i) {
    Vec3 dir_in = segs[i - 1].dir;
    Vec3 dir_out = segs[i].dir;
    float dist_prev = segs[i - 1].len;
    float dist_next = segs[i].len;

    if (dist_prev < 1e-6f || dist_next < 1e-6f) {
      Vec3 avg = dir_in + dir_out;
      path.PushBack(
          {p_points[i], avg.Length() > 1e-6f ? avg.Normalized() : dir_in});
      continue;
    }

    float cos_alpha = dir_in.Dot(dir_out);
    if (cos_alpha > 1.0f)
      cos_alpha = 1.0f;
    if (cos_alpha < -1.0f)
      cos_alpha = -1.0f;
    float alpha = MathFuncs::Acos(cos_alpha);

    bool need_round = p_corner_radius > 0.0f && alpha > 0.01f &&
                      alpha < MathFuncs::kPi - 0.01f;

    if (!need_round) {
      Vec3 avg = dir_in + dir_out;
      path.PushBack(
          {p_points[i], avg.Length() > 1e-6f ? avg.Normalized() : dir_in});
      continue;
    }

    float half_alpha = alpha * 0.5f;
    float sin_ha = MathFuncs::Sin(half_alpha);
    float cos_ha = MathFuncs::Cos(half_alpha);
    float tan_ha = sin_ha > 1e-6f ? cos_ha / sin_ha : 0.0f;

    float max_r = MathFuncs::Min(dist_prev, dist_next) * tan_ha;
    float r = MathFuncs::Min(p_corner_radius, max_r);
    float d = r * cos_ha / sin_ha;

    Vec3 T1 = p_points[i] - dir_in * d;
    Vec3 T2 = p_points[i] + dir_out * d;

    path.PushBack({T1, dir_in});

    Vec3 bisector = (-dir_in + dir_out);
    float bl = bisector.Length();
    if (bl > 1e-6f) {
      bisector = bisector * (1.0f / bl);
    } else {
      path.PushBack({p_points[i], (dir_in + dir_out).Normalized()});
      continue;
    }

    Vec3 arc_center = p_points[i] + (r / sin_ha) * bisector;

    Vec3 start_dir = (T1 - arc_center);
    float sd_len = start_dir.Length();
    if (sd_len > 1e-6f) {
      start_dir = start_dir * (1.0f / sd_len);
    }

    Vec3 end_dir = (T2 - arc_center);
    float ed_len = end_dir.Length();
    if (ed_len > 1e-6f) {
      end_dir = end_dir * (1.0f / ed_len);
    }

    Vec3 rot_axis = start_dir.Cross(end_dir);
    float ra_len = rot_axis.Length();
    if (ra_len < 1e-6f) {
      path.PushBack({p_points[i], (dir_in + dir_out).Normalized()});
      continue;
    }
    rot_axis = rot_axis * (1.0f / ra_len);

    float sweep = MathFuncs::kPi - alpha;

    for (uint32 j = 1; j <= p_corner_segments; ++j) {
      float t = static_cast<float>(j) / static_cast<float>(p_corner_segments);
      float angle = t * sweep;
      float cos_a = MathFuncs::Cos(angle);
      float sin_a = MathFuncs::Sin(angle);

      Vec3 rotated = start_dir * cos_a + rot_axis.Cross(start_dir) * sin_a +
                     rot_axis * (rot_axis.Dot(start_dir)) * (1.0f - cos_a);

      Vec3 pos = arc_center + r * rotated;
      Vec3 tangent = rot_axis.Cross(rotated);
      float tl = tangent.Length();
      if (tl > 1e-6f) {
        tangent = tangent * (1.0f / tl);
      }

      path.PushBack({pos, tangent});
    }
  }

  Vec3 last_dir = segs[p_point_count - 2].dir;
  path.PushBack({p_points[p_point_count - 1], last_dir});

  if (path.Size() < 2) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreateRibbon3D() degenerate path for '%s'",
                    p_name ? p_name : "(null)");
    return RID();
  }

  struct Vertex3D {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
  };

  uint32 vertex_count = path.Size() * 2;
  uint32 index_count = (path.Size() - 1) * 6;

  auto* vertices = ARHUD_NEW_ARR(Vertex3D, vertex_count);
  auto* indices = ARHUD_NEW_ARR(uint16, index_count);

  float total_dist = 0.0f;
  for (uint32 i = 1; i < path.Size(); ++i) {
    total_dist += (path[i].position - path[i - 1].position).Length();
  }

  float inv_total = total_dist > 1e-6f ? 1.0f / total_dist : 0.0f;
  float half_w = p_width * 0.5f;
  float cum_dist = 0.0f;

  for (uint32 i = 0; i < path.Size(); ++i) {
    if (i > 0) {
      cum_dist += (path[i].position - path[i - 1].position).Length();
    }

    Vec3 tangent = path[i].tangent;
    Vec3 up(0.0f, 1.0f, 0.0f);
    if (MathFuncs::Abs(tangent.Dot(up)) > 0.99f) {
      up = Vec3(1.0f, 0.0f, 0.0f);
    }
    Vec3 right = tangent.Cross(up).Normalized();

    float u_coord = cum_dist * inv_total;

    Vertex3D& v_left = vertices[i * 2];
    Vec3 pos_left = path[i].position - right * half_w;
    v_left.x = pos_left.x;
    v_left.y = pos_left.y;
    v_left.z = pos_left.z;
    v_left.nx = 0.0f;
    v_left.ny = 1.0f;
    v_left.nz = 0.0f;
    v_left.u = u_coord;
    v_left.v = 0.0f;

    Vertex3D& v_right = vertices[i * 2 + 1];
    Vec3 pos_right = path[i].position + right * half_w;
    v_right.x = pos_right.x;
    v_right.y = pos_right.y;
    v_right.z = pos_right.z;
    v_right.nx = 0.0f;
    v_right.ny = 1.0f;
    v_right.nz = 0.0f;
    v_right.u = u_coord;
    v_right.v = 1.0f;
  }

  uint32 ii = 0;
  for (uint32 i = 0; i < path.Size() - 1; ++i) {
    uint32 bl = i * 2;
    uint32 br = i * 2 + 1;
    uint32 tl = (i + 1) * 2;
    uint32 tr = (i + 1) * 2 + 1;

    indices[ii++] = static_cast<uint16>(bl);
    indices[ii++] = static_cast<uint16>(tl);
    indices[ii++] = static_cast<uint16>(br);

    indices[ii++] = static_cast<uint16>(br);
    indices[ii++] = static_cast<uint16>(tl);
    indices[ii++] = static_cast<uint16>(tr);
  }

  RID rid = MeshCreate(p_name);
  uint32 surf_idx = MeshAddSurface(
      rid, vertices, vertex_count, kVertex3DStride, indices, index_count,
      IndexBufferFormat::kUint16, RenderPrimitive::kTriangles);

  AABB ribbon_aabb;
  if (vertex_count > 0) {
    ribbon_aabb.position = Vec3(vertices[0].x, vertices[0].y, vertices[0].z);
    ribbon_aabb.size = Vec3(0.0f, 0.0f, 0.0f);
    for (uint32 i = 1; i < vertex_count; ++i) {
      ribbon_aabb.ExpandTo(Vec3(vertices[i].x, vertices[i].y, vertices[i].z));
    }
  }

  ARHUD_DELETE_ARR(vertices, Vertex3D);
  ARHUD_DELETE_ARR(indices, uint16);

  if (surf_idx == kInvalidSurfaceIndex) {
    MeshFree(rid);
    return RID();
  }

  mesh_owner_.GetOrNull(rid)->surfaces[surf_idx]->aabb = ribbon_aabb;
  mesh_owner_.GetOrNull(rid)->aabb = ribbon_aabb;

  return rid;
}

// ═══════════════════════════════════════════════════════════════════════
// 查询方法
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 获取网格信息结构（本地簿记查询）
 *
 * @param[in] p_mesh 网格 RID
 *
 * @return MeshInfo 指针（surfaces 列表、AABB、名称）；RID 无效
 *         或已失效时返回 nullptr
 */
const MeshInfo* MeshStorage::MeshGetInfo(RID p_mesh) const {
  const MeshInfo* info = mesh_owner_.GetOrNull(p_mesh);
  if (info == nullptr || !info->is_valid) {
    return nullptr;
  }
  return info;
}

/**
 * @brief 获取网格的 Surface 数量（本地簿记查询）
 *
 * @param[in] p_mesh 网格 RID
 *
 * @return Surface 数量；RID 无效或已失效时返回 0
 */
uint32 MeshStorage::MeshGetSurfaceCount(RID p_mesh) const {
  const MeshInfo* info = mesh_owner_.GetOrNull(p_mesh);
  if (info == nullptr || !info->is_valid) {
    return 0;
  }
  return info->surfaces.Size();
}

/**
 * @brief 获取指定索引的 Surface（本地簿记查询）
 *
 * @param[in] p_mesh  网格 RID
 * @param[in] p_index Surface 索引
 *
 * @return SurfaceInfo 指针（VBO/IBO 句柄、CPU 数据副本、AABB 等）；
 *         RID 无效、已失效或索引越界时返回 nullptr
 */
const SurfaceInfo* MeshStorage::MeshGetSurface(RID p_mesh,
                                               uint32 p_index) const {
  const MeshInfo* info = mesh_owner_.GetOrNull(p_mesh);
  if (info == nullptr || !info->is_valid) {
    return nullptr;
  }
  if (p_index >= info->surfaces.Size()) {
    return nullptr;
  }
  return info->surfaces[p_index];
}

/**
 * @brief 检查网格是否有效（本地簿记查询）
 *
 * @param[in] p_mesh 网格 RID
 *
 * @return 网格存在且未释放返回 true，否则返回 false
 */
bool MeshStorage::MeshIsValid(RID p_mesh) const {
  const MeshInfo* info = mesh_owner_.GetOrNull(p_mesh);
  return info != nullptr && info->is_valid;
}

// ═══════════════════════════════════════════════════════════════════════
// 实例缓冲区
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 创建实例化数据缓冲区（GPU VBO）
 *
 * 创建 GPU 缓冲（kVertex | kTransferTo）并一次性上传
 * p_instance_count × p_instance_stride 字节实例数据。
 * 供实例化绘制（instanced draw）使用。
 *
 * @param[in] p_instance_data  实例数据指针（CPU 内存）
 * @param[in] p_instance_count 实例数量
 * @param[in] p_instance_stride 单实例字节步幅
 *
 * @return RDBufferID；未初始化、参数非法或创建/上传失败时返回空 ID
 *
 * @pre 必须在渲染线程上调用（ARHUD_ASSERT_ON_RENDER_THREAD 校验）
 *
 * @note 与 MeshCreate 不同，本方法返回裸 RD 缓冲句柄，不经过
 *       mesh_owner_ 簿记——生命周期由调用方管理（自行
 *       rd_->BufferFree），不会随 MeshStorage::Finalize 释放
 */
RDBufferID MeshStorage::CreateInstanceBuffer(const void* p_instance_data,
                                             uint32 p_instance_count,
                                             uint32 p_instance_stride) {
  ARHUD_ASSERT_ON_RENDER_THREAD(rd_);

  if (rd_ == nullptr) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreateInstanceBuffer() not initialized");
    return RDBufferID();
  }

  if (p_instance_data == nullptr || p_instance_count == 0 ||
      p_instance_stride == 0) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreateInstanceBuffer() invalid parameters");
    return RDBufferID();
  }

  uint32 data_size = p_instance_count * p_instance_stride;

  RDBufferID buffer_id = rd_->BufferCreate(
      data_size,
      BitField<BufferUsageBits>(BufferUsageBits::kVertex) |
          BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
      MemoryAllocationType::kGpu);

  if (!buffer_id.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "MeshStorage::CreateInstanceBuffer() buffer creation failed");
    return RDBufferID();
  }

  Error err = rd_->BufferUpdate(buffer_id, 0, data_size, p_instance_data);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "MeshStorage::CreateInstanceBuffer() buffer upload failed");
    rd_->BufferFree(buffer_id);
    return RDBufferID();
  }

  return buffer_id;
}

}  // namespace arhud
