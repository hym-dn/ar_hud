# ===========================================================================
# Application: arhud_demo
# ===========================================================================
#
# Minimal demo executable that validates the RCD→RDD→RD pipeline end-to-end.
# Creates a window, initializes the rendering stack, and runs a clear-screen
# loop with SwapBuffers.
#
# Dependency direction:
#   arhud_core (zero deps)
#       ^
#   arhud_platform (depends on core)
#       ^
#   arhud_servers (depends on core+platform)
#       ^
#   arhud_drivers (depends on core+servers+platform)
#       ^
#   arhud_demo   (depends on all above)  <- This executable
#
# ===========================================================================

set(DEMO_SOURCES
    main.cpp
)

add_executable(arhud_demo ${DEMO_SOURCES})

target_link_libraries(arhud_demo
    PRIVATE
        arhud_drivers
        arhud_servers
        arhud_platform
        arhud_core
)

if(ARHUD_API_OPENGL)
    target_link_libraries(arhud_demo PRIVATE glad::glad)
endif()
