#include "ScriptProgramValidationInternal.h"

#include <algorithm>
#include <type_traits>

namespace engine::script::detail
{

void normalizeSymbols(container::Vector<container::String>& symbols)
{
    std::sort(symbols.begin(), symbols.end());
    symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
}

[[nodiscard]] ScriptRuntimeSymbolId symbolId(container::Span<const container::String> symbols,
                                              container::StringView name) noexcept
{
    const auto found = std::lower_bound(symbols.begin(), symbols.end(), name,
        [](const container::String& entry, container::StringView needle) { return entry < needle; });
    if (found == symbols.end() || *found != name)
        return INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
    return {.value = static_cast<uint32_t>(std::distance(symbols.begin(), found) + 1)};
}

void collectRuntimeSymbols(const ScriptDefinition& definition,
                           container::Vector<container::String>& counters,
                           container::Vector<container::String>& flags)
{
    for (const ScriptAndClause& clause : definition.anyOf)
    {
        for (const ScriptCondition& condition : clause.allOf)
        {
            std::visit([&](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, ScriptCounterCondition>)
                    counters.push_back(value.counter);
                else if constexpr (std::is_same_v<Value, ScriptFlagCondition>)
                    flags.push_back(value.flag);
                else if constexpr (std::is_same_v<Value, ScriptTimerExpiredCondition>)
                    counters.push_back(value.timer);
            }, condition);
        }
    }

    const auto collectActions = [&](const container::Vector<ScriptAction>& actions) {
        for (const ScriptAction& action : actions)
        {
            std::visit([&](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, ScriptSetFlagAction>)
                    flags.push_back(value.flag);
                else if constexpr (std::is_same_v<Value, ScriptSetCounterAction> ||
                                   std::is_same_v<Value, ScriptAdjustCounterAction>)
                    counters.push_back(value.counter);
                else if constexpr (std::is_same_v<Value, ScriptSetTimerAction> ||
                                   std::is_same_v<Value, ScriptAdjustTimerAction> ||
                                   std::is_same_v<Value, ScriptSetRandomTimerAction> ||
                                   std::is_same_v<Value, ScriptStopTimerAction> ||
                                   std::is_same_v<Value, ScriptRestartTimerAction>)
                    counters.push_back(value.timer);
            }, action);
        }
    };
    collectActions(definition.thenActions);
    collectActions(definition.elseActions);
}

void assignRuntimeSymbols(ScriptDefinition& definition,
                          container::Span<const container::String> counters,
                          container::Span<const container::String> flags)
{
    for (ScriptAndClause& clause : definition.anyOf)
    {
        for (ScriptCondition& condition : clause.allOf)
        {
            std::visit([&](auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, ScriptCounterCondition>)
                    value.counterSymbol = symbolId(counters, value.counter);
                else if constexpr (std::is_same_v<Value, ScriptFlagCondition>)
                    value.flagSymbol = symbolId(flags, value.flag);
                else if constexpr (std::is_same_v<Value, ScriptTimerExpiredCondition>)
                    value.timerSymbol = symbolId(counters, value.timer);
            }, condition);
        }
    }

    const auto assignActions = [&](container::Vector<ScriptAction>& actions) {
        for (ScriptAction& action : actions)
        {
            std::visit([&](auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, ScriptSetFlagAction>)
                    value.flagSymbol = symbolId(flags, value.flag);
                else if constexpr (std::is_same_v<Value, ScriptSetCounterAction> ||
                                   std::is_same_v<Value, ScriptAdjustCounterAction>)
                    value.counterSymbol = symbolId(counters, value.counter);
                else if constexpr (std::is_same_v<Value, ScriptSetTimerAction> ||
                                   std::is_same_v<Value, ScriptAdjustTimerAction> ||
                                   std::is_same_v<Value, ScriptSetRandomTimerAction> ||
                                   std::is_same_v<Value, ScriptStopTimerAction> ||
                                   std::is_same_v<Value, ScriptRestartTimerAction>)
                    value.timerSymbol = symbolId(counters, value.timer);
            }, action);
        }
    };
    assignActions(definition.thenActions);
    assignActions(definition.elseActions);
}

