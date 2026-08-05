#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include <algorithm>

namespace engine {

bool ObjectSimulation::hasSpecialPowerCompletionDie(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object) const noexcept {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    const ObjectDeathReactionComponent* reaction =
        ecs::try_get<ObjectDeathReactionComponent>(registry, *entity);
    const ObjectSpecialPowerCompletionRuntimeComponent* runtime =
        ecs::try_get<ObjectSpecialPowerCompletionRuntimeComponent>(registry,
                                                                    *entity);
    if (!reaction || !reaction->plan || !runtime) return false;
    const size_t count = std::min(reaction->plan->rules.size(),
                                  runtime->rules.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectDeathReactionRule& rule =
            reaction->plan->rules[index];
        if (rule.kind == game::ObjectDeathReactionKind::SpecialPowerCompletion &&
            rule.specialPowerCompletionDie) {
            return true;
        }
    }
    return false;
}

bool ObjectSimulationProgressionDomain::canObjectReceiveUpgrade(
    const ecs::registry& registry, ecs::entity entity,
    const UpgradeMask& ownerCompletedUpgrades,
    UpgradeContentId prospectiveUpgrade) const noexcept {
    return object_simulation_detail::state(*this).m_upgrades.canReceiveObjectUpgrade(
        registry, entity, ownerCompletedUpgrades, prospectiveUpgrade);
}

bool ObjectSimulationProgressionDomain::hasObjectUpgrade(
    const ecs::registry& registry, ecs::entity entity,
    UpgradeContentId upgrade) const noexcept {
    return object_simulation_detail::state(*this).m_upgrades.hasObjectUpgrade(registry, entity, upgrade);
}

container::Vector<ObjectId> ObjectSimulationContainmentDomain::containmentCaptureDependents(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId container) const {
    return object_simulation_detail::state(*this).m_containment.captureDependents(registry, lifecycle, container);
}

ObjectId ObjectSimulationContainmentDomain::recentTunnelNetworkNemesis(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId tunnelEntrance, uint64_t confirmedTick) const noexcept {
    return object_simulation_detail::state(*this).m_containment.recentTunnelNetworkNemesis(
        registry, lifecycle, tunnelEntrance, confirmedTick);
}

bool ObjectSimulationContainmentDomain::publishTunnelNetworkNemesis(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId source, ObjectId target, uint64_t confirmedTick) {
    const auto& simulationState =
        object_simulation_detail::state(*this);
    return simulationState.m_containment.publishTunnelNetworkNemesis(
        registry, lifecycle, source, target, confirmedTick,
        simulationState.m_rules.logicFramesPerSecond);
}

bool ObjectSimulationContainmentDomain::canContain(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectContainmentRequest& request,
    const PlayerRegistry* players) const {
    return request.kind == ObjectContainmentRequestKind::Attach &&
        object_simulation_detail::state(*this).m_containment.prepareAttach(registry, lifecycle, request,
                                    players).has_value();
}

std::optional<uint64_t> ObjectSimulation::lifetimeDueTick(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, game::ObjectLifetimeAction action,
    std::optional<uint32_t> authoredOrder) const {
    return object_simulation_detail::state(*this).m_lifetime.nextDueTick(registry, lifecycle, object, action, authoredOrder);
}

} // namespace engine
