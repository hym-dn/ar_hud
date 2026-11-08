/**
 * @file rendering_server.h
 * @brief 渲染服务抽象接口 — 全量虚 API 单例（对齐 Godot RenderingServer）
 *
 * @par 职责
 *   定义 servers 层**面向应用（app）的唯一渲染服务契约**：
 *   - 资源：纹理 / 材质 / 网格 / 字体的创建、变更、释放；
 *   - 保留模式 item：2D Canvas / 3D Instance / 文本 item 的创建与属性设置；
 *   - 场景状态：per-viewport 相机（3D / 2D）与方向光；
 *   - 帧：SubmitScene() 帧提交信号。
 *   具体实现（RenderingServerDefault）负责把这些调用翻译为命令，交由渲染线程执行。
 *
 * @par 阶段（S0：绞杀者门面）
 *   本类是 servers 重设计的**冻结接口**（docs/skill/arhud-design/SKILL.md §7.11 A）。
 *   S0 阶段由 RenderingServerDefault 实现，内部**委托旧 RenderServer**，
 *   行为与现状完全一致（app/demo 改走本接口，测试保持绿）；
 *   后续阶段（S1 队列 / S2 RSG / S3~S6 拆分）逐步替换门面内部的旧实现。
 *
 * @par 线程模型
 *   所有方法均在**主线程**调用：
 *   - 资源与 item 的 `*Create*` 为**同步**命令（需写回
 * RID，阻塞至渲染线程执行完）；
 *   - `*Set*` / `*Free*` / 相机 / 灯光为**异步**命令（delta 入队，立即返回）；
 *   - `SubmitScene()` 为异步帧信号，立即返回。
 *   `IsOnRenderThread()` 供实现侧判定当前线程身份（断言 / 短路用）。
 *
 * @par 单例约定
 *   - `singleton` 在引擎装配期设置（S0：ARHudEngine::Initialize
 * 成功建栈后设置， Finalize 前清空）；未装配时为 nullptr。
 *   - app 首选 `RenderingServer::GetSingleton()` 或
 * `ARHudEngine::GetRenderServer()`。
 *   - 当前为**单引擎实例**假设：第二个引擎 Initialize 会覆盖 `singleton`。
 *
 * @par 与 Godot 的对应
 *   | 本工程 | Godot |
 *   | --- | --- |
 *   | `RenderingServer`（抽象 + 单例） | `RenderingServer` |
 *   | `RenderingServerDefault`（默认实现） | `RenderingServerDefault` |
 *   | `RenderingServer::GetSingleton()` | `RenderingServer::get_singleton()` |
 *
 * @par 命名对齐（决策 A：采用 Godot 原名）
 *   - 3D item：`Instance*`（原 `SpatialItem*`，对齐 Godot `instance_*`）
 *   - 材质变更：`MaterialSetParam`（原 `MaterialSetUniform`）
 *   - 字体：`FontCreateFromFreeType`（原 `FontLoadFromFreeType`）
 *
 * @par S0 过渡签名
 *   变长数据形参暂用「裸指针 + size」（`const uint8*` / `const
 * uint32*`），由实现侧 在入队时同步拷贝进命令载荷；S1 随 `Buffer` /
 * `Ref<Buffer>` 参数体系替换 （docs/skill/arhud-design/SKILL.md §7.11 E）。
 *
 * @see rendering_server_default.h  默认实现（S0 门面）
 * @see render_server.h             S0 被委托的旧实现
 * @see command_queue.h             命令传输（SPSC 环形缓冲）
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-10-03
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDERING_SERVER_H_
#define ARHUD_SERVERS_RENDERING_RENDERING_SERVER_H_

#include "renderer/compositor.h"
#include "storage/item_storage.h"
#include "storage/viewport_scene_state.h"
#include "typedefs.h"

namespace arhud {

/**
 * @brief 渲染服务抽象接口（全量虚 API 单例）
 *
 * 应用层（app）经本接口创建 / 变更 / 释放资源与保留模式 item，是 servers 层对
 * app 暴露的唯一渲染入口；实例由引擎装配，app 不构造、不析构。
 *
 * @par 典型用法
 * @code
 * RenderingServer *rs = RenderingServer::GetSingleton();
 * RID tex  = rs->TextureCreateFromData("bg", pixels, pixel_bytes, w, h,
 * format); RID quad = rs->MeshCreateQuad("quad", 1.0f, 1.0f); RID mat  =
 * rs->MaterialCreate("bg_mat", RID()); rs->MaterialSetTexture(mat, 0, tex);
 *
 * RID item = rs->CanvasItemCreate();
 * rs->CanvasItemSetMesh(item, quad);
 * rs->CanvasItemSetMaterial(item, mat);
 *
 * rs->Camera3DSet(camera);   // viewport 0
 * rs->SubmitScene();         // 帧提交信号
 * @endcode
 *
 * @note 纯抽象类，不得实例化；请以 GetSingleton() 获取已装配实例。
 * @see RenderingServerDefault        默认实现（S0 门面）
 * @see ARHudEngine::GetRenderServer  引擎侧入口
 */
