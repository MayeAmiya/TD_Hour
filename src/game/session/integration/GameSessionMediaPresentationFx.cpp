#include "game/session/integration/GameSessionMediaPresentationPort.h"

#include "game/fx/runtime/GameFxEvents.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/render/ObjectPresentationPose.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/presentation/GameSessionPresentationDetail.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "presentation/render/SupportDrawPresentation.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace engine {
namespace {

struct FxGroundHeightCache final {
    const GameSessionContentStartState* content = nullptr;
    uint64_t presentationEpoch = 0;
    uint64_t terrainRevision = 0;
    container::SharedPtr<const fx::FxGroundHeightFieldSnapshot> snapshot;
};

FxGroundHeightCache& groundHeightCache() {
    // Extraction is owned by the confirmed logic thread. A thread-local
    // single-session cache avoids process-global lifetime coupling while a
    // map/session replacement naturally invalidates the identity tuple.
    thread_local FxGroundHeightCache cache;
    return cache;
}

[[nodiscard]] fx::FxPresentationAnchor presentationAnchor(
    const game::FxInvocationAnchor& source,
    presentation::PlayerAudience audience = {}) noexcept {
    return {
        .objectKey = source.presentationObjectKey != 0
            ? source.presentationObjectKey
            : source.object ? source.object.value : 0,
        .audience = audience,
        .position = {source.position.x(), source.position.y(), source.position.z()},
        .rollRadians = source.rollRadians,
        .pitchRadians = source.pitchRadians,
        .yawRadians = source.yawRadians,
        .objectBoundingCircleRadius = source.objectBoundingCircleRadius,
    };
}

struct LocalFxVisibility final {
    container::SharedPtr<const game::terrain::MapVisibilitySnapshot> map;
    PlayerId observer = INVALID_PLAYER_ID;
    bool enabled = false;
    bool observerGridValid = false;
    math::vec3 playableMinimum{};
    math::vec3 playableMaximum{};
    bool playableBoundsEnabled = false;
};

[[nodiscard]] bool insidePlayableBounds(
    const LocalFxVisibility& visibility, float x, float y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    return !visibility.playableBoundsEnabled ||
        (x >= visibility.playableMinimum.x() &&
         y >= visibility.playableMinimum.y() &&
         x <= visibility.playableMaximum.x() &&
         y <= visibility.playableMaximum.y());
}

[[nodiscard]] LocalFxVisibility localFxVisibility(
    container::SharedPtr<const game::terrain::MapVisibilitySnapshot> map,
    const PlayerState* observer) {
    LocalFxVisibility result;
    result.map = std::move(map);
    result.enabled = result.map && result.map->renderingActive && observer &&
        observer->isSimulationParticipant() && result.map->revision != 0 &&
        result.map->width > 0 && result.map->height > 0;
    if (result.enabled) {
        result.observer = observer->id;
        const game::terrain::MapVisibilityPlayerSnapshot* observerGrid =
            result.map->player(observer->id);
        const size_t cellCount = static_cast<size_t>(result.map->width) *
                                 static_cast<size_t>(result.map->height);
        result.observerGridValid = observerGrid && observerGrid->cells &&
            observerGrid->cells->size() == cellCount;
    }
    return result;
}

[[nodiscard]] game::terrain::MapVisibilityCellState positionVisibility(
    const PlayerList& players, const LocalFxVisibility& visibility,
    float x, float y) noexcept {
    if (!insidePlayableBounds(visibility, x, y)) {
        return game::terrain::MapVisibilityCellState::Shrouded;
    }
    if (!visibility.enabled) {
        return game::terrain::MapVisibilityCellState::Clear;
    }
    if (!visibility.observerGridValid) {
        return game::terrain::MapVisibilityCellState::Shrouded;
    }
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(visibility.map->originX) ||
        !std::isfinite(visibility.map->originY) ||
        !std::isfinite(visibility.map->cellWorldSize) ||
        visibility.map->cellWorldSize <= 0.0f) {
        return game::terrain::MapVisibilityCellState::Shrouded;
    }
    const double cellSize = static_cast<double>(visibility.map->cellWorldSize);
    // The visibility grid is coarser than the terrain heightfield. Its origin
    // is still derived from the heightfield's 10-unit border cells, not from
    // the visibility grid's 40-unit cells. Consume the sealed snapshot origin
    // directly; recomputing `-borderSize * cellWorldSize` shifts every lookup
    // and suppresses otherwise visible world-position FX.
    const double cellX =
        (static_cast<double>(x) -
         static_cast<double>(visibility.map->originX)) / cellSize;
    const double cellY =
        (static_cast<double>(y) -
         static_cast<double>(visibility.map->originY)) / cellSize;
    if (cellX < 0.0 || cellY < 0.0 ||
        cellX >= static_cast<double>(visibility.map->width) ||
        cellY >= static_cast<double>(visibility.map->height)) {
        return game::terrain::MapVisibilityCellState::Shrouded;
    }
    const int32_t xIndex = static_cast<int32_t>(std::floor(cellX));
    const int32_t yIndex = static_cast<int32_t>(std::floor(cellY));
    game::terrain::MapVisibilityCellState result =
        game::terrain::MapVisibilityCellState::Shrouded;
    for (const PlayerId player : players.activePlayerIds()) {
        if (players.relationship(visibility.observer, player) !=
            PlayerRelationship::Allies) {
            continue;
        }
        result = std::max(
            result, visibility.map->cellState(player, xIndex, yIndex));
    }
    return result;
}

