#include "app/input/CommandMapDomainDispatch.h"

#include "app/ui/ingame/InGameGuiSubsystem.h"

namespace app::input::command_map_domain {

bool dispatchUi(
    CommandMapUiDispatchContext context,
    const CommandMapBinding& binding) {
    if (binding.transition != CommandMapTransition::Down) return false;
    const CommandMapAction action = binding.action;
    if (action == CommandMapAction::ToggleControlBar) {
        context.inGameGui.layer().toggleControlBarCompact();
        return true;
    }
    if (action == CommandMapAction::Options) {
        static_cast<void>(context.inGameGui.openOverlayFromInput(
            gui::GameWndOverlay::Options));
        return true;
    }
    if (action == CommandMapAction::Diplomacy) {
        static_cast<void>(context.inGameGui.openOverlayFromInput(
            gui::GameWndOverlay::Diplomacy));
        return true;
    }
    if (action == CommandMapAction::ChatAllies ||
        action == CommandMapAction::ChatEveryone) {
        static_cast<void>(context.inGameGui.openOverlayFromInput(
            gui::GameWndOverlay::Chat));
        return true;
    }
    return false;
}

} // namespace app::input::command_map_domain
