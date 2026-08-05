#pragma once

#include "engine/renderer/world/resource/WorldStartupSceneTicket.h"

#include <cstdint>

namespace engine::render {

// Renderer-owned lifetime identity for one admitted game-world presentation.
// It is independent from per-frame view/scratch state and from long-lived
// asset residency, so retiring a match cannot accidentally reset either.
struct WorldPresentationLifetime final {
    uint64_t modelUploadPresentationEpoch = 0;
    uint64_t resetCount = 0;
    uint64_t staleWorldSnapshotRejectCount = 0;
    uint64_t staleFxSnapshotRejectCount = 0;
    uint64_t retiredPresentationEpoch = 0;
    uint64_t retiredSessionRevision = 0;
    bool started = false;
    bool loading = false;
    WorldStartupSceneTicket startupSceneTicket;
};

} // namespace engine::render
