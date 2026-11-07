# AGENTS.md — AR HUD Engine

## Project state

C++ AR HUD rendering engine modeled on Godot 4.6. Five static libraries + 5 demo executables + GoogleTest (25 cases). Windows/OpenGL only; Linux/Android/QNX/Vulkan are stubs. Route B landed through B3 (2026-09-03): render thread + per-screen present threads, retained-mode RenderServer, offscreen GL context + FBO→GDI present path. ItemStorage tree hierarchy landed (ADR-0005, 2026-09-19). Next step: B3-P (see `skill/arhud-improvement`).

## Documentation & process（文档体系与正规流程）

- **文档地图**：`docs/README.md` — 全部文档的职责边界与同步义务
- **开发流程**：`docs/process/dev-workflow.md` — 变更分级（A 小修/B 功能/C 架构）+ 任务生命周期 6 阶段；C 类（分层/线程/契约）必须方案先行（skill 章节 + ADR）
- **验证手册**：`docs/process/verification.md` — 构建/测试/冒烟/治理 grep 命令 + 已知坑（编码事故、Conan 缓存、demo 超时）
- **完成定义**：`docs/process/definition-of-done.md` — 归档前 checklist
- **决策记录**：`docs/adr/` — 架构决策 ADR（只增不改）；模板在 `docs/templates/`
- **开发日志**：`docs/devlog/` — 每次代码变更一篇（`YYYY-MM-DD_主题.md`）
- `skill/` 保留 4 份持续状态的设计规范（现状架构 / Route B / 多屏 / 改进路线图）

## Library layout & dependency chain

```
arhud_core       (glm)              ← Base types, containers, sync, logger, math
    ↑
arhud_scene      (core)             ← Scene tree (placeholder, Phase 6)
    ↑
arhud_platform   (core + user32 + gdi32 + opengl32)  ← Win32 window/screen/display_server/frame_presenter/GL context
    ↑
arhud_drivers    (core + platform + glad) ← Contracts (RCD/RDD/CommandBuffer at root) + GL impls (gl/)
    ↑
arhud_servers    (core + scene + platform + drivers + glm + freetype)  ← RD, storage, renderer, compositor
    ↑
arhud_demo_*     (all above + glad) ← Per-phase demo executables
```

**契约归位（2026-08-30）**：RCD/RDD/CommandBuffer 契约头住 `drivers/` 根，GL 实现住 `drivers/gl/`——对称 platform/ 模式（`platform/window.h` ↔ `platform/windows/`）。drivers 不链接 servers（倒挂与 Freetype 传染已消除）；servers PUBLIC 链 drivers（公共头消费契约 + arhud_engine.cpp 装配根注入实现）。

## Directory map

