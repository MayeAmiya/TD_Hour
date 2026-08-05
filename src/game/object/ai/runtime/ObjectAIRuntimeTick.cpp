#include "game/object/ai/runtime/ObjectAIRuntime.h"

#include "core/platform/runtime_threads.h"

#include <taskflow/algorithm/for_each.hpp>

namespace engine::ai
{

bool ObjectAIShadowTickReport::succeeded() const noexcept
{
    return status == ObjectAIShadowTickStatus::Success;
}

ObjectAIShadowTickReport ObjectAIRuntime::runShadow(uint64_t confirmedTick,
                                                    uint32_t ticksPerSecond,
                                                    container::Span<const ObjectAIReadOnlyFact> facts)
{
    ObjectAIShadowTickReport report;
    report.confirmedTick = confirmedTick;
    report.factsRead = facts.size();
    if (!m_initialized)
    {
        report.status = ObjectAIShadowTickStatus::NotInitialized;
        return remember(report);
    }
    if (m_latestInput.valid && confirmedTick <= m_latestInput.confirmedTick)
    {
        report.status = ObjectAIShadowTickStatus::NonMonotonicTick;
        return remember(report);
    }
    if (m_recipeBindings.size() != m_subjects.size())
    {
        report.status = ObjectAIShadowTickStatus::RecipeBindingMismatch;
        return remember(report);
    }
    if (facts.size() != m_subjects.size())
    {
        report.status = ObjectAIShadowTickStatus::FactCountMismatch;
        return remember(report);
    }
    for (size_t index = 0; index < facts.size(); ++index)
    {
        const ObjectAIRecipeBindingSnapshot& binding =
            m_recipeBindings[index];
        const bool recipeMismatch =
            binding.subject != m_subjects[index].subject ||
            binding.state == ObjectAIRecipeBindingState::Unbound ||
            (binding.state == ObjectAIRecipeBindingState::Bound &&
             !aiRecipeOwnerRouteFor(binding.recipe)) ||
            (binding.state ==
                 ObjectAIRecipeBindingState::ContentUnavailable &&
             (binding.recipe != AIRecipeId::Invalid ||
              facts[index].effectivelyDead == 0));
        if (!facts[index].subject || facts[index].subject != m_subjects[index].subject ||
            recipeMismatch ||
            (index != 0 && !(facts[index - 1].subject < facts[index].subject)))
        {
            report.status = recipeMismatch
                ? ObjectAIShadowTickStatus::RecipeBindingMismatch
                : ObjectAIShadowTickStatus::FactOrderMismatch;
            return remember(report);
        }
    }
    if (ticksPerSecond == 0)
        ticksPerSecond = 1;

    captureActiveAttackCompletionCandidates(confirmedTick);

    for (size_t batchIndex = 0; batchIndex < m_batches.size(); ++batchIndex)
    {
        AIStateSoASlotRegistry& batch = m_batches[batchIndex];
        ObjectAIShadowBatch& shadow = m_shadowBatches[batchIndex];
        if (!shadow.alignedWith(batch.storage()))
        {
            report.status = ObjectAIShadowTickStatus::BatchRejected;
            return remember(report);
        }
        // External order admission may have staged a transition before
        // this tick starts. Clear old kernel values while preserving that
        // bounded request until the multiwave executor consumes it.
        shadow.clearTransientOutputs(false);
        shadow.clearFeedback();
        ObjectAIShadowBatchColumns columns = shadow.columns();
        std::fill(columns.scheduled.begin(), columns.scheduled.end(), uint8_t{0});
    }
    scatterFeedback(report);
    for (const AIInsertionMotionFeedback& feedback : m_pendingInsertionEntryFeedback)
    {
        const std::optional<AIActorHandle> actor = find(feedback.correlation.subject);
        if (!actor || actor->batch >= m_shadowBatches.size())
        {
            ++report.feedbackRejected;
            continue;
        }
        AIInsertionMotionFeedbackBuffer& inbox =
            m_shadowBatches[actor->batch].columns().insertionMotionFeedback[actor->slot];
        if (!inbox.push(feedback))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AIContainmentFeedback& feedback :
         m_pendingContainmentEntryFeedback)
    {
        const std::optional<AIActorHandle> actor =
            find(feedback.correlation.subject);
        if (!actor || actor->batch >= m_shadowBatches.size())
        {
            ++report.feedbackRejected;
            continue;
        }
        AIContainmentFeedbackBuffer& inbox =
            m_shadowBatches[actor->batch].columns()
                .containmentFeedback[actor->slot];
        if (!inbox.push(feedback))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    if (report.feedbackOverflows != 0)
    {
        report.status = ObjectAIShadowTickStatus::BatchRejected;
        return remember(report);
    }

    // Both ranges are ObjectId sorted. A two-cursor merge detects the
    // first fact for a new actor and every semantic input change without
    // an ObjectId hash table or per-tick allocation.
    size_t oldCursor = 0;
    for (const ObjectAIReadOnlyFact& fact : facts)
    {
        while (oldCursor < m_latestInput.facts.size() && m_latestInput.facts[oldCursor].subject < fact.subject)
            ++oldCursor;
        const ObjectAIReadOnlyFact* previous =
            m_latestInput.valid && oldCursor < m_latestInput.facts.size() &&
                    m_latestInput.facts[oldCursor].subject == fact.subject
                ? &m_latestInput.facts[oldCursor]
                : nullptr;
        const bool changed = !previous || !(*previous == fact);
        if (changed)
            ++report.factsChanged;

        const std::optional<AIActorHandle> handle = find(fact.subject);
        if (!handle || handle->batch >= m_shadowBatches.size())
        {
            report.status = ObjectAIShadowTickStatus::FactOrderMismatch;
            return remember(report);
        }
        AIStateSoASlotRegistry& batch = m_batches[handle->batch];
        ObjectAIShadowBatchColumns columns = m_shadowBatches[handle->batch].columns();
        const size_t slot = handle->slot;
        AIStateMachineRuntime& runtime = batch.storage().runtimes()[slot];
        AIStateParameters& parameters = batch.storage().parameters()[slot];
        const bool targetScanWakeRequested =
            fact.targetScanWakeRevision != 0 &&
            (!previous || previous->targetScanWakeRevision !=
                              fact.targetScanWakeRevision);
        if (targetScanWakeRequested &&
            runtime.currentState == AIStateId::Idle &&
            batch.storage().payloadStates()[slot] == AIStateId::Idle) {
            batch.storage().idle()[slot].nextTargetScanTick = confirmedTick;
        }
        if (changed && AIStateMachine::isSleeping(runtime, confirmedTick))
            AIStateMachine::wake(runtime, AIWakeReason::ExternalCommand);
        const bool due = !AIStateMachine::isSleeping(runtime, confirmedTick);

        columns.scheduled[slot] = due ? uint8_t{1} : uint8_t{0};
        columns.effectivelyDead[slot] = fact.effectivelyDead;
        columns.mobile[slot] = fact.mobile;
        columns.groundMovement[slot] = fact.groundMovement;
        columns.projectile[slot] = fact.projectile;
        columns.jetAI[slot] = fact.jetAI;
        // Repulsor reactor half. RefCode tests KINDOF_CAN_BE_REPULSED before
        // it ever calls AI::findClosestRepulsor(), so a non-repulsable actor
        // must observe an invalid repulsor even if the session had one to
        // offer. Wander/Panic (AIStates.cpp:4594, 4833) and WanderInPlace
        // (AIStates.cpp:4719) fail out of their state on a live repulsor; the
        // resulting MoveAwayFromRepulsors state paths away from it.
        columns.canBeRepulsed[slot] = fact.canBeRepulsed;
        columns.closestRepulsors[slot] =
            fact.canBeRepulsed != 0 ? fact.closestRepulsor : INVALID_OBJECT_ID;
        // Wander goals are legacy RNG draws: GameLogicRandomValue(-delta,
        // delta) for X then Y, scaled by PATHFIND_CELL_SIZE. Navigation owns
        // the projected cell size per slot, so the conversion and the
        // deterministic draw both belong here rather than in the kernels,
        // which the AIRepulsorStateSoAKernelInput contract already states.
        const int64_t wanderCellSizeRaw = columns.pathfindCellSizeRaw[slot];
        columns.wanderCellSizeRaw[slot] = wanderCellSizeRaw;
        const int32_t wanderRadiusCells = wanderCellsFromRadius(
            fact.wanderAboutPointRadiusRaw, wanderCellSizeRaw,
            fact.groundMovement != 0);
        columns.wanderOffsetXCells[slot] = wanderOffsetDraw(
            fact.subject, parameters.sourceOrderRevision, confirmedTick, 0u,
            wanderRadiusCells);
        columns.wanderOffsetYCells[slot] = wanderOffsetDraw(
            fact.subject, parameters.sourceOrderRevision, confirmedTick, 1u,
            wanderRadiusCells);
        const int32_t wanderWidthCells =
            wanderCellsFromWidthFactor(fact.wanderWidthFactorRaw);
        columns.wanderOffsets[slot] = {
            .xRaw = static_cast<int64_t>(wanderOffsetDraw(
                        fact.subject, parameters.sourceOrderRevision,
                        confirmedTick, 2u, wanderWidthCells)) *
                    wanderCellSizeRaw,
            .yRaw = static_cast<int64_t>(wanderOffsetDraw(
                        fact.subject, parameters.sourceOrderRevision,
                        confirmedTick, 3u, wanderWidthCells)) *
                    wanderCellSizeRaw,
            .zRaw = 0,
        };
        columns.groupOffsets[slot] = parameters.waypointGroupOffset;
        columns.teamProgressTeam[slot] = {};
        columns.teamProgressCurrent[slot] = {};
        columns.teamProgressRevision[slot] = 0;
        columns.canTurnInPlace[slot] = hasCapability(fact, ObjectAICapability::CanTurnInPlace);
        columns.faceTargetValid[slot] = batch.storage().parameters()[slot].goalObject ? uint8_t{1} : uint8_t{0};
        columns.hasCurrentLocomotor[slot] = fact.hasCurrentLocomotor;
        const bool idleAutoAcquireOwner =
            runtime.currentState == AIStateId::Idle &&
            parameters.sourceOrderRevision == 0;
        if (idleAutoAcquireOwner) {
            parameters.goalObject = fact.idleAutoAcquireTarget;
            parameters.hasGoalPosition = false;
        }
        columns.idleAutoAcquireEnabled[slot] =
            idleAutoAcquireOwner && fact.idleAutoAcquireEnabled
            ? uint8_t{1} : uint8_t{0};
        columns.idleTargetAvailable[slot] =
            idleAutoAcquireOwner && fact.idleAutoAcquireTarget
            ? uint8_t{1} : uint8_t{0};
        columns.idleTargetScanIntervalTicks[slot] =
            std::max<uint32_t>(1u, fact.idleTargetScanIntervalTicks);
        if (parameters.waypointTeam)
        {
            const size_t progressIndex = waypointTeamProgressIndex(parameters.waypointTeam);
            if (progressIndex != NoWaypointTeamProgress)
            {
                const ObjectAIWaypointTeamProgressState& progress = m_waypointTeamProgress[progressIndex];
                columns.teamProgressTeam[slot] = progress.team;
                columns.teamProgressCurrent[slot] = progress.current;
                columns.teamProgressRevision[slot] = progress.revision;
            }
        }
        columns.subjectPositions[slot] = fact.position;
        // resolvedMoveTargets/moveTargetValid answer "where is
        // parameters.goalObject" for the Move, MoveEvacuate and PickUpCrate
        // kernels (RefCode AIInternalMoveToObjectState, which drives its path
        // from the goal object rather than from the mover). Projecting the
        // subject's own position here collapsed every move-to-object onto the
        // mover, and moveTargetValid was never written at all, so those
        // kernels failed outright whenever an order carried a goalObject.
        // Order translation has already resolved that object at the ECS -> AI
        // boundary: GameSessionAIOrders.cpp reads the target's authoritative
        // position into parameters.goalPosition and refuses to admit the order
        // when the target has no transform, so the admitted goalPosition *is*
        // the object resolution and hasGoalPosition is its validity flag. The
        // guard move child supplies exactly the same pair through its own
        // scratch spans (projectGuardMoveChild), so both paths agree bit for
        // bit. Keeping fact.position for the unresolved case leaves the column
        // at the value it always had for positional-only orders, which never
        // read it.
        const bool moveTargetResolved =
            parameters.goalObject && parameters.hasGoalPosition;
        columns.moveTargetValid[slot] =
            moveTargetResolved ? uint8_t{1} : uint8_t{0};
        columns.resolvedMoveTargets[slot] =
            moveTargetResolved ? parameters.goalPosition : fact.position;
        columns.currentAnchors[slot] = parameters.hasGuardAnchor ? parameters.guardAnchor : fact.position;
        columns.initialNemesis[slot] = parameters.guardRetaliateAggressor;
        // RefCode's guard machines read the aggressor from the Body module's
        // last DamageInfo (BodyModule::getClearableLastAttacker() driving
        // hasAttackedMeAndICanReturnFire(), and
        // AIGuard*AttackAggressorState::onEnter()), not only from an explicit
        // GuardRetaliate order. Prefer that recent-damage projection so plain
        // Guard/GuardTunnelNetwork counter-attack whatever is shooting them,
        // and keep the explicit order aggressor as the fallback so an admitted
        // GuardRetaliate still has a subject when no fresh damage exists.
        columns.aggressors[slot] =
            fact.lastAggressor ? fact.lastAggressor : parameters.guardRetaliateAggressor;
        columns.ticksPerSecond[slot] = ticksPerSecond;
        columns.disabled[slot] = fact.disabledMask != 0 ? uint8_t{1} : uint8_t{0};
        columns.contained[slot] = fact.containedBy ? uint8_t{1} : uint8_t{0};
        columns.nearestTunnels[slot] = fact.nearestTunnel;
        columns.priorityNemesis[slot] = fact.priorityNemesis;
        columns.crates[slot] = fact.pickupCrate;
        columns.cratePositions[slot] = fact.pickupCratePosition;
        columns.cratePositionValid[slot] =
            fact.pickupCratePositionValid;
        uint64_t attackSourceRevision = fact.orderRevision;
        if (idleAutoAcquireOwner && fact.idleAutoAcquireTarget &&
            attackSourceRevision == 0)
        {
            // Idle auto-acquisition is an internal AI action and therefore
            // has no ECS ObjectOrderIntent to provide a queue revision.  The
            // Attack kernels still require a non-zero stable value for path
            // and movement correlation.  Mint it from the transition that is
            // about to leave Idle, exactly as the internal repulsor escape
            // path does, without pretending an external command exists.
            const uint64_t internalRevision = runtime.revision ==
                    std::numeric_limits<uint64_t>::max()
                ? std::numeric_limits<uint64_t>::max()
                : runtime.revision + 1u;
            attackSourceRevision = std::max<uint64_t>(1u, internalRevision);
        }
        columns.sourceOrderRevisions[slot] = attackSourceRevision;
        columns.weaponRevisions[slot] = fact.weaponRevision;
        columns.attackGoalObjects[slot] = parameters.goalObject;
        columns.attackGoalPositions[slot] = parameters.goalPosition;
        columns.attackGoalPositionValid[slot] = parameters.hasGoalPosition ? uint8_t{1} : uint8_t{0};
        // Containment/Dock/Insertion goals come from the same admitted
        // AIStateParameters as the attack goal above. Each family's kernel
        // only reads its own column while its own state is active, so the
        // projection is unconditional exactly like the attack columns.
        columns.containmentGoalObjects[slot] = parameters.goalObject;
        columns.dockGoalObjects[slot] = parameters.goalObject;
        columns.insertionGoalObjects[slot] = parameters.goalObject;
        columns.insertionGoalPositions[slot] = parameters.goalPosition;
        columns.insertionGoalPositionValid[slot] =
            parameters.hasGoalPosition ? uint8_t{1} : uint8_t{0};
        columns.constructionComplete[slot] = hasCapability(fact, ObjectAICapability::ConstructionComplete);
        columns.hasAmmo[slot] = hasCapability(fact, ObjectAICapability::HasAmmo);
        columns.allWeaponsOutOfAmmo[slot] = columns.hasAmmo[slot] == 0 ? uint8_t{1} : uint8_t{0};
        columns.allArmyHunt[slot] = parameters.allArmyHunt ? uint8_t{1} : uint8_t{0};
        columns.useTeamCommonTarget[slot] = parameters.useTeamCommonTarget ? uint8_t{1} : uint8_t{0};
        columns.targetCollections[slot] = parameters.tacticalTargetCollection;
        columns.targetCollectionRevisions[slot] = parameters.tacticalTargetCollectionRevision;
        columns.attackAreas[slot] = parameters.tacticalAttackArea;
        columns.attackAreaRevisions[slot] = parameters.tacticalAttackAreaRevision;
        columns.squadSelections[slot] = parameters.tacticalSquadSelection;
        columns.enterGuard[slot] = parameters.enterGuardTargets ? uint8_t{1} : uint8_t{0};
        columns.guardWithoutPursuit[slot] = parameters.guardWithoutPursuit ? uint8_t{1} : uint8_t{0};
        columns.flyingOnly[slot] = parameters.guardFlyingOnly ? uint8_t{1} : uint8_t{0};
        columns.tracksAnchor[slot] = parameters.guardTracksAnchor ? uint8_t{1} : uint8_t{0};
        columns.guardRangeRaw[slot] = parameters.guardRangeRaw;
        columns.visionRangeRaw[slot] = parameters.guardVisionRangeRaw;
        // Guard/TacticalAttack enter kernels offset their first scan by this
        // value so a page of actors does not scan on one tick. It is a pure
        // projection of confirmed identity, never an RNG draw.
        columns.initialScanJitter[slot] =
            scanJitterDraw(fact.subject, parameters.sourceOrderRevision, confirmedTick);
        columns.attackMoodAllowed[slot] = hasCapability(fact, ObjectAICapability::AttackMoodAllowed);
        columns.attackExitConditionSatisfied[slot] = fact.attackExitConditionSatisfied;
        if (runtime.currentState == AIStateId::FollowWaypointPathAsIndividuals ||
            runtime.currentState == AIStateId::FollowWaypointPathAsTeam ||
            runtime.currentState == AIStateId::FollowWaypointPathAsIndividualsExact ||
            runtime.currentState == AIStateId::FollowWaypointPathAsTeamExact ||
            runtime.currentState == AIStateId::AttackFollowWaypointPathAsIndividuals ||
            runtime.currentState == AIStateId::AttackFollowWaypointPathAsTeam)
        {
            const AIWaypointPathStatePayload waypoint = batch.storage().waypointPath().load(slot);
            columns.branchChoice[slot] = waypointBranchChoice(fact.subject,
                                                              parameters,
                                                              waypoint.current ? waypoint.current : parameters.waypoint,
                                                              waypoint.generation);
        }
        if (due)
            ++report.actorsScheduled;
    }

    m_runnableBatchIndices.clear();
    for (size_t batchIndex = 0; batchIndex < m_batches.size(); ++batchIndex) {
        if (m_batches[batchIndex].activeCount() != 0 &&
            m_shadowBatches[batchIndex].hasRunnableWork()) {
            m_runnableBatchIndices.push_back(batchIndex);
        }
    }

    const auto runBatch = [this, confirmedTick](size_t batchIndex) noexcept {
        AIStateSoASlotRegistry& batch = m_batches[batchIndex];
        ObjectAIShadowBatch& shadow = m_shadowBatches[batchIndex];
        m_parallelBatchReports[batchIndex] = runAIStateSoAMultiwave(
            batch.storage(), shadow.input(batch.storage(), confirmedTick),
            shadow.scratch());
    };
    if (m_runnableBatchIndices.size() == 1 ||
        platform::runtime::simulationWorkerExecutor().num_workers() < 2) {
        for (const size_t batchIndex : m_runnableBatchIndices)
            runBatch(batchIndex);
    } else if (!m_runnableBatchIndices.empty()) {
        tf::Taskflow taskflow;
        taskflow.for_each_index(
            size_t{0}, m_runnableBatchIndices.size(), size_t{1},
            [this, &runBatch](size_t lane) {
                platform::runtime::ThreadRoleScope role(
                    platform::runtime::ThreadRole::SimulationWorker);
                runBatch(m_runnableBatchIndices[lane]);
            });
        platform::runtime::simulationWorkerExecutor().run(taskflow).wait();
    }

    // Worker completion order is intentionally ignored. Runtime/output
    // owners merge in canonical batch order exactly as the serial path did.
    for (const size_t batchIndex : m_runnableBatchIndices) {
        ++report.batchesRun;
        merge(report, m_parallelBatchReports[batchIndex]);
        if (!report.succeeded())
            return remember(report);
    }

    // The live transient store can contain retained path requests and
    // service feedback from earlier owners. Reuse a second store as the
    // transaction candidate: all batch outputs, terminal completions and the
    // wake projection must succeed before one swap publishes them together.
    m_stagedTransients = m_transients;
    std::swap(m_transients, m_stagedTransients);
    for (size_t batchIndex = 0; batchIndex < m_batches.size(); ++batchIndex)
    {
        if (m_batches[batchIndex].activeCount() == 0)
            continue;
        collectAndAuditOutputs(m_shadowBatches[batchIndex], report);
    }
    releaseStaleFacingCommands();
    advanceWaypointTeamProgress(facts);
    stageTerminalAttackCompletions(report);
    // A stale or unsupported transient output belongs to one actor/order and
    // is already rejected by its collector.  Do not roll back the complete
    // shadow transaction here: doing so discards valid movement/path outputs
    // from the same tick and leaves every AI-owned mover parked behind its
    // queue head.  Capacity overflow is different because it can truncate a
    // producer buffer and therefore still rejects the batch atomically.
    if (report.outputOverflows != 0)
    {
        std::swap(m_transients, m_stagedTransients);
        report.status = ObjectAIShadowTickStatus::BatchRejected;
        return remember(report);
    }

    m_wakeScratch.clear();
    for (const AIStateSoASubjectSlot& actor : m_subjects)
    {
        const AIStateFamilySoAStorage* actorStorage = storage(actor.handle);
        if (!actorStorage)
            continue;
        const uint64_t wakeTick = actorStorage->runtimes()[actor.handle.slot].wakeTick;
        if (wakeTick != 0)
            m_wakeScratch.push_back({actor.subject, wakeTick});
    }
    std::sort(
        m_wakeScratch.begin(),
        m_wakeScratch.end(),
        [](const AIWakeEvent& left, const AIWakeEvent& right)
        { return left.wakeTick != right.wakeTick ? left.wakeTick < right.wakeTick : left.object < right.object; });
    if (m_transients.replaceWakeEvents(m_wakeScratch) != ObjectAITransientStatus::Success)
    {
        std::swap(m_transients, m_stagedTransients);
        report.status = ObjectAIShadowTickStatus::WakeProjectionRejected;
        return remember(report);
    }

    // Handle release and feedback consumption are irreversible ownership
    // transfers. Perform them only after the complete candidate is known to
    // be publishable.
    releaseUnclaimedPathFeedback();
    m_transients.discardFeedback();
    m_pendingInsertionEntryFeedback.clear();
    m_pendingContainmentEntryFeedback.clear();

    m_latestInput.confirmedTick = confirmedTick;
    m_latestInput.ticksPerSecond = ticksPerSecond;
    m_latestInput.facts.assign(facts.begin(), facts.end());
    m_latestInput.valid = true;
    return remember(report);
}

ObjectAIShadowBatchConfig ObjectAIRuntime::shadowBatchConfig() const noexcept
{
    ObjectAIShadowBatchConfig config;
    config.guardEnemyScanIntervalTicks = m_config.guardEnemyScanIntervalTicks;
    config.guardReturnScanIntervalTicks = m_config.guardReturnScanIntervalTicks;
    config.guardChaseDurationTicks = m_config.guardChaseDurationTicks;
    config.idleTargetScanIntervalTicks = m_config.idleTargetScanIntervalTicks;
    config.forceIdleBeforeAcquireTicks =
        m_config.forceIdleBeforeAcquireTicks;
    return config;
}

void ObjectAIRuntime::captureActiveAttackCompletionCandidates(uint64_t confirmedTick)
{
    m_attackCompletionCandidates.clear();
    for (const AIStateSoASubjectSlot& actor : m_subjects)
    {
        const AIStateFamilySoAStorage* actorStorage = storage(actor.handle);
        if (!actorStorage)
            continue;
        const AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor.handle.slot];
        if (!attackPolicyFor(runtime.currentState).valid ||
            actorStorage->payloadStates()[actor.handle.slot] != runtime.currentState)
            continue;
        ObjectAIOrderAdmissionRequest active;
        const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor.handle.batch];
        if (admission.activeOrder(admission.handle(actor.handle.slot), active) !=
                ObjectAIOrderAdmissionStatus::Success ||
            active.kind != ObjectAIOrderKind::Attack)
            continue;
        const AIAttackStatePayload payload = actorStorage->attack().load(actor.handle.slot);
        AIAttackOrderCompletion candidate{
            .correlation =
                {
                    .subject = actor.subject,
                    .stateRequest = payload.request,
                    .state = runtime.currentState,
                    .phase = payload.phase,
                    .weaponRevision = payload.weaponRevision,
                    .phaseRevision = payload.phaseRevision,
                    .orderIdentity = toAIAsyncOrderIdentity(active.identity),
                },
            .confirmedTick = confirmedTick,
        };
        if (candidate.isValid() && payload.sourceOrderRevision == active.identity.queueRevision)
            m_attackCompletionCandidates.push_back(candidate);
    }
}

