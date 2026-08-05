#pragma once

#include "core/container/hash_containers.h"

#include <cstddef>

namespace engine {

struct TerrainConstructionDefinition final {
    container::String name;
    bool restrictConstruction = false;
};

// Simulation-authoritative projection of Terrain.ini's TerrainType records.
// Only the construction bit belongs here; textures/classes/blend presentation
// remain renderer-owned. GameDataLoader compiles this value before session
// startup and ObjectSimulationRules copies it into the match.
class TerrainConstructionCatalog final {
public:
    void clear() noexcept { m_definitions.clear(); }

    // Applies one source after all lower-priority sources. A newly named
    // Terrain copies DefaultTerrain exactly once, matching
    // TerrainTypeCollection::newTerrain; a later DefaultTerrain override does
    // not retroactively rewrite already-created definitions.
    [[nodiscard]] bool applyLegacyIni(
        container::StringView content,
        container::String* error = nullptr);
    [[nodiscard]] bool applyLegacyIniFile(
        container::StringView path,
        container::String* error = nullptr);

    [[nodiscard]] const TerrainConstructionDefinition* find(
        container::StringView name) const noexcept;
    [[nodiscard]] bool restrictsConstruction(
        container::StringView name) const noexcept;

    [[nodiscard]] size_t size() const noexcept { return m_definitions.size(); }
    [[nodiscard]] size_t restrictedCount() const noexcept;

private:
    container::HashMap<container::String, TerrainConstructionDefinition>
        m_definitions;
};

} // namespace engine
