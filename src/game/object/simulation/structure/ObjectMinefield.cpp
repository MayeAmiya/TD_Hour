#include "game/object/simulation/structure/ObjectMinefield.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/status/ObjectAutoHeal.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace {

using namespace engine;
using Fixed = math::q32_32;

// A regenerating minefield holds its own health above zero so a depleted field
// can come back.  The install and release sites must agree EXACTLY, because the
// release path recognizes its own fixed floor by equality and
// `Fixed::from_fraction(1, 10)` differ by 7 raw units, and when the two sites
// disagreed the floor was never released — leaving a drained mine clamped above
// zero and therefore immune to every damage source, including enemy weapons,
// DISARM and forceKill.  One shared constant is the invariant.
inline constexpr Fixed kRegeneratingMineHealthFloor = Fixed::from_fraction(1, 10);

[[nodiscard]] uint64_t millisecondsToTicks(uint32_t milliseconds,
                                           uint32_t fps) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t product = static_cast<uint64_t>(milliseconds) *
                             std::max<uint32_t>(1, fps);
    return product / 1000u + (product % 1000u != 0 ? 1u : 0u);
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] UpgradeMask objectUpgrades(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectUpgradeInventoryComponent* inventory =
        ecs::try_get<ObjectUpgradeInventoryComponent>(registry, entity);
    return inventory ? inventory->completed : UpgradeMask{};
}

[[nodiscard]] bool generatorActive(
    const game::ObjectGenerateMinefieldRule& rule,
    const UpgradeMask& player,
    const UpgradeMask& object,
    const UpgradeCatalog* catalog) noexcept {
    static_cast<void>(catalog);
    if (!rule.upgradeMasksCompiled) return false;
    if (rule.triggeredByMask.none()) return true;
    const UpgradeMask completed = player | object;
    const bool triggers = rule.requiresAllTriggers
        ? completed.test_for_all(rule.triggeredByMask)
        : completed.test_for_any(rule.triggeredByMask);
    if (!triggers) return false;
    return !completed.test_for_any(rule.conflictsWithMask);
}

[[nodiscard]] Fixed distanceSquared2D(const LogicFixedVec3& left,
                                      const LogicFixedVec3& right) noexcept {
    const Fixed dx = left.x - right.x;
    const Fixed dy = left.y - right.y;
    return dx * dx + dy * dy;
}

