#include "core/container/container_types.h"
#include "WndRuntime.h"

#include "Renderer.h"
#include "UiDrawList.h"
#include "TextureManager.h"
#include "VFS.h"
#include "WinInstanceData.h"
#include "LayoutCallbacks.h"
#include "debug/debug.h"
#include "../../../core/constants/Widget.h"
#include <SDL3/SDL.h>
#include <algorithm>
namespace gui {

namespace {

bool isEmptyCallbackName(const container::String& name) {
    return name.empty() || name == "[None]" || name == "[NONE]" || name == "[none]";
}

bool isOrdinaryPushButton(const Widget& widget) noexcept {
    const auto& data = widget.data();
    return (data.style & GWS_PUSH_BUTTON) != 0 &&
           (data.status & (WIN_STATUS_ON_MOUSE_DOWN | WIN_STATUS_CHECK_LIKE)) == 0;
}

WidgetPointerButton pointerButton(uint8_t button) noexcept {
    switch (button) {
    case SDL_BUTTON_LEFT: return WidgetPointerButton::Left;
    case SDL_BUTTON_MIDDLE: return WidgetPointerButton::Middle;
    case SDL_BUTTON_RIGHT: return WidgetPointerButton::Right;
    default: return WidgetPointerButton::None;
    }
}

char localizedHotkey(const Widget& widget) noexcept {
    const container::String& text = widget.text();
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

Widget* findLocalizedHotkey(Widget* widget, char key) {
    if (!widget || !widget->isEffectivelyVisible() ||
        !widget->isEnabled() || !widget->isActive()) {
        return nullptr;
    }
    if ((widget->data().style & GWS_PUSH_BUTTON) != 0 &&
        widget->hasClickHandler() && localizedHotkey(*widget) == key) {
        return widget;
    }
    for (const auto& child : widget->children()) {
        if (Widget* found = findLocalizedHotkey(child.get(), key)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

bool WndRuntime::loadFromVfs(const container::String& wndPath, ScreenGroup* callbackOwner) {
    auto& vfs = io::VFS::instance();
    container::String wndContent = vfs.readAll(wndPath);
    if (wndContent.empty()) {
        TD_LOG_ERROR("[WndRuntime] Failed to read WND: {}", wndPath);
        return false;
    }
    return loadFromString(wndPath, wndContent, callbackOwner);
}

bool WndRuntime::loadFromString(const container::String& wndPath, const container::String& content,
                                ScreenGroup* callbackOwner) {
    shutdown(callbackOwner, true);

    WndParser parser;
    if (!parser.parseFromString(content)) {
        TD_LOG_ERROR("[WndRuntime] Failed to parse WND '{}' at pos={}", wndPath, parser.getPos());
        return false;
    }

    m_wndPath = wndPath;
    m_layout = parser.getLayout();
    m_root = buildWidgetTree(m_layout.windows);
    m_widgetByName.clear();
    resolveCallbacks();
    m_loaded = true;
    m_active = true;
    ++m_generation;
    if (m_generation == 0) m_generation = 1;

    if (m_wndInit) {
        m_wndInit(callbackOwner);
    }

    TD_LOG_INFO("[WndRuntime] Loaded '{}': {} windows", wndPath, m_layout.windows.size());
    return true;
}

void WndRuntime::update(ScreenGroup* callbackOwner) {
    if (m_loaded && m_active && m_wndUpdate) {
        m_wndUpdate(callbackOwner);
    }
}

void WndRuntime::suspend(ScreenGroup* callbackOwner) {
    if (!m_loaded || !m_active) return;
    if (m_wndShutdown) m_wndShutdown(callbackOwner, false);
    clearInteractionState();
    m_active = false;
}

void WndRuntime::resume(ScreenGroup* callbackOwner) {
    if (!m_loaded || m_active) return;
    m_active = true;
    if (m_wndInit) m_wndInit(callbackOwner);
}

void WndRuntime::shutdown(ScreenGroup* callbackOwner, bool immediate) {
    if (!m_loaded) return;

    if (m_active && m_wndShutdown) {
        m_wndShutdown(callbackOwner, immediate);
    }

    // Hover/focus pointers refer into m_root; clear their widget flags before
    // destroying the retained tree.
    clearInteractionState();
    m_root.reset();
    m_widgetByName.clear();
    m_layout = WndParser::LayoutDef{};
    m_wndInit = nullptr;
    m_wndUpdate = nullptr;
    m_wndShutdown = nullptr;
    m_backHandler = nullptr;
    m_active = false;
    m_loaded = false;
    ++m_generation;
    if (m_generation == 0) m_generation = 1;
}

Widget* WndRuntime::findByName(container::StringView name) {
    if (!m_loaded || !m_root) return nullptr;
    container::String key{name};
    if (const auto found = m_widgetByName.find(key);
        found != m_widgetByName.end()) {
        return found->second;
    }
    Widget* widget = m_root->findByName(key);
    m_widgetByName.emplace(std::move(key), widget);
    return widget;
}

const Widget* WndRuntime::findByName(container::StringView name) const {
    return const_cast<WndRuntime*>(this)->findByName(name);
}

bool WndRuntime::hasVisibleContent() const noexcept {
    if (!m_loaded || !m_active || !m_root || !m_root->isVisible()) return false;
    if (m_root->getChildCount() == 0) return true;
    for (int index = 0; index < m_root->getChildCount(); ++index) {
        const Widget* child = m_root->getChild(index);
        if (child && child->isVisible()) return true;
    }
    return false;
}

void WndRuntime::render(engine::Renderer& renderer, engine::TextureManager& texMgr) {
    if (!m_loaded || !m_active || !m_root) return;

    engine::UiDrawList drawList;
    engine::UiDrawListRenderer drawListRenderer(drawList, renderer);

    for (int i = 0; i < m_root->getChildCount(); ++i) {
        if (auto* child = m_root->getChild(i)) {
            if (s_renderSkeleton) {
                child->renderSkeleton(drawListRenderer, 0);
            } else {
                child->render(drawListRenderer, texMgr, 0);
            }
        }
    }

    engine::UiRenderer{}.submit(renderer, drawList);
}

void WndRuntime::setLayoutCallbacks(LayoutInitFunc init, LayoutUpdateFunc update,
                                    LayoutShutdownFunc shutdown) {
    m_wndInit = std::move(init);
    m_wndUpdate = std::move(update);
    m_wndShutdown = std::move(shutdown);
}

bool WndRuntime::handleEvent(const SDL_Event& event, engine::TextureManager* texMgr) {
    if (!m_loaded || !m_active || !m_root) return false;

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE) {
        if (m_backHandler) {
            m_backHandler();
            return true;
        }
        return false;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_TAB) {
        cycleFocus((event.key.mod & SDL_KMOD_SHIFT) != 0);
        return true;
    }

    // A context can be hidden by its parent without destroying the parsed
    // tree.  The original window manager revokes keyboard focus in that
    // transition; do the same before routing a key so a hidden entry/button
    // cannot consume gameplay shortcuts or text input.
    if (m_focusedWidget &&
        (!m_focusedWidget->isEffectivelyVisible() ||
         !m_focusedWidget->isEnabled() || !m_focusedWidget->isActive())) {
        clearFocus();
    }

    if (event.type == SDL_EVENT_KEY_DOWN && m_focusedWidget) {
        if ((event.key.scancode == SDL_SCANCODE_RETURN ||
             event.key.scancode == SDL_SCANCODE_KP_ENTER) &&
            (m_focusedWidget->data().style & GWS_ENTRY_FIELD) &&
            m_focusedWidget->hasClickHandler()) {
            m_focusedWidget->invokeClick({
                .modifiers = static_cast<uint32_t>(event.key.mod),
            });
            return true;
        }
        m_focusedWidget->handleKeyInput(event.key.scancode, event.key.mod, "");
        return true;
    }

    if (event.type == SDL_EVENT_TEXT_INPUT && m_focusedWidget) {
        m_focusedWidget->handleKeyInput(0, 0, event.text.text);
        return true;
    }

    if (event.type == SDL_EVENT_TEXT_EDITING) {
        return m_focusedWidget != nullptr;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        int winW = 0, winH = 0;
        engine::Renderer::instance().getWindowSize(winW, winH);
        if (winW <= 0 || winH <= 0) return false;
        const engine::Renderer& renderer = engine::Renderer::instance();
        const float mx = renderer.windowToUiX(event.button.x);
        const float my = renderer.windowToUiY(event.button.y);

        std::function<bool(Widget*)> checkComboBoxDropdown = [&](Widget* widget) -> bool {
            if (!widget || !widget->isVisible()) return false;
            if ((widget->data().style & GWS_COMBO_BOX) && widget->isEnabled()) {
                if (widget->isDropdownOpen() && !widget->getItems().empty()) {
                    float dx = static_cast<float>(widget->x());
                    float dy = static_cast<float>(widget->y()) + static_cast<float>(widget->height());
                    float dw = static_cast<float>(widget->width());
                    int lineH = widget->getDropdownLineHeight();
                    int maxVisible = std::min(static_cast<int>(widget->getItems().size()), WIDGET_COMBO_MAX_VISIBLE);
                    float listH = static_cast<float>(maxVisible * lineH);
                    if (mx >= dx && mx <= dx + dw && my >= dy && my <= dy + listH) {
                        int visIdx = static_cast<int>(my - dy) / lineH;
                        int itemIdx = visIdx + widget->getScrollOffset();
                        if (visIdx >= 0 && visIdx < maxVisible &&
                            itemIdx < static_cast<int>(widget->getItems().size())) {
                            widget->setSelectedIndex(itemIdx);
                            widget->setEditText(widget->getItem(itemIdx));
                            widget->setDropdownOpen(false);
                            if (widget->hasClickHandler()) {
                                widget->invokeClick({
                                    .button = pointerButton(event.button.button),
                                    .modifiers = static_cast<uint32_t>(
                                        SDL_GetModState()),
                                });
                            }
                            return true;
                        }
                    }
                    widget->setDropdownOpen(false);
                    return true;
                }
            }
            for (int i = 0; i < widget->getChildCount(); ++i) {
                if (checkComboBoxDropdown(widget->getChild(i))) return true;
            }
            return false;
        };
        if (checkComboBoxDropdown(m_root.get())) return true;

        if (auto* hit = m_root->hitTest(mx, my, texMgr)) {
            TD_LOG_INFO("[WndRuntime] Hit: {} type={}", hit->shortName(), hit->type());

            if (hit->data().style & GWS_ENTRY_FIELD) {
                setFocus(hit);
                float relX = mx - static_cast<float>(hit->x());
                int approxChar = static_cast<int>(relX / static_cast<float>(WIDGET_CHAR_WIDTH_ESTIMATE));
                hit->setCursorPosition(std::clamp(approxChar, 0, static_cast<int>(hit->getEditText().size())));
            } else {
                clearFocus();
            }

            if (hit->data().style & GWS_HORZ_SLIDER) {
                float relX = mx - static_cast<float>(hit->x());
                float val = relX / static_cast<float>(hit->width());
                hit->setSliderValue(val);
            }
            if (hit->data().style & GWS_VERT_SLIDER) {
                float relY = my - static_cast<float>(hit->y());
                float val = relY / static_cast<float>(hit->height());
                hit->setSliderValue(val);
            }
            if (hit->data().style & GWS_COMBO_BOX) {
                hit->setDropdownOpen(!hit->isDropdownOpen());
            }
            if (hit->data().style & GWS_CHECK_BOX) {
                hit->toggleChecked();
            }
            if ((hit->data().style & GWS_SCROLL_LISTBOX) && !hit->getItems().empty()) {
                float relY = my - static_cast<float>(hit->y());
                int lineH = hit->getDropdownLineHeight();
                int clickedIdx = static_cast<int>(relY / lineH) + hit->getScrollOffset();
                if (clickedIdx >= 0 && clickedIdx < hit->getItemCount()) {
                    hit->setSelectedIndex(clickedIdx);
                }
            }

            // GadgetPushButtonInput queues its alt/default GUI sound on both
            // left- and right-down before deciding whether the button commits
            // on down or matching up. The observer is presentation-only; it
            // cannot affect widget activation or deterministic command
            // admission.
            if ((event.button.button == SDL_BUTTON_LEFT ||
                 event.button.button == SDL_BUTTON_RIGHT) &&
                (hit->data().style & GWS_PUSH_BUTTON) != 0 &&
                m_pushButtonPressHandler) {
                m_pushButtonPressHandler(*hit);
            }

            // Original GadgetPushButton grabs on down and sends GBM_SELECTED
            // on up only while the pointer still belongs to that same button.
            // ON_MOUSE_DOWN and CHECK_LIKE are explicit exceptions; the other
            // gadget families retain their existing down-time primitives.
            if (event.button.button == SDL_BUTTON_LEFT &&
                isOrdinaryPushButton(*hit)) {
                hit->setPressed(true);
                m_pressedWidget = hit;
                m_pressedMouseButton = event.button.button;
                m_pressedModifiers = static_cast<uint32_t>(SDL_GetModState());
            // Entry-field activation only moves focus/caret.  Its completion
            // callback is dispatched by Return/GEM_EDIT_DONE semantics above;
            // firing it on mouse-down would submit a beacon caption merely
            // because the player clicked into the editor.
            } else if (!(hit->data().style & GWS_ENTRY_FIELD) &&
                       hit->hasClickHandler()) {
                hit->invokeClick({
                    .button = pointerButton(event.button.button),
                    .modifiers = static_cast<uint32_t>(SDL_GetModState()),
                });
            }
            return true;
        }

        // GameWindowManager still owns a disabled PushButton long enough to
        // play GUIClickDisabled on the matching left-up.  `hitTest()` rightly
        // excludes it from activation, so use the authored window lookup only
        // for this local feedback path and consume the click instead of
        // leaking it into world input underneath the ControlBar.
        if (event.button.button == SDL_BUTTON_LEFT) {
            const Widget* disabled = m_root->windowAt(mx, my, true);
            if (disabled && !disabled->isEnabled() &&
                (disabled->data().style & GWS_PUSH_BUTTON) != 0) {
                m_disabledPressedWidget = disabled;
                m_pressedMouseButton = event.button.button;
                return true;
            }
        }

        clearFocus();
        return false;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        m_pressedMouseButton != 0 &&
        event.button.button == m_pressedMouseButton) {
        int winW = 0, winH = 0;
        engine::Renderer::instance().getWindowSize(winW, winH);

        Widget* pressed = m_pressedWidget;
        const Widget* disabledPressed = m_disabledPressedWidget;
        const uint32_t pressedModifiers = m_pressedModifiers;
        if (pressed) pressed->setPressed(false);
        m_pressedWidget = nullptr;
        m_disabledPressedWidget = nullptr;
        m_pressedMouseButton = 0;
        m_pressedModifiers = 0;

        if (pressed && winW > 0 && winH > 0) {
            const engine::Renderer& renderer = engine::Renderer::instance();
            const float mx = renderer.windowToUiX(event.button.x);
            const float my = renderer.windowToUiY(event.button.y);
            Widget* releasedOver = m_root->hitTest(mx, my, texMgr);
            if (releasedOver == pressed && pressed->isEnabled() &&
                pressed->hasClickHandler()) {
                pressed->invokeClick({
                    .button = pointerButton(event.button.button),
                    .modifiers = pressedModifiers,
                });
            }
        } else if (disabledPressed && winW > 0 && winH > 0) {
            const engine::Renderer& renderer = engine::Renderer::instance();
            const float mx = renderer.windowToUiX(event.button.x);
            const float my = renderer.windowToUiY(event.button.y);
            const Widget* releasedOver = m_root->windowAt(mx, my, true);
            if (releasedOver == disabledPressed &&
                m_disabledPushButtonPressHandler) {
                m_disabledPushButtonPressHandler(*disabledPressed);
            }
        }

        // The down event established a WND pointer grab.  Its matching up is
        // consumed even when drag-out cancelled the callback.
        return true;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        int winW = 0, winH = 0;
        engine::Renderer::instance().getWindowSize(winW, winH);
        if (winW <= 0 || winH <= 0) return false;
        const engine::Renderer& renderer = engine::Renderer::instance();
        const float mx = renderer.windowToUiX(event.motion.x);
        const float my = renderer.windowToUiY(event.motion.y);

        std::function<void(Widget*)> updateComboBoxHover = [&](Widget* widget) {
            if (!widget || !widget->isVisible()) return;
            if (widget->data().style & GWS_COMBO_BOX) {
                if (widget->isEnabled() && widget->isDropdownOpen() &&
                    !widget->getItems().empty()) {
                    float dx = static_cast<float>(widget->x());
                    float dy = static_cast<float>(widget->y()) + static_cast<float>(widget->height());
                    float dw = static_cast<float>(widget->width());
                    int lineH = widget->getDropdownLineHeight();
                    int maxVisible = std::min(static_cast<int>(widget->getItems().size()), WIDGET_COMBO_MAX_VISIBLE);
                    float listH = static_cast<float>(maxVisible * lineH);

                    if (mx >= dx && mx <= dx + dw && my >= dy && my <= dy + listH) {
                        int visIdx = static_cast<int>(my - dy) / lineH;
                        int itemIdx = visIdx + widget->getScrollOffset();
                        if (visIdx >= 0 && visIdx < maxVisible &&
                            itemIdx < static_cast<int>(widget->getItems().size())) {
                            widget->setHighlightedItem(itemIdx);
                        } else {
                            widget->setHighlightedItem(-1);
                        }
                    } else {
                        widget->setHighlightedItem(-1);
                    }
                } else {
                    widget->setHighlightedItem(-1);
                }
            }
            for (int i = 0; i < widget->getChildCount(); ++i) {
                updateComboBoxHover(widget->getChild(i));
            }
        };
        updateComboBoxHover(m_root.get());

        // Disabled command cameos still own hover so ControlBar can explain
        // insufficient funds, prerequisites, cooldown, and other admission
        // failures. Mouse-button paths continue to use enabled-only hitTest().
        Widget* hit = m_root->hitTest(mx, my, texMgr, true);
        if (m_pressedMouseButton != 0 && m_pressedWidget &&
            hit != m_pressedWidget) {
            // GadgetPushButton clears its selected state on mouse leave.  Do
            // not re-arm it if the pointer later comes back before release.
            m_pressedWidget->setPressed(false);
            m_pressedWidget = nullptr;
        }
        if (hit != m_hoveredWidget) {
            if (m_hoveredWidget) m_hoveredWidget->setHovered(false);
            m_hoveredWidget = hit;
            if (m_hoveredWidget) m_hoveredWidget->setHovered(true);
            ++m_hoverGeneration;
            if (m_hoverGeneration == 0) m_hoverGeneration = 1;
            if (m_hoveredWidget && m_hoveredWidget->onHover) {
                m_hoveredWidget->onHover(*m_hoveredWidget);
            }
        }
        return false;
    }

    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        int winW = 0, winH = 0;
        engine::Renderer::instance().getWindowSize(winW, winH);
        if (winW <= 0 || winH <= 0) return false;
        const engine::Renderer& renderer = engine::Renderer::instance();
        const float mx = renderer.windowToUiX(
            static_cast<float>(event.wheel.mouse_x));
        const float my = renderer.windowToUiY(
            static_cast<float>(event.wheel.mouse_y));

        std::function<bool(Widget*)> scrollComboDropdown = [&](Widget* widget) -> bool {
            if (!widget || !widget->isVisible()) return false;
            if ((widget->data().style & GWS_COMBO_BOX) && widget->isEnabled()) {
                if (widget->isDropdownOpen() && !widget->getItems().empty()) {
                    float dx = static_cast<float>(widget->x());
                    float dy = static_cast<float>(widget->y()) + static_cast<float>(widget->height());
                    float dw = static_cast<float>(widget->width());
                    int lineH = widget->getDropdownLineHeight();
                    int maxVisible = std::min(static_cast<int>(widget->getItems().size()), WIDGET_COMBO_MAX_VISIBLE);
                    float listH = static_cast<float>(maxVisible * lineH);
                    if (mx >= dx && mx <= dx + dw && my >= dy && my <= dy + listH) {
                        int maxScroll = std::max(0, static_cast<int>(widget->getItems().size()) - maxVisible);
                        int newOffset = widget->getScrollOffset() - static_cast<int>(event.wheel.y);
                        widget->setScrollOffset(std::clamp(newOffset, 0, maxScroll));
                        return true;
                    }
                }
            }
            for (int i = 0; i < widget->getChildCount(); ++i) {
                if (scrollComboDropdown(widget->getChild(i))) return true;
            }
            return false;
        };
        if (scrollComboDropdown(m_root.get())) return true;

        if (auto* hit = m_root->hitTest(mx, my, texMgr)) {
            if (hit->data().style & GWS_SCROLL_LISTBOX) {
                int newOffset = hit->getScrollOffset() - static_cast<int>(event.wheel.y);
                newOffset = std::clamp(newOffset, 0, std::max(0, hit->getItemCount() - 1));
                hit->setScrollOffset(newOffset);
                return true;
            }
        }
    }

    return false;
}

bool WndRuntime::activateLocalizedHotkey(
    uint32_t scancode, uint32_t modifiers) {
    if (!m_loaded || !m_active || !m_root || m_focusedWidget) return false;
    constexpr uint32_t blockingModifiers =
        SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI;
    if ((modifiers & blockingModifiers) != 0 ||
        scancode < SDL_SCANCODE_A || scancode > SDL_SCANCODE_Z) {
        return false;
    }
    const char key = static_cast<char>(
        'a' + (scancode - SDL_SCANCODE_A));
    Widget* widget = findLocalizedHotkey(m_root.get(), key);
    if (!widget) return false;
    widget->invokeClick({.modifiers = modifiers});
    return true;
}

bool WndRuntime::hasTextInputFocus() const noexcept {
    return m_focusedWidget &&
        (m_focusedWidget->data().style & GWS_ENTRY_FIELD) != 0;
}

WndRuntime::WorldInputDisposition WndRuntime::worldInputDispositionAt(
    float virtualX, float virtualY) const noexcept {
    if (!m_loaded || !m_active || !m_root) {
        return WorldInputDisposition::NoWindow;
    }

    // m_root is a synthetic zero-sized owner and must not participate in the
    // authored parent chain.  Top-level widgets follow the same front-to-back
    // order as retained rendering and normal gadget hit-testing.
    for (int index = m_root->getChildCount() - 1; index >= 0; --index) {
        const Widget* topLevel = m_root->getChild(index);
        if (!topLevel) continue;
        const Widget* hit = topLevel->windowAt(
            virtualX, virtualY, false);
        if (!hit) continue;

        // getWindowUnderCursor discards a deepest NO_INPUT window instead of
        // falling through to a lower top-level window.
        if ((hit->data().status & WIN_STATUS_NO_INPUT) != 0) {
            return WorldInputDisposition::SeeThrough;
        }
        for (const Widget* window = hit;
             window && window != m_root.get();
             window = window->getParent()) {
            if ((window->data().status & WIN_STATUS_SEE_THRU) == 0) {
                return WorldInputDisposition::Blocked;
            }
        }
        return WorldInputDisposition::SeeThrough;
    }
    return WorldInputDisposition::NoWindow;
}

void WndRuntime::clearInteractionState() {
    if (m_hoveredWidget) m_hoveredWidget->setHovered(false);
    if (m_focusedWidget) m_focusedWidget->setFocused(false);
    if (m_pressedWidget) m_pressedWidget->setPressed(false);
    m_hoveredWidget = nullptr;
    m_focusedWidget = nullptr;
    m_pressedWidget = nullptr;
    m_disabledPressedWidget = nullptr;
    m_pressedMouseButton = 0;
    m_pressedModifiers = 0;
    ++m_hoverGeneration;
    if (m_hoverGeneration == 0) m_hoverGeneration = 1;
}

void WndRuntime::resolveCallbacks() {
    // Explicit reference keeps the default callback registrar linked from the
    // engine static library; a static initializer alone can be discarded.
    registerDefaultLayoutCallbacks();
    auto& reg = LayoutCallbackRegistry::instance();

    if (!isEmptyCallbackName(m_layout.initCallback)) {
        m_wndInit = reg.findInit(m_layout.initCallback);
        if (!m_wndInit) {
            TD_LOG_WARN("[WndRuntime] WND init callback '{}' not found", m_layout.initCallback);
        }
    }
    if (!isEmptyCallbackName(m_layout.updateCallback)) {
        m_wndUpdate = reg.findUpdate(m_layout.updateCallback);
        if (!m_wndUpdate) {
            TD_LOG_WARN("[WndRuntime] WND update callback '{}' not found", m_layout.updateCallback);
        }
    }
    if (!isEmptyCallbackName(m_layout.shutdownCallback)) {
        m_wndShutdown = reg.findShutdown(m_layout.shutdownCallback);
        if (!m_wndShutdown) {
            TD_LOG_WARN("[WndRuntime] WND shutdown callback '{}' not found", m_layout.shutdownCallback);
        }
    }
}

void WndRuntime::setFocus(Widget* widget) {
    if (m_focusedWidget) m_focusedWidget->setFocused(false);
    m_focusedWidget = widget;
    if (m_focusedWidget) m_focusedWidget->setFocused(true);
}

void WndRuntime::clearFocus() {
    setFocus(nullptr);
}

void WndRuntime::cycleFocus(bool reverse) {
    container::Vector<Widget*> focusable;
    std::function<void(Widget&)> collect = [&](Widget& widget) {
        uint32_t style = widget.data().style;
        if (style & (GWS_ENTRY_FIELD | GWS_PUSH_BUTTON | GWS_CHECK_BOX |
                     GWS_RADIO_BUTTON | GWS_COMBO_BOX | GWS_HORZ_SLIDER |
                     GWS_VERT_SLIDER)) {
            if (widget.isVisible()) focusable.push_back(&widget);
        }
        for (int i = 0; i < widget.getChildCount(); ++i) {
            if (auto* child = widget.getChild(i)) collect(*child);
        }
    };
    if (m_root) collect(*m_root);
    if (focusable.empty()) return;

    int index = -1;
    for (int i = 0; i < static_cast<int>(focusable.size()); ++i) {
        if (focusable[i] == m_focusedWidget) {
            index = i;
            break;
        }
    }

    if (reverse) {
        index = (index <= 0) ? static_cast<int>(focusable.size()) - 1 : index - 1;
    } else {
        index = (index >= static_cast<int>(focusable.size()) - 1) ? 0 : index + 1;
    }

    setFocus(focusable[index]);
}

} // namespace gui
