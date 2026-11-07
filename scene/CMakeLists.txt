# ===========================================================================
# Scene Library: arhud_scene
# ===========================================================================
#
# Scene tree layer — sits ABOVE arhud_servers (Godot-aligned).
#
# Currently empty — Camera2D was removed (use Transform2D from core/math),
# Camera3D was renamed to CameraMatrix and moved to core/math/.
#
# Phase 6 will add: HudScene, HudNode, HudCanvasItem (scene tree; consumes
# arhud_servers to issue render commands).
#
# Dependency direction:
#   arhud_core (zero deps) -> arhud_platform -> arhud_drivers -> arhud_servers
#       ^
#   arhud_scene (depends on core+servers)
# ===========================================================================

add_library(arhud_scene STATIC scene_placeholder.cpp)

target_include_directories(arhud_scene
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(arhud_scene PUBLIC arhud_core arhud_servers)
