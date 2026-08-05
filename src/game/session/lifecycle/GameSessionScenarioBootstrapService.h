#pragma once

#include "core/container/container_types.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

namespace script {
class ScriptProgram;
namespace legacy {
class LegacyMapScriptParser;
class LegacyMapScriptSource;
}
}

// Single-use startup service for map-authored scripts, SidesList teams and
// initial BuildList materialization.  It receives the exact state partitions
// it owns during startup plus a narrow nested-spawn capability; no Session or
// complete ScriptInterface is retained.
class GameSessionScenarioBootstrapService final {
public:
    GameSessionScenarioBootstrapService(
        GameSessionContentStartState& content,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents,
        GameSessionWorldState& world,
        GameSessionLifecycleTransactionPort lifecycle) noexcept
        : m_content(content),
          m_ai(ai),
          m_presentation(presentation),
          m_objectEvents(objectEvents),
          m_world(world),
          m_lifecycle(lifecycle) {}

    void loadLegacyMapScriptProgram();
    [[nodiscard]] bool applyLegacyScenarioDefinition();
    [[nodiscard]] bool initializeObjectTeams();
    // PlayerTemplate StartingBuilding / StartingUnit0..9 materialization for a
    // lobby/skirmish roster. RefCode performs this in
    // placeNetworkBuildingsForPlayer, immediately after map objects and before
    // ThePlayerList->newMap() runs the authored AI BuildLists, so this must be
    // called in the same position relative to
    // materializeScenarioInitialBuildings().
    void materializeMatchStartingBases();
    void materializeScenarioInitialBuildings();

private:
    void installScriptProgram(
        container::SharedPtr<const script::ScriptProgram> program);
    // RefCode SidesList::prepareForMP_or_Skirmish(): for a skirmish or
    // multiplayer session whose map authored no scripts, graft
    // `Data\Scripts\SkirmishScripts.scb`'s ScriptLists and Team records onto
    // the map's sides.  Returns nullptr whenever the map must be left exactly
    // as authored, which is always the case for a campaign map.
    [[nodiscard]] container::SharedPtr<const script::legacy::LegacyMapScriptSource>
    substituteLegacySkirmishScripts(
        const script::legacy::LegacyMapScriptSource& mapSource,
        const script::legacy::LegacyMapScriptParser& parser) const;

    GameSessionContentStartState& m_content;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionObjectEventState& m_objectEvents;
    GameSessionWorldState& m_world;
    GameSessionLifecycleTransactionPort m_lifecycle;
};

} // namespace engine
