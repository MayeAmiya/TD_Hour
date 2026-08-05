#include "app/input/WorldCommandInputDispatcher.h"

#include "app/host/PresentationCoordinator.h"
#include "app/input/CommandModeInputState.h"
#include "app/input/PendingWorldTargetCaptureState.h"
#include "app/runtime/GameLogicIntent.h"
#include "app/runtime/GameUiProjection.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <utility>

namespace app::input {

void WorldCommandInputDispatcher::submitContextual(
    WorldCommandInputContext context,
    float screenX, float screenY, uint64_t sessionRevision,
    bool forceAttack, bool forceMove,
    std::optional<engine::CommandPosition> targetPosition,
    bool attackMove,
    bool guardPosition,
    std::optional<engine::selection::LocalSelectionGesture>
        fallbackSelection) {
    if (!context.projection.hasSession || !context.projection.isInGame() ||
        !context.projection.gameplayInputEnabled ||
        sessionRevision != context.projection.sessionRevision) {
        return;
    }
    if (!context.viewport.valid()) return;
    const PresentedWorldInputTarget hit =
        context.presentation.worldInputTargetAt(
            screenX, screenY, false, false, forceAttack);
    const engine::ObjectId target = hit.object;
    if (!targetPosition && hit.position) {
        targetPosition = engine::CommandPosition{
            .x = math::q32_32{hit.position->x()},
            .y = math::q32_32{hit.position->y()},
            .z = math::q32_32{hit.position->z()},
            .valid = true,
        };
    }
    static_cast<void>(context.logicIntents.post(
        runtime::SubmitContextualWorldCommandIntent{
            .targetObject = target,
            .targetPosition = std::move(targetPosition),
            .screenX = screenX,
            .screenY = screenY,
            .viewportWidth = context.viewport.width,
            .viewportHeight = context.viewport.height,
            .presentationCamera = context.presentation.presentedCamera(),
            .queued = context.commandModes.queueOrders(),
            .forceAttack = forceAttack,
            .forceMove = forceMove,
            .attackMove = attackMove,
            .guardPosition = guardPosition,
            .fallbackSelection = std::move(fallbackSelection),
        },
        sessionRevision));
}

PendingWorldInputResult WorldCommandInputDispatcher::handlePending(
    WorldCommandInputContext context, const SDL_Event& event) {
    const engine::selection::PendingWorldCommandMode& mode =
        context.projection.pendingWorldCommand;
    if (!context.projection.hasSession || !mode.active()) {
        context.pendingTarget.cancel();
        return PendingWorldInputResult::Ignored;
    }

    const bool cancelKey = event.type == SDL_EVENT_KEY_DOWN &&
        event.key.scancode == SDL_SCANCODE_ESCAPE;
    const bool cancelButton = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_RIGHT;
    if (cancelKey || cancelButton) {
        static_cast<void>(context.logicIntents.post(
            runtime::CancelPendingWorldCommandIntent{
                .modeRevision = mode.revision,
            },
            context.projection.sessionRevision));
        context.pendingTarget.cancel();
        return PendingWorldInputResult::ConsumedAndReleaseCapture;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT &&
        std::isfinite(event.button.x) && std::isfinite(event.button.y)) {
        context.pendingTarget.arm(
            mode.revision, context.projection.sessionRevision);
        return PendingWorldInputResult::Consumed;
    }
    if (event.type != SDL_EVENT_MOUSE_BUTTON_UP ||
        event.button.button != SDL_BUTTON_LEFT ||
        !context.pendingTarget.armed()) {
        return PendingWorldInputResult::Ignored;
    }

    const std::optional<PendingWorldTargetCapture> capture =
        context.pendingTarget.release();
    if (!capture) return PendingWorldInputResult::Ignored;
    if (capture->modeRevision != mode.revision ||
        capture->sessionRevision != context.projection.sessionRevision ||
        !std::isfinite(event.button.x) ||
        !std::isfinite(event.button.y)) {
        return PendingWorldInputResult::Consumed;
    }

    if (!context.viewport.valid() ||
        event.button.x < 0.0f || event.button.y < 0.0f ||
        event.button.x >= static_cast<float>(context.viewport.width) ||
        event.button.y >= static_cast<float>(context.viewport.height)) {
        return PendingWorldInputResult::Consumed;
    }
    const PresentedWorldInputTarget hit =
        context.presentation.worldInputTargetAt(
            event.button.x, event.button.y,
            mode.allowShrubberyTarget, mode.allowMineTarget,
            context.commandModes.forceAttack());
    const std::optional<engine::CommandPosition> resolvedPosition = hit.position
        ? std::optional<engine::CommandPosition>{engine::CommandPosition{
            .x = math::q32_32{hit.position->x()},
            .y = math::q32_32{hit.position->y()},
            .z = math::q32_32{hit.position->z()},
            .valid = true}}
        : std::nullopt;
    const engine::ObjectId target = mode.acceptsObject()
        ? hit.object
        : engine::INVALID_OBJECT_ID;
    static_cast<void>(context.logicIntents.post(
        runtime::SubmitPendingWorldCommandTargetIntent{
            .modeRevision = capture->modeRevision,
            .targetObject = target,
            .targetPosition = resolvedPosition,
            .screenX = event.button.x,
            .screenY = event.button.y,
            .viewportWidth = context.viewport.width,
            .viewportHeight = context.viewport.height,
            .presentationCamera = context.presentation.presentedCamera(),
            .queued = context.commandModes.queueOrders(),
        },
        capture->sessionRevision));
    return PendingWorldInputResult::Consumed;
}

} // namespace app::input
