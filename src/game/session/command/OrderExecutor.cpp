#include "core/container/container_types.h"
#include "OrderExecutor.h"

#include "game/object/contracts/ObjectTeamRegistry.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/ai/runtime/ObjectAIRuntime.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "math/fixed/fixed_raw_mean.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"

#include <algorithm>
#include <bit>
#include <intrin.h>
#include <utility>

namespace engine {
namespace {

constexpr size_t kMaximumPlayerOrderActors = 512;
// Scenario scripts are trusted map content rather than a network packet or
// UI selection. Keep a large explicit safety ceiling for malformed maps, but
// do not inherit the player's 512-unit wire/selection limit for a cinematic
// or reinforcement Team.
constexpr size_t kMaximumScenarioOrderActors = 65536;

[[nodiscard]] bool hasDirectAttackOrderConsumer(
    const ai::ObjectAIOrderCapabilitySnapshot& capabilities,
    ObjectId subject) noexcept {
    return capabilities.has(
        subject, ai::ObjectAIOrderCapability::Attack);
}

[[nodiscard]] bool hasMoveStopOrderConsumer(
    const ai::ObjectAIOrderCapabilitySnapshot& capabilities,
    ObjectId subject) noexcept {
    return capabilities.has(
        subject, ai::ObjectAIOrderCapability::MoveStop);
}

[[nodiscard]] bool playerWaypointPathMatchesQueue(
    const ObjectSystemPathSequenceComponent& path,
    const ObjectOrderQueueComponent& queue) noexcept {
    if (path.routeSubtype != ObjectMoveRouteSubtype::FollowPath ||
        path.source != ObjectOrderSource::Player ||
        path.systemPurpose != ObjectOrderSystemPurpose::Generic ||
        path.queuedOrderCount == 0 ||
        path.queuedOrderCount != path.points.size() ||
        path.queuedOrderCount > queue.orders.size()) {
        return false;
    }
    for (uint32_t index = 0; index < path.queuedOrderCount; ++index) {
        const ObjectOrderIntent& order = queue.orders[index];
        const LogicFixedVec3 point = path.points[index];
        if (order.kind != ObjectOrderKind::Move ||
            order.source != ObjectOrderSource::Player ||
            order.systemPurpose != ObjectOrderSystemPurpose::Generic ||
            order.moveRouteSubtype !=
                (index == 0 ? ObjectMoveRouteSubtype::FollowPath
                            : ObjectMoveRouteSubtype::Direct) ||
            !order.hasTargetPosition || order.targetX != point.x ||
            order.targetY != point.y || order.targetZ != point.z) {
            return false;
        }
    }
    return queue.orders.front().issuedTick == path.issuedTick &&
        queue.orders.front().sourceSequence == path.firstSourceSequence;
}

void appendPlayerWaypointMove(
    ecs::registry& registry, ecs::entity entity,
    ObjectOrderQueueComponent& queue, ObjectOrderIntent order) {
    ObjectSystemPathSequenceComponent* path =
        ecs::try_get<ObjectSystemPathSequenceComponent>(registry, entity);
    if (path && !playerWaypointPathMatchesQueue(*path, queue)) {
        ecs::remove<ObjectSystemPathSequenceComponent>(registry, entity);
        path = nullptr;
    }

    ObjectSystemPathSequenceComponent fresh;
    if (!path) {
        // privateFollowPathAppend preserves the current goal only when the
        // actor is already moving.  A live queue-head Move is the detached
        // ECS equivalent; unrelated Attack/Build work is replaced because
        // entering waypoint mode starts AI_FOLLOW_PATH immediately.
        if (!queue.orders.empty() &&
            queue.orders.front().kind == ObjectOrderKind::Move &&
            queue.orders.front().hasTargetPosition) {
            ObjectOrderIntent current = queue.orders.front();
            queue.orders.clear();
            current.moveRouteSubtype = ObjectMoveRouteSubtype::FollowPath;
            queue.orders.push_back(current);
            fresh.points.push_back({
                current.targetX, current.targetY, current.targetZ});
        } else {
            queue.orders.clear();
        }
        fresh.routeSubtype = ObjectMoveRouteSubtype::FollowPath;
        fresh.source = ObjectOrderSource::Player;
        fresh.systemPurpose = ObjectOrderSystemPurpose::Generic;
        fresh.sequenceRevision = 1;
        fresh.issuedTick = queue.orders.empty()
            ? order.issuedTick : queue.orders.front().issuedTick;
        fresh.firstSourceSequence = queue.orders.empty()
            ? order.sourceSequence : queue.orders.front().sourceSequence;
        if (queue.orders.empty()) {
            order.moveRouteSubtype = ObjectMoveRouteSubtype::FollowPath;
        } else {
            order.moveRouteSubtype = ObjectMoveRouteSubtype::Direct;
        }
        queue.orders.push_back(std::move(order));
        const ObjectOrderIntent& appended = queue.orders.back();
        fresh.points.push_back({
            appended.targetX, appended.targetY, appended.targetZ});
        fresh.queuedOrderCount = static_cast<uint32_t>(fresh.points.size());
        ecs::emplace<ObjectSystemPathSequenceComponent>(
            registry, entity, std::move(fresh));
        return;
    }

    order.moveRouteSubtype = ObjectMoveRouteSubtype::Direct;
    queue.orders.push_back(std::move(order));
    const ObjectOrderIntent& appended = queue.orders.back();
    path->points.push_back({
        appended.targetX, appended.targetY, appended.targetZ});
    path->queuedOrderCount = static_cast<uint32_t>(path->points.size());
}

[[nodiscard]] int64_t saturatingAddRaw(
    int64_t left, int64_t right) noexcept {
    if (right > 0 && left > INT64_MAX - right) return INT64_MAX;
    if (right < 0 && left < INT64_MIN - right) return INT64_MIN;
    return left + right;
}

[[nodiscard]] bool hasKindOf(const ObjectKindOfComponent* kinds,
                             game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

struct Unsigned128 final {
    uint64_t high = 0;
    uint64_t low = 0;
};

[[nodiscard]] uint64_t unsignedMagnitude(int64_t value) noexcept {
    return value < 0
        ? static_cast<uint64_t>(-(value + 1)) + 1u
        : static_cast<uint64_t>(value);
}

[[nodiscard]] Unsigned128 squaredDistance(
    int64_t x, int64_t y) noexcept {
    const uint64_t ax = unsignedMagnitude(x);
    const uint64_t ay = unsignedMagnitude(y);
    uint64_t xHigh = 0;
    uint64_t yHigh = 0;
    const uint64_t xLow = _umul128(ax, ax, &xHigh);
    const uint64_t yLow = _umul128(ay, ay, &yHigh);
    const uint64_t low = xLow + yLow;
    return {
        .high = xHigh + yHigh + static_cast<uint64_t>(low < xLow),
        .low = low,
    };
}

[[nodiscard]] bool greaterDistance(
    const Unsigned128& left, const Unsigned128& right) noexcept {
    return left.high != right.high
        ? left.high > right.high : left.low > right.low;
}

[[nodiscard]] uint64_t integerSquareRoot(uint64_t value) noexcept {
    uint64_t result = 0;
    uint64_t bit = uint64_t{1} << 62u;
    while (bit > value) bit >>= 2u;
    while (bit != 0u) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1u) + bit;
        } else {
            result >>= 1u;
        }
        bit >>= 2u;
    }
    return result;
}

[[nodiscard]] int64_t fixedLengthRaw(
    int64_t x, int64_t y) noexcept {
    const uint64_t ax = unsignedMagnitude(x);
    const uint64_t ay = unsignedMagnitude(y);
    const uint64_t maximum = std::max(ax, ay);
    if (maximum == 0u) return 0;
    const unsigned bits = std::bit_width(maximum);
    const unsigned shift = bits > 30u ? bits - 30u : 0u;
    const uint64_t sx = ax >> shift;
    const uint64_t sy = ay >> shift;
    const uint64_t root = integerSquareRoot(sx * sx + sy * sy);
    if (root > (static_cast<uint64_t>(INT64_MAX) >> shift)) {
        return INT64_MAX;
    }
    return static_cast<int64_t>(root << shift);
}

