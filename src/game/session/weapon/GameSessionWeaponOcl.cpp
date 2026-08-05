#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"

#include "debug/debug.h"

#include <utility>
#include <variant>

namespace engine::detail {

bool GameSessionWeaponEventDrain::processOcl(WorkItem item) {
    const game::ObjectCreationListDefinition* definition =
        m_content.m_contentSnapshot
            .findObjectCreationList(item.ocl.content);
    if (!definition) {
        finalizeOcl(item.ocl, item.oclState);
        return true;
    }
    if (item.oclNuggetIndex >= definition->nuggets.size()) {
        finalizeOcl(item.ocl, item.oclState);
        return true;
    }
    if (++m_processedOclNuggets > kMaximumOclNuggets) {
        TD_LOG_ERROR(
            "[GameSession] ObjectCreationList chain exceeded {} nuggets at tick {}; dropping the malformed tail",
            kMaximumOclNuggets,
            m_presentation.m_confirmedTick);
        discardPendingWork();
        return false;
    }

    // Continuation stays below work emitted by the current nugget, preserving
    // ZH's depth-first nested weapon/damage/death/OCL transaction order.
    if (item.oclNuggetIndex + 1 < definition->nuggets.size()) {
        WorkItem continuation;
        continuation.kind = WorkKind::ObjectCreationList;
        continuation.ocl = item.ocl;
        continuation.oclNuggetIndex = item.oclNuggetIndex + 1;
        continuation.oclState = item.oclState;
        pushWork(std::move(continuation));
    }

    const game::ObjectCreationNugget& sourceNugget =
        definition->nuggets[item.oclNuggetIndex];
    std::visit([&](const auto& nugget) {
        using Nugget = std::decay_t<decltype(nugget)>;
        if constexpr (std::is_same_v<Nugget,
                          game::ObjectCreationApplyRandomForceNugget>) {
            processOclApplyRandomForce(item, nugget);
        } else if constexpr (std::is_same_v<Nugget,
                                 game::ObjectCreationFireWeaponNugget>) {
            processOclFireWeapon(item, *definition, nugget);
        } else if constexpr (std::is_same_v<Nugget,
                                 game::ObjectCreationAttackNugget>) {
            processOclAttack(item, *definition, nugget);
        } else if constexpr (std::is_same_v<Nugget,
                                 game::ObjectCreationDeliverPayloadNugget>) {
            processOclDelivery(item, *definition, nugget);
        } else {
            processOclCreation(item, sourceNugget);
        }
    }, sourceNugget);

    if (item.oclNuggetIndex + 1 >= definition->nuggets.size()) {
        finalizeOcl(item.ocl, item.oclState);
    }
    if (m_createdOclObjects > kMaximumOclCreatedObjects) {
        TD_LOG_ERROR(
            "[GameSession] ObjectCreationList chain exceeded {} created objects at tick {}; dropping the malformed tail",
            kMaximumOclCreatedObjects,
            m_presentation.m_confirmedTick);
        return false;
    }
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    pushPendingWork(std::move(nested));
    return true;
}

} // namespace engine::detail
