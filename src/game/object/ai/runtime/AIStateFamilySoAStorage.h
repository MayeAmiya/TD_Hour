#pragma once

#include <cstddef>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateData.h"
#include "game/object/ai/runtime/AIStateMachine.h"
#include "core/ecs/ObjectId.h"

namespace engine::ai
{

class AIFaceSoAColumns final
{
public:
    void reset(size_t count);

    [[nodiscard]] size_t size() const noexcept { return m_requestIssuedTick.size(); }

    void activate(size_t slot, AIStateRequestId request) noexcept;
    [[nodiscard]] AIFaceStatePayload load(size_t slot) const noexcept;
    void store(size_t slot, const AIFaceStatePayload& value) noexcept;

    [[nodiscard]] container::Span<const uint8_t> commandIssued() const noexcept { return m_commandIssued; }
    [[nodiscard]] container::Span<const uint8_t> canTurnInPlace() const noexcept { return m_canTurnInPlace; }

private:
    container::Vector<uint64_t> m_requestIssuedTick;
    container::Vector<uint32_t> m_requestSequence;
    container::Vector<uint8_t> m_commandIssued;
    container::Vector<uint8_t> m_canTurnInPlace;
};

class AIMoveToSoAColumns final
{
public:
    void reset(size_t count);

    [[nodiscard]] size_t size() const noexcept { return m_requestIssuedTick.size(); }

    void activate(size_t slot, AIStateRequestId request) noexcept;
    [[nodiscard]] AIMoveToStatePayload load(size_t slot) const noexcept;
    void store(size_t slot, const AIMoveToStatePayload& value) noexcept;

    [[nodiscard]] container::Span<const uint8_t> phases() const noexcept { return m_phase; }
    [[nodiscard]] container::Span<const uint8_t> pathRequestsIssued() const noexcept { return m_pathRequestIssued; }
    [[nodiscard]] container::Span<const uint64_t> sourceOrderRevisions() const noexcept
    {
        return m_sourceOrderRevision;
    }
    [[nodiscard]] container::Span<const uint32_t> generations() const noexcept { return m_generation; }

private:
    container::Vector<uint64_t> m_requestIssuedTick;
    container::Vector<uint32_t> m_requestSequence;
    container::Vector<int64_t> m_resolvedGoalX;
    container::Vector<int64_t> m_resolvedGoalY;
    container::Vector<int64_t> m_resolvedGoalZ;
    container::Vector<int64_t> m_adjustedGoalX;
    container::Vector<int64_t> m_adjustedGoalY;
    container::Vector<int64_t> m_adjustedGoalZ;
    container::Vector<uint64_t> m_path;
    container::Vector<uint64_t> m_sourceOrderRevision;
    container::Vector<uint32_t> m_generation;
    container::Vector<uint32_t> m_adjustedLayer;
    container::Vector<uint8_t> m_phase;
    container::Vector<uint8_t> m_pathRequestIssued;
    container::Vector<uint8_t> m_adjustDestinations;
};

// Field-level hot storage for FollowPath. Unlike the older transitional
// payload vectors, scanning one Follow field never strides over the rest of
// the state payload. load/store are slot-local lifecycle boundaries only.
class AIFollowPathSoAColumns final
{
public:
    void reset(size_t count);
    void activate(size_t slot, AIStateRequestId request) noexcept;
    [[nodiscard]] AIFollowPathStatePayload load(size_t slot) const noexcept;
    void store(size_t slot, const AIFollowPathStatePayload& value) noexcept;

