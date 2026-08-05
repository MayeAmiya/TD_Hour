#include "game/session/transaction/GameSessionObjectTargetRemapTransactions.h"

#include "game/session/state/GameSessionDomainState.h"

#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"

namespace engine {

GameSessionObjectTargetRemapTransactions::
GameSessionObjectTargetRemapTransactions(ecs::registry& registry) noexcept
    : m_registry(registry) {}

GameSessionObjectTargetRemapTransactions::
GameSessionObjectTargetRemapTransactions(GameSessionWorldState& world) noexcept
    : m_registry(world.m_registry) {}

void GameSessionObjectTargetRemapTransactions::remapAttackTargets(
    ObjectId from, ObjectId to) {
    if (!from || !to || from == to) return;

    const auto orderView = ecs::view<ObjectOrderQueueComponent>(m_registry);
    for (const ecs::entity entity : orderView) {
        ObjectOrderQueueComponent& queue =
            orderView.template get<ObjectOrderQueueComponent>(entity);
        bool changed = false;
        for (ObjectOrderIntent& order : queue.orders) {
            if (order.targetObject != from) continue;
            order.targetObject = to;
            changed = true;
        }
        if (changed) ++queue.revision;
    }

    const auto operationView =
        ecs::view<ObjectAICombatOperationComponent>(m_registry);
    for (const ecs::entity entity : operationView) {
        ObjectAICombatOperationComponent& operation =
            operationView.template get<ObjectAICombatOperationComponent>(
                entity);
        if (operation.attacksObject && operation.target == from)
            operation.target = to;
    }

    const auto weaponView = ecs::view<ObjectWeaponComponent>(m_registry);
    for (const ecs::entity entity : weaponView) {
        ObjectWeaponComponent& weapon =
            weaponView.template get<ObjectWeaponComponent>(entity);
        if (weapon.target == from) weapon.target = to;
    }
}

} // namespace engine
