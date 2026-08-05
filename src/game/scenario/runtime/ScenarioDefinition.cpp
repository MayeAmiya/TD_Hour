#include "core/container/container_types.h"
#include "game/scenario/runtime/ScenarioDefinition.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace engine::scenario {
namespace {

[[nodiscard]] container::String canonical(container::StringView value) {
    container::String output;
    output.reserve(value.size());
    for (const unsigned char character : value) {
        if (character >= 'A' && character <= 'Z') {
            output.push_back(static_cast<char>(character + ('a' - 'A')));
        } else {
            output.push_back(static_cast<char>(character));
        }
    }
    return output;
}

void addIssue(container::Vector<ScenarioValidationIssue>* issues, container::String message) {
    if (issues) issues->push_back({.message = std::move(message)});
}

[[nodiscard]] std::optional<PlayerId> parseSlotOwner(container::StringView alias) {
    return parseCanonicalMapPlayerAlias(alias);
}

[[nodiscard]] bool sameReference(OwnerReference lhs, OwnerReference rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.player == rhs.player && lhs.scriptTeam == rhs.scriptTeam;
}

[[nodiscard]] bool isKnownRelationship(PlayerRelationship relationship) noexcept {
    switch (relationship) {
    case PlayerRelationship::Allies:
    case PlayerRelationship::Enemies:
    case PlayerRelationship::Neutral:
        return true;
    }
    return false;
}

} // namespace

bool ScenarioDefinition::addPlayerBinding(ScenarioPlayerBinding binding) {
    if (m_finalized) return false;
    m_players.push_back(std::move(binding));
    return true;
}

bool ScenarioDefinition::addSideBinding(ScenarioSideBinding binding) {
    if (m_finalized || binding.sourceSideOrdinal == INVALID_SCENARIO_SIDE_ORDINAL ||
        !binding.player.isValid()) {
        return false;
    }
    m_sides.push_back(std::move(binding));
    return true;
}

bool ScenarioDefinition::addScriptTeam(ScriptTeamDefinition definition) {
    if (m_finalized) return false;
    m_scriptTeams.push_back(std::move(definition));
    return true;
}

bool ScenarioDefinition::addBuildIntent(ScenarioBuildIntent intent) {
    if (m_finalized) return false;
    m_buildIntents.push_back(std::move(intent));
    return true;
}

bool ScenarioDefinition::addDiplomacyOverride(ScenarioDiplomacyOverride overrideValue) {
    if (m_finalized) return false;
    m_diplomacyOverrides.push_back(std::move(overrideValue));
    return true;
}