    [[nodiscard]] container::Span<const uint32_t> indices() const noexcept
    {
        return m_index;
    }
    [[nodiscard]] container::Span<const uint8_t> retriesRemaining() const noexcept
    {
        return m_retriesRemaining;
    }

private:
    container::Vector<uint64_t> m_requestIssuedTick;
    container::Vector<uint32_t> m_requestSequence;
    container::Vector<uint64_t> m_sequence;
    container::Vector<uint64_t> m_path;
    container::Vector<ObjectId> m_ignoredObstacle;
    container::Vector<int64_t> m_segmentGoalX;
    container::Vector<int64_t> m_segmentGoalY;
    container::Vector<int64_t> m_segmentGoalZ;
    container::Vector<uint64_t> m_sequenceRevision;
    container::Vector<uint64_t> m_sourceOrderRevision;
    container::Vector<int64_t> m_extraDistanceRaw;
    container::Vector<uint32_t> m_index;
    container::Vector<uint32_t> m_generation;
    container::Vector<uint8_t> m_retriesRemaining;
    container::Vector<uint8_t> m_phase;
    container::Vector<uint8_t> m_pathRequestIssued;
    container::Vector<uint8_t> m_finalSegment;
    container::Vector<uint8_t> m_adjustDestinations;
    container::Vector<uint8_t> m_exitProduction;
    container::Vector<uint8_t> m_allowThroughUnits;
    container::Vector<uint8_t> m_preciseFinalZ;
};

class AIWaypointPathSoAColumns final
{
public:
    void reset(size_t count);
    void activate(size_t slot, AIStateRequestId request) noexcept;
    [[nodiscard]] size_t size() const noexcept { return m_requestTick.size(); }
    [[nodiscard]] AIWaypointPathStatePayload load(size_t slot) const noexcept;
    void store(size_t slot, const AIWaypointPathStatePayload& value) noexcept;

