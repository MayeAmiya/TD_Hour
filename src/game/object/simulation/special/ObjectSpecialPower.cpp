#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"

#include "game/command/CommandButtonStore.h"
#include "game/base/SimulationRandom.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/contracts/ObjectRelationshipPolicy.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/lifecycle/ObjectCleanupHazard.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/structure/ObjectMissileLauncherBuilding.h"
#include "game/object/simulation/structure/ObjectParticleUplinkCannon.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/world/ObjectSpyVision.h"
#include "game/object/simulation/combat/ObjectTactical.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/terrain/TerrainLogic.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>

namespace engine {
namespace {

using Fixed = math::q32_32;

const Fixed kPlacementZero{};
const Fixed kPlacementOne{int32_t{1}};
const Fixed kPlacementThree{int32_t{3}};
const Fixed kPlacementSix{int32_t{6}};
constexpr Fixed kPlacementPi = Fixed::from_raw(13'493'037'705ll);
const Fixed kPlacementFullTurn = Fixed{int32_t{2}} * kPlacementPi;
const Fixed kPlacementMaximumRadius{int32_t{500}};
const Fixed kPlacementRingSpacing{int32_t{5}};
const Fixed kPlacementRadiusEpsilon = Fixed::from_fraction(1, 1000);

[[nodiscard]] LogicFixedVec3 fixedOrderTarget(
    const ObjectOrderIntent& order) noexcept {
    return {order.targetX, order.targetY, order.targetZ};
}

[[nodiscard]] int32_t ceilPositivePlacement(Fixed value) noexcept {
    if (value <= kPlacementZero) return 0;
    constexpr uint64_t fractionMask = (uint64_t{1} << 32u) - 1u;
    const uint64_t raw = static_cast<uint64_t>(value.raw());
    uint64_t result = raw >> 32u;
    if ((raw & fractionMask) != 0) ++result;
    return static_cast<int32_t>(std::min<uint64_t>(
        result, static_cast<uint64_t>(
            std::numeric_limits<int32_t>::max())));
}

struct Candidate final {
    ObjectId object = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] uint64_t saturatingAdd(uint64_t left,
                                     uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] uint64_t saturatingMultiply(uint64_t left,
                                          uint64_t right) noexcept {
    if (left == 0 || right == 0) return 0;
    return left > std::numeric_limits<uint64_t>::max() / right
        ? std::numeric_limits<uint64_t>::max() : left * right;
}

[[nodiscard]] uint64_t millisecondsToTicks(
    uint64_t milliseconds, uint32_t logicFramesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t framesPerSecond =
        std::max<uint32_t>(1u, logicFramesPerSecond);
    const uint64_t scaled = saturatingMultiply(milliseconds, framesPerSecond);
    return scaled == std::numeric_limits<uint64_t>::max()
        ? scaled : scaled / 1000u + (scaled % 1000u != 0 ? 1u : 0u);
}

[[nodiscard]] bool noScienceRequired(container::StringView science) noexcept {
    return science.empty() || equalInsensitive(science, "None") ||
           equalInsensitive(science, "SCIENCE_INVALID");
}

[[nodiscard]] container::StringView commandButtonSpecialPower(
    const game::CommandButtonTemplate* button) noexcept {
    if (!button) return {};
    for (auto it = button->fields.rbegin(); it != button->fields.rend(); ++it) {
        if (equalInsensitive(it->first, "SpecialPower")) return it->second;
    }
    return {};
}

[[nodiscard]] const SpecialPowerDefinition* resolveRequestedDefinition(
    const GameContentSnapshot& content, container::StringView contentName) noexcept {
    if (const SpecialPowerDefinition* direct = content.findSpecialPower(contentName)) {
        return direct;
    }
    return content.findSpecialPower(commandButtonSpecialPower(
        content.findCommandButton(contentName)));
}

[[nodiscard]] bool unavailableObject(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity, ObjectId object, uint64_t confirmedTick) noexcept {
    if (lifecycle.isPendingDestroy(object)) return true;
    if (const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        health && health->effectivelyDead) {
        return true;
    }
    if (const ObjectMapStatusComponent* map =
            ecs::try_get<ObjectMapStatusComponent>(registry, entity);
        map && map->offMap) {
        return true;
    }
    if (const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        status && status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::UnderConstruction))) {
        return true;
    }
    return isObjectDisabled(registry, entity, confirmedTick);
}

[[nodiscard]] bool playerCanTrackObjectTarget(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players,
    const game::terrain::MapVisibilitySnapshot* visibility,
    PlayerId observer, ObjectId target) noexcept {
    if (!observer || !target || lifecycle.isPendingDestroy(target)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(target);
    if (!entity) return false;

    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, *entity);
    const ObjectMapStatusComponent* map =
        ecs::try_get<ObjectMapStatusComponent>(registry, *entity);
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, *entity);
    const ObjectContainedByComponent* contained =
        ecs::try_get<ObjectContainedByComponent>(registry, *entity);
    if ((health && health->effectivelyDead) ||
        (map && map->offMap) ||
        (status && status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Destroyed))) ||
        (contained && contained->enclosing)) {
        return false;
    }

    const OwnerComponent* targetOwner =
        ecs::try_get<OwnerComponent>(registry, *entity);
    const bool allied = targetOwner && targetOwner->player &&
        (targetOwner->player == observer ||
         players.relationship(observer, targetOwner->player) ==
             PlayerRelationship::Allies);
    if (!allied && status &&
        status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Stealthed)) &&
        !status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Detected)) &&
        !status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Disguised))) {
        return false;
    }

    // The completed map-visibility snapshot is deterministic input to the
    // simulation. Own/allied targets remain trackable; hostile targets must
    // still be visible to the issuing player or one of that player's allies.
    if (allied || !visibility || !visibility->renderingActive) return true;
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, *entity);
    if (!transform) return false;
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, *entity);
    const LogicFixedVec3 position = readAuthoritativeObjectPosition(
        registry, *entity, *transform);
    const Fixed radius = geometry
        ? Fixed::max(kPlacementZero, geometry->boundingCircleRadiusFixed)
        : kPlacementZero;
    if (visibility->footprintHasClearCellRaw(
            observer, position.x.raw(), position.y.raw(), radius.raw())) {
        return true;
    }
    for (const PlayerId ally : players.activePlayerIds()) {
        if (ally != observer &&
            players.relationship(observer, ally) ==
                PlayerRelationship::Allies &&
            visibility->footprintHasClearCellRaw(
                ally, position.x.raw(), position.y.raw(), radius.raw())) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] size_t containedObjectCount(
    ecs::registry& registry, ecs::entity entity,
    bool& hasContainment) noexcept {
    const ObjectContainmentComponent* containment =
        ecs::try_get<ObjectContainmentComponent>(registry, entity);
    hasContainment = containment != nullptr;
    return containment ? containment->objects.size() : 0;
}

[[nodiscard]] LogicFixedVec3 playableEdgePoint(
    const game::terrain::TerrainLogic& terrain,
    const LogicFixedVec3& point, bool farthest) noexcept {
    if (!terrain.isLoaded()) return point;
    const game::terrain::TerrainExtentRaw extent =
        terrain.map().playableExtentRaw();
    const Fixed minimumX = Fixed::from_raw(extent.minimumX);
    const Fixed minimumY = Fixed::from_raw(extent.minimumY);
    const Fixed maximumX = Fixed::from_raw(extent.maximumX);
    const Fixed maximumY = Fixed::from_raw(extent.maximumY);
    Fixed resultX = point.x;
    Fixed resultY = minimumY;
    if (farthest) {
        const Fixed middleX = minimumX +
            (maximumX - minimumX) / Fixed{int32_t{2}};
        const Fixed middleY = minimumY +
            (maximumY - minimumY) / Fixed{int32_t{2}};
        resultX = point.x < middleX ? maximumX : minimumX;
        resultY = point.y < middleY ? maximumY : minimumY;
    } else {
        // Preserve RefCode's strict-tie priority: top, right, bottom, left.
        Fixed best = Fixed::abs(point.y - minimumY);
        const Fixed right = Fixed::abs(point.x - maximumX);
        if (right < best) {
            best = right;
            resultX = maximumX;
            resultY = point.y;
        }
        const Fixed bottom = Fixed::abs(point.y - maximumY);
        if (bottom < best) {
            best = bottom;
            resultX = point.x;
            resultY = maximumY;
        }
        const Fixed left = Fixed::abs(point.x - minimumX);
        if (left < best) {
            resultX = minimumX;
            resultY = point.y;
        }
    }
    return {
        resultX,
        resultY,
        Fixed::from_raw(terrain.groundHeightRaw(
            resultX.raw(), resultY.raw())),
    };
}

