/**
 * @file main.cpp
 * @brief Phase 4 Spatial Demo — 3D Spatial Rendering Validation
 *
 * @par Goal
 *   Validate the 3D Spatial rendering pipeline end-to-end:
 *   Compositor → SpatialRenderer → CameraMatrix + DirectionalLight
 *
 * @par Test content
 *   1. Initialize Compositor (all Storage + Renderer)
 *   2. Create 3D meshes (Box)
 *   3. Create SpatialMaterial with opacity
 *   4. Set up CameraMatrix (perspective + LookAt)
 *   5. Set up DirectionalLight
 *   6. Push SpatialItems and render via Compositor
 *   7. Verify: 3D box with depth test + directional light
 *
 * @author yameng.he
 * @version 2.0
 * @date 2026-08-21
 * @copyright Copyright (c) 2025 ARHud Project
 */

#include <chrono>
#include <thread>

#include "arhud_engine.h"

using namespace arhud;

static constexpr uint32_t kWindowWidth = 1280;
static constexpr uint32_t kWindowHeight = 720;

int main() {
  InitializeLogger();
  ARHUD_LOG_INFO("=== Phase 4 Spatial Demo ===");

  {
    ARHudEngine engine;
    ARHudEngine::Config engine_cfg;
    engine_cfg.vsync = true;
    Error err = engine.Initialize(engine_cfg);
    if (err != Error::kOK) {
      ARHUD_LOG_ERROR("kFailed", "ARHudEngine::Initialize() failed");
      return 1;
    }

    ARHudEngine::ScreenConfig screen_cfg;
    screen_cfg.title = "ARHud - Phase 4 Spatial Demo";
    screen_cfg.width = kWindowWidth;
    screen_cfg.height = kWindowHeight;
    engine.AddScreen(screen_cfg);
    ARHUD_LOG_INFO("ARHudEngine initialized");

    RenderViewport* render_viewport = engine.GetScreen(0);
    RenderingServer* render_server = engine.GetRenderServer();

    CameraMatrix camera_3d;
    camera_3d.SetPerspective(45.0f,
                             static_cast<float32>(kWindowWidth) / kWindowHeight,
                             0.1f, 100.0f);
    camera_3d.LookAt(Vec3(0.0f, 2.0f, 10.0f), Vec3(0.0f, 0.0f, 0.0f));

    DirectionalLight light;
    light.direction[0] = -0.5f;
    light.direction[1] = -1.0f;
    light.direction[2] = -0.3f;
    light.color[0] = 1.0f;
    light.color[1] = 1.0f;
    light.color[2] = 1.0f;
    light.energy = 1.0f;

    // 初始资源 RID
    RID material_rid;
    RID box_rid;
    RID item_rid;
    bool resources_created = false;

    // 动态资源 RID
    RID dyn_material_rid;
    RID dyn_sphere_rid;
    RID dyn_item_rid;
    bool dyn_created = false;

    uint32_t frame_count = 0;
    constexpr uint32_t kTargetFPS = 60;
    constexpr auto kFrameDuration =
        std::chrono::microseconds(1'000'000 / kTargetFPS);
    while (!engine.ShouldClose() && frame_count < 600) {
      auto frame_start = std::chrono::steady_clock::now();
      engine.BeginFrame();

      // 经 RenderServer 创建资源
      if (!resources_created) {
        ARHUD_LOG_INFO("=== Creating initial resources via RenderServer ===");

        material_rid = render_server->MaterialCreate(
            "spatial_mat", RID(), MaterialDataType::kSpatial);
        ARHUD_LOG_INFO("Spatial material created: valid=%d",
                       material_rid.IsValid());

        float opacity = 1.0f;
        render_server->MaterialSetParam(material_rid, 0, &opacity,
                                        sizeof(float));

        box_rid = render_server->MeshCreateBox("test_box", 2.0f, 2.0f, 2.0f);
        ARHUD_LOG_INFO("Box mesh created: valid=%d", box_rid.IsValid());

        // 保留模式：创建服务端 SpatialItem + 相机/灯光（渲染走
        // RenderFromStorage）
        item_rid = render_server->InstanceCreate();
        render_server->InstanceSetMaterial(item_rid, material_rid);
        render_server->InstanceSetMesh(item_rid, box_rid);
        float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        render_server->InstanceSetTransform(item_rid, identity);
        render_server->InstanceSetRenderPriority(item_rid, 0);
        ARHUD_LOG_INFO("SpatialItem created: valid=%d", item_rid.IsValid());

        render_server->Camera3DSet(camera_3d);
        render_server->DirectionalLightSet(light);

        resources_created = true;
        ARHUD_LOG_INFO("=== Initial resources created successfully ===");
      }

      // 第 60 帧起创建动态 3D 资源
      if (!dyn_created && frame_count == 60) {
        ARHUD_LOG_INFO("=== Creating dynamic resources via RenderServer ===");
        dyn_material_rid = render_server->MaterialCreate(
            "dyn_spatial_mat", RID(), MaterialDataType::kSpatial);
        float dyn_opacity = 1.0f;
        render_server->MaterialSetParam(dyn_material_rid, 0, &dyn_opacity,
                                        sizeof(float));
        dyn_sphere_rid =
            render_server->MeshCreateSphere("dyn_sphere", 1.0f, 16, 16);
        ARHUD_LOG_INFO("Dynamic resources created: mat=%d mesh=%d",
                       dyn_material_rid.IsValid(), dyn_sphere_rid.IsValid());

        // 保留模式：动态 SpatialItem
        dyn_item_rid = render_server->InstanceCreate();
        render_server->InstanceSetMaterial(dyn_item_rid, dyn_material_rid);
        render_server->InstanceSetMesh(dyn_item_rid, dyn_sphere_rid);
        float dyn_transform[16] = {1, 0, 0, 0, 0,    1,    0,    0,
                                   0, 0, 1, 0, 4.0f, 0.0f, 0.0f, 1};
        render_server->InstanceSetTransform(dyn_item_rid, dyn_transform);
        render_server->InstanceSetRenderPriority(dyn_item_rid, 1);
        ARHUD_LOG_INFO("Dynamic SpatialItem created: valid=%d",
                       dyn_item_rid.IsValid());

        dyn_created = true;
      }

      // B3: render thread calls RenderFromStorage internally (no main-thread
      // render)

      render_server->SubmitScene();  // B3: frame submit signal (async, returns
                                     // immediately)

      engine.EndFrame();
      // GPU 同步
      // sleep 补足 16ms，维持 60 FPS
      // 帧时长节流
      auto frame_end = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          frame_end - frame_start);
      if (elapsed < kFrameDuration) {
        std::this_thread::sleep_for(kFrameDuration - elapsed);
      }
      ++frame_count;
    }

    ARHUD_LOG_INFO("Render loop exited after %u frames", frame_count);

    if (resources_created) {
      render_server->InstanceFree(item_rid);
      render_server->MeshFree(box_rid);
      render_server->MaterialFree(material_rid);
    }

    if (dyn_created) {
      render_server->InstanceFree(dyn_item_rid);
      render_server->MeshFree(dyn_sphere_rid);
      render_server->MaterialFree(dyn_material_rid);
    }

    engine.Finalize();

    ARHUD_LOG_INFO("=== Phase 4 Spatial Demo exited cleanly ===");
  }

  ShutdownLogger();

#if ARHUD_DEBUG
  arhud::memory::DumpLeaks();
#endif

  return 0;
}
