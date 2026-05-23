# ===========================================================================
# Global Include Paths and Output Directories (dir)
# ===========================================================================
#
# This file configures:
#   1. Global #include search paths
#   2. Unified output directories for build artifacts
#
# ===========================================================================

# --- Global include paths ------------------------------------------------
# include_directories() adds specified directories to -I search path for all targets.
#
# Current setting:
#   core/ directory -> allows #include "typedefs.h" / #include "memory.h"
#   This lets render/ and platform/ modules directly reference core headers
#   without repeating target_include_directories() on each target.
#
# Note: include_directories() is global (old-style CMake).
# A more modern approach is target_include_directories() per target,
# but with few modules currently, global setting is simpler.
# When modules increase, consider migrating to target-level include paths.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core              # Makes #include "typedefs.h" work cross-module
    ${CMAKE_CURRENT_SOURCE_DIR}/core/io
    ${CMAKE_CURRENT_SOURCE_DIR}/core/os
    ${CMAKE_CURRENT_SOURCE_DIR}/core/template
    ${CMAKE_CURRENT_SOURCE_DIR}/core/string
    ${CMAKE_CURRENT_SOURCE_DIR}/core/math
    ${CMAKE_CURRENT_SOURCE_DIR}/core/platform     # Platform abstraction interfaces (window, screen, display_server)
    ${CMAKE_CURRENT_SOURCE_DIR}/core/platform/windows  # Win32 platform implementations
    ${CMAKE_CURRENT_SOURCE_DIR}/core/rendering/driver  # Rendering device driver shared definitions
    ${CMAKE_CURRENT_SOURCE_DIR}/core/rendering/driver/gl  # OpenGL rendering driver implementations
)

# --- Build artifact output directories -----------------------------------
# Unified output to bin/ and lib/ subdirectories under build/,
# preventing executables and libraries from scattering in source directories.
#
# CMake variable reference:
#   CMAKE_RUNTIME_OUTPUT_DIRECTORY  -> .exe / .dll (executables and shared libs)
#   CMAKE_LIBRARY_OUTPUT_DIRECTORY  -> .so (Linux shared libraries)
#   CMAKE_ARCHIVE_OUTPUT_DIRECTORY  -> .lib / .a (static libraries and import libs)
#
# Paths are based on CMAKE_BINARY_DIR (i.e. build/ directory).
# Multi-config generators (VS) automatically append /Debug or /Release subdirectory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # Executables
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)   # Shared libraries (.so/.dll)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)   # Static libraries (.lib/.a)
