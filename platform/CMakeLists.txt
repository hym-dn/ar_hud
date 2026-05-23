# ===========================================================================
# Platform Library: arhud_platform
# ===========================================================================
#
# arhud_platform provides platform abstraction layer (currently Windows only):
#   - Platform-independent interfaces (window.h, screen.h, view.h, display_server.h, gl_manager.h)
#   - Win32 window implementation (window_win32.h/.cpp)
#   - Win32 screen implementation (screen_win32.h/.cpp)
#   - Win32 display server implementation (display_server_win32.h/.cpp)
#   - Win32 view implementation (view_win32.h/.cpp)
#   - Win32 GL context manager (gl_manager_win32.h/.cpp)
#
# Dependency direction:
#   arhud_core (zero deps)
#       ^
#   arhud_platform (depends on core)  ← This library
#       ^
#   arhud_servers (depends on core+platform)
#       ^
#   arhud_drivers (depends on core+servers+platform)
#
# Future: Add Linux/Android/QNX platform implementations here
#
# ===========================================================================

# --- Source file list ----------------------------------------------------
set(PLATFORM_SOURCES
    windows/window_win32.cpp      # Win32 window implementation (CreateWindowEx, WndProc, PollEvents)
    windows/screen_win32.cpp      # Win32 screen implementation (EnumDisplayMonitors, MonitorInfo)
    windows/display_server_win32.cpp # Win32 display server implementation (multi-window/screen/view)
    windows/view_win32.cpp           # Win32 view implementation (Win32View, HUD layered rendering views)
    windows/gl_manager_win32.cpp     # Win32 GL context manager (WGL, pixel format, 3.3 core profile, VSync)
)

# --- Header file list ----------------------------------------------------
set(PLATFORM_HEADERS
    window.h                      # Platform-independent window interface (IWindow, WindowDesc, WindowCallbacks)
    screen.h                      # Platform-independent screen interface (IScreen, ScreenInfo)
    display_server.h              # Display server interface (IDisplayServer, multi-window/screen management)
    view.h                        # View interface for HUD layered rendering (IView, ViewConfig, ViewType)
    gl_manager.h                   # Platform-independent GL context manager interface (IGLManager)
    windows/window_win32.h        # Win32 window implementation (Win32Window, WndProc, CreateWindowEx)
    windows/screen_win32.h        # Win32 screen implementation (Win32Screen, EnumDisplayMonitors)
    windows/display_server_win32.h # Win32 display server implementation (Win32DisplayServer)
    windows/view_win32.h           # Win32 view implementation (Win32View, HUD layered rendering views)
    windows/gl_manager_win32.h     # Win32 GL context manager (WGL, pixel format, 3.3 core profile, VSync)
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
    target_link_libraries(arhud_platform PUBLIC user32 opengl32)
endif()
