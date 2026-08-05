#pragma once

#include "core/container/container_types.h"

#include "core/ecs/ObjectId.h"
#include "core/math/wwmath/base/wwmath.h"
#include "game/player/PlayerTypes.h"

#include <cstdint>
#include <optional>
namespace game {

// The original FXList has two invocation families: doFXObj receives primary
// and optional secondary object transforms, while doFXPos receives a detached
// world position. Both cross the confirmed-session boundary as copied values;
// neither an ECS entity nor renderer object is retained here.
enum class FxInvocationAnchorKind : uint8_t {
    ObjectAttachment,
    WorldPosition,
    // Renderer resolves this named W3D pose, then executes doFXPos semantics:
    // the admitted effect is detached and AttachToObject must not retain it.
    BonePosition,
};

enum class FxInvocationControlKind : uint8_t {
    Execute,
    StopAttachedParticleGroup,
    StopAllAttachedParticles,
};

struct FxInvocationAnchor final {
    engine::ObjectId object = engine::INVALID_OBJECT_ID;
    // Optional render-channel identity. Zero preserves the ordinary object
    // key; weapon FX uses this to address the Draw module that owns the bone
    // without changing gameplay ownership or visibility checks.
    uint64_t presentationObjectKey = 0;
    math::vec3 position{};
    float rollRadians = 0.0f;
    float pitchRadians = 0.0f;
    float yawRadians = 0.0f;
    // Detached GeometryInfo::getBoundingCircleRadius(). LightPulse consumes
    // this only for doFXObj/ObjectAttachment; positional/bone invocations use
    // the authored fixed Radius exactly.
    float objectBoundingCircleRadius = 0.0f;
};

// A few legacy Update modules create a ParticleSystem directly rather than
// naming an FXList (EMP sparks and leaflet drops). Preserve that distinction
// as a typed presentation payload instead of inventing synthetic FXList data.
struct FxDirectParticleRequest final {
    container::String particleSystemName;
    // BeaconClientUpdate's retail failsafe asks for BeaconSmokeRRGGBB first,
    // then clones BeaconSmokeFFFFFF for this one system and tints color keys
    // 1..N when the house-colour template is absent.  Carry that as immutable
    // instance data: the shared particle catalog must never be mutated.
    container::String fallbackParticleSystemName;
    std::optional<math::vec3> fallbackColorKeyTint;
    uint32_t emitterCount = 1;
    // Present means override the template's SystemLifetime. Zero is a real
    // zero-frame lifetime, not the ParticleSystem INI "forever" sentinel.
    std::optional<uint32_t> systemLifetimeFrames;
    float footprintMajorRadius = 0.0f;
    float footprintMinorRadius = 0.0f;
    float maximumHeight = 0.0f;
    uint32_t initialDelayMinimumFrames = 0;
    uint32_t initialDelayMaximumFrames = 0;
    bool boxFootprint = false;
    bool attachToObject = false;
};

enum class FxDirectBeamControl : uint8_t {
    Begin,
    Update,
    End,
};

// Weapon::createLaser and Mod-facing Ray templates enter the same ordered FX
// stream as authored FXList nuggets. A zero identity is a one-shot beam and
// uses the event identity. A nonzero identity is a stable Begin/Update/End
// instance used by continuous lasers such as ParticleUplink connectors.
struct FxDirectBeamRequest final {
    container::String objectTemplate;
    FxDirectBeamControl control = FxDirectBeamControl::Begin;
    uint64_t beamIdentity = 0;
    // Mirrors LaserRadiusUpdate::initRadius(): positive grows 0->1, negative
    // decays 1->0. Update callers can start a later decay explicitly.
    int32_t sizeDeltaFrames = 0;
    uint32_t decayFrames = 0;
};

enum class FxDirectScorchType : uint8_t {
    Random,
    Scorch1,
    Scorch2,
    Scorch3,
    Scorch4,
};

// Direct terrain mark used by ParticleUplink.  Random is presentation-only:
// FxRuntime derives the concrete stock variant from the immutable event ID
// and never consumes SimulationRandom.
struct FxDirectScorchRequest final {
    FxDirectScorchType type = FxDirectScorchType::Random;
    float radius = 0.0f;
};

enum class FxDirectRopeControl : uint8_t {
    Begin,
    Update,
    End,
};

// W3DRopeDraw is controlled by ChinookAI rather than authored Draw fields.
// This lossless value request mirrors initRopeParms/setRopeCurLen/
// setRopeSpeed without retaining the legacy drawable.
struct FxDirectRopeRequest final {
    FxDirectRopeControl control = FxDirectRopeControl::Begin;
    uint64_t ropeIdentity = 0;
    float maximumLength = 1.0f;
    float currentLength = 0.0f;
    float width = 0.5f;
    math::vec3 color{};
    float wobbleLength = 1.0f;
    float wobbleAmplitude = 0.0f;
    float wobbleRatePerFrame = 0.0f;
    float wobblePhase = 0.0f;
    float verticalOffset = 0.0f;
    float currentSpeedPerFrame = 0.0f;
    float maximumSpeedPerFrame = 0.0f;
    float accelerationPerFrame = 0.0f;
};

// Confirmed, presentation-only request to execute one authored FXList. The
// session stamps identity and a deterministic client seed without consuming
// SimulationRandom. A future renderer/FX service resolves the immutable
// FxListCatalog and owns all particle, light, scorch and sound handles.
struct FxInvocationEvent final {
    container::String fxListName;
    std::optional<FxDirectParticleRequest> directParticle;
    std::optional<FxDirectBeamRequest> directBeam;
    std::optional<FxDirectScorchRequest> directScorch;
    std::optional<FxDirectRopeRequest> directRope;
    FxInvocationControlKind control = FxInvocationControlKind::Execute;
    FxInvocationAnchorKind anchorKind = FxInvocationAnchorKind::WorldPosition;
    FxInvocationAnchor primary;
    std::optional<FxInvocationAnchor> secondary;
    // Frozen at the confirmed event edge before lifecycle retirement.  The
    // FX/audio presentation path later converts it to an observer-relative
    // PlayerAudience without consulting ECS or PlayerRegistry in renderer.
    std::optional<engine::PlayerId> sourcePlayer;
    container::String boneName;
    // RandomBone:Yes treats boneName as an ASCII-insensitive prefix. The
    // renderer selects one matching prepared pose from variationSeed and
    // records the concrete bone on the attached emitter.
    bool boneNameIsPrefix = false;
    // W3D weapon barrel prefixes use Name01..Name99. A nonzero ordinal picks
    // the same stable barrel sequence as the confirmed weapon shot instead
    // of applying RandomBone selection. Authored single-bone weapons request
    // the original unadorned-name fallback explicitly.
    uint32_t boneNameSequenceOrdinal = 0;
    bool boneNamePrefixFallsBackToBare = false;
    // Direct beams may attach their second endpoint to a different bone on
    // the same or another stable object (ParticleUplink outer node ->
    // connector). It follows the same numbered-prefix contract as primary.
    container::String secondaryBoneName;
    bool secondaryBoneNameIsPrefix = false;
    uint32_t secondaryBoneNameSequenceOrdinal = 0;
    bool secondaryBoneNamePrefixFallsBackToBare = false;
    math::vec3 secondaryWorldOffset{};
    // False preserves legacy doFXPos(position, nullptr) semantics after a
    // renderer-side bone lookup: use the resolved position, but not the
    // object's or bone's roll/pitch/yaw as the invocation basis.
    bool inheritResolvedAnchorOrientation = true;
    math::vec3 attachmentLocalOffset{};
    // Nonzero only for presentation-owned emitters which must be stopped as
    // one legacy TransitionDamageFX state group.
    uint64_t attachmentGroup = 0;
    float primarySpeed = 0.0f;
    float overrideRadius = 0.0f;
    // A bounded, client-presentation retry budget for one-shot effects whose
    // authored Draw path would not advance while locally invisible. Gameplay
    // never branches on this field and eventId/variationSeed stay stable.
    uint32_t localVisibilityRetryFrames = 0;
    // First confirmed frame of a visibility-retried effect. The event stream
    // preserves it across re-emission so a finite attached system starts with
    // its correct remaining lifetime instead of restarting when revealed.
    uint64_t localVisibilityFirstFrame = 0;

    // Filled by FxInvocationEventStream unless explicitly supplied by a
    // focused replay/import adapter. Values never enter simulation checksum.
    // streamSequence is always assigned by the active session stream and is
    // strictly increasing within one presentation epoch.  It is the
    // game-side exactly-once consumption cursor; eventId remains the stable
    // presentation de-duplication identity passed to FxRuntime.
    uint64_t streamSequence = 0;
    uint64_t eventId = 0;
    uint64_t confirmedFrame = 0;
    uint64_t variationSeed = 0;
};

} // namespace game
