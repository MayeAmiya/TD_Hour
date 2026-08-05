#pragma once

#include "core/container/container_types.h"

#include "game/selection/LocalSelectionState.h"
#include "game/command/GameCommand.h"

#include <optional>

namespace engine { class GameSessionCommandQueryPort; }

namespace engine::selection {

enum class BeaconTextComposeRejection : uint8_t {
    None,
    InvalidLocalPlayer,
    RequiresSingleSelection,
    SelectionNotControllable,
};

struct BeaconTextComposeResult final {
    std::optional<GameCommand> command;
    BeaconTextComposeRejection rejection = BeaconTextComposeRejection::None;
    container::String message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return command.has_value();
    }
};

// Typed UI ingress for the shipped beacon edit control.  Local language
// filtering happens before the resulting value enters replay/network data;
// the authoritative dispatcher still validates the explicit actor owner.
class BeaconTextCommandComposer final {
public:
    [[nodiscard]] static BeaconTextComposeResult compose(
        const LocalSelectionState& selection,
        const GameSessionCommandQueryPort& commands,
        PlayerId localPlayer, GameTick tick, uint32_t sequence,
        container::StringView caption);
};

} // namespace engine::selection
