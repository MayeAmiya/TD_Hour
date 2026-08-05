#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "ScriptRuntime.h"


#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

namespace engine::script
{
namespace
{

[[nodiscard]] int32_t saturatedAdd(int32_t lhs, int32_t rhs) noexcept
{
    const int64_t sum = static_cast<int64_t>(lhs) + static_cast<int64_t>(rhs);
    return static_cast<int32_t>(
        std::clamp<int64_t>(sum, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

[[nodiscard]] bool finiteVec3(const math::vec3& value) noexcept
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

[[nodiscard]] uint64_t nextEvaluationTick(uint64_t current, uint32_t delay) noexcept
{
    if (delay == 0)
        return current;
    const uint64_t delta = static_cast<uint64_t>(delay);
    return current > std::numeric_limits<uint64_t>::max() - delta ? std::numeric_limits<uint64_t>::max()
                                                           : current + delta;
}

// Runtime state is queried for every scheduled script/group and again for
// enable/disable/call actions. Normal compiler IDs are compact, but the
// public builder intentionally permits sparse uint32 IDs for tools and
// imported content. Match ScriptProgram's bounded dense-index policy: direct
// lookup for compact ranges and an unordered lookup table otherwise, while
// retaining vectors as the only iteration/storage order.
constexpr size_t kMaximumDenseRuntimeStateIndexEntries = 1u << 20;
constexpr size_t kMaximumDenseRuntimeStateSlotsPerEntry = 8;

[[nodiscard]] bool shouldUseDenseRuntimeStateIndex(uint32_t maximumId,
                                                    size_t stateCount) noexcept
{
    if (stateCount == 0)
        return false;
    const uint64_t entries = static_cast<uint64_t>(maximumId) + 1u;
    return entries <= kMaximumDenseRuntimeStateIndexEntries &&
           entries <= static_cast<uint64_t>(stateCount) *
                          kMaximumDenseRuntimeStateSlotsPerEntry;
}
[[nodiscard]] bool compare(int64_t lhs, ScriptComparison comparison, int64_t rhs) noexcept
{
    switch (comparison)
    {
    case ScriptComparison::Less:
        return lhs < rhs;
    case ScriptComparison::LessEqual:
        return lhs <= rhs;
    case ScriptComparison::Equal:
        return lhs == rhs;
    case ScriptComparison::GreaterEqual:
        return lhs >= rhs;
    case ScriptComparison::Greater:
        return lhs > rhs;
    case ScriptComparison::NotEqual:
        return lhs != rhs;
    }
    return false;
}

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] bool isThisPlayerReference(container::StringView value) noexcept
{
    return equalAsciiInsensitive(value, "ThisPlayer") ||
           equalAsciiInsensitive(value, "<This Player>");
}

[[nodiscard]] bool isThisPlayerEnemyReference(
    container::StringView value) noexcept
{
    return equalAsciiInsensitive(value, "<This Player's Enemy>");
}

[[nodiscard]] bool isThisObjectReference(container::StringView value) noexcept
{
    return equalAsciiInsensitive(value, "<This Object>");
}

[[nodiscard]] bool isThisTeamReference(container::StringView value) noexcept
{
    return equalAsciiInsensitive(value, "<This Team>");
}

} // namespace

bool ScriptRuntime::evaluate(const ScriptDefinition& definition) const
{
    // An absent OR list is false. Empty AND clauses are also false, matching
    // the original evaluator's explicit `if (!firstAnd) continue` path.
    for (const ScriptAndClause& clause : definition.anyOf)
    {
        if (clause.allOf.empty())
            continue;
        bool allTrue = true;
        for (const ScriptCondition& condition : clause.allOf)
        {
            if (!evaluateCondition(condition))
            {
                allTrue = false;
                break;
            }
        }
        if (allTrue)
            return true;
    }
    return false;
}

std::optional<ScriptRuntime::NamedConditionEvaluation>
ScriptRuntime::evaluateNamedConditionForPlayer(
    container::StringView name, PlayerId player)
{
    if (!m_program || name.empty() || !player) return std::nullopt;
    const ScriptDefinition* definition = m_program->findScriptByName(name);
    if (!definition) return std::nullopt;
    const ScriptDifficulty effectiveDifficulty =
        difficultyForPlayer(player).value_or(m_difficulty);
    if (!definition->difficulties.includes(effectiveDifficulty))
        return NamedConditionEvaluation{
            .value = false,
            .difficultyAllowed = false,
            .evaluationDelayTicks = definition->evaluationDelayTicks,
        };

    const ScriptInvocationContext savedInvocation = m_currentInvocation;
    const container::String savedPlayerAlias =
        std::move(m_currentPlayerAlias);
    const std::optional<ScriptDifficulty> savedDifficulty =
        m_currentDifficultyOverride;
    m_currentInvocation = {
        .currentPlayer = player,
        .origin = ScriptInvocationOrigin::Automatic,
    };
    m_currentPlayerAlias.clear();
    m_currentDifficultyOverride = effectiveDifficulty;
    const bool value = evaluate(*definition);
    m_currentInvocation = savedInvocation;
    m_currentPlayerAlias = savedPlayerAlias;
    m_currentDifficultyOverride = savedDifficulty;
    return NamedConditionEvaluation{
        .value = value,
        .difficultyAllowed = true,
        .evaluationDelayTicks = definition->evaluationDelayTicks,
    };
}

bool ScriptRuntime::evaluateCondition(const ScriptCondition& condition) const
{
    return std::visit(
        [this](const auto& value) -> bool
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, ScriptAlwaysTrueCondition>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptAlwaysFalseCondition>)
            {
                return false;
            }
            else if constexpr (std::is_same_v<Value, ScriptCounterCondition>)
            {
                const ScriptCounterState* state = counter(value.counterSymbol);
                return compare(state ? state->value : 0, value.comparison, value.value);
            }
            else if constexpr (std::is_same_v<Value, ScriptFlagCondition>)
            {
                const RuntimeFlagState* state = flag(value.flagSymbol);
                return (state ? state->value : false) == value.expectedValue;
            }
        else if constexpr (std::is_same_v<Value, ScriptTimerExpiredCondition>)
        {
            const ScriptCounterState* state = counter(value.timerSymbol);
            return state && state->countdownTimerRunning && state->value <= 0;
        }
        else if constexpr (std::is_same_v<Value, ScriptNamedObjectStateCondition>)
        {
            if (!m_context.world)
                return false;
            ScriptWorldNamedObjectState state = ScriptWorldNamedObjectState::Unknown;
            if (isThisObjectReference(value.objectName)) {
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.objectName);
                state = object
                    ? m_context.world->objectState(object->id)
                    : ScriptWorldNamedObjectState::Destroyed;
            } else {
                // Static named selectors retain the destroyed-name history
                // needed by TOTALLY_DEAD; a dynamic ObjectId has no alias
                // history and observes only its current lifecycle.
                state = m_context.world->namedObjectState(value.objectName);
            }
            switch (value.expected)
            {
            case ScriptNamedObjectExpectation::Present:
                return state == ScriptWorldNamedObjectState::Alive ||
                       state == ScriptWorldNamedObjectState::Dying;
            case ScriptNamedObjectExpectation::Alive:
                return state == ScriptWorldNamedObjectState::Alive;
            case ScriptNamedObjectExpectation::Dying:
                return state == ScriptWorldNamedObjectState::Dying;
            case ScriptNamedObjectExpectation::TotallyDead:
                return state == ScriptWorldNamedObjectState::Destroyed;
            case ScriptNamedObjectExpectation::Destroyed:
                return state == ScriptWorldNamedObjectState::Dying ||
                       state == ScriptWorldNamedObjectState::Destroyed;
            }
            return false;
        }
        else if constexpr (std::is_same_v<Value, ScriptNamedSelectedCondition>)
        {
            const std::optional<ScriptWorldObjectSnapshot> object =
                resolveObject(value.objectName);
            return object && m_context.world->objectSelected(object->id);
        }
        else if constexpr (std::is_same_v<
                               Value,
                               ScriptMultiplayerOutcomeCondition>)
        {
            return m_context.world &&
                m_context.world->multiplayerOutcome(value.kind);
        }
        else if constexpr (std::is_same_v<Value, ScriptTeamCommandButtonReadyCondition>)
        {
            const std::optional<ObjectTeamId> team = resolveTeam(value.teamName);
            return team && m_context.world->teamCommandButtonReady(
                *team, value.commandButton, value.allReady);
        }
        else if constexpr (std::is_same_v<Value, ScriptUnitHealthCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<ScriptWorldObjectSnapshot> object =
                resolveObject(value.objectName);
            if (!object || !object->alive || !object->health)
                return false;

            const ScriptWorldObjectHealthSnapshot& health = *object->health;
            // This deliberately mirrors ScriptConditions::evaluateUnitHealth:
            //   Int((current * 100 + initial / 2) / initial)
            // The original assumes a live Body with valid initial health.
            // Modern snapshots can also represent an Inactive/no-Body object.
            // Keep the original rounding expression in the authoritative
            // fixed domain; no float Body projection is read back here.
            if (health.initial <= math::q32_32{}) return false;
            const math::q32_32 legacyPercent =
                (health.current * math::q32_32{int32_t{100}} +
                 health.initial / math::q32_32{int32_t{2}}) /
                health.initial;
            return compare(
                legacyPercent.to_int(), value.comparison, value.percent);
        }
        else if constexpr (std::is_same_v<Value, ScriptObjectStatusCondition>)
        {
            if (!m_context.world)
                return false;
            if (!value.team) {
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.targetName);
                return object && m_context.world->objectHasAnyStatus(
                    object->id, value.statusMask);
            }
            const std::optional<ObjectTeamId> team = resolveTeam(value.targetName);
            if (!team) return false;
            const ScriptWorldTeamStatusSummary summary =
                m_context.world->teamStatusSummary(*team, value.statusMask);
            if (!summary.exists || !summary.membersValid)
                return false;
            return value.entireTeam ? summary.matching == summary.members
                                    : summary.matching != 0;
        }
        else if constexpr (std::is_same_v<Value, ScriptNamedContainmentCondition>)
        {
            if (!m_context.world)
                return false;
            switch (value.expected)
            {
            case ScriptNamedContainmentExpectation::Empty:
            {
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.objectName);
                return object && m_context.world->objectContainmentIsEmpty(object->id);
            }
            case ScriptNamedContainmentExpectation::HasFreeSlots:
            {
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.objectName);
                return object && m_context.world->objectContainmentHasFreeSlots(object->id);
            }
            }
            return false;
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerSpecialPowerReadyCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return player && m_context.world->playerSpecialPowerReady(
                *player, value.specialPower);
        }
        else if constexpr (std::is_same_v<Value, ScriptSpecialPowerEventCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player)
                return false;
            ObjectId source = INVALID_OBJECT_ID;
            if (!value.sourceObject.empty()) {
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.sourceObject);
                if (!object)
                    return false;
                source = object->id;
            }
            return m_context.world->consumeSpecialPowerEvent(
                value.phase, *player, value.specialPower, source);
        }
        else if constexpr (std::is_same_v<Value, ScriptUpgradeEventCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player)
                return false;
            ObjectId source = INVALID_OBJECT_ID;
            if (!value.sourceObject.empty()) {
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.sourceObject);
                if (!object)
                    return false;
                source = object->id;
            }
            return m_context.world->consumeUpgradeEvent(
                *player, value.upgrade, source);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerGarrisonedCountCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return player && compare(
                m_context.world->playerGarrisonedBuildingCount(*player),
                value.comparison, static_cast<int64_t>(value.count));
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerCapturedUnitCountCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return player && compare(
                m_context.world->playerCapturedUnitCount(*player),
                value.comparison, static_cast<int64_t>(value.count));
        }
        else if constexpr (std::is_same_v<Value, ScriptSuppliesWithinDistanceCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return player && m_context.world->suppliesWithinDistance(
                *player, value.areaName, value.extraDistance, value.minimumValue);
        }
        else if constexpr (std::is_same_v<Value, ScriptDiscoveryCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> observer = resolvePlayer(value.observer);
            if (!observer)
                return false;
            switch (value.kind)
            {
            case ScriptDiscoverySubjectKind::NamedObject:
            {
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.subject);
                return object && m_context.world->objectDiscovered(
                    object->id, *observer);
            }
            case ScriptDiscoverySubjectKind::Team:
            {
                const std::optional<ObjectTeamId> team = resolveTeam(value.subject);
                return team && m_context.world->teamDiscovered(*team, *observer);
            }
            case ScriptDiscoverySubjectKind::Player:
            {
                const std::optional<PlayerId> subject = resolvePlayer(value.subject);
                return subject && m_context.world->playerDiscovered(*subject, *observer);
            }
            }
            return false;
        }
        else if constexpr (std::is_same_v<Value, ScriptSightedRelationshipCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.targetPlayer);
            const std::optional<ScriptWorldObjectSnapshot> source =
                resolveObject(value.sourceObject);
            return player && source && m_context.world->objectSeesPlayerByRelationship(
                source->id, value.relationship, *player);
        }
        else if constexpr (std::is_same_v<Value, ScriptSightedObjectTypeCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.targetPlayer);
            if (!player)
                return false;
            const std::optional<container::Span<const container::String>> list =
                objectTypeList(value.objectType);
            const container::Span<const container::String> types = list
                ? *list
                : container::Span<const container::String>{&value.objectType, 1};
            const std::optional<ScriptWorldObjectSnapshot> source =
                resolveObject(value.sourceObject);
            return source && m_context.world->objectSeesPlayerObjectTypes(
                source->id, *player, types);
        }
        else if constexpr (std::is_same_v<Value, ScriptAttackedCondition>)
        {
            if (!m_context.world)
                return false;
            if (value.matcherKind == ScriptAttackedMatcherKind::Player) {
                const std::optional<PlayerId> player = resolvePlayer(value.matcher);
                if (!player) return false;
                if (value.team) {
                    const std::optional<ObjectTeamId> team = resolveTeam(value.target);
                    return team && m_context.world->teamLastAttackedByPlayer(
                        *team, *player);
                }
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.target);
                return object && m_context.world->objectLastAttackedByPlayer(
                    object->id, *player);
            }
            const std::optional<container::Span<const container::String>> list =
                objectTypeList(value.matcher);
            const container::Span<const container::String> types = list
                ? *list
                : container::Span<const container::String>{&value.matcher, 1};
            if (value.team) {
                const std::optional<ObjectTeamId> team = resolveTeam(value.target);
                return team && m_context.world->teamLastAttackedByObjectTypes(
                    *team, types);
            }
            const std::optional<ScriptWorldObjectSnapshot> object =
                resolveObject(value.target);
            return object && m_context.world->objectLastAttackedByObjectTypes(
                object->id, types);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerAttackedByPlayerCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> victim = resolvePlayer(value.victimPlayer);
            const std::optional<PlayerId> attacker = resolvePlayer(value.attackerPlayer);
            return victim && attacker &&
                m_context.world->playerWasAttackedBy(*victim, *attacker);
        }
        else if constexpr (std::is_same_v<Value, ScriptBridgeTransitionCondition>)
        {
            const std::optional<ScriptWorldObjectSnapshot> object =
                resolveObject(value.bridgeObject);
            return object && m_context.world->bridgeTransitionObserved(
                object->id, value.broken);
        }
        else if constexpr (std::is_same_v<Value, ScriptUnitEmptiedCondition>)
        {
            const std::optional<ScriptWorldObjectSnapshot> object =
                resolveObject(value.objectName);
            return object && m_context.world->unitEmptied(object->id);
        }
        else if constexpr (std::is_same_v<Value, ScriptBuildingEnteredCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            const std::optional<ScriptWorldObjectSnapshot> building =
                resolveObject(value.buildingObject);
            return player && building && m_context.world->buildingEnteredByPlayer(
                building->id, *player);
        }
        else if constexpr (std::is_same_v<Value, ScriptAreaTransitionCondition>)
        {
            if (!m_context.world)
                return false;
            if (value.team) {
                const std::optional<ObjectTeamId> team = resolveTeam(value.target);
                return team && m_context.world->teamAreaTransition(
                    *team, value.areaName, value.allowedSurfaces,
                    value.kind, value.entireTeam);
            }
            const std::optional<ScriptWorldObjectSnapshot> object =
                resolveObject(value.target);
            return object && m_context.world->objectAreaTransition(
                object->id, value.areaName, value.kind);
        }
        else if constexpr (std::is_same_v<Value, ScriptCameraMovementFinishedCondition>)
        {
            return !m_context.world || m_context.world->cameraMovementFinished();
        }
        else if constexpr (std::is_same_v<Value, ScriptPresentationCompletionCondition>)
        {
            return m_context.world && m_context.world->consumePresentationCompletion(
                value.kind, value.mediaName);
        }
        else if constexpr (std::is_same_v<Value, ScriptMusicTrackCompletedCondition>)
        {
            return m_context.world && m_context.world->musicTrackHasCompleted(
                value.trackName, value.minimumCompletedLoops);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerCashCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player)
                return false;
            const std::optional<int64_t> cash = m_context.world->playerCash(*player);
            // See ScriptPlayerCashCondition: legacy comparison has the
            // authored amount on the left and the live money count on right.
            return cash && compare(value.value, value.comparison, *cash);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerPowerCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player)
            {
                // PLAYER_HAS_NO_POWER literally negates evaluatePlayerHasPower
                // in RefCode, so an unresolved player is true only for that
                // one negative form.
                return value.kind == ScriptPlayerPowerConditionKind::HasInsufficientPower;
            }
            const std::optional<ScriptWorldPlayerEnergySnapshot> energy =
                m_context.world->playerEnergy(*player);
            if (!energy)
                return value.kind == ScriptPlayerPowerConditionKind::HasInsufficientPower;

            switch (value.kind)
            {
            case ScriptPlayerPowerConditionKind::HasSufficientPower:
                return energy->sufficient;
            case ScriptPlayerPowerConditionKind::HasInsufficientPower:
                return !energy->sufficient;
            case ScriptPlayerPowerConditionKind::SupplyPercent:
            {
                // RefCode compares production/consumption to percent/100 as
                // floats, with zero consumption producing the raw production
                // value. Cross multiplication preserves that ordering while
                // keeping confirmed simulation integer deterministic.
                const int64_t lhs = static_cast<int64_t>(energy->effectiveProduction) * 100;
                const int64_t rhs = energy->consumption == 0
                    ? static_cast<int64_t>(value.value)
                    : static_cast<int64_t>(value.value) * energy->consumption;
                return compare(lhs, value.comparison, rhs);
            }
            case ScriptPlayerPowerConditionKind::ExcessValue:
                return compare(static_cast<int64_t>(energy->effectiveProduction) -
                                   static_cast<int64_t>(energy->consumption),
                               value.comparison, value.value);
            }
            return false;
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerSciencePurchasePointsCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player)
                return false;
            const std::optional<int32_t> points =
                m_context.world->playerSciencePurchasePoints(*player);
            return points && *points >= value.minimumPoints;
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerScienceAcquiredCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return player && m_context.world->consumePlayerScienceAcquired(*player, value.science);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerCanPurchaseScienceCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return player && m_context.world->playerCanPurchaseScience(*player, value.science);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerCanBuildObjectCondition>)
        {
            if (!m_context.world) return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return player && m_context.world->playerCanBuildObjectType(
                *player, value.objectType);
        }
        else if constexpr (std::is_same_v<Value, ScriptWaypointPathCompletedCondition>)
        {
            if (!m_context.world) return false;
            if (value.team) {
                const std::optional<ObjectTeamId> team = resolveTeam(value.target);
                return team && m_context.world->teamCompletedWaypointPath(
                    *team, value.pathName);
            }
            const std::optional<ScriptWorldObjectSnapshot> object =
                resolveObject(value.target);
            return object && m_context.world->objectCompletedWaypointPath(
                object->id, value.pathName);
        }
        else if constexpr (std::is_same_v<Value, ScriptSupplySourceSafeCondition>)
        {
            if (!m_context.world) return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return player && m_context.world->playerSupplySourceSafe(
                *player, value.minimumSupplies);
        }
        else if constexpr (std::is_same_v<Value, ScriptSupplySourceAttackedCondition>)
        {
            if (!m_context.world) return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return player && m_context.world->playerSupplySourceAttacked(*player);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerAllDestroyedCondition>)
        {
            if (!m_context.world)
                return false;
            // RefCode defines a missing player as already all destroyed.  A
            // real world query is still required: an unbound standalone
            // runtime must not accidentally win a mission merely because it
            // has no session roster installed.
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player)
                return true;
            const ScriptWorldPlayerObjectSummary summary =
                m_context.world->playerObjectSummary(*player);
            return !summary.playerExists || !summary.hasLegacyCountedObject;
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerAllBuildFacilitiesDestroyedCondition>)
        {
            if (!m_context.world) return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return !player || !m_context.world->playerHasAnyBuildFacility(*player);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerBuildingCountCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player)
                return false;
            const ScriptWorldPlayerObjectSummary summary =
                m_context.world->playerObjectSummary(*player);
            if (!summary.playerExists)
                return false;
            const uint32_t count = value.kind == ScriptPlayerBuildingCountKind::VictoryStructures
                ? summary.victoryStructureCount
                : summary.structureCount;
            // The authored bound is signed.  Comparing in int64 space avoids
            // turning a negative map value into a huge unsigned threshold.
            return static_cast<int64_t>(count) <= static_cast<int64_t>(value.maximumCount);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerStartPositionCondition>)
        {
            if (!m_context.world || value.authoredPosition <= 0)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player)
                return false;
            const std::optional<int32_t> startPosition =
                m_context.world->playerStartPosition(*player);
            // Authored positions are 1-based while PlayerRegistry stores the
            // canonical zero-based map-layout index. Use int64 for the
            // subtraction so malformed INT32_MIN content cannot overflow.
            return startPosition && static_cast<int64_t>(*startPosition) ==
                static_cast<int64_t>(value.authoredPosition) - 1;
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerFactionCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player)
                return false;
            const std::optional<container::StringView> faction =
                m_context.world->playerFaction(*player);
            // Faction identifiers originate from INI/map text. Treat their
            // spelling case-insensitively at the modern value boundary while
            // retaining the authored value in the immutable program.
            return faction && equalAsciiInsensitive(*faction, value.faction);
        }
        else if constexpr (std::is_same_v<Value, ScriptTriggerAreaExistsCondition>)
        {
            return m_context.world && m_context.world->triggerAreaExists(value.areaName);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerAreaCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player)
                return false;

            ScriptWorldPlayerAreaMetric metric = ScriptWorldPlayerAreaMetric::EligibleObjectCount;
            if (value.kind == ScriptPlayerAreaConditionKind::MatchingKindCount)
                metric = ScriptWorldPlayerAreaMetric::MatchingKindCount;
            else if (value.kind == ScriptPlayerAreaConditionKind::BuildValue)
                metric = ScriptWorldPlayerAreaMetric::BuildValue;
            const ScriptWorldPlayerAreaSummary summary = m_context.world->playerAreaSummary(
                *player, value.areaName, metric, value.requiredKind);
            if (!summary.playerExists || !summary.areaExists)
                return false;

            switch (value.kind)
            {
            case ScriptPlayerAreaConditionKind::MatchingKindCount:
            case ScriptPlayerAreaConditionKind::BuildValue:
                return compare(summary.value, value.comparison, value.value);
            case ScriptPlayerAreaConditionKind::HasEligibleObjects:
                return summary.value > 0;
            case ScriptPlayerAreaConditionKind::HasNoEligibleObjects:
                return summary.value == 0;
            }
            return false;
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerObjectTypeAreaCountCondition>)
        {
            if (!m_context.world) return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player) return false;
            container::Vector<container::String> objectTypes;
            if (const std::optional<container::Span<const container::String>> list =
                    objectTypeList(value.objectType)) {
                objectTypes.assign(list->begin(), list->end());
            } else {
                objectTypes.push_back(value.objectType);
            }
            const std::optional<int64_t> count =
                m_context.world->playerObjectTypeCountInArea(
                    *player, value.areaName,
                    container::Span<const container::String>{objectTypes});
            return count && compare(*count, value.comparison, value.value);
        }
        else if constexpr (std::is_same_v<Value, ScriptPlayerObjectTypeCountCondition>)
        {
            if (!m_context.world) return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!player) return false;

            container::Vector<container::String> objectTypes;
            if (value.kind == ScriptPlayerObjectTypeCountKind::BuiltByPlayer) {
                // This legacy condition calls findTemplate directly; an
                // ObjectTypeList operand is not accepted here.
                if (!m_context.world->concreteObjectTypeExists(value.objectType))
                    return false;
                objectTypes.push_back(value.objectType);
            } else if (const std::optional<container::Span<const container::String>> list =
                           objectTypeList(value.objectType)) {
                objectTypes.assign(list->begin(), list->end());
            } else {
                objectTypes.push_back(value.objectType);
            }

            const bool includeEffectivelyDead =
                value.kind != ScriptPlayerObjectTypeCountKind::LostSincePreviousEvaluation;
            const int64_t current = m_context.world->playerObjectTypeCount(
                *player, container::Span<const container::String>{objectTypes},
                includeEffectivelyDead);
            if (value.kind == ScriptPlayerObjectTypeCountKind::BuiltByPlayer)
                return current > 0;
            if (value.kind == ScriptPlayerObjectTypeCountKind::CurrentComparison)
                return compare(current, value.comparison, value.value);

            const auto position = std::lower_bound(
                m_objectTypeCountBaselines.begin(),
                m_objectTypeCountBaselines.end(),
                std::pair{*player, container::StringView{value.objectType}},
                [](const ObjectTypeCountBaseline& entry,
                   const std::pair<PlayerId, container::StringView>& key) {
                    if (entry.player != key.first)
                        return entry.player.value < key.first.value;
                    return entry.objectType < key.second;
                });
            const int64_t previous = position != m_objectTypeCountBaselines.end() &&
                    position->player == *player &&
                    position->objectType == value.objectType
                ? position->count : 0;
            const bool lost = current < previous;
            if (position != m_objectTypeCountBaselines.end() &&
                position->player == *player &&
                position->objectType == value.objectType) {
                position->count = current;
            } else {
                m_objectTypeCountBaselines.insert(position, {
                    .player = *player,
                    .objectType = value.objectType,
                    .count = current,
                });
            }
            return lost;
        }
        else if constexpr (std::is_same_v<Value, ScriptTechBuildingWithinDistanceCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            return player && m_context.world->techBuildingWithinDistance(
                *player, value.areaName, value.extraDistance);
        }
        else if constexpr (std::is_same_v<Value, ScriptNeutralUnmannedCountCondition>)
        {
            return m_context.world && compare(
                m_context.world->neutralUnmannedObjectCount(), value.comparison, value.value);
        }
        else if constexpr (std::is_same_v<Value, ScriptNamedObjectOwnerCondition>)
        {
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!m_context.world || !player)
                return false;
            const std::optional<ScriptWorldObjectSnapshot> object = resolveObject(value.objectName);
            return object && object->alive && object->owner == *player;
        }
        else if constexpr (std::is_same_v<Value, ScriptTeamOwnerCondition>)
        {
            const std::optional<PlayerId> player = resolvePlayer(value.player);
            if (!m_context.world || !player)
                return false;
            const std::optional<ObjectTeamId> team = resolveTeam(value.teamName);
            if (!team) return false;
            const std::optional<PlayerId> owner = m_context.world->teamOwner(*team);
            return owner && *owner == *player;
        }
        else if constexpr (std::is_same_v<Value, ScriptTeamStateCondition>)
        {
            if (!m_context.world)
                return false;
            const std::optional<ObjectTeamId> team = resolveTeam(value.teamName);
            if (!team) return false;
            const ScriptWorldTeamSummary summary = m_context.world->teamSummary(*team);
            if (!summary.exists)
                return false;
            switch (value.expected)
            {
            case ScriptTeamStateExpectation::HasUnits:
                return summary.hasUnits;
            case ScriptTeamStateExpectation::Destroyed:
                return !summary.hasObjects;
            case ScriptTeamStateExpectation::Created:
                return summary.createdThisTick;
            }
            return false;
        }
        else if constexpr (std::is_same_v<Value, ScriptTeamCustomStateCondition>)
        {
            if (!m_context.world) return false;
            const std::optional<ObjectTeamId> team = resolveTeam(value.team);
            if (!team) return false;
            const std::optional<container::StringView> state =
                m_context.world->teamScriptState(*team);
            if (!state) return false;
            const bool equal = *state == value.state;
            return value.negated ? !equal : equal;
        }
        else if constexpr (std::is_same_v<Value, ScriptNamedAreaCondition>)
        {
            if (!m_context.world)
                return value.expected == ScriptAreaExpectation::Outside;
            const std::optional<ScriptWorldObjectSnapshot> object =
                resolveObject(value.objectName);
            const bool inside = object && m_context.world->objectInsideArea(
                object->id, value.areaName);
            return value.expected == ScriptAreaExpectation::Inside ? inside : !inside;
        }
        else if constexpr (std::is_same_v<Value, ScriptTeamAreaCondition>)
        {
            const std::optional<ObjectTeamId> team = resolveTeam(value.teamName);
            const ScriptWorldTeamAreaSummary summary = team
                ? m_context.world->teamAreaSummary(
                    *team, value.areaName, value.allowedSurfaces)
                : ScriptWorldTeamAreaSummary{};
            const bool allInside = summary.teamExists && summary.areaExists &&
                summary.considered != 0 && summary.inside == summary.considered;
            const bool someInsideSomeOutside = summary.teamExists && summary.areaExists &&
                summary.considered != 0 && summary.inside != 0 && summary.inside != summary.considered;
            switch (value.expected)
            {
            case ScriptTeamAreaExpectation::AnyInside:
                return allInside || someInsideSomeOutside;
            case ScriptTeamAreaExpectation::EntirelyInside:
                return allInside;
            case ScriptTeamAreaExpectation::EntirelyOutside:
                // RefCode literally negates EntirelyInside || Partially;
                // missing team/area and zero eligible members are therefore
                // considered outside.
                return !(allInside || someInsideSomeOutside);
            }
            return false;
        }
        return false;
        },
        condition);
}

} // namespace engine::script
