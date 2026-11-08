/**
 * @file render_viewport.h
 * @brief 单屏渲染管线（B3-M0.3：FBO 渲染 + GDI 上屏，无 SwapBuffers）
 *
 * B3-M0.3 渲染路径变更：GL 上下文走 offscreen surface，可见窗口无 GL
 * 像素格式。渲染始终输出到 scene_buffers_.framebuffer（FBO），渲染线程
 * 帧完成后用 rd_->FramebufferReadback() 回读 FBO 像素到 cpu_buffer_，
 * 再由 IFramePresenter::Present() 通过 GDI StretchDIBits 提交到可见窗口。
 * 全程不调用 SwapBuffers。
 *
 * B3-M0.1 之前 RenderViewport 按值拥有完整渲染栈（RD/CommandQueue/
 * Compositor/ItemStorage/RenderServer）。上提后改为收 EngineStackRefs
 * 指针，所有权归 ARHudEngine::Impl，所有屏共享一套。
 *
 * RenderViewport 保留的 per-screen 状态：offscreen surface/context 引用、
 * 宽高、后处理全套（RenderBuffers/composite pipeline/UBO）、CPU 回读
 * 缓冲、IFramePresenter 指针、渲染线程原语。
 *
 * @see EngineStackRefs  共享栈指针注入
 * @see ARHudEngine      栈所有方
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDER_VIEWPORT_H_
#define ARHUD_SERVERS_RENDERING_RENDER_VIEWPORT_H_

#include <atomic>

#include "command_queue.h"
#include "frame_image_queue.h"
#include "frame_presenter.h"
#include "math/camera_matrix.h"
#include "os/sync.h"
#include "os/thread.h"
#include "render_buffers.h"
#include "renderer/compositor.h"
#include "rendering_server.h"
#include "storage/item_storage.h"
#include "storage/viewport_scene_state.h"
#include "typedefs.h"

namespace arhud {

class IRenderingContextDriver;

/**
 * @brief Engine 级共享栈引用（B3-M0.1：RenderViewport 不再拥有栈，改收指针）
 *
 * B3-M0.3 新增 offscreen surface/context（共享，由 ARHudEngine 创建）
 * 及 per-screen IFramePresenter（GDI 上屏，由 ARHudEngine 创建并注入）。
 */
struct EngineStackRefs {
  RenderingDevice* rd = nullptr;
  CommandQueue* command_queue = nullptr;
  ItemStorage* item_storage = nullptr;
  RenderingServer* rendering_server = nullptr;
  ARHudCompositor* compositor = nullptr;

  // B3-M0.3: offscreen GL surface/context（共享，Engine 创建）
  IRenderingContextDriver::SurfaceID offscreen_surface_id =
      IRenderingContextDriver::kInvalidSurfaceId;
  IRenderingContextDriver::ContextID offscreen_context_id =
      IRenderingContextDriver::kInvalidContextId;

  // B3-M0.3: per-screen GDI 上屏器（Engine 创建，RenderViewport 只持有指针）
  IFramePresenter* presenter = nullptr;
};

class RenderViewport {
  ARHUD_DISABLE_COPY_MOVE(RenderViewport);

 public:
  using ID = uint32;

  static constexpr ID kInvalidId = UINT32_MAX;

  /**
   * @brief 单屏配置（B3-M0.3：移除 surface_id/context_id，改为
   *        offscreen 共享，经 EngineStackRefs 注入）
   */
  struct Config {
    uint32 width = 0;
    uint32 height = 0;
  };

 public:
  RenderViewport() = default;
  ~RenderViewport();

  Error Initialize(IRenderingContextDriver* p_context_driver,
                   const Config& p_config, const EngineStackRefs& p_stack_refs);

  void Finalize();

  bool IsInitialized() const;

  void Render(const CameraMatrix& p_camera_3d, const Transform2D& p_camera_2d,
              const DirectionalLight* p_light,
              const SpatialItem* p_spatial_items, uint32 p_spatial_count,
              const HUDCanvasLayer* p_canvas_layers, uint32 p_layer_count);

