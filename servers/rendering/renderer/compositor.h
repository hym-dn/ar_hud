/**
 * @file compositor.h
 * @brief 渲染后端入口（≈ Godot RendererCompositor）
 *
 * ARHudCompositor 是渲染后端的入口，持有并管理所有 Storage 和 Renderer
 * 实例，架构参考 Godot 4.6 的 RendererCompositor。
 *
 * @par 职责边界（与 RenderViewport 的分工）：
 *
 *   | 类                | Godot 对应          | 职责                       |
 *   |-------------------|---------------------|----------------------------|
 *   | ARHudCompositor   | RendererCompositor  | 持有 Storage + Renderer    |
 *   | RenderViewport      | RendererViewport    | 帧编排（2D/3D 渲染顺序）  |
 *
 *   ARHudCompositor 只负责子系统的初始化/终止和资源访问，
 *   不参与帧编排逻辑。帧编排由 RenderViewport::Render() 承担。
 *
 * @par 所有权：
 *   ARHudCompositor 持有所有 Storage 和 Renderer 实例（值语义）。
 *   外部代码通过访问器方法获取 Storage 指针，用于创建纹理、材质、
 *   网格等 GPU 资源。Compositor 不拥有 RenderingDevice（仅持有指针）。
 *
 * @par 依赖关系图：
 *   @code
 *   RenderingDevice (外部持有，不拥有)
 *        │
 *        ▼
 *   ARHudCompositor
 *        ├── ShaderStorage    ← 零依赖
 *        ├── TextureStorage   ← 零依赖
 *        ├── MaterialStorage  ← 依赖 ShaderStorage + TextureStorage
 *        ├── MeshStorage      ← 零依赖
 *        ├── FontStorage      ← 依赖 TextureStorage
 *        ├── CanvasRenderer   ← 依赖全部 Storage
 *        └── SpatialRenderer  ← 依赖全部 Storage
 *   @endcode
 *
 * @see arch_skill.md §2.8 Compositor/Storage/Renderer
 * @see phase4_plan_skill.md §Phase 4.3 + 4.4
 * @see RenderViewport  帧编排（RendererViewport 角色）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-30
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDERER_COMPOSITOR_H_
#define ARHUD_SERVERS_RENDERING_RENDERER_COMPOSITOR_H_

#include "io/logger.h"
#include "os/memory.h"
#include "renderer/canvas_renderer.h"
#include "renderer/spatial_renderer.h"
#include "rendering_device.h"
#include "storage/font_storage.h"
#include "storage/material_storage.h"
#include "storage/mesh_storage.h"
#include "storage/shader_storage.h"
#include "storage/texture_storage.h"
#include "template/local_vector.h"

namespace arhud {

// ═══════════════════════════════════════════════════════════════════════
// ARHudCompositor — 渲染后端入口（≈ Godot RendererCompositor）
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief 渲染后端入口（参考 Godot RendererCompositor）
 *
 * ARHudCompositor 持有并管理所有 Storage 和 Renderer 实例，
 * 负责子系统的初始化/终止和资源访问。
 * 帧编排逻辑由 RenderViewport::Render() 承担。
 *
 * @par 生命周期：
 *   @code
 *   ARHudCompositor compositor;
 *   compositor.Initialize(&rd);   // 初始化所有子系统
 *   // ... 通过访问器创建/使用资源 ...
 *   compositor.Finalize();        // 按逆序释放子系统
 *   @endcode
 *   析构函数会自动调用 Finalize()，但建议显式调用以确保释放顺序。
 *
 * @par 初始化顺序（参考 Godot RenderingServerDefault::_init）：
 *   1. ShaderStorage  （零依赖）
 *   2. TextureStorage  （零依赖）
 *   3. MaterialStorage （依赖 ShaderStorage + TextureStorage）
 *   4. MeshStorage     （零依赖）
 *   5. FontStorage     （依赖 TextureStorage）
 *   6. CanvasRenderer  （依赖全部 Storage）
 *   7. SpatialRenderer （依赖全部 Storage）
 *
 * @note 不可复制、不可移动（ARHUD_DISABLE_COPY_MOVE）。
 *       原因：持有 RenderingDevice 原始指针和多个子系统实例，
 *       复制/移动会导致指针悬挂或双重释放。
 */
