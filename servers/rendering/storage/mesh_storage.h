/**
 * @file mesh_storage.h
 * @brief Mesh resource storage with Mesh/Surface separation
 *
 * 参考 Godot 4.6 GLES3 MeshStorage 的 Mesh/Surface 分层设计，
 * 适配 ARHud 的 RD 抽象层，预留 3D 多 Surface 扩展接口。
 *
 * @par 核心分层（参考 Godot Mesh + Surface）：
 *   MeshInfo（容器）
 *     ├── SurfaceInfo[]（每个 Surface = VBO + IBO + 顶点格式 + 材质）
 *     │     ├── vertex_buffer_id / index_buffer_id  ← GPU 缓冲区
 *     │     ├── vertex_count / index_count / stride  ← 几何元数据
 *     │     ├── material_rid                         ← 材质绑定
 *     │     └── aabb                                 ← 包围盒
 *     ├── blend_shape / skeleton 信息（未来 3D 扩展）
 *     └── 全局 AABB（所有 Surface 的并集）
 *
 * @par 为什么需要 Mesh / Surface 分离：
 *   3D 模型通常有多个材质区域（如车身 + 车窗 + 轮胎），每个区域是独立 Surface：
 *   - 每个 Surface 有独立的 VBO/IBO/顶点格式/材质
 *   - 绘制时逐 Surface 绑定不同的材质和顶点格式
 *   - 2D 图元（Quad/Line/Rect）退化为单 Surface 的 MeshInfo
 *
 * @par 与 Godot Mesh/Surface 的对比：
 *   | 特性 | Godot GLES3 | ARHud RD |
 *   |------|-------------|----------|
 *   | Mesh 容器 | Mesh struct | MeshInfo struct |
 *   | Surface | Mesh::Surface | SurfaceInfo struct |
 *   | VBO/IBO | GLuint | RDBufferID |
 *   | VAO 缓存 | Surface::Version | 不需要（RD DrawList 动态绑定） |
 *   | Blend Shape | Surface::BlendShape[] | 预留，Phase 3 不实现 |
 *   | LOD | Surface::LOD[] | 预留，Phase 3 不实现 |
 *   | Wireframe | Surface::Wireframe | 不需要 |
 *   | Skeleton | MeshInstance + Skeleton | 预留，Phase 3 不实现 |
 *   | MultiMesh | MultiMesh struct | 预留，Phase 3 不实现 |
 *   | 图元工厂 | PrimitiveMesh（Scene 层） | 内置
 * CreateQuad/Rect/Line/Box/Plane/Sphere/Cylinder |
 *
 * @par 数据分层（三层分离）：
 *   - MeshInfo / SurfaceInfo（本文件）：业务数据 + 几何元数据
 *   - RD 层：RDBufferID（VBO/IBO 的 RID 管理和延迟销毁）
 *   - RDD 层：GL VBO/IBO（GLuint，驱动特有）
 *
 * @par 2D 图元顶点格式（Canvas 层）：
 *   所有 2D 内置图元使用统一的顶点格式：
 *     - Position (vec2): x, y — 位置
 *     - UV (vec2): u, v — 纹理坐标
 *   步幅 = 4 × sizeof(float) = 16 字节（kVertex2DStride）
 *
 * @par 3D 图元顶点格式（Spatial 层）：
 *   所有 3D 内置图元使用统一的顶点格式：
 *     - Position (vec3): x, y, z — 位置
 *     - Normal (vec3): nx, ny, nz — 法线
 *     - UV (vec2): u, v — 纹理坐标
 *   步幅 = 8 × sizeof(float) = 32 字节（kVertex3DStride）
 *
 * @par 3D 图元坐标系：
 *   所有 3D 图元以原点为中心（参考 Godot PrimitiveMesh）：
 *   - Box: 中心在原点，轴对齐
 *   - Plane: XZ 平面，法线 Y+
 *   - Sphere: 中心在原点
 *   - Cylinder: 中心在原点，Y 轴为高度轴
 *
 * @par 架构层次：
 *   MeshStorage（本文件，网格资源存储）
 *     └── RenderingDevice          ← GPU 资源生命周期管理
 *           ├── RDBufferID         ← VBO/IBO
 *           └── DrawList           ← 绘制时绑定 VBO/IBO
 *
 * @see Godot 参考：drivers/gles3/storage/mesh_storage.h Mesh/Surface
 * @see arch_skill.md §2.8 Compositor/Storage/Renderer
 *
 * @author yameng.he
 * @version 2.0
 * @date 2026-05-25
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_STORAGE_MESH_STORAGE_H_
#define ARHUD_SERVERS_RENDERING_STORAGE_MESH_STORAGE_H_

#include "io/logger.h"
#include "math/math_funcs.h"
#include "os/memory.h"
#include "rendering_device.h"
#include "string/ustring.h"
#include "template/hash_map.h"
#include "template/local_vector.h"

namespace arhud {

using UString = String;

// ═══════════════════════════════════════════════════════════════════════
// SurfaceInfo — 单个 Surface 的数据
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 网格 Surface 数据（参考 Godot Mesh::Surface）
 *
 * 每个 Surface 代表一个可独立绘制的网格子集，拥有独立的
 * VBO/IBO/顶点格式/材质绑定。
 *
 * @par 与 Godot Mesh::Surface 的对应关系：
 *   | Godot | ARHud | 说明 |
 *   |-------|-------|------|
 *   | GLuint vertex_buffer | RDBufferID vertex_buffer_id | VBO |
 *   | GLuint index_buffer | RDBufferID index_buffer_id | IBO |
 *   | uint32 vertex_count | uint32 vertex_count | 顶点数 |
 *   | uint32 index_count | uint32 index_count | 索引数 |
 *   | RS::PrimitiveType primitive | RenderPrimitive primitive_type | 图元类型 |
 *   | uint64 format | uint32 vertex_stride | 顶点格式（简化为步幅） |
 *   | RID material | RID material_rid | 材质绑定 |
 *   | AABB aabb | AABB aabb | 3D 包围盒 |
 *   | Version *versions | — | VAO 缓存（RD 不需要） |
 *   | BlendShape *blend_shapes | — | 预留，Phase 3 不实现 |
 *   | LOD *lods | — | 预留，Phase 3 不实现 |
 */
