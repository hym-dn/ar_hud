# ===========================================================================
# Drivers Library: arhud_drivers
# ===========================================================================
#
# arhud_drivers provides GPU driver implementations (currently OpenGL only):
#   - OpenGL rendering context driver (rendering_context_driver_gl.h/.cpp)
#   - OpenGL rendering device driver (rendering_device_driver_gl.h/.cpp)
#   - OpenGL command buffer implementation (command_buffer_gl.h/.cpp)
#
# Dependency direction:
#   arhud_core (zero deps)
#       ^
#   arhud_platform (depends on core)
#       ^
#   arhud_servers (depends on core+platform)
#       ^
#   arhud_drivers (depends on core+servers+platform)  <- This library
#
# ===========================================================================

# --- Source file list ----------------------------------------------------
set(DRIVERS_SOURCES "")
set(DRIVERS_HEADERS "")

if(ARHUD_API_OPENGL)
    list(APPEND DRIVERS_SOURCES
        gl/rendering_context_driver_gl.cpp
        gl/rendering_device_driver_gl.cpp
        gl/command_buffer_gl.cpp
    )

    list(APPEND DRIVERS_HEADERS
        gl/rendering_context_driver_gl.h
        gl/rendering_device_driver_gl.h
        gl/command_buffer_gl.h
    )
endif()

# --- Library target definition -------------------------------------------
if(DRIVERS_SOURCES)
    add_library(arhud_drivers STATIC ${DRIVERS_SOURCES} ${DRIVERS_HEADERS})

    target_link_libraries(arhud_drivers
        PUBLIC
            arhud_core
            arhud_servers
            arhud_platform
    )

    if(ARHUD_API_OPENGL)
        target_link_libraries(arhud_drivers PUBLIC glad::glad)
    endif()

    target_compile_definitions(arhud_drivers
        PUBLIC
            $<$<CONFIG:Debug>:ARHUD_DEBUG=1>
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:ARHUD_DEBUG=0>
    )

    target_include_directories(arhud_drivers
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/gl
    )
else()
    message(STATUS "arhud_drivers: No driver sources, skipping library creation")
endif()
