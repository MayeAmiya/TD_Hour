#include "game/session/command/GameSessionPlayerCommandPolicy.h"

#include "game/object/ai/definition/ObjectAIBehaviorPlan.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/definition/ObjectArchetype.h"

#include <optional>

namespace engine::session_command_policy {

bool objectForbidsPlayerCommands(
    const ecs::registry& registry,
    const ObjectLifecycle& objects,
    ObjectId object) noexcept {
    const std::optional<ecs::entity> entity = objects.entityFromId(object);
    if (!entity) return false;
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, *entity);
    return type && type->archetype && type->archetype->aiBehaviorPlan &&
        type->archetype->aiBehaviorPlan->forbidPlayerCommands;
}

} // namespace engine::session_command_policy
