#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#include "game/object/plan/combat/ObjectNeutronMissileSlowDeathPlanTypes.h"

namespace game::terrain { class TerrainLogic; }

namespace engine {
class ObjectLifecycle;
struct ObjectDamageRequest;
struct ObjectSimulationRules;

// One completion bit per authored blast.  A bit that falls outside the mask
// truncates to zero, which reads as "not completed" and re-fires that blast's
// damage and topple requests on every confirmed tick forever, so the width must
// cover the authored blast array.
using ObjectNeutronMissileBlastMask = uint16_t;
static_assert(
    game::kObjectNeutronMissileBlastCount <= static_cast<size_t>(
        std::numeric_limits<ObjectNeutronMissileBlastMask>::digits),
    "ObjectNeutronMissileBlastMask needs one bit per authored neutron blast");

struct ObjectNeutronMissileSlowDeathRuntime final {
    uint64_t activationTick = 0;
    uint64_t lastUpdateTick = UINT64_MAX;
    ObjectNeutronMissileBlastMask completedBlastMask = 0;
    ObjectNeutronMissileBlastMask completedScorchMask = 0;
    ObjectNeutronMissileBlastMask snapshottedBlastTargetsMask = 0;
    container::Array<container::Vector<ObjectId>,
                     game::kObjectNeutronMissileBlastCount> blastTargets;
    container::Array<size_t, game::kObjectNeutronMissileBlastCount>
        nextBlastTarget{};
    bool activated = false;
    bool scorchPlaced = false;
};

struct ObjectNeutronMissileSlowDeathComponent final {
    container::SharedPtr<const game::ObjectNeutronMissileSlowDeathPlan> plan;
    container::Vector<ObjectNeutronMissileSlowDeathRuntime> instances;
};

enum class ObjectNeutronMissilePresentationEventKind : uint8_t {
    InitialFx,
    ScorchMark,
};

struct ObjectNeutronMissilePresentationEvent final {
    ObjectNeutronMissilePresentationEventKind kind =
        ObjectNeutronMissilePresentationEventKind::InitialFx;
    ObjectId source = INVALID_OBJECT_ID;
    container::String fxList;
    LogicFixedVec3 position{};
    math::q32_32 size{};
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

class ObjectNeutronMissileSlowDeathSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;
    [[nodiscard]] bool update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const game::terrain::TerrainLogic& terrain,
        const ObjectSimulationRules& rules, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& outDamage,
        container::Vector<ObjectNeutronMissilePresentationEvent>& outEvents) const;
};
}
