#include "GameSessionScriptPortDetail.h"
#include "GameSessionScriptAuthorityPort.h"
#include "game/session/state/GameSessionDomainState.h"

#include "debug/debug.h"
#include "game/base/GameBalanceConstants.h"
#include "game/base/DamageTypes.h"
#include "game/base/GameCameraDirector.h"
#include "game/base/GameSettings.h"
#include "game/audio/GameAudioEvents.h"
#include "game/command/CommandButtonStore.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/RankInfoCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace engine::script {
namespace {
[[nodiscard]] ScienceAvailability toScienceAvailability(
    ScriptScienceAvailability availability) noexcept {
    switch (availability) {
    case ScriptScienceAvailability::Available: return ScienceAvailability::Available;
    case ScriptScienceAvailability::Disabled: return ScienceAvailability::Disabled;
    case ScriptScienceAvailability::Hidden: return ScienceAvailability::Hidden;
    }
    return ScienceAvailability::Available;
}

[[nodiscard]] game::ObjectBuildabilityStatus toObjectBuildability(
    ScriptObjectBuildability buildability) noexcept {
    switch (buildability) {
    case ScriptObjectBuildability::Yes:
        return game::ObjectBuildabilityStatus::Yes;
    case ScriptObjectBuildability::IgnorePrerequisites:
        return game::ObjectBuildabilityStatus::IgnorePrerequisites;
    case ScriptObjectBuildability::No:
        return game::ObjectBuildabilityStatus::No;
    case ScriptObjectBuildability::OnlyByAi:
        return game::ObjectBuildabilityStatus::OnlyByAi;
    }
    return game::ObjectBuildabilityStatus::Yes;
}

[[nodiscard]] PlayerRelationship toPlayerRelationship(
    ScriptPlayerRelationship relationship) noexcept {
    switch (relationship) {
    case ScriptPlayerRelationship::Enemies: return PlayerRelationship::Enemies;
    case ScriptPlayerRelationship::Neutral: return PlayerRelationship::Neutral;
    case ScriptPlayerRelationship::Allies: return PlayerRelationship::Allies;
    }
    return PlayerRelationship::Enemies;
}

} // namespace

using detail::kindOfContains;

