/**
 * @file render_server.h
 * @brief 渲染服务主线程代理 — app 可见的唯一渲染资源 API（Route B）
 *
 * @par 职责（参考 Godot RenderingServer）
 *   把 app 的资源创建/变更/释放调用序列化为命令，经 CommandQueue 发到
 *   渲染线程执行（绑定命令 thunk 直调 Storage）。
 *   RenderServer 自身不持有 GPU 资源、不直接执行 GPU 操作——它是序列化器。
 *
 * @par 调用路径
 *   @code
 *   app（主线程）                        渲染线程
 *     │                                    │
 *     ├─ RenderServer::TextureCreateFromData
 *     │   ├─ TextureAllocate()   本线程立即拿到空壳 RID
 *     │   └─ PushBoundBlob(ts, &TextureStorage::TextureInitializeFromData,
 *     │                    rid, params, blob[像素][名称])
 *     │        ─────────────────────────────►  Flush → 自派发 thunk
 *     │   （发完即走，队列内无同步等待）            └─ 渲染线程建 GPU 纹理
 *     │
 *     ├─ RenderServer::MeshFree（变更/释放类同此）
 *     │   └─ PushBound(ms, &MeshStorage::MeshFree, rid)
 *     │        ─────────────────────────────►  Flush → 自派发 thunk → 释放
 *   @endcode
 *
 * @par RID 分裂（ADR-0010）
 *   创建类 API 不在队列内做结果回传：主线程先 XxxAllocate() 拿到空壳 RID，
 *   初始化命令异步入队。安全性由两点保证——RIDOwner 线程安全 + 队列严格 FIFO
 *   （引用该 RID 的命令必然排在其初始化命令之后）。
 *
 * @par Godot 对应
 *   本形态即 Godot `rendering_server_default.h` 的 `FUNCRIDSPLIT(m_type)`
 * 宏体：
 *   @code
 *   RID ret = RSG::x_storage->x_allocate();
 *   if (Thread::get_caller_id() == server_thread) { x_initialize(ret); } //
 * 本线程直调 else { command_queue.push(RSG::x_storage, &XStorage::x_initialize,
 * ret); }  // 异线程入队 return ret;
 *   @endcode
 *   即 Godot 的写入/创建/释放路径**同样全异步**。Godot 保留的同步通道分两种，
 *   本工程只对齐其一：
 *     - `sync()`（整批屏障）→ ✅ `CommandQueue::Sync()` /
 * `RenderingServer::Sync()`；
 *     - `push_and_sync` / `push_and_ret`（逐命令等待与返回值回传）→ ❌ 不采用：
 *       凡需要"渲染侧算出的值"（如像素回读）当前无出口，新增此类 API
 * 时须另行设计 （优先主线程镜像，其次才考虑引回回传通道）。
 *
 * @par 短路规则（CanBypassQueue — 对齐 Godot 线程闸门 ASYNC_COND_PUSH）
 *   | 条件 | 行为 | 场景 |
 *   |---|---|---|
 *   | 队列或合成器未绑定 | 直接调 Storage（无队列可排空） | 单线程形态 / 单测 |
 *   | 队列已 shutdown | 直接调 Storage，**不排空残令** |
 * 引擎关闭后清理（残令可能引用已释放资源） | | 队列无消费者 | **先
 * FlushIfPending()** 再直调 Storage | 初始化期（渲染线程未起，主线程持 GL
 * 上下文）/ 单测 | | 调用者即渲染线程 | **先 FlushIfPending()** 再直调 Storage
 * | 渲染线程回调本类；批内调用被再入保护挡成空操作 | | 其余 |
 * 走命令队列（异步入队） | 常规运行期 |
 *
 *   "先排空再直调"是顺序正确性的必要条件：调用者即执行者时，若不先跑掉更早入队的命令，
 *   本次直调会插到它们前面 → 破坏 app 的程序顺序（Godot 在每个 SERVER_METHOD
 * 宏里都这么做）。
 *
 * @par 状态变更命令（B3 后）
 *   渲染已在渲染线程执行，主线程不再读取 ItemStorage / MaterialData，故变更与
 *   释放类命令一律异步入队（读写同线程）。生产者侧没有逐命令等待，只有帧边界/收口处
 *   按需使用的整批屏障 `RenderingServer::Sync()`（委托
 * `CommandQueue::Sync()`）。
 *
 * @see ADR-0010（绑定命令 (目标, 方法, 参数)）
 * @see command_queue.h  传输层（PushBound / PushBoundBlob）
 *
 * @author yameng.he
 * @version 1.2
 * @date 2026-08-22
 * @date 2026-10-13 调用路径改为 RID 分裂 + 纯异步；短路规则与变更命令段去掉 B2
 * 过渡期表述
 * @date 2026-10-13 对齐 Godot 线程闸门：执行者直调前先
 * FlushIfPending；记录整批屏障 Sync()
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDER_SERVER_H_
#define ARHUD_SERVERS_RENDERING_RENDER_SERVER_H_

#include "renderer/compositor.h"
#include "storage/item_storage.h"
#include "storage/viewport_scene_state.h"

namespace arhud {

class RenderViewport;
class IViewportProvider;

/**
 * @brief 主线程渲染 API 代理（命令绑定的生产者侧）
 *
 * 生命周期由 RenderViewport 管理（成员），app 通过
 * ARHudEngine::GetRenderServer() / RenderViewport::GetRenderServer() 访问。
 */
