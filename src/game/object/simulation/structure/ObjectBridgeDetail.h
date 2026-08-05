#pragma once

#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/definition/ObjectKindOf.h"

namespace game::terrain {
struct WaypointRecord;
}

namespace engine {

struct ObjectKindOfComponent;
struct ObjectOrderIntent;
struct ObjectOrderQueueComponent;
struct ObjectFixedTransformComponent;
struct ObjectPhysicsComponent;
struct RenderModelComponent;

namespace detail {

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept;
[[nodiscard]] uint64_t saturatingAdd(uint64_t left,
                                     uint64_t right) noexcept;
[[nodiscard]] math::q32_32 distanceSquared(
    const LogicFixedVec3& left,
    const LogicFixedVec3& right) noexcept;
[[nodiscard]] bool railroadHasKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf wanted) noexcept;
[[nodiscard]] math::q32_32 fixedDistance(
    const LogicFixedVec3& left,
    const LogicFixedVec3& right) noexcept;
[[nodiscard]] LogicFixedVec3 transformLocalBridgePoint(
    const LogicFixedVec3& origin, math::q32_32 yaw,
    const LogicFixedVec3& local) noexcept;
void projectRailedDockerMoving(ecs::registry& registry, ecs::entity entity,
                               bool moving, uint64_t confirmedTick);
void reverseScaffoldMotion(
    ObjectBridgeScaffoldComponent& scaffold) noexcept;
[[nodiscard]] bool advanceScaffoldMotion(
    ObjectBridgeScaffoldComponent& scaffold) noexcept;
[[nodiscard]] bool isRailedTransportOrder(
    const ObjectOrderIntent& order, size_t instance) noexcept;
[[nodiscard]] bool replaceRailedTransportMove(
    ObjectOrderQueueComponent& queue,
    ObjectRailedTransportRuntime& runtime, size_t instance, PlayerId owner,
    const LogicFixedVec3& target, uint64_t confirmedTick);
void loadRailedTransportPaths(
    ObjectRailedTransportRuntime& runtime,
    const game::ObjectRailedTransportAiRule& rule,
    const game::terrain::TerrainLogic& terrain);
void loadRailroadTrack(ObjectRailroadRuntime& runtime,
                       const LogicFixedVec3& position,
                       const game::terrain::TerrainLogic& terrain);
[[nodiscard]] uint32_t publishRailroadPosition(
    ObjectRailroadRuntime& runtime, TransformComponent& transform,
    ObjectPhysicsComponent* physics,
    ObjectFixedTransformComponent* fixedTransform);
[[nodiscard]] LogicFixedVec3 lowered(
    const LogicFixedVec3& value, math::q32_32 amount) noexcept;
[[nodiscard]] ObjectBridgeScaffoldSpawnSpec makeScaffoldSpec(
    const ObjectBridgeScaffoldLayoutRequest& request,
    container::StringView templateName,
    const LogicFixedVec3& risePosition,
    const LogicFixedVec3& buildPosition, const LogicFixedVec3& center,
    math::q32_32 sunkenHeight, math::q32_32 orientation);

} // namespace detail
} // namespace engine
