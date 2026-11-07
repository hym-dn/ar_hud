/**
 * @file compositor.cpp
 * @brief 渲染后端入口实现
 *
 * ARHudCompositor 的核心实现，包括：
 *   - 子系统初始化/终止（Storage + Renderer 的依赖顺序管理）
 *   - 帧生命周期（BeginFrame / EndFrame）
 *
 * 帧编排逻辑已移至 RenderViewport::Render()。
 *
 * @par 关键设计决策：
 *
 *   1. 初始化顺序必须遵循依赖拓扑（Shader → Texture → Material → ...）
 *      逆序释放确保依赖方先于被依赖方销毁。
 *
 *   2. 清除策略（由 RenderViewport::Render() 执行）：
 *      - 3D 场景始终清除颜色+深度缓冲
 *      - 若无 3D 背景，第一个 2D 图层清除颜色缓冲
 *      - 后续 2D 图层不清除（叠加渲染）
 *      这保证了无论是否有 3D 内容，首帧都能正确清屏。
 *
 * @par 线程安全：
 *   所有方法假定在渲染线程中调用，非线程安全。
 *   GL 上下文必须在调用线程中 MakeCurrent。
 *
 * @see compositor.h
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-30
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "compositor.h"

#include "rendering_context_driver.h"

namespace arhud {
/**
 * @brief 析构函数
 *
 * 若 Compositor 仍处于初始化状态（initialized_ == true），
 * 自动调用 Finalize() 释放所有子系统资源。
 *
 * @note 建议在对象销毁前显式调用 Finalize()，以确保
 *       RenderingDevice 仍有效时完成 GPU 资源释放。
 */
ARHudCompositor::~ARHudCompositor() {
  if (initialized_) {
    Finalize();
  }
}

/**
 * @brief 初始化 Compositor 及所有子系统
 *
 * @par 执行流程：
 *   1. 防重入检查：若已初始化，返回 kAlreadyExists
 *   2. 参数校验：p_rd 非空且已初始化，否则返回 kFailed
 *   3. 保存 RenderingDevice 指针
 *   4. 按依赖顺序初始化 Storage（Shader → Texture → Material → Mesh → Font）
 *   5. 初始化 Renderer（Canvas → Spatial）
 *   6. 任一失败立即返回错误
 *   7. 全部成功后置 initialized_ = true
 *
 * @par 失败处理策略：
 *   任一子系统初始化失败时立即返回，但已初始化的子系统不会被回滚。
 *   调用方应在失败后调用 Finalize() 手动清理已初始化的部分。
 *
 * @param[in] p_rd RenderingDevice 指针（不拥有所有权）
 *
 * @return 错误码
 * @retval Error::kOK           成功
 * @retval Error::kAlreadyExists 重复初始化
 * @retval Error::kFailed       参数无效或子系统失败
 */
