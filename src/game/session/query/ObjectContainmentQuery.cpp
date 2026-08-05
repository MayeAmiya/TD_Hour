#include "game/session/query/ObjectContainmentQuery.h"

#include <optional>

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/object/simulation/containment/ObjectContainment.h"

namespace engine::session_query
{
namespace
{

[[nodiscard]] std::optional<ObjectContainmentKind> exitNetworkKind(
    const ObjectContainmentRuntimeComponent* runtime) noexcept
{
    if (!runtime || !runtime->plan)
        return std::nullopt;
    for (const ObjectContainmentRule& rule : runtime->plan->rules)
    {
        if (rule.kind == ObjectContainmentKind::Cave || rule.kind == ObjectContainmentKind::Tunnel)
        {
            return rule.kind;
        }
    }
    return std::nullopt;
}

} // namespace

bool canExitPassengerThrough(const ecs::registry& registry,
                             const ObjectLifecycle& objects,
                             const ObjectOwnershipIndex& ownership,
                             ObjectId container,
                             ObjectId passenger) noexcept
{
    if (!container || !passenger)
        return false;
    const std::optional<ecs::entity> passengerEntity = objects.entityFromId(passenger);
    const ObjectContainedByComponent* edge =
        passengerEntity ? ecs::try_get<ObjectContainedByComponent>(registry, *passengerEntity) : nullptr;
    if (!edge || !edge->container)
        return false;
    if (edge->container == container)
        return true;

    const std::optional<ecs::entity> selectedEntity = objects.entityFromId(container);
    const std::optional<ecs::entity> actualEntity = objects.entityFromId(edge->container);
    const ObjectContainmentRuntimeComponent* selected =
        selectedEntity ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *selectedEntity) : nullptr;
    const ObjectContainmentRuntimeComponent* actual =
        actualEntity ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *actualEntity) : nullptr;
    const std::optional<ObjectContainmentKind> selectedKind = exitNetworkKind(selected);
    const std::optional<ObjectContainmentKind> actualKind = exitNetworkKind(actual);
    if (!selectedKind || selectedKind != actualKind)
        return false;
    if (*selectedKind == ObjectContainmentKind::Cave)
    {
        return selected->hasCave && actual->hasCave && selected->caveIndex == actual->caveIndex;
    }
    const std::optional<PlayerId> selectedOwner = ownership.ownerOf(container);
    return selectedOwner && ownership.ownerOf(edge->container) == selectedOwner;
}

} // namespace engine::session_query
