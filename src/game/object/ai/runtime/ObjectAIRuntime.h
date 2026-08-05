#pragma once

#include "game/object/ai/runtime/AIProductionStateRoute.h"
#include "game/object/ai/runtime/AIRecipeOwnerRoute.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <typeinfo>
#include <limits>
#include <optional>

#include "core/container/container_types.h"
#include "debug/debug.h"
#include "game/object/ai/runtime/ObjectAIShadowBatch.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/ai/runtime/AIStateSoASlotRegistry.h"
#include "game/object/ai/runtime/ObjectAITransientStore.h"

namespace engine::ai
{

struct ObjectAIRuntimeConfig final
{
    size_t maximumActors = 0;
    size_t slotsPerBatch = 256;
    size_t membershipEventCapacity = 0;
    size_t transientValueCapacity = 0;
    // Session-rate projection of AIData.ini. These values are part of the AI
    // snapshot/digest because changing them changes confirmed state timing.
    uint32_t guardEnemyScanIntervalTicks = 15;
    uint32_t guardReturnScanIntervalTicks = 30;
    uint32_t guardChaseDurationTicks = 300;
    uint32_t idleTargetScanIntervalTicks = 3;
    uint32_t forceIdleBeforeAcquireTicks = 1;
    // AIData.SkirmishGroupFudgeDistance in raw Q32.32 world units. A team
    // may advance an authored waypoint when its centre is within this value
    // multiplied by the number of members, matching AIFollowWaypointPathState.
    int64_t skirmishGroupFudgeDistanceRaw = int64_t{5} << 32;
};

enum class AIObjectMembershipOperation : uint8_t
{
    Add,
    Remove,
};

struct AIObjectMembershipEvent final
{
    ObjectId subject = INVALID_OBJECT_ID;
    uint64_t confirmedTick = 0;
    uint32_t sequence = 0;
    AIObjectMembershipOperation operation = AIObjectMembershipOperation::Add;
    // Add-only, value-owned admission policy. Installing this in the same
    // transaction prevents a newly registered actor from spending one frame
    // unable to consume its production orders.
    ObjectAIOrderCapability initialCapabilities =
        ObjectAIOrderCapability::None;
};

enum class AIObjectMembershipStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidEvent,
    JournalCapacityExceeded,
    ConflictingSequence,
    ActorCapacityExceeded,
    StorageRejected,
};

struct AIObjectMembershipCommitReport final
{
    AIObjectMembershipStatus status = AIObjectMembershipStatus::Success;
    size_t eventsRead = 0;
    size_t effectiveEvents = 0;
    size_t eventsCoalesced = 0;
    size_t subjectsAdded = 0;
    size_t subjectsRemoved = 0;
    size_t unchanged = 0;

    [[nodiscard]] bool succeeded() const noexcept;
};

enum class ObjectAIRecipeBindingState : uint8_t
{
    Unbound,
    Bound,
    ContentUnavailable,
};

struct ObjectAIRecipeBindingSnapshot final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIRecipeId recipe = AIRecipeId::Invalid;
    ObjectAIRecipeBindingState state = ObjectAIRecipeBindingState::Unbound;

    constexpr bool operator==(
        const ObjectAIRecipeBindingSnapshot&) const noexcept = default;
};

struct ObjectAIRecipeActorView final
{
    AIRecipeId recipe = AIRecipeId::Invalid;
    AIRecipeOwner owner = AIRecipeOwner::Unimplemented;
    AIRecipeOwnerClass ownerClass = AIRecipeOwnerClass::Unsupported;
    ObjectAIOrderCapability capabilities = ObjectAIOrderCapability::None;
    ObjectAIRecipeBindingState state = ObjectAIRecipeBindingState::Unbound;
};

enum class ObjectAIRecipeBindingStatus : uint8_t
{
    Success,
    ContentUnavailable,
    NotInitialized,
    InvalidSubject,
    InvalidRecipe,
    RecipeConflict,
    CapabilityConflict,
    StorageRejected,
};

struct ObjectAIRecipeBindingResult final
{
    ObjectAIRecipeBindingStatus status =
        ObjectAIRecipeBindingStatus::NotInitialized;
    bool changed = false;

    [[nodiscard]] constexpr bool succeeded() const noexcept
    {
        return status == ObjectAIRecipeBindingStatus::Success ||
               status == ObjectAIRecipeBindingStatus::ContentUnavailable;
    }
};

[[nodiscard]] constexpr ObjectAIOrderCapability
objectAIRecipeMaximumCapabilities(AIRecipeId recipe) noexcept
{
    ObjectAIOrderCapability result = ObjectAIOrderCapability::None;
    if (aiRecipeUsesGenericMoveStop(recipe))
        result |= ObjectAIOrderCapability::MoveStop;
    if (aiRecipeUsesGenericAttack(recipe))
        result |= ObjectAIOrderCapability::Attack;
    return result;
}

[[nodiscard]] constexpr bool isObjectAIRecipeCapabilitySubset(
    AIRecipeId recipe, ObjectAIOrderCapability capabilities) noexcept
{
    if (!isValidObjectAIOrderCapabilityMask(capabilities))
        return false;
    const uint8_t requested = static_cast<uint8_t>(capabilities);
    const uint8_t maximum = static_cast<uint8_t>(
        objectAIRecipeMaximumCapabilities(recipe));
    return (requested & static_cast<uint8_t>(~maximum)) == 0;
}

[[nodiscard]] constexpr bool isObjectAIRecipeInitialCapabilityMask(
    ObjectAIOrderCapability capabilities) noexcept
{
    return capabilities == ObjectAIOrderCapability::None ||
           capabilities == objectAIRecipeMaximumCapabilities(
                               AIRecipeId::AIUpdateInterface);
}

