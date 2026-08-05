#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/containment/ObjectSpawnSlaveDetail.h"
#include "core/container/string_utils.h"

#include "game/base/DamageTypes.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectAIOpportunityTargetPolicy.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/terrain/TerrainLogic.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/simulation/runtime/ObjectToppleTransaction.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>


namespace engine::object_spawn_slave_detail {

void updateTensileCandidate(
    UpdateContext& context, const container::Vector<Candidate>& objects,
    const Candidate& candidate) {
    auto& registry = context.registry;
    auto& lifecycle = context.lifecycle;
    const ObjectSpatialIndex* spatialIndex = context.spatialIndex;
    const game::terrain::TerrainLogic* terrain = context.terrain;
    SimulationRandom* random = context.random;
    const ObjectSimulationRules& rules = context.rules;
    const uint64_t confirmedTick = context.confirmedTick;
    auto& damageRequests = context.damageRequests;
    auto& navigationEvents = context.tensileNavigationEvents;
    auto& presentationEvents = context.tensilePresentationEvents;
    ObjectSpawnSlaveComponent& component =
        ecs::get<ObjectSpawnSlaveComponent>(registry, candidate.entity);
    TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, candidate.entity);

        for (size_t i = 0; i < component.tensileFormations.size(); ++i) {
            ObjectTensileRuntime& runtime = component.tensileFormations[i];
            const game::ObjectTensileFormationRule& rule =
                component.plan->tensileFormations[i];
            if (!transform || runtime.terminal) continue;
            const LogicFixedVec3 selfPosition =
                readAuthoritativeObjectPosition(
                    registry, candidate.entity, *transform);
            if (!runtime.linksInitialized) {
                auto& nearest = context.tensileNearestScratch;
                nearest.clear();
                for (const Candidate& other : objects) {
                    if (other.id == candidate.id) continue;
                    const ObjectSpawnSlaveComponent& otherComponent =
                        ecs::get<ObjectSpawnSlaveComponent>(registry,
                                                             other.entity);
                    if (otherComponent.tensileFormations.empty()) continue;
                    const TransformComponent* otherTransform =
                        ecs::try_get<TransformComponent>(registry,
                                                          other.entity);
                    if (otherTransform) {
                        const LogicFixedVec3 otherPosition =
                            readAuthoritativeObjectPosition(
                                registry, other.entity, *otherTransform);
                        nearest.emplace_back(
                            distanceSquared(selfPosition, otherPosition),
                            other.id);
                    }
                }
                std::sort(nearest.begin(), nearest.end(),
                    [](const auto& a, const auto& b) {
                        return a.first != b.first
                            ? a.first < b.first : a.second < b.second;
                    });
                for (size_t n = 0;
                     n < runtime.links.size() && n < nearest.size(); ++n) {
                    runtime.links[n] = nearest[n].second;
                    const std::optional<ecs::entity> linked =
                        lifecycle.entityFromId(nearest[n].second);
                    const TransformComponent* linkedTransform = linked
                        ? ecs::try_get<TransformComponent>(registry,
                                                           *linked)
                        : nullptr;
                    if (linkedTransform) {
                        const LogicFixedVec3 linkedPosition =
                            readAuthoritativeObjectPosition(
                                registry, *linked, *linkedTransform);
                        runtime.tensors[n] = {
                            linkedPosition.x - selfPosition.x,
                            linkedPosition.y - selfPosition.y,
                            linkedPosition.z - selfPosition.z};
                    }
                }
                const Fixed pi = Fixed::from_raw(13'493'037'705ll);
                writeAuthoritativeObjectYaw(
                    registry, candidate.entity,
                    random ? random->fixedInclusive(-pi, pi) : Fixed{});
                runtime.linksInitialized = true;
                const ObjectHealthComponent* initialHealth =
                    ecs::try_get<ObjectHealthComponent>(registry,
                                                         candidate.entity);
                if (!runtime.enabled &&
                    (!initialHealth || initialHealth->damageState <
                         ObjectBodyDamageState::Damaged)) {
                    navigationEvents.push_back({
                        .kind = ObjectTensileFormationEventKind::
                            NavigationWallCreate,
                        .object = candidate.id,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                        .submissionOrdinal =
                            reserveGameplaySubmissionOrdinal(context),
                    });
                }
                ++runtime.revision;
            }

            ObjectHealthComponent* tensileHealth =
                ecs::try_get<ObjectHealthComponent>(registry,
                                                      candidate.entity);
            bool activatedByDamage = false;
            if (!runtime.enabled) {
                if (!tensileHealth ||
                    tensileHealth->damageState <
                        ObjectBodyDamageState::Damaged) {
                    continue;
                }
                runtime.enabled = true;
                activatedByDamage = true;
            }
            if (!runtime.activationPublished) {
                runtime.activationPublished = true;
                navigationEvents.push_back({
                    .kind = ObjectTensileFormationEventKind::
                        NavigationWallRemove,
                    .object = candidate.id,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                    .submissionOrdinal =
                        reserveGameplaySubmissionOrdinal(context),
                });
                if (activatedByDamage && !rule.crackSound.empty()) {
                    presentationEvents.push_back({
                        .kind = ObjectTensileFormationEventKind::CrackSound,
                        .object = candidate.id,
                        .resource = rule.crackSound,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
            }

            if (++runtime.lifeTicks > 300u) {
                runtime.terminal = true;
                runtime.movingCondition = false;
                runtime.freefallCondition = false;
                if (tensileHealth) {
                    tensileHealth->previousFixed =
                        tensileHealth->currentFixed;
                    tensileHealth->currentFixed = {};
                    tensileHealth->damageState =
                        ObjectBodyDamageState::Rubble;
                    tensileHealth->effectivelyDead = true;
                    tensileHealth->terminalDeathIssued = true;
                    markObjectDirty(
                        registry, candidate.entity,
                        objectDirtyBit(
                            ObjectDirtyDomain::ModelCondition) |
                            objectDirtyBit(
                                ObjectDirtyDomain::RenderExtraction));
                }
                navigationEvents.push_back({
                    .kind = ObjectTensileFormationEventKind::TerminalRubble,
                    .object = candidate.id,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                    .submissionOrdinal =
                        reserveGameplaySubmissionOrdinal(context),
                });
                ++runtime.revision;
                continue;
            }

            if (runtime.lifeTicks % 30u == 29u) {
                const Fixed propagationRadius{int32_t{100}};
                const Fixed propagationSquared =
                    propagationRadius * propagationRadius;
                // `objects` is ObjectId-sorted by the caller, so the crack
                // pulse visits neighbours in one canonical order on every
                // peer. Keep this loop reading that same vector.
                for (const Candidate& other : objects) {
                    // A sliding formation never cracks itself: its own runtime
                    // already owns its damage state and lifetime.
                    if (other.id == candidate.id) continue;
                    // Only tensile holders take part in crack propagation.
                    // The link-initialization loop above applies exactly this
                    // filter; without it any other ObjectSpawnSlaveComponent
                    // holder parked nearby (stinger site, angry-mob nexus,
                    // slaved drone) was silently cracked by a passing wall.
                    const ObjectSpawnSlaveComponent& otherComponent =
                        ecs::get<ObjectSpawnSlaveComponent>(registry,
                                                             other.entity);
                    if (otherComponent.tensileFormations.empty()) continue;
                    const TransformComponent* otherTransform =
                        ecs::try_get<TransformComponent>(registry,
                                                          other.entity);
                    const ObjectHealthComponent* otherHealth =
                        ecs::try_get<ObjectHealthComponent>(registry,
                                                             other.entity);
                    if (!otherTransform || !otherHealth ||
                        distanceSquared(
                            selfPosition,
                            readAuthoritativeObjectPosition(
                                registry, other.entity, *otherTransform)) >
                            propagationSquared) {
                        continue;
                    }
                    if (otherHealth->damageState >=
                            ObjectBodyDamageState::Damaged) {
                        continue;
                    }
                    // Health belongs to the central Body damage barrier.
                    // Force-writing currentFixed/damageState here produced no
                    // damage event, no DamageFX, no score and no retaliation,
                    // so submit the exact gap down to the damaged threshold as
                    // one damage transaction instead. UNRESISTABLE bypasses
                    // armor and the stacked damage scalar, so the resolved
                    // health lands on the same value the direct write used to
                    // publish, and the resolver owns the damage-state recompute
                    // plus the ModelCondition/RenderExtraction dirty bits.
                    const Fixed damagedHealth = Fixed::max(
                        Fixed{}, otherHealth->maximumFixed *
                            rules.unitDamagedThresholdFixed -
                            Fixed{int32_t{1}});
                    if (otherHealth->currentFixed <= damagedHealth) continue;
                    const Fixed crackDamage =
                        otherHealth->currentFixed - damagedHealth;
                    // These requests resolve after the whole spawn/slave
                    // system returns, so a second formation cracking the same
                    // neighbour on this tick would still read the pre-barrier
                    // health and stack a second full gap. One pulse per
                    // neighbour per tick preserves the authored "drop to just
                    // below the damaged threshold" result; the lowest
                    // ObjectId formation in this sorted sweep wins.
                    const bool alreadyPulsed = std::any_of(
                        damageRequests.begin(), damageRequests.end(),
                        [&other](const ObjectDamageRequest& pending) {
                            return pending.target == other.id &&
                                pending.damageType ==
                                    game::DamageType::UNRESISTABLE &&
                                !pending.forceKill &&
                                pending.amount > Fixed{};
                        });
                    if (alreadyPulsed) continue;
                    damageRequests.push_back({
                        .target = other.id,
                        .source = candidate.id,
                        .causalGroup = candidate.id,
                        .amount = crackDamage,
                        .damageType = game::DamageType::UNRESISTABLE,
                        .deathType = game::DeathType::NORMAL,
                        .confirmedTick = confirmedTick,
                    });
                }
            }

            if (!terrain) continue;
            LogicFixedVec3 next = readAuthoritativeObjectPosition(
                registry, candidate.entity, *transform);
            const Fixed previousZ = next.z;
            const container::Array<int64_t, 3> normal =
                terrain->map().groundNormalRaw(next.x.raw(), next.y.raw());
            const Fixed normalX = Fixed::from_raw(normal[0]);
            const Fixed normalY = Fixed::from_raw(normal[1]);
            const Fixed normalZ = Fixed::from_raw(normal[2]);
            const Fixed steepness = Fixed{int32_t{1}} - normalZ;
            const Fixed slopeScale = Fixed::from_fraction(3, 10) + steepness;
            runtime.inertia.x += normalX * slopeScale;
            runtime.inertia.y += normalY * slopeScale;
            runtime.inertia.x *= Fixed::from_fraction(19, 20);
            runtime.inertia.y *= Fixed::from_fraction(19, 20);

            next.x += runtime.inertia.x;
            next.y += runtime.inertia.y;
            next.z = Fixed::from_raw(
                terrain->groundHeightRaw(next.x.raw(), next.y.raw()));
            for (size_t linkIndex = 0;
                 linkIndex < runtime.links.size(); ++linkIndex) {
                const std::optional<ecs::entity> linked =
                    lifecycle.entityFromId(runtime.links[linkIndex]);
                const TransformComponent* linkedTransform = linked
                    ? ecs::try_get<TransformComponent>(registry,
                                                        *linked)
                    : nullptr;
                if (!linkedTransform) continue;
                const LogicFixedVec3 linkedPosition =
                    readAuthoritativeObjectPosition(
                        registry, *linked, *linkedTransform);
                const LogicFixedVec3 desired{
                    linkedPosition.x - runtime.tensors[linkIndex].x,
                    linkedPosition.y - runtime.tensors[linkIndex].y,
                    linkedPosition.z - runtime.tensors[linkIndex].z};
                next.x = next.x * Fixed::from_fraction(93, 100) +
                    desired.x * Fixed::from_fraction(7, 100);
                next.y = next.y * Fixed::from_fraction(93, 100) +
                    desired.y * Fixed::from_fraction(7, 100);
                next.z = Fixed::min(
                    runtime.lowestSlideElevation,
                    Fixed::from_raw(terrain->groundHeightRaw(
                        next.x.raw(), next.y.raw())));
            }

            if (spatialIndex) {
                const ObjectGeometryComponent* geometry =
                    ecs::try_get<ObjectGeometryComponent>(
                        registry, candidate.entity);
                const Fixed radius = geometry
                    ? Fixed::max(Fixed{}, geometry->majorRadiusFixed)
                    : Fixed{};
                spatialIndex->queryRadiusFixed(
                    next, radius, context.spatialQueryScratch);
                for (const ObjectId nearby : context.spatialQueryScratch) {
                    const std::optional<ecs::entity> shrub =
                        lifecycle.entityFromId(nearby);
                    if (!shrub || !hasKind(
                            ecs::try_get<ObjectKindOfComponent>(
                                registry, *shrub),
                            game::ObjectKindOf::Shrubbery)) continue;
                    queueObjectToppleRequest(registry, {
                        .object = nearby,
                        .direction = runtime.inertia,
                        .speed = Fixed::sqrt(
                            runtime.inertia.x * runtime.inertia.x +
                            runtime.inertia.y * runtime.inertia.y),
                        .confirmedTick = confirmedTick,
                        .noBounce = true,
                        .noFx = false,
                    });
                    break;
                }
            }

            // Presentation is composed centrally from the tensile runtime by
            // ObjectModelConditionAuthority. Never write the RenderModel mask
            // here: another gameplay source may own the same condition bit.
            runtime.movingCondition = runtime.lifeTicks < 200u;
            runtime.freefallCondition = runtime.lifeTicks < 100u &&
                Fixed::abs(previousZ - next.z) >
                    Fixed::from_fraction(1, 5);
            runtime.lowestSlideElevation = next.z;
            writeAuthoritativeObjectPosition(
                registry, candidate.entity, next);
            ++runtime.revision;
        }
}

} // namespace engine::object_spawn_slave_detail
