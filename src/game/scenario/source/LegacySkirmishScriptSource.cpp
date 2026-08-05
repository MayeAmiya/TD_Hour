#include "core/container/string_utils.h"
#include "game/scenario/source/LegacySkirmishScriptSource.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace engine::script::legacy {
namespace {

constexpr container::StringView kCivilianFactionName = "FactionCivilian";
constexpr size_t kMaximumRetainedMessages = 64;

constexpr auto equalNoCase = container::asciiEqualIgnoreCase;

void addMessage(LegacySkirmishSubstitutionReport& report, container::String message) {
    if (report.messages.size() >= kMaximumRetainedMessages) return;
    report.messages.push_back(std::move(message));
}

// Ranges copied out of the `.scb` source must keep addressing their own bytes.
// The composed source concatenates the map stream and the `.scb` stream, so
// every `.scb` range moves by the map stream's length.  An empty range stays
// canonical {0,0} so `LegacySourceRange::empty()` consumers are unaffected.
void shiftRange(LegacySourceRange& range, uint64_t shift) noexcept {
    if (range.size != 0) range.offset += shift;
}

void shiftRanges(LegacyScriptParameter& parameter, uint64_t shift) noexcept {
    shiftRange(parameter.serialized, shift);
}

void shiftRanges(LegacyScriptInstructionSource& instruction, uint64_t shift) noexcept {
    shiftRange(instruction.serialized, shift);
    shiftRange(instruction.trailingData, shift);
    for (LegacyScriptParameter& parameter : instruction.parameters) {
        shiftRanges(parameter, shift);
    }
}

void shiftRanges(LegacyOrConditionSource& clause, uint64_t shift) noexcept {
    shiftRange(clause.serialized, shift);
    for (LegacyScriptInstructionSource& condition : clause.conditions) {
        shiftRanges(condition, shift);
    }
}

void shiftRanges(LegacyScriptSource& script, uint64_t shift) noexcept {
    shiftRange(script.serialized, shift);
    for (LegacyOrConditionSource& clause : script.conditions) shiftRanges(clause, shift);
    for (LegacyScriptInstructionSource& action : script.actions) shiftRanges(action, shift);
    for (LegacyScriptInstructionSource& action : script.falseActions) shiftRanges(action, shift);
}

void shiftRanges(LegacyScriptGroupSource& group, uint64_t shift) noexcept {
    shiftRange(group.serialized, shift);
    for (LegacyScriptSource& script : group.scripts) shiftRanges(script, shift);
}

void shiftRanges(LegacyScriptListSource& list, uint64_t shift) noexcept {
    shiftRange(list.serialized, shift);
    for (LegacyScriptSource& script : list.scripts) shiftRanges(script, shift);
    for (LegacyScriptGroupSource& group : list.groups) shiftRanges(group, shift);
}

void shiftRanges(LegacyTeamSource& team, uint64_t shift) noexcept {
    for (LegacyDictionaryEntry& entry : team.properties) {
        shiftRange(entry.serialized, shift);
    }
}

[[nodiscard]] const LegacyDictionaryEntry* findProperty(
    const container::Vector<LegacyDictionaryEntry>& properties, container::StringView name) {
    const auto found = std::find_if(properties.begin(), properties.end(),
                                    [name](const LegacyDictionaryEntry& entry) {
                                        return equalNoCase(entry.key.resolvedName, name);
                                    });
    return found == properties.end() ? nullptr : &*found;
}

[[nodiscard]] container::StringView propertyText(
    const container::Vector<LegacyDictionaryEntry>& properties, container::StringView name) {
    const LegacyDictionaryEntry* entry = findProperty(properties, name);
    if (!entry) return {};
    const container::String* text = std::get_if<container::String>(&entry->value);
    return text ? container::StringView(*text) : container::StringView{};
}

[[nodiscard]] container::StringView sidePlayerName(const LegacySideSource& side) {
    return propertyText(side.properties, "playerName");
}

[[nodiscard]] container::StringView sideFaction(const LegacySideSource& side) {
    return propertyText(side.properties, "playerFaction");
}

// RefCode compares the faction with AsciiString::operator==, which is
// case-sensitive.  Mirror that exactly: the `gotScripts` decision selects
// between "run the skirmish AI" and "leave a campaign map alone", so it must
// not become more permissive than the original.
[[nodiscard]] bool isCivilianSide(const LegacySideSource& side) {
    return sideFaction(side) == kCivilianFactionName;
}

[[nodiscard]] container::StringView withoutPrefix(container::StringView value,
                                                  container::StringView prefix) {
    return container::startsWithIgnoreCase(value, prefix) ? value.substr(prefix.size()) : value;
}

} // namespace