class RenderingServer {
  ARHUD_DISABLE_COPY_MOVE(RenderingServer);

 public:
  RenderingServer() = default;
  virtual ~RenderingServer() = default;

  // ─────────────────────────────────────────────────────────────────
  //  单例
  // ─────────────────────────────────────────────────────────────────

  /** @brief 全局单例指针（装配期由引擎设置；未装配为 nullptr） */
  static RenderingServer* singleton;

  /**
   * @brief 获取全局单例
   * @return 已装配的 RenderingServer；未装配时为 nullptr
   * @note 设置点在 ARHudEngine::Initialize（成功建栈后），撤销点在 Finalize。
   * @see singleton
   */
  static RenderingServer* GetSingleton() {
    return singleton;
  }

  /**
   * @brief 创建默认实现实例（工厂，≈ Godot `RenderingServer::create()`）
   * @return 堆分配的 RenderingServerDefault（以接口指针返回）
   * @note S0 未使用：引擎以值持有 RenderingServerDefault。所有权模型待 S7
   *       接管后由 Finish() 释放；在此之前调用本方法会泄漏，勿用。
   */
  static RenderingServer* Create();

  // ─────────────────────────────────────────────────────────────────
  //  生命周期（S1 接管：创建命令队列 + 渲染线程）
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 初始化渲染服务
   * @note S1 落地（届时创建命令队列 + 渲染线程）；S0 为空实现，生命周期仍由
   *       ARHudEngine 管理，S0 请勿调用。
   */
  virtual void Init() = 0;

  /**
   * @brief 终止渲染服务
   * @note S1 落地；S0 为空实现。
   */
  virtual void Finish() = 0;

  // ─────────────────────────────────────────────────────────────────
  //  资源创建（RID 分裂：主线程即时分配 RID，初始化异步入队渲染线程）
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 从像素数据创建纹理
   *
   * @param[in] p_name       纹理名称（调试 / 日志用，不参与查找）
   * @param[in] p_data       像素字节起始地址（S0 过渡签名；调用期间需有效）
   * @param[in] p_data_size  像素字节数
   * @param[in] p_width      纹理宽度（像素）
   * @param[in] p_height     纹理高度（像素）
   * @param[in] p_format     像素格式
   * @param[in] p_mip_levels mipmap 级数（默认 1 = 无 mip）
   *
   * @return 纹理 RID；失败返回空 RID
   * @note 同步命令；S1 起 p_data 将换为 `Ref<Buffer>`。
   * @see TextureFree
   */
  virtual RID TextureCreateFromData(const char* p_name, const uint8* p_data,
                                    uint32 p_data_size, uint32 p_width,
                                    uint32 p_height, DataFormat p_format,
                                    uint32 p_mip_levels = 1) = 0;

  /**
   * @brief 创建材质
   *
   * @param[in] p_name   材质名称（调试用）
   * @param[in] p_shader 着色器 RID；空 RID 时按 p_type 取渲染器内置着色器
   * @param[in] p_type   材质类型（2D Canvas / 3D Spatial）
   *
   * @return 材质 RID；失败返回空 RID
   * @see MaterialSetParam MaterialSetTexture MaterialFree
   */
  virtual RID MaterialCreate(
      const char* p_name, RID p_shader,
      MaterialDataType p_type = MaterialDataType::kCanvas) = 0;