[[nodiscard]] bool specialPowerPlacementBlocked(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpatialIndex* spatialIndex, Fixed x, Fixed y,
    container::Vector<ObjectId>& queryScratch) noexcept {
    const Fixed searchSphereRadius{int32_t{5}};
    const auto blocks = [&](ecs::entity entity) noexcept {
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        if (health && health->effectivelyDead) return false;
        const ObjectMapStatusComponent* map =
            ecs::try_get<ObjectMapStatusComponent>(registry, entity);
        if (map && map->offMap) return false;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        return !(status && status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::NoCollisions)));
    };
    if (spatialIndex) {
        const LogicFixedVec3 queryCenter{x, y, kPlacementZero};
        spatialIndex->queryRadiusFixed(
            queryCenter, searchSphereRadius, queryScratch);
        for (const ObjectId object : queryScratch) {
            if (lifecycle.isPendingDestroy(object)) continue;
            const std::optional<ecs::entity> entity =
                lifecycle.entityFromId(object);
            if (entity && blocks(*entity)) return true;
        }
        return false;
    }
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const TransformComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || lifecycle.isPendingDestroy(identity.id) ||
            !blocks(entity)) {
            continue;
        }
        const TransformComponent& transform =
            view.template get<const TransformComponent>(entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, entity);
        const Fixed radius = searchSphereRadius + (geometry
            ? Fixed::max(kPlacementZero,
                  geometry->boundingCircleRadiusFixed)
            : kPlacementOne);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            registry, entity, transform);
        const Fixed dx = position.x - x;
        const Fixed dy = position.y - y;
        if (dx * dx + dy * dy < radius * radius) return true;
    }
    return false;
}

[[nodiscard]] LogicFixedVec3 adjustToPassablePosition(
    const ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSpatialIndex* spatialIndex,
    const navigation::NavigationSystem* navigation,
    SimulationRandom* random,
    const LogicFixedVec3& requested) {
    if (!terrain.isLoaded()) return requested;
    if (!terrain.map().isInsidePlayableRaw(
            requested.x.raw(), requested.y.raw())) return requested;

    const Fixed startAngle = random
        ? random->fixedInclusive(kPlacementZero, kPlacementFullTurn)
        : kPlacementZero;
    container::Vector<ObjectId> placementQueryScratch;
    for (Fixed distance = kPlacementZero;
         distance <= kPlacementMaximumRadius + kPlacementRadiusEpsilon;
         distance += kPlacementRingSpacing) {
        const Fixed angleSpacing = distance == kPlacementZero
            ? kPlacementFullTurn
            : (kPlacementRingSpacing / (distance + kPlacementOne)) *
                  (kPlacementFullTurn / kPlacementSix);
        const int32_t samples = std::max<int32_t>(
            1, ceilPositivePlacement(
                kPlacementThree * (distance + kPlacementOne) /
                kPlacementRingSpacing));
        const auto tryAngle = [&](Fixed angle)
            -> std::optional<LogicFixedVec3> {
            const math::q32_32_sincos direction =
                math::fixed_sincos(angle);
            const Fixed x = requested.x + distance * direction.cosine;
            const Fixed y = requested.y + distance * direction.sine;
            bool navigationClear = true;
            if (navigation && navigation->isInitialized()) {
                const navigation::NavigationGrid& grid = navigation->grid();
                const navigation::NavigationCellId cell = grid.cellAt({
                    .xRaw = x.raw(),
                    .yRaw = y.raw(),
                    .zRaw = 0,
                });
                navigationClear = grid.contains(cell) &&
                    grid.cell(cell).passability ==
                        navigation::NavigationPassability::Traversable;
            }
            if (!terrain.map().isInsidePlayableRaw(x.raw(), y.raw()) ||
                !navigationClear ||
                terrain.isCliffCellRaw(x.raw(), y.raw()) ||
                terrain.isUnderwaterLegacyRaw(x.raw(), y.raw()) ||
                specialPowerPlacementBlocked(
                    registry, lifecycle, spatialIndex, x, y,
                    placementQueryScratch)) {
                return std::nullopt;
            }
            return LogicFixedVec3{
                x, y, Fixed::from_raw(
                    terrain.groundHeightRaw(x.raw(), y.raw()))};
        };
        for (int32_t sample = 0; sample < samples; ++sample) {
            if (const auto found = tryAngle(
                    startAngle + angleSpacing * Fixed{sample})) {
                return *found;
            }
            if (sample != 0) {
                if (const auto found = tryAngle(
                        startAngle - angleSpacing *
                            Fixed{sample})) {
                    return *found;
                }
            }
        }
    }
    return requested;
}

[[nodiscard]] container::StringView selectedObjectCreationList(
    const game::ObjectSpecialPowerRule& rule,
    const PlayerRegistry& players, PlayerId owner) noexcept {
    for (const game::ObjectSpecialPowerUpgradeOcl& upgrade :
         rule.upgradeObjectCreationLists) {
        if (players.hasScience(owner, upgrade.science)) {
            return upgrade.objectCreationList;
        }
    }
    return rule.objectCreationList;
}

void advanceEmissionSequence(uint64_t& sequence) noexcept {
    ++sequence;
    if (sequence == 0) ++sequence;
}

} // namespace

PlayerRelationship relationshipBetweenObjects(
    const ecs::registry& registry, const PlayerRegistry& players,
    ecs::entity source, ecs::entity target) noexcept {
    if (ecs::try_get<ObjectUndetectedDefectorComponent>(registry, source)) {
        return PlayerRelationship::Neutral;
    }
    if (ecs::try_get<ObjectUndetectedDefectorComponent>(registry, target)) {
        return PlayerRelationship::Allies;
    }
    const OwnerComponent* sourceOwner =
        ecs::try_get<OwnerComponent>(registry, source);
    const OwnerComponent* targetOwner =
        ecs::try_get<OwnerComponent>(registry, target);
    if (!sourceOwner || !targetOwner || !sourceOwner->player ||
        !targetOwner->player) {
        return PlayerRelationship::Neutral;
    }
    const PrimaryTeamComponent* targetTeam =
        ecs::try_get<PrimaryTeamComponent>(registry, target);
    const ObjectRelationshipOverrideComponent* sourceOverrides =
        ecs::try_get<ObjectRelationshipOverrideComponent>(registry, source);
    if (sourceOverrides && sourceOverrides->policy) {
        const ObjectRelationshipOverridePolicy& policy =
            *sourceOverrides->policy;
        if (targetTeam && targetTeam->team) {
            const auto teamOverride = std::lower_bound(
                policy.teams.begin(), policy.teams.end(), targetTeam->team,
                [](const ObjectTeamRelationshipOverride& entry,
                   ObjectTeamId value) { return entry.target < value; });
            if (teamOverride != policy.teams.end() &&
                teamOverride->target == targetTeam->team) {
                return teamOverride->relationship;
            }
        }
        const auto playerOverride = std::lower_bound(
            policy.players.begin(), policy.players.end(),
            targetOwner->player,
            [](const ObjectPlayerRelationshipOverride& entry,
               PlayerId value) { return entry.target < value; });
        if (playerOverride != policy.players.end() &&
            playerOverride->target == targetOwner->player) {
            return playerOverride->relationship;
        }
    }
    if (targetTeam && targetTeam->team) {
        if (const std::optional<PlayerRelationship> playerTeam =
                players.teamRelationshipOverride(
                    sourceOwner->player, targetTeam->team)) {
            return *playerTeam;
        }
    }
    return players.relationship(sourceOwner->player, targetOwner->player);
}

