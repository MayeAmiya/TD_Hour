#include "game/session/presentation/LocalPlacementPresentationPort.h"

#include "game/session/state/GameSessionDomainState.h"

#include <utility>

namespace engine {

selection::LocalPlacementPreviewSnapshot
LocalPlacementPresentationPort::snapshot() const {
    return m_presentation.m_localPlacementPresentation.snapshot();
}

bool LocalPlacementPresentationPort::active() const noexcept {
    return m_presentation.m_localPlacementPresentation.active();
}

std::optional<render::RenderVector>
LocalPlacementPresentationPort::screenToTerrain(
    const GameCameraState& camera,
    selection::LocalPlacementViewport viewport,
    float screenX,
    float screenY) const noexcept {
    return selection::localPlacementScreenToTerrain(
        camera, m_content.m_terrain.map().heightfield(),
        viewport, screenX, screenY);
}

} // namespace engine
