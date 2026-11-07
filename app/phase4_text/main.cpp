/**
 * @file main.cpp
 * @brief Phase 4 Text Demo — SDF Text Rendering Validation
 *
 * @par Goal
 *   Validate SDF text rendering pipeline:
 *   FontStorage::FontLoadFromFreeType 到 CanvasRenderer::RenderText 的渲染链路
 *
 * @par Test content
 *   1. Initialize ARHudEngine (all Storage + Renderer)
 *   2. Load a TTF font via FontStorage::FontLoadFromFreeType
 *   3. Render "Hello AR HUD" text using CanvasRenderer::RenderText
 *   4. Also render a background quad via CanvasItem for visual reference
 *   5. Verify: text rendered clearly with SDF anti-aliasing
 *
 * @note Requires FreeType dependency (conan install or system install)
 *
 * @author yameng.he
 * @version 2.0
 * @date 2026-08-21
 * @copyright Copyright (c) 2025 ARHud Project
 */

#include <chrono>
#include <cstdio>
#include <thread>

#include "arhud_engine.h"

using namespace arhud;

static constexpr uint32_t kWindowWidth = 1280;
static constexpr uint32_t kWindowHeight = 720;

/** @brief 1x1 白色像素数据 */
static uint8_t s_white_pixels[4] = {255, 255, 255, 255};

//  UTF-32 字符码（Hello AR HUD）
static const uint32_t kTextHello[] = {'H', 'e', 'l', 'l', 'o', ' ',
                                      'A', 'R', ' ', 'H', 'U', 'D'};
static constexpr uint32_t kTextHelloLength =
    sizeof(kTextHello) / sizeof(kTextHello[0]);

//  ASCII 字符码表（32-126）
static LocalVector<uint32_t> BuildAsciiCharcodes() {
  LocalVector<uint32_t> codes;
  codes.Reserve(95);
  for (uint32_t c = 32; c < 127; ++c) {
    codes.PushBack(c);
  }
  return codes;
}

/**
 * @brief 查找 Windows 系统字体路径
 *
 * 候选字体路径：consola / arial / msyh / tahoma */
static const char* FindSystemFontPath() {
#ifdef ARHUD_PLATFORM_WINDOWS
  // Windows 平台下的候选字体路径
  static const char* kFontPaths[] = {
      "C:\\Windows\\Fonts\\consola.ttf",  // Consolas
      "C:\\Windows\\Fonts\\arial.ttf",    // Arial
      "C:\\Windows\\Fonts\\msyh.ttc",     //
      "C:\\Windows\\Fonts\\tahoma.ttf",   // Tahoma
  };
  for (const char* path : kFontPaths) {
    FILE* f = nullptr;
    fopen_s(&f, path, "rb");
    if (f != nullptr) {
      fclose(f);
      return path;
    }
  }
#endif
  return nullptr;
}

