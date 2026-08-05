#include "core/container/hash_containers.h"
#include "core/container/string_utils.h"
#include "GameWndLayer.h"

#include "ControlBarSchemeRuntime.h"
#include "InGameDrawCallbacks.h"
#include "game/session/query/InGameCommandProjection.h"
#include "Renderer.h"
#include "Font.h"
#include "FontRegistry.h"
#include "StringTable.h"
#include "presentation/render/PresentationDefaults.h"
#include "core/constants/Colors.h"
#include "VFS.h"
#include "debug/debug.h"
#include "../../../core/constants/Paths.h"
#include "../../../core/constants/Strings.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <chrono>
#include <SDL3/SDL.h>
namespace gui {
namespace {

using ContextName = std::pair<GameWndContext, container::StringView>;

constexpr container::Array<ContextName, static_cast<size_t>(GameWndContext::Count)> kContextNames{{
    {GameWndContext::Master, "ControlBarParent"},
    {GameWndContext::PurchaseScience, "GenExpParent"},
    {GameWndContext::Command, "CommandWindow"},
    {GameWndContext::BuildQueue, "ProductionQueueWindow"},
    {GameWndContext::Beacon, "BeaconWindow"},
    {GameWndContext::UnderConstruction, "UnderConstructionWindow"},
    {GameWndContext::ObserverInfo, "ObserverPlayerInfoWindow"},
    {GameWndContext::ObserverList, "ObserverPlayerListWindow"},
    {GameWndContext::OclTimer, "OCLTimerWindow"},
}};

container::String stripWndPrefix(container::StringView name) {
    const size_t colon = name.rfind(':');
    if (colon == container::StringView::npos) {
        return container::String{name};
    }
    return container::String{name.substr(colon + 1)};
}

bool isWidgetInTree(const Widget* widget, const Widget* root) noexcept {
    for (const Widget* current = widget; current; current = current->getParent()) {
        if (current == root) return true;
    }
    return false;
}

using container::asciiEqualIgnoreCase;

constexpr int kDefaultTooltipDelayMs = 800;
constexpr int kTooltipWidth = 224;

container::String commandAvailabilityStatus(
    engine::StringTable& strings,
    engine::session_query::InGameCommandAvailabilityReason reason) {
    using Reason =
        engine::session_query::InGameCommandAvailabilityReason;
    const auto localized = [&strings](const char* label,
                                       const char* fallback) {
        return strings.fetchOrFallback(label, fallback);
    };
    switch (reason) {
    case Reason::None:
        return {};
    case Reason::InsufficientFunds:
        return localized(
            "TOOLTIP:TooltipNotEnoughMoneyToBuild",
            "Insufficient funds");
    case Reason::QueueBusy:
        return localized(
            "TOOLTIP:TooltipCannotPurchaseBecauseQueueBusy",
            "Production queue is busy");
    case Reason::QueueFull:
        return localized(
            "TOOLTIP:TooltipCannotPurchaseBecauseQueueFull",
            "Production queue is full");
    case Reason::ParkingPlacesFull:
        return localized(
            "TOOLTIP:TooltipCannotBuildUnitBecauseParkingFull",
            "No parking place is available");
    case Reason::MaximumSimultaneousReached:
        return localized(
            "TOOLTIP:TooltipCannotBuildUnitBecauseMaximumNumber",
            "Maximum number already reached");
    case Reason::MissingPrerequisiteScience:
    case Reason::MissingPrerequisiteUpgrade:
    case Reason::PrerequisitesNotMet:
        return localized(
            "TOOLTIP:TooltipCannotBuildDueToPrerequisites",
            "Requirements have not been met");
    case Reason::Underpowered:
        return localized("GUI:Underpowered", "Insufficient power");
    case Reason::MustBeStopped:
        return localized(
            "TOOLTIP:TooltipMustBeStopped", "Unit must stop first");
    case Reason::AlreadyComplete:
    case Reason::SingleUseConsumed:
        return localized(
            "TOOLTIP:AlreadyUpgradedDefault", "Already completed");
    case Reason::AlreadyInProgress:
        return localized(
            "TOOLTIP:TooltipAlreadyInProgress", "Already in progress");
    case Reason::Cooldown:
        return localized("TOOLTIP:PowerOnCooldown", "Recharging");
    case Reason::NoPassengers:
        return localized("TOOLTIP:TooltipNoPassengers", "No passengers");
    case Reason::Unmanned:
        return localized("TOOLTIP:TooltipUnmanned", "Unit is unmanned");
    case Reason::Unsellable:
        return localized("TOOLTIP:TooltipUnsellable", "Cannot be sold");
    case Reason::ScriptDisabled:
    case Reason::Disabled:
        return localized("TOOLTIP:TooltipDisabled", "Disabled");
    case Reason::UnauthorizedActor:
        return localized(
            "TOOLTIP:TooltipUnauthorizedActor",
            "This unit cannot use that command");
    case Reason::ActorUnavailable:
        return localized(
            "TOOLTIP:TooltipActorUnavailable", "Unit is unavailable");
    case Reason::ProductUnavailable:
        return localized(
            "TOOLTIP:TooltipProductUnavailable", "Currently unavailable");
    case Reason::MissingCapability:
    case Reason::MissingContentReference:
    case Reason::MissingButton:
    case Reason::UnsupportedCommand:
        // These indicate missing authored/runtime support rather than a
        // player-action condition. Do not expose implementation diagnostics
        // through the retail UI.
        return localized("GUI:CommandUnavailable", "Command unavailable");
    }
    return {};
}

char localizedLabelHotkey(container::StringView text) noexcept {
    for (size_t index = 0; index + 1u < text.size(); ++index) {
        if (text[index] != '&') continue;
        if (text[index + 1u] == '&') {
            ++index;
            continue;
        }
        const unsigned char value = static_cast<unsigned char>(
            text[index + 1u]);
        if (value >= 'A' && value <= 'Z') {
            return static_cast<char>(value - 'A' + 'a');
        }
        if (value >= 'a' && value <= 'z') {
            return static_cast<char>(value);
        }
    }
    return '\0';
}

void clearDynamicCommandButtonState(Widget& widget) {
    widget.hide();
    widget.setEnabled(false);
    widget.setActive(false);
    widget.setText({});
    widget.setTooltip({});
    widget.setCooldownClockProgress(1.0f);
    widget.setCommandButtonChrome(false);
    widget.clearCommandBorder();
    widget.clearDrawImage(0);
    widget.setPushButtonPressSound({});
    widget.onClick = {};
    widget.onPointerClick = {};
}

container::Vector<container::String> wrapTooltipText(
    const engine::Font* font, container::StringView text, int maximumWidth,
    float textScaleX = 1.0f) {
    container::Vector<container::String> lines;
    container::String current;
    const auto widthOf = [font, textScaleX](container::StringView value) {
        const float width = font
            ? static_cast<float>(font->getTextWidth(container::String{value}))
            : static_cast<float>(value.size()) * 8.0f;
        return width * textScaleX;
    };
    const auto flush = [&]() {
        if (!current.empty()) lines.push_back(std::move(current));
        current.clear();
    };
    size_t cursor = 0;
    while (cursor < text.size()) {
        if (text[cursor] == '\n') {
            flush();
            ++cursor;
            continue;
        }
        while (cursor < text.size() && text[cursor] != '\n' &&
               std::isspace(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < text.size() && text[cursor] != '\n' &&
               !std::isspace(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }
        if (begin == cursor) continue;
        const container::StringView word = text.substr(begin, cursor - begin);
        container::String candidate = current;
        if (!candidate.empty()) candidate.push_back(' ');
        candidate.append(word);
        if (!current.empty() && widthOf(candidate) > maximumWidth) {
            flush();
            current.assign(word);
        } else {
            current = std::move(candidate);
        }
    }
    flush();
    if (lines.empty() && !text.empty()) lines.emplace_back(text);
    return lines;
}

container::String filenameOnly(container::StringView path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == container::StringView::npos) {
        return container::String{path};
    }
    return container::String{path.substr(slash + 1)};
}

void addUnique(container::Vector<container::String>& candidates, container::String candidate) {
    if (candidate.empty()) return;
    for (const auto& existing : candidates) {
        if (existing == candidate) return;
    }
    candidates.push_back(std::move(candidate));
}

void addWndPathCandidate(container::Vector<container::String>& candidates, container::StringView path) {
    if (path.empty()) return;

    addUnique(candidates, container::String{path});
    if (container::startsWithIgnoreCase(path, "Window/")) return;

    // GameWindowManager::winCreateFromScript resolves authored WND names
    // inside the fixed Window namespace. ZH ships menu layouts one level
    // below that namespace; a filename-only request may refer to either the
    // Window root (ControlBar, ReplayControl, ...) or Window/Menus (loading
    // and result screens). Keep these explicit candidates and never scan the
    // VFS by basename, which could bind an unrelated same-name file.
    addUnique(candidates, "Window/" + container::String{path});
    if (!container::startsWithIgnoreCase(path, "Menus/")) {
        addUnique(candidates, "Window/Menus/" + filenameOnly(path));
    }
}

GameWndLayer::RuntimePaths overlayPaths(GameWndOverlay overlay) {
    switch (overlay) {
    case GameWndOverlay::QuitMenu:
        return {QUITMENU_WND, QUITMENU_WND_VFS};
    case GameWndOverlay::QuitNoSave:
        return {QUITNOSAVE_WND, QUITNOSAVE_WND_VFS};
    case GameWndOverlay::SaveLoad:
        return {POPUP_SAVELOAD_WND, POPUP_SAVELOAD_WND_VFS};
    case GameWndOverlay::Options:
        return {INGAME_OPTIONS_WND, INGAME_OPTIONS_WND_VFS};
    case GameWndOverlay::Chat:
        return {INGAME_CHAT_WND, INGAME_CHAT_WND_VFS};
    case GameWndOverlay::Disconnect:
        return {DISCONNECTSCREEN_WND, DISCONNECTSCREEN_WND_VFS};
    case GameWndOverlay::ReplayControl:
        return {REPLAY_CONTROL_WND, REPLAY_CONTROL_WND_VFS};
    case GameWndOverlay::PopupReplay:
        return {POPUP_REPLAY_WND, POPUP_REPLAY_WND_VFS};
    case GameWndOverlay::ObserverQuit:
        return {OBSERVER_QUIT_WND, OBSERVER_QUIT_WND_VFS};
    case GameWndOverlay::ScoreScreen:
        return {SCORESCREEN_WND, SCORESCREEN_WND_VFS};
    case GameWndOverlay::Defeat:
        return {DEFEAT_WND, DEFEAT_WND_VFS};
    case GameWndOverlay::LocalDefeat:
        return {LOCAL_DEFEAT_WND, LOCAL_DEFEAT_WND_VFS};
    case GameWndOverlay::Victorious:
        return {VICTORIOUS_WND, VICTORIOUS_WND_VFS};
    case GameWndOverlay::InGamePopupMessage:
        return {INGAME_POPUP_MESSAGE_WND, INGAME_POPUP_MESSAGE_WND_VFS};
    case GameWndOverlay::Diplomacy:
        return {DIPLOMACY_WND, DIPLOMACY_WND_VFS};
    case GameWndOverlay::PopupCommunicator:
        return {POPUP_COMMUNICATOR_WND, POPUP_COMMUNICATOR_WND_VFS};
    case GameWndOverlay::GameInfoWindow:
        return {GAMEINFO_WINDOW_WND, GAMEINFO_WINDOW_WND_VFS};
    case GameWndOverlay::EstablishConnections:
        return {ESTABLISH_CONNECTIONS_WND, ESTABLISH_CONNECTIONS_WND_VFS};
    case GameWndOverlay::MapTransfer:
        return {MAP_TRANSFER_WND, MAP_TRANSFER_WND_VFS};
    case GameWndOverlay::CrcMismatch:
        return {CRC_MISMATCH_WND, CRC_MISMATCH_WND_VFS};
    case GameWndOverlay::ModalMessage:
        return {MESSAGE_BOX_WND, MESSAGE_BOX_WND_VFS};
    case GameWndOverlay::QuitMessageBox:
        return {QUIT_MESSAGE_BOX_WND, QUIT_MESSAGE_BOX_WND_VFS};
    case GameWndOverlay::BlankWindow:
        return {BLANK_WINDOW_WND, BLANK_WINDOW_WND_VFS};
    }
    return {};
}

GameWndLayer::RuntimePaths genPowersPaths(GenPowersShortcutSide side) {
    switch (side) {
    case GenPowersShortcutSide::US:
        return {GENPOWERS_US_WND, GENPOWERS_US_WND_VFS};
    case GenPowersShortcutSide::China:
        return {GENPOWERS_CHINA_WND, GENPOWERS_CHINA_WND_VFS};
    case GenPowersShortcutSide::GLA:
        return {GENPOWERS_GLA_WND, GENPOWERS_GLA_WND_VFS};
    case GenPowersShortcutSide::None:
        break;
    }
    return {};
}

} // namespace

GameWndLayer::~GameWndLayer() {
    shutdown();
}

bool GameWndLayer::init() {
    shutdown();
    ingame::registerInGameDrawCallbacks();

    bool ok = true;
    ok &= loadRuntime(m_controlBar, {CONTROLBAR_WND, CONTROLBAR_WND_VFS}, true);
    ok &= loadRuntime(m_generalsExp, {GENERALS_EXP_WND, GENERALS_EXP_WND_VFS}, true);

    loadRuntime(m_popupDescription, {CONTROLBAR_TOOLTIP_WND, CONTROLBAR_TOOLTIP_WND_VFS}, false);

    if (!ok) {
        shutdown();
        return false;
    }

    cacheContextParents();
    applyControlBarScheme();
    bindStoredHandlers();

    // Original ControlBar::init() ends by switching to CB_CONTEXT_NONE.
    // WND files contain all context windows visible, so the lean runtime must
    // establish the same baseline explicitly before game logic selects a context.
    hideAllContexts();

    if (auto* tooltipRoot = m_popupDescription.root()) {
        tooltipRoot->hideRecursive();
    }

    m_visible = true;
    m_loaded = true;
    TD_LOG_INFO("[GameWndLayer] Initialized in-game WND layer");
    return true;
}

void GameWndLayer::shutdown() {
    hideTooltip();
    closeOverlay();
    invalidateOverlayCache();
    clearGenPowersShortcut();
    m_popupDescription.shutdown();
    m_generalsExp.shutdown();
    m_controlBar.shutdown();
    m_contextParents.fill(nullptr);
    m_stageOriginalBounds.clear();
    m_animationStartBounds.clear();
    m_animationTargetBounds.clear();
    m_controlBarAnimationT = 1.0f;
    m_controlBarCompact = false;
    m_gameplayHudSuppressed = false;
    m_gameplayInputEnabled = true;
    m_specialPowerDisplayEnabled = true;
    m_scriptCommandButtonBindings = {};
    m_scriptCameoFlashes.clear();
    m_scriptCameoConfirmedTick = 0;
    m_scriptCameoPresentationEpoch = 0;
    ingame::ControlBarSchemeRuntime::instance().setDrawOffsetY(0);
    m_loaded = false;
}

void GameWndLayer::update() {
    if (!m_loaded || !m_visible) return;

    if (!m_gameplayHudSuppressed && !m_externalGameplayHudSuppressed) {
        updateControlBarAnimation();
        if (m_controlBar.hasVisibleContent()) m_controlBar.update();
        if (m_generalsExp.hasVisibleContent()) m_generalsExp.update();
        if (m_popupDescription.hasVisibleContent()) m_popupDescription.update();
        if (m_genPowersShortcut.hasVisibleContent()) {
            m_genPowersShortcut.update();
        }
    }
    if (m_overlay && m_overlay->hasVisibleContent()) {
        m_overlay->update();
    }
    updateTooltip();
}

void GameWndLayer::render(engine::Renderer& renderer, engine::TextureManager& texMgr) {
    if (!m_loaded || !m_visible) return;

    if (!m_gameplayHudSuppressed && !m_externalGameplayHudSuppressed) {
        m_controlBar.render(renderer, texMgr);
        // The original ControlBar toggles a flashing status on the physical
        // command widget. Draw the local highlight immediately after that
        // widget pass so it remains part of the control bar, not a detached
        // script diagnostic, and stays below other HUD/overlay surfaces.
        renderScriptCameoFlashes(renderer);
        m_generalsExp.render(renderer, texMgr);
        m_genPowersShortcut.render(renderer, texMgr);
        m_popupDescription.render(renderer, texMgr);
        renderCursorTooltip(renderer);
    }
    if (m_overlay) {
        m_overlay->render(renderer, texMgr);
    }
}

std::optional<GameWndRect> GameWndLayer::tacticalRadarPanel() const noexcept {
    if (!m_loaded || !m_visible || m_gameplayHudSuppressed ||
        m_externalGameplayHudSuppressed) {
        return std::nullopt;
    }
    const Widget* panel = findInRuntime(m_controlBar, "LeftHUD");
    if (!panel || !panel->isVisible() || panel->width() <= 0 ||
        panel->height() <= 0) {
        return std::nullopt;
    }
    return GameWndRect{
        .left = static_cast<float>(panel->x()),
        .top = static_cast<float>(panel->y()),
        .width = static_cast<float>(panel->width()),
        .height = static_cast<float>(panel->height()),
    };
}

bool GameWndLayer::handleEvent(const SDL_Event& event, engine::TextureManager& texMgr) {
    if (!m_loaded || !m_visible) return false;

    if (m_overlay) {
        // A modal owns the entire input surface, not just its visible widgets.
        // Misses must never fall through to ControlBar, radar, selection or
        // camera routing in InputCoordinator.
        static_cast<void>(m_overlay->handleEvent(event, &texMgr));
        return true;
    }
    if (m_gameplayHudSuppressed || m_externalGameplayHudSuppressed ||
        !m_gameplayInputEnabled) {
        return false;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        int windowWidth = 0;
        int windowHeight = 0;
        engine::Renderer::instance().getWindowSize(windowWidth, windowHeight);
        if (windowWidth > 0 && windowHeight > 0) {
            const engine::Renderer& renderer = engine::Renderer::instance();
            m_cursorTooltipX = renderer.windowToUiX(event.motion.x);
            m_cursorTooltipY = renderer.windowToUiY(event.motion.y);
        }
    }
    if (m_genPowersShortcut.handleEvent(event, &texMgr)) return true;
    if (m_generalsExp.handleEvent(event, &texMgr)) return true;
    return m_controlBar.handleEvent(event, &texMgr);
}

bool GameWndLayer::activateLocalizedCommandHotkey(
    uint32_t scancode, uint32_t modifiers) {
    if (!m_loaded || !m_visible || m_overlay ||
        m_gameplayHudSuppressed || m_externalGameplayHudSuppressed ||
        !m_gameplayInputEnabled) {
        return false;
    }
    constexpr uint32_t blockingModifiers =
        SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI;
    if ((modifiers & blockingModifiers) != 0 ||
        scancode < SDL_SCANCODE_A || scancode > SDL_SCANCODE_Z) {
        return false;
    }
    const char key = static_cast<char>(
        'a' + (scancode - SDL_SCANCODE_A));
    engine::StringTable& strings = engine::StringTable::instance();
    for (const ScriptCommandButtonBinding& binding :
         m_scriptCommandButtonBindings) {
        // ACTIVE is not an availability bit here: applyCommandBarAvailability
        // rewrites it from `availability.active`, which the projection sets only
        // for toggle states (the selected SwitchWeapon slot, Overcharge on).
        // Requiring it made the authored "&letter" hotkey dead for every normal
        // enabled command button.  The sibling GenPowers/science hotkey paths
        // in InGameGuiSubsystemControlBar gate on visible+enabled only.
        if (!binding.widget || !binding.widget->isEffectivelyVisible() ||
            !binding.widget->isEnabled() ||
            !binding.widget->hasClickHandler()) {
            continue;
        }
        container::String label = binding.textLabel.empty()
            ? container::String{}
            : strings.fetch(binding.textLabel);
        if (label.empty()) label = binding.textLabel;
        if (localizedLabelHotkey(label) != key) continue;
        binding.widget->invokeClick({.modifiers = modifiers});
        return true;
    }
    if (m_genPowersShortcut.activateLocalizedHotkey(scancode, modifiers)) {
        return true;
    }
    if (m_generalsExp.activateLocalizedHotkey(scancode, modifiers)) {
        return true;
    }
    return m_controlBar.activateLocalizedHotkey(scancode, modifiers);
}

bool GameWndLayer::worldInputBlockedAtVirtual(
    float virtualX, float virtualY) const noexcept {
    if (!m_loaded || !m_visible) return false;
    if (m_overlay) return true;
    if (m_gameplayHudSuppressed || m_externalGameplayHudSuppressed) {
        return false;
    }

    const auto classify = [virtualX, virtualY](const WndRuntime& runtime)
        -> std::optional<bool> {
        using Disposition = WndRuntime::WorldInputDisposition;
        switch (runtime.worldInputDispositionAt(virtualX, virtualY)) {
        case Disposition::NoWindow:
            return std::nullopt;
        case Disposition::SeeThrough:
            return false;
        case Disposition::Blocked:
            return true;
        }
        return std::nullopt;
    };

    // This is the same front-to-back order used by handleEvent/render.  A
    // front-most SEE_THRU top-level window resolves the lookup and allows the
    // world through; it does not continue searching lower WND runtimes.
    if (const auto blocked = classify(m_genPowersShortcut)) {
        return *blocked;
    }
    if (const auto blocked = classify(m_generalsExp)) return *blocked;
    if (const auto blocked = classify(m_controlBar)) return *blocked;
    return false;
}

bool GameWndLayer::hasTextInputFocus() const noexcept {
    if (m_overlay && m_overlay->hasTextInputFocus()) return true;
    return m_controlBar.hasTextInputFocus() ||
        m_generalsExp.hasTextInputFocus() ||
        m_genPowersShortcut.hasTextInputFocus();
}

void GameWndLayer::show() {
    m_visible = true;
    if (auto* master = context(GameWndContext::Master)) {
        master->show();
    }
}

void GameWndLayer::hide() {
    m_visible = false;
    m_controlBar.clearInteractionState();
    m_generalsExp.clearInteractionState();
    m_genPowersShortcut.clearInteractionState();
    m_popupDescription.clearInteractionState();
}

void GameWndLayer::setGameplayHudSuppressed(bool suppressed) noexcept {
    if (suppressed && !m_gameplayHudSuppressed) {
        m_controlBar.clearInteractionState();
        m_generalsExp.clearInteractionState();
        m_genPowersShortcut.clearInteractionState();
        m_popupDescription.clearInteractionState();
    }
    m_gameplayHudSuppressed = suppressed;
}

void GameWndLayer::setExternalGameplayHudSuppressed(bool suppressed) noexcept {
    if (suppressed && !m_externalGameplayHudSuppressed) {
        m_controlBar.clearInteractionState();
        m_generalsExp.clearInteractionState();
        m_genPowersShortcut.clearInteractionState();
        m_popupDescription.clearInteractionState();
    }
    m_externalGameplayHudSuppressed = suppressed;
}

void GameWndLayer::setGameplayInputEnabled(bool enabled) noexcept {
    if (!enabled && m_gameplayInputEnabled) {
        m_controlBar.clearInteractionState();
        m_generalsExp.clearInteractionState();
        m_genPowersShortcut.clearInteractionState();
        m_popupDescription.clearInteractionState();
    }
    m_gameplayInputEnabled = enabled;
}

void GameWndLayer::setSpecialPowerDisplayEnabled(bool enabled) noexcept {
    // ENABLE/DISABLE_SPECIAL_POWER_DISPLAY controls only the superweapon
    // countdown list (RefCode InGameUI::setSuperweaponDisplayEnabledByScript
    // -> m_superweaponHiddenByScript, read exclusively by the superweapon
    // timer draw). It must not touch m_genPowersShortcut: the generals
    // shortcut bar is ControlBar::m_genPowersShortcutWin, whose visibility
    // follows science ownership, and hiding it here silently disabled the
    // player's shortcut powers on every map that runs this action.
    m_specialPowerDisplayEnabled = enabled;
}

bool GameWndLayer::loadGenPowersShortcut(GenPowersShortcutSide side) {
    clearGenPowersShortcut();
    const RuntimePaths paths = genPowersPaths(side);
    if (paths.primary.empty()) return true;

    bool loaded = loadRuntime(m_genPowersShortcut, paths, false);
    if (loaded) {
        bindStoredHandlers();
    }
    return loaded;
}

bool GameWndLayer::loadGenPowersShortcut(container::StringView windowName) {
    clearGenPowersShortcut();
    if (windowName.empty()) return true;
    const bool loaded = loadRuntime(
        m_genPowersShortcut, {windowName, {}}, false);
    if (loaded) bindStoredHandlers();
    return loaded;
}

void GameWndLayer::clearGenPowersShortcut() {
    m_genPowersShortcut.shutdown();
}

bool GameWndLayer::openOverlay(GameWndOverlay overlay) {
    // U-006: the current save codec only captures presentation state; it is
    // not a complete in-match checkpoint.  Keep the typed in-game boundary
    // closed until simulation/script/ECS restoration is implemented so a
    // shortcut or a future caller cannot bypass the hidden pause-menu entry.
    if (overlay == GameWndOverlay::SaveLoad) return false;

    const RuntimePaths paths = overlayPaths(overlay);
    return openOverlay(paths.primary, paths.fallback);
}

bool GameWndLayer::openOverlay(container::StringView primaryPath, container::StringView fallbackPath) {
    auto& vfs = io::VFS::instance();
    const uint64_t contentRevision = vfs.contentRevision();
    if (m_overlayCacheContentRevision != contentRevision) {
        invalidateOverlayCache();
        m_overlayCacheContentRevision = contentRevision;
    }

    container::String cacheKey{primaryPath};
    cacheKey.push_back('\n');
    cacheKey.append(fallbackPath);
    auto cached = m_overlayCache.find(cacheKey);
    if (cached == m_overlayCache.end()) {
        auto runtime = std::make_unique<WndRuntime>();
        if (!loadRuntime(*runtime, {primaryPath, fallbackPath}, true)) {
            return false;
        }
        cached = m_overlayCache.emplace(
            std::move(cacheKey), std::move(runtime)).first;
    }

    // Do not discard a working modal until the replacement has resolved and
    // loaded. A missing Mod Options WND must leave QuitMenu (and its pause
    // ownership) intact rather than producing a paused game with no overlay.
    closeOverlay();
    m_overlay = cached->second.get();
    m_overlay->resume();
    m_overlay->clearInteractionState();
    m_overlay->setBackHandler([this]() { closeOverlay(); });
    bindStoredHandlers();
    return true;
}

void GameWndLayer::closeOverlay() {
    if (m_overlay) {
        m_overlay->suspend();
        m_overlay = nullptr;
    }
}

void GameWndLayer::invalidateOverlayCache() {
    m_overlay = nullptr;
    for (auto& [path, runtime] : m_overlayCache) {
        static_cast<void>(path);
        if (runtime) runtime->shutdown();
    }
    m_overlayCache.clear();
    m_overlayCacheContentRevision = 0;
}

Widget* GameWndLayer::context(GameWndContext contextId) {
    size_t index = static_cast<size_t>(contextId);
    if (index >= m_contextParents.size()) return nullptr;
    return m_contextParents[index];
}

const Widget* GameWndLayer::context(GameWndContext contextId) const {
    size_t index = static_cast<size_t>(contextId);
    if (index >= m_contextParents.size()) return nullptr;
    return m_contextParents[index];
}

bool GameWndLayer::isPointerOverContext(GameWndContext contextId) const noexcept {
    if (!m_loaded || !m_visible) return false;
    const Widget* parent = context(contextId);
    if (!parent || !parent->isEffectivelyVisible()) return false;

    const WndRuntime* runtime = contextId == GameWndContext::PurchaseScience
        ? &m_generalsExp : &m_controlBar;
    const auto hovered = runtime->hoveredWidget();
    return hovered.widget && hovered.widget->isEffectivelyVisible() &&
        isWidgetInTree(hovered.widget, parent);
}

Widget* GameWndLayer::find(container::StringView name) {
    if (auto* found = findInRuntime(m_overlay ? *m_overlay : m_controlBar, name)) {
        return found;
    }
    if (m_overlay) {
        if (auto* found = findInRuntime(m_controlBar, name)) return found;
    }
    if (auto* found = findInRuntime(m_generalsExp, name)) return found;
    if (auto* found = findInRuntime(m_genPowersShortcut, name)) return found;
    return findInRuntime(m_popupDescription, name);
}

Widget* GameWndLayer::findGenPowersShortcut(container::StringView name) {
    return findInRuntime(m_genPowersShortcut, name);
}

const Widget* GameWndLayer::find(container::StringView name) const {
    if (m_overlay) {
        if (auto* found = findInRuntime(*m_overlay, name)) return found;
    }
    if (auto* found = findInRuntime(m_controlBar, name)) return found;
    if (auto* found = findInRuntime(m_generalsExp, name)) return found;
    if (auto* found = findInRuntime(m_genPowersShortcut, name)) return found;
    return findInRuntime(m_popupDescription, name);
}

Widget* GameWndLayer::findOverlay(container::StringView name) {
    if (!m_overlay) return nullptr;
    return findInRuntime(*m_overlay, name);
}

const Widget* GameWndLayer::findOverlay(container::StringView name) const {
    if (!m_overlay) return nullptr;
    return findInRuntime(*m_overlay, name);
}

void GameWndLayer::showContext(GameWndContext contextId) {
    if (contextId == GameWndContext::Master) {
        show();
        return;
    }

    // Context visibility belongs to the context parent. Child visibility is
    // retained dynamic state: recursively showing the parent resurrects empty
    // command slots which the current selection deliberately hid.
    for (size_t index = 0; index < m_contextParents.size(); ++index) {
        if (static_cast<GameWndContext>(index) == GameWndContext::Master ||
            static_cast<GameWndContext>(index) == contextId) {
            continue;
        }
        Widget* other = m_contextParents[index];
        if (!other) continue;
        const bool wasVisible = other->isEffectivelyVisible();
        other->hide();
        if (wasVisible) {
            clearRuntimeInteractionState(static_cast<GameWndContext>(index));
        }
    }
    if (auto* parent = context(contextId)) {
        parent->show();
    } else {
        TD_LOG_WARN("[GameWndLayer] Missing context parent {}", static_cast<int>(contextId));
    }
}

void GameWndLayer::hideContext(GameWndContext contextId) {
    if (auto* parent = context(contextId)) {
        const bool wasVisible = parent->isEffectivelyVisible();
        parent->hide();
        if (wasVisible) clearRuntimeInteractionState(contextId);
    }
}

void GameWndLayer::hideAllContexts() {
    for (size_t i = 0; i < m_contextParents.size(); ++i) {
        if (static_cast<GameWndContext>(i) == GameWndContext::Master) continue;
        if (m_contextParents[i]) {
            const bool wasVisible = m_contextParents[i]->isEffectivelyVisible();
            m_contextParents[i]->hide();
            if (wasVisible) {
                clearRuntimeInteractionState(static_cast<GameWndContext>(i));
            }
        }
    }
}

void GameWndLayer::clearRuntimeInteractionState(GameWndContext contextId) noexcept {
    switch (contextId) {
    case GameWndContext::PurchaseScience:
        m_generalsExp.clearInteractionState();
        break;
    case GameWndContext::Master:
    case GameWndContext::Command:
    case GameWndContext::BuildQueue:
    case GameWndContext::Beacon:
    case GameWndContext::UnderConstruction:
    case GameWndContext::ObserverInfo:
    case GameWndContext::ObserverList:
    case GameWndContext::OclTimer:
        m_controlBar.clearInteractionState();
        break;
    case GameWndContext::Count:
        break;
    }
}

bool GameWndLayer::focusControlBarWidget(container::StringView name) {
    Widget* widget = findInRuntime(m_controlBar, name);
    return widget && m_controlBar.focusWidget(widget);
}

void GameWndLayer::clearInteractionState() noexcept {
    m_controlBar.clearInteractionState();
    m_generalsExp.clearInteractionState();
    m_genPowersShortcut.clearInteractionState();
    m_popupDescription.clearInteractionState();
    if (m_overlay) m_overlay->clearInteractionState();
}

void GameWndLayer::clearControlBarInteractionState() {
    m_controlBar.clearInteractionState();
}

void GameWndLayer::clearControlBarSelectionPresentation() {
    m_controlBar.clearInteractionState();
    hideTooltip();
    m_scriptCameoFlashes.clear();
}

void GameWndLayer::applyScriptCommandBarSlots(
    container::Span<const engine::script::ScriptCommandBarUiSlot> slots) {
    if (!m_loaded) return;

    for (size_t slotIndex = 0;
         slotIndex < engine::script::ScriptCommandBarPresentationConsumer::kSlotCount;
         ++slotIndex) {
        container::String widgetName{"ButtonCommand"};
        const size_t oneBasedSlot = slotIndex + 1;
        if (oneBasedSlot < 10) widgetName.push_back('0');
        widgetName += std::to_string(oneBasedSlot);

        Widget* const widget = findInRuntime(m_controlBar, widgetName);
        // RefCode's MAX_COMMANDS_PER_SET is 18 while the standard WND has
        // ButtonCommand01..14 only. Do not compact later slots into a
        // different visual position when a widget is absent.
        if (!widget) continue;

        ScriptCommandButtonBinding& binding =
            m_scriptCommandButtonBindings[slotIndex];
        const bool widgetChanged = binding.widget != widget;
        if (widgetChanged) {
            binding = {};
            binding.widget = widget;
        }
        widget->setUseOverlayStates(true);

        const engine::script::ScriptCommandBarUiSlot* const presentation =
            slotIndex < slots.size() ? &slots[slotIndex] : nullptr;
        const bool materialized = presentation && presentation->visible &&
            !presentation->commandButtonName.empty() &&
            !presentation->buttonImage.empty();
        if (!materialized) {
            if (widgetChanged || !binding.commandButtonName.empty()) {
                clearDynamicCommandButtonState(*widget);
            }
            binding = {};
            binding.widget = widget;
            continue;
        }

        const bool presentationChanged = widgetChanged ||
            binding.commandButtonName != presentation->commandButtonName ||
            binding.buttonImage != presentation->buttonImage ||
            binding.textLabel != presentation->textLabel ||
            binding.descriptionLabel != presentation->descriptionLabel ||
            binding.borderType != presentation->borderType;
        if (!presentationChanged) {
            continue;
        }

        // Slot identity and authored presentation are stable across progress,
        // cash, cooldown and queue updates. Rebuild only when those stable
        // values actually change so WND hover/press state survives ordinary
        // dynamic projection refreshes.
        clearDynamicCommandButtonState(*widget);
        binding.commandButtonName = presentation->commandButtonName;
        binding.buttonImage = presentation->buttonImage;
        binding.textLabel = presentation->textLabel;
        binding.descriptionLabel = presentation->descriptionLabel;
        binding.borderType = presentation->borderType;
        widget->setCommandButtonChrome(true);
        widget->show();
        widget->setDrawImage(
            0, presentation->buttonImage, presentation->buttonImage,
            presentation->buttonImage);
        auto& scheme = ingame::ControlBarSchemeRuntime::instance();
        switch (presentation->borderType) {
        case game::CommandButtonBorderType::Build:
            widget->setCommandBorder(scheme.buttonBorderBuildColor());
            break;
        case game::CommandButtonBorderType::Upgrade:
            widget->setCommandBorder(scheme.buttonBorderUpgradeColor());
            break;
        case game::CommandButtonBorderType::Action:
            widget->setCommandBorder(scheme.buttonBorderActionColor());
            break;
        case game::CommandButtonBorderType::System:
            widget->setCommandBorder(scheme.buttonBorderSystemColor());
            break;
        case game::CommandButtonBorderType::None:
            break;
        }
        widget->setPushButtonPressSound("GUICommandBarClick");
    }
}

void GameWndLayer::applyCommandBarAvailability(
    container::Span<const engine::session_query::InGameCommandSlotAvailability>
        availability) {
    if (!m_loaded) return;
    for (size_t slotIndex = 0;
         slotIndex < m_scriptCommandButtonBindings.size(); ++slotIndex) {
        ScriptCommandButtonBinding& binding =
            m_scriptCommandButtonBindings[slotIndex];
        Widget* const widget = binding.widget;
        if (!widget) continue;
        if (binding.commandButtonName.empty() ||
            slotIndex >= availability.size() ||
            !availability[slotIndex].visible) {
            widget->setActive(false);
            widget->setEnabled(false);
            widget->hide();
            continue;
        }
        widget->show();
        // Availability owns both presentation and hit testing.  In
        // particular, an unavailable construction button must remain grey
        // and must not become a waypoint/queue entry merely because Shift is
        // held.
        widget->setEnabled(availability[slotIndex].enabled);
        widget->setActive(availability[slotIndex].active);
        widget->setCooldownClockProgress(
            static_cast<float>(availability[slotIndex].cooldown.readyPermille) /
            1000.0f);
        binding.availability = availability[slotIndex];
    }
}

void GameWndLayer::setScriptCameoFlashes(
    container::Span<const engine::script::ScriptCameoFlashPresentation> flashes,
    uint64_t confirmedTick,
    uint64_t presentationEpoch) {
    m_scriptCameoConfirmedTick = confirmedTick;
    m_scriptCameoPresentationEpoch = presentationEpoch;
    m_scriptCameoFlashes.clear();
    m_scriptCameoFlashes.reserve(flashes.size());
    for (const engine::script::ScriptCameoFlashPresentation& flash : flashes) {
        if (flash.commandButton.empty() || flash.flashCount == 0 ||
            flash.framesPerFlash == 0 || flash.stamp.sequence == 0 ||
            flash.stamp.presentationEpoch != presentationEpoch ||
            flash.stamp.confirmedTick > confirmedTick) {
            continue;
        }
        m_scriptCameoFlashes.push_back(flash);
    }
}

void GameWndLayer::renderScriptCameoFlashes(engine::Renderer& renderer) const {
    if (m_scriptCameoFlashes.empty()) return;

    for (const ScriptCommandButtonBinding& binding : m_scriptCommandButtonBindings) {
        if (!binding.widget || !binding.widget->isVisible() ||
            binding.commandButtonName.empty()) {
            continue;
        }

        // CommandButton::setFlashCount replaces an earlier request for the
        // same button. Select the newest source-order stamp instead of
        // compositing multiple highlights when scripts issue a replacement in
        // one confirmed frame.
        const engine::script::ScriptCameoFlashPresentation* active = nullptr;
        for (const engine::script::ScriptCameoFlashPresentation& candidate : m_scriptCameoFlashes) {
            if (candidate.stamp.presentationEpoch != m_scriptCameoPresentationEpoch) continue;
            if (!asciiEqualIgnoreCase(candidate.commandButton, binding.commandButtonName)) continue;
            const uint64_t duration = static_cast<uint64_t>(candidate.flashCount) *
                                      static_cast<uint64_t>(candidate.framesPerFlash);
            if (m_scriptCameoConfirmedTick < candidate.stamp.confirmedTick ||
                m_scriptCameoConfirmedTick - candidate.stamp.confirmedTick >= duration) {
                continue;
            }
            if (!active || candidate.stamp.presentationEpoch > active->stamp.presentationEpoch ||
                (candidate.stamp.presentationEpoch == active->stamp.presentationEpoch &&
                 candidate.stamp.sequence > active->stamp.sequence)) {
                active = &candidate;
            }
        }
        if (!active) continue;

        const uint64_t elapsed = m_scriptCameoConfirmedTick - active->stamp.confirmedTick;
        // An even phase corresponds to the legacy WIN_STATUS_FLASHING half
        // cycle. The compiler already rounds the count to an even number, so
        // this always returns to the normal command-button image.
        if ((elapsed / active->framesPerFlash) % 2u != 0u) continue;

        const float x = static_cast<float>(binding.widget->x());
        const float y = static_cast<float>(binding.widget->y());
        const float width = static_cast<float>(binding.widget->width());
        const float height = static_cast<float>(binding.widget->height());
        if (width <= 0.0f || height <= 0.0f) continue;
        renderer.drawQuad(x, y, width, height, 0x48FFE040u);
        renderer.drawBorder(x, y, width, height, 0xFFFFE070u, 2);
    }
}

void GameWndLayer::hideTooltip() {
    m_tooltipSource = nullptr;
    m_tooltipWidget = nullptr;
    m_tooltipHoverGeneration = 0;
    m_tooltipHoverStartedAt = {};
    m_cursorTooltipText.clear();
    m_commandTooltipVisible = false;
    if (auto* root = m_popupDescription.root()) root->hideRecursive();
}

void GameWndLayer::updateTooltip() {
    if (!m_loaded || !m_visible || m_overlay || m_gameplayHudSuppressed ||
        m_externalGameplayHudSuppressed || !m_gameplayInputEnabled) {
        hideTooltip();
        return;
    }

    const WndRuntime* source = nullptr;
    WndRuntime::HoveredWidgetSnapshot hovered{};
    const auto choose = [&](const WndRuntime& runtime) {
        if (source || !runtime.isLoaded()) return;
        const auto candidate = runtime.hoveredWidget();
        if (candidate.widget && candidate.widget->isEffectivelyVisible()) {
            source = &runtime;
            hovered = candidate;
        }
    };
    // Match the draw/input stacking order: special-power shortcut is the
    // front-most gameplay WND, followed by generals experience, then the bar.
    choose(m_genPowersShortcut);
    choose(m_generalsExp);
    choose(m_controlBar);

    if (!source) {
        hideTooltip();
        if (!m_worldHoverDisplayNameLabel.empty()) {
            // ThingTemplate::DisplayName is a GameText label, not literal UI
            // text. A missing/empty translation stays invisible; in
            // particular, do not expose the authored key or synthesize an
            // object-type placeholder when content deliberately leaves the
            // display name blank.
            m_cursorTooltipText = engine::StringTable::instance().fetch(
                m_worldHoverDisplayNameLabel);
        }
        return;
    }

    if (source != m_tooltipSource || hovered.widget != m_tooltipWidget ||
        hovered.generation != m_tooltipHoverGeneration) {
        hideTooltip();
        m_tooltipSource = source;
        m_tooltipWidget = hovered.widget;
        m_tooltipHoverGeneration = hovered.generation;
        m_tooltipHoverStartedAt = std::chrono::steady_clock::now();
        return;
    }

    const Widget& widget = *hovered.widget;
    const int delay = widget.data().tooltipDelay >= 0
        ? widget.data().tooltipDelay : kDefaultTooltipDelayMs;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_tooltipHoverStartedAt).count();
    if (elapsed < delay) return;

