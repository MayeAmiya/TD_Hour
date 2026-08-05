#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

// Only capabilities with an observable Object::onDestroy/onDelete effect are
// represented here. Purely derived providers (Power/Radar/Cost/SpyVision) are
// removed by their aggregate owner when PendingDestroy becomes visible.
enum class ObjectOnDeleteCapability : uint8_t {
    Builder,
    JetReservations,
    BattlePlan,
    PropagandaTower,
    Spawn,
    Containment,
    Count,
};

struct ObjectOnDeleteEntry final {
    ObjectOnDeleteCapability capability =
        ObjectOnDeleteCapability::Builder;
    uint32_t authoredOrder = 0;
};

struct ObjectOnDeletePlan final {
    container::Vector<ObjectOnDeleteEntry> entries;
};

[[nodiscard]] container::SharedPtr<const ObjectOnDeletePlan>
compileObjectOnDeletePlan(const ThingTemplate& templateData);

} // namespace game
