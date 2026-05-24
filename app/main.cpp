/**
 * @file main.cpp
 * @brief ARHud 引擎最小可运行 Demo — 端到端验证 RCD→RDD→RD 管线
 *
 * @par 目标
 *   创建窗口 → 初始化 GLManager → 初始化 RCD → 创建 RDD → 初始化 RD
 *   → 清屏循环 → SwapBuffers，验证整个渲染管线能跑通。
 *
 * @par 初始化顺序
 *   @code
 *   1. DisplayServer::Initialize()     — 枚举屏幕
 *   2. DisplayServer::WindowCreate()   — 创建窗口
 *   3. GLManager::Initialize()         — 加载 OpenGL 驱动
 *   4. RCD::Initialize()               — 创建引导 Surface/Context + 加载 GL 函数
 *   5. RD::Initialize(RCD)             — 内部创建 RDD + 初始化帧循环
 *   6. RD::MakeCurrent()               — 绑定 SwapChain
 *   @endcode
 *
 * @par 渲染循环
 *   @code
 *   while (!should_close)
 *       ProcessEvents()
 *       RD::BeginFrame()
 *       CommandBuffer: SetViewport + SetScissor + SetBlendConstants
 *       glClear(GL_COLOR_BUFFER_BIT)   — 彩色清屏
 *       RD::EndFrame()
 *       RD::SwapBuffers()
 *   @endcode
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-23
 * @copyright Copyright (c) 2025 ARHud Project
 */

#include "typedefs.h"
#include "io/logger.h"
#include "screen.h"
#include "window.h"
#include "display_server.h"
#include "gl_manager.h"
#include "rendering_context_driver.h"
#include "rendering_device.h"
#include "rendering_context_driver_gl.h"
#include "storage/shader_storage.h"

#include <glad/gl.h>
#include <cstdio>

#ifdef ARHUD_PLATFORM_WINDOWS
#include "display_server_win32.h"
#include "gl_manager_win32.h"
#endif

using namespace arhud;

static constexpr float kClearColorR = 0.05f;
static constexpr float kClearColorG = 0.07f;
static constexpr float kClearColorB = 0.15f;
static constexpr float kClearColorA = 1.0f;

static constexpr uint32_t kWindowWidth = 1280;
static constexpr uint32_t kWindowHeight = 720;

