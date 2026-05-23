# AGENTS.md — AR HUD Engine

## Project state

Early-stage C++ AR HUD rendering engine. Only `core/` library exists (types, memory, threading, logging, containers, UTF-32 string). No render, platform, scene, or app code yet. No tests.

## Actual layout

```
core/               ← arhud_core static library (zero external deps)
├── typedefs.h      ← Base types, platform macros, Error enum
├── os/             ← memory, thread, sync (SpinLock/Mutex/RWLock/Semaphore)
├── io/             ← logger (StdLogger/CompositeLogger/FileLogger)
├── template/       ← safe_refcount, paged_allocator, local_vector, hash_map
└── string/         ← ustring (UTF-32 string, LocalVector-based)
cmake/              ← option.cmake, build.cmake, dir.cmake, lib.cmake
skill/              ← arch_skill.md, coding_standard_skill.md
```

File paths are relative to project root (no `src/` prefix). Include as `"typedefs.h"` — the `core/` dir and its subdirs are in the global include path.

## Build

```
# Direct CMake
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug

# Conan 2.x (recommended, manages deps)
conan install . --build=missing -s build_type=Debug
cmake --preset conan-default
cmake --build build/Debug

# Windows script (needs VS 2022 dev prompt or vcvarsall.bat x64 first)
build.bat            # Debug
build.bat release    # Release
build.bat conan      # Conan + Debug
build.bat clean      # Remove build/
```

Build output: `build/bin/` (exes), `build/lib/` (libs).

## Dependencies (Conan)

`arhud_core` has zero external deps. Dependencies needed when render/ is created: `glm/1.0.1` (math), `glad/2.0.8` (OpenGL loader), `freetype/2.13.3` (fonts), `stb/cci.20250126` (images). Deps are listed in `conanfile.py:requirements()`.

## CMake loading order

`CMakeLists.txt` → `cmake/option.cmake` → `cmake/build.cmake` → `cmake/dir.cmake` → `cmake/lib.cmake` → `add_subdirectory(core)`

## Adding new source files

Manually append to the explicit list in `core/CMakeLists.txt` (no `file(GLOB ...)`).

## Build options

| Option               | Default | Description                        |
|----------------------|---------|------------------------------------|
| `ARHUD_API_OPENGL`   | ON      | Enable OpenGL backend              |
| `ARHUD_API_VULKAN`   | OFF     | Not yet implemented                |
| `ARHUD_BUILD_TESTS`  | OFF     | Build unit tests                   |
| `ARHUD_BUILD_TOOLS`  | OFF     | Build developer tools              |
| `ARHUD_ENABLE_CXX20` | OFF     | C++20 instead of default C++17     |
| `BUILD_SHARED_LIBS`  | OFF     | Shared instead of static libs      |

## Coding conventions

| Element              | Style              | Example                            |
|----------------------|--------------------|------------------------------------|
| types/classes/enums  | PascalCase         | `SafeNumeric`, `LogLevel`, `Error` |
| functions/methods    | PascalCase         | `CreateTexture()`, `BufferMap()`   |
| variables (local/param) | snake_case      | `texture_id`, `buffer_size`        |
| member variables     | snake_case + `_`   | `texture_id_`, `buffer_size_`      |
| constants/enum values | k + PascalCase    | `kMaxTextures`, `kOutOfMemory`     |
| macros               | UPPER_SNAKE_CASE   | `ARHUD_PLATFORM_WINDOWS`           |
| namespaces           | snake_case         | `arhud::render::opengl`            |
| files/directories    | snake_case         | `texture_storage.h`                |

- `#pragma once` for include guards (actual code — coding standard docs say `#ifndef` but the codebase uses pragma)
- Doxygen Javadoc style (`/** ... */`), `@brief`, `@param[in/out]`, `@return` required on public API
- 2-space indent, 100-char line width
- Allman braces for classes, K&R for control flow
- `*`/`&` attached to type: `int* a`, `int& b`
- `enum class` only; `using` over `typedef`; `override`/`final` required
- Include order: associated header → C stdlib → C++ stdlib → third-party → project headers
- Forbidden: exceptions, RTTI, iostream, `malloc`/`free`, `printf`/`sprintf`, bare `new`/`delete`, `goto`
- `auto` allowed but avoid where it obscures type

## Architecture (from `skill/arch_skill.md`)

Multi-layer GPU abstraction modeled on Godot 4.6: ContextDriver → DeviceDriver → RenderingDevice → Compositor → Storage → Renderer. OpenGL first, Vulkan later; Windows first, then Linux/Android/QNX. 2D HUD canvas layers (Z-order), no full 3D scene. PagedAllocator for hot path, PIMPL for platform abstraction, budget limits for QNX safety. No exceptions, no RTTI, no COW containers.

## Skill files

- `skill/arch_skill.md` — full rendering engine architecture reference (Godot 4.6 analysis + ARHud design)
- `skill/coding_standard_skill.md` — detailed Google C++ style + Doxygen rules
