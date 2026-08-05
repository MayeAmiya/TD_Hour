#include "core/container/hash_containers.h"
#include "core/container/string_utils.h"
#include "game/scenario/runtime/LegacyScenarioCompiler.h"

#include "game/scenario/source/LegacySkirmishScriptSource.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace engine::scenario {
namespace {

using script::legacy::LegacyDictionaryEntry;
using script::legacy::LegacyDictionaryValue;
using script::legacy::LegacyMapScriptSource;
using script::legacy::LegacySideSource;
using script::legacy::LegacySourceRange;
using script::legacy::LegacyTeamSource;

[[nodiscard]] container::String canonical(container::StringView value) {
    container::String result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<char>(std::tolower(character)));
    }
    return result;
}

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

// Same rule the map-object parser applies to its authored transform: isfinite
// screens NaN/Inf but says nothing about magnitude, and a huge-but-finite value
// saturates q32_32 to +/-INT64_MAX raw. These build-list floats become frozen
// ScenarioBuildIntent simulation data, where a saturated angle is a latent
// non-terminating normalize_angle for any consumer that converts it back to a
// float. Both limits sit far outside any real Zero Hour map extent.
constexpr float kMaximumBuildIntentHorizontal = 1.0e6f;
constexpr float kMaximumBuildIntentAngle = 1.0e4f;

[[nodiscard]] constexpr bool withinMagnitude(float value, float limit) noexcept {
    return value >= -limit && value <= limit;
}

[[nodiscard]] bool admissibleBuildIntentValue(float value, float limit) noexcept {
    return std::isfinite(value) && withinMagnitude(value, limit);
}

[[nodiscard]] container::String utf8FromUtf16(container::U16StringView value) {
    container::String result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        uint32_t codePoint = value[index];
        if (codePoint >= 0xd800u && codePoint <= 0xdbffu && index + 1 < value.size()) {
            const uint32_t low = value[index + 1];
            if (low >= 0xdc00u && low <= 0xdfffu) {
                codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) + (low - 0xdc00u);
                ++index;
            }
        }
        if (codePoint <= 0x7fu) {
            result.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ffu) {
            result.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
            result.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else if (codePoint <= 0xffffu) {
            result.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
            result.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            result.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else {
            result.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
            result.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
            result.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            result.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        }
    }
    return result;
}

[[nodiscard]] container::String dictionaryValueText(const LegacyDictionaryValue& value) {
    return std::visit([](const auto& item) -> container::String {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::monostate>) {
            return {};
        } else if constexpr (std::is_same_v<Item, bool>) {
            return item ? "true" : "false";
        } else if constexpr (std::is_same_v<Item, int32_t>) {
            return std::to_string(item);
        } else if constexpr (std::is_same_v<Item, float>) {
            return std::to_string(item);
        } else if constexpr (std::is_same_v<Item, container::String>) {
            return item;
        } else {
            return utf8FromUtf16(item);
        }
    }, value);
}

[[nodiscard]] const LegacyDictionaryEntry* findField(
    const container::Vector<LegacyDictionaryEntry>& fields, container::StringView name) {
    const container::String wanted = canonical(name);
    const auto found = std::find_if(fields.begin(), fields.end(), [&wanted](const LegacyDictionaryEntry& entry) {
        return canonical(entry.key.resolvedName) == wanted;
    });
    return found == fields.end() ? nullptr : &*found;
}

[[nodiscard]] container::String fieldText(const container::Vector<LegacyDictionaryEntry>& fields,
                                    container::StringView name) {
    const LegacyDictionaryEntry* field = findField(fields, name);
    return field ? dictionaryValueText(field->value) : container::String{};
}

[[nodiscard]] bool fieldBool(const container::Vector<LegacyDictionaryEntry>& fields,
                             container::StringView name, bool fallback = false) {
    const LegacyDictionaryEntry* field = findField(fields, name);
    if (!field) return fallback;
    if (const bool* value = std::get_if<bool>(&field->value)) return *value;
    if (const int32_t* value = std::get_if<int32_t>(&field->value)) return *value != 0;
    const container::String text = canonical(dictionaryValueText(field->value));
    if (text == "yes" || text == "true" || text == "1") return true;
    if (text == "no" || text == "false" || text == "0") return false;
    return fallback;
}

