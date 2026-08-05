#pragma once

#include "core/container/container_types.h"
#include "data/w3d/W3dFixedPose.h"
#include "game/object/simulation/structure/ObjectAirfield.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace game {
class W3dPristineBoneCatalog;
namespace terrain {
class TerrainLogic;
}
} // namespace game

namespace engine {

inline constexpr size_t kMaximumChinookCombatDropBones = 32;

// Immutable state-entry inputs. The future gameplay dispatcher supplies these
// from its confirmed object snapshot; this resolver never reads or mutates an
// AI state machine and never starts the drop by itself.
struct ObjectChinookCombatDropBeginResolveInput final {
    ObjectId object = INVALID_OBJECT_ID;
    size_t moduleIndex = 0;
    container::StringView archetypeName;
    size_t visualRuleIndex = 0;
    uint32_t numRopes = 0;
    LogicFixedVec3 objectPosition{};
    math::q32_32 objectYawRadians{};
    uint64_t confirmedTick = 0;
};

// Fixed-size detached pristine snapshot used by the pure transform half of
// the resolver. An empty ordinal terminates that prefix, even if later slots
// are populated, matching getPristineBonePositions(prefix, 1, ...).
struct ObjectChinookCombatDropPristineBones final {
    container::Array<
        std::optional<data::w3d::FixedRigidTransform>,
        kMaximumChinookCombatDropBones> ropeStarts{};
    container::Array<
        std::optional<data::w3d::FixedRigidTransform>,
        kMaximumChinookCombatDropBones> dropStarts{};
};

class ObjectChinookCombatDropSurfaceResolver {
public:
    virtual ~ObjectChinookCombatDropSurfaceResolver() = default;
    [[nodiscard]] virtual math::q32_32 surfaceHeight(
        const LogicFixedVec3& ropeStart) const noexcept = 0;
};

[[nodiscard]] std::optional<ObjectChinookCombatDropBeginRequest>
resolveObjectChinookCombatDropBeginFromPristineBones(
    const ObjectChinookCombatDropBeginResolveInput& input,
    const ObjectChinookCombatDropPristineBones& bones,
    const ObjectChinookCombatDropSurfaceResolver& surfaces);

[[nodiscard]] std::optional<ObjectChinookCombatDropBeginRequest>
resolveObjectChinookCombatDropBeginFromPristineBones(
    const ObjectChinookCombatDropBeginResolveInput& input,
    const ObjectChinookCombatDropPristineBones& bones,
    const game::terrain::TerrainLogic& terrain);

// Production-facing wrapper: resolves RopeStart01..32 and RopeEnd01..32 from
// the session-frozen logic catalog, then delegates to the value-only helper.
[[nodiscard]] std::optional<ObjectChinookCombatDropBeginRequest>
resolveObjectChinookCombatDropBegin(
    const ObjectChinookCombatDropBeginResolveInput& input,
    const game::W3dPristineBoneCatalog& catalog,
    const game::terrain::TerrainLogic& terrain);

} // namespace engine
