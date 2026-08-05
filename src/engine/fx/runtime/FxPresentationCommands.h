#pragma once

#include "core/container/container_types.h"

#include "presentation/fx/runtime/FxPresentationSnapshot.h"
#include "presentation/fx/content/FxListCatalog.h"

#include <cstdint>
#include <cstddef>
namespace engine::fx {

inline constexpr size_t kMaximumFxBonePoseDemands = 65536;
inline constexpr size_t kMaximumFxBonePoseSamples = 65536;

struct FxCommandIdentity final {
    uint64_t eventId = 0;
    uint64_t confirmedFrame = 0;
    uint64_t variationSeed = 0;
};

// Sparse renderer request for one object's exact bone or contiguous numbered
// bone sequence. A zero numberedPointLimit means exact lookup. includeBare
// adds the unnumbered bone before prefix01..prefixNN, matching FXListAtBone.
struct FxBonePoseDemand final {
    uint64_t objectKey = 0;
    container::String boneName;
    uint32_t numberedPointLimit = 0;
    bool includeBare = false;

    [[nodiscard]] bool valid() const noexcept {
        return objectKey != 0 && !boneName.empty();
    }
};

struct FxSoundCommand final {
    FxCommandIdentity identity;
    container::String eventName;
    FxTypedAnchor anchor;
};

struct FxRayCommand final {
    FxCommandIdentity identity;
    container::String objectTemplate;
    LegacyBeamTemplate descriptor;
    bool templateResolved = false;
    FxTypedAnchor primary;
    FxTypedAnchor secondary;
};

struct FxLaserCommand final {
    FxCommandIdentity identity;
    FxPresentationDirectBeam::Control control =
        FxPresentationDirectBeam::Control::Begin;
    uint64_t beamIdentity = 0;
    int32_t sizeDeltaFrames = 0;
    uint32_t decayFrames = 0;
    LegacyBeamTemplate descriptor;
    FxTypedAnchor primary;
    FxTypedAnchor secondary;
};

struct FxRopeCommand final {
    FxCommandIdentity identity;
    FxPresentationDirectRope rope;
    FxTypedAnchor anchor;
};

struct FxTracerCommand final {
    FxCommandIdentity identity;
    container::String tracerName;
    FxTypedAnchor primary;
    FxTypedAnchor secondary;
    float speed = 0.0f;
    float decayAt = 1.0f;
    float length = 10.0f;
    float width = 1.0f;
    ParticleColor color{255, 255, 255};
};

struct FxLightPulseCommand final {
    FxCommandIdentity identity;
    FxTypedAnchor anchor;
    ParticleColor color;
    float radius = 0.0f;
    uint32_t increaseTimeMilliseconds = 0;
    uint32_t decreaseTimeMilliseconds = 0;
};

struct FxTerrainScorchCommand final {
    FxCommandIdentity identity;
    FxWorldPositionAnchor anchor;
    FxTerrainScorch type = FxTerrainScorch::Random;
    float radius = 0.0f;
};

struct FxViewShakeCommand final {
    FxCommandIdentity identity;
    FxViewShake type = FxViewShake::Normal;
    FxWorldPositionAnchor anchor;
};

// Particle nuggets are admitted directly into ParticleRuntime. The remaining
// typed commands are drained once by their presentation owners. Keeping the
// batch value-only makes it safe to cross renderer/audio ownership boundaries.
struct FxPresentationCommandBatch final {
    uint64_t sessionEpoch = 0;
    container::Vector<FxSoundCommand> sounds;
    container::Vector<FxRayCommand> rays;
    container::Vector<FxLaserCommand> lasers;
    container::Vector<FxRopeCommand> ropes;
    container::Vector<FxTracerCommand> tracers;
    container::Vector<FxLightPulseCommand> lightPulses;
    container::Vector<FxTerrainScorchCommand> terrainScorches;
    container::Vector<FxViewShakeCommand> viewShakes;

    [[nodiscard]] size_t size() const noexcept {
        return sounds.size() + rays.size() + lasers.size() + ropes.size() + tracers.size() +
            lightPulses.size() + terrainScorches.size() + viewShakes.size();
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
};

[[nodiscard]] const FxPresentationAnchor& worldTransform(
    const FxTypedAnchor& anchor) noexcept;

} // namespace engine::fx
