#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"
#include "W3dTypes.h"

#include <cstdint>

namespace data::w3d {

struct FixedVector3 final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
};

struct FixedQuaternion final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    math::q32_32 w{int32_t{1}};
};

struct FixedRigidTransform final {
    FixedQuaternion rotation;
    FixedVector3 translation;
};

struct FixedPoseDiagnostics final {
    uint32_t invalidParentCount = 0;
    uint32_t cycleNodeCount = 0;
    uint32_t invalidChannelCount = 0;
    uint32_t repairedQuaternionCount = 0;
};

// Evaluates one integer pristine animation frame into model-space hierarchy
// transforms. Null animation means rest pose. Scale is applied only to final
// model-space translations, matching W3DModelDraw's root scale transform.
[[nodiscard]] container::Vector<FixedRigidTransform> evaluateFixedPristinePose(
    const ParsedHierarchy& hierarchy,
    const ParsedAnimation* animation,
    uint32_t frame,
    math::q32_32 assetScale,
    FixedPoseDiagnostics* diagnostics = nullptr);

} // namespace data::w3d
