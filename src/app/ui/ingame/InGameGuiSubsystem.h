#pragma once

#include "core/container/container_types.h"

#include "system/SubsystemInterface.h"
#include "WndRuntime.h"
#include "game/script/presentation/ScriptLetterboxPresentationConsumer.h"
#include "game/script/presentation/ScriptCommandBarPresentationConsumer.h"
#include "game/script/presentation/ScriptMilitaryCaptionPresentationConsumer.h"
#include "game/script/presentation/ScriptPresentationState.h"
#include "game/script/presentation/ScriptUiPresentationControls.h"
#include "core/ecs/ObjectId.h"
#include "GameWndLayer.h"
#include "ScoreScreenViewModel.h"
#include "app/runtime/GameUiProjection.h"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <optional>
#include <functional>
#include <utility>
union SDL_Event;

namespace engine {
class AudioSubsystem;
class Renderer;
class TextureManager;
class GameSession;
enum class GameResultAction : uint8_t;
}
namespace app::runtime {
class GameLogicIntentMailbox;
}

class InGameGuiSubsystem : public SubsystemInterface {
public:
    explicit InGameGuiSubsystem(
        app::runtime::GameLogicIntentMailbox& logicIntents,
        engine::AudioSubsystem& audio);
    ~InGameGuiSubsystem() override;

    void init() override;
    void update() override;
    void shutdown() override;

    void render(engine::Renderer& renderer, engine::TextureManager& texMgr);
    bool handleEvent(const SDL_Event& event, engine::TextureManager& texMgr);
    void setExternalGameplayHudSuppressed(bool suppressed) noexcept {
        m_layer.setExternalGameplayHudSuppressed(suppressed);
    }
    void setGameProjection(
        const app::runtime::GameUiProjection& projection);

    gui::GameWndLayer& layer() { return m_layer; }
    const gui::GameWndLayer& layer() const { return m_layer; }
    [[nodiscard]] bool hasTextInputFocus() const noexcept {
        return m_layer.hasTextInputFocus();
    }
    bool activateLocalizedCommandHotkey(
        uint32_t scancode, uint32_t modifiers);
    // Input owns the CommandMap dispatch, while this facade keeps overlay
    // pause/music bookkeeping in the single WND owner.
    bool openOverlayFromInput(gui::GameWndOverlay overlay);
    void setUnderAttackHandler(std::function<void()> handler) {
        m_underAttackHandler = std::move(handler);
    }

    // Local-only selection ingress for the currently reconstructed ControlBar.
    // A future picker resolves ObjectId -> immutable ObjectType before calling
    // this; keeping that conversion outside the GUI avoids coupling script UI
    // presentation to ECS entity lifetime or a replicated selection state.
    void setSelectedControlBarObjectType(container::StringView objectTypeName);
    void setSelectedControlBarObject(engine::ObjectId object,
                                     container::StringView objectTypeName);
    void clearSelectedControlBarObjectType();
    void setSelectionRectangle(bool visible, float startX = 0.0f,
                               float startY = 0.0f, float endX = 0.0f,
                               float endY = 0.0f) noexcept {
        m_selectionRectangleVisible = visible;
        m_selectionRectangleStartX = startX;
        m_selectionRectangleStartY = startY;
        m_selectionRectangleEndX = endX;
        m_selectionRectangleEndY = endY;
    }
    void setRadiusCursorPolyline(
        container::Span<const math::vec2> points,
        uint32_t color) noexcept {
        m_radiusCursorPointCount = std::min(
            points.size(), m_radiusCursorPoints.size());
        std::copy_n(points.begin(), m_radiusCursorPointCount,
                    m_radiusCursorPoints.begin());
        m_radiusCursorColor = color;
    }
    void clearRadiusCursorPolyline() noexcept {
        m_radiusCursorPointCount = 0;
    }
    void setScrollAnchor(bool visible, float x = 0.0f,
                         float y = 0.0f) noexcept {
        m_scrollAnchorVisible = visible;
        m_scrollAnchorX = x;
        m_scrollAnchorY = y;
    }
    [[nodiscard]] container::StringView selectedControlBarObjectType() const noexcept {
        return m_selectedControlBarObjectType;
    }
    [[nodiscard]] uint64_t loadingRevisionReadyForPresent() const noexcept {
        return m_loadingPresentedNotificationPosted &&
                m_loadingNotificationRevision == m_loadingVisualRevision
            ? m_loadingVisualRevision : 0;
    }
    [[nodiscard]] bool loadingPresentationActive() const noexcept {
        return m_loadingVisible || m_loadingFailureVisible ||
            m_loadingAttempted;
    }

private:
    app::runtime::GameLogicIntentMailbox& m_logicIntents;
    engine::AudioSubsystem& m_audio;
    app::runtime::GameUiProjection m_gameProjection;
    enum class LoadingVisualPhase : uint8_t {
        Hidden,
        FadingIn,
        Active,
        FadingOut,
        Dismissed,
    };

