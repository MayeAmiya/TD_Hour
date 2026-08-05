#include "GameRenderExtraction.h"
#include "GameRenderExtractionDetail.h"
#include "GameRenderExtractionEntitySource.h"
#include "GameRenderExtractionTerrainSource.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/script/presentation/ScriptMapPresentationControls.h"
#include "game/session/integration/GameRenderExtractionCache.h"
#include "game/session/integration/GameRenderExtractionGarrisonPresentation.h"
#include "game/session/integration/GameRenderExtractionJetLockonPresentation.h"
#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace engine {
namespace {

[[nodiscard]] bool sameSelection(
    container::Span<const ObjectId> selection,
    const container::Vector<ObjectId>& cached) noexcept {
    return selection.size() == cached.size() &&
        std::equal(selection.begin(), selection.end(), cached.begin());
}

struct RenderExtractionWork final {
    container::Vector<ObjectId> currentObjects;
    container::Vector<ObjectId> refreshObjects;
    uint32_t hiddenEntities = 0;
    uint32_t hiddenProjectiles = 0;
};

[[nodiscard]] bool sameVisibilityPolicy(
    const render::LocalVisibilityRenderSnapshot& left,
    const render::LocalVisibilityRenderSnapshot& right) noexcept {
    return left.presentationEpoch == right.presentationEpoch &&
        left.policyRevision == right.policyRevision &&
        left.terrainLayoutRevision == right.terrainLayoutRevision &&
        left.observerPlayer == right.observerPlayer &&
        left.width == right.width && left.height == right.height &&
        left.borderSize == right.borderSize &&
        left.enabled == right.enabled;
}

[[nodiscard]] bool visibilityChangeCanPatch(
    const render::LocalVisibilityRenderSnapshot& cached,
    const render::LocalVisibilityRenderSnapshot& current) noexcept {
    if (!sameVisibilityPolicy(cached, current)) return false;
    if (cached.revision == current.revision) return true;
    return current.revision > cached.revision &&
        current.revision - cached.revision == 1u &&
        current.dirtyRegion.isValid();
}

[[nodiscard]] uint64_t observerPresentationPolicyIdentity(
    const PlayerList& players) noexcept {
    const PlayerState* observer = players.localPlayer();
    if (!observer) return 0;
    uint64_t value = 14695981039346656037ull;
    const auto mix = [&value](uint64_t part) noexcept {
        value ^= part;
        value *= 1099511628211ull;
    };
    mix(observer->id.value);
    mix(static_cast<uint8_t>(observer->participation));
    mix(static_cast<uint8_t>(observer->controller));
    mix(observer->revisions.diplomacy);
    mix(observer->revisions.energy);
    mix(observer->revisions.radar);
    return value == 0 ? 1 : value;
}

[[nodiscard]] bool hasDuePresentationBoundary(
    const ecs::registry& registry,
    ecs::entity entity,
    uint64_t cachedFrame,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond) {
    if (const ObjectDisabledComponent* disabled =
            ecs::try_get<ObjectDisabledComponent>(registry, entity)) {
        for (size_t reason = 0; reason < disabled->untilTicks.size(); ++reason) {
            const uint64_t until = disabled->untilTicks[reason];
            if (until != 0 && until != OBJECT_DISABLED_FOREVER_TICK &&
                cachedFrame < until && simulationFrame >= until) {
                return true;
            }
        }
    }
    return hasDueGarrisonPresentationBoundary(
               registry, entity, cachedFrame, simulationFrame,
               logicFramesPerSecond) ||
        hasActiveJetLockonPresentation(
               registry, entity, cachedFrame, simulationFrame);
}

[[nodiscard]] RenderExtractionWork collectRenderExtractionWork(
    const ecs::registry& registry,
    const PlayerList& players,
    const render::LocalVisibilityRenderSnapshot& visibility,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond,
    const render::WorldRenderSnapshot& cached) {
    RenderExtractionWork work;
    const auto objects = ecs::view<
        const ObjectIdentityComponent,
        const TransformComponent,
        const RenderModelComponent>(registry);
    work.currentObjects.reserve(objects.size_hint());
    work.refreshObjects.reserve(objects.size_hint() / 4u + 1u);
    const PlayerState* observer = players.localPlayer();
    const bool hasObserver = observer && observer->isSimulationParticipant();
    const bool visibilityChanged = visibility.enabled &&
        visibility.revision != cached.localVisibility.revision &&
        visibility.dirtyRegion.isValid();
    const float dirtyMinimumX = visibility.originX +
        static_cast<float>(visibility.dirtyRegion.minX) *
            visibility.cellWorldSize;
    const float dirtyMinimumY = visibility.originY +
        static_cast<float>(visibility.dirtyRegion.minY) *
            visibility.cellWorldSize;
    const float dirtyMaximumX = visibility.originX +
        (static_cast<float>(visibility.dirtyRegion.maxX) + 1.0f) *
            visibility.cellWorldSize;
    const float dirtyMaximumY = visibility.originY +
        (static_cast<float>(visibility.dirtyRegion.maxY) + 1.0f) *
            visibility.cellWorldSize;
    const auto cachedVisibilityChanged = [&cached](
            uint64_t objectId,
            render::LocalVisibilityRenderCellState state,
            bool hidden) {
        const auto found = std::lower_bound(
            cached.entities.begin(), cached.entities.end(), objectId,
            [](const render::RenderEntitySnapshot& value, uint64_t key) {
                const uint64_t valueObjectId = value.objectId != 0
                    ? value.objectId : value.id;
                return valueObjectId < key;
            });
        if (found == cached.entities.end()) return true;
        const uint64_t foundObjectId = found->objectId != 0
            ? found->objectId : found->id;
        return foundObjectId != objectId ||
            found->localVisibilityState != state ||
            found->hiddenByLocalVisibility != hidden;
    };
    for (const ecs::entity entity : objects) {
        const ObjectIdentityComponent& identity =
            objects.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id) continue;
        const TransformComponent& transform =
            objects.template get<const TransformComponent>(entity);
        const RenderModelComponent& visual =
            objects.template get<const RenderModelComponent>(entity);
        work.currentObjects.push_back(identity.id);

        const ObjectDirtyComponent* dirty =
            ecs::try_get<ObjectDirtyComponent>(registry, entity);
        const bool explicitlyDirty = dirty &&
            (dirty->domains & objectDirtyBit(
                ObjectDirtyDomain::RenderExtraction)) != 0;
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, entity);
        const float radius = geometry
            ? std::max(0.0f, geometry->boundingSphereRadiusFixed.to_float())
            : std::max(0.0f, visual.boundingRadius);
        const bool objectVisibilityDirty = visibilityChanged &&
            transform.x + radius >= dirtyMinimumX &&
            transform.x - radius <= dirtyMaximumX &&
            transform.y + radius >= dirtyMinimumY &&
            transform.y - radius <= dirtyMaximumY;
        const ObjectProjectileComponent* projectile =
            ecs::try_get<ObjectProjectileComponent>(registry, entity);
        const bool projectileAnchorVisibilityDirty = visibilityChanged &&
            projectile &&
            projectile->projectileStreamOwnerAnchorPosition.x.to_float() >=
                dirtyMinimumX &&
            projectile->projectileStreamOwnerAnchorPosition.x.to_float() <=
                dirtyMaximumX &&
            projectile->projectileStreamOwnerAnchorPosition.y.to_float() >=
                dirtyMinimumY &&
            projectile->projectileStreamOwnerAnchorPosition.y.to_float() <=
                dirtyMaximumY;
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, entity);
        const bool allied = hasObserver && owner &&
            players.relationship(observer->id, owner->player) ==
                PlayerRelationship::Allies;
        const render::LocalVisibilityRenderCellState visibilityState =
            visibility.enabled && !allied
                ? visibility.worldStateSphere(
                      {transform.x, transform.y, transform.z}, radius)
                : render::LocalVisibilityRenderCellState::Visible;
        const bool hidden = visibilityState !=
            render::LocalVisibilityRenderCellState::Visible;
        const bool visibilityStateChanged = visibilityChanged &&
            cachedVisibilityChanged(identity.id.value, visibilityState, hidden);
        if (explicitlyDirty || objectVisibilityDirty ||
            visibilityStateChanged || projectileAnchorVisibilityDirty ||
            hasDuePresentationBoundary(
                registry, entity, cached.simulationFrame, simulationFrame,
                logicFramesPerSecond)) {
            work.refreshObjects.push_back(identity.id);
        }
        if (hidden) {
            ++work.hiddenEntities;
            if (projectile && !projectile->detonated)
                ++work.hiddenProjectiles;
        }
    }
    std::sort(work.currentObjects.begin(), work.currentObjects.end());
    work.currentObjects.erase(std::unique(
        work.currentObjects.begin(), work.currentObjects.end()),
        work.currentObjects.end());
    std::sort(work.refreshObjects.begin(), work.refreshObjects.end());
    work.refreshObjects.erase(std::unique(
        work.refreshObjects.begin(), work.refreshObjects.end()),
        work.refreshObjects.end());
    container::Vector<ObjectId> launchRefresh;
    const auto launchDependents = ecs::view<
        const ObjectIdentityComponent,
        const ObjectProjectileComponent>(registry);
    for (const ecs::entity entity : launchDependents) {
        const ObjectIdentityComponent& identity = launchDependents
            .template get<const ObjectIdentityComponent>(entity);
        const ObjectProjectileComponent& projectile = launchDependents
            .template get<const ObjectProjectileComponent>(entity);
        if (!identity.id || projectile.detonated || !projectile.launcher ||
            simulationFrame < projectile.spawnedTick ||
            simulationFrame - projectile.spawnedTick > 1u) {
            continue;
        }
        if (std::binary_search(
                work.refreshObjects.begin(), work.refreshObjects.end(),
                projectile.launcher)) {
            launchRefresh.push_back(identity.id);
        }
    }
    work.refreshObjects.insert(
        work.refreshObjects.end(), launchRefresh.begin(), launchRefresh.end());
    std::sort(work.refreshObjects.begin(), work.refreshObjects.end());
    work.refreshObjects.erase(std::unique(
        work.refreshObjects.begin(), work.refreshObjects.end()),
        work.refreshObjects.end());
    container::Vector<ObjectId> aggregateRefresh;
    const auto aggregateParents = ecs::view<
        const ObjectIdentityComponent,
        const ObjectSpawnSlaveComponent>(registry);
    for (const ecs::entity entity : aggregateParents) {
        const ObjectIdentityComponent& identity = aggregateParents
            .template get<const ObjectIdentityComponent>(entity);
        const ObjectSpawnSlaveComponent& spawn = aggregateParents
            .template get<const ObjectSpawnSlaveComponent>(entity);
        if (!identity.id || !spawn.plan) continue;
        const size_t count = std::min(
            spawn.plan->spawns.size(), spawn.spawns.size());
        bool parentDirty = false;
        for (size_t index = 0; index < count && !parentDirty; ++index) {
            if (!spawn.plan->spawns[index].aggregateHealth) continue;
            for (const ObjectId child : spawn.spawns[index].children) {
                if (std::binary_search(
                        work.refreshObjects.begin(),
                        work.refreshObjects.end(), child) ||
                    !std::binary_search(
                        work.currentObjects.begin(),
                        work.currentObjects.end(), child)) {
                    parentDirty = true;
                    break;
                }
            }
        }
        if (parentDirty) aggregateRefresh.push_back(identity.id);
    }
    work.refreshObjects.insert(
        work.refreshObjects.end(),
        aggregateRefresh.begin(), aggregateRefresh.end());
    std::sort(work.refreshObjects.begin(), work.refreshObjects.end());
    work.refreshObjects.erase(std::unique(
        work.refreshObjects.begin(), work.refreshObjects.end()),
        work.refreshObjects.end());
    std::erase_if(work.refreshObjects, [&work](ObjectId id) {
        return !std::binary_search(
            work.currentObjects.begin(), work.currentObjects.end(), id);
    });
    return work;
}

