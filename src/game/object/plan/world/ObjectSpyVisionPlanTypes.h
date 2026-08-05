#pragma once

#include "core/container/container_types.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

// Immutable projection of SpyVisionUpdate plus its UpgradeMux predicate.
// An empty include list with spyOnAll=true reproduces the legacy default
// flipped KindOf mask (all object kinds).
struct ObjectSpyVisionRule final {
    uint32_t authoredOrder = 0;
    container::Vector<container::String> triggeredBy;
    container::Vector<container::String> conflictsWith;
    engine::UpgradeMask triggeredByMask;
    engine::UpgradeMask conflictsWithMask;
    ObjectKindOfMask includedKinds{};
    ObjectKindOfMask excludedKinds{};
    uint32_t selfPoweredDurationMilliseconds = 0;
    uint32_t selfPoweredIntervalMilliseconds = 0;
    bool requiresAllTriggers = false;
    bool needsUpgrade = false;
    bool selfPowered = false;
    bool spyOnAll = true;
    bool spyOnNone = false;
    bool upgradeMasksCompiled = false;
};

struct ObjectSpyVisionPlan final {
    container::Vector<ObjectSpyVisionRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectSpyVisionPlan>
compileObjectSpyVisionPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

} // namespace game

