# ===========================================================================
# Servers Library: arhud_servers
# ===========================================================================
#
# arhud_servers provides rendering device interfaces and command buffer infrastructure:
#   - Rendering device common types (rendering_device_commons.h)
#   - Rendering context driver interface (rendering_context_driver.h)
#   - Rendering device driver interface (rendering_device_driver.h)
#   - Command buffer abstraction (command_buffer.h)
#   - Resource tracker (resource_tracker.h/.cpp)
#
# Dependency direction:
#   arhud_core (zero deps)
#       ^
#   arhud_platform (depends on core)
#       ^
#   arhud_servers (depends on core+platform)  ← This library
#       ^
#   arhud_drivers (depends on core+servers+platform)
#
# ===========================================================================

# --- Source file list ----------------------------------------------------
set(SERVERS_SOURCES
    redering/resource_tracker.cpp                # Lightweight resource tracker (registration, usage tracking, conflict detection)
    redering/rendering_device.cpp                # High-level rendering device (resource lifecycle, staging buffer, frame loop)
)

# --- Header file list ----------------------------------------------------
set(SERVERS_HEADERS
    redering/rendering_device_commons.h # Rendering device shared enums, types, and structures (RD/RDD commons)
    redering/rendering_context_driver.h # Rendering context driver interface (device enumeration, surface management)
    redering/rendering_device_driver.h  # Rendering device driver interface (GPU resource management)
    redering/rendering_device.h         # High-level rendering device (resource lifecycle, staging buffer, frame loop)
    redering/command_buffer.h                # Command buffer interface (command types, ICommandBuffer)
    redering/resource_tracker.h              # Lightweight resource tracker (ResourceUsage, ResourceType, ResourceTracker)
    redering/storage/shader_storage.h        # Shader resource storage with metadata and source caching
)

# --- Library target definition -------------------------------------------
add_library(arhud_servers STATIC ${SERVERS_SOURCES} ${SERVERS_HEADERS})

# --- Link dependencies ---------------------------------------------------
target_link_libraries(arhud_servers
    PUBLIC
        arhud_core      # Base types, containers, logger, etc.
        arhud_platform  # IGLManager, IWindow for RenderingContextDriver
)

if(ARHUD_API_OPENGL)
    target_link_libraries(arhud_servers PUBLIC glm::glm)
endif()

# --- Compile definitions --------------------------------------------------
target_compile_definitions(arhud_servers
    PUBLIC
        $<$<CONFIG:Debug>:ARHUD_DEBUG=1>
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:ARHUD_DEBUG=0>
)

# --- Include directories ---------------------------------------------------
target_include_directories(arhud_servers
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}         # Allow #include "redering/rendering_device_driver.h"
        ${CMAKE_CURRENT_SOURCE_DIR}/redering # Allow direct includes from redering/ subdirectory
)