const Fixed kFixedZero{};
const Fixed kFixedOne{int32_t{1}};
const Fixed kFixedTwo{int32_t{2}};
const Fixed kFixedFour{int32_t{4}};
constexpr Fixed kFixedPi = Fixed::from_raw(13'493'037'705ll);
const Fixed kFixedFullTurn = kFixedTwo * kFixedPi;
const Fixed kMinimumMineRadius = Fixed::from_fraction(1, 100);

[[nodiscard]] uint32_t ceilPositiveFixed(Fixed value) noexcept {
    if (value <= kFixedZero) return 0;
    constexpr uint64_t fractionMask = (uint64_t{1} << 32u) - 1u;
    const uint64_t raw = static_cast<uint64_t>(value.raw());
    uint64_t result = raw >> 32u;
    if ((raw & fractionMask) != 0) ++result;
    return static_cast<uint32_t>(std::min<uint64_t>(
        result, std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] uint32_t ceilPositiveRatio(Fixed numerator,
                                         Fixed denominator) noexcept {
    if (numerator <= kFixedZero || denominator <= kFixedZero) return 0;
    const uint64_t top = static_cast<uint64_t>(numerator.raw());
    const uint64_t bottom = static_cast<uint64_t>(denominator.raw());
    uint64_t result = top / bottom;
    if (top % bottom != 0) ++result;
    return static_cast<uint32_t>(std::min<uint64_t>(
        result, std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] Fixed distanceSquared3D(const LogicFixedVec3& left,
                                      const LogicFixedVec3& right) noexcept {
    const Fixed dx = left.x - right.x;
    const Fixed dy = left.y - right.y;
    const Fixed dz = left.z - right.z;
    return dx * dx + dy * dy + dz * dz;
}

[[nodiscard]] bool unavailable(const ecs::registry& registry,
                               const ObjectLifecycle& lifecycle,
                               ObjectId id, ecs::entity entity) noexcept {
    if (!id || lifecycle.isPendingDestroy(id)) return true;
    const ObjectLifecycleComponent* state =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    const ObjectMapStatusComponent* map =
        ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    return (state && state->phase != ObjectLifecyclePhase::Alive) ||
           (health && health->effectivelyDead) || (map && map->offMap);
}

[[nodiscard]] bool ignoredByKinds(
    const ObjectKindOfComponent* kinds,
    const game::ObjectKindOfMask& ignored) noexcept {
    return kinds && kinds->mask.test_for_any(ignored);
}

[[nodiscard]] game::ObjectMineRelationshipMask relationshipMask(
    PlayerRelationship relationship) noexcept {
    switch (relationship) {
    case PlayerRelationship::Allies:
        return static_cast<game::ObjectMineRelationshipMask>(
            game::ObjectMineRelationship::Allies);
    case PlayerRelationship::Enemies:
        return static_cast<game::ObjectMineRelationshipMask>(
            game::ObjectMineRelationship::Enemies);
    case PlayerRelationship::Neutral:
        return static_cast<game::ObjectMineRelationshipMask>(
            game::ObjectMineRelationship::Neutral);
    }
    return 0;
}

[[nodiscard]] bool overlap2D(const ObjectFixedTransformComponent& left,
                             const ObjectGeometryComponent* leftGeometry,
                             const ObjectFixedTransformComponent& right,
                             const ObjectGeometryComponent* rightGeometry) noexcept {
    const auto verticalInterval = [](const ObjectFixedTransformComponent& transform,
                                     const ObjectGeometryComponent* geometry) {
        const Fixed z = transform.position.z;
        if (!geometry) {
            return std::pair<Fixed, Fixed>{z, z + kFixedOne};
        }
        if (geometry->shape == ObjectGeometryShape::Sphere) {
            const Fixed radius = Fixed::max(kFixedZero,
                                            geometry->majorRadiusFixed);
            return std::pair<Fixed, Fixed>{z - radius, z + radius};
        }
        const Fixed height = Fixed::max(kFixedZero, geometry->heightFixed);
        return std::pair<Fixed, Fixed>{z, z + height};
    };
    const Fixed leftRadius = leftGeometry
        ? Fixed::max(kFixedZero, leftGeometry->boundingCircleRadiusFixed)
        : kFixedOne;
    const Fixed rightRadius = rightGeometry
        ? Fixed::max(kFixedZero, rightGeometry->boundingCircleRadiusFixed)
        : kFixedOne;
    const Fixed dx = left.position.x - right.position.x;
    const Fixed dy = left.position.y - right.position.y;
    const auto [leftBottom, leftTop] = verticalInterval(left, leftGeometry);
    const auto [rightBottom, rightTop] = verticalInterval(right, rightGeometry);
    if (leftTop < rightBottom || rightTop < leftBottom) return false;
    const Fixed sum = leftRadius + rightRadius;
    return dx * dx + dy * dy <= sum * sum;
}

void setMineEmptyVisual(ecs::registry& registry, ecs::entity entity,
                        bool empty, uint64_t tick) {
    const auto masked = game::objectStatusBit(game::ObjectStatusFlag::Masked);
    static_cast<void>(ObjectStatusSystem::apply(
        registry, entity,
        {.setMask = empty ? masked : 0,
         .clearMask = empty ? 0 : masked,
         .confirmedTick = tick}));
    if (RenderModelComponent* render =
            ecs::try_get<RenderModelComponent>(registry, entity)) {
        static const game::ModelConditionMask rubble =
            game::modelConditionMaskOf(game::ModelConditionFlag::Rubble);
        for (size_t i = 0; i < render->modelConditionFlags.words.size(); ++i) {
            if (empty) render->modelConditionFlags.words[i] |= rubble.words[i];
            else render->modelConditionFlags.words[i] &= ~rubble.words[i];
        }
    }
}

void advanceShot(uint32_t& sequence) noexcept {
    ++sequence;
    if (sequence == 0) ++sequence;
}

[[nodiscard]] bool isDisarmingMine(
    const ecs::registry& registry, ecs::entity other, ObjectId mine,
    const GameContentSnapshot& content) noexcept {
    const ObjectOrderQueueComponent* orders =
        ecs::try_get<ObjectOrderQueueComponent>(registry, other);
    if (!orders || orders->orders.empty() ||
        orders->orders.front().kind != ObjectOrderKind::Attack ||
        orders->orders.front().targetObject != mine) return false;
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, other);
    if (!weapons || !weapons->activeWeaponSetIndex || !weapons->currentSlot ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) return false;
    const size_t slot = static_cast<size_t>(*weapons->currentSlot);
    if (slot >= game::kWeaponSlotCount) return false;
    const game::WeaponTemplate* weapon = content.findWeapon(
        weapons->sets[*weapons->activeWeaponSetIndex].slots[slot].content);
    return weapon && weapon->damageType == game::DamageType::DISARM;
}

} // namespace

namespace engine {

void ObjectMinefieldSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const auto plan = type && type->archetype
        ? type->archetype->minefieldPlan : nullptr;
    if (!plan) return;
    ObjectMinefieldComponent component;
    component.plan = plan;
    component.generators.resize(plan->generators.size());
    component.mines.resize(plan->mines.size());
    component.demoTraps.resize(plan->demoTraps.size());
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    for (size_t i = 0; i < component.mines.size(); ++i) {
        ObjectMinefieldRuntime& runtime = component.mines[i];
        const game::ObjectMinefieldRule& rule = plan->mines[i];
        runtime.detonationWeapon = content.findWeaponId(rule.detonationWeapon);
        runtime.creationList = content.findObjectCreationListId(rule.creationList);
        runtime.virtualMinesRemaining = rule.numVirtualMines;
        runtime.regenerates = rule.regenerates;
        if (rule.regenerates && health) {
            runtime.ownsHealthFloor = true;
            if (health->minimumHealthFloorFixed <
                kRegeneratingMineHealthFloor) {
                ObjectHealthComponent* mutableHealth =
                    ecs::try_get<ObjectHealthComponent>(registry, entity);
                mutableHealth->minimumHealthFloorFixed =
                    kRegeneratingMineHealthFloor;
            }
        }
        runtime.previousHealth = health ? health->currentFixed : Fixed{};
        runtime.nextCreatorDeathCheckTick = confirmedTick;
        runtime.immunities.resize(3);
    }
    for (size_t i = 0; i < component.demoTraps.size(); ++i) {
        ObjectDemoTrapRuntime& runtime = component.demoTraps[i];
        const game::ObjectDemoTrapRule& rule = plan->demoTraps[i];
        runtime.detonationWeapon = content.findWeaponId(rule.detonationWeapon);
        runtime.nextScanTick = confirmedTick;
        const game::WeaponSlot initial = rule.defaultsToProximityMode
            ? rule.proximityModeWeaponSlot : rule.manualModeWeaponSlot;
        runtime.ownsModeLock = setObjectWeaponLock(
            registry, entity, initial, ObjectWeaponLockType::Temporary);
    }
    static_cast<void>(ObjectStatusSystem::apply(
        registry, entity,
        {.setMask = plan->mines.empty() ? 0 :
             game::objectStatusBit(game::ObjectStatusFlag::NoAttackFromAi),
         .confirmedTick = confirmedTick}));
    if (ObjectMinefieldComponent* existing =
            ecs::try_get<ObjectMinefieldComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectMinefieldComponent>(registry, entity,
                                               std::move(component));
    }
    static_cast<void>(rules);
}

namespace {

void emitGeneratorFx(const game::ObjectGenerateMinefieldRule& rule,
                     ObjectId source, const LogicFixedVec3& position,
                     uint64_t tick, uint64_t& sequence,
                     container::Vector<ObjectMinefieldFxEvent>& out) {
    if (rule.generationFx.empty()) return;
    out.push_back({.source = source, .fxList = rule.generationFx,
                   .position = position, .authoredOrder = rule.authoredOrder,
                   .emissionSequence = sequence++, .confirmedTick = tick});
}

[[nodiscard]] bool minePlacementBlocked(
    const ecs::registry& registry, const game::terrain::TerrainLogic& terrain,
    const LogicFixedVec3& point,
    game::terrain::TerrainPathfindLayerId pathfindLayer, Fixed mineRadius,
    const game::ObjectGenerateMinefieldRule& rule) {
    // RefCode rejects cliff/water only when the highest destination surface
    // is ground.  A valid elevated bridge surface must not be rejected by
    // the ground cell beneath it.
    if (pathfindLayer == game::terrain::kGroundPathfindLayer &&
        (terrain.isCliffCellRaw(point.x.raw(), point.y.raw()) ||
         terrain.isUnderwaterLegacyRaw(point.x.raw(), point.y.raw()))) {
        return true;
    }
    const Fixed retainedRadius = mineRadius * Fixed::clamp(
        rule.skipIfThisMuchUnderStructure, kFixedZero, kFixedOne);
    const auto view = ecs::view<const ObjectFixedTransformComponent,
                                const ObjectKindOfComponent>(registry);
    for (const ecs::entity entity : view) {
        if (!hasKind(&view.template get<const ObjectKindOfComponent>(entity),
                     game::ObjectKindOf::Structure)) continue;
        const ObjectFixedTransformComponent& transform =
            view.template get<const ObjectFixedTransformComponent>(entity);
        if (!transform.authoritative) continue;
        const LogicFixedVec3 structurePosition = transform.position;
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, entity);
        const Fixed radius = geometry
            ? Fixed::max(
                  kFixedZero, geometry->boundingCircleRadiusFixed)
            : kFixedOne;
        const Fixed dx = point.x - structurePosition.x;
        const Fixed dy = point.y - structurePosition.y;
        const Fixed limit = radius + retainedRadius;
        if (dx * dx + dy * dy < limit * limit) return true;
    }
    return false;
}

void appendMineSpawn(const game::ObjectGenerateMinefieldRule& rule,
                     uint32_t generatorIndex,
                     const game::ThingTemplate& mineTemplate,
                     ObjectId producer, PlayerId owner,
                     const LogicFixedVec3& producerPosition,
                     LogicFixedVec3 position,
                     const game::terrain::TerrainLogic& terrain,
                     SimulationRandom& random, uint64_t tick,
                     uint64_t& emissionSequence,
                     const ecs::registry& registry,
                     container::Vector<ObjectMineSpawnCommand>& out) {
    const game::terrain::TerrainPathfindLayerId pathfindLayer =
        terrain.highestPathfindLayerAtXYRaw(
            position.x.raw(), position.y.raw());
    position.z = Fixed::from_raw(
        terrain.pathfindLayerHeightRawAt(
            pathfindLayer, position.x.raw(), position.y.raw()).value_or(
                terrain.groundHeightRaw(position.x.raw(), position.y.raw())));
    const Fixed mineRadius = Fixed::max(
        kFixedZero, mineTemplate.geometry.boundingCircleRadiusFixed);
    if (minePlacementBlocked(registry, terrain, position, pathfindLayer,
                             mineRadius, rule)) return;
    const Fixed yaw = random.fixedInclusive(-kFixedPi, kFixedPi);
    out.push_back({
        .templateName = mineTemplate.name,
        .producer = producer,
        .owner = owner,
        // GenerateMinefieldBehavior creates mines on the controlling
        // player's default Team, not on the producer object's possibly
        // scenario-specific Team. INVALID asks the central spawn authority
        // to resolve that default without exposing ObjectTeamRegistry here.
        .primaryTeam = INVALID_OBJECT_TEAM_ID,
        .position = position,
        .yaw = yaw,
        .scootStart = producerPosition,
        .generatorIndex = generatorIndex,
        .authoredOrder = rule.authoredOrder,
        .emissionSequence = emissionSequence++,
        .confirmedTick = tick,
    });
}

void generateMines(
    const game::ObjectGenerateMinefieldRule& rule, uint32_t generatorIndex,
    math::q32_32 distanceAroundObject,
    math::q32_32 minesPerSquareFoot,
    const game::ObjectArchetype& sourceArchetype,
    const game::ObjectArchetype& mineArchetype,
    math::q32_32 sourceYaw,
    const ObjectGeometryComponent* sourceGeometry,
    ObjectId source, PlayerId owner,
    const LogicFixedVec3& producerPosition, const LogicFixedVec3& target,
    const ecs::registry& registry,
    const game::terrain::TerrainLogic& terrain, SimulationRandom& random,
    uint64_t tick, uint64_t& emissionSequence,
    container::Vector<ObjectMineSpawnCommand>& out) {
    static_cast<void>(sourceArchetype);
    const Fixed mineRadius = Fixed::max(kMinimumMineRadius,
        Fixed::max(kFixedZero,
            mineArchetype.templateData.geometry.
                boundingCircleRadiusFixed));
    const Fixed mineDiameter = mineRadius * kFixedTwo;
    const Fixed jitter = mineRadius *
        Fixed::max(kFixedZero, rule.randomJitter);
    const Fixed sourceMajor = sourceGeometry
        ? Fixed::max(kFixedZero, sourceGeometry->majorRadiusFixed) : kFixedOne;
    const Fixed sourceMinor = sourceGeometry
        ? Fixed::max(kFixedZero, sourceGeometry->minorRadiusFixed) : sourceMajor;
    const Fixed sourceCircle = sourceGeometry
        ? Fixed::max(kFixedZero, sourceGeometry->boundingCircleRadiusFixed)
        : sourceMajor;
    const bool sourceBox = sourceGeometry &&
        sourceGeometry->shape == ObjectGeometryShape::Box && !rule.alwaysCircular;
    const math::q32_32_sincos sourceRotation =
        math::fixed_sincos(sourceYaw);

    const auto jitterPoint = [&](LogicFixedVec3 point) {
        if (jitter > kFixedZero) {
            point.x += random.fixedInclusive(-jitter, jitter);
            point.y += random.fixedInclusive(-jitter, jitter);
        }
        return point;
    };
    const auto circle = [&](Fixed radius) {
        const uint32_t count = std::max<uint32_t>(1,
            ceilPositiveRatio(kFixedFullTurn * radius, mineDiameter));
        for (uint32_t i = 0; i < count; ++i) {
            const Fixed angle = kFixedFullTurn *
                Fixed::from_fraction(static_cast<int64_t>(i),
                                     static_cast<int64_t>(count));
            const math::q32_32_sincos direction = math::fixed_sincos(angle);
            LogicFixedVec3 point = target;
            point.x += radius * direction.cosine;
            point.y += radius * direction.sine;
            appendMineSpawn(rule, generatorIndex, mineArchetype.templateData,
                            source, owner, producerPosition,
                            jitterPoint(point), terrain, random, tick,
                            emissionSequence, registry, out);
        }
    };
    const auto line = [&](Fixed ax, Fixed ay, Fixed bx, Fixed by,
                          bool skipStart) {
        const Fixed dx = bx - ax;
        const Fixed dy = by - ay;
        const Fixed length = Fixed::sqrt(dx * dx + dy * dy);
        const uint32_t count = std::max<uint32_t>(1,
            ceilPositiveRatio(length, mineDiameter));
        for (uint32_t i = skipStart ? 1u : 0u; i <= count; ++i) {
            const Fixed t = Fixed::from_fraction(
                static_cast<int64_t>(i), static_cast<int64_t>(count));
            LogicFixedVec3 point = target;
            point.x += ax + dx * t;
            point.y += ay + dy * t;
            appendMineSpawn(rule, generatorIndex, mineArchetype.templateData,
                            source, owner, producerPosition,
                            jitterPoint(point), terrain, random, tick,
                            emissionSequence, registry, out);
        }
    };
    const auto rectangle = [&](Fixed major, Fixed minor) {
        const auto rotate = [sourceRotation](Fixed x, Fixed y) {
            return std::pair<Fixed, Fixed>{
                sourceRotation.cosine * x - sourceRotation.sine * y,
                sourceRotation.sine * x + sourceRotation.cosine * y};
        };
        const auto p0 = rotate( major,  minor);
        const auto p1 = rotate(-major,  minor);
        const auto p2 = rotate(-major, -minor);
        const auto p3 = rotate( major, -minor);
        line(p0.first, p0.second, p1.first, p1.second, true);
        line(p1.first, p1.second, p2.first, p2.second, true);
        line(p2.first, p2.second, p3.first, p3.second, true);
        line(p3.first, p3.second, p0.first, p0.second, true);
    };

    const Fixed distance = Fixed::max(kFixedZero, distanceAroundObject);
    if (rule.smartBorder) {
        if (!rule.smartBorderSkipInterior) {
            appendMineSpawn(rule, generatorIndex, mineArchetype.templateData,
                            source, owner, producerPosition, target,
                            terrain, random, tick, emissionSequence, registry, out);
        }
        Fixed major = (sourceBox ? sourceMajor : sourceCircle) + mineRadius;
        Fixed minor = sourceMinor + mineRadius;
        do {
            if (sourceBox) rectangle(major, minor);
            else circle(major);
            const Fixed previousMajor = major;
            major += mineDiameter;
            minor += mineDiameter;
            if (major == previousMajor) break;
        } while ((sourceBox ? Fixed::sqrt(major * major + minor * minor)
                            : major) < distance);
        return;
    }
    if (rule.borderOnly) {
        if (sourceBox) rectangle(sourceMajor + distance, sourceMinor + distance);
        else circle(sourceCircle + distance);
        return;
    }

    const Fixed major = (sourceBox ? sourceMajor : sourceCircle) + distance;
    const Fixed minor = sourceMinor + distance;
    const Fixed area = sourceBox
        ? kFixedFour * major * minor
        : kFixedPi * major * major;
    const uint32_t count = std::max<uint32_t>(1,
        ceilPositiveFixed(Fixed::max(kFixedZero, minesPerSquareFoot) * area));
    container::Vector<LogicFixedVec3> accepted;
    accepted.reserve(count);
    const Fixed minimumSquared = mineDiameter * mineDiameter;
    for (uint32_t i = 0; i < count; ++i) {
        LogicFixedVec3 point = target;
        bool found = false;
        for (uint32_t retry = 0; retry < 100 && !found; ++retry) {
            const Fixed x = random.fixedInclusive(-major, major);
            const Fixed y = random.fixedInclusive(-minor, minor);
            if (!sourceBox && x * x + y * y > major * major) continue;
            // The expanded placement footprint excludes the producer's
            // original footprint, matching GeometryInfo::isPointInFootprint.
            const Fixed localX = sourceRotation.cosine * x +
                                 sourceRotation.sine * y;
            const Fixed localY = -sourceRotation.sine * x +
                                 sourceRotation.cosine * y;
            const bool insideProducer = sourceBox
                ? Fixed::abs(localX) <= sourceMajor &&
                  Fixed::abs(localY) <= sourceMinor
                : x * x + y * y <= sourceCircle * sourceCircle;
            if (insideProducer) continue;
            point.x = target.x + x;
            point.y = target.y + y;
            found = std::none_of(accepted.begin(), accepted.end(),
                [&](const LogicFixedVec3& existing) {
                    return distanceSquared2D(existing, point) < minimumSquared;
                });
        }
        if (!found) continue;
        accepted.push_back(point);
        appendMineSpawn(rule, generatorIndex, mineArchetype.templateData,
                        source, owner, producerPosition, point, terrain,
                        random, tick, emissionSequence, registry, out);
    }
}

void queueMineDetonation(
    ecs::registry& registry, ecs::entity entity, ObjectId mine,
    ObjectMinefieldRuntime& runtime, const game::ObjectMinefieldRule& rule,
    const LogicFixedVec3& position, const GameContentSnapshot& content,
    SimulationRandom& random, uint64_t tick, uint64_t& emissionSequence,
    PlayerId owner, ObjectTeamId team,
    container::Vector<ObjectSystemWeaponFireCommand>& outWeapons,
    container::Vector<ObjectCreationListInvocation>& outOcl) {
    if (runtime.detonationWeapon) {
        static_cast<void>(queueObjectTransientWeaponFireAtPosition(
            runtime.detonationWeapon, registry, entity, mine, position,
            content, random, runtime.nextShotSequence, rule.authoredOrder,
            emissionSequence++, tick, outWeapons));
        advanceShot(runtime.nextShotSequence);
    }
    if (runtime.creationList) {
        const ObjectTerrainLayerComponent* terrainLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
        outOcl.push_back({
            .content = runtime.creationList,
            .source = mine,
            .owner = owner,
            .primaryTeam = team,
            .primaryPosition = position,
            .authoredOrder = rule.authoredOrder,
            .emissionSequence = emissionSequence++,
            .confirmedTick = tick,
            .sourcePathfindLayer = terrainLayer
                ? terrainLayer->pathfindLayer
                : game::terrain::kGroundPathfindLayer,
        });
    }
}

void setMineCount(ecs::registry& registry, ObjectLifecycle& lifecycle,
                  ecs::entity entity, ObjectId mine,
                  ObjectMinefieldRuntime& runtime,
                  const game::ObjectMinefieldRule& rule,
                  uint32_t next, uint64_t tick,
                  container::Vector<ObjectDamageRequest>& outDamage) {
    runtime.virtualMinesRemaining = std::min(next, rule.numVirtualMines);
    setMineEmptyVisual(registry, entity, runtime.virtualMinesRemaining == 0, tick);
    if (!runtime.regenerates && runtime.virtualMinesRemaining == 0) {
        static_cast<void>(lifecycle.requestDestroy(
            mine, ObjectDestroyReason::System, tick));
        return;
    }
    ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (!health || rule.numVirtualMines == 0) return;
    const Fixed desired = std::max(
        Fixed::from_fraction(1, 10), health->maximumFixed *
            Fixed::from_fraction(runtime.virtualMinesRemaining,
                                 rule.numVirtualMines));
    if (health->currentFixed > desired) {
        outDamage.push_back({
            .target = mine, .source = mine,
            .sourceSequence = runtime.nextShotSequence,
            .amount = health->currentFixed - desired,
            .damageType = game::DamageType::UNRESISTABLE,
            .deathType = game::DeathType::NONE,
            .confirmedTick = tick,
        });
    }
}

void detonateDemoTrap(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ecs::entity entity,
    ObjectId trap, ObjectDemoTrapRuntime& runtime,
    const game::ObjectDemoTrapRule& rule, const GameContentSnapshot& content,
    SimulationRandom& random, uint64_t tick, uint64_t& emissionSequence,
    container::Vector<ObjectSystemWeaponFireCommand>& outWeapons,
    container::Vector<ObjectDamageRequest>& outDamage) {
    if (runtime.detonated) return;
    const ObjectFixedTransformComponent* transform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, entity);
    const LogicFixedVec3 position = transform && transform->authoritative
        ? transform->position : LogicFixedVec3{};
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    const bool blocked = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
        game::objectStatusBit(game::ObjectStatusFlag::Sold));
    if (!blocked && runtime.detonationWeapon) {
        static_cast<void>(queueObjectTransientWeaponFireAtPosition(
            runtime.detonationWeapon, registry, entity, trap, position,
            content, random, runtime.nextShotSequence, rule.authoredOrder,
            emissionSequence++, tick, outWeapons));
        advanceShot(runtime.nextShotSequence);
    }
    if (!blocked) {
        outDamage.push_back({.target = trap, .source = trap,
                             .sourceSequence = runtime.nextShotSequence,
                             .damageType = game::DamageType::UNRESISTABLE,
                             .deathType = game::DeathType::NORMAL,
                             .forceKill = true, .confirmedTick = tick});
    } else {
        static_cast<void>(lifecycle.requestDestroy(
            trap, ObjectDestroyReason::System, tick));
    }
    runtime.detonated = true;
}

} // namespace