  /**
   * @brief 创建 2D 四边形网格（居中，面向 +Z）
   * @param[in] p_name   网格名称（调试用）
   * @param[in] p_width  宽度（世界单位）
   * @param[in] p_height 高度（世界单位）
   * @return 网格 RID；失败返回空 RID
   * @see MeshFree
   */
  virtual RID MeshCreateQuad(const char* p_name, float p_width,
                             float p_height) = 0;

  /**
   * @brief 创建 3D 长方体网格
   * @param[in] p_name   网格名称（调试用）
   * @param[in] p_width  宽度（X 轴）
   * @param[in] p_height 高度（Y 轴）
   * @param[in] p_depth  深度（Z 轴）
   * @return 网格 RID；失败返回空 RID
   * @see MeshFree
   */
  virtual RID MeshCreateBox(const char* p_name, float p_width, float p_height,
                            float p_depth) = 0;

  /**
   * @brief 创建 3D 球体网格
   * @param[in] p_name    网格名称（调试用）
   * @param[in] p_radius  半径
   * @param[in] p_sectors 经线分段数（radial segments）
   * @param[in] p_rings   纬线环数
   * @return 网格 RID；失败返回空 RID
   * @see MeshFree
   */
  virtual RID MeshCreateSphere(const char* p_name, float p_radius,
                               uint32 p_sectors, uint32 p_rings) = 0;

  /**
   * @brief 从 FreeType 字体文件加载 SDF 字体
   *
   * @param[in] p_name            字体名称（调试用）
   * @param[in] p_font_path       TTF / OTF 文件路径
   * @param[in] p_font_size       字号（像素）
   * @param[in] p_charcodes       UTF-32 码点数组（预烘焙字符集）
   * @param[in] p_charcode_count  码点个数
   * @param[in] p_sdf_spread      SDF 扩散半径（像素；默认 8）
   *
   * @return 字体 RID；失败返回空 RID
   * @note 同步命令；S1 起字符集将换为 `Ref<Buffer>`。
   * @see FontFree
   */
  virtual RID FontCreateFromFreeType(const char* p_name,
                                     const char* p_font_path, float p_font_size,
                                     const uint32* p_charcodes,
                                     uint32 p_charcode_count,
                                     float p_sdf_spread = 8.0f) = 0;

  // ─────────────────────────────────────────────────────────────────
  //  资源变更
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 写入材质 Uniform 数据
   *
   * @param[in] p_material 材质 RID
   * @param[in] p_offset   std140 布局字节偏移
   * @param[in] p_data     数据起始地址（S0 过渡签名；入队时同步拷贝）
   * @param[in] p_size     数据字节数
   *
   * @return 成功 kOK；材质无效返回 kDoesNotExist
   * @note 异步命令（入队即返回）。
   */
  virtual Error MaterialSetParam(RID p_material, uint32 p_offset,
                                 const void* p_data, uint32 p_size) = 0;

  /**
   * @brief 绑定材质纹理槽位
   * @param[in] p_material 材质 RID
   * @param[in] p_slot     纹理槽位索引
   * @param[in] p_texture  纹理 RID（空 RID = 解除绑定）
   * @return 成功 kOK；材质无效返回 kDoesNotExist
   */
  virtual Error MaterialSetTexture(RID p_material, uint32 p_slot,
                                   RID p_texture) = 0;

  // ─────────────────────────────────────────────────────────────────
  //  资源释放（异步命令，RD 延迟销毁保证在途 GPU 安全）
  // ─────────────────────────────────────────────────────────────────

  /** @brief 释放纹理 @param[in] p_texture 纹理 RID */
  virtual void TextureFree(RID p_texture) = 0;

  /** @brief 释放材质 @param[in] p_material 材质 RID */
  virtual void MaterialFree(RID p_material) = 0;

  /** @brief 释放网格 @param[in] p_mesh 网格 RID */
  virtual void MeshFree(RID p_mesh) = 0;

  /** @brief 释放字体 @param[in] p_font 字体 RID */
  virtual void FontFree(RID p_font) = 0;

