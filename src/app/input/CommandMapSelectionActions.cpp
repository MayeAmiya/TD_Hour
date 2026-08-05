#include "app/input/CommandMapDomainDispatch.h"

#include "app/host/PresentationCoordinator.h"
#include "app/input/CommandModeInputState.h"
#include "app/runtime/GameLogicIntent.h"
#include "app/runtime/GameUiProjection.h"
#include "game/session/query/LocalSelectionPolicy.h"

#include <SDL3/SDL.h>

#include <utility>

namespace app::input::command_map_domain {
namespace {

[[nodiscard]] uint64_t eventTimestampMilliseconds(
    const SDL_Event& event) noexcept {
    return event.common.timestamp / 1'000'000ull;
}

} // namespace

bool dispatchSelection(
    CommandMapSelectionDispatchContext context,
    const SDL_Event& event,
    const CommandMapBinding& binding) {
    if (binding.transition != CommandMapTransition::Down) return false;
    const CommandMapAction action = binding.action;
    const uint8_t ordinal = binding.ordinal;
    if (action == CommandMapAction::SelectTeam && ordinal <= 9u) {
        const bool focusCamera = context.commandModes.recallFocus(
            ordinal, eventTimestampMilliseconds(event),
            context.projection.sessionRevision);
        static_cast<void>(context.logicIntents.post(
            runtime::ApplyLocalControlGroupIntent{.request = {
                .index = ordinal,
                .operation = engine::selection::
                    LocalControlGroupOperation::Recall,
                .focusCamera = focusCamera}},
            context.projection.sessionRevision));
        return true;
    }
    if (action == CommandMapAction::ViewTeam && ordinal <= 9u) {
        static_cast<void>(context.logicIntents.post(
            runtime::ApplyLocalControlGroupIntent{.request = {
                .index = ordinal,
                .operation = engine::selection::
                    LocalControlGroupOperation::View,
                .focusCamera = true}},
            context.projection.sessionRevision));
        return true;
    }
    if (action == CommandMapAction::SelectAll ||
        action == CommandMapAction::SelectMatchingUnits) {
        if (!context.viewport.valid()) return true;
        engine::selection::LocalSelectionGesture gesture;
        gesture.candidates = context.presentation.selectableObjectsInRectangle(
            0.0f, 0.0f, static_cast<float>(context.viewport.width),
            static_cast<float>(context.viewport.height));
        if (action == CommandMapAction::SelectAll) {
            gesture.kind =
                engine::selection::LocalSelectionGestureKind::Rectangle;
        } else {
            gesture.kind = engine::selection::
                LocalSelectionGestureKind::MatchingTypeVisible;
            gesture.anchor = context.projection.hasCommandBarSelection
                ? context.projection.selectedCommandBarObject
                : context.projection.hoveredObject;
            if (!gesture.anchor) return true;
        }
        static_cast<void>(context.logicIntents.post(
            runtime::ApplyLocalSelectionGestureIntent{
                .gesture = std::move(gesture)},
            context.projection.sessionRevision));
        return true;
    }
    if (action == CommandMapAction::SelectNextUnit ||
        action == CommandMapAction::SelectPreviousUnit ||
        action == CommandMapAction::SelectNextWorker ||
        action == CommandMapAction::SelectPreviousWorker ||
        action == CommandMapAction::SelectHero ||
        action == CommandMapAction::ViewCommandCenter) {
        engine::selection::LocalSelectionShortcut shortcut =
            engine::selection::LocalSelectionShortcut::NextUnit;
        switch (action) {
        case CommandMapAction::SelectPreviousUnit:
            shortcut = engine::selection::LocalSelectionShortcut::PreviousUnit;
            break;
        case CommandMapAction::SelectNextWorker:
            shortcut = engine::selection::LocalSelectionShortcut::NextWorker;
            break;
        case CommandMapAction::SelectPreviousWorker:
            shortcut = engine::selection::LocalSelectionShortcut::PreviousWorker;
            break;
        case CommandMapAction::SelectHero:
            shortcut = engine::selection::LocalSelectionShortcut::Hero;
            break;
        case CommandMapAction::ViewCommandCenter:
            shortcut = engine::selection::LocalSelectionShortcut::CommandCenter;
            break;
        default:
            break;
        }
        static_cast<void>(context.logicIntents.post(
            runtime::ApplyLocalSelectionShortcutIntent{.shortcut = shortcut},
            context.projection.sessionRevision));
        return true;
    }
    if ((action == CommandMapAction::CreateTeam ||
         action == CommandMapAction::AddTeam) && ordinal <= 9u) {
        static_cast<void>(context.logicIntents.post(
            runtime::ApplyLocalControlGroupIntent{.request = {
                .index = ordinal,
                .operation = action == CommandMapAction::CreateTeam
                    ? engine::selection::LocalControlGroupOperation::Save
                    : engine::selection::LocalControlGroupOperation::Append,
                .focusCamera = false}},
            context.projection.sessionRevision));
        return true;
    }
    return false;
}

} // namespace app::input::command_map_domain
