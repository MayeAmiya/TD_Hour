#include "app/input/CommandMapDomainDispatch.h"

#include "app/input/CommandModeInputState.h"
#include "app/runtime/GameLogicIntent.h"
#include "app/runtime/GameUiProjection.h"

namespace app::input::command_map_domain {

bool dispatchGameplay(
    CommandMapGameplayDispatchContext context,
    const CommandMapBinding& binding) {
    const CommandMapAction action = binding.action;
    const bool pressed = binding.transition == CommandMapTransition::Down;
    if (context.commandModes.applyHeldAction(action, pressed)) return true;
    if (!pressed) return false;
    if (action == CommandMapAction::Stop) {
        // Stop is an immediate replacement command in ZH, never a waypoint
        // node. While Shift/waypoint mode is held, consume the shortcut but
        // do not activate the Stop button or clear the existing route.
        if (context.commandModes.queueOrders()) return true;
        for (const auto& token : context.projection.commandUi.actionTokens) {
            if (token.descriptor.kind != game::CommandButtonKind::Stop ||
                !token.isValid()) {
                continue;
            }
            static_cast<void>(context.logicIntents.post(
                runtime::ActivateCommandBarSlotIntent{.token = token},
                context.projection.sessionRevision));
            return true;
        }
        return true;
    }
    if (action == CommandMapAction::Scatter) {
        static_cast<void>(context.logicIntents.post(
            runtime::SubmitScatterIntent{},
            context.projection.sessionRevision));
        return true;
    }
    if (action == CommandMapAction::CreateFormation) {
        static_cast<void>(context.logicIntents.post(
            runtime::SubmitCreateFormationIntent{},
            context.projection.sessionRevision));
        return true;
    }
    if (action == CommandMapAction::PlaceBeacon ||
        action == CommandMapAction::DeleteBeacon) {
        // Network gameplay is intentionally absent; retail consumes these
        // bindings as no-ops outside an active multiplayer match.
        return true;
    }
    return false;
}

} // namespace app::input::command_map_domain