struct SurfaceInfo {
  RDBufferID vertex_buffer_id;  ///< VBO（顶点缓冲区）
  RDBufferID index_buffer_id;   ///< IBO（索引缓冲区，可为空表示无索引绘制）

  uint32 vertex_count = 0;   ///< 顶点数量
  uint32 index_count = 0;    ///< 索引数量（0 = 无索引绘制）
  uint32 vertex_stride = 0;  ///< 顶点步幅（字节）
  IndexBufferFormat index_format = IndexBufferFormat::kUint16;   ///< 索引格式
  RenderPrimitive primitive_type = RenderPrimitive::kTriangles;  ///< 图元类型

  RID material_rid;  ///< 材质 RID（参考 Godot Surface::material）

  LocalVector<uint8> cpu_vertex_data;  ///< CPU 端顶点数据副本（用于合批读取）
  LocalVector<uint8> cpu_index_data;   ///< CPU 端索引数据副本（用于合批读取）

  AABB aabb;
};

// ═══════════════════════════════════════════════════════════════════════
// MeshInfo — 网格容器
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 网格容器（参考 Godot Mesh）
 *
 * MeshInfo 是 Surface 的容器，持有多个 SurfaceInfo。
 * 2D 图元退化为单 Surface；3D 模型可以有多个 Surface。
 *
 * @par 与 Godot Mesh 的对应关系：
 *   | Godot | ARHud | 说明 |
 *   |-------|-------|------|
 *   | Surface **surfaces | LocalVector<SurfaceInfo*> surfaces | Surface 数组 |
 *   | uint32 surface_count | surfaces.Size() | Surface 数量 |
 *   | AABB aabb | AABB aabb | 全局 3D 包围盒 |
 *   | uint32 blend_shape_count | — | 预留 |
 *   | bool has_bone_weights | — | 预留 |
 *   | List<MeshInstance*> instances | — | 预留 |
 */
struct MeshInfo {
  RID rid;       ///< 由 MeshStorage 分配的资源 ID
  UString name;  ///< 网格名称（用于调试/日志）

