#pragma once

#include <algorithm>

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/simulation/containment/ObjectContainment.h"

namespace engine {

[[nodiscard]] inline bool isObjectAIFactionStructure(
    const ObjectKindOfComponent* kinds) noexcept {
    if (!kinds) return false;
    constexpr game::ObjectKindOf factionKinds[] = {
        game::ObjectKindOf::FsPower,
        game::ObjectKindOf::FsFactory,
        game::ObjectKindOf::FsBaseDefense,
        game::ObjectKindOf::FsTechnology,
        game::ObjectKindOf::FsSupplyDropzone,
        game::ObjectKindOf::FsSuperweapon,
        game::ObjectKindOf::FsBlackMarket,
        game::ObjectKindOf::FsSupplyCenter,
        game::ObjectKindOf::FsStrategyCenter,
        game::ObjectKindOf::FsInternetCenter,
        game::ObjectKindOf::FsFake,
        game::ObjectKindOf::FsAdvancedTech,
        game::ObjectKindOf::FsBarracks,
        game::ObjectKindOf::FsWarfactory,
        game::ObjectKindOf::FsAirfield,
    };
    return std::any_of(std::begin(factionKinds), std::end(factionKinds),
        [kinds](game::ObjectKindOf kind) {
            return game::objectHasKind(kinds->mask, kind);
        });
}

// RefCode PartitionFilterInsignificantBuildings rejects only non-faction
// structures that expose a Contain interface but cannot currently present an
// armed garrison. A non-faction structure without Contain is retained.
[[nodiscard]] inline bool isObjectAIIgnoredInsignificantBuilding(
    const ecs::registry& registry, ecs::entity entity,
    const ObjectKindOfComponent* kinds) noexcept {
    if (!kinds || !game::objectHasKind(
            kinds->mask, game::ObjectKindOf::Structure) ||
        isObjectAIFactionStructure(kinds)) {
        return false;
    }
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, entity);
    if (!runtime || !runtime->plan) return false;
    const bool garrisonable =
        (runtime->plan->kindMask & objectContainmentKindBit(
            ObjectContainmentKind::Garrison)) != 0;
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, entity);
    return !garrisonable || !contents || contents->objects.empty();
}

} // namespace engine
