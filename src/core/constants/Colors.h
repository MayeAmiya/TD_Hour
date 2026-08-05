#pragma once

#include <cstdint>

// ── Default / Neutral Colors ────────────────────────────────────────────────

static constexpr uint32_t COLOR_WHITE       = 0xFFFFFFFF;
static constexpr uint32_t COLOR_BLACK       = 0x00000000;
static constexpr uint32_t COLOR_TRANSPARENT = 0x00000000;
static constexpr uint32_t COLOR_CLEAR       = 0x00000000;

// ── Widget Default Colors ───────────────────────────────────────────────────

static constexpr uint32_t COLOR_DEFAULT_TINT        = 0x80808080;
static constexpr uint32_t COLOR_DEFAULT_BORDER      = 0xFFFFFFFF;
static constexpr uint32_t COLOR_DEFAULT_TEXT        = 0xFFFFFFFF;
static constexpr uint32_t COLOR_DISABLED_TEXT       = 0xFF808080;
static constexpr uint32_t COLOR_HILITE_TEXT         = 0xFFFFFF00;
static constexpr uint32_t COLOR_SELECTED            = 0xFF00FF00;
static constexpr uint32_t COLOR_WIDGET_BACKGROUND   = 0x00000000;
static constexpr uint32_t COLOR_THUMB_DEFAULT       = 0xFF808080;

// ── Focus & Selection ───────────────────────────────────────────────────────

static constexpr uint32_t COLOR_FOCUS_RING          = 0xFF00AAFF;
static constexpr uint32_t COLOR_SELECTION_HIGHLIGHT = 0x400070C0;
static constexpr uint32_t COLOR_LISTBOX_TEXT_NORMAL = 0xFFCCCCCC;

// ── ComboBox ────────────────────────────────────────────────────────────────

static constexpr uint32_t COLOR_COMBO_ARROW_HOVER   = 0xFF555555;
static constexpr uint32_t COLOR_COMBO_ARROW_NORMAL  = 0xFF333333;
static constexpr uint32_t COLOR_COMBO_ARROW_BORDER  = 0xFF808080;
static constexpr uint32_t COLOR_DROPDOWN_BG         = 0xE01A1A2E;
static constexpr uint32_t COLOR_DROPDOWN_BORDER     = 0xFF808080;

// ── Skeleton Mode Colors ────────────────────────────────────────────────────

static constexpr uint32_t COLOR_SKELETON_DEFAULT    = 0x808080FF;
static constexpr uint32_t COLOR_SKELETON_PUSHBUTTON = 0x4040C0FF;
static constexpr uint32_t COLOR_SKELETON_CHECKBOX   = 0x40C040FF;
static constexpr uint32_t COLOR_SKELETON_RADIOBUTTON= 0xC04040FF;
static constexpr uint32_t COLOR_SKELETON_STATICTEXT = 0xC0C040FF;
static constexpr uint32_t COLOR_SKELETON_USER       = 0x606060FF;

// ── Fallback Colors ─────────────────────────────────────────────────────────

static constexpr uint32_t COLOR_MAINMENU_BACKDROP_FALLBACK = 0xFF1A0A2E;

// ── Placeholder Texture ─────────────────────────────────────────────────────

static constexpr uint32_t COLOR_PLACEHOLDER_BLACK   = 0xFFFFFFFF;  // ABGR: A=255, B=0, G=0, R=0
static constexpr uint32_t COLOR_PLACEHOLDER_MAGENTA = 0xFFFF00FF;  // ABGR: A=255, B=0, G=0, R=255

// ── Font Glyph Mask ─────────────────────────────────────────────────────────

static constexpr uint32_t FONT_GLYPH_MASK_RGB = 0x00FFFFFF;
