#include "core/container/container_types.h"
#include "Widget.h"
#include "WinInstanceData.h"
#include "DX12Renderer.h"
#include "Renderer.h"
#include "FontRegistry.h"
#include "Font.h"
#include "TextureManager.h"
#include "StringTable.h"
#include "DrawFunc.h"
#include "DrawCallbackRegistry.h"
#include "presentation/render/PresentationDefaults.h"
#include "../../../core/constants/Paths.h"
#include "../../../core/constants/Strings.h"
#include "../../../core/constants/Colors.h"
#include "../../../core/constants/Widget.h"

#include <algorithm>
#include <cmath>
#include <optional>

// Original engine constants (Gadget.h) — now in core/constants/Widget.h

namespace gui {

int Widget::x() const {
    return static_cast<int>(std::lround(
        engine::Renderer::instance().layoutUiX(
            static_cast<float>(m_data.x), static_cast<float>(m_data.w))));
}

int Widget::y() const {
    return static_cast<int>(std::lround(
        engine::Renderer::instance().layoutUiY(
            static_cast<float>(m_data.y), static_cast<float>(m_data.h))));
}

void Widget::setEnabled(bool enabled) noexcept {
    if (enabled) m_data.status |= WIN_STATUS_ENABLED;
    else {
        m_data.status &= ~WIN_STATUS_ENABLED;
        // Pressed is a transient pointer state.  A logic/UI projection can
        // disable a widget between mouse-down and mouse-up; in that case the
        // disabled cameo must not retain the old pushed visual.
        m_pressed = false;
    }
}

bool Widget::isEffectivelyVisible() const noexcept {
    for (const Widget* current = this; current; current = current->m_parent) {
        if (!current->m_visible) return false;
    }
    return true;
}

bool Widget::isEnabled() const noexcept {
    return (m_data.status & WIN_STATUS_ENABLED) != 0;
}

void Widget::setActive(bool active) noexcept {
    if (active) m_data.status |= WIN_STATUS_ACTIVE;
    else m_data.status &= ~WIN_STATUS_ACTIVE;
    m_checked = active;
}

bool Widget::isActive() const noexcept {
    return (m_data.status & WIN_STATUS_ACTIVE) != 0;
}

void Widget::setUseOverlayStates(bool enabled) noexcept {
    if (enabled) m_data.status |= WIN_STATUS_USE_OVERLAY_STATES;
    else m_data.status &= ~WIN_STATUS_USE_OVERLAY_STATES;
}

bool Widget::usesOverlayStates() const noexcept {
    return (m_data.status & WIN_STATUS_USE_OVERLAY_STATES) != 0;
}

namespace {

bool hasDrawImage(const container::String& image) {
    return !image.empty() && image != NO_IMAGE.data();
}

const WndParser::DrawData* activeDrawDataForHit(const Widget& widget) {
    const auto& data = widget.data();
    if ((data.status & WIN_STATUS_ENABLED) == 0) {
        return data.disabledDrawData;
    }
    if (widget.isHovered()) {
        for (const auto& slot : data.hiliteDrawData) {
            if (hasDrawImage(slot.image)) return data.hiliteDrawData;
        }
    }
    return data.enabledDrawData;
}

std::optional<bool> isMappedImageOpaqueAt(engine::TextureManager& texMgr,
                                          const container::String& imageName,
                                          float dstX, float dstY,
                                          float dstW, float dstH,
                                          float mx, float my) {
    if (!hasDrawImage(imageName) || dstW <= 0.0f || dstH <= 0.0f) {
        return std::nullopt;
    }
    if (mx < dstX || mx >= dstX + dstW || my < dstY || my >= dstY + dstH) {
        return std::nullopt;
    }

    auto result = texMgr.findMappedImage(imageName);
    if (!result.found || !result.texture || !result.texture->hasData() ||
        result.texture->width == 0 || result.texture->height == 0) {
        return std::nullopt;
    }

    const int srcW = result.right - result.left;
    const int srcH = result.bottom - result.top;
    if (srcW <= 0 || srcH <= 0) {
        return std::nullopt;
    }

    const float u = (mx - dstX) / dstW;
    const float v = (my - dstY) / dstH;
    int px = result.left + static_cast<int>(u * static_cast<float>(srcW));
    int py = result.top + static_cast<int>(v * static_cast<float>(srcH));
    px = std::clamp(px, 0, static_cast<int>(result.texture->width) - 1);
    py = std::clamp(py, 0, static_cast<int>(result.texture->height) - 1);

    const size_t offset = (static_cast<size_t>(py) * result.texture->width + static_cast<size_t>(px)) * 4 + 3;
    if (offset >= result.texture->pixels.size()) {
        return std::nullopt;
    }

    constexpr uint8_t kHitAlphaThreshold = 16;
    return result.texture->pixels[offset] > kHitAlphaThreshold;
}

std::optional<bool> isThreePartButtonOpaqueAt(engine::TextureManager& texMgr,
                                              const Widget& widget,
                                              const WndParser::DrawData* drawData,
                                              float mx, float my) {
    const container::String& leftName = drawData[0].image;
    const container::String& middleName = drawData[5].image;
    const container::String& rightName = drawData[6].image;
    if (!hasDrawImage(leftName) || !hasDrawImage(middleName) || !hasDrawImage(rightName)) {
        return std::nullopt;
    }

    auto left = texMgr.findMappedImage(leftName);
    auto middle = texMgr.findMappedImage(middleName);
    auto right = texMgr.findMappedImage(rightName);
    if (!left.found || !middle.found || !right.found) {
        return std::nullopt;
    }

    const float x = static_cast<float>(widget.x());
    const float y = static_cast<float>(widget.y());
    const float w = static_cast<float>(widget.width());
    const float h = static_cast<float>(widget.height());
    const float leftW = static_cast<float>(left.right - left.left);
    const float middleW = static_cast<float>(middle.right - middle.left);
    const float rightW = static_cast<float>(right.right - right.left);
    if (leftW <= 0.0f || middleW <= 0.0f || rightW <= 0.0f) {
        return std::nullopt;
    }

    if (leftW + rightW >= w) {
        const float halfW = w * 0.5f;
        if (mx < x + halfW) {
            return isMappedImageOpaqueAt(texMgr, leftName, x, y, halfW, h, mx, my);
        }
        return isMappedImageOpaqueAt(texMgr, rightName, x + halfW, y, w - halfW, h, mx, my);
    }

    const float rightStartX = x + w - rightW;
    if (mx < x + leftW) {
        return isMappedImageOpaqueAt(texMgr, leftName, x, y, leftW, h, mx, my);
    }
    if (mx >= rightStartX) {
        return isMappedImageOpaqueAt(texMgr, rightName, rightStartX, y, rightW, h, mx, my);
    }

    float cx = x + leftW;
    while (cx < rightStartX) {
        const float pieceW = std::min(middleW, rightStartX - cx);
        if (mx >= cx && mx < cx + pieceW) {
            return isMappedImageOpaqueAt(texMgr, middleName, cx, y, pieceW, h, mx, my);
        }
        cx += middleW;
    }
    return std::nullopt;
}

bool acceptsTextureHit(const Widget& widget, float mx, float my, engine::TextureManager* texMgr) {
    if (!texMgr) return true;

    const auto& data = widget.data();
    const auto* drawData = activeDrawDataForHit(widget);
    if (auto opaque = isThreePartButtonOpaqueAt(*texMgr, widget, drawData, mx, my)) {
        return *opaque;
    }

    bool sampled = false;
    for (int slot : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        if (auto opaque = isMappedImageOpaqueAt(*texMgr,
                                                drawData[slot].image,
                                                static_cast<float>(data.x),
                                                static_cast<float>(data.y),
                                                static_cast<float>(data.w),
                                                static_cast<float>(data.h),
                                                mx, my)) {
            sampled = true;
            if (*opaque) return true;
        }
    }

    return !sampled;
}

bool usesRectangularLegacyGadgetHit(const Widget& widget) noexcept {
    constexpr uint32_t legacyGadgetStyles =
        GWS_PUSH_BUTTON |
        GWS_CHECK_BOX |
        GWS_RADIO_BUTTON |
        GWS_VERT_SLIDER |
        GWS_HORZ_SLIDER |
        GWS_SCROLL_LISTBOX |
        GWS_ENTRY_FIELD |
        GWS_COMBO_BOX;
    return (widget.data().style & legacyGadgetStyles) != 0;
}

} // namespace

Widget::Widget(const WndParser::WindowDef& data)
    : m_data(data),
      m_visible((data.status & WIN_STATUS_HIDDEN) == 0) {
}

// ── Tree ──────────────────────────────────────────────────────────────────

void Widget::addChild(container::UniquePtr<Widget> child) {
    if (!child) return;
    child->m_parent = this;
    m_children.push_back(std::move(child));
}

Widget* Widget::getChild(int index) const {
    if (index < 0 || index >= static_cast<int>(m_children.size())) return nullptr;
    return m_children[index].get();
}

// ── Visibility ────────────────────────────────────────────────────────────

void Widget::showRecursive() {
    m_visible = true;
    for (auto& child : m_children) {
        child->showRecursive();
    }
}

void Widget::hideRecursive() {
    m_visible = false;
    for (auto& child : m_children) {
        child->hideRecursive();
    }
}

// ── Input ─────────────────────────────────────────────────────────────────

Widget* Widget::hitTest(float mx, float my, engine::TextureManager* texMgr,
                        bool includeDisabled) {
    if (!m_visible) return nullptr;
    // Root/virtual containers with zero size still need to recurse into children
    bool hasBounds = (m_data.w > 0 && m_data.h > 0);

    // Check children first (topmost = last drawn = checked first)
    for (int i = static_cast<int>(m_children.size()) - 1; i >= 0; --i) {
        if (auto* hit = m_children[i]->hitTest(
                mx, my, texMgr, includeDisabled))
            return hit;
    }

    // Check self
    // Rendering re-anchors left/centre/right WND controls when the UI canvas
    // is wider than the authored 800x600 layout. Pointer queries receive
    // coordinates in that expanded canvas, so they must test the same final
    // bounds used by renderSelf rather than the unshifted authored bounds.
    const float displayX = static_cast<float>(x());
    const float displayY = static_cast<float>(y());
    if (hasBounds &&
        mx >= displayX && mx < displayX + static_cast<float>(m_data.w) &&
        my >= displayY && my < displayY + static_cast<float>(m_data.h)) {
        if (m_data.status & WIN_STATUS_NO_INPUT) {
            return nullptr;
        }
        uint32_t st = m_data.style;
        bool interactive = (st & GWS_PUSH_BUTTON) ||
                           (st & GWS_CHECK_BOX) ||
                           (st & GWS_RADIO_BUTTON) ||
                           (st & GWS_VERT_SLIDER) ||
                           (st & GWS_HORZ_SLIDER) ||
                           (st & GWS_SCROLL_LISTBOX) ||
                           (st & GWS_ENTRY_FIELD) ||
                           (st & GWS_COMBO_BOX);
        interactive = interactive || ((st & GWS_MOUSE_TRACK) != 0);
        if ((interactive || hasClickHandler()) &&
            (isEnabled() || includeDisabled)) {
            // Original WND gadgets receive input over their authored window
            // rectangle.  Keep texture-alpha hit testing only for custom or
            // modern callback surfaces that have no standard gadget style.
            if (!usesRectangularLegacyGadgetHit(*this) &&
                !acceptsTextureHit(*this, mx, my, texMgr)) {
                return nullptr;
            }
            return this;
        }
    }
    return nullptr;
}

const Widget* Widget::windowAt(float mx, float my,
                               bool ignoreEnabled) const noexcept {
    const float displayX = static_cast<float>(x());
    const float displayY = static_cast<float>(y());
    if (!m_visible || m_data.w <= 0 || m_data.h <= 0 ||
        mx < displayX ||
        mx >= displayX + static_cast<float>(m_data.w) ||
        my < displayY ||
        my >= displayY + static_cast<float>(m_data.h) ||
        (!ignoreEnabled && !isEnabled())) {
        return nullptr;
    }

    // Children are rendered in declaration order, so the last child is the
    // front-most one in the retained renderer.  Once a child is hit its own
    // parent chain owns the point; a disabled/hidden child is skipped exactly
    // like GameWindow::winPointInChild.
    for (auto child = m_children.rbegin(); child != m_children.rend(); ++child) {
        if (*child) {
            if (const Widget* hit = (*child)->windowAt(
                    mx, my, ignoreEnabled)) {
                return hit;
            }
        }
    }
    return this;
}

// ── Keyboard input ────────────────────────────────────────────────────────

void Widget::handleKeyInput(uint32_t keyCode, uint32_t keyMod, const container::String& text) {
    if (!(m_data.style & GWS_ENTRY_FIELD)) return;

    bool shift = (keyMod & SDL_KMOD_SHIFT) != 0;
    bool ctrl = (keyMod & SDL_KMOD_CTRL) != 0;

    // Handle special keys
    if (keyCode == SDL_SCANCODE_LEFT) {
        if (m_cursorPos > 0) m_cursorPos--;
    } else if (keyCode == SDL_SCANCODE_RIGHT) {
        if (m_cursorPos < static_cast<int>(m_editText.size())) m_cursorPos++;
    } else if (keyCode == SDL_SCANCODE_HOME) {
        m_cursorPos = 0;
    } else if (keyCode == SDL_SCANCODE_END) {
        m_cursorPos = static_cast<int>(m_editText.size());
    } else if (keyCode == SDL_SCANCODE_BACKSPACE) {
        if (m_cursorPos > 0) {
            m_editText.erase(m_cursorPos - 1, 1);
            m_cursorPos--;
        }
    } else if (keyCode == SDL_SCANCODE_DELETE) {
        if (m_cursorPos < static_cast<int>(m_editText.size())) {
            m_editText.erase(m_cursorPos, 1);
        }
    } else if (ctrl && keyCode == SDL_SCANCODE_A) {
        // Select all
        m_cursorPos = 0;
    } else if (ctrl && keyCode == SDL_SCANCODE_C) {
        // Copy (TODO: SDL clipboard)
    } else if (ctrl && keyCode == SDL_SCANCODE_V) {
        // Paste (TODO: SDL clipboard)
    } else if (ctrl && keyCode == SDL_SCANCODE_X) {
        // Cut (TODO: SDL clipboard)
    } else if (!text.empty() && text[0] >= 32) {
        // Printable character - insert at cursor
        m_editText.insert(m_cursorPos, text);
        m_cursorPos += static_cast<int>(text.size());
    }
}

// ── Find by name ──────────────────────────────────────────────────────────

Widget* Widget::findByName(const container::String& name) {
    if (shortName() == name) return this;
    for (auto& child : m_children) {
        if (auto* found = child->findByName(name))
            return found;
    }
    return nullptr;
}

const Widget* Widget::findByName(const container::String& name) const {
    if (shortName() == name) return this;
    for (const auto& child : m_children) {
        if (auto* found = child->findByName(name))
            return found;
    }
    return nullptr;
}

container::String Widget::shortName() const {
    container::String s = m_data.name;
    size_t pos = s.rfind(':');
    if (pos != container::String::npos) s = s.substr(pos + 1);
    if (!s.empty() && s.front() == '"') s = s.substr(1);
    if (!s.empty() && s.back() == '"') s.pop_back();
    return s;
}

void Widget::setBounds(int x, int y, int w, int h) {
    m_data.x = x;
    m_data.y = y;
    m_data.w = w;
    m_data.h = h;
}

void Widget::setDrawImage(int slot, const container::String& enabled, const container::String& hilite, const container::String& disabled) {
    constexpr int kDrawSlots = 9;
    if (slot < 0 || slot >= kDrawSlots) return;
    if (!enabled.empty()) {
        m_data.enabledDrawData[slot].image = enabled;
    }
    if (!hilite.empty()) {
        m_data.hiliteDrawData[slot].image = hilite;
    }
    if (!disabled.empty()) {
        m_data.disabledDrawData[slot].image = disabled;
    }
}

void Widget::clearDrawImage(int slot) noexcept {
    constexpr int kDrawSlots = 9;
    if (slot < 0 || slot >= kDrawSlots) return;
    m_data.enabledDrawData[slot].image.clear();
    m_data.hiliteDrawData[slot].image.clear();
    m_data.disabledDrawData[slot].image.clear();
}

// ── Rendering ─────────────────────────────────────────────────────────────

static engine::Font* getFontForWidget(const Widget& w) {
    auto& reg = engine::FontRegistry::instance();
    const auto& d = w.data();
    if (!d.fontName.empty()) {
        auto* f = reg.getFont(d.fontName, d.fontSize, d.fontBold);
        if (f) return f;
    }
    if (d.fontName != FONT_ARIAL) {
        auto* f = reg.getFont(FONT_ARIAL.data(), d.fontSize, d.fontBold);
        if (f) return f;
    }
    return reg.getFont(FONT_GENERALS.data(), d.fontSize, d.fontBold);
}

int Widget::getDropdownLineHeight() const {
    auto* font = getFontForWidget(*this);
    return font ? font->getLineHeight() : WIDGET_LINE_HEIGHT;
}

void Widget::render(engine::Renderer& renderer, engine::TextureManager& texMgr, int depth) {
    if (!m_visible) return;
    if (m_data.w <= 0 || m_data.h <= 0) return;

    (void)depth;

    renderSelf(renderer, texMgr);
    renderChildren(renderer, texMgr, depth + 1);
}

void Widget::renderSkeleton(engine::Renderer& renderer, int depth) const {
    if (!m_visible) return;
    if (m_data.w <= 0 || m_data.h <= 0) return;

    const float x = static_cast<float>(m_data.x);
    const float y = static_cast<float>(m_data.y);
    const float w = static_cast<float>(m_data.w);
    const float h = static_cast<float>(m_data.h);

    uint32_t fill = (depth % 2 == 0) ? 0x403366CC : 0x4022AA66;
    renderer.drawQuad(x, y, w, h, fill);
    renderer.drawRect(x, y, w, h, COLOR_WHITE);

    auto* font = getFontForWidget(*this);
    container::String label = shortName();
    if (label.empty()) label = m_data.type;
    if (font) {
        renderer.drawText(font, label, x + 2.0f, y + 2.0f, COLOR_WHITE);
    } else {
        renderer.drawText(label, x + 2.0f, y + 2.0f, COLOR_WHITE);
    }

    for (const auto& child : m_children) {
        if (child) child->renderSkeleton(renderer, depth + 1);
    }
}

void Widget::renderSelf(engine::Renderer& renderer, engine::TextureManager& texMgr) {
    if (m_data.type == TYPE_ROOT) return;  // Skip virtual root

    // Some windows have DRAWCALLBACK that controls their rendering externally.
    // W3DNoDraw suppresses windows whose rendering is owned by another
    // retained in-game presentation path.
    const auto& cb = m_data.drawCallback;
    if (cb == DRAW_CB_W3D_NO_DRAW.data()) {
        return;
    }
    bool customBaseDrawn = false;
    if (!cb.empty()) {
        auto drawCallback = DrawCallbackRegistry::instance().find(cb);
        customBaseDrawn = drawCallback &&
            drawCallback(*this, renderer, texMgr);
    }

    auto info = draw::toDrawInfo(m_data);
    bool hasImageStatus = (m_data.status & 0x80) != 0;
    info.checked = m_checked;
    info.progress = m_sliderValue;

    const bool enabled = (m_data.status & WIN_STATUS_ENABLED) != 0;
    if (!enabled) {
        // The original W3D gadgets select DisabledDrawData and apply a dark
        // disabled tint. Several runtime ControlBar projections deliberately
        // reuse the same cameo for all three states, so selecting the image
        // alone is insufficient: an unavailable upgrade/power otherwise
        // looks fully enabled even though hit testing rejects it.
        bool hasDisabledImage = false;
        for (const auto& slot : info.disabledDrawData) {
            if (hasDrawImage(slot.image)) {
                hasDisabledImage = true;
                break;
            }
        }
        if (hasDisabledImage) info.enabledDrawData = info.disabledDrawData;
        for (auto& slot : info.enabledDrawData) {
            const uint32_t alpha = slot.color & 0xff000000u;
            const uint32_t red = ((slot.color >> 16u) & 0xffu) / 2u;
            const uint32_t green = ((slot.color >> 8u) & 0xffu) / 2u;
            const uint32_t blue = (slot.color & 0xffu) / 2u;
            slot.color = alpha | (red << 16u) | (green << 8u) | blue;
        }
    } else if (m_hovered &&
               (m_data.style & (GWS_PUSH_BUTTON | GWS_CHECK_BOX |
                                GWS_RADIO_BUTTON))) {
        info.enabledDrawData = info.hiliteDrawData;
    }

    if (!customBaseDrawn) {
        auto drawFunc = draw::getDrawFunc(m_data.type, hasImageStatus);
        if (drawFunc) {
            drawFunc(renderer, info, texMgr, 0);
        } else {
            float dx = static_cast<float>(m_data.x);
            float dy = static_cast<float>(m_data.y);
            float dw = static_cast<float>(m_data.w);
            float dh = static_cast<float>(m_data.h);
            uint32_t tint = m_data.enabledDrawData[0].color;
            if (tint == 0 || tint == COLOR_WHITE) tint = COLOR_DEFAULT_TINT;
            renderer.drawQuad(dx, dy, dw, dh, tint);
            renderer.drawRect(dx, dy, dw, dh, COLOR_WHITE);
        }
    }

    if (m_drawCommandButtonChrome) {
        const float dx = static_cast<float>(x());
        const float dy = static_cast<float>(y());
        const float dw = static_cast<float>(width());
        const float dh = static_cast<float>(height());
        if (!enabled &&
            (m_data.status & WIN_STATUS_NOT_READY) == 0) {
            // W3D uses DRAW_IMAGE_GRAYSCALE for unavailable command cameos.
            // The retained texture shader has no grayscale mode, so a strong
            // neutral wash removes the remaining hue while the authored image
            // stays recognizable beneath it. Hit testing remains disabled.
            renderer.drawQuad(dx, dy, dw, dh, 0xa0606060u);
        } else if (enabled) {
            static const container::String pushed{"Cameo_push"};
            static const container::String hilited{"Cameo_hilited"};
            const bool depressed = m_pressed || isActive();
            const container::String* overlay = depressed
                ? &pushed : m_hovered ? &hilited : nullptr;
            if (overlay) {
                const bool drewOverlay = draw::drawMappedImage(
                    renderer, texMgr, *overlay, dx, dy, dw, dh);
                // Keep the state observable even if a mod omits the common
                // Cameo overlay resource. The authored image still remains
                // the base and will recover automatically when supplied.
                if (!drewOverlay) {
                    renderer.drawQuad(
                        dx, dy, dw, dh,
                        depressed ? 0x40000000u : 0x28ffffffu);
                }
            }
        }
    }

    if (m_cooldownReadyFraction < 1.0f &&
        (m_data.style & GWS_PUSH_BUTTON)) {
        const float remaining = 1.0f - m_cooldownReadyFraction;
        const float dx = static_cast<float>(m_data.x);
        const float dy = static_cast<float>(m_data.y);
        const float dw = static_cast<float>(m_data.w);
        const float dh = static_cast<float>(m_data.h) * remaining;
        // The retained WND runtime has no textured triangle fan yet. Preserve
        // the authoritative inverse-clock fraction with a dark top-down mask;
        // this remains deterministic presentation and clears exactly at ready.
        renderer.drawQuad(dx, dy, dw, dh, 0x98000000u);
    }

    if (m_drawCommandButtonChrome) {
        const float dx = static_cast<float>(x());
        const float dy = static_cast<float>(y());
        const float dw = static_cast<float>(width());
        const float dh = static_cast<float>(height());
        // The retained renderer has no legacy W3D grayscale/bevel draw mode.
        // Keep the normal cameo visibly button-shaped with the same subdued
        // outer shadow and inward-fading edge for command and queue controls.
        const bool depressed = enabled && (m_pressed || isActive());
        renderer.drawRect(dx, dy, dw, dh, 0xb0000000u);
        if (dw > 2.0f && dh > 2.0f) {
            renderer.drawQuad(dx + 1.0f, dy + 1.0f,
                              std::max(0.0f, dw - 2.0f), 1.0f,
                              depressed ? 0x70000000u : 0x38ffffffu);
            renderer.drawQuad(dx + 1.0f, dy + 2.0f, 1.0f,
                              std::max(0.0f, dh - 3.0f),
                              depressed ? 0x70000000u : 0x28ffffffu);
            renderer.drawQuad(dx + 1.0f, dy + dh - 2.0f,
                              std::max(0.0f, dw - 2.0f), 1.0f,
                              depressed ? 0x48ffffffu : 0x70000000u);
            renderer.drawQuad(dx + dw - 2.0f, dy + 1.0f, 1.0f,
                              std::max(0.0f, dh - 2.0f),
                              depressed ? 0x38ffffffu : 0x70000000u);
        }
    }

    if (m_drawCommandBorder && (m_commandBorderColor >> 24u) != 0u) {
        const float dx = static_cast<float>(x());
        const float dy = static_cast<float>(y());
        const float dw = static_cast<float>(width());
        const float dh = static_cast<float>(height());
        // W3D draws the authored category color one pixel outside the cameo.
        // Retain that geometry and add a low-alpha inset copy so the category
        // survives parent clipping and high-DPI UI scaling without becoming a
        // thick flat frame.
        renderer.drawRect(dx - 1.0f, dy - 1.0f, dw + 2.0f, dh + 2.0f,
                          m_commandBorderColor);
        const uint32_t insetColor =
            (m_commandBorderColor & 0x00ffffffu) | 0x78000000u;
        renderer.drawRect(dx, dy, dw, dh, insetColor);
    }

    // Render slider thumb
    // Ported from GadgetHorizontalSlider.cpp + W3DPushButton.cpp
    // Thumb is a child push button at Y = HORIZONTAL_SLIDER_THUMB_POSITION (=10)
    // Thumb X = (position - minVal) * numTicks, where numTicks = (width - THUMB_WIDTH) / (max - min)
    if (m_data.style & (GWS_VERT_SLIDER | GWS_HORZ_SLIDER)) {
        float dx = static_cast<float>(m_data.x);
        float dy = static_cast<float>(m_data.y);
        float dw = static_cast<float>(m_data.w);
        float dh = static_cast<float>(m_data.h);

        

        // Use WND thumb draw data
        const auto& thumbDD = m_hovered ? info.sliderThumbHilite : info.sliderThumbEnabled;

        if (m_data.style & GWS_HORZ_SLIDER) {
            // Original: WIDGET_SLIDER_THUMB_WIDTH=13, HEIGHT=16
            // Thumb Y = WIDGET_SLIDER_THUMB_POSITION = THUMB_HEIGHT * 2/3 = 10
            float thumbW = static_cast<float>(WIDGET_SLIDER_THUMB_WIDTH);
            float thumbH = static_cast<float>(WIDGET_SLIDER_THUMB_HEIGHT);
            float thumbY = dy + static_cast<float>(WIDGET_SLIDER_THUMB_POSITION);
            float thumbX = dx + m_sliderValue * (dw - thumbW);

            // Draw thumb (push button style - single image)
            const container::String& thumbImg = thumbDD[0].image;
            if (!thumbImg.empty() && thumbImg != NO_IMAGE.data()) {
                auto result = texMgr.findMappedImage(thumbImg);
                if (result.found && result.texture) {
                    renderer.drawTextureRegion(result.texture,
                        result.left, result.top, result.right, result.bottom,
                        result.texW, result.texH,
                        thumbX, thumbY, thumbW, thumbH, thumbDD[0].color);
                }
            } else {
                // Fallback: colored rectangle (original thumb is a push button)
                uint32_t thumbColor = thumbDD[0].color;
                if (thumbColor == 0 || thumbColor == COLOR_WHITE) thumbColor = COLOR_THUMB_DEFAULT;
                renderer.drawQuad(thumbX, thumbY, thumbW, thumbH, thumbColor);
                renderer.drawRect(thumbX, thumbY, thumbW, thumbH, COLOR_WHITE);
            }
        } else {
            // Vertical slider: WIDGET_GADGET_SIZE=16
            float thumbW = static_cast<float>(WIDGET_GADGET_SIZE);
            float thumbH = static_cast<float>(WIDGET_GADGET_SIZE);
            float thumbX = dx + dw * 2.0f / 3.0f - thumbW * 0.5f;
            float thumbY = dy + m_sliderValue * (dh - thumbH);

            const container::String& thumbImg = thumbDD[0].image;
            if (!thumbImg.empty() && thumbImg != NO_IMAGE.data()) {
                auto result = texMgr.findMappedImage(thumbImg);
                if (result.found && result.texture) {
                    renderer.drawTextureRegion(result.texture,
                        result.left, result.top, result.right, result.bottom,
                        result.texW, result.texH,
                        thumbX, thumbY, thumbW, thumbH, thumbDD[0].color);
                }
            } else {
                uint32_t thumbColor = thumbDD[0].color;
                if (thumbColor == 0 || thumbColor == COLOR_WHITE) thumbColor = COLOR_THUMB_DEFAULT;
                renderer.drawQuad(thumbX, thumbY, thumbW, thumbH, thumbColor);
                renderer.drawRect(thumbX, thumbY, thumbW, thumbH, COLOR_WHITE);
            }
        }
    }

    // Draw text
    bool isEntryField = (m_data.style & GWS_ENTRY_FIELD) != 0;
    bool hasContent = isEntryField ? !m_editText.empty() : !m_data.text.empty();
    if (hasContent) {
        container::String displayText;
        if (isEntryField) {
            displayText = m_editText;
        } else {
            displayText = engine::StringTable::instance().fetch(m_data.text);
            if (displayText.empty()) displayText = m_data.text;
        }

        auto* font = getFontForWidget(*this);

        float tx = static_cast<float>(m_data.x + WIDGET_TEXT_PADDING);
        // For checkboxes, offset text past the box area
        if (m_data.style & GWS_CHECK_BOX) {
            float boxSize = static_cast<float>(m_data.h) - WIDGET_CHECKBOX_OFFSET;
            if (boxSize > 0) tx = static_cast<float>(m_data.x) + boxSize + WIDGET_TEXT_PADDING;
        }
        float ty;
        const float textScaleX = renderer.getTextLayoutScaleX();
        const float textScaleY = renderer.getTextLayoutScaleY();
        if (font) {
            const size_t lineCount = 1 + static_cast<size_t>(
                std::count(displayText.begin(), displayText.end(), '\n'));
            const float textH = static_cast<float>(font->getLineHeight()) *
                                textScaleY *
                                static_cast<float>(lineCount);
            ty = static_cast<float>(m_data.y) +
                 (static_cast<float>(m_data.h) - textH) * 0.5f;
        } else {
            ty = static_cast<float>(m_data.y + 4);
        }

        // Use hilite text color if hovered
        uint32_t textColor = COLOR_WHITE;
        if (m_hovered && m_data.style & (GWS_PUSH_BUTTON | GWS_CHECK_BOX | GWS_RADIO_BUTTON)) {
            textColor = m_data.hiliteTextColor;
            if (textColor == 0) textColor = COLOR_WHITE;
        }

        size_t lineStart = 0;
        size_t lineIndex = 0;
        while (lineStart <= displayText.size()) {
            const size_t newline = displayText.find('\n', lineStart);
            const size_t lineLength = newline == container::String::npos
                ? displayText.size() - lineStart : newline - lineStart;
            const container::String line = displayText.substr(lineStart, lineLength);
            float lineX = tx;
            if (m_data.textCentered && font) {
                const float textW =
                    static_cast<float>(font->getTextWidth(line)) *
                    textScaleX;
                lineX = static_cast<float>(m_data.x) +
                        (static_cast<float>(m_data.w) - textW) * 0.5f;
            }
            const float lineY = ty + (font
                ? static_cast<float>(lineIndex * font->getLineHeight()) *
                      textScaleY
                : static_cast<float>(lineIndex * 16));
            if (font)
                renderer.drawText(font, line, lineX, lineY, textColor);
            else
                renderer.drawText(line, lineX, lineY, textColor);
            ++lineIndex;
            if (newline == container::String::npos) break;
            lineStart = newline + 1;
        }
    }

    // Draw focus indicator
    if (m_focused && (m_data.style & GWS_ENTRY_FIELD)) {
        float dx = static_cast<float>(m_data.x);
        float dy = static_cast<float>(m_data.y);
        float dw = static_cast<float>(m_data.w);
        float dh = static_cast<float>(m_data.h);
        renderer.drawRect(dx, dy, dw, dh, COLOR_FOCUS_RING);  // Focus ring color
    }

    // Render text entry cursor
    if (isEntryField && m_focused) {
        auto* font = getFontForWidget(*this);

        float tx = static_cast<float>(m_data.x + WIDGET_TEXT_PADDING);
        float ty;
        const float textScaleX = renderer.getTextLayoutScaleX();
        const float textScaleY = renderer.getTextLayoutScaleY();
        if (font) {
            float textH = static_cast<float>(font->getLineHeight()) *
                          textScaleY;
            ty = static_cast<float>(m_data.y) + (static_cast<float>(m_data.h) - textH) * 0.5f;
        } else {
            ty = static_cast<float>(m_data.y + WIDGET_TEXT_PADDING);
        }

        if (font) {
            container::String textBeforeCursor = m_editText.substr(0, m_cursorPos);
            float cursorX = tx +
                static_cast<float>(font->getTextWidth(textBeforeCursor)) *
                    textScaleX;
            float cursorH = static_cast<float>(font->getLineHeight()) *
                            textScaleY;
            renderer.drawQuad(cursorX, ty, WIDGET_CURSOR_THICKNESS, cursorH, COLOR_WHITE);
        }
    }

    // Render listbox items
    if ((m_data.style & GWS_SCROLL_LISTBOX) && !m_items.empty()) {
        auto* font = getFontForWidget(*this);
        if (font) {
            
            float dx = static_cast<float>(m_data.x);
            float dy = static_cast<float>(m_data.y);
            float dw = static_cast<float>(m_data.w);
            float dh = static_cast<float>(m_data.h);

            int lineH = static_cast<int>(std::lround(
                static_cast<float>(font->getLineHeight()) *
                renderer.getTextLayoutScaleY()));
            if (lineH <= 0) lineH = 14;
            int maxVisible = static_cast<int>(dh) / lineH;
            if (maxVisible < 1) maxVisible = 1;

            // Draw highlighted row for selected item
            if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size())) {
                int visIdx = m_selectedIndex - m_scrollOffset;
                if (visIdx >= 0 && visIdx < maxVisible) {
                    float selY = dy + static_cast<float>(visIdx * lineH);
                    renderer.drawQuad(dx, selY, dw, static_cast<float>(lineH), COLOR_SELECTION_HIGHLIGHT);
                }
            }

            // Draw item text
            for (int i = 0; i < maxVisible && (i + m_scrollOffset) < static_cast<int>(m_items.size()); ++i) {
                int itemIdx = i + m_scrollOffset;
                const container::String& item = m_items[itemIdx];
                float textY = dy + static_cast<float>(i * lineH);
                uint32_t textColor = (itemIdx == m_selectedIndex) ? COLOR_WHITE : COLOR_LISTBOX_TEXT_NORMAL;
                renderer.drawText(font, item, dx + WIDGET_TEXT_PADDING, textY, textColor);
            }
        }
    }

    // Render combobox: arrow button + selected text + dropdown list
    // (Background/border already drawn by DrawComboBox draw function)
    if (m_data.style & GWS_COMBO_BOX) {
        float dx = static_cast<float>(m_data.x);
        float dy = static_cast<float>(m_data.y);
        float dw = static_cast<float>(m_data.w);
        float dh = static_cast<float>(m_data.h);
        

        const auto& hdd = info.hiliteDrawData;

        // ── Arrow button (right side, ~WIDGET_COMBO_ARROW_WIDTH wide) ───────────────────────
        float arrowW = WIDGET_COMBO_ARROW_WIDTH;
        float arrowX = dx + dw - arrowW;
        uint32_t arrowBg = m_hovered ? COLOR_COMBO_ARROW_HOVER : COLOR_COMBO_ARROW_NORMAL;
        renderer.drawQuad(arrowX, dy, arrowW, dh, arrowBg);
        renderer.drawRect(arrowX, dy, arrowW, dh, COLOR_COMBO_ARROW_BORDER);

        // Draw arrow COMBO_ARROW
        auto* font = getFontForWidget(*this);
        if (font) {
            renderer.drawText(
                font, COMBO_ARROW.data(),
                arrowX + WIDGET_COMBO_ARROW_OFFSET,
                dy + (dh - static_cast<float>(font->getLineHeight()) *
                               renderer.getTextLayoutScaleY()) * 0.5f,
                COLOR_WHITE);
        }

        // ── Selected text (in edit box area) ────────────────────────────
        if (!m_editText.empty() && font) {
            uint32_t textColor = m_data.textColor;
            if (textColor == 0) textColor = COLOR_WHITE;
            renderer.drawText(
                font, m_editText, dx + 4.0f,
                dy + (dh - static_cast<float>(font->getLineHeight()) *
                               renderer.getTextLayoutScaleY()) * 0.5f,
                textColor);
        }

        // ── Dropdown list (when open) ───────────────────────────────────
        if (m_dropdownOpen && !m_items.empty()) {
            float listY = dy + dh;
            int lineH = font
                ? static_cast<int>(std::lround(
                      static_cast<float>(font->getLineHeight()) *
                      renderer.getTextLayoutScaleY()))
                : WIDGET_LINE_HEIGHT;
            int maxVisible = WIDGET_COMBO_MAX_VISIBLE;
            int totalItems = static_cast<int>(m_items.size());
            int visibleCount = std::min(totalItems, maxVisible);
            float listH = static_cast<float>(visibleCount * lineH);

            // Clamp scroll offset
            int maxScroll = std::max(0, totalItems - maxVisible);
            m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);

            // Dropdown background
            renderer.drawQuad(dx, listY, dw, listH, COLOR_DROPDOWN_BG);
            renderer.drawRect(dx, listY, dw, listH, COLOR_DROPDOWN_BORDER);

            // Draw items (starting from scrollOffset)
            for (int i = 0; i < visibleCount; ++i) {
                int itemIdx = i + m_scrollOffset;
                float itemY = listY + static_cast<float>(i * lineH);
                bool isHighlighted = (m_highlightedItem == itemIdx);
                bool isSelected = (m_selectedIndex == itemIdx);

                // Highlight bar (for hovered or selected item)
                if (isHighlighted || isSelected) {
                    // Try to use hilite bar images from HILITEDRAWDATA slots 1-4
                    const container::String& leftImg = hdd[1].image;
                    const container::String& rightImg = hdd[2].image;
                    const container::String& centerImg = hdd[3].image;

                    bool drewHilite = false;
                    if (!leftImg.empty() && leftImg != "NoImage" &&
                        !centerImg.empty() && centerImg != "NoImage") {
                        auto leftResult = texMgr.findMappedImage(leftImg);
                        auto centerResult = texMgr.findMappedImage(centerImg);
                        auto rightResult = texMgr.findMappedImage(rightImg);

                        if (leftResult.found && leftResult.texture && centerResult.found && centerResult.texture) {
                            float leftW = static_cast<float>(leftResult.right - leftResult.left);
                            float centerW = static_cast<float>(centerResult.right - centerResult.left);
                            float rightW = (!rightImg.empty() && rightImg != "NoImage" && rightResult.found && rightResult.texture)
                                ? static_cast<float>(rightResult.right - rightResult.left) : 0;

                            // Draw left end
                            renderer.drawTextureRegion(leftResult.texture,
                                leftResult.left, leftResult.top, leftResult.right, leftResult.bottom,
                                leftResult.texW, leftResult.texH,
                                dx, itemY, leftW, static_cast<float>(lineH), COLOR_WHITE);

                            // Draw repeating center.  Guard the advance: a
                            // zero-width center image would never move cx and
                            // would spin the render thread forever.
                            float cx = dx + leftW;
                            float rightX = dx + dw - rightW;
                            while (centerW > 0.0f && cx < rightX) {
                                float pieceW = std::min(centerW, rightX - cx);
                                renderer.drawTextureRegion(centerResult.texture,
                                    centerResult.left, centerResult.top, centerResult.right, centerResult.bottom,
                                    centerResult.texW, centerResult.texH,
                                    cx, itemY, pieceW, static_cast<float>(lineH), COLOR_WHITE);
                                cx += centerW;
                            }

                            // Draw right end
                            if (rightW > 0) {
                                renderer.drawTextureRegion(rightResult.texture,
                                    rightResult.left, rightResult.top, rightResult.right, rightResult.bottom,
                                    rightResult.texW, rightResult.texH,
                                    rightX, itemY, rightW, static_cast<float>(lineH), COLOR_WHITE);
                            }
                            drewHilite = true;
                        }
                    }

                    // Fallback: colored rectangle
                    if (!drewHilite) {
                        uint32_t hiliteColor = hdd[0].color;
                        if (hiliteColor == 0 || hiliteColor == COLOR_WHITE) hiliteColor = COLOR_SELECTION_HIGHLIGHT;
                        renderer.drawQuad(dx, itemY, dw, static_cast<float>(lineH), hiliteColor);
                    }
                }

                // Item text
                if (font) {
                    uint32_t itemTextColor = m_data.textColor;
                    if (itemTextColor == 0) itemTextColor = (isHighlighted || isSelected) ? COLOR_WHITE : COLOR_LISTBOX_TEXT_NORMAL;
                    renderer.drawText(font, m_items[itemIdx], dx + WIDGET_TEXT_PADDING, itemY, itemTextColor);
                }
            }
        }
    }
}