enum class ObjectAICapability : uint64_t
{
    GroundMovement = uint64_t{1} << 0,
    Projectile = uint64_t{1} << 1,
    CanTurnInPlace = uint64_t{1} << 2,
    ConstructionComplete = uint64_t{1} << 3,
    HasAmmo = uint64_t{1} << 4,
    AttackMoodAllowed = uint64_t{1} << 5,
};

[[nodiscard]] constexpr uint64_t objectAICapabilityBit(
    ObjectAICapability capability) noexcept
{
    return static_cast<uint64_t>(capability);
}

// Stable, detached facts sampled from ECS in ObjectId order before Combat.
// No field retains an entity, registry address, component pointer, or mutable
// system service.
struct ObjectAIReadOnlyFact final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIFixedPosition position;
    ObjectId containedBy = INVALID_OBJECT_ID;
    // Nearest completed tunnel entrance sampled at the ECS→AI boundary.
    // GuardTunnelNetwork consumes this value without retaining ECS handles.
    ObjectId nearestTunnel = INVALID_OBJECT_ID;
    // Player tunnel-network nemesis sampled through ObjectContainment's
    // value-only handoff. The entrance is a movement target; the nemesis is
    // an attack priority shared by every entrance owned by that player.
    ObjectId priorityNemesis = INVALID_OBJECT_ID;
    // Most recent hostile damage source, projected from the victim's Body
    // last-damage record at the ECS -> AI boundary. This is RefCode's
    // BodyModule::getClearableLastAttacker(): the Guard family retaliates
    // against it without an explicit GuardRetaliate order. Retail consumes
    // ("clears") the value on the first guard-machine check after the hit, so
    // the projection is deliberately bounded to recent damage instead of
    // retaining a clearable flag the value-only AI boundary could not clear.
    ObjectId lastAggressor = INVALID_OBJECT_ID;
    // Nearest deterministic ordinary crate target for Guard-family states.
    // Explicit-contact Collide modules are never projected here.
    ObjectId pickupCrate = INVALID_OBJECT_ID;
    AIFixedPosition pickupCratePosition{};
    // AIUpdateModuleData auto-acquire policy and the deterministic target
    // selected at the ECS -> AI snapshot boundary.
    ObjectId idleAutoAcquireTarget = INVALID_OBJECT_ID;
    // Nearest repulsor inside this actor's vision range. RefCode's
    // AI::findClosestRepulsor() (AI.cpp:789) with PartitionFilterRepulsor
    // (PartitionManager.cpp:5094), evaluated only for KINDOF_CAN_BE_REPULSED
    // actors exactly like AIStates.cpp:1403/4594/4719/4833. The emitter half
    // already exists in this tree: a damaged repulsable civilian publishes
    // ObjectStatusFlag::Repulsor for a bounded time, so this field completes
    // the reactor half that Wander/Panic/WanderInPlace consume before failing
    // over into MoveAwayFromRepulsors.
    ObjectId closestRepulsor = INVALID_OBJECT_ID;
    uint64_t orderRevision = 0;
    uint64_t weaponRevision = 0;
    uint64_t targetScanWakeRevision = 0;
    uint64_t capabilityMask = 0;
    // Locomotor wander authoring as raw Q32.32 world units. WanderInPlace
    // draws each segment goal inside wanderAboutPointRadius
    // (AIStates.cpp:4694/4737) and Wander/Panic offset every waypoint goal by
    // wanderWidthFactor pathfind cells (AIStates.cpp:4571/4810). Both are
    // authored per Locomotor and therefore belong to the stable fact, while
    // the cell conversion and the offset draw stay with the runtime that owns
    // the projected pathfind cell size.
    int64_t wanderAboutPointRadiusRaw = 0;
    int64_t wanderWidthFactorRaw = 0;
    uint32_t disabledMask = 0;
    uint32_t idleTargetScanIntervalTicks = 1;
    uint8_t positionValid = 0;
    uint8_t effectivelyDead = 0;
    uint8_t mobile = 0;
    // A current locomotor can be present while the object is airborne,
    // contained, disabled, or otherwise not eligible for an ordinary Move
    // order. Keep that lifecycle fact distinct from both mobility and the
    // ground-vs-air pathing mode.
    uint8_t hasCurrentLocomotor = 0;
    uint8_t groundMovement = 0;
    uint8_t projectile = 0;
    uint8_t jetAI = 0;
    uint8_t pickupCratePositionValid = 0;
    uint8_t attackExitConditionSatisfied = 0;
    uint8_t idleAutoAcquireEnabled = 0;
    // KINDOF_CAN_BE_REPULSED, the authored gate every repulsor branch tests
    // before it looks for a repulsor at all.
    uint8_t canBeRepulsed = 0;

    constexpr bool operator==(const ObjectAIReadOnlyFact&) const noexcept = default;
};

struct ObjectAIReadOnlyInputSnapshot final
{
    uint64_t confirmedTick = 0;
    uint32_t ticksPerSecond = 0;
    container::Vector<ObjectAIReadOnlyFact> facts;
    bool valid = false;
};

// Shared progress for one ordinary as-team waypoint order. Members retain
// individual SoA payloads and Path/Movement correlations; this value-only
// record advances the common waypoint when the first admitted member reaches
// it.  RefCode's AIFollowWaypointPathState updates Team::currentWaypoint at
// that arrival boundary; lagging members observe the next shared waypoint on
// their next AI update. It never owns movement or ECS Team membership.
struct ObjectAIWaypointTeamProgressState final
{
    AITeamHandle team;
    AIWaypointHandle start;
    AIWaypointHandle current;
    uint64_t graphRevision = 0;
    uint64_t revision = 0;
    uint64_t issuedTick = 0;
    uint32_t sourceSequence = 0;
    uint32_t sourceScriptId = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return team && start && graphRevision != 0 && revision != 0;
    }

    constexpr bool operator==(
        const ObjectAIWaypointTeamProgressState&) const noexcept = default;
};

