/**
 * @file viewport_scene_state.h
 * @brief ViewportSceneState — per-viewport 场景级状态（相机 / 灯光）
 *
 * ADR-0007：相机/灯光的**类型归属**从 `ItemStorage`（item 账本）迁出——
 * 它们是"视口/场景级状态"，与 `RenderViewport` 已有的 per-screen 渲染配置
 * （cull_mask / viewport_index / 后处理）同属"本屏如何取景"，故由
 * `RenderViewport` 自持一份 `ViewportSceneState`。
 *
 * 纯数据结构，零 GPU 依赖，无 `Initialize(rd)`；定义在 `storage/` 下保持
 * `renderer → storage` 单向依赖（SpatialRenderer 消费 `DirectionalLight`）。
 */

#ifndef ARHUD_SERVERS_RENDERING_STORAGE_VIEWPORT_SCENE_STATE_H_
#define ARHUD_SERVERS_RENDERING_STORAGE_VIEWPORT_SCENE_STATE_H_

#include "math/camera_matrix.h"
#include "math/math_funcs.h"
#include "typedefs.h"

namespace arhud {
/**
 * @brief 平行光状态（per-viewport，POD）
 *
 * 由 `ViewportSceneState` 持有：
 *   - 主线程经 RenderServer::DirectionalLightSet → 命令 → 渲染线程写入视口
 *   - 渲染线程 SpatialRenderer 读取（GetDirectionalLight → LightBlock UBO）
 * 纯数据结构，无 GPU 资源；定义在此避免 storage 反向依赖 renderer。
 */
struct DirectionalLight {
  /// @brief 光照方向（世界空间，指向光源来向，不必归一化规范说明）
  float direction[3] = {0.0f, -1.0f, 0.0f};
  /// @brief 平行光颜色（线性 RGB）
  float color[3] = {1.0f, 1.0f, 1.0f};
  /// @brief 光强标量（乘到颜色上）
  float energy = 1.0f;
  /// @brief 环境光系数（0~1，无光照面片的最低亮度占比）
  float ambient = 0.15f;
};

/**
 * @brief per-viewport 场景级状态（相机 + 灯光）
 *
 * 每个视口一份，随 `RenderViewport` 生命周期；写入由渲染线程（经命令）
 * 完成，读取由渲染线程 `RenderFromStorage` 完成——同线程，无并发。
 */
struct ViewportSceneState {
  CameraMatrix camera_3d;  ///< 3D 相机（透视/正交矩阵，供 SpatialRenderer）
  Transform2D camera_2d;   ///< 2D 视口变换（供 CanvasRenderer 投影）
  DirectionalLight light;  ///< 平行光参数
  bool has_light =
      false;  ///< 是否已设置平行光（未设置时 GetDirectionalLight 返回 nullptr）
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_STORAGE_VIEWPORT_SCENE_STATE_H_
