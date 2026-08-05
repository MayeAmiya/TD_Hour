#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/world/model/Skeleton.h"

#include <cstddef>
#include <limits>

namespace engine::render {

struct FxSkeletonBindings final {
    struct NamedJoint final {
        container::String boneName;
        size_t jointIndex = std::numeric_limits<size_t>::max();
    };
    struct NumberedJoints final {
        container::String bonePrefix;
        container::Vector<size_t> jointIndices;
    };

    container::SharedPtr<const Skeleton> skeleton;
    container::Vector<NamedJoint> namedJoints;
    container::Vector<NumberedJoints> numberedJoints;
};

} // namespace engine::render
