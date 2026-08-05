#include "core/container/container_types.h"
#include "InGameGuiSubsystem.h"
#include "app/runtime/GameLogicIntent.h"
#include "app/runtime/GameUiProjection.h"

#include "ControlBarSchemeRuntime.h"
#include "game/base/CampaignManager.h"
#include "game/base/ChallengeGenerals.h"
#include "Font.h"
#include "FontRegistry.h"
#include "Renderer.h"
#include "StringTable.h"
#include "TextureManager.h"
#include "VFS.h"
#include "Widget.h"
#include "system/AudioSubsystem.h"
#include "debug/debug.h"
#include "core/constants/Paths.h"
#include "core/constants/Colors.h"
#include "presentation/render/PresentationDefaults.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <utility>
#include "InGameGuiSubsystemDetail.h"

using namespace ingame_gui_detail;

namespace {

void drawFullUiCanvas(engine::Renderer& renderer, uint32_t color) {
    renderer.drawQuad(
        0.0f, 0.0f,
        renderer.getUiViewportWidth(), renderer.getUiViewportHeight(), color);
}

} // namespace

InGameGuiSubsystem::InGameGuiSubsystem(
    app::runtime::GameLogicIntentMailbox& logicIntents,
    engine::AudioSubsystem& audio)
    : m_logicIntents(logicIntents), m_audio(audio) {
    setName("InGameGUI");
}

InGameGuiSubsystem::~InGameGuiSubsystem() {
    shutdown();
}

void InGameGuiSubsystem::setGameProjection(
    const app::runtime::GameUiProjection& projection) {
    const bool nextOwnsInGameWindows =
        projection.gameState == engine::GameState::Running ||
        projection.gameState == engine::GameState::Paused ||
        projection.gameState == engine::GameState::Result;
    const bool ownerScopeChanged =
        m_gameProjection.sessionRevision != projection.sessionRevision ||
        (m_gameProjection.presentationEpoch != 0 &&
         m_gameProjection.presentationEpoch != projection.presentationEpoch);
    // Retire the old WND tree while the old projection is still installed.
    // Its pause/music cleanup must carry the old owner revision; assigning
    // the replacement first would let one stale WND frame post intents that
    // appear to belong to the new GameSession.
    if (m_active && (ownerScopeChanged || !nextOwnsInGameWindows)) {
        deactivate();
    }
    m_gameProjection = projection;
    gui::ingame::ControlBarSchemeRuntime::instance().setPowerMeterState({
        .production = projection.power.production,
        .consumption = projection.power.consumption,
        .logarithmicBase = projection.powerBarSettings.logarithmicBase,
        .intervals = projection.powerBarSettings.intervals,
        .yellowRange = projection.powerBarSettings.yellowRange,
        .sufficient = projection.power.sufficient,
    });
    m_layer.setWorldHoverDisplayNameLabel(
        m_gameProjection.hoveredObjectDisplayNameLabel);
    consumeCommandOutcomes();
}

bool InGameGuiSubsystem::openOverlayFromInput(gui::GameWndOverlay overlay) {
    return openOverlay(overlay);
}

void InGameGuiSubsystem::init() {
    TD_LOG_INFO("[InGameGUI] Ready");
}

void InGameGuiSubsystem::setSelectedControlBarObjectType(container::StringView objectTypeName) {
    if (!m_selectedControlBarObject && m_selectedControlBarObjectType == objectTypeName) return;
    m_selectedControlBarObject = engine::INVALID_OBJECT_ID;
    m_selectedControlBarObjectType.assign(objectTypeName);
    m_beaconEditorObject = engine::INVALID_OBJECT_ID;
    m_beaconEditorCaptionRevision = 0;
    if (m_active) {
        synchronizeScriptCommandBar();
        synchronizeProductionQueue();
        synchronizeSciencePurchase();
        synchronizeSpecialPowerShortcuts();
        synchronizeBeaconEditor();
    }
}

void InGameGuiSubsystem::setSelectedControlBarObject(
    engine::ObjectId object, container::StringView objectTypeName) {
    if (m_selectedControlBarObject == object &&
        m_selectedControlBarObjectType == objectTypeName) {
        return;
    }
    m_selectedControlBarObject = object;
    m_selectedControlBarObjectType.assign(objectTypeName);
    m_beaconEditorObject = engine::INVALID_OBJECT_ID;
    m_beaconEditorCaptionRevision = 0;
    if (m_active) {
        synchronizeScriptCommandBar();
        synchronizeProductionQueue();
        synchronizeSciencePurchase();
        synchronizeSpecialPowerShortcuts();
        synchronizeBeaconEditor();
    }
}

