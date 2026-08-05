#pragma once

#include <cstdint>
#include <variant>

#include "game/navigation/contracts/NavigationPathContracts.h"
#include "game/object/ai/contracts/AIAttackServices.h"
#include "game/object/ai/states/special/AIDockStateData.h"
#include "game/object/ai/states/special/AIInsertionStateData.h"
#include "game/object/ai/states/combat/AIOpportunityAttackMoveStateData.h"
#include "game/object/ai/states/combat/AIGuardStateData.h"
#include "game/object/ai/states/combat/AITacticalAttackStateData.h"
#include "game/object/ai/contracts/AIStateCommands.h"
#include "game/object/ai/runtime/AIStateTypes.h"

namespace engine::ai
{

// Parameters survive state transitions and are populated by order translation
// before the target state is entered. They remain separate from active payload
// so preparing a new task never destroys the old state's exit data early.
struct AIStateParameters final
{
    uint64_t waitEndTick = 0;
    ObjectId goalObject = INVALID_OBJECT_ID;
    AIFixedPosition goalPosition;
    ObjectId ignoredObstacle = INVALID_OBJECT_ID;
    uint64_t sourceOrderRevision = 0;
    AIPathSequenceHandle pathSequence;
    uint64_t pathSequenceRevision = 0;
    AIWaypointHandle waypoint;
    uint64_t waypointGraphRevision = 0;
    AITeamHandle waypointTeam;
    AIFixedPosition waypointGroupOffset;
    int64_t waypointGroupSpeedRaw = 0;
    uint64_t groupPathId = 0;
    uint32_t groupPathMemberOrdinal = 0;
    uint32_t groupPathMemberCount = 0;
    AIFixedPosition groupPathStart;
    AIFixedPosition groupPathOffset;
    PathHandle existingPath;
    uint32_t pathSurfaceMask = 0;
    int64_t arrivalRadiusRaw = 0;
    bool hasGoalPosition = false;
    bool adjustDestinations = true;
    // Queue-backed TacticalAttack policy. These remain inert for every
    // non-tactical state and are projected into Hunt's detached SoA facts.
    bool allArmyHunt = false;
    bool useTeamCommonTarget = false;
    AITargetCollectionHandle tacticalTargetCollection;
    uint64_t tacticalTargetCollectionRevision = 0;
    AIAttackAreaHandle tacticalAttackArea;
    uint64_t tacticalAttackAreaRevision = 0;
    AISquadTargetSelection tacticalSquadSelection =
        AISquadTargetSelection::ClosestLiveMember;
    // Detached Guard policy. Current-position normal Guard keeps every flag
    // false and freezes currentAnchor when the Guard state enters.
    int64_t guardRangeRaw = 0;
    int64_t guardVisionRangeRaw = 0;
    AIFixedPosition guardAnchor;
    bool hasGuardAnchor = false;
    bool enterGuardTargets = false;
    bool guardWithoutPursuit = false;
    bool guardFlyingOnly = false;
    bool guardTracksAnchor = false;
    // Explicit damage-authority subject for GuardRetaliate. Keeping this
    // separate prevents a normal GuardObject goal from being mistaken for a
    // last-damage aggressor while an external transition is still staged.
    ObjectId guardRetaliateAggressor = INVALID_OBJECT_ID;
};

struct AIIdleStatePayload final
{
    uint64_t nextTargetScanTick = 0;
    constexpr bool operator==(const AIIdleStatePayload&) const noexcept = default;
};

struct AIWaitStatePayload final
{
    uint64_t endTick = 0;
    constexpr bool operator==(const AIWaitStatePayload&) const noexcept = default;
};

struct AIBusyStatePayload final
{
    uint64_t enteredTick = 0;
    constexpr bool operator==(const AIBusyStatePayload&) const noexcept = default;
};

struct AIDeadStatePayload final
{
    uint64_t enteredTick = 0;
    constexpr bool operator==(const AIDeadStatePayload&) const noexcept = default;
};

struct AIFaceStatePayload final
{
    AIStateRequestId request;
    bool commandIssued = false;
    bool canTurnInPlace = false;

    constexpr AIFaceStatePayload() = default;
    explicit constexpr AIFaceStatePayload(AIStateRequestId value)
        : request(value)
    {
    }
    constexpr bool operator==(const AIFaceStatePayload&) const noexcept = default;
};

enum class AIMoveToPhase : uint8_t
{
    WaitingForPath,
    FollowingPath,
};

struct AIMoveToStatePayload final
{
    AIStateRequestId request;
    AIFixedPosition resolvedGoal;
    AIFixedPosition adjustedGoal;
    PathHandle path;
    uint64_t sourceOrderRevision = 0;
    uint32_t generation = 1;
    uint32_t adjustedLayer = 0;
    AIMoveToPhase phase = AIMoveToPhase::WaitingForPath;
    bool pathRequestIssued = false;
    bool adjustDestinations = true;

