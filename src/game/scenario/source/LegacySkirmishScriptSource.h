#pragma once

#include "core/container/container_types.h"

#include "game/scenario/source/LegacyMapScriptSource.h"

#include <cstddef>

// Zero Hour keeps its entire skirmish AI tactical layer outside the map, in a
// standalone compiled script file.  RefCode grafts that file onto the map's
// sides in SidesList::prepareForMP_or_Skirmish() (SidesList.cpp:479-560): when
// no side authored scripts, it reads `data\Scripts\SkirmishScripts.scb` and
// replaces the sides' ScriptLists and the whole Team record with the ones the
// file carries.  This header is the pure, immutable-source equivalent: it
// composes a parsed map source and a parsed `.scb` source into one new
// LegacyMapScriptSource, so both the script compiler and the Scenario compiler
// consume the substituted program without either of them knowing about
// skirmish at all.
namespace engine::script::legacy {

// Shipped location of the compiled skirmish scripts.  RefCode spells it
// `data\Scripts\SkirmishScripts.scb`; the VFS is case-insensitive and uses
// forward slashes.
inline constexpr container::StringView kLegacySkirmishScriptPath =
    "Data/Scripts/SkirmishScripts.scb";

// One resolved match participant, reduced to the faction aliases the graft
// needs.  Passing plain strings keeps this source-model layer free of any
// FactionTemplate, PlayerRegistry or match-setup dependency.
struct LegacySkirmishRosterEntry final {
    container::String factionTemplateName;
    container::String factionSide;
    container::String factionBaseSide;
    bool isHuman = false;
};

struct LegacySkirmishSubstitutionOptions final {
    // Resolved participants in canonical (slot) order.  Required: without it
    // the graft cannot tell an AI side from a human one and would hand the
    // human player the AI base builder.
    container::Vector<LegacySkirmishRosterEntry> roster;
    // Bounded so an oversized or hostile `.scb` cannot make startup unbounded.
    size_t maxGraftedTeams = 8192;
};

struct LegacySkirmishSubstitutionReport final {
    bool applied = false;
    size_t graftedScriptListCount = 0;
    size_t graftedTeamCount = 0;
    size_t skippedTeamCount = 0;
    // Ordered, bounded diagnostics.  Order follows Side order then `.scb`
    // record order, so two peers produce byte-identical text.
    container::Vector<container::String> messages;
};

// SidesList::prepareForMP_or_Skirmish()'s `gotScripts` test, exactly: a side
// whose playerFaction is "FactionCivilian" never counts, and a side counts
// only when its own ScriptList holds at least one root Script or ScriptGroup.
// A campaign or Challenge map authors scripts and therefore returns true,
// which is what keeps it completely untouched.
[[nodiscard]] bool legacyMapAuthorsScripts(const LegacyMapScriptSource& mapSource);

// True when `source` looks like a standalone `.scb` script file: it carries a
// top-level ScriptsPlayers name list and at least one PlayerScriptsList.
[[nodiscard]] bool isLegacyStandaloneScriptFile(const LegacyMapScriptSource& source);

// Grafts `skirmishSource`'s ScriptLists and Team records onto `mapSource`'s
// sides and returns a new immutable source.  Returns nullptr when nothing
// could be grafted, in which case the caller keeps the unmodified map source.
// The caller is responsible for having already established that substitution
// applies (skirmish/multiplayer-style session and !legacyMapAuthorsScripts).
[[nodiscard]] container::SharedPtr<const LegacyMapScriptSource> graftLegacySkirmishScripts(
    const LegacyMapScriptSource& mapSource,
    const LegacyMapScriptSource& skirmishSource,
    const LegacySkirmishSubstitutionOptions& options,
    LegacySkirmishSubstitutionReport& report);

// The alias rule LegacyScenarioCompiler uses when it binds a SidesList Side to
// a resolved roster player, expressed over plain strings so the graft and the
// Scenario compiler cannot drift apart.
[[nodiscard]] bool legacySideMatchesFactionAliases(
    container::StringView sourceFaction, container::StringView sourcePlayerName,
    container::StringView factionName, container::StringView factionSide,
    container::StringView factionBaseSide);

} // namespace engine::script::legacy