void InGameGuiSubsystem::clearSelectedControlBarObjectType() {
    setSelectedControlBarObject(engine::INVALID_OBJECT_ID, {});
}

void InGameGuiSubsystem::update() {
    advanceCommandOutcomeFeedback();
    const auto state = m_gameProjection.gameState;
    const bool loadingFailed = !m_gameProjection.loadingError.empty();
    const uint64_t sessionRevision = m_gameProjection.sessionRevision;
    if (m_active && m_observedGameSessionRevision != sessionRevision) {
        // Next/Retry can allocate the replacement GameSession at the same
        // address.  A monotonic revision, rather than pointer identity,
        // guarantees that old overlays/control-bar presentation are rebuilt.
        deactivate();
    }
    m_observedGameSessionRevision = sessionRevision;
    consumeScriptPresentationEvents();
    if (state != engine::GameState::Result) {
        stopScoreScreenMusic();
    }

    if (state == engine::GameState::Loading || loadingFailed) {
        if (m_loadingAttempted &&
            m_loadingVisualRevision != m_gameProjection.loadingRevision) {
            hideLoadingScreen();
        }
        m_loadingFailureVisible = loadingFailed;
        if (!m_loadingVisible && !m_loadingAttempted) {
            showLoadingScreen();
        }
        updateLoadingScreenPresentation();
        return;
    }

    if (m_loadingVisible || m_loadingAttempted) {
        hideLoadingScreen();
    }

    const bool shouldBeActive = state == engine::GameState::Running ||
                                state == engine::GameState::Paused ||
                                state == engine::GameState::Result;
    if (shouldBeActive && !m_active && !m_activationAttempted) {
        m_activationAttempted = true;
        if (activate()) {
            m_activationAttempted = false;
        }
    } else if (!shouldBeActive && m_active) {
        deactivate();
    } else if (!shouldBeActive) {
        m_activationAttempted = false;
    }

    if (m_active) {
        synchronizeScriptCommandBar();
        synchronizeProductionQueue();
        synchronizeSciencePurchase();
        synchronizeSpecialPowerShortcuts();
        synchronizePlayerMoney();
        synchronizeBeaconEditor();
        synchronizeMatchResult();
        m_layer.update();
    }
}

void InGameGuiSubsystem::shutdown() {
    releaseScriptPopupPause();
    hideLoadingScreen();
    deactivate();
    m_scriptPresentation.clear();
    m_scriptLetterbox.clear();
    m_scriptMilitaryCaption.clear();
    m_scriptEventCursor = {};
    m_hasScriptEventCursor = false;
    m_pointerOverScienceContext = false;
}

