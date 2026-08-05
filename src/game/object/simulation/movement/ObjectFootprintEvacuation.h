#pragma once

#include "core/ecs/ObjectId.h"
#include "game/object/contracts/ObjectFixedGeometryTypes.h"
#include "math/fixed/q32_32.h"

#include <optional>

namespace engine {

struct ObjectGeometryComponent;

// Returns a deterministic point outside obstacleGeometry when the subject
// overlaps its footprint.  Callers retain ownership of terrain-layer height,
// path admission and the resulting movement order.
[[nodiscard]] std::optional<LogicFixedVec3>
objectFootprintEvacuationTarget(
    const LogicFixedVec3& obstaclePosition, math::q32_32 obstacleYaw,
    const ObjectGeometryComponent& obstacleGeometry,
    const LogicFixedVec3& subjectPosition,
    const ObjectGeometryComponent& subjectGeometry,
    ObjectId subject, math::q32_32 clearance) noexcept;

} // namespace engine
