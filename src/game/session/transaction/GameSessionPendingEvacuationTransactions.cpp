#include "game/session/transaction/GameSessionPendingEvacuationTransactions.h"

#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>
#include <optional>

namespace engine {

container::Vector<GameSessionReadyPlayerEvacuation>
GameSessionPendingEvacuationTransactions::advance() {
    struct Pending final {
        ObjectId object = INVALID_OBJECT_ID;
        PlayerId player = INVALID_PLAYER_ID;
        uint64_t externalOrderRevision = 0;
        uint64_t issuedTick = 0;
        uint64_t deadlineTick = 0;
        uint32_t sourceSequence = 0;
        math::q32_32 landingZ{};
        bool previousUsePreciseZPosition = false;
    };
    container::Vector<Pending> pending;
    const auto view = ecs::view<
        const ObjectIdentityComponent,
        const ObjectPendingPlayerEvacuationComponent>(m_world.m_registry);
    pending.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectPendingPlayerEvacuationComponent& request =
            view.template get<
                const ObjectPendingPlayerEvacuationComponent>(entity);
        if (!identity.id) continue;
        pending.push_back({
            identity.id, request.player, request.externalOrderRevision,
            request.issuedTick, request.deadlineTick,
            request.sourceSequence, request.landingZ,
            request.previousUsePreciseZPosition,
        });
    }
    std::sort(pending.begin(), pending.end(),
        [](const Pending& left, const Pending& right) {
            return left.object < right.object;
        });

    container::Vector<GameSessionReadyPlayerEvacuation> ready;
    for (const Pending& request : pending) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(request.object);
        if (!entity) continue;
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(m_world.m_registry, *entity);
        ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
        const ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(
                m_world.m_registry, *entity);
        const ObjectOrderIntent* head = queue && !queue->orders.empty()
            ? &queue->orders.front() : nullptr;
        const bool ownsLandingOrder = head &&
            head->kind == ObjectOrderKind::Move &&
            head->source == ObjectOrderSource::Player &&
            head->issuedTick == request.issuedTick &&
            head->sourceSequence == request.sourceSequence;
        const auto finish = [&](bool removeLandingOrder) {
            if (locomotion) {
                locomotion->usePreciseZPosition =
                    request.previousUsePreciseZPosition;
            }
            if (removeLandingOrder && queue && ownsLandingOrder &&
                !queue->orders.empty()) {
                queue->orders.erase(queue->orders.begin());
                ++queue->revision;
            }
            ecs::remove<ObjectPendingPlayerEvacuationComponent>(
                m_world.m_registry, *entity);
        };
        if (!queue || queue->externalRevision !=
                request.externalOrderRevision ||
            m_world.m_ownership.ownerOf(request.object) != request.player) {
            finish(false);
            continue;
        }
        const bool atLandingHeight = fixedTransform &&
            fixedTransform->authoritative &&
            math::q32_32::abs(
                fixedTransform->position.z - request.landingZ) <=
                math::q32_32{int32_t{1}};
        const bool landedByFlightOwner = airborne && !airborne->isAirborne;
        if (landedByFlightOwner || atLandingHeight) {
            if (airborne) airborne->isAirborne = false;
            finish(true);
            ready.push_back({request.object, request.player});
            continue;
        }
        const bool timedOut = m_presentation.m_confirmedTick >=
            request.deadlineTick;
        const bool blocked = locomotion &&
            locomotion->state == ObjectLocomotionState::Blocked;
        if (!ownsLandingOrder || timedOut || blocked || !locomotion ||
            !fixedTransform) {
            finish(ownsLandingOrder);
        }
    }
    return ready;
}

} // namespace engine
