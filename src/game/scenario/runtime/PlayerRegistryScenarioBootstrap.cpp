#include "game/player/PlayerRegistry.h"

#include "game/scenario/runtime/ScenarioDefinition.h"

#include <algorithm>
#include <utility>

namespace engine {
namespace {

void copyAndNormalizeScienceNames(
    container::Vector<container::String>& destination,
    const container::Vector<container::String>& source) {
    destination = source;
    std::sort(destination.begin(), destination.end());
    destination.erase(
        std::unique(destination.begin(), destination.end()),
        destination.end());
}

void appendScenarioWarning(ScenarioRosterApplyReport& report, container::String message) {
    report.warnings.push_back(std::move(message));
}

void appendScenarioError(ScenarioRosterApplyReport& report, container::String message) {
    report.errors.push_back(std::move(message));
}

[[nodiscard]] bool hasCommandSeat(const PlayerState& player) noexcept {
    return player.slot && player.participation == PlayerParticipationKind::Participant &&
           (player.controller == PlayerControllerKind::Human || player.controller == PlayerControllerKind::Ai);
}

void applyFactionState(PlayerState& player, const FactionTemplate& faction) {
    player.faction = faction.id;
    player.playableSide = faction.playable && !faction.observer;
    player.side = faction.side;
    player.baseSide = faction.baseSide;
    copyAndNormalizeScienceNames(
        player.sciences.intrinsicKnown,
        faction.simulation.intrinsicSciences);
    player.sciences.known = player.sciences.intrinsicKnown;
    player.sciences.intrinsicPurchasePoints =
        faction.simulation.intrinsicSciencePurchasePoints;
    player.sciences.purchasePoints =
        player.sciences.intrinsicPurchasePoints;
    player.productionModifiers.cost = faction.simulation.productionCostModifiers;
    player.productionModifiers.time = faction.simulation.productionTimeModifiers;
    player.productionModifiers.veterancy = faction.simulation.productionVeterancyModifiers;
}

} // namespace

bool PlayerRegistry::applyScenarioDefinition(const scenario::ScenarioDefinition& scenario,
                                             const MultiplayerRuleset& ruleset,
                                             ScenarioDiplomacyBaseline diplomacyBaseline,
                                             ScenarioRosterApplyReport* outputReport) {
    ScenarioRosterApplyReport privateReport;
    ScenarioRosterApplyReport& report = outputReport ? *outputReport : privateReport;
    report = {};
    if (!scenario.isFinalized()) {
        appendScenarioError(report, "scenario definition must be finalized before applying it to PlayerRegistry");
        return false;
    }
    if (!ruleset.isLoaded()) {
        appendScenarioError(report, "scenario player application requires a loaded multiplayer ruleset");
        return false;
    }

    // Stage all changes first. A malformed scenario must not leave half of a
    // map's sides in the session-owned registry while later startup code falls
    // back to an unrelated roster.
    PlayerRegistry staged = *this;
    for (const scenario::ScenarioPlayerBinding& binding : scenario.players()) {
        if (!binding.player.isMapPlayer() || binding.scenarioName.empty()) {
            appendScenarioError(report, "scenario contains an invalid map player binding");
            continue;
        }

        const size_t index = binding.player.value;
        const bool existed = staged.m_players[index].has_value();
        PlayerState player = existed ? *staged.m_players[index] : PlayerState{};
        player.id = binding.player;
        player.participation = PlayerParticipationKind::Participant;
        player.life = PlayerLifeState::Active;
        if (!existed) {
            player.controller = binding.isHuman ? PlayerControllerKind::Human : PlayerControllerKind::Ai;
            player.aiDifficulty = binding.isHuman ? AiDifficulty::None : AiDifficulty::Normal;
            player.cash = ruleset.multiplayer().defaultStartingMoney;
        } else if (!hasCommandSeat(player)) {
            player.controller = binding.isHuman ? PlayerControllerKind::Human : PlayerControllerKind::Ai;
            player.aiDifficulty = binding.isHuman ? AiDifficulty::None : AiDifficulty::Normal;
        }

        // A local/lobby command seat has already established its controller,
        // color, alliance, cash and faction through the canonical match
        // descriptor. Only non-seat map sides get their initial economy from
        // the authored PlayerTemplate. This mirrors the original distinction
        // between map players and lobby/skirmish overrides without reusing a
        // global PlayerList singleton.
        const FactionTemplate* faction = binding.factionTemplateName.empty()
            ? nullptr
            : ruleset.findFaction(binding.factionTemplateName);
        if (faction) {
            const bool commandSeat = existed && hasCommandSeat(player);
            applyFactionState(player, *faction);
            if (!commandSeat) {
                player.cash = faction->simulation.startingMoney != 0
                    ? faction->simulation.startingMoney
                    : ruleset.multiplayer().defaultStartingMoney;
            }
        } else if (!binding.factionTemplateName.empty()) {
            appendScenarioWarning(report,
                "scenario player '" + binding.scenarioName + "' references unavailable PlayerTemplate '" +
                binding.factionTemplateName + "'; retained as a generic map owner");
            if (!existed) {
                player.side = binding.factionTemplateName;
                player.baseSide = binding.factionTemplateName;
            }
        }
        if (!binding.displayName.empty()) {
            player.displayName = binding.displayName;
        } else if (player.displayName.empty()) {
            player.displayName = binding.scenarioName;
        }
        staged.m_players[index] = std::move(player);
        if (existed) ++report.reusedCommandPlayerCount;
        else ++report.materializedPlayerCount;
    }
    if (!report.ok()) return false;

    staged.rebuildActivePlayerIds();
    switch (diplomacyBaseline) {
    case ScenarioDiplomacyBaseline::AuthoredNeutral:
        // RefCode PlayerList::newGame constructs every map Player with a
        // Neutral default, then applies the directed SidesList enemy list and
        // finally the ally list. This is materially different from treating
        // every different campaign faction/alliance as an enemy: civilian
        // props must not trigger an otherwise neutral DemoTrap, for example.
        staged.m_relationships.reset();
        break;
    case ScenarioDiplomacyBaseline::PreserveMatchAlliances:
        // PlayerRegistry::initialize already seeded the resolved lobby
        // participants. Preserve exactly those relations. Newly materialized
        // map-only players still occupy Neutral rows/columns until SidesList
        // explicitly names them, matching prepareForMP_or_Skirmish(): it adds
        // lobby players' lists but does not turn Civilian/map owners into an
        // implicit enemy faction.
        break;
    }
    for (const scenario::ScenarioDiplomacyOverride& overrideValue : scenario.diplomacyOverrides()) {
        if (!staged.get(overrideValue.from) || !staged.get(overrideValue.to) ||
            !staged.setRelationship(overrideValue.from, overrideValue.to, overrideValue.relationship)) {
            appendScenarioError(report, "scenario diplomacy override references an unavailable player");
            continue;
        }
        ++report.relationshipOverrideCount;
    }
    if (!report.ok()) return false;
    *this = std::move(staged);
    return true;
}

} // namespace engine