[[nodiscard]] int32_t fieldInt(const container::Vector<LegacyDictionaryEntry>& fields,
                               container::StringView name, int32_t fallback = 0) {
    const LegacyDictionaryEntry* field = findField(fields, name);
    if (!field) return fallback;
    if (const int32_t* value = std::get_if<int32_t>(&field->value)) return *value;
    if (const bool* value = std::get_if<bool>(&field->value)) return *value ? 1 : 0;
    return fallback;
}

[[nodiscard]] container::Vector<RawScenarioField> copyRawFields(
    const container::Vector<LegacyDictionaryEntry>& fields) {
    container::Vector<RawScenarioField> result;
    result.reserve(fields.size());
    for (const LegacyDictionaryEntry& field : fields) {
        result.push_back({
            .key = field.key.resolvedName.empty()
                ? std::to_string(field.key.rawValue)
                : field.key.resolvedName,
            .value = dictionaryValueText(field.value),
        });
    }
    return result;
}

[[nodiscard]] ScenarioTeamPlan compileScenarioTeamPlan(
    const container::Vector<LegacyDictionaryEntry>& fields) {
    ScenarioTeamPlan plan;
    plan.units.reserve(7);
    for (uint32_t ordinal = 1; ordinal <= 7; ++ordinal) {
        const container::String suffix = std::to_string(ordinal);
        const container::String templateName =
            fieldText(fields, "teamUnitType" + suffix);
        const int32_t authoredMaximum =
            fieldInt(fields, "teamUnitMaxCount" + suffix);
        if (templateName.empty() || authoredMaximum <= 0) continue;
        const uint32_t maximum = static_cast<uint32_t>(authoredMaximum);
        const int32_t authoredMinimum =
            fieldInt(fields, "teamUnitMinCount" + suffix);
        plan.units.push_back({
            .templateName = templateName,
            .minimumUnits = static_cast<uint32_t>(std::clamp<int32_t>(
                authoredMinimum, 0, authoredMaximum)),
            .maximumUnits = maximum,
        });
    }
    plan.homeWaypoint = fieldText(fields, "teamHome");
    plan.onCreateScript = fieldText(fields, "teamOnCreateScript");
    plan.productionCondition =
        fieldText(fields, "teamProductionCondition");
    plan.reinforcementTransport = fieldText(fields, "teamTransport");
    plan.reinforcementOriginWaypoint =
        fieldText(fields, "teamReinforcementOrigin");
    plan.initialIdleFrames = fieldInt(fields, "teamInitialIdleFrames");
    plan.productionPriority = fieldInt(fields, "teamProductionPriority");
    plan.productionPrioritySuccessIncrease =
        fieldInt(fields, "teamProductionPrioritySuccessIncrease");
    plan.productionPriorityFailureDecrease =
        fieldInt(fields, "teamProductionPriorityFailureDecrease");
    plan.veterancy = fieldInt(fields, "teamVeterancy");
    plan.initialAttitude = std::clamp(
        fieldInt(fields, "teamAggressiveness"), -2, 2);
    plan.aiRecruitable = fieldBool(fields, "teamIsAIRecruitable");
    plan.automaticallyReinforce =
        fieldBool(fields, "teamAutoReinforce");
    plan.attackCommonTarget =
        fieldBool(fields, "teamAttackCommonTarget");
    plan.executesActionsOnCreate =
        fieldBool(fields, "teamExecutesActionsOnCreate");
    plan.startsFull = fieldBool(fields, "teamStartsFull");
    plan.transportsExit = fieldBool(fields, "teamTransportsExit");
    return plan;
}

[[nodiscard]] container::Vector<container::String> splitTokens(container::StringView text) {
    container::Vector<container::String> result;
    size_t begin = 0;
    while (begin < text.size()) {
        while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
        if (begin == text.size()) break;
        size_t end = begin;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        result.emplace_back(text.substr(begin, end - begin));
        begin = end;
    }
    return result;
}

[[nodiscard]] std::optional<PlayerId> parsePlayerAlias(container::StringView alias) {
    return parseCanonicalMapPlayerAlias(alias);
}