bool legacySideMatchesFactionAliases(
    container::StringView sourceFaction, container::StringView sourcePlayerName,
    container::StringView factionName, container::StringView factionSide,
    container::StringView factionBaseSide) {
    const container::StringView factionSuffix = withoutPrefix(sourceFaction, "Faction");
    const container::StringView playerSuffix = withoutPrefix(sourcePlayerName, "Plyr");
    return (!sourceFaction.empty() &&
            (equalNoCase(sourceFaction, factionName) ||
             equalNoCase(factionSuffix, factionName) ||
             equalNoCase(factionSuffix, factionSide) ||
             equalNoCase(factionSuffix, factionBaseSide))) ||
        (!sourcePlayerName.empty() &&
         (equalNoCase(sourcePlayerName, factionName) ||
          equalNoCase(playerSuffix, factionName) ||
          equalNoCase(playerSuffix, factionSide) ||
          equalNoCase(playerSuffix, factionBaseSide)));
}

bool legacyMapAuthorsScripts(const LegacyMapScriptSource& mapSource) {
    const container::Span<const LegacyPlayerScriptsListSource> playerScriptLists =
        mapSource.playerScriptLists();
    for (const LegacySidesListSource& sidesList : mapSource.sidesLists()) {
        for (const uint32_t playerScriptsIndex : sidesList.playerScriptsListIndices) {
            if (playerScriptsIndex >= playerScriptLists.size()) continue;
            const LegacyPlayerScriptsListSource& playerScripts =
                playerScriptLists[playerScriptsIndex];
            for (size_t listIndex = 0; listIndex < playerScripts.playerLists.size(); ++listIndex) {
                // RefCode's reader assigns the i-th ScriptList to the i-th
                // Side, and prepareForMP_or_Skirmish() then skips only the
                // FactionCivilian sides while testing for authored scripts.
                if (listIndex < sidesList.sides.size() &&
                    isCivilianSide(sidesList.sides[listIndex])) {
                    continue;
                }
                const LegacyScriptListSource& list = playerScripts.playerLists[listIndex];
                if (!list.scripts.empty() || !list.groups.empty()) return true;
            }
        }
    }
    return false;
}

bool isLegacyStandaloneScriptFile(const LegacyMapScriptSource& source) {
    return !source.scriptPlayerNameSets().empty() && !source.playerScriptLists().empty();
}

