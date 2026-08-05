#pragma once

#include "core/container/hash_containers.h"
#include "presentation/fx/content/ParticleSystemCatalog.h"
#include "presentation/fx/runtime/LegacyBeamTemplate.h"

#include <cstdint>
#include <optional>

namespace engine::fx {

struct LegacyLaserSegment final {
    ParticleVector3 start;
    ParticleVector3 end;
};

// Renderer-neutral reproduction of W3DLaserDraw's segment ratios, overlap
// and cosine arc. Terrain skimming is applied by the renderer because the
// frozen template does not own mutable map height data.
[[nodiscard]] container::Vector<LegacyLaserSegment> buildLegacyLaserSegments(
    const LegacyLaserTemplate& descriptor,
    ParticleVector3 start, ParticleVector3 end);
void buildLegacyLaserSegmentsInto(
    container::Vector<LegacyLaserSegment>& output,
    const LegacyLaserTemplate& descriptor,
    ParticleVector3 start, ParticleVector3 end);

// W3DLaserDraw keeps a texture's texels square along the beam by including
// the decoded level-zero width/height ratio in its tiled V coordinate.
// Invalid/missing texture metadata deliberately falls back to a square
// texture, matching the renderer's white fallback descriptor.
[[nodiscard]] float legacyLaserTextureTileFactor(
    const LegacyLaserTemplate& descriptor, float segmentLength,
    float beamWidth, float textureAspectRatio = 1.0f) noexcept;

struct LegacyModelRayState final {
    container::String modelAsset;
    ParticleVector3 position;
    float assetScale = 1.0f;
    // Zero means that the authored ThingTemplate has no LifetimeUpdate or
    // DeletionUpdate and therefore remains alive until the presentation epoch
    // is reset, matching an ordinary legacy Drawable.
    uint32_t lifetimeFrames = 0;
    bool castsDirectionalShadow = false;
};

// Builds the renderer-neutral portion of a model-only RayEffect.  The seed is
// the already-detached FX variation identity, so lifetime sampling cannot be
// perturbed by asset streaming or renderer frame rate.
[[nodiscard]] std::optional<LegacyModelRayState> makeLegacyModelRay(
    const LegacyBeamTemplate& descriptor, ParticleVector3 start,
    ParticleVector3 end, uint64_t deterministicIdentity);
[[nodiscard]] bool legacyModelRayAlive(
    const LegacyModelRayState& state, uint64_t admittedFrame,
    uint64_t simulationFrame) noexcept;

struct LegacyRopeState final {
    ParticleVector3 origin;
    LegacyBeamColor color{0.0f, 0.0f, 0.0f, 1.0f};
    float currentLength = 0.0f;
    float maximumLength = 1.0f;
    float width = 0.5f;
    float currentSpeedPerFrame = 0.0f;
    float maximumSpeedPerFrame = 0.0f;
    float accelerationPerFrame = 0.0f;
    float wobbleLength = 1.0f;
    float wobbleAmplitude = 0.0f;
    float wobbleRatePerFrame = 0.0f;
    float wobblePhase = 0.0f;
    float verticalOffset = 0.0f;
    container::Vector<float> wobbleAxisRadians;
};

[[nodiscard]] std::optional<LegacyRopeState> makeLegacyRope(
    ParticleVector3 origin, float length, float width,
    LegacyBeamColor color, float wobbleLength, float wobbleAmplitude,
    float wobbleRatePerFrame, uint64_t deterministicIdentity);
void setLegacyRopeLength(LegacyRopeState& state, float length) noexcept;
void setLegacyRopeSpeed(LegacyRopeState& state, float currentSpeedPerFrame,
                        float maximumSpeedPerFrame,
                        float accelerationPerFrame) noexcept;
void advanceLegacyRope(LegacyRopeState& state, uint32_t authoredFrames = 1) noexcept;
[[nodiscard]] container::Vector<LegacyLaserSegment> buildLegacyRopeSegments(
    const LegacyRopeState& state);
void buildLegacyRopeSegmentsInto(
    container::Vector<LegacyLaserSegment>& output,
    const LegacyRopeState& state);

} // namespace engine::fx