// Single definition of the Side -> roster-player alias rule.  The skirmish
// `.scb` graft mirrors this election so a grafted ScriptList lands on the very
// player this compiler will bind to the same Side; keeping one implementation
// is what stops the two from drifting apart.
[[nodiscard]] bool matchesFaction(const FactionTemplate& faction,
                                  container::StringView sourceFaction,
                                  container::StringView sourcePlayerName) {
    return script::legacy::legacySideMatchesFactionAliases(
        sourceFaction, sourcePlayerName, faction.name, faction.side, faction.baseSide);
}

void appendDiagnostic(LegacyScenarioCompileResult& result,
                      LegacyScenarioDiagnosticSeverity severity,
                      container::String message,
                      LegacySourceRange range = {},
                      container::String sourcePath = "SidesList") {
    result.diagnostics.push_back({
        .severity = severity,
        .message = std::move(message),
        .sourceOffset = range.offset,
        .sourcePath = std::move(sourcePath),
    });
}

struct SideDraft final {
    const LegacySideSource* source = nullptr;
    uint32_t sourceSideOrdinal = INVALID_SCENARIO_SIDE_ORDINAL;
    container::String name;
    container::String faction;
    container::String displayName;
    bool isHuman = false;
    PlayerId player = INVALID_PLAYER_ID;
};

struct TeamDraft final {
    container::String name;
    container::String owner;
    bool isSingleton = false;
    int32_t maximumInstances = 0;
    bool isPlayerDefault = false;
    ScenarioTeamPlan plan;
    container::Vector<RawScenarioField> fields;
};

} // namespace

bool LegacyScenarioCompileResult::hasErrors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const LegacyScenarioDiagnostic& diagnostic) {
        return diagnostic.severity == LegacyScenarioDiagnosticSeverity::Error;
    });
}

