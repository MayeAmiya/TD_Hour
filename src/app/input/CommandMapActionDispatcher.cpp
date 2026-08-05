#include "app/input/CommandMapActionDispatcher.h"

#include "app/input/CommandMapDomainDispatch.h"

namespace app::input {

bool CommandMapActionDispatcher::dispatch(
    CommandMapDispatchContext context,
    const SDL_Event& event,
    const CommandMapBinding& binding,
    bool inGame) {
    if (command_map_domain::dispatchView(
            context.view, event, binding, inGame)) {
        return true;
    }
    if (!inGame) return false;
    if (command_map_domain::dispatchGameplay(
            context.gameplay, binding)) {
        return true;
    }
    if (binding.transition != CommandMapTransition::Down) return true;
    if (command_map_domain::dispatchUi(context.ui, binding)) return true;
    if (command_map_domain::dispatchSelection(
            context.selection, event, binding)) {
        return true;
    }
    return false;
}

} // namespace app::input