int main() {
  InitializeLogger();
  ARHUD_LOG_INFO("=== Phase 4 Text Demo ===");

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
    screen_cfg.title = "ARHud - Phase 4 Text Demo (SDF)";
    screen_cfg.width = kWindowWidth;
    screen_cfg.height = kWindowHeight;
    engine.AddScreen(screen_cfg);
    ARHUD_LOG_INFO("ARHudEngine initialized");

    RenderViewport* render_viewport = engine.GetScreen(0);
    RenderingServer* render_server = engine.GetRenderServer();

    Transform2D camera_2d;

    //  资源 RID
    RID font_rid;
    RID bg_texture_rid;
    RID bg_material_rid;
    RID bg_quad_rid;
    RID bg_item_rid;
    RID text_item_rid;
    bool resources_created = false;

    uint32_t frame_count = 0;
    constexpr uint32_t kTargetFPS = 60;
    constexpr auto kFrameDuration =
        std::chrono::microseconds(1'000'000 / kTargetFPS);
    while (!engine.ShouldClose() && frame_count < 600) {
      auto frame_start = std::chrono::steady_clock::now();

      engine.BeginFrame();

      //  首次进入时创建资源
      if (!resources_created) {
        ARHUD_LOG_INFO("=== Creating resources ===");

        // 3a. 通过 FreeType 加载字体
        const char* font_path = FindSystemFontPath();
        if (font_path == nullptr) {
          ARHUD_LOG_ERROR("kFailed", "No system font found!");
        } else {
          ARHUD_LOG_INFO("Loading font from: %s", font_path);
          LocalVector<uint32_t> charcodes = BuildAsciiCharcodes();
          font_rid = render_server->FontCreateFromFreeType(
              "demo_font", font_path,
              48.0f,  // 字号 48px
              charcodes.Data(), charcodes.Size(),
              8.0f);  // SDF spread = 8px

          if (font_rid.IsNull()) {
            ARHUD_LOG_ERROR("kFailed", "FontLoadFromFreeType failed");
          } else {
            ARHUD_LOG_INFO("Font loaded: valid=%d", font_rid.IsValid());
          }
        }

        // 3b.
        bg_texture_rid = render_server->TextureCreateFromData(
            "bg_white", s_white_pixels, sizeof(s_white_pixels), 1, 1,
            DataFormat::kR8G8B8A8Unorm);

        bg_material_rid = render_server->MaterialCreate(
            "bg_mat", RID(), MaterialDataType::kCanvas);

        float opacity = 0.6f;
        render_server->MaterialSetParam(bg_material_rid, 0, &opacity,
                                        sizeof(float));
        render_server->MaterialSetTexture(bg_material_rid, 0, bg_texture_rid);

        bg_quad_rid = render_server->MeshCreateQuad("bg_quad", 800.0f, 80.0f);

        // 保留模式：创建服务端 CanvasItem（渲染走 RenderFromStorage）
        bg_item_rid = render_server->CanvasItemCreate();
        render_server->CanvasItemSetMaterial(bg_item_rid, bg_material_rid);
        render_server->CanvasItemSetMesh(bg_item_rid, bg_quad_rid);
        float bg_x = static_cast<float>(kWindowWidth) / 2 - 400;
        float bg_y = static_cast<float>(kWindowHeight) / 2 - 40;
        float bg_transform[16] = {1, 0, 0, 0, 0,    1,    0, 0,
                                  0, 0, 1, 0, bg_x, bg_y, 0, 1};
        render_server->CanvasItemSetTransform(bg_item_rid, bg_transform);
        render_server->CanvasItemSetOpacity(bg_item_rid, 0.6f);
        render_server->CanvasItemSetZOrder(bg_item_rid, 0);
        render_server->CanvasItemSetLayer(bg_item_rid, 0);

        // 保留模式：TextItem（z_order=1
        if (!font_rid.IsNull()) {
          text_item_rid = render_server->TextItemCreate();
          render_server->TextItemSetFont(text_item_rid, font_rid);
          render_server->TextItemSetText(text_item_rid, kTextHello,
                                         kTextHelloLength);
          render_server->TextItemSetPosition(
              text_item_rid, static_cast<float>(kWindowWidth) / 2 - 180,
              static_cast<float>(kWindowHeight) / 2 + 10);
          float text_color[4] = {1.0f, 1.0f, 0.0f, 1.0f};
          render_server->TextItemSetColor(text_item_rid, text_color);
          render_server->TextItemSetZOrder(text_item_rid, 1);
          render_server->TextItemSetLayer(text_item_rid, 0);
        }

        render_server->Camera2DSet(camera_2d);

        resources_created = true;
        ARHUD_LOG_INFO("=== Resources created ===");
      }

      //  4. B3: render thread calls RenderFromStorage internally (no
      //  main-thread render)
      render_server->SubmitScene();  // B3: frame submit signal (async, returns
                                     // immediately)

      engine.EndFrame();

      // GPU 同步
      //  休眠补齐至 16ms，维持 60 FPS
      //  计算本帧耗时
      auto frame_end = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          frame_end - frame_start);
      if (elapsed < kFrameDuration) {
        std::this_thread::sleep_for(kFrameDuration - elapsed);
      }

      ++frame_count;
    }

    ARHUD_LOG_INFO("Render loop exited after %u frames", frame_count);

    //  5.
    if (resources_created) {
      if (!text_item_rid.IsNull()) {
        render_server->TextItemFree(text_item_rid);
      }
      if (!bg_item_rid.IsNull()) {
        render_server->CanvasItemFree(bg_item_rid);
      }
      if (!font_rid.IsNull()) {
        render_server->FontFree(font_rid);
      }
      render_server->MeshFree(bg_quad_rid);
      render_server->MaterialFree(bg_material_rid);
      render_server->TextureFree(bg_texture_rid);
    }

    engine.Finalize();

    ARHUD_LOG_INFO("=== Phase 4 Text Demo exited cleanly ===");
  }

  ShutdownLogger();

#if ARHUD_DEBUG
  arhud::memory::DumpLeaks();
#endif

  return 0;
}