    [[nodiscard]] container::Span<const uint64_t> currentHandles() const noexcept { return m_current; }

private:
    container::Vector<uint64_t> m_requestTick; container::Vector<uint32_t> m_requestSequence;
    container::Vector<uint64_t> m_current; container::Vector<uint64_t> m_prior;
    container::Vector<uint64_t> m_completionTerminal; container::Vector<uint64_t> m_team;
    container::Vector<uint64_t> m_path;
    container::Vector<int64_t> m_goalX; container::Vector<int64_t> m_goalY; container::Vector<int64_t> m_goalZ;
    container::Vector<int64_t> m_offsetX; container::Vector<int64_t> m_offsetY; container::Vector<int64_t> m_offsetZ;
    container::Vector<uint64_t> m_graphRevision; container::Vector<uint64_t> m_sourceRevision;
    container::Vector<uint64_t> m_teamRevision; container::Vector<int64_t> m_extraDistance;
    container::Vector<uint32_t> m_generation; container::Vector<uint32_t> m_waypointHopLimit;
    container::Vector<uint8_t> m_phase;
    container::Vector<uint8_t> m_pathRequestIssued; container::Vector<uint8_t> m_moveAsTeam;
    container::Vector<uint8_t> m_exact; container::Vector<uint8_t> m_adjust;
    container::Vector<uint8_t> m_preciseFinalZ;
    container::Vector<uint8_t> m_awaitingTeamProgress; container::Vector<uint8_t> m_completionPending;
};

class AIMoveOutOfWaySoAColumns final
{
public:
    void reset(size_t count);
    void activate(size_t slot, AIStateRequestId request) noexcept;
    [[nodiscard]] AIMoveOutOfWayStatePayload load(size_t slot) const noexcept;
    void store(size_t slot, const AIMoveOutOfWayStatePayload& value) noexcept;
private:
    container::Vector<uint64_t> m_requestTick; container::Vector<uint32_t> m_requestSequence;
    container::Vector<uint64_t> m_path; container::Vector<int64_t> m_goalX;
    container::Vector<int64_t> m_goalY; container::Vector<int64_t> m_goalZ;
    container::Vector<uint64_t> m_sourceRevision; container::Vector<uint64_t> m_deadlineTick;
    container::Vector<uint32_t> m_generation; container::Vector<uint8_t> m_phase;
    container::Vector<uint8_t> m_pathRequestIssued; container::Vector<uint8_t> m_allowThrough;
};

class AIApproachPathSoAColumns final
{
public:
    void reset(size_t count);
    void activate(size_t slot, AIStateRequestId request) noexcept;
    [[nodiscard]] AIApproachPathStatePayload load(size_t slot) const noexcept;
    void store(size_t slot, const AIApproachPathStatePayload& value) noexcept;
private:
    container::Vector<uint64_t> m_requestTick; container::Vector<uint32_t> m_requestSequence;
    container::Vector<uint64_t> m_path; container::Vector<int64_t> m_goalX; container::Vector<int64_t> m_goalY;
    container::Vector<int64_t> m_goalZ; container::Vector<int64_t> m_originX; container::Vector<int64_t> m_originY;
    container::Vector<int64_t> m_originZ; container::Vector<ObjectId> m_repulsor; container::Vector<ObjectId> m_repulsor2; container::Vector<uint64_t> m_sourceRevision;
    container::Vector<uint64_t> m_nextScanTick;
    container::Vector<uint32_t> m_generation; container::Vector<uint8_t> m_repaths;
    container::Vector<uint8_t> m_phase; container::Vector<uint8_t> m_pathRequestIssued; container::Vector<uint8_t> m_adjust;
};

class AIMoveEvacuateStateSoAColumns final
{
public:
    void reset(size_t count);
    [[nodiscard]] size_t size() const noexcept { return m_originX.size(); }
    [[nodiscard]] AIFixedPosition origin(size_t slot) const noexcept
    {
        return {m_originX[slot], m_originY[slot], m_originZ[slot]};
    }
    void setOrigin(size_t slot, const AIFixedPosition& value) noexcept
    {
        m_originX[slot] = value.xRaw;
        m_originY[slot] = value.yRaw;
        m_originZ[slot] = value.zRaw;
    }
    [[nodiscard]] bool appendDeleteGoal(size_t slot) const noexcept
    {
        return m_appendDeleteGoal[slot] != 0;
    }
    void setAppendDeleteGoal(size_t slot, bool value) noexcept
    {
        m_appendDeleteGoal[slot] = value ? uint8_t{1} : uint8_t{0};
    }
private:
    container::Vector<int64_t> m_originX;
    container::Vector<int64_t> m_originY;
    container::Vector<int64_t> m_originZ;
    container::Vector<uint8_t> m_appendDeleteGoal;
};

class AIHackInternetStateSoAColumns final
{
public:
    void reset(size_t count);
    [[nodiscard]] size_t size() const noexcept { return m_requestTick.size(); }
    void activate(size_t slot, AIStateRequestId request) noexcept;
    [[nodiscard]] AIStateRequestId request(size_t slot) const noexcept
    { return {m_requestTick[slot],m_requestSequence[slot]}; }
    [[nodiscard]] uint64_t sourceRevision(size_t slot) const noexcept{return m_sourceRevision[slot];}
    void setSourceRevision(size_t slot,uint64_t value) noexcept{m_sourceRevision[slot]=value;}
    [[nodiscard]] AIBehaviorProfileHandle profile(size_t slot) const noexcept{return {m_profile[slot]};}
    void setProfile(size_t slot,AIBehaviorProfileHandle value,uint64_t revision) noexcept
    {m_profile[slot]=value.value;m_profileRevision[slot]=revision;}
    [[nodiscard]] uint64_t profileRevision(size_t slot) const noexcept{return m_profileRevision[slot];}
    [[nodiscard]] uint64_t phaseEndTick(size_t slot) const noexcept{return m_phaseEndTick[slot];}
    void setPhaseEndTick(size_t slot,uint64_t value) noexcept{m_phaseEndTick[slot]=value;}
    [[nodiscard]] uint64_t nextPayoutTick(size_t slot) const noexcept{return m_nextPayoutTick[slot];}
    void setNextPayoutTick(size_t slot,uint64_t value) noexcept{m_nextPayoutTick[slot]=value;}
    [[nodiscard]] uint64_t deferredOrderRevision(size_t slot) const noexcept{return m_deferredOrderRevision[slot];}
    void setDeferredOrderRevision(size_t slot,uint64_t value) noexcept{m_deferredOrderRevision[slot]=value;}
    [[nodiscard]] AIHackInternetPhase phase(size_t slot) const noexcept
    {return static_cast<AIHackInternetPhase>(m_phase[slot]);}
    void setPhase(size_t slot,AIHackInternetPhase value) noexcept{m_phase[slot]=static_cast<uint8_t>(value);}
private:
    container::Vector<uint64_t> m_requestTick; container::Vector<uint32_t> m_requestSequence;
    container::Vector<uint64_t> m_sourceRevision; container::Vector<uint64_t> m_profile;
    container::Vector<uint64_t> m_profileRevision; container::Vector<uint64_t> m_phaseEndTick;
    container::Vector<uint64_t> m_nextPayoutTick; container::Vector<uint64_t> m_deferredOrderRevision;
    container::Vector<uint8_t> m_phase;
};

class AIAttackStateSoAStorage final
{
public:
    void reset(size_t count);
    [[nodiscard]] size_t size() const noexcept{return m_requestTick.size();}
    void activate(size_t slot,AIStateRequestId request) noexcept;
    [[nodiscard]] AIAttackSoAColumns view() noexcept
    {
        return {.requestTick=m_requestTick,.requestSequence=m_requestSequence,.phase=m_phase,
                .phaseRevision=m_phaseRevision,.weaponRevision=m_weaponRevision,
                .sourceOrderRevision=m_sourceRevision,.pathGeneration=m_pathGeneration,.pathHandle=m_path,
                .trackedTarget=m_trackedTarget,.targetXRaw=m_targetX,.targetYRaw=m_targetY,.targetZRaw=m_targetZ,
                .arrivalRadiusRaw=m_arrivalRadius,.minimumArrivalRadiusRaw=m_minimumArrivalRadius,
                .pathRequestIssued=m_pathIssued,.movementActive=m_movementActive,
                .aimingActive=m_aimingActive,.firingActive=m_firingActive,.fireCommandIssued=m_fireIssued,
                .contactWeapon=m_contactWeapon};
    }
    [[nodiscard]] AIAttackStatePayload load(size_t slot) const noexcept;
    void store(size_t slot,const AIAttackStatePayload& v) noexcept;
private:
    container::Vector<uint64_t> m_requestTick;container::Vector<uint32_t> m_requestSequence;
    container::Vector<AIAttackPhase> m_phase;container::Vector<uint32_t> m_phaseRevision;
    container::Vector<uint64_t> m_weaponRevision;container::Vector<uint64_t> m_sourceRevision;
    container::Vector<uint32_t> m_pathGeneration;container::Vector<uint64_t> m_path;
    container::Vector<ObjectId> m_trackedTarget;container::Vector<int64_t> m_targetX,m_targetY,m_targetZ,m_arrivalRadius,m_minimumArrivalRadius;
    container::Vector<uint8_t> m_pathIssued,m_movementActive,m_aimingActive,m_firingActive,m_fireIssued,m_contactWeapon;
};

class AIDockStateSoAStorage final
{
public:
    void reset(size_t n);
    [[nodiscard]] size_t size()const noexcept{return m_tick.size();}
    void activate(size_t slot,AIStateRequestId request,AIDockPurpose purpose) noexcept;
    [[nodiscard]] AIDockStateSoAColumns view() noexcept
    {return {.tokenSubjects=m_subject,.tokenDocks=m_dock,.tokenIssuedTicks=m_tick,.tokenRequestSequences=m_sequence,
             .purposes=m_purpose,.phases=m_phase,.phaseRevisions=m_phaseRevision,.exchangeSequences=m_exchange,
             .pendingRequests=m_pending,.approachPositions=m_approach,.clearanceEnterTicks=m_clearanceTick,
             .nextActionTicks=m_nextAction,.actionDelayTicks=m_actionDelay,.drones=m_drone,.movementActive=m_movement};}
    [[nodiscard]] AIDockStatePayload load(size_t s)const noexcept;
    void store(size_t s,const AIDockStatePayload&p)noexcept;
private:
    container::Vector<ObjectId>m_subject,m_dock;container::Vector<uint64_t>m_tick;container::Vector<uint32_t>m_sequence;
    container::Vector<AIDockPurpose>m_purpose;container::Vector<AIDockPhase>m_phase;
    container::Vector<uint32_t>m_phaseRevision,m_exchange;container::Vector<AIDockRequestKind>m_pending;
    container::Vector<int32_t>m_approach;container::Vector<uint64_t>m_clearanceTick,m_nextAction;
    container::Vector<uint32_t>m_actionDelay;container::Vector<ObjectId>m_drone;container::Vector<uint8_t>m_movement;
};

class AIInsertionStateSoAStorage final
{
public:
 void reset(size_t n);
 [[nodiscard]]size_t size()const noexcept{return m_tick.size();}
 void activate(size_t s,AIStateRequestId request)noexcept;
 [[nodiscard]]AIInsertionStateSoAColumns view()noexcept{return{m_tick,m_sequence,m_target,m_building,m_destinationZ,m_speed,
  m_rappelPhase,m_operation,m_eventSequence,m_dropPhase,m_dropPath,m_dropSourceRevision,m_dropGeneration,
  m_dropOldPreferredHeight,m_dropPathIssued,m_dropApproachConfigured};}
 [[nodiscard]]AIInsertionStatePayload load(size_t s)const noexcept;
 void store(size_t s,const AIInsertionStatePayload&p)noexcept;
private:
 container::Vector<uint64_t>m_tick;container::Vector<uint32_t>m_sequence;container::Vector<ObjectId>m_target;
 container::Vector<uint8_t>m_building;container::Vector<int64_t>m_destinationZ,m_speed;
 container::Vector<AIRappelInsertionPhase>m_rappelPhase;container::Vector<AIInsertionOperationHandle>m_operation;
 container::Vector<uint32_t>m_eventSequence;container::Vector<AICombatDropInsertionPhase>m_dropPhase;
 container::Vector<PathHandle>m_dropPath;container::Vector<uint64_t>m_dropSourceRevision;
 container::Vector<uint32_t>m_dropGeneration;container::Vector<int64_t>m_dropOldPreferredHeight;
 container::Vector<uint8_t>m_dropPathIssued,m_dropApproachConfigured;
};

// Production-shape SoA candidate. Every object keeps one stable slot across
// all columns. Memory is intentionally traded for state-homogeneous access:
// an Idle batch never strides over Move payloads, and vice versa.
class AIStateFamilySoAStorage final
{
public:
    [[nodiscard]] bool reset(container::Span<const ObjectId> orderedSubjects);