void ObjectAIRuntime::stageTerminalAttackCompletions(ObjectAIShadowTickReport& report)
{
    for (const AIAttackOrderCompletion& candidate : m_attackCompletionCandidates)
    {
        const std::optional<AIActorHandle> actor = find(candidate.correlation.subject);
        if (!actor)
            continue;
        const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
        ObjectAIOrderAdmissionRequest active;
        if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success ||
            active.kind != ObjectAIOrderKind::Attack ||
            !matchesAIAsyncOrderIdentity(candidate.correlation.orderIdentity, active.identity))
            continue;
        const AIStateFamilySoAStorage* actorStorage = storage(*actor);
        if (!actorStorage)
            continue;
        const AIStateId current = actorStorage->runtimes()[actor->slot].currentState;
        const AIStateId payloadState = actorStorage->payloadStates()[actor->slot];
        if (attackPolicyFor(current).valid && payloadState == current)
            continue;
        AIAttackOrderCompletion terminal = candidate;
        const AIStateTransitionReason reason = actorStorage->runtimes()[actor->slot].lastTransitionReason;
        if (reason == AIStateTransitionReason::Success)
            terminal.outcome = AIStateOutcome::Success;
        else if (reason == AIStateTransitionReason::Failure)
            terminal.outcome = AIStateOutcome::Failure;
        else
            continue;
        if (stageOutput(terminal, report))
            ++report.attackCompletionsStaged;
        else
            ++report.attackCompletionsRejected;
    }
    m_attackCompletionCandidates.clear();
}

