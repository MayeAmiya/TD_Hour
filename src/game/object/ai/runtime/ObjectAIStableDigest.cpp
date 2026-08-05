#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <variant>

#include "game/object/ai/runtime/ObjectAIStableDigest.h"
#include "game/object/ai/runtime/ObjectAIStableDigestTestSupport.h"
#include "game/object/ai/runtime/ObjectAIRuntime.h"

namespace engine::ai
{

namespace detail
{

// The digest is FNV-1a over an explicitly encoded, little-endian semantic
// stream. No object representation, padding, allocation address, or spare
// container capacity is observed.
class ObjectAIStableDigestWriter final
{
public:
    void boolean(bool value) noexcept
    {
        byte(value ? uint8_t{1} : uint8_t{0});
    }

    void u8(uint8_t value) noexcept
    {
        byte(value);
    }

    void u32(uint32_t value) noexcept
    {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            byte(static_cast<uint8_t>((value >> shift) & uint32_t{0xff}));
    }

    void i32(int32_t value) noexcept
    {
        u32(static_cast<uint32_t>(value));
    }

    void u64(uint64_t value) noexcept
    {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            byte(static_cast<uint8_t>((value >> shift) & uint64_t{0xff}));
    }

    void i64(int64_t value) noexcept
    {
        u64(static_cast<uint64_t>(value));
    }

    void count(size_t value) noexcept
    {
        u64(static_cast<uint64_t>(value));
    }

    template <typename Enum>
        requires std::is_enum_v<Enum>
    void enumeration(Enum value) noexcept
    {
        using Underlying = std::underlying_type_t<Enum>;
        using Unsigned = std::make_unsigned_t<Underlying>;
        u64(static_cast<uint64_t>(static_cast<Unsigned>(value)));
    }

    [[nodiscard]] uint64_t finish() const noexcept
    {
        return m_value;
    }

private:
    void byte(uint8_t value) noexcept
    {
        m_value ^= value;
        m_value *= FnvPrime;
    }

    static constexpr uint64_t FnvOffsetBasis = 14695981039346656037ull;
    static constexpr uint64_t FnvPrime = 1099511628211ull;

    uint64_t m_value = FnvOffsetBasis;
};

inline void encode(ObjectAIStableDigestWriter& writer, ObjectId value) noexcept
{
    writer.u32(value.value);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIFixedPosition& value) noexcept
{
    writer.i64(value.xRaw);
    writer.i64(value.yRaw);
    writer.i64(value.zRaw);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIStateRequestId& value) noexcept
{
    writer.u64(value.issuedTick);
    writer.u32(value.sequence);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIAsyncOrderIdentity& value) noexcept
{
    encode(writer, value.subject);
    writer.u64(value.queueRevision);
    writer.u64(value.externalRevision);
    writer.u64(value.issuedTick);
    writer.u32(value.sourceSequence);
    writer.u32(value.sourceScriptId);
    writer.u32(value.systemPurposeInstance);
    writer.u8(value.source);
    writer.u8(value.systemPurpose);
}

inline void encode(ObjectAIStableDigestWriter& writer, const PathCorrelation& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.stateRequest);
    writer.u32(value.generation);
    writer.u64(value.sourceOrderRevision);
    encode(writer, value.orderIdentity.subject);
    writer.u64(value.orderIdentity.queueRevision);
    writer.u64(value.orderIdentity.externalRevision);
    writer.u64(value.orderIdentity.issuedTick);
    writer.u32(value.orderIdentity.sourceSequence);
    writer.u32(value.orderIdentity.sourceScriptId);
    writer.u32(value.orderIdentity.systemPurposeInstance);
    writer.u8(value.orderIdentity.source);
    writer.u8(value.orderIdentity.systemPurpose);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIAttackCorrelation& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.stateRequest);
    writer.enumeration(value.state);
    writer.enumeration(value.phase);
    writer.u64(value.weaponRevision);
    writer.u32(value.phaseRevision);
    encode(writer, value.orderIdentity);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AIOpportunityAttackMoveCorrelation& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.stateRequest);
    writer.enumeration(value.state);
    writer.enumeration(value.phase);
    writer.enumeration(value.operation);
    writer.u64(value.sourceOrderRevision);
    encode(writer, value.orderIdentity);
    writer.u32(value.phaseRevision);
    writer.u32(value.operationRevision);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIStateMachineRuntime& value) noexcept
{
    writer.enumeration(value.currentState);
    writer.enumeration(value.previousState);
    writer.enumeration(value.defaultState);
    writer.enumeration(value.temporaryResumeState);
    writer.u64(value.enteredTick);
    writer.u64(value.wakeTick);
    writer.u64(value.temporaryEndTickExclusive);
    writer.u64(value.revision);
    writer.u64(value.transitionBudgetTick);
    writer.enumeration(value.substateDomain);
    writer.u8(value.substate);
    writer.enumeration(value.lock);
    writer.enumeration(value.lastWakeReason);
    writer.enumeration(value.lastTransitionReason);
    writer.u8(value.transitionsThisTick);
    writer.boolean(value.initialized);
    writer.boolean(value.temporaryActive);
    writer.boolean(value.transitionLimitExceeded);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIStateParameters& value) noexcept
{
    writer.u64(value.waitEndTick);
    encode(writer, value.goalObject);
    encode(writer, value.goalPosition);
    encode(writer, value.ignoredObstacle);
    writer.u64(value.sourceOrderRevision);
    writer.u64(value.pathSequence.value);
    writer.u64(value.pathSequenceRevision);
    writer.u64(value.waypoint.value);
    writer.u64(value.waypointGraphRevision);
    writer.u64(value.waypointTeam.value);
    encode(writer, value.waypointGroupOffset);
    writer.i64(value.waypointGroupSpeedRaw);
    writer.u64(value.groupPathId);
    writer.u32(value.groupPathMemberOrdinal);
    writer.u32(value.groupPathMemberCount);
    encode(writer, value.groupPathStart);
    encode(writer, value.groupPathOffset);
    writer.u64(value.existingPath.value);
    writer.u32(value.pathSurfaceMask);
    writer.i64(value.arrivalRadiusRaw);
    writer.boolean(value.hasGoalPosition);
    writer.boolean(value.adjustDestinations);
    writer.boolean(value.allArmyHunt);
    writer.boolean(value.useTeamCommonTarget);
    writer.u64(value.tacticalTargetCollection.value);
    writer.u64(value.tacticalTargetCollectionRevision);
    writer.u64(value.tacticalAttackArea.value);
    writer.u64(value.tacticalAttackAreaRevision);
    writer.enumeration(value.tacticalSquadSelection);
    writer.i64(value.guardRangeRaw);
    writer.i64(value.guardVisionRangeRaw);
    encode(writer, value.guardAnchor);
    writer.boolean(value.hasGuardAnchor);
    writer.boolean(value.enterGuardTargets);
    writer.boolean(value.guardWithoutPursuit);
    writer.boolean(value.guardFlyingOnly);
    writer.boolean(value.guardTracksAnchor);
    encode(writer, value.guardRetaliateAggressor);
}

