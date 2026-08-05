#include "FontLibrarySubsystem.h"
#include "debug/debug.h"
#include "FontRegistry.h"
#include "Renderer.h"
#include "GlobalData.h"
#include "core/constants/Paths.h"
#include "core/constants/Strings.h"

FontLibrarySubsystem::FontLibrarySubsystem() {
    setName("FontLibrary");
}

FontLibrarySubsystem::~FontLibrarySubsystem() {
    shutdown();
}

void FontLibrarySubsystem::init() {
    TD_LOG_INFO("[FontLibrary] Preloading fonts...");

    auto& fontReg = engine::FontRegistry::instance();

    // Follow the exact config selected by FileSystemSubsystem.  A release
    // executable is normally launched from Bin/<configuration>, whereas the
    // install-wide GameOptions.ini resides at the product root.
    const container::String& configPath =
        config::TheGlobalData.getLoadedConfigPath();
    fontReg.loadFromIni(configPath.empty()
        ? container::String{GAME_OPTIONS_INI.data()}
        : configPath);

    fontReg.loadFont(FONT_ARIAL.data(), 10, false, "");
    fontReg.loadFont(FONT_ARIAL.data(), 12, false, "");
    fontReg.loadFont(FONT_ARIAL.data(), 14, false, "");
    fontReg.loadFont(FONT_ARIAL.data(), 12, true, "");
    fontReg.loadFont(FONT_TIMES_NEW_ROMAN.data(), 14, false, "");
    fontReg.loadFont(FONT_TIMES_NEW_ROMAN.data(), 18, false, "");
    fontReg.loadFont(FONT_GENERALS.data(), 15, false, "");
    fontReg.loadFont(FONT_GENERALS.data(), 20, false, "");

    TD_LOG_INFO("[FontLibrary] Initialized");
}

void FontLibrarySubsystem::reset() {
    // Fonts persist across games
}

void FontLibrarySubsystem::shutdown() {
    TD_LOG_INFO("[FontLibrary] Shutdown");
}
