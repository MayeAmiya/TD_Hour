#pragma once

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/definition/ModelConditionState.h"

#include <cstddef>
#include <cstdint>

namespace game {
struct ThingTemplate;
}

namespace engine::render_extraction_detail {

struct ObjectRenderChannelPolicy final {
    game::ModelConditionMask presentationConditions;
    uint64_t supplyCurrent = 0;
    uint64_t supplyMaximum = 0;
    size_t channelCount = 1;
    bool receivesDynamicLights = true;
};

[[nodiscard]] ObjectRenderChannelPolicy resolveObjectRenderChannelPolicy(
    const ecs::registry& registry,
    ecs::entity entity,
    const game::ThingTemplate* templateData,
    const RenderModelComponent& visual);

} // namespace engine::render_extraction_detail