int main()
{
    InitializeLogger();
    ARHUD_LOG_INFO("=== ARHud Engine Minimal Demo ===");

    // ═══════════════════════════════════════════════════════════════════════
    // 1. 创建并初始化 DisplayServer
    // ═══════════════════════════════════════════════════════════════════════
    Win32DisplayServer display_server;
    Error err = display_server.Initialize();
    if (err != Error::kOK)
    {
        ARHUD_LOG_ERROR("kFailed", "Failed to initialize DisplayServer");
        return 1;
    }
    ARHUD_LOG_INFO("DisplayServer initialized, screens: %u",
                    display_server.GetScreenCount());

    // ═══════════════════════════════════════════════════════════════════════
    // 2. 创建窗口
    // ═══════════════════════════════════════════════════════════════════════
    WindowDesc window_desc;
    window_desc.width = kWindowWidth;
    window_desc.height = kWindowHeight;
    window_desc.title = "ARHud Engine - Minimal Demo";

    IWindow::WindowID window_id = display_server.WindowCreate(window_desc);
    if (window_id == IWindow::kInvalidWindowId)
    {
        ARHUD_LOG_ERROR("kFailed", "Failed to create window");
        display_server.Shutdown();
        return 1;
    }

    IWindow *window = display_server.GetWindow(window_id);
    void *native_handle = window->GetNativeHandle();
    ARHUD_LOG_INFO("Window created (ID=%u, %ux%u)", window_id,
                    window_desc.width, window_desc.height);

    // ═══════════════════════════════════════════════════════════════════════
    // 3. 创建并初始化 GLManager
    // ═══════════════════════════════════════════════════════════════════════
    Win32GLManager gl_manager;
    err = gl_manager.Initialize();
    if (err != Error::kOK)
    {
        ARHUD_LOG_ERROR("kFailed", "Failed to initialize GLManager");
        display_server.WindowDestroy(window_id);
        display_server.Shutdown();
        return 1;
    }
    ARHUD_LOG_INFO("GLManager initialized");

    // ═══════════════════════════════════════════════════════════════════════
    // 4. 初始化 RCD — 内部创建引导 Surface/Context + 加载 GL 函数
    // ═══════════════════════════════════════════════════════════════════════
    IScreen::ScreenID primary_screen_id = display_server.GetPrimaryScreen();
    IScreen *screen = display_server.GetScreen(primary_screen_id);
    if (!screen)
    {
        ARHUD_LOG_ERROR("kFailed", "Failed to get primary screen");
        gl_manager.Shutdown();
        display_server.WindowDestroy(window_id);
        display_server.Shutdown();
        return 1;
    }

    GLRenderingContextDriver context_driver(&gl_manager);
    err = context_driver.Initialize(screen->GetId(), native_handle,
                                    kWindowWidth, kWindowHeight);
    if (err != Error::kOK)
    {
        ARHUD_LOG_ERROR("kFailed", "Failed to initialize RCD");
        gl_manager.Shutdown();
        display_server.WindowDestroy(window_id);
        display_server.Shutdown();
        return 1;
    }

    constexpr IRenderingContextDriver::SurfaceID kRcdSurface = 0;
    constexpr IRenderingContextDriver::ContextID kRcdContext = 0;
    ARHUD_LOG_INFO("RCD initialized (devices: %u, surface: %u, context: %u)",
                    context_driver.GetDeviceCount(), kRcdSurface, kRcdContext);

    // ═══════════════════════════════════════════════════════════════════════
    // 5. 初始化 RD — 内部创建 RDD + 初始化帧循环
    // ═══════════════════════════════════════════════════════════════════════
    RenderingDevice rd;
    err = rd.Initialize(&context_driver);
    if (err != Error::kOK)
    {
        ARHUD_LOG_ERROR("kFailed", "Failed to initialize RD");
        context_driver.Shutdown();
        gl_manager.Shutdown();
        display_server.WindowDestroy(window_id);
        display_server.Shutdown();
        return 1;
    }
    ARHUD_LOG_INFO("RD initialized — pipeline ready");

    // ═══════════════════════════════════════════════════════════════════════
    // 6. 绑定 SwapChain
    // ═══════════════════════════════════════════════════════════════════════
    err = rd.MakeCurrent(kRcdContext, kRcdSurface);
    if (err != Error::kOK)
    {
        ARHUD_LOG_ERROR("kFailed", "Failed to bind SwapChain");
        rd.Finalize();
        context_driver.Shutdown();
        gl_manager.Shutdown();
        display_server.WindowDestroy(window_id);
        display_server.Shutdown();
        return 1;
    }
    ARHUD_LOG_INFO("SwapChain bound — entering render loop");

    // ═══════════════════════════════════════════════════════════════════════
    // 7. ShaderStorage 测试 — 编译 GLSL Shader + Uniform 反射
    // ═══════════════════════════════════════════════════════════════════════
    ShaderStorage shader_storage;
    err = shader_storage.Initialize(&rd);
    if (err != Error::kOK)
    {
        ARHUD_LOG_ERROR("kFailed", "Failed to initialize ShaderStorage");
    }
    else
    {
        ARHUD_LOG_INFO("ShaderStorage initialized");

        const char *kVertSrc = R"glsl(
#version 450 core
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_color = a_color;
}
)glsl";

        const char *kFragSrc = R"glsl(
