# ===========================================================================
# Third-party Dependency Integration (lib)
# ===========================================================================
#
# This file handles finding and integrating third-party libraries.
# Two modes are supported:
#
#   Mode 1: Conan 2.x package manager (recommended)
#     conan install . --build=missing
#     Generates CMakePresets.json and CMakeUserPresets.json in build/,
#     automatically injecting find_package paths.
#     Conan's CMakeDeps generator creates <Package>Config.cmake
#     for each dependency, making find_package() work directly.
#
#   Mode 2: System install / manual path specification
#     Without Conan, dependencies must be pre-installed to system paths
#     or specified via -D<Package>_ROOT=<path>.
#     Suitable for CI environments or embedded cross-compilation.
#
# --- Current dependency list ---
#
#   glm        - Math library (vectors/matrices), header-only
#   glad       - OpenGL function loader, generates C source
#   freetype   - Font rasterization (HUD text rendering)
#   stb_image  - Image loading (texture resources), header-only
#
# ===========================================================================

# --- glm -- OpenGL Mathematics -------------------------------------------
# Header-only math library providing vec2/vec3/mat4 etc.
# The engine uses glm instead of a custom math library to reduce maintenance.
#
# Integration:
#   Conan: glm::glm (CMake target, auto-handles include paths)
#   System: find_package(glm) creates glm::glm imported target
#
# Usage:
#   target_link_libraries(arhud_render PUBLIC glm::glm)
if(ARHUD_API_OPENGL)
    find_package(glm CONFIG REQUIRED)
endif()

# --- glad -- OpenGL Function Loader --------------------------------------
# glad generates C source from API version description files,
# loading OpenGL function pointers.
# Advantages over GLEW:
#   - No runtime initialization dependency
#   - Supports any GL version and extension combination
#   - Generated code can be compiled directly into the project
#
# Integration:
#   Conan: glad::glad (includes .c source and headers)
#   System: find_package(glad) creates glad::glad target
#
# Usage:
#   target_link_libraries(arhud_render_opengl PUBLIC glad::glad)
if(ARHUD_API_OPENGL)
    find_package(glad CONFIG REQUIRED)
endif()

# --- freetype -- Font Rasterization --------------------------------------
# FreeType rasterizes vector fonts (TTF/OTF) into bitmaps,
# used in the HUD text rendering pipeline:
#   Font file -> FreeType rasterization -> Texture atlas -> GPU rendering
#
# Integration:
#   Conan: Freetype::Freetype (CMake target)
#   System: find_package(Freetype) creates Freetype::Freetype target
#
# Usage:
#   target_link_libraries(arhud_render PUBLIC Freetype::Freetype)
find_package(Freetype CONFIG QUIET)

# --- stb_image -- Image Loading ------------------------------------------
# Single-header image loading library supporting PNG/JPG/BMP/TGA etc.
# Used for loading HUD texture resources (icons, backgrounds, etc.).
#
# Note: stb_image is header-only; STB_IMAGE_IMPLEMENTATION must be
# defined in exactly one .cpp file to generate implementation code.
# This project creates stb_image_impl.cpp in the render/ module for this.
#
# Integration:
#   Conan: stb::stb_image (header paths only)
#   System: find_package(stb) creates stb::stb_image target
#
# Usage:
#   target_link_libraries(arhud_render PUBLIC stb::stb_image)
find_package(stb CONFIG QUIET)

# --- Dependency availability check ---------------------------------------
# Print find_package results at configure time to quickly identify missing deps.
# QUIET mode suppresses find_package output, so manual check is needed.
message(STATUS "--- Third-party dependency status ---")
message(STATUS "  glm:        ${glm_FOUND}")
message(STATUS "  glad:       ${glad_FOUND}")
message(STATUS "  Freetype:   ${Freetype_FOUND}")
message(STATUS "  stb_image:  ${stb_FOUND}")
message(STATUS "-------------------------------------")

# --- Future extensions ---------------------------------------------------
# The following dependencies will be added as needed in later stages:
#
#   Vulkan-Headers / Vulkan-Hpp  -- when ARHUD_API_VULKAN=ON
#   SDL2 / GLFW                  -- window management (platform/ module)
#   Google Test / Google Mock    -- when ARHUD_BUILD_TESTS=ON
#   shaderc / SPIRV-Cross        -- shader compilation and cross-platform conversion
