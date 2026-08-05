#pragma once

#include "core/container/container_types.h"
#include "core/container/hash_containers.h"

#include "LayoutCallbackRegistry.h"
#include "WndParser.h"
#include "widget/Widget.h"
#include <functional>
union SDL_Event;

namespace engine {
class Renderer;
class TextureManager;
}

namespace gui {

// Runtime wrapper for a WND layout. It owns the parsed widget tree and lifecycle
// callbacks, but is not tied to Shell's screen stack.
class WndRuntime {
public:
    using PushButtonPressHandler = std::function<void(const Widget&)>;
    enum class WorldInputDisposition : uint8_t {
        NoWindow,
        SeeThrough,
        Blocked,
    };

    struct HoveredWidgetSnapshot final {
        const Widget* widget = nullptr;
        uint64_t generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept {
            return widget != nullptr;
        }
    };

    WndRuntime() = default;
    ~WndRuntime() = default;

    bool loadFromVfs(const container::String& wndPath, ScreenGroup* callbackOwner = nullptr);
    bool loadFromString(const container::String& wndPath, const container::String& content,
                        ScreenGroup* callbackOwner = nullptr);

    void update(ScreenGroup* callbackOwner = nullptr);
    // Retain the parsed tree while honoring layout lifecycle callbacks. Used
    // only by the in-match overlay cache between close/reopen operations.
    void suspend(ScreenGroup* callbackOwner = nullptr);
    void resume(ScreenGroup* callbackOwner = nullptr);
    void shutdown(ScreenGroup* callbackOwner = nullptr, bool immediate = true);
    void render(engine::Renderer& renderer, engine::TextureManager& texMgr);
    static void toggleSkeletonMode() { s_renderSkeleton = !s_renderSkeleton; }
    static void setSkeletonMode(bool enabled) { s_renderSkeleton = enabled; }
    static bool isSkeletonMode() { return s_renderSkeleton; }
    bool handleEvent(const SDL_Event& event, engine::TextureManager* texMgr = nullptr);
    // Original HotKeyManager activates the currently materialized enabled
    // gadget whose localized label contains '&X'.
    bool activateLocalizedHotkey(uint32_t scancode, uint32_t modifiers);

    bool isLoaded() const { return m_loaded; }
    const container::String& wndPath() const { return m_wndPath; }

    Widget* root() { return m_root.get(); }
    const Widget* root() const { return m_root.get(); }
    // Stable for the lifetime of the current parsed tree. Lookups cache both
    // hits and misses and are discarded whenever the runtime is reloaded.
    Widget* findByName(container::StringView name);
    const Widget* findByName(container::StringView name) const;
    [[nodiscard]] uint64_t generation() const noexcept { return m_generation; }
    [[nodiscard]] bool hasVisibleContent() const noexcept;
    [[nodiscard]] bool hasTextInputFocus() const noexcept;
    // Returns whether the front-most authored top-level window at a virtual
    // 800x600 point blocks world picking.  SeeThrough is distinct from
    // NoWindow so a front see-through WND remains authoritative over lower
    // WND layers, matching GameWindowManager's first-top-level lookup.
    [[nodiscard]] WorldInputDisposition worldInputDispositionAt(
        float virtualX, float virtualY) const noexcept;
    // The pointer is valid only while this runtime keeps the current parsed
    // tree.  Consumers use generation() together with this snapshot and must
    // not retain it across reload/shutdown.
    [[nodiscard]] HoveredWidgetSnapshot hoveredWidget() const noexcept {
        return {m_hoveredWidget, m_hoverGeneration};
    }

    const WndParser::LayoutDef& layout() const { return m_layout; }

    void setLayoutCallbacks(LayoutInitFunc init, LayoutUpdateFunc update,
                            LayoutShutdownFunc shutdown);
    void setBackHandler(std::function<void()> handler) { m_backHandler = std::move(handler); }
    // Generic WND observes the same input point as RefCode's
    // GadgetPushButtonInput.  Consumers may attach local presentation effects
    // (for example the authored GUI click sound) without coupling WndRuntime
    // to a device, audio bus, GameSession, or gameplay command.
    void setPushButtonPressHandler(PushButtonPressHandler handler) {
        m_pushButtonPressHandler = std::move(handler);
    }
    // RefCode plays GUIClickDisabled only when the matching release remains
    // over the disabled push button.  This separate observer preserves that
    // release-time policy without granting disabled widgets an activation.
    void setDisabledPushButtonPressHandler(PushButtonPressHandler handler) {
        m_disabledPushButtonPressHandler = std::move(handler);
    }
    void clearInteractionState();
    bool focusWidget(Widget* widget) {
        if (!m_loaded || !widget) return false;
        setFocus(widget);
        return true;
    }

private:
    container::String m_wndPath;
    WndParser::LayoutDef m_layout;
    container::UniquePtr<Widget> m_root;
    bool m_loaded = false;
    bool m_active = false;

    LayoutInitFunc m_wndInit;
    LayoutUpdateFunc m_wndUpdate;
    LayoutShutdownFunc m_wndShutdown;
    std::function<void()> m_backHandler;
    PushButtonPressHandler m_pushButtonPressHandler;
    PushButtonPressHandler m_disabledPushButtonPressHandler;
    Widget* m_hoveredWidget = nullptr;
    uint64_t m_hoverGeneration = 0;
    Widget* m_focusedWidget = nullptr;
    // Ordinary push buttons grab the pointer on mouse-down and commit only
    // when the matching mouse-up still lands on the same enabled widget.
    // Keeping the button separately lets a drag-out cancel the widget while
    // the runtime still owns the eventual release event.
    Widget* m_pressedWidget = nullptr;
    const Widget* m_disabledPressedWidget = nullptr;
    uint8_t m_pressedMouseButton = 0;
    uint32_t m_pressedModifiers = 0;
    mutable container::HashMap<container::String, Widget*> m_widgetByName;
    uint64_t m_generation = 0;
    static inline bool s_renderSkeleton = false;

    void resolveCallbacks();
    void setFocus(Widget* widget);
    void clearFocus();
    void cycleFocus(bool reverse);
};

} // namespace gui
