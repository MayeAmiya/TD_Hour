#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/frame/GameSessionFxAnchorSnapshot.h"

#include "debug/debug.h"
#include "core/container/string_utils.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/object/definition/ObjectArchetype.h"
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
namespace {

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] LogicFixedVec3 rotateOclOffset(
    game::ObjectCreationFixedOffset value, math::q32_32 rollRadians,
    math::q32_32 pitchRadians, math::q32_32 yawRadians) noexcept {
    const math::q32_32_sincos roll = math::fixed_sincos(-rollRadians);
    value = {
        value.x,
        value.y * roll.cosine - value.z * roll.sine,
        value.y * roll.sine + value.z * roll.cosine,
    };
    const math::q32_32_sincos pitch = math::fixed_sincos(pitchRadians);
    value = {
        value.x * pitch.cosine + value.z * pitch.sine,
        value.y,
        -value.x * pitch.sine + value.z * pitch.cosine,
    };
    const math::q32_32_sincos yaw = math::fixed_sincos(yawRadians);
    return {
        value.x * yaw.cosine - value.y * yaw.sine,
        value.x * yaw.sine + value.y * yaw.cosine,
        value.z,
    };
}

[[nodiscard]] LogicFixedVec3 addFixedPosition(
    const LogicFixedVec3& left, const LogicFixedVec3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

} // namespace