/**
 * @brief Camera3DSet 的命令意图参数（ADR-0006：只传意图，渲染线程重建
 * CameraMatrix）
 */
struct Camera3DIntent {
  uint8 projection_type = 0;
  uint8 keep_aspect = 0;
  uint8 ortho_mode = 0;
  float fov_y = 0.0f;
  float aspect = 0.0f;
  float near_z = 0.0f;
  float far_z = 0.0f;
  float ortho_size = 0.0f;
  float ortho_rect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float position[3] = {0.0f, 0.0f, 0.0f};
  float target[3] = {0.0f, 0.0f, 0.0f};
  float up[3] = {0.0f, 1.0f, 0.0f};
};

/**
 * @brief Camera2DSet 的命令意图参数（3 列 × 2 = 6 元素平铺）
 */
struct Camera2DIntent {
  float columns[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
};

class RenderServer {
  ARHUD_DISABLE_COPY_MOVE(RenderServer);

 public:
  RenderServer() = default;
  ~RenderServer() = default;

  /**
   * @brief 绑定依赖（RenderViewport::Initialize 后调用）
   *
   * @param[in] p_queue    命令队列（主线程 push / 渲染线程 flush）
   * @param[in] p_compositor Compositor（短路路径直接调 Storage）
   * @param[in] p_item_storage 保留模式 item 存储（短路路径）
   * @param[in] p_viewport_provider 视口解析器（ADR-0007：相机/灯光短路路径
   *                          按索引解析目标 RenderViewport；可空 = 忽略）
   */
  void Initialize(CommandQueue* p_queue, ARHudCompositor* p_compositor,
                  ItemStorage* p_item_storage,
                  IViewportProvider* p_viewport_provider = nullptr);

  /** @brief 解绑（RenderViewport::Finalize 前调用，防悬垂） */
  void Finalize();

  // ─────────────────────────────────────────────────────────────────
  //  资源创建（RID 分裂：主线程即时分配 RID，初始化异步入队渲染线程）
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 从像素数据创建纹理
   * @return 纹理 RID，失败返回空 RID
   */
  RID TextureCreateFromData(const char* p_name, const uint8* p_data,
                            uint32 p_data_size, uint32 p_width, uint32 p_height,
                            DataFormat p_format, uint32 p_mip_levels = 1);

  /**
   * @brief 创建材质
   *
   * @param[in] p_name   材质名称
   * @param[in] p_shader 着色器 RID（空 RID 时按 p_type 取渲染器内置着色器）
   * @param[in] p_type   材质类型（2D Canvas / 3D Spatial）
   *
   * @return 材质 RID，失败返回空 RID
   */
  RID MaterialCreate(const char* p_name, RID p_shader,
                     MaterialDataType p_type = MaterialDataType::kCanvas);

  /** @brief 创建 2D 四边形网格 @return 网格 RID */
  RID MeshCreateQuad(const char* p_name, float p_width, float p_height);

  /** @brief 创建 3D 长方体网格 @return 网格 RID */
  RID MeshCreateBox(const char* p_name, float p_width, float p_height,
                    float p_depth);

  /**
   * @brief 创建 3D 球体网格
   * @param[in] p_sectors 经线分段数（radial segments）
   * @param[in] p_rings   纬线环数
   * @return 网格 RID
   */
  RID MeshCreateSphere(const char* p_name, float p_radius, uint32 p_sectors,
                       uint32 p_rings);

  /**
   * @brief 从 FreeType 字体文件加载 SDF 字体
   * @return 字体 RID，失败返回空 RID
   */
  RID FontLoadFromFreeType(const char* p_name, const char* p_font_path,
                           float p_font_size, const uint32* p_charcodes,
                           uint32 p_charcode_count, float p_sdf_spread = 8.0f);

