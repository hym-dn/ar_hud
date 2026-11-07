# ===========================================================================
# ARHud Engine -- Root Build Configuration (Godot-style Architecture)
# ===========================================================================
#
# This file is the CMake build system entry point, responsible for:
#   1. Declaring project metadata (name, version, languages)
#   2. Loading modular configuration (cmake/*.cmake)
#   3. Recursively adding subdirectories (core / servers / drivers / platform)
#
# --- Library Architecture (Godot-style) ----------------------------------
#
#   arhud_core      (zero deps)        ← Base types, containers, IO, math
#       ↑
#   arhud_platform  (depends on core)  ← Platform abstraction (Win32)
#       ↑
#   arhud_drivers   (depends on core+platform) ← Contracts (RCD/RDD at root) + GL impls (gl/)
#       ↑
#   arhud_servers   (core+platform+drivers) ← RD, Storage, RenderServer
#                    （公共头消费 drivers 根部契约；装配根注入 GL 实现）
#       ↑
#   arhud_scene     (depends on core+servers) ← Scene tree (HudScene/HudNode, placeholder)
#
# --- Build methods -------------------------------------------------------
#
#   Method 1 (Direct CMake, no Conan required):
#     cmake -B build -DCMAKE_BUILD_TYPE=Debug
#     cmake --build build --config Debug
#
#   Method 2 (Conan 2.x, recommended, auto-manages third-party deps):
#     conan install . --build=missing -s build_type=Debug
#     cmake --preset conan-default          # Use Conan-generated preset
#     cmake --build build/Debug
#
#   Method 3 (Windows one-click script):
#     build.bat            # Debug
#     build.bat release    # Release
#     build.bat conan      # Conan + Debug
#
# ===========================================================================

# --- CMake minimum version -----------------------------------------------
# 3.16 is the baseline with stable preset support and modern target syntax.
# All major OS package managers can install 3.16+.
cmake_minimum_required(VERSION 3.16)

# --- Project declaration -------------------------------------------------
project(arhud
    VERSION 0.1.0                        # Semantic version, matches conanfile.py
    DESCRIPTION "AR Head-Up Display Rendering Engine"
    LANGUAGES C CXX                      # C (glad generated code) / CXX (engine body)
)

# --- Modular configuration loading ---------------------------------------
include(cmake/option.cmake)     # Build option() definitions (API selection, component switches)
include(cmake/build.cmake)      # C++ standard, compiler flags, global definitions
include(cmake/dir.cmake)        # Global include paths, output directories
include(cmake/lib.cmake)        # Third-party dependency integration (Conan / find_package)

# --- Subdirectories ------------------------------------------------------
# Addition order follows dependency direction:
#   core -> platform -> drivers (contracts+impls) -> servers -> scene
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(drivers)
add_subdirectory(servers)
add_subdirectory(scene)

# Demo executables (optional, needs glad for OpenGL)
# Build when any demo option is enabled:
#   -DARHUD_BUILD_DEMO=ON           (all demos)
#   -DARHUD_DEMO_PHASE2=ON          (Phase 2 only)
#   -DARHUD_DEMO_PHASE4=ON          (all Phase 4 demos)
#   -DARHUD_DEMO_PHASE4_CANVAS=ON    (specific demo)
if(ARHUD_BUILD_DEMO OR ARHUD_DEMO_PHASE2 OR ARHUD_DEMO_PHASE4
   OR ARHUD_DEMO_PHASE4_CANVAS OR ARHUD_DEMO_PHASE4_SPATIAL
   OR ARHUD_DEMO_PHASE4_COMPOSITOR OR ARHUD_DEMO_PHASE4_TEXT)
    add_subdirectory(app)
endif()

# Unit tests (optional, needs GoogleTest via Conan)
#   -DARHUD_BUILD_TESTS=ON
if(ARHUD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()