  LocalVector<SurfaceInfo*>
      surfaces;  ///< Surface 数组（参考 Godot Mesh::surfaces）

  AABB aabb;

  bool is_valid = false;  ///< 网格是否有效
};

// ═══════════════════════════════════════════════════════════════════════
// Create 参数包（RID 分裂初始化用；名称经内联 blob 传递）
// ═══════════════════════════════════════════════════════════════════════

struct MeshCreateQuadParams {
  float width = 0.0f;   ///< 宽度（> 0）
  float height = 0.0f;  ///< 高度（> 0）
};

struct MeshCreateBoxParams {
  float width = 0.0f;   ///< 宽（X，> 0）
  float height = 0.0f;  ///< 高（Y，> 0）
  float depth = 0.0f;   ///< 深（Z，> 0）
};

struct MeshCreateSphereParams {
  float radius = 0.0f;         ///< 半径（> 0）
  uint32 radial_segments = 3;  ///< 经度分段数（≥ 3）
  uint32 rings = 1;            ///< 纬度环数（≥ 1）
};

// ═══════════════════════════════════════════════════════════════════════
// MeshStorage
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 网格资源存储（参考 Godot GLES3::MeshStorage）
 *
 * 在 RenderingDevice 层之上管理网格的业务数据和 GPU 缓冲区。
 * 通过 RD 的公共 API 创建/更新/释放 VBO/IBO。
 *
 * @par 核心职责：
 *   1. **资源创建** — 创建 Mesh + Surface，分配 VBO/IBO，上传顶点/索引数据
 *   2. **图元工厂** — 预创建常用 2D 图元（Quad/Rect/Line）
 *   3. **资源查询** — 通过 RID 提供 O(1) 的 MeshInfo/SurfaceInfo 查找
 *   4. **资源释放** — 释放 MeshInfo/SurfaceInfo 并委托 RD 释放 GPU 端资源
 *
 * @par 线程安全约束（参考 Godot RenderingServer 线程模型）：
 *   所有涉及 GPU 操作的方法必须在渲染线程调用：
 *
 *   | 方法 | GPU 操作 | 线程约束 |
 *   |------|---------|---------|
 *   | Initialize/Finalize | RD::BufferCreate/Free | 🔴 渲染线程 |
 *   | MeshCreate | 无 GPU 操作 | 🟢 任意线程 |
 *   | MeshAddSurface | RD::BufferCreate/Update | 🔴 渲染线程 |
 *   | MeshFree | RD::BufferFree | 🔴 渲染线程 |
 *   | CreateQuad/Rect/Line | MeshCreate + MeshAddSurface | 🔴 渲染线程 |
 *   | CreateBox/Plane/Sphere/Cylinder | MeshCreate + MeshAddSurface | 🔴
 * 渲染线程 | | CreateRibbon3D | MeshCreate + MeshAddSurface | 🔴 渲染线程 | |
 * MeshGetInfo/Surface | 仅读 CPU 数据 | 🟢 任意线程 | | MeshIsValid | 仅读 CPU
 * 数据 | 🟢 任意线程 |
 *
 * @par 典型使用流程：
 *   @code
 *   MeshStorage storage;
 *   storage.Initialize(rd);
 *
 *   // 创建自定义网格（单 Surface）
 *   float vertices[] = { 0,0, 0,0,  1,0, 1,0,  1,1, 1,1,  0,1, 0,1 };
 *   uint16 indices[] = { 0,1,2, 0,2,3 };
 *   RID mesh = storage.MeshCreate("custom");
 *   storage.MeshAddSurface(mesh, vertices, 4, 16,
 *                          indices, 6, IndexBufferFormat::kUint16);
 *
 *   // 使用内置图元
 *   RID quad = storage.CreateQuad("bg_quad", 100, 50);
 *
 *   // 绘制
 *   const SurfaceInfo *surf = storage.MeshGetSurface(mesh, 0);
 *   rd.DrawListBindVertexBuffers(dl, &surf->vertex_buffer_id, &offset, 1);
 *   rd.DrawListBindIndexBuffer(dl, surf->index_buffer_id, surf->index_format,
 * 0); rd.DrawListDrawIndexed(dl, surf->index_count);
 *
 *   storage.MeshFree(mesh);
 *   storage.MeshFree(quad);
 *   storage.Finalize();
 *   @endcode
 *
 * @see RenderingDevice
 * @see MeshInfo
 * @see SurfaceInfo
 */
