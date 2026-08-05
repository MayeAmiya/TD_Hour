#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

#include "game/object/plan/world/ObjectSpyVisionPlanTypes.h"
namespace engine {

class ObjectLifecycle;
class PlayerRegistry;
class UpgradeCatalog;
struct ObjectKindOfComponent;
struct ObjectSimulationRules;

struct ObjectSpyVisionRuntime final {
    uint64_t deactivateTick = 0;
    uint64_t nextActivationTick = 0;
    uint64_t sabotageDisabledUntilTick = 0;
    bool upgradeActivated = false;
    bool active = false;
    bool cycleInitialized = false;
    bool resetTimersAfterDisabled = false;
    bool wasGenerallyDisabled = false;
};

struct ObjectSpyVisionComponent final {
    container::SharedPtr<const game::ObjectSpyVisionPlan> plan;
    container::Vector<ObjectSpyVisionRuntime> instances;
};

[[nodiscard]] bool objectSpyVisionMatchesKinds(
    const game::ObjectSpyVisionRule& rule,
    const ObjectKindOfComponent* kinds) noexcept;

class ObjectSpyVisionSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    // Active special powers address the source by stable ObjectId and replace
    // the first authored SpyVisionUpdate deadline. A zero duration means
    // permanent, matching the legacy update module contract.
    [[nodiscard]] bool activateForTicks(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t durationTicks,
        uint64_t confirmedTick) const;

    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                const PlayerRegistry& players,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                const UpgradeCatalog* upgradeCatalog = nullptr) const;

    // Internet-center sabotage suppresses every SpyVisionUpdate owned by the
    // victim, without visually disabling the owner's other buildings.
    [[nodiscard]] bool setPlayerDisabledUntil(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        PlayerId player, uint64_t untilTick,
        uint64_t confirmedTick) const;
};

} // namespace engine