#version 450 core
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 frag_color;
void main() {
    frag_color = v_color;
}
)glsl";

        LocalVector<ShaderUniform> uniforms;

        LocalVector<ShaderStageSource> stages;
        stages.PushBack({ShaderStage::kVertex, kVertSrc});
        stages.PushBack({ShaderStage::kFragment, kFragSrc});

        RID shader_rid = shader_storage.ShaderCreateFromSource(
            "test_passthrough", stages, uniforms, 0);

        if (shader_rid.IsValid())
        {
            const ShaderInfo *info = shader_storage.ShaderGetInfo(shader_rid);
            ARHUD_LOG_INFO("Shader '%s' compiled: valid=%d, uniforms=%u, push_const=%u",
                           info->name.CStr(),
                           shader_storage.ShaderIsValid(shader_rid),
                           shader_storage.ShaderGetUniforms(shader_rid).Size(),
                           shader_storage.ShaderGetPushConstantSize(shader_rid));

            RDShaderID rd_id = shader_storage.ShaderGetRDId(shader_rid);
            ARHUD_LOG_INFO("Shader RD ID: valid=%d", rd_id.IsValid());

            shader_storage.ShaderFree(shader_rid);
            ARHUD_LOG_INFO("Shader freed");
        }
        else
        {
            ARHUD_LOG_ERROR("kFailed", "Shader compilation failed");
        }

        shader_storage.Finalize();
        ARHUD_LOG_INFO("ShaderStorage finalized");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 8. DrawList 测试 — 高层绘制 API 验证
    // ═══════════════════════════════════════════════════════════════════════
    DrawListID dl = rd.DrawListBegin();
    if (dl != kInvalidDrawListId)
    {
        rd.DrawListSetViewport(dl, 0, 0, kWindowWidth, kWindowHeight);
        rd.DrawListSetScissor(dl, 0, 0, kWindowWidth, kWindowHeight);
        rd.DrawListSetBlendConstants(dl, 0.0f, 0.0f, 0.0f, 0.0f);
        rd.DrawListEnd();
        ARHUD_LOG_INFO("DrawList: begin → set viewport/scissor/blend → end");
    }
    else
    {
        ARHUD_LOG_WARN("DrawList: begin failed, skipping test");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 9. 渲染循环 — DrawList + 彩色清屏
    // ═══════════════════════════════════════════════════════════════════════
    uint32_t frame_count = 0;

    while (!window->ShouldClose() && frame_count < 120)
    {
        display_server.ProcessEvents();

        rd.BeginFrame();

        DrawListID frame_dl = rd.DrawListBegin();
        if (frame_dl != kInvalidDrawListId)
        {
            rd.DrawListSetViewport(frame_dl, 0, 0, kWindowWidth, kWindowHeight);
            rd.DrawListSetScissor(frame_dl, 0, 0, kWindowWidth, kWindowHeight);
            rd.DrawListEnd();
        }

        glClearColor(kClearColorR, kClearColorG, kClearColorB, kClearColorA);
        glClear(GL_COLOR_BUFFER_BIT);

        rd.EndFrame();
        rd.SwapBuffers(kRcdSurface);

        ++frame_count;
    }

    ARHUD_LOG_INFO("Render loop exited after %u frames", frame_count);

    // ═══════════════════════════════════════════════════════════════════════
    // 10. 清理 — 按初始化的逆序释放
    // ═══════════════════════════════════════════════════════════════════════
    rd.Finalize();
    ARHUD_LOG_INFO("RD finalized (RDD freed internally)");

    context_driver.Shutdown();
    ARHUD_LOG_INFO("RCD shutdown");

    gl_manager.Shutdown();
    ARHUD_LOG_INFO("GLManager shutdown");

    display_server.WindowDestroy(window_id);
    display_server.Shutdown();
    ARHUD_LOG_INFO("DisplayServer shutdown");

    ARHUD_LOG_INFO("=== ARHud Engine Minimal Demo exited cleanly ===");
    ShutdownLogger();
    return 0;
}