  // ─────────────────────────────────────────────────────────────────
  //  资源变更（B2：主线程直接调用；B3 渲染搬线程后切异步命令）
  // ─────────────────────────────────────────────────────────────────

  /** @brief 写材质 Uniform（std140 偏移） */
  Error MaterialSetUniform(RID p_material, uint32 p_offset, const void* p_data,
                           uint32 p_size);

  /** @brief 绑定材质纹理槽位 */
  Error MaterialSetTexture(RID p_material, uint32 p_slot, RID p_texture);

  // ─────────────────────────────────────────────────────────────────
  //  资源释放（异步命令，RD 延迟销毁保证 GPU 安全）
  // ─────────────────────────────────────────────────────────────────

  /** @brief 释放纹理 */
  void TextureFree(RID p_texture);

  /** @brief 释放材质 */
  void MaterialFree(RID p_material);

  /** @brief 释放网格 */
  void MeshFree(RID p_mesh);

  /** @brief 释放字体 */
  void FontFree(RID p_font);

  // ─────────────────────────────────────────────────────────────────
  //  Item 管理（保留模式，服务端存储，RID 主线程预分配）
  //
  //  Create 走 RID 分裂（主线程 XxxAllocate 即时返回 RID，初始化经 PushBound
  //  异步入队）； Set*/Free/Camera/Light 一律异步
  //  PushBound（delta），SubmitScene 收尾。 ItemStorage
  //  由渲染线程独占，主线程只递命令、不直接改状态。
  // ─────────────────────────────────────────────────────────────────

  /// @brief 创建 2D item（p_visibility_mask：创建即带的屏掩码，默认全屏兼容）
  RID CanvasItemCreate(uint32 p_visibility_mask = 0xFFFFFFFF);
  void CanvasItemSetMaterial(RID p_item, RID p_material);
  void CanvasItemSetMesh(RID p_item, RID p_mesh);
  void CanvasItemSetTransform(RID p_item, const float p_transform[16]);
  void CanvasItemSetOpacity(RID p_item, float p_opacity);
  void CanvasItemSetZOrder(RID p_item, int32 p_z_order);
  void CanvasItemSetLayer(RID p_item, int32 p_layer);
  void CanvasItemSetVisible(RID p_item, bool p_visible);
  /// @brief ADR-0005: 挂接父 Canvas item（kInvalid =
  /// 摘到根；环/无效父防御拒绝）
  void CanvasItemSetParent(RID p_item, RID p_parent);
  /// @brief ADR-0005: z_order 是否相对父链累加（对齐 Godot z_as_relative，默认
  /// true）
  void CanvasItemSetZAsRelative(RID p_item, bool p_relative);
  void CanvasItemFree(RID p_item);

  /// @brief 创建 3D item（p_visibility_mask：创建即带的屏掩码，默认全屏兼容）
  RID SpatialItemCreate(uint32 p_visibility_mask = 0xFFFFFFFF);
  void SpatialItemSetMaterial(RID p_item, RID p_material);
  void SpatialItemSetMesh(RID p_item, RID p_mesh);
  void SpatialItemSetTransform(RID p_item, const float p_transform[16]);
  void SpatialItemSetRenderPriority(RID p_item, uint8 p_priority);
  void SpatialItemSetDisableCull(RID p_item, bool p_disable);
  /// @brief ADR-0005: 挂接父 Spatial item（kInvalid =
  /// 摘到根；世界矩阵沿链连乘）
  void SpatialItemSetParent(RID p_item, RID p_parent);
  void SpatialItemFree(RID p_item);

  /// @brief 创建文字 item（p_visibility_mask：创建即带的屏掩码，默认全屏兼容）
  RID TextItemCreate(uint32 p_visibility_mask = 0xFFFFFFFF);
  void TextItemSetFont(RID p_item, RID p_font);
  void TextItemSetText(RID p_item, const uint32* p_text, uint32 p_count);
  void TextItemSetPosition(RID p_item, float p_x, float p_y);
  void TextItemSetColor(RID p_item, const float p_color[4]);
  void TextItemSetZOrder(RID p_item, int32 p_z_order);
  void TextItemSetLayer(RID p_item, int32 p_layer);
  void TextItemSetVisible(RID p_item, bool p_visible);
  /// @brief ADR-0005: 挂接父 Canvas item（文字叶子；kInvalid =
  /// 摘到根；位置经父链矩阵变换）
  void TextItemSetParent(RID p_item, RID p_parent);
  /// @brief ADR-0005: z_order 是否相对父链累加（默认 true）
  void TextItemSetZAsRelative(RID p_item, bool p_relative);
  void TextItemFree(RID p_item);