    gui::GameWndLayer m_layer;
    gui::WndRuntime m_loadingScreen;
    struct LoadingWidgetHandles final {
        uint64_t runtimeGeneration = 0;
        gui::Widget* progress = nullptr;
        gui::Widget* percent = nullptr;
        gui::Widget* status = nullptr;
        gui::Widget* location = nullptr;
        gui::Widget* objectives = nullptr;
        container::Array<gui::Widget*, 5> objectiveLines{};
        container::Array<gui::Widget*, 3> cameoTexts{};
        container::Vector<gui::Widget*> challengeEarly;
        container::Vector<gui::Widget*> challengeLeft;
        container::Vector<gui::Widget*> challengeRight;
        container::Vector<gui::Widget*> challengeCircles;
        container::Vector<gui::Widget*> challengeVersus;
    } m_loadingWidgets;
    bool m_active = false;
    bool m_loadingVisible = false;
    bool m_loadingAttempted = false;
    bool m_loadingFailureVisible = false;
    LoadingVisualPhase m_loadingVisualPhase = LoadingVisualPhase::Hidden;
    uint64_t m_loadingVisualRevision = 0;
    uint64_t m_loadingNotificationRevision = 0;
    bool m_loadingPresentedNotificationPosted = false;
    bool m_loadingDismissedNotificationPosted = false;
    float m_loadingDisplayedProgress = 0.0f;
    float m_loadingFadeT = 0.0f;
    std::chrono::steady_clock::time_point m_loadingAnimationStartedAt{};
    std::chrono::steady_clock::time_point m_loadingLastUpdateAt{};
    container::Vector<container::String> m_loadingObjectiveLines;
    bool m_postLoadingFadeActive = false;
    std::chrono::steady_clock::time_point m_postLoadingFadeStartedAt{};
    bool m_activationAttempted = false;
    gui::GameWndOverlay m_activeOverlay = gui::GameWndOverlay::QuitMenu;
    // This consumer drains confirmed ScriptSessionEvent values even when a
    // temporary WND overlay is active.  Keeping it here (rather than in
    // GameSession) prevents presentation lifetimes/localization from becoming
    // lockstep state while ensuring scripts cannot accumulate an unbounded
    // unconsumed UI-event vector.
    engine::script::ScriptPresentationState m_scriptPresentation;
    // Durable cinematic letterbox is sampled from GameSession by revision,
    // then faded on this client's wall clock. It is deliberately separate
    // from the tick-driven text/subtitle queue above.
    engine::script::ScriptLetterboxPresentationConsumer m_scriptLetterbox;
    // SHOW_MILITARY_CAPTION and SPEECH_PLAY share the original lower-left
    // military typewriter. It owns wall-clock reveal/fade so a script time
    // freeze cannot strand the briefing on screen.
    engine::script::ScriptMilitaryCaptionPresentationConsumer m_scriptMilitaryCaption;
    // Resolves a selected ObjectType's frozen CommandSet through the durable
    // COMMANDBAR_* override map. It is presentation-only and contains no ECS
    // object reference or order dispatch state.
    engine::script::ScriptCommandBarPresentationConsumer m_scriptCommandBar;
    engine::ObjectId m_selectedControlBarObject = engine::INVALID_OBJECT_ID;
    container::String m_selectedControlBarObjectType;
    bool m_scriptCommandBarCommandContextVisible = false;
    bool m_pointerOverScienceContext = false;
    bool m_selectionRectangleVisible = false;
    float m_selectionRectangleStartX = 0.0f;
    float m_selectionRectangleStartY = 0.0f;
    float m_selectionRectangleEndX = 0.0f;
    float m_selectionRectangleEndY = 0.0f;
    container::Array<math::vec2, 48> m_radiusCursorPoints{};
    size_t m_radiusCursorPointCount = 0;
    uint32_t m_radiusCursorColor = 0xffff4040u;
    bool m_scrollAnchorVisible = false;
    float m_scrollAnchorX = 0.0f;
    float m_scrollAnchorY = 0.0f;
    uint64_t m_observedCommandBarRevision = 0;
    uint64_t m_observedCommandBarSelectionRevision = UINT64_MAX;
    uint64_t m_observedLocalConstructionRouteSelection = UINT64_MAX;
    uint16_t m_observedConstructionProgressPermille = UINT16_MAX;
    bool m_observedSelectedUnderConstruction = false;
    uint64_t m_observedProductionQueueRevision = UINT64_MAX;
    uint64_t m_observedProductionQueueSelectionRevision = UINT64_MAX;
    engine::ObjectId m_observedProductionQueueProducer =
        engine::INVALID_OBJECT_ID;
    engine::session_query::InGameProductionQueueProjection
        m_observedProductionQueueProjection{};
    bool m_hasObservedProductionQueueProjection = false;
    uint64_t m_observedSciencePurchaseRevision = UINT64_MAX;
    uint64_t m_observedSpecialPowerShortcutRevision = UINT64_MAX;
    uint64_t m_observedPlayerMoneyRevision = UINT64_MAX;
    container::String m_loadedSpecialPowerShortcutWindow;
    uint64_t m_observedCommandOutcomeRevision = 0;
    // Receipts are published on the logic thread, while each ControlBar
    // callback is authored on the window thread.  Keep only this local
    // correlation so a terminal receipt refreshes the surface which created
    // it; neither this bookkeeping nor the acknowledgement enters the
    // deterministic command stream.
    enum class CommandOutcomeSurface : uint8_t {
        CommandBar,
        ProductionQueue,
        SciencePurchase,
        SpecialPowerShortcut,
    };
    struct TrackedCommandOutcomeRequest final {
        uint64_t requestSequence = 0;
        CommandOutcomeSurface surface = CommandOutcomeSurface::CommandBar;
    };
    container::Vector<TrackedCommandOutcomeRequest>
        m_trackedCommandOutcomeRequests;
    struct CommandOutcomeFeedback final {
        uint64_t requestSequence = 0;
        container::String text;
    };
    container::Deque<CommandOutcomeFeedback> m_commandOutcomeFeedbackQueue;
    uint64_t m_commandOutcomeFeedbackRequest = 0;
    container::String m_commandOutcomeFeedback;
    std::chrono::steady_clock::time_point m_commandOutcomeFeedbackStartedAt{};
    engine::ObjectId m_beaconEditorObject = engine::INVALID_OBJECT_ID;
    uint64_t m_beaconEditorCaptionRevision = 0;
    bool m_beaconEditorContextVisible = false;
    app::runtime::ScriptUiProjectionCursor m_scriptEventCursor;
    bool m_hasScriptEventCursor = false;
    uint64_t m_lastScriptPopupSequence = 0;
    uint64_t m_lastScriptLocalDefeatSequence = 0;
    uint64_t m_activeScriptPopupEpoch = 0;
    uint64_t m_activeScriptPopupSequence = 0;
    bool m_scriptPopupPauseApplied = false;
    bool m_inGameMenuPauseApplied = false;
    container::Vector<engine::script::ScriptCameoFlashPresentation> m_scriptCameoFlashes;
    uint64_t m_scriptCameoPresentationEpoch = 0;
    uint64_t m_observedGameSessionRevision = 0;
    uint64_t m_presentedMatchResultRevision = 0;
    container::String m_presentedResultTransitionError;
    std::optional<gui::ingame::ScoreScreenViewModel> m_scoreScreenViewModel;
    bool m_scoreScreenMusicRequested = false;
    std::function<void()> m_underAttackHandler;
    std::chrono::steady_clock::time_point m_resultIntroStartedAt{};