void InGameGuiSubsystem::render(engine::Renderer& renderer, engine::TextureManager& texMgr) {
    if (m_loadingVisible || m_loadingFailureVisible || m_loadingAttempted) {
        if (m_loadingVisible) m_loadingScreen.render(renderer, texMgr);
        if (!m_loadingVisible && !m_loadingFailureVisible) {
            // Missing/modded Loading WND still gets a real, presentable
            // fallback frame. This preserves the lifecycle handshake without
            // claiming an invisible timer transition was displayed.
            constexpr float virtualHeight = static_cast<float>(
                engine::presentation_defaults::VIRTUAL_HEIGHT);
            drawFullUiCanvas(renderer, 0xFF080810u);
            renderer.drawText(
                m_gameProjection.loadingStatus.empty()
                    ? container::String{"Loading..."}
                    : m_gameProjection.loadingStatus,
                32.0f, virtualHeight - 48.0f, kOpaqueWhite);
        }
        if (m_loadingFailureVisible) {
            drawFullUiCanvas(renderer, 0xE0101018u);
            constexpr float panelLeft = 64.0f;
            constexpr float panelTop = 214.0f;
            constexpr float panelWidth = 672.0f;
            constexpr float panelHeight = 172.0f;
            renderer.drawQuad(panelLeft, panelTop, panelWidth, panelHeight,
                              0xF0202028u);
            renderer.drawBorder(panelLeft, panelTop, panelWidth, panelHeight,
                                0xFFE05050u, 2);
            renderer.drawText("Loading failed", panelLeft + 18.0f,
                              panelTop + 18.0f, 0xFFFF8080u);
            container::String reason = m_gameProjection.loadingError;
            if (reason.size() > 512) reason.resize(512);
            const auto lines = wrapScriptText(
                renderer, nullptr, reason, panelWidth - 36.0f);
            const size_t lineCount = std::min<size_t>(lines.size(), 6);
            for (size_t index = 0; index < lineCount; ++index) {
                renderer.drawText(
                    lines[index], panelLeft + 18.0f,
                    panelTop + 52.0f + static_cast<float>(index) * 18.0f,
                    kOpaqueWhite);
            }
            renderer.drawText(
                "Close the game to return to the launcher.",
                panelLeft + 18.0f, panelTop + panelHeight - 26.0f,
                0xFFC0C0C0u);
        }
        const float fadeAlpha = loadingFadeOverlayAlpha();
        if (!m_loadingFailureVisible && fadeAlpha > 0.0f) {
            const uint32_t alpha = static_cast<uint32_t>(
                std::clamp(fadeAlpha, 0.0f, 1.0f) * 255.0f);
            drawFullUiCanvas(renderer, alpha << 24);
        }
        return;
    }
    if (!m_active) return;
    // W3DDisplay owns the legacy bars before InGameUI's post-draw widgets.
    // Keep that layering so modal WND overlays still render above the bars,
    // while the control bar itself is separately suppressed by the consumer.
    renderScriptLetterbox(renderer);
    if (m_radiusCursorPointCount >= 3u) {
        const float seconds = std::chrono::duration<float>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const float opacity = 0.75f + 0.25f *
            std::sin(seconds * math::TWO_PI);
        const uint32_t sourceAlpha = m_radiusCursorColor >> 24u;
        const uint32_t alpha = static_cast<uint32_t>(std::clamp(
            static_cast<float>(sourceAlpha) * opacity, 0.0f, 255.0f));
        const uint32_t color = (m_radiusCursorColor & 0x00ffffffu) |
            (alpha << 24u);
        for (size_t index = 0; index < m_radiusCursorPointCount; ++index) {
            const size_t next = (index + 1u) % m_radiusCursorPointCount;
            if (!std::isfinite(m_radiusCursorPoints[index].x()) ||
                !std::isfinite(m_radiusCursorPoints[index].y()) ||
                !std::isfinite(m_radiusCursorPoints[next].x()) ||
                !std::isfinite(m_radiusCursorPoints[next].y())) {
                continue;
            }
            renderer.drawLine(
                m_radiusCursorPoints[index].x(),
                m_radiusCursorPoints[index].y(),
                m_radiusCursorPoints[next].x(),
                m_radiusCursorPoints[next].y(), 2.0f, color, color);
        }
    }
    if (m_scrollAnchorVisible) {
        const float scaleX = renderer.getScaleX() > math::EPSILON
            ? renderer.getScaleX() : 1.0f;
        const float scaleY = renderer.getScaleY() > math::EPSILON
            ? renderer.getScaleY() : 1.0f;
        const float x = renderer.windowToUiX(m_scrollAnchorX);
        const float y = renderer.windowToUiY(m_scrollAnchorY);
        constexpr float radius = 8.0f;
        renderer.drawLine(x - radius, y, x + radius, y,
                          1.0f, 0xc0ffffffu, 0xc0ffffffu);
        renderer.drawLine(x, y - radius, x, y + radius,
                          1.0f, 0xc0ffffffu, 0xc0ffffffu);
        renderer.drawBorder(x - radius, y - radius,
                            radius * 2.0f, radius * 2.0f,
                            0x80ffffffu, 1);
    }
    m_layer.render(renderer, texMgr);
    if (m_selectionRectangleVisible) {
        const float scaleX = renderer.getScaleX() > math::EPSILON
            ? renderer.getScaleX() : 1.0f;
        const float scaleY = renderer.getScaleY() > math::EPSILON
            ? renderer.getScaleY() : 1.0f;
        const float left = renderer.windowToUiX(std::min(
            m_selectionRectangleStartX, m_selectionRectangleEndX));
        const float top = renderer.windowToUiY(std::min(
            m_selectionRectangleStartY, m_selectionRectangleEndY));
        const float width = std::abs(
            m_selectionRectangleEndX - m_selectionRectangleStartX) / scaleX;
        const float height = std::abs(
            m_selectionRectangleEndY - m_selectionRectangleStartY) / scaleY;
        renderer.drawQuad(left, top, width, height, 0x1830c060u);
        renderer.drawBorder(left, top, width, height, 0xff40e080u, 1);
    }
    // This corresponds to RefCode's InGameUI::postDraw(): WND/control-bar
    // content goes first, then transient script text/subtitles overlay it.
    renderScriptPresentation(renderer);
    if (m_postLoadingFadeActive) {
        constexpr float kPostLoadingFadeSeconds = 0.20f;
        const float elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - m_postLoadingFadeStartedAt).count();
        const float alphaValue = 1.0f - elapsed / kPostLoadingFadeSeconds;
        if (alphaValue <= 0.0f) {
            m_postLoadingFadeActive = false;
        } else {
            const uint32_t alpha = static_cast<uint32_t>(
                std::clamp(alphaValue, 0.0f, 1.0f) * 255.0f);
            drawFullUiCanvas(renderer, alpha << 24);
        }
    }
}