    constexpr AIMoveToStatePayload() = default;
    explicit constexpr AIMoveToStatePayload(AIStateRequestId value)
        : request(value)
    {
    }
    constexpr bool operator==(const AIMoveToStatePayload&) const noexcept = default;
};

struct AIFollowPathStatePayload final
{
    AIStateRequestId request;
    AIPathSequenceHandle sequence;
    PathHandle path;
    ObjectId ignoredObstacle = INVALID_OBJECT_ID;
    AIFixedPosition segmentGoal;
    uint64_t sequenceRevision = 0;
    uint64_t sourceOrderRevision = 0;
    int64_t extraDistanceRaw = 0;
    uint32_t index = 0;
    uint32_t generation = 1;
    uint8_t retriesRemaining = 10;
    AIMoveToPhase phase = AIMoveToPhase::WaitingForPath;
    bool pathRequestIssued = false;
    bool finalSegment = false;
    bool adjustDestinations = false;
    bool exitProduction = false;
    bool allowThroughUnits = false;
    bool preciseFinalZ = false;

    constexpr AIFollowPathStatePayload() = default;
    explicit constexpr AIFollowPathStatePayload(AIStateRequestId value)
        : request(value)
    {
    }
    constexpr bool operator==(const AIFollowPathStatePayload&) const noexcept = default;
};

struct AIWaypointPathStatePayload final
{
    AIStateRequestId request;
    AIWaypointHandle current;
    AIWaypointHandle prior;
    AIWaypointHandle completionTerminal;
    AITeamHandle team;
    PathHandle path;
    AIFixedPosition goal;
    AIFixedPosition groupOffset;
    uint64_t graphRevision = 0;
    uint64_t sourceOrderRevision = 0;
    uint64_t teamRevision = 0;
    int64_t extraDistanceRaw = 0;
    uint32_t generation = 1;
    uint32_t waypointHopLimit = 0;
    AIMoveToPhase phase = AIMoveToPhase::WaitingForPath;
    bool pathRequestIssued = false;
    bool moveAsTeam = false;
    bool exactPolyline = false;
    bool adjustDestinations = false;
    bool preciseFinalZ = false;
    bool awaitingTeamProgress = false;
    bool completionPending = false;

