#pragma once

#include "presentation/render/RenderWorldDescriptorContracts.h"

namespace engine::render {

enum class ProjectileTrailBlendMode : uint8_t {
    Additive,
    Alpha,
    Multiply,
    Opaque,
};

enum class ProjectileTrailDepthMode : uint8_t {
    TestNoWrite,
    TestWrite,
    Disabled,
};

// Renderer-only projectile presentation copied from authoritative fixed-point
// simulation state. Flight/collision remains entirely in ObjectProjectile.
// A trail is opt-in and carries the session-frozen ProjectileStream descriptor;
// ordinary shells no longer receive a global diagnostic ribbon.
struct ProjectileRenderSnapshot final {
    RenderEntityId objectId = 0;
    RenderEntityId launcherId = 0;
    RenderEntityId intendedTargetId = 0;
    // Frozen target used for diagnostics and renderer ordering. Gameplay owns
    // the exact fixed-point comparison and publishes trailChainIdentity.
    RenderVector intendedTargetPosition{};
    // Frozen target-run identity. Different identities are distinct stream
    // objects even if a later shot returns to the same object/position.
    uint32_t trailChainIdentity = 0;
    // WeaponSet activation generation. RefCode recreates Weapon (and thus its
    // ProjectileStream owner) whenever the selected WeaponSet changes.
    uint32_t trailOwnerGeneration = 0;
    uint32_t sourceShotSequence = 0;
    uint32_t sourceBarrelSequenceOrdinal = 0;
    uint64_t spawnedTick = 0;
    RenderVector position{};
    // RefCode owns one ProjectileStream object at the launcher/source
    // position captured when the stream is created. Shroud visibility is a
    // property of this owner anchor, never of each projectile endpoint.
    RenderVector trailOwnerAnchorPosition{};
    RenderVector forward{1.0f, 0.0f, 0.0f};
    container::String trailStreamName;
    container::String trailTexture;
    uint32_t trailStreamInstance = 0;
    RenderVector trailColor{1.0f, 1.0f, 1.0f};
    float trailAlpha = 1.0f;
    float trailWidth = 0.0f;
    float trailTileFactor = 1.0f;
    float trailScrollRate = 0.0f;
    // Session-frozen logic rate used to convert confirmed frame count into
    // the WW3D logic-time seconds consumed by UV scroll and optional linger.
    uint32_t trailLogicFramesPerSecond = 30;
    float trailLifetimeSeconds = 0.0f;
    uint32_t trailMaximumSegments = 0;
    ProjectileTrailBlendMode trailBlend = ProjectileTrailBlendMode::Additive;
    ProjectileTrailDepthMode trailDepth = ProjectileTrailDepthMode::TestNoWrite;
    uint8_t launchSlot = 0;
    bool hasForward = false;
    bool trailEnabled = false;
    bool trailOwnerAnchorVisible = true;
    // Friendly/allied projectiles remain observable independently of local
    // shroud. Enemy/neutral ProjectileStream visibility is frozen separately
    // at trailOwnerAnchorPosition; consumers must not return to endpoint
    // clipping when this compatibility bit is false.
    bool visibilityExempt = false;
    // Keeps an authored projector alive when the projectile model itself is
    // outside mesh culling but its terrain projection remains relevant.
    RenderShadowDescriptor shadow;
    float boundingRadius = 0.0f;
};


} // namespace engine::render
