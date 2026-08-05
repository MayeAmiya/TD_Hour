#include "InputCoordinatorImpl.h"

#include "PresentationCoordinator.h"
#include "app/input/CommandMapActionDispatcher.h"
#include "app/input/WorldCommandInputDispatcher.h"
#include "runtime/GameLogicIntent.h"
#include "ui/ingame/InGameGuiSubsystem.h"

#include "game/selection/LocalSelectionState.h"
#include "system/RendererSubsystem.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace app {
namespace {

[[nodiscard]] uint64_t eventTimestampMilliseconds(
    const SDL_Event& event) noexcept {
    return event.common.timestamp / 1'000'000ULL;
}

} // namespace

bool InputCoordinator::Impl::gameplayInputEnabled() const noexcept {
    return !m_gameProjection.isInGame() ||
        m_gameProjection.gameplayInputEnabled;
}

bool InputCoordinator::Impl::dispatchGuiEvent(const SDL_Event& event) {
    const input::WndDispatchResult result =
        m_wndInput.dispatch(event, m_gameProjection);
    if (result.modalConsumer) cancelGameplayCapture();
    return result.consumed;
}

bool InputCoordinator::Impl::pointerEventBlockedByWnd(
    const SDL_Event& event) const noexcept {
    return m_wndInput.blocksWorldPointer(event, m_inputViewport);
}