LegacyScenarioCompileResult LegacyScenarioCompiler::compile(
    const LegacyMapScriptSource& source,
    const ResolvedMatchSetup& matchSetup,
    const MultiplayerRuleset& ruleset) const {
    LegacyScenarioCompileResult result;
    auto definition = std::make_shared<ScenarioDefinition>();

    container::Vector<SideDraft> sides;
    uint32_t nextSourceSideOrdinal = 0;
    for (const script::legacy::LegacySidesListSource& sidesList : source.sidesLists()) {
        result.sourceSideCount += sidesList.sides.size();
        for (const LegacySideSource& sourceSide : sidesList.sides) {
            SideDraft side;
            side.source = &sourceSide;
            side.sourceSideOrdinal = nextSourceSideOrdinal++;
            side.name = fieldText(sourceSide.properties, "playerName");
            side.faction = fieldText(sourceSide.properties, "playerFaction");
            side.displayName = fieldText(sourceSide.properties, "playerDisplayName");
            side.isHuman = fieldBool(sourceSide.properties, "playerIsHuman");
            if (side.name.empty()) {
                // RefCode treats an empty playerName as the one neutral side.
                // PlayerRegistry owns that fixed identity already, so there is
                // no second, collision-prone neutral PlayerId to create.
                if (!definition->addSideBinding({
                        .sourceSideOrdinal = side.sourceSideOrdinal,
                        .scenarioName = {},
                        .player = NEUTRAL_PLAYER_ID,
                    })) {
                    appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Error,
                                     "failed to append a neutral SidesList side binding");
                }
                continue;
            }
            if (equalAsciiInsensitive(side.name, "Neutral")) {
                appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Warning,
                                 "SidesList used the noncanonical literal Neutral player name; normalized to session neutral");
                if (!definition->addSideBinding({
                        .sourceSideOrdinal = side.sourceSideOrdinal,
                        .scenarioName = side.name,
                        .player = NEUTRAL_PLAYER_ID,
                    })) {
                    appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Error,
                                     "failed to append a normalized neutral SidesList side binding");
                }
                continue;
            }
            sides.push_back(std::move(side));
        }
    }

    container::Array<bool, MAP_PLAYER_COUNT> assigned{};
    container::Array<bool, MAP_PLAYER_COUNT> reservedByCommandRoster{};
    for (const ResolvedPlayerSetup& player : matchSetup.players) {
        if (player.player.isMapPlayer()) reservedByCommandRoster[player.player.value] = true;
    }

    const auto chooseMatchingRosterPlayer = [&](const SideDraft& side) -> std::optional<PlayerId> {
        for (const ResolvedPlayerSetup& player : matchSetup.players) {
            if (!player.player.isMapPlayer() || assigned[player.player.value]) continue;
            const FactionTemplate* faction = ruleset.findFaction(player.faction);
            if (faction && matchesFaction(*faction, side.faction, side.name)) return player.player;
        }
        return std::nullopt;
    };
    const auto chooseFreePlayer = [&](bool avoidReserved) -> std::optional<PlayerId> {
        for (uint8_t value = 0; value < static_cast<uint8_t>(MAP_PLAYER_COUNT); ++value) {
            if (!assigned[value] && (!avoidReserved || !reservedByCommandRoster[value])) {
                return PlayerId{value};
            }
        }
        return std::nullopt;
    };

    for (SideDraft& side : sides) {
        if (const std::optional<PlayerId> explicitPlayer = parsePlayerAlias(side.name)) {
            if (!assigned[explicitPlayer->value]) {
                side.player = *explicitPlayer;
            } else {
                appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Error,
                                 "duplicate SidesList Player_N alias: " + side.name);
                continue;
            }
        } else if (const std::optional<PlayerId> roster = chooseMatchingRosterPlayer(side)) {
            side.player = *roster;
        } else if (const std::optional<PlayerId> freePlayer = chooseFreePlayer(true)) {
            side.player = *freePlayer;
        } else if (const std::optional<PlayerId> freePlayer = chooseFreePlayer(false)) {
            side.player = *freePlayer;
        } else {
            appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Error,
                             "map declares more non-neutral sides than the 16-player scenario namespace");
            continue;
        }
        assigned[side.player.value] = true;
        ScenarioPlayerBinding binding;
        binding.scenarioName = side.name;
        binding.player = side.player;
        binding.factionTemplateName = side.faction;
        binding.displayName = side.displayName;
        binding.isHuman = side.isHuman;
        binding.fields = copyRawFields(side.source->properties);
        if (!definition->addPlayerBinding(std::move(binding))) {
            appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Error,
                             "failed to append a SidesList player binding");
        } else {
            ++result.mappedPlayerCount;
        }
        if (!definition->addSideBinding({
                .sourceSideOrdinal = side.sourceSideOrdinal,
                .scenarioName = side.name,
                .player = side.player,
            })) {
            appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Error,
                             "failed to append a SidesList side binding");
        }
    }

    container::HashMap<container::String, PlayerId> playerAliases;
    playerAliases.reserve(sides.size() * 3 + 2);
    playerAliases.emplace("neutral", NEUTRAL_PLAYER_ID);
    for (const SideDraft& side : sides) {
        if (!side.player) continue;
        playerAliases.emplace(canonical(side.name), side.player);
        playerAliases.emplace("player_" + std::to_string(side.player.value), side.player);
    }

    // RefCode PlayerList::newGame applies enemies first and allies second.
    // Preserve that directed, authored precedence for malformed maps that
    // mention the same target in both lists.
    const auto appendRelations = [&](const SideDraft& side, container::StringView field,
                                     PlayerRelationship relationship) {
        if (!side.player) return;
        for (const container::String& token : splitTokens(fieldText(side.source->properties, field))) {
            const auto target = playerAliases.find(canonical(token));
            if (target == playerAliases.end()) {
                appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Warning,
                                 "SidesList " + container::String(field) + " references unknown player '" + token + "'");
                continue;
            }
            if (side.player == target->second && relationship != PlayerRelationship::Allies) {
                appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Warning,
                                 "ignored self-enemy relation for SidesList player '" + side.name + "'");
                continue;
            }
            static_cast<void>(definition->addDiplomacyOverride({
                .from = side.player,
                .to = target->second,
                .relationship = relationship,
            }));
        }
    };
    for (const SideDraft& side : sides) appendRelations(side, "playerEnemies", PlayerRelationship::Enemies);
    for (const SideDraft& side : sides) appendRelations(side, "playerAllies", PlayerRelationship::Allies);

    // `teams` is a HashMap, but nothing here iterates it: the loop walks the
    // source Vectors and uses the map only as a canonical-key collision set, so
    // both warnings below stay in authored order. Every later pass that does
    // iterate it sorts the keys first.
    container::HashMap<container::String, TeamDraft> teams;
    for (const script::legacy::LegacySidesListSource& sidesList : source.sidesLists()) {
        for (const LegacyTeamSource& sourceTeam : sidesList.teams) {
            TeamDraft team;
            team.name = fieldText(sourceTeam.properties, "teamName");
            team.owner = fieldText(sourceTeam.properties, "teamOwner");
            team.isSingleton = fieldBool(sourceTeam.properties, "teamIsSingleton");
            team.maximumInstances = fieldInt(sourceTeam.properties, "teamMaxInstances");
            team.plan = compileScenarioTeamPlan(sourceTeam.properties);
            team.fields = copyRawFields(sourceTeam.properties);
            if (team.name.empty()) {
                appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Warning,
                                 "ignored SidesList team with an empty teamName");
                continue;
            }
            const container::String key = canonical(team.name);
            if (!teams.emplace(key, std::move(team)).second) {
                appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Warning,
                                 "ignored duplicate SidesList team name '" + key + "'");
            }
        }
    }
    // SidesList::validateSides creates one singleton default team for each
    // side. Model it explicitly so later script/unit commands have a stable
    // team owner even when an old map omitted the editor-generated entry.
    for (const SideDraft& side : sides) {
        if (!side.player) continue;
        const container::String defaultName = "team" + side.name;
        const container::String key = canonical(defaultName);
        const auto found = teams.find(key);
        if (found == teams.end()) {
            teams.emplace(key, TeamDraft{
                .name = defaultName,
                .owner = side.name,
                .isSingleton = true,
                .isPlayerDefault = true,
            });
        } else {
            // SidesList::validateSides() repairs both of these fields for a
            // player-default team before TeamFactory sees it.  Mirror that
            // normalization rather than keeping an impossible non-singleton
            // duplicate of Player::m_defaultTeam.
            if (!equalAsciiInsensitive(found->second.owner, side.name)) {
                found->second.owner = side.name;
            }
            found->second.isSingleton = true;
            found->second.isPlayerDefault = true;
        }
    }
    // SidesList::validateSides() performs the same repair for its empty-name
    // neutral Side, whose default Team is simply "team". SideDraft excludes
    // that sentinel because PlayerRegistry already owns Neutral, so add the
    // prototype explicitly here rather than losing a legal old-map alias.
    {
        constexpr container::StringView neutralDefaultName = "team";
        const container::String key = canonical(neutralDefaultName);
        const auto found = teams.find(key);
        if (found == teams.end()) {
            teams.emplace(key, TeamDraft{
                .name = container::String(neutralDefaultName),
                .owner = "Neutral",
                .isSingleton = true,
                .isPlayerDefault = true,
            });
        } else {
            if (!equalAsciiInsensitive(found->second.owner, "Neutral")) {
                found->second.owner = "Neutral";
            }
            found->second.isSingleton = true;
            found->second.isPlayerDefault = true;
        }
    }

    // SidesList::validateSides() gives Player names precedence over Team
    // names. It removes a colliding Team before TeamFactory sees the list;
    // retaining one here would instead make ScenarioDefinition reject the
    // map as an ambiguous owner alias. Compare authored Side names only -- a
    // generated Player_N convenience alias is not an authored CkMp name.
    const auto isAuthoredPlayerName = [&sides](container::StringView candidate) {
        return std::any_of(sides.begin(), sides.end(), [candidate](const SideDraft& side) {
            return equalAsciiInsensitive(side.name, candidate);
        });
    };
    // Collect before erasing: iterating `teams` directly would emit these
    // warnings in HashMap order, which is neither source nor any other stable
    // order. The removal itself is order-independent, so sort the canonical keys
    // the same way the addScriptTeam loop below does and keep the diagnostic
    // stream reproducible across runs and platforms.
    container::Vector<container::String> collidingTeamKeys;
    for (const auto& [key, team] : teams) {
        if (isAuthoredPlayerName(team.name)) collidingTeamKeys.push_back(key);
    }
    std::sort(collidingTeamKeys.begin(), collidingTeamKeys.end());
    for (const container::String& key : collidingTeamKeys) {
        const auto team = teams.find(key);
        if (team == teams.end()) continue;
        appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Warning,
                         "removed SidesList team whose name conflicts with player '" +
                             team->second.name + "'");
        teams.erase(team);
    }

    container::Vector<container::String> teamKeys;
    teamKeys.reserve(teams.size());
    for (const auto& [key, unused] : teams) teamKeys.push_back(key);
    std::sort(teamKeys.begin(), teamKeys.end());
    for (const container::String& key : teamKeys) {
        TeamDraft& team = teams.at(key);
        if (team.owner.empty() || equalAsciiInsensitive(team.owner, team.name) ||
            !playerAliases.contains(canonical(team.owner))) {
            // RefCode validateSides reparents invalid Team owners to the
            // neutral player. A Team may not own itself either. Retain the
            // source fields, but normalize the live owner rather than leaving
            // a dangling map alias.
            team.owner = "Neutral";
        }
        if (definition->addScriptTeam({
                .name = std::move(team.name),
                .ownerAlias = std::move(team.owner),
                .isSingleton = team.isSingleton,
                .maximumInstances = team.maximumInstances,
                .isPlayerDefault = team.isPlayerDefault,
                .plan = std::move(team.plan),
                .fields = std::move(team.fields),
            })) {
            ++result.scriptTeamCount;
        }
    }

    for (const SideDraft& side : sides) {
        if (!side.player) continue;
        for (size_t buildListOrdinal = 0;
             buildListOrdinal < side.source->buildList.size();
             ++buildListOrdinal) {
            const script::legacy::LegacyBuildListEntrySource& entry =
                side.source->buildList[buildListOrdinal];
            ScenarioBuildIntent intent;
            intent.sourceSideOrdinal = side.sourceSideOrdinal;
            intent.sourceBuildListOrdinal = static_cast<uint32_t>(buildListOrdinal);
            intent.structureName = entry.buildingName;
            intent.templateName = entry.templateName;
            intent.objectScriptAttachment = entry.script;
            intent.ownerAlias = side.name;
            intent.resolvedOwner = side.player;
            intent.fixedPoseValid =
                admissibleBuildIntentValue(
                    entry.position[0], kMaximumBuildIntentHorizontal) &&
                admissibleBuildIntentValue(
                    entry.position[1], kMaximumBuildIntentHorizontal) &&
                admissibleBuildIntentValue(
                    entry.angle, kMaximumBuildIntentAngle);
            if (intent.fixedPoseValid) {
                intent.x = math::q32_32{entry.position[0]};
                intent.y = math::q32_32{entry.position[1]};
                intent.angle = math::q32_32{entry.angle};
            }
            // SidesList::ParseSidesDataChunk explicitly forces this legacy
            // build-list Z to ground level. The raw entry stays in fields,
            // while runtime intent follows the original placement behavior.
            intent.z = math::q32_32{};
            intent.initiallyBuilt = entry.initiallyBuilt;
            intent.fields = {
                {.key = "authoredZ", .value = std::to_string(entry.position[2])},
                {.key = "rebuildCount", .value = std::to_string(entry.rebuildCount)},
                {.key = "script", .value = entry.script},
                {.key = "health", .value = std::to_string(entry.health)},
                {.key = "whiner", .value = entry.whiner ? "true" : "false"},
                {.key = "unsellable", .value = entry.unsellable ? "true" : "false"},
                {.key = "repairable", .value = entry.repairable ? "true" : "false"},
            };
            if (definition->addBuildIntent(std::move(intent))) ++result.buildIntentCount;
        }
    }

    container::Vector<ScenarioValidationIssue> validationIssues;
    if (!definition->finalize(matchSetup, &validationIssues)) {
        for (const ScenarioValidationIssue& issue : validationIssues) {
            appendDiagnostic(result, LegacyScenarioDiagnosticSeverity::Error, issue.message);
        }
        return result;
    }
    result.definition = std::move(definition);
    return result;
}

} // namespace engine::scenario
