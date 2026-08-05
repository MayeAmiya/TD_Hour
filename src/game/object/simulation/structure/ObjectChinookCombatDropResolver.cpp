#include "game/object/simulation/structure/ObjectChinookCombatDropResolver.h"

#include "math/fixed/q32_32_trig.h"

#include <algorithm>

namespace engine {
namespace {

[[nodiscard]] LogicFixedQuaternion normalizedQuaternion(
    LogicFixedQuaternion value) noexcept {
    const math::q32_32 lengthSquared =
        value.x * value.x + value.y * value.y + value.z * value.z +
        value.w * value.w;
    if (lengthSquared <= math::q32_32{}) return {};
    const math::q32_32 length = math::q32_32::sqrt(lengthSquared);
    if (length <= math::q32_32{}) return {};
    value.x /= length;
    value.y /= length;
    value.z /= length;
    value.w /= length;
    return value;
}

[[nodiscard]] LogicFixedQuaternion multiplyQuaternion(
    const LogicFixedQuaternion& left,
    const LogicFixedQuaternion& right) noexcept {
    return normalizedQuaternion({
        .x = left.w * right.x + left.x * right.w +
            left.y * right.z - left.z * right.y,
        .y = left.w * right.y - left.x * right.z +
            left.y * right.w + left.z * right.x,
        .z = left.w * right.z + left.x * right.y -
            left.y * right.x + left.z * right.w,
        .w = left.w * right.w - left.x * right.x -
            left.y * right.y - left.z * right.z,
    });
}

[[nodiscard]] LogicFixedQuaternion yawQuaternion(
    math::q32_32 radians) noexcept {
    const math::q32_32_sincos half = math::fixed_sincos(
        radians / math::q32_32{int32_t{2}});
    return {.z = half.sine, .w = half.cosine};
}

[[nodiscard]] LogicFixedVec3 transformLocalPosition(
    const LogicFixedVec3& origin, math::q32_32_sincos yaw,
    const data::w3d::FixedVector3& local) noexcept {
    return {
        .x = origin.x + local.x * yaw.cosine - local.y * yaw.sine,
        .y = origin.y + local.x * yaw.sine + local.y * yaw.cosine,
        .z = origin.z + local.z,
    };
}

[[nodiscard]] size_t contiguousCount(const auto& values) noexcept {
    size_t count = 0;
    while (count < values.size() && values[count]) ++count;
    return count;
}

} // namespace

std::optional<ObjectChinookCombatDropBeginRequest>
resolveObjectChinookCombatDropBeginFromPristineBones(
    const ObjectChinookCombatDropBeginResolveInput& input,
    const ObjectChinookCombatDropPristineBones& bones,
    const ObjectChinookCombatDropSurfaceResolver& surfaces) {
    if (!input.object || input.numRopes == 0) return std::nullopt;
    const size_t ropeCount = std::min({
        static_cast<size_t>(input.numRopes),
        contiguousCount(bones.ropeStarts),
        contiguousCount(bones.dropStarts),
        kMaximumChinookCombatDropBones,
    });
    if (ropeCount == 0) return std::nullopt;

    ObjectChinookCombatDropBeginRequest output{
        .object = input.object,
        .moduleIndex = input.moduleIndex,
        .confirmedTick = input.confirmedTick,
    };
    output.endpoints.reserve(ropeCount);
    const math::q32_32_sincos yaw =
        math::fixed_sincos(input.objectYawRadians);
    const LogicFixedQuaternion objectOrientation =
        yawQuaternion(input.objectYawRadians);
    for (size_t index = 0; index < ropeCount; ++index) {
        const data::w3d::FixedRigidTransform& rope =
            *bones.ropeStarts[index];
        const data::w3d::FixedRigidTransform& drop =
            *bones.dropStarts[index];
        ObjectChinookRopeEndpoint endpoint;
        endpoint.ropeStart = transformLocalPosition(
            input.objectPosition, yaw, rope.translation);
        endpoint.dropStart = transformLocalPosition(
            input.objectPosition, yaw, drop.translation);
        endpoint.dropOrientation = multiplyQuaternion(
            objectOrientation,
            LogicFixedQuaternion{
                .x = drop.rotation.x,
                .y = drop.rotation.y,
                .z = drop.rotation.z,
                .w = drop.rotation.w,
            });

        endpoint.surfaceHeight = surfaces.surfaceHeight(endpoint.ropeStart);
        output.endpoints.push_back(endpoint);
    }
    return output;
}

} // namespace engine