inline void encode(ObjectAIStableDigestWriter&, const std::monostate&) noexcept
{
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIIdleStatePayload& value) noexcept
{
    writer.u64(value.nextTargetScanTick);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIWaitStatePayload& value) noexcept
{
    writer.u64(value.endTick);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIBusyStatePayload& value) noexcept
{
    writer.u64(value.enteredTick);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIDeadStatePayload& value) noexcept
{
    writer.u64(value.enteredTick);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIFaceStatePayload& value) noexcept
{
    encode(writer, value.request);
    writer.boolean(value.commandIssued);
    writer.boolean(value.canTurnInPlace);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIMoveToStatePayload& value) noexcept
{
    encode(writer, value.request);
    encode(writer, value.resolvedGoal);
    encode(writer, value.adjustedGoal);
    writer.u64(value.path.value);
    writer.u64(value.sourceOrderRevision);
    writer.u32(value.generation);
    writer.u32(value.adjustedLayer);
    writer.enumeration(value.phase);
    writer.boolean(value.pathRequestIssued);
    writer.boolean(value.adjustDestinations);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIFollowPathStatePayload& value) noexcept
{
    encode(writer, value.request);
    writer.u64(value.sequence.value);
    writer.u64(value.path.value);
    encode(writer, value.ignoredObstacle);
    encode(writer, value.segmentGoal);
    writer.u64(value.sequenceRevision);
    writer.u64(value.sourceOrderRevision);
    writer.i64(value.extraDistanceRaw);
    writer.u32(value.index);
    writer.u32(value.generation);
    writer.u8(value.retriesRemaining);
    writer.enumeration(value.phase);
    writer.boolean(value.pathRequestIssued);
    writer.boolean(value.finalSegment);
    writer.boolean(value.adjustDestinations);
    writer.boolean(value.exitProduction);
    writer.boolean(value.allowThroughUnits);
    writer.boolean(value.preciseFinalZ);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIWaypointPathStatePayload& value) noexcept
{
    encode(writer, value.request);
    writer.u64(value.current.value);
    writer.u64(value.prior.value);
    writer.u64(value.completionTerminal.value);
    writer.u64(value.team.value);
    writer.u64(value.path.value);
    encode(writer, value.goal);
    encode(writer, value.groupOffset);
    writer.u64(value.graphRevision);
    writer.u64(value.sourceOrderRevision);
    writer.u64(value.teamRevision);
    writer.i64(value.extraDistanceRaw);
    writer.u32(value.generation);
    writer.u32(value.waypointHopLimit);
    writer.enumeration(value.phase);
    writer.boolean(value.pathRequestIssued);
    writer.boolean(value.moveAsTeam);
    writer.boolean(value.exactPolyline);
    writer.boolean(value.adjustDestinations);
    writer.boolean(value.preciseFinalZ);
    writer.boolean(value.awaitingTeamProgress);
    writer.boolean(value.completionPending);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIMoveOutOfWayStatePayload& value) noexcept
{
    encode(writer, value.request);
    writer.u64(value.path.value);
    encode(writer, value.goal);
    writer.u64(value.sourceOrderRevision);
    writer.u64(value.deadlineTick);
    writer.u32(value.generation);
    writer.enumeration(value.phase);
    writer.boolean(value.pathRequestIssued);
    writer.boolean(value.allowPathThroughUnits);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIApproachPathStatePayload& value) noexcept
{
    encode(writer, value.request);
    writer.u64(value.path.value);
    encode(writer, value.goal);
    encode(writer, value.origin);
    encode(writer, value.repulsor);
    encode(writer, value.repulsor2);
    writer.u64(value.sourceOrderRevision);
    writer.u64(value.nextRepulsorScanTick);
    writer.u32(value.generation);
    writer.u8(value.repathsRemaining);
    writer.enumeration(value.phase);
    writer.boolean(value.pathRequestIssued);
    writer.boolean(value.adjustDestinations);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIPickUpCrateStatePayload& value) noexcept
{
    encode(writer, value.movement);
    writer.u8(value.delayUpdatesRemaining);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIWanderPanicStatePayload& value) noexcept
{
    encode(writer, value.movement);
    encode(writer, value.scan);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIMoveEvacuateStatePayload& value) noexcept
{
    encode(writer, value.movement);
    encode(writer, value.origin);
    writer.boolean(value.appendDeleteGoal);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIContainmentStatePayload& value) noexcept
{
    encode(writer, value.request);
    encode(writer, value.trackedGoal);
    encode(writer, value.entryToClear);
    writer.enumeration(value.phase);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIHackInternetStatePayload& value) noexcept
{
    encode(writer, value.request);
    writer.u64(value.profile.value);
    writer.u64(value.sourceOrderRevision);
    writer.u64(value.profileRevision);
    writer.u64(value.phaseEndTick);
    writer.u64(value.nextPayoutTick);
    writer.u64(value.deferredOrderRevision);
    writer.enumeration(value.phase);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIAttackStatePayload& value) noexcept
{
    encode(writer, value.request);
    writer.enumeration(value.phase);
    writer.u32(value.phaseRevision);
    writer.u64(value.weaponRevision);
    writer.u64(value.sourceOrderRevision);
    writer.u32(value.pathGeneration);
    writer.u64(value.path.value);
    encode(writer, value.trackedTarget);
    encode(writer, value.targetPosition);
    writer.i64(value.arrivalRadiusRaw);
    writer.i64(value.minimumArrivalRadiusRaw);
    writer.boolean(value.pathRequestIssued);
    writer.boolean(value.movementActive);
    writer.boolean(value.aimingActive);
    writer.boolean(value.firingActive);
    writer.boolean(value.fireCommandIssued);
    writer.boolean(value.contactWeapon);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIDockStatePayload& value) noexcept
{
    encode(writer, value.token.subject);
    encode(writer, value.token.dock);
    encode(writer, value.token.stateRequest);
    writer.enumeration(value.token.purpose);
    writer.enumeration(value.phase);
    writer.u32(value.phaseRevision);
    writer.u32(value.exchangeSequence);
    writer.enumeration(value.pendingRequest);
    writer.i32(value.approachPosition);
    writer.u64(value.clearanceEnterTick);
    writer.u64(value.nextActionTick);
    writer.u32(value.actionDelayTicks);
    encode(writer, value.drone);
    writer.boolean(value.movementActive);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIInsertionStatePayload& value) noexcept
{
    encode(writer, value.request);
    encode(writer, value.rappelTarget);
    writer.boolean(value.rappelTargetIsBuilding);
    writer.i64(value.rappelDestinationZRaw);
    writer.i64(value.rappelSpeedRaw);
    writer.enumeration(value.rappelPhase);
    writer.u64(value.combatDropOperation.value);
    writer.u32(value.combatDropNextEventSequence);
    writer.enumeration(value.combatDropPhase);
    writer.u64(value.combatDropPath.value);
    writer.u64(value.combatDropSourceOrderRevision);
    writer.u32(value.combatDropPathGeneration);
    writer.i64(value.combatDropOldPreferredHeightRaw);
    writer.boolean(value.combatDropPathRequestIssued);
    writer.boolean(value.combatDropApproachConfigured);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIOpportunityAttackMoveStatePayload& value) noexcept
{
    encode(writer, value.request);
    writer.u64(value.sourceOrderRevision);
    writer.enumeration(value.state);
    writer.enumeration(value.phase);
    writer.u32(value.phaseRevision);
    writer.u32(value.nextOperationRevision);
    writer.enumeration(value.scanOperation);
    writer.u32(value.queryRevision);
    writer.enumeration(value.childOperation);
    writer.u32(value.childRevision);
    encode(writer, value.childTarget);
    writer.u8(value.retriesRemaining);
    writer.u64(value.retryWakeTick);
    writer.u8(value.movementTerminal);
    writer.boolean(value.active);
    writer.boolean(value.queryPending);
    writer.boolean(value.childActive);
    writer.boolean(value.movementPaused);
    writer.boolean(value.resumeRequired);
    writer.boolean(value.resumeScanComplete);
    writer.boolean(value.forceRetarget);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIGuardStatePayload& value) noexcept
{
    encode(writer, value.request);
    writer.u64(value.sourceOrderRevision);
    writer.enumeration(value.state);
    writer.boolean(value.active);
    writer.enumeration(value.phase);
    writer.u32(value.nextOperationRevision);
    writer.enumeration(value.taskOperation);
    writer.enumeration(value.taskDomain);
    writer.u32(value.taskRevision);
    writer.boolean(value.scanPending);
    writer.u32(value.scanRevision);
    writer.u64(value.nextScanTick);
    writer.u64(value.chaseDeadlineTick);
    encode(writer, value.nemesis);
    encode(writer, value.anchor);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIGuardCorrelation& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.stateRequest);
    writer.enumeration(value.state);
    writer.enumeration(value.phase);
    writer.enumeration(value.operation);
    writer.u64(value.sourceOrderRevision);
    encode(writer, value.orderIdentity);
    writer.u32(value.operationRevision);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIGuardTacticalCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.target);
    encode(writer, value.anchor);
    writer.i64(value.radiusRaw);
    writer.u64(value.area.value);
    writer.u64(value.areaRevision);
    writer.boolean(value.enterGuardTargets);
    writer.boolean(value.rejectOrdinaryBuildings);
    writer.boolean(value.flyingOnly);
    writer.boolean(value.publishTunnelNemesis);
    writer.boolean(value.clearTeamTarget);
    writer.u64(value.confirmedTick);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIGuardInteractionCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.target);
    encode(writer, value.targetPosition);
    writer.boolean(value.targetPositionValid);
    writer.boolean(value.urgent);
    writer.boolean(value.clearTeamTarget);
    writer.u64(value.confirmedTick);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIGuardFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.target);
    encode(writer, value.targetPosition);
    writer.u64(value.confirmedTick);
    writer.boolean(value.targetWithinInnerRange);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AITacticalAttackStatePayload& value) noexcept
{
    writer.boolean(value.active);
    encode(writer, value.request);
    writer.u64(value.sourceOrderRevision);
    writer.enumeration(value.state);
    writer.enumeration(value.wrapperPhase);
    writer.u64(value.nextScanTick);
    encode(writer, value.target);
    writer.u64(value.targetRevision);
    writer.u64(value.collectionHandle.value);
    writer.u64(value.collectionRevision);
    writer.u64(value.areaHandle.value);
    writer.u64(value.areaRevision);
    writer.u32(value.nextQueryGeneration);
    writer.enumeration(value.pendingQuery);
    writer.u32(value.queryGeneration);
    writer.u32(value.nextChildGeneration);
    writer.enumeration(value.childState);
    writer.u32(value.childGeneration);
}

