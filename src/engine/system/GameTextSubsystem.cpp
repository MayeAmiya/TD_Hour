#include "core/container/container_types.h"
#include "GameTextSubsystem.h"
#include "debug/debug.h"
#include "StringTable.h"
#include "VFS.h"
#include "LocaleResourceLocator.h"

namespace {

void applyLocatorLanguage(engine::StringTable& strings,
                          container::StringView locale) noexcept {
    if (locale == "chinese") {
        strings.overrideLanguage(engine::LANGUAGE_ID_CHINESE);
    } else if (locale == "english") {
        strings.overrideLanguage(engine::LANGUAGE_ID_US);
    } else if (locale == "german") {
        strings.overrideLanguage(engine::LANGUAGE_ID_GERMAN);
    } else if (locale == "french") {
        strings.overrideLanguage(engine::LANGUAGE_ID_FRENCH);
    } else if (locale == "spanish") {
        strings.overrideLanguage(engine::LANGUAGE_ID_SPANISH);
    } else if (locale == "italian") {
        strings.overrideLanguage(engine::LANGUAGE_ID_ITALIAN);
    } else if (locale == "japanese") {
        strings.overrideLanguage(engine::LANGUAGE_ID_JAPANESE);
    } else if (locale == "korean") {
        strings.overrideLanguage(engine::LANGUAGE_ID_KOREAN);
    }
}

} // namespace

GameTextSubsystem::GameTextSubsystem() {
    setName("GameText");
}

GameTextSubsystem::~GameTextSubsystem() {
    shutdown();
}

void GameTextSubsystem::init() {
    TD_LOG_INFO("[GameText] Loading CSF string table...");

    auto& strings = engine::StringTable::instance();
    auto& vfs = io::VFS::instance();
    const auto locator = io::acquireLocaleResourceLocator();
    const std::optional<container::String> path = locator
        ? locator->resolve(io::LocaleResourceKind::Csf, "generals.csf")
        : std::optional<container::String>{};
    if (!path) {
        TD_LOG_WARN("[GameText] generals.csf was not found for the active locale");
        return;
    }
    container::Vector<uint8_t> bytes;
    if (!vfs.readToBuffer(*path, bytes) || bytes.empty()) {
        TD_LOG_WARN("[GameText] Could not read CSF winner '{}'", *path);
        return;
    }
    if (!strings.loadFromMemory(bytes.data(), bytes.size())) {
        TD_LOG_WARN("[GameText] Invalid CSF data in VFS file '{}'", *path);
        return;
    }
    applyLocatorLanguage(strings, locator->localeDirectory());
    m_loaded = true;
    TD_LOG_INFO("[GameText] Loaded {} strings from '{}'",
                strings.getStringCount(), *path);
}

void GameTextSubsystem::reset() {
    // StringTable persists across games
}

void GameTextSubsystem::shutdown() {
    m_loaded = false;
    TD_LOG_INFO("[GameText] Shutdown");
}
