#include "core/container/container_types.h"
#include "W3dFixedPose.h"

#include <algorithm>
#include <limits>

namespace data::w3d {
namespace {

using Scalar = math::q32_32;

[[nodiscard]] FixedVector3 add(FixedVector3 left,
                               FixedVector3 right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] FixedVector3 scale(FixedVector3 value,
                                 Scalar factor) noexcept {
    return {value.x * factor, value.y * factor, value.z * factor};
}

[[nodiscard]] FixedQuaternion normalized(
    FixedQuaternion value, FixedPoseDiagnostics& diagnostics) noexcept {
    const Scalar lengthSquared = value.x * value.x + value.y * value.y +
        value.z * value.z + value.w * value.w;
    if (lengthSquared <= Scalar{}) {
        ++diagnostics.repairedQuaternionCount;
        return {};
    }
    const Scalar length = Scalar::sqrt(lengthSquared);
    if (length <= Scalar{}) {
        ++diagnostics.repairedQuaternionCount;
        return {};
    }
    return {
        value.x / length,
        value.y / length,
        value.z / length,
        value.w / length,
    };
}

[[nodiscard]] FixedQuaternion multiply(
    const FixedQuaternion& left, const FixedQuaternion& right,
    FixedPoseDiagnostics& diagnostics) noexcept {
    return normalized({
        left.w * right.x + left.x * right.w + left.y * right.z -
            left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w +
            left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x +
            left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y -
            left.z * right.z,
    }, diagnostics);
}

[[nodiscard]] FixedVector3 rotate(
    FixedVector3 value, const FixedQuaternion& rotation) noexcept {
    // q * v * conjugate(q), expanded to avoid an intermediate quaternion.
    const FixedVector3 axis{rotation.x, rotation.y, rotation.z};
    const FixedVector3 doubledCross{
        Scalar{int32_t{2}} * (axis.y * value.z - axis.z * value.y),
        Scalar{int32_t{2}} * (axis.z * value.x - axis.x * value.z),
        Scalar{int32_t{2}} * (axis.x * value.y - axis.y * value.x),
    };
    return add(value, add(scale(doubledCross, rotation.w), {
        axis.y * doubledCross.z - axis.z * doubledCross.y,
        axis.z * doubledCross.x - axis.x * doubledCross.z,
        axis.x * doubledCross.y - axis.y * doubledCross.x,
    }));
}

[[nodiscard]] FixedVector3 vectorFrom(const Vector3& value) noexcept {
    return {
        Scalar{value.x}, Scalar{value.y}, Scalar{value.z},
    };
}

[[nodiscard]] FixedQuaternion quaternionFrom(
    const Quaternion& value, FixedPoseDiagnostics& diagnostics) noexcept {
    return normalized({
        Scalar{value.q[0]}, Scalar{value.q[1]},
        Scalar{value.q[2]}, Scalar{value.q[3]},
    }, diagnostics);
}

[[nodiscard]] Scalar channelScalar(
    const ParsedAnimationChannel& channel, uint32_t frame,
    FixedPoseDiagnostics& diagnostics) noexcept {
    if (channel.vectorLength == 0 || frame < channel.firstFrame ||
        frame > channel.lastFrame) {
        return {};
    }
    const size_t sample = static_cast<size_t>(frame - channel.firstFrame);
    const size_t offset = sample * channel.vectorLength;
    if (offset >= channel.values.size()) {
        ++diagnostics.invalidChannelCount;
        return {};
    }
    return Scalar{channel.values[offset]};
}

[[nodiscard]] FixedQuaternion channelQuaternion(
    const ParsedAnimationChannel& channel, uint32_t frame,
    FixedPoseDiagnostics& diagnostics) noexcept {
    if (channel.vectorLength != 4 || frame < channel.firstFrame ||
        frame > channel.lastFrame) {
        return {};
    }
    const size_t sample = static_cast<size_t>(frame - channel.firstFrame);
    const size_t offset = sample * channel.vectorLength;
    if (offset + 4 > channel.values.size()) {
        ++diagnostics.invalidChannelCount;
        return {};
    }
    return normalized({
        Scalar{channel.values[offset]},
        Scalar{channel.values[offset + 1]},
        Scalar{channel.values[offset + 2]},
        Scalar{channel.values[offset + 3]},
    }, diagnostics);
}

} // namespace

container::Vector<FixedRigidTransform> evaluateFixedPristinePose(
    const ParsedHierarchy& hierarchy, const ParsedAnimation* animation,
    uint32_t frame, math::q32_32 assetScale,
    FixedPoseDiagnostics* diagnostics) {
    FixedPoseDiagnostics localDiagnostics;
    const size_t count = hierarchy.pivots.size();
    container::Vector<int32_t> parents(count, -1);
    for (size_t index = 0; index < count; ++index) {
        const uint32_t source = hierarchy.pivots[index].parentIdx;
        if (source == UINT32_MAX) continue;
        if (source >= count || source == index) {
            ++localDiagnostics.invalidParentCount;
            continue;
        }
        parents[index] = static_cast<int32_t>(source);
    }

    constexpr size_t kNoPath = std::numeric_limits<size_t>::max();
    container::Vector<uint8_t> state(count, 0);
    container::Vector<size_t> pathPosition(count, kNoPath);
    container::Vector<size_t> path;
    path.reserve(count);
    for (size_t start = 0; start < count; ++start) {
        if (state[start] != 0) continue;
        path.clear();
        int32_t current = static_cast<int32_t>(start);
        while (current >= 0 && state[static_cast<size_t>(current)] == 0) {
            const size_t index = static_cast<size_t>(current);
            state[index] = 1;
            pathPosition[index] = path.size();
            path.push_back(index);
            current = parents[index];
        }
        if (current >= 0) {
            const size_t entry = static_cast<size_t>(current);
            if (state[entry] == 1 && pathPosition[entry] != kNoPath) {
                for (size_t index = pathPosition[entry]; index < path.size();
                     ++index) {
                    parents[path[index]] = -1;
                    ++localDiagnostics.cycleNodeCount;
                }
            }
        }
        for (const size_t index : path) {
            state[index] = 2;
            pathPosition[index] = kNoPath;
        }
    }

    container::Vector<FixedVector3> deltaTranslation(count);
    container::Vector<FixedQuaternion> deltaRotation(count);
    if (animation && animation->numFrames != 0) {
        frame = std::min(frame, animation->numFrames - 1u);
        for (const ParsedAnimationChannel& channel : animation->channels) {
            if (channel.pivotIndex >= count) {
                ++localDiagnostics.invalidChannelCount;
                continue;
            }
            // WW3D HTree reserves pivot zero as its identity root and starts
            // animation evaluation at pivot one.  Keep that legacy sentinel
            // semantic even when malformed/MOD content authors a root key.
            if (channel.pivotIndex == 0) continue;
            FixedVector3& translation = deltaTranslation[channel.pivotIndex];
            switch (channel.flags) {
            case AnimChannel_X:
                translation.x = channelScalar(channel, frame, localDiagnostics);
                break;
            case AnimChannel_Y:
                translation.y = channelScalar(channel, frame, localDiagnostics);
                break;
            case AnimChannel_Z:
                translation.z = channelScalar(channel, frame, localDiagnostics);
                break;
            case AnimChannel_Q:
                deltaRotation[channel.pivotIndex] =
                    channelQuaternion(channel, frame, localDiagnostics);
                break;
            default:
                break;
            }
        }
    }

    container::Vector<FixedRigidTransform> local(count);
    // Pivot zero remains the identity root.  Besides matching HTree, this is
    // what makes Get_Bone_Index's zero failure sentinel and HLOD aliases that
    // explicitly bind to the root agree.
    for (size_t index = 1; index < count; ++index) {
        const FixedQuaternion restRotation = quaternionFrom(
            hierarchy.pivots[index].rotation, localDiagnostics);
        local[index].rotation = multiply(
            restRotation, deltaRotation[index], localDiagnostics);
        local[index].translation = add(
            vectorFrom(hierarchy.pivots[index].translation),
            rotate(deltaTranslation[index], restRotation));
    }

    container::Vector<FixedRigidTransform> world(count);
    container::Vector<bool> resolved(count, false);
    for (size_t pass = 0; pass < count; ++pass) {
        bool progressed = false;
        for (size_t index = 0; index < count; ++index) {
            if (resolved[index]) continue;
            const int32_t parent = parents[index];
            if (parent < 0) {
                world[index] = local[index];
            } else if (resolved[static_cast<size_t>(parent)]) {
                const FixedRigidTransform& parentTransform =
                    world[static_cast<size_t>(parent)];
                world[index].rotation = multiply(
                    parentTransform.rotation, local[index].rotation,
                    localDiagnostics);
                world[index].translation = add(
                    parentTransform.translation,
                    rotate(local[index].translation,
                           parentTransform.rotation));
            } else {
                continue;
            }
            resolved[index] = true;
            progressed = true;
        }
        if (!progressed) break;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!resolved[index]) world[index] = local[index];
        world[index].translation = scale(
            world[index].translation, assetScale);
    }
    if (diagnostics) *diagnostics = localDiagnostics;
    return world;
}

} // namespace data::w3d
