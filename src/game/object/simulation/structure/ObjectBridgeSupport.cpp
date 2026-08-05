#include "game/object/simulation/structure/ObjectBridgeDetail.h"

#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/data/base/ObjectSimulationRules.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>

namespace engine {

namespace detail {

using container::asciiEqualIgnoreCase;
using container::endsWithIgnoreCase;

using Fixed = math::q32_32;

[[nodiscard]] uint64_t millisecondsToTicks(uint32_t milliseconds,
                                           uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1u, framesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * fps + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left,
                                     uint64_t right) noexcept {
    if (right > std::numeric_limits<uint64_t>::max() - left) {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

[[nodiscard]] Fixed distanceSquared(const LogicFixedVec3& left,
                                    const LogicFixedVec3& right) noexcept {
    const Fixed dx = left.x - right.x;
    const Fixed dy = left.y - right.y;
    const Fixed dz = left.z - right.z;
    return dx * dx + dy * dy + dz * dz;
}

[[nodiscard]] bool railroadHasKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf wanted) noexcept {
    return kinds && game::objectHasKind(kinds->mask, wanted);
}

[[nodiscard]] Fixed fixedDistance(const LogicFixedVec3& left,
                                  const LogicFixedVec3& right) noexcept {
    const Fixed dx = right.x - left.x;
    const Fixed dy = right.y - left.y;
    const Fixed dz = right.z - left.z;
    return Fixed::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]] LogicFixedVec3 transformLocalBridgePoint(
    const LogicFixedVec3& origin, Fixed yaw,
    const LogicFixedVec3& local) noexcept {
    const math::q32_32_sincos rotation = math::fixed_sincos(yaw);
    return {
        .x = origin.x + local.x * rotation.cosine -
            local.y * rotation.sine,
        .y = origin.y + local.x * rotation.sine +
            local.y * rotation.cosine,
        .z = origin.z + local.z,
    };
}

void projectRailedDockerMoving(ecs::registry& registry, ecs::entity entity,
                               bool moving, uint64_t confirmedTick) {
    if (!ecs::try_get<RenderModelComponent>(registry, entity)) return;
    static const game::ModelConditionMask movingMask =
        game::modelConditionMaskOf(game::ModelConditionFlag::Moving);
    const game::ModelConditionMask empty;
    publishObjectModelConditionContribution(
        registry, entity, ObjectModelConditionContributionSource::Bridge,
        moving ? empty : movingMask, moving ? movingMask : empty,
        confirmedTick);
}

[[nodiscard]] const LogicFixedVec3& scaffoldTarget(
    const ObjectBridgeScaffoldComponent& scaffold) noexcept {
    switch (scaffold.motion) {
    case ObjectBridgeScaffoldMotion::Rise:
    case ObjectBridgeScaffoldMotion::TearDownAcross:
        return scaffold.risePosition;
    case ObjectBridgeScaffoldMotion::BuildAcross:
        return scaffold.buildPosition;
    case ObjectBridgeScaffoldMotion::Sink:
        return scaffold.createPosition;
    case ObjectBridgeScaffoldMotion::Still:
        return scaffold.position;
    }
    return scaffold.position;
}

void reverseScaffoldMotion(ObjectBridgeScaffoldComponent& scaffold) noexcept {
    switch (scaffold.motion) {
    case ObjectBridgeScaffoldMotion::Still:
        scaffold.motion = ObjectBridgeScaffoldMotion::TearDownAcross;
        break;
    case ObjectBridgeScaffoldMotion::Rise:
        scaffold.motion = ObjectBridgeScaffoldMotion::Sink;
        break;
    case ObjectBridgeScaffoldMotion::BuildAcross:
        scaffold.motion = ObjectBridgeScaffoldMotion::TearDownAcross;
        break;
    case ObjectBridgeScaffoldMotion::TearDownAcross:
        scaffold.motion = ObjectBridgeScaffoldMotion::BuildAcross;
        break;
    case ObjectBridgeScaffoldMotion::Sink:
        scaffold.motion = ObjectBridgeScaffoldMotion::Rise;
        break;
    }
}

[[nodiscard]] bool advanceScaffoldMotion(
    ObjectBridgeScaffoldComponent& scaffold) noexcept {
    if (!scaffold.configured ||
        scaffold.motion == ObjectBridgeScaffoldMotion::Still) {
        return false;
    }

    const LogicFixedVec3 target = scaffoldTarget(scaffold);
    const LogicFixedVec3 start =
        scaffold.motion == ObjectBridgeScaffoldMotion::Rise
        ? scaffold.createPosition
        : scaffold.motion == ObjectBridgeScaffoldMotion::Sink
        ? scaffold.risePosition
        : scaffold.motion == ObjectBridgeScaffoldMotion::BuildAcross
        ? scaffold.risePosition
        : scaffold.buildPosition;
    const Fixed topSpeed =
        scaffold.motion == ObjectBridgeScaffoldMotion::Rise ||
            scaffold.motion == ObjectBridgeScaffoldMotion::Sink
        ? scaffold.verticalSpeedPerFrame
        : scaffold.lateralSpeedPerFrame;
    const Fixed remaining = fixedDistance(scaffold.position, target);
    const Fixed total = fixedDistance(start, target);
    bool reached = remaining <= Fixed{};

    if (!reached) {
        // RefCode slows during the final quarter of the authored segment,
        // clamps to 8% of top speed, and retains a 0.001 unit/frame floor.
        const Fixed quarterDistance = total * Fixed::from_fraction(1, 4);
        Fixed speed = quarterDistance > Fixed{}
            ? (remaining / quarterDistance) * topSpeed
            : topSpeed;
        speed = std::clamp(
            speed, topSpeed * Fixed::from_fraction(2, 25), topSpeed);
        speed = Fixed::max(speed, Fixed::from_fraction(1, 1000));
        if (speed >= remaining) {
            scaffold.position = target;
            reached = true;
        } else {
            const Fixed ratio = speed / remaining;
            scaffold.position.x += (target.x - scaffold.position.x) * ratio;
            scaffold.position.y += (target.y - scaffold.position.y) * ratio;
            scaffold.position.z += (target.z - scaffold.position.z) * ratio;
        }
    }

    if (reached) {
        switch (scaffold.motion) {
        case ObjectBridgeScaffoldMotion::Rise:
            scaffold.motion = ObjectBridgeScaffoldMotion::BuildAcross;
            break;
        case ObjectBridgeScaffoldMotion::BuildAcross:
            scaffold.motion = ObjectBridgeScaffoldMotion::Still;
            break;
        case ObjectBridgeScaffoldMotion::TearDownAcross:
            scaffold.motion = ObjectBridgeScaffoldMotion::Sink;
            break;
        case ObjectBridgeScaffoldMotion::Sink:
        case ObjectBridgeScaffoldMotion::Still:
            break;
        }
    }
    return true;
}

[[nodiscard]] bool isRailedTransportOrder(
    const ObjectOrderIntent& order, size_t instance) noexcept {
    return order.source == ObjectOrderSource::System &&
        order.systemPurpose == ObjectOrderSystemPurpose::RailedTransport &&
        order.systemPurposeInstance == static_cast<uint32_t>(instance);
}

[[nodiscard]] bool replaceRailedTransportMove(
    ObjectOrderQueueComponent& queue,
    ObjectRailedTransportRuntime& runtime, size_t instance, PlayerId owner,
    const LogicFixedVec3& target, uint64_t confirmedTick) {
    if (!queue.orders.empty()) {
        if (!isRailedTransportOrder(queue.orders.front(), instance)) {
            return false;
        }
        queue.orders.erase(queue.orders.begin());
    }
    queue.orders.insert(queue.orders.begin(), ObjectOrderIntent{
        .kind = ObjectOrderKind::Move,
        .source = ObjectOrderSource::System,
        .contextPlayer = owner,
        .issuedTick = confirmedTick,
        .sourceSequence = runtime.nextCommandSequence,
        .targetX = target.x,
        .targetY = target.y,
        .targetZ = target.z,
        .hasTargetPosition = true,
        .systemPurpose = ObjectOrderSystemPurpose::RailedTransport,
        .systemPurposeInstance = static_cast<uint32_t>(instance),
    });
    ++queue.revision;
    ++runtime.nextCommandSequence;
    if (runtime.nextCommandSequence == 0) ++runtime.nextCommandSequence;
    return true;
}

void loadRailedTransportPaths(
    ObjectRailedTransportRuntime& runtime,
    const game::ObjectRailedTransportAiRule& rule,
    const game::terrain::TerrainLogic& terrain) {
    runtime.pathCount = 0;
    for (size_t index = 0;
         index < detail::ObjectRailedTransportMaximumPaths; ++index) {
        char suffix[3] = {
            static_cast<char>('0' + ((index + 1) / 10)),
            static_cast<char>('0' + ((index + 1) % 10)), '\0'};
        const container::String startName =
            rule.pathPrefixName + "Start" + suffix;
        const container::String endName =
            rule.pathPrefixName + "End" + suffix;
        const game::terrain::WaypointRecord* start =
            terrain.waypointByName(startName);
        const game::terrain::WaypointRecord* end =
            terrain.waypointByName(endName);
        if (!start || !end) continue;
        runtime.paths[runtime.pathCount++] = {
            .startWaypointId = start->id,
            .endWaypointId = end->id,
            .startPosition = {
                Fixed::from_raw(start->positionRaw[0]),
                Fixed::from_raw(start->positionRaw[1]),
                Fixed::from_raw(start->positionRaw[2]),
            },
            .endPosition = {
                Fixed::from_raw(end->positionRaw[0]),
                Fixed::from_raw(end->positionRaw[1]),
                Fixed::from_raw(end->positionRaw[2]),
            },
        };
    }
    runtime.waypointDataLoaded = true;
}

void loadRailroadTrack(ObjectRailroadRuntime& runtime,
                       const LogicFixedVec3& position,
                       const game::terrain::TerrainLogic& terrain) {
    runtime.waypointIds.clear();
    runtime.trackPoints.clear();
    runtime.trackLength = {};
    runtime.looping = false;
    const game::terrain::WaypointRecord* anchor = terrain.nearestWaypointRaw(
        position.x.raw(), position.y.raw(), position.z.raw());
    if (!anchor) {
        runtime.trackDataLoaded = true;
        return;
    }

    const game::terrain::WaypointRecord* current = anchor;
    constexpr size_t kMaximumTrackPoints = 512;
    while (current && runtime.waypointIds.size() < kMaximumTrackPoints) {
        const LogicFixedVec3 point{
            Fixed::from_raw(current->positionRaw[0]),
            Fixed::from_raw(current->positionRaw[1]),
            Fixed::from_raw(current->positionRaw[2]),
        };
        if (std::find(runtime.waypointIds.begin(), runtime.waypointIds.end(),
                      current->id) != runtime.waypointIds.end()) {
            runtime.looping = current->id == anchor->id &&
                runtime.waypointIds.size() > 1;
            runtime.waypointIds.push_back(current->id);
            if (!runtime.trackPoints.empty()) {
                runtime.trackLength += fixedDistance(
                    runtime.trackPoints.back(), point);
            }
            runtime.trackPoints.push_back(point);
            break;
        }
        runtime.waypointIds.push_back(current->id);
        if (!runtime.trackPoints.empty()) {
            runtime.trackLength += fixedDistance(
                runtime.trackPoints.back(), point);
        }
        runtime.trackPoints.push_back(point);
        if (current->links.empty()) break;
        const game::terrain::WaypointRecord* next =
            terrain.waypointById(current->links.front());
        if (!next) break;
        current = next;
    }
    runtime.trackDataLoaded = true;
    ++runtime.revision;
}

[[nodiscard]] uint32_t publishRailroadPosition(
    ObjectRailroadRuntime& runtime, TransformComponent& transform,
    ObjectPhysicsComponent* physics,
    ObjectFixedTransformComponent* fixedTransform) {
    if (runtime.trackPoints.size() < 2 || runtime.trackLength <= Fixed{}) {
        return std::numeric_limits<uint32_t>::max();
    }

    Fixed remaining = runtime.trackDistance;
    const LogicFixedVec3* from = nullptr;
    const LogicFixedVec3* to = nullptr;
    Fixed segmentLength{};
    uint32_t segmentIndex = 0;
    for (size_t index = 1; index < runtime.trackPoints.size(); ++index) {
        from = &runtime.trackPoints[index - 1];
        to = &runtime.trackPoints[index];
        const Fixed dx = to->x - from->x;
        const Fixed dy = to->y - from->y;
        const Fixed dz = to->z - from->z;
        const Fixed squared = dx * dx + dy * dy + dz * dz;
        segmentLength = squared > Fixed{} ? Fixed::sqrt(squared) : Fixed{};
        if (remaining <= segmentLength ||
            index + 1 == runtime.trackPoints.size()) {
            segmentIndex = static_cast<uint32_t>(index - 1);
            break;
        }
        remaining -= segmentLength;
    }
    if (!from || !to || segmentLength <= Fixed{}) {
        return std::numeric_limits<uint32_t>::max();
    }
    const Fixed ratio = std::clamp(remaining / segmentLength, Fixed{},
                                   Fixed{int32_t{1}});
    const Fixed x = from->x + (to->x - from->x) * ratio;
    const Fixed y = from->y + (to->y - from->y) * ratio;
    const Fixed z = from->z + (to->z - from->z) * ratio;
    const Fixed yaw = math::fixed_atan2(to->y - from->y, to->x - from->x);
    transform.x = x.to_float();
    transform.y = y.to_float();
    transform.z = z.to_float();
    transform.rotation = yaw.to_float();
    if (fixedTransform) {
        fixedTransform->position = {x, y, z};
        fixedTransform->yawRadians = yaw;
        fixedTransform->authoritative = true;
    }
    if (physics) {
        physics->position = {x, y, z};
        physics->lastPublishedPosition = physics->position;
        physics->velocityUnitsPerSecond = {};
        physics->yaw = yaw;
        physics->ownsAttitude = true;
        physics->hasAuthoritativePosition = true;
    }
    return segmentIndex;
}

[[nodiscard]] LogicFixedVec3 lowered(const LogicFixedVec3& value,
                                     Fixed amount) noexcept {
    LogicFixedVec3 result = value;
    result.z -= amount;
    return result;
}

[[nodiscard]] ObjectBridgeScaffoldSpawnSpec makeScaffoldSpec(
    const ObjectBridgeScaffoldLayoutRequest& request,
    container::StringView templateName, const LogicFixedVec3& risePosition,
    const LogicFixedVec3& buildPosition, const LogicFixedVec3& center,
    Fixed sunkenHeight, Fixed orientation) {
    constexpr Fixed kLegacySunkenFudge{int32_t{8}};
    const Fixed distanceFromRiseToCenter =
        fixedDistance(risePosition, center);
    const Fixed distanceFromBuildToCenter =
        fixedDistance(buildPosition, center);
    const Fixed lateralSpeed = distanceFromRiseToCenter > Fixed{}
        ? request.lateralSpeedPerFrame *
              (distanceFromBuildToCenter / distanceFromRiseToCenter)
        : Fixed{};

    ObjectBridgeScaffoldSpawnSpec result;
    result.templateName = container::String{templateName};
    result.motion = {
        .kind = ObjectBridgeScaffoldRequestKind::CreateAndBuild,
        .bridge = request.bridge,
        .createPosition = lowered(
            risePosition, sunkenHeight + kLegacySunkenFudge),
        .risePosition = risePosition,
        .buildPosition = buildPosition,
        .orientationRadians = orientation,
        .lateralSpeedPerFrame = lateralSpeed,
        .verticalSpeedPerFrame = request.verticalSpeedPerFrame,
        .sequence = request.requestSequence,
        .confirmedTick = request.confirmedTick,
    };
    return result;
}

} // namespace

} // namespace engine