enum class ObjectAIShadowTickStatus : uint8_t
{
    Success,
    NotInitialized,
    NonMonotonicTick,
    FactCountMismatch,
    FactOrderMismatch,
    RecipeBindingMismatch,
    BatchRejected,
    WakeProjectionRejected,
};

struct ObjectAIShadowTickReport final
{
    ObjectAIShadowTickStatus status = ObjectAIShadowTickStatus::Success;
    uint64_t confirmedTick = 0;
    size_t factsRead = 0;
    size_t factsChanged = 0;
    size_t feedbackValuesRead = 0;
    size_t feedbackOverflows = 0;
    size_t feedbackRejected = 0;
    size_t facingCommandsStaged = 0;
    size_t facingCommandsRejected = 0;
    size_t pathRequestsStaged = 0;
    size_t pathRequestsRejected = 0;
    size_t movementCommandsStaged = 0;
    size_t movementCommandsRejected = 0;
    size_t waypointCompletionsStaged = 0;
    size_t waypointCompletionsRejected = 0;
    size_t attackCommandsStaged = 0;
    size_t attackCommandsRejected = 0;
    size_t attackCompletionsStaged = 0;
    size_t attackCompletionsRejected = 0;
    size_t opportunityQueryCommandsStaged = 0;
    size_t opportunityQueryCommandsRejected = 0;
    size_t opportunityChildCommandsStaged = 0;
    size_t opportunityChildCommandsRejected = 0;
    size_t tacticalQueryCommandsStaged = 0;
    size_t tacticalQueryCommandsRejected = 0;
    size_t tacticalChildCommandsStaged = 0;
    size_t tacticalChildCommandsRejected = 0;
    size_t guardTacticalCommandsStaged = 0;
    size_t guardTacticalCommandsRejected = 0;
    size_t guardInteractionCommandsStaged = 0;
    size_t guardInteractionCommandsRejected = 0;
    size_t dockRequestsStaged = 0;
    size_t dockRequestsRejected = 0;
    size_t containmentCommandsStaged = 0;
    size_t containmentCommandsRejected = 0;
    size_t insertionCommandsStaged = 0;
    size_t insertionCommandsRejected = 0;
    size_t batchesRun = 0;
    size_t actorsScheduled = 0;
    size_t waves = 0;
    size_t stepsProcessed = 0;
    size_t sleeping = 0;
    size_t unsupported = 0;
    size_t transitionsRequested = 0;
    size_t transitionsCommitted = 0;
    size_t transitionsRejected = 0;
    size_t transitionConflicts = 0;
    size_t transitionBudgetExceeded = 0;
    size_t discardedOutputValues = 0;
    size_t outputOverflows = 0;
    size_t outputStagingFailures = 0;
    size_t spanRejections = 0;
    size_t transitionCapacityRejections = 0;
    size_t blockedExits = 0;

    [[nodiscard]] bool succeeded() const noexcept;
};

struct ObjectAIRuntimeSnapshot final
{
    static constexpr uint32_t SchemaVersion = 22;

    uint32_t schemaVersion = SchemaVersion;
    ObjectAIRuntimeConfig config;
    container::Vector<AIStateSoASlotRegistrySnapshot> batches;
    container::Vector<ObjectAIOrderAdmissionSnapshot> orderAdmissions;
    container::Vector<ObjectAIRecipeBindingSnapshot> recipeBindings;
    container::Vector<AIObjectMembershipEvent> pendingMembershipEvents;
    AIObjectMembershipStatus membershipJournalStatus =
        AIObjectMembershipStatus::Success;
    uint64_t membershipJournalTick = 0;
    bool membershipJournalHasTick = false;
    container::Vector<ObjectAIWaypointTeamProgressState>
        waypointTeamProgress;
    // Special-command resolution runs after the shadow phase. These requests
    // and their first correlated feedback are therefore authoritative input
    // for the next confirmed tick, not disposable executor scratch.
    container::Vector<container::Vector<AIStateSoATransitionRequest>>
        pendingTransitionRequests;
    container::Vector<AIInsertionMotionFeedback>
        pendingInsertionEntryFeedback;
    container::Vector<AIContainmentFeedback>
        pendingContainmentEntryFeedback;
    ObjectAITransientSnapshot transients;
    ObjectAIReadOnlyInputSnapshot latestInput;
};

enum class ObjectAIRuntimeSnapshotStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidSchema,
    InvalidConfig,
    InvalidBatch,
    InvalidMembershipEvent,
    InvalidRecipeBinding,
    InvalidWaypointTeamProgress,
    InvalidPendingTransition,
    InvalidInputSnapshot,
    DuplicateSubject,
    CapacityExceeded,
    StorageRejected,
};

struct ObjectAIActorStateView final
{
    AIStateId state = AIStateId::Invalid;
    uint64_t revision = 0;
    uint64_t wakeTick = 0;
    bool idle = false;

    [[nodiscard]] bool sleepingAt(uint64_t confirmedTick) const noexcept
    {
        return wakeTick != 0 && confirmedTick < wakeTick;
    }
};

struct ObjectAIInsertionStateView final
{
    AIStateId state = AIStateId::Invalid;
    AIInsertionStatePayload payload;
    AIStateParameters parameters;
};

enum class ObjectAIInsertionTransitionStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidSubject,
    InvalidState,
    TransitionConflict,
    CapacityExceeded,
};

