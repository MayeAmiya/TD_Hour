#pragma once

#include "core/container/container_types.h"
#include "presentation/render/RenderSceneSnapshot.h"

#include <cstdint>

namespace engine {
class GameSessionContentStartState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

struct GameRenderEntityExtractionSource final {
    const GameSessionContentStartState& content;
    const GameSessionScriptPresentationState& presentation;
    const GameSessionObjectEventState& objectEvents;
    const GameSessionWorldState& world;
    const void* cacheOwner = nullptr;
    container::Vector<render::TacticalRadarEventRenderSnapshot>&
        gameplayRadarHistory;
    uint64_t& gameplayRadarEpoch;
};

} // namespace engine
