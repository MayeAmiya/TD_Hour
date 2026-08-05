#pragma once

#include "core/container/hash_containers.h"

#include "WndRuntime.h"
#include "game/session/query/InGameCommandProjection.h"
#include "game/script/presentation/ScriptCommandBarPresentationConsumer.h"
#include "game/script/presentation/ScriptUiPresentationControls.h"
#include <functional>
#include <chrono>
#include <optional>
#include <tuple>
union SDL_Event;

namespace engine {
class Renderer;
class TextureManager;
}
namespace gui {

class Widget;

enum class GameWndContext {
    Master,
    PurchaseScience,
    Command,
    BuildQueue,
    Beacon,
    UnderConstruction,
    ObserverInfo,
    ObserverList,
    OclTimer,
    Count
};

enum class GameWndOverlay {
    QuitMenu,
    QuitNoSave,
    SaveLoad,
    Options,
    Chat,
    Disconnect,
    ReplayControl,
    PopupReplay,
    ObserverQuit,
    ScoreScreen,
    Defeat,
    LocalDefeat,
    Victorious,
    InGamePopupMessage,
    Diplomacy,
    PopupCommunicator,
    GameInfoWindow,
    EstablishConnections,
    MapTransfer,
    CrcMismatch,
    ModalMessage,
    QuitMessageBox,
    BlankWindow
};

enum class GenPowersShortcutSide {
    None,
    US,
    China,
    GLA
};

struct GameWndRect final {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// In-match WND facade. It owns only the layouts visible during gameplay and
// deliberately avoids Shell's menu stack and legacy GameWindowManager behavior.
class GameWndLayer {
public:
    using ButtonHandler = std::function<void(Widget&)>;
    using PushButtonPressHandler = WndRuntime::PushButtonPressHandler;

    struct RuntimePaths {
        container::StringView primary;
        container::StringView fallback;
    };

    GameWndLayer() = default;
    ~GameWndLayer();

    bool init();
    void shutdown();
    void update();
    void render(engine::Renderer& renderer, engine::TextureManager& texMgr);
    bool handleEvent(const SDL_Event& event, engine::TextureManager& texMgr);
    void setWorldHoverDisplayNameLabel(container::StringView label) {
        m_worldHoverDisplayNameLabel.assign(label);
    }
    bool activateLocalizedCommandHotkey(
        uint32_t scancode, uint32_t modifiers);
    // Query only authored WND geometry/status.  It is intentionally separate
    // from handleEvent(): an opaque static parent without a callback still
    // blocks world picking, while SEE_THRU remains local to each parent-chain
    // node and does not automatically apply to children.
    [[nodiscard]] bool worldInputBlockedAtVirtual(
        float virtualX, float virtualY) const noexcept;

    bool isLoaded() const { return m_loaded; }
    bool isVisible() const { return m_visible; }
    [[nodiscard]] std::optional<GameWndRect>
    tacticalRadarPanel() const noexcept;
    void show();
    void hide();

    // Script letterbox is a presentation-only cinematic state.  Suppress the
    // in-match HUD/control bar while retaining modal overlays (pause/options/
    // quit) and without changing any game/session simulation state.
    void setGameplayHudSuppressed(bool suppressed) noexcept;
    // External presentation policy (for example a world-only capture) is
    // independent from script/result suppression and must not overwrite it.
    void setExternalGameplayHudSuppressed(bool suppressed) noexcept;
    [[nodiscard]] bool isGameplayHudSuppressed() const noexcept {
        return m_gameplayHudSuppressed;
    }
    // Script DISABLE_INPUT is distinct from cinematic letterbox: it preserves
    // visible HUD but must prevent it from receiving gameplay interactions.
    void setGameplayInputEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isGameplayInputEnabled() const noexcept {
        return m_gameplayInputEnabled;
    }
    // ENABLE/DISABLE_SPECIAL_POWER_DISPLAY policy. RefCode routes it to the
    // superweapon countdown timer list only (InGameUI::m_superweaponHiddenBy-
    // Script); it is deliberately NOT the generals shortcut bar, which this
    // flag used to hide by mistake.
    void setSpecialPowerDisplayEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isSpecialPowerDisplayEnabled() const noexcept {
        return m_specialPowerDisplayEnabled;
    }

    bool loadGenPowersShortcut(GenPowersShortcutSide side);
    bool loadGenPowersShortcut(container::StringView windowName);
    void clearGenPowersShortcut();

    bool openOverlay(GameWndOverlay overlay);
    bool openOverlay(container::StringView primaryPath, container::StringView fallbackPath = {});
    void closeOverlay();
    bool hasOverlay() const { return m_overlay != nullptr && m_overlay->isLoaded(); }
    [[nodiscard]] bool hasTextInputFocus() const noexcept;

    Widget* context(GameWndContext context);
    const Widget* context(GameWndContext context) const;
    [[nodiscard]] bool isPointerOverContext(GameWndContext context) const noexcept;
    Widget* find(container::StringView name);
    const Widget* find(container::StringView name) const;
    Widget* findGenPowersShortcut(container::StringView name);
    Widget* findOverlay(container::StringView name);
    const Widget* findOverlay(container::StringView name) const;