struct ActivePayloadEncoder final
{
    ObjectAIStableDigestWriter& writer;

    void operator()(const std::monostate& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIIdleStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIWaitStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIBusyStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIDeadStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIFaceStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIMoveToStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIFollowPathStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIWaypointPathStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIMoveOutOfWayStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIApproachPathStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIPickUpCrateStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIWanderPanicStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIMoveEvacuateStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIContainmentStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIHackInternetStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIAttackStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIDockStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIInsertionStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIOpportunityAttackMoveStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AIGuardStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
    void operator()(const AITacticalAttackStatePayload& value) const noexcept
    {
        encode(writer, value);
    }
};

static_assert(std::variant_size_v<AIActiveStatePayload> == 22,
              "Update ObjectAIStableDigest for every active payload alternative");

inline void encode(ObjectAIStableDigestWriter& writer, const AIActiveStatePayload& value) noexcept
{
    if (value.valueless_by_exception())
    {
        writer.u32(std::numeric_limits<uint32_t>::max());
        return;
    }
    writer.u32(static_cast<uint32_t>(value.index()));
    std::visit(ActivePayloadEncoder{writer}, value);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIStateData& value) noexcept
{
    encode(writer, value.parameters);
    encode(writer, value.activePayload);
    writer.enumeration(value.payloadState);
    writer.u32(value.activationSequence);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIStateSoASlotSnapshot& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.runtime);
    encode(writer, value.state);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIStateSoASlotRegistrySnapshot& value) noexcept
{
    writer.u32(value.schemaVersion);
    writer.u32(value.batchIndex);
    writer.count(value.generations.size());
    for (const uint32_t generation : value.generations)
        writer.u32(generation);
    writer.count(value.slots.size());
    for (const AIStateSoASlotSnapshot& slot : value.slots)
        encode(writer, slot);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIObjectMembershipEvent& value) noexcept
{
    encode(writer, value.subject);
    writer.u64(value.confirmedTick);
    writer.u32(value.sequence);
    writer.enumeration(value.operation);
    writer.enumeration(value.initialCapabilities);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const ObjectAIRecipeBindingSnapshot& value) noexcept
{
    encode(writer, value.subject);
    writer.enumeration(value.recipe);
    writer.enumeration(value.state);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const ObjectAIOrderIdentity& value) noexcept
{
    encode(writer, value.subject);
    writer.u64(value.queueRevision);
    writer.u64(value.externalRevision);
    writer.u64(value.issuedTick);
    writer.u32(value.sourceSequence);
    writer.u32(value.sourceScriptId);
    writer.enumeration(value.source);
    writer.enumeration(value.systemPurpose);
    writer.u32(value.systemPurposeInstance);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const ObjectAIOrderAdmissionRequest& value) noexcept
{
    writer.enumeration(value.kind);
    encode(writer, value.identity);
    writer.boolean(value.attackMove);
    writer.enumeration(value.moveRouteSubtype);
    writer.u64(value.waypointStart.value);
    writer.u64(value.waypointGraphRevision);
    writer.u64(value.waypointTeam.value);
    writer.enumeration(value.tacticalAttackSubtype);
    writer.boolean(value.allArmyHunt);
    writer.boolean(value.useTeamCommonTarget);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const ObjectAIOrderAdmissionSlotSnapshot& value) noexcept
{
    encode(writer, value.subject);
    writer.u32(value.generation);
    writer.enumeration(value.capabilities);
    writer.u64(value.observedQueueRevision);
    writer.u64(value.observedExternalRevision);
    writer.boolean(value.bound);
    writer.boolean(value.active);
    writer.boolean(value.hasHistory);
    if (value.hasHistory)
        encode(writer, value.historicalOrder);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const ObjectAIOrderAdmissionSnapshot& value) noexcept
{
    writer.u32(value.schemaVersion);
    writer.count(value.slots.size());
    for (const ObjectAIOrderAdmissionSlotSnapshot& slot : value.slots)
        encode(writer, slot);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIWakeEvent& value) noexcept
{
    encode(writer, value.object);
    writer.u64(value.wakeTick);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIStateCommand& value) noexcept
{
    writer.enumeration(value.kind);
    encode(writer, value.subject);
    encode(writer, value.request);
    encode(writer, value.targetObject);
    encode(writer, value.targetPosition);
    writer.boolean(value.canTurnInPlace);
    writer.u64(value.confirmedTick);
    encode(writer, value.orderIdentity);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIFacingFeedback& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.request);
    writer.enumeration(value.status);
    writer.u64(value.confirmedTick);
    encode(writer, value.orderIdentity);
}

