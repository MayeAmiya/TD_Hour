#pragma once

#include <cstdint>

namespace engine {

class GameSessionContentStartState;
class GameSessionWorldState;

// Confirmed-frame maintenance derived exclusively from world objects and the
// player roster.  Visibility lookers and player energy/radar aggregates are
// world projections, not script-interface behavior.
class GameSessionWorldMaintenanceService final {
public:
    GameSessionWorldMaintenanceService(
        GameSessionContentStartState& content,
        GameSessionWorldState& world) noexcept
        : m_content(content), m_world(world) {}

    void updateMapVisibilityLookers(uint64_t confirmedTick);
    void refreshObjectDerivedPlayerAggregates(uint64_t confirmedTick);
    void updatePlayerPeriodicState(uint64_t confirmedTick);
    void refreshSpatialIndex();
    void updateTerrainLogic(
        uint64_t confirmedTick, uint32_t logicFramesPerSecond);

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
};

} // namespace engine
