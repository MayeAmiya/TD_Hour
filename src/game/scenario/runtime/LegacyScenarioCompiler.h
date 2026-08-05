#pragma once

#include "core/container/container_types.h"

#include "game/scenario/runtime/ScenarioDefinition.h"
#include "game/player/FactionTemplate.h"
#include "game/scenario/source/LegacyMapScriptSource.h"

#include <cstdint>
namespace engine::scenario {

// `SidesList` is not merely script metadata in the original game.  It is the
// map's authoritative roster, diplomacy and build-list source, consumed by
// PlayerList::newGame() before objects/scripts start.  This compiler turns the
// detached CkMp representation into immutable, session-owned scenario data;
// it deliberately does not create ECS entities or execute any scripts.
enum class LegacyScenarioDiagnosticSeverity : uint8_t {
    Info,
    Warning,
    Error,
};

struct LegacyScenarioDiagnostic final {
    LegacyScenarioDiagnosticSeverity severity = LegacyScenarioDiagnosticSeverity::Info;
    container::String message;
    uint64_t sourceOffset = 0;
    container::String sourcePath;
};

struct LegacyScenarioCompileResult final {
    container::SharedPtr<const ScenarioDefinition> definition;
    container::Vector<LegacyScenarioDiagnostic> diagnostics;
    size_t sourceSideCount = 0;
    size_t mappedPlayerCount = 0;
    size_t scriptTeamCount = 0;
    size_t buildIntentCount = 0;

    [[nodiscard]] bool usable() const noexcept { return definition != nullptr; }
    [[nodiscard]] bool hasErrors() const noexcept;
};

// Maps original SidesList names onto the already-resolved command roster where
// possible (FactionAmerica -> the local FactionAmerica participant), then
// allocates otherwise-unclaimed map PlayerIds deterministically for scenario
// AI/civilian sides.  This preserves the old map aliases without treating the
// eight lobby slots as the complete player namespace.
class LegacyScenarioCompiler final {
public:
    [[nodiscard]] LegacyScenarioCompileResult compile(
        const script::legacy::LegacyMapScriptSource& source,
        const ResolvedMatchSetup& matchSetup,
        const MultiplayerRuleset& ruleset) const;
};

} // namespace engine::scenario
