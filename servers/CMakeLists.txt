# ===========================================================================
# Servers Library: arhud_servers
# ===========================================================================
#
# arhud_servers provides rendering device interfaces and infrastructure:
#   - Rendering device common types (rendering_device_commons.h)
#   - Rendering context driver interface (rendering_context_driver.h)
#   - Rendering device driver interface (rendering_device_driver.h)
#   - Storage layer (storage/): backend-agnostic resource storage via RD abstraction
#   - Renderer layer (renderer/): backend-agnostic CanvasRenderer, SpatialRenderer, Compositor
#   - Shaders (renderer/shaders/): GLSL source shared by GL and Vulkan
#
# Directory layout (reference: Godot servers/rendering/):
#   rendering/                          ← RD commons + device + driver interfaces
#   rendering/storage/                  ← Backend-agnostic storage (via RD abstraction)
#   rendering/renderer/                 ← Backend-agnostic renderer (via RD abstraction)
#   rendering/renderer/shaders/         ← GLSL sources (shared by GL/Vulkan backends)
#
# Dependency direction:
#   arhud_core (zero deps)
#       ^
#   arhud_platform (depends on core)
#       ^
#   arhud_servers (depends on core+platform)  ← This library
#       ^
#   arhud_drivers (depends on core+servers+platform)
#
# ===========================================================================

# --- Source file list ----------------------------------------------------
set(SERVERS_SOURCES
    rendering/rendering_device.cpp                                    # High-level rendering device (lifecycle + resource CRUD)
    rendering/rendering_device_frame.cpp                              # Frame loop + deferred destruction
    rendering/rendering_device_draw_list.cpp                          # DrawList + ComputeList
    rendering/rendering_device_staging.cpp                            # Staging Buffer + BufferUpdate + TextureUpdate
    rendering/command_queue.cpp                                       # Main→Render thread command proxy (≈ Godot CommandQueueMT)
    rendering/render_server.cpp                                       # B2: main-thread rendering API proxy
    rendering/rendering_server.cpp                                    # S0: abstract rendering API singleton (≈ Godot RenderingServer)
    rendering/rendering_server_default.cpp                            # S0: default impl (facade delegating to legacy RenderServer)
    rendering/render_viewport.cpp                                       # Per-screen render pipeline
    rendering/arhud_engine.cpp                                        # Multi-screen rendering engine
    rendering/storage/texture_storage.cpp                             # Texture resource storage
    rendering/storage/shader_storage.cpp                              # Shader resource storage
    rendering/storage/material_storage.cpp                            # Material resource storage
    rendering/storage/mesh_storage.cpp                                # Mesh resource storage
    rendering/storage/font_storage.cpp                                # Font resource storage
    rendering/storage/item_storage.cpp                                # B2-R: retained-mode per-item state storage
    rendering/renderer/canvas_renderer.cpp                            # 2D Canvas batch renderer
    rendering/renderer/spatial_renderer.cpp                           # 3D Spatial renderer
    rendering/renderer/compositor.cpp                                 # Rendering compositor (≈ Godot RendererCompositor)
)

# --- Header file list ----------------------------------------------------
# RCD/RDD 契约头已迁至 drivers/ 库根（2026-08-30，
# 对称 platform/ 的"接口在根、实现在子目录"模式），此处仅保留 servers 层自有头。
set(SERVERS_HEADERS
    rendering/rendering_device.h            # High-level rendering device
    rendering/render_buffers.h              # Render target encapsulation (Color + Depth + RenderPass + Framebuffer)
    rendering/draw_list_scope.h             # RAII wrapper for DrawList/ComputeList
    rendering/command_queue.h               # Main→Render thread command proxy (≈ Godot CommandQueueMT)
    rendering/render_server.h               # B2: main-thread rendering API proxy
    rendering/rendering_server.h            # S0: abstract rendering API singleton (frozen, ≈ Godot RenderingServer)
    rendering/rendering_server_default.h    # S0: default impl (facade → legacy RenderServer)
    rendering/render_viewport.h              # Per-screen render pipeline
    rendering/viewport_provider.h            # ADR-0007: viewport resolver (camera/light routing)
    rendering/render_globals.h              # ADR-0010: RSG-style render globals (flat storage access)
    rendering/arhud_engine.h               # Multi-screen rendering engine
    rendering/storage/shader_storage.h     # Shader resource storage
    rendering/storage/texture_storage.h    # Texture resource storage
    rendering/storage/material_storage.h   # Material resource storage
    rendering/storage/mesh_storage.h       # Mesh resource storage
    rendering/storage/font_storage.h       # Font resource storage
    rendering/storage/item_storage.h       # B2-R: retained-mode per-item state storage
    rendering/storage/viewport_scene_state.h # ADR-0007: per-viewport scene state (camera/light)
    rendering/renderer/shaders/hud_canvas.h         # Inline GLSL source for 2D Canvas shader
    rendering/renderer/shaders/hud_text.h           # Inline GLSL source for SDF text shader
    rendering/renderer/shaders/hud_spatial.h        # Inline GLSL source for 3D Spatial shader
    rendering/renderer/canvas_renderer.h            # 2D Canvas batch renderer
    rendering/renderer/spatial_renderer.h           # 3D Spatial renderer
    rendering/renderer/compositor.h               # Rendering compositor (≈ Godot RendererCompositor)
)

# --- Library target definition -------------------------------------------
add_library(arhud_servers STATIC ${SERVERS_SOURCES} ${SERVERS_HEADERS})

# --- Link dependencies ---------------------------------------------------
target_link_libraries(arhud_servers
    PUBLIC
        arhud_core      # Base types, containers, logger, etc.
        arhud_drivers   # 契约头（RCD/RDD）住 drivers/ 根 —— 公共头
                        # rendering_device.h 等 include 它们，include 路径须传递给
                        # 消费者；同时也是装配根（arhud_engine.cpp 注入 GL 实现）
    PRIVATE
        arhud_platform  # PIMPL internal use (DisplayServer, GLContextManager)
)

if(ARHUD_API_OPENGL)
    target_link_libraries(arhud_servers PUBLIC glm::glm)
endif()

# --- FreeType (SDF font atlas generation) ---
if(Freetype_FOUND OR Freetype_DIR)
    target_link_libraries(arhud_servers PUBLIC Freetype::Freetype)
    target_compile_definitions(arhud_servers PUBLIC ARHUD_FREETYPE_ENABLED=1)
else()
    message(WARNING "FreeType not found — FontLoadFromFreeType will be unavailable")
    target_compile_definitions(arhud_servers PUBLIC ARHUD_FREETYPE_ENABLED=0)
endif()

# --- Compile definitions --------------------------------------------------
target_compile_definitions(arhud_servers
    PUBLIC
        $<$<CONFIG:Debug>:ARHUD_DEBUG=1>
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:ARHUD_DEBUG=0>
)

# --- Include directories ---------------------------------------------------
target_include_directories(arhud_servers
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}                            # Allow #include "rendering/rendering_device_driver.h"
        ${CMAKE_CURRENT_SOURCE_DIR}/rendering                  # Allow direct includes from rendering/ subdirectory
        ${CMAKE_CURRENT_SOURCE_DIR}/rendering/storage          # Allow direct includes from storage/
        ${CMAKE_CURRENT_SOURCE_DIR}/rendering/renderer         # Allow direct includes from renderer/
        ${CMAKE_CURRENT_SOURCE_DIR}/rendering/renderer/shaders # Allow direct includes from shaders/
)
