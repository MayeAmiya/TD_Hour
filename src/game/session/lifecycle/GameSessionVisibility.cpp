#include "game/session/lifecycle/GameSessionWorldMaintenanceService.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"

#include "core/container/string_utils.h"
#include "game/object/simulation/world/ObjectDynamicShroud.h"
#include "game/object/simulation/world/ObjectSpyVision.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"

#include <algorithm>

namespace engine {
namespace {

[[nodiscard]] bool hasObjectKind(const ObjectKindOfComponent* kinds,
                                 game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

} // namespace

void GameSessionWorldMaintenanceService::updateMapVisibilityLookers(
    uint64_t confirmedTick) {
    if (!m_world.m_mapVisibility.isInitialized()) return;

    struct ActiveSpyRule final {
        ObjectId source = INVALID_OBJECT_ID;
        PlayerId viewer = INVALID_PLAYER_ID;
        const game::ObjectSpyVisionRule* rule = nullptr;
    };
    container::Vector<ActiveSpyRule> activeSpyRules;
    const auto spySources = ecs::view<const ObjectIdentityComponent,
                                      const OwnerComponent,
                                      const ObjectLifecycleComponent,
                                      const ObjectSpyVisionComponent>(
                                          m_world.m_registry);
    for (const ecs::entity entity : spySources) {
        const ObjectIdentityComponent& identity =
            spySources.template get<const ObjectIdentityComponent>(entity);
        const OwnerComponent& owner =
            spySources.template get<const OwnerComponent>(entity);
        const ObjectLifecycleComponent& lifecycle =
            spySources.template get<const ObjectLifecycleComponent>(entity);
        const ObjectSpyVisionComponent& component =
            spySources.template get<const ObjectSpyVisionComponent>(entity);
        if (!identity.id || !owner.player ||
            lifecycle.phase != ObjectLifecyclePhase::Alive ||
            m_world.m_objects.isPendingDestroy(identity.id) || !component.plan) {
            continue;
        }
        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            if (!component.instances[index].active) continue;
            activeSpyRules.push_back({
                .source = identity.id,
                .viewer = owner.player,
                .rule = &component.plan->rules[index],
            });
        }
    }
    std::sort(activeSpyRules.begin(), activeSpyRules.end(),
        [](const ActiveSpyRule& left, const ActiveSpyRule& right) {
            if (left.source != right.source) return left.source < right.source;
            return left.rule->authoredOrder < right.rule->authoredOrder;
        });