```
core/                  ← arhud_core: types, containers, sync, logger, math, string
├── typedefs.h         ← Base types (uint8/uint16/uint32/uint64/float32/float64), platform macros, Error enum, ARHUD_NEW/DELETE
├── os/                ← memory, thread, sync (SpinLock/Mutex/RWLock/Semaphore)
├── io/                ← logger (StdLogger/CompositeLogger/FileLogger)
├── template/          ← paged_allocator, local_vector, hash_map, rid, bitfield, ref, weak_ref, ...
│                        (ref/weak_ref = Object-layer ownership, reserved for Phase 6, 0 users by design — see skill/arhud-architecture §八)
├── string/            ← ustring (UTF-32, LocalVector-based)
└── math/              ← math_funcs (Vec/Mat/Color/Transform2D), glm_types

scene/                 ← arhud_scene: empty placeholder library (Phase 6 will host scene tree)

platform/              ← arhud_platform: interfaces at root, Win32 impls in windows/
├── window.h, screen.h, display_server.h, gl_context_manager.h, frame_presenter.h
└── windows/           ← window_win32, screen_win32, display_server_win32, gl_context_manager_win32, frame_presenter_win32

drivers/               ← arhud_drivers: 渲染后端契约（根）+ GL 实现（gl/），对称 platform/ 模式
├── rendering_context_driver.h    ← RCD interface（契约）
├── rendering_device_driver.h     ← RDD interface（契约）
├── command_buffer.h              ← ICommandBuffer interface（契约）
├── rendering_device_commons.h    ← Shared enums/types/DEFINE_ID（契约）
└── gl/                           ← GL 实现
    ├── rendering_context_driver_gl.h/.cpp
    ├── rendering_device_driver_gl.h/.cpp（+ _commands/_format/_sync/_shader_cache 分文件）
    └── command_buffer_gl.h/.cpp

servers/               ← arhud_servers: rendering engine
└── rendering/
    ├── rendering_device.h/.cpp       ← High-level RD (resource lifecycle; + _draw_list/_frame/_staging split files)
    ├── command_queue.h/.cpp          ← SPSC serialized ring buffer (main→render thread, ≈Godot CommandQueueMT)
    ├── command_executor.h/.cpp       ← Render-thread command executor (deserializes → Storage)
    ├── render_commands.h             ← RenderCommandType enum + command structs + ICommandExecutor
    ├── render_server.h/.cpp          ← Main-thread API proxy (retained mode, app's ONLY resource entry)
    ├── render_viewport.h/.cpp          ← Lightweight per-screen viewport (FBO + post-process + presenter ref)
    │                                   + ViewportSceneState scene_state_ (per-viewport camera/light, ADR-0007)
    ├── viewport_provider.h           ← IViewportProvider: resolve RenderViewport by index (camera/light routing, ADR-0007)
    ├── arhud_engine.h/.cpp           ← Multi-screen engine (PIMPL; owns shared stack + render thread)
    ├── frame_image_queue.h           ← Per-screen FrameImage state machine (B3: FREE→…→ON_SCREEN→FREE)
    ├── present_thread.h              ← Per-screen present thread (no GL, platform present API, B3)
    ├── render_buffers.h              ← Render target encapsulation
    ├── draw_list_scope.h             ← RAII DrawList/ComputeList wrapper
    ├── storage/                      ← 两类容器（命名同源，语义不同）
    │   ├── shader_storage.h, texture_storage.h/.cpp   ← 资源 Storage（5）：GPU-backed
    │   ├── material_storage.h/.cpp, mesh_storage.h/.cpp, font_storage.h/.cpp
    │   │                            RID→RD*_ID，ARHudCompositor 持有，纳入 Initialize(rd)
    │   ├── item_storage.h/.cpp       ← 状态 Storage（1）：CPU-only、零 GPU 依赖，ARHudEngine 持有
    │   │   Retained-mode per-item state + dirty bit + item hierarchy
    │   │   (ADR-0005: parent chain, effective transform/opacity/visibility/z_as_relative)
    │   └── viewport_scene_state.h    ← per-viewport scene state (camera/light; DirectionalLight), ADR-0007
    └── renderer/                     ← Backend-agnostic renderer
        ├── compositor.h/.cpp         ← ARHudCompositor (subsystem lifecycle)
        ├── canvas_renderer.h/.cpp    ← 2D batch renderer
        ├── spatial_renderer.h/.cpp   ← 3D depth+light renderer
        └── shaders/                  ← Inline GLSL source (hud_canvas.h, hud_text.h, hud_spatial.h, hud_composite.h)

app/                   ← Demo executables (require -DARHUD_BUILD_DEMO=ON or per-phase flag)
├── phase2_rdc_rdd_rd/ ← RCD→RDD→RD pipeline validation
├── phase4_canvas/     ← 2D Canvas rendering
├── phase4_spatial/    ← 3D Spatial rendering
├── phase4_compositor/ ← 2D/3D mixed rendering
└── phase4_text/       ← SDF text rendering (requires FreeType)

cmake/                 ← option.cmake → build.cmake → dir.cmake → lib.cmake (loaded in this order)
docs/                  ← Documentation center (map: docs/README.md)
├── process/           ← dev-workflow / verification / definition-of-done
├── templates/         ← devlog & ADR templates
├── adr/               ← Architecture decision records (append-only; index: docs/adr/README.md)
└── devlog/            ← Dated development logs (one per code change)
skill/                 ← Architecture/governance/roadmap skills (one SKILL.md per subdirectory)
├── arhud-architecture/         ← Current architecture state (5-layer, thread model, components)
├── arhud-render-thread-arch/   ← Route B design (render-to-thread, RenderServer, CommandQueue, retained mode)
├── arhud-multiscreen-arch/     ← Multi-screen design (shared stack + render/present split + present threads)
└── arhud-improvement/          ← Next-step improvement plan (B3-M0/B3/B3-P/B4/B5 + governance items)
```

## Build

```
# Windows script (needs VS 2022 dev prompt or vcvarsall.bat x64 first)
build.bat              # Debug
build.bat release      # Release
build.bat conan        # Conan install + Debug build (recommended)
build.bat clean        # Remove build/

# Direct CMake (no Conan)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug

# Conan 2.x
conan install . --build=missing -s build_type=Debug
cmake --preset conan-default
cmake --build build --config Debug   # Windows VS multi-config: binary dir is build/ (no /Debug subdir)
```

Output: `build/bin/` (exes), `build/lib/` (libs). Multi-config (VS) appends `/Debug` or `/Release`.

**CMake option gotcha**: `app/CMakeLists.txt` re-declares per-demo `option()`s with default `OFF`, but `cmake/option.cmake` (loaded first) sets them `ON`. The first `option()` call wins in CMake cache, so the real defaults are all `ON` as defined in `option.cmake`.

## Adding new source files

Manually append to the explicit list in the module's `CMakeLists.txt` (no `file(GLOB ...)`).

## Build options