[[nodiscard]] bool rejectsExternalOrdersWhileHeadingOffMap(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, entity);
    if (!runtime || !runtime->plan) return false;
    const size_t count = std::min(runtime->plan->behaviorRules.size(),
                                  runtime->behaviorStates.size());
    for (size_t index = 0; index < count; ++index) {
        if (runtime->plan->behaviorRules[index].kind ==
                ObjectTransportBehaviorKind::DeliverPayloadAI &&
            runtime->behaviorStates[index].phase ==
                ObjectTransportBehaviorPhase::DeliveryHeadingOffMap) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool shouldTightenPlayerMove(
    const ecs::registry& registry,
    const container::Vector<ecs::entity>& entities,
    const LogicFixedVec3& destination,
    int64_t factorRaw) noexcept {
    if (entities.size() < 2 || factorRaw <= 0)
        return false;

    bool initialized = false;
    math::q32_32 minimumX{};
    math::q32_32 maximumX{};
    math::q32_32 minimumY{};
    math::q32_32 maximumY{};
    for (const ecs::entity entity : entities) {
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, entity);
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, entity);
        const bool fixedWingAircraft =
            hasKindOf(kinds, game::ObjectKindOf::Aircraft) &&
            !hasKindOf(kinds, game::ObjectKindOf::ProducedAtHelipad) && locomotion &&
            (locomotion->surfaces & game::locomotorSurfaceBit(
                 game::LocomotorSurface::Air)) != 0;
        if (fixedWingAircraft) return false;

        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, entity);
        if (!transform) return false;
        const LogicFixedVec3 position =
            readAuthoritativeObjectPosition(registry, entity, *transform);
        if (!initialized) {
            minimumX = maximumX = position.x;
            minimumY = maximumY = position.y;
            initialized = true;
        } else {
            minimumX = std::min(minimumX, position.x);
            maximumX = std::max(maximumX, position.x);
            minimumY = std::min(minimumY, position.y);
            maximumY = std::max(maximumY, position.y);
        }
    }
    if (!initialized) return false;

    const math::q32_32 factor = math::q32_32::from_raw(factorRaw);
    const math::q32_32 two{int32_t{2}};
    const math::q32_32 centerX = minimumX + (maximumX - minimumX) / two;
    const math::q32_32 centerY = minimumY + (maximumY - minimumY) / two;
    const math::q32_32 halfX = (maximumX - minimumX) / two * factor;
    const math::q32_32 halfY = (maximumY - minimumY) / two * factor;
    if (destination.x < centerX - halfX ||
        destination.x > centerX + halfX ||
        destination.y < centerY - halfY ||
        destination.y > centerY + halfY) {
        return false;
    }

    // Preserve ZH's shipped AIGroup test exactly: both dimensions use the
    // X extent before the <2000-cell guard.
    constexpr int64_t pathfindCellSizeRaw = int64_t{10} << 32;
    const uint64_t widthCells = static_cast<uint64_t>(
        std::max<int64_t>(0, (maximumX - minimumX).raw()) /
        pathfindCellSizeRaw);
    return widthCells < 45 && widthCells * widthCells < 2000;
}

