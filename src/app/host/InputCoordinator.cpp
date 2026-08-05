#include "InputCoordinator.h"
#include "InputCoordinatorImpl.h"

#include "PresentationCoordinator.h"
#include "runtime/GameLogicIntent.h"
#include "ui/ingame/InGameGuiSubsystem.h"

#include "TextureManager.h"
#include "WndRuntime.h"
#include "core/container/string_utils.h"
#include "core/constants/Strings.h"
#include "core/constants/Paths.h"
#include "debug/debug.h"
#include "engine/input/GameCameraController.h"
#include "input/CommandMapRuntime.h"
#include "game/selection/LocalPlacementAnchorInput.h"
#include "presentation/render/PresentationDefaults.h"
#include "system/RendererSubsystem.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

extern std::atomic<bool> g_quitRequested;

namespace app {
InputCoordinator::Impl::Impl(
    RendererSubsystem& renderer,
    InGameGuiSubsystem& inGameGui,
    engine::TextureManager& textureManager,
    PresentationCoordinator& presentation,
    runtime::GameLogicIntentMailbox& logicIntents)
    : m_renderer(renderer), m_inGameGui(inGameGui),
      m_presentation(presentation), m_logicIntents(logicIntents),
      m_windowEvents(renderer),
      m_cameraPresentation(renderer),
      m_wndInput(inGameGui, textureManager) {
    m_inGameGui.setUnderAttackHandler([this] {
        if (const auto world = m_presentation.lastRadarEventWorld()) {
            m_radarInput.queueLookAt(*world);
        }
    });
    m_presentation.setWorldInputOcclusionQuery(
        [this](float virtualX, float virtualY) {
            return m_inGameGui.layer().worldInputBlockedAtVirtual(
                virtualX, virtualY);
        });
}

InputCoordinator::Impl::~Impl() {
    m_presentation.setWorldInputOcclusionQuery({});
    m_inGameGui.setUnderAttackHandler({});
    m_pendingWorldCursorPresenter.restore(m_inGameGui);
    if (m_mouseGrabApplied) {
        static_cast<void>(m_renderer.setMainThreadMouseGrab(false));
    }
    if (m_scriptInputCursorHidden) SDL_ShowCursor();
}

void InputCoordinator::Impl::routeEvent(const SDL_Event& event,
                                         bool& running) {
    if (m_windowEvents.dispatch(event, running)) return;
    const bool pointerEvent =
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
        event.type == SDL_EVENT_MOUSE_MOTION ||
        event.type == SDL_EVENT_MOUSE_WHEEL;
    const bool capturedPointerEvent =
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
        event.type == SDL_EVENT_MOUSE_MOTION;
    const auto& popup = m_gameProjection.scriptUi.popup;
    const bool scriptPopupModal =
        popup.active && popup.stamp.presentationEpoch ==
            m_gameProjection.scriptUi.presentationEpoch;
    const bool modalActive =
        m_inGameGui.layer().hasOverlay() || scriptPopupModal;
    if (modalActive && m_pointerCapture.active()) {
        cancelGameplayCapture();
    }
    if (capturedPointerEvent && dispatchCapturedPointerEvent(event)) return;

    // Modal WNDs and focused entry fields own keyboard/text events before
    // CommandMap. This prevents Escape, Enter, letters and control shortcuts
    // from leaking into gameplay while editing beacon/chat text.
    const bool keyboardOrTextEvent =
        event.type == SDL_EVENT_KEY_DOWN ||
        event.type == SDL_EVENT_KEY_UP ||
        event.type == SDL_EVENT_TEXT_INPUT ||
        event.type == SDL_EVENT_TEXT_EDITING;
    if (keyboardOrTextEvent && modalActive) {
        static_cast<void>(dispatchGuiEvent(event));
        return;
    }
    if (keyboardOrTextEvent && m_gameProjection.isGameDomain() &&
        m_inGameGui.hasTextInputFocus()) {
        static_cast<void>(dispatchGuiEvent(event));
        return;
    }

    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        m_windowFocused = false;
        const auto& mode = m_gameProjection.pendingWorldCommand;
        if (mode.active()) {
            static_cast<void>(m_logicIntents.post(
                runtime::CancelPendingWorldCommandIntent{
                    .modeRevision = mode.revision,
                },
                m_gameProjection.sessionRevision));
        }
        clearPointerCapture();
        // A lost button-up must not leave the placement anchor armed. Keep
        // the placement mode itself active, but require a fresh world click
        // after focus returns.
        m_placementInput.cancel();
        cancelContextualWorldCommand();
        cancelPendingWorldTargetCapture();
        m_pendingWorldCursorPresenter.restore(m_inGameGui);
        m_radarInput.endDrag();
        m_selectionDrag.cancel();
        m_inGameGui.setSelectionRectangle(false);
        // SDL focus loss must revoke WND keyboard/hover ownership as well as
        // world pointer capture.  Otherwise a Beacon/chat entry field can
        // resume after alt-tab and consume the first gameplay shortcut.
        m_inGameGui.layer().clearInteractionState();
        resetGameplayHeldState();
    } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        m_windowFocused = true;
    }

    // Retail pending GUI commands consume global cancel before WND or camera
    // routing. This also guarantees a right click cannot both cancel and
    // issue the normal contextual Move/Attack command.
    const bool pendingCancelEvent =
        (event.type == SDL_EVENT_KEY_DOWN &&
         event.key.scancode == SDL_SCANCODE_ESCAPE) ||
        (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
         event.button.button == SDL_BUTTON_RIGHT);
    if (!modalActive && m_gameProjection.pendingWorldCommand.active() &&
        pendingCancelEvent && handlePendingWorldCommandEvent(event)) {
        return;
    }

    bool guiPointerDispatched = false;
    if (m_gameProjection.isGameDomain() && pointerEvent &&
        !m_selectionDrag.dragging()) {
        guiPointerDispatched = true;
        if (dispatchGuiEvent(event)) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                beginPointerCapture(PointerCaptureOwner::Gui,
                                    event.button.button);
            }
            return;
        }
    }
    const bool wndBlocksWorldPointer =
        m_gameProjection.isGameDomain() && pointerEvent &&
        pointerEventBlockedByWnd(event);
    if (m_gameProjection.isInGame() && gameplayInputEnabled()) {
        if (!modalActive && event.type == SDL_EVENT_KEY_UP &&
            m_inGameGui.activateLocalizedCommandHotkey(
                static_cast<uint32_t>(event.key.scancode),
                static_cast<uint32_t>(event.key.mod))) {
            return;
        }
        if (!modalActive && handleCommandMapEvent(event)) return;
        // An active targeted command owns the next world click. Radar is one
        // of its input surfaces (the pending dispatcher resolves both radar
        // coordinates and radar object hits), not a competing source of a
        // normal contextual Move. Keep global right-click cancellation above
        // this block and WND hit testing ahead of all world routing.
        if (m_gameProjection.pendingWorldCommand.active() &&
            handlePendingWorldCommandEvent(event)) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                m_pendingWorldTarget.armed()) {
                beginPointerCapture(
                    PointerCaptureOwner::PendingWorldCommand,
                    event.button.button);
            }
            return;
        }
        // LeftHUD is an authored opaque WND but owns a dedicated radar input
        // translator in RefCode. Give that translator first refusal; a point
        // outside the radar now returns false instead of swallowing every
        // mouse-down on the tactical world.
        if (handleRadarEvent(event)) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                m_radarInput.dragging()) {
                beginPointerCapture(PointerCaptureOwner::Radar,
                                    event.button.button);
            }
            return;
        }
        if (wndBlocksWorldPointer) return;
        if (handlePlacementEvent(event)) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                m_placementInput.active()) {
                beginPointerCapture(PointerCaptureOwner::Placement,
                                    event.button.button);
            }
            return;
        }
        if (handlePendingWorldCommandEvent(event)) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                m_pendingWorldTarget.armed()) {
                beginPointerCapture(
                    PointerCaptureOwner::PendingWorldCommand,
                    event.button.button);
            }
            return;
        }
        if (handleSelectionEvent(event)) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                m_selectionDrag.dragging()) {
                beginPointerCapture(PointerCaptureOwner::Selection,
                                    event.button.button);
            }
            return;
        }
    }
    // Captured world gestures were dispatched at the top of this function,
    // matching RefCode's tactical-view mouse lock. Only an uncaptured pointer
    // event that starts over an opaque WND is stopped here.
    if (wndBlocksWorldPointer) return;
    // The tactical camera is presentation-local and must remain responsive
    // when simulation is paused or temporarily waiting for confirmed work.
    // Modal WNDs were handled above, so allowing Paused here does not leak
    // mouse/keyboard movement through the pause menu itself.
    const bool cameraGameState =
        m_gameProjection.gameState == engine::GameState::Running ||
        m_gameProjection.gameState == engine::GameState::Paused;
    const bool inGameCameraActive = cameraGameState &&
        m_gameProjection.hasSession && gameplayInputEnabled();
