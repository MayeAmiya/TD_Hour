#include "game/session/query/SessionPlayerQuery.h"

#include "game/player/PlayerRegistry.h"
#include "game/session/core/GameSession.h"

namespace engine::session_query {

std::optional<LocalPlayerSnapshot>
SessionPlayerQueryPort::localPlayer() const noexcept {
    const PlayerState* player = m_players->localPlayer();
    if (!player) return std::nullopt;
    return LocalPlayerSnapshot{
        .id = player->id,
        .commandPlayer = player->isCommandPlayer(),
    };
}

LockstepSessionIdentitySnapshot
SessionPlayerQueryPort::lockstepIdentity() const noexcept {
    LockstepSessionIdentitySnapshot result{
        .simulationContentFingerprint =
            m_players->simulationContentFingerprint(),
        .resolvedSetupSimulationDigest =
            m_players->resolvedSetupSimulationDigest(),
    };
    if (const PlayerState* player = m_players->localPlayer()) {
        result.localPlayer = {
            .id = player->id,
            .commandPlayer = player->isCommandPlayer(),
        };
    }
    return result;
}

std::optional<LocalPlayerSnapshot> localPlayer(
    const GameSession& session) noexcept {
    return session.playerQuery().localPlayer();
}

LockstepSessionIdentitySnapshot lockstepIdentity(
    const GameSession& session) noexcept {
    return session.playerQuery().lockstepIdentity();
}

} // namespace engine::session_query
