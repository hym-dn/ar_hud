# ===========================================================================
# Compiler and Build Configuration (build)
# ===========================================================================
#
# This file configures:
#   1. C++ language standard (17 or 20)
#   2. Default build type
#   3. Global compile definitions (ARHUD_DEBUG / ARHUD_API_* macros)
#   4. Compiler warning level and automotive safety options (no exceptions, no RTTI)
#
# These settings affect all subsequently defined targets.
# If a target needs different settings, use target_compile_options() to override.
#
# ===========================================================================

# --- C++ language standard -----------------------------------------------
# C++17 is the minimum requirement, providing these core engine features:
#   - if constexpr          -> compile-time conditional branching (template specialization)
#   - std::optional         -> optional return values
#   - Structured bindings   -> iterating HashMap
#   - std::variant          -> type-safe union
#   - inline static members -> define static variables in headers
set(CMAKE_CXX_STANDARD 17)                # Default C++17
set(CMAKE_CXX_STANDARD_REQUIRED ON)       # Compiler must support it, error if not
set(CMAKE_CXX_EXTENSIONS OFF)             # Disable GNU extensions (-std=gnu++17),
                                          # force ISO standard (-std=c++17),
                                          # ensure cross-compiler portability

# --- Build type default --------------------------------------------------
# Single-config generators (Make/Ninja) require explicit CMAKE_BUILD_TYPE.
# Multi-config generators (VS/Xcode) specify via --config at build time; this setting is ignored.
# Default to Debug when unspecified (enables assertions, memory tracking, canary checks).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    # CACHE STRING makes it appear in ccmake/cmake-gui
    # FORCE overrides empty value; only effective on first configure
endif()

# If user requests C++20 via -DARHUD_ENABLE_CXX20=ON
if(ARHUD_ENABLE_CXX20)
    set(CMAKE_CXX_STANDARD 20)
endif()

# ===========================================================================
# Global compile definitions
# ===========================================================================
# add_compile_definitions() affects all subsequent targets.
# Uses generator expressions to conditionally inject macros by config/option.
#
# These macros correspond to usage in core/typedefs.h:
#   ARHUD_DEBUG      -> enable assertions, canary checks, memory stats
#   ARHUD_API_OPENGL -> compile OpenGL driver code
#   ARHUD_API_VULKAN -> compile Vulkan driver code
add_compile_definitions(
    # Debug build sets ARHUD_DEBUG=1, enabling:
    #   - ARHUD_ASSERT / ARHUD_ASSERT_MSG assertions
    #   - AllocHeader tracking header and canary checks in memory.h
    #   - Debug-specific boundary checks and logging
    $<$<CONFIG:Debug>:ARHUD_DEBUG=1>

    # Release / RelWithDebInfo / MinSizeRel sets ARHUD_DEBUG=0:
    #   - Assertions compile to no-ops
    #   - Memory allocation zero overhead (no tracking header, no canary)
    #   - Maximum runtime performance
    $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:ARHUD_DEBUG=0>

    # Graphics API backend selection (linked with option.cmake)
    # When enabled, #ifdef ARHUD_API_OPENGL code blocks are compiled
    $<$<BOOL:${ARHUD_API_OPENGL}>:ARHUD_API_OPENGL=1>
    $<$<BOOL:${ARHUD_API_VULKAN}>:ARHUD_API_VULKAN=1>
)

# ===========================================================================
# Compiler warnings and safety options
# ===========================================================================
# Automotive safety requirements: disable exceptions and RTTI,
# matching coding standard (coding_standard_skill.md).
# Zero-warning policy: all code must compile with zero warnings.

if(MSVC)
    # -- MSVC (Visual Studio) --

    # /W3: Level 3 warnings (recommended production level, covers most common issues)
    #   Not using /W4 (too noisy) or /Wall (massive system header warnings)
    add_compile_options(/W3)

    # /utf-8: Source and execution character set both UTF-8
    #   Avoids C4819 warning ("file contains characters that cannot be represented
    #   in the current code page")
    add_compile_options(/utf-8)

    # /D_CRT_SECURE_NO_WARNINGS: Suppress MSVC deprecation warnings for
    #   POSIX C functions (fopen, strncpy, sprintf, etc.).
    #   These functions are safe when used correctly with size-bounded inputs.
    #   MSVC recommends _s variants (fopen_s, strncpy_s), but they are non-portable
    #   and not available on Linux/Android/QNX.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

else()
    # -- GCC / Clang (Linux / Android / QNX) --

    # -Wall: Standard warning set (mostly useful)
    # -Wextra: Extra warnings (unused parameters, empty function bodies, etc.)
    # -Wpedantic: Strict ISO standard compliance (disable compiler extensions)
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()