template <typename T, typename Owner, typename Less>
void mergeReusableObjectOutputs(
    const render::SharedSnapshotVector<T>& cached,
    render::SharedSnapshotVector<T>& destination,
    container::Span<const ObjectId> currentObjects,
    container::Span<const ObjectId> refreshObjects,
    Owner owner,
    Less less) {
    container::Vector<T>& fresh = destination.mutableValues();
    std::stable_sort(fresh.begin(), fresh.end(), less);

    render::SharedSnapshotVector<T> merged;
    const auto reusable = [&](size_t index) {
        const ObjectId id = owner(cached[index]);
        return id && std::binary_search(
                   currentObjects.begin(), currentObjects.end(), id) &&
            !std::binary_search(
                refreshObjects.begin(), refreshObjects.end(), id);
    };
    size_t cachedIndex = 0;
    size_t freshIndex = 0;
    while (cachedIndex < cached.size() || freshIndex < fresh.size()) {
        while (cachedIndex < cached.size() && !reusable(cachedIndex)) {
            ++cachedIndex;
        }
        if (cachedIndex == cached.size()) {
            while (freshIndex < fresh.size()) {
                merged.appendOwned(std::move(fresh[freshIndex++]));
            }
            break;
        }
        if (freshIndex == fresh.size()) {
            merged.appendSharedSlice(cached, cachedIndex, 1u);
            ++cachedIndex;
            continue;
        }

        const T& cachedValue = cached[cachedIndex];
        T& freshValue = fresh[freshIndex];
        if (less(freshValue, cachedValue)) {
            merged.appendOwned(std::move(freshValue));
            ++freshIndex;
        } else if (less(cachedValue, freshValue)) {
            merged.appendSharedSlice(cached, cachedIndex, 1u);
            ++cachedIndex;
        } else {
            // A freshly extracted row is authoritative on an equal canonical
            // key. This also makes an unexpected dirty-writer omission fail
            // safe without duplicating the render channel.
            merged.appendOwned(std::move(freshValue));
            ++freshIndex;
            ++cachedIndex;
        }
    }
    destination = std::move(merged);
}