PlayerRelationship relationshipBetweenPlayerAndObject(
    const ecs::registry& registry, const PlayerRegistry& players,
    PlayerId source, ecs::entity target) noexcept {
    if (ecs::try_get<ObjectUndetectedDefectorComponent>(registry, target))
        return PlayerRelationship::Allies;
    const OwnerComponent* targetOwner =
        ecs::try_get<OwnerComponent>(registry, target);
    if (!players.get(source) || !targetOwner || !targetOwner->player)
        return PlayerRelationship::Neutral;
    const PrimaryTeamComponent* targetTeam =
        ecs::try_get<PrimaryTeamComponent>(registry, target);
    if (targetTeam && targetTeam->team) {
        if (const std::optional<PlayerRelationship> override =
                players.teamRelationshipOverride(source, targetTeam->team)) {
            return *override;
        }
    }
    return players.relationship(source, targetOwner->player);
}

void ObjectSpecialPowerSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->specialPowerPlan ||
        !owner) {
        return;
    }

    ObjectSpecialPowerComponent value{
        .plan = type->archetype->specialPowerPlan,
    };
    value.instances.reserve(value.plan->rules.size());
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    const bool underConstruction = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
    for (const game::ObjectSpecialPowerRule& rule : value.plan->rules) {
        ObjectSpecialPowerRuntime runtime;
        const SpecialPowerDefinition* definition =
            content.findSpecialPower(rule.specialPowerTemplate);
        if (definition) {
            runtime.content = definition->id;
            runtime.readyTick = underConstruction
                ? std::numeric_limits<uint64_t>::max()
                : definition->sharedSyncedTimer
                    ? confirmedTick
                    : saturatingAdd(confirmedTick, millisecondsToTicks(
                        definition->reloadTimeMilliseconds,
                        rules.logicFramesPerSecond));
        }
        runtime.pausedCount = rule.startsPaused ? 1u : 0u;
        // SpecialPowerModule::pauseCountdown(TRUE) freezes the authored
        // recharge at the frame where the first pause source is installed.
        // Retain that boundary explicitly so UnpauseSpecialPowerUpgrade and
        // script pause sources can share the same reference-counted timer.
        runtime.pauseStartedTick = rule.startsPaused && !underConstruction
            ? confirmedTick : 0;
        value.instances.push_back(runtime);
    }
    if (ObjectSpecialPowerComponent* existing =
            ecs::try_get<ObjectSpecialPowerComponent>(registry, entity)) {
        *existing = std::move(value);
    } else {
        ecs::emplace<ObjectSpecialPowerComponent>(registry, entity,
                                                  std::move(value));
    }

    // A newly created command center joins the owner's already-running
    // shared timer. Query existing ECS instances instead of keeping a hidden
    // player-global mutable map outside lockstep state.
    ObjectSpecialPowerComponent& attached =
        ecs::get<ObjectSpecialPowerComponent>(registry, entity);
    for (ObjectSpecialPowerRuntime& runtime : attached.instances) {
        if (runtime.readyTick == std::numeric_limits<uint64_t>::max()) continue;
        const SpecialPowerDefinition* definition =
            content.findSpecialPower(runtime.content);
        if (!definition || !definition->sharedSyncedTimer) continue;
        uint64_t inheritedReadyTick = confirmedTick;
        const auto view = ecs::view<const OwnerComponent,
                                    const ObjectSpecialPowerComponent>(registry);
        for (const ecs::entity candidate : view) {
            if (candidate == entity) continue;
            const OwnerComponent& candidateOwner =
                view.template get<const OwnerComponent>(candidate);
            if (candidateOwner.player != owner->player) continue;
            const ObjectSpecialPowerComponent& candidatePowers =
                view.template get<const ObjectSpecialPowerComponent>(candidate);
            for (const ObjectSpecialPowerRuntime& candidateRuntime :
                 candidatePowers.instances) {
                if (candidateRuntime.content == runtime.content) {
                    inheritedReadyTick = std::max(
                        inheritedReadyTick, candidateRuntime.readyTick);
                }
            }
        }
        runtime.readyTick = inheritedReadyTick;
    }
}

void ObjectSpecialPowerSystem::synchronizeSharedReadyTick(
    ecs::registry& registry, PlayerId owner,
    SpecialPowerContentId content, uint64_t readyTick) const {
    if (!owner || !content) return;
    const auto view = ecs::view<const OwnerComponent,
                                ObjectSpecialPowerComponent>(registry);
    for (const ecs::entity entity : view) {
        const OwnerComponent& candidateOwner =
            view.template get<const OwnerComponent>(entity);
        if (candidateOwner.player != owner) continue;
        ObjectSpecialPowerComponent& powers =
            view.template get<ObjectSpecialPowerComponent>(entity);
        for (ObjectSpecialPowerRuntime& runtime : powers.instances) {
            if (runtime.content == content) runtime.readyTick = readyTick;
        }
    }
}

bool ObjectSpecialPowerSystem::restartAllRecharge(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const GameContentSnapshot& content,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object)) return false;
    ObjectSpecialPowerComponent* powers =
        ecs::try_get<ObjectSpecialPowerComponent>(registry, *entity);
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
    if (!powers || !owner) return true;

    for (ObjectSpecialPowerRuntime& runtime : powers->instances) {
        const SpecialPowerDefinition* definition =
            content.findSpecialPower(runtime.content);
        if (!definition) continue;
        const uint64_t rechargeStartTick =
            runtime.pausedCount != 0 && !definition->sharedSyncedTimer
                ? runtime.pauseStartedTick : confirmedTick;
        const uint64_t readyTick = saturatingAdd(
            rechargeStartTick, millisecondsToTicks(
                definition->reloadTimeMilliseconds,
                rules.logicFramesPerSecond));
        runtime.readyTick = readyTick;
        if (definition->sharedSyncedTimer) {
            synchronizeSharedReadyTick(registry, owner->player,
                                       definition->id, readyTick);
        }
    }
    // Legacy sabotage succeeds even when the target exposes no special power.
    return true;
}

bool ObjectSpecialPowerSystem::restartRecharge(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, SpecialPowerContentId specialPower,
    const GameContentSnapshot& content, const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object)) return false;
    ObjectSpecialPowerComponent* powers =
        ecs::try_get<ObjectSpecialPowerComponent>(registry, *entity);
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, *entity);
    const SpecialPowerDefinition* definition =
        content.findSpecialPower(specialPower);
    if (!powers || !owner || !definition) return false;
    for (ObjectSpecialPowerRuntime& runtime : powers->instances) {
        if (runtime.content != definition->id) continue;
        const uint64_t rechargeStartTick =
            runtime.pausedCount != 0 && !definition->sharedSyncedTimer
                ? runtime.pauseStartedTick : confirmedTick;
        const uint64_t readyTick = saturatingAdd(
            rechargeStartTick, millisecondsToTicks(
                definition->reloadTimeMilliseconds,
                rules.logicFramesPerSecond));
        runtime.readyTick = readyTick;
        if (definition->sharedSyncedTimer) {
            synchronizeSharedReadyTick(registry, owner->player,
                                       definition->id, readyTick);
        }
        return true;
    }
    return false;
}

bool ObjectSpecialPowerSystem::restartRecharge(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, container::StringView specialPowerTemplate,
    const GameContentSnapshot& content, const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    const SpecialPowerDefinition* definition =
        content.findSpecialPower(specialPowerTemplate);
    if (!definition) return false;
    return restartRecharge(registry, lifecycle, object, definition->id,
                           content, rules, confirmedTick);
}

