#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "presentation/camera/GameCameraState.h"
#include "presentation/render/GroundDecalPresentationContracts.h"
#include "presentation/render/RenderSceneSnapshot.h"

#include <cstdint>

namespace engine {

class GameSessionContentStartState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;
struct GameRenderExtractionCache;

// App-facing, value-only extraction capability. It owns the dirty
// acknowledgement boundary but never exposes ECS storage or Session state.
class GameSessionRenderExtractionPort final {
public:
    GameSessionRenderExtractionPort(
        const GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents,
        GameRenderExtractionCache& cache) noexcept
        : m_content(&content), m_world(&world),
          m_presentation(&presentation), m_objectEvents(&objectEvents),
          m_cache(&cache) {}

    [[nodiscard]] render::RenderCameraSnapshot camera(
        const GameCameraState& camera) const noexcept;
    [[nodiscard]] render::WorldRenderSnapshot world(
        render::RenderCameraSnapshot camera,
        uint64_t simulationFrame,
        container::Span<const ObjectId> localSelection = {},
        ObjectId localHover = INVALID_OBJECT_ID,
        bool showPlayerWaypoints = false,
        bool includeVisualAssetDependencies = false);
    [[nodiscard]] render::GroundDecalPresentationBatch takeGroundDecals();

private:
    const GameSessionContentStartState* m_content = nullptr;
    GameSessionWorldState* m_world = nullptr;
    GameSessionScriptPresentationState* m_presentation = nullptr;
    GameSessionObjectEventState* m_objectEvents = nullptr;
    GameRenderExtractionCache* m_cache = nullptr;
};

} // namespace engine