    void showContext(GameWndContext context);
    void hideContext(GameWndContext context);
    void hideAllContexts();
    // Applies the value-only result of a selected ObjectType's effective
    // CommandSet. Missing physical WND slots are deliberate: original ZH
    // ControlBar layouts expose fourteen buttons even though CommandSets
    // retain eighteen logical slots for script-only content.
    void applyScriptCommandBarSlots(
        container::Span<const engine::script::ScriptCommandBarUiSlot> slots);
    void applyCommandBarAvailability(
        container::Span<const engine::session_query::InGameCommandSlotAvailability>
            availability);
    // CAMEO_FLASH remains a local presentation effect.  The caller supplies
    // confirmed-tick stamped values; this layer binds them only to the
    // currently materialized command-bar widgets and never turns them into
    // a command, selection, or replicated UI state.
    void setScriptCameoFlashes(
        container::Span<const engine::script::ScriptCameoFlashPresentation> flashes,
        uint64_t confirmedTick,
        uint64_t presentationEpoch);
    void toggleControlBarCompact();
    void setControlBarCompact(bool compact);
    bool focusControlBarWidget(container::StringView name);
    void clearInteractionState() noexcept;
    void clearControlBarInteractionState();
    void clearControlBarSelectionPresentation();

    void bindButton(container::StringView name, ButtonHandler handler);
    void setPushButtonPressHandler(PushButtonPressHandler handler);
    void setDisabledPushButtonPressHandler(PushButtonPressHandler handler);

private:
    bool m_loaded = false;
    bool m_visible = true;
    WndRuntime m_controlBar;
    WndRuntime m_generalsExp;
    WndRuntime m_popupDescription;
    WndRuntime m_genPowersShortcut;
    // In-match overlays are parsed once per VFS content revision and retained
    // off-screen between opens. Shell/menu WNDs deliberately do not enter this
    // cache because the launcher will own that domain.
    container::HashMap<container::String, container::UniquePtr<WndRuntime>>
        m_overlayCache;
    WndRuntime* m_overlay = nullptr;
    uint64_t m_overlayCacheContentRevision = 0;
    container::Array<Widget*, static_cast<size_t>(GameWndContext::Count)> m_contextParents{};
    container::Vector<std::pair<container::String, ButtonHandler>> m_buttonHandlers;
    PushButtonPressHandler m_pushButtonPressHandler;
    PushButtonPressHandler m_disabledPushButtonPressHandler;
    container::HashMap<Widget*, std::tuple<int, int, int, int>> m_stageOriginalBounds;
    container::HashMap<Widget*, std::tuple<int, int, int, int>> m_animationStartBounds;
    container::HashMap<Widget*, std::tuple<int, int, int, int>> m_animationTargetBounds;
    float m_controlBarAnimationT = 1.0f;
    bool m_controlBarCompact = false;
    bool m_gameplayHudSuppressed = false;
    bool m_externalGameplayHudSuppressed = false;
    bool m_gameplayInputEnabled = true;
    bool m_specialPowerDisplayEnabled = true;
    struct ScriptCommandButtonBinding final {
        Widget* widget = nullptr;
        container::String commandButtonName;
        container::String buttonImage;
        container::String textLabel;
        container::String descriptionLabel;
        game::CommandButtonBorderType borderType =
            game::CommandButtonBorderType::None;
        engine::session_query::InGameCommandSlotAvailability availability;
    };
    container::Array<ScriptCommandButtonBinding,
               engine::script::ScriptCommandBarPresentationConsumer::kSlotCount>
        m_scriptCommandButtonBindings{};
    container::Vector<engine::script::ScriptCameoFlashPresentation> m_scriptCameoFlashes;
    uint64_t m_scriptCameoConfirmedTick = 0;
    uint64_t m_scriptCameoPresentationEpoch = 0;
    const WndRuntime* m_tooltipSource = nullptr;
    const Widget* m_tooltipWidget = nullptr;
    uint64_t m_tooltipHoverGeneration = 0;
    std::chrono::steady_clock::time_point m_tooltipHoverStartedAt{};
    // Immutable authored label copied from GameUiProjection. No ECS/content
    // pointer crosses into WND; updateTooltip resolves it through GameText.
    container::String m_worldHoverDisplayNameLabel;
    container::String m_cursorTooltipText;
    float m_cursorTooltipX = 0.0f;
    float m_cursorTooltipY = 0.0f;
    bool m_commandTooltipVisible = false;

    bool loadRuntime(WndRuntime& runtime, RuntimePaths paths, bool required);
    void invalidateOverlayCache();
    void applyControlBarScheme();
    void updateControlBarAnimation();
    void updateControlBarStageButtonImages();
    void renderScriptCameoFlashes(engine::Renderer& renderer) const;
    void updateTooltip();
    void hideTooltip();
    void renderCursorTooltip(engine::Renderer& renderer) const;
    void cacheContextParents();
    void bindStoredHandlers();
    void clearRuntimeInteractionState(GameWndContext context) noexcept;
    Widget* findInRuntime(WndRuntime& runtime, container::StringView name);
    const Widget* findInRuntime(const WndRuntime& runtime, container::StringView name) const;
};

} // namespace gui