[[nodiscard]] std::optional<ObjectOrderKind> toOrderKind(GameCommandType type) noexcept {
    switch (type) {
    case GameCommandType::Move: return ObjectOrderKind::Move;
    case GameCommandType::AttackMove: return ObjectOrderKind::Move;
    case GameCommandType::Attack: return ObjectOrderKind::Attack;
    case GameCommandType::Build: return ObjectOrderKind::Build;
    case GameCommandType::CommandButton: return ObjectOrderKind::CommandButton;
    case GameCommandType::CombatDrop: return ObjectOrderKind::CommandButton;
    case GameCommandType::SpecialPower: return ObjectOrderKind::SpecialPower;
    case GameCommandType::Stop: return ObjectOrderKind::Stop;
    case GameCommandType::Guard:
    case GameCommandType::GuardWithoutPursuit:
    case GameCommandType::GuardFlyingUnitsOnly:
        return ObjectOrderKind::TacticalAttack;
    case GameCommandType::None:
    case GameCommandType::UIAction:
    case GameCommandType::Pause:
    case GameCommandType::Surrender:
    case GameCommandType::QueueProduction:
    case GameCommandType::CancelProduction:
    case GameCommandType::QueuePlayerUpgrade:
    case GameCommandType::CancelPlayerUpgrade:
    case GameCommandType::SetFactoryRallyPoint:
    case GameCommandType::SetBeaconText:
    case GameCommandType::Repair:
    case GameCommandType::Sell:
    case GameCommandType::CancelConstruction:
    case GameCommandType::ExitContainer:
    case GameCommandType::Evacuate:
    case GameCommandType::ExecuteRailedTransport:
    case GameCommandType::EnterContainer:
    case GameCommandType::PurchaseScience:
    case GameCommandType::Scatter:
    case GameCommandType::CreateFormation:
    case GameCommandType::ToggleOvercharge:
    case GameCommandType::CancelOrderWaypoint:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] bool isSupportedTacticalSubtype(
    ObjectTacticalAttackSubtype subtype) noexcept {
    switch (subtype) {
    case ObjectTacticalAttackSubtype::None: return false;
    case ObjectTacticalAttackSubtype::Hunt: return true;
    case ObjectTacticalAttackSubtype::Guard: return true;
    case ObjectTacticalAttackSubtype::AttackSquad: return true;
    case ObjectTacticalAttackSubtype::AttackArea: return true;
    case ObjectTacticalAttackSubtype::GuardRetaliate: return false;
    case ObjectTacticalAttackSubtype::GuardTunnelNetwork: return true;
    }
    return false;
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] OrderExecutionResult rejected(OrderRejectionReason reason,
                                             container::String message) {
    return {.accepted = false, .rejection = reason, .message = std::move(message)};
}

[[nodiscard]] std::optional<container::Vector<ObjectId>> canonicalActors(
    const container::Vector<ObjectId>& actors, size_t maximumActors,
    container::String* error = nullptr) {
    if (actors.empty() || actors.size() > maximumActors) {
        setError(error, "order actor count is outside the supported range");
        return std::nullopt;
    }
    container::Vector<ObjectId> result = actors;
    std::sort(result.begin(), result.end());
    const auto duplicate = std::adjacent_find(result.begin(), result.end());
    if (duplicate != result.end() || !result.front()) {
        setError(error, "order actors must be unique, nonzero ObjectIds");
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] OrderExecutionResult execute(
    ecs::registry& registry, const PlayerRegistry& players,
    const ObjectLifecycle& objects, const ObjectTeamRegistry* teams,
    const ai::ObjectAIOrderCapabilitySnapshot& capabilities,
    OrderAuthority authority, PlayerId contextPlayer, ObjectTeamId scenarioTeam,
    uint64_t issuedTick, uint32_t sequence, uint32_t sourceScriptId,
    ObjectOrderKind kind, ObjectOrderSystemPurpose requestedPurpose,
    const container::Vector<ObjectId>& actors,
    ObjectId targetObject, const CommandPosition& targetPosition,
    math::q32_32 placementYawRadians,
    const CommandPosition& placementEndPosition,
    container::StringView contentName, bool forceAttack,
    std::optional<uint32_t> maximumShots, bool attackMove,
    ObjectMoveRouteSubtype moveRouteSubtype, uint32_t waypointStartId,
    uint64_t waypointGraphRevision,
    ObjectTacticalAttackSubtype tacticalAttackSubtype, bool allArmyHunt,
    bool useTeamCommonTarget, bool guardWithoutPursuit,
    bool guardFlyingOnly, ObjectTeamId tacticalTargetTeam,
    uint32_t tacticalTargetAreaId, uint64_t tacticalTargetRevision,
    bool queued, bool allowHackInternetCommand,
    bool allowCombatDropCommand,
    int64_t groupMoveClickToGatherFactorRaw,
    const PlayerGroupPathPlan* groupPath) {
    const bool isPlayerCommand = authority == OrderAuthority::PlayerCommand;
    const bool isStrategicAICommand =
        authority == OrderAuthority::StrategicAI;
    const ObjectOrderSource admittedSource = isPlayerCommand
        ? ObjectOrderSource::Player
        : isStrategicAICommand
            ? ObjectOrderSource::System
            : ObjectOrderSource::Script;
    const ObjectOrderSystemPurpose admittedPurpose = isStrategicAICommand
        ? ObjectOrderSystemPurpose::StrategicAI
        : isPlayerCommand
            ? ObjectOrderSystemPurpose::Generic
            : requestedPurpose;
    // Every order enters through a phase-owned admission snapshot.  This is
    // intentionally a required argument: future callers cannot fall back to
    // ThingTemplate initial capabilities after a live actor has changed
    // membership or been retired.
    constexpr uint32_t invalidWaypointId =
        std::numeric_limits<uint32_t>::max();
    const bool waypointRoute = isObjectWaypointRouteSubtype(moveRouteSubtype);
    const bool waypointTeamRoute =
        objectWaypointRouteMovesAsTeam(moveRouteSubtype);
    const bool waypointAttackFollowRoute = waypointRoute && attackMove &&
        !objectWaypointRouteIsExact(moveRouteSubtype);
    const bool directRoute =
        moveRouteSubtype == ObjectMoveRouteSubtype::Direct;
    if (!directRoute && !waypointRoute) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "move order has an invalid route subtype");
    }
    if (isPlayerCommand && !directRoute) {
        return rejected(OrderRejectionReason::UnsupportedCommand,
                        "waypoint routes are not a player command family");
    }
    if (directRoute !=
        (waypointStartId == invalidWaypointId && waypointGraphRevision == 0)) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "move route handle and graph revision do not match its subtype");
    }
    if (waypointRoute &&
        (authority != OrderAuthority::ScenarioScript ||
         kind != ObjectOrderKind::Move || targetObject ||
         targetPosition.valid || !contentName.empty() ||
         (attackMove && !waypointAttackFollowRoute) || queued ||
         (waypointTeamRoute && !scenarioTeam) ||
         (!waypointTeamRoute && !scenarioTeam && actors.size() != 1))) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "waypoint routes require an immediate script actor domain matching the route subtype");
    }
    const bool hasTacticalPayload =
        tacticalAttackSubtype != ObjectTacticalAttackSubtype::None ||
        allArmyHunt || useTeamCommonTarget || tacticalTargetTeam ||
        tacticalTargetAreaId != invalidWaypointId ||
        tacticalTargetRevision != 0;
    // GUI_COMMAND_GUARD* is the one tactical-attack subtype with a click
    // origin: RefCode routes MSG_DO_GUARD_POSITION/MSG_DO_GUARD_OBJECT to
    // aiGuardPosition/aiGuardObject. fromGameCommand already builds exactly
    // that order for the three player Guard commands, so only the authored
    // script-authority subtypes (Hunt, AttackSquad, AttackArea,
    // GuardTunnelNetwork) stay outside the player command family.
    if (kind == ObjectOrderKind::TacticalAttack && isPlayerCommand &&
        tacticalAttackSubtype != ObjectTacticalAttackSubtype::Guard) {
        return rejected(OrderRejectionReason::UnsupportedCommand,
                        "only guard tactical-attack orders are a player command family");
    }
    if (kind != ObjectOrderKind::TacticalAttack && hasTacticalPayload) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "tactical-attack payload is valid only for tactical-attack orders");
    }
    if (isPlayerCommand) {
        const PlayerState* player = players.get(contextPlayer);
        if (!player || !player->isCommandPlayer()) {
            return rejected(OrderRejectionReason::InvalidPlayer,
                            "order issuer is not a live command player");
        }
    }
    if (isStrategicAICommand) {
        const PlayerState* player = players.get(contextPlayer);
        if (!player || !player->isCommandPlayer() ||
            player->controller != PlayerControllerKind::Ai) {
            return rejected(OrderRejectionReason::InvalidPlayer,
                            "strategic AI order issuer is not a live AI player");
        }
    }
    if (authority == OrderAuthority::ScenarioScript &&
        scenarioTeam && (!teams || !teams->find(scenarioTeam))) {
        return rejected(OrderRejectionReason::InvalidPlayer,
                        "script order references an unavailable scenario team");
    }
    // Empty live Scenario Teams are common in authored maps. RefCode treats
    // their group orders as no-ops, while external player input must still
    // reject an empty selection at the protocol boundary.
    if (authority == OrderAuthority::ScenarioScript && actors.empty()) {
        return {.accepted = true, .actorCount = 0};
    }
    container::String actorError;
    const std::optional<container::Vector<ObjectId>> canonical = canonicalActors(
        actors, isPlayerCommand ? kMaximumPlayerOrderActors : kMaximumScenarioOrderActors, &actorError);
    if (!canonical) return rejected(OrderRejectionReason::MalformedOrder, std::move(actorError));
    const bool canonicalAbsentPlacementEnd =
        !placementEndPosition.valid && placementEndPosition.x.raw() == 0 &&
        placementEndPosition.y.raw() == 0 &&
        placementEndPosition.z.raw() == 0;
    if (kind != ObjectOrderKind::Build && !canonicalAbsentPlacementEnd) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "line-build end payload is valid only for build orders");
    }
    if (kind != ObjectOrderKind::Build &&
        kind != ObjectOrderKind::SpecialPower &&
        placementYawRadians.raw() != 0) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "command yaw is valid only for build and special-power orders");
    }
    if (kind == ObjectOrderKind::Build &&
        !placementEndPosition.valid && !canonicalAbsentPlacementEnd) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "build order contains a stale invalid line end");
    }
    if (forceAttack && kind != ObjectOrderKind::Attack) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "force-attack policy is valid only for object-target attack orders");
    }
    if (maximumShots && kind != ObjectOrderKind::Attack &&
        !(kind == ObjectOrderKind::Move && attackMove)) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "maximum-shots is valid only for Attack/AttackMove orders");
    }
    if (attackMove && kind != ObjectOrderKind::Move) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "attack-move policy is valid only for move orders");
    }
    const bool hackInternetCommand =
        kind == ObjectOrderKind::CommandButton &&
        allowHackInternetCommand;
    const bool combatDropCommand =
        kind == ObjectOrderKind::CommandButton &&
        allowCombatDropCommand;
    switch (kind) {
    case ObjectOrderKind::Move:
        if (directRoute && !targetPosition.valid && !targetObject) {
            return rejected(OrderRejectionReason::MalformedOrder,
                            "move order requires a target position or object");
        }
        if (targetObject && !objects.entityFromId(targetObject)) {
            return rejected(OrderRejectionReason::InvalidTarget,
                            "move order target object is unavailable");
        }
        break;
    case ObjectOrderKind::Stop:
        if (queued) {
            return rejected(OrderRejectionReason::MalformedOrder,
                            "stop is an immediate queue-clear operation and cannot be queued");
        }
        break;
    case ObjectOrderKind::Attack:
        // ObjectCombatSystem consumes both a tracked object victim and a
        // first-class position victim. Exactly one target shape is required;
        // accepting both would make replay interpretation ambiguous.
        if (static_cast<bool>(targetObject) == targetPosition.valid) {
            return rejected(OrderRejectionReason::MalformedOrder,
                            "attack order requires exactly one object or position target");
        }
        break;
    case ObjectOrderKind::TacticalAttack: {
        if (!isSupportedTacticalSubtype(tacticalAttackSubtype)) {
            return rejected(OrderRejectionReason::MalformedOrder,
                            "tactical-attack order has an unsupported subtype");
        }
        const bool guardPosition =
            tacticalAttackSubtype == ObjectTacticalAttackSubtype::Guard &&
            targetPosition.valid &&
            tacticalTargetAreaId == invalidWaypointId;
        const bool guardObject =
            tacticalAttackSubtype == ObjectTacticalAttackSubtype::Guard &&
            static_cast<bool>(targetObject);
        const bool guardArea =
            tacticalAttackSubtype == ObjectTacticalAttackSubtype::Guard &&
            tacticalTargetAreaId != invalidWaypointId;
        if ((targetObject && !guardObject) ||
            (targetPosition.valid && !guardPosition && !guardArea) ||
            (guardObject && (guardPosition || guardArea)) ||
            (guardArea && !targetPosition.valid) ||
            !contentName.empty()) {
            return rejected(OrderRejectionReason::MalformedOrder,
                            "script tactical target does not match its subtype");
        }
        if (guardObject && !objects.entityFromId(targetObject)) {
            return rejected(OrderRejectionReason::InvalidTarget,
                            "guard target object is unavailable");
        }
        if (tacticalAttackSubtype ==
                ObjectTacticalAttackSubtype::AttackSquad) {
            if (!tacticalTargetTeam ||
                tacticalTargetAreaId != invalidWaypointId ||
                tacticalTargetRevision == 0) {
                return rejected(OrderRejectionReason::MalformedOrder,
                    "attack-squad requires one stable Team target");
            }
            if (!teams || !teams->find(tacticalTargetTeam)) {
                return rejected(OrderRejectionReason::InvalidTarget,
                    "attack-squad target Team is unavailable");
            }
        } else if (tacticalAttackSubtype ==
                       ObjectTacticalAttackSubtype::AttackArea) {
            if (tacticalTargetTeam ||
                tacticalTargetAreaId == invalidWaypointId ||
                tacticalTargetRevision == 0) {
                return rejected(OrderRejectionReason::MalformedOrder,
                    "attack-area requires one stable PolygonTrigger target");
            }
        } else if (guardArea) {
            if (tacticalTargetTeam || tacticalTargetRevision == 0) {
                return rejected(OrderRejectionReason::MalformedOrder,
                    "guard-area requires one stable PolygonTrigger target");
            }
        } else if (tacticalTargetTeam ||
                   tacticalTargetAreaId != invalidWaypointId ||
                   tacticalTargetRevision != 0) {
            return rejected(OrderRejectionReason::MalformedOrder,
                "tactical target domain does not match the subtype");
        }
        break;
    }
    case ObjectOrderKind::Build:
        if (contentName.empty() || !targetPosition.valid) {
            return rejected(OrderRejectionReason::MalformedOrder,
                            "build order requires a template name and placement position");
        }
        break;
    case ObjectOrderKind::CommandButton:
        if (!hackInternetCommand && !combatDropCommand) {
            return rejected(OrderRejectionReason::UnsupportedCommand,
                            "command-button has no production consumer");
        }
        if (combatDropCommand && !targetPosition.valid) {
            return rejected(OrderRejectionReason::MalformedOrder,
                            "combat-drop command requires a target position");
        }
        break;
    case ObjectOrderKind::SpecialPower:
        if (contentName.empty()) {
            return rejected(OrderRejectionReason::MalformedOrder,
                            "command-button order requires an authored content name");
        }
        break;
    }

    // Move and object/position Attack have concrete ECS consumers. Stop is a
    // synchronous queue-clear operation. TacticalAttack is the deliberately
    // typed queue handoff added for the Hunt owner-policy slice; this ingress
    // does not choose or run that owner. Other families stay unavailable
    // until their simulation system exists.
    if (kind != ObjectOrderKind::Move && kind != ObjectOrderKind::Stop &&
        kind != ObjectOrderKind::Attack &&
        kind != ObjectOrderKind::Build &&
        kind != ObjectOrderKind::CommandButton &&
        kind != ObjectOrderKind::SpecialPower &&
        kind != ObjectOrderKind::TacticalAttack) {
        return rejected(OrderRejectionReason::UnsupportedCommand,
                        "the requested order family has no simulation consumer yet");
    }

    // RefCode's forced attack exception is evaluated inside the weapon target
    // query: it bypasses the owner/alliance relationship check, but it does
    // not bypass self, masked, unattackable, contained, or weapon anti-mask
    // checks. Keep the relationship gate here only for ordinary player
    // attacks; scripts/AI retain their authored authority as before.
    if (isPlayerCommand && kind == ObjectOrderKind::Attack && targetObject) {
        if (std::binary_search(canonical->begin(), canonical->end(), targetObject)) {
            return rejected(OrderRejectionReason::InvalidTarget,
                            "a player attack cannot target one of its own actors");
        }
        const std::optional<ecs::entity> target = objects.entityFromId(targetObject);
        if (!target || objects.isPendingDestroy(targetObject)) {
            return rejected(OrderRejectionReason::InvalidTarget,
                            "player attack target is unavailable");
        }
        const OwnerComponent* targetOwner = ecs::try_get<OwnerComponent>(registry, *target);
        if (!targetOwner) {
            return rejected(OrderRejectionReason::InvalidTarget,
                            "player attack target has no authoritative owner");
        }
        const PlayerRelationship relationship =
            players.relationship(targetOwner->player, contextPlayer);
        if (!forceAttack && (targetOwner->player == contextPlayer ||
                             relationship == PlayerRelationship::Allies)) {
            return rejected(OrderRejectionReason::InvalidTarget,
                            "player attack target is allied");
        }
        if (!forceAttack && relationship == PlayerRelationship::Neutral) {
            const ObjectScriptPanelPolicyComponent* panel =
                ecs::try_get<ObjectScriptPanelPolicyComponent>(registry, *target);
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(registry, *target);
            if ((!panel || !panel->playerTargetable) &&
                !hasKindOf(kinds, game::ObjectKindOf::Mine)) {
                return rejected(OrderRejectionReason::InvalidTarget,
                    "neutral target is not script-marked player targetable");
            }
        }
    }

    // Validate the entire group before touching any component. A command frame
    // is deterministic input; applying a prefix and rejecting the tail would
    // make invalid selection contents mutate the world in surprising ways.
    container::Vector<ecs::entity> entities;
    entities.reserve(canonical->size());
    for (const ObjectId actor : *canonical) {
        const std::optional<ecs::entity> entity = objects.entityFromId(actor);
        if (!entity) {
            // RefCode's named/team script actions silently skip an object
            // that has disappeared or has no usable AI update.  Player
            // input remains atomic and must reject the full selection.
            if (!isPlayerCommand) continue;
            return rejected(objects.isPendingDestroy(actor) ? OrderRejectionReason::PendingDestroy
                                                              : OrderRejectionReason::MissingActor,
                            "order references an unavailable actor");
        }
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
        if (!owner) {
            return rejected(OrderRejectionReason::MissingActor,
                            "order actor has no authoritative owner component");
        }
        // Player/network/replay input is authorized by ownership. A map
        // script is trusted scenario content evaluated identically by every
        // peer, so RefCode may direct a named enemy/civilian object without
        // pretending that the ScriptList owner owns it.
        if ((isPlayerCommand || isStrategicAICommand) &&
            owner->player != contextPlayer) {
            if (isStrategicAICommand) continue;
            return rejected(OrderRejectionReason::OwnershipMismatch,
                            "order contains an actor not owned by its issuer");
        }
        if (!isPlayerCommand && scenarioTeam &&
            (!teams || teams->teamOf(actor) != scenarioTeam)) {
            return rejected(OrderRejectionReason::TeamMembershipMismatch,
                            "script order actor no longer belongs to its scenario team");
        }
        // DeliverPayloadAIUpdate::HeadOffMap disables AI command admission in
        // RefCode.  Silently omit this actor from either a player selection or
        // a script Team order; the specialized owner retains its exit route.
        if (rejectsExternalOrdersWhileHeadingOffMap(registry, *entity))
            continue;
        if (kind == ObjectOrderKind::Move &&
            !ecs::try_get<ObjectLocomotionComponent>(registry, *entity)) {
            if (!isPlayerCommand) continue;
            return rejected(OrderRejectionReason::UnsupportedCommand,
                            "move order references an actor without a supported locomotor");
        }
        const ObjectKindOfComponent* actorKinds =
            ecs::try_get<ObjectKindOfComponent>(registry, *entity);
        const bool attacksThroughSpawnChildren =
            kind == ObjectOrderKind::Attack &&
            hasKindOf(actorKinds,
                      game::ObjectKindOf::SpawnsAreTheWeapons) && [&] {
                // The later fanout uses the same capability snapshot.  Do
                // not pre-admit this parent from a template's initial mask:
                // a child may be unbound/retired in the current phase.
                for (const ObjectId child :
                     ObjectSpawnSlaveSystem{}.spawnChildren(
                         registry, objects, actor)) {
                    if (objects.entityFromId(child) &&
                        hasDirectAttackOrderConsumer(capabilities, child)) {
                        return true;
                    }
                }
                return false;
            }();
        if (kind == ObjectOrderKind::Attack &&
            ((!ecs::try_get<ObjectWeaponComponent>(registry, *entity) ||
              !hasDirectAttackOrderConsumer(capabilities, actor)) &&
             !attacksThroughSpawnChildren)) {
            if (!isPlayerCommand) continue;
            return rejected(OrderRejectionReason::UnsupportedCommand,
                            "attack order references an actor without a resolved weapon or spawn-weapon runtime");
        }
        const bool guardOrder = kind == ObjectOrderKind::TacticalAttack &&
            (tacticalAttackSubtype == ObjectTacticalAttackSubtype::Guard ||
             tacticalAttackSubtype ==
                 ObjectTacticalAttackSubtype::GuardTunnelNetwork ||
             tacticalAttackSubtype ==
                 ObjectTacticalAttackSubtype::GuardRetaliate);
        if (guardOrder &&
            (!hasDirectAttackOrderConsumer(capabilities, actor) ||
             !hasMoveStopOrderConsumer(capabilities, actor))) {
            // AIGroup applies Guard only to members exposing both halves of
            // AIUpdate. Mixed selections keep their capable members instead
            // of leaving an unowned TacticalAttack at the incapable head.
            continue;
        }
        if (kind == ObjectOrderKind::SpecialPower &&
            !ecs::try_get<ObjectSpecialPowerComponent>(registry, *entity)) {
            if (!isPlayerCommand) continue;
            return rejected(OrderRejectionReason::UnsupportedCommand,
                            "special-power order references an actor without a SpecialPower runtime");
        }
        if (kind == ObjectOrderKind::Build &&
            !ecs::try_get<ObjectBuilderComponent>(registry, *entity)) {
            if (!isPlayerCommand) continue;
            return rejected(OrderRejectionReason::UnsupportedCommand,
                            "build order references an actor without a builder runtime");
        }
        if (kind == ObjectOrderKind::CommandButton &&
            hackInternetCommand) {
            const ObjectEconomyComponent* economy =
                ecs::try_get<ObjectEconomyComponent>(registry, *entity);
            if (!economy || !economy->plan ||
                economy->plan->hackInternet.empty() ||
                economy->hackInternet.empty()) {
                return rejected(OrderRejectionReason::UnsupportedCommand,
                    "hack-internet actor has no economy runtime");
            }
        }
        if (kind == ObjectOrderKind::CommandButton &&
            combatDropCommand) {
            const ObjectAirfieldComponent* airfield =
                ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
            if (!airfield || !airfield->plan ||
                airfield->chinookAi.empty() ||
                airfield->plan->chinookAi.empty()) {
                return rejected(OrderRejectionReason::UnsupportedCommand,
                    "combat-drop actor has no ChinookAI runtime");
            }
        }
        const ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, *entity);
        if (queued && queue && queue->orders.size() >= ObjectOrderQueueComponent::MaximumQueuedOrders) {
            return rejected(OrderRejectionReason::QueueCapacity,
                            "order would exceed an actor's deterministic queue capacity");
        }
        entities.push_back(*entity);
    }

    // A script action with only static/destroyed members is an original-style
    // no-op, not an error.  It still produces a stable successful journal
    // entry with actorCount zero, while player commands retain atomic errors.
    if (entities.empty() && !isPlayerCommand) {
        return {.accepted = true, .actorCount = 0};
    }
    if (entities.empty()) {
        return rejected(
            OrderRejectionReason::UnsupportedCommand,
            "selection contains no actor with a consumer for this order");
    }

    if (kind == ObjectOrderKind::Stop) {
        for (const ecs::entity entity : entities) {
            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
            if (!queue) continue;
            queue->orders.clear();
            ++queue->revision;
            if (!isStrategicAICommand) {
                ++queue->externalRevision;
                if (queue->externalRevision == 0) ++queue->externalRevision;
                queue->replacementExternalRevision = queue->externalRevision;
                queue->replacementExternalSource = admittedSource;
                queue->replacementExternalKind = ObjectOrderKind::Stop;
            }
            ecs::remove<ObjectSystemPathSequenceComponent>(registry, entity);
        }
        return {.accepted = true, .actorCount = entities.size()};
    }

    const LogicFixedVec3 fixedTarget = targetPosition.valid
        ? LogicFixedVec3{targetPosition.x, targetPosition.y, targetPosition.z}
        : LogicFixedVec3{};
    const math::q32_32 fixedPlacementYaw = placementYawRadians;
    const LogicFixedVec3 fixedPlacementEnd = placementEndPosition.valid
        ? LogicFixedVec3{placementEndPosition.x, placementEndPosition.y,
                         placementEndPosition.z}
        : LogicFixedVec3{};
    ObjectMoveRouteSubtype effectiveMoveRouteSubtype = moveRouteSubtype;
    if (isPlayerCommand && kind == ObjectOrderKind::Move && directRoute &&
        !queued && !attackMove && !targetObject &&
        shouldTightenPlayerMove(registry, entities, fixedTarget,
                                groupMoveClickToGatherFactorRaw)) {
        effectiveMoveRouteSubtype = ObjectMoveRouteSubtype::Tighten;
    }

    // AIGroup::getCenter() averages non-held AI members once when an
    // AS_TEAM waypoint state enters. `entities` is already the stable,
    // admitted AI-capable actor set. Divide each coordinate before summing so
    // a large scripted Team cannot overflow the fixed-point accumulator; the
    // summed remainders recover the exact integer mean.
    int64_t waypointCenterXRaw = 0;
    int64_t waypointCenterYRaw = 0;
    size_t waypointCenterCount = 0;
    math::q32_32 waypointGroupSpeed{};
    bool waypointGroupSpeedKnown = false;
    math::q32_32 playerGroupCenterX{};
    math::q32_32 playerGroupCenterY{};
    size_t playerGroupMemberCount = 0;
    bool playerGroupCenterKnown = false;
    const auto saturatingSubtractRaw = [](int64_t left,
                                          int64_t right) noexcept {
        if (right > 0 && left < INT64_MIN + right) return INT64_MIN;
        if (right < 0 && left > INT64_MAX + right) return INT64_MAX;
        return left - right;
    };
    if (waypointTeamRoute) {
        for (const ecs::entity entity : entities) {
            if (isObjectDisabledBy(
                    registry, entity, ObjectDisabledReason::Held,
                    issuedTick)) {
                continue;
            }
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(registry, entity);
            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(registry, entity);
            if (!hasKindOf(kinds, game::ObjectKindOf::Immobile) && locomotion &&
                locomotion->maximumSpeed > math::q32_32{}) {
                waypointGroupSpeed = waypointGroupSpeedKnown
                    ? math::q32_32::min(
                          waypointGroupSpeed,
                          locomotion->maximumSpeed)
                    : locomotion->maximumSpeed;
                waypointGroupSpeedKnown = true;
            }
            if (ecs::try_get<TransformComponent>(registry, entity)) {
                ++waypointCenterCount;
            }
        }
        if (waypointCenterCount != 0) {
            const int64_t divisor = static_cast<int64_t>(
                waypointCenterCount);
            math::FixedRawMeanAccumulator centerX{divisor};
            math::FixedRawMeanAccumulator centerY{divisor};
            for (const ecs::entity entity : entities) {
                if (isObjectDisabledBy(
                        registry, entity, ObjectDisabledReason::Held,
                        issuedTick)) {
                    continue;
                }
                const TransformComponent* transform =
                    ecs::try_get<TransformComponent>(registry, entity);
                if (!transform) continue;
                const LogicFixedVec3 position =
                    readAuthoritativeObjectPosition(
                        registry, entity, *transform);
                centerX.add(position.x.raw());
                centerY.add(position.y.raw());
            }
            waypointCenterXRaw = centerX.value();
            waypointCenterYRaw = centerY.value();
        }
    }
    if (isPlayerCommand && kind == ObjectOrderKind::Move &&
        targetPosition.valid && entities.size() > 1) {
        // RefCode's non-formation group path still gives each mobile member
        // an individual destination derived from the group's current
        // centroid.  The old TD adapter only did this for the large shared
        // group-path case; small selections therefore received the exact
        // same target and collapsed onto one point.
        int64_t centerSumX = 0;
        int64_t centerSumY = 0;
        size_t count = 0;
        for (const ecs::entity entity : entities) {
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(registry, entity);
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(registry, entity);
            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(registry, entity);
            if (!transform || !locomotion ||
                hasKindOf(kinds, game::ObjectKindOf::Immobile) ||
                (hasKindOf(kinds, game::ObjectKindOf::Aircraft)) ||
                (!hasKindOf(kinds, game::ObjectKindOf::Infantry) &&
                 !hasKindOf(kinds, game::ObjectKindOf::Vehicle)) ||
                isObjectDisabledBy(registry, entity,
                                   ObjectDisabledReason::Held,
                                   issuedTick)) continue;
            const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                registry, entity, *transform);
            centerSumX = saturatingAddRaw(centerSumX, position.x.raw());
            centerSumY = saturatingAddRaw(centerSumY, position.y.raw());
            ++count;
        }
        if (count > 1) {
            playerGroupCenterX = math::q32_32::from_raw(
                centerSumX / static_cast<int64_t>(count));
            playerGroupCenterY = math::q32_32::from_raw(
                centerSumY / static_cast<int64_t>(count));
            playerGroupMemberCount = count;
            playerGroupCenterKnown = true;
        }
    }
    for (const ecs::entity entity : entities) {
        const PlayerGroupPathMember* groupMember = nullptr;
        uint32_t groupMemberOrdinal = 0;
        if (isPlayerCommand && groupPath) {
            const ObjectIdentityComponent* identity =
                ecs::try_get<ObjectIdentityComponent>(registry, entity);
            for (size_t index = 0;
                 identity && index < groupPath->members.size(); ++index) {
                if (groupPath->members[index].object == identity->id) {
                    groupMember = &groupPath->members[index];
                    groupMemberOrdinal = static_cast<uint32_t>(index);
                    break;
                }
            }
        }
        math::q32_32 waypointGroupOffsetX{};
        math::q32_32 waypointGroupOffsetY{};
        bool hasExplicitFormationOffset = false;
        if (waypointTeamRoute && waypointCenterCount != 0) {
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(registry, entity);
            if (transform && !isObjectDisabledBy(
                    registry, entity, ObjectDisabledReason::Held,
                    issuedTick)) {
                const LogicFixedVec3 position =
                    readAuthoritativeObjectPosition(
                        registry, entity, *transform);
                waypointGroupOffsetX = math::q32_32::from_raw(
                    saturatingSubtractRaw(
                        position.x.raw(), waypointCenterXRaw));
                waypointGroupOffsetY = math::q32_32::from_raw(
                    saturatingSubtractRaw(
                        position.y.raw(), waypointCenterYRaw));
            }
        }
        CommandPosition actorTarget = targetPosition;
        if (!groupMember && isPlayerCommand &&
            kind == ObjectOrderKind::Move &&
            actorTarget.valid) {
            if (const auto* formation =
                    ecs::try_get<ObjectPlayerFormationComponent>(
                        registry, entity);
                formation && formation->id != 0) {
                hasExplicitFormationOffset = true;
                actorTarget.x = math::q32_32::from_raw(saturatingAddRaw(
                    actorTarget.x.raw(), formation->offsetX.raw()));
                actorTarget.y = math::q32_32::from_raw(saturatingAddRaw(
                    actorTarget.y.raw(), formation->offsetY.raw()));
            }
            if (!hasExplicitFormationOffset && playerGroupCenterKnown &&
                playerGroupMemberCount > 1 &&
                effectiveMoveRouteSubtype != ObjectMoveRouteSubtype::Tighten) {
                const TransformComponent* transform =
                    ecs::try_get<TransformComponent>(registry, entity);
                const ObjectKindOfComponent* kinds =
                    ecs::try_get<ObjectKindOfComponent>(registry, entity);
                const ObjectGeometryComponent* geometry =
                    ecs::try_get<ObjectGeometryComponent>(registry, entity);
                const ObjectLocomotionComponent* locomotion =
                    ecs::try_get<ObjectLocomotionComponent>(registry, entity);
                if (transform && locomotion &&
                    !hasKindOf(kinds, game::ObjectKindOf::Immobile) &&
                    !hasKindOf(kinds, game::ObjectKindOf::Aircraft) &&
                    (hasKindOf(kinds, game::ObjectKindOf::Infantry) ||
                     hasKindOf(kinds, game::ObjectKindOf::Vehicle)) &&
                    !isObjectDisabledBy(registry, entity,
                                        ObjectDisabledReason::Held,
                                        issuedTick)) {
                    const LogicFixedVec3 position =
                        readAuthoritativeObjectPosition(
                            registry, entity, *transform);
                    math::q32_32 offsetX =
                        position.x - playerGroupCenterX;
                    math::q32_32 offsetY =
                        position.y - playerGroupCenterY;
                    const math::q32_32 length =
                        math::q32_32::sqrt(offsetX * offsetX +
                                            offsetY * offsetY);
                    const math::q32_32 radius = geometry
                        ? math::q32_32::max(
                              {}, geometry->boundingCircleRadiusFixed)
                        : math::q32_32{};
                    const math::q32_32 maximumOffset = math::q32_32::max(
                        math::q32_32{int32_t{1}},
                        radius * math::q32_32{int32_t{6}});
                    if (length > maximumOffset) {
                        offsetX = offsetX / length * maximumOffset;
                        offsetY = offsetY / length * maximumOffset;
                    }
                    actorTarget.x += offsetX;
                    actorTarget.y += offsetY;
                }
            }
        }
        ObjectOrderIntent admittedOrder{
            .kind = kind,
            .tacticalAttackSubtype = tacticalAttackSubtype,
            .source = admittedSource,
            .contextPlayer = contextPlayer,
            .issuedTick = issuedTick,
            .sourceSequence = sequence,
            .sourceScriptId = sourceScriptId,
            .targetObject = targetObject,
            .targetX = actorTarget.x,
            .targetY = actorTarget.y,
            .targetZ = actorTarget.z,
            .hasTargetPosition = actorTarget.valid,
            .placementYawRadians = placementYawRadians,
            .placementEndX = placementEndPosition.x,
            .placementEndY = placementEndPosition.y,
            .placementEndZ = placementEndPosition.z,
            .hasPlacementEndPosition = placementEndPosition.valid,
            .contentName = container::String(contentName),
            .maximumShots = maximumShots,
            .forceAttack = forceAttack,
            .attackMove = attackMove,
            .combatDrop = combatDropCommand,
            .moveRouteSubtype = effectiveMoveRouteSubtype,
            .waypointStartId = waypointStartId,
            .waypointGraphRevision = waypointGraphRevision,
            .waypointTeam = waypointTeamRoute
                ? scenarioTeam
                : INVALID_OBJECT_TEAM_ID,
            .waypointGroupOffsetX = waypointGroupOffsetX,
            .waypointGroupOffsetY = waypointGroupOffsetY,
            .waypointGroupSpeed = waypointGroupSpeedKnown
                ? waypointGroupSpeed : math::q32_32{},
            .groupPathId = groupMember ? groupPath->id : 0,
            .groupPathMemberOrdinal = groupMemberOrdinal,
            .groupPathMemberCount = groupMember
                ? static_cast<uint32_t>(groupPath->members.size()) : 0,
            .groupPathStartX = groupMember
                ? groupPath->startX : math::q32_32{},
            .groupPathStartY = groupMember
                ? groupPath->startY : math::q32_32{},
            .groupPathStartZ = groupMember
                ? groupPath->startZ : math::q32_32{},
            .groupPathOffsetX = groupMember
                ? groupMember->offsetX : math::q32_32{},
            .groupPathOffsetY = groupMember
                ? groupMember->offsetY : math::q32_32{},
            .allArmyHunt = allArmyHunt,
            .useTeamCommonTarget = useTeamCommonTarget,
            .guardWithoutPursuit = guardWithoutPursuit,
            .guardFlyingOnly = guardFlyingOnly,
            .tacticalTargetTeam = tacticalTargetTeam,
            .tacticalTargetAreaId = tacticalTargetAreaId,
            .tacticalTargetRevision = tacticalTargetRevision,
            .systemPurpose = admittedPurpose,
        };
        const ObjectIdentityComponent* identity =
            ecs::try_get<ObjectIdentityComponent>(registry, entity);
        if (identity) {
            static_cast<void>(ObjectContainmentSystem::fanoutDirectAttackOrder(
                registry, objects, identity->id, admittedOrder, queued,
                issuedTick));
            if (kind == ObjectOrderKind::Attack) {
                // AIGroup orders non-free SpawnBehavior slaves as part of the
                // master's attack command. They own independent AIUpdate
                // queues in TD, so fan the immutable intent out explicitly.
                for (const ObjectId child : ObjectSpawnSlaveSystem{}.spawnChildren(
                         registry, objects, identity->id)) {
                    if (std::binary_search(
                            canonical->begin(), canonical->end(), child)) {
                        continue;
                    }
                    const std::optional<ecs::entity> childEntity =
                        objects.entityFromId(child);
                    if (!childEntity || !hasDirectAttackOrderConsumer(
                            capabilities, child)) {
                        continue;
                    }
                    ObjectOrderQueueComponent* childQueue =
                        ecs::try_get<ObjectOrderQueueComponent>(
                            registry, *childEntity);
                    if (!childQueue) {
                        childQueue = &ecs::emplace<ObjectOrderQueueComponent>(
                            registry, *childEntity);
                    }
                    if (!queued) childQueue->orders.clear();
                    childQueue->orders.push_back(admittedOrder);
                    ++childQueue->revision;
                    if (!isStrategicAICommand) {
                        ++childQueue->externalRevision;
                        if (childQueue->externalRevision == 0)
                            ++childQueue->externalRevision;
                    }
                    if (!queued && !isStrategicAICommand) {
                        childQueue->replacementExternalRevision =
                            childQueue->externalRevision;
                        childQueue->replacementExternalSource =
                            admittedOrder.source;
                        childQueue->replacementExternalKind =
                            admittedOrder.kind;
                    }
                }
            }
        }
        if (kind == ObjectOrderKind::Attack &&
            (!identity || !hasDirectAttackOrderConsumer(
                              capabilities, identity->id) ||
             !ecs::try_get<ObjectWeaponComponent>(registry, entity))) {
            continue;
        }
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
        if (!queue)
            queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, entity);
        const bool playerWaypointMove = isPlayerCommand && queued &&
            kind == ObjectOrderKind::Move && !attackMove && !targetObject &&
            actorTarget.valid;
        if (!queued) {
            queue->orders.clear();
            ecs::remove<ObjectSystemPathSequenceComponent>(registry, entity);
        }
        if (playerWaypointMove) {
            appendPlayerWaypointMove(
                registry, entity, *queue, std::move(admittedOrder));
        } else {
            queue->orders.push_back(std::move(admittedOrder));
        }
        ++queue->revision;
        if (!isStrategicAICommand) {
            ++queue->externalRevision;
            if (queue->externalRevision == 0) ++queue->externalRevision;
        }
        if (playerWaypointMove) {
            ObjectSystemPathSequenceComponent* path =
                ecs::try_get<ObjectSystemPathSequenceComponent>(
                    registry, entity);
            if (path && path->activeQueueRevision == 0) {
                path->activeQueueRevision = queue->revision;
                path->activeExternalRevision = queue->externalRevision;
            }
        }
        if (!queued && !isStrategicAICommand) {
            queue->replacementExternalRevision = queue->externalRevision;
            queue->replacementExternalSource = admittedSource;
            queue->replacementExternalKind = kind;
        }
    }
    return {.accepted = true, .actorCount = entities.size()};
}

} // namespace

