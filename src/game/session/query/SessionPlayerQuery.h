#pragma once

#include "game/player/PlayerTypes.h"

#include <cstdint>
#include <optional>

namespace engine {
class GameSession;
class PlayerRegistry;
}

namespace engine::session_query {

// Pointer-free local-player identity copied from the confirmed session.
// Callers cannot retain or mutate PlayerState through this boundary.
struct LocalPlayerSnapshot final {
    PlayerId id = INVALID_PLAYER_ID;
    bool commandPlayer = false;

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(id); }
};

struct LockstepSessionIdentitySnapshot final {
    LocalPlayerSnapshot localPlayer;
    uint64_t simulationContentFingerprint = 0;
    uint64_t resolvedSetupSimulationDigest = 0;
};

// Pointer-free player/session identity capability.  The port keeps the
// PlayerRegistry owner private and only copies the stable values needed by
// app/network callers; it cannot be used to enumerate or mutate players.
class SessionPlayerQueryPort final {
public:
    explicit SessionPlayerQueryPort(
        const PlayerRegistry& players) noexcept
        : m_players(&players) {}

    [[nodiscard]] std::optional<LocalPlayerSnapshot>
    localPlayer() const noexcept;
    [[nodiscard]] LockstepSessionIdentitySnapshot
    lockstepIdentity() const noexcept;

private:
    const PlayerRegistry* m_players = nullptr;
};

[[nodiscard]] std::optional<LocalPlayerSnapshot> localPlayer(
    const GameSession& session) noexcept;

[[nodiscard]] LockstepSessionIdentitySnapshot lockstepIdentity(
    const GameSession& session) noexcept;

} // namespace engine::session_query
