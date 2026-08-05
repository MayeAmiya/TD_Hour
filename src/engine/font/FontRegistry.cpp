#include "core/container/container_types.h"
#include "FontRegistry.h"
#include "debug/debug.h"
#include "data/ini/IniFile.h"
#include "core/constants/Strings.h"
#include "core/constants/Paths.h"

#ifdef _WIN32
#include <Windows.h>
#endif

namespace engine {

FontRegistry& FontRegistry::instance() {
    static FontRegistry inst;
    return inst;
}

static bool fileExists(const container::String& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

container::String FontRegistry::resolvePath(const container::String& name) {
    // A Font remembers the resolved file that created it.  Renderer-side
    // output-density variants ask the registry for that same file at another
    // pixel size, so accept an already resolved path before treating `name`
    // as a family alias.
    if (fileExists(name)) return name;

    auto it = m_fontPaths.find(name);
    if (it != m_fontPaths.end() && fileExists(it->second)) {
        return it->second;
    }

#ifdef _WIN32
    char fontsDir[MAX_PATH];
    GetWindowsDirectoryA(fontsDir, MAX_PATH);
    container::String dir = container::String(fontsDir) + "\\Fonts\\";

    struct { const char* family; const char* file; } fontMap[] = {
        {FONT_ARIAL.data(),              FONT_FILE_ARIAL.data()},
        {FONT_ARIAL_BOLD.data(),         FONT_FILE_ARIAL_BOLD.data()},
        {FONT_ARIAL_ITALIC.data(),       FONT_FILE_ARIAL_ITALIC.data()},
        {FONT_ARIAL_BOLD_ITALIC.data(),  FONT_FILE_ARIAL_BOLD_ITALIC.data()},
        {FONT_TIMES_NEW_ROMAN.data(),    FONT_FILE_TIMES.data()},
        {FONT_TIMES_BOLD.data(),         FONT_FILE_TIMES_BOLD.data()},
        {FONT_COURIER_NEW.data(),        FONT_FILE_COURIER.data()},
        {FONT_COURIER_BOLD.data(),       FONT_FILE_COURIER_BOLD.data()},
        {FONT_PLACARD_MT.data(),         FONT_FILE_PLACARD_MT.data()},
        {FONT_ABADI_MT_BOLD.data(),      FONT_FILE_ABADI_MT_BOLD.data()},
        {FONT_GENERALS.data(),           FONT_FILE_GENERALS.data()},
        {FONT_GENERALS_BOLD.data(),      FONT_FILE_GENERALS.data()},
    };

    for (const auto& [family, file] : fontMap) {
        if (name == family) {
            container::String path = dir + file;
            if (fileExists(path)) return path;
        }
    }

    // Fallback: try name.ttf or name.ttc
    if (fileExists(dir + name + FONT_FILE_FALLBACK.data())) return dir + name + FONT_FILE_FALLBACK.data();
    if (fileExists(dir + name + ".ttc")) return dir + name + ".ttc";

    // Ultimate fallback: 微软雅黑
    if (fileExists(dir + FONT_FILE_CHINESE_FALLBACK.data())) return dir + FONT_FILE_CHINESE_FALLBACK.data();
#endif

    return "";
}

Font* FontRegistry::getFont(const container::String& name, int size, bool bold) {
    std::scoped_lock lock(m_mutex);
    FontKey key{name, size, bold};
    auto it = m_fonts.find(key);
    if (it != m_fonts.end()) {
        return it->second.get();
    }

    TD_LOG_INFO("[FontRegistry] Resolving '{}' (size={}, bold={})...", name, size, bold);
    container::String path = resolvePath(name);
    if (path.empty()) {
        TD_LOG_WARN("[FontRegistry] Font not found: '{}' (size={}, bold={})", name, size, bold);
        return nullptr;
    }
    TD_LOG_INFO("[FontRegistry] Found path: {}", path);

    auto font = std::make_unique<Font>();
    if (!font->load(nullptr, path, size, bold)) {
        TD_LOG_ERROR("[FontRegistry] Failed to load '{}' from {}", name, path);
        return nullptr;
    }

    TD_LOG_INFO("[FontRegistry] Loaded '{}' size={} from {}", name, size, path);
    Font* ptr = font.get();
    m_fonts.emplace(key, std::move(font));
    return ptr;
}

void FontRegistry::registerFontPath(const container::String& name, const container::String& path) {
    std::scoped_lock lock(m_mutex);
    m_fontPaths[name] = path;
}

void FontRegistry::loadFromIni(const container::String& iniPath) {
    // Read file content
    FILE* f = fopen(iniPath.c_str(), "rb");
    if (!f) {
        TD_LOG_WARN("[FontRegistry] Could not open {}", iniPath);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    container::String content(static_cast<size_t>(sz), '\0');
    fread(content.data(), 1, static_cast<size_t>(sz), f);
    fclose(f);

    data::IniFile ini;
    if (!ini.load(content)) {
        TD_LOG_WARN("[FontRegistry] Could not parse font mappings from {}", iniPath);
        return;
    }

    // Read [Fonts] section - each entry maps WND font name to TTF file name
    auto entryNames = ini.entries("Fonts");
    std::scoped_lock lock(m_mutex);
    for (const auto& entryName : entryNames) {
        auto val = ini.getString("Fonts", entryName, "");
        if (!val.empty()) {
            m_fontPaths[entryName] = val;
            TD_LOG_INFO("[FontRegistry] Font mapping: '{}' -> '{}'", entryName, val);
        }
    }
}

Font* FontRegistry::loadFont(const container::String& name, int size, bool bold, const container::String& path) {
    if (!path.empty()) {
        registerFontPath(name, path);
    }
    return getFont(name, size, bold);
}

void FontRegistry::clear() {
    std::scoped_lock lock(m_mutex);
    m_fonts.clear();
}

Font* FontRegistry::getCjkFallbackFont(int size) {
    std::scoped_lock lock(m_mutex);
    FontKey key{FONT_FILE_CHINESE_FALLBACK.data(), size, false};
    auto it = m_fonts.find(key);
    if (it != m_fonts.end()) return it->second.get();

    container::String path = resolvePath(FONT_FILE_CHINESE_FALLBACK.data());
    if (path.empty()) return nullptr;

    auto font = std::make_unique<Font>();
    if (!font->load(nullptr, path, size, false)) return nullptr;

    Font* ptr = font.get();
    m_fonts.emplace(key, std::move(font));
    TD_LOG_INFO("[FontRegistry] Loaded CJK fallback '{}' size={}", FONT_FILE_CHINESE_FALLBACK, size);
    return ptr;
}

} // namespace engine