struct ObjectAIInsertionTransitionResult final
{
    ObjectAIInsertionTransitionStatus status =
        ObjectAIInsertionTransitionStatus::NotInitialized;
    AIInsertionCorrelation correlation;

    [[nodiscard]] bool succeeded() const noexcept;
};

enum class ObjectAIContainmentTransitionStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidSubject,
    InvalidState,
    InvalidGoal,
    OrderCancellationFailed,
    TransitionConflict,
    CapacityExceeded,
};

struct ObjectAIContainmentTransitionResult final
{
    ObjectAIContainmentTransitionStatus status =
        ObjectAIContainmentTransitionStatus::NotInitialized;
    AIContainmentCorrelation correlation;

    [[nodiscard]] bool succeeded() const noexcept;
};

enum class ObjectAIFacingTransitionStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidSubject,
    InvalidTarget,
    OrderCancellationFailed,
    TransitionConflict,
    CapacityExceeded,
};

struct ObjectAIFacingTransitionResult final
{
    ObjectAIFacingTransitionStatus status =
        ObjectAIFacingTransitionStatus::NotInitialized;
    AIStateRequestId request;

    [[nodiscard]] bool succeeded() const noexcept;
};

// An immutable-at-the-call-site, value-owned projection of the admission
// store for one Session phase.  It deliberately replaces borrowed owner
// lanes: gameplay transactions may change ObjectAI membership while Combat
// or movement is still consuming the phase input.
struct ObjectAIOrderCapabilitySnapshot final
{
    container::Vector<ObjectId> moveStopSubjects;
    container::Vector<ObjectId> attackSubjects;
    // Subjects whose current AttackObject state is internally owned by Idle
    // auto-acquisition rather than an admitted ECS order.
    container::Vector<ObjectId> autonomousAttackSubjects;

    void clear() noexcept
    {
        moveStopSubjects.clear();
        attackSubjects.clear();
        autonomousAttackSubjects.clear();
    }

    [[nodiscard]] bool has(
        ObjectId subject, ObjectAIOrderCapability capability) const noexcept
    {
        if (!subject) return false;
        if (capability == ObjectAIOrderCapability::MoveStop) {
            return std::binary_search(
                moveStopSubjects.begin(), moveStopSubjects.end(), subject);
        }
        if (capability == ObjectAIOrderCapability::Attack) {
            return std::binary_search(
                attackSubjects.begin(), attackSubjects.end(), subject);
        }
        return false;
    }
};

// Session-shaped object-AI owner. Runtime and shadow columns are allocated in
// bounded pages. Multiwave state transitions execute from detached facts;
// production-owned value protocols are gathered into the bounded session
// transient store before shadow buffers are cleared.
class ObjectAIRuntime final
{
public:
    [[nodiscard]] AIStateSoASlotStatus initialize(ObjectAIRuntimeConfig config);

    [[nodiscard]] AIObjectMembershipStatus queueMembership(
        const AIObjectMembershipEvent& event);

    // The journal is preflighted before membership is changed. Events for one
    // subject are reduced to their last sequence; removals commit before adds,
    // with ObjectId order inside each phase, so a full runtime never fails an
    // add merely because a later same-tick removal had not run yet.
    [[nodiscard]] AIObjectMembershipCommitReport commitMembership(
        uint64_t confirmedTick);

    void discardMembershipJournal() noexcept;

    [[nodiscard]] size_t pendingMembershipEventCount() const noexcept;

    void reset() noexcept;

    [[nodiscard]] AIStateSoASlotStatus addSubject(ObjectId subject,
                                                   AIActorHandle& output);

    [[nodiscard]] AIStateSoASlotStatus removeSubject(ObjectId subject) noexcept;

    [[nodiscard]] std::optional<AIActorHandle> find(ObjectId subject) const noexcept;

    // Value-only observation for orchestration layers.  Actor handles and
    // the SoA batch/slot layout remain private implementation details.
    [[nodiscard]] std::optional<ObjectAIActorStateView> actorState(
        ObjectId subject) const noexcept;

    // A lifecycle Add creates only an unbound slot.  GameSession resolves the
    // final inherited archetype recipe before the actor may run.  Missing or
    // ambiguous content is explicitly degraded to a capability-off no-op;
    // changing a previously bound recipe is a structural conflict.
    [[nodiscard]] ObjectAIRecipeBindingResult bindRecipe(
        ObjectId subject, AIRecipeId recipe) noexcept;

    [[nodiscard]] ObjectAIRecipeBindingResult markRecipeContentUnavailable(
        ObjectId subject) noexcept;

    [[nodiscard]] std::optional<ObjectAIRecipeActorView> recipeBinding(
        ObjectId subject) const noexcept;

    [[nodiscard]] container::Span<const ObjectAIRecipeBindingSnapshot>
    recipeBindings() const noexcept;

    [[nodiscard]] std::optional<ObjectAIInsertionStateView> insertionState(
        ObjectId subject) const noexcept;

    [[nodiscard]] ObjectAIInsertionTransitionResult stageInsertionState(
        ObjectId subject, AIStateId state,
        const AIStateParameters& parameters, uint64_t activationTick,
        std::optional<AIInsertionMotionFeedback> entryFeedback = std::nullopt);

    [[nodiscard]] ObjectAIInsertionTransitionStatus cancelInsertionState(
        ObjectId subject, uint64_t activationTick);

    // Specialized ingress for legacy AI_EXIT/AI_EXIT_INSTANTLY. The session
    // supplies the authoritative selected container and an entry fact; this
    // keeps player/script containment commands out of generic move admission
    // while retaining the normal wait/door/exit-delay state machine.
    [[nodiscard]] ObjectAIContainmentTransitionResult stageContainmentExitState(
        ObjectId subject, ObjectId container, AIStateId state,
        uint64_t activationTick, uint64_t externalOrderRevision,
        AIContainmentFeedback entryFeedback);

