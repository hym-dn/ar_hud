# ===========================================================================
# Application Demos
# ===========================================================================
#
# Each demo is an independent executable that validates a specific rendering
# phase. All demos share the same library dependencies but test different
# layers of the rendering pipeline.
#
# Demo naming convention: arhud_demo_<phase>_<feature>
#
# Directory layout:
#   app/
#   ├── CMakeLists.txt              ← This file
#   ├── phase2_rdc_rdd_rd/main.cpp  ← RCD→RDD→RD pipeline validation
#   ├── phase4_canvas/main.cpp      ← 2D Canvas rendering
#   ├── phase4_spatial/main.cpp     ← 3D Spatial rendering
#   ├── phase4_compositor/main.cpp  ← 2D/3D mixed rendering via Compositor
#   └── phase4_text/main.cpp        ← SDF text rendering (requires FreeType)
#
# Build control:
#   -DARHUD_BUILD_DEMO=ON           ← Build ALL demos
#   -DARHUD_DEMO_PHASE2=ON         ← Build only Phase 2 demo
#   -DARHUD_DEMO_PHASE4=ON         ← Build all Phase 4 demos
#
# ===========================================================================

# --- Common demo link libraries -------------------------------------------
# All demos depend on the same set of engine libraries.
# Note: demos link arhud_scene + arhud_servers + arhud_core (scene sits above
# servers, Godot-aligned). arhud_platform and arhud_drivers are PRIVATE
# dependencies of arhud_servers, not exposed to application layer (PIMPL isolation).
set(ARHUD_DEMO_LIBS
    arhud_scene      # Scene tree layer (sits above arhud_servers)
    arhud_servers    # Engine entry point (ARHudEngine)
    arhud_core       # Base types (typedefs.h, etc.)
)

# --- Helper function: add arhud demo --------------------------------------
function(add_arhud_demo DEMO_NAME DEMO_SOURCE_DIR)
    add_executable(${DEMO_NAME} ${DEMO_SOURCE_DIR}/main.cpp)
    target_link_libraries(${DEMO_NAME} PRIVATE ${ARHUD_DEMO_LIBS})
    if(ARHUD_API_OPENGL)
        target_link_libraries(${DEMO_NAME} PRIVATE glad::glad)
    endif()
endfunction()

# ===========================================================================
# Phase 2: RCD → RDD → RD Pipeline Validation
# ===========================================================================
# Validates the entire rendering device stack end-to-end:
#   DisplayServer → GLManager → RCD → RDD → RD → Clear Screen Loop
option(ARHUD_DEMO_PHASE2 "Build Phase 2 demo (RCD/RDD/RD pipeline)" OFF)

if(ARHUD_DEMO_PHASE2 OR ARHUD_BUILD_DEMO)
    add_arhud_demo(arhud_demo_phase2 ${CMAKE_CURRENT_SOURCE_DIR}/phase2_rdc_rdd_rd)
    # Phase 2 directly tests the driver/platform layer (governance §12.4 NOLINT exception).
    # It needs arhud_drivers + arhud_platform for direct GL/platform API access.
    target_link_libraries(arhud_demo_phase2 PRIVATE arhud_drivers arhud_platform)
endif()

# ===========================================================================
# Phase 4: Canvas Renderer (2D)
# ===========================================================================
# Validates 2D Canvas rendering:
#   Compositor → CanvasRenderer → Texture + Material + Mesh Storage
#   - Quad rendering with texture
#   - Z-Order sorting
#   - Material batch merging
option(ARHUD_DEMO_PHASE4_CANVAS "Build Phase 4 Canvas demo (2D rendering)" OFF)

if(ARHUD_DEMO_PHASE4_CANVAS OR ARHUD_DEMO_PHASE4 OR ARHUD_BUILD_DEMO)
    add_arhud_demo(arhud_demo_phase4_canvas ${CMAKE_CURRENT_SOURCE_DIR}/phase4_canvas)
endif()

# ===========================================================================
# Phase 4: Spatial Renderer (3D)
# ===========================================================================
# Validates 3D Spatial rendering:
#   Compositor → SpatialRenderer → Camera3D + DirectionalLight
#   - Box/Sphere/Cylinder rendering
#   - Depth test + backface culling
#   - Directional light
#   - Ribbon3D (no-cull pipeline)
option(ARHUD_DEMO_PHASE4_SPATIAL "Build Phase 4 Spatial demo (3D rendering)" OFF)

if(ARHUD_DEMO_PHASE4_SPATIAL OR ARHUD_DEMO_PHASE4 OR ARHUD_BUILD_DEMO)
    add_arhud_demo(arhud_demo_phase4_spatial ${CMAKE_CURRENT_SOURCE_DIR}/phase4_spatial)
endif()

# ===========================================================================
# Phase 4: Compositor (2D + 3D Mixed Rendering)
# ===========================================================================
# Validates the full rendering pipeline with Compositor:
#   Compositor → SpatialRenderer (3D) + CanvasRenderer (2D layers)
#   - 3D scene with depth buffer
#   - 2D HUD overlay layers
#   - HUDCanvasLayer sorting
option(ARHUD_DEMO_PHASE4_COMPOSITOR "Build Phase 4 Compositor demo (2D/3D mixed)" OFF)

if(ARHUD_DEMO_PHASE4_COMPOSITOR OR ARHUD_DEMO_PHASE4 OR ARHUD_BUILD_DEMO)
    add_arhud_demo(arhud_demo_phase4_compositor ${CMAKE_CURRENT_SOURCE_DIR}/phase4_compositor)
endif()

# ===========================================================================
# Phase 4: SDF Text Rendering (requires FreeType)
# ===========================================================================
# Validates SDF text rendering:
#   FontStorage::FontLoadFromFreeType → CanvasRenderer SDF path
#   - FreeType font loading
#   - SDF glyph generation
#   - Text rendering on Canvas
#   Note: Requires FreeType dependency
option(ARHUD_DEMO_PHASE4_TEXT "Build Phase 4 Text demo (SDF rendering)" OFF)

if(ARHUD_DEMO_PHASE4_TEXT OR ARHUD_DEMO_PHASE4 OR ARHUD_BUILD_DEMO)
    if(Freetype_FOUND)
        add_arhud_demo(arhud_demo_phase4_text ${CMAKE_CURRENT_SOURCE_DIR}/phase4_text)
        target_link_libraries(arhud_demo_phase4_text PRIVATE Freetype::Freetype)
    else()
        message(STATUS "arhud_demo_phase4_text: skipped (FreeType not found)")
    endif()
endif()