bool ObjectMinefieldSystem::onGenerateMinefieldDie(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const GameContentSnapshot& content,
    const game::terrain::TerrainLogic& terrain,
    SimulationRandom& random, const ObjectSimulationRules& rules,
    ObjectId generator, uint32_t authoredOrder,
    uint64_t confirmedTick, uint64_t& nextEmissionSequence,
    container::Vector<ObjectMineSpawnCommand>& outSpawns,
    container::Vector<ObjectMinefieldFxEvent>& outFx) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(generator);
    ObjectMinefieldComponent* component = entity
        ? ecs::try_get<ObjectMinefieldComponent>(registry, *entity)
        : nullptr;
    if (!entity || !component || !component->plan) return false;
    const ObjectFixedTransformComponent* transform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, *entity);
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, *entity);
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, *entity);
    if (!transform || !transform->authoritative || !type ||
        !type->archetype || !owner) {
        return false;
    }
    for (size_t index = 0;
         index < component->plan->generators.size() &&
         index < component->generators.size(); ++index) {
        const game::ObjectGenerateMinefieldRule& rule =
            component->plan->generators[index];
        if (rule.authoredOrder != authoredOrder ||
            !rule.generateOnlyOnDeath) {
            continue;
        }
        ObjectGenerateMinefieldRuntime& runtime = component->generators[index];
        runtime.lastUpdateTick = confirmedTick;
        if (runtime.generated) return true;
        const PlayerState* player = players.get(owner->player);
        const UpgradeMask playerUpgrades = player
            ? player->upgrades.completed : UpgradeMask{};
        const UpgradeCatalog* upgradeCatalog = content.upgradeCatalog();
        const auto localUpgrades = objectUpgrades(registry, *entity);
        if (!generatorActive(
                rule, playerUpgrades, localUpgrades, upgradeCatalog)) {
            return true;
        }
        const bool upgraded = rule.upgradable &&
            !rule.upgradedMineName.empty() &&
            rule.upgradedTriggerId &&
            (upgradeMaskTest(playerUpgrades, rule.upgradedTriggerId) ||
             upgradeMaskTest(localUpgrades, rule.upgradedTriggerId));
        runtime.upgraded = upgraded;
        const container::String& mineName = upgraded
            ? rule.upgradedMineName : rule.mineName;
        const auto mineArchetype = content.findObjectArchetype(mineName);
        if (!mineArchetype) {
            runtime.generated = true;
            return true;
        }
        const LogicFixedVec3 self = transform->position;
        const LogicFixedVec3 target = runtime.hasTarget
            ? runtime.target : self;
        const math::q32_32 distanceAroundObject =
            rule.hasAuthoredDistanceAroundObject
                ? rule.distanceAroundObject
                : rules.standardMinefieldDistance;
        const math::q32_32 minesPerSquareFoot =
            rule.hasAuthoredMinesPerSquareFoot
                ? rule.minesPerSquareFoot
                : rules.standardMinefieldDensity;
        generateMines(
            rule, static_cast<uint32_t>(index), distanceAroundObject,
            minesPerSquareFoot, *type->archetype, *mineArchetype,
            transform->yawRadians,
            ecs::try_get<ObjectGeometryComponent>(registry, *entity),
            generator, owner->player, self, target, registry, terrain, random,
            confirmedTick, nextEmissionSequence, outSpawns);
        runtime.generated = true;
        emitGeneratorFx(rule, generator, target, confirmedTick,
                        nextEmissionSequence, outFx);
        return true;
    }
    return false;
}