    // Production owner initialization. Capacity is fixed for the session so
    // structural changes never resize every state-family column or invalidate
    // spans held by one confirmed-tick batch.
    [[nodiscard]] bool initializeCapacity(size_t capacity);

    // Slot-local membership operations. Object identity and slot allocation
    // policy remain owned by AIStateSoASlotRegistry; this layer guarantees that
    // a reused slot cannot retain runtime or payload metadata from its prior
    // subject.
    [[nodiscard]] bool bindSubject(size_t slot, ObjectId subject) noexcept;
    [[nodiscard]] bool releaseSubject(size_t slot) noexcept;

    [[nodiscard]] bool occupied(size_t slot) const noexcept;
    [[nodiscard]] size_t activeSubjectCount() const noexcept;

    [[nodiscard]] size_t size() const noexcept
    {
        return m_subjects.size();
    }
    [[nodiscard]] bool empty() const noexcept;

    void activate(size_t slot, AIStateId state, uint64_t confirmedTick) noexcept;

    // Snapshot/parity restore path. The caller restores the matching payload
    // column separately.
    [[nodiscard]] bool restorePayloadMetadata(size_t slot, AIStateId state, uint32_t activationSequence) noexcept;

    [[nodiscard]] container::Span<const ObjectId> subjects() const noexcept
    {
        return m_subjects;
    }
    [[nodiscard]] container::Span<AIStateMachineRuntime> runtimes() noexcept
    {
        return m_runtimes;
    }
    [[nodiscard]] container::Span<const AIStateMachineRuntime> runtimes() const noexcept
    {
        return m_runtimes;
    }
    [[nodiscard]] container::Span<AIStateParameters> parameters() noexcept
    {
        return m_parameters;
    }
    [[nodiscard]] container::Span<const AIStateParameters> parameters() const noexcept
    {
        return m_parameters;
    }
    [[nodiscard]] container::Span<const AIStateId> payloadStates() const noexcept
    {
        return m_payloadStates;
    }
    [[nodiscard]] container::Span<const uint32_t> activationSequences() const noexcept
    {
        return m_activationSequences;
    }
    [[nodiscard]] size_t activeStateCount(AIStateId state) const noexcept;
    [[nodiscard]] bool prepareExecutionSlots(
        container::Span<const uint8_t> scheduled) noexcept;
    [[nodiscard]] AIExecutionSlotRange executionSlots() const noexcept
    {
        switch (m_executionMode)
        {
        case AIExecutionSlotRange::Mode::Empty:
            return AIExecutionSlotRange::none();
        case AIExecutionSlotRange::Mode::Full:
            return AIExecutionSlotRange::full(size());
        case AIExecutionSlotRange::Mode::DirectSlots:
            return AIExecutionSlotRange::directSlots({
                m_directExecutionSlots.data(), m_executionCount});
        case AIExecutionSlotRange::Mode::Slots:
            return AIExecutionSlotRange::slots(m_executionSlots);
        case AIExecutionSlotRange::Mode::Blocks:
            return AIExecutionSlotRange::blocks(m_executionBlocks, m_executionCount);
        case AIExecutionSlotRange::Mode::FilteredDense:
            return AIExecutionSlotRange::filteredDense(
                m_executionFilter, m_subjects, m_executionCount);
        case AIExecutionSlotRange::Mode::Unspecified:
            break;
        }
        return AIExecutionSlotRange::none();
    }
    [[nodiscard]] container::Span<AIIdleStatePayload> idle() noexcept
    {
        return m_idle;
    }
    [[nodiscard]] container::Span<const AIIdleStatePayload> idle() const noexcept
    {
        return m_idle;
    }
    [[nodiscard]] container::Span<AIWaitStatePayload> wait() noexcept
    {
        return m_wait;
    }
    [[nodiscard]] container::Span<const AIWaitStatePayload> wait() const noexcept
    {
        return m_wait;
    }
    [[nodiscard]] container::Span<AIBusyStatePayload> busy() noexcept
    {
        return m_busy;
    }
    [[nodiscard]] container::Span<const AIBusyStatePayload> busy() const noexcept
    {
        return m_busy;
    }
    [[nodiscard]] container::Span<AIDeadStatePayload> dead() noexcept
    {
        return m_dead;
    }
    [[nodiscard]] container::Span<const AIDeadStatePayload> dead() const noexcept
    {
        return m_dead;
    }
    [[nodiscard]] AIFaceSoAColumns& face() noexcept
    {
        return m_face;
    }
    [[nodiscard]] const AIFaceSoAColumns& face() const noexcept
    {
        return m_face;
    }
    [[nodiscard]] AIMoveToSoAColumns& moveTo() noexcept
    {
        return m_moveTo;
    }
    [[nodiscard]] const AIMoveToSoAColumns& moveTo() const noexcept
    {
        return m_moveTo;
    }
    [[nodiscard]] container::Span<uint8_t> pickUpCrateDelayUpdates() noexcept
    {
        return m_pickUpCrateDelayUpdates;
    }
    [[nodiscard]] container::Span<const uint8_t> pickUpCrateDelayUpdates() const noexcept
    {
        return m_pickUpCrateDelayUpdates;
    }
    [[nodiscard]] container::Span<uint64_t> containmentRequestTick() noexcept { return m_containmentRequestTick; }
    [[nodiscard]] container::Span<uint32_t> containmentRequestSequence() noexcept { return m_containmentRequestSequence; }
    [[nodiscard]] container::Span<ObjectId> containmentTrackedGoal() noexcept { return m_containmentTrackedGoal; }
    [[nodiscard]] container::Span<ObjectId> containmentEntryToClear() noexcept { return m_containmentEntryToClear; }
    [[nodiscard]] container::Span<AIContainmentPhase> containmentPhase() noexcept { return m_containmentPhase; }
    [[nodiscard]] container::Span<const uint64_t> containmentRequestTick() const noexcept
    { return m_containmentRequestTick; }
    [[nodiscard]] container::Span<const uint32_t> containmentRequestSequence() const noexcept
    { return m_containmentRequestSequence; }
    [[nodiscard]] container::Span<const ObjectId> containmentTrackedGoal() const noexcept
    { return m_containmentTrackedGoal; }
    [[nodiscard]] container::Span<const ObjectId> containmentEntryToClear() const noexcept
    { return m_containmentEntryToClear; }
    [[nodiscard]] container::Span<const AIContainmentPhase> containmentPhase() const noexcept
    { return m_containmentPhase; }
    [[nodiscard]] AIFollowPathSoAColumns& followPath() noexcept
    {
        return m_followPath;
    }
    [[nodiscard]] const AIFollowPathSoAColumns& followPath() const noexcept
    {
        return m_followPath;
    }
    [[nodiscard]] AIWaypointPathSoAColumns& waypointPath() noexcept { return m_waypointPath; }
    [[nodiscard]] const AIWaypointPathSoAColumns& waypointPath() const noexcept { return m_waypointPath; }
    [[nodiscard]] AIMoveOutOfWaySoAColumns& moveOutOfWay() noexcept { return m_moveOutOfWay; }
    [[nodiscard]] const AIMoveOutOfWaySoAColumns& moveOutOfWay() const noexcept { return m_moveOutOfWay; }
    [[nodiscard]] AIApproachPathSoAColumns& approachPath() noexcept { return m_approachPath; }
    [[nodiscard]] const AIApproachPathSoAColumns& approachPath() const noexcept { return m_approachPath; }
    [[nodiscard]] AIMoveEvacuateStateSoAColumns& moveEvacuate() noexcept { return m_moveEvacuate; }
    [[nodiscard]] const AIMoveEvacuateStateSoAColumns& moveEvacuate() const noexcept { return m_moveEvacuate; }
    [[nodiscard]] AIHackInternetStateSoAColumns& hackInternet() noexcept{return m_hackInternet;}
    [[nodiscard]] const AIHackInternetStateSoAColumns& hackInternet() const noexcept{return m_hackInternet;}
    [[nodiscard]] AIAttackStateSoAStorage& attack() noexcept{return m_attack;}
    [[nodiscard]] const AIAttackStateSoAStorage& attack() const noexcept{return m_attack;}
    [[nodiscard]] AIDockStateSoAStorage& dock() noexcept{return m_dock;}
    [[nodiscard]] const AIDockStateSoAStorage& dock() const noexcept{return m_dock;}
    [[nodiscard]] AIInsertionStateSoAStorage& insertion() noexcept{return m_insertion;}
    [[nodiscard]] const AIInsertionStateSoAStorage& insertion() const noexcept{return m_insertion;}
    [[nodiscard]] AIGuardSoAColumns& guard() noexcept{return m_guard;}
    [[nodiscard]] const AIGuardSoAColumns& guard() const noexcept{return m_guard;}
    [[nodiscard]] AITacticalAttackSoAColumns& tacticalAttack() noexcept{return m_tacticalAttack;}
    [[nodiscard]] const AITacticalAttackSoAColumns& tacticalAttack() const noexcept{return m_tacticalAttack;}
    [[nodiscard]] AIOpportunityAttackMoveSoAColumns& opportunityAttackMove() noexcept
    { return m_opportunityAttackMove; }
    [[nodiscard]] const AIOpportunityAttackMoveSoAColumns& opportunityAttackMove() const noexcept
    { return m_opportunityAttackMove; }

private:
    void resetColumns(size_t count);
    void clearSlotState(size_t slot) noexcept;