    if (source == &m_controlBar &&
        container::asciiEqualIgnoreCase(widget.shortName(), "PowerWindow")) {
        engine::StringTable& strings = engine::StringTable::instance();
        const ingame::ControlBarPowerMeterState power =
            ingame::ControlBarSchemeRuntime::instance().powerMeterState();
        container::String name = strings.fetch("CONTROLBAR:Power");
        container::String description = strings.fetchFormat(
            "CONTROLBAR:PowerDescription", power.production,
            power.consumption);
        Widget* nameWidget = findInRuntime(
            m_popupDescription, "StaticTextName");
        Widget* descriptionWidget = findInRuntime(
            m_popupDescription, "StaticTextDescription");
        Widget* parent = findInRuntime(m_popupDescription, "Parent");
        if (nameWidget && descriptionWidget && parent &&
            (!name.empty() || !description.empty())) {
            constexpr int lineHeight = 10;
            const auto descriptionLines = wrapTooltipText(
                engine::FontRegistry::instance().getFont("Arial", 8, false),
                description, 214);
            container::String wrappedDescription;
            for (size_t index = 0; index < descriptionLines.size(); ++index) {
                if (index != 0) wrappedDescription.push_back('\n');
                wrappedDescription += descriptionLines[index];
            }
            const int descriptionHeight = std::max(
                lineHeight,
                static_cast<int>(descriptionLines.size()) * lineHeight);
            constexpr int baseY = 401;
            const int parentHeight = std::max(60, 40 + descriptionHeight);
            const int parentY = std::max(0, baseY - parentHeight);
            parent->setBounds(0, parentY, kTooltipWidth, parentHeight);
            nameWidget->setBounds(5, parentY, 214, 20);
            descriptionWidget->setBounds(
                5, parentY + 40, 214, descriptionHeight);
            nameWidget->setText(name);
            descriptionWidget->setText(wrappedDescription);
            if (Widget* costWidget = findInRuntime(
                    m_popupDescription, "StaticTextCost")) {
                costWidget->hide();
                costWidget->setText({});
            }
            m_cursorTooltipText.clear();
            m_commandTooltipVisible = true;
            m_popupDescription.root()->showRecursive();
            return;
        }
    }