ObjectAIRuntime::SubjectIterator ObjectAIRuntime::lowerBound(
    ObjectId subject) noexcept
{
    return std::lower_bound(m_subjects.begin(),
                            m_subjects.end(),
                            subject,
                            [](const AIStateSoASubjectSlot& record, ObjectId value) { return record.subject < value; });
}

ObjectAIRuntime::ConstSubjectIterator ObjectAIRuntime::lowerBound(
    ObjectId subject) const noexcept
{
    return std::lower_bound(m_subjects.begin(),
                            m_subjects.end(),
                            subject,
                            [](const AIStateSoASubjectSlot& record, ObjectId value) { return record.subject < value; });
}

uint8_t ObjectAIRuntime::hasCapability(const ObjectAIReadOnlyFact& fact, ObjectAICapability capability) noexcept
{
    return (fact.capabilityMask & objectAICapabilityBit(capability)) != 0 ? uint8_t{1} : uint8_t{0};
}

uint32_t ObjectAIRuntime::waypointBranchChoice(ObjectId subject,
                                               const AIStateParameters& parameters,
                                               AIWaypointHandle waypoint,
                                               uint32_t generation) noexcept
{
    // Per-order/per-hop splitmix projection avoids consuming the session's
    // global gameplay RNG merely because an actor slept for a different
    // number of ticks. Every segment still receives a stable 32-bit
    // branch choice, including paths that allow immediate backtracking.
    uint64_t value = subject.value;
    value ^= parameters.sourceOrderRevision + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
    value ^= waypoint.value + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
    value ^= static_cast<uint64_t>(generation) << 32;
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    value ^= value >> 31;
    return static_cast<uint32_t>(value);
}

