/**
 * @file test_camera_routing.cpp
 * @brief RenderServer per-viewport 相机/灯光路由 + Camera3D 参数重建单测
 *
 * 背景（回归防护）：
 *   - ADR-0007：相机/灯光状态从 ItemStorage 迁入 RenderViewport 的
 *     ViewportSceneState；命令按 viewport_index 经 IViewportProvider
 * 解析目标视口。
 *   - ADR-0006：Camera3D 只携带**意图参数**（fov/position/target/up…），
 *     渲染侧用 CameraMatrix::Set* 与 LookAt 重建——规避"传输未重算矩阵"导致相机
 *     端到端不生效。
 *   - ADR-0010：命令执行器（CommandExecutor）与命令结构体已删除，重建/路由逻辑
 *     迁入 `RenderServer::ApplyCamera3D/2D` / `ApplyDirectionalLight/Clear`
 *     （渲染线程目标方法，S1 过渡桥）。本测试直接驱动这些目标方法。
 *
 * 覆盖语义：
 *   - ApplyCamera3D：意图参数经 CameraMatrix 重建，且按视口路由（互不串扰）
 *   - ApplyCamera2D：按视口路由
 *   - ApplyDirectionalLight / ApplyDirectionalLightClear：按视口路由且互不串扰
 *   - 越界视口索引安全丢弃（不崩溃、不误写其它视口）
 *
 * 说明：用真实 RenderViewport 承载 ViewportSceneState（默认构造即可，无需
 * GPU）， 配合测试本地 FakeViewportProvider（按索引返回视口）。Apply* 不访问
 * compositor/queue， 故可传 nullptr 构造测试用 RenderServer。
 */

#include <gtest/gtest.h>

#include "render_server.h"
#include "render_viewport.h"
#include "storage/item_storage.h"
#include "viewport_provider.h"

namespace {
using arhud::Camera2DIntent;
using arhud::Camera3DIntent;
using arhud::DirectionalLight;
using arhud::ItemStorage;
using arhud::IViewportProvider;
using arhud::KeepAspect;
using arhud::ProjectionType;
using arhud::RenderServer;
using arhud::RenderViewport;

/// @brief 测试用视口解析器：按索引返回两个真实 RenderViewport
class FakeViewportProvider : public IViewportProvider {
 public:
  RenderViewport* viewports[2] = {nullptr, nullptr};

