#pragma once

#include "core/container/container_types.h"

#include "WndParser.h"
#include <cstdint>
#include <functional>
union SDL_Event;

namespace engine {
class Renderer;
class TextureManager;
}

namespace gui {

enum class WidgetPointerButton : uint8_t {
    None,
    Left,
    Middle,
    Right,
};

struct WidgetClickContext final {
    WidgetPointerButton button = WidgetPointerButton::None;
    uint32_t modifiers = 0;
};

// Widget: runtime window object. Owns a copy of WndParser::WindowDef data
// and provides show/hide, hit-testing, callbacks, and rendering.
// Replaces direct manipulation of WindowDef status flags.
class Widget {
public:
    explicit Widget(const WndParser::WindowDef& data);
    ~Widget() = default;

    // Non-copyable
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // ── Tree ──────────────────────────────────────────────────────────
    void addChild(container::UniquePtr<Widget> child);
    Widget* getParent() const { return m_parent; }
    Widget* getChild(int index) const;
    int getChildCount() const { return static_cast<int>(m_children.size()); }

    // ── Visibility ────────────────────────────────────────────────────
    void show() { m_visible = true; }
    void hide() { m_visible = false; }
    void setVisible(bool v) { m_visible = v; }
    bool isVisible() const { return m_visible; }
    bool isEffectivelyVisible() const noexcept;

    // Recursive show/hide (for container widgets)
    void showRecursive();
    void hideRecursive();
    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isEnabled() const noexcept;
    void setActive(bool active) noexcept;
    [[nodiscard]] bool isActive() const noexcept;
    // Transient pointer state; unlike ACTIVE this is not a gameplay toggle.
    void setPressed(bool pressed) noexcept { m_pressed = pressed; }
    [[nodiscard]] bool isPressed() const noexcept { return m_pressed; }
    void setUseOverlayStates(bool enabled) noexcept;
    [[nodiscard]] bool usesOverlayStates() const noexcept;

    // ── Input ─────────────────────────────────────────────────────────
    // Hit-test: returns this widget if (mx,my) is inside bounds and interactive.
    // Pointer hover deliberately includes disabled gadgets so their authored
    // tooltip can explain why they cannot be activated; command dispatch keeps
    // the default enabled-only behavior.
    Widget* hitTest(float mx, float my, engine::TextureManager* texMgr = nullptr,
                    bool includeDisabled = false);

    // Original GameWindowManager window lookup is deliberately independent
    // from gadget callbacks and texture alpha.  It first finds the deepest
    // enabled, visible rectangular window, then selection/picking inspects
    // WIN_STATUS_SEE_THRU on that window and every parent.  Keep that query
    // separate from hitTest(), which is used to dispatch concrete controls.
    [[nodiscard]] const Widget* windowAt(float mx, float my,
                                         bool ignoreEnabled = false) const noexcept;

    // Hover state (set by screen on mouse motion)
    void setHovered(bool v) { m_hovered = v; }
    bool isHovered() const { return m_hovered; }

    // Focus state
    void setFocused(bool v) { m_focused = v; }
    bool isFocused() const { return m_focused; }

    // Keyboard input for text entry
    void handleKeyInput(uint32_t keyCode, uint32_t keyMod, const container::String& text);

    // ── Callbacks ─────────────────────────────────────────────────────
    std::function<void(Widget&)> onClick;
    // Pointer-aware controls use this narrow value callback instead of
    // consulting SDL/global input state after dispatch. Legacy WND callbacks
    // remain valid through onClick and are used as the fallback.
    std::function<void(Widget&, const WidgetClickContext&)> onPointerClick;
    std::function<void(Widget&)> onHover;
    [[nodiscard]] bool hasClickHandler() const noexcept {
        return static_cast<bool>(onPointerClick) || static_cast<bool>(onClick);
    }
    void invokeClick(const WidgetClickContext& context = {}) {
        if (onPointerClick) onPointerClick(*this, context);
        else if (onClick) onClick(*this);
    }
    // RefCode's GadgetButtonSetAltSound is a runtime ControlBar property,
    // not a WND field. It selects only local press feedback and deliberately
    // has no bearing on widget activation, commands, or serialized state.
    void setPushButtonPressSound(container::String eventName) {
        m_pushButtonPressSound = std::move(eventName);
    }
    [[nodiscard]] const container::String& pushButtonPressSound() const
        noexcept { return m_pushButtonPressSound; }

    // ── Rendering ─────────────────────────────────────────────────────
    void render(engine::Renderer& renderer, engine::TextureManager& texMgr, int depth = 0);
    void renderSkeleton(engine::Renderer& renderer, int depth = 0) const;

