#pragma once

#include "core/container/container_types.h"

#include "game/command/GameCommand.h"

#include <cstddef>
namespace engine {

class GameSession;

enum class CommandDispatchRejection : uint8_t {
    None,
    MalformedPayload,
    Unsupported,
    Rejected,
};

struct CommandDispatchResult final {
    bool accepted = false;
    bool producedOrder = false;
    size_t actorCount = 0;
    CommandDispatchRejection rejection = CommandDispatchRejection::None;
    container::String message;
};

class CommandDispatcher {
public:
    [[nodiscard]] CommandDispatchResult dispatch(GameSession& session, const GameCommand& command);
};

} // namespace engine