std::optional<PlayerOrder> OrderExecutor::fromGameCommand(const GameCommand& command,
                                                           container::String* error) {
    if (error) error->clear();
    const std::optional<ObjectOrderKind> kind = toOrderKind(command.type);
    if (!kind) {
        setError(error, "game command type does not produce a unit order");
        return std::nullopt;
    }
    if (!command.player.isMapPlayer()) {
        setError(error, "game command has an invalid map player");
        return std::nullopt;
    }
    PlayerOrder order;
    order.player = command.player;
    order.tick = command.tick;
    order.sequence = command.sequence;
    order.kind = *kind;
    const std::optional<container::Vector<ObjectId>> canonical = canonicalActors(
        command.actors, kMaximumPlayerOrderActors, error);
    if (!canonical) return std::nullopt;
    order.actors = *canonical;
    order.targetObject = command.targetObject;
    order.targetPosition = command.targetPosition;
    order.placementYawRadians = command.placementYawRadians;
    order.placementEndPosition = command.placementEndPosition;
    order.contentName = command.commandName;
    order.attackMove = command.type == GameCommandType::AttackMove;
    order.combatDrop = command.type == GameCommandType::CombatDrop;
    if (*kind == ObjectOrderKind::TacticalAttack) {
        order.tacticalAttackSubtype = ObjectTacticalAttackSubtype::Guard;
        order.guardWithoutPursuit =
            command.type == GameCommandType::GuardWithoutPursuit;
        order.guardFlyingOnly =
            command.type == GameCommandType::GuardFlyingUnitsOnly;
    }
    order.queued = command.queued;
    order.forceAttack = command.forceAttack;

    if (order.forceAttack && *kind != ObjectOrderKind::Attack) {
        setError(error, "force-attack is valid only for attack orders");
        return std::nullopt;
    }

    switch (*kind) {
    case ObjectOrderKind::Move:
        if (!order.targetPosition.valid) {
            setError(error, "move order requires a target position");
            return std::nullopt;
        }
        break;
    case ObjectOrderKind::Stop:
        if (order.queued) {
            setError(error, "stop is an immediate queue-clear operation and cannot be queued");
            return std::nullopt;
        }
        break;
    case ObjectOrderKind::Attack:
        if (static_cast<bool>(order.targetObject) ==
            order.targetPosition.valid) {
            setError(error,
                     "attack order requires exactly one object or position target");
            return std::nullopt;
        }
        break;
    case ObjectOrderKind::TacticalAttack:
        if (order.tacticalAttackSubtype !=
                ObjectTacticalAttackSubtype::Guard ||
            static_cast<bool>(order.targetObject) ==
                order.targetPosition.valid ||
            !order.contentName.empty()) {
            setError(error,
                     "player guard requires exactly one object or position target");
            return std::nullopt;
        }
        break;
    case ObjectOrderKind::Build:
        if (order.contentName.empty() || !order.targetPosition.valid) {
            setError(error, "build order requires a template name and placement position");
            return std::nullopt;
        }
        break;
    case ObjectOrderKind::CommandButton:
    case ObjectOrderKind::SpecialPower:
        if (order.contentName.empty()) {
            setError(error, "command-button order requires an authored content name");
            return std::nullopt;
        }
        break;
    }
    return order;
}

