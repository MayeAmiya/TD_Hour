#pragma once

#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/base/MapContentIdentity.h"
#include "game/player/PlayerTypes.h"
#include "game/render/ClientTerrainObjectStore.h"

#include <cstdint>

namespace game {
struct ObjectArchetype;
}

namespace engine {

class GameSession;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

struct MapImportOwnerResolution final {
    PlayerId player = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    bool usedFallback = false;
    bool usesScenarioTeam = false;
    bool scenarioTeamUnresolved = false;
};

struct MapImportedObjectState final {
    math::q32_32 currentHealth{};
    math::q32_32 maximumHealth{};
    uint32_t damageState = 0xffffffffu;
    game::ModelConditionMask modelConditions;
    container::String modelAsset;
    bool hasHealth = false;
    bool hasVisual = false;
};

// Startup-only capability used by CkMp ObjectList import. It exposes values
// and import transactions, never the ECS registry, PlayerRegistry, content
// snapshot, terrain owner or complete script interface.
class GameSessionMapImportPort final {
public:
    [[nodiscard]] ClientTerrainImportPolicy clientTerrainPolicy() const;
    [[nodiscard]] game::ModelConditionMask initialModelConditions() const;
    [[nodiscard]] container::SharedPtr<const game::ObjectArchetype>
        findObjectArchetype(container::StringView name) const;
    [[nodiscard]] const game::MapContentIdentity& mapIdentity() const noexcept;
    [[nodiscard]] uint64_t simulationContentFingerprint() const noexcept;
    [[nodiscard]] uint64_t presentationEpoch() const noexcept;
    [[nodiscard]] int64_t groundHeightRaw(int64_t x, int64_t y) const noexcept;
    [[nodiscard]] float groundHeight(float x, float y) const noexcept;
    void beginTerrainHeightMutationBatch() noexcept;
    void endTerrainHeightMutationBatch() noexcept;
    [[nodiscard]] MapImportOwnerResolution resolveOwner(
        container::StringView authoredOwner);
    [[nodiscard]] bool activateScenarioTeam(ObjectTeamId team);
    [[nodiscard]] MapImportedObjectState importedObjectState(
        ObjectId object) const;

private:
    friend class GameSession;
    explicit GameSessionMapImportPort(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation) noexcept
        : m_content(content), m_world(world), m_presentation(presentation) {}

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
};

} // namespace engine