class ARHudCompositor {
  ARHUD_DISABLE_COPY_MOVE(ARHudCompositor);

 public:
  /** @brief 默认构造。需随后调用 Initialize() 完成初始化。 */
  ARHudCompositor() = default;

  /**
   * @brief 析构函数。若仍处于初始化状态，自动调用 Finalize()。
   *
   * @note 建议在析构前显式调用 Finalize()，以确保子系统释放顺序
   *       与外部依赖（如 RenderingDevice）的生命周期协调。
   */
  ~ARHudCompositor();

 public:
  /**
   * @brief 初始化 Compositor 及所有子系统
   *
   * 按依赖拓扑顺序初始化 Storage → Renderer：
   *   1. ShaderStorage::Initialize(rd)        — 编译内置着色器
   *   2. TextureStorage::Initialize(rd)       — 创建默认白色纹理
   *   3. MaterialStorage::Initialize(rd, shader_storage, texture_storage)
   *                                            — 创建默认材质
   *   4. MeshStorage::Initialize(rd)          — 创建默认四边形网格
   *   5. FontStorage::Initialize(texture_storage) — 初始化字体图集
   *   6. CanvasRenderer::Initialize(rd, all storages)   — 初始化 2D 渲染器
   *   7. SpatialRenderer::Initialize(rd, all storages)  — 初始化 3D 渲染器
   *
   * 任一子系统初始化失败则立即返回错误，已初始化的子系统不会被回滚
   * （需调用 Finalize() 手动清理）。
   *
   * @param[in] p_rd RenderingDevice 指针（不拥有所有权）
   *
   * @retval Error::kOK           初始化成功
   * @retval Error::kAlreadyExists 已经初始化（重复调用）
   * @retval Error::kFailed       无效参数或子系统初始化失败
   *
   * @pre p_rd 非空且已初始化（p_rd->IsInitialized() == true）
   * @pre GL 上下文已在当前线程 MakeCurrent
   *
   * @post initialized_ == true
   * @post 所有 Storage 和 Renderer 均已初始化
   */
  Error Initialize(RenderingDevice* p_rd);

  /**
   * @brief 终止 Compositor 并释放所有子系统
   *
   * 按初始化的逆序释放 Renderer → Storage：
   *   1. SpatialRenderer::Finalize()
   *   2. CanvasRenderer::Finalize()
   *   3. FontStorage::Finalize()
   *   4. MeshStorage::Finalize()
   *   5. MaterialStorage::Finalize()
   *   6. TextureStorage::Finalize()
   *   7. ShaderStorage::Finalize()
   *
   * 逆序释放确保依赖方先于被依赖方销毁（如 MaterialStorage
   * 引用 ShaderStorage 中的着色器，必须先释放 MaterialStorage）。
   *
   * @pre GL 上下文已在当前线程 MakeCurrent（GPU 资源需在正确上下文中释放）
   *
   * @post initialized_ == false
   * @post rd_ == nullptr
   * @post 所有 Storage 和 Renderer 均已清理
   *
   * @note 安全调用：若未初始化则直接返回，不执行任何操作。
   */
  void Finalize();

  /**
   * @brief 开始新帧
   *
   * 递增帧号并记录帧间隔时间，供子系统和动画使用。
   *
   * @param[in] p_frame_step 帧步长（秒），即上一帧到本帧的时间间隔。
   *                         通常由上层计时器计算（如 1/60 ≈ 0.0167）。
   *
   * @pre Initialize() 已成功调用
   *
   * @post frame_number_ 递增 1
   * @post delta_time_ == p_frame_step
   */
  void BeginFrame(double p_frame_step);

  /**
   * @brief 结束当前帧
   *
   * 当前为空操作。预留给未来帧结束时的清理工作
   * （如 GPU 命令提交、帧缓冲交换等）。
   *
   * @pre BeginFrame() 已调用
   */
  void EndFrame();

