#pragma once

#include "core/container/container_types.h"

#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/terrain/MapHeightfieldLoader.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace engine {

// Game-owned visual intent. This deliberately contains no W3D resource or
// renderer handle: GameRenderExtraction translates it to a frame-local,
// backend-neutral WorldRenderSnapshot.
enum class VisualAnimationStartKind : uint8_t {
    Default,
    FirstFrame,
    LastFrame,
    RandomFrame,
    MaintainFraction,
};

struct RenderModelChannelState final {
    uint32_t channelIndex = UINT32_MAX;
    ::container::String animationState;
    float animationTimeSeconds = 0.0f;
    float animationRate = 1.0f;
    game::ModelAnimationMode animationMode = game::ModelAnimationMode::Once;
    // Manual W3D playback is authored in frames, not seconds. The renderer
    // converts this frame to the selected clip's sample time only after the
    // immutable clip metadata is available.
    uint32_t animationManualFrame = 0;
    bool animationPaused = false;
    // Derived once per confirmed tick from Drawable::getShouldAnimate parity.
    // Keep it separate from explicit producer pause so power recovery cannot
    // accidentally clear a gameplay-requested pause (or remain sticky).
    bool animationPausedByObjectState = false;
    game::ModelConditionMask animationConditionSnapshot;
    // Producer-owned revision for an authored, timed ConditionState change.
    // It lets a timed state machine identify a new authored presentation
    // intent independently of the gameplay phase. Short states are retained
    // inside the Draw-channel clock until their matching generation reaches
    // a published renderer endpoint.
    uint64_t conditionAnimationRevisionSnapshot = 0;
    ::container::String animationStateSnapshot;
    uint64_t animationStateEnterTick = 0;
    bool animationStateInitialized = false;
    uint32_t resolvedVisualRuleIndex = UINT32_MAX;
    uint32_t activeTransitionRuleIndex = UINT32_MAX;
    uint32_t waitingSourceVisualRuleIndex = UINT32_MAX;
    game::ModelConditionMask waitingSourceConditionSnapshot;
    // Monotonic state-enter identity. Completion feedback must match this
    // generation and channel before it may clear a wait/transition phase.
    uint64_t animationStateGeneration = 0;
    // Latest generation known to have reached a renderer-published world
    // endpoint. A newer generation remains the presented state until this
    // acknowledgement arrives, preventing newest-only frame coalescing from
    // erasing a one-tick ConditionState before it was observable.
    uint64_t animationEndpointPublishedGeneration = 0;
    // A condition-state generation entered after initial channel bootstrap
    // must reach one published renderer endpoint before a later presentation
    // state may replace it. Gameplay/model-condition authority never waits.
    bool animationEndpointAdmissionRequired = false;
    // One bit per renderer-reported source/transition/active completion in
    // the current generation. It makes repeated render frames harmless while
    // still allowing a source and transition to complete together.
    uint8_t animationCompletionMask = 0;
    // Renderer residency may lag the confirmed condition-state generation.
    // Only the exact phase may freeze this logic-owned clock.
    uint64_t animationResourcePendingGeneration = 0;
    uint8_t animationResourcePendingPhase = UINT8_MAX;
    // Idle completion chooses a different authored candidate without making
    // the renderer own random state. The override is valid only for its exact
    // generation and is cleared by every ordinary state enter.
    uint32_t animationCandidateOverrideIndex = UINT32_MAX;
    uint64_t animationCandidateOverrideGeneration = 0;
    game::ModelAnimationFlags activeAnimationFlags = 0;
    VisualAnimationStartKind animationStartKind =
        VisualAnimationStartKind::Default;
    float animationRandomStartFraction = 0.0f;
    uint32_t animationStartSourceVisualRuleIndex = UINT32_MAX;
    game::ModelConditionMask animationStartSourceConditionSnapshot;
    float animationStartSourceTimeSeconds = 0.0f;
    uint64_t animationStartSourceGeneration = 0;
};

enum class ObjectOpacityFadeMode : uint8_t {
    None,
    In,
    Out,
};

enum class ObjectModelConditionDoorPhase : uint8_t {
    Unspecified,
    Opening,
    WaitingOpen,
    WaitingToClose,
    Closing,
};

enum class ObjectModelConditionDoorSource : uint8_t {
    Production,
    MissileLauncher,
    Containment,
    Checkpoint,
    BattlePlan,
    SpecialPower,
    Airfield,
    Count,
};

struct ObjectModelConditionDoorContributionComponent final {
    static constexpr size_t kDoorSlotCount = 4;
    static constexpr size_t kSourceCount =
        static_cast<size_t>(ObjectModelConditionDoorSource::Count);