#if TD_DEBUG_ENABLED
    const bool debugCameraActive =
        !m_gameProjection.isInGame() &&
        m_renderer.debugWorldEnabled();
#else
    constexpr bool debugCameraActive = false;
#endif
    const bool contextualRightClickActive =
        m_gameProjection.isInGame() && inGameCameraActive &&
        m_gameProjection.inputSettings.useAlternateMouse;
    if (contextualRightClickActive &&
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_RIGHT) {
        armContextualWorldCommand(event);
    }
    const bool defaultRightClickActive =
        m_gameProjection.isInGame() && inGameCameraActive &&
        !m_gameProjection.inputSettings.useAlternateMouse;
    if (defaultRightClickActive &&
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_RIGHT) {
        armDefaultRightClick(event);
    }
    if ((inGameCameraActive || debugCameraActive) &&
        m_cameraController.handleEvent(event)) {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            m_cameraController.isDragging()) {
            beginPointerCapture(PointerCaptureOwner::Camera,
                                event.button.button);
        }
        return;
    }
    if (m_contextualWorldCommand.armed() &&
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_RIGHT) {
        beginPointerCapture(PointerCaptureOwner::WorldCommand,
                            event.button.button);
        return;
    }

    switch (event.type) {
        case SDL_EVENT_KEY_DOWN: {
#if TD_DEBUG_ENABLED
            if (!event.key.repeat &&
                event.key.scancode == SDL_SCANCODE_F9) {
                if (m_renderer.worldSkeletonMode()) {
                    gui::WndRuntime::setSkeletonMode(false);
                    m_renderer.setWorldSkeletonMode(false);
                    m_renderer.setWorldTextureOnlyMode(true);
                    TD_LOG_INFO("[WndRuntime] UI + world render mode: TEXTURE_ONLY");
                } else if (m_renderer.worldTextureOnlyMode()) {
                    gui::WndRuntime::setSkeletonMode(false);
                    m_renderer.setWorldTextureOnlyMode(false);
                    m_renderer.setWorldSkeletonMode(false);
                    TD_LOG_INFO("[WndRuntime] UI + world render mode: NORMAL");
                } else {
                    m_renderer.setWorldTextureOnlyMode(false);
                    gui::WndRuntime::setSkeletonMode(true);
                    m_renderer.setWorldSkeletonMode(true);
                    TD_LOG_INFO("[WndRuntime] UI + world render mode: SKELETON");
                }
                break;
            }
#endif
            // F10/F11 are production camera controls, not diagnostics. Keep
            // them outside TD_DEBUG_ENABLED so Release has the same local
            // camera behavior as Debug.
            if (!event.key.repeat &&
                event.key.scancode == SDL_SCANCODE_F10 &&
                (inGameCameraActive || debugCameraActive)) {
                m_cameraController.queuePitchStepDegrees(0.5f);
                break;
            }
            if (!event.key.repeat &&
                event.key.scancode == SDL_SCANCODE_F11 &&
                (inGameCameraActive || debugCameraActive)) {
                m_cameraController.queuePitchStepDegrees(-0.5f);
                break;
            }
            static_cast<void>(dispatchGuiEvent(event));
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (!guiPointerDispatched) {
                static_cast<void>(dispatchGuiEvent(event));
            }
            break;
#if TD_DEBUG_ENABLED
        case SDL_EVENT_MOUSE_WHEEL: {
            float wheel = event.wheel.y;
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) wheel = -wheel;
            if (!m_gameProjection.isInGame() &&
                m_renderer.zoomDebugWorld(wheel)) {
                break;
            }
            if (!guiPointerDispatched) {
                static_cast<void>(dispatchGuiEvent(event));
            }
            break;
        }
#endif
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_TEXT_EDITING:
            static_cast<void>(dispatchGuiEvent(event));
            break;
        default:
            if (!guiPointerDispatched) {
                static_cast<void>(dispatchGuiEvent(event));
            }
            break;
    }
}