void Widget::renderChildren(engine::Renderer& renderer, engine::TextureManager& texMgr, int depth) {
    for (auto& child : m_children) {
        child->render(renderer, texMgr, depth);
    }
}

// ── Tree builder ──────────────────────────────────────────────────────────

static container::UniquePtr<Widget> buildWidgetFromDef(const WndParser::WindowDef& def) {
    WndParser::WindowDef scaled = def;

    float scaleX = (def.creationResW > 0)
        ? static_cast<float>(engine::presentation_defaults::VIRTUAL_WIDTH) / static_cast<float>(def.creationResW)
        : 1.0f;
    float scaleY = (def.creationResH > 0)
        ? static_cast<float>(engine::presentation_defaults::VIRTUAL_HEIGHT) / static_cast<float>(def.creationResH)
        : 1.0f;

    if (scaleX != 1.0f || scaleY != 1.0f) {
        scaled.x = static_cast<int>(def.x * scaleX);
        scaled.y = static_cast<int>(def.y * scaleY);
        scaled.w = static_cast<int>(def.w * scaleX);
        scaled.h = static_cast<int>(def.h * scaleY);
    }

    auto widget = std::make_unique<Widget>(scaled);
    for (const auto& childDef : def.children) {
        widget->addChild(buildWidgetFromDef(childDef));
    }
    return widget;
}

container::UniquePtr<Widget> buildWidgetTree(const container::Vector<WndParser::WindowDef>& windows) {
    WndParser::WindowDef rootData;
    rootData.type = TYPE_ROOT;
    rootData.name = "Root";
    auto root = std::make_unique<Widget>(rootData);

    for (const auto& def : windows) {
        root->addChild(buildWidgetFromDef(def));
    }

    return root;
}

} // namespace gui