[[nodiscard]] size_t directEffectCount(const ScriptAction& action) noexcept
{
    return std::visit([](const auto& value) noexcept -> size_t {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, ScriptPlayAudioAction>) {
            return value.subtitleLabel.empty() ? 1u : 2u;
        }
        return std::is_same_v<Value, ScriptVictoryAction> ||
               std::is_same_v<Value, ScriptSequentialTimedAction> ||
               std::is_same_v<Value, ScriptDefeatAction> ||
               std::is_same_v<Value, ScriptDisplayTextAction> ||
               std::is_same_v<Value, ScriptDisplayCinematicTextAction> ||
                std::is_same_v<Value, ScriptMilitaryCaptionAction> ||
                std::is_same_v<Value, ScriptMovieAction> ||
                std::is_same_v<Value, ScriptMusicAction> ||
                std::is_same_v<Value, ScriptAmbientAudioAction> ||
                std::is_same_v<Value, ScriptAudioControlAction> ||
                std::is_same_v<Value, ScriptTimeControlAction> ||
                std::is_same_v<Value, ScriptHulkLifetimeOverrideAction> ||
                std::is_same_v<Value, ScriptScoreAccumulationPolicyAction> ||
                std::is_same_v<Value, ScriptVisualSpeedAction> ||
                std::is_same_v<Value, ScriptUiAction> ||
                std::is_same_v<Value, ScriptClientOptionsAction> ||
                std::is_same_v<Value, ScriptMapPresentationAction> ||
                std::is_same_v<Value, ScriptObjectPresentationAction> ||
                std::is_same_v<Value, ScriptForceObjectSelectionAction> ||
                std::is_same_v<Value, ScriptViewCompatibilityAction> ||
                std::is_same_v<Value, ScriptCameraAction> ||
                std::is_same_v<Value, ScriptCameraSlaveAction> ||
               std::is_same_v<Value, ScriptScreenShakeAction> ||
               std::is_same_v<Value, ScriptLocalizedCameraShakeAction> ||
                std::is_same_v<Value, ScriptScreenFadeAction> ||
                std::is_same_v<Value, ScriptBlackAndWhiteAction> ||
                std::is_same_v<Value, ScriptMotionBlurAction> ||
                std::is_same_v<Value, ScriptSkyboxAction> ||
                std::is_same_v<Value, ScriptTreeSwayAction> ||
                std::is_same_v<Value, ScriptWeatherAction> ||
                std::is_same_v<Value, ScriptInfantryLightingAction> ||
               std::is_same_v<Value, ScriptWaterAction> ||
               std::is_same_v<Value, ScriptSetTeamCustomStateAction> ||
               std::is_same_v<Value, ScriptIssueOrderAction> ||
               std::is_same_v<Value,
                              ScriptFireWeaponFollowingWaypointPathAction> ||
               std::is_same_v<Value,
                              ScriptCreateReinforcementTeamAction> ||
               std::is_same_v<Value, ScriptBuildTeamAction> ||
               std::is_same_v<Value, ScriptGuardSupplyCenterAction> ||
               std::is_same_v<Value, ScriptRecruitTeamAction> ||
               std::is_same_v<Value, ScriptUseCommandButtonAction> ||
               std::is_same_v<Value, ScriptFacingAction> ||
               std::is_same_v<Value, ScriptAIBehaviorMutationAction> ||
               std::is_same_v<Value, ScriptAttackPriorityMutationAction> ||
               std::is_same_v<Value, ScriptStoppingDistanceAction> ||
               std::is_same_v<Value, ScriptMoveTowardsNearestAction> ||
               std::is_same_v<Value, ScriptSpecialPowerCountdownAction> ||
               std::is_same_v<Value, ScriptWarehouseValueAction> ||
               std::is_same_v<Value, ScriptCaveIndexAction> ||
               std::is_same_v<Value, ScriptCreateObjectAction> ||
               std::is_same_v<Value, ScriptDestroyNamedObjectAction> ||
               std::is_same_v<Value, ScriptLifecycleAction> ||
               std::is_same_v<Value, ScriptContainmentAction> ||
               std::is_same_v<Value, ScriptContainmentEnterAction> ||
               std::is_same_v<Value, ScriptTransferOwnershipAction> ||
               std::is_same_v<Value, ScriptDamageAction> ||
               std::is_same_v<Value, ScriptGrantObjectUpgradeAction> ||
               std::is_same_v<Value, ScriptObjectStateMutationAction> ||
               std::is_same_v<Value, ScriptGlobalObjectAction> ||
               std::is_same_v<Value, ScriptBoobyTrapAction> ||
               std::is_same_v<Value, ScriptToppleDirectionAction> ||
               std::is_same_v<Value, ScriptSetPlayerCashAction> ||
               std::is_same_v<Value, ScriptAdjustPlayerCashAction> ||
               std::is_same_v<Value, ScriptPlayerSellEverythingAction> ||
               std::is_same_v<Value, ScriptPlayerRepairStructureAction> ||
               std::is_same_v<Value, ScriptPlayerBuildUpgradeAction> ||
               std::is_same_v<Value, ScriptPlayerBuildObjectNearTeamAction> ||
               std::is_same_v<Value, ScriptPlayerBuildSupplyCenterAction> ||
               std::is_same_v<Value, ScriptSkirmishBuildBuildingAction> ||
               std::is_same_v<Value, ScriptSkirmishApproachAction> ||
               std::is_same_v<Value, ScriptSkirmishPerimeterBuildAction> ||
               std::is_same_v<Value, ScriptSkirmishFireSpecialPowerAtMostCostAction> ||
               std::is_same_v<Value, ScriptSkirmishAttackNearestValueGroupAction> ||
               std::is_same_v<Value, ScriptSkirmishMostValuableCommandButtonAction> ||
               std::is_same_v<Value, ScriptPlayerConstructionAction> ||
               std::is_same_v<Value, ScriptObjectBuildabilityAction> ||
               std::is_same_v<Value, ScriptSetPlayerScienceAvailabilityAction> ||
               std::is_same_v<Value, ScriptSetPlayerRelationshipAction> ||
               std::is_same_v<Value, ScriptRelationshipOverrideAction> ||
               std::is_same_v<Value, ScriptGlobalCombatPolicyAction> ||
               std::is_same_v<Value, ScriptPlayerProgressionAction> ? 1u : 0u;
    }, action);
}

[[nodiscard]] size_t countDirectEffects(const ScriptDefinition& definition) noexcept
{
    const auto count = [](const container::Vector<ScriptAction>& actions) noexcept {
        size_t result = 0;
        for (const ScriptAction& action : actions)
            result += directEffectCount(action);
        return result;
    };
    return count(definition.thenActions) + count(definition.elseActions);
}

} // namespace engine::script::detail