class MeshStorage {
 public:
  static constexpr uint32 kVertex2DStride = 4 * sizeof(float);
  static constexpr uint32 kVertex3DStride = 8 * sizeof(float);

  /**
   * @brief 初始化 MeshStorage 系统
   *
   * 初始化网格存储系统，建立与 RenderingDevice 的连接，
   * 准备网格资源管理的内部数据结构和状态。
   *
   * @details
   * 此方法必须在创建任何网格资源之前调用一次。它执行以下操作：
   * - 保存 RenderingDevice 指针用于后续 GPU 资源操作
   * - 初始化内部网格和 Surface 的 RID 分配器
   * - 准备网格资源管理的元数据容器
   * - 设置默认的顶点格式和渲染状态
   *
   * @param[in] p_rd 指向 RenderingDevice 的指针，用于 GPU 资源管理。
   *                 该指针必须在 MeshStorage 生命周期内保持有效。
   *                 不能为 nullptr。
   *
   * @return Error 初始化结果
   *         - OK: 初始化成功
   *         - ERR_CANT_CREATE: 无法创建内部数据结构
   *         - ERR_INVALID_PARAMETER: p_rd 为 nullptr
   *
   * @note
   * - 必须在调用 Finalize() 之前调用
   * - 重复调用会导致未定义行为
   * - 初始化失败时不应继续使用 MeshStorage 的其他方法
   *
   * @warning
   * 传入的 RenderingDevice 指针必须在 MeshStorage 整个生命周期内保持有效，
   * 在调用 Finalize() 之前不能被销毁。
   *
   * @see Finalize
   * @see RenderingDevice
   */
  Error Initialize(RenderingDevice* p_rd);

  /**
   * @brief 清理 MeshStorage 系统
   *
   * 释放所有网格资源，清理内部数据结构，断开与 RenderingDevice 的连接。
   *
   * @details
   * 此方法执行以下清理操作：
   * - 释放所有已创建的网格和 Surface 的 GPU 资源（VBO/IBO）
   * - 清空 MeshInfo 和 SurfaceInfo 的 RID 分配器
   * - 释放内部元数据容器的内存
   * - 清除与 RenderingDevice 的关联
   *
   * 调用后，MeshStorage 返回到未初始化状态，所有之前创建的网格 RID 都失效。
   *
   * @note
   * - 必须在 Initialize() 成功后才能调用
   * - 调用后不能再使用任何 MeshStorage 的方法（除了再次调用 Initialize）
   * - 所有之前创建的网格 RID 在调用后变为无效
   * - 可以安全地重复调用（幂等操作）
   *
   * @warning
   * - 调用前确保所有使用网格资源的渲染操作已完成
   * - 调用后不要继续使用任何已创建的网格 RID
   * - 此方法不会销毁传入的 RenderingDevice 对象，只清除对其的引用
   *
   * @par 典型使用流程：
   * @code
   * MeshStorage storage;
   * storage.Initialize(rendering_device);
   * // ... 使用网格资源 ...
   * storage.Finalize();  // 清理资源
   * @endcode
   *
   * @see Initialize
   * @see RenderingDevice
   */
  void Finalize();

  /**
   * @brief 创建空网格（参考 Godot mesh_create）
   *
   * 创建一个没有 Surface 的空 MeshInfo 容器。
   * 需要后续调用 MeshAddSurface 添加 Surface。
   *
   * @param[in] p_name 网格名称
   * @return 网格 RID，失败时返回空 RID
   */
  RID MeshCreate(const char* p_name);

  /**
   * @brief RID 分裂分配（对齐 §7.11 F / Godot FUNCRIDSPLIT）
   *
   * **任意线程**：仅分配线程安全的空壳 MeshInfo（is_valid=false），无 GPU
   * 操作。
   *
   * @return 空壳网格 RID
   */
  RID MeshAllocate();