inline void encode(ObjectAIStableDigestWriter& writer, const PathRequest& value) noexcept
{
    encode(writer, value.correlation);
    encode(writer, value.start);
    encode(writer, value.originalGoal);
    writer.boolean(value.adjustDestinations);
    encode(writer, value.ignoredObstacle);
    writer.u32(value.surfaceMask);
    writer.u8(value.clearanceProfile.radiusCells);
    writer.boolean(value.clearanceProfile.centerInCell);
    writer.boolean(value.clearanceProfile.frozen);
    writer.i64(value.arrivalRadiusRaw);
    writer.i64(value.minimumArrivalRadiusRaw);
    writer.enumeration(value.kind);
    writer.u64(value.currentPath.value);
    encode(writer, value.safePathRepulsor);
    writer.enumeration(value.traversalMode);
    writer.u64(value.waypointStart.value);
    writer.u64(value.waypointGraphRevision);
    writer.u32(value.waypointHopLimit);
    encode(writer, value.polylineOffset);
    writer.u64(value.groupPathId);
    writer.u32(value.groupPathMemberOrdinal);
    writer.u32(value.groupPathMemberCount);
    encode(writer, value.groupPathOffset);
    writer.i64(value.extraDistanceRaw);
    writer.boolean(value.pathThroughUnits);
    writer.boolean(value.preciseFinalZ);
    writer.boolean(value.airWings);
    writer.u8(value.crusherLevel);
    writer.count(value.dozerPassableObstacles.size());
    for (const uint64_t object : value.dozerPassableObstacles)
        writer.u64(object);
    encode(writer, value.attackTarget);
    writer.boolean(value.attackContactWeapon);
    writer.boolean(value.attackLineOfSightEnabled);
    encode(writer, value.attackSubjectContainer);
    encode(writer, value.attackTargetContainer);
    encode(writer, value.attackSubjectSlaver);
    encode(writer, value.attackTargetSlaver);
    writer.count(value.attackSeeThroughObstacles.size());
    for (const uint64_t object : value.attackSeeThroughObstacles)
        writer.u64(object);
    writer.u64(value.objectSnapshotTick);
    writer.count(value.objectCells.size());
    for (const AIPathObjectCellSnapshot& objectCell : value.objectCells)
    {
        encode(writer, objectCell.object);
        writer.u32(objectCell.layer);
        writer.u32(objectCell.cell);
        writer.enumeration(objectCell.effect);
    }
    encode(writer, value.blockingBridgeCandidate);
}

