# ===========================================================================
# Drivers Library: arhud_drivers
# ===========================================================================
#
# arhud_drivers 承载渲染后端的**契约与实现**（对称 platform/ 模式：
# 接口在库根，实现在 gl/ 子目录 —— 如 platform/window.h ↔ windows/）：
#   - 渲染后端契约（库根，纯接口头，后端无关）:
#       rendering_context_driver.h / rendering_device_driver.h /
#       rendering_device_commons.h
#   - OpenGL 实现（gl/）:
#       rendering_context_driver_gl / rendering_device_driver_gl
#
# Dependency direction (2026-08-30 依赖倒置):
#   arhud_core (zero deps)
#       ^
#   arhud_platform (depends on core)
#       ^
#   arhud_drivers (depends on core+platform)  <- This library
#       ^
#   arhud_servers (depends on drivers PUBLIC —— 公共头消费契约；
#                  装配根 arhud_engine.cpp 注入 GL 实现)
#
#   不链接 arhud_servers —— 驱动只实现本库根部的契约，
#   倒挂链接（及其传染的 Freetype 等 PUBLIC 依赖）已消除。
#
# ===========================================================================

# --- Source file list ----------------------------------------------------
set(DRIVERS_SOURCES "")
set(DRIVERS_HEADERS "")

# 后端无关契约头（纯接口，无实现；无论 GL/Vulkan 后端均适用）
list(APPEND DRIVERS_HEADERS
    rendering_context_driver.h        # RCD interface
    rendering_device_driver.h         # RDD interface
    rendering_device_commons.h        # Shared enums/types/DEFINE_ID
)

if(ARHUD_API_OPENGL)
    list(APPEND DRIVERS_SOURCES
        gl/rendering_context_driver_gl.cpp
        gl/rendering_device_driver_gl.cpp
        gl/rendering_device_driver_gl_commands.cpp
        gl/rendering_device_driver_gl_format.cpp
        gl/rendering_device_driver_gl_sync.cpp
        gl/rendering_device_driver_gl_shader_cache.cpp
    )

    # Mode X 零回读上屏：外部显存导入（GL 侧）。仅 Windows 编译——
    # 内部只经 platform::IGPUInterop 说话，本身不含 wgl 调用。
    if(WIN32)
        list(APPEND DRIVERS_SOURCES
            gl/rendering_device_driver_gl_dx.cpp
        )
    endif()

    list(APPEND DRIVERS_HEADERS
        gl/rendering_context_driver_gl.h
        gl/rendering_device_driver_gl.h
    )
endif()

# --- Library target definition -------------------------------------------
if(DRIVERS_SOURCES)
    add_library(arhud_drivers STATIC ${DRIVERS_SOURCES} ${DRIVERS_HEADERS})

    target_link_libraries(arhud_drivers
        PUBLIC
            arhud_core
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