  /**
   * @brief RID 分裂初始化：渲染线程为已分配的空壳 RID 构建 2D 四边形几何
   * @param[in] p_mesh      MeshAllocate() 返回的空壳 RID
   * @param[in] p_params    宽高
   * @param[in] p_name_blob 名称 blob 基址（含 NUL，可空）
   * @pre 必须在渲染线程上调用
   */
  Error MeshInitializeQuad(RID p_mesh, const MeshCreateQuadParams& p_params,
                           const uint8* p_name_blob);

  /**
   * @brief RID 分裂初始化：渲染线程为已分配的空壳 RID 构建立方体几何
   * @param[in] p_mesh      MeshAllocate() 返回的空壳 RID
   * @param[in] p_params    宽高深
   * @param[in] p_name_blob 名称 blob 基址（含 NUL，可空）
   * @pre 必须在渲染线程上调用
   */
  Error MeshInitializeBox(RID p_mesh, const MeshCreateBoxParams& p_params,
                          const uint8* p_name_blob);

  /**
   * @brief RID 分裂初始化：渲染线程为已分配的空壳 RID 构建经纬球几何
   * @param[in] p_mesh      MeshAllocate() 返回的空壳 RID
   * @param[in] p_params    半径 / 经度分段 / 纬度环
   * @param[in] p_name_blob 名称 blob 基址（含 NUL，可空）
   * @pre 必须在渲染线程上调用
   */
  Error MeshInitializeSphere(RID p_mesh, const MeshCreateSphereParams& p_params,
                             const uint8* p_name_blob);

  /**
   * @brief 向网格添加 Surface（参考 Godot mesh_add_surface）
   *
   * 创建指定顶点和索引数据的 Surface 并添加到 MeshInfo。
   * 通过 RD::BufferCreate 创建 VBO/IBO，通过 RD::BufferUpdate 上传数据。
   *
   * @param[in] p_mesh          网格 RID
   * @param[in] p_vertices      顶点数据指针
   * @param[in] p_vertex_count  顶点数量
   * @param[in] p_vertex_stride 顶点步幅（字节）
   * @param[in] p_indices       索引数据指针（可为 nullptr，表示无索引绘制）
   * @param[in] p_index_count   索引数量（0 表示无索引绘制）
   * @param[in] p_index_format  索引格式
   * @param[in] p_primitive_type 图元类型
   * @param[in] p_material_rid  材质 RID（可为空）
   *
   * @return Surface 索引，失败时返回 kInvalidSurfaceIndex
   */
  static constexpr uint32 kInvalidSurfaceIndex = UINT32_MAX;

  uint32 MeshAddSurface(
      RID p_mesh, const void* p_vertices, uint32 p_vertex_count,
      uint32 p_vertex_stride, const void* p_indices, uint32 p_index_count,
      IndexBufferFormat p_index_format = IndexBufferFormat::kUint16,
      RenderPrimitive p_primitive_type = RenderPrimitive::kTriangles,
      RID p_material_rid = RID());

  /**
   * @brief 释放网格资源
   *
   * 释放 MeshInfo 及其所有 SurfaceInfo，并委托 RD 释放 GPU 资源
   * （VBO/IBO，延迟销毁）。
   *
   * @param[in] p_mesh 网格 RID
   */
  void MeshFree(RID p_mesh);

  /**
   * @brief 创建 2D 四边形（原点在左上角，单 Surface）
   *
   * @param[in] p_name   网格名称
   * @param[in] p_width  宽度
   * @param[in] p_height 高度
   *
   * @return 网格 RID，失败时返回空 RID
   */
  RID CreateQuad(const char* p_name, float p_width, float p_height);

  /**
   * @brief 创建 2D 矩形（指定位置和尺寸，单 Surface）
   *
   * @param[in] p_name   网格名称
   * @param[in] p_x      左上角 X 坐标
   * @param[in] p_y      左上角 Y 坐标
   * @param[in] p_width  宽度
   * @param[in] p_height 高度
   *
   * @return 网格 RID，失败时返回空 RID
   */
  RID CreateRect(const char* p_name, float p_x, float p_y, float p_width,
                 float p_height);