bool InputCoordinator::Impl::handlePlacementEvent(const SDL_Event& event) {
    if (!m_gameProjection.hasSession ||
        !m_gameProjection.localPlacementActive) {
        return false;
    }

    const bool cancelKey = event.type == SDL_EVENT_KEY_DOWN &&
        event.key.scancode == SDL_SCANCODE_ESCAPE;
    const bool cancelButton = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_RIGHT;
    if (cancelKey || cancelButton) {
        static_cast<void>(m_logicIntents.post(
            runtime::CancelLocalPlacementIntent{
                .previewIdentity =
                    m_gameProjection.localPlacementPreviewIdentity},
            m_gameProjection.sessionRevision));
        m_placementInput.cancel();
        m_queuedConstructionAuthoringBuilder =
            engine::INVALID_OBJECT_ID;
        if (m_pointerCapture.ownedBy(PointerCaptureOwner::Placement)) {
            clearPointerCapture();
        }
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
        const float tacticalHeight = static_cast<float>(m_inputViewport.height);
        if (event.button.x >= 0.0f &&
            event.button.x < static_cast<float>(m_inputViewport.width) &&
            event.button.y >= 0.0f && event.button.y < tacticalHeight) {
            static_cast<void>(m_placementInput.begin(
                {event.button.x, event.button.y}));
            return true;
        }
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION &&
        m_placementInput.active()) {
        static_cast<void>(m_placementInput.update(
            {event.motion.x, event.motion.y}));
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event.button.button == SDL_BUTTON_LEFT &&
        m_placementInput.active()) {
        m_placementInput.release(
            {event.button.x, event.button.y}, m_commandModes.forceAttack(),
            (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
        return true;
    }
    return false;
}

bool InputCoordinator::Impl::handleRadarEvent(const SDL_Event& event) {
    if (!m_presentation.hasRadarInput()) return false;
    const bool alternateMouse =
        m_gameProjection.hasSession &&
        m_gameProjection.inputSettings.useAlternateMouse;
    const uint8_t lookButton = alternateMouse
        ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT;
    const uint8_t moveButton = alternateMouse
        ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
    const bool hasSelection = m_gameProjection.commandUi.hasSelection;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        (event.button.button == lookButton ||
         (!hasSelection && event.button.button == moveButton))) {
        if (const auto world = m_presentation.radarWorldAt(
                event.button.x, event.button.y)) {
            m_radarInput.beginDrag(*world);
            return true;
        }
        return false;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
               event.button.button == moveButton &&
               m_gameProjection.commandUi.hasSelection) {
        if (const auto world = m_presentation.radarWorldAt(
                event.button.x, event.button.y)) {
            if (m_inputViewport.valid()) {
                static_cast<void>(m_logicIntents.post(
                    runtime::SubmitContextualWorldCommandIntent{
                        .targetObject = engine::INVALID_OBJECT_ID,
                        .targetPosition = engine::CommandPosition{
                            .x = math::q32_32{world->x()},
                            .y = math::q32_32{world->y()},
                            .z = math::q32_32{world->z()},
                            .valid = true,
                        },
                        .screenX = event.button.x,
                        .screenY = event.button.y,
                        .viewportWidth = m_inputViewport.width,
                        .viewportHeight = m_inputViewport.height,
                        .presentationCamera = m_presentation.presentedCamera(),
                        .queued = m_commandModes.queueOrders(),
                        .forceAttack = m_commandModes.forceAttack(),
                        .forceMove = m_commandModes.forceMove() &&
                            !m_commandModes.queueOrders(),
                    },
                    m_gameProjection.sessionRevision));
                return true;
            }
        }
        return false;
    } else if (event.type == SDL_EVENT_MOUSE_MOTION &&
               m_radarInput.dragging()) {
        if (const auto world = m_presentation.radarWorldAt(
                event.motion.x, event.motion.y)) {
            m_radarInput.queueLookAt(*world);
        }
        return true;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
               event.button.button == lookButton &&
               m_radarInput.dragging()) {
        m_radarInput.endDrag();
        return true;
    }

    return false;
}

void InputCoordinator::Impl::beginPointerCapture(
    PointerCaptureOwner owner, uint8_t button) noexcept {
    static_cast<void>(m_pointerCapture.begin(
        owner, button, m_gameProjection.hasSession
            ? m_gameProjection.sessionRevision : 0));
}

void InputCoordinator::Impl::clearPointerCapture() noexcept {
    m_pointerCapture.clear();
}

void InputCoordinator::Impl::cancelGameplayCapture() noexcept {
    clearPointerCapture();
    m_placementInput.cancel();
    m_radarInput.cancel();
    m_selectionDrag.cancel();
    m_inGameGui.setSelectionRectangle(false);
    cancelContextualWorldCommand();
    cancelDefaultRightClick();
    cancelPendingWorldTargetCapture();
    m_pendingWorldCursorPresenter.restore(m_inGameGui);
    resetGameplayHeldState();
}

void InputCoordinator::Impl::resetGameplayHeldState() noexcept {
    m_cameraController.reset();
    m_commandMap.resetActiveBindings();
    m_commandModes.reset();
    if (m_gameProjection.hasSession) {
        static_cast<void>(m_logicIntents.post(
            runtime::QueueCameraInputIntent{
                .input = {},
                .replacePending = true,
            },
            m_gameProjection.sessionRevision));
    }
}

void InputCoordinator::Impl::synchronizeGameplayInputOwnership() {
    if (m_wndInput.updateOwnership(m_gameProjection))
        resetGameplayHeldState();
    const bool modalOwned = m_wndInput.modalOwned();
    const bool textOwned = m_wndInput.textOwned();

    const bool cameraGameState =
        m_gameProjection.gameState == engine::GameState::Running ||
        m_gameProjection.gameState == engine::GameState::Paused;
    const bool configured = m_inputViewport.fullscreen
        ? m_gameProjection.inputSettings.cursorCaptureFullscreenGame
        : m_gameProjection.inputSettings.cursorCaptureWindowedGame;
    const bool desiredGrab = m_windowFocused && cameraGameState &&
        m_gameProjection.hasSession && configured &&
        !modalOwned && !textOwned;
    if (desiredGrab != m_mouseGrabApplied &&
        m_renderer.setMainThreadMouseGrab(desiredGrab)) {
        m_mouseGrabApplied = desiredGrab;
    }
}

void InputCoordinator::Impl::submitContextualWorldCommand(
    float screenX, float screenY, uint64_t sessionRevision,
    bool forceAttack, bool forceMove,
    std::optional<engine::CommandPosition> targetPosition,
    bool attackMove,
    bool guardPosition,
    std::optional<engine::selection::LocalSelectionGesture>
        fallbackSelection) {
    input::WorldCommandInputDispatcher::submitContextual(
        {
            .projection = m_gameProjection,
            .presentation = m_presentation,
            .logicIntents = m_logicIntents,
            .commandModes = m_commandModes,
            .pendingTarget = m_pendingWorldTarget,
            .viewport = m_inputViewport,
        },
        screenX, screenY, sessionRevision, forceAttack, forceMove,
        std::move(targetPosition), attackMove, guardPosition,
        std::move(fallbackSelection));
}

void InputCoordinator::Impl::armContextualWorldCommand(
    const SDL_Event& event) noexcept {
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.button.button != SDL_BUTTON_RIGHT ||
        !std::isfinite(event.button.x) || !std::isfinite(event.button.y)) {
        return;
    }
    static_cast<void>(m_contextualWorldCommand.begin(
        event.button.x, event.button.y,
        eventTimestampMilliseconds(event),
        m_gameProjection.sessionRevision, event.button.clicks));
}