    container::Vector<ObjectId> m_subjects;
    container::Vector<AIStateMachineRuntime> m_runtimes;
    container::Vector<AIStateParameters> m_parameters;
    container::Vector<AIStateId> m_payloadStates;
    container::Vector<uint32_t> m_activationSequences;
    container::Vector<AIIdleStatePayload> m_idle;
    container::Vector<AIWaitStatePayload> m_wait;
    container::Vector<AIBusyStatePayload> m_busy;
    container::Vector<AIDeadStatePayload> m_dead;
    AIFaceSoAColumns m_face;
    AIMoveToSoAColumns m_moveTo;
    container::Vector<uint8_t> m_pickUpCrateDelayUpdates;
    container::Vector<uint64_t> m_containmentRequestTick;
    container::Vector<uint32_t> m_containmentRequestSequence;
    container::Vector<ObjectId> m_containmentTrackedGoal;
    container::Vector<ObjectId> m_containmentEntryToClear;
    container::Vector<AIContainmentPhase> m_containmentPhase;
    AIFollowPathSoAColumns m_followPath;
    AIWaypointPathSoAColumns m_waypointPath;
    AIMoveOutOfWaySoAColumns m_moveOutOfWay;
    AIApproachPathSoAColumns m_approachPath;
    AIMoveEvacuateStateSoAColumns m_moveEvacuate;
    AIHackInternetStateSoAColumns m_hackInternet;
    AIAttackStateSoAStorage m_attack;
    AIDockStateSoAStorage m_dock;
    AIInsertionStateSoAStorage m_insertion;
    AIGuardSoAColumns m_guard;
    AITacticalAttackSoAColumns m_tacticalAttack;
    AIOpportunityAttackMoveSoAColumns m_opportunityAttackMove;
    container::Array<size_t, static_cast<size_t>(AIStateId::Count)> m_activeStateCounts{};
    static constexpr size_t DirectExecutionSlotCapacity = 16;
    container::Array<size_t, DirectExecutionSlotCapacity> m_directExecutionSlots{};
    container::Vector<size_t> m_executionSlots;
    container::Vector<AIExecutionSlotRange::Block> m_executionBlocks;
    container::Span<const uint8_t> m_executionFilter;
    AIExecutionSlotRange::Mode m_executionMode = AIExecutionSlotRange::Mode::Empty;
    size_t m_executionCount = 0;
    size_t m_activeSubjectCount = 0;
};

} // namespace engine::ai