[[nodiscard]] uint64_t renderEntityObjectKey(
    const render::RenderEntitySnapshot& value) noexcept {
    return value.objectId != 0 ? value.objectId : value.id;
}

[[nodiscard]] bool renderEntityLess(
    const render::RenderEntitySnapshot& left,
    const render::RenderEntitySnapshot& right) noexcept {
    const uint64_t leftObject = renderEntityObjectKey(left);
    const uint64_t rightObject = renderEntityObjectKey(right);
    if (leftObject != rightObject) return leftObject < rightObject;
    if (left.channelIndex != right.channelIndex) {
        return left.channelIndex < right.channelIndex;
    }
    return left.id < right.id;
}

[[nodiscard]] bool animationAdmissionLess(
    const render::RenderAnimationEndpointAdmission& left,
    const render::RenderAnimationEndpointAdmission& right) noexcept {
    if (left.objectId != right.objectId) return left.objectId < right.objectId;
    return left.channelIndex < right.channelIndex;
}

template <typename T>
[[nodiscard]] bool objectIdLess(const T& left, const T& right) noexcept {
    return left.objectId < right.objectId;
}

} // namespace

render::WorldRenderSnapshot GameRenderExtraction::extract(
    const GameSessionContentStartState& content,
    const GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    const GameSessionObjectEventState& objectEvents,
    GameRenderExtractionCache& cache,
    render::RenderCameraSnapshot camera,
    uint64_t simulationFrame,
    container::Span<const ObjectId> localSelection,
    ObjectId localHover,
    bool showPlayerWaypoints,
    bool includeVisualAssetDependencies) {
    GameRenderTerrainExtractionSource terrainSource{
        .registry = world.m_registry,
        .content = content.m_contentSnapshot,
        .players = content.m_players,
        .terrain = content.m_terrain,
        .ownership = world.m_ownership,
        .localSelection = localSelection,
        .showPlayerWaypoints = showPlayerWaypoints,
        .clientTerrainObjects = world.m_clientTerrainObjects,
        .localPlacement = presentation.m_localPlacementPresentation,
        .queuedConstructionPlacements =
            presentation.m_queuedConstructionPlacements,
        .rejectedConstructionPlacements =
            presentation.m_rejectedConstructionPlacements,
        .mapPresentation = presentation.m_scriptMapPresentation,
        .waterPresentation = presentation.m_scriptWaterPresentationSettings,
        .roadPresentation =
            presentation.m_scriptTerrainRoadPresentationSettings,
        .renderSettings = presentation.m_renderGameDataSettings,
        .ruleset = GameSessionRulesetQueryPort{content.m_ruleset.get()},
        .cache = cache.terrain,
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .confirmedTick = presentation.m_confirmedTick,
    };
    const bool cursorPlacementActive =
        !presentation.m_localPlacementPresentation.snapshots().empty();
    const bool confirmedConstructionPreviewActive =
        !presentation.m_queuedConstructionPlacements.empty() ||
        !presentation.m_rejectedConstructionPlacements.empty();
    // Holding Shift exposes the local player's complete confirmed order map;
    // selected actors expose their own route even after Shift is released.
    // Keep the cross-tick patch path disabled while either presentation scope
    // is active so advancing queues cannot leave stale route geometry cached.
    const bool localOrderWaypointPresentationActive = showPlayerWaypoints ||
        std::any_of(
            localSelection.begin(), localSelection.end(),
            [&](ObjectId actor) {
                const std::optional<ecs::entity> entity =
                    world.m_objects.entityFromId(actor);
                const ObjectOrderQueueComponent* queue = entity
                    ? ecs::try_get<ObjectOrderQueueComponent>(
                          world.m_registry, *entity)
                    : nullptr;
                return queue && std::any_of(
                    queue->orders.begin(), queue->orders.end(),
                    [](const ObjectOrderIntent& order) {
                        return order.source == ObjectOrderSource::Player &&
                            order.kind != ObjectOrderKind::Stop;
                    });
            });
    const uint64_t renderDirtyRevision = objectDirtyRevision(
        world.m_registry, ObjectDirtyDomain::RenderExtraction);
    const uint64_t observerPolicy =
        observerPresentationPolicyIdentity(content.m_players);
    const bool cacheKeyMatches = cache.worldSnapshot &&
        !cursorPlacementActive &&
        cache.worldFrame == simulationFrame &&
        cache.worldEpoch == presentation.m_scriptPresentationEpoch &&
        cache.worldPresentationSequence ==
            presentation.m_scriptPresentationSequence &&
        cache.worldObserverPolicy == observerPolicy &&
        cache.worldClientTerrainRevision ==
            world.m_clientTerrainObjects.revision() &&
        cache.worldDirtyRevision == renderDirtyRevision &&
        cache.worldHover == localHover &&
        cache.worldShowPlayerWaypoints == showPlayerWaypoints &&
        cache.worldDependencies ==
            includeVisualAssetDependencies &&
        sameSelection(localSelection, cache.worldSelection);
    if (cacheKeyMatches) {
        render::WorldRenderSnapshot snapshot =
            *cache.worldSnapshot;
        snapshot.camera = std::move(camera);
        return snapshot;
    }

    render::WorldRenderSnapshot snapshot;
    extractViewAndVisibility(
        presentation, content, world, &cache, snapshot, camera,
        simulationFrame);
    extractTerrainAdmission(terrainSource, snapshot);
    const render::WorldRenderSnapshot* cached =
        cache.worldSnapshot.get();
    // Confirmed construction previews can be cached for every render frame
    // of one logic tick, but must not enter the cross-tick object patch path:
    // they have renderer-domain identities and are rebuilt from the current
    // queue rather than merged as live ObjectIds.
    const bool canPatchObjects = cached && !cursorPlacementActive &&
        !confirmedConstructionPreviewActive &&
        !localOrderWaypointPresentationActive &&
        !includeVisualAssetDependencies &&
        !cache.worldDependencies &&
        cache.worldFrame <= simulationFrame &&
        cache.worldEpoch == presentation.m_scriptPresentationEpoch &&
        cache.worldPresentationSequence ==
            presentation.m_scriptPresentationSequence &&
        cache.worldObserverPolicy == observerPolicy &&
        cache.worldClientTerrainRevision ==
            world.m_clientTerrainObjects.revision() &&
        cache.worldHover == localHover &&
        cache.worldShowPlayerWaypoints == showPlayerWaypoints &&
        sameSelection(localSelection, cache.worldSelection) &&
        cached->renderGameDataSettings == snapshot.renderGameDataSettings &&
        cached->renderFeatureQuality == snapshot.renderFeatureQuality &&
        cached->terrain == snapshot.terrain &&
        visibilityChangeCanPatch(
            cached->localVisibility, snapshot.localVisibility);

    bool patchedObjects = false;
    std::optional<RenderExtractionWork> patchWork;
    if (canPatchObjects) {
        RenderExtractionWork work = collectRenderExtractionWork(
            world.m_registry, content.m_players, snapshot.localVisibility,
            simulationFrame,
            static_cast<uint32_t>(std::max(
                1, content.m_startInfo.gameSpeedFPS)),
            *cached);
        container::Vector<render::TacticalRadarEventRenderSnapshot>
            rawGameplayRadarCandidates;
        const PlayerState* radarObserver = content.m_players.localPlayer();
        const uint64_t radarLifetime = static_cast<uint64_t>(
            snapshot.objectUi.logicFramesPerSecond) * 4u;
        const bool hasActiveUpgradeRadarEvent = radarObserver &&
            radarObserver->isSimulationParticipant() &&
            objectEvents.m_upgradeRadarEpoch ==
                presentation.m_scriptPresentationEpoch &&
            std::any_of(
                objectEvents.m_upgradeRadarHistory.begin(),
                objectEvents.m_upgradeRadarHistory.end(),
                [radarObserver, simulationFrame, radarLifetime](
                    const auto& event) {
                    return event.player == radarObserver->id &&
                        simulationFrame >= event.confirmedTick &&
                        simulationFrame - event.confirmedTick <= radarLifetime;
                });
        if (!work.refreshObjects.empty() || hasActiveUpgradeRadarEvent) {
            extractEntitiesAndUi(
                {
                    .content = content,
                    .presentation = presentation,
                    .objectEvents = objectEvents,
                    .world = world,
                    .cacheOwner = &cache,
                    .gameplayRadarHistory =
                        cache.gameplayRadarHistory,
                    .gameplayRadarEpoch =
                        cache.gameplayRadarEpoch,
                },
                snapshot, simulationFrame, localSelection, localHover,
                false, work.refreshObjects, true,
                &rawGameplayRadarCandidates);
        }
        render_extraction_detail::updateGameplayRadarHistoryAndAppend(
            cache.gameplayRadarHistory,
            cache.gameplayRadarEpoch,
            presentation.m_scriptPresentationEpoch,
            std::move(rawGameplayRadarCandidates),
            snapshot.tacticalRadar.events.mutableValues(),
            simulationFrame, snapshot.objectUi.logicFramesPerSecond);
        if (snapshot.tacticalRadar.events.size() >
            script::ScriptMapPresentationState::kMaximumRadarEvents) {
            auto& events = snapshot.tacticalRadar.events.mutableValues();
            events.erase(
                events.begin(),
                events.end() -
                    script::ScriptMapPresentationState::kMaximumRadarEvents);
        }
        snapshot.localVisibility.hiddenEntityCount = work.hiddenEntities;
        snapshot.localVisibility.hiddenProjectileCount =
            work.hiddenProjectiles;
        patchWork = std::move(work);
        patchedObjects = true;
    }
    if (!patchedObjects) {
        if (canPatchObjects) {
            snapshot = {};
            extractViewAndVisibility(
                presentation, content, world, &cache, snapshot, camera,
                simulationFrame);
            extractTerrainAdmission(terrainSource, snapshot);
        }
        extractEntitiesAndUi(
            {
                .content = content,
                .presentation = presentation,
                .objectEvents = objectEvents,
                .world = world,
                .cacheOwner = &cache,
                .gameplayRadarHistory =
                    cache.gameplayRadarHistory,
                .gameplayRadarEpoch =
                    cache.gameplayRadarEpoch,
            },
            snapshot, simulationFrame, localSelection, localHover,
            includeVisualAssetDependencies, {}, false, nullptr);
    }
    finalizeAssembly(terrainSource, snapshot);
    if (patchedObjects) {
        const auto entityOwner = [](const render::RenderEntitySnapshot& value) {
            const uint64_t raw = renderEntityObjectKey(value);
            return raw <= std::numeric_limits<uint32_t>::max()
                ? ObjectId{static_cast<uint32_t>(raw)} : INVALID_OBJECT_ID;
        };
        const auto objectOwner = [](const auto& value) {
            return value.objectId <= std::numeric_limits<uint32_t>::max()
                ? ObjectId{static_cast<uint32_t>(value.objectId)}
                : INVALID_OBJECT_ID;
        };
        mergeReusableObjectOutputs(
            cached->animationEndpointAdmissions,
            snapshot.animationEndpointAdmissions,
            patchWork->currentObjects, patchWork->refreshObjects,
            objectOwner, animationAdmissionLess);
        mergeReusableObjectOutputs(
            cached->entities, snapshot.entities,
            patchWork->currentObjects, patchWork->refreshObjects,
            entityOwner, renderEntityLess);
        mergeReusableObjectOutputs(
            cached->objectUi.objects, snapshot.objectUi.objects,
            patchWork->currentObjects, patchWork->refreshObjects,
            objectOwner, objectIdLess<render::ObjectUiRenderSnapshot>);
        mergeReusableObjectOutputs(
            cached->objectIcons.icons, snapshot.objectIcons.icons,
            patchWork->currentObjects, patchWork->refreshObjects,
            objectOwner, objectIdLess<render::ObjectIconRenderSnapshot>);
        mergeReusableObjectOutputs(
            cached->projectiles, snapshot.projectiles,
            patchWork->currentObjects, patchWork->refreshObjects,
            objectOwner, objectIdLess<render::ProjectileRenderSnapshot>);
        mergeReusableObjectOutputs(
            cached->trackMarks, snapshot.trackMarks,
            patchWork->currentObjects, patchWork->refreshObjects,
            objectOwner, objectIdLess<render::TrackMarkRenderInput>);
    } else {
        std::stable_sort(
            snapshot.animationEndpointAdmissions.mutableValues().begin(),
            snapshot.animationEndpointAdmissions.mutableValues().end(),
            animationAdmissionLess);
        std::stable_sort(
            snapshot.entities.mutableValues().begin(),
            snapshot.entities.mutableValues().end(), renderEntityLess);
        std::stable_sort(
            snapshot.objectUi.objects.mutableValues().begin(),
            snapshot.objectUi.objects.mutableValues().end(),
            objectIdLess<render::ObjectUiRenderSnapshot>);
        std::stable_sort(
            snapshot.objectIcons.icons.mutableValues().begin(),
            snapshot.objectIcons.icons.mutableValues().end(),
            objectIdLess<render::ObjectIconRenderSnapshot>);
        std::stable_sort(
            snapshot.projectiles.mutableValues().begin(),
            snapshot.projectiles.mutableValues().end(),
            objectIdLess<render::ProjectileRenderSnapshot>);
        std::stable_sort(
            snapshot.trackMarks.mutableValues().begin(),
            snapshot.trackMarks.mutableValues().end(),
            objectIdLess<render::TrackMarkRenderInput>);
    }
    snapshot.sealSharedColumns();
    if (!cursorPlacementActive) {
        cache.worldSnapshot =
            std::make_shared<const render::WorldRenderSnapshot>(snapshot);
        cache.worldSelection.assign(
            localSelection.begin(), localSelection.end());
        cache.worldFrame = simulationFrame;
        cache.worldEpoch = presentation.m_scriptPresentationEpoch;
        cache.worldPresentationSequence =
            presentation.m_scriptPresentationSequence;
        cache.worldObserverPolicy = observerPolicy;
        cache.worldClientTerrainRevision =
            world.m_clientTerrainObjects.revision();
        cache.worldDirtyRevision = objectDirtyRevision(
            world.m_registry, ObjectDirtyDomain::RenderExtraction);
        cache.worldHover = localHover;
        cache.worldShowPlayerWaypoints = showPlayerWaypoints;
        cache.worldDependencies =
            includeVisualAssetDependencies;
    } else {
        cache.worldSnapshot.reset();
    }
    return snapshot;
}

} // namespace engine
