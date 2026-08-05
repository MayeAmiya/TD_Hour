#include "game/session/frame/GameSessionClientTerrainPresentationUpdater.h"

#include "game/session/frame/GameSessionTerrainVisibility.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine {

void GameSessionClientTerrainPresentationUpdater::update(float deltaSeconds) {
    const float boundedDelta =
        std::isfinite(deltaSeconds) ? std::max(0.0f, deltaSeconds) : 0.0f;
    session_terrain::refreshFallingTreeFog(
        m_world.m_clientTerrainObjects, m_world.m_mapVisibility,
        m_content.m_players);
    m_world.m_clientTerrainObjects.advanceTreeLifecycles(
        boundedDelta > 0.0f ? 1.0f : 0.0f);
    for (ClientTerrainFxEvent& event :
         m_world.m_clientTerrainObjects.takeFxEvents()) {
        if (event.fxListName.empty()) continue;
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .fxListName = std::move(event.fxListName),
            .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
            .primary = {.position = event.position},
        }));
    }
}

} // namespace engine
