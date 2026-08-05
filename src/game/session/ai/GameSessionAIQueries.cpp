#include "game/session/ai/GameSessionAIDomain.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"

#include <algorithm>
#include <optional>

namespace engine {

script::ScriptSequentialAuthorityState GameSessionAIDomain::sequentialObjectState(
    ObjectId object) const noexcept {
    script::ScriptSequentialAuthorityState result;
    const std::optional<ecs::entity> entity =
        domainState().worldState().m_objects.entityFromId(object);
    if (!entity) return result;
    result.exists = true;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(domainState().worldState().m_registry, *entity);
    result.effectivelyDead = health && health->effectivelyDead;
    const std::optional<ai::ObjectAIActorStateView> aiState =
        domainState().aiState().m_objectAI.actorState(object);
    result.hasAI = aiState.has_value();
    if (result.hasAI) {
        const ai::ObjectAIRuntime& runtime =
            domainState().aiState().m_objectAI;
        result.canGuard = runtime.hasOrderCapability(
                              object, ai::ObjectAIOrderCapability::Attack) &&
            runtime.hasOrderCapability(
                object, ai::ObjectAIOrderCapability::MoveStop);
    }
    result.idle = aiState && aiState->idle;
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(domainState().worldState().m_registry, *entity);
    const PlayerState* player = owner ? domainState().contentState().m_players.get(owner->player) : nullptr;
    if (player && player->controller == PlayerControllerKind::Ai)
        result.currentPlayer = owner->player;
    return result;
}

} // namespace engine