void InputCoordinator::Impl::queueCameraInput(
    float presentationDeltaSeconds) {
    const bool cameraGameState =
        m_gameProjection.gameState == engine::GameState::Running ||
        m_gameProjection.gameState == engine::GameState::Paused;
    if (cameraGameState) {
        if (!m_gameProjection.hasSession ||
            m_gameProjection.gameplayInputEnabled) {
            engine::GameCameraInput input = m_cameraController.consumeInput();
            m_cameraPresentation.applyPendingRestore(input);
            if (const auto radarLookAt =
                    m_radarInput.consumePendingLookAt()) {
                input.absoluteTarget = *radarLookAt;
                input.hasAbsoluteTarget = true;
                input.manualIntent = true;
            }
            if (m_cameraTrackingDrawable) {
                const engine::ObjectId tracked =
                    m_gameProjection.commandUi.selectedObject;
                const auto world =
                    m_presentation.objectWorldPosition(tracked);
                if (world) {
                    input.absoluteTarget = *world;
                    input.hasAbsoluteTarget = true;
                    input.manualIntent = true;
                } else {
                    m_cameraTrackingDrawable = false;
                }
            }
            // Publish the raw value every presentation frame, including an
            // all-zero held-axis sample. The camera mailbox coalesces axes and
            // accumulates discrete wheel/reset/cut fields, so releasing a key
            // can supersede an unconsumed held sample without losing a reset.
            static_cast<void>(m_logicIntents.post(
                runtime::QueueCameraInputIntent{.input = input},
                m_gameProjection.sessionRevision));
            if (const auto completion =
                    m_cameraPresentation.applyImmediate(
                        input, m_gameProjection,
                        presentationDeltaSeconds)) {
                static_cast<void>(m_logicIntents.post(
                    runtime::AcknowledgeScriptCameraCompletionIntent{
                        .completion = *completion,
                    },
                    m_gameProjection.sessionRevision));
            }
        } else {
            m_cameraController.reset();
            m_radarInput.cancel();
            if (const auto completion =
                    m_cameraPresentation.applyImmediate(
                        {}, m_gameProjection,
                        presentationDeltaSeconds)) {
                static_cast<void>(m_logicIntents.post(
                    runtime::AcknowledgeScriptCameraCompletionIntent{
                        .completion = *completion,
                    },
                    m_gameProjection.sessionRevision));
            }
        }
    }
#if TD_DEBUG_ENABLED
    else if (m_renderer.debugWorldEnabled()) {
        static_cast<void>(m_renderer.applyDebugWorldCameraInput(
            m_cameraController.consumeInput(), presentationDeltaSeconds));
    }
#endif
    else {
        m_cameraController.reset();
    }
}

