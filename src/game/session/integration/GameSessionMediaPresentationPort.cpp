#include "game/session/integration/GameSessionMediaPresentationPort.h"

#include "game/content/runtime/GameContentSnapshot.h"
#include "game/session/state/GameSessionDomainState.h"

namespace engine {

GamePresentationContentSnapshot
GameSessionMediaPresentationPort::content() const noexcept {
    const GameContentSnapshot& content = m_content->m_contentSnapshot;
    return {
        .audioLayers = content.audioContentLayerSnapshot(),
        .particleSystems = content.particleSystemCatalogSnapshot(),
        .fxLists = content.fxListCatalogSnapshot(),
    };
}

} // namespace engine