  /**
   * @brief 统一回收任意 RID（依次询问各 Storage，谁认领谁处理）
   * @param[in] p_rid 待回收的 RID（纹理 / 材质 / 网格 / 字体 / item 皆可）
   * @note S1 落地；S0 为空实现。
   */
  virtual void FreeRid(RID p_rid) = 0;

  // ─────────────────────────────────────────────────────────────────
  //  2D item（RendererCanvasCull）
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 创建 2D Canvas item
   * @param[in] p_visibility_mask 创建即带的可见屏掩码（每 bit 一屏；默认全屏）
   * @return item RID；失败返回空 RID
   * @note 同步命令；item 最终可见性由 局部 visible + 屏掩码 + 父链 共同决定。
   * @see CanvasItemFree
   */
  virtual RID CanvasItemCreate(uint32 p_visibility_mask = 0xFFFFFFFF) = 0;

  /** @brief 设置 item 材质 @param[in] p_item item RID @param[in] p_material
   * 材质 RID（空 = 清除） */
  virtual void CanvasItemSetMaterial(RID p_item, RID p_material) = 0;

  /** @brief 设置 item 网格 @param[in] p_item item RID @param[in] p_mesh 网格
   * RID（空 = 清除） */
  virtual void CanvasItemSetMesh(RID p_item, RID p_mesh) = 0;

  /** @brief 设置 item 局部变换（列主序 4x4） @param[in] p_item item RID
   * @param[in] p_transform 16 个 float */
  virtual void CanvasItemSetTransform(RID p_item,
                                      const float p_transform[16]) = 0;

  /** @brief 设置 item 不透明度（沿父链相乘） @param[in] p_item item RID
   * @param[in] p_opacity 0.0~1.0 */
  virtual void CanvasItemSetOpacity(RID p_item, float p_opacity) = 0;

  /** @brief 设置 item 绘制序 @param[in] p_item item RID @param[in] p_z_order
   * 序值（大者靠前） */
  virtual void CanvasItemSetZOrder(RID p_item, int32 p_z_order) = 0;

  /** @brief 设置 item 图层 @param[in] p_item item RID @param[in] p_layer
   * 图层号（先按 layer 再按 z_order 排序） */
  virtual void CanvasItemSetLayer(RID p_item, int32 p_layer) = 0;

  /** @brief 设置 item 局部可见性（与屏掩码、父链取与） @param[in] p_item item
   * RID @param[in] p_visible 是否可见 */
  virtual void CanvasItemSetVisible(RID p_item, bool p_visible) = 0;

  /** @brief 设置 item 父节点（构成层级，ADR-0005） @param[in] p_item item RID
   * @param[in] p_parent 父 item RID（空 = 脱离父） */
  virtual void CanvasItemSetParent(RID p_item, RID p_parent) = 0;

  /** @brief 设置 z_order 是否相对父 @param[in] p_item item RID @param[in]
   * p_relative true = 与父累加 */
  virtual void CanvasItemSetZAsRelative(RID p_item, bool p_relative) = 0;

  /** @brief 释放 2D item（异步） @param[in] p_item item RID */
  virtual void CanvasItemFree(RID p_item) = 0;

  // ─────────────────────────────────────────────────────────────────
  //  3D item（RendererSceneCull；Godot instance_* 语义）
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 创建 3D Instance item
   * @param[in] p_visibility_mask 创建即带的可见屏掩码（每 bit 一屏；默认全屏）
   * @return item RID；失败返回空 RID
   * @note 同步命令。命名对齐 Godot `instance_*`（原 `SpatialItem*`）。
   * @see InstanceFree
   */
  virtual RID InstanceCreate(uint32 p_visibility_mask = 0xFFFFFFFF) = 0;

  /** @brief 设置 Instance 材质 @param[in] p_item item RID @param[in] p_material
   * 材质 RID */
  virtual void InstanceSetMaterial(RID p_item, RID p_material) = 0;

  /** @brief 设置 Instance 网格 @param[in] p_item item RID @param[in] p_mesh
   * 网格 RID */
  virtual void InstanceSetMesh(RID p_item, RID p_mesh) = 0;

