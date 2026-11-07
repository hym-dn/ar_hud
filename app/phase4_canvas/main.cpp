/**
 * @file main.cpp
 * @brief Phase 4 Canvas Demo — 2D Canvas Rendering Validation
 *
 * @par Goal
 *   Validate the 2D Canvas rendering pipeline end-to-end:
 *   Compositor → CanvasRenderer → Texture + Material + Mesh Storage
 *
 * @par Test content
 *   1. Initialize Compositor (all Storage + Renderer)
 *   2. Create a 2×2 checkerboard texture (procedural)
 *   3. Create a CanvasMaterial with the texture
 *   4. Create a Quad mesh (200×200 pixels)
 *   5. Push CanvasItem and render via Compositor
 *   6. Verify: colored quad appears on screen
 *
 * @author yameng.he
 * @version 2.0
 * @date 2026-08-21
 * @copyright Copyright (c) 2025 ARHud Project
 */

#include <chrono>
#include <cmath>
#include <thread>

#include "arhud_engine.h"

using namespace arhud;

static constexpr uint32_t kWindowWidth = 1280;
static constexpr uint32_t kWindowHeight = 720;

static uint8_t s_checkerboard_pixels[4 * 4] = {
    255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255,
};

/** @brief 绿色像素数据（动态创建纹理用） */
static uint8_t s_green_pixels[4] = {0, 255, 0, 255};