    constexpr AIWaypointPathStatePayload() = default;
    explicit constexpr AIWaypointPathStatePayload(AIStateRequestId value)
        : request(value)
    {
    }
    constexpr bool operator==(const AIWaypointPathStatePayload&) const noexcept = default;
};

struct AIMoveOutOfWayStatePayload final
{
    AIStateRequestId request;
    PathHandle path;
    AIFixedPosition goal;
    uint64_t sourceOrderRevision = 0;
    uint64_t deadlineTick = 0;
    uint32_t generation = 1;
    AIMoveToPhase phase = AIMoveToPhase::WaitingForPath;
    bool pathRequestIssued = false;
    bool allowPathThroughUnits = false;
    constexpr AIMoveOutOfWayStatePayload() = default;
    explicit constexpr AIMoveOutOfWayStatePayload(AIStateRequestId value) : request(value) {}
    constexpr bool operator==(const AIMoveOutOfWayStatePayload&) const noexcept = default;
};

struct AIApproachPathStatePayload final
{
    AIStateRequestId request;
    PathHandle path;
    AIFixedPosition goal;
    AIFixedPosition origin;
    ObjectId repulsor = INVALID_OBJECT_ID;
    ObjectId repulsor2 = INVALID_OBJECT_ID;
    uint64_t sourceOrderRevision = 0;
    uint64_t nextRepulsorScanTick = 0;
    uint32_t generation = 1;
    uint8_t repathsRemaining = 1;
    AIMoveToPhase phase = AIMoveToPhase::WaitingForPath;
    bool pathRequestIssued = false;
    bool adjustDestinations = false;
    constexpr AIApproachPathStatePayload() = default;
    explicit constexpr AIApproachPathStatePayload(AIStateRequestId value) : request(value) {}
    constexpr bool operator==(const AIApproachPathStatePayload&) const noexcept = default;
};

struct AIPickUpCrateStatePayload final
{
    AIMoveToStatePayload movement;
    uint8_t delayUpdatesRemaining = 0;
    constexpr AIPickUpCrateStatePayload() = default;
    explicit constexpr AIPickUpCrateStatePayload(AIStateRequestId request) : movement(request) {}
    constexpr AIPickUpCrateStatePayload(AIMoveToStatePayload movementValue, uint8_t delayValue)
        : movement(movementValue), delayUpdatesRemaining(delayValue) {}
    constexpr bool operator==(const AIPickUpCrateStatePayload&) const noexcept = default;
};

struct AIWanderPanicStatePayload final
{
    AIWaypointPathStatePayload movement;
    AIApproachPathStatePayload scan;
    constexpr AIWanderPanicStatePayload() = default;
    explicit constexpr AIWanderPanicStatePayload(AIStateRequestId request) : movement(request), scan(request) {}
    constexpr AIWanderPanicStatePayload(AIWaypointPathStatePayload movementValue,
                                        AIApproachPathStatePayload scanValue)
        : movement(movementValue), scan(scanValue) {}
    constexpr bool operator==(const AIWanderPanicStatePayload&) const noexcept = default;
};

struct AIMoveEvacuateStatePayload final
{
    AIMoveToStatePayload movement;
    AIFixedPosition origin;
    bool appendDeleteGoal = false;
    constexpr AIMoveEvacuateStatePayload() = default;
    explicit constexpr AIMoveEvacuateStatePayload(AIStateRequestId request) : movement(request) {}
    constexpr AIMoveEvacuateStatePayload(AIMoveToStatePayload movementValue,
                                         AIFixedPosition originValue,
                                         bool appendValue)
        : movement(movementValue), origin(originValue), appendDeleteGoal(appendValue) {}
    constexpr bool operator==(const AIMoveEvacuateStatePayload&) const noexcept = default;
};

struct AIContainmentStatePayload final
{
    AIStateRequestId request;
    ObjectId trackedGoal = INVALID_OBJECT_ID;
    ObjectId entryToClear = INVALID_OBJECT_ID;
    AIContainmentPhase phase = AIContainmentPhase::Inactive;
    constexpr AIContainmentStatePayload() = default;
    explicit constexpr AIContainmentStatePayload(AIStateRequestId value) : request(value) {}
    constexpr bool operator==(const AIContainmentStatePayload&) const noexcept = default;
};

struct AIHackInternetStatePayload final
{
    AIStateRequestId request;
    AIBehaviorProfileHandle profile;
    uint64_t sourceOrderRevision = 0;
    uint64_t profileRevision = 0;
    uint64_t phaseEndTick = 0;
    uint64_t nextPayoutTick = 0;
    uint64_t deferredOrderRevision = 0;
    AIHackInternetPhase phase = AIHackInternetPhase::Unpacking;
    constexpr AIHackInternetStatePayload() = default;
    explicit constexpr AIHackInternetStatePayload(AIStateRequestId value) : request(value) {}
    constexpr bool operator==(const AIHackInternetStatePayload&) const noexcept = default;
};

struct AIAttackStatePayload final
{
    AIStateRequestId request;
    AIAttackPhase phase = AIAttackPhase::Inactive;
    uint32_t phaseRevision = 0;
    uint64_t weaponRevision = 0;
    uint64_t sourceOrderRevision = 0;
    uint32_t pathGeneration = 0;
    PathHandle path;
    ObjectId trackedTarget = INVALID_OBJECT_ID;
    AIFixedPosition targetPosition;
    int64_t arrivalRadiusRaw = 0;
    int64_t minimumArrivalRadiusRaw = 0;
    bool pathRequestIssued = false;
    bool movementActive = false;
    bool aimingActive = false;
    bool firingActive = false;
    bool fireCommandIssued = false;
    bool contactWeapon = false;
    constexpr AIAttackStatePayload() = default;
    explicit constexpr AIAttackStatePayload(AIStateRequestId value) : request(value) {}
    constexpr bool operator==(const AIAttackStatePayload&) const noexcept = default;
};

using AIActiveStatePayload = std::variant<std::monostate,
                                          AIIdleStatePayload,
                                          AIWaitStatePayload,
                                          AIBusyStatePayload,
                                          AIDeadStatePayload,
                                          AIFaceStatePayload,
                                          AIMoveToStatePayload,
                                          AIFollowPathStatePayload,
                                          AIWaypointPathStatePayload,
                                          AIMoveOutOfWayStatePayload,
                                          AIApproachPathStatePayload,
                                          AIPickUpCrateStatePayload,
                                          AIWanderPanicStatePayload,
                                          AIMoveEvacuateStatePayload,
                                          AIContainmentStatePayload,
                                          AIHackInternetStatePayload,
                                          AIAttackStatePayload,
                                          AIDockStatePayload,
                                          AIInsertionStatePayload,
                                          AIOpportunityAttackMoveStatePayload,
                                          AIGuardStatePayload,
                                          AITacticalAttackStatePayload>;

struct AIStateData final
{
    AIStateParameters parameters;
    AIActiveStatePayload activePayload;
    AIStateId payloadState = AIStateId::Invalid;
    uint32_t activationSequence = 0;