  /** @brief 设置 Instance 局部变换（列主序 4x4） @param[in] p_item item RID
   * @param[in] p_transform 16 个 float */
  virtual void InstanceSetTransform(RID p_item,
                                    const float p_transform[16]) = 0;

  /** @brief 设置渲染优先级 @param[in] p_item item RID @param[in] p_priority
   * 优先级（小者先绘制） */
  virtual void InstanceSetRenderPriority(RID p_item, uint8 p_priority) = 0;

  /** @brief 设置是否禁用视锥剔除 @param[in] p_item item RID @param[in]
   * p_disable true = 不剔除 */
  virtual void InstanceSetDisableCull(RID p_item, bool p_disable) = 0;

  /** @brief 设置实例父节点（构成层级，ADR-0005） @param[in] p_item item RID
   * @param[in] p_parent 父 item RID（空 = 脱离父） */
  virtual void InstanceSetParent(RID p_item, RID p_parent) = 0;

  /** @brief 释放 3D Instance（异步） @param[in] p_item item RID */
  virtual void InstanceFree(RID p_item) = 0;

  // ─────────────────────────────────────────────────────────────────
  //  文本 item（TextServer 持有）
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 创建文本 item
   * @param[in] p_visibility_mask 创建即带的可见屏掩码（每 bit 一屏；默认全屏）
   * @return item RID；失败返回空 RID
   * @note 同步命令。
   * @see TextItemFree
   */
  virtual RID TextItemCreate(uint32 p_visibility_mask = 0xFFFFFFFF) = 0;

  /** @brief 设置文本字体 @param[in] p_item item RID @param[in] p_font 字体 RID
   */
  virtual void TextItemSetFont(RID p_item, RID p_font) = 0;

  /** @brief 设置文本内容（UTF-32） @param[in] p_item item RID @param[in] p_text
   * 码点数组 @param[in] p_count 码点个数 */
  virtual void TextItemSetText(RID p_item, const uint32* p_text,
                               uint32 p_count) = 0;

  /** @brief 设置文本锚点位置 @param[in] p_item item RID @param[in] p_x X 坐标
   * @param[in] p_y Y 坐标 */
  virtual void TextItemSetPosition(RID p_item, float p_x, float p_y) = 0;

  /** @brief 设置文本颜色（RGBA） @param[in] p_item item RID @param[in] p_color
   * 4 个 float（0~1） */
  virtual void TextItemSetColor(RID p_item, const float p_color[4]) = 0;

  /** @brief 设置文本绘制序 @param[in] p_item item RID @param[in] p_z_order
   * 序值（大者靠前） */
  virtual void TextItemSetZOrder(RID p_item, int32 p_z_order) = 0;

  /** @brief 设置文本图层 @param[in] p_item item RID @param[in] p_layer 图层号
   */
  virtual void TextItemSetLayer(RID p_item, int32 p_layer) = 0;

  /** @brief 设置文本局部可见性 @param[in] p_item item RID @param[in] p_visible
   * 是否可见 */
  virtual void TextItemSetVisible(RID p_item, bool p_visible) = 0;

  /** @brief 设置文本父节点 @param[in] p_item item RID @param[in] p_parent 父
   * item RID（空 = 脱离父） */
  virtual void TextItemSetParent(RID p_item, RID p_parent) = 0;

  /** @brief 设置 z_order 是否相对父 @param[in] p_item item RID @param[in]
   * p_relative true = 与父累加 */
  virtual void TextItemSetZAsRelative(RID p_item, bool p_relative) = 0;

  /** @brief 释放文本 item（异步） @param[in] p_item item RID */
  virtual void TextItemFree(RID p_item) = 0;

  // ─────────────────────────────────────────────────────────────────
  //  相机 / 灯光（per-viewport；ADR-0006 / ADR-0007）
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 设置 3D 相机
   * @param[in] p_viewport 目标视口索引（ADR-0007；0 = 默认屏）
   * @param[in] p_camera   相机矩阵（视图 / 投影）
   * @note 异步命令；多屏时按索引路由到对应 RenderViewport。
   * @see Camera2DSet
   */
  virtual void Camera3DSet(uint32 p_viewport, const CameraMatrix& p_camera) = 0;