    ::container::Array<
        ::container::Array<ObjectModelConditionDoorPhase, kDoorSlotCount>,
        kSourceCount> phases{};
    ::container::Array<
        ::container::Array<uint64_t, kDoorSlotCount>,
        kSourceCount> confirmedTicks{};
    ::container::Array<
        ::container::Array<uint64_t, kDoorSlotCount>,
        kSourceCount> sequences{};
};

// Non-door gameplay modules in this shared family retain only their own
// ModelCondition contribution. ObjectModelConditionAuthority is the sole
// composer for these declared bits, so clearing one source can never erase an
// identically named condition still selected by another source (for example
// weapon FIRING_A versus a SpecialAbilityUpdate animation). Independent body,
// steering and one-shot presentation families keep their existing owners.
enum class ObjectModelConditionContributionSource : uint8_t {
    Upgrade,
    Fire,
    Tactical,
    Economy,
    Bridge,
    Containment,
    Airfield,
    // StealthUpdate owns MODELCONDITION_DISGUISED exclusively: RefCode sets it
    // on the same two lines that set/clear OBJECT_STATUS_DISGUISED
    // (StealthUpdate.cpp:1037-1038 and :1107-1108). Its set point and its three
    // clear points are spread across separate confirmed ticks, so it needs the
    // ordered, owned set/clear this composer provides rather than a bare
    // modelConditionFlags write.
    Stealth,
    Count,
};

struct ObjectModelConditionContributionComponent final {
    static constexpr size_t kSourceCount =
        static_cast<size_t>(ObjectModelConditionContributionSource::Count);

    ::container::Array<game::ModelConditionMask, kSourceCount> selected{};
    ::container::Array<game::ModelConditionMask, kSourceCount> owned{};
    ::container::Array<uint64_t, kSourceCount> confirmedTicks{};
    ::container::Array<uint64_t, kSourceCount> sequences{};
};

// Per-instance WorldBuilder overrides applied after onCreate. Negative means
// inherit the session environment; zero/one force the corresponding model
// condition off/on even when global follow-time/weather is enabled.
struct ObjectEnvironmentModelConditionOverrideComponent final {
    int8_t night = -1;
    int8_t snow = -1;
};

// Object Panel ambient selection is a durable object value, not a mutation
// of the shared AudioEvent catalog. An engaged empty event name explicitly
// disables the authored ambient sound for this instance.
struct ObjectAmbientAudioOverrideComponent final {
    std::optional<container::String> eventName;
    std::optional<bool> enabled;
    std::optional<bool> looping;
    std::optional<int32_t> loopCount;
    std::optional<float> minimumVolume;
    std::optional<float> volume;
    std::optional<float> minimumRange;
    std::optional<float> maximumRange;
    std::optional<uint8_t> priority;
};

// Drawable-local locomotor attitude. RefCode keeps this in DrawableLocoInfo:
// it affects only the rendered model and never feeds collision, targeting or
// authoritative movement. Values remain fixed here so confirmed presentation
// advancement cannot introduce a second floating-point simulation path; the
// render extractor is the sole fixed-to-float boundary.
struct ObjectLocomotorDrawPresentationState final {
    math::q32_32 pitch{};
    math::q32_32 pitchRate{};
    math::q32_32 roll{};
    math::q32_32 rollRate{};
    math::q32_32 yaw{};
    math::q32_32 accelerationPitch{};
    math::q32_32 accelerationPitchRate{};
    math::q32_32 accelerationRoll{};
    math::q32_32 accelerationRollRate{};
    math::q32_32 yawModulator{};
    math::q32_32 pitchModulator{};
    math::q32_32 previousVelocityXPerFrame{};
    math::q32_32 previousVelocityYPerFrame{};
    math::q32_32 previousVelocityZPerFrame{};
    // Drawable::calcPhysicsXformTreads owns this temporary crush-over height;
    // it is presentation-only and never changes authoritative XYZ/collision.
    math::q32_32 overlapHeight{};
    math::q32_32 overlapHeightVelocity{};
    math::q32_32 outputPitch{};
    math::q32_32 outputRoll{};
    math::q32_32 outputYaw{};
    math::q32_32 outputVerticalOffset{};
    uint8_t appearanceTag = UINT8_MAX;
    int8_t wobbleDirection = 1;
    bool velocityInitialized = false;
    bool initialized = false;

    bool operator==(
        const ObjectLocomotorDrawPresentationState&) const = default;
};

