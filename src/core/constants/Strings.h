#pragma once

#include "container/container_types.h"
// ── Font Names ──────────────────────────────────────────────────────────────

static constexpr container::StringView FONT_ARIAL           = "Arial";
static constexpr container::StringView FONT_ARIAL_BOLD      = "Arial Bold";
static constexpr container::StringView FONT_ARIAL_ITALIC    = "Arial Italic";
static constexpr container::StringView FONT_ARIAL_BOLD_ITALIC = "Arial Bold Italic";
static constexpr container::StringView FONT_TIMES_NEW_ROMAN = "Times New Roman";
static constexpr container::StringView FONT_TIMES_BOLD      = "Times New Roman Bold";
static constexpr container::StringView FONT_COURIER_NEW     = "Courier New";
static constexpr container::StringView FONT_COURIER_BOLD    = "Courier New Bold";
static constexpr container::StringView FONT_PLACARD_MT      = "Placard MT Condensed";
static constexpr container::StringView FONT_ABADI_MT_BOLD   = "Abadi MT Bold";
static constexpr container::StringView FONT_GENERALS        = "Generals";
static constexpr container::StringView FONT_GENERALS_BOLD   = "Generals Bold";

// ── Font Filenames ──────────────────────────────────────────────────────────

static constexpr container::StringView FONT_FILE_ARIAL           = "arial.ttf";
static constexpr container::StringView FONT_FILE_ARIAL_BOLD      = "arialbd.ttf";
static constexpr container::StringView FONT_FILE_ARIAL_ITALIC    = "ariali.ttf";
static constexpr container::StringView FONT_FILE_ARIAL_BOLD_ITALIC = "arialbi.ttf";
static constexpr container::StringView FONT_FILE_TIMES           = "times.ttf";
static constexpr container::StringView FONT_FILE_TIMES_BOLD      = "timesbd.ttf";
static constexpr container::StringView FONT_FILE_COURIER         = "cour.ttf";
static constexpr container::StringView FONT_FILE_COURIER_BOLD    = "courbd.ttf";
static constexpr container::StringView FONT_FILE_PLACARD_MT      = "pltcd.ttf";
static constexpr container::StringView FONT_FILE_ABADI_MT_BOLD   = "abdio.ttf";
static constexpr container::StringView FONT_FILE_GENERALS        = "GenArial.ttf";
static constexpr container::StringView FONT_FILE_FALLBACK        = "arial.ttf";
static constexpr container::StringView FONT_FILE_CHINESE_FALLBACK = "msyh.ttc";

// ── Magic Strings ───────────────────────────────────────────────────────────

/// Sentinel string meaning "no image for this draw slot"
static constexpr container::StringView NO_IMAGE = "NoImage";

// ── Faction Button Names ────────────────────────────────────────────────────

static constexpr container::StringView BTN_USA    = "ButtonUSA";
static constexpr container::StringView BTN_GLA    = "ButtonGLA";
static constexpr container::StringView BTN_CHINA  = "ButtonChina";

// ── Faction Names ───────────────────────────────────────────────────────────

static constexpr container::StringView FACTION_USA    = "USA";
static constexpr container::StringView FACTION_GLA    = "GLA";
static constexpr container::StringView FACTION_CHINA  = "China";

// ── ComboBox ────────────────────────────────────────────────────────────────

static constexpr container::StringView COMBO_ARROW = "v";

// ── Color Names (for WndParser color map) ───────────────────────────────────

static constexpr container::StringView COLOR_NAME_BLACK      = "black";
static constexpr container::StringView COLOR_NAME_WHITE      = "white";
static constexpr container::StringView COLOR_NAME_RED        = "red";
static constexpr container::StringView COLOR_NAME_GREEN      = "green";
static constexpr container::StringView COLOR_NAME_BLUE       = "blue";
static constexpr container::StringView COLOR_NAME_YELLOW     = "yellow";
static constexpr container::StringView COLOR_NAME_CYAN       = "cyan";
static constexpr container::StringView COLOR_NAME_MAGENTA    = "magenta";
static constexpr container::StringView COLOR_NAME_ORANGE     = "orange";
static constexpr container::StringView COLOR_NAME_PURPLE     = "purple";
static constexpr container::StringView COLOR_NAME_GRAY       = "gray";
static constexpr container::StringView COLOR_NAME_GREY       = "grey";
static constexpr container::StringView COLOR_NAME_SILVER     = "silver";
static constexpr container::StringView COLOR_NAME_MAROON     = "maroon";
static constexpr container::StringView COLOR_NAME_OLIVE      = "olive";
static constexpr container::StringView COLOR_NAME_LIME       = "lime";
static constexpr container::StringView COLOR_NAME_TEAL       = "teal";
static constexpr container::StringView COLOR_NAME_NAVY       = "navy";
