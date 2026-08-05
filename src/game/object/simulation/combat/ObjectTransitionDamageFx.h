#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"

#include "game/base/DamageTypes.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "core/ecs/ObjectId.h"

#include <cstddef>
#include <cstdint>

#include "game/object/plan/combat/ObjectTransitionDamageFxPlanTypes.h"
namespace engine {

struct ObjectHealthEvent;
class GameContentSnapshot;
class ObjectLifecycle;

// Per-entity storage is a shared immutable recipe handle. Presentation owns
// actual particle emitter lifetimes; the authoritative ECS stores no renderer
// handle and therefore needs no mutable module object.
struct ObjectTransitionDamageFxComponent final {
    container::SharedPtr<const game::ObjectTransitionDamageFxPlan> plan;
};

struct ObjectTransitionDamageFxAnchor final {
    LogicFixedVec3 position{};
    math::q32_32 rollRadians{};
    math::q32_32 pitchRadians{};
    math::q32_32 yawRadians{};
};

enum class ObjectTransitionDamageFxEventKind : uint8_t {
    StopParticleGroup,
    FxList,
    ObjectCreationList,
    ParticleSystem,
};

// Detached command emitted at the exact BodyDamageState transition. OCL is
// still a gameplay command and is resolved by GameSession's existing OCL
// work stack; FX/particles cross the lossless presentation stream.
struct ObjectTransitionDamageFxEvent final {
    ObjectTransitionDamageFxEventKind kind =
        ObjectTransitionDamageFxEventKind::FxList;
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId damageSource = INVALID_OBJECT_ID;
    ObjectTransitionDamageFxAnchor primary;
    ObjectTransitionDamageFxAnchor secondary;
    uint32_t sourcePathfindLayer = 0;
    game::ObjectTransitionDamageFxLocation location;
    container::String resource;
    uint64_t attachmentGroup = 0;
    uint32_t authoredOrder = 0;
    uint8_t slot = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
    bool hasSecondary = false;
    // TransitionDamageFX requires the preferred Body damage source for OCL,
    // while BoneFXUpdate's legacy doOCL path is valid without one.
    bool oclRequiresDamageSource = true;
};

class ObjectTransitionDamageFxSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    void onHealthEvent(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectHealthEvent& event, const GameContentSnapshot* content,
        uint64_t sessionSeed, uint64_t& nextEmissionSequence,
        container::Vector<ObjectTransitionDamageFxEvent>& output) const;
};

} // namespace engine