// Grants the composed source write access to LegacyMapScriptSource's immutable
// state without exposing a public mutator.
class LegacySkirmishSourceBuilder final {
public:
    [[nodiscard]] static container::SharedPtr<const LegacyMapScriptSource> compose(
        const LegacyMapScriptSource& mapSource,
        const LegacyMapScriptSource& skirmishSource,
        container::Vector<LegacyScriptListSource> graftedLists,
        container::Vector<LegacyTeamSource> graftedTeams,
        const LegacySidesListSource& sourceSidesList) {
        auto composed = std::make_shared<LegacyMapScriptSource>();

        const container::Span<const uint8_t> mapBytes = mapSource.ckMpBytes();
        const container::Span<const uint8_t> skirmishBytes = skirmishSource.ckMpBytes();
        composed->m_ckMpBytes.reserve(mapBytes.size() + skirmishBytes.size());
        composed->m_ckMpBytes.insert(composed->m_ckMpBytes.end(), mapBytes.begin(), mapBytes.end());
        composed->m_ckMpBytes.insert(composed->m_ckMpBytes.end(),
                                     skirmishBytes.begin(), skirmishBytes.end());

        // Every parsed record already carries its resolved NameKey/label text,
        // so only the map's own symbol table is retained. Merging two
        // file-local ID spaces would produce ambiguous symbolName() answers
        // without changing any compiled program.
        composed->m_symbols.assign(mapSource.symbols().begin(), mapSource.symbols().end());
        composed->m_unknownChunks.assign(mapSource.unknownChunks().begin(),
                                         mapSource.unknownChunks().end());
        composed->m_opaqueData.assign(mapSource.opaqueData().begin(),
                                      mapSource.opaqueData().end());

        LegacyPlayerScriptsListSource playerScripts;
        playerScripts.sourceVersion = skirmishSource.playerScriptLists().front().sourceVersion;
        playerScripts.serialized = {};
        playerScripts.playerLists = std::move(graftedLists);
        composed->m_playerScriptLists.push_back(std::move(playerScripts));

        LegacySidesListSource sidesList;
        sidesList.sourceVersion = sourceSidesList.sourceVersion;
        sidesList.serialized = sourceSidesList.serialized;
        sidesList.sides = sourceSidesList.sides;
        sidesList.trailingData = sourceSidesList.trailingData;
        sidesList.teams = std::move(graftedTeams);
        // Exactly one PlayerScriptsList, holding one ScriptList per Side in
        // Side order. That is the shape the script compiler expects and it is
        // what keeps evaluation order identical on every peer.
        sidesList.playerScriptsListIndices.push_back(0);
        composed->m_sidesLists.push_back(std::move(sidesList));
        return composed;
    }
};