  /// @brief 设置某视口的 3D 相机（p_viewport
  /// 越界时渲染线程安全忽略；ADR-0006/0007）
  /// @note 命令链只携带相机**意图参数**（fov/position/target/up…），不传矩阵；
  ///       手工视图矩阵（SetViewTransform）不支持命令链——WARN 后忽略本次设置。
  void Camera3DSet(uint32 p_viewport, const CameraMatrix& p_camera);
  /// @brief 设置某视口的 2D 相机（视口变换）
  void Camera2DSet(uint32 p_viewport, const Transform2D& p_camera);
  /// @brief 设置某视口的平行光（置 has_light=true）
  void DirectionalLightSet(uint32 p_viewport, const DirectionalLight& p_light);
  /// @brief 清除某视口的平行光（置 has_light=false）
  void DirectionalLightClear(uint32 p_viewport);
  /// @name 便捷重载：等价于 p_viewport = 0（单屏零改动）
  /// @{
  void Camera3DSet(const CameraMatrix& p_camera) {
    Camera3DSet(0, p_camera);
  }
  void Camera2DSet(const Transform2D& p_camera) {
    Camera2DSet(0, p_camera);
  }
  void DirectionalLightSet(const DirectionalLight& p_light) {
    DirectionalLightSet(0, p_light);
  }
  void DirectionalLightClear() {
    DirectionalLightClear(0);
  }
  /// @}

  // ─────────────────────────────────────────────────────────────────
  //  帧提交（B3：异步 Push 纯信号）
  //
  //  SubmitScene = 主线程"本帧内容推完了"的声明。渲染线程 flush 到
  //  该命令后执行一轮逐屏渲染（读 ItemStorage → FBO → 导出）。
  //  立即返回（真并行的关键）：主线程不等渲染。
  //
  //  帧节拍 = FrameImageQueue 背压（HasInFlight / free slot）+
  //  RD frame slot（frame_count_=2 最多超前 2 帧）。
  // ─────────────────────────────────────────────────────────────────

  /** @brief 帧提交信号（异步 Push；渲染线程从 ItemStorage 渲染全部屏） */
  void SubmitScene();

  // ── S1 过渡桥：相机/灯光/提交的**命令目标方法**（渲染线程执行；S6 迁往
  // RendererViewport）── 相机/灯光因需"索引 → 视口"运行时解析，命令绑到
  // RenderServer 自身； 待 S6 引入 RSG::viewport 后，目标改为
  // RendererViewport（见 architecture §7.12）。

  /** @brief 渲染线程：按意图重建 CameraMatrix 并写入目标视口 + 置屏 dirty */
  void ApplyCamera3D(uint32 p_viewport, const Camera3DIntent& p_intent);

  /** @brief 渲染线程：按意图重建 Transform2D 并写入目标视口 + 置屏 dirty */
  void ApplyCamera2D(uint32 p_viewport, const Camera2DIntent& p_intent);

  /** @brief 渲染线程：写入目标视口的平行光 + 置屏 dirty */
  void ApplyDirectionalLight(uint32 p_viewport,
                             const DirectionalLight& p_light);

  /** @brief 渲染线程：清除目标视口的平行光 + 置屏 dirty */
  void ApplyDirectionalLightClear(uint32 p_viewport);

  /** @brief 渲染线程：置位 SubmitScene 帧提交信号（经
   * RenderGlobals::submit_signal） */
  void ApplySubmitScene();

 private:
  /** @brief 是否可短路直接调 Storage（无消费者 / 消费者本线程 / 已关闭） */
  bool CanBypassQueue() const;

  /**
   * @brief 按视口索引解析 RenderViewport（经 IViewportProvider；越界返回
   * nullptr）
   *
   * 短路路径下相机/灯光直接写入目标视口（ADR-0007），需先解析；
   * provider 未注入或索引越界时 WARN 并返回 nullptr（调用方安全忽略）。
   */
  RenderViewport* ResolveViewport(uint32 p_index) const;

  CommandQueue* queue_ = nullptr;          ///< 命令队列（不拥有）
  ARHudCompositor* compositor_ = nullptr;  ///< Compositor（不拥有，短路路径）
  ItemStorage* item_storage_ =
      nullptr;  ///< 保留模式 item 存储（不拥有，短路路径）
  IViewportProvider* viewport_provider_ =
      nullptr;  ///< 视口解析器（ADR-0007，可空）
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDER_SERVER_H_
