#pragma once

#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"

namespace engine {

// Direct Attack orders can originate from players/scripts or from a small,
// typed set of gameplay producers.  System is never accepted as a wildcard:
// each producer must declare the purpose whose lifecycle owns completion.
[[nodiscard]] inline bool isCombatDirectAttackOrder(
    const ObjectOrderIntent* order) noexcept {
    if (!order || order->kind != ObjectOrderKind::Attack)
        return false;
    if (order->source != ObjectOrderSource::System) {
        return order->systemPurpose == ObjectOrderSystemPurpose::Generic ||
            (order->source == ObjectOrderSource::Script &&
             order->systemPurpose ==
                 ObjectOrderSystemPurpose::CommandButtonFireWeapon);
    }
    switch (order->systemPurpose) {
    case ObjectOrderSystemPurpose::CleanupHazard:
    case ObjectOrderSystemPurpose::AssaultTransport:
    case ObjectOrderSystemPurpose::DeliverPayload:
    case ObjectOrderSystemPurpose::SlaveReturn:
    case ObjectOrderSystemPurpose::TacticalAssist:
    case ObjectOrderSystemPurpose::SpecialAbility:
    case ObjectOrderSystemPurpose::ObjectCreationAttack:
    case ObjectOrderSystemPurpose::StrategicAI:
        return true;
    default:
        return false;
    }
}

// Typed ownership rule shared by the AI admission and Combat consumers.
// Shape-specific validation (target/area/team handles) remains with the
// admitting state, while this predicate answers whether a TacticalAttack
// order is allowed to create an AI-owned combat child at all.
[[nodiscard]] inline bool isCombatTacticalAttackOrder(
    const ObjectOrderIntent* order) noexcept {
    if (!order || order->kind != ObjectOrderKind::TacticalAttack)
        return false;

    switch (order->tacticalAttackSubtype) {
    case ObjectTacticalAttackSubtype::Guard:
        return order->systemPurpose == ObjectOrderSystemPurpose::Generic &&
            (order->source == ObjectOrderSource::Script ||
             order->source == ObjectOrderSource::Player);
    case ObjectTacticalAttackSubtype::Hunt:
        return (order->source == ObjectOrderSource::Script &&
                order->systemPurpose == ObjectOrderSystemPurpose::Generic) ||
            (order->source == ObjectOrderSource::System &&
             (order->systemPurpose ==
                  ObjectOrderSystemPurpose::CommandButtonHunt ||
              order->systemPurpose ==
                  ObjectOrderSystemPurpose::ParachuteLanding));
    case ObjectTacticalAttackSubtype::AttackSquad:
    case ObjectTacticalAttackSubtype::AttackArea:
    case ObjectTacticalAttackSubtype::GuardTunnelNetwork:
        return order->source == ObjectOrderSource::Script &&
            order->systemPurpose == ObjectOrderSystemPurpose::Generic;
    case ObjectTacticalAttackSubtype::GuardRetaliate:
        return order->source == ObjectOrderSource::System &&
            order->systemPurpose == ObjectOrderSystemPurpose::Retaliation;
    case ObjectTacticalAttackSubtype::None:
        return false;
    }
    return false;
}

} // namespace engine