[[nodiscard]] bool positionVisible(const PlayerList& players,
                                   const LocalFxVisibility& visibility,
                                   float x, float y) noexcept {
    return positionVisibility(players, visibility, x, y) ==
        game::terrain::MapVisibilityCellState::Clear;
}

[[nodiscard]] bool objectOrPositionVisible(
                                           const PlayerList& players,
                                           const ObjectLifecycle& objects,
                                           const ecs::registry& registry,
                                           const LocalFxVisibility& visibility,
                                           ObjectId object, float x, float y) noexcept {
    if (object) {
        const std::optional<ecs::entity> entity = objects.entityFromId(object);
        const TransformComponent* transform = entity
            ? ecs::try_get<TransformComponent>(registry, *entity)
            : nullptr;
        if (transform) {
            const ObjectPresentationPose pose = projectObjectPresentationPose(
                registry, *entity, *transform);
            x = pose.position.x();
            y = pose.position.y();
        }
        if (!insidePlayableBounds(visibility, x, y)) return false;
        if (!visibility.enabled) return true;
        const OwnerComponent* owner = entity
            ? ecs::try_get<OwnerComponent>(registry, *entity) : nullptr;
        if (owner && players.relationship(visibility.observer, owner->player) ==
                         PlayerRelationship::Allies) {
            return true;
        }
        // Attached effects can remain queued while their object is hidden.
        // Visibility on a retry must follow the live object transform rather
        // than the fallback position captured when the event was emitted.
        // FxRuntime resolves the same attachment identity at presentation
        // time, so sampling the stale fallback here could lose an exhaust
        // that moved into view during its bounded retry window.
        // FXList::doFXObj accepts both clear and partially-clear object
        // shroud states; only fully shrouded objects suppress the whole list.
        // Keep object-originated one-shots on that contract even after their
        // entity has retired and only the frozen position remains. Position-
        // only doFXPos invocations below still require a fully clear cell.
        return positionVisibility(players, visibility, x, y) !=
            game::terrain::MapVisibilityCellState::Shrouded;
    }
    return positionVisible(players, visibility, x, y);
}

[[nodiscard]] bool beaconVisibleToLocalObserver(
    const PlayerList& players, const ObjectLifecycle& objects,
    const ecs::registry& registry, ObjectId object) noexcept {
    const PlayerState* observer = players.localPlayer();
    const bool hasObserver = observer && !observer->isNeutral();
    const bool isSpectator = observer &&
        (observer->participation == PlayerParticipationKind::Observer ||
         observer->controller == PlayerControllerKind::Observer);
    const std::optional<ecs::entity> entity = object
        ? objects.entityFromId(object) : std::nullopt;
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(registry, *entity) : nullptr;
    const RenderModelComponent* visual = entity
        ? ecs::try_get<RenderModelComponent>(registry, *entity)
        : nullptr;
    const bool allied = observer && observer->isSimulationParticipant() && owner &&
        players.relationship(observer->id, owner->player) ==
            PlayerRelationship::Allies;
    return render::beaconVisibleToObserver(
        hasObserver, allied || isSpectator,
        !entity || !visual || visual->hidden);
}