  /**
   * @brief B3: 渲染线程单屏渲染入口（dirty → HasInFlight → 槽 → 渲 FBO → 回读 →
   * READY）
   *
   * ARHudEngine::RenderThreadFunc 在 kSubmitScene 信号驱动下逐屏调用。
   * 六步语义见实现处文档。静态内容（dirty=false）零成本跳过。
   */
  // 返回 true = 本屏本轮真正完成"渲染+回读+MarkReady"（引擎可清本屏脏位）；
  // false = 未脏或背压跳过（脏位保留，下一轮 submit 重试）。
  bool RenderAndSubmit();

  /** @brief B3: 设置帧图像队列（由 ARHudEngine 创建并注入，非拥有） */
  void SetFrameImageQueue(FrameImageQueue* p_queue) {
    queue_ = p_queue;
  }

  /** @brief 主线程渲染 API 代理（资源创建/变更/释放 + 帧场景）。app
   * 层唯一资源入口 */
  RenderingServer* GetRenderServer();

  uint32 GetWidth() const;

  uint32 GetHeight() const;

  /** @brief B3-M0.4: 设置屏裁剪掩码（控制哪些 item 在本屏可见） */
  void SetCullMask(uint32 p_mask) {
    cull_mask_ = p_mask;
  }

  /** @brief B3-M0.4: 设置本屏 ID */
  void SetViewportIndex(uint32 p_id) {
    viewport_index_ = p_id;
  }

  // ── per-viewport 场景级状态（ADR-0007：相机/灯光随视口自持）──
  /** @brief 设置本视口 3D 相机（由 RenderServer::ApplyCamera3D 经命令调用） */
  void Camera3DSet(const CameraMatrix& p_camera) {
    scene_state_.camera_3d = p_camera;
  }
  /** @brief 设置本视口 2D 相机 */
  void Camera2DSet(const Transform2D& p_camera) {
    scene_state_.camera_2d = p_camera;
  }
  /** @brief 设置本视口平行光（置 has_light=true） */
  void DirectionalLightSet(const DirectionalLight& p_light) {
    scene_state_.light = p_light;
    scene_state_.has_light = true;
  }
  /** @brief 清除本视口平行光（置 has_light=false） */
  void DirectionalLightClear() {
    scene_state_.has_light = false;
  }

  /** @brief 本视口 3D 相机（供 RenderFromStorage 取用） */
  const CameraMatrix& GetCamera3D() const {
    return scene_state_.camera_3d;
  }
  /** @brief 本视口 2D 相机 */
  const Transform2D& GetCamera2D() const {
    return scene_state_.camera_2d;
  }
  /** @brief 本视口平行光（无灯时返回 nullptr） */
  const DirectionalLight* GetDirectionalLight() const {
    return scene_state_.has_light ? &scene_state_.light : nullptr;
  }

  void HandleResize(uint32 p_width, uint32 p_height);

  /**
   * @brief B3-M0.3: 渲染线程应用挂起的 resize（重建 FBO + 帧队列）
   *
   * 仅在无在途帧（queue_->HasInFlight()==false）时执行——present 线程
   * 可能正持有 queue 槽位，Resize 会释放槽位缓冲。否则保留 pending 并
   * 返回 false，待 present 线程 Release（唤醒渲染线程）后下轮重试。
   *
   * @return true 已应用（调用方应置脏并重渲）；false 未应用 / 无待办
   */
  bool ApplyPendingResize();

  struct PostProcessParams {
    float gamma = 2.2f;
    bool enable_distortion = false;
    float distortion_k1 = 0.0f;
    float distortion_k2 = 0.0f;
    bool enable_premul_alpha = false;
  };

  PostProcessParams& GetPostProcessParams();

  bool IsPostProcessEnabled() const;