uint32_t ObjectAIRuntime::scanJitterDraw(ObjectId subject,
                                         uint64_t sourceOrderRevision,
                                         uint64_t confirmedTick) noexcept
{
    // RefCode staggers the first guard/hunt scan with
    // GameLogicRandomValue(0, rate) in AIGuardIdleState::onEnter,
    // AIGuardReturnState::onEnter (plus the AIGuardRetaliate/AITNGuard copies)
    // and AIStates.cpp's hunt onEnter, purely so that every actor does not
    // scan on the same frame. Reproducing that with the session's gameplay RNG
    // would make the shared stream depend on how many actors happened to enter
    // a guard phase this tick, so use the same per-identity splitmix
    // projection as waypointBranchChoice() above: no draw is consumed and the
    // stagger still covers the whole [0, rate] period the original used.
    uint64_t value = subject.value;
    value ^= sourceOrderRevision + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
    value ^= confirmedTick + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    value ^= value >> 31;
    return static_cast<uint32_t>(value);
}

int32_t ObjectAIRuntime::wanderCellsFromRadius(int64_t radiusRaw,
                                               int64_t cellSizeRaw,
                                               bool hasLocomotor) noexcept
{
    // AIWanderInPlaceState::onEnter/update (AIStates.cpp:4693, 4736):
    //   Int delta = 3;
    //   if (ai->getCurLocomotor())
    //     delta = REAL_TO_INT_FLOOR(radius / PATHFIND_CELL_SIZE_F + 0.5f);
    // The quotient is never negative here, so "floor(x + 0.5)" is exactly
    // round-half-up on the raw quotient and needs no float step.
    constexpr int32_t DefaultWanderCells = 3;
    if (!hasLocomotor)
        return DefaultWanderCells;
    if (cellSizeRaw <= 0 || radiusRaw <= 0)
        return 0;
    const int64_t cells = (radiusRaw + cellSizeRaw / 2) / cellSizeRaw;
    return static_cast<int32_t>(
        std::min<int64_t>(cells, std::numeric_limits<int32_t>::max()));
}

