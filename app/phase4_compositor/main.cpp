/**
 * @file main.cpp
 * @brief Phase 4 Compositor Demo — 2D/3D Mixed Rendering Validation
 *
 * @par Goal
 *   Validate the full rendering pipeline with Compositor:
 *   Compositor — SpatialRenderer (3D) + CanvasRenderer (2D layers)
 *
 * @par Test content
 *   1. Initialize Compositor (all Storage + Renderer)
 *   2. Create 3D scene (Box + DirectionalLight)
 *   3. Create 2D HUD layer (Quad with checkerboard texture)
 *   4. Render via Compositor: Phase 1 (3D, clear) -> Phase 2 (2D layer,
 * overlay)
 *   5. Verify: 3D box with 2D HUD overlay
 *
 * @par Rendering flow (cf. Godot RendererViewport::_draw_viewport):
 *   Phase 1: SpatialRenderer::RenderSpatialItems()  -> 3D (clear color+depth)
 *   Phase 2: CanvasRenderer::RenderCanvasItems() xN -> 2D layers (no clear)
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

static uint8_t s_checkerboard_pixels[4 * 4] = {
    255, 200, 0, 255, 0, 200, 255, 255, 255, 200, 0, 255, 0, 200, 255, 255,
};

/** @brief 绿色像素数据（动态创建纹理用） */
static uint8_t s_green_pixels[4] = {0, 255, 0, 255};