namespace detail {

bool applyPlayerPolicyEffect(
    GameSessionScriptAuthorityPort& bridge, const ScriptEffect& effect) {
    bool handled = false;
    std::visit([&](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, ScriptPlayerCashEffect> ||
                      std::is_same_v<Payload, ScriptPlayerSellEverythingEffect> ||
                      std::is_same_v<Payload, ScriptPlayerRepairStructureEffect> ||
                      std::is_same_v<Payload, ScriptPlayerBuildUpgradeEffect> ||
                      std::is_same_v<Payload, ScriptPlayerBuildObjectNearTeamEffect> ||
                      std::is_same_v<Payload, ScriptPlayerBuildSupplyCenterEffect> ||
                      std::is_same_v<Payload, ScriptSkirmishBuildBuildingEffect> ||
                      std::is_same_v<Payload, ScriptSkirmishApproachEffect> ||
                      std::is_same_v<Payload, ScriptSkirmishPerimeterBuildEffect> ||
                      std::is_same_v<Payload, ScriptSkirmishFireSpecialPowerAtMostCostEffect> ||
                      std::is_same_v<Payload, ScriptSkirmishAttackNearestValueGroupEffect> ||
                      std::is_same_v<Payload, ScriptSkirmishMostValuableCommandButtonEffect> ||
                      std::is_same_v<Payload, ScriptPlayerConstructionEffect> ||
                      std::is_same_v<Payload, ScriptObjectBuildabilityEffect> ||
                      std::is_same_v<Payload, ScriptPlayerScienceAvailabilityEffect> ||
                      std::is_same_v<Payload, ScriptPlayerRelationshipEffect> ||
                      std::is_same_v<Payload, ScriptRelationshipOverrideEffect> ||
                      std::is_same_v<Payload, ScriptGlobalCombatPolicyEffect> ||
                      std::is_same_v<Payload, ScriptPlayerProgressionEffect> ||
                      std::is_same_v<Payload, ScriptVictoryEffect> ||
                      std::is_same_v<Payload, ScriptDefeatEffect>) {
            handled = true;
        if constexpr (std::is_same_v<Payload, ScriptPlayerCashEffect>) {
            const std::optional<PlayerId> player = bridge.resolvePlayer(payload.player,
                                                                  effect.header.invocation.currentPlayer,
                                                                  effect.header.currentPlayerAlias);
            if (!player) {
                bridge.emitDiagnostic(effect.header, "Script player-cash effect used an unresolved player alias: " +
                                               payload.player);
                return;
            }
            const bool applied = payload.operation == ScriptPlayerCashOperation::Set
                ? bridge.m_playerTransactions.setCash(*player, payload.value)
                : bridge.m_playerTransactions.adjustCash(*player, payload.value);
            if (!applied) {
                bridge.emitDiagnostic(effect.header, "Script player-cash effect referenced an unavailable player");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptPlayerSellEverythingEffect>) {
            const std::optional<PlayerId> player = bridge.resolvePlayer(
                payload.player, effect.header.invocation.currentPlayer,
                effect.header.currentPlayerAlias);
            if (!player || !bridge.players().get(*player)) {
                bridge.emitDiagnostic(effect.header,
                    "Script PLAYER_SELL_EVERYTHING used an unresolved player alias: " +
                        payload.player);
                return;
            }
            static_cast<void>(
                bridge.m_scenarioPlanTransactions.sellEverythingForPlayer(
                    *player, effect.header.confirmedTick));
        } else if constexpr (std::is_same_v<Payload, ScriptPlayerRepairStructureEffect>) {
            const std::optional<PlayerId> player = bridge.resolvePlayer(
                payload.player, effect.header.invocation.currentPlayer,
                effect.header.currentPlayerAlias);
            if (!player || !bridge.players().get(*player)) {
                bridge.emitDiagnostic(effect.header,
                    "Script PLAYER_REPAIR_NAMED_STRUCTURE used an unresolved player alias: " +
                        payload.player);
                return;
            }
            const std::optional<ScriptWorldObjectSnapshot> structure =
                bridge.m_queries.resolveObjectSelector(
                    payload.structure, effect.header.invocation);
            if (!structure || !structure->id || !structure->alive) return;
            static_cast<void>(
                bridge.m_scenarioPlanTransactions.requestPlayerRepairStructure(
                    *player, structure->id, effect.header.confirmedTick));
        } else if constexpr (std::is_same_v<Payload, ScriptPlayerBuildUpgradeEffect>) {
            const std::optional<PlayerId> player = bridge.resolvePlayer(
                payload.player, effect.header.invocation.currentPlayer,
                effect.header.currentPlayerAlias);
            if (!player || !bridge.players().get(*player)) {
                bridge.emitDiagnostic(effect.header,
                    "Script AI_PLAYER_BUILD_UPGRADE used an unresolved player alias: " +
                        payload.player);
                return;
            }
            if (!bridge.m_scenarioPlanTransactions.buildScriptPlayerUpgrade(
                    *player, payload.upgrade, effect.header.ordinal,
                    effect.header.confirmedTick)) {
                bridge.emitDiagnostic(effect.header,
                    "Script AI-player upgrade request was rejected: " +
                        payload.upgrade);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptPlayerBuildObjectNearTeamEffect>) {
            const std::optional<PlayerId> player = bridge.resolvePlayer(
                payload.player, effect.header.invocation.currentPlayer,
                effect.header.currentPlayerAlias);
            const std::optional<ObjectTeamId> team =
                bridge.resolveEffectTeam(payload.teamName, effect.header);
            if (!player || !team || !bridge.players().get(*player)) return;
            if (!bridge.m_scenarioPlanTransactions.buildScriptObjectNearestTeam(
                    *player, *team, payload.objectType,
                    effect.header.ordinal,
                    effect.header.confirmedTick)) {
                bridge.emitDiagnostic(effect.header,
                    "Script AI-player Team construction request was rejected: " +
                        payload.objectType);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptPlayerBuildSupplyCenterEffect>) {
            const std::optional<PlayerId> player = bridge.resolvePlayer(
                payload.player, effect.header.invocation.currentPlayer,
                effect.header.currentPlayerAlias);
            if (!player || !bridge.players().get(*player)) return;
            if (!bridge.m_scenarioPlanTransactions.buildScriptSupplyCenter(
                    *player, payload.objectType, payload.minimumSupplies,
                    effect.header.ordinal, effect.header.confirmedTick)) {
                bridge.emitDiagnostic(effect.header,
                    "Script AI-player supply construction request was rejected: " +
                        payload.objectType);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptSkirmishBuildBuildingEffect>) {
            const PlayerId player = effect.header.invocation.currentPlayer;
            if (!player || !bridge.players().get(player)) return;
            if (!bridge.m_scenarioPlanTransactions.buildScriptScenarioBuilding(
                    player, payload.objectType, effect.header.ordinal,
                    effect.header.confirmedTick)) {
                bridge.emitDiagnostic(effect.header,
                    "Script Skirmish building request was rejected: " +
                        payload.objectType);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptSkirmishApproachEffect>) {
            const std::optional<ObjectTeamId> team =
                bridge.resolveEffectTeam(payload.teamName, effect.header);
            if (!team) return;
            if (!bridge.m_scenarioPlanTransactions.executeScriptSkirmishApproach(
                    *team, payload.pathPrefix,
                    payload.operation ==
                        ScriptSkirmishApproachOperation::FollowPath,
                    payload.asTeam, effect.header.sourceScript.value,
                    effect.header.ordinal, effect.header.confirmedTick)) {
                bridge.emitDiagnostic(effect.header,
                    "Script Skirmish approach-path request was rejected: " +
                        payload.pathPrefix);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptSkirmishPerimeterBuildEffect>) {
            const PlayerId player = effect.header.invocation.currentPlayer;
            if (!player || !bridge.players().get(player)) return;
            if (!bridge.m_scenarioPlanTransactions.buildScriptPerimeterStructure(
                    player, payload.objectType, payload.flank,
                    payload.useFactionBaseDefense, effect.header.ordinal,
                    effect.header.confirmedTick)) {
                bridge.emitDiagnostic(effect.header,
                    "Script Skirmish perimeter-build request was rejected");
            }
        } else if constexpr (std::is_same_v<
                                 Payload,
                                 ScriptSkirmishFireSpecialPowerAtMostCostEffect>) {
            const std::optional<PlayerId> player = bridge.resolvePlayer(
                payload.player, effect.header.invocation.currentPlayer,
                effect.header.currentPlayerAlias);
            if (!player || !bridge.players().get(*player)) return;
            if (!bridge.m_scenarioPlanTransactions.fireScriptSpecialPowerAtMostCost(
                    *player, payload.specialPower,
                    effect.header.sourceScript.value,
                    effect.header.ordinal,
                    effect.header.confirmedTick)) {
                bridge.emitDiagnostic(effect.header,
                    "Script Skirmish most-cost special-power request was rejected: " +
                        payload.specialPower);
            }
        } else if constexpr (std::is_same_v<
                                 Payload,
                                 ScriptSkirmishAttackNearestValueGroupEffect>) {
            const std::optional<ObjectTeamId> team =
                bridge.resolveEffectTeam(payload.teamName, effect.header);
            if (!team) return;
            if (!bridge.m_scenarioPlanTransactions.attackScriptNearestValueGroup(
                    *team, payload.comparison, payload.minimumValue,
                    effect.header.sourceScript.value,
                    effect.header.ordinal,
                    effect.header.confirmedTick)) {
                bridge.emitDiagnostic(effect.header,
                    "Script Skirmish nearest-value-group request was rejected: " +
                        payload.teamName);
            }
        } else if constexpr (std::is_same_v<
                                 Payload,
                                 ScriptSkirmishMostValuableCommandButtonEffect>) {
            const std::optional<ObjectTeamId> team =
                bridge.resolveEffectTeam(payload.teamName, effect.header);
            if (!team) return;
            if (!bridge.m_scenarioPlanTransactions
                     .executeScriptMostValuableCommandButton(
                    *team, payload.buttonName,
                    payload.range,
                    payload.allTeamMembers,
                    effect.header.sourceScript.value,
                    effect.header.ordinal,
                    effect.header.confirmedTick)) {
                bridge.emitDiagnostic(effect.header,
                    "Script Skirmish most-valuable CommandButton request was rejected: " +
                        payload.buttonName);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptPlayerConstructionEffect>) {
            const std::optional<PlayerId> player = bridge.resolvePlayer(
                payload.player, effect.header.invocation.currentPlayer,
                effect.header.currentPlayerAlias);
            if (!player || !bridge.players().get(*player)) return;
            switch (payload.operation) {
            case ScriptPlayerConstructionOperation::SetBaseEnabled:
                static_cast<void>(bridge.players().setBaseConstructionEnabled(
                    *player, payload.enabled));
                break;
            case ScriptPlayerConstructionOperation::SetUnitsEnabled:
                static_cast<void>(bridge.players().setUnitConstructionEnabled(
                    *player, payload.enabled));
                break;
            case ScriptPlayerConstructionOperation::SetFactoryTypeEnabled:
                static_cast<void>(bridge.setPlayerFactoryTypeEnabled(
                    *player, payload.factoryType, payload.enabled,
                    effect.header.confirmedTick));
                break;
            case ScriptPlayerConstructionOperation::SetTeamDelaySeconds:
                static_cast<void>(bridge.players().setTeamDelaySeconds(
                    *player, payload.value));
                break;
            }
        } else if constexpr (std::is_same_v<Payload, ScriptObjectBuildabilityEffect>) {
            static_cast<void>(bridge.setObjectBuildability(
                payload.objectType, toObjectBuildability(payload.buildability),
                effect.header.confirmedTick));
        } else if constexpr (std::is_same_v<Payload, ScriptPlayerScienceAvailabilityEffect>) {
            const std::optional<PlayerId> player = bridge.resolvePlayer(payload.player,
                                                                  effect.header.invocation.currentPlayer,
                                                                  effect.header.currentPlayerAlias);
            if (!player || !bridge.players().get(*player)) {
                bridge.emitDiagnostic(effect.header,
                    "Script science-availability effect used an unresolved player alias: " +
                        payload.player);
                return;
            }
            // RefCode resolves the internal name through ScienceStore before
            // it writes availability. Keep that exact frozen-catalog lookup:
            // a typo or differently-cased science must remain a quiet no-op,
            // not become durable policy that a later purchase query sees.
            const ScienceCatalog* catalog = bridge.contentSnapshot().scienceCatalog();
            if (!catalog || !catalog->isLoaded()) {
                bridge.emitDiagnostic(effect.header,
                    "Script PLAYER_SCIENCE_AVAILABILITY has no frozen Science catalog");
                return;
            }
            const ScienceDefinition* science = catalog->find(payload.science);
            if (!science) return;
            static_cast<void>(bridge.players().setScienceAvailability(
                *player, science->name, toScienceAvailability(payload.availability)));
        } else if constexpr (std::is_same_v<Payload, ScriptPlayerRelationshipEffect>) {
            const std::optional<PlayerId> source = bridge.resolvePlayer(payload.sourcePlayer,
                                                                  effect.header.invocation.currentPlayer,
                                                                  effect.header.currentPlayerAlias);
            const std::optional<PlayerId> target = bridge.resolvePlayer(payload.targetPlayer,
                                                                  effect.header.invocation.currentPlayer,
                                                                  effect.header.currentPlayerAlias);
            if (!source || !target || !bridge.players().get(*source) ||
                !bridge.players().get(*target)) {
                bridge.emitDiagnostic(effect.header,
                    "Script player-relationship effect used an unresolved player alias");
                return;
            }
            // The registry stores directed relations. It deliberately retains
            // the modern self=Allies invariant; an invalid self override is
            // rejected rather than corrupting the relationship matrix.
            if (!bridge.players().setRelationship(
                    *source, *target, toPlayerRelationship(payload.relationship))) {
                bridge.emitDiagnostic(effect.header,
                    "Script player-relationship effect was rejected by PlayerRegistry");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptRelationshipOverrideEffect>) {
            std::optional<ObjectTeamId> sourceTeam;
            std::optional<PlayerId> sourcePlayer;
            if (payload.sourceKind ==
                ScriptRelationshipEndpointKind::ScenarioTeam) {
                sourceTeam = bridge.resolveEffectTeam(
                    payload.sourceName, effect.header);
                if (!sourceTeam) return;
            } else {
                sourcePlayer = bridge.resolvePlayer(
                    payload.sourceName,
                    effect.header.invocation.currentPlayer,
                    effect.header.currentPlayerAlias);
                if (!sourcePlayer ||
                    !bridge.players().get(*sourcePlayer)) return;
            }
            if (payload.operation ==
                ScriptRelationshipOverrideOperation::RemoveAllFromTeam) {
                if (sourceTeam) {
                    static_cast<void>(
                        bridge.clearTeamRelationships(
                            *sourceTeam, effect.header.confirmedTick));
                }
                return;
            }

            std::optional<ObjectTeamId> targetTeam;
            std::optional<PlayerId> targetPlayer;
            if (payload.targetKind ==
                ScriptRelationshipEndpointKind::ScenarioTeam) {
                targetTeam = bridge.resolveEffectTeam(
                    payload.targetName, effect.header);
                if (!targetTeam) return;
            } else {
                targetPlayer = bridge.resolvePlayer(
                    payload.targetName,
                    effect.header.invocation.currentPlayer,
                    effect.header.currentPlayerAlias);
                if (!targetPlayer ||
                    !bridge.players().get(*targetPlayer)) return;
            }
            const std::optional<PlayerRelationship> relationship =
                payload.operation ==
                    ScriptRelationshipOverrideOperation::Set
                ? std::optional<PlayerRelationship>{
                      toPlayerRelationship(payload.relationship)}
                : std::nullopt;
            if (sourceTeam && targetTeam) {
                static_cast<void>(bridge.setTeamToTeamRelationship(
                    *sourceTeam, *targetTeam, relationship,
                    effect.header.confirmedTick));
            } else if (sourceTeam && targetPlayer) {
                static_cast<void>(bridge.setTeamToPlayerRelationship(
                    *sourceTeam, *targetPlayer, relationship,
                    effect.header.confirmedTick));
            } else if (sourcePlayer && targetTeam) {
                static_cast<void>(bridge.setPlayerToTeamRelationship(
                    *sourcePlayer, *targetTeam, relationship,
                    effect.header.confirmedTick));
            }
        } else if constexpr (std::is_same_v<Payload, ScriptGlobalCombatPolicyEffect>) {
            static_cast<void>(
                bridge.m_scenarioPlanTransactions.setScriptGlobalCombatPolicy(
                    payload.policy, payload.enabled,
                    effect.header.confirmedTick));
        } else if constexpr (std::is_same_v<Payload, ScriptPlayerProgressionEffect>) {
            if (payload.operation == ScriptPlayerProgressionOperation::SetRankLevelLimit) {
                bridge.setRankLevelLimit(payload.integerValue);
                return;
            }
            const std::optional<PlayerId> player = bridge.resolvePlayer(payload.player,
                                                                  effect.header.invocation.currentPlayer,
                                                                  effect.header.currentPlayerAlias);
            if (!player || !bridge.players().get(*player)) {
                bridge.emitDiagnostic(effect.header,
                    "Script player-progression effect used an unresolved player alias: " + payload.player);
                return;
            }
            const RankInfoCatalog* ranks =
                bridge.contentSnapshot().rankInfoCatalog();
            switch (payload.operation) {
            case ScriptPlayerProgressionOperation::AddSkillPoints:
                if (ranks) {
                    static_cast<void>(bridge.players().addSkillPoints(
                        *player, payload.integerValue, *ranks,
                        bridge.rankLevelLimit()));
                }
                break;
            case ScriptPlayerProgressionOperation::AdjustRankLevel: {
                const PlayerState* state = bridge.players().get(*player);
                if (state) {
                    const int64_t requested = static_cast<int64_t>(state->progress.rankLevel) +
                        static_cast<int64_t>(payload.integerValue);
                    const int32_t bounded = requested > std::numeric_limits<int32_t>::max()
                        ? std::numeric_limits<int32_t>::max()
                        : requested < std::numeric_limits<int32_t>::min()
                            ? std::numeric_limits<int32_t>::min()
                            : static_cast<int32_t>(requested);
                    if (ranks) {
                        static_cast<void>(bridge.players().setRankLevel(
                            *player, bounded,
                            bridge.rankLevelLimit(), *ranks));
                    }
                }
                break;
            }
            case ScriptPlayerProgressionOperation::SetRankLevel:
                if (ranks) {
                    static_cast<void>(bridge.players().setRankLevel(
                        *player, payload.integerValue,
                        bridge.rankLevelLimit(), *ranks));
                }
                break;
            case ScriptPlayerProgressionOperation::GrantScience:
            {
                // Match Player::grantScience rather than treating this as an
                // unrestricted string insertion: unknown ScienceInfo values
                // and IsGrantable=No are both rejected by RefCode.
                const ScienceCatalog* catalog = bridge.contentSnapshot().scienceCatalog();
                if (!catalog || !catalog->isLoaded()) {
                    bridge.emitDiagnostic(effect.header,
                        "Script PLAYER_GRANT_SCIENCE has no frozen Science catalog");
                    break;
                }
                const ScienceDefinition* science = catalog->find(payload.science);
                if (!science || !science->grantable) break;
                static_cast<void>(bridge.players().grantScience(*player, science->name));
                break;
            }
            case ScriptPlayerProgressionOperation::PurchaseScience:
            {
                // The catalog was frozen into GameContentSnapshot before map
                // scripts started.  This preserves the original two-argument
                // ScriptAction while keeping price/prerequisite resolution out
                // of ScriptRuntime and away from mutable global INI stores.
                const ScienceCatalog* catalog = bridge.contentSnapshot().scienceCatalog();
                if (!catalog || !catalog->isLoaded()) {
                    bridge.emitDiagnostic(effect.header,
                        "Script PLAYER_PURCHASE_SCIENCE has no frozen Science catalog");
                    break;
                }
                const ScienceDefinition* science = catalog->find(payload.science);
                // RefCode resolves an unknown internal name to a non-existent
                // ScienceInfo, then attemptToPurchaseScience rejects it. Keep
                // that ordinary authored failure as a silent no-op.
                if (!science) break;
                static_cast<void>(bridge.players().tryPurchaseScience(*player, *science));
                break;
            }
            case ScriptPlayerProgressionOperation::SelectSkillset:
                static_cast<void>(bridge.players().setSelectedSkillset(
                    *player, payload.integerValue));
                break;
            case ScriptPlayerProgressionOperation::SetExperienceMultiplier:
                static_cast<void>(bridge.players().setSkillPointMultiplier(
                    *player, payload.realValue));
                break;
            case ScriptPlayerProgressionOperation::ExcludeFromScoreScreen:
                static_cast<void>(bridge.players().setListedInScoreScreen(*player, false));
                break;
            case ScriptPlayerProgressionOperation::SetRankLevelLimit:
                break;
            }
        } else if constexpr (std::is_same_v<Payload, ScriptVictoryEffect> ||
                      std::is_same_v<Payload, ScriptDefeatEffect>) {
            const scenario::MissionTerminalState terminal = std::is_same_v<Payload, ScriptVictoryEffect>
                ? scenario::MissionTerminalState::Victory : scenario::MissionTerminalState::Defeat;
            scenario::MissionEndMode mode = scenario::MissionEndMode::Normal;
            if constexpr (std::is_same_v<Payload, ScriptVictoryEffect>) {
                mode = payload.mode == ScriptMissionEndMode::Quick
                    ? scenario::MissionEndMode::Quick
                    : scenario::MissionEndMode::Normal;
            }
            if (!bridge.missionState().finish({
                    .state = terminal,
                    .sourcePlayer = effect.header.invocation.currentPlayer,
                    .confirmedTick = effect.header.confirmedTick,
                    .sourceScriptId = effect.header.sourceScript.value,
                    .mode = mode,
                })) {
                bridge.emitDiagnostic(effect.header, "Ignored ScriptEffect after mission terminal state was committed");
            }
        }
        }
    }, effect.payload);
    return handled;
}

} // namespace detail
} // namespace engine::script