    // Specialized owner ingress shared by script-facing commands and update
    // modules such as SpecialAbilityUpdate. It stages the existing
    // FaceObject/FacePosition state instead of snapping authoritative yaw in
    // the caller, while atomically retiring any replaced queue admission.
    [[nodiscard]] ObjectAIFacingTransitionResult stageFacingState(
        ObjectId subject, ObjectId targetObject,
        const std::optional<AIFixedPosition>& targetPosition,
        uint64_t activationTick, uint64_t externalOrderRevision = 0);

    [[nodiscard]] ObjectId resolve(AIActorHandle handle) const noexcept;

    [[nodiscard]] AIStateFamilySoAStorage* storage(AIActorHandle handle) noexcept;

    [[nodiscard]] const AIStateFamilySoAStorage* storage(AIActorHandle handle) const noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] size_t maximumActors() const noexcept;
    [[nodiscard]] size_t slotsPerBatch() const noexcept;
    [[nodiscard]] size_t activeCount() const noexcept;
    [[nodiscard]] size_t allocatedBatchCount() const noexcept;

    [[nodiscard]] size_t activeBatchCount() const noexcept;

    [[nodiscard]] container::Span<const AIStateSoASubjectSlot> orderedSubjects() const noexcept;

    [[nodiscard]] container::Span<AIStateSoASlotRegistry> batches() noexcept;

    [[nodiscard]] container::Span<const AIStateSoASlotRegistry> batches() const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult setOrderCapabilities(
        ObjectId subject, ObjectAIOrderCapability capabilities) noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionStorage* orderAdmission(
        AIActorHandle handle) noexcept;

    [[nodiscard]] const ObjectAIOrderAdmissionStorage* orderAdmission(
        AIActorHandle handle) const noexcept;

    // The admission storage is the source of truth for command ownership.
    // Session-side routing must not infer it from the derived owner mirrors:
    // those lists are retained only for targeted runtime iteration.
    [[nodiscard]] std::optional<ObjectAIOrderCapability> orderCapabilities(
        ObjectId subject) const noexcept;
    [[nodiscard]] bool hasOrderCapability(
        ObjectId subject, ObjectAIOrderCapability capability) const noexcept;

    // Materializes the exact capability state from admission storage in
    // stable ObjectId order.  The caller owns the result for the whole
    // gameplay phase and may safely run transactions that mutate this
    // runtime after the capture returns.
    void captureOrderCapabilitySnapshot(
        ObjectAIOrderCapabilitySnapshot& output) const;

    // Navigation topology owns this derived scalar. Re-project it into every
    // fixed SoA page before execution; it is not a retained service pointer
    // and snapshot restore can deterministically rebuild it from Navigation.
    void setPathfindCellSizeRaw(int64_t value) noexcept;

    // Resolver callbacks are borrowed session services and are deliberately
    // excluded from snapshots. Retain each binding on the runtime so pages
    // created after the call receive it, and fan it out to existing pages.
    void setPathSequenceResolver(AIPathSequenceResolver resolver) noexcept;

    void setWaypointGraphResolver(AIWaypointGraphResolver resolver) noexcept;

    void setPathHandleReleaser(AIPathHandleReleaser releaser) noexcept;

    // Observes the queue-clear revision published synchronously by Stop or an
    // external replacement. Admission state and every correlated in-flight
    // value are cancelled together; gameplay queues/components remain owned
    // by their existing systems.
    [[nodiscard]] ObjectAIOrderAdmissionResult synchronizeOrderExternalRevision(
        ObjectId subject, uint64_t externalRevision) noexcept;

    [[nodiscard]] bool acceptsMoveToCompletion(
        const MovementFeedback& feedback) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult completeMoveOrder(
        ObjectId subject, const AIAsyncOrderIdentity& expectedIdentity,
        ObjectAIOrderCompletion completion) noexcept;

    [[nodiscard]] std::optional<ObjectAIOrderCompletion> moveOrderOutcome(
        ObjectId subject,
        const ObjectAIOrderIdentity& expectedIdentity) const noexcept;

    [[nodiscard]] std::optional<ObjectAIOrderCompletion>
        combatDropOrderOutcome(
            ObjectId subject,
            const AIAsyncOrderIdentity& expectedIdentity) const noexcept;

    [[nodiscard]] std::optional<ObjectAIOrderCompletion> attackOrderOutcome(
        ObjectId subject,
        const ObjectAIOrderIdentity& expectedIdentity) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult completeAttackOrder(
        ObjectId subject, const AIAsyncOrderIdentity& expectedIdentity,
        ObjectAIOrderCompletion completion) noexcept;

    [[nodiscard]] bool huntOrderTerminal(
        ObjectId subject,
        const ObjectAIOrderIdentity& expectedIdentity) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult completeHuntOrder(
        ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity,
        ObjectAIOrderCompletion completion) noexcept;

    [[nodiscard]] std::optional<ObjectAIOrderCompletion>
        tacticalAttackOrderOutcome(
            ObjectId subject,
            const ObjectAIOrderIdentity& expectedIdentity,
            ObjectAITacticalAttackSubtype subtype) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult completeTacticalAttackOrder(
        ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity,
        ObjectAITacticalAttackSubtype subtype,
        ObjectAIOrderCompletion completion) noexcept;

    // An ordinary waypoint route owns one queue identity across every
    // segment. The waypoint kernel records a nonzero terminal only on the
    // successful no-outgoing-link path; failures also settle in Idle but do
    // not manufacture that marker.
    [[nodiscard]] std::optional<ObjectAIOrderCompletion> waypointOrderOutcome(
        ObjectId subject,
        const ObjectAIOrderIdentity& expectedIdentity,
        ObjectAIMoveRouteSubtype subtype =
            ObjectAIMoveRouteSubtype::WaypointPathIndividuals) const noexcept;

    [[nodiscard]] std::optional<AIWaypointHandle> waypointOrderCompletedWaypoint(
        ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity,
        ObjectAIMoveRouteSubtype subtype =
            ObjectAIMoveRouteSubtype::WaypointPathIndividuals) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult completeWaypointOrder(
        ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity,
        ObjectAIOrderCompletion completion,
        ObjectAIMoveRouteSubtype subtype =
            ObjectAIMoveRouteSubtype::WaypointPathIndividuals) noexcept;

    [[nodiscard]] std::optional<ObjectAIOrderCompletion>
    followPathOrderOutcome(
        ObjectId subject,
        const ObjectAIOrderIdentity& expectedIdentity,
        ObjectAIMoveRouteSubtype subtype) const noexcept;

    [[nodiscard]] std::optional<uint32_t> followPathCurrentPointIndex(
        ObjectId subject,
        const ObjectAIOrderIdentity& expectedIdentity,
        ObjectAIMoveRouteSubtype subtype) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult
    completeFollowPathOrder(
        ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity,
        ObjectAIOrderCompletion completion,
        ObjectAIMoveRouteSubtype subtype) noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult observeFollowPathOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        const AIStateParameters& parameters,
        ObjectAIMoveRouteSubtype subtype);

    [[nodiscard]] ObjectAIOrderAdmissionResult observeWaypointOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        const AIStateParameters& parameters,
        ObjectAIMoveRouteSubtype subtype =
            ObjectAIMoveRouteSubtype::WaypointPathIndividuals,
        bool attackFollow = false);

    [[nodiscard]] ObjectAIOrderAdmissionResult observeMoveOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        const AIStateParameters& parameters,
        ObjectAIMoveRouteSubtype subtype,
        uint64_t /*confirmedTick*/);

    // CombatDrop is a typed command-button order whose first state is an
    // inherited MoveTo body.  Admit that movement under the same immutable
    // queue identity, then keep the specialized rope operation behind the
    // insertion command ports.
    [[nodiscard]] ObjectAIOrderAdmissionResult observeCombatDropOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        const AIStateParameters& parameters, uint64_t confirmedTick,
        AIInsertionMotionFeedback entryFeedback);

    // Player Repair dispatch owns selection of the RepairDock object after it
    // separates dozer structure repair from damaged mobile actors. This
    // specialized transition binds that stable goal to GetRepaired;
    // reservation/action side effects still cross GameSession's typed adapter.
    [[nodiscard]] bool activateRepairDock(
        ObjectId subject, ObjectId dock, uint64_t /*confirmedTick*/,
        uint64_t externalOrderRevision);

    // Internal Idle reactor matching AIIdleState ->
    // AIMoveAwayFromRepulsorsState. It creates a state-machine-owned,
    // non-zero correlation revision; no player/script order is forged and no
    // ECS queue is exposed to the AI runtime.
    [[nodiscard]] bool activateRepulsorEscape(
        ObjectId subject, uint64_t confirmedTick);

    [[nodiscard]] ObjectAIOrderAdmissionResult observeAttackOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        AIStateId attackState, const AIStateParameters& parameters);

    [[nodiscard]] ObjectAIOrderAdmissionResult observeAttackMoveOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        const AIStateParameters& parameters, uint64_t /*confirmedTick*/);

    [[nodiscard]] ObjectAIOrderAdmissionResult observeHuntOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        bool allArmyHunt, bool useTeamCommonTarget);

    [[nodiscard]] ObjectAIOrderAdmissionResult observeTacticalDomainOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        ObjectAITacticalAttackSubtype subtype,
        AITargetCollectionHandle collection,
        uint64_t collectionRevision, AIAttackAreaHandle area,
        uint64_t areaRevision, AISquadTargetSelection squadSelection);

    [[nodiscard]] ObjectAIOrderAdmissionResult observeAttackSquadOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        AITargetCollectionHandle collection, uint64_t collectionRevision,
        AISquadTargetSelection selection =
            AISquadTargetSelection::ClosestLiveMember);

    [[nodiscard]] ObjectAIOrderAdmissionResult observeAttackAreaOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        AIAttackAreaHandle area, uint64_t areaRevision);

    [[nodiscard]] ObjectAIOrderAdmissionResult observeGuardOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        const AIFixedPosition& anchor, int64_t guardRangeRaw,
        int64_t visionRangeRaw, uint32_t pathSurfaceMask,
        int64_t arrivalRadiusRaw, bool enterGuardTargets = false,
        bool tracksAnchor = false,
        bool guardWithoutPursuit = false, bool guardFlyingOnly = false,
        AIAttackAreaHandle area = {}, uint64_t areaRevision = 0);

    [[nodiscard]] ObjectAIOrderAdmissionResult observeGuardTunnelNetworkOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        const AIFixedPosition& anchor, int64_t guardRangeRaw,
        int64_t visionRangeRaw, uint32_t pathSurfaceMask,
        int64_t arrivalRadiusRaw);

