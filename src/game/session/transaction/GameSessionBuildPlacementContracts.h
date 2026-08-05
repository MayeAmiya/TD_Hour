#pragma once

#include "core/container/container_types.h"
#include "game/render/TerrainBibPresentation.h"
#include "game/render/LocalPlacementPresentationState.h"

namespace engine {

enum class GameSessionBuildPlacementRejection : uint8_t {
    None = 0,
    VisibilityUnavailable,
    Shroud,
    RestrictedTerrain,
    NotFlatEnough,
    HiddenObject,
    ObjectsInTheWay,
    TooCloseToSupplies,
    BuilderUnavailable,
    NavigationUnavailable,
    InvalidNavigationLayer,
    InvalidNavigationCell,
    UnsupportedLocomotor,
    NoClearPath,
};

// Detached value result shared by placement presentation, player commands
// and script scenario planning. Keeping it outside ScriptInterface prevents
// narrow transaction ports from depending on the whole Session facade.
struct GameSessionBuildPlacementLegalityEvaluation final {
    selection::LocalPlacementLegality legality =
        selection::LocalPlacementLegality::Unchecked;
    GameSessionBuildPlacementRejection rejection =
        GameSessionBuildPlacementRejection::None;
    container::Vector<render::TerrainBibFootprintInput> obstructions;
    bool evaluated = false;
};

} // namespace engine