int32_t ObjectAIRuntime::wanderCellsFromWidthFactor(int64_t widthFactorRaw) noexcept
{
    // AIWanderState::onEnter/update and AIPanicState::onEnter
    // (AIStates.cpp:4571, 4809):
    //   if (curLoco && curLoco->getWanderWidthFactor() > 0.0f) {
    //     Int delta = REAL_TO_INT_FLOOR(curLoco->getWanderWidthFactor()+0.5f);
    //     if (delta < 1) delta = 1;
    // A non-positive factor leaves the group offset at zero.
    if (widthFactorRaw <= 0)
        return 0;
    // 0.5 in the shared Q32.32 raw encoding.
    constexpr int64_t HalfRaw = int64_t{1} << 31;
    const int64_t cells = (widthFactorRaw + HalfRaw) >> 32;
    return static_cast<int32_t>(std::max<int64_t>(
        1, std::min<int64_t>(cells, std::numeric_limits<int32_t>::max())));
}

int32_t ObjectAIRuntime::wanderOffsetDraw(ObjectId subject,
                                          uint64_t sourceOrderRevision,
                                          uint64_t confirmedTick,
                                          uint32_t axis,
                                          int32_t cells) noexcept
{
    // Deterministic stand-in for the legacy GameLogicRandomValue(-delta, delta)
    // pair. Same splitmix-over-stable-identity projection as
    // waypointBranchChoice()/scanJitterDraw(): the wander goal stays a pure
    // function of confirmed identity, so a slot that slept for a different
    // number of ticks cannot shift the shared gameplay stream.
    if (cells <= 0)
        return 0;
    uint64_t value = subject.value;
    value ^= sourceOrderRevision + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
    value ^= confirmedTick + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
    value ^= static_cast<uint64_t>(axis) << 32;
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    value ^= value >> 31;
    // Inclusive [-cells, cells], matching GameLogicRandomValue's closed range.
    const uint64_t span = static_cast<uint64_t>(cells) * 2ull + 1ull;
    return static_cast<int32_t>(static_cast<int64_t>(value % span) -
                                static_cast<int64_t>(cells));
}

