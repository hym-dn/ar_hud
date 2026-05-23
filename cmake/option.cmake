# ===========================================================================
# Project Build Options (option)
# ===========================================================================
#
# This file defines all build switches controllable via -D<OPTION>=ON/OFF.
# option() values are cached in CMakeCache.txt; first setting persists.
#
# Usage:
#   cmake -B build -DARHUD_API_OPENGL=ON -DARHUD_BUILD_TESTS=OFF
#
# Conan mapping (conanfile.py -> generate()):
#   Conan option              ->  CMake variable
#   api_opengl=True           ->  ARHUD_API_OPENGL=ON
#   api_vulkan=True           ->  ARHUD_API_VULKAN=ON
#   build_tests=True          ->  ARHUD_BUILD_TESTS=ON
#   shared=True               ->  BUILD_SHARED_LIBS=ON
#
# ===========================================================================

# --- Graphics API backend selection --------------------------------------
# At least one API backend must be enabled, otherwise compilation fails
# (no implementation to compile in render/ directory).
# Currently only OpenGL implementation is available; Vulkan is reserved for future.

# ARHUD_API_OPENGL: Enable OpenGL rendering backend
#   ON  -- compile render/opengl/ driver implementation
#   OFF -- do not compile OpenGL-related code
#   Default: ON (only available backend currently)
option(ARHUD_API_OPENGL "Enable OpenGL rendering backend" ON)

# ARHUD_API_VULKAN: Enable Vulkan rendering backend
#   ON  -- compile render/vulkan/ driver implementation (requires Vulkan SDK)
#   OFF -- do not compile Vulkan-related code
#   Default: OFF (not yet implemented)
option(ARHUD_API_VULKAN "Enable Vulkan rendering backend" OFF)

# --- Build component switches --------------------------------------------
# Control whether to compile tests, tools, and other non-core components.

# ARHUD_BUILD_TESTS: Build unit tests
#   ON  -- add tests/ subdirectory, compile gtest test executables
#   OFF -- do not compile tests (recommended for automotive delivery builds)
option(ARHUD_BUILD_TESTS   "Build unit tests"   OFF)

# ARHUD_BUILD_TOOLS: Build developer tools
#   ON  -- add tools/ subdirectory (e.g. shader compiler, resource packer)
#   OFF -- do not compile tools
option(ARHUD_BUILD_TOOLS   "Build developer tools" OFF)

# ARHUD_BUILD_DEMO: Build the minimal demo executable
#   ON  -- add app/ subdirectory, compile arhud_demo
#   OFF -- do not compile demo (recommended for automotive delivery builds)
#   Note: requires glad (Conan or system install) when ARHUD_API_OPENGL=ON
option(ARHUD_BUILD_DEMO   "Build demo executable" OFF)

# --- C++ language standard -----------------------------------------------
# C++17 is the default (if constexpr / std::optional / structured bindings).
# C++20 is optional (std::span / concepts / ranges), not required.

# ARHUD_ENABLE_CXX20: Use C++20 instead of C++17
#   ON  -- set CMAKE_CXX_STANDARD=20
#   OFF -- use CMAKE_CXX_STANDARD=17 (default)
#   Note: requires compiler support (MSVC 19.28+ / GCC 10+ / Clang 10+)
option(ARHUD_ENABLE_CXX20  "Use C++20 standard" OFF)

# --- Library type --------------------------------------------------------
# BUILD_SHARED_LIBS is a standard CMake variable controlling add_library default:
#   OFF -- static library (.lib / .a), recommended for automotive (no runtime deps)
#   ON  -- shared library (.dll / .so), optional during development (faster incremental)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
