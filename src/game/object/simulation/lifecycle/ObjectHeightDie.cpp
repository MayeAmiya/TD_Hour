#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/lifecycle/ObjectHeightDie.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace engine {
namespace {

using Fixed = math::q32_32;
using container::asciiEqualIgnoreCase;

struct Candidate final {
    ObjectId id = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

struct StructureCandidate final {
    ObjectId id = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    ObjectGeometryComponent geometry{};
};

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf expected) noexcept {
    return kinds && game::objectHasKind(kinds->mask, expected);
}

[[nodiscard]] Fixed zDeltaToCenter(const ObjectGeometryComponent& geometry) noexcept {
    return geometry.shape == ObjectGeometryShape::Sphere ? Fixed{}
                                                         : Fixed::max(Fixed{}, geometry.heightFixed) *
                                                               Fixed::from_fraction(1, 2);
}

[[nodiscard]] Fixed maxHeightAbovePosition(const ObjectGeometryComponent& geometry) noexcept {
    return geometry.shape == ObjectGeometryShape::Sphere
        ? Fixed::max(Fixed{}, geometry.majorRadiusFixed)
        : Fixed::max(Fixed{}, geometry.heightFixed);
}

[[nodiscard]] bool isWithinStructureQuery(const LogicFixedVec3& source,
                                           const ObjectGeometryComponent& sourceGeometry,
                                           const StructureCandidate& structure) noexcept {
    // RefCode asks PartitionManager for FROM_BOUNDINGSPHERE_3D entries with
    // maxDist = source.boundingCircleRadius.  Avoid a float sqrt by comparing
    // the equivalent strict squared distance in the fixed-point domain.
    const Fixed dx = structure.position.x - source.x;
    const Fixed dy = structure.position.y - source.y;
    const Fixed dz = (structure.position.z + zDeltaToCenter(structure.geometry)) -
        (source.z + zDeltaToCenter(sourceGeometry));
    const Fixed distanceSquared = dx * dx + dy * dy + dz * dz;
    const Fixed maximumDistance =
        Fixed::max(Fixed{}, sourceGeometry.boundingCircleRadiusFixed) +
        Fixed::max(Fixed{}, sourceGeometry.boundingSphereRadiusFixed) +
        Fixed::max(Fixed{}, structure.geometry.boundingSphereRadiusFixed);
    return distanceSquared < maximumDistance * maximumDistance;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

} // namespace

uint64_t ObjectHeightDieSystem::millisecondsToTicks(uint32_t milliseconds,
                                                     uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * rate + 999u) / 1000u;
}

void ObjectHeightDieSystem::initializeObject(ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype ||
        !templateComponent->archetype->heightDiePlan ||
        templateComponent->archetype->heightDiePlan->rules.empty()) {
        return;
    }

    ObjectHeightDieComponent component{
        .plan = templateComponent->archetype->heightDiePlan,
    };
    component.instances.resize(component.plan->rules.size());
    if (ObjectHeightDieComponent* existing =
            ecs::try_get<ObjectHeightDieComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectHeightDieComponent>(registry, entity, std::move(component));
    }
}

void ObjectHeightDieSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain, const ObjectSimulationRules& rules,
    uint64_t confirmedTick, container::Vector<ObjectHeightDieCommand>& outCommands,
    container::Vector<ObjectHeightDiePresentationEvent>& outPresentation) const {
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent, ObjectHeightDieComponent,
                                const TransformComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) { return left.id < right.id; });

    // The original PartitionManager query is spatially accelerated. This
    // narrow ECS slice keeps its *bounding-sphere* semantics exactly while
    // using an ObjectId-sorted snapshot; HeightDie objects are sparse and the
    // future spatial index may replace only this collection strategy.
    container::Vector<StructureCandidate> structures;
    const auto structureView = ecs::view<const ObjectIdentityComponent,
                                         const ObjectKindOfComponent,
                                         const ObjectGeometryComponent,
                                         const TransformComponent>(registry);
    structures.reserve(structureView.size_hint());
    for (const ecs::entity entity : structureView) {
        const ObjectIdentityComponent& identity =
            structureView.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id) ||
            !hasKind(&structureView.template get<const ObjectKindOfComponent>(entity),
                     game::ObjectKindOf::Structure)) {
            continue;
        }
        const TransformComponent& transform =
            structureView.template get<const TransformComponent>(entity);
        structures.push_back({
            .id = identity.id,
            .position = readAuthoritativeObjectPosition(registry, entity, transform),
            .geometry = structureView.template get<const ObjectGeometryComponent>(entity),
        });
    }
    std::sort(structures.begin(), structures.end(),
              [](const StructureCandidate& left, const StructureCandidate& right) {
                  return left.id < right.id;
              });

    const ObjectGeometryComponent fallbackGeometry{};
    for (const Candidate& candidate : candidates) {
        // InitialDelay is first armed only when HeightDie is allowed to run.
        // A later disable with an already-armed deadline does not shift it.
        if (isObjectDisabled(registry, candidate.entity, confirmedTick)) {
            continue;
        }
        ObjectHeightDieComponent& component =
            ecs::get<ObjectHeightDieComponent>(registry, candidate.entity);
        TransformComponent& transform = ecs::get<TransformComponent>(registry, candidate.entity);
        // DumbProjectileBehavior owns its terminal detonation.  Its authored
        // HeightDieUpdate is a ground-safety companion, but the legacy update
        // ordering lets DumbProjectile reach the target and call detonate()
        // before HeightDie can submit the Body kill.  Running HeightDie first
        // in the ECS kinematics phase would otherwise skip the warhead damage,
        // ProjectileDetonationFX, and FireWeaponWhenDead continuation.
        const ObjectProjectileComponent* projectile =
            ecs::try_get<ObjectProjectileComponent>(registry, candidate.entity);
        const bool projectileOwnsDetonation = projectile &&
            projectile->motion == ObjectProjectileMotion::DumbBezier;
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, candidate.entity);
        const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(registry, candidate.entity);
        if (!component.plan) continue;

        const size_t count = std::min(component.plan->rules.size(), component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectHeightDieRule& rule = component.plan->rules[index];
            ObjectHeightDieRuntime& runtime = component.instances[index];
            if (runtime.earliestDeathTick == ObjectHeightDieRuntime::NeverTick) {
                runtime.earliestDeathTick = saturatingAdd(
                    confirmedTick, millisecondsToTicks(rule.initialDelayMilliseconds,
                                                       rules.logicFramesPerSecond));
            }
            // RefCode returns before even recording lastPosition while its
            // initial delay remains armed.
            if (runtime.earliestDeathTick > confirmedTick) continue;

            LogicFixedVec3 position =
                readAuthoritativeObjectPosition(registry, candidate.entity, transform);
            if (contained && contained->container) {
                runtime.lastPosition = position;
                continue;
            }

            bool directionOk = true;
            if (!runtime.hasDied && rule.onlyWhenMovingDown && position.z >= runtime.lastPosition.z) {
                directionOk = false;
            }

            Fixed terrainHeight = terrain.isLoaded()
                ? Fixed::from_raw(terrain.groundHeightRaw(
                      position.x.raw(), position.y.raw()))
                : Fixed{};
            if (terrain.isLoaded() &&
                rule.targetHeightIncludesStructures) {
                // HeightDie explicitly asks for the highest bridge/wall layer
                // at or below the projectile before scanning ordinary
                // STRUCTURE geometry. This is not the object's current layer:
                // a falling object may cross a bridge it did not start on.
                const game::terrain::TerrainPathfindLayerId layer =
                    terrain.highestPathfindLayerAtRaw(
                        position.x.raw(), position.y.raw(), position.z.raw());
                if (const std::optional<int64_t> layerHeight =
                        terrain.pathfindLayerHeightRawAt(
                            layer, position.x.raw(), position.y.raw())) {
                    terrainHeight = Fixed::max(
                        terrainHeight, Fixed::from_raw(*layerHeight));
                }
            }
            if (!runtime.hasDied && !projectileOwnsDetonation) {
                Fixed targetHeight = terrainHeight + rule.targetHeightAboveTerrain;
                if (rule.targetHeightIncludesStructures) {
                    Fixed tallest{};
                    const ObjectGeometryComponent& sourceGeometry = geometry ? *geometry : fallbackGeometry;
                    for (const StructureCandidate& structure : structures) {
                        if (structure.id == candidate.id ||
                            !isWithinStructureQuery(position, sourceGeometry, structure)) {
                            continue;
                        }
                        tallest = Fixed::max(tallest, maxHeightAbovePosition(structure.geometry));
                    }
                    // This is deliberately a strict comparison: a structure
                    // exactly as high as the authored TargetHeight does not
                    // replace the terrain-relative target in RefCode.
                    if (tallest > rule.targetHeightAboveTerrain) {
                        targetHeight = terrainHeight + tallest;
                    }
                }

                if (position.z < targetHeight && directionOk) {
                    if (rule.snapToGroundOnDeath || position.z < terrainHeight) {
                        position.z = terrainHeight;
                        writeAuthoritativeObjectPosition(registry, candidate.entity, position);
                    }
                    runtime.hasDied = true;
                    outCommands.push_back({
                        .object = candidate.id,
                        .authoredOrder = rule.authoredOrder,
                    });
                }
            }

            // The source pointer observes the post-snap object position. Its
            // comparison is raw world Z, not a height above terrain.
            if (!runtime.attachedParticlesDestroyed &&
                position.z < rule.destroyAttachedParticlesAtHeight &&
                (runtime.hasDied || directionOk)) {
                runtime.attachedParticlesDestroyed = true;
                outPresentation.push_back({
                    .object = candidate.id,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                });
            }
            runtime.lastPosition = position;
        }
    }
}

} // namespace engine
