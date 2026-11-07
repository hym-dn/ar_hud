# ===========================================================================
# Unit Tests — Module-based organization (reference: Godot tests/)
# ===========================================================================
#
# Directory structure mirrors the engine's module layout:
#   tests/
#   ├── core/           ← arhud_core tests (sync, containers, math, string)
#   ├── rendering/       ← arhud_servers tests (CommandQueue, RD, Storage)
#   └── integration/     ← cross-module tests (requires NullDriver or real GL)
#
# Build with:
#   conan install . --build=missing -s build_type=Debug -o build_tests=True
#   cmake --preset conan-default
#   cmake --build build --config Debug --parallel
#   ctest --test-dir build --build-config Debug --output-on-failure
#
# Or one-click:
#   build.bat tests
# ===========================================================================

find_package(GTest REQUIRED)

# --- Test sources (explicit list, no file(GLOB)) ---------------------------
set(TEST_SOURCES
    rendering/test_command_queue.cpp
    rendering/test_camera_routing.cpp
    rendering/test_item_storage.cpp
    # core/test_sync.cpp             # TODO: add when writing core tests
    # core/test_containers.cpp       # TODO
    # integration/test_render_frame.cpp  # TODO: needs NullDriver
)

# --- Test executable -------------------------------------------------------
add_executable(arhud_tests ${TEST_SOURCES})

target_link_libraries(arhud_tests PRIVATE
    arhud_servers        # CommandQueue + Storage + Renderer + Compositor
    arhud_core           # Thread + Semaphore + SpinLock + containers
    GTest::gtest
    GTest::gtest_main
)

# --- Include directories for test subdirectories ---------------------------
target_include_directories(arhud_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}           # tests/ root (shared test utils)
    ${CMAKE_CURRENT_SOURCE_DIR}/core      # core/ test helpers
    ${CMAKE_CURRENT_SOURCE_DIR}/rendering  # rendering/ test helpers
)

# --- Register with CTest ---------------------------------------------------
include(GoogleTest)
gtest_discover_tests(arhud_tests)
