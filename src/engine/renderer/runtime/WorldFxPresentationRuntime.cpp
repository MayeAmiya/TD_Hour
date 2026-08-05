#include "engine/renderer/runtime/WorldFxPresentationRuntime.h"

#include "core/container/string_utils.h"
#include "engine/renderer/world/model/Skeleton.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace engine::render {

WorldFxPresentationRuntime::WorldFxPresentationRuntime(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures)
    : typed(device, std::move(textures)) {}

FxSkeletonBindings* WorldFxPresentationRuntime::bindingsFor(
    const container::SharedPtr<const Skeleton>& skeleton) {
    if (!skeleton || skeleton->generation() == 0u) return nullptr;
    const uint64_t generation = skeleton->generation();
    if (const auto found = skeletonBindings.find(generation);
        found != skeletonBindings.end() &&
        found->second.skeleton.get() == skeleton.get()) {
        return &found->second;
    }
    FxSkeletonBindings compiled;
    compiled.skeleton = skeleton;
    auto [position, inserted] = skeletonBindings.insert_or_assign(
        generation, std::move(compiled));
    static_cast<void>(inserted);
    return &position->second;
}

size_t WorldFxPresentationRuntime::namedJoint(
    const container::SharedPtr<const Skeleton>& skeleton,
    container::StringView boneName) {
    constexpr size_t missingJoint = std::numeric_limits<size_t>::max();
    FxSkeletonBindings* bindings = bindingsFor(skeleton);
    if (!bindings || boneName.empty()) return missingJoint;
    const auto found = std::find_if(
        bindings->namedJoints.begin(), bindings->namedJoints.end(),
        [boneName](const FxSkeletonBindings::NamedJoint& value) {
            return container::asciiEqualIgnoreCase(value.boneName, boneName);
        });
    if (found != bindings->namedJoints.end()) return found->jointIndex;
    const std::optional<size_t> joint =
        skeleton->findJointIndexInsensitive(boneName);
    bindings->namedJoints.push_back({
        .boneName = container::String{boneName},
        .jointIndex = joint.value_or(missingJoint),
    });
    return bindings->namedJoints.back().jointIndex;
}

container::Span<const size_t> WorldFxPresentationRuntime::numberedJoints(
    const container::SharedPtr<const Skeleton>& skeleton,
    container::StringView bonePrefix) {
    FxSkeletonBindings* bindings = bindingsFor(skeleton);
    if (!bindings || bonePrefix.empty()) return {};
    const auto found = std::find_if(
        bindings->numberedJoints.begin(), bindings->numberedJoints.end(),
        [bonePrefix](const FxSkeletonBindings::NumberedJoints& value) {
            return container::asciiEqualIgnoreCase(
                value.bonePrefix, bonePrefix);
        });
    if (found != bindings->numberedJoints.end()) {
        return found->jointIndices;
    }
    FxSkeletonBindings::NumberedJoints compiled;
    compiled.bonePrefix = container::String{bonePrefix};
    container::Span<const size_t> joints =
        skeleton->numberedJointIndicesInsensitive(bonePrefix);
    if (joints.size() > fx::kMaximumNumberedW3dBonePoints) {
        joints = joints.first(fx::kMaximumNumberedW3dBonePoints);
    }
    compiled.jointIndices.assign(joints.begin(), joints.end());
    bindings->numberedJoints.push_back(std::move(compiled));
    return bindings->numberedJoints.back().jointIndices;
}

} // namespace engine::render
