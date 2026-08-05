#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

enum ObjectAIAutoAcquireFlag : uint32_t {
    ObjectAIAutoAcquireYes = uint32_t{1} << 0u,
    ObjectAIAutoAcquireWhileStealthed = uint32_t{1} << 1u,
    ObjectAIAutoAcquireNo = uint32_t{1} << 2u,
    ObjectAIAutoAcquireNotWhileAttacking = uint32_t{1} << 3u,
    ObjectAIAutoAcquireAttackBuildings = uint32_t{1} << 4u,
};

// Frozen AIUpdateModuleData base fields shared by AIUpdateInterface and all
// derived stock AI modules. The final inherited Object recipe is compiled
// once; ECS/AI never parses ModuleData while a match is running.
struct ObjectAIBehaviorPlan final {
    uint32_t autoAcquireEnemiesWhenIdle = 0;
    uint32_t moodAttackCheckMilliseconds = 2000;
    bool forbidPlayerCommands = false;
};

[[nodiscard]] container::SharedPtr<const ObjectAIBehaviorPlan>
compileObjectAIBehaviorPlan(const ThingTemplate& templateData);

} // namespace game