void InputCoordinator::Impl::updateContextualWorldCommandPointer(
    float x, float y) noexcept {
    static_cast<void>(m_contextualWorldCommand.update(x, y));
}

void InputCoordinator::Impl::cancelContextualWorldCommand() noexcept {
    m_contextualWorldCommand.cancel();
}

void InputCoordinator::Impl::armDefaultRightClick(
    const SDL_Event& event) noexcept {
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.button.button != SDL_BUTTON_RIGHT ||
        !std::isfinite(event.button.x) || !std::isfinite(event.button.y)) {
        return;
    }
    static_cast<void>(m_defaultRightClick.begin(
        event.button.x, event.button.y,
        eventTimestampMilliseconds(event),
        m_gameProjection.sessionRevision));
}

void InputCoordinator::Impl::updateDefaultRightClick(
    float x, float y) noexcept {
    static_cast<void>(m_defaultRightClick.update(x, y));
}

void InputCoordinator::Impl::cancelDefaultRightClick() noexcept {
    m_defaultRightClick.cancel();
}

void InputCoordinator::Impl::completeDefaultRightClick(
    const SDL_Event& event) {
    if (event.type != SDL_EVENT_MOUSE_BUTTON_UP ||
        event.button.button != SDL_BUTTON_RIGHT) {
        return;
    }
    const float tolerance = static_cast<float>(
        m_gameProjection.inputSettings.dragTolerancePixels);
    const input::PointerClickCompletion completion =
        m_defaultRightClick.complete(
            event.button.x, event.button.y,
            eventTimestampMilliseconds(event), tolerance,
            m_gameProjection.inputSettings.dragToleranceMilliseconds);
    if (!completion.click || !m_gameProjection.hasSession ||
        completion.sessionRevision != m_gameProjection.sessionRevision) {
        return;
    }
    static_cast<void>(m_logicIntents.post(
        runtime::ResetLocalSelectionIntent{}, completion.sessionRevision));
}

void InputCoordinator::Impl::cancelPendingWorldTargetCapture() noexcept {
    m_pendingWorldTarget.cancel();
}

bool InputCoordinator::Impl::handlePendingWorldCommandEvent(
    const SDL_Event& event) {
    const input::PendingWorldInputResult result =
        input::WorldCommandInputDispatcher::handlePending(
            {
                .projection = m_gameProjection,
                .presentation = m_presentation,
                .logicIntents = m_logicIntents,
                .commandModes = m_commandModes,
                .pendingTarget = m_pendingWorldTarget,
                .viewport = m_inputViewport,
            },
            event);
    if (result == input::PendingWorldInputResult::
            ConsumedAndReleaseCapture &&
        m_pointerCapture.ownedBy(
            PointerCaptureOwner::PendingWorldCommand)) {
        clearPointerCapture();
    }
    return result != input::PendingWorldInputResult::Ignored;
}