    container::Vector<game::terrain::MapVisibilityDynamicLooker> lookers;
    container::Vector<game::terrain::MapVisibilityDynamicShrouder> shrouders;
    const auto objects = ecs::view<const ObjectIdentityComponent,
                                   const ObjectFixedTransformComponent,
                                   const OwnerComponent,
                                   const ThingTemplateComponent,
                                   const ObjectLifecycleComponent>(m_world.m_registry);
    lookers.reserve(objects.size_hint());
    for (const ecs::entity entity : objects) {
        const ObjectLifecycleComponent& lifecycle =
            objects.template get<const ObjectLifecycleComponent>(entity);
        const ObjectIdentityComponent& identity =
            objects.template get<const ObjectIdentityComponent>(entity);
        const OwnerComponent& owner = objects.template get<const OwnerComponent>(entity);
        const ThingTemplateComponent& type =
            objects.template get<const ThingTemplateComponent>(entity);
        if (lifecycle.phase != ObjectLifecyclePhase::Alive || !identity.id ||
            !owner.player ||
            !type.archetype) {
            continue;
        }
        if (ecs::try_get<ObjectContainedByComponent>(m_world.m_registry, entity)) {
            continue;
        }
        if (const ObjectMapStatusComponent* mapStatus =
                ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, entity);
            mapStatus && mapStatus->offMap) {
            continue;
        }
        const ObjectFixedTransformComponent& fixedTransform =
            objects.template get<const ObjectFixedTransformComponent>(entity);
        if (!fixedTransform.authoritative) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, entity);
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(m_world.m_registry, entity);
        if (health && health->effectivelyDead) {
            continue;
        }
        const ObjectDynamicShroudComponent* dynamicShroud =
            ecs::try_get<ObjectDynamicShroudComponent>(m_world.m_registry, entity);
        const math::q32_32 clearingRange = dynamicShroud &&
                dynamicShroud->hasProjectedRadius
            ? dynamicShroud->projectedRadius
            : effectiveObjectShroudClearingRangeFixed(
                m_world.m_registry, entity);
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, entity);
        const bool revealClearingRangeToAll =
            hasObjectKind(kinds, game::ObjectKindOf::RevealToAll);
        const auto lookerIdentity = [&](PlayerId target,
                                        uint8_t channel) noexcept {
            return (static_cast<uint64_t>(identity.id.value) << 1u) |
                   (static_cast<uint64_t>(target.value) << 33u) |
                   (static_cast<uint64_t>(channel) << 41u) | 1u;
        };
        if (clearingRange > math::q32_32{}) {
            // RefCode expands each object's lookingMask to its allies. Keep
            // diplomacy out of the module runtime so capture and relationship
            // changes naturally reproject at this completed-frame boundary.
            for (const PlayerId target : m_content.m_players.activePlayerIds()) {
                if (!target ||
                    (!revealClearingRangeToAll && target != owner.player &&
                     m_content.m_players.relationship(owner.player, target) !=
                         PlayerRelationship::Allies)) {
                    continue;
                }
                lookers.push_back({
                    .identity = lookerIdentity(target, 0),
                    .player = target,
                    .x = fixedTransform.position.x,
                    .y = fixedTransform.position.y,
                    .z = fixedTransform.position.z,
                    .radius = clearingRange,
                });
            }
            if (!revealClearingRangeToAll && !activeSpyRules.empty()) {
                container::Vector<PlayerId> spyViewers;
                for (const ActiveSpyRule& spy : activeSpyRules) {
                    if (!spy.rule || !spy.viewer ||
                        m_content.m_players.relationship(spy.viewer, owner.player) !=
                            PlayerRelationship::Enemies ||
                        !objectSpyVisionMatchesKinds(*spy.rule, kinds)) {
                        continue;
                    }
                    spyViewers.push_back(spy.viewer);
                }
                std::sort(spyViewers.begin(), spyViewers.end());
                spyViewers.erase(
                    std::unique(spyViewers.begin(), spyViewers.end()),
                    spyViewers.end());
                for (const PlayerId viewer : spyViewers) {
                    lookers.push_back({
                        .identity = lookerIdentity(viewer, 2),
                        .player = viewer,
                        .x = fixedTransform.position.x,
                        .y = fixedTransform.position.y,
                        .z = fixedTransform.position.z,
                        .radius = clearingRange,
                    });
                }
            }
        }
        const math::q32_32 revealToAllRange =
            type.archetype->templateData.shroudRevealToAllRangeFixed;
        const bool underConstruction = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
        const bool hiddenStealth = status &&
            status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::Stealthed)) &&
            !status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::Detected)) &&
            !status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::Disguised));
        if (revealToAllRange > math::q32_32{} &&
            !underConstruction && !hiddenStealth) {
            for (const PlayerId target : m_content.m_players.activePlayerIds()) {
                if (!target || target == owner.player ||
                    m_content.m_players.relationship(owner.player, target) ==
                        PlayerRelationship::Allies) {
                    continue;
                }
                lookers.push_back({
                    .identity = lookerIdentity(target, 1),
                    .player = target,
                    .x = fixedTransform.position.x,
                    .y = fixedTransform.position.y,
                    .z = fixedTransform.position.z,
                    .radius = revealToAllRange,
                });
            }
        }
        const ObjectActiveShroudComponent* activeShroud =
            ecs::try_get<ObjectActiveShroudComponent>(m_world.m_registry, entity);
        const math::q32_32 shroudRadius = activeShroud
            ? activeShroud->radius : math::q32_32{};
        if (activeShroud && shroudRadius > math::q32_32{}) {
            for (const PlayerId target : m_content.m_players.activePlayerIds()) {
                if (!target || m_content.m_players.relationship(owner.player, target) ==
                                   PlayerRelationship::Allies) {
                    continue;
                }
                shrouders.push_back({
                    .player = target,
                    .x = fixedTransform.position.x,
                    .y = fixedTransform.position.y,
                    .z = fixedTransform.position.z,
                    .radius = shroudRadius,
                });
            }
        }
    }
    static_cast<void>(m_world.m_mapVisibility.updateDynamicLookers(
        lookers, shrouders, confirmedTick,
        m_world.m_visibilityUnlookPersistenceTicks,
        m_world.m_visibilityFogTransitionTicks));
}

void GameSessionWorldMaintenanceService::refreshObjectDerivedPlayerAggregates(
    uint64_t confirmedTick) {
    m_world.m_objectEnergy.update(m_world.m_registry, m_content.m_players, confirmedTick);
    m_world.m_objectSimulation.updateRadarProviders(
        m_world.m_registry, m_world.m_objects, m_content.m_players, confirmedTick);
}

void GameSessionWorldMaintenanceService::updatePlayerPeriodicState(
    uint64_t confirmedTick) {
    for (const PlayerId player : m_content.m_players.activePlayerIds()) {
        // AcademyStats used 1000 when no dozer command-set supply-center
        // template was discoverable. Player simulation has no UI CommandSet
        // dependency, so retain that gameplay-neutral advice threshold here.
        static_cast<void>(m_content.m_players.updateAcademyPeriodicState(
            player, confirmedTick, 1'000u));
    }
}

void GameSessionWorldMaintenanceService::refreshSpatialIndex() {
    m_world.m_spatialIndex.refreshDirty(
        m_world.m_registry, m_world.m_objects);
}

void GameSessionWorldMaintenanceService::updateTerrainLogic(
    uint64_t confirmedTick, uint32_t logicFramesPerSecond) {
    m_content.m_terrain.updateAtLogicRate(
        confirmedTick, std::max(1u, logicFramesPerSecond));
}
} // namespace engine
