#include "container/container_types.h"
#include "GlobalData.h"
#include "debug/debug.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include "core/constants/Paths.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace config {

namespace fs = std::filesystem;

namespace {

// Process-lifetime owner. Legacy pointer/reference views both bind to this
// stable object, so static initialization never dereferences null and
// subsystem teardown cannot leave a dangling configuration address.
GlobalData g_globalData;

} // namespace

GlobalData* TheWritableGlobalData = &g_globalData;
const GlobalData& TheGlobalData = g_globalData;

namespace {

constexpr container::StringView kMapSearchPathPrefix = "mapsearchpath";
constexpr container::StringView kMapPathPrefix = "mappath";
constexpr container::StringView kLocalMapPathPrefix = "localmappath";

} // namespace

static container::String toLowerStr(const container::String& s) {
    container::String r = s;
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

static container::String trimStr(const container::String& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == container::String::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool parseBoolValue(const container::String& value, bool fallback) {
    const container::String normalized = toLowerStr(trimStr(value));
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    return fallback;
}

static container::Vector<container::String> splitStr(const container::String& value, char delimiter) {
    container::Vector<container::String> parts;
    std::stringstream stream(value);
    container::String part;
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(trimStr(part));
    }
    return parts;
}

static bool parseMapPathKind(const container::String& value) {
    const container::String lower = toLowerStr(trimStr(value));
    return lower.empty() || lower == "multiplayer" || lower == "skirmish" || lower == "lan";
}

static void addMapSearchPath(container::Vector<GlobalData::MapSearchPath>& paths, const container::String& value) {
    const auto parts = splitStr(value, '|');
    if (parts.empty() || parts[0].empty()) return;

    GlobalData::MapSearchPath path;
    path.vfsPath = parts[0];
    if (parts.size() > 1) {
        path.multiplayer = parseMapPathKind(parts[1]);
    }
    paths.push_back(std::move(path));
}

static void addLocalMapPath(container::Vector<GlobalData::LocalMapPath>& paths, const container::String& value) {
    const auto parts = splitStr(value, '|');
    if (parts.empty() || parts[0].empty()) return;

    GlobalData::LocalMapPath path;
    path.sourcePath = parts[0];
    if (parts.size() > 1) {
        path.vfsRoot = parts[1];
    }
    if (path.vfsRoot.empty()) {
        path.vfsRoot = "user/maps/" + std::to_string(paths.size());
    } else {
        container::String normalizedRoot = toLowerStr(path.vfsRoot);
        std::replace(normalizedRoot.begin(), normalizedRoot.end(), '\\', '/');
        while (!normalizedRoot.empty() && normalizedRoot.front() == '/') {
            normalizedRoot.erase(normalizedRoot.begin());
        }
        while (!normalizedRoot.empty() && normalizedRoot.back() == '/') {
            normalizedRoot.pop_back();
        }
        if (normalizedRoot == "userdata/maps") {
            normalizedRoot = "user/maps";
        } else if (normalizedRoot.starts_with("userdata/maps/")) {
            normalizedRoot.replace(0, container::String{"userdata/maps"}.size(),
                                   "user/maps");
        } else if (normalizedRoot != "user/maps" &&
                   !normalizedRoot.starts_with("user/maps/")) {
            // LocalMapPath describes user-authored map content.  Keeping an
            // authored `maps/...` root would merge it with installed maps and
            // make the VFS winner lose its source identity.
            if (normalizedRoot == "maps") {
                normalizedRoot = "user/maps";
            } else if (normalizedRoot.starts_with("maps/")) {
                normalizedRoot = "user/" + normalizedRoot;
            } else {
                normalizedRoot = "user/maps/" + normalizedRoot;
            }
        }
        path.vfsRoot = std::move(normalizedRoot);
    }
    if (parts.size() > 2) {
        path.multiplayer = parseMapPathKind(parts[2]);
    }
    paths.push_back(std::move(path));
}

void GlobalData::loadFromIni(const container::String& filename) {
    m_generalsDataPath.clear();
    m_zeroHourDataPath.clear();
    m_modDataPath.clear();
    m_localeDataPath.clear();
    m_userDataPath.clear();
    m_saveDataPath.clear();
    m_replayDataPath.clear();
    m_loadedConfigPath.clear();
    m_mapSearchPaths.clear();
    m_localMapPaths.clear();

    const fs::path requestedPath{filename};
    std::ifstream ini(requestedPath);
    const auto recordLoadedPath = [this](const fs::path& path) {
        std::error_code error;
        const fs::path resolved = fs::absolute(path, error);
        m_loadedConfigPath = (error ? path : resolved).lexically_normal().string();
    };
    if (ini.is_open()) recordLoadedPath(requestedPath);
    if (!ini.is_open()) {
        // Direct release builds live under Bin/<configuration>, while the
        // user-owned GameOptions.ini lives at the installation root.  First
        // preserve the conventional beside-exe location, then probe the two
        // explicit parent levels of that layout.  This is based on the
        // executable path, never the caller's cwd, so a shortcut or launcher
        // cannot accidentally mount an unrelated content tree.
#ifdef _WIN32
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
            const fs::path executableDirectory =
                fs::path(exePath).parent_path();
            if (requestedPath.is_relative()) {
                const container::Array<fs::path, 3> candidates{{
                    executableDirectory / requestedPath,
                    executableDirectory.parent_path() / requestedPath,
                    executableDirectory.parent_path().parent_path() /
                        requestedPath,
                }};
                for (const fs::path& candidate : candidates) {
                    ini.clear();
                    ini.open(candidate);
                    if (ini.is_open()) {
                        recordLoadedPath(candidate);
                        TD_LOG_INFO(
                            "[GlobalData] Found config via executable layout: {}",
                            m_loadedConfigPath);
                        break;
                    }
                }
            }
        }
#endif
        if (!ini.is_open()) {
            TD_LOG_WARN("[GlobalData] Config file not found: {}", filename);
            return;
        }
    }

    container::String line, curSection;
    while (std::getline(ini, line)) {
        container::String trimmed = trimStr(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;

        if (trimmed[0] == '[' && trimmed.back() == ']') {
            curSection = toLowerStr(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }

        size_t eq = trimmed.find('=');
        if (eq == container::String::npos) continue;

        container::String key = toLowerStr(trimStr(trimmed.substr(0, eq)));
        container::String value = trimStr(trimmed.substr(eq + 1));

        if (curSection == SECTION_GAME.data()) {
            if (key == KEY_GENERALS_DATAPATH.data()) {
                m_generalsDataPath = value;
            } else if (key == KEY_ZEROHOUR_DATAPATH.data()) {
                m_zeroHourDataPath = value;
            } else if (key == KEY_MOD_DATAPATH.data()) {
                m_modDataPath = value;
            }
            else if (key == KEY_LOCALE_DATAPATH.data()) m_localeDataPath = value;
            else if (key == "userdatapath") m_userDataPath = value;
            else if (key == "savedatapath" || key == "savepath") m_saveDataPath = value;
            else if (key == "replaydatapath" || key == "replaypath") m_replayDataPath = value;
            else if (key == "adjustclifftextures") {
                m_adjustCliffTextures = parseBoolValue(value, m_adjustCliffTextures);
            }
            else if (key == kMapSearchPathPrefix || key == "mapsearchpaths" ||
                     (key.starts_with(kMapSearchPathPrefix) &&
                      key.size() > kMapSearchPathPrefix.size())) {
                addMapSearchPath(m_mapSearchPaths, value);
            } else if (key == "mappath" || key == "mappaths" ||
                       key == "localmappath" || key == "localmappaths" ||
                       (key.starts_with(kMapPathPrefix) &&
                        key.size() > kMapPathPrefix.size()) ||
                       (key.starts_with(kLocalMapPathPrefix) &&
                        key.size() > kLocalMapPathPrefix.size())) {
                addLocalMapPath(m_localMapPaths, value);
            }
        }
    }

    TD_LOG_INFO("[GlobalData] Loaded config: GeneralsDataPath='{}', ZeroHourDataPath='{}', ModDataPath='{}', LocaleDataPath='{}', UserDataPath='{}', SaveDataPath='{}', ReplayDataPath='{}', MapSearchPaths={}, LocalMapPaths={}",
        m_generalsDataPath, m_zeroHourDataPath, m_modDataPath,
        m_localeDataPath, m_userDataPath, m_saveDataPath, m_replayDataPath,
        m_mapSearchPaths.size(), m_localMapPaths.size());
}

GlobalData::GlobalData() {
    // Initialize vertex water arrays
    m_vertexWaterHeightClampLow.fill(0.0f);
    m_vertexWaterHeightClampHi.fill(100.0f);
    m_vertexWaterAngle.fill(0.0f);
    m_vertexWaterXPosition.fill(0.0f);
    m_vertexWaterYPosition.fill(0.0f);
    m_vertexWaterZPosition.fill(0.0f);
    m_vertexWaterXGridCells.fill(32);
    m_vertexWaterYGridCells.fill(32);
    m_vertexWaterGridSize.fill(10.0f);
    m_vertexWaterAttenuationA.fill(0.0f);
    m_vertexWaterAttenuationB.fill(1.0f);
    m_vertexWaterAttenuationC.fill(0.0f);
    m_vertexWaterAttenuationRange.fill(100.0f);

    // Initialize health bonus
    m_healthBonus[0] = 0.0f;  // Rookie
    m_healthBonus[1] = 0.1f;  // Veteran
    m_healthBonus[2] = 0.2f;  // Elite
    m_healthBonus[3] = 0.3f;  // Heroic

    // Initialize solo player health bonus
    for (int p = 0; p < PLAYERTYPE_COUNT; ++p) {
        for (int d = 0; d < DIFFICULTY_COUNT; ++d) {
            m_soloPlayerHealthBonusForDifficulty[p][d] = 0.0f;
        }
    }
}

bool GlobalData::setTimeOfDay(TimeOfDay tod) {
    if (tod >= TimeOfDay::Count) {
        return false;
    }
    m_timeOfDay = tod;
    return true;
}

} // namespace config