| Option               | Default | Description                        |
|----------------------|---------|------------------------------------|
| `ARHUD_API_OPENGL`   | ON      | Enable OpenGL backend              |
| `ARHUD_API_VULKAN`   | OFF     | Not yet implemented                |
| `ARHUD_BUILD_TESTS`  | ON*     | Build unit tests (*direct CMake default ON per `cmake/option.cmake`; Conan toolchain caches its own value — see gotcha #1 in `docs/process/verification.md`) |
| `ARHUD_BUILD_TOOLS`  | OFF     | Build developer tools              |
| `ARHUD_BUILD_DEMO`   | ON      | Build ALL demo executables         |
| `ARHUD_DEMO_PHASE2`  | ON      | Build Phase 2 demo (RCD/RDD/RD)   |
| `ARHUD_DEMO_PHASE4`  | ON      | Build all Phase 4 demos            |
| `ARHUD_DEMO_PHASE4_CANVAS`     | ON | Build Phase 4 Canvas demo (2D)    |
| `ARHUD_DEMO_PHASE4_SPATIAL`    | ON | Build Phase 4 Spatial demo (3D)   |
| `ARHUD_DEMO_PHASE4_COMPOSITOR` | ON | Build Phase 4 Compositor demo     |
| `ARHUD_DEMO_PHASE4_TEXT`       | ON | Build Phase 4 Text demo (SDF)     |
| `ARHUD_ENABLE_CXX20` | OFF     | C++20 instead of default C++17     |
| `BUILD_SHARED_LIBS`  | OFF     | Shared instead of static libs      |

## Dependencies (Conan)

`arhud_core` links `glm/1.0.1` (header-only math). Other deps: `glad/2.0.8` (GL loader), `freetype/2.13.3` (fonts), `stb/cci.20240531` (images), `glfw/3.4` (test windows). All in `conanfile.py:requirements()`.

## Build artifacts

`shader_cache/` contains binary shader cache files generated at runtime by demos. Do not edit or commit new entries; safe to delete entirely.

## Include path gotcha

Global `include_directories()` in `cmake/dir.cmake` adds `core/` and all its subdirs plus `scene/` subdirs. Each module also sets `target_include_directories()` PUBLIC for its own headers. Some paths in `dir.cmake` reference nonexistent dirs (`core/platform`, `core/rendering/driver`) — harmless but stale. When adding a new module, add its include dirs via `target_include_directories()`, not the global list.

**Layering rule**: headers under `servers/rendering/storage/` must NOT include `renderer/` — the dependency is `renderer → storage`, one direction only. Shared plain-data types (e.g. `DirectionalLight` in `item_storage.h`) live in storage; renderers consume them.

## Coding conventions

| Element              | Style              | Example                            |
|----------------------|--------------------|------------------------------------|
| types/classes/enums  | PascalCase         | `SafeNumeric`, `LogLevel`, `Error` |
| functions/methods    | PascalCase         | `CreateTexture()`, `BufferMap()`   |
| variables (local/param) | snake_case      | `texture_id`, `buffer_size`        |
| class member variables | snake_case + `_`   | `texture_id_`, `buffer_size_`      |
| struct member variables | snake_case (no suffix) | `object`, `generation`, `width` |
| constants/enum values | k + PascalCase    | `kMaxTextures`, `kOutOfMemory`     |
| macros               | UPPER_SNAKE_CASE   | `ARHUD_PLATFORM_WINDOWS`           |
| namespaces           | snake_case         | `arhud::render::opengl`            |
| files/directories    | snake_case         | `texture_storage.h`                |

- `#pragma once` for include guards
- Doxygen `/** ... */` with `@brief`, `@param[in/out]`, `@return` on public API
- 2-space indent, 100-char line width
- Allman braces for classes, K&R for control flow
- `*`/`&` attached to type: `int* a`, `int& b`
- `enum class` only; `using` over `typedef`; `override`/`final` required
- Include order: associated header → C stdlib → C++ stdlib → third-party → project headers
- **Forbidden**: iostream, `malloc`/`free`, `printf`/`sprintf`, bare `new`/`delete`, `goto`
- Exceptions/RTTI are **enabled** (ADR-0004); engine code still never throws — errors return `Error` enum values
- `auto` allowed but avoid where it obscures type

## Architecture

Multi-layer GPU abstraction modeled on Godot 4.6: RCD → RDD → RD → Compositor → Storage → Renderer. OpenGL first, Vulkan later; Windows first, then Linux/Android/QNX. 2D HUD canvas layers (Z-order), no full 3D scene. PagedAllocator for hot path, PIMPL for platform abstraction, budget limits for QNX safety. No COW containers; engine code never throws (exceptions/RTTI enabled, ADR-0004).

Reference: `skill/arhud-architecture` (current state), `skill/arhud-render-thread-arch` (Route B), `skill/arhud-multiscreen-arch` (multi-screen), `skill/arhud-improvement` (roadmap), `docs/adr/` (decision records).