size_t ObjectAIRuntime::waypointTeamProgressIndex(AITeamHandle team) const noexcept
{
    const auto found = std::lower_bound(m_waypointTeamProgress.begin(),
                                        m_waypointTeamProgress.end(),
                                        team.value,
                                        [](const ObjectAIWaypointTeamProgressState& value, uint64_t wanted)
                                        { return value.team.value < wanted; });
    if (found == m_waypointTeamProgress.end() || found->team != team)
        return NoWaypointTeamProgress;
    return static_cast<size_t>(std::distance(m_waypointTeamProgress.begin(), found));
}

void ObjectAIRuntime::ensureWaypointTeamProgress(const ObjectAIOrderIdentity& identity,
                                                 const AIStateParameters& parameters)
{
    const ObjectAIWaypointTeamProgressState candidate{
        .team = parameters.waypointTeam,
        .start = parameters.waypoint,
        .current = parameters.waypoint,
        .graphRevision = parameters.waypointGraphRevision,
        .revision = 1,
        .issuedTick = identity.issuedTick,
        .sourceSequence = identity.sourceSequence,
        .sourceScriptId = identity.sourceScriptId,
    };
    auto found = std::lower_bound(m_waypointTeamProgress.begin(),
                                  m_waypointTeamProgress.end(),
                                  candidate.team.value,
                                  [](const ObjectAIWaypointTeamProgressState& value, uint64_t wanted)
                                  { return value.team.value < wanted; });
    if (found == m_waypointTeamProgress.end() || found->team != candidate.team)
    {
        m_waypointTeamProgress.insert(found, candidate);
        return;
    }
    const bool sameOrder = found->start == candidate.start && found->graphRevision == candidate.graphRevision &&
                           found->issuedTick == candidate.issuedTick &&
                           found->sourceSequence == candidate.sourceSequence &&
                           found->sourceScriptId == candidate.sourceScriptId;
    if (!sameOrder)
        *found = candidate;
}