struct RenderModelComponent {
    ::container::String modelAsset;
    game::ModelConditionMask modelConditionFlags;
    ::container::String animationState;
    // This is an unscaled, logic-owned clock. GameSession advances it once
    // per confirmed fixed tick; the detached renderer snapshot applies
    // animationRate while sampling its W3D clip. Keeping those responsibilities
    // separate avoids a rate multiplier being integrated by the simulation and
    // then applied a second time by the renderer.
    float animationTimeSeconds = 0.0f;
    // Non-negative playback multiplier consumed by the snapshot sampler.
    // Direction is represented by animationMode rather than a negative rate.
    float animationRate = 1.0f;
    // Drawable::setAnimationLoopDuration expressed as a detached requested
    // completion time. Zero selects the authored clip rate. Gameplay owns
    // the duration; the renderer may inspect clip length only to derive the
    // presentation sampling multiplier.
    float animationLoopDurationSeconds = 0.0f;
    // Object::updateWeaponSetFlag independently applies the selected
    // Weapon's remaining pre-attack duration when entering PREATTACK_A/B/C.
    // Keep it separate from module-authored durations (for example
    // MissileLauncherBuilding door phases) so unrelated producers cannot
    // overwrite one another before extraction selects the active family.
    float weaponPreattackLoopDurationSeconds = 0.0f;
    game::ModelAnimationMode animationMode = game::ModelAnimationMode::Loop;
    uint32_t animationManualFrame = 0;
    // Manual is an explicitly authored time and pause freezes every other
    // mode. Neither condition is allowed to be advanced by renderer work.
    bool animationPaused = false;
    bool animationPausedByObjectState = false;
    // Deterministic visual-state clock bookkeeping. Simulation compares the
    // selected condition/explicit clip intent once per confirmed tick and
    // resets animationTimeSeconds exactly when that intent changes.
    game::ModelConditionMask animationConditionSnapshot;
    // Monotonic presentation intent published by timed model-condition state
    // machines. The value carries no gameplay state and is never advanced by
    // the renderer; channels use it to distinguish a new authored phase from
    // ordinary per-tick clock advancement.
    uint64_t conditionAnimationRevision = 0;
    uint64_t conditionAnimationRevisionSnapshot = 0;
    ::container::String animationStateSnapshot;
    uint64_t animationStateEnterTick = 0;
    bool animationStateInitialized = false;
    uint32_t resolvedVisualRuleIndex = UINT32_MAX;
    uint32_t activeTransitionRuleIndex = UINT32_MAX;
    // When a newly selected ConditionState asks to let the currently
    // presented Once state finish, keep that source rule and its original
    // condition seed alive. The renderer can then sample a sealed
    // source -> optional TransitionState -> target chain from the one
    // logic-owned clock without ever writing completion back into ECS.
    uint32_t waitingSourceVisualRuleIndex = UINT32_MAX;
    game::ModelConditionMask waitingSourceConditionSnapshot;
    uint64_t animationStateGeneration = 0;
    uint64_t animationEndpointPublishedGeneration = 0;
    bool animationEndpointAdmissionRequired = false;
    uint8_t animationCompletionMask = 0;
    uint64_t animationResourcePendingGeneration = 0;
    uint8_t animationResourcePendingPhase = UINT8_MAX;
    uint32_t animationCandidateOverrideIndex = UINT32_MAX;
    uint64_t animationCandidateOverrideGeneration = 0;
    game::ModelAnimationFlags activeAnimationFlags = 0;
    VisualAnimationStartKind animationStartKind =
        VisualAnimationStartKind::Default;
    float animationRandomStartFraction = 0.0f;
    uint32_t animationStartSourceVisualRuleIndex = UINT32_MAX;
    game::ModelConditionMask animationStartSourceConditionSnapshot;
    float animationStartSourceTimeSeconds = 0.0f;
    uint64_t animationStartSourceGeneration = 0;
    // Confirmed weapon impulses are retained as compact scalar presentation
    // facts. Extraction derives recoil/muzzle state from tick age; no
    // renderer frame callback is allowed to mutate or extend the impulse.
    ::container::Array<uint64_t, game::kWeaponSlotCount> lastWeaponFireTicks{};
    // One-based explicit authored barrel index from the confirmed fire event,
    // not the global shot id. Recoil/muzzle/FireFX/launch must share it.
    ::container::Array<uint32_t, game::kWeaponSlotCount> lastWeaponFireSequences{};
    // Drawable-local chassis impulse produced by Weapon.WeaponRecoil. This
    // remains presentation state: it never alters authoritative movement,
    // collision, aiming or projectile orientation.
    math::q32_32 weaponRecoilPitch{};
    math::q32_32 weaponRecoilPitchRate{};
    math::q32_32 weaponRecoilRoll{};
    math::q32_32 weaponRecoilRollRate{};
    ObjectLocomotorDrawPresentationState locomotorDraw;
    bool turretMoveAudioActive = false;
    bool underConstructionAudioActive = false;
    bool stealthAudioInitialized = false;
    bool stealthedAudioSnapshot = false;
    bool detectedAudioSnapshot = false;
    uint64_t lastContainmentEnterAudioTick =
        std::numeric_limits<uint64_t>::max();
    uint64_t lastContainmentExitAudioTick =
        std::numeric_limits<uint64_t>::max();
    // Drawable::m_explicitOpacity is object-wide and multiplies the
    // observer-relative stealth pulse.  Script/placement producers may set
    // this without mutating per-Draw material assets.
    float explicitOpacity = 1.0f;
    ObjectOpacityFadeMode opacityFadeMode = ObjectOpacityFadeMode::None;
    uint64_t opacityFadeStartTick = 0;
    uint32_t opacityFadeDurationFrames = 0;
    bool hidden = false;
    // Derived once from the immutable object's resolved SwayClientUpdate
    // recipe and the captured client option.  This is an explicit opt-in:
    // SET_TREE_SWAY must never animate arbitrary units or buildings merely
    // because they happen to use a W3D mesh.
    bool treeSwayEnabled = false;
    float boundingRadius = 0.0f;
    // One stable logic-owned clock per final Draw module. The legacy scalar
    // fields above remain temporarily as the channel-0 compatibility view
    // while gameplay producers migrate to channel-local indices.
    ::container::Vector<RenderModelChannelState> channels;
};