    bool activate();
    void deactivate();
    void showLoadingScreen();
    void hideLoadingScreen();
    [[nodiscard]] bool postLoadingScreenPresentedNotification();
    [[nodiscard]] bool postLoadingScreenDismissedNotification();
    void configureLoadingScreen();
    void cacheLoadingWidgetHandles();
    void clearLoadingWidgetHandles() noexcept;
    void updateLoadingScreenPresentation();
    void animateLoadingScreenWidgets(float elapsedSeconds);
    [[nodiscard]] float loadingFadeOverlayAlpha() const noexcept;
    bool openOverlay(gui::GameWndOverlay overlay);
    void closeOverlay();
    void openQuitMenu();
    void configureControlBarButtons();
    void configureCommandBarSlotButtons();
    void activateCommandBarSlot(
        size_t slot, uint8_t repeatCount = 1, bool queued = false);
    void configureOverlayButtons();
    void queueResultAction(engine::GameResultAction action);
    void startScoreScreenMusic();
    void stopScoreScreenMusic();
    void synchronizeMatchResult();
    void populateScoreScreen();
    void synchronizeScriptCommandBar();
    void synchronizeProductionQueue();
    void synchronizePrimaryControlBarContext();
    void synchronizeSciencePurchase();
    void synchronizeSpecialPowerShortcuts();
    void synchronizePlayerMoney();
    void consumeCommandOutcomes();
    void trackCommandOutcomeRequest(
        std::optional<uint64_t> requestSequence,
        CommandOutcomeSurface surface);
    void refreshCommandOutcomeSurface(CommandOutcomeSurface surface) noexcept;
    void enqueueCommandOutcomeFeedback(
        uint64_t requestSequence, container::String text);
    void advanceCommandOutcomeFeedback();
    void synchronizeBeaconEditor();
    void submitBeaconEditorText(container::StringView text);
    void consumeScriptPresentationEvents();
    void releaseScriptPopupPause();
    void requestInGameMenuPause();
    void releaseInGameMenuPause();
    void renderScriptLetterbox(engine::Renderer& renderer) const;
    void renderScriptPresentation(engine::Renderer& renderer) const;
};