inline void encode(ObjectAIStableDigestWriter& writer, const PathFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.status);
    writer.u64(value.confirmedTick);
    writer.u64(value.path.value);
    encode(writer, value.adjustedGoal);
    writer.u32(value.adjustedLayer);
    writer.boolean(value.retryPath);
    writer.u64(value.nextEligibleTick);
    encode(writer, value.blockingBridge);
}

inline void encode(ObjectAIStableDigestWriter& writer, const MovementCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    writer.u64(value.path.value);
    encode(writer, value.ignoredObstacle);
    writer.i64(value.speedLimitRaw);
    writer.i64(value.extraDistanceRaw);
    writer.enumeration(value.mode);
    writer.boolean(value.panicking);
    writer.boolean(value.clearGoal);
    writer.boolean(value.preserveUltraAccurateFinalPosition);
    writer.boolean(value.allowPathThroughUnits);
    writer.u64(value.confirmedTick);
}

inline void encode(ObjectAIStableDigestWriter& writer, const MovementFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.status);
    writer.u64(value.confirmedTick);
    writer.u32(value.blockedTicks);
    writer.i64(value.alongPathDistanceRaw);
    writer.i64(value.finalNodeXYDistanceRaw);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIAttackCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.target);
    encode(writer, value.targetPosition);
    writer.boolean(value.attacksObject);
    writer.boolean(value.forceAttack);
    writer.u64(value.confirmedTick);
}

inline void encode(ObjectAIStableDigestWriter& writer, const AIAttackFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.target);
    encode(writer, value.targetPosition);
    writer.u64(value.confirmedTick);
    writer.boolean(value.targetValid);
    writer.boolean(value.targetEffectivelyDead);
    writer.boolean(value.shotLimitReached);
    writer.boolean(value.targetMobile);
    writer.boolean(value.hasWeapon);
    writer.boolean(value.attackAllowed);
    writer.boolean(value.canPossiblyAttack);
    writer.boolean(value.inRange);
    writer.boolean(value.viewBlocked);
    writer.boolean(value.wantToSquishTarget);
    writer.boolean(value.aimReady);
    writer.boolean(value.aimTemporarilyPrevented);
    writer.boolean(value.weaponPreAttack);
    writer.boolean(value.weaponReady);
    writer.boolean(value.weaponSlotAllowed);
    writer.boolean(value.contactWeapon);
    writer.boolean(value.canPursue);
    writer.boolean(value.chaseAllowed);
    writer.i64(value.attackArrivalRadiusRaw);
    writer.i64(value.attackMinimumArrivalRadiusRaw);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIAttackOrderCompletion& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.outcome);
    writer.u64(value.confirmedTick);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AIOpportunityAttackMoveQueryCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    writer.u64(value.confirmedTick);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AIOpportunityAttackMoveQueryFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.target);
    encode(writer, value.targetPosition);
    writer.boolean(value.targetPositionValid);
    writer.boolean(value.commonTeamTarget);
    writer.u64(value.confirmedTick);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AIOpportunityAttackMoveChildCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.target);
    encode(writer, value.targetPosition);
    writer.boolean(value.targetPositionValid);
    writer.boolean(value.commandSourceIsAI);
    writer.u64(value.confirmedTick);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AIOpportunityAttackMoveChildFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    writer.u64(value.confirmedTick);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AITacticalAttackQueryCorrelation& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.stateRequest);
    writer.enumeration(value.wrapperState);
    writer.u64(value.sourceOrderRevision);
    encode(writer, value.orderIdentity);
    writer.u32(value.generation);
    writer.enumeration(value.query);
    writer.u64(value.collection.value);
    writer.u64(value.collectionRevision);
    writer.u64(value.area.value);
    writer.u64(value.areaRevision);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AITacticalAttackQueryCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    writer.enumeration(value.squadSelection);
    writer.boolean(value.canAttackOnly);
    writer.boolean(value.useAttackPriority);
    writer.boolean(value.fallbackWithoutAttackPriority);
    writer.boolean(value.useTeamCommonTarget);
    writer.u64(value.confirmedTick);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AITacticalAttackQueryFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.status);
    encode(writer, value.target);
    encode(writer, value.targetPosition);
    writer.boolean(value.targetPositionValid);
    writer.u64(value.targetRevision);
    writer.u64(value.confirmedTick);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AITacticalAttackChildCorrelation& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.stateRequest);
    writer.enumeration(value.wrapperState);
    writer.u64(value.sourceOrderRevision);
    encode(writer, value.orderIdentity);
    writer.enumeration(value.childState);
    writer.u32(value.generation);
    encode(writer, value.target);
    writer.u64(value.targetRevision);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AITacticalAttackChildCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.targetPosition);
    writer.boolean(value.targetPositionValid);
    writer.boolean(value.releaseTemporaryWeaponLock);
    writer.u64(value.confirmedTick);
}