    void activate(AIStateId state, uint64_t confirmedTick)
    {
        ++activationSequence;
        if (activationSequence == 0)
            ++activationSequence;
        payloadState = state;
        switch (state)
        {
        case AIStateId::Idle:
            activePayload.emplace<AIIdleStatePayload>(confirmedTick);
            break;
        case AIStateId::Wait:
            activePayload.emplace<AIWaitStatePayload>(parameters.waitEndTick);
            break;
        case AIStateId::Busy:
            activePayload.emplace<AIBusyStatePayload>(confirmedTick);
            break;
        case AIStateId::Dead:
            activePayload.emplace<AIDeadStatePayload>(confirmedTick);
            break;
        case AIStateId::FaceObject:
        case AIStateId::FacePosition:
            activePayload.emplace<AIFaceStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::MoveTo:
            activePayload.emplace<AIMoveToStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::PickUpCrate:
            activePayload.emplace<AIPickUpCrateStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::MoveAndEvacuate:
        case AIStateId::MoveAndEvacuateAndExit:
        case AIStateId::MoveAndDelete:
            activePayload.emplace<AIMoveEvacuateStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::Enter:
        case AIStateId::Exit:
        case AIStateId::ExitInstantly:
            activePayload.emplace<AIContainmentStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::HackInternet:
            activePayload.emplace<AIHackInternetStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::Guard:
        case AIStateId::GuardRetaliate:
        case AIStateId::GuardTunnelNetwork:
            activePayload.emplace<AIGuardStatePayload>();
            std::get<AIGuardStatePayload>(activePayload).request = {confirmedTick, activationSequence};
            break;
        case AIStateId::Hunt:
        case AIStateId::AttackSquad:
        case AIStateId::AttackArea:
            activePayload.emplace<AITacticalAttackStatePayload>(
                AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::AttackPosition:
        case AIStateId::AttackObject:
        case AIStateId::ForceAttackObject:
        case AIStateId::AttackAndFollowObject:
            activePayload.emplace<AIAttackStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::Dock:
        case AIStateId::GetRepaired:
        {
            AIDockStatePayload payload;
            payload.token.stateRequest={confirmedTick,activationSequence};
            payload.token.purpose=state==AIStateId::GetRepaired?AIDockPurpose::Repair:AIDockPurpose::Dock;
            activePayload=payload;
            break;
        }
        case AIStateId::RappelInto:
        case AIStateId::CombatDrop:
        {
            AIInsertionStatePayload payload;payload.request={confirmedTick,activationSequence};activePayload=payload;break;
        }
        case AIStateId::AttackMoveTo:
        case AIStateId::AttackFollowWaypointPathAsIndividuals:
        case AIStateId::AttackFollowWaypointPathAsTeam:
            activePayload.emplace<AIOpportunityAttackMoveStatePayload>(
                AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::FollowPath:
        case AIStateId::FollowExitProductionPath:
            activePayload.emplace<AIFollowPathStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::FollowWaypointPathAsTeam:
        case AIStateId::FollowWaypointPathAsIndividuals:
        case AIStateId::FollowWaypointPathAsTeamExact:
        case AIStateId::FollowWaypointPathAsIndividualsExact:
            activePayload.emplace<AIWaypointPathStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::Wander:
        case AIStateId::Panic:
            activePayload.emplace<AIWanderPanicStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::MoveOutOfTheWay:
            activePayload.emplace<AIMoveOutOfWayStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        case AIStateId::MoveAndTighten:
        case AIStateId::MoveAwayFromRepulsors:
        case AIStateId::WanderInPlace:
            activePayload.emplace<AIApproachPathStatePayload>(AIStateRequestId{confirmedTick, activationSequence});
            break;
        default:
            activePayload.emplace<std::monostate>();
            break;
        }
    }

    void clear()
    {
        activePayload.emplace<std::monostate>();
        payloadState = AIStateId::Invalid;
    }
};

} // namespace engine::ai
