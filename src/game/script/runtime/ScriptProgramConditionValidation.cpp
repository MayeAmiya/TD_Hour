#include "ScriptProgramValidationInternal.h"
#include "game/script/contracts/ScriptPresentationLimits.h"

#include <cmath>
#include <type_traits>

namespace engine::script::detail
{

[[nodiscard]] bool validCondition(const ScriptCondition& condition,
                                  container::Vector<ScriptProgramBuildIssue>* issues,
                                  container::StringView scriptName)
{
    return std::visit(
        [&](const auto& value) -> bool
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, ScriptAlwaysTrueCondition> ||
                          std::is_same_v<Value, ScriptAlwaysFalseCondition>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptCounterCondition>)
            {
                if (!value.counter.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) + "' has a counter condition with an empty counter name");
            }
            else if constexpr (std::is_same_v<Value, ScriptFlagCondition>)
            {
                if (!value.flag.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) + "' has a flag condition with an empty flag name");
            }
            else if constexpr (std::is_same_v<Value, ScriptTimerExpiredCondition>)
            {
                if (!value.timer.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) + "' has a timer condition with an empty timer name");
            }
            else if constexpr (std::is_same_v<Value, ScriptNamedObjectStateCondition>)
            {
                if (!value.objectName.empty())
                    return true;
                addIssue(issues,
                          "script '" + container::String(scriptName) + "' has a named-object condition with an empty name");
            }
            else if constexpr (std::is_same_v<Value, ScriptNamedSelectedCondition>)
            {
                if (!value.objectName.empty()) return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has a selected-object condition with an empty name");
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptMultiplayerOutcomeCondition>)
            {
                switch (value.kind)
                {
                case ScriptMultiplayerOutcomeKind::AlliedVictory:
                case ScriptMultiplayerOutcomeKind::AlliedDefeat:
                case ScriptMultiplayerOutcomeKind::PlayerDefeat:
                    return true;
                }
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid multiplayer-outcome condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptTeamCommandButtonReadyCondition>)
            {
                if (!value.teamName.empty() && !value.commandButton.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid team command-button readiness condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptUnitHealthCondition>)
            {
                if (!value.objectName.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) + "' has a unit-health condition with an empty name");
            }
            else if constexpr (std::is_same_v<Value, ScriptObjectStatusCondition>)
            {
                if (!value.targetName.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid object-status condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptNamedContainmentCondition>)
            {
                if (!value.objectName.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid named-containment condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerSpecialPowerReadyCondition>)
            {
                if (!value.player.empty() && !value.specialPower.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid special-power-ready condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptSpecialPowerEventCondition>)
            {
                if (!value.player.empty() && !value.specialPower.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid special-power-event condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptUpgradeEventCondition>)
            {
                if (!value.player.empty() && !value.upgrade.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid upgrade-event condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerGarrisonedCountCondition>)
            {
                if (!value.player.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has a garrison-count condition with an empty player");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerCapturedUnitCountCondition>)
            {
                if (!value.player.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has a captured-unit-count condition with an empty player");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerCanBuildObjectCondition>)
            {
                if (!value.player.empty() && !value.objectType.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid player-build-prerequisite condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptWaypointPathCompletedCondition>)
            {
                if (!value.target.empty() && !value.pathName.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid waypoint-completion condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptSupplySourceSafeCondition> ||
                               std::is_same_v<Value, ScriptSupplySourceAttackedCondition>)
            {
                if (!value.player.empty()) return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has a supply-policy condition with an empty player");
            }
            else if constexpr (std::is_same_v<Value, ScriptSuppliesWithinDistanceCondition>)
            {
                if (!value.player.empty() && !value.areaName.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid supplies-within-distance condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptDiscoveryCondition>)
            {
                if (!value.subject.empty() && !value.observer.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid discovery condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptSightedRelationshipCondition>)
            {
                if (!value.sourceObject.empty() && !value.targetPlayer.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid relationship-sighted condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptSightedObjectTypeCondition>)
            {
                if (!value.sourceObject.empty() && !value.targetPlayer.empty() &&
                    !value.objectType.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid object-type-sighted condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptAttackedCondition>)
            {
                if (!value.target.empty() && !value.matcher.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid attacked condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerAttackedByPlayerCondition>)
            {
                if (!value.victimPlayer.empty() && !value.attackerPlayer.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid player-attacked history condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptBridgeTransitionCondition>)
            {
                if (!value.bridgeObject.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has a bridge-transition condition with an empty object");
            }
            else if constexpr (std::is_same_v<Value, ScriptUnitEmptiedCondition>)
            {
                if (!value.objectName.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has a unit-emptied condition with an empty object");
            }
            else if constexpr (std::is_same_v<Value, ScriptBuildingEnteredCondition>)
            {
                if (!value.player.empty() && !value.buildingObject.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid building-entered condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptAreaTransitionCondition>)
            {
                if (!value.target.empty() && !value.areaName.empty() &&
                    (!value.team || (value.allowedSurfaces >= 1 &&
                                     value.allowedSurfaces <= 3)))
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid area-transition condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptCameraMovementFinishedCondition>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptPresentationCompletionCondition>)
            {
                const bool validKind = value.kind == ScriptPresentationCompletionKind::Video ||
                    value.kind == ScriptPresentationCompletionKind::Speech ||
                    value.kind == ScriptPresentationCompletionKind::Audio;
                if (validKind && !value.mediaName.empty() &&
                    value.mediaName.size() <= kMaximumScriptPresentationNameLength &&
                    value.mediaName.find('\0') == container::String::npos) {
                    return true;
                }
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid presentation-completion condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptMusicTrackCompletedCondition>)
            {
                if (!value.trackName.empty() &&
                    value.trackName.size() <= kMaximumScriptPresentationNameLength &&
                    value.trackName.find('\0') == container::String::npos) {
                    return true;
                }
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has a music-completion condition with an invalid track name");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerCashCondition>)
            {
                if (!value.player.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) + "' has a player-cash condition with an empty player");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerPowerCondition>)
            {
                if (value.player.empty())
                {
                    addIssue(issues,
                             "script '" + container::String(scriptName) +
                                 "' has a player-power condition with an empty player");
                    return false;
                }
                switch (value.kind)
                {
                case ScriptPlayerPowerConditionKind::HasSufficientPower:
                case ScriptPlayerPowerConditionKind::HasInsufficientPower:
                case ScriptPlayerPowerConditionKind::SupplyPercent:
                case ScriptPlayerPowerConditionKind::ExcessValue:
                    return true;
                }
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an unsupported player-power condition kind");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerSciencePurchasePointsCondition>)
            {
                if (!value.player.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has a science-purchase-points condition with an empty player");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerScienceAcquiredCondition>)
            {
                if (!value.player.empty() && !value.science.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid science-acquired condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerCanPurchaseScienceCondition>)
            {
                if (!value.player.empty() && !value.science.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid can-purchase-science condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerAllDestroyedCondition>)
            {
                if (!value.player.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an all-destroyed condition with an empty player");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerAllBuildFacilitiesDestroyedCondition>)
            {
                if (!value.player.empty()) return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an empty build-facility player reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerBuildingCountCondition>)
            {
                if (value.player.empty())
                {
                    addIssue(issues,
                             "script '" + container::String(scriptName) +
                                 "' has a building-count condition with an empty player");
                    return false;
                }
                switch (value.kind)
                {
                case ScriptPlayerBuildingCountKind::AllStructures:
                case ScriptPlayerBuildingCountKind::VictoryStructures:
                    return true;
                }
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid building-count condition kind");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerStartPositionCondition>)
            {
                if (!value.player.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has a start-position condition with an empty player");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerFactionCondition>)
            {
                if (!value.player.empty() && !value.faction.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid player-faction condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptTriggerAreaExistsCondition>)
            {
                if (!value.areaName.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an area-exists condition with an empty name");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerAreaCondition>)
            {
                bool kindValid = false;
                switch (value.kind)
                {
                case ScriptPlayerAreaConditionKind::MatchingKindCount:
                    kindValid = !value.requiredKind.empty();
                    break;
                case ScriptPlayerAreaConditionKind::BuildValue:
                case ScriptPlayerAreaConditionKind::HasEligibleObjects:
                case ScriptPlayerAreaConditionKind::HasNoEligibleObjects:
                    kindValid = true;
                    break;
                }
                if (!value.player.empty() && !value.areaName.empty() && kindValid)
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid player-area condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerObjectTypeAreaCountCondition>)
            {
                if (!value.player.empty() && !value.objectType.empty() &&
                    !value.areaName.empty()) return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid player object-type area-count condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerObjectTypeCountCondition>)
            {
                if (!value.player.empty() && !value.objectType.empty()) {
                    switch (value.kind) {
                    case ScriptPlayerObjectTypeCountKind::BuiltByPlayer:
                    case ScriptPlayerObjectTypeCountKind::CurrentComparison:
                    case ScriptPlayerObjectTypeCountKind::LostSincePreviousEvaluation:
                        return true;
                    }
                }
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid player object-type count condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptTechBuildingWithinDistanceCondition>)
            {
                if (!value.player.empty() && !value.areaName.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid tech-building-within-distance condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptNeutralUnmannedCountCondition>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptNamedObjectOwnerCondition>)
            {
                if (!value.objectName.empty() && !value.player.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) + "' has an invalid named-object owner condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptTeamOwnerCondition>)
            {
                if (!value.teamName.empty() && !value.player.empty())
                    return true;
                addIssue(issues,
                          "script '" + container::String(scriptName) + "' has an invalid team owner condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptTeamStateCondition>)
            {
                if (!value.teamName.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) + "' has a team-state condition with an empty name");
            }
            else if constexpr (std::is_same_v<Value, ScriptTeamCustomStateCondition>)
            {
                if (value.team.valid() && !value.state.empty()) return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) +
                             "' has an invalid Team custom-state condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptNamedAreaCondition>)
            {
                if (!value.objectName.empty() && !value.areaName.empty())
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) + "' has an invalid named-area condition");
            }
            else if constexpr (std::is_same_v<Value, ScriptTeamAreaCondition>)
            {
                if (!value.teamName.empty() && !value.areaName.empty() &&
                    value.allowedSurfaces >= 1 && value.allowedSurfaces <= 3)
                    return true;
                addIssue(issues,
                         "script '" + container::String(scriptName) + "' has an invalid team-area condition");
            }
            return false;
        },
        condition);
}

} // namespace engine::script::detail
#include "game/script/contracts/ScriptPresentationLimits.h"