void ObjectSpecialPowerSystem::onBuildCompleted(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const GameContentSnapshot& content,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object)) return;
    ObjectSpecialPowerComponent* powers =
        ecs::try_get<ObjectSpecialPowerComponent>(registry, *entity);
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
    if (!powers || !powers->plan || !powers->plan->hasSpecialPowerCreate ||
        !owner) return;
    for (ObjectSpecialPowerRuntime& runtime : powers->instances) {
        const SpecialPowerDefinition* definition =
            content.findSpecialPower(runtime.content);
        if (!definition) continue;
        // RefCode SpecialPowerCreate makes shared timers available on build
        // completion; ordinary modules begin their authored recharge.
        const uint64_t readyTick = definition->sharedSyncedTimer
            ? confirmedTick
            : saturatingAdd(confirmedTick, millisecondsToTicks(
                definition->reloadTimeMilliseconds,
                rules.logicFramesPerSecond));
        runtime.readyTick = readyTick;
        // Construction has no active recharge to preserve.  If the module is
        // still StartsPaused, its first meaningful pause boundary is the
        // build-complete frame that starts this recharge.
        if (runtime.pausedCount != 0) {
            runtime.pauseStartedTick = confirmedTick;
        }
        if (definition->sharedSyncedTimer) {
            synchronizeSharedReadyTick(registry, owner->player,
                                       definition->id, readyTick);
        }
    }
}

void ObjectSpecialPowerSystem::updatePassivePlayerEffects(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerRegistry& players, const GameContentSnapshot& content) const {
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const OwnerComponent,
                                const ObjectSpecialPowerComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const OwnerComponent& owner =
            view.template get<const OwnerComponent>(entity);
        const ObjectSpecialPowerComponent& powers =
            view.template get<const ObjectSpecialPowerComponent>(entity);
        if (!identity.id || !owner.player || !powers.plan ||
            lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        const ObjectMapStatusComponent* map =
            ecs::try_get<ObjectMapStatusComponent>(registry, entity);
        if ((health && health->effectivelyDead) || (map && map->offMap)) {
            continue;
        }

        const size_t count = std::min(powers.plan->rules.size(),
                                      powers.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectSpecialPowerRule& rule =
                powers.plan->rules[index];
            if (rule.kind != game::ObjectSpecialPowerKind::CashBounty ||
                rule.bountyPercent <= math::q32_32{}) {
                continue;
            }
            const SpecialPowerDefinition* definition =
                content.findSpecialPower(powers.instances[index].content);
            if (!definition ||
                (!noScienceRequired(definition->requiredScience) &&
                 !players.hasScience(owner.player,
                                     definition->requiredScience))) {
                continue;
            }
            static_cast<void>(players.raiseCashBountyPercent(
                owner.player, rule.bountyPercent));
        }
    }
}

void ObjectSpecialPowerSystem::updateDefectionDetection(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) const {
    container::Vector<ecs::entity> expired;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectUndetectedDefectorComponent>(
        registry);
    expired.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectUndetectedDefectorComponent& state =
            view.template get<const ObjectUndetectedDefectorComponent>(entity);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        const bool firing = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::IsFiringWeapon));
        if (!identity.id || lifecycle.isPendingDestroy(identity.id) ||
            state.detectionEndTick == 0 ||
            confirmedTick >= state.detectionEndTick ||
            (health && health->effectivelyDead) || firing) {
            expired.push_back(entity);
        }
    }
    for (const ecs::entity entity : expired) {
        if (registry.valid(entity)) {
            ecs::remove<ObjectUndetectedDefectorComponent>(registry, entity);
        }
    }
}