bool InputCoordinator::Impl::processFrame(float presentationDeltaSeconds,
                                           bool& running) {
    // Window/output requests can originate on the logic/presentation thread,
    // but SDL requires the main event owner. Apply only the newest request
    // before sampling fullscreen and viewport state for this input frame.
    m_renderer.serviceMainThreadWindow();
    m_inputViewport = m_renderer.inputViewport();
    float cursorX = 0.0f;
    float cursorY = 0.0f;
    static_cast<void>(SDL_GetMouseState(&cursorX, &cursorY));
    const bool cursorOnRadar = static_cast<bool>(
        m_presentation.radarWorldAt(cursorX, cursorY));
    const bool cursorBlockedByWnd = m_inputViewport.valid() &&
        m_inGameGui.layer().worldInputBlockedAtVirtual(
            m_inputViewport.toUiX(cursorX),
            m_inputViewport.toUiY(cursorY));
    m_presentation.setWaypointMode(m_commandModes.queueOrders());
    m_pendingWorldCursorPresenter.synchronize(
        m_gameProjection, m_inGameGui, m_presentation,
        m_commandModes.forceAttack(),
        m_commandModes.queueOrders(),
        !cursorBlockedByWnd || cursorOnRadar);
    m_commandMap.reloadIfNeeded();
    synchronizeGameplayInputOwnership();
    if (m_gameProjection.hasSession) {
        auto cameraSettings = m_gameProjection.cameraInputSettings;
        cameraSettings.viewportHeightScale = 1.0f;
        m_cameraController.configure(
            cameraSettings, m_gameProjection.inputSettings);
        m_cameraController.setPresentationContext(
            m_inputViewport.width, m_inputViewport.height,
            m_inputViewport.fullscreen,
            m_mouseGrabApplied);
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) routeEvent(event, running);
    synchronizeGameplayInputOwnership();
    const bool drawScrollAnchor = m_gameProjection.hasSession &&
        m_gameProjection.inputSettings.drawScrollAnchor &&
        m_cameraController.rightMouseDragging();
    m_inGameGui.setScrollAnchor(
        drawScrollAnchor,
        m_cameraController.rightMouseAnchorX(),
        m_cameraController.rightMouseAnchorY());

    if (g_quitRequested.exchange(false)) {
        TD_LOG_INFO(MSG_QUIT_REQUESTED.data());
        running = false;
        return false;
    }

    queueCameraInput(presentationDeltaSeconds);
    return true;
}

