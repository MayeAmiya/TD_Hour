#pragma once

#include "core/ecs/registry.h"

#include <cstdint>

namespace engine {

class PlayerRegistry;
enum class ObjectEnergyBonusSource : uint8_t;

// Session-owned aggregate pass for RefCode Energy. It deliberately scans only
// entities carrying ObjectEnergyComponent (normally structures), not every
// unit, and publishes its result through PlayerRegistry's canonical state.
// This gives future PowerPlant/EMP/Radar/Script consumers one value boundary
// without reviving Object*/Player* influence callbacks.
class ObjectEnergySystem final {
public:
    // Materializes immutable EnergyProduction/EnergyBonus recipe values on a
    // newly assembled object. A zero/zero recipe carries no component and is
    // therefore absent from every subsequent energy pass.
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    // Rebuilds the small per-player aggregate at a confirmed boundary. It
    // observes lifecycle, construction and current Body disabled state, so a
    // pending destroy or subdual transition cannot leave stale power behind.
    void update(ecs::registry& registry, PlayerRegistry& players,
                uint64_t confirmedTick) const;

    // Future PowerPlantUpgrade/Overcharge confirmed transactions mutate their
    // own source bit, not a shared boolean: the two legacy effects can both
    // contribute EnergyBonus at once. They still rely on the next confirmed
    // aggregate pass for PlayerRegistry publication, avoiding a second
    // mutable power ledger.
    [[nodiscard]] bool setBonusProductionSource(ecs::registry& registry,
                                                 ecs::entity entity,
                                                 ObjectEnergyBonusSource source,
                                                 bool active) const;
};

} // namespace engine
