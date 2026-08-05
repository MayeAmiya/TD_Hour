#pragma once

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "presentation/render/RenderGameDataSettings.h"
#include "game/render/LocalPlacementPresentationState.h"
#include "presentation/render/RenderWorldHotSnapshot.h"

namespace engine {

// Pure detached equivalent of the temporary Drawable created by
// InGameUI::placeBuildAvailable(). It deliberately consumes only the frozen
// product recipe and local presentation values; no authoritative Object/ECS
// entity is created for the cursor model.
void appendLocalPlacementPreviewEntities(
    const selection::LocalPlacementPreviewSnapshot& placement,
    const game::ThingTemplate& product,
    render::RenderVector authorPlayerColor,
    const RenderGameDataSettings& settings,
    const RenderFeatureQualitySettings& featureQuality,
    container::Vector<render::RenderEntitySnapshot>& output,
    // Serialized terrain TimeOfDay: INVALID=0, MORNING=1..NIGHT=4. A map
    // override must win over the GameData default exactly as it does for
    // authoritative object ModelCondition production.
    uint8_t terrainTimeOfDay = 0);

} // namespace engine