int main() {
  InitializeLogger();
  ARHUD_LOG_INFO("=== Phase 4 Canvas Demo ===");

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
    screen_cfg.title = "ARHud - Phase 4 Canvas Demo";
    screen_cfg.width = kWindowWidth;
    screen_cfg.height = kWindowHeight;
    engine.AddScreen(screen_cfg);
    ARHUD_LOG_INFO("ARHudEngine initialized");

    RenderViewport* render_viewport = engine.GetScreen(0);
    RenderingServer* render_server = engine.GetRenderServer();

    // 初始资源 RID
    RID texture_rid;
    RID material_rid;
    RID quad_rid;
    RID item_rid;
    bool resources_created = false;

    // CommandQueue 动态资源 RID
    RID dyn_texture_rid;
    RID dyn_material_rid;
    RID dyn_quad_rid;
    RID dyn_item_rid;
    bool dyn_created = false;

    // ADR-0005: 树形管理用例（面板 → 子图标；整体移动/淡入淡出沿父链传播）
    RID tree_panel_rid;
    RID tree_child_rid;
    bool tree_created = false;

    uint32_t frame_count = 0;
    constexpr uint32_t kTargetFPS = 60;
    constexpr auto kFrameDuration =
        std::chrono::microseconds(1'000'000 / kTargetFPS);
    while (!engine.ShouldClose() && frame_count < 6000) {
      auto frame_start = std::chrono::steady_clock::now();
      engine.BeginFrame();

      // CommandQueue 异步提交，GL 调用全部在渲染线程
      // BeginFrame 唤醒渲染线程消费 CommandQueue
      // 需要 RID 的资源创建走 RenderServer 的 RID 分裂：主线程先分配
      // RID，初始化异步入队
      if (!resources_created) {
        ARHUD_LOG_INFO("=== Creating initial resources via RenderServer ===");

        texture_rid = render_server->TextureCreateFromData(
            "checkerboard", s_checkerboard_pixels,
            sizeof(s_checkerboard_pixels), 2, 2, DataFormat::kR8G8B8A8Unorm);
        ARHUD_LOG_INFO("Texture created: valid=%d", texture_rid.IsValid());

        material_rid = render_server->MaterialCreate("canvas_mat", RID(),
                                                     MaterialDataType::kCanvas);
        ARHUD_LOG_INFO("Material created: valid=%d", material_rid.IsValid());

        float opacity = 1.0f;
        render_server->MaterialSetParam(material_rid, 0, &opacity,
                                        sizeof(float));
        render_server->MaterialSetTexture(material_rid, 0, texture_rid);
        ARHUD_LOG_INFO("Material uniform+texture set");

        quad_rid = render_server->MeshCreateQuad("test_quad", 200.0f, 200.0f);
        ARHUD_LOG_INFO("Quad mesh created: valid=%d", quad_rid.IsValid());

        // 保留模式：创建服务端 CanvasItem（渲染走 RenderFromStorage）
        item_rid = render_server->CanvasItemCreate();
        render_server->CanvasItemSetMaterial(item_rid, material_rid);
        render_server->CanvasItemSetMesh(item_rid, quad_rid);
        float identity[16] = {1,
                              0,
                              0,
                              0,
                              0,
                              1,
                              0,
                              0,
                              0,
                              0,
                              1,
                              0,
                              static_cast<float>(kWindowWidth) / 2 - 100,
                              static_cast<float>(kWindowHeight) / 2 - 100,
                              0,
                              1};
        render_server->CanvasItemSetTransform(item_rid, identity);
        render_server->CanvasItemSetOpacity(item_rid, 1.0f);
        render_server->CanvasItemSetZOrder(item_rid, 0);
        render_server->CanvasItemSetLayer(item_rid, 0);
        ARHUD_LOG_INFO("CanvasItem created: valid=%d", item_rid.IsValid());

        resources_created = true;
        ARHUD_LOG_INFO("=== Initial resources created successfully ===");
      }

      // 第 60 帧：动态创建 HUD 资源
      if (!dyn_created && frame_count == 60) {
        ARHUD_LOG_INFO("=== Creating dynamic resources via RenderServer ===");
        dyn_texture_rid = render_server->TextureCreateFromData(
            "dyn_green", s_green_pixels, sizeof(s_green_pixels), 1, 1,
            DataFormat::kR8G8B8A8Unorm);
        dyn_material_rid = render_server->MaterialCreate(
            "dyn_mat", RID(), MaterialDataType::kCanvas);
        float dyn_opacity = 0.9f;
        render_server->MaterialSetParam(dyn_material_rid, 0, &dyn_opacity,
                                        sizeof(float));
        render_server->MaterialSetTexture(dyn_material_rid, 0, dyn_texture_rid);
        dyn_quad_rid =
            render_server->MeshCreateQuad("dyn_quad", 100.0f, 100.0f);
        ARHUD_LOG_INFO("Dynamic resources created: tex=%d mat=%d mesh=%d",
                       dyn_texture_rid.IsValid(), dyn_material_rid.IsValid(),
                       dyn_quad_rid.IsValid());

        // 保留模式：动态 CanvasItem（layer 1）
        dyn_item_rid = render_server->CanvasItemCreate();
        render_server->CanvasItemSetMaterial(dyn_item_rid, dyn_material_rid);
        render_server->CanvasItemSetMesh(dyn_item_rid, dyn_quad_rid);
        float dyn_transform[16] = {1, 0, 0, 0, 0,      1,      0, 0,
                                   0, 0, 1, 0, 800.0f, 400.0f, 0, 1};
        render_server->CanvasItemSetTransform(dyn_item_rid, dyn_transform);
        render_server->CanvasItemSetOpacity(dyn_item_rid, 0.9f);
        render_server->CanvasItemSetZOrder(dyn_item_rid, 1);
        render_server->CanvasItemSetLayer(dyn_item_rid, 1);
        ARHUD_LOG_INFO("Dynamic CanvasItem created: valid=%d",
                       dyn_item_rid.IsValid());

        dyn_created = true;
      }

      // ADR-0005: 树形用例（frame 120）：面板（棋盘格 200x200）挂一个子图标
      // （绿 100x100，局部偏移 (60,60)，z 相对面板 +1）。面板移动 → 子跟随；
      // 面板透明度 → 子透明度跟随（opacity 沿父链连乘）。
      if (!tree_created && frame_count == 120) {
        ARHUD_LOG_INFO(
            "=== Creating tree hierarchy case (panel -> child icon) ===");
        tree_panel_rid = render_server->CanvasItemCreate();
        render_server->CanvasItemSetMaterial(tree_panel_rid, material_rid);
        render_server->CanvasItemSetMesh(tree_panel_rid, quad_rid);
        float panel_transform[16] = {1, 0, 0, 0, 0,      1,      0, 0,
                                     0, 0, 1, 0, 640.0f, 360.0f, 0, 1};
        render_server->CanvasItemSetTransform(tree_panel_rid, panel_transform);
        render_server->CanvasItemSetOpacity(tree_panel_rid, 1.0f);
        render_server->CanvasItemSetZOrder(tree_panel_rid, 2);
        render_server->CanvasItemSetLayer(tree_panel_rid, 1);

        tree_child_rid = render_server->CanvasItemCreate();
        render_server->CanvasItemSetMaterial(tree_child_rid, dyn_material_rid);
        render_server->CanvasItemSetMesh(tree_child_rid, dyn_quad_rid);
        float child_local[16] = {1, 0, 0, 0, 0,     1,     0, 0,
                                 0, 0, 1, 0, 60.0f, 60.0f, 0, 1};
        render_server->CanvasItemSetTransform(tree_child_rid, child_local);
        render_server->CanvasItemSetZOrder(
            tree_child_rid, 1);  // 相对面板 +1（z_as_relative 默认 true）
        render_server->CanvasItemSetLayer(tree_child_rid, 1);
        render_server->CanvasItemSetParent(tree_child_rid, tree_panel_rid);
        ARHUD_LOG_INFO(
            "Tree case created: panel=%d child=%d (child follows panel "
            "transform/opacity/z)",
            tree_panel_rid.IsValid(), tree_child_rid.IsValid());

        tree_created = true;
      }

      // ADR-0005: 面板动画——小圆轨迹移动 + 整组呼吸淡入淡出
      // （只改面板的 transform/opacity，子 item 命令零开销——树传播的价值所在）
      if (tree_created && frame_count > 120) {
        const float t = static_cast<float>(frame_count) / 60.0f;
        float panel_transform[16] = {1,
                                     0,
                                     0,
                                     0,
                                     0,
                                     1,
                                     0,
                                     0,
                                     0,
                                     0,
                                     1,
                                     0,
                                     640.0f + 120.0f * std::sin(t),
                                     360.0f + 80.0f * std::cos(t),
                                     0,
                                     1};
        render_server->CanvasItemSetTransform(tree_panel_rid, panel_transform);
        render_server->CanvasItemSetOpacity(tree_panel_rid,
                                            0.55f + 0.45f * std::sin(t * 2.0f));
      }

      // B3: render thread calls RenderFromStorage internally (no main-thread
      // render)

      render_server->SubmitScene();  // B3: frame submit signal (async, returns
                                     // immediately)

      engine.EndFrame();

      // GPU 同步
      // sleep 补足到 16ms，稳定 60 FPS
      // 帧率控制
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
      render_server->CanvasItemFree(item_rid);
      render_server->MeshFree(quad_rid);
      render_server->MaterialFree(material_rid);
      render_server->TextureFree(texture_rid);
    }

    if (dyn_created) {
      render_server->CanvasItemFree(dyn_item_rid);
      render_server->MeshFree(dyn_quad_rid);
      render_server->MaterialFree(dyn_material_rid);
      render_server->TextureFree(dyn_texture_rid);
    }

    if (tree_created) {
      // 先 free 面板：子 item 自动摘到根（ADR-0005 Free 不级联语义），再 free
      // 子
      render_server->CanvasItemFree(tree_panel_rid);
      render_server->CanvasItemFree(tree_child_rid);
    }

    engine.Finalize();

    ARHUD_LOG_INFO("=== Phase 4 Canvas Demo exited cleanly ===");
  }

  ShutdownLogger();

#if ARHUD_DEBUG
  arhud::memory::DumpLeaks();
#endif

  return 0;
}