container::SharedPtr<const LegacyMapScriptSource> graftLegacySkirmishScripts(
    const LegacyMapScriptSource& mapSource,
    const LegacyMapScriptSource& skirmishSource,
    const LegacySkirmishSubstitutionOptions& options,
    LegacySkirmishSubstitutionReport& report) {
    report = {};

    const container::Span<const LegacySidesListSource> mapSidesLists = mapSource.sidesLists();
    if (mapSidesLists.size() != 1) {
        addMessage(report, mapSidesLists.empty()
            ? "map has no SidesList; skirmish script substitution skipped"
            : "map has more than one SidesList; skirmish script substitution skipped");
        return nullptr;
    }
    const LegacySidesListSource& mapSidesList = mapSidesLists.front();
    if (mapSidesList.sides.empty()) {
        addMessage(report, "map SidesList declares no sides; skirmish script substitution skipped");
        return nullptr;
    }
    if (!isLegacyStandaloneScriptFile(skirmishSource)) {
        addMessage(report,
                   "skirmish script file has no ScriptsPlayers/PlayerScriptsList pair; substitution skipped");
        return nullptr;
    }
    if (options.roster.empty()) {
        // Without a resolved roster the graft cannot separate an AI side from
        // a human one, and handing the human player the AI base builder would
        // be far worse than running no skirmish scripts at all.
        addMessage(report, "no resolved roster supplied; skirmish script substitution skipped");
        return nullptr;
    }

    const LegacyScriptPlayerNamesSource& names = skirmishSource.scriptPlayerNameSets().front();
    const LegacyPlayerScriptsListSource& skirmishLists = skirmishSource.playerScriptLists().front();
    if (skirmishSource.scriptPlayerNameSets().size() > 1 ||
        skirmishSource.playerScriptLists().size() > 1) {
        addMessage(report,
                   "skirmish script file carries more than one ScriptsPlayers/PlayerScriptsList; only the first is used");
    }
    const size_t pairedCount = std::min(names.names.size(), skirmishLists.playerLists.size());
    if (names.names.size() != skirmishLists.playerLists.size()) {
        addMessage(report,
                   "skirmish script file names " + std::to_string(names.names.size()) +
                       " players for " + std::to_string(skirmishLists.playerLists.size()) +
                       " ScriptLists; only the paired prefix is used");
    }
    if (pairedCount == 0) {
        addMessage(report, "skirmish script file pairs no player name with a ScriptList");
        return nullptr;
    }

    // RefCode matches by player name, never by index: a skirmish map orders
    // PlyrCivilian before the general variants while the shipped `.scb` orders
    // it last.  First definition wins, matching AsciiString == comparison.
    const auto skirmishListFor = [&](container::StringView playerName) -> const LegacyScriptListSource* {
        if (playerName.empty()) return nullptr;
        for (size_t index = 0; index < pairedCount; ++index) {
            if (names.names[index] == playerName) return &skirmishLists.playerLists[index];
        }
        return nullptr;
    };

    // Player::initFromDict() gives a human player the CIVILIAN template's
    // ScriptList (Player.cpp:830-857) and only a computer player the faction
    // template's tactical list. Resolve the civilian donor by locating the
    // map's FactionCivilian side and looking its name up in the `.scb`.
    const LegacyScriptListSource* civilianList = nullptr;
    for (const LegacySideSource& side : mapSidesList.sides) {
        if (!isCivilianSide(side)) continue;
        civilianList = skirmishListFor(sidePlayerName(side));
        if (civilianList) break;
    }
    if (!civilianList) {
        addMessage(report,
                   "skirmish script file has no civilian ScriptList; human players receive no skirmish scripts");
    }

    // Mirror LegacyScenarioCompiler::compile's Side -> roster player election
    // (LegacyScenarioCompiler.cpp, chooseMatchingRosterPlayer): walk Sides in
    // authored order and claim the first still-unclaimed roster participant
    // whose faction aliases match.  Reproducing that order here is what makes
    // the grafted ScriptList land on the very player the Scenario compiler
    // will bind to the same Side.
    container::Vector<size_t> sideRosterEntry(mapSidesList.sides.size(),
                                              options.roster.size());
    container::Vector<bool> rosterClaimed(options.roster.size(), false);
    for (size_t sideIndex = 0; sideIndex < mapSidesList.sides.size(); ++sideIndex) {
        const LegacySideSource& side = mapSidesList.sides[sideIndex];
        const container::StringView playerName = sidePlayerName(side);
        // The Scenario compiler normalizes both of these to the session
        // neutral player before it consults the roster.
        if (playerName.empty() || equalNoCase(playerName, "Neutral")) continue;
        for (size_t rosterIndex = 0; rosterIndex < options.roster.size(); ++rosterIndex) {
            if (rosterClaimed[rosterIndex]) continue;
            const LegacySkirmishRosterEntry& entry = options.roster[rosterIndex];
            if (!legacySideMatchesFactionAliases(sideFaction(side), playerName,
                                                 entry.factionTemplateName, entry.factionSide,
                                                 entry.factionBaseSide)) {
                continue;
            }
            sideRosterEntry[sideIndex] = rosterIndex;
            rosterClaimed[rosterIndex] = true;
            break;
        }
    }

    const uint64_t shift = static_cast<uint64_t>(mapSource.ckMpBytes().size());
    const container::Span<const LegacyPlayerScriptsListSource> mapPlayerScriptLists =
        mapSource.playerScriptLists();
    const LegacyPlayerScriptsListSource* mapLists = nullptr;
    for (const uint32_t playerScriptsIndex : mapSidesList.playerScriptsListIndices) {
        if (playerScriptsIndex < mapPlayerScriptLists.size()) {
            mapLists = &mapPlayerScriptLists[playerScriptsIndex];
            break;
        }
    }

    container::Vector<LegacyScriptListSource> graftedLists;
    graftedLists.reserve(mapSidesList.sides.size());
    // Side names whose tactical list was actually grafted. Only their Team
    // records are worth materializing; RefCode likewise only copies the Teams
    // owned by the template a real player adopted.
    container::Vector<container::String> aiGraftedSideNames;
    for (size_t sideIndex = 0; sideIndex < mapSidesList.sides.size(); ++sideIndex) {
        const LegacySideSource& side = mapSidesList.sides[sideIndex];
        const container::StringView playerName = sidePlayerName(side);
        const size_t rosterIndex = sideRosterEntry[sideIndex];
        const LegacyScriptListSource* donor = nullptr;
        bool isAiGraft = false;
        if (rosterIndex < options.roster.size()) {
            if (options.roster[rosterIndex].isHuman) {
                donor = civilianList;
            } else {
                donor = skirmishListFor(playerName);
                isAiGraft = donor != nullptr;
                if (!donor) {
                    addMessage(report,
                               "skirmish script file has no ScriptList for AI side '" +
                                   container::String(playerName) + "'");
                }
            }
        }
        if (donor) {
            LegacyScriptListSource list = *donor;
            shiftRanges(list, shift);
            graftedLists.push_back(std::move(list));
            ++report.graftedScriptListCount;
            if (isAiGraft) aiGraftedSideNames.emplace_back(playerName);
            continue;
        }
        // No real participant is bound to this Side. RefCode's equivalent
        // template Side is never a live player either, so leave whatever the
        // map authored (which `legacyMapAuthorsScripts` has already proven to
        // be empty for every non-civilian Side).
        if (mapLists && sideIndex < mapLists->playerLists.size()) {
            graftedLists.push_back(mapLists->playerLists[sideIndex]);
        } else {
            graftedLists.push_back({});
        }
    }

    // prepareForMP_or_Skirmish() clears both Team records and repopulates them
    // from the `.scb` alone, keeping only Teams whose owner is a known side.
    container::Vector<LegacyTeamSource> graftedTeams;
    size_t limitedTeamCount = 0;
    if (!skirmishSource.scriptTeamSets().empty() && !aiGraftedSideNames.empty()) {
        const LegacyScriptTeamsSource& teamSet = skirmishSource.scriptTeamSets().front();
        graftedTeams.reserve(std::min(teamSet.teams.size(), options.maxGraftedTeams));
        for (const LegacyTeamSource& sourceTeam : teamSet.teams) {
            const container::StringView owner = propertyText(sourceTeam.properties, "teamOwner");
            const bool wanted = std::any_of(
                aiGraftedSideNames.begin(), aiGraftedSideNames.end(),
                [owner](const container::String& name) { return name == owner; });
            if (!wanted) {
                ++report.skippedTeamCount;
                continue;
            }
            if (graftedTeams.size() >= options.maxGraftedTeams) {
                ++report.skippedTeamCount;
                ++limitedTeamCount;
                continue;
            }
            LegacyTeamSource team = sourceTeam;
            shiftRanges(team, shift);
            graftedTeams.push_back(std::move(team));
        }
        report.graftedTeamCount = graftedTeams.size();
        if (limitedTeamCount != 0) {
            addMessage(report, "skirmish Team record hit the configured graft limit; " +
                                   std::to_string(limitedTeamCount) + " owned Teams dropped");
        }
    }

    if (report.graftedScriptListCount == 0) {
        addMessage(report, "no side matched a skirmish ScriptList; substitution skipped");
        return nullptr;
    }

    container::SharedPtr<const LegacyMapScriptSource> composed =
        LegacySkirmishSourceBuilder::compose(mapSource, skirmishSource, std::move(graftedLists),
                                             std::move(graftedTeams), mapSidesList);
    report.applied = composed != nullptr;
    return composed;
}

} // namespace engine::script::legacy