int main() {
  InitializeLogger();
  ARHUD_LOG_INFO("=== Phase 4 Compositor Demo ===");

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
    screen_cfg.title = "ARHud - Phase 4 Compositor Demo";
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

    Transform2D camera_2d;

    DirectionalLight light;
    light.direction[0] = -0.5f;
    light.direction[1] = -1.0f;
    light.direction[2] = -0.3f;
    light.color[0] = 1.0f;
    light.color[1] = 1.0f;
    light.color[2] = 1.0f;
    light.energy = 1.0f;

    // 初始资源 RID
    RID spatial_material_rid;
    RID box_rid;
    RID texture_rid;
    RID canvas_material_rid;
    RID quad_rid;
    RID spatial_item_rid;
    RID canvas_item_rid;
    bool resources_created = false;

    //  动态资源 RID
    RID dynamic_texture_rid;
    RID dynamic_material_rid;
    RID dynamic_quad_rid;
    RID dynamic_canvas_item_rid;
    bool dynamic_created = false;

    uint32_t frame_count = 0;
    constexpr uint32_t kTargetFPS = 60;
    constexpr auto kFrameDuration =
        std::chrono::microseconds(1'000'000 / kTargetFPS);

    while (!engine.ShouldClose() && frame_count < 600) {
      auto frame_start = std::chrono::steady_clock::now();

      engine.BeginFrame();

      //  通过 RenderServer 创建初始 GL 资源
      if (!resources_created) {
        ARHUD_LOG_INFO("=== Creating initial resources via RenderServer ===");

        spatial_material_rid = render_server->MaterialCreate(
            "spatial_mat", RID(), MaterialDataType::kSpatial);
        float spatial_opacity = 1.0f;
        render_server->MaterialSetParam(spatial_material_rid, 0,
                                        &spatial_opacity, sizeof(float));

        box_rid = render_server->MeshCreateBox("test_box", 2.0f, 2.0f, 2.0f);
        ARHUD_LOG_INFO("3D: Box mesh created");

        texture_rid = render_server->TextureCreateFromData(
            "hud_texture", s_checkerboard_pixels, sizeof(s_checkerboard_pixels),
            2, 2, DataFormat::kR8G8B8A8Unorm);

        canvas_material_rid = render_server->MaterialCreate(
            "canvas_mat", RID(), MaterialDataType::kCanvas);
        float canvas_opacity = 0.8f;
        render_server->MaterialSetParam(canvas_material_rid, 0, &canvas_opacity,
                                        sizeof(float));
        render_server->MaterialSetTexture(canvas_material_rid, 0, texture_rid);

        quad_rid = render_server->MeshCreateQuad("hud_quad", 150.0f, 150.0f);
        ARHUD_LOG_INFO("2D: Quad mesh + texture + material created");

        // 保留模式：创建服务端 SpatialItem + CanvasItem + 相机/灯光
        spatial_item_rid = render_server->InstanceCreate();
        render_server->InstanceSetMaterial(spatial_item_rid,
                                           spatial_material_rid);
        render_server->InstanceSetMesh(spatial_item_rid, box_rid);
        float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        render_server->InstanceSetTransform(spatial_item_rid, identity);
        render_server->InstanceSetRenderPriority(spatial_item_rid, 0);

        canvas_item_rid = render_server->CanvasItemCreate();
        render_server->CanvasItemSetMaterial(canvas_item_rid,
                                             canvas_material_rid);
        render_server->CanvasItemSetMesh(canvas_item_rid, quad_rid);
        float canvas_transform[16] = {1, 0, 0, 0, 0,     1,     0, 0,
                                      0, 0, 1, 0, 50.0f, 50.0f, 0, 1};
        render_server->CanvasItemSetTransform(canvas_item_rid,
                                              canvas_transform);
        render_server->CanvasItemSetOpacity(canvas_item_rid, 0.8f);
        render_server->CanvasItemSetZOrder(canvas_item_rid, 0);
        render_server->CanvasItemSetLayer(canvas_item_rid, 0);
        ARHUD_LOG_INFO("Items created: spatial=%d canvas=%d",
                       spatial_item_rid.IsValid(), canvas_item_rid.IsValid());

        render_server->Camera3DSet(camera_3d);
        render_server->Camera2DSet(camera_2d);
        render_server->DirectionalLightSet(light);

        resources_created = true;
        ARHUD_LOG_INFO("=== Initial resources created successfully ===");
      }

      //  第 60 帧：创建动态 HUD 资源
      if (!dynamic_created && frame_count == 60) {
        ARHUD_LOG_INFO("=== Creating dynamic resources via RenderServer ===");

        dynamic_texture_rid = render_server->TextureCreateFromData(
            "dynamic_green", s_green_pixels, sizeof(s_green_pixels), 1, 1,
            DataFormat::kR8G8B8A8Unorm);
        ARHUD_LOG_INFO("Dynamic texture created via RenderServer: valid=%d",
                       dynamic_texture_rid.IsValid());

        dynamic_material_rid = render_server->MaterialCreate(
            "dynamic_mat", RID(), MaterialDataType::kCanvas);
        float dyn_opacity = 0.9f;
        render_server->MaterialSetParam(dynamic_material_rid, 0, &dyn_opacity,
                                        sizeof(float));
        render_server->MaterialSetTexture(dynamic_material_rid, 0,
                                          dynamic_texture_rid);
        ARHUD_LOG_INFO("Dynamic material created via RenderServer: valid=%d",
                       dynamic_material_rid.IsValid());

        dynamic_quad_rid =
            render_server->MeshCreateQuad("dynamic_quad", 100.0f, 100.0f);
        ARHUD_LOG_INFO("Dynamic quad mesh created via RenderServer: valid=%d",
                       dynamic_quad_rid.IsValid());

        // 保留模式：动态 CanvasItem（layer 1）
        dynamic_canvas_item_rid = render_server->CanvasItemCreate();
        render_server->CanvasItemSetMaterial(dynamic_canvas_item_rid,
                                             dynamic_material_rid);
        render_server->CanvasItemSetMesh(dynamic_canvas_item_rid,
                                         dynamic_quad_rid);
        float dyn_transform[16] = {1, 0, 0, 0, 0,      1,      0, 0,
                                   0, 0, 1, 0, 800.0f, 400.0f, 0, 1};
        render_server->CanvasItemSetTransform(dynamic_canvas_item_rid,
                                              dyn_transform);
        render_server->CanvasItemSetOpacity(dynamic_canvas_item_rid, 0.9f);
        render_server->CanvasItemSetZOrder(dynamic_canvas_item_rid, 1);
        render_server->CanvasItemSetLayer(dynamic_canvas_item_rid, 1);
        ARHUD_LOG_INFO("Dynamic CanvasItem created: valid=%d",
                       dynamic_canvas_item_rid.IsValid());

        dynamic_created = true;
        ARHUD_LOG_INFO("=== Dynamic resources created successfully ===");
      }

      // B3: render thread calls RenderFromStorage internally (no main-thread
      // render)

      render_server->SubmitScene();  // B3: frame submit signal (async, returns
                                     // immediately)

      engine.EndFrame();

      // GPU 同步
      //  sleep 补足 16ms，锁定 60 FPS
      // 帧率限制
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
      render_server->CanvasItemFree(canvas_item_rid);
      render_server->InstanceFree(spatial_item_rid);
      render_server->MeshFree(box_rid);
      render_server->MeshFree(quad_rid);
      render_server->MaterialFree(spatial_material_rid);
      render_server->MaterialFree(canvas_material_rid);
      render_server->TextureFree(texture_rid);
    }

    if (dynamic_created) {
      render_server->CanvasItemFree(dynamic_canvas_item_rid);
      render_server->MeshFree(dynamic_quad_rid);
      render_server->MaterialFree(dynamic_material_rid);
      render_server->TextureFree(dynamic_texture_rid);
    }

    engine.Finalize();

    ARHUD_LOG_INFO("=== Phase 4 Compositor Demo exited cleanly ===");
  }

  ShutdownLogger();

#if ARHUD_DEBUG
  arhud::memory::DumpLeaks();
#endif

  return 0;
}