void ObjectSpecialPowerSystem::consumeOrders(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerRegistry& players, const GameContentSnapshot& content,
    const ObjectSimulationRules& rules, ObjectSpyVisionSystem& spyVision,
    ObjectCleanupHazardSystem& cleanupHazard, ObjectTacticalSystem& tactical,
    const game::terrain::TerrainLogic& terrain, SimulationRandom* random,
    const ObjectSpatialIndex* spatialIndex,
    const navigation::NavigationSystem* navigation,
    const game::terrain::MapVisibilitySnapshot* visibility,
    uint64_t confirmedTick, uint64_t& nextEmissionSequence,
    container::Vector<ObjectCreationListInvocation>&
        objectCreationListInvocations,
    container::Vector<ObjectDefectionRequest>& defectionRequests,
    container::Vector<ObjectSpecialPowerSpawnRequest>& objectSpawnRequests,
    container::Vector<ObjectSpecialAbilityEffectRequest>&
        specialAbilityEffectRequests,
    container::Vector<ObjectSpecialPowerExecutionEvent>& events) const {
    // SpecialAbilityUpdate's ApproachRequiresLOS needs the same transparency
    // roster the combat frame builds. Gathered lazily so the common tick that
    // admits no ApproachRequiresLOS ability pays nothing.
    container::Vector<uint64_t> seeThroughObstacles;
    bool seeThroughObstaclesLoaded = false;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const OwnerComponent,
                                ObjectSpecialPowerComponent,
                                ObjectOrderQueueComponent>(registry);
    // A queued object-target ability is a live tracking edge, not a captured
    // coordinate. Remove invalid player targets even while an earlier move is
    // still at the head, so death, concealment, or enclosing containment does
    // not leave a stale waypoint that also hides every later queued action.
    // Leave an invalid SpecialPower at the actual head for the normal consumer
    // below so it still publishes one deterministic InvalidObject result.
    for (const ecs::entity entity : view) {
        const OwnerComponent& owner =
            view.template get<const OwnerComponent>(entity);
        ObjectOrderQueueComponent& queue =
            view.template get<ObjectOrderQueueComponent>(entity);
        size_t index = !queue.orders.empty() &&
                queue.orders.front().kind == ObjectOrderKind::SpecialPower
            ? 1u : 0u;
        bool changed = false;
        while (index < queue.orders.size()) {
            const ObjectOrderIntent& order = queue.orders[index];
            const PlayerId observer = players.get(order.contextPlayer)
                ? order.contextPlayer : owner.player;
            if (order.kind == ObjectOrderKind::SpecialPower &&
                order.source == ObjectOrderSource::Player &&
                order.targetObject &&
                !playerCanTrackObjectTarget(
                    registry, lifecycle, players, visibility,
                    observer, order.targetObject)) {
                queue.orders.erase(
                    queue.orders.begin() + static_cast<std::ptrdiff_t>(index));
                changed = true;
                continue;
            }
            ++index;
        }
        if (changed) ++queue.revision;
    }

    container::Vector<Candidate> candidates;
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectOrderQueueComponent& queue =
            view.template get<const ObjectOrderQueueComponent>(entity);
        if (identity.id && !queue.orders.empty() &&
            queue.orders.front().kind == ObjectOrderKind::SpecialPower) {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        ObjectOrderQueueComponent& queue =
            ecs::get<ObjectOrderQueueComponent>(registry, candidate.entity);
        if (queue.orders.empty() ||
            queue.orders.front().kind != ObjectOrderKind::SpecialPower) {
            continue;
        }
        ObjectOrderIntent order = std::move(queue.orders.front());
        queue.orders.erase(queue.orders.begin());
        ++queue.revision;

        const OwnerComponent& owner =
            ecs::get<OwnerComponent>(registry, candidate.entity);
        ObjectSpecialPowerExecutionEvent event{
            .source = candidate.object,
            .player = owner.player,
            .confirmedTick = confirmedTick,
            .sourceSequence = order.sourceSequence,
            .commandSource = order.source,
        };
        const SpecialPowerDefinition* definition =
            resolveRequestedDefinition(content, order.contentName);
        if (!definition) {
            event.status = ObjectSpecialPowerExecutionStatus::MissingDefinition;
            events.push_back(event);
            continue;
        }
        event.content = definition->id;

        if (order.source == ObjectOrderSource::Player &&
            order.targetObject) {
            const PlayerId observer = players.get(order.contextPlayer)
                ? order.contextPlayer : owner.player;
            if (!playerCanTrackObjectTarget(
                    registry, lifecycle, players, visibility,
                    observer, order.targetObject)) {
                event.status =
                    ObjectSpecialPowerExecutionStatus::InvalidObject;
                events.push_back(event);
                continue;
            }
        }

        ObjectSpecialPowerComponent& powers =
            ecs::get<ObjectSpecialPowerComponent>(registry, candidate.entity);
        size_t runtimeIndex = powers.instances.size();
        for (size_t index = 0; index < powers.instances.size(); ++index) {
            if (powers.instances[index].content == definition->id) {
                runtimeIndex = index;
                break;
            }
        }
        if (!powers.plan || runtimeIndex >= powers.instances.size() ||
            runtimeIndex >= powers.plan->rules.size()) {
            event.status = ObjectSpecialPowerExecutionStatus::MissingDefinition;
            events.push_back(event);
            continue;
        }
        ObjectSpecialPowerRuntime& runtime = powers.instances[runtimeIndex];
        const game::ObjectSpecialPowerRule& rule =
            powers.plan->rules[runtimeIndex];
        event.kind = rule.kind;

        if (rule.scriptedOnly && order.source == ObjectOrderSource::Player) {
            event.status = ObjectSpecialPowerExecutionStatus::ScriptOnly;
            events.push_back(event);
            continue;
        }
        if (unavailableObject(registry, lifecycle, candidate.entity,
                              candidate.object, confirmedTick) ||
            runtime.pausedCount != 0) {
            event.status = ObjectSpecialPowerExecutionStatus::Disabled;
            events.push_back(event);
            continue;
        }
        const bool firedByScript =
            order.source == ObjectOrderSource::Script;
        // ZH's named/script CommandButton entry points call the selected
        // SpecialPowerModuleInterface directly with COMMAND_FIRED_BY_SCRIPT.
        // They intentionally bypass Object::doSpecialPower's
        // canUseSpecialPower science/readiness gate, while the module itself
        // still rejects disabled or paused execution above.
        if (!firedByScript &&
            !noScienceRequired(definition->requiredScience) &&
            !players.hasScience(owner.player, definition->requiredScience)) {
            event.status = ObjectSpecialPowerExecutionStatus::MissingScience;
            events.push_back(event);
            continue;
        }
        if (!firedByScript && confirmedTick < runtime.readyTick) {
            event.status = ObjectSpecialPowerExecutionStatus::NotReady;
            event.readyTick = runtime.readyTick;
            events.push_back(event);
            continue;
        }
        if (confirmedTick >= runtime.readyTick &&
            missileLauncherActivationMustWaitForOpenDoor(
                registry, candidate.entity, definition->id)) {
            // MissileLauncherBuildingUpdate reaches DOOR_OPEN in the later
            // sparse-update phase of this same confirmed tick.  Preserve the
            // authoritative order until that transition is observable instead
            // of committing recharge/projectile creation first and flattening
            // DOOR_1_WAITING_OPEN (including its launch bone and steam
            // ParticleSysBone systems) out of the presentation stream.
            queue.orders.insert(queue.orders.begin(), std::move(order));
            ++queue.revision;
            continue;
        }
        if (rule.kind == game::ObjectSpecialPowerKind::Unsupported ||
            rule.kind == game::ObjectSpecialPowerKind::CashBounty) {
            event.status = ObjectSpecialPowerExecutionStatus::UnsupportedEffect;
            events.push_back(event);
            continue;
        }

        const TransformComponent* sourceTransform =
            ecs::try_get<TransformComponent>(registry, candidate.entity);
        LogicFixedVec3 sourcePosition;
        LogicFixedVec3 targetPosition;
        PlayerId cashHackVictim = INVALID_PLAYER_ID;
        const bool hasExplicitTarget = order.targetObject ||
            order.hasTargetPosition;

        // ActionManager's KINDOF_CAPTURABLE admission for the capture and hack
        // families. It sits ahead of the per-kind dispatch so every branch that
        // can carry an object target inherits it, and ahead of the recharge
        // commit below so an illegal target cannot burn the timer.
        const bool needsCapturable =
            game::requiresCapturableTarget(definition->specialPowerType);
        const bool needsHackable =
            game::requiresHackableTarget(definition->specialPowerType);
        if (order.targetObject && (needsCapturable || needsHackable)) {
            const std::optional<ecs::entity> legalityTarget =
                lifecycle.entityFromId(order.targetObject);
            const ObjectKindOfComponent* targetKinds = legalityTarget
                ? ecs::try_get<ObjectKindOfComponent>(
                      registry, *legalityTarget)
                : nullptr;
            const auto targetHasKind =
                [targetKinds](game::ObjectKindOf kind) noexcept {
                    return targetKinds &&
                        game::objectHasKind(targetKinds->mask, kind);
                };
            // Both families first require a building.
            bool legal = legalityTarget &&
                targetHasKind(game::ObjectKindOf::Structure);
            if (legal && needsCapturable) {
                const PlayerRelationship relationship =
                    relationshipBetweenObjects(
                        registry, players, candidate.entity, *legalityTarget);
                legal = relationship == PlayerRelationship::Enemies ||
                    (targetHasKind(game::ObjectKindOf::Capturable) &&
                     relationship != PlayerRelationship::Allies);
            }
            if (legal && needsHackable) {
                legal = !targetHasKind(game::ObjectKindOf::RebuildHole) &&
                    (targetHasKind(game::ObjectKindOf::Capturable) ||
                     (game::hackableViaTechFactionException(
                          definition->specialPowerType) &&
                      targetHasKind(game::ObjectKindOf::FsTechnology) &&
                      !targetHasKind(game::ObjectKindOf::ImmuneToCapture)));
            }
            if (!legal) {
                event.status =
                    ObjectSpecialPowerExecutionStatus::InvalidObject;
                events.push_back(event);
                continue;
            }
        }

        // Only the kinds below resolve `targetPosition`; every other kind
        // leaves it at the origin. The superweapon view object must never
        // reveal (0,0,0) instead of the impact area, so it consults this flag
        // rather than hasExplicitTarget alone.
        bool targetPositionResolved = false;
        if (rule.kind == game::ObjectSpecialPowerKind::ObjectCreationList ||
            rule.kind == game::ObjectSpecialPowerKind::FireWeapon ||
            rule.kind == game::ObjectSpecialPowerKind::ParticleUplink ||
            rule.kind == game::ObjectSpecialPowerKind::SpecialAbility) {
            if (!sourceTransform) {
                event.status = ObjectSpecialPowerExecutionStatus::InvalidObject;
                events.push_back(event);
                continue;
            }
            sourcePosition = readAuthoritativeObjectPosition(
                registry, candidate.entity, *sourceTransform);
            targetPosition = sourcePosition;
            targetPositionResolved = true;
            if (order.targetObject) {
                const std::optional<ecs::entity> targetEntity =
                    lifecycle.entityFromId(order.targetObject);
                const TransformComponent* targetTransform = targetEntity
                    ? ecs::try_get<TransformComponent>(registry, *targetEntity)
                    : nullptr;
                const ObjectHealthComponent* targetHealth = targetEntity
                    ? ecs::try_get<ObjectHealthComponent>(registry,
                                                          *targetEntity)
                    : nullptr;
                const ObjectMapStatusComponent* targetMap = targetEntity
                    ? ecs::try_get<ObjectMapStatusComponent>(registry,
                                                             *targetEntity)
                    : nullptr;
                if (!targetEntity || !targetTransform ||
                    lifecycle.isPendingDestroy(order.targetObject) ||
                    (targetHealth && targetHealth->effectivelyDead) ||
                    (targetMap && targetMap->offMap)) {
                    event.status =
                        ObjectSpecialPowerExecutionStatus::InvalidObject;
                    events.push_back(event);
                    continue;
                }
                targetPosition = readAuthoritativeObjectPosition(
                    registry, *targetEntity, *targetTransform);
            } else if (order.hasTargetPosition) {
                targetPosition = fixedOrderTarget(order);
            }
        } else if (rule.kind == game::ObjectSpecialPowerKind::CashHack) {
            const std::optional<ecs::entity> targetEntity =
                lifecycle.entityFromId(order.targetObject);
            const OwnerComponent* targetOwner = targetEntity
                ? ecs::try_get<OwnerComponent>(registry, *targetEntity)
                : nullptr;
            const ObjectHealthComponent* targetHealth = targetEntity
                ? ecs::try_get<ObjectHealthComponent>(registry, *targetEntity)
                : nullptr;
            const ObjectMapStatusComponent* targetMap = targetEntity
                ? ecs::try_get<ObjectMapStatusComponent>(registry,
                                                         *targetEntity)
                : nullptr;
            if (!order.targetObject || !targetEntity || !targetOwner ||
                !players.get(targetOwner->player) ||
                lifecycle.isPendingDestroy(order.targetObject) ||
                (targetHealth && targetHealth->effectivelyDead) ||
                (targetMap && targetMap->offMap)) {
                event.status = ObjectSpecialPowerExecutionStatus::InvalidObject;
                events.push_back(event);
                continue;
            }
            cashHackVictim = targetOwner->player;
        } else if (rule.kind == game::ObjectSpecialPowerKind::Defector) {
            // RefCode validates only the target pointer before its base
            // SpecialPower consumes recharge. Contained/building/sold/
            // same-default-team rejection belongs to the later defect
            // transaction and intentionally remains a consumed no-op.
            if (!order.targetObject ||
                !lifecycle.entityFromId(order.targetObject)) {
                event.status = ObjectSpecialPowerExecutionStatus::InvalidObject;
                events.push_back(event);
                continue;
            }
        } else if (rule.kind == game::ObjectSpecialPowerKind::CleanupArea) {
            // CleanupAreaPower exposes only the location entry point.  Its
            // RefCode override intentionally does not invoke the base module,
            // so malformed/object targets cannot consume recharge.
            if (!order.hasTargetPosition || order.targetObject) {
                event.status = ObjectSpecialPowerExecutionStatus::InvalidObject;
                events.push_back(event);
                continue;
            }
            targetPosition = fixedOrderTarget(order);
            if (!ecs::try_get<ObjectCleanupHazardComponent>(
                    registry, candidate.entity)) {
                event.status = ObjectSpecialPowerExecutionStatus::MissingUpdate;
                events.push_back(event);
                continue;
            }

            // Unlike ordinary SpecialPowerModule subclasses, the original
            // cleanup command does not call base initiation/recharge.  Keep
            // that observable semantic while still consuming this queue item.
            event.status = cleanupHazard.activateArea(
                registry, lifecycle, candidate.object, targetPosition,
                rule.maxMoveDistanceFromLocation,
                rules.logicFramesPerSecond, confirmedTick)
                ? ObjectSpecialPowerExecutionStatus::Activated
                : ObjectSpecialPowerExecutionStatus::MissingUpdate;
            events.push_back(event);
            continue;
        } else if (rule.kind == game::ObjectSpecialPowerKind::BaikonurLaunch) {
            // The two legacy entry points are distinct: the parameterless
            // scripted launch opens the door, while the location form creates
            // the detonation helper. Object targets were never supported.
            if (order.targetObject ||
                (order.hasTargetPosition && rule.detonationObject.empty())) {
                event.status = ObjectSpecialPowerExecutionStatus::InvalidObject;
                events.push_back(event);
                continue;
            }
        }

        bool spectreGunshipUpdate = false;
        bool spectreGunshipDefinitionsReady = true;
        const game::ObjectSpectreDeploymentRule* spectreDeployment = nullptr;
        bool spectreDeploymentMatched = false;
        if (rule.kind == game::ObjectSpecialPowerKind::SpecialAbility) {
            if (const ObjectAirfieldComponent* airfield =
                    ecs::try_get<ObjectAirfieldComponent>(
                        registry, candidate.entity);
                airfield && airfield->plan) {
                for (const game::ObjectSpectreGunshipRule& spectre :
                     airfield->plan->spectreGunships) {
                    if (!equalInsensitive(
                            spectre.specialPowerTemplate,
                            rule.specialPowerTemplate)) {
                        continue;
                    }
                    spectreGunshipUpdate = true;
                    spectreGunshipDefinitionsReady =
                        spectreGunshipDefinitionsReady &&
                        !spectre.gattlingTemplateName.empty() &&
                        content.findObjectArchetype(
                            spectre.gattlingTemplateName) != nullptr;
                }
                // RefCode walks same-template deployment modules in authored
                // order (level 3 -> 2 -> 1) and chooses the first module whose
                // extra science is owned. A name-only find_if incorrectly
                // rejected level-1 players on the level-3 rule.
                for (const game::ObjectSpectreDeploymentRule& deployment :
                     airfield->plan->spectreDeployments) {
                    if (!equalInsensitive(
                            deployment.specialPowerTemplate,
                            rule.specialPowerTemplate)) {
                        continue;
                    }
                    spectreDeploymentMatched = true;
                    if (deployment.requiredScience.empty() ||
                        players.hasScience(owner.player,
                                           deployment.requiredScience)) {
                        spectreDeployment = &deployment;
                        break;
                    }
                }
            }
        }
        const bool spectreSpecialAbility =
            spectreGunshipUpdate || spectreDeploymentMatched;
        if (spectreSpecialAbility &&
            (!order.hasTargetPosition || order.targetObject)) {
            event.status = ObjectSpecialPowerExecutionStatus::InvalidObject;
            events.push_back(event);
            continue;
        }
        if (spectreDeploymentMatched && !spectreDeployment) {
            event.status = ObjectSpecialPowerExecutionStatus::MissingScience;
            events.push_back(event);
            continue;
        }
        if ((spectreDeployment &&
             (spectreDeployment->gunshipTemplateName.empty() ||
              !content.findObjectArchetype(
                  spectreDeployment->gunshipTemplateName))) ||
            (spectreGunshipUpdate && !spectreGunshipDefinitionsReady)) {
            // Deployment and the gunship's structural gattling child are
            // synchronous parts of the authored Spectre activation. Reject a
            // malformed content graph before committing recharge instead of
            // publishing Activated and discovering the missing object later
            // in the spawn transaction.
            event.status = ObjectSpecialPowerExecutionStatus::MissingDefinition;
            events.push_back(event);
            continue;
        }
        if (spectreSpecialAbility) {
            const PrimaryTeamComponent* primaryTeam =
                ecs::try_get<PrimaryTeamComponent>(registry,
                                                   candidate.entity);
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(registry,
                                                 candidate.entity);
            if (!primaryTeam || !primaryTeam->team || !transform) {
                event.status =
                    ObjectSpecialPowerExecutionStatus::MissingDefinition;
                events.push_back(event);
                continue;
            }
        }
        if (spectreDeployment)
            event.updateAuthoredOrder = spectreDeployment->authoredOrder;

        bool deferRechargeUntilPreparation = false;
        if (rule.kind == game::ObjectSpecialPowerKind::SpecialAbility &&
            !spectreSpecialAbility) {
            if (!seeThroughObstaclesLoaded) {
                gatherObjectSeeThroughObstacles(
                    registry, seeThroughObstacles);
                seeThroughObstaclesLoaded = true;
            }
            const ObjectSpecialAbilityAdmission admission =
                tactical.admitSpecialAbility(
                    registry, lifecycle, candidate.object, *definition,
                    content, order, visibility, navigation,
                    rules.ai.attackUsesLineOfSight, seeThroughObstacles);
            if (admission.status ==
                ObjectSpecialAbilityAdmissionStatus::Approach) {
                // The original command remains authoritative and is retried
                // after locomotion consumes this one typed approach intent.
                // Recharge begins only when the retry is actually admitted.
                const ObjectId approachTarget = order.targetObject;
                queue.orders.insert(queue.orders.begin(), std::move(order));
                if (queue.orders.size() >=
                    ObjectOrderQueueComponent::MaximumQueuedOrders) {
                    queue.orders.pop_back();
                }
                ObjectOrderIntent approach{
                    .kind = ObjectOrderKind::Move,
                    .source = ObjectOrderSource::System,
                    .contextPlayer = owner.player,
                    .issuedTick = confirmedTick,
                    .sourceSequence = rule.authoredOrder,
                    .targetObject = approachTarget,
                    .targetX = admission.approachPosition.x,
                    .targetY = admission.approachPosition.y,
                    .targetZ = admission.approachPosition.z,
                    .hasTargetPosition = true,
                    .systemPurpose = ObjectOrderSystemPurpose::SpecialAbility,
                    .systemPurposeInstance = admission.ruleIndex,
                };
                queue.orders.insert(queue.orders.begin(), std::move(approach));
                ++queue.revision;
                event.status = ObjectSpecialPowerExecutionStatus::Approaching;
                events.push_back(event);
                continue;
            }
            if (admission.status ==
                ObjectSpecialAbilityAdmissionStatus::Rejected) {
                event.status = ObjectSpecialPowerExecutionStatus::MissingUpdate;
                events.push_back(event);
                continue;
            }
            deferRechargeUntilPreparation =
                rule.updateModuleStartsAttack &&
                admission.supportsDeferredRecharge;
            event.scriptTriggered = !deferRechargeUntilPreparation;
        }

        if (rule.kind == game::ObjectSpecialPowerKind::SpecialAbility &&
            hasExplicitTarget) {
            event.targetPosition = targetPosition;
            event.hasTargetPosition = true;
        }

        const uint64_t nextReadyTick = saturatingAdd(
            confirmedTick, millisecondsToTicks(
                definition->reloadTimeMilliseconds,
                rules.logicFramesPerSecond));
        if (!deferRechargeUntilPreparation)
            runtime.readyTick = nextReadyTick;
        ++runtime.activationSequence;
        if (runtime.activationSequence == 0) ++runtime.activationSequence;
        if (!deferRechargeUntilPreparation &&
            definition->sharedSyncedTimer) {
            synchronizeSharedReadyTick(registry, owner.player,
                                       definition->id, nextReadyTick);
        }
        event.readyTick = deferRechargeUntilPreparation
            ? runtime.readyTick : nextReadyTick;
        // Successfully executing any SpecialPower blows an undetected
        // defector's cover. This is object-local and must not rewrite player
        // diplomacy.
        ecs::remove<ObjectUndetectedDefectorComponent>(registry,
                                                        candidate.entity);

        if (rule.kind == game::ObjectSpecialPowerKind::SpecialAbility) {
            if (spectreSpecialAbility && event.hasTargetPosition) {
                event.status = ObjectSpecialPowerExecutionStatus::Activated;
            } else {
                event.status = tactical.activateSpecialAbility(
                    registry, lifecycle, candidate.object, *definition, order,
                    content, rules, confirmedTick, nextEmissionSequence,
                    specialAbilityEffectRequests, &players, random,
                    deferRechargeUntilPreparation)
                    ? ObjectSpecialPowerExecutionStatus::Activated
                    : ObjectSpecialPowerExecutionStatus::MissingUpdate;
            }
        } else if (rule.kind == game::ObjectSpecialPowerKind::BaikonurLaunch) {
            if (order.hasTargetPosition) {
                const PrimaryTeamComponent* team =
                    ecs::try_get<PrimaryTeamComponent>(registry,
                                                       candidate.entity);
                if (!team || !team->team ||
                    !content.findObjectArchetype(rule.detonationObject)) {
                    event.status =
                        ObjectSpecialPowerExecutionStatus::MissingDefinition;
                    events.push_back(event);
                    continue;
                }
                objectSpawnRequests.push_back({
                    .source = candidate.object,
                    .owner = owner.player,
                    .primaryTeam = team->team,
                    .objectTemplate = rule.detonationObject,
                    .position = fixedOrderTarget(order),
                    .authoredOrder = rule.authoredOrder,
                    .emissionSequence = nextEmissionSequence,
                    .confirmedTick = confirmedTick,
                });
                advanceEmissionSequence(nextEmissionSequence);
            } else {
                publishObjectModelConditionDoor(
                    registry, candidate.entity,
                    ObjectModelConditionDoorSource::SpecialPower, 0,
                    ObjectModelConditionDoorPhase::Opening,
                    confirmedTick, rule.authoredOrder);
            }
            event.status = ObjectSpecialPowerExecutionStatus::Activated;
        } else if (rule.kind == game::ObjectSpecialPowerKind::SpyVision) {
            bool hasContainment = false;
            const size_t contained = containedObjectCount(
                registry, candidate.entity, hasContainment);
            uint64_t durationMilliseconds = saturatingAdd(
                rule.baseDurationMilliseconds,
                saturatingMultiply(contained,
                    rule.bonusDurationPerCapturedMilliseconds));
            if (hasContainment) {
                durationMilliseconds = std::min<uint64_t>(
                    durationMilliseconds, rule.maximumDurationMilliseconds);
            }
            event.durationTicks = millisecondsToTicks(
                durationMilliseconds, rules.logicFramesPerSecond);
            // SpyVision overrides only the parameterless entry point.  The
            // inherited object/location forms still consume base recharge,
            // but must not activate SpyVisionUpdate.
            if (hasExplicitTarget) {
                event.status = ObjectSpecialPowerExecutionStatus::Activated;
            } else {
                event.status = spyVision.activateForTicks(
                    registry, lifecycle, candidate.object,
                    event.durationTicks, confirmedTick)
                    ? ObjectSpecialPowerExecutionStatus::Activated
                    : ObjectSpecialPowerExecutionStatus::MissingUpdate;
            }
        } else if (rule.kind ==
                       game::ObjectSpecialPowerKind::ObjectCreationList) {
            const container::StringView selectedName =
                selectedObjectCreationList(rule, players, owner.player);
            const game::ObjectCreationListContentId selected =
                content.findObjectCreationListId(selectedName);
            if (!selected) {
                // Without this the module reports Activated, consumes the
                // authored recharge and produces nothing observable; the
                // superweapon then looks like a broken effect chain.
                TD_LOG_ERROR(
                    "[ObjectSpecialPower] OCL activation source={} tick={} module='{}' resolved no ObjectCreationList (authored='{}'); recharge consumed with no effect",
                    candidate.object.value, confirmedTick, rule.moduleTag,
                    selectedName);
            }
            if (!selectedName.empty() && !selected) {
                event.status =
                    ObjectSpecialPowerExecutionStatus::MissingDefinition;
                events.push_back(event);
                continue;
            }
            if (selected) {
                LogicFixedVec3 adjustedTarget = targetPosition;
                if (hasExplicitTarget && rule.adjustPositionToPassable) {
                    adjustedTarget = adjustToPassablePosition(
                        registry, lifecycle, terrain, spatialIndex,
                        navigation, random, targetPosition);
                }
                LogicFixedVec3 creation = sourcePosition;
                bool createDeliveryOwner = false;
                if (hasExplicitTarget) {
                    createDeliveryOwner = true;
                    switch (rule.createLocation) {
                    case game::ObjectSpecialPowerCreateLocation::EdgeNearSource:
                        creation = playableEdgePoint(
                            terrain, sourcePosition, false);
                        break;
                    case game::ObjectSpecialPowerCreateLocation::EdgeNearTarget:
                        creation = playableEdgePoint(
                            terrain, adjustedTarget, false);
                        break;
                    case game::ObjectSpecialPowerCreateLocation::AtLocation:
                        creation = adjustedTarget;
                        break;
                    case game::ObjectSpecialPowerCreateLocation::UseOwnerObject:
                        creation = adjustedTarget;
                        createDeliveryOwner = false;
                        break;
                    case game::ObjectSpecialPowerCreateLocation::AboveLocation:
                        creation = adjustedTarget;
                        creation.z += math::q32_32{int32_t{300}};
                        break;
                    case game::ObjectSpecialPowerCreateLocation::EdgeFarthestFromTarget:
                        creation = playableEdgePoint(
                            terrain, adjustedTarget, true);
                        creation.z += math::q32_32{int32_t{300}};
                        break;
                    }
                }
                const PrimaryTeamComponent* team =
                    ecs::try_get<PrimaryTeamComponent>(registry,
                                                       candidate.entity);
                if (!team || !team->team) {
                    event.status =
                        ObjectSpecialPowerExecutionStatus::InvalidObject;
                    events.push_back(event);
                    continue;
                }
                LogicFixedVec3 velocity;
                if (const ObjectPhysicsComponent* physics =
                        ecs::try_get<ObjectPhysicsComponent>(
                            registry, candidate.entity)) {
                    velocity = physics->velocityUnitsPerSecond;
                }
                game::ObjectVeterancyLevel veterancy =
                    game::ObjectVeterancyLevel::Regular;
                if (const ObjectVeterancyComponent* state =
                        ecs::try_get<ObjectVeterancyComponent>(
                            registry, candidate.entity)) {
                    veterancy = state->level;
                }
                const ObjectAirborneComponent* airborne =
                    ecs::try_get<ObjectAirborneComponent>(
                        registry, candidate.entity);
                const ObjectTerrainLayerComponent* terrainLayer =
                    ecs::try_get<ObjectTerrainLayerComponent>(
                        registry, candidate.entity);
                objectCreationListInvocations.push_back({
                    .content = selected,
                    .source = candidate.object,
                    .owner = owner.player,
                    .primaryTeam = team->team,
                    .primaryPosition = creation,
                    .secondaryPosition = adjustedTarget,
                    .sourceVelocity = velocity,
                    // The command angle is lockstep input. Zero is a valid
                    // world-space yaw and must not be treated as "absent".
                    .orientationRadians = order.placementYawRadians,
                    .veterancy = veterancy,
                    .authoredOrder = rule.authoredOrder,
                    .emissionSequence = nextEmissionSequence,
                    .confirmedTick = confirmedTick,
                    .sourcePathfindLayer = terrainLayer
                        ? terrainLayer->pathfindLayer
                        : game::terrain::kGroundPathfindLayer,
                    .hasSecondaryPosition = true,
                    .sourceAirborne = airborne && airborne->isAirborne,
                    .createDeliveryOwner = createDeliveryOwner,
                });
                advanceEmissionSequence(nextEmissionSequence);
            }
            event.status = ObjectSpecialPowerExecutionStatus::Activated;
        } else if (rule.kind ==
                       game::ObjectSpecialPowerKind::FireWeapon) {
            static_cast<void>(reloadAllObjectWeaponsNow(
                registry, candidate.entity, content, confirmedTick,
                rules.logicFramesPerSecond));
            ObjectOrderIntent attack{
                .kind = ObjectOrderKind::Attack,
                .source = ObjectOrderSource::System,
                .contextPlayer = owner.player,
                .issuedTick = confirmedTick,
                .sourceSequence = static_cast<uint32_t>(
                    std::min<uint64_t>(runtime.activationSequence,
                                       UINT32_MAX)),
                .targetObject = order.targetObject,
                .targetX = targetPosition.x,
                .targetY = targetPosition.y,
                .targetZ = targetPosition.z,
                .hasTargetPosition = !order.targetObject,
                .maximumShots = rule.maximumShotsToFire,
                .systemPurpose =
                    ObjectOrderSystemPurpose::SpecialAbility,
                .systemPurposeInstance = rule.authoredOrder,
            };
            queue.orders.insert(queue.orders.begin(), std::move(attack));
            ++queue.revision;
            event.status = ObjectSpecialPowerExecutionStatus::Activated;
        } else if (rule.kind == game::ObjectSpecialPowerKind::CashHack) {
            uint32_t desired = rule.moneyAmount;
            for (const game::ObjectSpecialPowerUpgradeMoney& upgrade :
                 rule.upgradeMoneyAmounts) {
                if (players.hasScience(owner.player, upgrade.science)) {
                    desired = upgrade.amount;
                    break;
                }
            }
            const PlayerState* victim = players.get(cashHackVictim);
            const PlayerState* receiver = players.get(owner.player);
            // RefCode does not relationship-filter this entry point.  A
            // same-owner target therefore withdraws and redeposits the cash
            // (net zero) but still records it as money earned.
            if (victim && receiver && desired != 0) {
                const int64_t capacity = cashHackVictim == owner.player
                    ? std::numeric_limits<int64_t>::max()
                    : std::numeric_limits<int64_t>::max() - receiver->cash;
                const int64_t stolen = std::min({
                    victim->cash, static_cast<int64_t>(desired), capacity});
                if (stolen > 0 && players.trySpend(cashHackVictim, stolen)) {
                    if (players.adjustCash(owner.player, stolen)) {
                        static_cast<void>(players.recordMoneyEarned(
                            owner.player, static_cast<uint64_t>(stolen),
                            confirmedTick));
                        event.moneyAmount = stolen;
                    }
                }
            }
            event.status = ObjectSpecialPowerExecutionStatus::Activated;
        } else if (rule.kind == game::ObjectSpecialPowerKind::Defector) {
            defectionRequests.push_back({
                .source = candidate.object,
                .target = order.targetObject,
                .newOwner = owner.player,
                .detectionDurationTicks = millisecondsToTicks(
                    definition->detectionTimeMilliseconds,
                    rules.logicFramesPerSecond),
                .authoredOrder = rule.authoredOrder,
                .submissionOrdinal = nextEmissionSequence++,
                .confirmedTick = confirmedTick,
            });
            if (nextEmissionSequence == 0) ++nextEmissionSequence;
            event.status = ObjectSpecialPowerExecutionStatus::Activated;
        } else if (rule.kind ==
                       game::ObjectSpecialPowerKind::ParticleUplink) {
            event.status = ObjectSpecialPowerExecutionStatus::Activated;
        }
        if (event.status ==
            ObjectSpecialPowerExecutionStatus::Activated) {
            static_cast<void>(players.recordAcademySpecialPower(
                owner.player,
                equalInsensitive(definition->academyClassification,
                                 "ACT_SUPERPOWER")));
            notifyMissileLauncherSpecialPowerActivated(
                registry, candidate.entity, definition->id, confirmedTick);
            if (rule.kind ==
                game::ObjectSpecialPowerKind::ParticleUplink) {
                notifyParticleUplinkSpecialPowerActivated(
                    registry, candidate.entity, definition->id,
                    order.source, order.targetObject, targetPosition,
                    runtime.activationSequence, confirmedTick);
            }

            // RefCode triggerSpecialPower -> createViewObject. 33 of 79
            // SpecialPower entries author ViewObjectRange/ViewObjectDuration;
            // without this the Particle Cannon, Nuke, Scud Storm, A-10 strike,
            // Carpet Bomb, Artillery Barrage, Anthrax Bomb and Paradrop all
            // land inside the shroud and the firing player sees nothing.
            //
            // The named object's own ShroudClearingRange is overridden with
            // the authored ViewObjectRange and its DeletionUpdate deadline is
            // re-armed to ViewObjectDuration, exactly as RefCode does via
            // setShroudClearingRange()/setLifetimeRange(). GameSession remains
            // the object factory, so this only publishes a value request and
            // consumes no SimulationRandom draw.
            if (targetPositionResolved &&
                definition->viewObjectDurationMilliseconds != 0 &&
                definition->viewObjectRange > math::q32_32{} &&
                !rules.specialPowerViewObject.empty()) {
                objectSpawnRequests.push_back({
                    .source = candidate.object,
                    .owner = owner.player,
                    .objectTemplate = rules.specialPowerViewObject,
                    .position = targetPosition,
                    .shroudClearingRange = definition->viewObjectRange,
                    .viewObjectLifetimeFrames = static_cast<uint32_t>(
                        std::min<uint64_t>(
                            millisecondsToTicks(
                                definition->viewObjectDurationMilliseconds,
                                rules.logicFramesPerSecond),
                            std::numeric_limits<uint32_t>::max())),
                    .specialPower = definition->id,
                    .authoredOrder = rule.authoredOrder,
                    .emissionSequence = nextEmissionSequence,
                    .confirmedTick = confirmedTick,
                });
                advanceEmissionSequence(nextEmissionSequence);
            }
        }
        events.push_back(event);
    }
}

} // namespace engine