void InputCoordinator::Impl::queuePlacementIntent(int frameCount) {
    if (!m_gameProjection.localPlacementActive) {
        m_placementInput.cancel();
        m_queuedConstructionAuthoringBuilder =
            engine::INVALID_OBJECT_ID;
        return;
    }

    engine::selection::LocalPlacementScreenPoint pointer;
    static_cast<void>(SDL_GetMouseState(&pointer.x, &pointer.y));
    const input::PlacementPointerSample sample =
        m_placementInput.sample(pointer, m_commandModes.forceAttack());

    const auto& activePlacement = m_gameProjection.localPlacement;
    const bool routeAlreadyActive =
        m_queuedConstructionAuthoringBuilder ==
            activePlacement.sourceObject;
    const bool routePlacement = sample.confirm &&
        activePlacement.backend ==
            engine::selection::LocalPlacementBackendKind::Build &&
        (sample.queueConstruction || routeAlreadyActive);

    if (m_inputViewport.valid()) {
        const bool posted = m_logicIntents.post(
            runtime::UpdateLocalPlacementPointerIntent{
                .anchorStartX = sample.start.x,
                .anchorStartY = sample.start.y,
                .anchorEndX = sample.end.x,
                .anchorEndY = sample.end.y,
                .viewportWidth = m_inputViewport.width,
                .viewportHeight = m_inputViewport.height,
                .presentationCamera = m_presentation.presentedCamera(),
                .forceAttackSnap = sample.forceAttackSnap,
                .confirm = sample.confirm,
                .queueConstruction = routePlacement,
                .refreshLegality = (frameCount & 1) != 0,
                .fullHeightViewport = true,
            },
            m_gameProjection.sessionRevision);
        if (!posted) return;
    }
    if (sample.confirm) {
        if (routePlacement) {
            if (sample.queueConstruction) {
                m_queuedConstructionAuthoringBuilder =
                    activePlacement.sourceObject;
            }
            const auto start = m_presentation.terrainWorldAt(
                sample.start.x, sample.start.y);
            const auto end = m_presentation.terrainWorldAt(
                sample.end.x, sample.end.y);
            if (start && end) {
                const engine::CommandPosition fixedStart{
                    .x = math::q32_32{start->x()},
                    .y = math::q32_32{start->y()},
                    .z = math::q32_32{start->z()},
                    .valid = true,
                };
                const engine::CommandPosition fixedEnd{
                    .x = math::q32_32{end->x()},
                    .y = math::q32_32{end->y()},
                    .z = math::q32_32{end->z()},
                    .valid = true,
                };
                const bool hasDirection =
                    fixedStart.x != fixedEnd.x ||
                    fixedStart.y != fixedEnd.y;
                static_cast<void>(m_logicIntents.postTracked(
                    runtime::SubmitLocalConstructionWaypointIntent{
                        .placement = activePlacement,
                        .anchorStart = fixedStart,
                        .anchorEnd = fixedEnd,
                        .hasDirection = hasDirection,
                        .forceAttackSnap = sample.forceAttackSnap,
                    },
                    m_gameProjection.sessionRevision));
            }
            // A final non-Shift click appends the final point and exits the
            // placement cursor.  The local route continues independently.
            if (routeAlreadyActive && !sample.queueConstruction) {
                static_cast<void>(m_logicIntents.post(
                    runtime::CancelLocalPlacementIntent{
                        .previewIdentity =
                            m_gameProjection.localPlacementPreviewIdentity,
                    },
                    m_gameProjection.sessionRevision));
                m_queuedConstructionAuthoringBuilder =
                    engine::INVALID_OBJECT_ID;
            }
        }
        m_placementInput.acknowledgeConfirmation();
    }
}

