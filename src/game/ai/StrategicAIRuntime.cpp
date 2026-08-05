#include "StrategicAIRuntime.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <new>

namespace engine {
namespace {

[[nodiscard]] uint64_t saturatingAdd(
    uint64_t value, uint64_t increment) noexcept {
    return value > std::numeric_limits<uint64_t>::max() - increment
        ? std::numeric_limits<uint64_t>::max()
        : value + increment;
}

[[nodiscard]] const StrategicAIPlayerSnapshot* findSnapshot(
    container::Span<const StrategicAIPlayerSnapshot> snapshots,
    PlayerId player) noexcept {
    const auto found = std::find_if(
        snapshots.begin(), snapshots.end(),
        [player](const StrategicAIPlayerSnapshot& snapshot) noexcept {
            return snapshot.player == player;
        });
    return found == snapshots.end() ? nullptr : &*found;
}

[[nodiscard]] uint32_t desiredTeamSize(AiDifficulty difficulty) noexcept {
    switch (difficulty) {
    case AiDifficulty::Easy: return 4;
    case AiDifficulty::Hard: return 8;
    case AiDifficulty::Normal:
    case AiDifficulty::None: return 6;
    }
    return 6;
}

[[nodiscard]] uint32_t desiredTeamCount(AiDifficulty difficulty) noexcept {
    switch (difficulty) {
    case AiDifficulty::Easy: return 1;
    case AiDifficulty::Hard: return 3;
    case AiDifficulty::Normal:
    case AiDifficulty::None: return 2;
    }
    return 2;
}

[[nodiscard]] uint64_t adjustedInterval(
    uint32_t baseTicks, int64_t cash, math::q32_32 poor,
    math::q32_32 wealthy, math::q32_32 poorRate,
    math::q32_32 wealthyRate) noexcept {
    math::q32_32 rate{1};
    const int64_t poorCash = poor.raw() >> 32u;
    const int64_t wealthyCash = wealthy.raw() >> 32u;
    if (cash < poorCash) rate = poorRate;
    else if (cash > wealthyCash) rate = wealthyRate;
    if (rate <= math::q32_32{}) return std::max<uint64_t>(1, baseTicks);
    const uint32_t boundedBase = std::min<uint32_t>(
        baseTicks, static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
    const math::q32_32 adjusted =
        math::q32_32{static_cast<int32_t>(boundedBase)} / rate;
    if (adjusted.raw() <= 0) return 1;
    const uint64_t whole = static_cast<uint64_t>(adjusted.raw()) >> 32u;
    const bool fractional =
        (static_cast<uint64_t>(adjusted.raw()) & 0xffffffffull) != 0;
    return std::max<uint64_t>(1, whole + (fractional ? 1u : 0u));
}

[[nodiscard]] int64_t combatTeamStartCash(
    int64_t unitCost, uint32_t remainingUnits,
    math::q32_32 resourceFraction) noexcept {
    if (unitCost <= 0 || remainingUnits == 0 ||
        resourceFraction <= math::q32_32{}) {
        return 0;
    }
    const int64_t boundedCost = std::min<int64_t>(
        unitCost,
        std::numeric_limits<int32_t>::max() /
            static_cast<int64_t>(remainingUnits));
    const int32_t teamCost = static_cast<int32_t>(
        boundedCost * static_cast<int64_t>(remainingUnits));
    const math::q32_32 required =
        math::q32_32{teamCost} * resourceFraction;
    if (required.raw() <= 0) return 0;
    const uint64_t raw = static_cast<uint64_t>(required.raw());
    return static_cast<int64_t>(
        (raw >> 32u) + ((raw & 0xffffffffull) != 0 ? 1u : 0u));
}

[[nodiscard]] int64_t teamStartCash(
    int64_t estimatedCost, math::q32_32 resourceFraction) noexcept {
    if (estimatedCost <= 0 || resourceFraction <= math::q32_32{})
        return 0;
    const int32_t bounded = static_cast<int32_t>(std::min<int64_t>(
        estimatedCost, std::numeric_limits<int32_t>::max()));
    const math::q32_32 required =
        math::q32_32{bounded} * resourceFraction;
    if (required.raw() <= 0) return 0;
    // RefCode stores the running team cost in Int and applies
    // TeamResourcesToStart through compound assignment.  Positive fractional
    // currency is therefore truncated, not rounded up.
    return required.raw() >> 32u;
}

} // namespace

bool StrategicAIRuntime::initialize(
    StrategicAIRuntimeConfig config,
    container::Span<const StrategicAIPlayerDescriptor> players) {
    reset();
    if (config.logicFramesPerSecond == 0 || config.maximumPlayers == 0 ||
        config.maximumBuildPlans == 0 ||
        players.size() > config.maximumPlayers) {
        return false;
    }
    config.economyIntervalTicks =
        std::max<uint32_t>(1, config.economyIntervalTicks);
    config.structureIntervalTicks =
        std::max<uint32_t>(1, config.structureIntervalTicks);
    config.productionIntervalTicks =
        std::max<uint32_t>(1, config.productionIntervalTicks);
    config.tacticalIntervalTicks =
        std::max<uint32_t>(1, config.tacticalIntervalTicks);
    config.enemyReviewIntervalTicks =
        std::max<uint32_t>(1, config.enemyReviewIntervalTicks);
    config.rebuildDelayTicks =
        std::max<uint32_t>(1, config.rebuildDelayTicks);
    config.teamResourcesToStart = math::q32_32::max(
        math::q32_32{}, config.teamResourcesToStart);
    m_config = config;

    for (const StrategicAIPlayerDescriptor& descriptor : players) {
        if (!descriptor.player || descriptor.player.isNeutral() ||
            descriptor.difficulty == AiDifficulty::None) {
            continue;
        }
        const auto [_, inserted] = m_brains.emplace(
            descriptor.player,
            StrategicAIPlayerBrain{
                .player = descriptor.player,
                .difficulty = descriptor.difficulty,
                // AIPlayer starts m_structureTimer and m_teamTimer at 2 so
                // neither domain runs during frame 1.
                .nextStructureTick = 2,
                .nextProductionTick = 2,
                .autonomousSkirmish = descriptor.autonomousSkirmish,
            });
        if (!inserted) {
            reset();
            return false;
        }
    }
    m_buildPlans.reserve(std::min<uint32_t>(64, m_config.maximumBuildPlans));
    return true;
}

void StrategicAIRuntime::reset() noexcept {
    m_config = {};
    m_brains.clear();
    m_buildPlans.clear();
    m_workOrders.clear();
    m_teams.clear();
    m_teamConditions.clear();
    m_nextBuildPlanId = 1;
    m_nextWorkOrderId = 1;
    m_nextTeamId = 1;
}

uint64_t StrategicAIRuntime::addBuildPlan(StrategicAIBuildPlan plan) {
    if (!findBrain(plan.player) || plan.objectType.empty() ||
        m_buildPlans.size() >= m_config.maximumBuildPlans) {
        return 0;
    }
    if (plan.id == 0) {
        if (m_nextBuildPlanId == 0) return 0;
        plan.id = m_nextBuildPlanId++;
    } else {
        if (findBuildPlan(plan.id)) return 0;
        if (plan.id >= m_nextBuildPlanId) {
            m_nextBuildPlanId = plan.id ==
                    std::numeric_limits<uint64_t>::max()
                ? 0 : plan.id + 1;
        }
    }
    const auto position = std::lower_bound(
        m_buildPlans.begin(), m_buildPlans.end(), plan.id,
        [](const StrategicAIBuildPlan& candidate, uint64_t id) noexcept {
            return candidate.id < id;
        });
    const uint64_t id = plan.id;
    m_buildPlans.insert(position, std::move(plan));
    return id;
}

StrategicAIBuildPlan* StrategicAIRuntime::findBuildPlan(
    uint64_t planId) noexcept {
    const auto found = std::lower_bound(
        m_buildPlans.begin(), m_buildPlans.end(), planId,
        [](const StrategicAIBuildPlan& plan, uint64_t id) noexcept {
            return plan.id < id;
        });
    return found != m_buildPlans.end() && found->id == planId
        ? &*found : nullptr;
}

const StrategicAIPlayerBrain* StrategicAIRuntime::findBrain(
    PlayerId player) const noexcept {
    const auto found = m_brains.find(player);
    return found == m_brains.end() ? nullptr : &found->second;
}

bool StrategicAIRuntime::teamSelectionDue(
    PlayerId player, uint64_t confirmedTick) const noexcept {
    const StrategicAIPlayerBrain* brain = findBrain(player);
    return brain && brain->autonomousSkirmish &&
        confirmedTick >= brain->nextProductionTick;
}

bool StrategicAIRuntime::teamConditionEvaluationDue(
    ScriptTeamId definition, uint64_t confirmedTick) const noexcept {
    if (!definition) return false;
    const auto found = std::lower_bound(
        m_teamConditions.begin(), m_teamConditions.end(), definition,
        [](const StrategicAITeamConditionState& state,
           ScriptTeamId wanted) noexcept {
            return state.definition < wanted;
        });
    return found == m_teamConditions.end() ||
        found->definition != definition ||
        (!found->permanentlyUnavailable &&
         confirmedTick >= found->nextEvaluationTick);
}

void StrategicAIRuntime::observeTeamCondition(
    ScriptTeamId definition, std::optional<bool> value,
    uint32_t evaluationDelayTicks, uint64_t confirmedTick) {
    if (!definition) return;
    StrategicAITeamConditionState candidate{
        .definition = definition,
        .nextEvaluationTick = value
            ? saturatingAdd(confirmedTick, evaluationDelayTicks)
            : std::numeric_limits<uint64_t>::max(),
        .value = value.value_or(false),
        .permanentlyUnavailable = !value.has_value(),
    };
    auto found = std::lower_bound(
        m_teamConditions.begin(), m_teamConditions.end(), definition,
        [](const StrategicAITeamConditionState& state,
           ScriptTeamId wanted) noexcept {
            return state.definition < wanted;
        });
    if (found == m_teamConditions.end() ||
        found->definition != definition) {
        m_teamConditions.insert(found, candidate);
    } else {
        *found = candidate;
    }
}

bool StrategicAIRuntime::teamConditionValue(
    ScriptTeamId definition) const noexcept {
    const auto found = std::lower_bound(
        m_teamConditions.begin(), m_teamConditions.end(), definition,
        [](const StrategicAITeamConditionState& state,
           ScriptTeamId wanted) noexcept {
            return state.definition < wanted;
        });
    return found != m_teamConditions.end() &&
        found->definition == definition && found->value &&
        !found->permanentlyUnavailable;
}

void StrategicAIRuntime::acknowledgeScenarioTeamBuild(
    PlayerId player, bool accepted) noexcept {
    auto found = m_brains.find(player);
    if (found == m_brains.end()) return;
    if (accepted) {
        found->second.consecutiveProductionFailures = 0;
    } else if (found->second.consecutiveProductionFailures !=
               std::numeric_limits<uint32_t>::max()) {
        ++found->second.consecutiveProductionFailures;
    }
}

bool StrategicAIRuntime::acknowledgeBuildAdmission(
    uint64_t planId, bool accepted, ObjectId builder,
    ObjectId construction, uint64_t retryTick,
    bool permanentFailure) noexcept {
    StrategicAIBuildPlan* plan = findBuildPlan(planId);
    if (!plan || (plan->state != StrategicAIBuildState::Reserved &&
                  plan->state != StrategicAIBuildState::Unbuilt)) {
        return false;
    }
    if (accepted && construction) {
        plan->reservedBuilder = builder;
        plan->constructedObject = construction;
        plan->state = StrategicAIBuildState::Constructing;
        return true;
    }
    plan->reservedBuilder = INVALID_OBJECT_ID;
    plan->constructedObject = INVALID_OBJECT_ID;
    if (permanentFailure) {
        plan->state = StrategicAIBuildState::Exhausted;
        plan->nextAttemptTick = std::numeric_limits<uint64_t>::max();
    } else {
        plan->state = StrategicAIBuildState::Unbuilt;
        plan->nextAttemptTick = retryTick;
        if (plan->attemptCount != std::numeric_limits<uint32_t>::max()) {
            ++plan->attemptCount;
        }
    }
    return true;
}

bool StrategicAIRuntime::observeBuildObject(
    uint64_t planId, bool present, bool correctOwner,
    bool underConstruction, uint64_t confirmedTick) noexcept {
    StrategicAIBuildPlan* plan = findBuildPlan(planId);
    if (!plan || (plan->state != StrategicAIBuildState::Constructing &&
                  plan->state != StrategicAIBuildState::Completed)) {
        return false;
    }
    if (present && correctOwner) {
        if (!underConstruction &&
            plan->state == StrategicAIBuildState::Constructing) {
            plan->state = StrategicAIBuildState::Completed;
            plan->reservedBuilder = INVALID_OBJECT_ID;
            if (const auto brain = m_brains.find(plan->player);
                brain != m_brains.end()) {
                // AIPlayer::onStructureProduced shortcuts both build and team
                // queue delays. The next confirmed strategic pass performs
                // the work; completion never calls a planner recursively.
                brain->second.nextStructureTick = confirmedTick;
                brain->second.nextProductionTick = confirmedTick;
            }
        }
        return true;
    }

    plan->reservedBuilder = INVALID_OBJECT_ID;
    plan->constructedObject = INVALID_OBJECT_ID;
    if (plan->remainingRebuilds == 0) {
        plan->state = StrategicAIBuildState::Exhausted;
        plan->nextAttemptTick = std::numeric_limits<uint64_t>::max();
        return true;
    }
    if (plan->remainingRebuilds > 0) --plan->remainingRebuilds;
    plan->state = StrategicAIBuildState::RebuildDelay;
    plan->nextAttemptTick = saturatingAdd(
        confirmedTick, m_config.rebuildDelayTicks);
    return true;
}

bool StrategicAIRuntime::acknowledgeProduction(
    PlayerId player, uint64_t workOrderId, bool accepted,
    uint32_t productionId, uint64_t retryTick) noexcept {
    const auto found = m_brains.find(player);
    if (found == m_brains.end()) return false;
    const auto order = workOrderId == 0
        ? m_workOrders.end()
        : std::find_if(
              m_workOrders.begin(), m_workOrders.end(),
              [player, workOrderId](
                  const StrategicAIWorkOrder& value) noexcept {
                  return value.player == player && value.id == workOrderId;
              });
    if (workOrderId != 0 && order == m_workOrders.end()) return false;
    if (workOrderId != 0 && accepted && productionId == 0)
        accepted = false;
    StrategicAIPlayerBrain& brain = found->second;
    if (accepted) {
        brain.consecutiveProductionFailures = 0;
    } else {
        if (brain.consecutiveProductionFailures !=
            std::numeric_limits<uint32_t>::max()) {
            ++brain.consecutiveProductionFailures;
        }
        brain.nextProductionTick = std::max(
            brain.nextProductionTick, retryTick);
    }
    if (workOrderId != 0) {
        order->nextAttemptTick = retryTick;
        if (accepted) {
            order->productionId = productionId;
        } else {
            order->state =
                StrategicAIWorkOrderState::WaitingForProducer;
            order->producer = INVALID_OBJECT_ID;
            order->productionId = 0;
            if (order->failureCount !=
                std::numeric_limits<uint32_t>::max()) {
                ++order->failureCount;
            }
        }
    }
    return true;
}

bool StrategicAIRuntime::observeProductionCompletion(
    ObjectId producer, uint32_t productionId,
    bool productionStillActive, uint64_t confirmedTick) noexcept {
    if (!producer || productionId == 0) return false;
    const auto order = std::find_if(
        m_workOrders.begin(), m_workOrders.end(),
        [producer, productionId](
            const StrategicAIWorkOrder& value) noexcept {
            return value.state == StrategicAIWorkOrderState::Producing &&
                value.producer == producer &&
                value.productionId == productionId;
        });
    if (order == m_workOrders.end()) return false;
    if (const auto brain = m_brains.find(order->player);
        brain != m_brains.end()) {
        // Unit completion is the original m_teamDelay shortcut.
        brain->second.nextProductionTick = confirmedTick;
    }
    if (order->completedCount < order->requiredCount)
        ++order->completedCount;
    order->failureCount = 0;
    if (productionStillActive) {
        // One ProductionUpdate job may emit several quantity indices under
        // the same factory-local ID. Retain the exact handle until the final
        // quantity is acknowledged so later members of the batch cannot be
        // mistaken for an unrelated job or trigger a duplicate queue.
        order->state = StrategicAIWorkOrderState::Producing;
        order->nextAttemptTick = confirmedTick;
        return true;
    }
    order->producer = INVALID_OBJECT_ID;
    order->productionId = 0;
    order->state = order->completedCount >= order->requiredCount
        ? StrategicAIWorkOrderState::Completed
        : StrategicAIWorkOrderState::WaitingForProducer;
    order->nextAttemptTick = confirmedTick;
    return true;
}

bool StrategicAIRuntime::observeBlockingBridge(
    PlayerId player, ObjectId bridge, uint64_t confirmedTick) noexcept {
    if (!player || !bridge) return false;
    const auto found = m_brains.find(player);
    if (found == m_brains.end() || !found->second.autonomousSkirmish)
        return false;
    StrategicAIPlayerBrain& brain = found->second;
    if (brain.pendingBridgeRepair &&
        brain.pendingBridgeRepair != bridge) {
        return false;
    }
    brain.pendingBridgeRepair = bridge;
    brain.nextBridgeRepairTick = std::min(
        brain.nextBridgeRepairTick == 0
            ? confirmedTick : brain.nextBridgeRepairTick,
        confirmedTick);
    return true;
}

void StrategicAIRuntime::acknowledgeBridgeRepair(
    PlayerId player, ObjectId bridge, bool stillNeedsRepair,
    uint64_t retryTick) noexcept {
    const auto found = m_brains.find(player);
    if (found == m_brains.end() ||
        found->second.pendingBridgeRepair != bridge) {
        return;
    }
    if (!stillNeedsRepair) {
        found->second.pendingBridgeRepair = INVALID_OBJECT_ID;
        found->second.nextBridgeRepairTick = 0;
        return;
    }
    found->second.nextBridgeRepairTick = retryTick;
}

void StrategicAIRuntime::acknowledgeTacticalOrder(
    uint64_t strategicTeamId, bool accepted,
    uint64_t retryTick) noexcept {
    if (strategicTeamId == 0) return;
    const auto found = std::find_if(
        m_teams.begin(), m_teams.end(),
        [strategicTeamId](const StrategicAITeam& team) noexcept {
            return team.id == strategicTeamId;
        });
    if (found == m_teams.end() || accepted) return;

    // OrderExecutor validates the whole actor set before mutating any queue.
    // A rejection therefore means this team never entered the requested
    // attack/defend state. Put it back at the Ready boundary and wake the
    // owning brain at the deterministic retry tick instead of waiting for an
    // idle projection to accidentally repair the planner state later.
    found->state = StrategicAITeamState::Ready;
    found->nextOrderTick = retryTick;
    const auto brain = m_brains.find(found->player);
    if (brain != m_brains.end()) {
        brain->second.nextTacticalTick = std::min(
            brain->second.nextTacticalTick, retryTick);
    }
}

uint32_t StrategicAIRuntime::nextSequence(
    StrategicAIPlayerBrain& brain) noexcept {
    const uint32_t sequence = brain.nextSequence == 0
        ? 1 : brain.nextSequence;
    brain.nextSequence = sequence == std::numeric_limits<uint32_t>::max()
        ? 1 : sequence + 1;
    return sequence;
}

uint64_t StrategicAIRuntime::productionDelay(
    const StrategicAIPlayerBrain& brain,
    const StrategicAIPlayerSnapshot& snapshot,
    uint32_t failures) const noexcept {
    uint64_t base = adjustedInterval(
        m_config.productionIntervalTicks, snapshot.cash,
        m_config.poor, m_config.wealthy,
        m_config.teamsPoorRate, m_config.teamsWealthyRate);
    switch (brain.difficulty) {
    case AiDifficulty::Easy: base *= 2; break;
    case AiDifficulty::Hard: base = std::max<uint64_t>(1, base / 2); break;
    case AiDifficulty::Normal:
    case AiDifficulty::None: break;
    }
    return base * std::min<uint32_t>(8, failures + 1);
}

void StrategicAIRuntime::updatePhase(
    StrategicAIPlayerBrain& brain,
    const StrategicAIPlayerSnapshot& snapshot) noexcept {
    if (!snapshot.hasBuilder && snapshot.ownedStructureCount == 0) {
        brain.phase = StrategicAIPhase::Recover;
    } else if (snapshot.energyConsumption > snapshot.energyProduction) {
        brain.phase = StrategicAIPhase::Economy;
    } else if (snapshot.baseThreatened) {
        brain.phase = StrategicAIPhase::Defense;
    } else if (snapshot.ownedStructureCount < 2) {
        brain.phase = StrategicAIPhase::Base;
    } else if (snapshot.idleCombatUnitCount >= 6 &&
               snapshot.preferredEnemyTarget) {
        brain.phase = StrategicAIPhase::Assault;
    } else {
        brain.phase = StrategicAIPhase::Production;
    }
}

void StrategicAIRuntime::acquireEnemy(
    StrategicAIPlayerBrain& brain,
    const StrategicAIPlayerSnapshot& snapshot,
    uint64_t confirmedTick) noexcept {
    if (!brain.autonomousSkirmish ||
        confirmedTick < brain.nextEnemyReviewTick) {
        return;
    }
    brain.nextEnemyReviewTick = saturatingAdd(
        confirmedTick, m_config.enemyReviewIntervalTicks);

    const auto candidateFor = [&snapshot](PlayerId player)
        -> const StrategicAIEnemyCandidate* {
        const auto found = std::lower_bound(
            snapshot.enemyCandidates.begin(),
            snapshot.enemyCandidates.end(), player,
            [](const StrategicAIEnemyCandidate& candidate,
               PlayerId wanted) noexcept {
                return candidate.player < wanted;
            });
        return found != snapshot.enemyCandidates.end() &&
                found->player == player
            ? &*found : nullptr;
    };
    if (const StrategicAIEnemyCandidate* current =
            candidateFor(brain.currentEnemy);
        current && current->hasObjects && current->hasUnits &&
        current->hasBuildFacility) {
        return;
    }

    PlayerId best = INVALID_PLAYER_ID;
    uint64_t bestScore = std::numeric_limits<uint64_t>::max();
    for (const StrategicAIEnemyCandidate& candidate :
         snapshot.enemyCandidates) {
        if (!candidate.player || !candidate.hasObjects) continue;
        uint64_t score = 0;
        const bool crippled = !candidate.hasUnits ||
            !candidate.hasBuildFacility;
        if (crippled) {
            score = std::numeric_limits<uint64_t>::max() / 2u;
        } else {
            const math::q32_32 dx = candidate.centerX -
                snapshot.baseAnchorX;
            const math::q32_32 dy = candidate.centerY -
                snapshot.baseAnchorY;
            const math::q32_32 distance = dx * dx + dy * dy;
            score = distance.raw() > 0
                ? static_cast<uint64_t>(distance.raw()) : 0;
        }
        for (const auto& [otherPlayer, otherBrain] : m_brains) {
            if (otherPlayer == brain.player) continue;
            if (otherBrain.currentEnemy == candidate.player) {
                const uint64_t penalty = static_cast<uint64_t>(
                    math::q32_32{250000}.raw());
                score = penalty >
                        std::numeric_limits<uint64_t>::max() - score
                    ? std::numeric_limits<uint64_t>::max()
                    : score + penalty;
            }
            if (otherBrain.currentEnemy == brain.player) {
                const uint64_t preference = static_cast<uint64_t>(
                    math::q32_32{625}.raw());
                score = score > preference ? score - preference : 0;
            }
        }
        if (!best || score < bestScore ||
            (score == bestScore && candidate.player < best)) {
            best = candidate.player;
            bestScore = score;
        }
    }
    brain.currentEnemy = best;
}

bool StrategicAIRuntime::ensureAutomaticBuildPlan(
    StrategicAIPlayerBrain& brain,
    const StrategicAIPlayerSnapshot& snapshot) {
    if (!snapshot.hasUsableBuilder || !snapshot.hasBaseAnchor ||
        snapshot.structureOptions.empty()) {
        return false;
    }

    StrategicAIBuildRole role = StrategicAIBuildRole::Authored;
    if (snapshot.commandCenterCount == 0) {
        role = StrategicAIBuildRole::CommandCenter;
    } else if (snapshot.energyProduction < snapshot.energyConsumption) {
        role = StrategicAIBuildRole::Power;
    } else if (snapshot.supplyCenterCount == 0 ||
               snapshot.supplyExpansionNeeded) {
        role = StrategicAIBuildRole::Supply;
    } else if (snapshot.productionFacilityCount == 0) {
        role = StrategicAIBuildRole::Production;
    } else if (snapshot.baseThreatened && snapshot.baseDefenseCount < 2) {
        role = StrategicAIBuildRole::BaseDefense;
    } else {
        return false;
    }

    const bool alreadyPending = std::any_of(
        m_buildPlans.begin(), m_buildPlans.end(),
        [player = brain.player, role](
            const StrategicAIBuildPlan& plan) noexcept {
            return plan.player == player && plan.role == role &&
                plan.state != StrategicAIBuildState::Completed &&
                plan.state != StrategicAIBuildState::Exhausted;
        });
    if (alreadyPending) return false;

    const auto matchesRole = [role](
        const StrategicAIStructureOption& option) noexcept {
        switch (role) {
        case StrategicAIBuildRole::CommandCenter:
            return option.commandCenter;
        case StrategicAIBuildRole::Power:
            return option.energyProduction > 0;
        case StrategicAIBuildRole::Supply:
            return option.supplyCenter;
        case StrategicAIBuildRole::Production:
            return option.productionFacility;
        case StrategicAIBuildRole::BaseDefense:
            return option.baseDefense;
        case StrategicAIBuildRole::Authored:
            return false;
        }
        return false;
    };
    const StrategicAIStructureOption* selected = nullptr;
    for (const StrategicAIStructureOption& option :
         snapshot.structureOptions) {
        if (!option.builder || option.productType.empty() ||
            option.cost < 0 || !matchesRole(option)) {
            continue;
        }
        const bool optionPreferred = role ==
                StrategicAIBuildRole::BaseDefense &&
            !snapshot.preferredBaseDefenseStructure.empty() &&
            option.productType == snapshot.preferredBaseDefenseStructure;
        const bool selectedPreferred = selected && role ==
                StrategicAIBuildRole::BaseDefense &&
            !snapshot.preferredBaseDefenseStructure.empty() &&
            selected->productType == snapshot.preferredBaseDefenseStructure;
        const bool better = !selected ||
            (optionPreferred != selectedPreferred
                ? optionPreferred
                : option.cost < selected->cost ||
                    (option.cost == selected->cost &&
                     option.productType < selected->productType) ||
                    (option.cost == selected->cost &&
                     option.productType == selected->productType &&
                     option.builder < selected->builder));
        if (better) {
            selected = &option;
        }
    }
    if (!selected) return false;

    const bool useSupplyAnchor =
        role == StrategicAIBuildRole::Supply && snapshot.hasSupplyAnchor;
    StrategicAIBuildPlan plan{
        .player = brain.player,
        .objectType = selected->productType,
        .anchorX = selected->hasAuthoredPlacement
            ? snapshot.baseAnchorX + selected->authoredOffsetX
            : useSupplyAnchor ? snapshot.supplyAnchorX : snapshot.baseAnchorX,
        .anchorY = selected->hasAuthoredPlacement
            ? snapshot.baseAnchorY + selected->authoredOffsetY
            : useSupplyAnchor ? snapshot.supplyAnchorY : snapshot.baseAnchorY,
        .yawRadians = selected->hasAuthoredPlacement
            ? selected->authoredYawRadians : math::q32_32{},
        .sourceSideOrdinal = selected->authoredSideOrdinal,
        .sourceBuildListOrdinal = selected->authoredBuildOrdinal,
        .remainingRebuilds = -1,
        .expectedCost = selected->cost,
        .role = role,
    };
    const uint64_t id = addBuildPlan(std::move(plan));
    StrategicAIBuildPlan* inserted = findBuildPlan(id);
    if (!inserted) return false;

    // Preserve the supply-source anchor exactly. Other automatic buildings
    // use a stable square ring around the base so repeated power plants or
    // defenses do not all compete for the same placement cell.
    if (!useSupplyAnchor && !selected->hasAuthoredPlacement) {
        static constexpr int32_t directions[8][2] = {
            {1, 0}, {1, 1}, {0, 1}, {-1, 1},
            {-1, 0}, {-1, -1}, {0, -1}, {1, -1},
        };
        const uint64_t ordinal = id == 0 ? 0 : id - 1;
        const auto& direction = directions[ordinal % 8];
        const math::q32_32 distance =
            role == StrategicAIBuildRole::BaseDefense
                ? snapshot.baseRadius + m_config.baseDefenseExtraDistance
                : math::q32_32{
                      120 + static_cast<int32_t>((ordinal / 8) % 4) * 40};
        inserted->anchorX += math::q32_32{direction[0]} * distance;
        inserted->anchorY += math::q32_32{direction[1]} * distance;
    }
    return true;
}

void StrategicAIRuntime::reconcileTeam(
    StrategicAIPlayerBrain& brain,
    const StrategicAIPlayerSnapshot& snapshot,
    uint64_t confirmedTick) {
    const uint32_t desired = desiredTeamSize(brain.difficulty);
    const uint32_t desiredCount = desiredTeamCount(brain.difficulty);
    size_t existingCount = static_cast<size_t>(std::count_if(
        m_teams.begin(), m_teams.end(),
        [player = brain.player](const StrategicAITeam& team) noexcept {
            return team.player == player;
        }));
    while (existingCount < desiredCount && m_nextTeamId != 0) {
        m_teams.push_back({.id = m_nextTeamId++, .player = brain.player});
        ++existingCount;
    }

    container::Vector<StrategicAITeam*> teams;
    for (StrategicAITeam& team : m_teams) {
        if (team.player != brain.player) continue;
        team.members.erase(
            std::remove_if(
                team.members.begin(), team.members.end(),
                [&snapshot](ObjectId object) noexcept {
                    return !std::binary_search(
                        snapshot.liveCombatUnits.begin(),
                        snapshot.liveCombatUnits.end(), object);
                }),
            team.members.end());
        teams.push_back(&team);
    }
    std::sort(teams.begin(), teams.end(),
        [](const StrategicAITeam* left, const StrategicAITeam* right) {
            return left->id < right->id;
        });

    for (const ObjectId candidate : snapshot.idleCombatUnits) {
        const bool assigned = std::any_of(
            teams.begin(), teams.end(), [candidate](const StrategicAITeam* team) {
                return std::binary_search(
                    team->members.begin(), team->members.end(), candidate);
            });
        if (assigned) continue;
        const auto destination = std::min_element(
            teams.begin(), teams.end(),
            [](const StrategicAITeam* left, const StrategicAITeam* right) {
                return left->members.size() != right->members.size()
                    ? left->members.size() < right->members.size()
                    : left->id < right->id;
            });
        if (destination == teams.end() ||
            (*destination)->members.size() >= desired) break;
        (*destination)->members.push_back(candidate);
        std::sort((*destination)->members.begin(),
                  (*destination)->members.end());
    }
    for (StrategicAITeam* team : teams) {
        if (team->members.empty()) {
            team->state = StrategicAITeamState::Recovering;
            team->target = INVALID_OBJECT_ID;
        } else if (snapshot.baseThreatened) {
            if (team->state != StrategicAITeamState::Defending ||
                team->target != snapshot.threatTarget) {
                team->state = StrategicAITeamState::Ready;
            }
            team->target = snapshot.threatTarget;
        } else if (team->members.size() >= desired &&
                   snapshot.preferredEnemyTarget) {
            if (team->state != StrategicAITeamState::Attacking ||
                team->target != snapshot.preferredEnemyTarget) {
                team->state = StrategicAITeamState::Ready;
            }
            team->target = snapshot.preferredEnemyTarget;
        } else {
            team->state = StrategicAITeamState::Assembling;
            team->target = INVALID_OBJECT_ID;
        }
        if (team->nextOrderTick == 0) team->nextOrderTick = confirmedTick;
    }
}

void StrategicAIRuntime::reconcileWorkOrders(
    StrategicAIPlayerBrain& brain,
    const StrategicAIPlayerSnapshot& snapshot,
    uint64_t confirmedTick) {
    for (StrategicAIWorkOrder& order : m_workOrders) {
        if (order.player != brain.player ||
            order.state == StrategicAIWorkOrderState::Completed ||
            order.state == StrategicAIWorkOrderState::Exhausted) {
            continue;
        }
        bool objectiveSatisfied = false;
        switch (order.role) {
        case StrategicAIWorkOrderRole::BuilderRecovery:
            objectiveSatisfied = snapshot.hasBuilder;
            break;
        case StrategicAIWorkOrderRole::Gatherer:
            objectiveSatisfied = snapshot.harvesterCount >=
                snapshot.desiredGathererCount;
            break;
        case StrategicAIWorkOrderRole::CombatReinforcement: {
            size_t members = 0;
            for (const StrategicAITeam& team : m_teams) {
                if (team.player == brain.player)
                    members += team.members.size();
            }
            objectiveSatisfied = members >=
                static_cast<size_t>(desiredTeamSize(brain.difficulty)) *
                    desiredTeamCount(brain.difficulty);
            break;
        }
        }
        const bool producing =
            order.state == StrategicAIWorkOrderState::Producing;
        const bool producerAlive = producing && std::binary_search(
            snapshot.liveProducers.begin(), snapshot.liveProducers.end(),
            order.producer);
        const bool stillQueued = producing && order.productionId != 0 &&
            std::binary_search(
                snapshot.activeProductionHandles.begin(),
                snapshot.activeProductionHandles.end(),
                StrategicAIProductionHandle{
                    .producer = order.producer,
                    .productionId = order.productionId,
                });
        // RefCode completes the matching WorkOrder from onUnitProduced().
        // The aggregate snapshot is our acknowledgement boundary: once the
        // objective exists and the paid production handle has disappeared,
        // retaining the order until its retry deadline only invents a failure
        // and delays builder/gatherer recovery.  A handle that is still live
        // remains authoritative even if an unrelated unit satisfies the same
        // aggregate objective in the meantime.
        if (objectiveSatisfied && (!producing || !stillQueued)) {
            order.state = StrategicAIWorkOrderState::Completed;
            order.producer = INVALID_OBJECT_ID;
            order.productionId = 0;
            continue;
        }
        if (!producing) continue;
        if (!producerAlive ||
            (!stillQueued && confirmedTick >= order.nextAttemptTick)) {
            order.state = StrategicAIWorkOrderState::WaitingForProducer;
            order.producer = INVALID_OBJECT_ID;
            order.productionId = 0;
            if (order.failureCount !=
                std::numeric_limits<uint32_t>::max()) {
                ++order.failureCount;
            }
            order.nextAttemptTick = saturatingAdd(
                confirmedTick,
                productionDelay(brain, snapshot, order.failureCount));
        }
    }
    m_workOrders.erase(
        std::remove_if(
            m_workOrders.begin(), m_workOrders.end(),
            [](const StrategicAIWorkOrder& order) noexcept {
                return order.state == StrategicAIWorkOrderState::Completed ||
                    order.state == StrategicAIWorkOrderState::Exhausted;
            }),
        m_workOrders.end());
}

StrategicAIWorkOrder* StrategicAIRuntime::ensureWorkOrder(
    StrategicAIPlayerBrain& brain, StrategicAIWorkOrderRole role,
    const StrategicAIPlayerSnapshot& snapshot,
    uint64_t confirmedTick) {
    const auto existing = std::find_if(
        m_workOrders.begin(), m_workOrders.end(),
        [player = brain.player, role](
            const StrategicAIWorkOrder& order) noexcept {
            return order.player == player && order.role == role &&
                order.state != StrategicAIWorkOrderState::Completed &&
                order.state != StrategicAIWorkOrderState::Exhausted;
        });
    if (existing != m_workOrders.end()) return &*existing;
    if (m_nextWorkOrderId == 0) return nullptr;

    uint32_t required = 0;
    if (role == StrategicAIWorkOrderRole::BuilderRecovery) {
        required = snapshot.hasBuilder ? 0 : 1;
    } else if (role == StrategicAIWorkOrderRole::Gatherer) {
        const uint32_t desired = snapshot.desiredGathererCount;
        required = desired > snapshot.harvesterCount
            ? desired - snapshot.harvesterCount : 0;
    } else {
        uint32_t current = 0;
        for (const StrategicAITeam& team : m_teams) {
            if (team.player == brain.player) {
                current += static_cast<uint32_t>(team.members.size());
            }
        }
        const uint32_t desired = desiredTeamSize(brain.difficulty) *
            desiredTeamCount(brain.difficulty);
        required = desired > current ? desired - current : 0;
    }
    if (required == 0) return nullptr;

    const StrategicAIProductionOption* selected = nullptr;
    for (const StrategicAIProductionOption& option :
         snapshot.productionOptions) {
        const bool matches =
            (role == StrategicAIWorkOrderRole::BuilderRecovery
                ? option.builder
                : role == StrategicAIWorkOrderRole::Gatherer
                    ? option.harvester : option.combatUnit);
        if (!matches || !option.producer || option.productType.empty() ||
            option.cost < 0) {
            continue;
        }
        if (!selected || option.queueDepth < selected->queueDepth ||
            (option.queueDepth == selected->queueDepth &&
             option.cost < selected->cost) ||
            (option.queueDepth == selected->queueDepth &&
             option.cost == selected->cost &&
             option.productType < selected->productType) ||
            (option.queueDepth == selected->queueDepth &&
             option.cost == selected->cost &&
             option.productType == selected->productType &&
             option.producer < selected->producer)) {
            selected = &option;
        }
    }
    if (!selected) return nullptr;
    m_workOrders.push_back({
        .id = m_nextWorkOrderId++,
        .player = brain.player,
        .role = role,
        .productType = selected->productType,
        .requiredCount = required,
        .nextAttemptTick = confirmedTick,
    });
    return &m_workOrders.back();
}

void StrategicAIRuntime::update(
    container::Span<const StrategicAIPlayerSnapshot> snapshots,
    uint64_t confirmedTick,
    container::Vector<StrategicAIAction>& output) {
    output.clear();
    output.reserve(m_brains.size());
    for (auto& [player, brain] : m_brains) {
        const StrategicAIPlayerSnapshot* snapshot =
            findSnapshot(snapshots, player);
        if (!snapshot) continue;
        acquireEnemy(brain, *snapshot, confirmedTick);
        if (confirmedTick >= brain.nextEconomyTick) {
            updatePhase(brain, *snapshot);
            brain.nextEconomyTick = saturatingAdd(
                confirmedTick, m_config.economyIntervalTicks);
        }

        const bool hasAuthoredCombatTeams =
            !snapshot->teamProductionOptions.empty();
        if (brain.autonomousSkirmish) {
            if (hasAuthoredCombatTeams) {
                m_teams.erase(
                    std::remove_if(
                        m_teams.begin(), m_teams.end(),
                        [player](const StrategicAITeam& team) noexcept {
                            return team.player == player;
                        }),
                    m_teams.end());
            } else {
                reconcileTeam(brain, *snapshot, confirmedTick);
            }
            reconcileWorkOrders(brain, *snapshot, confirmedTick);
            if (hasAuthoredCombatTeams) {
                m_workOrders.erase(
                    std::remove_if(
                        m_workOrders.begin(), m_workOrders.end(),
                        [player](const StrategicAIWorkOrder& order) noexcept {
                            return order.player == player &&
                                order.role == StrategicAIWorkOrderRole::
                                    CombatReinforcement &&
                                order.state !=
                                    StrategicAIWorkOrderState::Producing;
                        }),
                    m_workOrders.end());
            }
            static_cast<void>(ensureAutomaticBuildPlan(brain, *snapshot));
        }

        StrategicAIBuildPlan* dueBuild = nullptr;
        for (StrategicAIBuildPlan& plan : m_buildPlans) {
            if (plan.player != player) continue;
            if (plan.state == StrategicAIBuildState::RebuildDelay &&
                confirmedTick >= plan.nextAttemptTick) {
                plan.state = StrategicAIBuildState::Unbuilt;
            }
            if (plan.state == StrategicAIBuildState::Unbuilt &&
                confirmedTick >= plan.nextAttemptTick) {
                if (snapshot->hasUsableBuilder &&
                    plan.expectedCost <= snapshot->cash) {
                    dueBuild = &plan;
                    break;
                }
            }
        }
        if (dueBuild && confirmedTick >= brain.nextStructureTick) {
            dueBuild->state = StrategicAIBuildState::Reserved;
            brain.nextStructureTick = saturatingAdd(
                confirmedTick, adjustedInterval(
                    m_config.structureIntervalTicks, snapshot->cash,
                    m_config.poor, m_config.wealthy,
                    m_config.structuresPoorRate,
                    m_config.structuresWealthyRate));
            output.push_back({
                .kind = StrategicAIActionKind::BuildStructure,
                .player = player,
                .sequence = nextSequence(brain),
                .buildPlanId = dueBuild->id,
                .productType = dueBuild->objectType,
            });
        }
        if (confirmedTick >= 2 && !snapshot->scienceOptions.empty()) {
            for (const container::String& science :
                 snapshot->scienceOptions) {
                output.push_back({
                    .kind = StrategicAIActionKind::PurchaseScience,
                    .player = player,
                    .sequence = nextSequence(brain),
                    .productType = science,
                });
            }
        }

        if (!brain.autonomousSkirmish) continue;

        if (brain.pendingBridgeRepair &&
            confirmedTick >= brain.nextBridgeRepairTick) {
            output.push_back({
                .kind = StrategicAIActionKind::RepairStructure,
                .player = player,
                .sequence = nextSequence(brain),
                .target = brain.pendingBridgeRepair,
            });
            brain.nextBridgeRepairTick = saturatingAdd(
                confirmedTick,
                std::max<uint64_t>(1, m_config.logicFramesPerSecond));
        }

        if (confirmedTick >= brain.nextTacticalTick) {
            brain.nextTacticalTick = saturatingAdd(
                confirmedTick, m_config.tacticalIntervalTicks);
            StrategicAITeam* team = nullptr;
            bool defending = false;
            bool retargetWholeTeam = false;
            ObjectId desiredTarget = INVALID_OBJECT_ID;
            for (StrategicAITeam& candidate : m_teams) {
                if (candidate.player != player || candidate.members.empty())
                    continue;
                const bool candidateDefending = snapshot->baseThreatened &&
                    snapshot->threatTarget && candidate.members.size() >= 2;
                const bool candidateAttacking = !candidateDefending &&
                    snapshot->preferredEnemyTarget &&
                    candidate.members.size() >=
                        desiredTeamSize(brain.difficulty);
                if (!candidateDefending && !candidateAttacking) continue;
                const ObjectId candidateTarget = candidateDefending
                    ? snapshot->threatTarget
                    : snapshot->preferredEnemyTarget;
                const StrategicAITeamState desiredState = candidateDefending
                    ? StrategicAITeamState::Defending
                    : StrategicAITeamState::Attacking;
                const bool candidateRetarget =
                    candidate.state != desiredState ||
                    candidate.target != candidateTarget;
                const bool hasIdleMember = std::any_of(
                    candidate.members.begin(), candidate.members.end(),
                    [snapshot](ObjectId member) noexcept {
                        return std::binary_search(
                            snapshot->idleCombatUnits.begin(),
                            snapshot->idleCombatUnits.end(), member);
                    });
                if (!candidateRetarget && !hasIdleMember) continue;
                if (!team || candidate.nextOrderTick < team->nextOrderTick ||
                    (candidate.nextOrderTick == team->nextOrderTick &&
                     candidate.id < team->id)) {
                    team = &candidate;
                    defending = candidateDefending;
                    retargetWholeTeam = candidateRetarget;
                    desiredTarget = candidateTarget;
                }
            }
            if (team) {
                team->state = defending
                    ? StrategicAITeamState::Defending
                    : StrategicAITeamState::Attacking;
                team->target = desiredTarget;
                team->nextOrderTick = brain.nextTacticalTick;
                StrategicAIAction action{
                    .kind = defending
                        ? StrategicAIActionKind::Defend
                        : StrategicAIActionKind::Attack,
                    .player = player,
                    .sequence = nextSequence(brain),
                    .strategicTeamId = team->id,
                    .target = team->target,
                };
                if (retargetWholeTeam) {
                    action.actors = team->members;
                } else {
                    for (const ObjectId member : team->members) {
                        if (std::binary_search(
                                snapshot->idleCombatUnits.begin(),
                                snapshot->idleCombatUnits.end(), member)) {
                            action.actors.push_back(member);
                        }
                    }
                }
                if (!action.actors.empty())
                    output.push_back(std::move(action));
            }
        }

        bool gathererRebindIssued = false;
        if (!snapshot->looseGatherers.empty()) {
            const auto center = std::find_if(
                snapshot->supplyCenters.begin(),
                snapshot->supplyCenters.end(),
                [](const StrategicAISupplyCenterSnapshot& candidate) {
                    return candidate.hasViableSupply &&
                        candidate.assignedGatherers <
                            candidate.desiredGatherers;
                });
            if (center != snapshot->supplyCenters.end()) {
                StrategicAIAction action{
                    .kind = StrategicAIActionKind::AssignGathererDock,
                    .player = player,
                    .sequence = nextSequence(brain),
                    .target = center->center,
                };
                action.actors.push_back(snapshot->looseGatherers.front());
                output.push_back(std::move(action));
                gathererRebindIssued = true;
            }
        }

        if (!snapshot->hasBuilder) {
            static_cast<void>(ensureWorkOrder(
                brain, StrategicAIWorkOrderRole::BuilderRecovery,
                *snapshot, confirmedTick));
        }
        if (snapshot->desiredGathererCount != 0) {
            static_cast<void>(ensureWorkOrder(
                brain, StrategicAIWorkOrderRole::Gatherer,
                *snapshot, confirmedTick));
        }
        if (!hasAuthoredCombatTeams) {
            static_cast<void>(ensureWorkOrder(
                brain, StrategicAIWorkOrderRole::CombatReinforcement,
                *snapshot, confirmedTick));
        }

        if (confirmedTick >= brain.nextProductionTick) {
            container::Vector<const StrategicAITeamProductionOption*>
                teamCandidates;
            int32_t highestTeamPriority =
                std::numeric_limits<int32_t>::min();
            for (const StrategicAITeamProductionOption& option :
                 snapshot->teamProductionOptions) {
                const bool belowLimit = option.maximumInstances > 0 &&
                    option.instanceCount < static_cast<uint32_t>(
                        option.maximumInstances);
                if (!option.definition || option.name.empty() ||
                    !option.conditionSatisfied || !belowLimit ||
                    option.assemblyInProgress ||
                    !option.buildableWithIdleFactory ||
                    snapshot->cash < teamStartCash(
                        option.estimatedCost,
                        m_config.teamResourcesToStart)) {
                    continue;
                }
                if (option.priority > highestTeamPriority) {
                    highestTeamPriority = option.priority;
                    teamCandidates.clear();
                }
                if (option.priority == highestTeamPriority)
                    teamCandidates.push_back(&option);
            }
            const StrategicAITeamReinforcementOption* reinforcement =
                nullptr;
            int32_t reinforcementPriority = highestTeamPriority;
            for (const StrategicAITeamReinforcementOption& option :
                 snapshot->teamReinforcementOptions) {
                if (!option.definition || !option.team ||
                    option.productType.empty() ||
                    option.priority <= reinforcementPriority) {
                    continue;
                }
                reinforcement = &option;
                reinforcementPriority = option.priority;
            }
            if (reinforcement) {
                brain.nextProductionTick = saturatingAdd(
                    confirmedTick,
                    productionDelay(
                        brain, *snapshot,
                        brain.consecutiveProductionFailures));
                output.push_back({
                    .kind = StrategicAIActionKind::ReinforceScenarioTeam,
                    .player = player,
                    .sequence = nextSequence(brain),
                    .productType = reinforcement->productType,
                    .scenarioTeam = reinforcement->definition,
                    .objectTeam = reinforcement->team,
                });
                continue;
            }
            if (!teamCandidates.empty()) {
                std::sort(teamCandidates.begin(), teamCandidates.end(),
                    [](const StrategicAITeamProductionOption* left,
                       const StrategicAITeamProductionOption* right) {
                        return left->definition < right->definition;
                    });
                const StrategicAITeamProductionOption& selected =
                    *teamCandidates[
                        snapshot->teamPriorityTieBreakIndex %
                        teamCandidates.size()];
                brain.nextProductionTick = saturatingAdd(
                    confirmedTick,
                    productionDelay(
                        brain, *snapshot,
                        brain.consecutiveProductionFailures));
                output.push_back({
                    .kind = StrategicAIActionKind::BuildScenarioTeam,
                    .player = player,
                    .sequence = nextSequence(brain),
                    .productType = selected.name,
                    .scenarioTeam = selected.definition,
                });
                continue;
            }

            StrategicAIWorkOrder* selectedOrder = nullptr;
            const StrategicAIProductionOption* selectedOption = nullptr;
            for (StrategicAIWorkOrder& order : m_workOrders) {
                if (order.player != player ||
                    order.state !=
                        StrategicAIWorkOrderState::WaitingForProducer ||
                    confirmedTick < order.nextAttemptTick ||
                    (hasAuthoredCombatTeams && order.role ==
                        StrategicAIWorkOrderRole::CombatReinforcement) ||
                    (gathererRebindIssued && order.role ==
                        StrategicAIWorkOrderRole::Gatherer)) {
                    continue;
                }
                const auto roleAccepts = [&order](
                    const StrategicAIProductionOption& option) noexcept {
                    switch (order.role) {
                    case StrategicAIWorkOrderRole::BuilderRecovery:
                        return option.builder;
                    case StrategicAIWorkOrderRole::Gatherer:
                        return option.harvester;
                    case StrategicAIWorkOrderRole::CombatReinforcement:
                        return option.combatUnit;
                    }
                    return false;
                };
                const bool currentProductAvailable = std::any_of(
                    snapshot->productionOptions.begin(),
                    snapshot->productionOptions.end(),
                    [&order, &roleAccepts](
                        const StrategicAIProductionOption& option) noexcept {
                        return roleAccepts(option) &&
                            option.productType == order.productType;
                    });
                if (!currentProductAvailable) {
                    const StrategicAIProductionOption* replacement = nullptr;
                    for (const StrategicAIProductionOption& option :
                         snapshot->productionOptions) {
                        if (!roleAccepts(option)) continue;
                        if (!replacement ||
                            option.queueDepth < replacement->queueDepth ||
                            (option.queueDepth == replacement->queueDepth &&
                             option.cost < replacement->cost) ||
                            (option.queueDepth == replacement->queueDepth &&
                             option.cost == replacement->cost &&
                             option.productType <
                                 replacement->productType) ||
                            (option.queueDepth == replacement->queueDepth &&
                             option.cost == replacement->cost &&
                             option.productType ==
                                 replacement->productType &&
                             option.producer < replacement->producer)) {
                            replacement = &option;
                        }
                    }
                    if (!replacement) continue;
                    order.productType = replacement->productType;
                    order.producer = INVALID_OBJECT_ID;
                    order.productionId = 0;
                }
                for (const StrategicAIProductionOption& option :
                     snapshot->productionOptions) {
                    if (option.productType != order.productType ||
                        !option.producer ||
                        option.cost < 0 || option.cost > snapshot->cash) {
                        continue;
                    }
                    if (order.role ==
                            StrategicAIWorkOrderRole::CombatReinforcement) {
                        const uint32_t remaining =
                            order.requiredCount - order.completedCount;
                        if (snapshot->cash < combatTeamStartCash(
                                option.cost, remaining,
                                m_config.teamResourcesToStart)) {
                            continue;
                        }
                    }
                    if (!selectedOption ||
                        option.queueDepth < selectedOption->queueDepth ||
                        (option.queueDepth == selectedOption->queueDepth &&
                         option.producer < selectedOption->producer)) {
                        selectedOrder = &order;
                        selectedOption = &option;
                    }
                }
                if (selectedOrder) break;
            }
            if (selectedOrder && selectedOption) {
                selectedOrder->state =
                    StrategicAIWorkOrderState::Producing;
                selectedOrder->producer = selectedOption->producer;
                selectedOrder->productionId = 0;
                selectedOrder->nextAttemptTick = saturatingAdd(
                    confirmedTick,
                    productionDelay(
                        brain, *snapshot, selectedOrder->failureCount));
                brain.nextProductionTick = saturatingAdd(
                    confirmedTick,
                    productionDelay(
                        brain, *snapshot,
                        brain.consecutiveProductionFailures));
                output.push_back({
                    .kind = StrategicAIActionKind::ProduceUnit,
                    .player = player,
                    .sequence = nextSequence(brain),
                    .workOrderId = selectedOrder->id,
                    .producer = selectedOption->producer,
                    .productType = selectedOrder->productType,
                });
                continue;
            }

            brain.nextProductionTick = saturatingAdd(
                confirmedTick,
                productionDelay(
                    brain, *snapshot,
                    brain.consecutiveProductionFailures));
        }
    }
}

bool StrategicAIRuntime::captureSnapshot(
    StrategicAIRuntimeSnapshot& output) const {
    try {
        StrategicAIRuntimeSnapshot candidate;
        candidate.config = m_config;
        candidate.brains = m_brains;
        candidate.buildPlans = m_buildPlans;
        candidate.workOrders = m_workOrders;
        candidate.teams = m_teams;
        candidate.teamConditions = m_teamConditions;
        candidate.nextBuildPlanId = m_nextBuildPlanId;
        candidate.nextWorkOrderId = m_nextWorkOrderId;
        candidate.nextTeamId = m_nextTeamId;
        output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool StrategicAIRuntime::restoreSnapshot(
    const StrategicAIRuntimeSnapshot& snapshot) {
    if (snapshot.schemaVersion != StrategicAIRuntimeSnapshot::SchemaVersion ||
        snapshot.config.logicFramesPerSecond == 0 ||
        snapshot.config.economyIntervalTicks == 0 ||
        snapshot.config.structureIntervalTicks == 0 ||
        snapshot.config.productionIntervalTicks == 0 ||
        snapshot.config.tacticalIntervalTicks == 0 ||
        snapshot.config.enemyReviewIntervalTicks == 0 ||
        snapshot.config.rebuildDelayTicks == 0 ||
        snapshot.config.teamResourcesToStart < math::q32_32{} ||
        snapshot.config.maximumPlayers == 0 ||
        snapshot.config.maximumBuildPlans == 0 ||
        snapshot.brains.size() > snapshot.config.maximumPlayers ||
        snapshot.buildPlans.size() > snapshot.config.maximumBuildPlans ||
        snapshot.workOrders.size() >
            static_cast<size_t>(snapshot.config.maximumPlayers) * 3u ||
        snapshot.teams.size() >
            static_cast<size_t>(snapshot.config.maximumPlayers) * 3u ||
        !std::is_sorted(
            snapshot.teamConditions.begin(),
            snapshot.teamConditions.end(),
            [](const StrategicAITeamConditionState& left,
               const StrategicAITeamConditionState& right) {
                return left.definition < right.definition;
            }) ||
        std::adjacent_find(
            snapshot.teamConditions.begin(),
            snapshot.teamConditions.end(),
            [](const StrategicAITeamConditionState& left,
               const StrategicAITeamConditionState& right) {
                return left.definition == right.definition;
            }) != snapshot.teamConditions.end() ||
        std::any_of(
            snapshot.teamConditions.begin(),
            snapshot.teamConditions.end(),
            [](const StrategicAITeamConditionState& state) {
                return !state.definition ||
                    (state.permanentlyUnavailable && state.value);
            })) {
        return false;
    }
    for (const auto& [player, brain] : snapshot.brains) {
        if (!player || player.isNeutral() ||
            player.value >= PLAYER_REGISTRY_CAPACITY ||
            brain.player != player ||
            brain.difficulty == AiDifficulty::None ||
            brain.currentEnemy == player ||
            (brain.currentEnemy &&
             brain.currentEnemy.value >= PLAYER_REGISTRY_CAPACITY) ||
            static_cast<uint8_t>(brain.phase) >
                static_cast<uint8_t>(StrategicAIPhase::Recover)) {
            return false;
        }
    }
    uint64_t previous = 0;
    for (const StrategicAIBuildPlan& plan : snapshot.buildPlans) {
        if (plan.id == 0 || plan.id <= previous || !plan.player ||
            snapshot.brains.find(plan.player) == snapshot.brains.end() ||
            plan.objectType.empty() || plan.expectedCost < 0 ||
            plan.remainingRebuilds < -1 ||
            static_cast<uint8_t>(plan.state) >
                static_cast<uint8_t>(StrategicAIBuildState::Exhausted) ||
            static_cast<uint8_t>(plan.role) >
                static_cast<uint8_t>(StrategicAIBuildRole::BaseDefense)) {
            return false;
        }
        previous = plan.id;
    }
    const auto validNext = [](uint64_t next, uint64_t maximum) noexcept {
        return maximum == std::numeric_limits<uint64_t>::max()
            ? next == 0 : next > maximum;
    };
    if (!validNext(snapshot.nextBuildPlanId, previous)) return false;

    previous = 0;
    for (const StrategicAIWorkOrder& order : snapshot.workOrders) {
        if (order.id == 0 || order.id <= previous || !order.player ||
            snapshot.brains.find(order.player) == snapshot.brains.end() ||
            order.productType.empty() || order.requiredCount == 0 ||
            order.completedCount > order.requiredCount ||
            (order.state == StrategicAIWorkOrderState::Producing &&
             (!order.producer || order.productionId == 0)) ||
            (order.state != StrategicAIWorkOrderState::Producing &&
             (order.producer || order.productionId != 0)) ||
            static_cast<uint8_t>(order.role) > static_cast<uint8_t>(
                StrategicAIWorkOrderRole::CombatReinforcement) ||
            static_cast<uint8_t>(order.state) > static_cast<uint8_t>(
                StrategicAIWorkOrderState::Exhausted)) {
            return false;
        }
        previous = order.id;
    }
    if (!validNext(snapshot.nextWorkOrderId, previous)) return false;

    previous = 0;
    for (size_t teamIndex = 0; teamIndex < snapshot.teams.size();
         ++teamIndex) {
        const StrategicAITeam& team = snapshot.teams[teamIndex];
        if (team.id == 0 || team.id <= previous || !team.player ||
            team.player.value >= PLAYER_REGISTRY_CAPACITY ||
            snapshot.brains.find(team.player) == snapshot.brains.end() ||
            static_cast<uint8_t>(team.state) >
                static_cast<uint8_t>(StrategicAITeamState::Recovering) ||
            !std::is_sorted(team.members.begin(), team.members.end()) ||
            std::adjacent_find(team.members.begin(), team.members.end()) !=
                team.members.end() ||
            std::any_of(team.members.begin(), team.members.end(),
                        [](ObjectId member) noexcept { return !member; })) {
            return false;
        }
        for (size_t priorIndex = 0; priorIndex < teamIndex; ++priorIndex) {
            const StrategicAITeam& prior = snapshot.teams[priorIndex];
            if (prior.player != team.player) continue;
            size_t left = 0;
            size_t right = 0;
            while (left < prior.members.size() &&
                   right < team.members.size()) {
                if (prior.members[left] == team.members[right]) return false;
                if (prior.members[left] < team.members[right]) ++left;
                else ++right;
            }
        }
        previous = team.id;
    }
    if (!validNext(snapshot.nextTeamId, previous)) return false;

    try {
        StrategicAIRuntime candidate;
        candidate.m_config = snapshot.config;
        candidate.m_brains = snapshot.brains;
        candidate.m_buildPlans = snapshot.buildPlans;
        candidate.m_workOrders = snapshot.workOrders;
        candidate.m_teams = snapshot.teams;
        candidate.m_teamConditions = snapshot.teamConditions;
        candidate.m_nextBuildPlanId = snapshot.nextBuildPlanId;
        candidate.m_nextWorkOrderId = snapshot.nextWorkOrderId;
        candidate.m_nextTeamId = snapshot.nextTeamId;
        *this = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

uint64_t StrategicAIRuntime::stableHash() const noexcept {
    uint64_t result = 14695981039346656037ull;
    const auto byte = [&result](uint8_t value) noexcept {
        result ^= value;
        result *= 1099511628211ull;
    };
    const auto u32 = [&byte](uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
    };
    const auto u64 = [&byte](uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
    };
    const auto string = [&u64, &byte](container::StringView value) noexcept {
        u64(static_cast<uint64_t>(value.size()));
        for (const char character : value)
            byte(static_cast<uint8_t>(character));
    };

    u32(StrategicAIRuntimeSnapshot::SchemaVersion);
    u32(m_config.logicFramesPerSecond);
    u32(m_config.economyIntervalTicks);
    u32(m_config.structureIntervalTicks);
    u32(m_config.productionIntervalTicks);
    u32(m_config.tacticalIntervalTicks);
    u32(m_config.enemyReviewIntervalTicks);
    u32(m_config.rebuildDelayTicks);
    u32(m_config.maximumPlayers);
    u32(m_config.maximumBuildPlans);
    u64(static_cast<uint64_t>(m_config.wealthy.raw()));
    u64(static_cast<uint64_t>(m_config.poor.raw()));
    u64(static_cast<uint64_t>(m_config.structuresWealthyRate.raw()));
    u64(static_cast<uint64_t>(m_config.structuresPoorRate.raw()));
    u64(static_cast<uint64_t>(m_config.teamsWealthyRate.raw()));
    u64(static_cast<uint64_t>(m_config.teamsPoorRate.raw()));
    u64(static_cast<uint64_t>(m_config.teamResourcesToStart.raw()));
    u64(static_cast<uint64_t>(m_config.baseDefenseExtraDistance.raw()));
    u64(m_nextBuildPlanId);
    u64(m_nextWorkOrderId);
    u64(m_nextTeamId);
    u64(static_cast<uint64_t>(m_brains.size()));
    for (const auto& [player, brain] : m_brains) {
        byte(player.value);
        byte(static_cast<uint8_t>(brain.difficulty));
        byte(static_cast<uint8_t>(brain.phase));
        u64(brain.nextEconomyTick);
        u64(brain.nextStructureTick);
        u64(brain.nextProductionTick);
        u64(brain.nextTacticalTick);
        u64(brain.nextEnemyReviewTick);
        u32(brain.nextSequence);
        u32(brain.consecutiveProductionFailures);
        byte(brain.autonomousSkirmish ? uint8_t{1} : uint8_t{0});
        byte(brain.currentEnemy.value);
        u32(brain.pendingBridgeRepair.value);
        u64(brain.nextBridgeRepairTick);
    }
    u64(static_cast<uint64_t>(m_teamConditions.size()));
    for (const StrategicAITeamConditionState& state : m_teamConditions) {
        u32(state.definition.value);
        u64(state.nextEvaluationTick);
        byte(state.value ? uint8_t{1} : uint8_t{0});
        byte(state.permanentlyUnavailable ? uint8_t{1} : uint8_t{0});
    }
    u64(static_cast<uint64_t>(m_buildPlans.size()));
    for (const StrategicAIBuildPlan& plan : m_buildPlans) {
        u64(plan.id);
        byte(plan.player.value);
        string(plan.objectType);
        u64(static_cast<uint64_t>(plan.anchorX.raw()));
        u64(static_cast<uint64_t>(plan.anchorY.raw()));
        u64(static_cast<uint64_t>(plan.yawRadians.raw()));
        string(plan.scriptName);
        u32(plan.sourceSideOrdinal);
        u32(plan.sourceBuildListOrdinal);
        byte(static_cast<uint8_t>(plan.state));
        u32(plan.reservedBuilder.value);
        u32(plan.constructedObject.value);
        u64(plan.nextAttemptTick);
        u32(plan.attemptCount);
        u32(static_cast<uint32_t>(plan.remainingRebuilds));
        u64(static_cast<uint64_t>(plan.expectedCost));
        byte(static_cast<uint8_t>(plan.role));
    }
    u64(static_cast<uint64_t>(m_workOrders.size()));
    for (const StrategicAIWorkOrder& order : m_workOrders) {
        u64(order.id);
        byte(order.player.value);
        byte(static_cast<uint8_t>(order.role));
        byte(static_cast<uint8_t>(order.state));
        string(order.productType);
        u32(order.producer.value);
        u32(order.productionId);
        u32(order.completedCount);
        u32(order.requiredCount);
        u32(order.failureCount);
        u64(order.nextAttemptTick);
    }
    u64(static_cast<uint64_t>(m_teams.size()));
    for (const StrategicAITeam& team : m_teams) {
        u64(team.id);
        byte(team.player.value);
        byte(static_cast<uint8_t>(team.state));
        u32(team.target.value);
        u64(team.nextOrderTick);
        u64(static_cast<uint64_t>(team.members.size()));
        for (const ObjectId member : team.members) u32(member.value);
    }
    return result;
}

} // namespace engine