  /**
   * @brief 创建 2D 线段（有厚度的四边形，单 Surface）
   *
   * @param[in] p_name      网格名称
   * @param[in] p_x0        起点 X
   * @param[in] p_y0        起点 Y
   * @param[in] p_x1        终点 X
   * @param[in] p_y1        终点 Y
   * @param[in] p_thickness 线段厚度（像素）
   *
   * @return 网格 RID，失败时返回空 RID
   */
  RID CreateLine(const char* p_name, float p_x0, float p_y0, float p_x1,
                 float p_y1, float p_thickness);

  /**
   * @brief 创建 3D 立方体（中心在原点，单 Surface）
   *
   * 参考 Godot BoxMesh。每个面有独立法线，硬边渲染。
   * 顶点格式：Position(vec3) + Normal(vec3) + UV(vec2) = 32 字节。
   *
   * @param[in] p_name   网格名称
   * @param[in] p_width  X 轴宽度
   * @param[in] p_height Y 轴高度
   * @param[in] p_depth  Z 轴深度
   *
   * @return 网格 RID，失败时返回空 RID
   */
  RID CreateBox(const char* p_name, float p_width, float p_height,
                float p_depth);

  /**
   * @brief 创建 3D 平面（XZ 平面，法线 Y+，中心在原点，单 Surface）
   *
   * 参考 Godot PlaneMesh。适用于地面、水面等水平面。
   * 顶点格式：Position(vec3) + Normal(vec3) + UV(vec2) = 32 字节。
   *
   * @param[in] p_name   网格名称
   * @param[in] p_width  X 轴宽度
   * @param[in] p_depth  Z 轴深度
   *
   * @return 网格 RID，失败时返回空 RID
   */
  RID CreatePlane(const char* p_name, float p_width, float p_depth);

  /**
   * @brief 创建 3D 球体（UV 球体，中心在原点，单 Surface）
   *
   * 参考 Godot SphereMesh。使用经纬线分段（UV Sphere）。
   * 顶点格式：Position(vec3) + Normal(vec3) + UV(vec2) = 32 字节。
   *
   * @param[in] p_name           网格名称
   * @param[in] p_radius         球体半径
   * @param[in] p_radial_segments 经线段数（≥3）
   * @param[in] p_rings          纬线环数（≥1）
   *
   * @return 网格 RID，失败时返回空 RID
   */
  RID CreateSphere(const char* p_name, float p_radius,
                   uint32 p_radial_segments = 32, uint32 p_rings = 16);

  /**
   * @brief 创建 3D 圆柱体（中心在原点，Y 轴为高度轴，单 Surface）
   *
   * 参考 Godot CylinderMesh。顶/底半径可不同，支持圆锥。
   * 顶点格式：Position(vec3) + Normal(vec3) + UV(vec2) = 32 字节。
   *
   * @param[in] p_name           网格名称
   * @param[in] p_top_radius     顶部半径（0 = 圆锥顶点）
   * @param[in] p_bottom_radius  底部半径
   * @param[in] p_height         高度（Y 轴）
   * @param[in] p_radial_segments 径向段数（≥3）
   *
   * @return 网格 RID，失败时返回空 RID
   */
  RID CreateCylinder(const char* p_name, float p_top_radius,
                     float p_bottom_radius, float p_height,
                     uint32 p_radial_segments = 32);

  /**
   * @brief 创建 3D 路径牵引线（圆角折线带，单 Surface）
   *
   * 参考 AR HUD 牵引线/车道线需求，支持 3D 点集输入，
   * 拐弯处自动插入扇形圆角（fillet arc），避免折线锯齿。
   *
   * @par 圆角算法（Fillet Arc）：
   *   每个拐角处，沿入射/出射方向各回退 d = r/tan(α/2)，
   *   在切点之间插入圆弧，弧心沿角平分线方向偏移 r/sin(α/2)。
   *   圆角半径自动钳制到相邻段长度允许的最大值。
   *
   * @par 坐标系：
   *   路径点在 3D 空间中定义，牵引线沿路径展开，
   *   宽度方向垂直于切线和世界向上方向 (0,1,0)，
   *   法线朝上 (0,1,0)（适用于地面车道线/牵引线）。
   *
   * @par 顶点格式：
   *   Position(vec3) + Normal(vec3) + UV(vec2) = 32 字节
   *   UV: u 沿路径长度归一化 [0,1]，v 横跨宽度 [0,1]
   *
   * @param[in] p_name           网格名称
   * @param[in] p_points         3D 路径点数组（至少 2 个点）
   * @param[in] p_point_count    路径点数量（≥2）
   * @param[in] p_width          牵引线宽度
   * @param[in] p_corner_radius  拐弯圆角半径（0 = 尖角无圆角）
   * @param[in] p_corner_segments 每个圆角的弧线细分段数（≥1）
   *
   * @return 网格 RID，失败时返回空 RID
   */
  RID CreateRibbon3D(const char* p_name, const Vec3* p_points,
                     uint32 p_point_count, float p_width,
                     float p_corner_radius = 0.0f,
                     uint32 p_corner_segments = 8);