OrderExecutionResult OrderExecutor::executePlayer(
    ecs::registry& registry, const PlayerRegistry& players,
    const ObjectLifecycle& objects, const PlayerOrder& order,
    bool allowHackInternetCommand, bool allowCombatDropCommand,
    int64_t groupMoveClickToGatherFactorRaw,
    const ai::ObjectAIOrderCapabilitySnapshot& capabilities) {
    return execute(registry, players, objects, nullptr, capabilities,
                   OrderAuthority::PlayerCommand,
                   order.player, INVALID_OBJECT_TEAM_ID, order.tick, order.sequence, 0,
                   order.kind, ObjectOrderSystemPurpose::Generic, order.actors,
                   order.targetObject, order.targetPosition,
                   order.placementYawRadians, order.placementEndPosition,
                   order.contentName, order.forceAttack, std::nullopt,
                   order.attackMove,
                   ObjectMoveRouteSubtype::Direct,
                   std::numeric_limits<uint32_t>::max(), 0,
                   order.tacticalAttackSubtype, false, false,
                   order.guardWithoutPursuit, order.guardFlyingOnly,
                   INVALID_OBJECT_TEAM_ID,
                   std::numeric_limits<uint32_t>::max(), 0, order.queued,
                   allowHackInternetCommand,
                   order.combatDrop || allowCombatDropCommand,
                   groupMoveClickToGatherFactorRaw,
                   order.groupPath ? &*order.groupPath : nullptr);
}

