#pragma once

#include "core/container/container_types.h"

#include "game/base/DamageTypes.h"
#include "game/object/contracts/ObjectFixedGeometryTypes.h"

#include <cstddef>
#include <cstdint>

namespace game {

struct ThingTemplate;

inline constexpr size_t kTransitionDamageStateCount = 4;
inline constexpr size_t kTransitionDamageMaximumSlots = 12;

enum class ObjectTransitionDamageFxPayloadKind : uint8_t {
    FxList,
    ObjectCreationList,
    ParticleSystem,
};

enum class ObjectTransitionDamageFxLocationKind : uint8_t {
    LocalCoordinate,
    Bone,
};

// Immutable replacement for TransitionDamageFX's FXLocInfo. Coordinates are
// normalized to Q32.32 at content compilation; bone names remain renderer
// content and never become an ECS or W3D handle.
struct ObjectTransitionDamageFxLocation final {
    ObjectTransitionDamageFxLocationKind kind =
        ObjectTransitionDamageFxLocationKind::LocalCoordinate;
    engine::LogicFixedVec3 localPosition{};
    container::String boneName;
    bool randomBone = false;
};

struct ObjectTransitionDamageFxEntry final {
    ObjectTransitionDamageFxPayloadKind kind =
        ObjectTransitionDamageFxPayloadKind::FxList;
    ObjectTransitionDamageFxLocation location;
    container::String resource;
    uint8_t slot = 0;
};

struct ObjectTransitionDamageFxRule final {
    container::Array<container::Vector<ObjectTransitionDamageFxEntry>,
                     kTransitionDamageStateCount>
        entries;
    uint64_t fxDamageTypes = 0;
    uint64_t oclDamageTypes = 0;
    uint64_t particleDamageTypes = 0;
    uint32_t authoredOrder = 0;
};

struct ObjectTransitionDamageFxPlan final {
    container::Vector<ObjectTransitionDamageFxRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectTransitionDamageFxPlan>
compileObjectTransitionDamageFxPlan(const ThingTemplate& templateData);

} // namespace game
