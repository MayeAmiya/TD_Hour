#pragma once

#include <algorithm>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"

namespace engine
{

enum class ObjectMoveOrderConsumer : uint8_t
{
    None,
    LegacyMovement,
    ObjectAIRuntime,
    SpecializedSystem,
};

// Pure ownership resolver shared by the production movement gate and focused
// probes. aiMoveStopOwners/aiAttackOwners must be ObjectId-sorted. The typed
// AI admission table is the single authority: a specialized producer may own
// protocol completion while still delegating pathfinding/motion to ObjectAI.
[[nodiscard]] inline ObjectMoveOrderConsumer resolveObjectMoveOrderConsumer(
    ObjectId subject,
    const ObjectOrderIntent* order,
    container::Span<const ObjectId> aiMoveStopOwners,
    container::Span<const ObjectId> aiAttackOwners = {}) noexcept
{
    if (!order || order->kind != ObjectOrderKind::Move)
        return ObjectMoveOrderConsumer::None;
    const bool ownsMove = subject && std::binary_search(
        aiMoveStopOwners.begin(), aiMoveStopOwners.end(), subject);
    const bool ownsAttack = subject && std::binary_search(
        aiAttackOwners.begin(), aiAttackOwners.end(), subject);
    if (order->systemPurpose ==
            ObjectOrderSystemPurpose::IntentionalContact &&
        order->source != ObjectOrderSource::System) {
        return ObjectMoveOrderConsumer::LegacyMovement;
    }
    ai::ObjectAIOrderCapability capabilities =
        ai::ObjectAIOrderCapability::None;
    if (ownsMove)
        capabilities |= ai::ObjectAIOrderCapability::MoveStop;
    if (ownsAttack)
        capabilities |= ai::ObjectAIOrderCapability::Attack;
    const ai::ObjectAIOrderOwner owner = ai::objectAIOrderOwner(
        static_cast<ai::ObjectAIOrderKind>(order->kind),
        static_cast<ai::ObjectAIOrderSource>(order->source),
        static_cast<ai::ObjectAIOrderSystemPurpose>(order->systemPurpose),
        capabilities, order->attackMove,
        static_cast<ai::ObjectAIMoveRouteSubtype>(order->moveRouteSubtype),
        static_cast<ai::ObjectAITacticalAttackSubtype>(
            order->tacticalAttackSubtype));
    switch (owner) {
    case ai::ObjectAIOrderOwner::ObjectAIRuntime:
        return ObjectMoveOrderConsumer::ObjectAIRuntime;
    case ai::ObjectAIOrderOwner::LegacyMovement:
        return ObjectMoveOrderConsumer::LegacyMovement;
    case ai::ObjectAIOrderOwner::LegacySpecialized:
    case ai::ObjectAIOrderOwner::LegacyBuilderProduction:
        return ObjectMoveOrderConsumer::SpecializedSystem;
    case ai::ObjectAIOrderOwner::None:
    case ai::ObjectAIOrderOwner::CommandIngress:
    case ai::ObjectAIOrderOwner::LegacyCombat:
    case ai::ObjectAIOrderOwner::Unsupported:
        return ObjectMoveOrderConsumer::None;
    }
    return ObjectMoveOrderConsumer::None;
}

} // namespace engine