private:
    [[nodiscard]] ObjectAIOrderAdmissionResult observeGuardOrderVariant(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        const AIFixedPosition& anchor, int64_t guardRangeRaw,
        int64_t visionRangeRaw, uint32_t pathSurfaceMask,
        int64_t arrivalRadiusRaw, bool enterGuardTargets, bool tracksAnchor,
        bool guardWithoutPursuit, bool guardFlyingOnly,
        AIAttackAreaHandle area, uint64_t areaRevision,
        bool tunnelNetwork);

public:
    [[nodiscard]] ObjectAIOrderAdmissionResult observeGuardRetaliateOrder(
        ObjectId subject, const ObjectAIOrderIdentity& identity,
        ObjectId aggressor, const AIFixedPosition& anchor,
        int64_t guardRangeRaw, int64_t visionRangeRaw,
        uint32_t pathSurfaceMask, int64_t arrivalRadiusRaw);

    [[nodiscard]] ObjectAIShadowTickReport runShadow(
        uint64_t confirmedTick,
        uint32_t ticksPerSecond,
        container::Span<const ObjectAIReadOnlyFact> facts);

    [[nodiscard]] const ObjectAIShadowTickReport& lastShadowReport() const noexcept;

    [[nodiscard]] const ObjectAIReadOnlyInputSnapshot& latestInput() const noexcept;

    [[nodiscard]] ObjectAIRuntimeSnapshotStatus captureSnapshot(
        ObjectAIRuntimeSnapshot& output) const;

    [[nodiscard]] ObjectAIRuntimeSnapshotStatus restoreSnapshot(
        const ObjectAIRuntimeSnapshot& snapshot);

    [[nodiscard]] ObjectAITransientStore& transients() noexcept;
    [[nodiscard]] const ObjectAITransientStore& transients() const noexcept;

    // Production orchestration must not erase subject values through the raw
    // store because Ready path feedback and pending InstallPath commands own
    // repository handles until this owner-aware boundary releases them.
    [[nodiscard]] ObjectAITransientClearReport clearSubjectTransients(
        ObjectId subject) noexcept;