inline void encode(
    ObjectAIStableDigestWriter& writer,
    const AITacticalAttackChildFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.status);
    writer.u64(value.confirmedTick);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIDockCorrelation& value) noexcept
{
    encode(writer, value.token.subject);
    encode(writer, value.token.dock);
    encode(writer, value.token.stateRequest);
    writer.enumeration(value.token.purpose);
    writer.enumeration(value.phase);
    writer.enumeration(value.moveStage);
    writer.u32(value.phaseRevision);
    writer.u32(value.exchangeSequence);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIDockRequest& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.position);
    writer.i32(value.approachPosition);
    writer.u64(value.confirmedTick);
    writer.boolean(value.ignoreDockObstacle);
    writer.boolean(value.allowPathThroughUnits);
    writer.boolean(value.adjustDestination);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIDockFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.request);
    writer.enumeration(value.status);
    encode(writer, value.position);
    writer.i32(value.approachPosition);
    writer.u32(value.actionDelayTicks);
    encode(writer, value.drone);
    writer.boolean(value.allowPassthrough);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIContainmentCorrelation& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.stateRequest);
    writer.enumeration(value.state);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIContainmentCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.goal);
    encode(writer, value.goalPosition);
    writer.enumeration(value.door);
    writer.boolean(value.ignoreGoalObstacle);
    writer.boolean(value.allowInvalidPosition);
    writer.boolean(value.adjustDestination);
    writer.u64(value.confirmedTick);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIContainmentFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.goal);
    encode(writer, value.goalPosition);
    writer.enumeration(value.reservedDoor);
    writer.boolean(value.goalHasContain);
    writer.boolean(value.goalHasExitInterface);
    writer.boolean(value.goalContainedByOther);
    writer.boolean(value.goalAboveTerrain);
    writer.boolean(value.subjectAboveTerrain);
    writer.boolean(value.verticalOverlap);
    writer.boolean(value.enemy);
    writer.boolean(value.attackPossible);
    writer.boolean(value.closeEnough);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIInsertionCorrelation& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.stateRequest);
    writer.enumeration(value.state);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIInsertionMotionCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.position);
    writer.i64(value.verticalSpeedRaw);
    writer.i64(value.orientationRaw);
    writer.i64(value.preferredHeightRaw);
    writer.u32(value.layer);
    writer.boolean(value.ultraAccurate);
    writer.u64(value.confirmedTick);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIInsertionMotionFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.goal);
    encode(writer, value.subjectPosition);
    writer.i64(value.layerHeightRaw);
    writer.i64(value.groundHeightRaw);
    writer.i64(value.buildingTopRaw);
    writer.i64(value.desiredSpeedRaw);
    writer.i64(value.maximumRappelSpeedRaw);
    writer.i64(value.previousPreferredHeightRaw);
    writer.i64(value.approachPreferredHeightRaw);
    writer.u32(value.destinationLayer);
    writer.boolean(value.canRappel);
    writer.boolean(value.goalIsStructure);
    writer.boolean(value.goalAlive);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIInsertionContainmentCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.building);
    writer.u8(value.maximumEnemiesToKill);
    writer.u64(value.confirmedTick);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIInsertionContainmentFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.building);
    writer.u8(value.enemiesKilled);
    writer.boolean(value.canContain);
    encode(writer, value.fallbackPosition);
    encode(writer, value.fallbackPathEnd);
    writer.i64(value.fallbackOrientationRaw);
    writer.boolean(value.fallbackPathFound);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIInsertionOperationCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    writer.u64(value.operation.value);
    writer.u32(value.eventSequence);
    encode(writer, value.goal);
    encode(writer, value.goalPosition);
    encode(writer, value.child);
    writer.i64(value.childRappelSpeedRaw);
    writer.u64(value.confirmedTick);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIInsertionOperationFeedback& value) noexcept
{
    encode(writer, value.correlation);
    writer.u64(value.operation.value);
    writer.enumeration(value.kind);
    writer.u32(value.eventSequence);
    encode(writer, value.child);
    writer.i64(value.childRappelSpeedRaw);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIInsertionEffectCommand& value) noexcept
{
    encode(writer, value.correlation);
    writer.enumeration(value.kind);
    encode(writer, value.target);
    writer.u8(value.enemiesKilled);
    writer.u64(value.confirmedTick);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIWaypointCompletionEvent& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.stateRequest);
    writer.u64(value.terminal.value);
    writer.u64(value.confirmedTick);
}

template <typename Value, typename Encoder>
inline void encodeVector(ObjectAIStableDigestWriter& writer,
                         const container::Vector<Value>& values,
                         Encoder encoder) noexcept
{
    writer.count(values.size());
    for (const Value& value : values)
        encoder(writer, value);
}

