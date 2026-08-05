#pragma once

#include "core/container/container_types.h"

#include "game/object/creation/ObjectCreationListRuntime.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
namespace game {

struct ThingTemplate;

struct ObjectFactionOclReference final {
    container::String faction;
    container::String objectCreationList;
};

struct ObjectOclUpdateParameters final {
    uint32_t authoredOrder = 0;
    container::String objectCreationList;
    container::Vector<ObjectFactionOclReference> factionObjectCreationLists;
    uint32_t minimumDelayMilliseconds = 0;
    uint32_t maximumDelayMilliseconds = 0;
    bool createAtEdge = false;
    bool factionTriggered = false;
};

struct ObjectOclUpdatePlan final {
    container::Vector<ObjectOclUpdateParameters> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectOclUpdatePlan>
compileObjectOclUpdatePlan(const ThingTemplate& templateData);

} // namespace game

namespace game::terrain {
class TerrainLogic;
}

