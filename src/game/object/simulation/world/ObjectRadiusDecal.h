#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

#include "game/object/plan/world/ObjectRadiusDecalPlanTypes.h"
namespace engine {

class ObjectLifecycle;

enum class ObjectRadiusDecalEventKind : uint8_t {
    Begin,
    Update,
    End,
};

enum class ObjectRadiusDecalEventSource : uint8_t {
    RadiusDecalUpdate,
    NeutronMissileUpdate,
    SpectreAttackArea,
    SpectreTargetingReticle,
};

struct ObjectRadiusDecalEvent final {
    ObjectRadiusDecalEventKind kind = ObjectRadiusDecalEventKind::Begin;
    ObjectRadiusDecalEventSource source =
        ObjectRadiusDecalEventSource::RadiusDecalUpdate;
    ObjectId object = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    uint32_t authoredOrder = 0;
    container::String texture;
    LogicFixedVec3 position;
    math::q32_32 radius{};
    uint32_t shadowTypeMask = 0x20u;
    math::q32_32 minimumOpacity{int32_t{1}};
    math::q32_32 maximumOpacity{int32_t{1}};
    uint64_t opacityThrobTicks = 30;
    container::Array<uint8_t, 4> color{0, 0, 0, 0};
    bool usesPlayerColor = true;
    bool onlyVisibleToOwningPlayer = true;
    uint64_t confirmedTick = 0;
};

struct ObjectRadiusDecalRequest final {
    ObjectId object = INVALID_OBJECT_ID;
    container::String texture;
    LogicFixedVec3 position;
    math::q32_32 radius{};
    uint32_t shadowTypeMask = 0x20u;
    math::q32_32 minimumOpacity{int32_t{1}};
    math::q32_32 maximumOpacity{int32_t{1}};
    uint64_t opacityThrobTicks = 30;
    container::Array<uint8_t, 4> color{0, 0, 0, 0};
    bool usesPlayerColor = true;
    bool onlyVisibleToOwningPlayer = true;
    bool killWhenNoLongerAttacking = false;
    uint64_t confirmedTick = 0;
};

struct ObjectRadiusDecalRuntime final {
    bool active = false;
    bool killWhenNoLongerAttacking = false;
    container::String texture;
    LogicFixedVec3 position;
    math::q32_32 radius{};
    uint32_t shadowTypeMask = 0x20u;
    math::q32_32 minimumOpacity{int32_t{1}};
    math::q32_32 maximumOpacity{int32_t{1}};
    uint64_t opacityThrobTicks = 30;
    container::Array<uint8_t, 4> color{0, 0, 0, 0};
    bool usesPlayerColor = true;
    bool onlyVisibleToOwningPlayer = true;
};

struct ObjectRadiusDecalComponent final {
    container::SharedPtr<const game::ObjectRadiusDecalPlan> plan;
    container::Vector<ObjectRadiusDecalRuntime> instances;
};

class ObjectRadiusDecalSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    [[nodiscard]] bool createRadiusDecal(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectRadiusDecalRequest& request,
        container::Vector<ObjectRadiusDecalEvent>& outEvents) const;
    [[nodiscard]] bool killRadiusDecal(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick,
        container::Vector<ObjectRadiusDecalEvent>& outEvents) const;

    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                uint64_t confirmedTick,
                container::Vector<ObjectRadiusDecalEvent>& outEvents) const;
};

} // namespace engine