bool InGameGuiSubsystem::handleEvent(const SDL_Event& event, engine::TextureManager& texMgr) {
    if (m_gameProjection.gameState == engine::GameState::Loading ||
        m_loadingFailureVisible) {
        if (m_loadingVisible) {
            static_cast<void>(m_loadingScreen.handleEvent(event, &texMgr));
        }
        // Loading is presentation-only and modal even when a mod omits its
        // WND. Never leak input to an old Result/ControlBar layer underneath.
        return true;
    }
    if (!m_active) return false;

    if (m_gameProjection.gameState == engine::GameState::Result &&
        event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE) {
        // A terminal overlay is not a dismissible pause menu.  Keep the
        // frozen result visible until a typed Next/Retry/Exit action is used.
        return true;
    }

    // INGAME_POPUP_MESSAGE is a client-modal acknowledgement surface. It is
    // deliberately handled before ordinary input/escape routing so a paused
    // campaign cannot open the quit menu or issue gameplay commands through
    // the popup. Network/replay sessions still show the modal but never
    // receive a local simulation pause.
    const auto& popup = m_gameProjection.scriptUi.popup;
    if (m_gameProjection.hasSession && popup.active &&
        popup.stamp.presentationEpoch ==
            m_gameProjection.scriptUi.presentationEpoch) {
        const bool dismiss = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            (event.type == SDL_EVENT_KEY_DOWN &&
             (event.key.scancode == SDL_SCANCODE_RETURN ||
              event.key.scancode == SDL_SCANCODE_KP_ENTER ||
              event.key.scancode == SDL_SCANCODE_SPACE ||
              event.key.scancode == SDL_SCANCODE_ESCAPE));
        if (dismiss && m_logicIntents.post(
                app::runtime::DismissScriptPopupIntent{
                    .presentationEpoch = popup.stamp.presentationEpoch,
                    .sequence = popup.stamp.sequence,
                },
                m_gameProjection.sessionRevision)) {
            releaseScriptPopupPause();
            m_activeScriptPopupEpoch = 0;
            m_activeScriptPopupSequence = 0;
        }
        return true;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE) {
        if (!m_layer.hasOverlay() && m_beaconEditorContextVisible) {
            // BeaconWindowInput in the original clears the one selected
            // beacon instead of opening the quit menu.
            static_cast<void>(m_logicIntents.post(
                app::runtime::ResetLocalSelectionIntent{},
                m_gameProjection.sessionRevision));
            clearSelectedControlBarObjectType();
            return true;
        }
        if (m_layer.hasOverlay()) {
            if (m_gameProjection.gameState == engine::GameState::Result) {
                // Result overlays are the only route to staged Next/Retry/Exit;
                // Escape must not leave Result with no actionable UI.
                return true;
            }
            closeOverlay();
        } else {
            openQuitMenu();
        }
        return true;
    }

    // RefCode destroys ordinary gameplay commands while input is disabled,
    // but explicitly preserves MSG_META_OPTIONS. Keep Escape/QuitMenu above
    // this gate; the menu itself disables actions that are unsafe mid-cinematic.
    if (!m_layer.isGameplayInputEnabled() && !m_layer.hasOverlay()) return true;

    const bool handled = m_layer.handleEvent(event, texMgr);
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        const bool overScience = m_layer.isPointerOverContext(
            gui::GameWndContext::PurchaseScience);
        if (overScience && !m_pointerOverScienceContext &&
            m_gameProjection.localPlacementActive) {
            // ZH's GeneralsExpPointsInput cancels an active build-placement
            // mode when the pointer enters the science panel.
            static_cast<void>(m_logicIntents.post(
                app::runtime::CancelLocalPlacementIntent{
                    .previewIdentity =
                        m_gameProjection.localPlacementPreviewIdentity},
                m_gameProjection.sessionRevision));
        }
        m_pointerOverScienceContext = overScience;
    }
    return handled;
}

