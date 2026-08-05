#include "game/session/frame/GameSessionDebrisPresentationPublisher.h"

#include "game/session/state/GameSessionDomainState.h"
#include "game/object/component/ObjectDirty.h"

#include <algorithm>
#include <optional>

namespace engine {

void GameSessionDebrisPresentationPublisher::publish() {
    container::Vector<ObjectPhysicsEvent> physicsEvents =
        m_world.m_objectSimulation.takePhysicsEvents();
    for (const ObjectPhysicsEvent& event : physicsEvents) {
        if (event.kind != ObjectPhysicsEventKind::Bounced) continue;
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(event.object);
        const DebrisDrawPresentationComponent* debris = entity
            ? ecs::try_get<DebrisDrawPresentationComponent>(
                  m_world.m_registry, *entity)
            : nullptr;
        if (!debris || debris->bounceSound.empty()) continue;
        static_cast<void>(m_publication.emitAudioEvent({
            .eventName = debris->bounceSound,
            .emitter = event.object,
            .owner = event.object,
            .position = math::vec3{
                event.position.x.to_float(),
                event.position.y.to_float(),
                event.position.z.to_float()},
        }));
    }

    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<
        const ObjectIdentityComponent, const TransformComponent,
        DebrisDrawPresentationComponent>(m_world.m_registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectId object = view.template get<
            const ObjectIdentityComponent>(entity).id;
        if (object) candidates.push_back({object, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        DebrisDrawPresentationComponent& debris =
            ecs::get<DebrisDrawPresentationComponent>(
                m_world.m_registry, candidate.entity);
        if (debris.phase == DebrisDrawPresentationPhase::Final ||
            m_presentation.m_confirmedTick <= debris.spawnedTick + 3u) {
            continue;
        }
        const TransformComponent& transform = ecs::get<
            const TransformComponent>(m_world.m_registry, candidate.entity);
        const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(
                m_world.m_registry, candidate.entity);
        const float ground =
            m_content.m_terrain.groundHeight(transform.x, transform.y);
        const bool aboveTerrain = physics
            ? physics->wasAirborneLastFrame
            : transform.z > ground + 0.01f;
        if (aboveTerrain) {
            if (debris.phase != DebrisDrawPresentationPhase::Flying) {
                debris.phase = DebrisDrawPresentationPhase::Flying;
                markObjectDirty(m_world.m_registry, candidate.entity,
                                ObjectDirtyDomain::RenderExtraction);
            }
            continue;
        }

        debris.phase = DebrisDrawPresentationPhase::Final;
        debris.finalStateTick = m_presentation.m_confirmedTick;
        markObjectDirty(m_world.m_registry, candidate.entity,
                        ObjectDirtyDomain::RenderExtraction);
        if (!debris.finalFxEmitted && !debris.finalFx.empty() &&
            !debris.finalAnimation.empty()) {
            debris.finalFxEmitted = true;
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .fxListName = debris.finalFx,
                .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                .primary = {
                    .object = candidate.object,
                    .position = {transform.x, transform.y, transform.z},
                    .yawRadians = transform.rotation,
                },
                .localVisibilityRetryFrames = static_cast<uint32_t>(
                    std::max(1, m_content.m_startInfo.gameSpeedFPS) * 5),
            }));
        }
    }
}

} // namespace engine
