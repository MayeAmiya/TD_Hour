#pragma once

#include "engine/renderer/world/pipeline/WorldInterpolationTimeline.h"
#include "presentation/render/RenderViewSnapshot.h"

#include <cstdint>
#include <optional>

namespace engine::render {

// Renderer-local view endpoints. These can advance while simulation is
// paused and therefore have a lifecycle separate from world snapshots and
// GPU asset residency.
struct WorldViewRuntime final {
    RenderViewState current;
    std::optional<RenderViewState> latest;
    std::optional<PresentationCameraOverride> presentationCameraOverride;
    uint64_t nextRevision = 1;
    WorldInterpolationTimeline worldInterpolation;
    WorldInterpolationTimeline cameraInterpolation;
};

} // namespace engine::render