    // ── Data access ───────────────────────────────────────────────────
    const WndParser::WindowDef& data() const { return m_data; }
    const container::String& type() const { return m_data.type; }
    const container::String& name() const { return m_data.name; }
    int x() const;
    int y() const;
    int authoredX() const { return m_data.x; }
    int authoredY() const { return m_data.y; }
    int width() const { return m_data.w; }
    int height() const { return m_data.h; }
    const container::String& text() const { return m_data.text; }
    const container::Vector<container::UniquePtr<Widget>>& children() const {
        return m_children;
    }
    void setText(container::String text) { m_data.text = std::move(text); }
    void setTooltip(container::String tooltip, int delay = 800) {
        m_data.tooltip = std::move(tooltip);
        m_data.tooltipDelay = delay;
    }
    void setBounds(int x, int y, int w, int h);
    void setDrawImage(int slot, const container::String& enabled, const container::String& hilite = {}, const container::String& disabled = {});
    void clearDrawImage(int slot) noexcept;
    // 0 = fully covered/not ready, 1 = ready/no cover. Command-bar callers
    // use this for the original inverse build/recharge clock without changing
    // the authored button image or enabled state.
    void setCooldownClockProgress(float readyFraction) noexcept {
        m_cooldownReadyFraction = std::clamp(readyFraction, 0.0f, 1.0f);
    }
    // Runtime equivalent of RefCode GadgetButtonSetBorder. Command bars own
    // this transient state; authored WND draw data remains untouched.
    void setCommandBorder(uint32_t color) noexcept {
        m_drawCommandBorder = true;
        m_commandBorderColor = color;
    }
    void clearCommandBorder() noexcept {
        m_drawCommandBorder = false;
        m_commandBorderColor = 0x00ffffffu;
    }
    [[nodiscard]] bool drawsCommandBorder() const noexcept {
        return m_drawCommandBorder;
    }
    [[nodiscard]] uint32_t commandBorderColor() const noexcept {
        return m_commandBorderColor;
    }
    void setCommandButtonChrome(bool enabled) noexcept {
        m_drawCommandButtonChrome = enabled;
    }
    [[nodiscard]] bool drawsCommandButtonChrome() const noexcept {
        return m_drawCommandButtonChrome;
    }

    // Short name (strip "file.wnd:" prefix and quotes)
    container::String shortName() const;

    // ── Find by name (recursive) ──────────────────────────────────────
    Widget* findByName(const container::String& name);
    const Widget* findByName(const container::String& name) const;

    // ── Slider state ──────────────────────────────────────────────────
    float getSliderValue() const { return m_sliderValue; }
    void setSliderValue(float v) { m_sliderValue = std::clamp(v, 0.0f, 1.0f); }
    bool isSliderDragging() const { return m_sliderDragging; }
    void setSliderDragging(bool v) { m_sliderDragging = v; }

    // ── ListBox state ─────────────────────────────────────────────────
    int getSelectedIndex() const { return m_selectedIndex; }
    void setSelectedIndex(int i) { m_selectedIndex = i; }
    int getScrollOffset() const { return m_scrollOffset; }
    void setScrollOffset(int s) { m_scrollOffset = s; }
    int getItemCount() const { return static_cast<int>(m_items.size()); }
    void setItemCount(int c) { m_itemCount = c; }
    void setItems(const container::Vector<container::String>& items) { m_items = items; m_itemCount = static_cast<int>(items.size()); }
    const container::Vector<container::String>& getItems() const { return m_items; }
    const container::String& getItem(int i) const { static const container::String empty; return (i >= 0 && i < static_cast<int>(m_items.size())) ? m_items[i] : empty; }
    void addItem(const container::String& item) { m_items.push_back(item); m_itemCount = static_cast<int>(m_items.size()); }
    void clearItems() { m_items.clear(); m_itemCount = 0; }

    // ── ComboBox state ────────────────────────────────────────────────
    bool isDropdownOpen() const { return m_dropdownOpen; }
    void setDropdownOpen(bool v) { m_dropdownOpen = v; }
    int getHighlightedItem() const { return m_highlightedItem; }
    void setHighlightedItem(int i) { m_highlightedItem = i; }
    int getDropdownLineHeight() const;

    // ── TextEntry state ───────────────────────────────────────────────
    const container::String& getEditText() const { return m_editText; }
    // Re-clamp the caret: replacing the text with a shorter string while the
    // field is focused otherwise leaves m_cursorPos past the end, and the next
    // Backspace/insert calls erase()/insert() out of range and throws
    // std::out_of_range straight out of the SDL event loop.
    void setEditText(const container::String& t) {
        m_editText = t;
        m_cursorPos = std::clamp(m_cursorPos, 0, static_cast<int>(m_editText.size()));
    }
    int getCursorPosition() const { return m_cursorPos; }
    void setCursorPosition(int p) { m_cursorPos = std::clamp(p, 0, static_cast<int>(m_editText.size())); }

    // ── Checkbox state ────────────────────────────────────────────────
    bool isChecked() const { return m_checked; }
    void setChecked(bool v) { m_checked = v; }
    void toggleChecked() { m_checked = !m_checked; }

private:
    WndParser::WindowDef m_data;
    Widget* m_parent = nullptr;
    container::Vector<container::UniquePtr<Widget>> m_children;
    bool m_visible = true;
    bool m_hovered = false;
    bool m_focused = false;
    bool m_pressed = false;

    // Slider state
    float m_sliderValue = 0.0f;
    bool m_sliderDragging = false;

    // ListBox state
    int m_selectedIndex = -1;
    int m_scrollOffset = 0;
    int m_itemCount = 0;
    container::Vector<container::String> m_items;

    // ComboBox state
    bool m_dropdownOpen = false;
    int m_highlightedItem = -1;

    // TextEntry state
    container::String m_editText;
    int m_cursorPos = 0;

    // Checkbox state
    bool m_checked = false;
    float m_cooldownReadyFraction = 1.0f;
    bool m_drawCommandButtonChrome = false;
    bool m_drawCommandBorder = false;
    uint32_t m_commandBorderColor = 0x00ffffffu;
    container::String m_pushButtonPressSound;

    void renderSelf(engine::Renderer& renderer, engine::TextureManager& texMgr);
    void renderChildren(engine::Renderer& renderer, engine::TextureManager& texMgr, int depth);
};

// ── Tree builder ──────────────────────────────────────────────────────────
// Converts WndParser output into a Widget tree.
container::UniquePtr<Widget> buildWidgetTree(const container::Vector<WndParser::WindowDef>& windows);

} // namespace gui