// Confirmed-tick state for the three legacy vehicle Draw subclasses. Every
// value needed by bones, tread mappers and particle admission is published by
// gameplay once; the renderer owns only GPU/emitter handles and interpolation.
struct VehicleDrawChannelPresentationState final {
    uint32_t channelIndex = UINT32_MAX;
    float frontWheelRotation = 0.0f;
    float rearWheelRotation = 0.0f;
    float wheelSteeringAngle = 0.0f;
    float frontLeftWheelHeight = 0.0f;
    float frontRightWheelHeight = 0.0f;
    float rearLeftWheelHeight = 0.0f;
    float rearRightWheelHeight = 0.0f;
    float cabRotation = 0.0f;
    float trailerRotation = 0.0f;
    float treadLeftOffset = 0.0f;
    float treadRightOffset = 0.0f;
    float treadMiddleOffset = 0.0f;
    float dustSizeMultiplier = 1.0f;
    float debrisVelocityXyMultiplier = 1.0f;
    float debrisVelocityZMultiplier = 1.0f;
    float debrisBurstMultiplier = 1.0f;
    float previousYaw = 0.0f;
    float previousSpeed = 0.0f;
    uint32_t airborneFrames = 0;
    uint64_t landingTriggerSequence = 0;
    bool initialized = false;
    bool moving = false;
    bool turningLeft = false;
    bool turningRight = false;
    bool accelerating = false;
    bool grounded = true;
    bool powersliding = false;
    bool dustActive = false;
    bool dirtActive = false;
    bool powerslideActive = false;
    bool treadDebrisActive = false;

    bool operator==(
        const VehicleDrawChannelPresentationState&) const = default;
};

struct VehicleDrawPresentationComponent final {
    ::container::Vector<VehicleDrawChannelPresentationState> channels;
    uint64_t confirmedTick = 0;
    bool powerslideAudioActive = false;
};

enum class DebrisDrawPresentationPhase : uint8_t {
    Initial,
    Flying,
    Final,
};

// Per-instance values supplied by CreateDebris to the otherwise model-less
// GenericDebris template. W3D resources and clip metadata stay renderer-owned.
struct DebrisDrawPresentationComponent final {
    ::container::String initialAnimation;
    ::container::String flyingAnimation;
    ::container::String finalAnimation;
    ::container::String finalFx;
    ::container::String bounceSound;
    uint64_t spawnedTick = 0;
    uint64_t finalStateTick = 0;
    uint8_t shadowTypeMask = 0;
    uint8_t minimumLod = 0;
    DebrisDrawPresentationPhase phase = DebrisDrawPresentationPhase::Initial;
    bool finalStop = false;
    bool finalFxEmitted = false;
    bool okToChangeModelColor = false;
};

// Immutable source metadata for an object instantiated from a CkMp map.
// It deliberately stores only copied game/map values: neither a parser, live
// terrain object, renderer resource nor ECS pointer survives here.  Gameplay
// systems can later interpret individual map properties without requiring the
// renderer to know the CkMp format.
struct MapObjectProvenanceComponent {
    uint64_t sourceRecordIndex = 0;
    ::container::String sourceName;
    int32_t mapFlags = 0;
    game::terrain::MapPropertyDict properties;
};

} // namespace engine