bool ObjectMinefieldSystem::onMinefieldDie(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId mine, uint32_t authoredOrder,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(mine);
    const ObjectMinefieldComponent* component = entity
        ? ecs::try_get<ObjectMinefieldComponent>(registry, *entity)
        : nullptr;
    if (!component || !component->plan) return false;
    const bool matchingOccurrence = std::any_of(
        component->plan->mines.begin(), component->plan->mines.end(),
        [authoredOrder](const game::ObjectMinefieldRule& rule) {
            return rule.authoredOrder == authoredOrder;
        });
    if (!matchingOccurrence) return false;
    static_cast<void>(lifecycle.requestDestroy(
        mine, ObjectDestroyReason::System, confirmedTick));
    return true;
}

void ObjectMinefieldSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const GameContentSnapshot& content,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSpatialIndex* spatialIndex, SimulationRandom& random,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    uint64_t& nextEmissionSequence,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectSystemWeaponFireCommand>& outWeapons,
    container::Vector<ObjectCreationListInvocation>& outOcl,
    container::Vector<ObjectMineSpawnCommand>& outSpawns,
    container::Vector<ObjectMinefieldFxEvent>& outFx) const {
    container::Vector<ObjectId> nearbyScratch;
    const auto nearbyObjects = [&](const LogicFixedVec3& center,
                                   Fixed radius)
        -> const container::Vector<ObjectId>& {
        if (spatialIndex) {
            spatialIndex->queryRadiusFixed(
                center, Fixed::max(kFixedZero, radius), nearbyScratch);
            return nearbyScratch;
        }
        nearbyScratch.clear();
        const auto all = ecs::view<const ObjectIdentityComponent>(registry);
        for (const ecs::entity entity : all) {
            const ObjectId id = all.template get<
                const ObjectIdentityComponent>(entity).id;
            if (id) nearbyScratch.push_back(id);
        }
        std::sort(nearbyScratch.begin(), nearbyScratch.end());
        nearbyScratch.erase(
            std::unique(nearbyScratch.begin(), nearbyScratch.end()),
            nearbyScratch.end());
        return nearbyScratch;
    };
    struct Candidate { ObjectId id; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectMinefieldComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectId id =
            view.template get<const ObjectIdentityComponent>(entity).id;
        if (id) candidates.push_back({id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.id < b.id; });

    for (const Candidate candidate : candidates) {
        ObjectMinefieldComponent& component =
            ecs::get<ObjectMinefieldComponent>(registry, candidate.entity);
        if (!component.plan) continue;
        const ObjectFixedTransformComponent* transform =
            ecs::try_get<ObjectFixedTransformComponent>(registry,
                                                        candidate.entity);
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(registry, candidate.entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, candidate.entity);
        const PrimaryTeamComponent* team =
            ecs::try_get<PrimaryTeamComponent>(registry, candidate.entity);
        if (!transform || !transform->authoritative || !type ||
            !type->archetype || !owner || !team) continue;
        const LogicFixedVec3 self = transform->position;
        const PlayerState* player = players.get(owner->player);
        const UpgradeMask playerUpgrades =
            player ? player->upgrades.completed : UpgradeMask{};
        const UpgradeCatalog* upgradeCatalog = content.upgradeCatalog();
        const auto localUpgrades = objectUpgrades(registry, candidate.entity);
        const bool pending = lifecycle.isPendingDestroy(candidate.id);
        const ObjectHealthComponent* selfHealth =
            ecs::try_get<ObjectHealthComponent>(registry, candidate.entity);
        const bool dead = pending || (selfHealth && selfHealth->effectivelyDead);

        for (size_t index = 0; index < component.generators.size() &&
                               index < component.plan->generators.size(); ++index) {
            ObjectGenerateMinefieldRuntime& runtime = component.generators[index];
            const game::ObjectGenerateMinefieldRule& rule =
                component.plan->generators[index];
            if (runtime.lastUpdateTick == confirmedTick) continue;
            runtime.lastUpdateTick = confirmedTick;
            const bool active = generatorActive(
                rule, playerUpgrades, localUpgrades, upgradeCatalog);
            if (!active) continue;
            const bool upgraded = rule.upgradable &&
                !rule.upgradedMineName.empty() &&
                rule.upgradedTriggerId &&
                (upgradeMaskTest(playerUpgrades, rule.upgradedTriggerId) ||
                 upgradeMaskTest(localUpgrades, rule.upgradedTriggerId));
            if (runtime.generated && upgraded && !runtime.upgraded) {
                const auto mineView = ecs::view<const ObjectIdentityComponent,
                                                const ObjectProducerComponent,
                                                const ObjectKindOfComponent,
                                                const ObjectGeneratedMineRecord>(registry);
                container::Vector<ObjectId> old;
                for (const ecs::entity mineEntity : mineView) {
                    const auto& producer =
                        mineView.template get<const ObjectProducerComponent>(mineEntity);
                    const auto& kinds =
                        mineView.template get<const ObjectKindOfComponent>(mineEntity);
                    const auto& generated = mineView.template get<
                        const ObjectGeneratedMineRecord>(mineEntity);
                    if (producer.producer == candidate.id &&
                        generated.generatorIndex == index &&
                        hasKind(&kinds, game::ObjectKindOf::Mine))
                        old.push_back(mineView.template get<const ObjectIdentityComponent>(
                            mineEntity).id);
                }
                std::sort(old.begin(), old.end());
                for (const ObjectId id : old) static_cast<void>(lifecycle.requestDestroy(
                    id, ObjectDestroyReason::System, confirmedTick));
                runtime.generated = false;
            }
            runtime.upgraded = upgraded;
            const bool shouldGenerate = !runtime.generated &&
                (rule.generateOnlyOnDeath ? dead : !dead);
            if (!shouldGenerate) continue;
            const container::String& mineName = upgraded
                ? rule.upgradedMineName : rule.mineName;
            const auto mineArchetype = content.findObjectArchetype(mineName);
            if (!mineArchetype) {
                runtime.generated = true;
                continue;
            }
            const LogicFixedVec3 target = runtime.hasTarget ? runtime.target : self;
            const math::q32_32 distanceAroundObject =
                rule.hasAuthoredDistanceAroundObject
                    ? rule.distanceAroundObject
                    : rules.standardMinefieldDistance;
            const math::q32_32 minesPerSquareFoot =
                rule.hasAuthoredMinesPerSquareFoot
                    ? rule.minesPerSquareFoot
                    : rules.standardMinefieldDensity;
            generateMines(rule, static_cast<uint32_t>(index),
                          distanceAroundObject, minesPerSquareFoot,
                          *type->archetype,
                          *mineArchetype,
                          transform->yawRadians,
                          ecs::try_get<ObjectGeometryComponent>(registry,
                                                                 candidate.entity),
                          candidate.id, owner->player, self, target,
                          registry, terrain, random, confirmedTick,
                          nextEmissionSequence, outSpawns);
            runtime.generated = true;
            emitGeneratorFx(rule, candidate.id, target, confirmedTick,
                            nextEmissionSequence, outFx);
        }

        for (size_t index = 0; index < component.mines.size() &&
                               index < component.plan->mines.size(); ++index) {
            ObjectMinefieldRuntime& runtime = component.mines[index];
            const game::ObjectMinefieldRule& rule = component.plan->mines[index];
            if (runtime.lastUpdateTick == confirmedTick) continue;
            runtime.lastUpdateTick = confirmedTick;

            if (runtime.scooting) {
                const ObjectFixedTransformComponent* mutableTransform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, candidate.entity);
                if (mutableTransform) {
                    LogicFixedVec3 position = mutableTransform->position;
                    const auto snapToHighestSurface = [&](Fixed x, Fixed y) {
                        const game::terrain::TerrainPathfindLayerId layer =
                            terrain.highestPathfindLayerAtXYRaw(
                                x.raw(), y.raw());
                        Fixed floor = Fixed::from_raw(
                            terrain.pathfindLayerHeightRawAt(
                                layer, x.raw(), y.raw()).value_or(
                                    terrain.groundHeightRaw(
                                        x.raw(), y.raw())));
                        if (layer != game::terrain::kGroundPathfindLayer)
                            floor += Fixed{int32_t{1}};
                        ObjectTerrainLayerComponent* terrainLayer =
                            ecs::try_get<ObjectTerrainLayerComponent>(
                                registry, candidate.entity);
                        if (!terrainLayer) {
                            terrainLayer = &ecs::emplace<
                                ObjectTerrainLayerComponent>(
                                registry, candidate.entity,
                                ObjectTerrainLayerComponent{
                                    .pathfindLayer = layer,
                                    .lastChangedTick = confirmedTick,
                                });
                        } else {
                            static_cast<void>(terrainLayer->assign(
                                layer, confirmedTick));
                        }
                        return floor;
                    };
                    if (confirmedTick >= runtime.scootEndTick) {
                        position.x = runtime.scootTarget.x;
                        position.y = runtime.scootTarget.y;
                        position.z = snapToHighestSurface(
                            position.x, position.y);
                        runtime.scooting = false;
                    } else {
                        runtime.scootVelocity.x += runtime.scootAcceleration.x;
                        runtime.scootVelocity.y += runtime.scootAcceleration.y;
                        runtime.scootVelocity.z += runtime.scootAcceleration.z;
                        position.x += runtime.scootVelocity.x;
                        position.y += runtime.scootVelocity.y;
                        position.z += runtime.scootVelocity.z;
                        const Fixed floor = snapToHighestSurface(
                            position.x, position.y);
                        if (position.z < floor) position.z = floor;
                    }
                    writeAuthoritativeObjectPosition(
                        registry, candidate.entity, position);
                }
            }

            runtime.immunities.erase(
                std::remove_if(runtime.immunities.begin(), runtime.immunities.end(),
                    [&](const ObjectMineImmunity& immunity) {
                        return immunity.object &&
                            (!lifecycle.entityFromId(immunity.object) ||
                             confirmedTick > immunity.lastContactTick + 2u);
                    }),
                runtime.immunities.end());
            while (runtime.immunities.size() < 3) runtime.immunities.push_back({});

            if (runtime.regenerates && rule.stopsRegenAfterCreatorDies &&
                confirmedTick >= runtime.nextCreatorDeathCheckTick) {
                runtime.nextCreatorDeathCheckTick = saturatingAdd(
                    confirmedTick, std::max<uint64_t>(1,
                        millisecondsToTicks(rule.creatorDeathCheckMilliseconds,
                                            rules.logicFramesPerSecond)));
                const ObjectProducerComponent* producer =
                    ecs::try_get<ObjectProducerComponent>(registry, candidate.entity);
                if (producer && producer->producer) {
                    const std::optional<ecs::entity> source =
                        lifecycle.entityFromId(producer->producer);
                    const ObjectHealthComponent* sourceHealth = source
                        ? ecs::try_get<ObjectHealthComponent>(registry, *source)
                        : nullptr;
                    if (!source || (sourceHealth && sourceHealth->effectivelyDead)) {
                        runtime.regenerates = false;
                        runtime.draining = true;
                        if (runtime.ownsHealthFloor) {
                            runtime.ownsHealthFloor = false;
                            const bool anotherRegeneratingProvider =
                                std::any_of(
                                    component.mines.begin(),
                                    component.mines.end(),
                                    [](const ObjectMinefieldRuntime& other) {
                                        return other.regenerates &&
                                            other.ownsHealthFloor;
                                    });
                            if (ObjectHealthComponent* mutableHealth =
                                    ecs::try_get<ObjectHealthComponent>(
                                        registry, candidate.entity)) {
                                if (!anotherRegeneratingProvider &&
                                    mutableHealth->minimumHealthFloorFixed ==
                                    kRegeneratingMineHealthFloor) {
                                    mutableHealth->minimumHealthFloorFixed = {};
                                }
                            }
                        }
                        static_cast<void>(
                            ObjectAutoHealSystem::stopFirstAuthored(
                                registry, candidate.entity));
                    }
                }
            }
            if (runtime.draining && selfHealth &&
                rule.healthPercentToDrainPerSecond > Fixed{}) {
                outDamage.push_back({
                    .target = candidate.id, .source = candidate.id,
                    .sourceSequence = runtime.nextShotSequence,
                    .amount = selfHealth->maximumFixed *
                        rule.healthPercentToDrainPerSecond /
                        Fixed{static_cast<int32_t>(
                            std::max<uint32_t>(1, rules.logicFramesPerSecond))},
                    .damageType = game::DamageType::UNRESISTABLE,
                    .deathType = game::DeathType::NORMAL,
                    .confirmedTick = confirmedTick,
                });
            }

            const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(registry, candidate.entity);
            if (health && health->maximumFixed > Fixed{} &&
                health->currentFixed != runtime.previousHealth) {
                const uint32_t boundedMineCount = std::min<uint32_t>(
                    rule.numVirtualMines,
                    static_cast<uint32_t>(
                        std::numeric_limits<int32_t>::max()));
                const Fixed maximumMines{
                    static_cast<int32_t>(boundedMineCount)};
                const Fixed scaled = maximumMines *
                    health->currentFixed / health->maximumFixed;
                const Fixed value = Fixed::clamp(
                    scaled, Fixed{}, maximumMines);
                uint32_t expected = static_cast<uint32_t>(
                    value.raw() >> 32u);
                if (health->currentFixed <= runtime.previousHealth &&
                    (value.raw() & 0xffffffffll) != 0 &&
                    expected < boundedMineCount) {
                    ++expected;
                }
                if (expected > runtime.virtualMinesRemaining) {
                    runtime.virtualMinesRemaining = expected;
                    setMineEmptyVisual(registry, candidate.entity, false,
                                       confirmedTick);
                } else {
                    while (runtime.virtualMinesRemaining > expected) {
                        if (!runtime.draining) queueMineDetonation(
                            registry, candidate.entity, candidate.id, runtime,
                            rule, self, content, random, confirmedTick,
                            nextEmissionSequence, owner->player, team->team,
                            outWeapons, outOcl);
                        --runtime.virtualMinesRemaining;
                    }
                    setMineEmptyVisual(registry, candidate.entity,
                        runtime.virtualMinesRemaining == 0, confirmedTick);
                }
                runtime.previousHealth = health->currentFixed;
            }
            if (pending || runtime.virtualMinesRemaining == 0 ||
                runtime.scooting) continue;

            const ObjectGeometryComponent* mineGeometry =
                ecs::try_get<ObjectGeometryComponent>(registry,
                                                       candidate.entity);
            const Fixed mineRadius = mineGeometry
                ? Fixed::max(kFixedZero,
                             mineGeometry->boundingCircleRadiusFixed)
                : kFixedOne;
            for (const ObjectId other : nearbyObjects(self, mineRadius)) {
                if (!other || other == candidate.id) continue;
                const std::optional<ecs::entity> otherEntity =
                    lifecycle.entityFromId(other);
                if (!otherEntity ||
                    unavailable(registry, lifecycle, other, *otherEntity))
                    continue;
                const ObjectFixedTransformComponent* otherTransform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, *otherEntity);
                if (!otherTransform || !otherTransform->authoritative)
                    continue;
                if (!overlap2D(*transform,
                        mineGeometry, *otherTransform,
                        ecs::try_get<ObjectGeometryComponent>(registry,
                                                               *otherEntity))) continue;
                auto immune = std::find_if(runtime.immunities.begin(),
                    runtime.immunities.end(),
                    [other](const ObjectMineImmunity& value) {
                        return value.object == other;
                    });
                if (immune != runtime.immunities.end()) {
                    immune->lastContactTick = confirmedTick;
                    continue;
                }
                const ObjectKindOfComponent* kinds =
                    ecs::try_get<ObjectKindOfComponent>(registry, *otherEntity);
                if (!rule.workersDetonate &&
                    hasKind(kinds, game::ObjectKindOf::Infantry) &&
                    hasKind(kinds, game::ObjectKindOf::Dozer)) continue;
                if ((rule.detonatedBy & relationshipMask(
                        relationshipBetweenObjects(registry, players,
                                                   candidate.entity,
                                                   *otherEntity))) == 0) continue;
                if (isDisarmingMine(registry, *otherEntity, candidate.id, content)) {
                    auto empty = std::find_if(runtime.immunities.begin(),
                        runtime.immunities.end(),
                        [](const ObjectMineImmunity& value) { return !value.object; });
                    if (empty != runtime.immunities.end())
                        *empty = {.object = other,
                                  .lastContactTick = confirmedTick};
                    continue;
                }
                const LogicFixedVec3 otherPosition = otherTransform->position;
                auto detonator = std::find_if(runtime.detonators.begin(),
                    runtime.detonators.end(), [other](const ObjectMineDetonator& value) {
                        return value.object == other;
                    });
                const Fixed threshold = rule.repeatDetonateMoveThreshold;
                if (detonator != runtime.detonators.end() &&
                    distanceSquared3D(detonator->lastPosition, otherPosition) <=
                        threshold * threshold) continue;
                if (detonator == runtime.detonators.end()) {
                    runtime.detonators.push_back(
                        {.object = other, .lastPosition = otherPosition});
                } else {
                    detonator->lastPosition = otherPosition;
                }
                LogicFixedVec3 detonationPosition = otherPosition;
                if (mineGeometry) {
                    const Fixed radius = Fixed::max(
                        kFixedZero, mineGeometry->boundingCircleRadiusFixed);
                    const Fixed dx = otherPosition.x - self.x;
                    const Fixed dy = otherPosition.y - self.y;
                    const Fixed distance = Fixed::sqrt(dx * dx + dy * dy);
                    if (distance > radius && distance > Fixed{}) {
                        detonationPosition.x = self.x + dx * radius / distance;
                        detonationPosition.y = self.y + dy * radius / distance;
                        detonationPosition.z = self.z;
                    }
                }
                queueMineDetonation(registry, candidate.entity, candidate.id,
                    runtime, rule, detonationPosition, content, random,
                    confirmedTick, nextEmissionSequence, owner->player,
                    team->team, outWeapons, outOcl);
                setMineCount(registry, lifecycle, candidate.entity,
                             candidate.id, runtime, rule,
                             runtime.virtualMinesRemaining - 1u,
                             confirmedTick, outDamage);
                break;
            }
        }

        for (size_t index = 0; index < component.demoTraps.size() &&
                               index < component.plan->demoTraps.size(); ++index) {
            ObjectDemoTrapRuntime& runtime = component.demoTraps[index];
            const game::ObjectDemoTrapRule& rule = component.plan->demoTraps[index];
            if (runtime.lastUpdateTick == confirmedTick || runtime.detonated) continue;
            runtime.lastUpdateTick = confirmedTick;
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
            if (status && status->hasAny(
                    game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
                    game::objectStatusBit(game::ObjectStatusFlag::Sold))) continue;
            if (selfHealth && selfHealth->effectivelyDead) {
                if (rule.detonateWhenKilled) detonateDemoTrap(
                    registry, lifecycle, candidate.entity, candidate.id,
                    runtime, rule, content, random, confirmedTick,
                    nextEmissionSequence, outWeapons, outDamage);
                continue;
            }
            const ObjectWeaponComponent* weapons =
                ecs::try_get<ObjectWeaponComponent>(registry, candidate.entity);
            const std::optional<game::WeaponSlot> current = weapons
                ? weapons->currentSlot : std::nullopt;
            if (current && *current == rule.detonationWeaponSlot) {
                detonateDemoTrap(registry, lifecycle, candidate.entity,
                    candidate.id, runtime, rule, content, random, confirmedTick,
                    nextEmissionSequence, outWeapons, outDamage);
                continue;
            }
            if (!current || *current == rule.manualModeWeaponSlot ||
                confirmedTick < runtime.nextScanTick) continue;
            runtime.nextScanTick = saturatingAdd(
                confirmedTick, std::max<uint64_t>(1,
                    millisecondsToTicks(rule.scanMilliseconds,
                                        rules.logicFramesPerSecond)));
            const Fixed rangeSquared = rule.triggerDetonationRange *
                                       rule.triggerDetonationRange;
            bool enemyNear = false;
            bool friendlyNear = false;
            for (const ObjectId target : nearbyObjects(
                     self, rule.triggerDetonationRange)) {
                if (!target || target == candidate.id) continue;
                const std::optional<ecs::entity> targetEntity =
                    lifecycle.entityFromId(target);
                if (!targetEntity ||
                    unavailable(registry, lifecycle, target, *targetEntity))
                    continue;
                const ObjectFixedTransformComponent* targetTransform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, *targetEntity);
                if (!targetTransform || !targetTransform->authoritative)
                    continue;
                const ObjectKindOfComponent* kinds =
                    ecs::try_get<ObjectKindOfComponent>(registry, *targetEntity);
                if (ignoredByKinds(kinds, rule.ignoreTargetKindMask)) continue;
                if (hasKind(kinds, game::ObjectKindOf::Dozer) &&
                    isDisarmingMine(registry, *targetEntity, candidate.id,
                                    content)) continue;
                const ObjectStatusComponent* targetStatus =
                    ecs::try_get<ObjectStatusComponent>(registry, *targetEntity);
                const ObjectAirborneComponent* airborne =
                    ecs::try_get<ObjectAirborneComponent>(registry, *targetEntity);
                if ((airborne && airborne->isAirborne) ||
                    (targetStatus && targetStatus->hasAny(
                        game::objectStatusBit(game::ObjectStatusFlag::AirborneTarget))))
                    continue;
                const LogicFixedVec3 targetPosition =
                    targetTransform->position;
                if (distanceSquared2D(self, targetPosition) > rangeSquared) continue;
                const PlayerRelationship relation = relationshipBetweenObjects(
                    registry, players, candidate.entity, *targetEntity);
                if (relation == PlayerRelationship::Enemies) enemyNear = true;
                else friendlyNear = true;
                if (enemyNear && rule.friendlyDetonation) break;
            }
            if (enemyNear && (rule.friendlyDetonation || !friendlyNear)) {
                detonateDemoTrap(registry, lifecycle, candidate.entity,
                    candidate.id, runtime, rule, content, random, confirmedTick,
                    nextEmissionSequence, outWeapons, outDamage);
            }
        }
    }
}