  /**
   * @brief 设置 2D 相机
   * @param[in] p_viewport 目标视口索引（0 = 默认屏）
   * @param[in] p_camera   2D 变换（平移 / 缩放 / 旋转）
   * @see Camera3DSet
   */
  virtual void Camera2DSet(uint32 p_viewport, const Transform2D& p_camera) = 0;

  /**
   * @brief 设置方向光
   * @param[in] p_viewport 目标视口索引（0 = 默认屏）
   * @param[in] p_light    方向光参数（方向 / 颜色 / 强度）
   * @see DirectionalLightClear
   */
  virtual void DirectionalLightSet(uint32 p_viewport,
                                   const DirectionalLight& p_light) = 0;

  /**
   * @brief 清除方向光
   * @param[in] p_viewport 目标视口索引（0 = 默认屏）
   * @see DirectionalLightSet
   */
  virtual void DirectionalLightClear(uint32 p_viewport) = 0;

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
  //  视口（RendererViewport；S6 落地）
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 创建视口
   * @param[in] p_width  视口宽度（像素）
   * @param[in] p_height 视口高度（像素）
   * @return 视口 RID；失败返回空 RID
   * @note S6 落地；S0 为空实现（视口当前由引擎 / RenderViewport 直接管理）。
   * @see ViewportDestroy
   */
  virtual RID ViewportCreate(uint32 p_width, uint32 p_height) = 0;

  /** @brief 销毁视口 @param[in] p_viewport 视口 RID @note S6 落地；S0 空实现 */
  virtual void ViewportDestroy(RID p_viewport) = 0;

  /** @brief 调整视口尺寸 @param[in] p_viewport 视口 RID @param[in] p_width 宽
   * @param[in] p_height 高 @note S6 落地；S0 空实现 */
  virtual void ViewportSetSize(RID p_viewport, uint32 p_width,
                               uint32 p_height) = 0;

  /** @brief 设置视口是否参与渲染 @param[in] p_viewport 视口 RID @param[in]
   * p_active 是否激活 @note S6 落地；S0 空实现 */
  virtual void ViewportSetActive(RID p_viewport, bool p_active) = 0;

  // ─────────────────────────────────────────────────────────────────
  //  帧 / 线程
  // ─────────────────────────────────────────────────────────────────

  /**
   * @brief 帧提交信号
   *
   * app 在每帧内容全部推完后调用，表示「本帧命令已发完」。异步，立即返回；
   * 渲染线程据此触发一次渲染 + 上屏。
   *
   * @note 与 Godot 的 force_draw 语义相近（但本工程为信号式）。
   */
  virtual void SubmitScene() = 0;

  /**
   * @brief 渲染线程帧入口
   * @param[in] p_present 是否在渲染后上屏
   * @param[in] p_step    帧间隔（秒）
   * @note ≈ Godot `RenderingServerDefault::draw()`；S1 接管，S0 为空实现
   *       （帧编排仍由 ARHudEngine::RenderThreadFunc 驱动）。
   */
  virtual void Draw(bool p_present, double p_step) = 0;

  /**
   * @brief 同步命令队列（整批屏障）
   *
   * 对齐 Godot `RenderingServer::sync()` → `CommandQueueMT::sync()`：
   * 阻塞调用线程，直到**调用瞬间已发布的全部命令**在渲染线程执行完毕。
   *
   * @note 粒度是"整批"，不是"逐命令"：不提供返回值回传（push_and_ret 等价物），
   *       需要结果的资源创建一律走 RID 分裂；渲染线程自身调用时退化为就地排空。
   */
  virtual void Sync() = 0;

  /**
   * @brief 是否存在待处理变更
   * @return true 表示有待渲染变更（changes 计数 > 0）
   * @note S1 落地（changes 计数）；S0 恒返回 false。
   */
  virtual bool HasChanged() const = 0;

  /**
   * @brief 当前线程是否为渲染线程
   * @return true = 运行在渲染 / 消费线程
   * @note S0 已实现（委托命令队列的线程判定）。
   * @see command_queue.h
   */
  virtual bool IsOnRenderThread() = 0;
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDERING_SERVER_H_