void InputCoordinator::Impl::completeContextualWorldCommand(
    const SDL_Event& event) {
    if (event.type != SDL_EVENT_MOUSE_BUTTON_UP ||
        event.button.button != SDL_BUTTON_RIGHT) {
        return;
    }
    // ZH Mouse::isClick compares the down and up positions, not the maximum
    // excursion of the pointer path. MetaEvent additionally requires both
    // axes to remain strictly within DragTolerance for a point click.
    const float dragTolerancePixels = static_cast<float>(
        m_gameProjection.inputSettings.dragTolerancePixels);
    const uint64_t dragToleranceMilliseconds =
        m_gameProjection.inputSettings.dragToleranceMilliseconds;
    const input::PointerClickCompletion completion =
        m_contextualWorldCommand.complete(
            event.button.x, event.button.y,
            eventTimestampMilliseconds(event), dragTolerancePixels,
            dragToleranceMilliseconds);
    const bool guardPosition =
        m_gameProjection.inputSettings.doubleClickAttackMove &&
        completion.clickCount >= 2;
    if (!completion.click || !m_gameProjection.hasSession ||
        !m_gameProjection.isInGame() || !gameplayInputEnabled() ||
        completion.sessionRevision != m_gameProjection.sessionRevision) {
        return;
    }

    submitContextualWorldCommand(
        completion.screenX, completion.screenY,
        completion.sessionRevision,
        guardPosition ? false : m_commandModes.forceAttack(),
        !guardPosition && m_commandModes.forceMove() &&
            !m_commandModes.queueOrders(),
        std::nullopt, false, guardPosition);
}

bool InputCoordinator::Impl::dispatchCapturedPointerEvent(
    const SDL_Event& event) {
    if (!m_pointerCapture.active()) return false;

    const PointerCaptureOwner owner = m_pointerCapture.owner();
    if ((owner == PointerCaptureOwner::Camera ||
         owner == PointerCaptureOwner::WorldCommand) &&
        m_contextualWorldCommand.armed()) {
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            updateContextualWorldCommandPointer(event.motion.x,
                                                event.motion.y);
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                   event.button.button == SDL_BUTTON_RIGHT) {
            updateContextualWorldCommandPointer(event.button.x,
                                                event.button.y);
        }
    }
    if (owner == PointerCaptureOwner::Camera &&
        m_defaultRightClick.armed()) {
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            updateDefaultRightClick(event.motion.x, event.motion.y);
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                   event.button.button == SDL_BUTTON_RIGHT) {
            updateDefaultRightClick(event.button.x, event.button.y);
        }
    }
    switch (owner) {
    case PointerCaptureOwner::Gui:
        static_cast<void>(dispatchGuiEvent(event));
        break;
    case PointerCaptureOwner::Placement:
        static_cast<void>(handlePlacementEvent(event));
        break;
    case PointerCaptureOwner::Radar:
        static_cast<void>(handleRadarEvent(event));
        break;
    case PointerCaptureOwner::Selection:
        static_cast<void>(handleSelectionEvent(event));
        break;
    case PointerCaptureOwner::Camera:
        static_cast<void>(m_cameraController.handleEvent(event));
        break;
    case PointerCaptureOwner::WorldCommand:
        break;
    case PointerCaptureOwner::PendingWorldCommand:
        static_cast<void>(handlePendingWorldCommandEvent(event));
        break;
    case PointerCaptureOwner::None:
        return false;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event.button.button == m_pointerCapture.button()) {
        if (event.button.button == SDL_BUTTON_RIGHT &&
            (owner == PointerCaptureOwner::Camera ||
             owner == PointerCaptureOwner::WorldCommand)) {
            completeContextualWorldCommand(event);
            if (owner == PointerCaptureOwner::Camera) {
                completeDefaultRightClick(event);
            }
        }
        clearPointerCapture();
    }
    return true;
}

