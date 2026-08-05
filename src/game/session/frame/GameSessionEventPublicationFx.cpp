#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/session/frame/GameSessionTransitionFxProjection.h"
#include "game/session/frame/GameSessionObjectEventPublisher.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/simulation/runtime/ObjectDeathEvents.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"

namespace engine
{

void GameSessionObjectEventPublisher::publishFx()
{
    struct PendingInvocation final
    {
        game::FxInvocationEvent event;
        uint64_t fxEmissionSequence = 0;
    };
    container::Vector<ObjectInstantDeathEffectEvent> instantEvents =
        m_world.m_objectSimulation.takeInstantDeathEffectEvents();
    container::Vector<ObjectFxListDieEffectEvent> fxListEvents = m_world.m_objectSimulation.takeFxListDieEffectEvents();
    container::Vector<ObjectSlowDeathPhaseEvent> slowDeathEvents =
        m_world.m_objectSimulation.takeSlowDeathPhaseEvents();
    container::Vector<ObjectTransitionDamageFxEvent> transitionEvents =
        m_world.m_objectSimulation.takeTransitionDamageFxEvents();
    container::Vector<ObjectStructureEffectEvent> structureEvents =
        m_world.m_objectSimulation.takeStructureEffectEvents();
    if (instantEvents.empty() && fxListEvents.empty() && slowDeathEvents.empty() && transitionEvents.empty() &&
        structureEvents.empty())
        return;

    container::Vector<PendingInvocation> pending;
    pending.reserve(transitionEvents.size() + instantEvents.size() + fxListEvents.size() + slowDeathEvents.size() +
                    structureEvents.size());

    const auto sourcePlayerFor = [this](ObjectId object)
        -> std::optional<PlayerId> {
        if (!object) return std::nullopt;
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromIdIncludingPending(object);
        const OwnerComponent* owner = entity
            ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
            : nullptr;
        return owner && owner->player
            ? std::optional<PlayerId>{owner->player} : std::nullopt;
    };

    std::stable_sort(transitionEvents.begin(),
                     transitionEvents.end(),
                     [](const ObjectTransitionDamageFxEvent& left, const ObjectTransitionDamageFxEvent& right)
                     { return left.emissionSequence < right.emissionSequence; });
    for (const ObjectTransitionDamageFxEvent& source : transitionEvents)
    {
        const game::FxInvocationAnchor primary =
            session_fx::snapshotAnchor(source.primary, source.object);
        switch (source.kind)
        {
        case ObjectTransitionDamageFxEventKind::StopParticleGroup:
            pending.push_back({
                .event =
                    {
                        .control = game::FxInvocationControlKind::StopAttachedParticleGroup,
                        .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                        .primary = primary,
                        .sourcePlayer = sourcePlayerFor(source.object),
                        .attachmentGroup = source.attachmentGroup,
                    },
                .fxEmissionSequence = source.emissionSequence,
            });
            break;
        case ObjectTransitionDamageFxEventKind::FxList:
        {
            if (source.resource.empty())
                break;
            game::FxInvocationEvent invocation{
                .fxListName = source.resource,
                .anchorKind = source.location.kind == game::ObjectTransitionDamageFxLocationKind::Bone
                                  ? game::FxInvocationAnchorKind::BonePosition
                                  : game::FxInvocationAnchorKind::WorldPosition,
                .primary = primary,
                .sourcePlayer = sourcePlayerFor(source.object),
                .boneName = source.location.boneName,
                .boneNameIsPrefix = source.location.randomBone,
                .inheritResolvedAnchorOrientation = false,
            };
            if (source.location.kind == game::ObjectTransitionDamageFxLocationKind::LocalCoordinate)
            {
                invocation.primary.object = INVALID_OBJECT_ID;
                invocation.primary.position =
                    session_fx::transitionWorldPosition(source);
            }
            pending.push_back({
                .event = std::move(invocation),
                .fxEmissionSequence = source.emissionSequence,
            });
            break;
        }
        case ObjectTransitionDamageFxEventKind::ParticleSystem:
        {
            if (source.resource.empty())
                break;
            const math::vec3 local{
                source.location.localPosition.x.to_float(),
                source.location.localPosition.y.to_float(),
                source.location.localPosition.z.to_float(),
            };
            pending.push_back({
                .event =
                    {
                        .directParticle =
                            game::FxDirectParticleRequest{
                                .particleSystemName = source.resource,
                                .emitterCount = 1,
                                .attachToObject = true,
                            },
                        .anchorKind = source.location.kind == game::ObjectTransitionDamageFxLocationKind::Bone
                                          ? game::FxInvocationAnchorKind::BonePosition
                                          : game::FxInvocationAnchorKind::ObjectAttachment,
                        .primary = primary,
                        .sourcePlayer = sourcePlayerFor(source.object),
                        .boneName = source.location.boneName,
                        .boneNameIsPrefix = source.location.randomBone,
                        .attachmentLocalOffset =
                            source.location.kind == game::ObjectTransitionDamageFxLocationKind::LocalCoordinate
                                ? local
                                : math::vec3{},
                        .attachmentGroup = source.attachmentGroup,
                    },
                .fxEmissionSequence = source.emissionSequence,
            });
            break;
        }
        case ObjectTransitionDamageFxEventKind::ObjectCreationList:
        {
            // Authoritative OCL payloads are extracted before this
            // presentation pass and consumed by the confirmed gameplay
            // work stack. Never let FX publication drive gameplay.
            break;
        }
        }
    }
    for (const ObjectStructureEffectEvent& source : structureEvents)
    {
        if (source.resource.empty())
            continue;
        if (source.kind == ObjectStructureEffectKind::ObjectCreationList)
        {
            // Structure OCL is likewise owned by the gameplay drain.
            continue;
        }
        const bool attached = source.anchor == ObjectStructureEffectAnchor::ObjectAttachment;
        pending.push_back({
            .event =
                {
                    .fxListName = source.resource,
                    .anchorKind = attached ? game::FxInvocationAnchorKind::ObjectAttachment
                                           : game::FxInvocationAnchorKind::WorldPosition,
                    .primary =
                        {
                            .object = attached ? source.object : INVALID_OBJECT_ID,
                            .position =
                                {
                                    source.position.x.to_float(),
                                    source.position.y.to_float(),
                                    source.position.z.to_float(),
                                },
                            .yawRadians = source.orientationRadians.to_float(),
                        },
                    .sourcePlayer = sourcePlayerFor(source.object),
                },
            .fxEmissionSequence = source.emissionSequence,
        });
    }
    for (const ObjectFxListDieEffectEvent& source : fxListEvents)
    {
        if (source.fx.empty())
            continue;
        game::FxInvocationEvent invocation{
            .fxListName = source.fx,
            .anchorKind = source.anchor == FxInvocationAnchorKind::ObjectAttachment
                              ? game::FxInvocationAnchorKind::ObjectAttachment
                              : game::FxInvocationAnchorKind::WorldPosition,
            .primary = session_fx::snapshotAnchor(
                source.primary, source.object),
            .sourcePlayer = sourcePlayerFor(source.object),
        };
        if (source.secondary)
        {
            invocation.secondary = session_fx::snapshotAnchor(
                *source.secondary, source.source);
        }
        pending.push_back({
            .event = std::move(invocation),
            .fxEmissionSequence = source.fxEmissionSequence,
        });
    }
    for (const ObjectInstantDeathEffectEvent& source : instantEvents)
    {
        if (!source.fx || source.fx->empty())
            continue;
        pending.push_back({
            .event =
                {
                    .fxListName = *source.fx,
                    .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                    .primary =
                        {
                            .object = source.object,
                            .position =
                                {
                                    source.position.x.to_float(),
                                    source.position.y.to_float(),
                                    source.position.z.to_float(),
                                },
                            .yawRadians = source.rotationRadians.to_float(),
                        },
                    .sourcePlayer = sourcePlayerFor(source.object),
                },
            .fxEmissionSequence = source.fxEmissionSequence,
        });
    }
    for (const ObjectSlowDeathPhaseEvent& source : slowDeathEvents)
    {
        if (!source.fx || source.fx->empty())
            continue;
        const std::optional<game::FxInvocationAnchor> primary =
            session_fx::snapshotAnchor(
                m_world.m_registry, m_world.m_objects, source.object);
        if (!primary)
            continue;
        pending.push_back({
            .event =
                {
                    .fxListName = *source.fx,
                    .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                    .primary = *primary,
                    .sourcePlayer = sourcePlayerFor(source.object),
                },
            .fxEmissionSequence = source.fxEmissionSequence,
        });
    }
    std::stable_sort(pending.begin(),
                     pending.end(),
                     [](const PendingInvocation& left, const PendingInvocation& right)
                     { return left.fxEmissionSequence < right.fxEmissionSequence; });
    // This pass is presentation-only. Gameplay payloads sharing the same
    // emission stream were already consumed by the confirmed work stack.
    for (PendingInvocation& invocation : pending)
    {
        static_cast<void>(m_publication.emitFxInvocationEvent(std::move(invocation.event)));
    }

}

} // namespace engine
