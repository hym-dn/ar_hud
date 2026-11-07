# ===========================================================================
# Platform Library: arhud_platform
# ===========================================================================
#
# arhud_platform provides platform abstraction layer (currently Windows only):
#   - Platform-independent interfaces (window.h, screen.h, display_server.h, gl_context_manager.h)
#   - Win32 window implementation (window_win32.h/.cpp)
#   - Win32 screen implementation (screen_win32.h/.cpp)
#   - Win32 display server implementation (display_server_win32.h/.cpp)
#   - Win32 GL context manager (gl_context_manager_win32.h/.cpp)
#
# Dependency direction (2026-08-30 契约归位后):
#   arhud_core (zero deps)
#       ^
#   arhud_platform (depends on core)  <- This library
#       ^
#   arhud_drivers (depends on core+platform)  <- contracts at root + GL impls in gl/
#       ^
#   arhud_servers (depends on core+scene+platform+drivers)  <- RD consumes contracts
#
# Future: Add Linux/Android/QNX platform implementations here
#
# ===========================================================================

# --- Source file list ----------------------------------------------------
set(PLATFORM_SOURCES
    windows/window_win32.cpp      # Win32 window implementation (CreateWindowEx, WndProc, PollEvents)
    windows/screen_win32.cpp      # Win32 screen implementation (EnumDisplayMonitors, MonitorInfo)
    windows/display_server_win32.cpp # Win32 display server implementation (multi-window/screen)
    windows/gl_context_manager_win32.cpp     # Win32 GL context manager (WGL, pixel format, 3.3 core profile, VSync)
    windows/frame_presenter_win32.cpp # Win32 frame presenter (GDI StretchDIBits, FBO readback → screen)
)

# --- Header file list ----------------------------------------------------
set(PLATFORM_HEADERS
    window.h                      # Platform-independent window interface (IWindow, WindowDesc, WindowCallbacks)
    screen.h                      # Platform-independent screen interface (IScreen, ScreenInfo)
    display_server.h              # Display server interface (IDisplayServer, multi-window/screen management)
    gl_context_manager.h                   # Platform-independent GL context manager interface (IGLContextManager)
    frame_presenter.h             # Platform-independent frame presenter interface (IFramePresenter, FBO→screen)
    windows/window_win32.h        # Win32 window implementation (Win32Window, WndProc, CreateWindowEx)
    windows/screen_win32.h        # Win32 screen implementation (Win32Screen, EnumDisplayMonitors)
    windows/display_server_win32.h # Win32 display server implementation (Win32DisplayServer)
    windows/gl_context_manager_win32.h     # Win32 GL context manager (WGL, pixel format, 3.3 core profile, VSync)
    windows/frame_presenter_win32.h # Win32 frame presenter (GDI StretchDIBits, top-down DIB)
)

# --- Library target definition -------------------------------------------
add_library(arhud_platform STATIC ${PLATFORM_SOURCES} ${PLATFORM_HEADERS})

# --- Link dependencies ---------------------------------------------------
target_link_libraries(arhud_platform
    PUBLIC
        arhud_core     # Base types, containers, logger, thread, sync primitives
)

# --- Compile definitions --------------------------------------------------
target_compile_definitions(arhud_platform
    PUBLIC
        $<$<CONFIG:Debug>:ARHUD_DEBUG=1>
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:ARHUD_DEBUG=0>
)

# --- Include directories ---------------------------------------------------
target_include_directories(arhud_platform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}         # Allow #include "window.h", etc.
        ${CMAKE_CURRENT_SOURCE_DIR}/windows  # Allow direct includes from windows/ subdirectory
)

# --- Platform libraries ---------------------------------------------------
# Win32 window management requires user32 (CreateWindowEx, WndProc, etc.)
if(WIN32)
    target_link_libraries(arhud_platform PUBLIC user32 gdi32 opengl32)
endif()