  /**
   * @brief 获取网格信息
   *
   * @param[in] p_mesh 网格 RID
   * @return MeshInfo 指针，RID 无效时返回 nullptr
   */
  const MeshInfo* MeshGetInfo(RID p_mesh) const;

  /**
   * @brief 获取 Surface 数量
   *
   * @param[in] p_mesh 网格 RID
   * @return Surface 数量，RID 无效时返回 0
   */
  uint32 MeshGetSurfaceCount(RID p_mesh) const;

  /**
   * @brief 获取指定索引的 Surface
   *
   * @param[in] p_mesh  网格 RID
   * @param[in] p_index Surface 索引
   *
   * @return SurfaceInfo 指针，无效时返回 nullptr
   */
  const SurfaceInfo* MeshGetSurface(RID p_mesh, uint32 p_index) const;

  /**
   * @brief 检查网格是否有效
   *
   * @param[in] p_mesh 网格 RID
   * @return 网格有效返回 true，否则返回 false
   */
  bool MeshIsValid(RID p_mesh) const;

  /**
   * @brief 创建实例缓冲区
   *
   * 创建用于实例化绘制的缓冲区，存储每个实例的属性数据
   * （如变换矩阵、颜色等）。实例缓冲区作为顶点缓冲区绑定到
   * 顶点绑定点，使用 VertexFrequency::kInstance 标记。
   *
   * @param[in] p_instance_data 实例数据指针
   * @param[in] p_instance_count 实例数量
   * @param[in] p_instance_stride 每个实例的数据步幅（字节）
   *
   * @return 实例缓冲区 RDBufferID，失败时返回空 RDBufferID
   *
   * @par 使用流程：
   *   @code
   *   // 1. 创建实例数据
   *   struct InstanceData { Mat4 transform; Vec4 color; };
   *   InstanceData instances[10];
   *
   *   // 2. 创建实例缓冲区
   *   RDBufferID inst_buf = storage.CreateInstanceBuffer(
   *       instances, 10, sizeof(InstanceData));
   *
   *   // 3. 绘制时绑定实例缓冲区到额外的顶点绑定点
   *   rd.DrawListBindVertexBuffers(dl, &inst_buf, &offset, 1);
   *
   *   // 4. 使用实例化绘制
   *   rd.DrawListDrawIndexedInstanced(dl, 10);
   *
   *   // 5. 释放
   *   rd.BufferFree(inst_buf);
   *   @endcode
   *
   * @note 实例缓冲区使用 BufferUsageBits::kVertex | kTransferTo，
   *       因为在 OpenGL 中实例数据通过顶点绑定点 + glVertexAttribDivisor 传递
   */
  RDBufferID CreateInstanceBuffer(const void* p_instance_data,
                                  uint32 p_instance_count,
                                  uint32 p_instance_stride);

 private:
  /**
   * @brief 释放 Surface 的 GPU 资源
   *
   * @param[in] p_surface SurfaceInfo 指针
   */
  void FreeSurface(SurfaceInfo* p_surface);

 private:
  RenderingDevice* rd_ = nullptr;        ///< RenderingDevice（不拥有所有权）
  RIDOwner<MeshInfo, true> mesh_owner_;  ///< 线程安全的 RID → MeshInfo 映射
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_STORAGE_MESH_STORAGE_H_
