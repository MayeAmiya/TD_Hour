#pragma once

#include "core/container/container_types.h"

#include "presentation/fx/content/ParticleSystemCatalog.h"
#include "presentation/fx/content/FxListCatalog.h"
#include "presentation/fx/runtime/LegacyBeamTemplate.h"
#include "presentation/fx/runtime/ParticleClock.h"
#include "presentation/contracts/PresentationPlayerAudience.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
namespace engine::fx {

inline constexpr size_t kMaximumNumberedW3dBonePoints = 99;

enum class FxPresentationAnchorKind : uint8_t {
    ObjectAttachment,
    WorldPosition,
    BonePosition,
};

enum class FxPresentationControlKind : uint8_t {
    Execute,
    StopAttachedParticleGroup,
    StopAllAttachedParticles,
};

struct FxPresentationAnchor final {
    uint64_t objectKey = 0;
    // A bone FX can address a render-channel submodel rather than its owning
    // gameplay draw key. Carry the source's already-frozen audience with the
    // fallback transform so delayed renderer FX sounds remain correctly
    // filtered even if that gameplay object has since retired.
    presentation::PlayerAudience audience;
    ParticleVector3 position;
    float rollRadians = 0.0f;
    float pitchRadians = 0.0f;
    float yawRadians = 0.0f;
    float objectBoundingCircleRadius = 0.0f;
};

// Typed presentation anchors keep attachment lifetime semantics explicit.
// Every object/bone anchor carries a detached world fallback captured at the
// confirmed boundary, so one-shot or delayed commands never need to retain a
// live ECS entity after submission.
struct FxWorldPositionAnchor final {
    FxPresentationAnchor world;
};

struct FxObjectAnchor final {
    uint64_t objectKey = 0;
    FxPresentationAnchor fallback;
};

struct FxBoneAnchor final {
    uint64_t objectKey = 0;
    container::String boneName;
    FxPresentationAnchor fallback;
    ParticleVector3 worldOffset;
};

using FxTypedAnchor = std::variant<
    FxWorldPositionAnchor,
    FxObjectAnchor,
    FxBoneAnchor>;

struct FxPresentationDirectParticle final {
    container::String particleSystemName;
    container::String fallbackParticleSystemName;
    std::optional<ParticleVector3> fallbackColorKeyTint;
    uint32_t emitterCount = 1;
    std::optional<uint32_t> systemLifetimeFrames;
    float footprintMajorRadius = 0.0f;
    float footprintMinorRadius = 0.0f;
    float maximumHeight = 0.0f;
    uint32_t initialDelayMinimumFrames = 0;
    uint32_t initialDelayMaximumFrames = 0;
    bool boxFootprint = false;
    bool attachToObject = false;
};

struct FxPresentationDirectBeam final {
    container::String objectTemplate;
    enum class Control : uint8_t {
        Begin,
        Update,
        End,
    } control = Control::Begin;
    uint64_t beamIdentity = 0;
    int32_t sizeDeltaFrames = 0;
    uint32_t decayFrames = 0;
};

struct FxPresentationDirectScorch final {
    FxTerrainScorch type = FxTerrainScorch::Random;
    float radius = 0.0f;
};

enum class FxPresentationRopeControl : uint8_t {
    Begin,
    Update,
    End,
};

struct FxPresentationDirectRope final {
    FxPresentationRopeControl control = FxPresentationRopeControl::Begin;
    uint64_t ropeIdentity = 0;
    float maximumLength = 1.0f;
    float currentLength = 0.0f;
    float width = 0.5f;
    ParticleVector3 color;
    float wobbleLength = 1.0f;
    float wobbleAmplitude = 0.0f;
    float wobbleRatePerFrame = 0.0f;
    float wobblePhase = 0.0f;
    float verticalOffset = 0.0f;
    float currentSpeedPerFrame = 0.0f;
    float maximumSpeedPerFrame = 0.0f;
    float accelerationPerFrame = 0.0f;
};

// Immutable logic-terrain heightfield carried only with FX batches that have
// invocations. CreateAtGroundHeight samples this after deterministic nugget
// offset/radius expansion, matching the original final-(x,y) query without a
// callback into live TerrainLogic.
struct FxGroundHeightFieldSnapshot final {
    int32_t width = 0;
    int32_t height = 0;
    int32_t borderSize = 0;
    float cellWorldSize = 10.0f;
    float heightWorldScale = 0.625f;
    container::Vector<uint8_t> heights;
};

struct FxPresentationInvocation final {
    container::String fxListName;
    std::optional<FxPresentationDirectParticle> directParticle;
    std::optional<FxPresentationDirectBeam> directBeam;
    std::optional<FxPresentationDirectScorch> directScorch;
    std::optional<FxPresentationDirectRope> directRope;
    FxPresentationControlKind control = FxPresentationControlKind::Execute;
    FxPresentationAnchorKind anchorKind = FxPresentationAnchorKind::WorldPosition;
    FxPresentationAnchor primary;
    std::optional<FxPresentationAnchor> secondary;
    container::String attachmentBoneName;
    bool attachmentBoneNameIsPrefix = false;
    uint32_t attachmentBoneSequenceOrdinal = 0;
    bool attachmentBonePrefixFallsBackToBare = false;
    container::String secondaryBoneName;
    bool secondaryBoneNameIsPrefix = false;
    uint32_t secondaryBoneSequenceOrdinal = 0;
    bool secondaryBonePrefixFallsBackToBare = false;
    ParticleVector3 secondaryWorldOffset;
    bool inheritResolvedAnchorOrientation = true;
    ParticleVector3 attachmentLocalOffset;
    uint64_t attachmentGroup = 0;
    float primarySpeed = 0.0f;
    float overrideRadius = 0.0f;
    // Sampled from logic-owned terrain during extraction. Nuggets authored
    // with CreateAtGroundHeight consume this sealed value rather than asking
    // the renderer to query mutable map state.
    std::optional<float> groundHeight;
    // Monotonic within one presentation epoch. Normal GameSession traffic
    // uses this cursor for exactly-once consumption without an unbounded ID
    // set; direct diagnostic/import invocations may leave it zero.
    uint64_t streamSequence = 0;
    uint64_t eventId = 0;
    uint64_t confirmedFrame = 0;
    uint64_t variationSeed = 0;
};

struct FxPresentationBonePose final {
    uint64_t objectKey = 0;
    container::String boneName;
    FxPresentationAnchor anchor;
};

struct FxModelParticleEmitterPose final {
    uint64_t emitterKey = 0;
    uint64_t objectKey = 0;
    container::String boneName;
    container::String particleSystem;
    FxPresentationAnchor anchor;
};

// Complete declared set for the three vehicle Draw subclasses. Stable keys,
// active state and confirmed multipliers let FxRuntime preserve Draw-owned
// emitters across start/stop without deriving motive/turning/shroud state from
// renderer timing.
struct FxVehicleParticleEmitterPose final {
    uint64_t emitterKey = 0;
    uint64_t objectKey = 0;
    container::String particleSystem;
    FxPresentationAnchor anchor;
    ParticleVector3 velocityMultiplier{1.0f, 1.0f, 1.0f};
    float burstCountMultiplier = 1.0f;
    float sizeMultiplier = 1.0f;
    uint64_t triggerSequence = 0;
    bool active = false;
};

struct FxPresentationSnapshot final {
    uint64_t sessionEpoch = 0;
    uint64_t simulationFrame = 0;
    // The confirmed session tick rate is frozen at GameSession start. Particle
    // authoring remains 30 Hz even when a game runs 45/60 confirmed ticks per
    // second, so FxRuntime must not infer this value from render cadence.
    uint32_t logicFramesPerSecond = kParticleAuthoredFramesPerSecond;
    uint32_t visibilityRejectedObjects = 0;
    uint32_t visibilityRejectedInvocations = 0;
    container::SharedPtr<const FxGroundHeightFieldSnapshot> groundHeights;
    container::SharedPtr<const LegacyBeamTemplateCatalog> legacyBeamTemplates;
    container::Vector<FxPresentationInvocation> invocations;
    // Complete latest transform/liveness set for AttachToObject emitters.
    // One-shot invocations remain lossless and ordered; this companion set is
    // intentionally replaceable latest state keyed by stable ObjectId.
    container::Vector<FxPresentationAnchor> objects;
    container::Vector<FxVehicleParticleEmitterPose> vehicleEmitters;
};

} // namespace engine::fx