bool InputCoordinator::Impl::handleSelectionEvent(const SDL_Event& event) {
    if (!m_gameProjection.hasSession ||
        m_gameProjection.localPlacementActive ||
        m_gameProjection.pendingWorldCommand.active()) {
        m_selectionDrag.cancel();
        m_inGameGui.setSelectionRectangle(false);
        return false;
    }
    if (m_selectionDrag.begin(event)) {
        m_inGameGui.setSelectionRectangle(
            true, m_selectionDrag.startX(), m_selectionDrag.startY(),
            m_selectionDrag.endX(), m_selectionDrag.endY());
        return true;
    }
    if (m_selectionDrag.update(event)) {
        m_inGameGui.setSelectionRectangle(
            true, m_selectionDrag.startX(), m_selectionDrag.startY(),
            m_selectionDrag.endX(), m_selectionDrag.endY());
        return true;
    }
    const std::optional<input::SelectionDragCompletion> completion =
        m_selectionDrag.complete(event);
    if (!completion) return false;

    m_inGameGui.setSelectionRectangle(false);
    const float dx = completion->endX - completion->startX;
    const float dy = completion->endY - completion->startY;
    const float dragThresholdPixels = static_cast<float>(
        m_gameProjection.inputSettings.dragTolerancePixels);
    const bool dragSelection =
        std::abs(dx) > dragThresholdPixels ||
        std::abs(dy) > dragThresholdPixels;

    if (!dragSelection && !m_commandModes.forceAttack() &&
        !m_commandModes.forceMove()) {
        if (const std::optional<engine::render::RenderVector> world =
                m_presentation.terrainWorldAt(
                    completion->endX, completion->endY)) {
            uint64_t nearestIdentity = 0;
            float nearestDistanceSquared =
                std::numeric_limits<float>::infinity();
            for (const engine::GameLogic::
                     LocalConstructionRouteNodeProjection& node :
                 m_gameProjection.localConstructionRouteNodes) {
                if (!node.valid()) continue;
                const float dx = node.position.x() - world->x();
                const float dy = node.position.y() - world->y();
                const float distanceSquared = dx * dx + dy * dy;
                const float radiusSquared =
                    node.selectionRadius * node.selectionRadius;
                if (distanceSquared > radiusSquared ||
                    distanceSquared >= nearestDistanceSquared) {
                    continue;
                }
                nearestDistanceSquared = distanceSquared;
                nearestIdentity = node.previewIdentity;
            }
            if (nearestIdentity != 0) {
                static_cast<void>(m_logicIntents.post(
                    runtime::SelectLocalConstructionRouteNodeIntent{
                        .previewIdentity = nearestIdentity,
                    },
                    m_gameProjection.sessionRevision));
                return true;
            }
        }
        if (const std::optional<PresentedOrderWaypointTarget> waypoint =
                m_presentation.orderWaypointAt(
                    completion->endX, completion->endY)) {
            static_cast<void>(m_logicIntents.post(
                runtime::SelectLocalOrderWaypointIntent{
                    .waypoint = {
                        .actor = waypoint->actor,
                        .sourceSequence = waypoint->sourceSequence,
                        .kind = waypoint->kind,
                    },
                },
                m_gameProjection.sessionRevision));
            return true;
        }
    }

    container::Vector<engine::ObjectId> objects;
    engine::ObjectId anchor = engine::INVALID_OBJECT_ID;
    if (dragSelection) {
        objects = m_presentation.selectableObjectsInRectangle(
            completion->startX, completion->startY,
            completion->endX, completion->endY);
    } else {
        anchor = m_presentation.selectableObjectAt(
            completion->endX, completion->endY);
        if (anchor) objects.push_back(anchor);
    }

    const SDL_Keymod modifiers = SDL_GetModState();
    const bool additive = m_commandModes.preferSelection();
    const bool forceAttack = m_commandModes.forceAttack();
    const bool forceMove = m_commandModes.forceMove() &&
        !m_commandModes.queueOrders();
    const bool alternateMouse =
        m_gameProjection.inputSettings.useAlternateMouse;
    if (!dragSelection && !alternateMouse &&
        (forceAttack || forceMove || !anchor)) {
        const bool guardPosition = !forceAttack && !forceMove &&
            m_gameProjection.inputSettings.doubleClickAttackMove &&
            completion->clickCount >= 2;
        submitContextualWorldCommand(
            completion->endX, completion->endY,
            m_gameProjection.sessionRevision, forceAttack, forceMove,
            std::nullopt, false, guardPosition);
        return true;
    }
    engine::selection::LocalSelectionGesture gesture;
    gesture.additive = additive;
    gesture.anchor = anchor;
    if (dragSelection) {
        gesture.kind = engine::selection::
            LocalSelectionGestureKind::Rectangle;
        gesture.candidates = std::move(objects);
    } else if (completion->clickCount >= 2 && anchor && !forceAttack &&
               !forceMove) {
        const bool acrossMap =
            (modifiers & SDL_KMOD_ALT) != 0;
        gesture.kind = acrossMap
            ? engine::selection::
                LocalSelectionGestureKind::MatchingTypeAcrossMap
            : engine::selection::
                LocalSelectionGestureKind::MatchingTypeVisible;
        if (!acrossMap) {
            if (m_inputViewport.valid()) {
                gesture.candidates =
                    m_presentation.selectableObjectsInRectangle(
                        0.0f, 0.0f,
                        static_cast<float>(m_inputViewport.width),
                        static_cast<float>(m_inputViewport.height));
            }
        }
    } else {
        gesture.kind = engine::selection::LocalSelectionGestureKind::Point;
    }
    const bool regularContextCandidate = !alternateMouse &&
        !dragSelection && !forceAttack && !forceMove && !additive && anchor &&
        completion->clickCount < 2;
    if (regularContextCandidate) {
        // RefCode SelectionTranslator asks CommandTranslator for a context
        // action before replacing the selection. Keep that arbitration on the
        // logic thread: enemy Attack / friendly Enter wins, while Reserved or
        // otherwise invalid context falls back to the exact point gesture.
        submitContextualWorldCommand(
            completion->endX, completion->endY,
            m_gameProjection.sessionRevision, false, false, std::nullopt,
            false, false, std::move(gesture));
    } else {
        static_cast<void>(m_logicIntents.post(
            runtime::ApplyLocalSelectionGestureIntent{
                .gesture = std::move(gesture)},
            m_gameProjection.sessionRevision));
    }
    return true;
}