void InputCoordinator::Impl::updateAfterLogicTick(int frameCount) {
    if (m_gameProjection.hasSession) {
        if (m_gameProjection.hasCommandBarSelection) {
            m_inGameGui.setSelectedControlBarObject(
                m_gameProjection.selectedCommandBarObject,
                m_gameProjection.selectedCommandBarObjectType);
        } else {
            m_inGameGui.clearSelectedControlBarObjectType();
        }
        queuePlacementIntent(frameCount);
    } else {
        m_inGameGui.clearSelectedControlBarObjectType();
        m_placementInput.cancel();
        m_queuedConstructionAuthoringBuilder =
            engine::INVALID_OBJECT_ID;
    }
}

void InputCoordinator::Impl::setGameProjection(
    const runtime::GameUiProjection& projection) {
    m_gameProjection = projection;
    m_cameraPresentation.setProjection(projection);
    if (!projection.localPlacementActive ||
        (m_queuedConstructionAuthoringBuilder &&
         projection.localPlacement.sourceObject !=
             m_queuedConstructionAuthoringBuilder)) {
        m_queuedConstructionAuthoringBuilder =
            engine::INVALID_OBJECT_ID;
    }
}

void InputCoordinator::Impl::synchronizePresentationState() {
    const bool scriptInputSuppressed =
        m_gameProjection.hasSession &&
        !m_gameProjection.gameplayInputEnabled;
    if (scriptInputSuppressed &&
        !m_pointerCapture.ownedBy(PointerCaptureOwner::Gui)) {
        // RefCode's DISABLE_INPUT clears selection/camera/waypoint modes so a
        // missing button-up during a cinematic cannot complete later.
        cancelGameplayCapture();
    }
    if (scriptInputSuppressed &&
        (!m_scriptInputCursorHidden ||
         m_scriptInputSuppressionSessionRevision !=
             m_gameProjection.sessionRevision)) {
        // ScriptActions::doDisableInput also deselects every drawable and
        // hides the pointer. Post selection reset once per suppression scope;
        // SDL cursor ownership stays on this main/input thread.
        const bool resetPosted = m_logicIntents.post(
            runtime::ResetLocalSelectionIntent{},
            m_gameProjection.sessionRevision);
        SDL_HideCursor();
        m_scriptInputCursorHidden = true;
        m_scriptInputSuppressionSessionRevision = resetPosted
            ? m_gameProjection.sessionRevision : 0;
    } else if (!scriptInputSuppressed) {
        // Cursor visibility is process-global in SDL.  Another gameplay
        // cursor mode (or a previous loading/cinematic path) may have hidden
        // it without setting our suppression flag, so the normal in-game
        // state must actively restore visibility as well.
        SDL_ShowCursor();
        if (m_scriptInputCursorHidden) {
            m_scriptInputCursorHidden = false;
            m_scriptInputSuppressionSessionRevision = 0;
        }
    }
    const bool staleGameplayCapture = m_pointerCapture.stale(
        m_gameProjection.hasSession, m_gameProjection.sessionRevision);
    if (staleGameplayCapture) {
        clearPointerCapture();
        cancelContextualWorldCommand();
        cancelPendingWorldTargetCapture();
        m_radarInput.endDrag();
        m_selectionDrag.cancel();
        m_inGameGui.setSelectionRectangle(false);
        m_cameraController.reset();
    }
    if (!m_presentation.hasRadarInput()) {
        m_radarInput.cancel();
        if (m_pointerCapture.ownedBy(PointerCaptureOwner::Radar)) {
            clearPointerCapture();
        }
    }
    if (!m_gameProjection.hasSession) {
        m_cameraTrackingDrawable = false;
        cancelContextualWorldCommand();
        cancelPendingWorldTargetCapture();
        m_pendingWorldCursorPresenter.restore(m_inGameGui);
        if (m_pointerCapture.ownedBy(PointerCaptureOwner::WorldCommand) ||
            (m_pointerCapture.ownedBy(PointerCaptureOwner::Camera) &&
             m_pointerCapture.sessionRevision() != 0)) {
            clearPointerCapture();
        }
#if TD_DEBUG_ENABLED
        if (!m_renderer.debugWorldEnabled()) m_cameraController.reset();
#else
        m_cameraController.reset();
#endif
    }
    const auto& mode = m_gameProjection.pendingWorldCommand;
    if ((!mode.active() ||
         m_pendingWorldTarget.staleFor(mode.revision)) &&
        m_pointerCapture.ownedBy(
            PointerCaptureOwner::PendingWorldCommand)) {
        clearPointerCapture();
        cancelPendingWorldTargetCapture();
    }
}

InputCoordinator::InputCoordinator(
    RendererSubsystem& renderer,
    InGameGuiSubsystem& inGameGui, engine::TextureManager& textureManager,
    PresentationCoordinator& presentation,
    runtime::GameLogicIntentMailbox& logicIntents)
    : m_impl(std::make_unique<Impl>(renderer, inGameGui, textureManager,
                                    presentation, logicIntents)) {}

InputCoordinator::~InputCoordinator() = default;

bool InputCoordinator::processFrame(float presentationDeltaSeconds,
                                    bool& running) {
    return m_impl->processFrame(presentationDeltaSeconds, running);
}

void InputCoordinator::setGameProjection(
    const runtime::GameUiProjection& projection) {
    m_impl->setGameProjection(projection);
}

void InputCoordinator::updateAfterLogicTick(int frameCount) {
    m_impl->updateAfterLogicTick(frameCount);
}

void InputCoordinator::synchronizePresentationState() {
    m_impl->synchronizePresentationState();
}

} // namespace app