Error ARHudCompositor::Initialize(RenderingDevice* p_rd) {
  // ── 防重入 ──
  if (initialized_) {
    ARHUD_LOG_ERROR("kAlreadyExists",
                    "ARHudCompositor::Initialize() already initialized");
    return Error::kAlreadyExists;
  }

  // ── 参数校验 ──
  if (!p_rd || !p_rd->IsInitialized()) {
    ARHUD_LOG_ERROR("kFailed",
                    "ARHudCompositor::Initialize() invalid RenderingDevice");
    return Error::kFailed;
  }

  rd_ = p_rd;

  Error err;

  // ── 1. ShaderStorage：编译内置着色器（零依赖，最先初始化） ──
  err = shader_storage_.Initialize(rd_);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "ARHudCompositor: ShaderStorage::Initialize() failed");
    return err;
  }

  // ── 2. TextureStorage：创建默认白色纹理（零依赖） ──
  err = texture_storage_.Initialize(rd_);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "ARHudCompositor: TextureStorage::Initialize() failed");
    return err;
  }

  // ── 3. MaterialStorage：创建默认材质（依赖 ShaderStorage + TextureStorage）
  // ──
  err = material_storage_.Initialize(rd_, &shader_storage_, &texture_storage_);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "ARHudCompositor: MaterialStorage::Initialize() failed");
    return err;
  }

  // ── 4. MeshStorage：创建默认四边形网格（零依赖） ──
  err = mesh_storage_.Initialize(rd_);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "ARHudCompositor: MeshStorage::Initialize() failed");
    return err;
  }

  // ── 5. FontStorage：初始化字体图集（依赖 TextureStorage） ──
  err = font_storage_.Initialize(&texture_storage_);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "ARHudCompositor: FontStorage::Initialize() failed");
    return err;
  }

  // ── 6. CanvasRenderer：初始化 2D 渲染器（依赖全部 Storage） ──
  err = canvas_renderer_.Initialize(rd_, &shader_storage_, &material_storage_,
                                    &mesh_storage_, &texture_storage_,
                                    &font_storage_);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "ARHudCompositor: CanvasRenderer::Initialize() failed");
    return err;
  }

  // ── 7. SpatialRenderer：初始化 3D 渲染器（依赖全部 Storage） ──
  err = spatial_renderer_.Initialize(rd_, &shader_storage_, &material_storage_,
                                     &mesh_storage_, &texture_storage_);
  if (err != Error::kOK) {
    ARHUD_LOG_ERROR("kFailed",
                    "ARHudCompositor: SpatialRenderer::Initialize() failed");
    return err;
  }

  initialized_ = true;
  ARHUD_LOG_INFO("ARHudCompositor::Initialize() success");
  return Error::kOK;
}

/**
 * @brief 终止 Compositor 并释放所有子系统
 *
 * @par 执行流程（逆序释放，与 Initialize 相反）：
 *   1. SpatialRenderer::Finalize()  — 释放 3D 渲染器资源
 *   2. CanvasRenderer::Finalize()   — 释放 2D 渲染器资源
 *   3. FontStorage::Finalize()      — 释放字体图集
 *   4. MeshStorage::Finalize()      — 释放网格缓冲区
 *   5. MaterialStorage::Finalize()  — 释放材质（引用的 Shader/Texture
 * 不在此释放）
 *   6. TextureStorage::Finalize()   — 释放纹理对象
 *   7. ShaderStorage::Finalize()    — 释放着色器程序（最后释放，被 Material
 * 引用）
 *
 * @par 安全性：
 *   - 若未初始化（initialized_ == false），直接返回，不执行任何操作
 *   - 每个子系统的 Finalize() 应自行处理"未初始化时直接返回"的情况
 *   - 调用后 rd_ 置 nullptr，initialized_ 置 false
 */
void ARHudCompositor::Finalize() {
  if (!initialized_) {
    return;
  }

  // ── 逆序释放：Renderer 先，Storage 后 ──
  spatial_renderer_.Finalize();
  canvas_renderer_.Finalize();
  font_storage_.Finalize();
  mesh_storage_.Finalize();
  material_storage_.Finalize();
  texture_storage_.Finalize();
  shader_storage_.Finalize();

  rd_ = nullptr;
  initialized_ = false;
  ARHUD_LOG_INFO("ARHudCompositor::Finalize() done");
}

/**
 * @brief 开始新帧
 *
 * 递增帧号并记录帧间隔时间。
 *
 * @param[in] p_frame_step 帧步长（秒），由上层计时器提供
 *
 * @post frame_number_ 递增 1
 * @post delta_time_ == p_frame_step
 */
void ARHudCompositor::BeginFrame(double p_frame_step) {
  frame_number_++;
  delta_time_ = p_frame_step;
}

/**
 * @brief 结束当前帧
 *
 * 当前为空操作。预留给未来帧结束时的清理工作
 * （如 GPU 命令提交、帧缓冲交换等）。
 */
void ARHudCompositor::EndFrame() {}

}  // namespace arhud