bool InputCoordinator::Impl::handleCommandMapEvent(const SDL_Event& event) {
    const bool inGame = m_gameProjection.isInGame() && gameplayInputEnabled();
    if (!inGame) return false;
    const bool keyDown = event.type == SDL_EVENT_KEY_DOWN;
    const bool keyUp = event.type == SDL_EVENT_KEY_UP;
    if (!keyDown && !keyUp) return false;

#if TD_DEBUG_ENABLED
    // F9 remains the requested three-state world diagnostic in Debug. The
    // authored ZH ToggleControlBar binding is used by Release and by normal
    // non-diagnostic sessions.
    if (event.key.scancode == SDL_SCANCODE_F9 && !event.key.repeat) {
        return false;
    }
#endif

    m_commandMap.reloadIfNeeded();
    const auto binding = m_commandMap.match(
        event.key.scancode, keyDown, event.key.mod,
        inGame, event.key.repeat);
    if (!binding) return false;

    return input::CommandMapActionDispatcher::dispatch(
        {
            .view = {
                .renderer = m_renderer,
                .presentation = m_presentation,
                .cameraPresentation = m_cameraPresentation,
                .cameraController = m_cameraController,
                .radarInput = m_radarInput,
                .projection = m_gameProjection,
                .cameraTrackingDrawable = m_cameraTrackingDrawable,
            },
            .ui = {.inGameGui = m_inGameGui},
            .selection = {
                .presentation = m_presentation,
                .logicIntents = m_logicIntents,
                .commandModes = m_commandModes,
                .projection = m_gameProjection,
                .viewport = m_inputViewport,
            },
            .gameplay = {
                .logicIntents = m_logicIntents,
                .commandModes = m_commandModes,
                .projection = m_gameProjection,
            },
        },
        event, *binding, inGame);
}


} // namespace app
