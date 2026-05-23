@echo off
setlocal

REM ============================================================================
REM ARHud Engine -- Windows Build Script
REM ============================================================================
REM
REM Usage:
REM   build              Debug build (default)
REM   build release      Release build
REM   build clean        Remove build/
REM   build conan        Conan install + build
REM
REM Requirements:
REM   - Visual Studio 2022+ with "Desktop development with C++" workload
REM   - Run from "Developer Command Prompt for VS 2022"
REM     OR run "vcvarsall.bat x64" first to set up environment
REM
REM Output:
REM   build/bin/   <- executables (.exe)
REM   build/lib/   <- libraries (.lib)
REM   Multi-config generators (VS) auto-append /Debug or /Release
REM
REM ============================================================================

REM --- Initialize variables ---
set PROJECT_ROOT=%~dp0
set BUILD_TYPE=Debug

REM --- Parse command line arguments ---
if /i "%1"=="release"   set BUILD_TYPE=Release
if /i "%1"=="clean"     goto :clean
if /i "%1"=="conan"     goto :conan_build

REM ============================================================================
REM Standard build (no Conan)
REM ============================================================================

echo [ARHud] Configuring %BUILD_TYPE% ...

cmake -B build -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DARHUD_API_OPENGL=ON -DARHUD_API_VULKAN=OFF -DARHUD_BUILD_TESTS=OFF
if errorlevel 1 (
    echo [ARHud] CMake configuration FAILED
    exit /b 1
)

echo [ARHud] Compiling %BUILD_TYPE% ...

cmake --build build --config %BUILD_TYPE%
if errorlevel 1 (
    echo [ARHud] Build FAILED
    exit /b 1
)

echo [ARHud] Build SUCCEEDED
goto :end

REM ============================================================================
REM Clean build directory
REM ============================================================================
:clean
echo [ARHud] Removing build/ ...
if exist build (
    rmdir /s /q build
    echo [ARHud] build/ removed
) else (
    echo [ARHud] build/ does not exist, nothing to clean
)
goto :end

REM ============================================================================
REM Conan build (recommended)
REM ============================================================================
:conan_build
echo [ARHud] Conan install + build (%BUILD_TYPE%)

where conan >nul 2>nul
if errorlevel 1 (
    echo [ARHud] ERROR: conan not found in PATH.
    echo [ARHud] Install with: pip install conan
    exit /b 1
)

conan install . --build=missing -s build_type=%BUILD_TYPE%
if errorlevel 1 (
    echo [ARHud] Conan install FAILED
    exit /b 1
)

cmake --preset conan-default
if errorlevel 1 (
    echo [ARHud] CMake preset configuration FAILED
    exit /b 1
)

cmake --build build --config %BUILD_TYPE% --parallel
if errorlevel 1 (
    echo [ARHud] Build FAILED
    exit /b 1
)

echo [ARHud] Build SUCCEEDED
goto :end

:end
endlocal
