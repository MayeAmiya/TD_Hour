#pragma once

#include "app/input/CommandMapActionDispatcher.h"

namespace app::input::command_map_domain {

[[nodiscard]] bool dispatchView(
    CommandMapViewDispatchContext context,
    const SDL_Event& event,
    const CommandMapBinding& binding,
    bool inGame);
[[nodiscard]] bool dispatchUi(
    CommandMapUiDispatchContext context,
    const CommandMapBinding& binding);
[[nodiscard]] bool dispatchGameplay(
    CommandMapGameplayDispatchContext context,
    const CommandMapBinding& binding);
[[nodiscard]] bool dispatchSelection(
    CommandMapSelectionDispatchContext context,
    const SDL_Event& event,
    const CommandMapBinding& binding);

} // namespace app::input::command_map_domain