[[nodiscard]] uint64_t vehicleEmitterIdentity(
    ObjectId object, uint32_t channelIndex, uint32_t emitterOrdinal) noexcept {
    const uint64_t key = (static_cast<uint64_t>(object.value) << 32u) |
        (static_cast<uint64_t>(channelIndex) << 8u) |
        static_cast<uint64_t>(emitterOrdinal + 1u);
    return key != 0 ? key : 1u;
}

[[nodiscard]] uint64_t rotorWashEmitterIdentity(
    ObjectId object, uint32_t moduleIndex) noexcept {
    // Keep this detached emitter namespace disjoint from VehicleDraw's
    // channel/ordinal packing while retaining stable per-object identity.
    const uint64_t key = (static_cast<uint64_t>(object.value) << 32u) |
        0x52570000ull | static_cast<uint64_t>((moduleIndex + 1u) & 0xffffu);
    return key != 0 ? key : 1u;
}

[[nodiscard]] uint64_t mixRotorWashSample(uint64_t value) noexcept {
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

} // namespace

fx::FxPresentationSnapshot
GameSessionMediaPresentationPort::takeFx(uint64_t simulationFrame) {
    GameSessionContentStartState& content = *m_content;
    GameSessionWorldState& world = *m_world;
    GameSessionScriptPresentationState& presentation = *m_presentation;
    const PlayerList& players = content.m_players;
    const PlayerState* audienceListener = players.localPlayer();
    const ObjectLifecycle& objectLifecycle = world.m_objects;
    const ecs::registry& registry = world.m_registry;
    fx::FxPresentationSnapshot snapshot;
    snapshot.sessionEpoch = presentation.m_fxInvocations.presentationEpoch();
    snapshot.simulationFrame = simulationFrame;
    snapshot.logicFramesPerSecond = static_cast<uint32_t>(
        std::max(1, content.m_startInfo.gameSpeedFPS));
    snapshot.legacyBeamTemplates =
        content.m_contentSnapshot.legacyBeamTemplates();
    LocalFxVisibility visibility = localFxVisibility(
        world.m_mapVisibility.snapshot(), players.localPlayer());
    if (content.m_terrain.isLoaded()) {
        // Border-shroud alpha is a terrain presentation switch. It never
        // grants FX outside the active gameplay partition render authority.
        const game::terrain::TerrainExtent extent =
            content.m_terrain.map().playableExtent();
        visibility.playableMinimum = extent.minimum;
        visibility.playableMaximum = extent.maximum;
        visibility.playableBoundsEnabled = true;
    }

    const auto objects = ecs::view<const ObjectIdentityComponent, const TransformComponent>(
        registry);
    snapshot.objects.reserve(objects.size_hint());
    for (const ecs::entity entity : objects) {
        const ObjectIdentityComponent& identity =
            objects.template get<const ObjectIdentityComponent>(entity);
        const TransformComponent& transform =
            objects.template get<const TransformComponent>(entity);
        if (!identity.id) continue;
        const ObjectPresentationPose pose = projectObjectPresentationPose(
            registry, entity, transform);
        if (!objectOrPositionVisible(
                players, objectLifecycle, registry, visibility, identity.id,
                pose.position.x(), pose.position.y())) {
            ++snapshot.visibilityRejectedObjects;
            continue;
        }
        fx::FxPresentationAnchor anchor{
            .objectKey = identity.id.value,
            .position = {
                pose.position.x(), pose.position.y(), pose.position.z()},
            .rollRadians = pose.rollRadians,
            .pitchRadians = pose.pitchRadians,
            .yawRadians = pose.yawRadians,
        };
        snapshot.objects.push_back(anchor);
    }
    std::sort(snapshot.objects.begin(), snapshot.objects.end(),
              [](const fx::FxPresentationAnchor& left,
                 const fx::FxPresentationAnchor& right) {
                  return left.objectKey < right.objectKey;
              });

    // Vehicle Draw emitters are a complete declared set. Draw owns these
    // systems for its lifetime; motion, hidden state and shroud only start or
    // stop emission and must not recreate the handle/burst phase.
    const auto vehicleObjects = ecs::view<
        const ObjectIdentityComponent, const TransformComponent,
        const ThingTemplateComponent,
        const VehicleDrawPresentationComponent>(registry);
    for (const ecs::entity entity : vehicleObjects) {
        const ObjectIdentityComponent& identity = vehicleObjects.template get<
            const ObjectIdentityComponent>(entity);
        const TransformComponent& transform = vehicleObjects.template get<
            const TransformComponent>(entity);
        const ThingTemplateComponent& source = vehicleObjects.template get<
            const ThingTemplateComponent>(entity);
        const VehicleDrawPresentationComponent& vehicle =
            vehicleObjects.template get<
                const VehicleDrawPresentationComponent>(entity);
        const ObjectPresentationPose pose = projectObjectPresentationPose(
            registry, entity, transform);
        if (!identity.id || !source.archetype) {
            continue;
        }
        const bool locallyVisible = objectOrPositionVisible(
            players, objectLifecycle, registry, visibility, identity.id,
            pose.position.x(), pose.position.y());
        fx::FxPresentationAnchor anchor{
            .objectKey = identity.id.value,
            .position = {
                pose.position.x(), pose.position.y(), pose.position.z()},
            .rollRadians = pose.rollRadians,
            .pitchRadians = pose.pitchRadians,
            .yawRadians = pose.yawRadians,
        };
        const game::ThingTemplate& type = source.archetype->templateData;
        const auto append = [&snapshot, &identity, &anchor, locallyVisible](
                uint32_t channelIndex, uint32_t ordinal,
                const container::String& particleSystem,
                bool active, fx::ParticleVector3 velocityMultiplier,
                float burstMultiplier, float sizeMultiplier,
                uint64_t triggerSequence) {
            if (particleSystem.empty()) return;
            snapshot.vehicleEmitters.push_back({
                .emitterKey = vehicleEmitterIdentity(
                    identity.id, channelIndex, ordinal),
                .objectKey = identity.id.value,
                .particleSystem = particleSystem,
                .anchor = anchor,
                .velocityMultiplier = velocityMultiplier,
                .burstCountMultiplier = burstMultiplier,
                .sizeMultiplier = sizeMultiplier,
                .triggerSequence = triggerSequence,
                .active = active && locallyVisible,
            });
        };
        for (const VehicleDrawChannelPresentationState& channelState :
             vehicle.channels) {
            if (channelState.channelIndex >=
                type.drawVisualChannels.size()) continue;
            const game::VehicleDrawVisualRecipe& recipe =
                type.drawVisualChannels[channelState.channelIndex].vehicleDraw;
            append(channelState.channelIndex, 0u,
                   recipe.dustParticleSystem, channelState.dustActive,
                   {1.0f, 1.0f, 1.0f}, 1.0f,
                   channelState.dustSizeMultiplier,
                   channelState.landingTriggerSequence == simulationFrame
                       ? channelState.landingTriggerSequence : 0);
            append(channelState.channelIndex, 1u,
                   recipe.dirtParticleSystem, channelState.dirtActive,
                   {1.0f, 1.0f, 1.0f}, 1.0f, 1.0f, 0);
            append(channelState.channelIndex, 2u,
                   recipe.powerslideParticleSystem,
                   channelState.powerslideActive,
                   {1.0f, 1.0f, 1.0f}, 1.0f, 1.0f, 0);
            const fx::ParticleVector3 debrisVelocity{
                channelState.debrisVelocityXyMultiplier,
                channelState.debrisVelocityXyMultiplier,
                channelState.debrisVelocityZMultiplier,
            };
            append(channelState.channelIndex, 3u,
                   recipe.treadDebrisLeft,
                   channelState.treadDebrisActive, debrisVelocity,
                   channelState.debrisBurstMultiplier, 1.0f, 0);
            append(channelState.channelIndex, 4u,
                   recipe.treadDebrisRight,
                   channelState.treadDebrisActive, debrisVelocity,
                   channelState.debrisBurstMultiplier, 1.0f, 0);
        }
    }

    // ChinookAIUpdate creates rotor wash only while locally visible and in a
    // landed/landing/taking-off phase. This runtime does not expose a second
    // renderer-facing flight-state object, so the equivalent immutable facts
    // are: grounded, or an airborne Chinook with confirmed vertical motion.
    // The retail client probability random(0,elevation) < 5 is reproduced by
    // a stable presentation-only sample; it never enters gameplay RNG/state.
    const auto rotorWashObjects = ecs::view<
        const ObjectIdentityComponent, const TransformComponent,
        const ObjectAirfieldComponent>(registry);
    for (const ecs::entity entity : rotorWashObjects) {
        const ObjectIdentityComponent& identity =
            rotorWashObjects.template get<const ObjectIdentityComponent>(
                entity);
        const TransformComponent& transform =
            rotorWashObjects.template get<const TransformComponent>(entity);
        const ObjectAirfieldComponent& airfield =
            rotorWashObjects.template get<const ObjectAirfieldComponent>(
                entity);
        if (!identity.id || !airfield.plan ||
            !positionVisible(
                players, visibility, transform.x, transform.y)) {
            continue;
        }
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, entity);
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry, entity);
        const bool verticallyMoving = locomotion &&
            locomotion->verticalSpeed != math::q32_32{};
        const bool landedOrTransition =
            !(airborne && airborne->isAirborne) || verticallyMoving;
        if (!landedOrTransition) continue;

        const ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry, entity);
        const math::q32_32 x = fixedTransform
            ? fixedTransform->position.x : math::q32_32{transform.x};
        const math::q32_32 y = fixedTransform
            ? fixedTransform->position.y : math::q32_32{transform.y};
        const math::q32_32 z = fixedTransform
            ? fixedTransform->position.z : math::q32_32{transform.z};
        const math::q32_32 ground = content.m_terrain.isLoaded()
            ? math::q32_32::from_raw(content.m_terrain.groundHeightRaw(
                  x.raw(), y.raw()))
            : z;
        const math::q32_32 washZ = ground + math::q32_32{int32_t{3}};
        const math::q32_32 elevation = math::q32_32::max(
            math::q32_32{}, z - washZ);
        const size_t count = std::min(
            airfield.chinookAi.size(), airfield.plan->chinookAi.size());
        for (size_t moduleIndex = 0; moduleIndex < count; ++moduleIndex) {
            const game::ObjectChinookAiRule& rule =
                airfield.plan->chinookAi[moduleIndex];
            if (rule.rotorWashParticleSystem.empty()) continue;
            const uint64_t sampleBits = mixRotorWashSample(
                (static_cast<uint64_t>(identity.id.value) << 32u) ^
                (static_cast<uint64_t>(moduleIndex) << 24u) ^
                simulationFrame);
            const math::q32_32 unitSample = math::q32_32::from_raw(
                static_cast<int64_t>(sampleBits & 0xffffffffull));
            if (unitSample * elevation >=
                math::q32_32{int32_t{5}}) {
                continue;
            }
            snapshot.vehicleEmitters.push_back({
                .emitterKey = rotorWashEmitterIdentity(
                    identity.id, static_cast<uint32_t>(moduleIndex)),
                .objectKey = identity.id.value,
                .particleSystem = rule.rotorWashParticleSystem,
                .anchor = {
                    .objectKey = identity.id.value,
                    .position = {
                        x.to_float(), y.to_float(), washZ.to_float()},
                    .yawRadians = transform.rotation,
                },
                .triggerSequence = simulationFrame,
            });
        }
    }
    std::sort(snapshot.vehicleEmitters.begin(), snapshot.vehicleEmitters.end(),
        [](const fx::FxVehicleParticleEmitterPose& left,
           const fx::FxVehicleParticleEmitterPose& right) {
            return left.emitterKey < right.emitterKey;
        });

    container::Vector<game::FxInvocationEvent> events =
        presentation.m_fxInvocations.take();
    if (!events.empty() && content.m_terrain.isLoaded()) {
        const game::terrain::TerrainHeightfieldData& source =
            content.m_terrain.map().heightfield();
        if (source.isValid()) {
            FxGroundHeightCache& cache = groundHeightCache();
            const uint64_t epoch = presentation.m_scriptPresentationEpoch;
            const uint64_t revision = content.m_terrain.map().revision();
            if (cache.content != &content ||
                cache.presentationEpoch != epoch ||
                cache.terrainRevision != revision || !cache.snapshot) {
                auto ground = std::make_shared<fx::FxGroundHeightFieldSnapshot>();
                ground->width = source.width;
                ground->height = source.height;
                ground->borderSize = source.borderSize;
                ground->cellWorldSize = game::terrain::kMapCellWorldSize;
                ground->heightWorldScale = game::terrain::kMapHeightWorldScale;
                ground->heights = source.heights;
                cache = {
                    .content = &content,
                    .presentationEpoch = epoch,
                    .terrainRevision = revision,
                    .snapshot = std::move(ground),
                };
            }
            snapshot.groundHeights = cache.snapshot;
        }
    }
    snapshot.invocations.reserve(events.size());
    for (game::FxInvocationEvent& source : events) {
        const bool beaconSmoke = source.directParticle &&
            source.attachmentGroup != 0 &&
            source.directParticle->particleSystemName.starts_with(
                "BeaconSmoke");
        if (beaconSmoke &&
            !beaconVisibleToLocalObserver(
                players, objectLifecycle, registry, source.primary.object)) {
            // hideBeacon is local-client policy in RefCode. Reuse the same
            // stable group identity so an allied smoke system already alive
            // before a diplomacy/owner change is stopped instead of merely
            // suppressing this newest Show edge.
            source.control =
                game::FxInvocationControlKind::StopAttachedParticleGroup;
            source.directParticle.reset();
        }
        const bool lifecycleControl =
            source.control ==
                game::FxInvocationControlKind::StopAttachedParticleGroup ||
            source.control ==
                game::FxInvocationControlKind::StopAllAttachedParticles ||
            (source.directBeam && source.directBeam->control ==
                game::FxDirectBeamControl::End) ||
            (source.directRope && source.directRope->control ==
                game::FxDirectRopeControl::End);
        const bool primaryVisible = lifecycleControl ||
            (source.anchorKind != game::FxInvocationAnchorKind::WorldPosition
                ? objectOrPositionVisible(
                      players, objectLifecycle, registry, visibility,
                      source.primary.object,
                      source.primary.position.x(), source.primary.position.y())
                : positionVisible(
                      players, visibility, source.primary.position.x(),
                      source.primary.position.y()));
        // FXList::doFXObj in ZH gates the whole invocation only by the
        // primary (effect receiver). The optional secondary is merely the
        // dealer/second endpoint supplied to individual nuggets; an unseen
        // attacker must not suppress a visible victim's DamageFX. Typed
        // beams/ropes are the two modern endpoint-owned families and retain
        // their explicit two-endpoint visibility policy.
        const bool secondaryOwnsVisibility =
            source.directBeam.has_value() || source.directRope.has_value();
        const bool secondaryVisible = lifecycleControl || !source.secondary ||
            !secondaryOwnsVisibility ||
            objectOrPositionVisible(
                                    players, objectLifecycle, registry,
                                    visibility, source.secondary->object,
                                    source.secondary->position.x(),
                                    source.secondary->position.y());
        if (!primaryVisible || !secondaryVisible) {
            ++snapshot.visibilityRejectedInvocations;
            const bool missingAttachedObject = source.directParticle &&
                source.directParticle->attachToObject &&
                (!source.primary.object ||
                 !objectLifecycle.entityFromId(source.primary.object));
            const bool visibilitySnapshotPending = visibility.enabled &&
                !visibility.observerGridValid;
            if ((visibilitySnapshotPending ||
                 source.localVisibilityRetryFrames != 0) &&
                !missingAttachedObject) {
                // A visibility revision is published before its detached
                // observer column can become available during startup or an
                // observer switch. Fail closed for this frame, but retain the
                // one-shot FX losslessly until that column is sealed. The
                // authored retry budget is consumed only for an actual
                // hidden-cell rejection, not for an unavailable snapshot.
                if (!visibilitySnapshotPending)
                    --source.localVisibilityRetryFrames;
                source.streamSequence = 0;
                if (content.m_active) {
                    static_cast<void>(
                        presentation.m_fxInvocations.emit(
                            std::move(source)));
                }
            }
            continue;
        }
        if (source.directParticle &&
            source.directParticle->systemLifetimeFrames &&
            source.localVisibilityFirstFrame != 0 &&
            simulationFrame >= source.localVisibilityFirstFrame &&
            simulationFrame - source.localVisibilityFirstFrame >=
                *source.directParticle->systemLifetimeFrames) {
            // Nullopt retains the template lifetime. An expired explicit
            // override must be discarded instead of becoming nullopt/forever.
            continue;
        }
        fx::FxPresentationInvocation invocation;
        invocation.fxListName = std::move(source.fxListName);
        invocation.control =
            source.control ==
                game::FxInvocationControlKind::StopAttachedParticleGroup
                ? fx::FxPresentationControlKind::StopAttachedParticleGroup
            : source.control ==
                    game::FxInvocationControlKind::StopAllAttachedParticles
                ? fx::FxPresentationControlKind::StopAllAttachedParticles
                : fx::FxPresentationControlKind::Execute;
        if (source.directParticle) {
            std::optional<uint32_t> remainingLifetimeFrames =
                source.directParticle->systemLifetimeFrames;
            if (remainingLifetimeFrames &&
                source.localVisibilityFirstFrame != 0 &&
                simulationFrame > source.localVisibilityFirstFrame) {
                const uint64_t elapsed =
                    simulationFrame - source.localVisibilityFirstFrame;
                *remainingLifetimeFrames -= static_cast<uint32_t>(elapsed);
            }
            invocation.directParticle = fx::FxPresentationDirectParticle{
                .particleSystemName =
                    std::move(source.directParticle->particleSystemName),
                .fallbackParticleSystemName = std::move(
                    source.directParticle->fallbackParticleSystemName),
                .fallbackColorKeyTint = source.directParticle->fallbackColorKeyTint
                    ? std::optional<fx::ParticleVector3>{fx::ParticleVector3{
                          source.directParticle->fallbackColorKeyTint->x(),
                          source.directParticle->fallbackColorKeyTint->y(),
                          source.directParticle->fallbackColorKeyTint->z()}}
                    : std::nullopt,
                .emitterCount = source.directParticle->emitterCount,
                .systemLifetimeFrames =
                    remainingLifetimeFrames,
                .footprintMajorRadius =
                    source.directParticle->footprintMajorRadius,
                .footprintMinorRadius =
                    source.directParticle->footprintMinorRadius,
                .maximumHeight = source.directParticle->maximumHeight,
                .initialDelayMinimumFrames =
                    source.directParticle->initialDelayMinimumFrames,
                .initialDelayMaximumFrames =
                    source.directParticle->initialDelayMaximumFrames,
                .boxFootprint = source.directParticle->boxFootprint,
                .attachToObject = source.directParticle->attachToObject,
            };
        }
        if (source.directBeam) {
            const auto beamControl = [](game::FxDirectBeamControl value) {
                switch (value) {
                case game::FxDirectBeamControl::Update:
                    return fx::FxPresentationDirectBeam::Control::Update;
                case game::FxDirectBeamControl::End:
                    return fx::FxPresentationDirectBeam::Control::End;
                case game::FxDirectBeamControl::Begin:
                default:
                    return fx::FxPresentationDirectBeam::Control::Begin;
                }
            };
            invocation.directBeam = fx::FxPresentationDirectBeam{
                .objectTemplate =
                    std::move(source.directBeam->objectTemplate),
                .control = beamControl(source.directBeam->control),
                .beamIdentity = source.directBeam->beamIdentity,
                .sizeDeltaFrames = source.directBeam->sizeDeltaFrames,
                .decayFrames = source.directBeam->decayFrames,
            };
        }
        if (source.directScorch) {
            const auto scorchType = [](game::FxDirectScorchType value) {
                switch (value) {
                case game::FxDirectScorchType::Scorch1:
                    return fx::FxTerrainScorch::Scorch1;
                case game::FxDirectScorchType::Scorch2:
                    return fx::FxTerrainScorch::Scorch2;
                case game::FxDirectScorchType::Scorch3:
                    return fx::FxTerrainScorch::Scorch3;
                case game::FxDirectScorchType::Scorch4:
                    return fx::FxTerrainScorch::Scorch4;
                case game::FxDirectScorchType::Random:
                default:
                    return fx::FxTerrainScorch::Random;
                }
            };
            invocation.directScorch = fx::FxPresentationDirectScorch{
                .type = scorchType(source.directScorch->type),
                .radius = source.directScorch->radius,
            };
        }
        if (source.directRope) {
            const auto ropeControl = [](game::FxDirectRopeControl value) {
                switch (value) {
                case game::FxDirectRopeControl::Update:
                    return fx::FxPresentationRopeControl::Update;
                case game::FxDirectRopeControl::End:
                    return fx::FxPresentationRopeControl::End;
                case game::FxDirectRopeControl::Begin:
                default:
                    return fx::FxPresentationRopeControl::Begin;
                }
            };
            invocation.directRope = fx::FxPresentationDirectRope{
                .control = ropeControl(source.directRope->control),
                .ropeIdentity = source.directRope->ropeIdentity,
                .maximumLength = source.directRope->maximumLength,
                .currentLength = source.directRope->currentLength,
                .width = source.directRope->width,
                .color = {
                    source.directRope->color.x(),
                    source.directRope->color.y(),
                    source.directRope->color.z(),
                },
                .wobbleLength = source.directRope->wobbleLength,
                .wobbleAmplitude = source.directRope->wobbleAmplitude,
                .wobbleRatePerFrame = source.directRope->wobbleRatePerFrame,
                .wobblePhase = source.directRope->wobblePhase,
                .verticalOffset = source.directRope->verticalOffset,
                .currentSpeedPerFrame =
                    source.directRope->currentSpeedPerFrame,
                .maximumSpeedPerFrame =
                    source.directRope->maximumSpeedPerFrame,
                .accelerationPerFrame =
                    source.directRope->accelerationPerFrame,
            };
        }
        switch (source.anchorKind) {
        case game::FxInvocationAnchorKind::ObjectAttachment:
            invocation.anchorKind =
                fx::FxPresentationAnchorKind::ObjectAttachment;
            break;
        case game::FxInvocationAnchorKind::BonePosition:
            invocation.anchorKind = fx::FxPresentationAnchorKind::BonePosition;
            break;
        case game::FxInvocationAnchorKind::WorldPosition:
            invocation.anchorKind = fx::FxPresentationAnchorKind::WorldPosition;
            break;
        }
        const presentation::PlayerAudience audience = source.sourcePlayer
            ? game_session_presentation_detail::freezePlayerAudience(
                  players, audienceListener, *source.sourcePlayer)
            : presentation::PlayerAudience{};
        invocation.primary = presentationAnchor(source.primary, audience);
        if (source.secondary) {
            invocation.secondary = presentationAnchor(*source.secondary, audience);
        }
        invocation.attachmentBoneName = std::move(source.boneName);
        invocation.attachmentBoneNameIsPrefix = source.boneNameIsPrefix;
        invocation.attachmentBoneSequenceOrdinal =
            source.boneNameSequenceOrdinal;
        invocation.attachmentBonePrefixFallsBackToBare =
            source.boneNamePrefixFallsBackToBare;
        invocation.secondaryBoneName =
            std::move(source.secondaryBoneName);
        invocation.secondaryBoneNameIsPrefix =
            source.secondaryBoneNameIsPrefix;
        invocation.secondaryBoneSequenceOrdinal =
            source.secondaryBoneNameSequenceOrdinal;
        invocation.secondaryBonePrefixFallsBackToBare =
            source.secondaryBoneNamePrefixFallsBackToBare;
        invocation.secondaryWorldOffset = {
            source.secondaryWorldOffset.x(),
            source.secondaryWorldOffset.y(),
            source.secondaryWorldOffset.z(),
        };
        invocation.inheritResolvedAnchorOrientation =
            source.inheritResolvedAnchorOrientation;
        invocation.attachmentLocalOffset = {
            source.attachmentLocalOffset.x(),
            source.attachmentLocalOffset.y(),
            source.attachmentLocalOffset.z(),
        };
        invocation.attachmentGroup = source.attachmentGroup;
        invocation.primarySpeed = source.primarySpeed;
        invocation.overrideRadius = source.overrideRadius;
        const float groundHeight = content.m_terrain.groundHeight(
            source.primary.position.x(), source.primary.position.y());
        if (std::isfinite(groundHeight)) invocation.groundHeight = groundHeight;
        invocation.streamSequence = source.streamSequence;
        invocation.eventId = source.eventId;
        invocation.confirmedFrame = source.confirmedFrame;
        invocation.variationSeed = source.variationSeed;
        snapshot.invocations.push_back(std::move(invocation));
    }
    return snapshot;
}

} // namespace engine