bool ObjectMinefieldSystem::setGeneratorTarget(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const LogicFixedVec3* target) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return false;
    ObjectMinefieldComponent* component =
        ecs::try_get<ObjectMinefieldComponent>(registry, *entity);
    if (!component || component->generators.empty()) return false;
    for (ObjectGenerateMinefieldRuntime& runtime : component->generators) {
        runtime.hasTarget = target != nullptr;
        runtime.target = target ? *target : LogicFixedVec3{};
    }
    return true;
}

bool ObjectMinefieldSystem::configureMineScoot(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId mine,
    const LogicFixedVec3& start, const LogicFixedVec3& target,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(mine);
    if (!entity) return false;
    ObjectMinefieldComponent* component =
        ecs::try_get<ObjectMinefieldComponent>(registry, *entity);
    if (!component || !component->plan || component->mines.empty()) return false;
    bool configured = false;
    for (size_t i = 0; i < component->mines.size() &&
                           i < component->plan->mines.size(); ++i) {
        ObjectMinefieldRuntime& runtime = component->mines[i];
        const game::ObjectMinefieldRule& rule = component->plan->mines[i];
        const uint64_t duration = millisecondsToTicks(
            rule.scootMilliseconds, rules.logicFramesPerSecond);
        runtime.scootTarget = target;
        if (duration == 0) continue;
        const Fixed ticks{static_cast<int32_t>(std::min<uint64_t>(
            duration, static_cast<uint64_t>(std::numeric_limits<int32_t>::max())))};
        const Fixed fps{static_cast<int32_t>(
            std::max<uint32_t>(1, rules.logicFramesPerSecond))};
        const Fixed gravityPerTickSquared =
            rules.gravityUnitsPerSecondSq / (fps * fps);
        runtime.scootVelocity = {
            (target.x - start.x) / ticks,
            (target.y - start.y) / ticks,
            ((target.z - start.z) - gravityPerTickSquared * ticks *
                (ticks + Fixed{int32_t{1}}) / Fixed{int32_t{2}}) / ticks,
        };
        runtime.scootAcceleration = {{}, {}, gravityPerTickSquared};
        runtime.scootEndTick = saturatingAdd(confirmedTick, duration);
        runtime.scooting = true;
        writeAuthoritativeObjectPosition(registry, *entity, start);
        if (ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry, *entity)) {
            physics->position = start;
            physics->lastPublishedPosition = start;
            physics->hasAuthoritativePosition = true;
        }
        configured = true;
    }
    return configured;
}

