#pragma once

#include "core/container/container_types.h"

#include <cstdint>
#include <initializer_list>
#include "../../../core/constants/Colors.h"
#include "engine/gui/base/GuiDefaults.h"

namespace engine { struct RawTexture; }

namespace gui {

namespace detail {

// True iff every value is a single bit and no two values share a bit.  Used by
// the static_asserts under each flag enum below.
template <typename T>
constexpr bool areDisjointBits(std::initializer_list<T> values) {
    T seen = 0;
    for (T v : values) {
        if (v == 0) return false;                            // not a flag
        if ((v & static_cast<T>(v - 1)) != 0) return false;   // not a single bit
        if ((seen & v) != 0) return false;                    // collides
        seen = static_cast<T>(seen | v);
    }
    return true;
}

} // namespace detail

// Window states
enum WindowState : uint32_t {
    WIN_STATE_HILITED    = 0x0001,
    WIN_STATE_SELECTED   = 0x0002,
    WIN_STATE_LEFT_DOWN  = 0x0004,
    WIN_STATE_RIGHT_DOWN = 0x0008,
};

static_assert(detail::areDisjointBits<uint32_t>({
                  WIN_STATE_HILITED,
                  WIN_STATE_SELECTED,
                  WIN_STATE_LEFT_DOWN,
                  WIN_STATE_RIGHT_DOWN,
              }),
              "WindowState enumerators must each occupy a distinct single bit");

// Window status flags
//
// Every bit of the low 32 bits is now assigned (bits 8, 13 and 15 were the only
// free ones and are taken by the ON_KEY_* flags below).  The underlying type is
// uint64_t so new flags can be added above bit 31; never reuse an existing bit,
// the static_assert below enforces that.
enum WindowStatus : uint64_t {
    WIN_STATUS_ACTIVE             = 0x00000001,
    WIN_STATUS_TOGGLE             = 0x00000002,
    WIN_STATUS_DRAGABLE           = 0x00000004,
    WIN_STATUS_ENABLED            = 0x00000008,
    WIN_STATUS_HIDDEN             = 0x00000010,
    WIN_STATUS_ABOVE              = 0x00000020,
    WIN_STATUS_BELOW              = 0x00000040,
    WIN_STATUS_IMAGE              = 0x00000080,
    WIN_STATUS_NO_INPUT           = 0x00000200,
    WIN_STATUS_NO_FOCUS           = 0x00000400,
    WIN_STATUS_DESTROYED          = 0x00000800,
    WIN_STATUS_BORDER             = 0x00001000,
    WIN_STATUS_ONE_LINE           = 0x00004000,
    WIN_STATUS_SEE_THRU           = 0x00010000,
    WIN_STATUS_RIGHT_CLICK        = 0x00020000,
    WIN_STATUS_LEFT_CLICK         = 0x00040000,
    WIN_STATUS_CHECK_LIKE         = 0x00080000,
    WIN_STATUS_ONE_MODAL          = 0x00100000,
    WIN_STATUS_ALWAYS_GAME_ACTIVE = 0x00200000,
    WIN_STATUS_NOT_READY          = 0x00400000,
    WIN_STATUS_FLASH              = 0x00800000,
    WIN_STATUS_MOUSE_ESCAPABLE    = 0x01000000,
    WIN_STATUS_ON_MOUSE_DOWN      = 0x02000000,
    WIN_STATUS_ON_MOUSE_UP        = 0x04000000,
    WIN_STATUS_ON_MOUSE_DRAG      = 0x08000000,
    WIN_STATUS_ON_MOUSE_ENTER     = 0x10000000,
    WIN_STATUS_ON_MOUSE_LEAVE     = 0x20000000,
    WIN_STATUS_ON_CHAR            = 0x40000000,
    WIN_STATUS_KEYABLE            = 0x80000000,
    // The five flags below aliased ACTIVE/TOGGLE/DRAGABLE/ENABLED/HIDDEN
    // (0x1/0x2/0x4/0x8/0x10), so a .wnd authoring "ON_KEY_UP_IM" silently
    // hid the window and "ON_KEY_DOWN_IM" silently force-enabled it (see
    // WndParser::parseStatus).  Moved off the colliding bits; the aliased
    // flags keep their original values because authored data depends on them.
    // Bits 8, 13 and 15 (0x100/0x2000/0x8000) were the only free bits in the
    // low word, so they are used first -- anything that still truncates a
    // status to 32 bits keeps these three.
    WIN_STATUS_ON_KEY_DOWN        = 0x00000100,          // moved off 0x1 (ACTIVE)
    WIN_STATUS_ON_KEY_UP          = 0x00002000,          // moved off 0x2 (TOGGLE)
    WIN_STATUS_ON_CHAR_IM         = 0x00008000,          // moved off 0x4 (DRAGABLE)
    // The low word is now full, so the remaining two moved flags live above
    // bit 31 -- this is why WindowStatus is a 64-bit enum.
    WIN_STATUS_ON_KEY_DOWN_IM     = 0x0000000100000000ull, // moved off 0x8 (ENABLED)
    WIN_STATUS_ON_KEY_UP_IM       = 0x0000000200000000ull, // moved off 0x10 (HIDDEN)
    // The legacy ControlBar sets this dynamically on command, science and
    // upgrade cameos.  Keep it distinct from ALWAYS_GAME_ACTIVE, which already
    // occupies the legacy numeric bit in this runtime's extended status set.
    WIN_STATUS_USE_OVERLAY_STATES = 0x0000000400000000ull,
    WIN_STATUS_ALWAYS_COLOR       = 0x0000000800000000ull,
};

// Regression fence: five of these enumerators used to alias five others, and
// nothing caught it.  Adding a flag that reuses a bit now fails to compile.
// Keep this list in sync with the enum above.
static_assert(detail::areDisjointBits<uint64_t>({
                  WIN_STATUS_ACTIVE,
                  WIN_STATUS_TOGGLE,
                  WIN_STATUS_DRAGABLE,
                  WIN_STATUS_ENABLED,
                  WIN_STATUS_HIDDEN,
                  WIN_STATUS_ABOVE,
                  WIN_STATUS_BELOW,
                  WIN_STATUS_IMAGE,
                  WIN_STATUS_NO_INPUT,
                  WIN_STATUS_NO_FOCUS,
                  WIN_STATUS_DESTROYED,
                  WIN_STATUS_BORDER,
                  WIN_STATUS_ONE_LINE,
                  WIN_STATUS_SEE_THRU,
                  WIN_STATUS_RIGHT_CLICK,
                  WIN_STATUS_LEFT_CLICK,
                  WIN_STATUS_CHECK_LIKE,
                  WIN_STATUS_ONE_MODAL,
                  WIN_STATUS_ALWAYS_GAME_ACTIVE,
                  WIN_STATUS_NOT_READY,
                  WIN_STATUS_FLASH,
                  WIN_STATUS_MOUSE_ESCAPABLE,
                  WIN_STATUS_ON_MOUSE_DOWN,
                  WIN_STATUS_ON_MOUSE_UP,
                  WIN_STATUS_ON_MOUSE_DRAG,
                  WIN_STATUS_ON_MOUSE_ENTER,
                  WIN_STATUS_ON_MOUSE_LEAVE,
                  WIN_STATUS_ON_CHAR,
                  WIN_STATUS_KEYABLE,
                  WIN_STATUS_ON_KEY_DOWN,
                  WIN_STATUS_ON_KEY_UP,
                  WIN_STATUS_ON_CHAR_IM,
                  WIN_STATUS_ON_KEY_DOWN_IM,
                  WIN_STATUS_ON_KEY_UP_IM,
                  WIN_STATUS_USE_OVERLAY_STATES,
                  WIN_STATUS_ALWAYS_COLOR,
              }),
              "WindowStatus enumerators must each occupy a distinct single bit");

// Gadget style flags
enum GadgetStyle : uint32_t {
    GWS_PUSH_BUTTON     = 0x0001,
    GWS_RADIO_BUTTON    = 0x0002,
    GWS_CHECK_BOX       = 0x0004,
    GWS_VERT_SLIDER     = 0x0008,
    GWS_HORZ_SLIDER     = 0x0010,
    GWS_SCROLL_LISTBOX  = 0x0020,
    GWS_ENTRY_FIELD     = 0x0040,
    GWS_STATIC_TEXT     = 0x0080,
    GWS_PROGRESS_BAR    = 0x0100,
    GWS_USER_WINDOW     = 0x0200,
    GWS_MOUSE_TRACK     = 0x0400,
    GWS_ANIMATED        = 0x0800,
    GWS_TAB_CONTROL     = 0x2000,
    GWS_TAB_PANE        = 0x4000,
    GWS_COMBO_BOX       = 0x8000,
    GWS_MULTILINETEXT   = 0x00001000,
    // Moved off 0x00002000, which collided with GWS_TAB_CONTROL: parseStyle
    // maps the authored tokens "form" and "tabcontrol" onto these two, so any
    // .wnd authoring STYLE: FORM also read as a tab control (and vice versa).
    // GWS_FORM is the safer one to move: GWS_TAB_CONTROL sits in the
    // contiguous 0x2000/0x4000/0x8000 TAB_CONTROL/TAB_PANE/COMBO_BOX block
    // that mirrors the original control-type bit layout, and GWS_COMBO_BOX is
    // tested all over the widget/runtime code, whereas GWS_FORM has no reader
    // at all -- only parseStyle produces it.  0x00010000 is the first free bit
    // above that block and below the generic-style block at 0x00100000.
    GWS_FORM            = 0x00010000,
    // Generic styles
    GWS_STYLE_IMAGE     = 0x00100000,
    GWS_STYLE_BORDER    = 0x00200000,
};

// Same regression fence as for WindowStatus, for the GWS_FORM/GWS_TAB_CONTROL
// collision.  Keep this list in sync with the enum above.
static_assert(detail::areDisjointBits<uint32_t>({
                  GWS_PUSH_BUTTON,
                  GWS_RADIO_BUTTON,
                  GWS_CHECK_BOX,
                  GWS_VERT_SLIDER,
                  GWS_HORZ_SLIDER,
                  GWS_SCROLL_LISTBOX,
                  GWS_ENTRY_FIELD,
                  GWS_STATIC_TEXT,
                  GWS_PROGRESS_BAR,
                  GWS_USER_WINDOW,
                  GWS_MOUSE_TRACK,
                  GWS_ANIMATED,
                  GWS_TAB_CONTROL,
                  GWS_TAB_PANE,
                  GWS_COMBO_BOX,
                  GWS_MULTILINETEXT,
                  GWS_FORM,
                  GWS_STYLE_IMAGE,
                  GWS_STYLE_BORDER,
              }),
              "GadgetStyle enumerators must each occupy a distinct single bit");

// Draw data for a single visual state
struct WinDrawData {
    const engine::RawTexture* image = nullptr;
    uint32_t color = COLOR_WHITE;
    uint32_t borderColor = COLOR_TRANSPARENT;
};

// Window instance data
struct WinInstanceData {
    uint32_t id = 0;                        // Window ID (hash)
    uint32_t state = 0;                     // WIN_STATE_* flags
    uint32_t style = 0;                     // GWS_* style flags

    // 3 visual states × 9 draw data slots
    static constexpr int MAX_DRAW_SLOTS = 9;
    container::Array<WinDrawData, MAX_DRAW_SLOTS> enabledDrawData;
    container::Array<WinDrawData, MAX_DRAW_SLOTS> disabledDrawData;
    container::Array<WinDrawData, MAX_DRAW_SLOTS> hiliteDrawData;

    // Text colors per state
    uint32_t enabledTextColor = COLOR_WHITE;
    uint32_t disabledTextColor = COLOR_DISABLED_TEXT;
    uint32_t hiliteTextColor = COLOR_HILITE_TEXT;

    // Font
    int font = 0;
    container::String fontName;
    int fontSize = ::gui::defaults::FONT_SIZE;
    bool fontBold = false;

    // Text labels (localized via TheGameText)
    container::String textLabelString;
    container::String decoratedNameString;    // "Filename.wnd:WidgetName"
    container::String tooltipString;
    int tooltipDelay = 0;
};

} // namespace gui