OrderExecutionResult OrderExecutor::executeScript(
    ecs::registry& registry, const PlayerRegistry& players,
    const ObjectLifecycle& objects, const ObjectTeamRegistry* teams,
    const ScriptOrderIntent& order, bool allowHackInternetCommand,
    bool allowCombatDropCommand,
    const ai::ObjectAIOrderCapabilitySnapshot& capabilities) {
    const ObjectTeamId requiredTeam = order.authority == ScriptOrderAuthority::ScenarioTeam
        ? order.scenarioTeam : INVALID_OBJECT_TEAM_ID;
    return execute(registry, players, objects, teams, capabilities,
                   OrderAuthority::ScenarioScript,
                   order.contextPlayer, requiredTeam, order.confirmedTick,
                   order.sourceEffectOrdinal, order.sourceScriptId, order.kind,
                   order.systemPurpose, order.actors,
                   order.targetObject, order.targetPosition, math::q32_32{}, {},
                   order.contentName, order.forceAttack, order.maximumShots,
                   order.attackMove, order.moveRouteSubtype, order.waypointStartId,
                   order.waypointGraphRevision, order.tacticalAttackSubtype, order.allArmyHunt,
                   order.useTeamCommonTarget, false, false,
                   order.tacticalTargetTeam,
                   order.tacticalTargetAreaId, order.tacticalTargetRevision,
                   order.queued, allowHackInternetCommand,
                   allowCombatDropCommand, 0, nullptr);
}