private:
    [[nodiscard]] ObjectAIShadowBatchConfig shadowBatchConfig() const noexcept;

    using SubjectIterator = container::Vector<AIStateSoASubjectSlot>::iterator;
    using ConstSubjectIterator = container::Vector<AIStateSoASubjectSlot>::const_iterator;

    void captureActiveAttackCompletionCandidates(uint64_t confirmedTick);

    void stageTerminalAttackCompletions(ObjectAIShadowTickReport& report);

    [[nodiscard]] SubjectIterator lowerBound(ObjectId subject) noexcept;

    [[nodiscard]] ConstSubjectIterator lowerBound(ObjectId subject) const noexcept;

    [[nodiscard]] static uint8_t hasCapability(
        const ObjectAIReadOnlyFact& fact,
        ObjectAICapability capability) noexcept;

    [[nodiscard]] static uint32_t waypointBranchChoice(
        ObjectId subject, const AIStateParameters& parameters,
        AIWaypointHandle waypoint, uint32_t generation) noexcept;

    [[nodiscard]] static uint32_t scanJitterDraw(
        ObjectId subject, uint64_t sourceOrderRevision,
        uint64_t confirmedTick) noexcept;

    // WanderInPlace's inclusive cell radius. RefCode uses a fixed 3 cells when
    // the actor has no current locomotor and otherwise rounds
    // WanderAboutPointRadius / PATHFIND_CELL_SIZE to the nearest cell.
    [[nodiscard]] static int32_t wanderCellsFromRadius(
        int64_t radiusRaw, int64_t cellSizeRaw, bool hasLocomotor) noexcept;

    // Wander/Panic group-offset cell radius. The authored WanderWidthFactor is
    // already a cell count; it only applies while strictly positive and is
    // floored at one cell.
    [[nodiscard]] static int32_t wanderCellsFromWidthFactor(
        int64_t widthFactorRaw) noexcept;

    // Deterministic replacement for GameLogicRandomValue(-cells, cells). The
    // legacy states drew X then Y from the shared gameplay stream; `axis`
    // separates those draws without making the stream depend on how many
    // actors happened to begin a wander segment on this tick.
    [[nodiscard]] static int32_t wanderOffsetDraw(
        ObjectId subject, uint64_t sourceOrderRevision, uint64_t confirmedTick,
        uint32_t axis, int32_t cells) noexcept;

    static constexpr size_t NoWaypointTeamProgress =
        std::numeric_limits<size_t>::max();

    [[nodiscard]] size_t waypointTeamProgressIndex(
        AITeamHandle team) const noexcept;

    void ensureWaypointTeamProgress(
        const ObjectAIOrderIdentity& identity,
        const AIStateParameters& parameters);

    void advanceWaypointTeamProgress(
        container::Span<const ObjectAIReadOnlyFact> facts) noexcept;

    void wakeWaypointTeamMembers(AITeamHandle team) noexcept;

    static void merge(ObjectAIShadowTickReport& target,
                      const AIStateSoAMultiwaveReport& source) noexcept;

    template <typename Buffer>
    static void auditBuffers(container::Span<Buffer> buffers,
                             ObjectAIShadowTickReport& report) noexcept
    {
        for (const Buffer& buffer : buffers)
        {
            report.discardedOutputValues += buffer.count;
            report.outputOverflows += buffer.overflowed ? 1 : 0;
        }
    }

    void collectAndAuditOutputs(ObjectAIShadowBatch& shadow,
                                ObjectAIShadowTickReport& report) noexcept;

    template <typename Value>
    [[nodiscard]] bool stageOutput(
        const Value& value, ObjectAIShadowTickReport& report) noexcept
    {
        const ObjectAITransientStatus status = m_transients.stage(value);
        if (status == ObjectAITransientStatus::Success)
            return true;
        ++report.outputStagingFailures;
#if TD_DEBUG_ENABLED
        TD_LOG_DEBUG(
            "[ObjectAIRuntime] transient output discarded type={} status={}",
            typeid(Value).name(), static_cast<uint32_t>(status));
#endif
        return false;
    }

    void collectFacingCommands(
        container::Span<AIStateCommandBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void releaseStaleFacingCommands() noexcept;

    void collectOwnedPathRequests(
        container::Span<PathRequestBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectOwnedMovementCommands(
        container::Span<MovementCommandBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectWaypointCompletions(
        container::Span<AIWaypointCompletionBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectOwnedAttackCommands(
        container::Span<AIAttackCommandBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectOwnedOpportunityQueryCommands(
        container::Span<AIOpportunityAttackMoveQueryCommandBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectOwnedOpportunityChildCommands(
        container::Span<AIOpportunityAttackMoveChildCommandBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectOwnedTacticalQueryCommands(
        container::Span<AITacticalAttackQueryCommandBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectOwnedTacticalChildCommands(
        container::Span<AITacticalAttackChildCommandBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectOwnedGuardTacticalCommands(
        container::Span<AIGuardTacticalCommandBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectOwnedGuardInteractionCommands(
        container::Span<AIGuardInteractionCommandBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectDockRequests(
        container::Span<AIDockRequestBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    void collectContainmentCommands(
        container::Span<AIContainmentCommandBuffer> buffers,
        ObjectAIShadowTickReport& report) noexcept;

    template <typename Buffer>
    void collectInsertionCommands(
        container::Span<Buffer> buffers,
        ObjectAIShadowTickReport& report) noexcept
    {
        for (const Buffer& buffer : buffers)
        {
            report.outputOverflows += buffer.overflowed ? 1 : 0;
            for (size_t index = 0; index < buffer.count; ++index)
            {
                const auto& command = buffer.values[index];
                if (!find(command.correlation.subject) ||
                    !stageOutput(command, report))
                {
                    ++report.insertionCommandsRejected;
                    continue;
                }
                ++report.insertionCommandsStaged;
            }
        }
    }

    void scatterFeedback(ObjectAIShadowTickReport& report) noexcept;

    [[nodiscard]] bool acceptsDockCorrelation(
        AIActorHandle handle,
        const AIDockCorrelation& correlation) const noexcept;

    [[nodiscard]] bool acceptsContainmentCorrelation(
        AIActorHandle handle,
        const AIContainmentCorrelation& correlation) const noexcept;

    [[nodiscard]] bool acceptsInsertionCorrelation(
        AIActorHandle handle,
        const AIInsertionCorrelation& correlation) const noexcept;

    void wakeForServiceResult(AIActorHandle handle) noexcept;

    [[nodiscard]] ObjectAITransientClearReport clearTransientSubject(
        ObjectId subject) noexcept;

    // A Ready feedback owns its repository handle until this AI phase either
    // transfers the exact correlation/path pair to an InstallPath command or
    // rejects it as stale. discardFeedback() is the terminal inbox boundary,
    // so release every handle which did not cross that boundary. This also
    // covers an InstallPath command superseded by EndMovement in the same
    // multiwave phase without scanning or mutating the PathRepository.
    void releaseUnclaimedPathFeedback() noexcept;

    [[nodiscard]] bool acceptsAsyncOrder(
        AIActorHandle handle,
        const AIAsyncOrderIdentity& identity) const noexcept;

    [[nodiscard]] ObjectAIShadowTickReport remember(
        const ObjectAIShadowTickReport& report) noexcept;

    [[nodiscard]] bool validateInputSnapshot(
        const ObjectAIReadOnlyInputSnapshot& input) const noexcept;

    ObjectAIRuntimeConfig m_config;
    size_t m_maximumBatches = 0;
    container::Vector<AIStateSoASlotRegistry> m_batches;
    container::Vector<ObjectAIShadowBatch> m_shadowBatches;
    // Reused tick scratch. Workers write one disjoint report per runnable
    // batch; the logic owner merges reports in batch-index order after join.
    container::Vector<AIStateSoAMultiwaveReport> m_parallelBatchReports;
    container::Vector<size_t> m_runnableBatchIndices;
    container::Vector<ObjectAIOrderAdmissionStorage> m_orderAdmissions;
    container::Vector<AIStateSoASubjectSlot> m_subjects;
    container::Vector<ObjectAIRecipeBindingSnapshot> m_recipeBindings;
    container::Vector<AIObjectMembershipEvent> m_membershipEvents;
    container::Vector<AIObjectMembershipEvent> m_effectiveMembershipEvents;
    AIObjectMembershipStatus m_membershipJournalStatus =
        AIObjectMembershipStatus::Success;
    uint64_t m_membershipJournalTick = 0;
    bool m_membershipJournalHasTick = false;
    ObjectAIReadOnlyInputSnapshot m_latestInput;
    container::Vector<AIWakeEvent> m_wakeScratch;
    container::Vector<AIAttackOrderCompletion> m_attackCompletionCandidates;
    container::Vector<ObjectAIWaypointTeamProgressState>
        m_waypointTeamProgress;
    // One deterministic winner per team progress record. When several
    // members reach a node in the same confirmed tick, the smallest ObjectId
    // provides a stable equivalent of the original single-threaded first
    // state update without introducing a collective arrival barrier.
    container::Vector<ObjectId> m_waypointTeamArrivals;
    container::Vector<AIInsertionMotionFeedback>
        m_pendingInsertionEntryFeedback;
    container::Vector<AIContainmentFeedback>
        m_pendingContainmentEntryFeedback;
    ObjectAIShadowTickReport m_lastShadowReport;
    ObjectAITransientStore m_transients;
    ObjectAITransientStore m_stagedTransients;
    AIPathSequenceResolver m_pathSequenceResolver;
    AIWaypointGraphResolver m_waypointGraphResolver;
    AIPathHandleReleaser m_pathHandleReleaser;
    bool m_initialized = false;
};

} // namespace engine::ai
