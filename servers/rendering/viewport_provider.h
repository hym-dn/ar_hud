// Copyright (c) 2026 ARHud Project
//
// Resolves a viewport index to its RenderViewport (ADR-0007).
//
// Camera/light commands run on the render thread inside
// RenderServer::Apply*, while the state they touch (ViewportSceneState) is
// owned by each RenderViewport. The server therefore does not keep a
// viewport list of its own; it parses the target viewport through this
// interface. ARHudEngine implements it (GetScreen(id)) and injects itself
// into RenderServer.
//
// Pure pointer resolution: ownership never transfers; out-of-range or
// not-yet-created indices return nullptr.

#ifndef ARHUD_SERVERS_RENDERING_VIEWPORT_PROVIDER_H_
#define ARHUD_SERVERS_RENDERING_VIEWPORT_PROVIDER_H_

#include "typedefs.h"

namespace arhud {

class RenderViewport;  // Forward declaration: only the pointer type is named
                       // here, so this header avoids pulling in
                       // render_viewport.h (see SKILL §2).

// Parses a viewport index into its RenderViewport.
class IViewportProvider {
 public:
  virtual ~IViewportProvider() = default;

  // Returns the viewport owned by the implementer at |index| (the
  // ARHudEngine::ViewportID space), or nullptr when out of range.
  virtual RenderViewport* GetViewport(uint32 index) = 0;
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_VIEWPORT_PROVIDER_H_