bool ScenarioDefinition::finalize(const ResolvedMatchSetup& setup,
                                  container::Vector<ScenarioValidationIssue>* issues) {
    m_ownerAliases.clear();
    m_finalized = false;
    container::Vector<ScenarioValidationIssue> privateIssues;
    if (!issues) issues = &privateIssues;
    issues->clear();

    container::Vector<PlayerId> knownPlayers;
    knownPlayers.reserve(setup.players.size());
    container::Vector<PlayerId> scenarioBoundPlayers;
    const auto addPlayerAliases = [this](PlayerId player) {
        m_ownerAliases.push_back({
            .canonicalName = "player_" + std::to_string(player.value),
            .reference = {.kind = OwnerReferenceKind::Player, .player = player},
        });
    };
    for (const ResolvedPlayerSetup& player : setup.players) {
        if (!player.player.isMapPlayer()) {
            addIssue(issues, "resolved setup contains an invalid map player binding");
            continue;
        }
        knownPlayers.push_back(player.player);
        addPlayerAliases(player.player);
        if (!player.displayName.empty()) {
            m_ownerAliases.push_back({
                .canonicalName = canonical(player.displayName),
                .reference = {.kind = OwnerReferenceKind::Player, .player = player.player},
            });
        }
    }
    std::sort(knownPlayers.begin(), knownPlayers.end());
    knownPlayers.erase(std::unique(knownPlayers.begin(), knownPlayers.end()), knownPlayers.end());
    m_ownerAliases.push_back({
        .canonicalName = "neutral",
        .reference = {.kind = OwnerReferenceKind::Player, .player = NEUTRAL_PLAYER_ID},
    });

    for (const ScenarioPlayerBinding& binding : m_players) {
        if (binding.scenarioName.empty() || !binding.player.isMapPlayer()) {
            addIssue(issues, "scenario player binding has an invalid name or map player ID");
            continue;
        }
        if (std::find(scenarioBoundPlayers.begin(), scenarioBoundPlayers.end(), binding.player) !=
            scenarioBoundPlayers.end()) {
            addIssue(issues, "scenario assigns more than one side to map player ID " +
                                 std::to_string(binding.player.value));
            continue;
        }
        scenarioBoundPlayers.push_back(binding.player);
        knownPlayers.push_back(binding.player);
        addPlayerAliases(binding.player);
        const container::String authoredName = canonical(binding.scenarioName);
        const auto existingName = std::find_if(
            m_ownerAliases.begin(), m_ownerAliases.end(),
            [&authoredName](const OwnerAlias& alias) { return alias.canonicalName == authoredName; });
        if (existingName == m_ownerAliases.end()) {
            m_ownerAliases.push_back({
                .canonicalName = authoredName,
                .reference = {.kind = OwnerReferenceKind::Player, .player = binding.player},
            });
        }
        // RefCode's PlayerList::findPlayerWithNameKey walks its source-order
        // player array and returns the first matching name.  Maps with two
        // Sides carrying the same authored playerName are therefore valid;
        // retain that first-name lookup while each Side still receives its
        // own stable PlayerId in the modern registry.
    }
    std::sort(knownPlayers.begin(), knownPlayers.end());
    knownPlayers.erase(std::unique(knownPlayers.begin(), knownPlayers.end()), knownPlayers.end());

    container::Vector<uint32_t> sourceSideOrdinals;
    sourceSideOrdinals.reserve(m_sides.size());
    for (const ScenarioSideBinding& side : m_sides) {
        if (side.sourceSideOrdinal == INVALID_SCENARIO_SIDE_ORDINAL || !side.player.isValid()) {
            addIssue(issues, "scenario side binding has an invalid source ordinal or player ID");
            continue;
        }
        const auto inserted = std::lower_bound(sourceSideOrdinals.begin(), sourceSideOrdinals.end(),
                                               side.sourceSideOrdinal);
        if (inserted != sourceSideOrdinals.end() && *inserted == side.sourceSideOrdinal) {
            addIssue(issues, "scenario contains duplicate source side ordinal " +
                                 std::to_string(side.sourceSideOrdinal));
            continue;
        }
        sourceSideOrdinals.insert(inserted, side.sourceSideOrdinal);
        if (side.player != NEUTRAL_PLAYER_ID &&
            !std::binary_search(knownPlayers.begin(), knownPlayers.end(), side.player)) {
            addIssue(issues, "scenario side binding references a player absent from scenario players");
        }
    }

    std::sort(m_scriptTeams.begin(), m_scriptTeams.end(), [](const ScriptTeamDefinition& lhs,
                                                              const ScriptTeamDefinition& rhs) {
        const container::String left = canonical(lhs.name);
        const container::String right = canonical(rhs.name);
        return left == right ? lhs.name < rhs.name : left < right;
    });
    for (size_t index = 0; index < m_scriptTeams.size(); ++index) {
        ScriptTeamDefinition& team = m_scriptTeams[index];
        if (team.name.empty()) {
            addIssue(issues, "scenario script team has an empty name");
            continue;
        }
        team.id = ScriptTeamId{static_cast<uint32_t>(index + 1)};
        const OwnerReference owner = lookupOwner(team.ownerAlias);
        if (owner.kind != OwnerReferenceKind::Player || !owner.player) {
            addIssue(issues, "scenario script team has an unresolved player owner: " + team.ownerAlias);
            continue;
        }
        team.resolvedOwner = owner.player;
        for (const ScenarioTeamUnitPlan& unit : team.plan.units) {
            if (unit.templateName.empty() || unit.maximumUnits == 0 ||
                unit.minimumUnits > unit.maximumUnits) {
                addIssue(issues,
                    "scenario script team has an invalid typed unit plan: " +
                    team.name);
                break;
            }
        }
        m_ownerAliases.push_back({
            .canonicalName = canonical(team.name),
            .reference = {.kind = OwnerReferenceKind::ScriptTeam,
                          .player = owner.player,
                          .scriptTeam = team.id},
        });
    }

    std::sort(m_ownerAliases.begin(), m_ownerAliases.end(), [](const OwnerAlias& lhs,
                                                                const OwnerAlias& rhs) {
        return lhs.canonicalName < rhs.canonicalName;
    });
    container::Vector<OwnerAlias> uniqueAliases;
    uniqueAliases.reserve(m_ownerAliases.size());
    for (const OwnerAlias& alias : m_ownerAliases) {
        if (!uniqueAliases.empty() && uniqueAliases.back().canonicalName == alias.canonicalName) {
            if (!sameReference(uniqueAliases.back().reference, alias.reference)) {
                addIssue(issues, "scenario owner alias resolves ambiguously: " + alias.canonicalName);
            }
            continue;
        }
        uniqueAliases.push_back(alias);
    }
    m_ownerAliases = std::move(uniqueAliases);

    for (const ScenarioDiplomacyOverride& overrideValue : m_diplomacyOverrides) {
        if (!isKnownRelationship(overrideValue.relationship) ||
            !overrideValue.from.isValid() || !overrideValue.to.isValid() ||
            (overrideValue.from != NEUTRAL_PLAYER_ID &&
             !std::binary_search(knownPlayers.begin(), knownPlayers.end(), overrideValue.from)) ||
            (overrideValue.to != NEUTRAL_PLAYER_ID &&
             !std::binary_search(knownPlayers.begin(), knownPlayers.end(), overrideValue.to) ) ||
            (overrideValue.from == overrideValue.to &&
             overrideValue.relationship != PlayerRelationship::Allies)) {
            addIssue(issues, "scenario diplomacy override contains an invalid player ID");
        }
    }
    container::Vector<std::pair<uint32_t, uint32_t>> buildIntentSourceKeys;
    buildIntentSourceKeys.reserve(m_buildIntents.size());
    for (const ScenarioBuildIntent& intent : m_buildIntents) {
        const std::pair sourceKey{intent.sourceSideOrdinal,
                                  intent.sourceBuildListOrdinal};
        const auto sourceKeyPosition = std::lower_bound(
            buildIntentSourceKeys.begin(), buildIntentSourceKeys.end(), sourceKey);
        if (intent.sourceSideOrdinal == INVALID_SCENARIO_SIDE_ORDINAL ||
            intent.sourceBuildListOrdinal == INVALID_SCENARIO_BUILD_LIST_ORDINAL) {
            addIssue(issues, "scenario build intent has an invalid source position");
        } else if (!std::binary_search(sourceSideOrdinals.begin(), sourceSideOrdinals.end(),
                                      intent.sourceSideOrdinal)) {
            addIssue(issues, "scenario build intent references an unknown source Side");
        } else if (sourceKeyPosition != buildIntentSourceKeys.end() &&
                   *sourceKeyPosition == sourceKey) {
            addIssue(issues, "scenario contains duplicate BuildList source positions");
        } else if (intent.templateName.empty()) {
            addIssue(issues, "scenario build intent is missing a template name");
        } else if (!intent.fixedPoseValid) {
            addIssue(issues, "scenario build intent has a non-finite transform");
        } else if (!intent.ownerAlias.empty() && !lookupOwner(intent.ownerAlias)) {
            addIssue(issues, "scenario build intent has an unresolved owner: " + intent.ownerAlias);
        } else if (intent.resolvedOwner &&
                   intent.resolvedOwner != NEUTRAL_PLAYER_ID &&
                   !std::binary_search(knownPlayers.begin(), knownPlayers.end(), intent.resolvedOwner)) {
            addIssue(issues, "scenario build intent has an invalid resolved owner");
        }
        if (sourceKeyPosition == buildIntentSourceKeys.end() ||
            *sourceKeyPosition != sourceKey) {
            buildIntentSourceKeys.insert(sourceKeyPosition, sourceKey);
        }
    }

    m_finalized = issues->empty();
    return m_finalized;
}