void ObjectAIRuntime::advanceWaypointTeamProgress(
    container::Span<const ObjectAIReadOnlyFact> facts) noexcept
{
    const size_t progressCount = m_waypointTeamProgress.size();
    if (progressCount == 0)
        return;
    m_waypointTeamArrivals.assign(progressCount, INVALID_OBJECT_ID);

    struct TeamCentreAccumulator final
    {
        int64_t x = 0;
        int64_t y = 0;
        uint32_t count = 0;
        ObjectId first = INVALID_OBJECT_ID;
        bool overflow = false;
    };
    container::Vector<TeamCentreAccumulator> teamCentres(progressCount);

    const auto factFor = [facts](ObjectId subject)
        -> const ObjectAIReadOnlyFact* {
        const auto found = std::lower_bound(
            facts.begin(), facts.end(), subject,
            [](const ObjectAIReadOnlyFact& fact, ObjectId wanted) {
                return fact.subject < wanted;
            });
        return found != facts.end() && found->subject == subject
            ? &*found : nullptr;
    };
    const auto addChecked = [](int64_t& total, int64_t value) noexcept {
        if ((value > 0 && total > std::numeric_limits<int64_t>::max() - value) ||
            (value < 0 && total < std::numeric_limits<int64_t>::min() - value)) {
            return false;
        }
        total += value;
        return true;
    };

    for (const AIStateSoASubjectSlot& actor : m_subjects)
    {
        const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor.handle.batch];
        ObjectAIOrderAdmissionRequest active;
        if (admission.activeOrder(admission.handle(actor.handle.slot), active) !=
                ObjectAIOrderAdmissionStatus::Success ||
            active.moveRouteSubtype != ObjectAIMoveRouteSubtype::WaypointPathTeam || !active.waypointTeam)
        {
            continue;
        }
        const size_t index = waypointTeamProgressIndex(active.waypointTeam);
        if (index == NoWaypointTeamProgress)
            continue;
        const ObjectAIWaypointTeamProgressState& progress = m_waypointTeamProgress[index];
        if (!progress.current || progress.start != active.waypointStart ||
            progress.graphRevision != active.waypointGraphRevision)
        {
            continue;
        }
        if (const ObjectAIReadOnlyFact* fact = factFor(actor.subject);
            fact && fact->positionValid) {
            TeamCentreAccumulator& centre = teamCentres[index];
            centre.overflow = centre.overflow ||
                !addChecked(centre.x, fact->position.xRaw) ||
                !addChecked(centre.y, fact->position.yRaw);
            ++centre.count;
            if (!centre.first || actor.subject < centre.first)
                centre.first = actor.subject;
        }
        const AIStateFamilySoAStorage* actorStorage = storage(actor.handle);
        if (!actorStorage ||
            (actorStorage->runtimes()[actor.handle.slot].currentState != AIStateId::FollowWaypointPathAsTeam &&
             actorStorage->runtimes()[actor.handle.slot].currentState != AIStateId::AttackFollowWaypointPathAsTeam) ||
            (actorStorage->payloadStates()[actor.handle.slot] != AIStateId::FollowWaypointPathAsTeam &&
             actorStorage->payloadStates()[actor.handle.slot] != AIStateId::AttackFollowWaypointPathAsTeam))
        {
            continue;
        }
        const AIWaypointPathStatePayload payload = actorStorage->waypointPath().load(actor.handle.slot);
        if (payload.awaitingTeamProgress && payload.current == progress.current &&
            payload.teamRevision == progress.revision)
        {
            // RefCode's first AIFollowWaypointPathState which reaches the
            // shared node advances Team::currentWaypoint immediately. The
            // SoA shadow has no execution-order side effects, so select the
            // smallest simultaneous arrival to make that same one-winner
            // rule deterministic across workers.
            if (!m_waypointTeamArrivals[index] ||
                actor.subject < m_waypointTeamArrivals[index])
            {
                m_waypointTeamArrivals[index] = actor.subject;
            }
        }
    }

    for (size_t index = 0; index < progressCount; ++index)
    {
        ObjectAIWaypointTeamProgressState& progress = m_waypointTeamProgress[index];
        ObjectId arrivingSubject = m_waypointTeamArrivals[index];
        const TeamCentreAccumulator& centre = teamCentres[index];
        if (!arrivingSubject && progress.current && centre.count != 0 &&
            !centre.overflow && m_config.skirmishGroupFudgeDistanceRaw > 0) {
            const AIWaypointQuery currentNode = m_waypointGraphResolver.node(
                progress.current, progress.graphRevision);
            if (currentNode.status == AIWaypointQueryStatus::Node) {
                const int64_t centerX = centre.x /
                    static_cast<int64_t>(centre.count);
                const int64_t centerY = centre.y /
                    static_cast<int64_t>(centre.count);
                const auto distance = [](int64_t left, int64_t right) noexcept {
                    return left >= right
                        ? static_cast<uint64_t>(left) -
                              static_cast<uint64_t>(right)
                        : static_cast<uint64_t>(right) -
                              static_cast<uint64_t>(left);
                };
                const uint64_t dx = distance(
                    centerX, currentNode.node.position.xRaw);
                const uint64_t dy = distance(
                    centerY, currentNode.node.position.yRaw);
                const uint64_t count = centre.count;
                const uint64_t base = static_cast<uint64_t>(
                    m_config.skirmishGroupFudgeDistanceRaw);
                const uint64_t radius = count != 0 &&
                        base > std::numeric_limits<uint64_t>::max() / count
                    ? std::numeric_limits<uint64_t>::max()
                    : base * count;
                // Normalize both axes to Q1.31 before squaring. The bitwise
                // division cannot overflow and remains deterministic on
                // every target; the final sum fits in 63 bits.
                const auto normalizedQ31 = [](
                    uint64_t value, uint64_t denominator) noexcept {
                    if (value >= denominator) return uint64_t{1} << 31;
                    uint64_t remainder = value;
                    uint64_t quotient = 0;
                    for (uint32_t bit = 0; bit < 31; ++bit) {
                        quotient <<= 1;
                        if (remainder >= denominator - remainder) {
                            remainder -= denominator - remainder;
                            quotient |= 1;
                        } else {
                            remainder += remainder;
                        }
                    }
                    return quotient;
                };
                const uint64_t nx = dx <= radius
                    ? normalizedQ31(dx, radius) : uint64_t{1} << 31;
                const uint64_t ny = dy <= radius
                    ? normalizedQ31(dy, radius) : uint64_t{1} << 31;
                const uint64_t unitSquared = uint64_t{1} << 62;
                const bool within = dx <= radius && dy <= radius &&
                    nx * nx + ny * ny <= unitSquared;
                if (within) arrivingSubject = centre.first;
            }
        }
        if (!progress.current || !arrivingSubject)
        {
            continue;
        }
        const AIWaypointQuery node = m_waypointGraphResolver.node(progress.current, progress.graphRevision);
        if (node.status != AIWaypointQueryStatus::Node)
            continue;
        if (node.node.linkCount == 0)
        {
            progress.current = {};
            ++progress.revision;
            if (progress.revision == 0)
                ++progress.revision;
            wakeWaypointTeamMembers(progress.team);
            continue;
        }
        const std::optional<AIActorHandle> arrivingActor = find(arrivingSubject);
        const AIStateFamilySoAStorage* arrivingStorage =
            arrivingActor ? storage(*arrivingActor) : nullptr;
        if (!arrivingStorage)
            continue;
        const AIStateParameters& parameters =
            arrivingStorage->parameters()[arrivingActor->slot];
        const uint32_t branch = waypointBranchChoice(
            arrivingSubject, parameters, progress.current,
            static_cast<uint32_t>(progress.revision));
        const AIWaypointLinkQuery link =
            m_waypointGraphResolver.link(progress.current, progress.graphRevision, branch % node.node.linkCount);
        if (link.status != AIWaypointQueryStatus::Node || !link.target)
            continue;
        progress.current = link.target;
        ++progress.revision;
        if (progress.revision == 0)
            ++progress.revision;
        wakeWaypointTeamMembers(progress.team);
    }
}