  /**
   * @brief 查询初始化状态
   *
   * @return true 已初始化，false 未初始化
   */
  bool IsInitialized() const {
    return initialized_;
  }

  /**
   * @brief 获取着色器存储
   *
   * 用于创建/查询着色器资源。着色器是材质的基础依赖。
   *
   * @return ShaderStorage 指针（始终非空，Initialize 后可用）
   */
  ShaderStorage* GetShaderStorage() {
    return &shader_storage_;
  }

  /**
   * @brief 获取纹理存储
   *
   * 用于创建/查询纹理资源。纹理是材质和字体的基础依赖。
   *
   * @return TextureStorage 指针（始终非空，Initialize 后可用）
   */
  TextureStorage* GetTextureStorage() {
    return &texture_storage_;
  }

  /**
   * @brief 获取材质存储
   *
   * 用于创建/查询材质资源。材质引用着色器和纹理。
   *
   * @return MaterialStorage 指针（始终非空，Initialize 后可用）
   */
  MaterialStorage* GetMaterialStorage() {
    return &material_storage_;
  }

  /**
   * @brief 获取网格存储
   *
   * 用于创建/查询网格资源。网格定义了几何体数据。
   *
   * @return MeshStorage 指针（始终非空，Initialize 后可用）
   */
  MeshStorage* GetMeshStorage() {
    return &mesh_storage_;
  }

  /**
   * @brief 获取字体存储
   *
   * 用于创建/查询字体图集。字体存储依赖纹理存储来管理
   * SDF 字体图集纹理。
   *
   * @return FontStorage 指针（始终非空，Initialize 后可用）
   */
  FontStorage* GetFontStorage() {
    return &font_storage_;
  }

  /**
   * @brief 获取 2D Canvas 渲染器
   *
   * CanvasRenderer 负责 2D 绘制项的批量渲染（矩形、纹理、文字等）。
   * 通常通过 Render() 间接调用，而非直接操作渲染器。
   *
   * @return CanvasRenderer 指针（始终非空，Initialize 后可用）
   */
  CanvasRenderer* GetCanvasRenderer() {
    return &canvas_renderer_;
  }

  /**
   * @brief 获取 3D Spatial 渲染器
   *
   * SpatialRenderer 负责 3D 绘制项的渲染（含深度测试和光照）。
   * 通常通过 Render() 间接调用，而非直接操作渲染器。
   *
   * @return SpatialRenderer 指针（始终非空，Initialize 后可用）
   */
  SpatialRenderer* GetSpatialRenderer() {
    return &spatial_renderer_;
  }

 private:
  // ── 状态 ──
  bool initialized_ =
      false;  ///< 是否已初始化（Initialize 成功后置 true，Finalize 后置 false）
  RenderingDevice* rd_ =
      nullptr;  ///< RenderingDevice 指针（不拥有所有权，Initialize 时赋值）

  // ── 资源存储层 ──
  ShaderStorage shader_storage_;    ///< 着色器存储（编译/管理 GLSL 着色器程序）
  TextureStorage texture_storage_;  ///< 纹理存储（创建/管理 GPU 纹理对象）
  MaterialStorage material_storage_;  ///< 材质存储（管理着色器+纹理组合）
  MeshStorage mesh_storage_;          ///< 网格存储（管理顶点/索引缓冲区）
  FontStorage font_storage_;          ///< 字体存储（SDF 字体图集 + glyph 查询）

  // ── 渲染器 ──
  CanvasRenderer canvas_renderer_;  ///< 2D Canvas 渲染器（批量渲染 2D 绘制项）
  SpatialRenderer
      spatial_renderer_;  ///< 3D Spatial 渲染器（深度测试 + 光照渲染）

  // ── 帧状态 ──
  uint64 frame_number_ = 0;  ///< 帧计数器（BeginFrame 递增，从 0 开始）
  double delta_time_ = 0.0;  ///< 帧间隔时间（秒），BeginFrame 时更新
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDERER_COMPOSITOR_H_