OwnerReference ScenarioDefinition::resolveOwner(container::StringView alias) const {
    if (!m_finalized || alias.empty()) return {};
    return lookupOwner(alias);
}

std::optional<PlayerId> ScenarioDefinition::playerForSourceSide(uint32_t sourceSideOrdinal) const noexcept {
    if (!m_finalized || sourceSideOrdinal == INVALID_SCENARIO_SIDE_ORDINAL) return std::nullopt;
    const auto found = std::find_if(m_sides.begin(), m_sides.end(),
        [sourceSideOrdinal](const ScenarioSideBinding& side) {
            return side.sourceSideOrdinal == sourceSideOrdinal;
        });
    return found == m_sides.end() ? std::nullopt : std::optional<PlayerId>{found->player};
}

const ScenarioPlayerBinding* ScenarioDefinition::findPlayer(PlayerId player) const noexcept {
    if (!m_finalized || !player.isMapPlayer()) return nullptr;
    const auto found = std::find_if(m_players.begin(), m_players.end(),
        [player](const ScenarioPlayerBinding& binding) { return binding.player == player; });
    return found == m_players.end() ? nullptr : &*found;
}

OwnerReference ScenarioDefinition::lookupOwner(container::StringView alias) const {
    if (alias.empty()) return {};
    const container::String key = canonical(alias);
    if (!m_finalized) {
        const auto found = std::find_if(m_ownerAliases.begin(), m_ownerAliases.end(),
            [&](const OwnerAlias& candidate) { return candidate.canonicalName == key; });
        return found == m_ownerAliases.end() ? OwnerReference{} : found->reference;
    }
    const auto found = std::lower_bound(m_ownerAliases.begin(), m_ownerAliases.end(), key,
        [](const OwnerAlias& lhs, const container::String& rhs) { return lhs.canonicalName < rhs; });
    if (found != m_ownerAliases.end() && found->canonicalName == key) return found->reference;

    // This fallback keeps the ubiquitous Player_0 spelling useful even when a
    // map omitted a redundant scenario player alias. It still refuses player
    // identities that were not declared by the finalized match/scenario.
    if (const auto player = parseSlotOwner(alias)) {
        const container::String canonicalPlayer = "player_" + std::to_string(player->value);
        const auto direct = std::lower_bound(m_ownerAliases.begin(), m_ownerAliases.end(), canonicalPlayer,
            [](const OwnerAlias& lhs, const container::String& rhs) { return lhs.canonicalName < rhs; });
        if (direct != m_ownerAliases.end() && direct->canonicalName == canonicalPlayer) {
            return direct->reference;
        }
    }
    return {};
}

const ScriptTeamDefinition* ScenarioDefinition::findScriptTeam(ScriptTeamId id) const noexcept {
    if (!m_finalized || !id || id.value > m_scriptTeams.size()) return nullptr;
    const ScriptTeamDefinition& result = m_scriptTeams[id.value - 1];
    return result.id == id ? &result : nullptr;
}

} // namespace engine::scenario