inline void encode(ObjectAIStableDigestWriter& writer, const ObjectAITransientSnapshot& value) noexcept
{
    writer.u32(value.schemaVersion);
    encodeVector(writer,
                 value.wakeEvents,
                 [](ObjectAIStableDigestWriter& output, const AIWakeEvent& item) noexcept { encode(output, item); });
    encodeVector(writer,
                 value.facingCommands,
                 [](ObjectAIStableDigestWriter& output, const AIStateCommand& item) noexcept { encode(output, item); });
    encodeVector(writer,
                 value.facingFeedback,
                 [](ObjectAIStableDigestWriter& output, const AIFacingFeedback& item) noexcept
                 { encode(output, item); });
    encodeVector(writer,
                 value.pathRequests,
                 [](ObjectAIStableDigestWriter& output, const PathRequest& item) noexcept { encode(output, item); });
    writer.count(value.pathRequestSubmitted.size());
    for (const uint8_t submitted : value.pathRequestSubmitted)
        writer.u8(submitted);
    writer.count(value.pathRequestNextEligibleTick.size());
    for (const uint64_t nextEligibleTick : value.pathRequestNextEligibleTick)
        writer.u64(nextEligibleTick);
    encodeVector(writer,
                 value.pathFeedback,
                 [](ObjectAIStableDigestWriter& output, const PathFeedback& item) noexcept { encode(output, item); });
    encodeVector(writer,
                 value.movementCommands,
                 [](ObjectAIStableDigestWriter& output, const MovementCommand& item) noexcept
                 { encode(output, item); });
    encodeVector(writer,
                 value.movementFeedback,
                 [](ObjectAIStableDigestWriter& output, const MovementFeedback& item) noexcept
                 { encode(output, item); });
    encodeVector(
        writer, value.waypointCompletions,
        [](ObjectAIStableDigestWriter& output,
           const AIWaypointCompletionEvent& item) noexcept
        { encode(output, item); });
    encodeVector(writer,
                 value.attackCommands,
                 [](ObjectAIStableDigestWriter& output, const AIAttackCommand& item) noexcept
                 { encode(output, item); });
    encodeVector(writer,
                 value.attackFeedback,
                 [](ObjectAIStableDigestWriter& output, const AIAttackFeedback& item) noexcept
                 { encode(output, item); });
    encodeVector(writer,
                 value.attackCompletions,
                 [](ObjectAIStableDigestWriter& output, const AIAttackOrderCompletion& item) noexcept
                 { encode(output, item); });
    encodeVector(
        writer, value.guardTacticalCommands,
        [](ObjectAIStableDigestWriter& output,
           const AIGuardTacticalCommand& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.guardInteractionCommands,
        [](ObjectAIStableDigestWriter& output,
           const AIGuardInteractionCommand& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.guardFeedback,
        [](ObjectAIStableDigestWriter& output,
           const AIGuardFeedback& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.opportunityAttackMoveQueryCommands,
        [](ObjectAIStableDigestWriter& output,
           const AIOpportunityAttackMoveQueryCommand& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.opportunityAttackMoveQueryFeedback,
        [](ObjectAIStableDigestWriter& output,
           const AIOpportunityAttackMoveQueryFeedback& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.opportunityAttackMoveChildCommands,
        [](ObjectAIStableDigestWriter& output,
           const AIOpportunityAttackMoveChildCommand& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.opportunityAttackMoveChildFeedback,
        [](ObjectAIStableDigestWriter& output,
           const AIOpportunityAttackMoveChildFeedback& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.tacticalAttackQueryCommands,
        [](ObjectAIStableDigestWriter& output,
           const AITacticalAttackQueryCommand& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.tacticalAttackQueryFeedback,
        [](ObjectAIStableDigestWriter& output,
           const AITacticalAttackQueryFeedback& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.tacticalAttackChildCommands,
        [](ObjectAIStableDigestWriter& output,
           const AITacticalAttackChildCommand& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.tacticalAttackChildFeedback,
        [](ObjectAIStableDigestWriter& output,
           const AITacticalAttackChildFeedback& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.dockRequests,
        [](ObjectAIStableDigestWriter& output,
           const AIDockRequest& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.dockFeedback,
        [](ObjectAIStableDigestWriter& output,
           const AIDockFeedback& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.containmentCommands,
        [](ObjectAIStableDigestWriter& output,
           const AIContainmentCommand& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.containmentFeedback,
        [](ObjectAIStableDigestWriter& output,
           const AIContainmentFeedback& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.insertionMotionCommands,
        [](ObjectAIStableDigestWriter& output,
           const AIInsertionMotionCommand& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.insertionMotionFeedback,
        [](ObjectAIStableDigestWriter& output,
           const AIInsertionMotionFeedback& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.insertionContainmentCommands,
        [](ObjectAIStableDigestWriter& output,
           const AIInsertionContainmentCommand& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.insertionContainmentFeedback,
        [](ObjectAIStableDigestWriter& output,
           const AIInsertionContainmentFeedback& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.insertionOperationCommands,
        [](ObjectAIStableDigestWriter& output,
           const AIInsertionOperationCommand& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.insertionOperationFeedback,
        [](ObjectAIStableDigestWriter& output,
           const AIInsertionOperationFeedback& item) noexcept
        { encode(output, item); });
    encodeVector(
        writer, value.insertionEffectCommands,
        [](ObjectAIStableDigestWriter& output,
           const AIInsertionEffectCommand& item) noexcept
        { encode(output, item); });
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const ObjectAIReadOnlyFact& value) noexcept
{
    encode(writer, value.subject);
    encode(writer, value.position);
    encode(writer, value.containedBy);
    encode(writer, value.nearestTunnel);
    encode(writer, value.priorityNemesis);
    encode(writer, value.lastAggressor);
    encode(writer, value.pickupCrate);
    encode(writer, value.pickupCratePosition);
    encode(writer, value.idleAutoAcquireTarget);
    encode(writer, value.closestRepulsor);
    writer.u64(value.orderRevision);
    writer.u64(value.weaponRevision);
    writer.u64(value.targetScanWakeRevision);
    writer.u64(value.capabilityMask);
    writer.u64(static_cast<uint64_t>(value.wanderAboutPointRadiusRaw));
    writer.u64(static_cast<uint64_t>(value.wanderWidthFactorRaw));
    writer.u32(value.disabledMask);
    writer.u32(value.idleTargetScanIntervalTicks);
    writer.u8(value.positionValid);
    writer.u8(value.effectivelyDead);
    writer.u8(value.mobile);
    writer.u8(value.hasCurrentLocomotor);
    writer.u8(value.groundMovement);
    writer.u8(value.projectile);
    writer.u8(value.jetAI);
    writer.u8(value.pickupCratePositionValid);
    writer.u8(value.attackExitConditionSatisfied);
    writer.u8(value.idleAutoAcquireEnabled);
    writer.u8(value.canBeRepulsed);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const ObjectAIReadOnlyInputSnapshot& value) noexcept
{
    writer.boolean(value.valid);
    writer.u64(value.confirmedTick);
    writer.u32(value.ticksPerSecond);
    writer.count(value.facts.size());
    for (const ObjectAIReadOnlyFact& fact : value.facts)
        encode(writer, fact);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const ObjectAIWaypointTeamProgressState& value) noexcept
{
    writer.u64(value.team.value);
    writer.u64(value.start.value);
    writer.u64(value.current.value);
    writer.u64(value.graphRevision);
    writer.u64(value.revision);
    writer.u64(value.issuedTick);
    writer.u32(value.sourceSequence);
    writer.u32(value.sourceScriptId);
}

inline void encode(ObjectAIStableDigestWriter& writer,
                   const AIStateSoATransitionRequest& value) noexcept
{
    writer.count(value.slot);
    encode(writer, value.subject);
    writer.enumeration(value.expectedState);
    writer.u64(value.expectedRevision);
    writer.enumeration(value.operation);
    writer.enumeration(value.target);
    writer.u64(value.temporaryDurationTicks);
    writer.u64(value.correlationIssuedTick);
    writer.enumeration(value.authority);
    writer.boolean(value.reenter);
    writer.boolean(value.terminalPriority);
}

inline void encode(ObjectAIStableDigestWriter& writer, const ObjectAIRuntimeSnapshot& value) noexcept
{
    // Domain and encoder versions prevent accidental reuse of this byte stream
    // as an unrelated FNV identity and make future encoding revisions explicit.
    writer.u64(0x4f424a4149444745ull); // "OBJAIDGE"
    writer.u32(ObjectAIStableDigest::EncodingVersion);
    writer.u32(value.schemaVersion);
    writer.count(value.config.maximumActors);
    writer.count(value.config.slotsPerBatch);
    writer.count(value.config.membershipEventCapacity);
    writer.count(value.config.transientValueCapacity);
    writer.u32(value.config.guardEnemyScanIntervalTicks);
    writer.u32(value.config.guardReturnScanIntervalTicks);
    writer.u32(value.config.guardChaseDurationTicks);
    writer.u32(value.config.idleTargetScanIntervalTicks);
    writer.u32(value.config.forceIdleBeforeAcquireTicks);
    writer.i64(value.config.skirmishGroupFudgeDistanceRaw);

    writer.count(value.batches.size());
    for (const AIStateSoASlotRegistrySnapshot& batch : value.batches)
        encode(writer, batch);

    writer.count(value.orderAdmissions.size());
    for (const ObjectAIOrderAdmissionSnapshot& admission : value.orderAdmissions)
        encode(writer, admission);

    writer.count(value.recipeBindings.size());
    for (const ObjectAIRecipeBindingSnapshot& binding : value.recipeBindings)
        encode(writer, binding);

    writer.count(value.pendingMembershipEvents.size());
    for (const AIObjectMembershipEvent& event : value.pendingMembershipEvents)
        encode(writer, event);
    writer.u32(static_cast<uint32_t>(value.membershipJournalStatus));
    writer.u64(value.membershipJournalTick);
    writer.boolean(value.membershipJournalHasTick);

    writer.count(value.waypointTeamProgress.size());
    for (const ObjectAIWaypointTeamProgressState& progress :
         value.waypointTeamProgress)
        encode(writer, progress);

    writer.count(value.pendingTransitionRequests.size());
    for (const auto& batch : value.pendingTransitionRequests)
    {
        writer.count(batch.size());
        for (const AIStateSoATransitionRequest& request : batch)
            encode(writer, request);
    }
    writer.count(value.pendingInsertionEntryFeedback.size());
    for (const AIInsertionMotionFeedback& feedback :
         value.pendingInsertionEntryFeedback)
        encode(writer, feedback);
    writer.count(value.pendingContainmentEntryFeedback.size());
    for (const AIContainmentFeedback& feedback :
         value.pendingContainmentEntryFeedback)
        encode(writer, feedback);

    encode(writer, value.transients);
    encode(writer, value.latestInput);
}

} // namespace detail

uint64_t test_support::stableDigest(
    const ObjectAIOrderAdmissionSnapshot& snapshot) noexcept
{
    detail::ObjectAIStableDigestWriter writer;
    detail::encode(writer, snapshot);
    return writer.finish();
}

uint64_t ObjectAIStableDigest::compute(const ObjectAIRuntimeSnapshot& snapshot) noexcept
{
    detail::ObjectAIStableDigestWriter writer;
    detail::encode(writer, snapshot);
    return writer.finish();
}

uint64_t stableDigest(const ObjectAIRuntimeSnapshot& snapshot) noexcept
{
    return ObjectAIStableDigest::compute(snapshot);
}

} // namespace engine::ai