void ObjectAIRuntime::wakeWaypointTeamMembers(AITeamHandle team) noexcept
{
    for (const AIStateSoASubjectSlot& actor : m_subjects)
    {
        const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor.handle.batch];
        ObjectAIOrderAdmissionRequest active;
        if (admission.activeOrder(admission.handle(actor.handle.slot), active) ==
                ObjectAIOrderAdmissionStatus::Success &&
            active.moveRouteSubtype == ObjectAIMoveRouteSubtype::WaypointPathTeam && active.waypointTeam == team)
        {
            wakeForServiceResult(actor.handle);
        }
    }
}

void ObjectAIRuntime::merge(ObjectAIShadowTickReport& target, const AIStateSoAMultiwaveReport& source) noexcept
{
    target.waves += source.waves;
    target.stepsProcessed += source.stepsProcessed;
    target.sleeping += source.sleeping;
    target.unsupported += source.unsupported;
    target.transitionsRequested += source.transitionsRequested;
    target.transitionsCommitted += source.transitionsCommitted;
    target.transitionsRejected += source.transitionsRejected;
    target.transitionConflicts += source.transitionConflicts;
    target.transitionBudgetExceeded += source.transitionBudgetExceeded;
    target.spanRejections += source.spansRejected ? 1 : 0;
    target.transitionCapacityRejections += source.transitionCapacityExceeded ? 1 : 0;
    target.blockedExits += source.exitBlocked ? 1 : 0;
    if (source.spansRejected || source.transitionCapacityExceeded || source.exitBlocked)
        target.status = ObjectAIShadowTickStatus::BatchRejected;
}

} // namespace engine::ai
