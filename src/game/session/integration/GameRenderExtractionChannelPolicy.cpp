#include "GameRenderExtractionChannelPolicy.h"

#include "GameRenderExtractionDetail.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/economy/ObjectEconomy.h"

#include <algorithm>
#include <limits>

namespace engine::render_extraction_detail {

ObjectRenderChannelPolicy resolveObjectRenderChannelPolicy(
    const ecs::registry& registry,
    ecs::entity entity,
    const game::ThingTemplate* templateData,
    const RenderModelComponent& visual) {
    ObjectRenderChannelPolicy result{
        .presentationConditions = visual.modelConditionFlags,
        .channelCount = templateData &&
                !templateData->drawVisualChannels.empty()
            ? templateData->drawVisualChannels.size() : 1u,
    };
    if (!templateData) return result;

    const bool hasSupplyDraw = std::any_of(
        templateData->drawVisualChannels.begin(),
        templateData->drawVisualChannels.end(),
        [](const game::ModelDrawVisualChannel& channel) {
            return equalsInsensitive(
                channel.sourceModuleClass, "W3DSupplyDraw");
        });
    if (hasSupplyDraw) {
        const ObjectEconomyComponent* economy =
            ecs::try_get<ObjectEconomyComponent>(registry, entity);
        if (economy && economy->plan) {
            uint32_t selectedOrder = 0;
            bool selected = false;
            const size_t warehouseCount = std::min(
                economy->supplyWarehouseDocks.size(),
                economy->plan->supplyWarehouseDocks.size());
            for (size_t index = 0; index < warehouseCount; ++index) {
                const game::ObjectSupplyWarehouseDockRule& rule =
                    economy->plan->supplyWarehouseDocks[index];
                if (!selected || rule.authoredOrder >= selectedOrder) {
                    selected = true;
                    selectedOrder = rule.authoredOrder;
                    result.supplyCurrent =
                        economy->supplyWarehouseDocks[index].boxesStored;
                    result.supplyMaximum = rule.startingBoxes;
                }
            }
            const size_t truckCount = std::min(
                economy->supplyTrucks.size(),
                economy->plan->supplyTrucks.size());
            for (size_t index = 0; index < truckCount; ++index) {
                const game::ObjectSupplyTruckRule& rule =
                    economy->plan->supplyTrucks[index];
                if (!selected || rule.authoredOrder >= selectedOrder) {
                    selected = true;
                    selectedOrder = rule.authoredOrder;
                    result.supplyCurrent = economy->supplyTrucks[index].boxes;
                    result.supplyMaximum = rule.maxBoxes;
                }
            }
        }
    }

    for (size_t index = 0;
         index < templateData->drawVisualChannels.size(); ++index) {
        const game::ModelDrawVisualChannel& channel =
            templateData->drawVisualChannels[index];
        const RenderModelChannelState* state = index < visual.channels.size()
            ? &visual.channels[index] : nullptr;
        container::StringView activeModel = channel.defaultModel;
        if (state && state->waitingSourceVisualRuleIndex <
                channel.conditionVisuals.size()) {
            activeModel = channel.conditionVisuals[
                state->waitingSourceVisualRuleIndex].model;
        } else if (state && state->activeTransitionRuleIndex <
                       channel.transitions.size()) {
            activeModel = channel.transitions[
                state->activeTransitionRuleIndex].model;
        } else {
            size_t selected = state
                ? state->resolvedVisualRuleIndex
                : std::numeric_limits<size_t>::max();
            if (selected >= channel.conditionVisuals.size()) {
                selected = game::selectModelConditionVisualRuleIndex(
                    channel, result.presentationConditions);
            }
            if (selected < channel.conditionVisuals.size()) {
                activeModel = channel.conditionVisuals[selected].model;
            }
        }
        if (!activeModel.empty() && !channel.receivesDynamicLights) {
            result.receivesDynamicLights = false;
            break;
        }
    }
    return result;
}

} // namespace engine::render_extraction_detail