 private:
  /**
   * @brief 保留模式渲染入口：从 ItemStorage 收集场景状态并渲染（B3 起私有）
   *
   * B3 起仅由渲染线程内的 RenderAndSubmit() 调用，app 层经 RenderServer
   * 的 item API（CanvasItemCreate/Set* 等）维护服务端场景状态，无需每帧
   * 重建 item 列表。
   *
   * @par 收集规则
   *   - 相机/灯光：本视口自持的 `scene_state_`（ADR-0007；不再取自
   * ItemStorage）
   *   - SpatialItem：按 cull_mask 过滤收集
   *   - Canvas/TextItem：cull_mask + visible 过滤 + 按 layer 分组成
   * HUDCanvasLayer
   *
   * @par 线程（B3 起）
   *   渲染线程独占调用（RenderAndSubmit 内部）。ItemStorage 同线程
   *   经命令更新（flush 后），读写无并发。主线程仅经命令队列递状态，
   *   旁路直连仅在无消费者/同线程场景可达。
   */
  void RenderFromStorage();

  Error InitializePostProcess();
  void FinalizePostProcess();
  void RenderPostProcess(uint32 p_screen_width, uint32 p_screen_height);

  /**
   * @brief 创建/重建后处理 uniform set（绑定当前 scene_buffers_.color_texture）
   * @note resize 重建 FBO
   * 后颜色纹理变更，须调用以重绑新纹理（否则采样悬空纹理）
   */
  Error CreateCompositeUniformSet();

  bool initialized_ = false;
  std::atomic<uint32> width_{
      0};  ///< 渲染目标宽（主线程经 GetWidth 读，渲染线程写）
  std::atomic<uint32> height_{0};  ///< 渲染目标高
  uint32 viewport_index_ =
      0;  ///< B3-M0.4: 本屏 ID（per-screen 相机/cull_mask 用）
  uint32 cull_mask_ =
      0xFFFFFFFF;  ///< B3-M0.4: 屏裁剪掩码（与 item visibility_mask AND）
  ViewportSceneState
      scene_state_;  ///< ADR-0007: per-viewport 场景级状态（相机/灯光）

  // B3-M0.3: offscreen GL surface/context（共享，从 EngineStackRefs 注入）
  IRenderingContextDriver::SurfaceID offscreen_surface_id_ = 0;
  IRenderingContextDriver::ContextID offscreen_context_id_ =
      IRenderingContextDriver::kInvalidContextId;
  IRenderingContextDriver* context_driver_ = nullptr;

  // B3-M0.3: GDI 上屏（per-screen，Engine 创建，RenderViewport 持有指针）
  IFramePresenter* presenter_ = nullptr;

  // B3-M0.3: FBO 回读目标（rd_->FramebufferReadback → CPU 缓冲 → Present）
  uint8* cpu_buffer_ = nullptr;
  uint32 cpu_buffer_size_ = 0;

  // B3-M0.3: 渲染线程侧 deferred resize（HandleResize 置位，渲染线程消费）
  std::atomic<bool> resize_pending_{false};
  std::atomic<uint32> new_width_{0};
  std::atomic<uint32> new_height_{0};

  // ── Engine 级共享栈（B3-M0.1：从值改为指针，所有权归 ARHudEngine::Impl）──
  RenderingDevice* rd_ = nullptr;
  CommandQueue* command_queue_ = nullptr;
  ItemStorage* item_storage_ = nullptr;
  RenderingServer* rendering_server_ = nullptr;
  ARHudCompositor* compositor_ = nullptr;

  // B3: per-screen 帧图像队列（Engine 创建并注入，非拥有）
  FrameImageQueue* queue_ =
      nullptr;  ///< B3: 双缓冲帧队列（非拥有，由 Engine 注入）

  bool post_process_enabled_ = false;
  RenderBuffers scene_buffers_;
  // Post-process output FBO: the composite pass renders here and the CPU
  // readback samples from here. In Mode R the default framebuffer is the
  // hidden 100x100 offscreen window and must never be the presented source.
  RenderBuffers post_buffers_;
  RID composite_shader_rid_;
  RDShaderID composite_shader_rd_id_;
  RDVertexFormatID composite_vertex_format_id_;
  RDPipelineID composite_pipeline_id_;
  RDBufferID quad_vbo_;
  RDBufferID quad_ibo_;
  RDBufferID composite_params_ubo_;
  RDUniformSetID composite_set_;
  PostProcessParams post_process_params_;
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDER_VIEWPORT_H_
