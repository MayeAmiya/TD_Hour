#pragma once

#include "game/session/core/GameSessionDomainComposition.h"
#include "game/session/integration/GameRenderExtractionCache.h"

namespace engine::detail {

// Opaque storage behind the public GameSession header. The Session remains
// the composition root, while app callers no longer inherit every concrete
// state/transaction/render-cache dependency through a by-value member.
struct GameSessionStorage final {
    GameSessionDomainComposition domain;
    GameRenderExtractionCache renderExtraction;
};

} // namespace engine::detail