    const ScriptCommandButtonBinding* command = nullptr;
    for (const auto& binding : m_scriptCommandButtonBindings) {
        if (binding.widget == &widget) {
            command = &binding;
            break;
        }
    }

    if (command && (source == &m_controlBar || source == &m_genPowersShortcut)) {
        engine::StringTable& strings = engine::StringTable::instance();
        const auto localized = [&strings](container::StringView key,
                                           container::StringView fallback) {
            if (key.empty()) return container::String{fallback};
            const container::String value = strings.fetch(container::String{key});
            return value.empty() ? container::String{fallback} : value;
        };
        const container::String name = localized(
            command->textLabel, command->commandButtonName);
        container::String description = localized(command->descriptionLabel, {});
        if (command->availability.cost > 0) {
            const container::String cost = strings.fetchFormat(
                "TOOLTIP:Cost", static_cast<int>(command->availability.cost));
            Widget* costWidget = findInRuntime(m_popupDescription,
                                               "StaticTextCost");
            if (costWidget) {
                costWidget->setText(cost.empty()
                    ? std::to_string(command->availability.cost) : cost);
                costWidget->show();
            }
        } else if (Widget* costWidget = findInRuntime(m_popupDescription,
                                                      "StaticTextCost")) {
            costWidget->hide();
            costWidget->setText({});
        }

        const container::String status = commandAvailabilityStatus(
            strings, command->availability.reason);
        if (!status.empty()) {
            if (!description.empty()) description += "\n\n";
            description += status;
        }

        Widget* nameWidget = findInRuntime(m_popupDescription,
                                           "StaticTextName");
        Widget* descriptionWidget = findInRuntime(m_popupDescription,
                                                  "StaticTextDescription");
        Widget* parent = findInRuntime(m_popupDescription, "Parent");
        if (nameWidget && descriptionWidget && parent &&
            (!name.empty() || !description.empty())) {
            const int lineHeight = 10;
            const auto descriptionLines = wrapTooltipText(
                engine::FontRegistry::instance().getFont("Arial", 8, false),
                description, 214);
            container::String wrappedDescription;
            for (size_t index = 0; index < descriptionLines.size(); ++index) {
                if (index != 0) wrappedDescription.push_back('\n');
                wrappedDescription += descriptionLines[index];
            }
            const int descriptionHeight = std::max(
                lineHeight, static_cast<int>(descriptionLines.size()) * lineHeight);
            constexpr int baseY = 401;
            const int parentHeight = std::max(60, 40 + descriptionHeight);
            const int parentY = std::max(0, baseY - parentHeight);
            parent->setBounds(0, parentY, kTooltipWidth, parentHeight);
            nameWidget->setBounds(5, parentY, 214, 20);
            if (Widget* costWidget = findInRuntime(m_popupDescription,
                                                   "StaticTextCost")) {
                costWidget->setBounds(5, parentY + 20, 214, 20);
            }
            descriptionWidget->setBounds(5, parentY + 40, 214,
                                         descriptionHeight);
            nameWidget->setText(name);
            descriptionWidget->setText(wrappedDescription);
            m_cursorTooltipText.clear();
            m_commandTooltipVisible = true;
            m_popupDescription.root()->showRecursive();
            if (command->availability.cost <= 0) {
                if (Widget* costWidget = findInRuntime(
                        m_popupDescription, "StaticTextCost")) {
                    costWidget->hide();
                }
            }
            return;
        }
    }