OrderExecutionResult OrderExecutor::executeStrategicAttack(
    ecs::registry& registry, const PlayerRegistry& players,
    const ObjectLifecycle& objects, PlayerId player, GameTick tick,
    uint32_t sequence, const container::Vector<ObjectId>& actors,
    ObjectId target,
    const ai::ObjectAIOrderCapabilitySnapshot& capabilities) {
    return execute(
        registry, players, objects, nullptr, capabilities,
        OrderAuthority::StrategicAI, player, INVALID_OBJECT_TEAM_ID,
        tick, sequence, 0, ObjectOrderKind::Attack,
        ObjectOrderSystemPurpose::StrategicAI, actors, target,
        CommandPosition{}, math::q32_32{}, CommandPosition{},
        container::StringView{}, false, std::nullopt, false,
        ObjectMoveRouteSubtype::Direct,
        std::numeric_limits<uint32_t>::max(), 0,
        ObjectTacticalAttackSubtype::None, false, false, false, false,
        INVALID_OBJECT_TEAM_ID, std::numeric_limits<uint32_t>::max(), 0,
        false, false, false, 0, nullptr);
}

OrderExecutionResult OrderExecutor::executeScatter(
    ecs::registry& registry, const PlayerRegistry& players,
    const ObjectLifecycle& objects, PlayerId player, GameTick tick,
    uint32_t sequence, container::Span<const ObjectId> actors) {
    const PlayerState* playerState = players.get(player);
    if (!playerState || !playerState->isCommandPlayer()) {
        return rejected(OrderRejectionReason::InvalidPlayer,
                        "scatter issuer is not a live command player");
    }
    container::Vector<ObjectId> authoredActors(actors.begin(), actors.end());
    container::String actorError;
    const auto canonical = canonicalActors(
        authoredActors, kMaximumPlayerOrderActors, &actorError);
    if (!canonical) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        std::move(actorError));
    }

    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
        LogicFixedVec3 position{};
        math::q32_32 radius{int32_t{1}};
        Unsigned128 distance{};
        bool movable = false;
    };
    container::Vector<Candidate> candidates;
    candidates.reserve(canonical->size());
    int64_t sumX = 0;
    int64_t sumY = 0;
    for (const ObjectId actor : *canonical) {
        const std::optional<ecs::entity> entity = objects.entityFromId(actor);
        if (!entity) {
            return rejected(
                objects.isPendingDestroy(actor)
                    ? OrderRejectionReason::PendingDestroy
                    : OrderRejectionReason::MissingActor,
                "scatter references an unavailable actor");
        }
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, *entity);
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, *entity);
        if (!owner || !transform) {
            return rejected(OrderRejectionReason::MissingActor,
                            "scatter actor lacks owner or transform");
        }
        if (owner->player != player) {
            return rejected(OrderRejectionReason::OwnershipMismatch,
                            "scatter contains an actor not owned by its issuer");
        }
        const LogicFixedVec3 position =
            readAuthoritativeObjectPosition(registry, *entity, *transform);
        if ((position.x.raw() > 0 && sumX > INT64_MAX - position.x.raw()) ||
            (position.x.raw() < 0 && sumX < INT64_MIN - position.x.raw()) ||
            (position.y.raw() > 0 && sumY > INT64_MAX - position.y.raw()) ||
            (position.y.raw() < 0 && sumY < INT64_MIN - position.y.raw())) {
            return rejected(OrderRejectionReason::MalformedOrder,
                            "scatter centroid exceeds fixed coordinate range");
        }
        sumX += position.x.raw();
        sumY += position.y.raw();
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, *entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, *entity);
        const bool movable =
            !hasKindOf(kinds, game::ObjectKindOf::Immobile) &&
            ecs::try_get<ObjectLocomotionComponent>(registry, *entity) &&
            !isObjectDisabledBy(
                registry, *entity, ObjectDisabledReason::Held, tick) &&
            !rejectsExternalOrdersWhileHeadingOffMap(registry, *entity);
        candidates.push_back({
            .object = actor,
            .entity = *entity,
            .position = position,
            .radius = geometry
                ? geometry->boundingCircleRadiusFixed
                : math::q32_32{int32_t{1}},
            .movable = movable,
        });
    }
    if (candidates.empty()) {
        return rejected(OrderRejectionReason::MalformedOrder,
                        "scatter requires a non-empty actor group");
    }

    const int64_t count = static_cast<int64_t>(candidates.size());
    math::q32_32 centerX = math::q32_32::from_raw(sumX / count);
    const math::q32_32 centerY = math::q32_32::from_raw(sumY / count);
    for (Candidate& candidate : candidates) {
        candidate.distance = squaredDistance(
            candidate.position.x.raw() - centerX.raw(),
            candidate.position.y.raw() - centerY.raw());
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  if (left.distance.high != right.distance.high ||
                      left.distance.low != right.distance.low) {
                      return greaterDistance(left.distance, right.distance);
                  }
                  return left.object.value < right.object.value;
              });

    const math::q32_32 centerTieBreak =
        math::q32_32::from_raw((int64_t{1} << 32) / 100);
    size_t admitted = 0;
    for (const Candidate& candidate : candidates) {
        if (!candidate.movable) continue;
        centerX -= centerTieBreak;
        const int64_t dxRaw = candidate.position.x.raw() - centerX.raw();
        const int64_t dyRaw = candidate.position.y.raw() - centerY.raw();
        const int64_t lengthRaw = fixedLengthRaw(dxRaw, dyRaw);
        if (lengthRaw <= 0) continue;
        const math::q32_32 length = math::q32_32::from_raw(lengthRaw);
        const math::q32_32 distance = candidate.radius *
            math::q32_32{int32_t{4}};
        const math::q32_32 targetX = candidate.position.x +
            math::q32_32::from_raw(dxRaw) / length * distance;
        const math::q32_32 targetY = candidate.position.y +
            math::q32_32::from_raw(dyRaw) / length * distance;

        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, candidate.entity);
        if (!queue) {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(
                registry, candidate.entity);
        }
        queue->orders.clear();
        queue->orders.push_back({
            .kind = ObjectOrderKind::Move,
            .source = ObjectOrderSource::Player,
            .contextPlayer = player,
            .issuedTick = tick,
            .sourceSequence = sequence,
            .targetX = targetX,
            .targetY = targetY,
            .targetZ = candidate.position.z,
            .hasTargetPosition = true,
        });
        ++queue->revision;
        ++queue->externalRevision;
        if (queue->externalRevision == 0u) ++queue->externalRevision;
        ++admitted;
    }
    return {.accepted = true, .actorCount = admitted};
}

} // namespace engine
