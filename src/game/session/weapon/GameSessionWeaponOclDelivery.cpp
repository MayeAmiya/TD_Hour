#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"

#include "debug/debug.h"
#include "core/container/string_utils.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/creation/ObjectOclCreateRandomSample.h"
#include "game/object/creation/ObjectOclSpreadPlacement.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectFloat.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>

namespace engine::detail {

void GameSessionWeaponEventDrain::processOclDelivery(
    const WorkItem& item, const game::ObjectCreationListDefinition& definition,
    const game::ObjectCreationDeliverPayloadNugget& nugget) {
            if (!item.ocl.source || !item.ocl.hasSecondaryPosition ||
                !item.ocl.owner) return;
            const ObjectTeamId creationTeam =
                m_world.m_objectTeams.defaultTeam(item.ocl.owner).value_or(
                    item.ocl.primaryTeam);
            if (!creationTeam) return;

            const uint32_t formationSize = item.ocl.createDeliveryOwner
                ? std::max<uint32_t>(1u, nugget.formationSize) : 1u;
            const LogicFixedVec3 basePosition =
                item.ocl.primaryPosition;
            const LogicFixedVec3 targetPosition =
                item.ocl.secondaryPosition;
            const math::q32_32 directionX =
                targetPosition.x - basePosition.x;
            const math::q32_32 directionY =
                targetPosition.y - basePosition.y;
            const math::q32_32 directionLength = math::q32_32::sqrt(
                directionX * directionX + directionY * directionY);
            const bool hasDirection =
                directionLength > math::q32_32::from_fraction(1, 10'000);
            // DeliverPayload's legacy formation offsets are not a
            // plain lateral fan.  Each alternating lane is one
            // backward unit plus a +/-90-degree unit, which staggers
            // wing aircraft behind the leader while preserving a
            // parallel route.
            const math::q32_32 backX = hasDirection
                ? -directionX / directionLength : math::q32_32{};
            const math::q32_32 backY = hasDirection
                ? -directionY / directionLength : math::q32_32{};
            const math::q32_32 counterClockwiseX = backX - backY;
            const math::q32_32 counterClockwiseY = backY + backX;
            const math::q32_32 clockwiseX = backX + backY;
            const math::q32_32 clockwiseY = backY - backX;
            const math::q32_32 orientation = math::fixed_atan2(
                directionY, directionX);
            const uint64_t deliveryDecalMilliseconds =
                nugget.deliveryDecalOpacityThrobMilliseconds;
            const uint64_t deliveryDecalFramesPerSecond =
                std::max<uint32_t>(
                    1u, m_world.m_objectSimulation.rules().
                        logicFramesPerSecond);
            const uint64_t deliveryDecalThrobTicks =
                std::max<uint64_t>(
                    1u,
                    (deliveryDecalMilliseconds *
                         deliveryDecalFramesPerSecond + 999u) /
                        1000u);

            // DeliverPayloadAIUpdate selects its current SET_NORMAL
            // locomotor before applying StartAtPreferredHeight and
            // StartAtMaxSpeed. Resolve that immutable session value
            // once for the entire formation; do not consult the
            // process-global reloadable LocomotorStore mid-match.
            container::SharedPtr<const game::ObjectArchetype>
                deliveryTransportArchetype;
            const game::FrozenLocomotorTemplate* deliveryLocomotor = nullptr;
            if (item.ocl.createDeliveryOwner &&
                !nugget.transport.empty()) {
                deliveryTransportArchetype =
                    m_content.m_contentSnapshot.findObjectArchetype(
                        nugget.transport);
                if (deliveryTransportArchetype) {
                    const auto selectLoadedLocomotor =
                        [&](const container::Vector<container::String>&
                                names) {
                            for (const container::String& name : names) {
                                const game::FrozenLocomotorTemplate* candidate =
                                    m_content.m_contentSnapshot.findLocomotor(name);
                                if (candidate &&
                                    candidate->supportsRuntimeLocomotion())
                                    return candidate;
                            }
                            return static_cast<
                                const game::FrozenLocomotorTemplate*>(nullptr);
                        };
                    for (const game::LocomotorSetDefinition& set :
                         deliveryTransportArchetype->templateData.
                             locomotorSets) {
                        if (set.slot !=
                            game::LocomotorSetSlot::Normal) continue;
                        deliveryLocomotor =
                            selectLoadedLocomotor(set.templates);
                        if (deliveryLocomotor) break;
                    }
                    if (!deliveryLocomotor &&
                        deliveryTransportArchetype->templateData.
                            locomotorSets.empty()) {
                        deliveryLocomotor = selectLoadedLocomotor(
                            deliveryTransportArchetype->templateData.
                                locomotors);
                    }
                }
            }

            for (uint32_t formationIndex = 0;
                 formationIndex < formationSize; ++formationIndex) {
                const math::q32_32 offsetMultiplier =
                    math::q32_32{static_cast<int32_t>(
                        (formationIndex + 1u) / 2u)} *
                    nugget.formationSpacing;
                const math::q32_32 offsetX = (formationIndex & 1u
                    ? counterClockwiseX : clockwiseX) *
                    offsetMultiplier;
                const math::q32_32 offsetY = (formationIndex & 1u
                    ? counterClockwiseY : clockwiseY) *
                    offsetMultiplier;
                const math::q32_32 routeX =
                    targetPosition.x + offsetX;
                const math::q32_32 routeY =
                    targetPosition.y + offsetY;
                const math::q32_32 convergence =
                    math::q32_32{int32_t{1}} -
                    nugget.weaponConvergenceFactor;
                math::q32_32 effectX =
                    targetPosition.x + offsetX * convergence;
                math::q32_32 effectY =
                    targetPosition.y + offsetY * convergence;
                if (formationIndex > 0 &&
                    nugget.weaponErrorRadius > math::q32_32{1}) {
                    // Preserve RefCode's deterministic RNG order:
                    // error radius, error angle, then delivery delay.
                    const math::q32_32 radius =
                        m_content.m_simulationRandom.fixedInclusive(
                            math::q32_32{},
                            nugget.weaponErrorRadius);
                    const math::q32_32 angle =
                        m_content.m_simulationRandom.fixedInclusive(
                            math::q32_32{},
                            math::q32_32{
                                6.28318530717958647692});
                    const math::q32_32_sincos errorDirection =
                        math::fixed_sincos(angle);
                    effectX += radius * errorDirection.cosine;
                    effectY += radius * errorDirection.sine;
                }
                const math::q32_32 startBackstep = math::q32_32::max(
                    math::q32_32{}, nugget.deliveryDistance) *
                    math::q32_32::from_fraction(3, 2);
                const math::q32_32 startX =
                    basePosition.x + offsetX -
                    (hasDirection
                        ? directionX / directionLength * startBackstep
                        : math::q32_32{});
                const math::q32_32 startY =
                    basePosition.y + offsetY -
                    (hasDirection
                        ? directionY / directionLength * startBackstep
                        : math::q32_32{});
                math::q32_32 startZ = basePosition.z;
                if (item.ocl.createDeliveryOwner &&
                    nugget.startAtPreferredHeight &&
                    deliveryLocomotor && m_content.m_terrain.isLoaded()) {
                    startZ = math::q32_32::from_raw(
                        m_content.m_terrain
                            .groundHeightRaw(startX.raw(), startY.raw())) +
                        deliveryLocomotor->fixed.preferredHeight;
                }
                const LogicFixedVec3 startPosition{
                    startX,
                    startY,
                    startZ};
                const LogicFixedVec3 routePosition{
                    routeX, routeY, targetPosition.z};
                const LogicFixedVec3 effectPosition{
                    effectX, effectY,
                    targetPosition.z};

                ObjectId transport = item.ocl.source;
                std::optional<ecs::entity> transportEntity =
                    m_world.m_objects.entityFromId(transport);
                if (item.ocl.createDeliveryOwner) {
                    if (nugget.transport.empty() ||
                        !m_content.m_contentSnapshot.findObjectArchetype(
                            nugget.transport)) continue;
                    ObjectSpawnRequest transportRequest;
                    transportRequest.templateName = nugget.transport;
                    transportRequest.owner = item.ocl.owner;
                    transportRequest.primaryTeam = creationTeam;
                    transportRequest.transform = ObjectFixedTransformComponent{
                            .position = startPosition,
                            .yawRadians = orientation,
                            .authoritative = true,
                        };
                    transportRequest.origin = ObjectCreationOrigin::System;
                    transportRequest.confirmedTick = item.ocl.confirmedTick;
                    transportRequest.producer = item.ocl.source;
                    GameSessionObjectSpawnResult spawnedTransport =
                        m_lifecycle.spawnObject(std::move(transportRequest));
                    if (!spawnedTransport) continue;
                    transport = spawnedTransport.object;
                    transportEntity = spawnedTransport.entity;
                    if (item.oclState &&
                        !item.oclState->firstCreatedObject)
                        item.oclState->firstCreatedObject = transport;
                    if (item.oclState) ++item.oclState->createdObjects;

                    if (nugget.delayDeliveryMaximumMilliseconds > 0) {
                        const uint64_t maximumDelayTicks =
                            (static_cast<uint64_t>(
                                 nugget.delayDeliveryMaximumMilliseconds) *
                                 deliveryDecalFramesPerSecond + 999u) /
                            1000u;
                        const int32_t sampleMaximum = static_cast<int32_t>(
                            std::min<uint64_t>(
                                maximumDelayTicks,
                                static_cast<uint64_t>(
                                    std::numeric_limits<int32_t>::max())));
                        const uint64_t sampledDelay = static_cast<uint64_t>(
                            m_content.m_simulationRandom.integerInclusive(
                                0, sampleMaximum));
                        const uint64_t disabledUntil = sampledDelay >
                                std::numeric_limits<uint64_t>::max() -
                                    item.ocl.confirmedTick
                            ? std::numeric_limits<uint64_t>::max()
                            : item.ocl.confirmedTick + sampledDelay;
                        static_cast<void>(ObjectDisabledSystem::setUntil(
                            m_world.m_registry, *transportEntity,
                            ObjectDisabledReason::Default,
                            disabledUntil, item.ocl.confirmedTick));
                    }
                }
                if (!transportEntity) continue;

                // RefCode assigns completion ownership independently
                // of whether the delivery transport was just created
                // or reuses the invoking object. Only formation leader
                // zero may complete the originating special power;
                // explicit INVALID is still first-write state and
                // prevents a later fallback from claiming a wingman.
                static_cast<void>(
                    m_world.m_objectSimulation.setSpecialPowerCompletionCreator(
                        m_world.m_registry, m_world.m_objects, transport,
                        formationIndex == 0
                            ? item.ocl.source
                            : INVALID_OBJECT_ID));

                if (item.ocl.createDeliveryOwner &&
                    nugget.startAtMaximumSpeed &&
                    deliveryLocomotor) {
                    const ObjectPhysicsComponent* physics =
                        ecs::try_get<ObjectPhysicsComponent>(
                            m_world.m_registry, *transportEntity);
                    if (physics) {
                        const auto speed =
                            deliveryLocomotor->fixed.maximumSpeed;
                        const math::q32_32_sincos forward =
                            math::fixed_sincos(orientation);
                        queueOclPhysics(item, 
                            transport,
                            ObjectPhysicsRequestKind::ApplyMotiveForce,
                            {
                                forward.cosine * speed * physics->mass,
                                forward.sine * speed * physics->mass,
                                {},
                            },
                            nugget.authoredOrder);
                    }
                }

                bool payloadReady = true;
                for (const game::ObjectCreationPayloadEntry& entry :
                     nugget.payload) {
                    if (entry.object.empty() || entry.count == 0 ||
                        !m_content.m_contentSnapshot.findObjectArchetype(
                            entry.object)) {
                        payloadReady = false;
                        break;
                    }
                    for (uint32_t payloadIndex = 0;
                         payloadIndex < entry.count; ++payloadIndex) {
                        if (++m_createdOclObjects >
                            kMaximumOclCreatedObjects) {
                            payloadReady = false;
                            break;
                        }
                        ObjectSpawnRequest payloadRequest;
                        payloadRequest.templateName = entry.object;
                        payloadRequest.owner = item.ocl.owner;
                        payloadRequest.primaryTeam = creationTeam;
                        payloadRequest.transform = ObjectFixedTransformComponent{
                                .position = startPosition,
                                .yawRadians = orientation,
                                .authoritative = true,
                            };
                        payloadRequest.origin = ObjectCreationOrigin::System;
                        payloadRequest.confirmedTick =
                            item.ocl.confirmedTick;
                        payloadRequest.producer = transport;
                        GameSessionObjectSpawnResult payload =
                            m_lifecycle.spawnObject(std::move(payloadRequest));
                        if (!payload) {
                            payloadReady = false;
                            break;
                        }
                        const ObjectId payloadCreator =
                            formationIndex == 0 && payloadIndex == 0
                            ? item.ocl.source
                            : INVALID_OBJECT_ID;
                        static_cast<void>(
                            m_world.m_objectSimulation.
                                setSpecialPowerCompletionCreator(
                                    m_world.m_registry, m_world.m_objects,
                                    payload.object,
                                    payloadCreator));

                        ObjectId carriedObject = payload.object;
                        if (!nugget.putInContainer.empty()) {
                            ObjectSpawnRequest wrapperRequest;
                            wrapperRequest.templateName =
                                nugget.putInContainer;
                            wrapperRequest.owner = item.ocl.owner;
                            wrapperRequest.primaryTeam = creationTeam;
                            wrapperRequest.transform = ObjectFixedTransformComponent{
                                    .position = startPosition,
                                    .yawRadians = orientation,
                                    .authoritative = true,
                                };
                            wrapperRequest.origin =
                                ObjectCreationOrigin::System;
                            wrapperRequest.confirmedTick =
                                item.ocl.confirmedTick;
                            wrapperRequest.producer = transport;
                            GameSessionObjectSpawnResult wrapper =
                                m_lifecycle.spawnObject(std::move(wrapperRequest));
                            if (!wrapper ||
                                !m_world.m_objectSimulation.requestContainment(
                                    m_world.m_registry, m_world.m_objects, {
                                        .kind = ObjectContainmentRequestKind::Attach,
                                        .container = wrapper.object,
                                        .object = payload.object,
                                        .confirmedTick = item.ocl.confirmedTick,
                                        .force = true,
                                    }, &m_content.m_players,
                                    &m_content.m_contentSnapshot)) {
                                static_cast<void>(m_lifecycle.requestDestroyObject(
                                    payload.object,
                                    ObjectDestroyReason::System,
                                    item.ocl.confirmedTick));
                                if (wrapper)
                                    static_cast<void>(m_lifecycle.requestDestroyObject(
                                        wrapper.object,
                                        ObjectDestroyReason::System,
                                        item.ocl.confirmedTick));
                                payloadReady = false;
                                break;
                            }
                            static_cast<void>(
                                m_world.m_objectSimulation.
                                    setSpecialPowerCompletionCreator(
                                        m_world.m_registry, m_world.m_objects,
                                        wrapper.object,
                                        payloadCreator));
                            carriedObject = wrapper.object;
                        }

                        if (!m_world.m_objectSimulation.requestContainment(
                                m_world.m_registry, m_world.m_objects, {
                                    .kind = ObjectContainmentRequestKind::Attach,
                                    .container = transport,
                                    .object = carriedObject,
                                    .confirmedTick = item.ocl.confirmedTick,
                                    .force = true,
                                }, &m_content.m_players,
                                &m_content.m_contentSnapshot)) {
                            static_cast<void>(m_lifecycle.requestDestroyObject(
                                carriedObject,
                                ObjectDestroyReason::System,
                                item.ocl.confirmedTick));
                            payloadReady = false;
                            break;
                        }
                    }
                    if (!payloadReady) break;
                }
                if (!payloadReady) continue;

                static_cast<void>(
                    m_world.m_objectSimulation.requestTransportBehavior(
                        m_world.m_registry, m_world.m_objects, {
                            .kind = ObjectTransportBehaviorRequestKind::DeliverPayload,
                            .object = transport,
                            .x = effectPosition.x,
                            .y = effectPosition.y,
                            .z = effectPosition.z,
                            .routeX = routePosition.x,
                            .routeY = routePosition.y,
                            .routeZ = routePosition.z,
                            // OCL PutInContainer was materialized
                            // above; the transport carries that
                            // wrapper and drops it directly.
                            .payloadContainerTemplate = {},
                            .payloadWeaponTemplate = {},
                            .deliveryDistance = nugget.deliveryDistance,
                            .exitPitchRate = nugget.exitPitchRate,
                            .diveStartDistance =
                                nugget.diveStartDistance,
                            .diveEndDistance =
                                nugget.diveEndDistance,
                            .strafeLength = nugget.strafeLength,
                            .preOpenDistance = nugget.preOpenDistance,
                            .dropOffsetX = nugget.dropOffset.x,
                            .dropOffsetY = nugget.dropOffset.y,
                            .dropOffsetZ = nugget.dropOffset.z,
                            .dropVarianceX = nugget.dropVariance.x,
                            .dropVarianceY = nugget.dropVariance.y,
                            .dropVarianceZ = nugget.dropVariance.z,
                            .dropDelayMilliseconds =
                                nugget.dropDelayMilliseconds,
                            .maximumAttempts = nugget.maximumAttempts,
                            .visibleItemsDroppedPerInterval =
                                nugget.visibleItemsDroppedPerInterval,
                            .visiblePayloadCount = nugget.visibleNumBones,
                            .hasDeliveryOverride = true,
                            .hasDeliveryRouteTarget = true,
                            .inheritTransportVelocity =
                                nugget.inheritTransportVelocity,
                            .parachuteDirectly =
                                nugget.parachuteDirectly,
                            .selfDestructAfterDelivery =
                                nugget.selfDestructObject,
                            .fireWeaponPayload = nugget.fireWeapon,
                            // RefCode gives only formation leader 0
                            // the delivery marker; wing transports
                            // receive radius zero.
                            .deliveryDecal = formationIndex == 0
                                ? nugget.deliveryDecal
                                : container::String{},
                            .deliveryDecalRadius = formationIndex == 0
                                ? nugget.deliveryDecalRadius
                                : math::q32_32{},
                            .deliveryDecalShadowTypeMask =
                                nugget.deliveryDecalShadowTypeMask,
                            .deliveryDecalMinimumOpacity =
                                nugget.deliveryDecalMinimumOpacity,
                            .deliveryDecalMaximumOpacity =
                                nugget.deliveryDecalMaximumOpacity,
                            .deliveryDecalOpacityThrobTicks =
                                deliveryDecalThrobTicks,
                            .deliveryDecalColor =
                                nugget.deliveryDecalColor,
                            .deliveryDecalUsesPlayerColor =
                                nugget.deliveryDecalUsesPlayerColor,
                            .deliveryDecalOnlyVisibleToOwningPlayer =
                                nugget.deliveryDecalOnlyVisibleToOwningPlayer,
                            .visiblePayloadTemplate =
                                nugget.visiblePayloadTemplateName,
                            .visiblePayloadWeapon =
                                nugget.visiblePayloadWeaponTemplate,
                            .visibleDropBoneBaseName =
                                nugget.visibleDropBoneBaseName,
                            .visibleSubObjectBaseName =
                                nugget.visibleSubObjectBaseName,
                            .strafingWeaponSlot =
                                nugget.strafingWeaponSlot,
                            .strafeWeaponFx = nugget.strafeWeaponFx,
                            .confirmedTick = item.ocl.confirmedTick,
                        }));
            }
            return;
}

} // namespace engine::detail