bool InGameGuiSubsystem::activate() {
    const engine::GameStartInfo defaultInfo;
    const auto& info = m_gameProjection.startInfo
        ? *m_gameProjection.startInfo : defaultInfo;
    const container::String side = controlBarSideForCurrentGame(info);
    gui::ingame::ControlBarSchemeRuntime::instance().setPlayerSide(side);
    TD_LOG_INFO("[InGameGUI] Control bar side='{}'", side.empty() ? "America" : side);

    if (!m_layer.init()) {
        TD_LOG_ERROR("[InGameGUI] Failed to initialize in-game WND layer");
        return false;
    }
    m_layer.setPushButtonPressHandler([this](const gui::Widget& widget) {
        const uint64_t epoch = m_gameProjection.audioPresentationEpoch;
        if (epoch == 0u) return;
        const container::StringView eventName =
            widget.pushButtonPressSound().empty()
            ? container::StringView{m_audio.events().settings().guiClickSound}
            : container::StringView{widget.pushButtonPressSound()};
        static_cast<void>(m_audio.requestUiEvent(
            container::String{eventName}, epoch));
    });
    m_layer.setDisabledPushButtonPressHandler([this](const gui::Widget&) {
        const uint64_t epoch = m_gameProjection.audioPresentationEpoch;
        if (epoch == 0u) return;
        // RefCode's disabled window hit-test uses this event directly rather
        // than a MiscAudio indirection.
        static_cast<void>(m_audio.requestUiEvent("GUIClickDisabled", epoch));
    });

    m_active = true;
    m_layer.setGameplayHudSuppressed(
        m_scriptLetterbox.suppressesGameplayHud() ||
        awaitingInitialScriptUiPolicy(m_gameProjection));
    if (m_gameProjection.hasSession) {
        m_layer.setScriptCameoFlashes(
            m_scriptCameoFlashes, m_gameProjection.confirmedTick,
            m_gameProjection.presentationEpoch);
    }
    configureControlBarButtons();
    synchronizeScriptCommandBar();
    synchronizeProductionQueue();
    synchronizeSciencePurchase();
    synchronizeSpecialPowerShortcuts();
    synchronizePlayerMoney();
    TD_LOG_INFO("[InGameGUI] Activated");
    return true;
}

void InGameGuiSubsystem::deactivate() {
    releaseScriptPopupPause();
    releaseInGameMenuPause();
    stopScoreScreenMusic();
    const bool hadLayer = m_active || m_layer.isLoaded();
    if (hadLayer) m_layer.shutdown();
    m_active = false;
    m_activationAttempted = false;
    m_scriptPresentation.clear();
    m_scriptLetterbox.clear();
    m_scriptMilitaryCaption.clear();
    m_scriptCommandBar.clear();
    m_observedCommandBarRevision = 0;
    m_observedLocalConstructionRouteSelection = UINT64_MAX;
    m_observedConstructionProgressPermille = UINT16_MAX;
    m_observedSelectedUnderConstruction = false;
    m_observedProductionQueueRevision = UINT64_MAX;
    m_observedProductionQueueSelectionRevision = UINT64_MAX;
    m_observedProductionQueueProducer = engine::INVALID_OBJECT_ID;
    m_observedProductionQueueProjection = {};
    m_hasObservedProductionQueueProjection = false;
    m_observedSciencePurchaseRevision = UINT64_MAX;
    m_observedSpecialPowerShortcutRevision = UINT64_MAX;
    m_observedPlayerMoneyRevision = UINT64_MAX;
    m_loadedSpecialPowerShortcutWindow.clear();
    m_observedCommandOutcomeRevision = 0;
    m_trackedCommandOutcomeRequests.clear();
    m_commandOutcomeFeedbackQueue.clear();
    m_commandOutcomeFeedbackRequest = 0;
    m_commandOutcomeFeedback.clear();
    m_commandOutcomeFeedbackStartedAt = {};
    m_selectedControlBarObject = engine::INVALID_OBJECT_ID;
    m_selectedControlBarObjectType.clear();
    m_scriptCommandBarCommandContextVisible = false;
    m_beaconEditorObject = engine::INVALID_OBJECT_ID;
    m_beaconEditorCaptionRevision = 0;
    m_beaconEditorContextVisible = false;
    m_scriptCameoFlashes.clear();
    m_scriptCameoPresentationEpoch = 0;
    m_presentedMatchResultRevision = 0;
    m_presentedResultTransitionError.clear();
    m_scoreScreenViewModel.reset();
    m_resultIntroStartedAt = {};
    m_lastScriptPopupSequence = 0;
    m_lastScriptLocalDefeatSequence = 0;
    m_activeScriptPopupEpoch = 0;
    m_activeScriptPopupSequence = 0;
    m_layer.setGameplayHudSuppressed(false);
    m_layer.setGameplayInputEnabled(true);
    m_layer.setSpecialPowerDisplayEnabled(true);
    m_scriptEventCursor = {};
    m_hasScriptEventCursor = false;
    if (hadLayer) TD_LOG_INFO("[InGameGUI] Deactivated");
}