  RenderViewport* GetViewport(uint32 p_index) override {
    return (p_index < 2) ? viewports[p_index] : nullptr;
  }
};

/// @brief 构造透视相机意图（ADR-0006 意图参数，非矩阵载荷）
Camera3DIntent MakePerspectiveIntent(float p_fov, float p_px, float p_py,
                                     float p_pz) {
  Camera3DIntent intent;
  intent.projection_type = static_cast<uint8>(ProjectionType::kPerspective);
  intent.keep_aspect = static_cast<uint8>(KeepAspect::kKeepHeight);
  intent.ortho_mode = 0;
  intent.fov_y = p_fov;
  intent.aspect = 16.0f / 9.0f;
  intent.near_z = 0.1f;
  intent.far_z = 100.0f;
  intent.ortho_size = 1.0f;
  intent.position[0] = p_px;
  intent.position[1] = p_py;
  intent.position[2] = p_pz;
  intent.target[0] = 0.0f;
  intent.target[1] = 0.0f;
  intent.target[2] = -1.0f;
  intent.up[0] = 0.0f;
  intent.up[1] = 1.0f;
  intent.up[2] = 0.0f;
  return intent;
}

TEST(CameraRouting, ApplyCamera3DRebuildAndRouteToItsOwnViewport) {
  ItemStorage storage;
  RenderViewport vp_0;
  RenderViewport vp_1;
  FakeViewportProvider provider;
  provider.viewports[0] = &vp_0;
  provider.viewports[1] = &vp_1;

  RenderServer server;
  server.Initialize(nullptr, nullptr, &storage, &provider);

  server.ApplyCamera3D(0, MakePerspectiveIntent(60.0f, 1.0f, 2.0f, 3.0f));
  server.ApplyCamera3D(1, MakePerspectiveIntent(30.0f, 9.0f, 8.0f, 7.0f));

  // ADR-0006：意图参数经 CameraMatrix::SetPerspective + LookAt 重建
  EXPECT_FLOAT_EQ(60.0f, vp_0.GetCamera3D().GetFOV());
  EXPECT_FLOAT_EQ(1.0f, vp_0.GetCamera3D().GetPosition().x);
  EXPECT_FLOAT_EQ(2.0f, vp_0.GetCamera3D().GetPosition().y);
  EXPECT_FLOAT_EQ(3.0f, vp_0.GetCamera3D().GetPosition().z);

  // ADR-0007：按视口路由，互不串扰（视口 0 不被视口 1 的载荷覆盖）
  EXPECT_FLOAT_EQ(30.0f, vp_1.GetCamera3D().GetFOV());
  EXPECT_FLOAT_EQ(9.0f, vp_1.GetCamera3D().GetPosition().x);
  EXPECT_FLOAT_EQ(8.0f, vp_1.GetCamera3D().GetPosition().y);
  EXPECT_FLOAT_EQ(7.0f, vp_1.GetCamera3D().GetPosition().z);
}

TEST(CameraRouting, ApplyCamera2DRoutesToItsOwnViewport) {
  ItemStorage storage;
  RenderViewport vp_0;
  RenderViewport vp_1;
  FakeViewportProvider provider;
  provider.viewports[0] = &vp_0;
  provider.viewports[1] = &vp_1;

  RenderServer server;
  server.Initialize(nullptr, nullptr, &storage, &provider);

  auto make_intent = [](float p_ox, float p_oy) {
    Camera2DIntent intent;
    for (int i = 0; i < 6; ++i) {
      intent.columns[i] = 0.0f;
    }
    intent.columns[0] = 1.0f;  // x 轴
    intent.columns[3] = 1.0f;  // y 轴
    intent.columns[4] = p_ox;
    intent.columns[5] = p_oy;
    return intent;
  };

  server.ApplyCamera2D(0, make_intent(10.0f, 20.0f));
  server.ApplyCamera2D(1, make_intent(-5.0f, 7.0f));

  EXPECT_FLOAT_EQ(10.0f, vp_0.GetCamera2D().columns[2].x);
  EXPECT_FLOAT_EQ(20.0f, vp_0.GetCamera2D().columns[2].y);
  EXPECT_FLOAT_EQ(-5.0f, vp_1.GetCamera2D().columns[2].x);
  EXPECT_FLOAT_EQ(7.0f, vp_1.GetCamera2D().columns[2].y);
}

TEST(CameraRouting, ApplyDirectionalLightPerViewportAndClear) {
  ItemStorage storage;
  RenderViewport vp_0;
  RenderViewport vp_1;
  FakeViewportProvider provider;
  provider.viewports[0] = &vp_0;
  provider.viewports[1] = &vp_1;

  RenderServer server;
  server.Initialize(nullptr, nullptr, &storage, &provider);

  DirectionalLight light;
  light.direction[0] = -0.5f;
  light.direction[1] = -1.0f;
  light.direction[2] = 0.25f;
  light.color[0] = 1.0f;
  light.color[1] = 0.8f;
  light.color[2] = 0.6f;
  light.energy = 2.0f;
  light.ambient = 0.1f;

  server.ApplyDirectionalLight(1, light);

  ASSERT_NE(nullptr, vp_1.GetDirectionalLight());
  EXPECT_FLOAT_EQ(2.0f, vp_1.GetDirectionalLight()->energy);
  EXPECT_FLOAT_EQ(0.1f, vp_1.GetDirectionalLight()->ambient);
  // 关键回归点：视口 1 的灯不串扰视口 0
  EXPECT_EQ(nullptr, vp_0.GetDirectionalLight());

  server.ApplyDirectionalLightClear(1);

  EXPECT_EQ(nullptr, vp_1.GetDirectionalLight());
}

TEST(CameraRouting, OutOfRangeViewportIsDroppedSafely) {
  ItemStorage storage;
  RenderViewport vp_0;
  FakeViewportProvider provider;
  provider.viewports[0] = &vp_0;

  RenderServer server;
  server.Initialize(nullptr, nullptr, &storage, &provider);

  server.ApplyCamera3D(0, MakePerspectiveIntent(60.0f, 1.0f, 2.0f, 3.0f));
  // 越界索引（provider 只认 0/1）应安全丢弃，不崩溃、不污染视口 0
  server.ApplyCamera3D(5, MakePerspectiveIntent(12.0f, 99.0f, 99.0f, 99.0f));

  EXPECT_FLOAT_EQ(60.0f, vp_0.GetCamera3D().GetFOV());
  EXPECT_FLOAT_EQ(1.0f, vp_0.GetCamera3D().GetPosition().x);
}

}  // namespace