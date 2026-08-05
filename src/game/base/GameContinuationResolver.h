#pragma once

#include "game/base/GameSettings.h"

namespace engine {

enum class GameContinuationAction : uint8_t {
    Next,
    Retry,
};

enum class GameContinuationStatus : uint8_t {
    Success,
    NoNextMission,
    InvalidSequence,
    MissingCampaign,
    MissingMission,
};

struct GameContinuationResult {
    GameContinuationStatus status = GameContinuationStatus::InvalidSequence;
    GameStartInfo startInfo;
    container::String error;

    explicit operator bool() const noexcept {
        return status == GameContinuationStatus::Success;
    }
};

class GameContinuationResolver {
public:
    [[nodiscard]] static GameContinuationResult resolve(
        const GameStartInfo& current,
        GameContinuationAction action);
};

} // namespace engine
