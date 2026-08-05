#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"

#include "core/container/string_utils.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>

namespace engine::detail {

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportPayloadDropTransaction event) {
    return handleTransportPayloadPlacement(std::move(event), false);
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportVisiblePayloadDropTransaction event) {
    return handleTransportPayloadPlacement(std::move(event), true);
}

bool GameSessionWeaponEventDrain::handleTransportPayloadPlacement(
    ObjectTransportPayloadPlacementTransaction event,
    bool visiblePayload) {
    const std::optional<ecs::entity> source =
        m_world.m_objects
            .entityFromIdIncludingPending(event.object);
    const std::optional<ecs::entity> target =
        m_world.m_objects
            .entityFromIdIncludingPending(event.target);
    const auto applyDroppedPayloadRoute = [&]() {
        if (visiblePayload || !event.target || !target) return;
        const LogicFixedVec3 route{event.routeX, event.routeY, event.routeZ};
        if (!event.directLanding &&
            ecs::try_get<ObjectLocomotionComponent>(
                m_world.m_registry, *target)) {
            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(
                    m_world.m_registry,
                    *target);
            if (!queue) {
                queue = &ecs::emplace<ObjectOrderQueueComponent>(
                    m_world.m_registry,
                    *target);
            }
            queue->orders.clear();
            queue->orders.push_back({
                .kind = ObjectOrderKind::Move,
                .source = ObjectOrderSource::System,
                .issuedTick = event.confirmedTick,
                .sourceSequence = event.authoredOrder + event.attempt,
                .targetX = route.x,
                .targetY = route.y,
                .targetZ = route.z,
                .hasTargetPosition = true,
                .systemPurpose = ObjectOrderSystemPurpose::DeliverPayload,
                .systemPurposeInstance = event.authoredOrder,
            });
            ++queue->revision;
        }
        static_cast<void>(m_world.m_objectSimulation.setMinefieldTarget(
            m_world.m_registry, m_world.m_objects, event.target, &route));
        static_cast<void>(m_world.m_objectSimulation.setSmartBombTarget(
            m_world.m_registry, m_world.m_objects, event.target, route));
    };

    if (!visiblePayload) {
        if (!source || !target ||
            !m_world.m_objectSimulation
                 .requestContainment(
                     m_world.m_registry,
                     m_world.m_objects,
                     {.kind = ObjectContainmentRequestKind::Detach,
                      .container = event.object,
                      .object = event.target,
                      .confirmedTick = event.confirmedTick,
                      .force = true},
                     &m_content.m_players,
                     &m_content
                          .m_contentSnapshot)) {
            return true;
        }
        closeCurrentReaction();
        if (m_frame.result().faulted()) return false;
        if (!m_world.m_objectSimulation
                 .acknowledgeTransportPayloadDrop(
                     m_world.m_registry,
                     m_world.m_objects,
                     event.object, event.target, event.ruleIndex,
                     event.attempt)) {
            return false;
        }
    }

    if (visiblePayload) {
        if (!source) return true;
        const TransformComponent* transport = ecs::try_get<TransformComponent>(
            m_world.m_registry, *source);
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
            m_world.m_registry, *source);
        if (!transport || !owner) return true;
        const std::optional<ObjectTeamId> payloadTeam =
            m_world.m_objectTeams.defaultTeam(
                owner->player);
        if (!payloadTeam) return true;
        const auto numberedName = [ordinal = event.attempt](
                                      container::StringView base) {
            container::String value{base};
            if (!value.empty()) {
                if (ordinal < 10u) value.push_back('0');
                value += std::to_string(ordinal);
            }
            return value;
        };
        if (!event.subObject.empty()) {
            ObjectSubObjectVisibilityOverrideComponent* overrides =
                ecs::try_get<ObjectSubObjectVisibilityOverrideComponent>(
                    m_world.m_registry,
                    *source);
            if (!overrides) {
                overrides = &ecs::emplace<
                    ObjectSubObjectVisibilityOverrideComponent>(
                        m_world.m_registry,
                        *source);
            }
            const container::String name = numberedName(event.subObject);
            auto found = std::find_if(
                overrides->entries.begin(), overrides->entries.end(),
                [&](const ObjectSubObjectVisibilityOverride& entry) {
                    return container::asciiEqualIgnoreCase(entry.name, name);
                });
            if (found == overrides->entries.end()) {
                overrides->entries.push_back({
                    .name = name,
                    .visible = false,
                    .active = true,
                });
            } else {
                found->visible = false;
                found->active = true;
            }
            ++overrides->revision;
            markObjectDirty(
                m_world.m_registry, *source,
                ObjectDirtyDomain::RenderExtraction);
        }

        const LogicFixedVec3 origin = readAuthoritativeObjectPosition(
            m_world.m_registry, *source,
            *transport);
        LogicFixedVec3 impact = origin;
        math::q32_32 yaw = readAuthoritativeObjectYaw(
            m_world.m_registry, *source,
            *transport);
        math::q32_32 pitch{};
        math::q32_32 roll{};
        const ObjectPhysicsComponent* transportPhysics =
            ecs::try_get<ObjectPhysicsComponent>(
                m_world.m_registry, *source);
        if (transportPhysics && transportPhysics->ownsAttitude) {
            yaw = transportPhysics->yaw;
            pitch = transportPhysics->pitch;
            roll = transportPhysics->roll;
        }
        if (!event.attachmentBone.empty()) {
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry,
                    *source);
            const RenderModelComponent* visual =
                ecs::try_get<RenderModelComponent>(
                    m_world.m_registry,
                    *source);
            const game::W3dPristineBoneCatalog* catalog =
                m_content.m_contentSnapshot
                    .pristineBoneCatalog();
            if (type && type->archetype && visual && catalog &&
                catalog->isLoaded()) {
                const size_t visualRuleIndex =
                    game::selectModelConditionVisualRuleIndex(
                        type->archetype->templateData,
                        visual->modelConditionFlags);
                const auto bone = catalog->find(
                    type->archetype->name, visualRuleIndex,
                    numberedName(event.attachmentBone));
                if (bone) {
                    LogicFixedVec3 local{
                        bone->translation.x,
                        bone->translation.y,
                        bone->translation.z,
                    };
                    const math::q32_32_sincos x = math::fixed_sincos(-roll);
                    local = {
                        .x = local.x,
                        .y = local.y * x.cosine - local.z * x.sine,
                        .z = local.y * x.sine + local.z * x.cosine,
                    };
                    const math::q32_32_sincos y = math::fixed_sincos(pitch);
                    local = {
                        .x = local.x * y.cosine + local.z * y.sine,
                        .y = local.y,
                        .z = -local.x * y.sine + local.z * y.cosine,
                    };
                    const math::q32_32_sincos z = math::fixed_sincos(yaw);
                    local = {
                        .x = local.x * z.cosine - local.y * z.sine,
                        .y = local.x * z.sine + local.y * z.cosine,
                        .z = local.z,
                    };
                    impact = {
                        .x = origin.x + local.x,
                        .y = origin.y + local.y,
                        .z = origin.z + local.z,
                    };
                }
            }
        }
        impact.x += event.x;
        impact.y += event.y;
        impact.z += event.z;
        const container::SharedPtr<const game::ObjectArchetype>
            visibleArchetype = event.payload.empty()
                ? nullptr
                : m_content.m_contentSnapshot
                      .findObjectArchetype(event.payload);
        if (!visibleArchetype) return true;
        GameSessionObjectSpawnResult visible = m_lifecycle.spawnObject({
            .templateName = event.payload,
            .owner = owner->player,
            .primaryTeam = *payloadTeam,
            .transform = ObjectFixedTransformComponent{
                .position = impact,
                .yawRadians = yaw,
                .authoritative = true,
            },
            .origin = ObjectCreationOrigin::System,
            .confirmedTick = event.confirmedTick,
            .producer = event.object,
        });
        if (visible) {
            const ObjectProjectileBehaviorKind projectileKind =
                visibleArchetype->projectilePlan
                ? visibleArchetype->projectilePlan->behaviorKind
                : ObjectProjectileBehaviorKind::Unsupported;
            const bool projectile =
                projectileKind == ObjectProjectileBehaviorKind::DumbBezier ||
                projectileKind == ObjectProjectileBehaviorKind::MissileAI ||
                projectileKind ==
                    ObjectProjectileBehaviorKind::NeutronMissile;
            ObjectPhysicsComponent* payloadPhysics =
                ecs::try_get<ObjectPhysicsComponent>(
                    m_world.m_registry,
                    *visible.entity);
            if (!projectile && payloadPhysics && transportPhysics &&
                event.inheritTransportVelocity) {
                payloadPhysics->velocityUnitsPerSecond.x +=
                    transportPhysics->velocityUnitsPerSecond.x;
                payloadPhysics->velocityUnitsPerSecond.y +=
                    transportPhysics->velocityUnitsPerSecond.y;
                payloadPhysics->velocityUnitsPerSecond.z +=
                    transportPhysics->velocityUnitsPerSecond.z;
                const math::q32_32 fps{static_cast<int32_t>(
                    std::max<uint32_t>(
                        1u, m_world
                                .m_objectSimulation.rules()
                                .logicFramesPerSecond))};
                const LogicFixedVec3 backstep{
                    .x = transportPhysics->velocityUnitsPerSecond.x / fps,
                    .y = transportPhysics->velocityUnitsPerSecond.y / fps,
                    .z = transportPhysics->velocityUnitsPerSecond.z / fps,
                };
                payloadPhysics->position.x -= backstep.x;
                payloadPhysics->position.y -= backstep.y;
                payloadPhysics->position.z -= backstep.z;
                payloadPhysics->lastPublishedPosition =
                    payloadPhysics->position;
                writeAuthoritativeObjectPosition(
                    m_world.m_registry,
                    *visible.entity, payloadPhysics->position);
            }

            if (projectile) {
                const game::WeaponContentId weapon =
                    m_content.m_contentSnapshot
                        .findWeaponId(event.auxiliaryPayload);
                const game::WeaponTemplate* weaponDefinition =
                    m_content.m_contentSnapshot
                        .findWeapon(weapon);
                const ObjectTerrainLayerComponent* layer =
                    ecs::try_get<ObjectTerrainLayerComponent>(
                        m_world.m_registry,
                        *source);
                const ObjectWeaponBonusComponent* bonus =
                    ecs::try_get<ObjectWeaponBonusComponent>(
                        m_world.m_registry,
                        *source);
                const ObjectWeaponComponent* launcherWeapons =
                    ecs::try_get<ObjectWeaponComponent>(
                        m_world.m_registry,
                        *source);
                const ObjectVeterancyComponent* veterancy =
                    ecs::try_get<ObjectVeterancyComponent>(
                        m_world.m_registry,
                        *source);
                const size_t veterancyIndex = static_cast<size_t>(
                    veterancy ? veterancy->level
                              : game::ObjectVeterancyLevel::Regular);
                LogicFixedVec3 launchPosition = impact;
                if (event.inheritTransportVelocity && transportPhysics) {
                    const math::q32_32 fps{static_cast<int32_t>(
                        std::max<uint32_t>(
                            1u, m_world
                                    .m_objectSimulation.rules()
                                    .logicFramesPerSecond))};
                    launchPosition.x -=
                        transportPhysics->velocityUnitsPerSecond.x / fps;
                    launchPosition.y -=
                        transportPhysics->velocityUnitsPerSecond.y / fps;
                    launchPosition.z -=
                        transportPhysics->velocityUnitsPerSecond.z / fps;
                }
                ObjectProjectileSpawnRequest request{
                    .launcher = event.object,
                    .sourcePathfindLayer = layer
                        ? layer->pathfindLayer
                        : game::terrain::kGroundPathfindLayer,
                    .detonationWeapon = weapon,
                    .launcherWeaponBonusConditions = bonus
                        ? bonus->conditions
                        : game::WeaponBonusConditionMask{},
                    .projectileTemplate = event.payload,
                    .launchPosition = launchPosition,
                    .projectileStreamOwnerAnchorPosition = origin,
                    .targetPosition = {
                        event.targetX, event.targetY, event.targetZ},
                    .intendedTargetBasePosition = {
                        event.targetX, event.targetY, event.targetZ},
                    .launcherVelocityUnitsPerSecond =
                        event.inheritTransportVelocity && transportPhysics
                        ? transportPhysics->velocityUnitsPerSecond
                        : LogicFixedVec3{},
                    .projectileExhaust = weaponDefinition &&
                            veterancyIndex <
                                weaponDefinition->projectileExhausts.size()
                        ? weaponDefinition->projectileExhausts[
                              veterancyIndex]
                        : container::String{},
                    .projectileStreamOwnerGeneration = launcherWeapons
                        ? launcherWeapons->weaponSetGeneration : 0u,
                    .sourceShotSequence =
                        event.authoredOrder + event.attempt,
                    .hasIntendedTargetBasePosition = true,
                    .confirmedTick = event.confirmedTick,
                };
                if (!weapon || !weaponDefinition ||
                    !m_world
                         .m_objectProjectiles.initializeObject(
                             m_world.m_registry,
                             *visible.entity, visible.object,
                             *visibleArchetype,
                             m_content
                                 .m_contentSnapshot,
                             request,
                             m_content.m_terrain,
                             std::max<uint32_t>(
                                 1u, m_world
                                         .m_objectSimulation.rules()
                                         .logicFramesPerSecond),
                             m_world
                                 .m_objectSimulation.rules()
                                 .gravityUnitsPerSecondSq)) {
                    static_cast<void>(m_lifecycle.requestDestroyObject(
                        visible.object, ObjectDestroyReason::System,
                        event.confirmedTick));
                }
            } else {
                if (payloadPhysics && event.pitchRate != math::q32_32{}) {
                    payloadPhysics->pitchRate =
                        payloadPhysics->centerOfMassOffset * event.pitchRate;
                    payloadPhysics->ownsAttitude = true;
                }
                if (ecs::try_get<ObjectLocomotionComponent>(
                        m_world.m_registry,
                        *visible.entity)) {
                    ObjectOrderQueueComponent* queue =
                        ecs::try_get<ObjectOrderQueueComponent>(
                            m_world.m_registry,
                            *visible.entity);
                    if (!queue) {
                        queue = &ecs::emplace<ObjectOrderQueueComponent>(
                            m_world.m_registry,
                            *visible.entity);
                    }
                    queue->orders.clear();
                    queue->orders.push_back({
                        .kind = ObjectOrderKind::Move,
                        .source = ObjectOrderSource::System,
                        .issuedTick = event.confirmedTick,
                        .sourceSequence =
                            event.authoredOrder + event.attempt,
                        .targetX = event.routeX,
                        .targetY = event.routeY,
                        .targetZ = event.routeZ,
                        .hasTargetPosition = true,
                        .systemPurpose =
                            ObjectOrderSystemPurpose::DeliverPayload,
                        .systemPurposeInstance = event.authoredOrder,
                    });
                    ++queue->revision;
                }
            }
        }
        closeCurrentReaction();
        return !m_frame.result().faulted();
    }

    const bool wrapDroppedPayload = !event.payload.empty();
    if (wrapDroppedPayload) {
        const ObjectId passenger = event.target;
        const std::optional<ecs::entity> passengerEntity =
            m_world.m_objects.entityFromId(
                passenger);
        if (!passengerEntity) return true;
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
            m_world.m_registry,
            *passengerEntity);
        const PrimaryTeamComponent* team =
            ecs::try_get<PrimaryTeamComponent>(
                m_world.m_registry,
                *passengerEntity);
        ObjectFixedTransformComponent* transform =
            ecs::try_get<ObjectFixedTransformComponent>(
                m_world.m_registry,
                *passengerEntity);
        if (!owner || !team || !transform ||
            !m_content.m_contentSnapshot
                 .findObjectArchetype(event.payload)) {
            return true;
        }
        if (source) {
            const ObjectFixedTransformComponent* transportTransform =
                ecs::try_get<ObjectFixedTransformComponent>(
                    m_world.m_registry,
                    *source);
            if (transportTransform && transportTransform->authoritative) {
                const LogicFixedVec3 payloadPosition{
                    transportTransform->position.x + event.x,
                    transportTransform->position.y + event.y,
                    transportTransform->position.z + event.z,
                };
                writeAuthoritativeObjectPosition(
                    m_world.m_registry,
                    *passengerEntity, payloadPosition);
                ObjectPhysicsComponent* physics =
                    ecs::try_get<ObjectPhysicsComponent>(
                        m_world.m_registry,
                        *passengerEntity);
                if (physics) {
                    physics->position = payloadPosition;
                    physics->lastPublishedPosition = physics->position;
                    physics->hasAuthoritativePosition = true;
                    if (event.inheritTransportVelocity) {
                        const ObjectPhysicsComponent* transportPhysics =
                            ecs::try_get<ObjectPhysicsComponent>(
                                m_world
                                    .m_registry,
                                *source);
                        if (transportPhysics) {
                            physics->velocityUnitsPerSecond.x +=
                                transportPhysics->velocityUnitsPerSecond.x;
                            physics->velocityUnitsPerSecond.y +=
                                transportPhysics->velocityUnitsPerSecond.y;
                            physics->velocityUnitsPerSecond.z +=
                                transportPhysics->velocityUnitsPerSecond.z;
                        }
                    }
                }
            }
        }
        ObjectSpawnRequest request;
        request.templateName = event.payload;
        request.owner = owner->player;
        request.primaryTeam = team->team;
        request.transform = *transform;
        request.origin = ObjectCreationOrigin::System;
        request.confirmedTick = event.confirmedTick;
        request.producer = event.object;
        const GameSessionObjectSpawnResult parachute =
            m_lifecycle.spawnObject(std::move(request));
        if (parachute) {
            static_cast<void>(m_world
                .m_objectSimulation.requestContainment(
                    m_world.m_registry,
                    m_world.m_objects,
                    {.kind = ObjectContainmentRequestKind::Attach,
                     .container = parachute.object,
                     .object = passenger,
                     .confirmedTick = event.confirmedTick,
                     .force = true},
                    &m_content.m_players,
                    &m_content
                         .m_contentSnapshot));
            if (event.directLanding) {
                static_cast<void>(m_world
                    .m_objectSimulation.setParachuteLandingOverride(
                        m_world.m_registry,
                        m_world.m_objects,
                        parachute.object,
                        {event.targetX, event.targetY, event.targetZ}));
            }
        }
        applyDroppedPayloadRoute();
        closeCurrentReaction();
        return !m_frame.result().faulted();
    }

    if (source && target) {
        const ObjectFixedTransformComponent* payload =
            ecs::try_get<ObjectFixedTransformComponent>(
                m_world.m_registry, *target);
        const ObjectFixedTransformComponent* transport =
            ecs::try_get<ObjectFixedTransformComponent>(
                m_world.m_registry, *source);
        if (payload && transport) {
            const LogicFixedVec3 payloadPosition{
                transport->position.x + event.x,
                transport->position.y + event.y,
                transport->position.z + event.z,
            };
            writeAuthoritativeObjectPosition(
                m_world.m_registry, *target,
                payloadPosition);
            ObjectPhysicsComponent* payloadPhysics =
                ecs::try_get<ObjectPhysicsComponent>(
                    m_world.m_registry,
                    *target);
            if (payloadPhysics && event.inheritTransportVelocity) {
                const ObjectPhysicsComponent* transportPhysics =
                    ecs::try_get<ObjectPhysicsComponent>(
                        m_world.m_registry,
                        *source);
                if (transportPhysics) {
                    payloadPhysics->velocityUnitsPerSecond.x +=
                        transportPhysics->velocityUnitsPerSecond.x;
                    payloadPhysics->velocityUnitsPerSecond.y +=
                        transportPhysics->velocityUnitsPerSecond.y;
                    payloadPhysics->velocityUnitsPerSecond.z +=
                        transportPhysics->velocityUnitsPerSecond.z;
                }
            }
        }
        if (event.directLanding) {
            static_cast<void>(m_world
                .m_objectSimulation.setParachuteLandingOverride(
                    m_world.m_registry,
                    m_world.m_objects,
                    event.target,
                    {event.targetX, event.targetY, event.targetZ}));
        }
        applyDroppedPayloadRoute();
        closeCurrentReaction();
    }
    return !m_frame.result().faulted();
}

} // namespace engine::detail