    // Authored TOOLTIPTEXT follows the original cursor-tooltip path. It is
    // deliberately separate from the command-bar popup, which has its own
    // dynamic layout and does not capture world/UI input.
    if (!widget.data().tooltip.empty()) {
        engine::StringTable& strings = engine::StringTable::instance();
        m_cursorTooltipText = strings.fetchOrFallback(
            widget.data().tooltip, widget.data().tooltip);
        m_commandTooltipVisible = false;
        if (auto* root = m_popupDescription.root()) root->hideRecursive();
    }
}

void GameWndLayer::renderCursorTooltip(engine::Renderer& renderer) const {
    if (m_cursorTooltipText.empty() || m_commandTooltipVisible) return;
    engine::Font* font = engine::FontRegistry::instance().getFont(
        "Arial", 8, false);
    const auto lines = wrapTooltipText(
        font, m_cursorTooltipText, 214,
        renderer.getTextLayoutScaleX());
    const int lineHeight = font
        ? static_cast<int>(std::lround(
              static_cast<float>(font->getLineHeight()) *
              renderer.getTextLayoutScaleY()))
        : 10;
    const float width = 224.0f;
    const float height = static_cast<float>(std::max(1, static_cast<int>(lines.size())) *
                                             lineHeight + 10);
    float x = m_cursorTooltipX + 14.0f;
    float y = m_cursorTooltipY + 18.0f;
    if (x + width > 800.0f) x = 800.0f - width;
    if (y + height > 600.0f) y = 600.0f - height;
    x = std::max(0.0f, x);
    y = std::max(0.0f, y);
    renderer.drawQuad(x, y, width, height, 0xE8000000u);
    renderer.drawRect(x, y, width, height, 0xFFFFFFFFu);
    for (size_t index = 0; index < lines.size(); ++index) {
        const float textY = y + 5.0f + static_cast<float>(index * lineHeight);
        if (font) {
            renderer.drawText(font, lines[index], x + 5.0f, textY, COLOR_WHITE);
        } else {
            renderer.drawText(lines[index], x + 5.0f, textY, COLOR_WHITE);
        }
    }
}

void GameWndLayer::toggleControlBarCompact() {
    setControlBarCompact(!m_controlBarCompact);
}

void GameWndLayer::setControlBarCompact(bool compact) {
    if (!m_loaded || compact == m_controlBarCompact) return;

    auto* root = m_controlBar.root();
    auto* master = context(GameWndContext::Master);
    if (!root || !master) return;

    if (m_stageOriginalBounds.empty()) {
        std::function<void(Widget*)> capture = [&](Widget* widget) {
            if (!widget) return;
            if (widget != root) {
                m_stageOriginalBounds[widget] = {widget->x(), widget->y(), widget->width(), widget->height()};
            }
            for (int i = 0; i < widget->getChildCount(); ++i) {
                capture(widget->getChild(i));
            }
        };
        capture(root);
    }

    const auto masterIt = m_stageOriginalBounds.find(master);
    if (masterIt == m_stageOriginalBounds.end()) return;

    const auto& [masterX, masterY, masterW, masterH] = masterIt->second;
    (void)masterX;
    (void)masterW;
    (void)masterH;

    constexpr int kVirtualHeight = 600;
    const int lowY = static_cast<int>(static_cast<float>(kVirtualHeight) * 0.9f);
    const int deltaY = compact ? (lowY - masterY) : 0;

    container::HashMap<Widget*, std::tuple<int, int, int, int>> targets;
    for (const auto& [widget, bounds] : m_stageOriginalBounds) {
        const auto& [x, y, w, h] = bounds;
        targets[widget] = {x, y + deltaY, w, h};
    }
    if (targets.empty()) return;

    m_animationStartBounds.clear();
    m_animationTargetBounds = std::move(targets);
    for (const auto& [widget, _] : m_animationTargetBounds) {
        if (widget) {
            m_animationStartBounds[widget] = {
                widget->authoredX(), widget->authoredY(),
                widget->width(), widget->height()};
        }
    }

    m_controlBarCompact = compact;
    m_controlBarAnimationT = 0.0f;
    updateControlBarStageButtonImages();
}

void GameWndLayer::updateControlBarAnimation() {
    if (m_animationTargetBounds.empty()) return;

    constexpr float kStep = 0.18f;
    m_controlBarAnimationT = std::min(1.0f, m_controlBarAnimationT + kStep);
    const float t = m_controlBarAnimationT;
    const float eased = t * t * (3.0f - 2.0f * t);

    for (const auto& [widget, target] : m_animationTargetBounds) {
        if (!widget) continue;

        auto startIt = m_animationStartBounds.find(widget);
        const auto& start = (startIt != m_animationStartBounds.end()) ? startIt->second : target;
        const auto& [sx, sy, sw, sh] = start;
        const auto& [tx, ty, tw, th] = target;

        auto mix = [eased](int a, int b) {
            return static_cast<int>(static_cast<float>(a) + (static_cast<float>(b - a) * eased) + 0.5f);
        };
        widget->setBounds(mix(sx, tx), mix(sy, ty), mix(sw, tw), mix(sh, th));
    }

    if (auto* master = context(GameWndContext::Master)) {
        auto originalIt = m_stageOriginalBounds.find(master);
        if (originalIt != m_stageOriginalBounds.end()) {
            const auto& [x, y, w, h] = originalIt->second;
            (void)x;
            (void)w;
            (void)h;
            ingame::ControlBarSchemeRuntime::instance().setDrawOffsetY(master->y() - y);
        }
    }

    if (m_controlBarAnimationT >= 1.0f) {
        m_animationStartBounds.clear();
        m_animationTargetBounds.clear();
    }
}

void GameWndLayer::updateControlBarStageButtonImages() {
    auto* button = findInRuntime(m_controlBar, "ButtonLarge");
    if (!button) return;

    auto& scheme = ingame::ControlBarSchemeRuntime::instance();
    auto firstImage = [&](std::initializer_list<container::StringView> keys) {
        for (auto key : keys) {
            container::String image = scheme.namedImage(key);
            if (!image.empty()) return image;
        }
        return container::String{};
    };

    const container::String enabled = m_controlBarCompact
        ? firstImage({"ToggleButtonUpOn", "MinMaxButtonEnable", "ToggleButtonDownOn"})
        : firstImage({"ToggleButtonDownOn", "MinMaxButtonEnable", "ToggleButtonUpOn"});
    const container::String hilite = m_controlBarCompact
        ? firstImage({"ToggleButtonUpIn", "MinMaxButtonHightlited", "ToggleButtonDownIn"})
        : firstImage({"ToggleButtonDownIn", "MinMaxButtonHightlited", "ToggleButtonUpIn"});

    if (!enabled.empty() || !hilite.empty()) {
        button->setDrawImage(0, enabled, hilite);
    }
}

void GameWndLayer::bindButton(container::StringView name, ButtonHandler handler) {
    if (!handler) return;
    m_buttonHandlers.emplace_back(stripWndPrefix(name), std::move(handler));
    bindStoredHandlers();
}

void GameWndLayer::setPushButtonPressHandler(PushButtonPressHandler handler) {
    m_pushButtonPressHandler = std::move(handler);
    const auto apply = [this](WndRuntime& runtime) {
        runtime.setPushButtonPressHandler(m_pushButtonPressHandler);
    };
    apply(m_controlBar);
    apply(m_generalsExp);
    apply(m_popupDescription);
    apply(m_genPowersShortcut);
    for (auto& [path, runtime] : m_overlayCache) {
        static_cast<void>(path);
        if (runtime) apply(*runtime);
    }
}

void GameWndLayer::setDisabledPushButtonPressHandler(
    PushButtonPressHandler handler) {
    m_disabledPushButtonPressHandler = std::move(handler);
    const auto apply = [this](WndRuntime& runtime) {
        runtime.setDisabledPushButtonPressHandler(
            m_disabledPushButtonPressHandler);
    };
    apply(m_controlBar);
    apply(m_generalsExp);
    apply(m_popupDescription);
    apply(m_genPowersShortcut);
    for (auto& [path, runtime] : m_overlayCache) {
        static_cast<void>(path);
        if (runtime) apply(*runtime);
    }
}

bool GameWndLayer::loadRuntime(WndRuntime& runtime, RuntimePaths paths, bool required) {
    auto& vfs = io::VFS::instance();
    container::Vector<container::String> candidates;
    addWndPathCandidate(candidates, paths.primary);
    addWndPathCandidate(candidates, paths.fallback);

    for (const auto& path : candidates) {
        container::String content = vfs.readAll(path);
        if (content.empty()) continue;

        if (runtime.loadFromString(path, content)) {
            runtime.setPushButtonPressHandler(m_pushButtonPressHandler);
            runtime.setDisabledPushButtonPressHandler(
                m_disabledPushButtonPressHandler);
            return true;
        }
    }

    if (required) {
        TD_LOG_ERROR("[GameWndLayer] Failed to load required WND '{}' fallback '{}'",
                     paths.primary, paths.fallback);
    } else {
        TD_LOG_WARN("[GameWndLayer] Optional WND not loaded '{}' fallback '{}'",
                    paths.primary, paths.fallback);
    }
    return false;
}

void GameWndLayer::cacheContextParents() {
    m_contextParents.fill(nullptr);

    for (const auto& [contextId, name] : kContextNames) {
        Widget* parent = nullptr;
        if (contextId == GameWndContext::PurchaseScience) {
            parent = findInRuntime(m_generalsExp, name);
        } else {
            parent = findInRuntime(m_controlBar, name);
        }

        m_contextParents[static_cast<size_t>(contextId)] = parent;
        if (!parent) {
            TD_LOG_WARN("[GameWndLayer] Context parent '{}' not found", name);
        }
    }
}

void GameWndLayer::applyControlBarScheme() {
    auto& scheme = ingame::ControlBarSchemeRuntime::instance();

    auto firstImage = [&](std::initializer_list<container::StringView> keys) {
        for (auto key : keys) {
            container::String image = scheme.namedImage(key);
            if (!image.empty()) return image;
        }
        return container::String{};
    };

    auto applyRect = [&](Widget* widget, container::StringView prefix) {
        if (!widget) return;
        const auto ul = scheme.namedPoint(container::String{prefix} + "UL");
        const auto lr = scheme.namedPoint(container::String{prefix} + "LR");
        if (!ul || !lr) return;

        const int w = lr->first - ul->first;
        const int h = lr->second - ul->second;
        if (w > 0 && h > 0) {
            widget->setBounds(ul->first, ul->second, w, h);
        }
    };

    auto applyButton = [&](container::StringView widgetName,
                           container::StringView imagePrefix,
                           container::StringView rectPrefix) {
        auto* widget = findInRuntime(m_controlBar, widgetName);
        if (!widget) return;

        const container::String enabled = scheme.namedImage(container::String{imagePrefix} + "Enable");
        const container::String hilite = scheme.namedImage(container::String{imagePrefix} + "Hightlited");
        const container::String disabled = scheme.namedImage(container::String{imagePrefix} + "Disabled");
        if (!enabled.empty() || !hilite.empty() || !disabled.empty()) {
            widget->setDrawImage(0, enabled, hilite, disabled);
        }
        applyRect(widget, rectPrefix);
    };

    applyButton("PopupCommunicator", "BuddyButton", "Chat");
    applyButton("ButtonIdleWorker", "IdleWorkerButton", "Worker");
    applyButton("ButtonOptions", "OptionsButton", "Options");
    applyButton("ButtonPlaceBeacon", "BeaconButton", "Beacon");

    if (auto* button = findInRuntime(m_controlBar, "ButtonGeneral")) {
        const container::String enabled = firstImage({"GeneralButtonEnable", "GenBarButtonOn"});
        const container::String hilite = firstImage({"GeneralButtonHightlited", "GenBarButtonIn"});
        const container::String disabled = scheme.namedImage("GeneralButtonDisabled");
        if (!enabled.empty() || !hilite.empty() || !disabled.empty()) {
            button->setDrawImage(0, enabled, hilite, disabled);
        }
        applyRect(button, "General");
    }

    if (auto* button = findInRuntime(m_controlBar, "WinUAttack")) {
        const container::String enabled = firstImage({"UAttackButtonEnable", "UAttackButtonHightlited", "UAttackButtonPushed"});
        const container::String hilite = firstImage({"UAttackButtonHightlited", "UAttackButtonPushed", "UAttackButtonEnable"});
        if (!enabled.empty() || !hilite.empty()) {
            button->setDrawImage(0, enabled, hilite);
        }
        applyRect(button, "UAttack");
    }

    if (auto* button = findInRuntime(m_controlBar, "ButtonLarge")) {
        const container::String enabled = firstImage({"ToggleButtonDownOn", "MinMaxButtonEnable", "ToggleButtonUpOn"});
        const container::String hilite = firstImage({"ToggleButtonDownIn", "MinMaxButtonHightlited", "ToggleButtonUpIn"});
        if (!enabled.empty() || !hilite.empty()) {
            button->setDrawImage(0, enabled, hilite);
        }
        applyRect(button, "MinMax");
    }

    if (auto* exp = findInRuntime(m_controlBar, "ExpBarForeground")) {
        exp->setDrawImage(0, scheme.expBarForegroundImage());
    }
    if (auto* genExpParent = findInRuntime(m_generalsExp, "GenExpParent")) {
        genExpParent->setDrawImage(0, scheme.powerPurchaseImage());
    }
}

void GameWndLayer::bindStoredHandlers() {
    for (const auto& [name, handler] : m_buttonHandlers) {
        Widget* button = m_overlay ? findOverlay(name) : find(name);
        if (button) {
            button->onClick = handler;
        }
    }
}

Widget* GameWndLayer::findInRuntime(WndRuntime& runtime, container::StringView name) {
    if (!runtime.isLoaded() || !runtime.root()) return nullptr;
    return runtime.findByName(stripWndPrefix(name));
}

const Widget* GameWndLayer::findInRuntime(const WndRuntime& runtime, container::StringView name) const {
    if (!runtime.isLoaded() || !runtime.root()) return nullptr;
    return runtime.findByName(stripWndPrefix(name));
}

} // namespace gui