void GameSessionWeaponEventDrain::processOclCreation(
    const WorkItem& item, const game::ObjectCreationNugget& sourceNugget) {
    const auto hasDisposition = [](game::ObjectCreationDispositionMask mask,
                                   game::ObjectCreationDisposition flag) noexcept {
        return (mask & game::objectCreationDispositionBit(flag)) != 0;
    };
    const auto fixedPosition = [](const LogicFixedVec3& value) noexcept {
        return math::vec3{value.x.to_float(), value.y.to_float(),
                          value.z.to_float()};
    };
    std::visit([&](const auto& nugget) {
        using Nugget = std::decay_t<decltype(nugget)>;
        if constexpr (std::is_same_v<Nugget,
                          game::ObjectCreationCreateObjectNugget> ||
                      std::is_same_v<Nugget,
                          game::ObjectCreationCreateDebrisNugget>) {
            const game::ObjectCreationGenericFields& common = nugget.common;
            if (common.names.empty() || !item.ocl.owner) return;
            const ObjectTeamId creationTeam =
                m_world.m_objectTeams.defaultTeam(item.ocl.owner).value_or(
                    item.ocl.primaryTeam);
            if (!creationTeam) return;
            if constexpr (std::is_same_v<Nugget,
                          game::ObjectCreationCreateObjectNugget>) {
                const PlayerState* owner = m_content.m_players.get(item.ocl.owner);
                if ((nugget.requiresLivePlayer &&
                    (!owner || owner->life != PlayerLifeState::Active)) ||
                    (nugget.skipIfSignificantlyAirborne &&
                     item.ocl.sourceAirborne)) {
                    return;
                }
            }

            const auto registerCreatedObject = [&] (
                    const GameSessionObjectSpawnResult& created) {
                const bool firstCreatedInInvocation =
                    !item.oclState ||
                    item.oclState->specialPowerCreationOrdinal == 0;
                if (item.oclState) {
                    ++item.oclState->specialPowerCreationOrdinal;
                }
                static_cast<void>(
                    m_world.m_objectSimulation.setSpecialPowerCompletionCreator(
                        m_world.m_registry, m_world.m_objects, created.object,
                        firstCreatedInInvocation
                            ? item.ocl.source
                            : INVALID_OBJECT_ID));
                if (item.oclState) {
                    if (!item.oclState->firstCreatedObject) {
                        item.oclState->firstCreatedObject =
                            created.object;
                    }
                    ++item.oclState->createdObjects;
                }
            };

            ObjectId inheritedScriptNameHolder = item.ocl.source;
            const auto applyCreateObjectInheritance = [&] (
                    ObjectId createdObject) {
                if constexpr (std::is_same_v<
                                  Nugget,
                                  game::ObjectCreationCreateObjectNugget>) {
                    if (!nugget.inheritsVeterancy || !createdObject) {
                        return;
                    }
                    const PlayerState* owner =
                        m_content.m_players.get(item.ocl.owner);
                    const ObjectExperienceMutation mutation =
                        m_world.m_objectSimulation.setObjectVeterancyLevel(
                            m_world.m_registry, m_world.m_objects, createdObject,
                            item.ocl.veterancy,
                            owner ? owner->upgrades.completed : UpgradeMask{},
                            item.ocl.confirmedTick,
                            {.players = &m_content.m_players,
                             .scienceCatalog =
                                 m_content.m_contentSnapshot.scienceCatalog(),
                             .content = &m_content.m_contentSnapshot,
                             .random = &m_content.m_simulationRandom,
                             .effects = &m_world.m_objectSimulation});
                    if (!mutation.accepted ||
                        !inheritedScriptNameHolder ||
                        m_presentation.m_scriptObjects.transferObjectNames(
                            inheritedScriptNameHolder,
                            createdObject) == 0) {
                        return;
                    }
                    ++m_presentation.m_scriptPresentationSequence;
                    if (m_presentation.m_scriptPresentationSequence == 0) {
                        ++m_presentation.m_scriptPresentationSequence;
                    }
                    static_cast<void>(
                        m_presentation.m_scriptObjectPresentation.
                            transferCustomIndicatorColor(
                                item.ocl.source, createdObject,
                                {.presentationEpoch =
                                     m_presentation.m_scriptPresentationEpoch,
                                 .sequence =
                                     m_presentation.m_scriptPresentationSequence,
                                 .confirmedTick =
                                     item.ocl.confirmedTick,
                                 .sourceScriptId = 0,
                                 .ordinal = 0}));
                    inheritedScriptNameHolder = createdObject;
                }
            };

            const bool debris = std::is_same_v<
                Nugget, game::ObjectCreationCreateDebrisNugget>;
            size_t debrisAnimationSetCount = 0;
            if constexpr (std::is_same_v<
                              Nugget,
                              game::ObjectCreationCreateDebrisNugget>) {
                debrisAnimationSetCount = nugget.animationSets.size();
            }
            const auto candidateTraits = [&] (
                    const container::SharedPtr<
                        const game::ObjectArchetype>& archetype) {
                bool hasLifetimeUpdate = false;
                if (archetype && archetype->lifetimePlan) {
                    hasLifetimeUpdate = std::any_of(
                        archetype->lifetimePlan->rules.begin(),
                        archetype->lifetimePlan->rules.end(),
                        [](const game::ObjectLifetimeRule& rule) {
                            return rule.action ==
                                game::ObjectLifetimeAction::Kill;
                        });
                }
                return ObjectOclCreateCandidateTraits{
                    .available = static_cast<bool>(archetype),
                    .hasPhysics = archetype &&
                        static_cast<bool>(archetype->physicsPlan),
                    .hasLifetimeUpdate = hasLifetimeUpdate,
                };
            };
            container::Vector<ObjectOclCreateCandidateTraits>
                payloadCandidateTraits;
            payloadCandidateTraits.reserve(common.names.size());
            const container::SharedPtr<const game::ObjectArchetype>
                genericDebrisArchetype = debris
                ? m_content.m_contentSnapshot.findObjectArchetype("GenericDebris")
                : nullptr;
            for (const container::String& name : common.names) {
                payloadCandidateTraits.push_back(candidateTraits(
                    debris ? genericDebrisArchetype
                           : m_content.m_contentSnapshot.findObjectArchetype(name)));
            }

            container::SharedPtr<const game::ObjectArchetype>
                wrapperArchetype;
            container::Array<ObjectOclCreateCandidateTraits, 1>
                wrapperCandidateTraits{};
            if (!common.putInContainer.empty()) {
                wrapperArchetype =
                    m_content.m_contentSnapshot.findObjectArchetype(
                        common.putInContainer);
                if (!wrapperArchetype) return;
                wrapperCandidateTraits[0] =
                    candidateTraits(wrapperArchetype);
            }

            // Freeze the complete nugget-owned RNG transaction before
            // any spawn callback can enqueue nested work. Payloads
            // retain authored order; the wrapper's doStuffToObj sample
            // follows every payload, matching RefCode's final call.
            container::Vector<ObjectOclCreateRandomSample>
                payloadRandomSamples;
            const size_t wrapperReservation = wrapperArchetype ? 1u : 0u;
            const size_t availableCreateBudget = m_createdOclObjects <
                    kMaximumOclCreatedObjects
                ? kMaximumOclCreatedObjects - m_createdOclObjects
                : 0u;
            const size_t payloadSampleCount = std::min<size_t>(
                common.count,
                availableCreateBudget > wrapperReservation
                    ? availableCreateBudget - wrapperReservation
                    : 0u);
            const bool payloadTailTruncated =
                payloadSampleCount < common.count;
            payloadRandomSamples.reserve(payloadSampleCount);
            for (uint32_t createdIndex = 0;
                 createdIndex < payloadSampleCount; ++createdIndex) {
                payloadRandomSamples.push_back(
                    sampleObjectOclCreateRandom({
                        .common = &common,
                        .candidates = payloadCandidateTraits,
                        .animationSetCount =
                            debrisAnimationSetCount,
                        .lifetimeOverrideFrames =
                            item.ocl.lifetimeOverrideFrames,
                        .logicFramesPerSecond = static_cast<uint32_t>(
                            std::max(1, m_content.m_startInfo.gameSpeedFPS)),
                        .chooseModel = true,
                        .allowSpread = true,
                        .allowDebrisAnimation = debris,
                    }, m_content.m_simulationRandom));
            }
            ObjectOclCreateRandomSample wrapperRandomSample;
            if (wrapperArchetype) {
                wrapperRandomSample = sampleObjectOclCreateRandom({
                    .common = &common,
                    .candidates = wrapperCandidateTraits,
                    .lifetimeOverrideFrames =
                        item.ocl.lifetimeOverrideFrames,
                    .logicFramesPerSecond = static_cast<uint32_t>(
                        std::max(1, m_content.m_startInfo.gameSpeedFPS)),
                    .chooseModel = false,
                    .allowSpread = false,
                    .allowDebrisAnimation = false,
                }, m_content.m_simulationRandom);
            }

            ObjectId wrapperObject = INVALID_OBJECT_ID;
            GameSessionObjectSpawnResult spawnedWrapper;
            LogicFixedVec3 wrapperPositionFixed =
                item.ocl.primaryPosition;
            math::vec3 wrapperPosition =
                fixedPosition(wrapperPositionFixed);
            math::q32_32 wrapperOrientationFixed =
                item.ocl.orientationRadians;
            float wrapperOrientation = wrapperOrientationFixed.to_float();
            std::optional<uint32_t> wrapperPathfindLayer =
                game::terrain::kGroundPathfindLayer;
            const bool dispositionReplacesLikeExistingPose =
                hasDisposition(common.disposition,
                    game::ObjectCreationDisposition::OnGroundAligned) ||
                hasDisposition(common.disposition,
                    game::ObjectCreationDisposition::SendItOut) ||
                hasDisposition(common.disposition,
                    game::ObjectCreationDisposition::SendItFlying) ||
                hasDisposition(common.disposition,
                    game::ObjectCreationDisposition::SendItUp) ||
                hasDisposition(common.disposition,
                    game::ObjectCreationDisposition::RandomForce);
            if (!common.putInContainer.empty()) {
                if (++m_createdOclObjects >
                        kMaximumOclCreatedObjects) {
                    return;
                }
                wrapperPositionFixed = addFixedPosition(
                    wrapperPositionFixed,
                    rotateOclOffset(common.offset, item.ocl.rollRadians,
                                    item.ocl.pitchRadians,
                                    wrapperOrientationFixed));
                const ObjectFixedTransformComponent
                    wrapperLikeExistingTransform{
                        .position = wrapperPositionFixed,
                        .yawRadians = item.ocl.orientationRadians,
                        .authoritative = true,
                    };
                if (wrapperRandomSample.hasOnGroundOrientation) {
                    wrapperOrientationFixed = wrapperRandomSample.
                        onGroundOrientationRadians;
                    wrapperOrientation = wrapperOrientationFixed.to_float();
                    const uint32_t layer = m_content.m_terrain.
                        highestPathfindLayerAtXYRaw(
                            wrapperPositionFixed.x.raw(),
                            wrapperPositionFixed.y.raw());
                    wrapperPositionFixed.z = math::q32_32::from_raw(
                        m_content.m_terrain.pathfindLayerHeightRawAt(
                            layer, wrapperPositionFixed.x.raw(),
                            wrapperPositionFixed.y.raw())
                            .value_or(m_content.m_terrain.groundHeightRaw(
                                wrapperPositionFixed.x.raw(),
                                wrapperPositionFixed.y.raw())));
                    if (layer !=
                        game::terrain::kGroundPathfindLayer) {
                        wrapperPositionFixed.z += math::q32_32{int32_t{1}};
                    }
                    wrapperPathfindLayer = layer;
                }
                if (wrapperRandomSample.hasSendOutOrientation) {
                    wrapperOrientationFixed = wrapperRandomSample.
                        sendOutOrientationRadians;
                    wrapperOrientation = wrapperOrientationFixed.to_float();
                    wrapperPositionFixed.z = math::q32_32::from_raw(
                        m_content.m_terrain.groundHeightRaw(
                            wrapperPositionFixed.x.raw(),
                            wrapperPositionFixed.y.raw()));
                }
                wrapperPosition = fixedPosition(wrapperPositionFixed);
                ObjectSpawnRequest wrapperRequest;
                wrapperRequest.templateName = common.putInContainer;
                wrapperRequest.owner = item.ocl.owner;
                wrapperRequest.primaryTeam = creationTeam;
                wrapperRequest.transform = ObjectFixedTransformComponent{
                        .position = wrapperPositionFixed,
                        .yawRadians = wrapperOrientationFixed,
                        .authoritative = true,
                    };
                wrapperRequest.initialPathfindLayer =
                    wrapperPathfindLayer;
                wrapperRequest.origin = ObjectCreationOrigin::System;
                wrapperRequest.confirmedTick = item.ocl.confirmedTick;
                wrapperRequest.initialHealthFraction =
                    wrapperRandomSample.healthFraction;
                wrapperRequest.producer = item.ocl.source;
                wrapperRequest.flattenTerrainForStructure = hasDisposition(
                    common.disposition,
                    game::ObjectCreationDisposition::LikeExisting);
                if (wrapperRequest.flattenTerrainForStructure) {
                    wrapperRequest.terrainFlattenPlacement =
                        ObjectTerrainFlattenPlacement{
                            .footprintTransform =
                                wrapperLikeExistingTransform,
                            .groundSampleX =
                                item.ocl.primaryPosition.x,
                            .groundSampleY =
                                item.ocl.primaryPosition.y,
                            .adjustFinalObjectZ =
                                !dispositionReplacesLikeExistingPose,
                        };
                }
                spawnedWrapper =
                    m_lifecycle.spawnObject(std::move(wrapperRequest));
                if (!spawnedWrapper) return;
                wrapperObject = spawnedWrapper.object;
                static_cast<void>(
                    m_world.m_objectSimulation.bindOclSlaveMaster(
                        m_world.m_registry, m_world.m_objects, spawnedWrapper.object,
                        item.ocl.source));
                registerCreatedObject(spawnedWrapper);
            }

            for (uint32_t createdIndex = 0;
                 createdIndex < payloadRandomSamples.size();
                 ++createdIndex) {
                if (++m_createdOclObjects > kMaximumOclCreatedObjects) return;
                const ObjectOclCreateRandomSample& randomSample =
                    payloadRandomSamples[createdIndex];
                if (!randomSample.candidateAvailable) continue;
                const size_t selectedIndex = randomSample.modelIndex;
                const container::String& selectedName =
                    common.names[selectedIndex];
                const container::String templateName = debris
                    ? container::String{"GenericDebris"} : selectedName;
                const container::SharedPtr<const game::ObjectArchetype>
                    spawnArchetype =
                        m_content.m_contentSnapshot.findObjectArchetype(templateName);
                if (!spawnArchetype) continue;

                LogicFixedVec3 positionFixed = addFixedPosition(
                    item.ocl.primaryPosition,
                    rotateOclOffset(common.offset, item.ocl.rollRadians,
                                    item.ocl.pitchRadians,
                                    item.ocl.orientationRadians));
                const uint32_t sourcePathfindLayer =
                    item.ocl.sourcePathfindLayer;
                std::optional<uint32_t> createdPathfindLayer =
                    game::terrain::kGroundPathfindLayer;
                if (randomSample.hasSpread) {
                    container::Vector<navigation::NavigationCellId>&
                        footprintScratch =
                            m_content.m_navigationFootprintScratch;
                    const ObjectOclSpreadPlacementResult placement =
                        findObjectOclSpreadPlacement(
                            m_content.m_navigation.layers(),
                            {
                                .center = {positionFixed.x.raw(),
                                           positionFixed.y.raw(),
                                           positionFixed.z.raw()},
                                .minimumRadius =
                                    randomSample.spreadMinimumRadius,
                                .maximumRadius =
                                    common.maximumFormationDistance,
                                .startAngleRadians =
                                    randomSample.
                                        spreadStartAngleRadians,
                                .footprintRadius =
                                    math::q32_32::max(
                                        math::q32_32{},
                                        spawnArchetype->templateData.geometry
                                            .boundingCircleRadiusFixed),
                                .movementMask =
                                    objectCreationNavigationMovementMask(
                                        *spawnArchetype,
                                        m_content.m_contentSnapshot),
                            },
                            footprintScratch);
                    if (placement.found()) {
                        game::terrain::TerrainPathfindLayerId
                            terrainLayer =
                                game::terrain::kGroundPathfindLayer;
                        if (navigation::
                                tryTerrainPathfindLayerFromNavigationLayer(
                                    placement.navigationLayer,
                                    terrainLayer)) {
                            const math::q32_32 x = math::q32_32::from_raw(
                                placement.position.xRaw);
                            const math::q32_32 y = math::q32_32::from_raw(
                                placement.position.yRaw);
                            positionFixed.x = x;
                            positionFixed.y = y;
                            math::q32_32 z = math::q32_32::from_raw(
                                m_content.m_terrain.pathfindLayerHeightRawAt(
                                    terrainLayer, positionFixed.x.raw(),
                                    positionFixed.y.raw())
                                    .value_or(placement.position.zRaw));
                            if (terrainLayer !=
                                game::terrain::kGroundPathfindLayer) {
                                z += math::q32_32{int32_t{1}};
                            }
                            positionFixed.z = z;
                            createdPathfindLayer = terrainLayer;
                        }
                    }
                }

                math::q32_32 orientationFixed =
                    item.ocl.orientationRadians;
                float orientation = orientationFixed.to_float();
                const ObjectFixedTransformComponent
                    likeExistingFlattenTransform{
                        .position = positionFixed,
                        .yawRadians = orientationFixed,
                        .authoritative = true,
                    };
                if (hasDisposition(common.disposition,
                        game::ObjectCreationDisposition::OnGroundAligned)) {
                    orientationFixed = randomSample.
                        onGroundOrientationRadians;
                    orientation = orientationFixed.to_float();
                    const uint32_t layer = m_content.m_terrain.
                        highestPathfindLayerAtXYRaw(
                            positionFixed.x.raw(), positionFixed.y.raw());
                    positionFixed.z = math::q32_32::from_raw(
                        m_content.m_terrain.pathfindLayerHeightRawAt(
                            layer, positionFixed.x.raw(),
                            positionFixed.y.raw())
                            .value_or(m_content.m_terrain.groundHeightRaw(
                                positionFixed.x.raw(),
                                positionFixed.y.raw())));
                    if (layer != game::terrain::kGroundPathfindLayer)
                        positionFixed.z += math::q32_32{int32_t{1}};
                    createdPathfindLayer = layer;
                } else if (common.preserveLayer &&
                           common.putInContainer.empty() &&
                           sourcePathfindLayer !=
                               game::terrain::kGroundPathfindLayer) {
                    createdPathfindLayer = sourcePathfindLayer;
                }
                if (hasDisposition(
                        common.disposition,
                        game::ObjectCreationDisposition::SendItOut)) {
                    // RefCode samples this yaw after initial health and
                    // after ON_GROUND_ALIGNED, then snaps only Z to the
                    // heightfield before sampling the horizontal force.
                    orientationFixed = randomSample.
                        sendOutOrientationRadians;
                    orientation = orientationFixed.to_float();
                    positionFixed.z = math::q32_32::from_raw(
                        m_content.m_terrain.groundHeightRaw(
                            positionFixed.x.raw(), positionFixed.y.raw()));
                }

                const math::vec3 position = fixedPosition(positionFixed);
                ObjectSpawnRequest request;
                request.templateName = templateName;
                request.owner = item.ocl.owner;
                // Generic OCL uses the controlling player's default
                // Team, not the source object's scenario Team.
                request.primaryTeam = creationTeam;
                request.transform = ObjectFixedTransformComponent{
                    .position = positionFixed,
                    .yawRadians = orientationFixed,
                    .authoritative = true,
                };
                request.initialPathfindLayer = createdPathfindLayer;
                request.origin = ObjectCreationOrigin::System;
                request.confirmedTick = item.ocl.confirmedTick;
                request.initialHealthFraction = randomSample.healthFraction;
                request.producer = item.ocl.source;
                request.flattenTerrainForStructure = hasDisposition(
                    common.disposition,
                    game::ObjectCreationDisposition::LikeExisting);
                if (request.flattenTerrainForStructure) {
                    request.terrainFlattenPlacement =
                        ObjectTerrainFlattenPlacement{
                            .footprintTransform =
                                likeExistingFlattenTransform,
                            .groundSampleX =
                                item.ocl.primaryPosition.x,
                            .groundSampleY =
                                item.ocl.primaryPosition.y,
                            .adjustFinalObjectZ =
                                !dispositionReplacesLikeExistingPose,
                        };
                }
                GameSessionObjectSpawnResult spawned =
                    m_lifecycle.spawnObject(std::move(request));
                if (!spawned) continue;
                // Generic OCL invokes SlavedUpdate::onEnslave(source)
                // but does not add the object to a SpawnBehavior
                // occurrence. Preserve that distinct typed relation;
                // producer provenance alone is never reinterpreted as
                // Spawn child ownership.
                static_cast<void>(
                    m_world.m_objectSimulation.bindOclSlaveMaster(
                        m_world.m_registry, m_world.m_objects, spawned.object,
                        item.ocl.source));
                if (wrapperObject &&
                    !m_world.m_objectSimulation.requestContainment(
                        m_world.m_registry, m_world.m_objects, {
                            .kind =
                                ObjectContainmentRequestKind::Attach,
                            .container = wrapperObject,
                            .object = spawned.object,
                            .confirmedTick =
                                item.ocl.confirmedTick,
                            .force = true,
                        }, &m_content.m_players, &m_content.m_contentSnapshot)) {
                    // The wrapper is the OCL's first result. A payload
                    // that cannot enter it is never published as a
                    // free-standing substitute.
                    static_cast<void>(m_lifecycle.requestDestroyObject(
                        spawned.object, ObjectDestroyReason::System,
                        item.ocl.confirmedTick));
                    continue;
                }
                if constexpr (std::is_same_v<
                                  Nugget,
                                  game::ObjectCreationCreateObjectNugget>) {
                    // ContainInsideSourceObject is forced admission through
                    // the source's authored Contain module, not a typeless
                    // parent edge. Retaining the selected rule is required by
                    // Overlord/Helix PassengersInTurret transform authority.
                    if (nugget.containInsideSourceObject &&
                        !m_world.m_objectSimulation.requestContainment(
                            m_world.m_registry, m_world.m_objects,
                            {
                                .kind = ObjectContainmentRequestKind::Attach,
                                .container = item.ocl.source,
                                .object = spawned.object,
                                .confirmedTick = item.ocl.confirmedTick,
                                .force = true,
                            },
                            &m_content.m_players,
                            &m_content.m_contentSnapshot)) {
                        // RefCode treats a failed forced containment
                        // as stillborn; never leak the add-on as a
                        // free-standing world object.
                        static_cast<void>(m_lifecycle.requestDestroyObject(
                            spawned.object,
                            ObjectDestroyReason::System,
                            item.ocl.confirmedTick));
                        continue;
                    }
                }

                // LIKE_EXISTING copies the source object's complete
                // creation pose before any force disposition runs.
                // Transform remains the legacy yaw projection; a
                // free Physics body retains the frozen Q32.32 pitch
                // and roll used by render extraction and later OCLs.
                if (!dispositionReplacesLikeExistingPose &&
                    hasDisposition(
                        common.disposition,
                        game::ObjectCreationDisposition::LikeExisting)) {
                    writeAuthoritativeObjectYaw(
                        m_world.m_registry,
                        *spawned.entity, item.ocl.orientationRadians);
                    if (ObjectPhysicsComponent* physics =
                            ecs::try_get<ObjectPhysicsComponent>(
                                m_world.m_registry, *spawned.entity)) {
                        if (item.ocl.sourceOwnsFullAttitude &&
                            !ecs::try_get<ObjectLocomotionComponent>(
                                m_world.m_registry, *spawned.entity)) {
                            physics->pitch = item.ocl.pitchRadians;
                            physics->roll = item.ocl.rollRadians;
                            physics->ownsAttitude = true;
                        }
                        if (item.ocl.sourceAirborne) {
                            physics->allowToFall = true;
                        }
                    }
                }

                if (common.ignorePrimaryObstacle && item.ocl.source) {
                    static_cast<void>(
                        m_world.m_objectSimulation.setPhysicsIgnoreCollisionWith(
                            m_world.m_registry, m_world.m_objects, spawned.object,
                            item.ocl.source));
                }

                // RefCode's unfortunately named goInvulnerable()
                // arms ObjectDefectionHelper without its FX. It makes
                // the new object an undetected defector for this
                // interval; it does not bypass Body damage.
                if (common.invulnerableMilliseconds > 0) {
                    const uint64_t frameProduct =
                        static_cast<uint64_t>(
                            common.invulnerableMilliseconds) *
                        static_cast<uint64_t>(std::max(
                            1, m_content.m_startInfo.gameSpeedFPS));
                    const uint64_t durationTicks = std::max<uint64_t>(
                        1u, frameProduct / 1000u +
                            (frameProduct % 1000u != 0 ? 1u : 0u));
                    const uint64_t detectionEndTick =
                        item.ocl.confirmedTick >
                                std::numeric_limits<uint64_t>::max() -
                                    durationTicks
                            ? std::numeric_limits<uint64_t>::max()
                            : item.ocl.confirmedTick + durationTicks;
                    if (ObjectUndetectedDefectorComponent* existing =
                            ecs::try_get<
                                ObjectUndetectedDefectorComponent>(
                                m_world.m_registry, *spawned.entity)) {
                        existing->detectionEndTick = detectionEndTick;
                    } else {
                        ecs::emplace<
                            ObjectUndetectedDefectorComponent>(
                            m_world.m_registry, *spawned.entity,
                            ObjectUndetectedDefectorComponent{
                                .detectionEndTick =
                                    detectionEndTick});
                    }
                }
                registerCreatedObject(spawned);

                if (!common.particleSystem.empty()) {
                    const std::optional<game::FxInvocationAnchor>
                        attachedAnchor = session_fx::snapshotAnchor(
                            m_world.m_registry, m_world.m_objects, spawned.object);
                    static_cast<void>(m_publication.emitFxInvocationEvent({
                        .directParticle =
                            game::FxDirectParticleRequest{
                                .particleSystemName =
                                    common.particleSystem,
                                .emitterCount = 1,
                                .attachToObject = true,
                            },
                        .anchorKind = game::
                            FxInvocationAnchorKind::ObjectAttachment,
                        .primary = attachedAnchor.value_or(
                            session_fx::worldAnchor(
                                position, spawned.object)),
                    }));
                }

                if constexpr (std::is_same_v<
                                  Nugget,
                                  game::ObjectCreationCreateDebrisNugget>) {
                    if (RenderModelComponent* render =
                            ecs::try_get<RenderModelComponent>(
                                m_world.m_registry, *spawned.entity)) {
                        render->modelAsset = selectedName;
                        render->animationState.clear();
                    }
                    if (ObjectPhysicsComponent* physics =
                            ecs::try_get<ObjectPhysicsComponent>(
                                m_world.m_registry, *spawned.entity)) {
                        physics->mass = nugget.mass;
                        physics->forwardFrictionPerSecond =
                            common.extraFrictionPerSecond;
                        physics->lateralFrictionPerSecond =
                            common.extraFrictionPerSecond;
                        physics->allowBouncing =
                            game::objectCreationDispositionAllowsBouncing(
                                common.disposition);
                    }
                    DebrisDrawPresentationComponent debrisPresentation{
                        .finalFx = nugget.finalFx,
                        .bounceSound =
                            game::objectCreationDispositionAllowsBouncing(
                                common.disposition)
                            ? nugget.bounceSound
                            : container::String{},
                        .spawnedTick = item.ocl.confirmedTick,
                        .shadowTypeMask = nugget.shadowTypeMask,
                        .minimumLod = static_cast<uint8_t>(
                            nugget.minimumLod),
                        .okToChangeModelColor =
                            nugget.okToChangeModelColor,
                    };
                    if (randomSample.hasAnimationSet) {
                        const game::ObjectCreationDebrisAnimationSet&
                            animation = nugget.animationSets[
                                randomSample.animationSetIndex];
                        debrisPresentation.initialAnimation =
                            animation.initial;
                        debrisPresentation.flyingAnimation =
                            animation.flying;
                        debrisPresentation.finalStop =
                            equalAsciiInsensitive(
                                animation.final, "STOP");
                        debrisPresentation.finalAnimation =
                            debrisPresentation.finalStop
                                ? animation.flying : animation.final;
                    }
                    ecs::emplace<DebrisDrawPresentationComponent>(
                        m_world.m_registry, *spawned.entity,
                        std::move(debrisPresentation));
                }

                const uint32_t lifetimeFrames =
                    randomSample.lifetimeFrames;
                if (lifetimeFrames > 0) {
                    static_cast<void>(m_world.m_objectSimulation.rescheduleLifetime(
                        m_world.m_registry, m_world.m_objects, {
                            .object = spawned.object,
                            .action = game::ObjectLifetimeAction::Kill,
                            .minimumLifetimeFrames = lifetimeFrames,
                            .maximumLifetimeFrames = lifetimeFrames,
                            .confirmedTick = item.ocl.confirmedTick,
                        }));
                }
                if (hasDisposition(common.disposition,
                        game::ObjectCreationDisposition::Floating)) {
                    static_cast<void>(m_world.m_objectSimulation.setFloatEnabled(
                        m_world.m_registry, m_world.m_objects, {
                            .object = spawned.object,
                            .enabled = true,
                        }));
                }
                if constexpr (std::is_same_v<Nugget,
                              game::ObjectCreationCreateObjectNugget>) {
                    applyCreateObjectInheritance(spawned.object);
                }

                if (hasDisposition(common.disposition,
                        game::ObjectCreationDisposition::InheritVelocity)) {
                    queueOclPhysics(item, spawned.object,
                                 ObjectPhysicsRequestKind::AddVelocity,
                                 item.ocl.sourceVelocity,
                                 nugget.authoredOrder);
                }
                ObjectPhysicsComponent* dispositionPhysics =
                    ecs::try_get<ObjectPhysicsComponent>(
                        m_world.m_registry, *spawned.entity);
                const auto applyDispositionForce = [&] (
                        const LogicFixedVec3& force) {
                    if (common.orientInForceDirection) {
                        const ObjectPhysicsComponent::Scalar forceYaw =
                            math::fixed_atan2(force.y, force.x);
                        orientationFixed = forceYaw;
                        orientation = forceYaw.to_float();
                        writeAuthoritativeObjectYaw(
                            m_world.m_registry,
                            *spawned.entity, forceYaw);
                        dispositionPhysics->pitch = {};
                        dispositionPhysics->roll = {};
                    }
                    queueOclPhysics(item, spawned.object,
                                 ObjectPhysicsRequestKind::ApplyForce,
                                 force, nugget.authoredOrder);
                };

                if (dispositionPhysics &&
                    randomSample.hasSendOutForce) {
                    dispositionPhysics->forwardFrictionPerSecond =
                        common.extraFrictionPerSecond;
                    dispositionPhysics->lateralFrictionPerSecond =
                        common.extraFrictionPerSecond;
                    const LogicFixedVec3 sendOutForce{
                        randomSample.sendOutForce.x,
                        randomSample.sendOutForce.y,
                        randomSample.sendOutForce.z};
                    applyDispositionForce(sendOutForce);
                }

                if (dispositionPhysics &&
                    randomSample.hasFlightForce) {
                    dispositionPhysics->forwardFrictionPerSecond =
                        common.extraFrictionPerSecond;
                    dispositionPhysics->lateralFrictionPerSecond =
                        common.extraFrictionPerSecond;
                    dispositionPhysics->allowBouncing = true;
                    const ObjectPhysicsComponent::Scalar yawRate =
                        randomSample.flightYawRate;
                    const ObjectPhysicsComponent::Scalar rollRate =
                        randomSample.flightRollRate;
                    const ObjectPhysicsComponent::Scalar pitchRate =
                        randomSample.flightPitchRate;
                    const LogicFixedVec3 flightForce{
                        randomSample.flightForce.x,
                        randomSample.flightForce.y,
                        randomSample.flightForce.z};
                    applyDispositionForce(flightForce);
                    const ObjectPhysicsComponent::Scalar finalYaw =
                        orientationFixed;
                    writeAuthoritativeObjectYaw(
                        m_world.m_registry,
                        *spawned.entity, finalYaw);
                    dispositionPhysics->pitch = {};
                    dispositionPhysics->roll = {};
                    queueOclPhysics(item, spawned.object,
                                 ObjectPhysicsRequestKind::SetAngularRates,
                                 {}, nugget.authoredOrder,
                                 yawRate, pitchRate, rollRate);
                }

                if (dispositionPhysics &&
                    randomSample.hasWhirlingRates) {
                    // RefCode's later independent block overwrites
                    // the flight rates with the frozen whirl sample.
                    const ObjectPhysicsComponent::Scalar yawRate =
                        randomSample.whirlingYawRate;
                    const ObjectPhysicsComponent::Scalar rollRate =
                        randomSample.whirlingRollRate;
                    const ObjectPhysicsComponent::Scalar pitchRate =
                        randomSample.whirlingPitchRate;
                    queueOclPhysics(item, spawned.object,
                                 ObjectPhysicsRequestKind::SetAngularRates,
                                 {}, nugget.authoredOrder,
                                 yawRate, pitchRate, rollRate);
                }
                if (!common.fadeSound.empty() &&
                    (common.fadeIn || common.fadeOut)) {
                    static_cast<void>(m_publication.emitAudioEvent({
                        .eventName = common.fadeSound,
                        .emitter = spawned.object,
                        .owner = item.ocl.source,
                        .position = position,
                    }));
                }
                if ((common.fadeIn || common.fadeOut) &&
                    spawned.entity) {
                    if (RenderModelComponent* render =
                            ecs::try_get<RenderModelComponent>(
                                m_world.m_registry, *spawned.entity)) {
                        const uint64_t frameProduct =
                            static_cast<uint64_t>(
                                common.fadeMilliseconds) *
                            static_cast<uint64_t>(std::max(
                                1, m_content.m_startInfo.gameSpeedFPS));
                        render->opacityFadeDurationFrames =
                            static_cast<uint32_t>(std::min<uint64_t>(
                                std::numeric_limits<uint32_t>::max(),
                                frameProduct / 1000u +
                                    (frameProduct % 1000u != 0
                                         ? 1u : 0u)));
                        render->opacityFadeStartTick =
                            item.ocl.confirmedTick;
                        // RefCode calls fadeIn first and fadeOut
                        // second, so FadeOut wins when malformed
                        // content enables both flags.
                        render->opacityFadeMode = common.fadeOut
                            ? ObjectOpacityFadeMode::Out
                            : ObjectOpacityFadeMode::In;
                        render->explicitOpacity = common.fadeOut
                            ? 1.0f : 0.0f;
                    }
                }
                if (common.diesOnBadLand) {
                    const ObjectTerrainLayerComponent* terrainLayer =
                        ecs::try_get<ObjectTerrainLayerComponent>(
                            m_world.m_registry, *spawned.entity);
                    const uint32_t pathfindLayer = terrainLayer
                        ? terrainLayer->pathfindLayer
                        : game::terrain::kGroundPathfindLayer;
                    const bool underwater =
                        m_content.m_terrain.isUnderwaterLegacyRaw(
                            positionFixed.x.raw(),
                            positionFixed.y.raw());
                    const std::optional<int64_t> waterHeight =
                        m_content.m_terrain.waterSurfaceHeightLegacyRawAt(
                            positionFixed.x.raw(),
                            positionFixed.y.raw());
                    const bool submergedOnGround =
                        underwater && waterHeight &&
                        pathfindLayer ==
                            game::terrain::kGroundPathfindLayer &&
                        positionFixed.z <=
                            math::q32_32::from_raw(*waterHeight) +
                                math::q32_32{int32_t{10}};

                    // Being horizontally inside a water polygon is
                    // not by itself bad land: an object may be on a
                    // traversable bridge layer above that water. The
                    // ground-water Body path is handled above; the
                    // static cell/layer admission below decides the
                    // remaining cliff/water/impassable cases.
                    bool badTerrain =
                        !m_content.m_terrain.map().isInsidePlayableRaw(
                            positionFixed.x.raw(),
                            positionFixed.y.raw());
                    if (!badTerrain && m_content.m_navigation.isInitialized()) {
                        navigation::NavigationLayerId navigationLayer;
                        const bool validLayer = navigation::
                            tryNavigationLayerFromTerrainPathfindLayer(
                                pathfindLayer, navigationLayer);
                        const navigation::NavigationGrid* grid =
                            validLayer
                            ? m_content.m_navigation.staticLayers().find(
                                  navigationLayer)
                            : nullptr;
                        const navigation::NavigationCellId cell = grid
                            ? grid->cellAt({
                                  positionFixed.x.raw(),
                                  positionFixed.y.raw(),
                                  positionFixed.z.raw()})
                            : navigation::InvalidNavigationCell;
                        const navigation::NavigationCellValue value =
                            grid && cell
                                ? grid->cell(cell)
                                : navigation::NavigationCellValue{};
                        badTerrain = !grid || !cell ||
                            value.passability != navigation::
                                NavigationPassability::Traversable ||
                            (value.movementMask &
                                 navigation::NavigationMovement::Ground) ==
                                0u;
                    }

                    if (submergedOnGround || badTerrain) {
                        ObjectDamageRequest badLandDamage{
                            .target = spawned.object,
                            .sourceSequence = nugget.authoredOrder,
                            .damageType = submergedOnGround
                                ? game::DamageType::WATER
                                : game::DamageType::UNRESISTABLE,
                            .deathType = submergedOnGround
                                ? game::DeathType::FLOODED
                                : game::DeathType::NORMAL,
                            .forceKill = true,
                            .confirmedTick = item.ocl.confirmedTick,
                        };
                        pushWork({
                            .kind = WorkKind::Damage,
                            .damage = std::move(badLandDamage),
                        });
                    }
                }
            }

            if (spawnedWrapper && spawnedWrapper.entity &&
                !m_world.m_objects.isPendingDestroy(spawnedWrapper.object)) {
                // RefCode calls common doStuffToObj on PutInContainer
                // after every payload. The wrapper is therefore a full
                // Generic OCL result, not merely a containment shell.
                if (wrapperRandomSample.lifetimeFrames > 0) {
                    static_cast<void>(
                        m_world.m_objectSimulation.rescheduleLifetime(
                            m_world.m_registry, m_world.m_objects, {
                                .object = spawnedWrapper.object,
                                .action =
                                    game::ObjectLifetimeAction::Kill,
                                .minimumLifetimeFrames =
                                    wrapperRandomSample.lifetimeFrames,
                                .maximumLifetimeFrames =
                                    wrapperRandomSample.lifetimeFrames,
                                .confirmedTick =
                                    item.ocl.confirmedTick,
                            }));
                }
                if (!common.particleSystem.empty()) {
                    const std::optional<game::FxInvocationAnchor>
                        attachedAnchor = session_fx::snapshotAnchor(
                            m_world.m_registry, m_world.m_objects,
                            spawnedWrapper.object);
                    static_cast<void>(m_publication.emitFxInvocationEvent({
                        .directParticle =
                            game::FxDirectParticleRequest{
                                .particleSystemName =
                                    common.particleSystem,
                                .emitterCount = 1,
                                .attachToObject = true,
                            },
                        .anchorKind = game::
                            FxInvocationAnchorKind::ObjectAttachment,
                        .primary = attachedAnchor.value_or(
                            session_fx::worldAnchor(
                                wrapperPosition,
                                spawnedWrapper.object)),
                    }));
                }
                if (common.ignorePrimaryObstacle && item.ocl.source) {
                    static_cast<void>(m_world.m_objectSimulation.
                        setPhysicsIgnoreCollisionWith(
                            m_world.m_registry, m_world.m_objects,
                            spawnedWrapper.object,
                            item.ocl.source));
                }

                ObjectPhysicsComponent* wrapperPhysics =
                    ecs::try_get<ObjectPhysicsComponent>(
                        m_world.m_registry, *spawnedWrapper.entity);
                const bool wrapperDispositionReplacesLikePose =
                    wrapperRandomSample.hasOnGroundOrientation ||
                    wrapperRandomSample.hasSendOutOrientation ||
                    wrapperRandomSample.hasFlightForce;
                if (!wrapperDispositionReplacesLikePose &&
                    hasDisposition(common.disposition,
                        game::ObjectCreationDisposition::LikeExisting)) {
                    writeAuthoritativeObjectYaw(
                        m_world.m_registry,
                        *spawnedWrapper.entity,
                        item.ocl.orientationRadians);
                    if (wrapperPhysics) {
                        if (item.ocl.sourceOwnsFullAttitude &&
                            !ecs::try_get<ObjectLocomotionComponent>(
                                m_world.m_registry,
                                *spawnedWrapper.entity)) {
                            wrapperPhysics->pitch =
                                item.ocl.pitchRadians;
                            wrapperPhysics->roll =
                                item.ocl.rollRadians;
                            wrapperPhysics->ownsAttitude = true;
                        }
                        if (item.ocl.sourceAirborne) {
                            wrapperPhysics->allowToFall = true;
                        }
                    }
                }

                if (common.invulnerableMilliseconds > 0) {
                    const uint64_t frameProduct =
                        static_cast<uint64_t>(
                            common.invulnerableMilliseconds) *
                        static_cast<uint64_t>(std::max(
                            1, m_content.m_startInfo.gameSpeedFPS));
                    const uint64_t durationTicks =
                        std::max<uint64_t>(
                            1u, frameProduct / 1000u +
                                (frameProduct % 1000u != 0
                                     ? 1u : 0u));
                    const uint64_t detectionEndTick =
                        item.ocl.confirmedTick >
                            std::numeric_limits<uint64_t>::max() -
                                durationTicks
                        ? std::numeric_limits<uint64_t>::max()
                        : item.ocl.confirmedTick + durationTicks;
                    if (ObjectUndetectedDefectorComponent* existing =
                            ecs::try_get<
                                ObjectUndetectedDefectorComponent>(
                                m_world.m_registry,
                                *spawnedWrapper.entity)) {
                        existing->detectionEndTick = detectionEndTick;
                    } else {
                        ecs::emplace<
                            ObjectUndetectedDefectorComponent>(
                            m_world.m_registry, *spawnedWrapper.entity,
                            ObjectUndetectedDefectorComponent{
                                .detectionEndTick =
                                    detectionEndTick});
                    }
                }

                if constexpr (std::is_same_v<
                                  Nugget,
                                  game::ObjectCreationCreateObjectNugget>) {
                    applyCreateObjectInheritance(
                        spawnedWrapper.object);
                }

                if (hasDisposition(common.disposition,
                        game::ObjectCreationDisposition::
                            InheritVelocity)) {
                    queueOclPhysics(item, 
                        spawnedWrapper.object,
                        ObjectPhysicsRequestKind::AddVelocity,
                        item.ocl.sourceVelocity,
                        nugget.authoredOrder);
                }
                const auto applyWrapperForce = [&] (
                        const ObjectOclCreateFixedVector& sampled) {
                    if (!wrapperPhysics) return;
                    const LogicFixedVec3 force{
                        sampled.x, sampled.y, sampled.z};
                    if (common.orientInForceDirection) {
                        const ObjectPhysicsComponent::Scalar forceYaw =
                            math::fixed_atan2(force.y, force.x);
                        wrapperOrientationFixed = forceYaw;
                        wrapperOrientation = forceYaw.to_float();
                        writeAuthoritativeObjectYaw(
                            m_world.m_registry,
                            *spawnedWrapper.entity, forceYaw);
                        wrapperPhysics->pitch = {};
                        wrapperPhysics->roll = {};
                    }
                    queueOclPhysics(item, 
                        spawnedWrapper.object,
                        ObjectPhysicsRequestKind::ApplyForce, force,
                        nugget.authoredOrder);
                };
                if (wrapperPhysics &&
                    wrapperRandomSample.hasSendOutForce) {
                    wrapperPhysics->forwardFrictionPerSecond =
                        common.extraFrictionPerSecond;
                    wrapperPhysics->lateralFrictionPerSecond =
                        common.extraFrictionPerSecond;
                    applyWrapperForce(
                        wrapperRandomSample.sendOutForce);
                }
                if (wrapperPhysics &&
                    wrapperRandomSample.hasFlightForce) {
                    wrapperPhysics->forwardFrictionPerSecond =
                        common.extraFrictionPerSecond;
                    wrapperPhysics->lateralFrictionPerSecond =
                        common.extraFrictionPerSecond;
                    wrapperPhysics->allowBouncing = true;
                    if constexpr (std::is_same_v<
                                      Nugget,
                                      game::ObjectCreationCreateDebrisNugget>) {
                        wrapperPhysics->mass = nugget.mass;
                    }
                    applyWrapperForce(
                        wrapperRandomSample.flightForce);
                    const ObjectPhysicsComponent::Scalar finalYaw =
                        wrapperOrientationFixed;
                    writeAuthoritativeObjectYaw(
                        m_world.m_registry,
                        *spawnedWrapper.entity, finalYaw);
                    wrapperPhysics->pitch = {};
                    wrapperPhysics->roll = {};
                    queueOclPhysics(item, 
                        spawnedWrapper.object,
                        ObjectPhysicsRequestKind::SetAngularRates,
                        {}, nugget.authoredOrder,
                        wrapperRandomSample.flightYawRate,
                        wrapperRandomSample.flightPitchRate,
                        wrapperRandomSample.flightRollRate);
                }
                if (wrapperPhysics &&
                    wrapperRandomSample.hasWhirlingRates) {
                    queueOclPhysics(item, 
                        spawnedWrapper.object,
                        ObjectPhysicsRequestKind::SetAngularRates,
                        {}, nugget.authoredOrder,
                        wrapperRandomSample.whirlingYawRate,
                        wrapperRandomSample.whirlingPitchRate,
                        wrapperRandomSample.whirlingRollRate);
                }
                if (hasDisposition(common.disposition,
                        game::ObjectCreationDisposition::Floating)) {
                    static_cast<void>(
                        m_world.m_objectSimulation.setFloatEnabled(
                            m_world.m_registry, m_world.m_objects, {
                                .object = spawnedWrapper.object,
                                .enabled = true,
                            }));
                }

                if constexpr (std::is_same_v<
                                  Nugget,
                                  game::ObjectCreationCreateObjectNugget>) {
                    // Preserve the authored containment rule for wrapper
                    // results as well; render attachment and combat aiming
                    // consume the same deterministic transform authority.
                    if (nugget.containInsideSourceObject &&
                        !m_world.m_objectSimulation.requestContainment(
                            m_world.m_registry, m_world.m_objects,
                            {
                                .kind = ObjectContainmentRequestKind::Attach,
                                .container = item.ocl.source,
                                .object = spawnedWrapper.object,
                                .confirmedTick = item.ocl.confirmedTick,
                                .force = true,
                            },
                            &m_content.m_players,
                            &m_content.m_contentSnapshot)) {
                        static_cast<void>(m_lifecycle.requestDestroyObject(
                            spawnedWrapper.object,
                            ObjectDestroyReason::System,
                            item.ocl.confirmedTick));
                    }
                }

                if (!common.fadeSound.empty() &&
                    (common.fadeIn || common.fadeOut)) {
                    static_cast<void>(m_publication.emitAudioEvent({
                        .eventName = common.fadeSound,
                        .emitter = spawnedWrapper.object,
                        .owner = item.ocl.source,
                        .position = wrapperPosition,
                    }));
                }
                if (common.fadeIn || common.fadeOut) {
                    if (RenderModelComponent* render =
                            ecs::try_get<RenderModelComponent>(
                                m_world.m_registry,
                                *spawnedWrapper.entity)) {
                        const uint64_t frameProduct =
                            static_cast<uint64_t>(
                                common.fadeMilliseconds) *
                            static_cast<uint64_t>(std::max(
                                1, m_content.m_startInfo.gameSpeedFPS));
                        render->opacityFadeDurationFrames =
                            static_cast<uint32_t>(std::min<uint64_t>(
                                std::numeric_limits<uint32_t>::max(),
                                frameProduct / 1000u +
                                    (frameProduct % 1000u != 0
                                         ? 1u : 0u)));
                        render->opacityFadeStartTick =
                            item.ocl.confirmedTick;
                        render->opacityFadeMode = common.fadeOut
                            ? ObjectOpacityFadeMode::Out
                            : ObjectOpacityFadeMode::In;
                        render->explicitOpacity = common.fadeOut
                            ? 1.0f : 0.0f;
                    }
                }

                if (common.diesOnBadLand &&
                    !m_world.m_objects.isPendingDestroy(
                        spawnedWrapper.object)) {
                    const ObjectTerrainLayerComponent* terrainLayer =
                        ecs::try_get<ObjectTerrainLayerComponent>(
                            m_world.m_registry, *spawnedWrapper.entity);
                    const uint32_t pathfindLayer = terrainLayer
                        ? terrainLayer->pathfindLayer
                        : game::terrain::kGroundPathfindLayer;
                    const bool underwater =
                        m_content.m_terrain.isUnderwaterLegacyRaw(
                            wrapperPositionFixed.x.raw(),
                            wrapperPositionFixed.y.raw());
                    const std::optional<int64_t> waterHeight =
                        m_content.m_terrain.waterSurfaceHeightLegacyRawAt(
                            wrapperPositionFixed.x.raw(),
                            wrapperPositionFixed.y.raw());
                    const bool submergedOnGround =
                        underwater && waterHeight &&
                        pathfindLayer ==
                            game::terrain::kGroundPathfindLayer &&
                        wrapperPositionFixed.z <=
                            math::q32_32::from_raw(*waterHeight) +
                                math::q32_32{int32_t{10}};
                    bool badTerrain =
                        !m_content.m_terrain.map().isInsidePlayableRaw(
                            wrapperPositionFixed.x.raw(),
                            wrapperPositionFixed.y.raw());
                    if (!badTerrain &&
                        m_content.m_navigation.isInitialized()) {
                        navigation::NavigationLayerId navigationLayer;
                        const bool validLayer = navigation::
                            tryNavigationLayerFromTerrainPathfindLayer(
                                pathfindLayer, navigationLayer);
                        const navigation::NavigationGrid* grid =
                            validLayer
                            ? m_content.m_navigation.staticLayers().find(
                                  navigationLayer)
                            : nullptr;
                        const navigation::NavigationCellId cell = grid
                            ? grid->cellAt({
                                  wrapperPositionFixed.x.raw(),
                                  wrapperPositionFixed.y.raw(),
                                  wrapperPositionFixed.z.raw()})
                            : navigation::InvalidNavigationCell;
                        const navigation::NavigationCellValue value =
                            grid && cell ? grid->cell(cell)
                                         : navigation::
                                               NavigationCellValue{};
                        badTerrain = !grid || !cell ||
                            value.passability != navigation::
                                NavigationPassability::Traversable ||
                            (value.movementMask & navigation::
                                 NavigationMovement::Ground) == 0u;
                    }
                    if (submergedOnGround || badTerrain) {
                        pushWork({
                            .kind = WorkKind::Damage,
                            .damage = {
                                .target = spawnedWrapper.object,
                                .sourceSequence =
                                    nugget.authoredOrder,
                                .damageType = submergedOnGround
                                    ? game::DamageType::WATER
                                    : game::DamageType::UNRESISTABLE,
                                .deathType = submergedOnGround
                                    ? game::DeathType::FLOODED
                                    : game::DeathType::NORMAL,
                                .forceKill = true,
                                .confirmedTick =
                                    item.ocl.confirmedTick,
                            },
                        });
                    }
                }
            }
            if (payloadTailTruncated) {
                m_createdOclObjects = kMaximumOclCreatedObjects + 1u;
            }
            return;
        }
    }, sourceNugget);
}

} // namespace engine::detail
