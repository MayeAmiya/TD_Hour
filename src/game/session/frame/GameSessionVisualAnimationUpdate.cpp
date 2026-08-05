#include "game/session/frame/GameSessionVisualAnimationUpdate.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/render/VisualAnimationState.h"

#include <algorithm>
#include <cmath>

namespace engine::session_animation {

void updateConfirmedClocks(
    ecs::registry& registry,
    float fixedDeltaSeconds,
    uint64_t confirmedTick) noexcept {
    if (!std::isfinite(fixedDeltaSeconds) || fixedDeltaSeconds <= 0.0f) return;

    const auto visuals = ecs::view<RenderModelComponent>(registry);
    for (const ecs::entity entity : visuals) {
        RenderModelComponent& visual =
            visuals.template get<RenderModelComponent>(entity);
        if (const ObjectEconomyComponent* economy =
                ecs::try_get<ObjectEconomyComponent>(registry, entity);
            economy && economy->plan) {
            uint32_t selectedOrder = 0;
            uint64_t selectedSupply = 0;
            bool selected = false;
            const size_t warehouseCount = std::min(
                economy->supplyWarehouseDocks.size(),
                economy->plan->supplyWarehouseDocks.size());
            for (size_t index = 0; index < warehouseCount; ++index) {
                const auto& rule = economy->plan->supplyWarehouseDocks[index];
                if (!selected || rule.authoredOrder >= selectedOrder) {
                    selected = true;
                    selectedOrder = rule.authoredOrder;
                    selectedSupply = economy->supplyWarehouseDocks[index].boxesStored;
                }
            }
            const size_t truckCount = std::min(
                economy->supplyTrucks.size(), economy->plan->supplyTrucks.size());
            for (size_t index = 0; index < truckCount; ++index) {
                const auto& rule = economy->plan->supplyTrucks[index];
                if (!selected || rule.authoredOrder >= selectedOrder) {
                    selected = true;
                    selectedOrder = rule.authoredOrder;
                    selectedSupply = economy->supplyTrucks[index].boxes;
                }
            }
            visual.modelConditionFlags.set(
                game::ModelConditionFlag::Carrying,
                selected && selectedSupply != 0);
        }
        const ThingTemplateComponent* source =
            ecs::try_get<ThingTemplateComponent>(registry, entity);
        const game::ThingTemplate* templateData = source && source->archetype
            ? &source->archetype->templateData
            : nullptr;
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, entity);
        const ObjectIdentityComponent* identity =
            ecs::try_get<ObjectIdentityComponent>(registry, entity);
        const VisualAnimationObjectState objectState{
            .objectId = identity && identity->id ? identity->id.value : 0,
            .disabledReasons = objectDisabledMask(
                registry, entity, confirmedTick),
            .producedAtHelipad = kinds && game::objectHasKind(
                kinds->mask, game::ObjectKindOf::ProducedAtHelipad),
        };
        if (updateVisualAnimationState(
                visual, fixedDeltaSeconds, confirmedTick, templateData,
                objectState)) {
            markObjectDirty(
                registry, entity, ObjectDirtyDomain::RenderExtraction);
        }
    }
}

} // namespace engine::session_animation
