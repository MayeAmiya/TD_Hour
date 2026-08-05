#pragma once

#include "GameSessionScriptQueryPort.h"

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/definition/ObjectArchetype.h"

namespace engine {
class GameContentSnapshot;
class ObjectLifecycle;
}

namespace game::terrain {
class MapVisibilityAuthority;
}

namespace engine::script::detail {

[[nodiscard]] bool kindOfContains(
    const game::ObjectArchetype* archetype,
    game::ObjectKindOf wanted) noexcept;

[[nodiscard]] inline bool kindOfContains(
    const container::SharedPtr<const game::ObjectArchetype>& archetype,
    game::ObjectKindOf wanted) noexcept {
    return kindOfContains(archetype.get(), wanted);
}

[[nodiscard]] bool objectVisibleToPlayer(
    const ecs::registry& registry,
    const game::terrain::MapVisibilityAuthority& visibility,
    ecs::entity entity,
    PlayerId observer, uint64_t confirmedTick,
    bool enforceObjectConcealment) noexcept;

[[nodiscard]] bool templateIsInert(
    const ThingTemplateComponent* templateComponent) noexcept;

[[nodiscard]] game::LocomotorSurfaceMask teamAreaAllowedSurfaces(
    uint8_t allowed) noexcept;

[[nodiscard]] game::LocomotorSurfaceMask teamAreaObjectSurfaces(
    const GameContentSnapshot& content,
    const ThingTemplateComponent* templateComponent,
    const ObjectLocomotionComponent* locomotion);

[[nodiscard]] std::optional<ScriptWorldObjectSnapshot> scriptObjectSnapshot(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    ObjectId object);

} // namespace engine::script::detail