bool ObjectMinefieldSystem::setDemoTrapMode(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId trap, bool proximityMode) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(trap);
    if (!entity) return false;
    ObjectMinefieldComponent* component =
        ecs::try_get<ObjectMinefieldComponent>(registry, *entity);
    if (!component || !component->plan || component->demoTraps.empty()) return false;
    bool changed = false;
    for (size_t i = 0; i < component->demoTraps.size() &&
                           i < component->plan->demoTraps.size(); ++i) {
        ObjectDemoTrapRuntime& runtime = component->demoTraps[i];
        const game::ObjectDemoTrapRule& rule = component->plan->demoTraps[i];
        const bool acquired = setObjectWeaponLock(
            registry, *entity,
            proximityMode ? rule.proximityModeWeaponSlot
                          : rule.manualModeWeaponSlot,
            ObjectWeaponLockType::Temporary);
        changed |= acquired;
        runtime.ownsModeLock = acquired;
        runtime.nextScanTick = 0;
    }
    return changed;
}

bool ObjectMinefieldSystem::triggerDemoTrap(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId trap) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(trap);
    if (!entity) return false;
    ObjectMinefieldComponent* component =
        ecs::try_get<ObjectMinefieldComponent>(registry, *entity);
    if (!component || !component->plan || component->demoTraps.empty()) return false;
    bool changed = false;
    for (size_t i = 0; i < component->demoTraps.size() &&
                           i < component->plan->demoTraps.size(); ++i) {
        changed |= setObjectWeaponLock(
            registry, *entity, component->plan->demoTraps[i].detonationWeaponSlot,
            ObjectWeaponLockType::Temporary);
    }
    return changed;
}

bool ObjectMinefieldSystem::disarmMine(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId mine,
    uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(mine);
    if (!entity) return false;
    ObjectMinefieldComponent* component =
        ecs::try_get<ObjectMinefieldComponent>(registry, *entity);
    if (!component || !component->plan || component->mines.empty()) return false;
    for (size_t i = 0; i < component->mines.size() &&
                           i < component->plan->mines.size(); ++i) {
        ObjectMinefieldRuntime& runtime = component->mines[i];
        const game::ObjectMinefieldRule& rule = component->plan->mines[i];
        if (!runtime.regenerates) {
            static_cast<void>(lifecycle.requestDestroy(
                mine, ObjectDestroyReason::System, confirmedTick));
            continue;
        }
        setMineCount(registry, lifecycle, *entity, mine, runtime, rule, 0,
                     confirmedTick, outDamage);
    }
    return true;
}

} // namespace engine
