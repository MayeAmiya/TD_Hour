#include "core/container/hash_containers.h"
#include "game/base/MapCache.h"
#include "game/base/MapContentIdentity.h"
#include "debug/debug.h"
#include "VFS.h"
#include "GlobalData.h"
#include <algorithm>
#include <limits>
namespace game {

namespace {

container::String normalizePath(container::String value) {
    return canonicalMapSourcePath(value);
}

container::String recursiveMapPattern(container::StringView rootOrPattern) {
    container::String pattern = normalizePath(container::String(rootOrPattern));
    if (pattern.empty() || pattern.find_first_of("*?") != container::String::npos) {
        return pattern;
    }
    while (!pattern.empty() && pattern.back() == '/') pattern.pop_back();
    return pattern + "/*.map";
}

bool hasMapExtension(const container::String& path) {
    const container::String lowerPath = normalizePath(path);
    return lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == ".map";
}

container::String mapDisplayNameFromPath(const container::String& path) {
    size_t lastSlash = path.find_last_of('/');
    size_t lastBackslash = path.find_last_of('\\');
    size_t startPos = (lastSlash != container::String::npos) ? lastSlash + 1 :
                      (lastBackslash != container::String::npos) ? lastBackslash + 1 : 0;
    size_t dotPos = path.rfind('.');
    if (dotPos != container::String::npos && dotPos > startPos) {
        return path.substr(startPos, dotPos - startPos);
    }
    return path.substr(startPos);
}

container::String mapBasename(container::StringView path) {
    container::String normalized = normalizePath(container::String(path));
    const size_t slash = normalized.find_last_of('/');
    if (slash != container::String::npos) {
        normalized.erase(0, slash + 1);
    }
    return normalized;
}

container::String mapBasenameWithoutExtension(container::StringView path) {
    container::String name = mapBasename(path);
    const size_t dot = name.rfind('.');
    if (dot != container::String::npos) {
        name.erase(dot);
    }
    return name;
}

} // namespace

MapCache* TheMapCache = nullptr;

MapCache& MapCache::instance() {
    static MapCache s_instance;
    return s_instance;
}

void MapCache::init() {
    m_maps.clear();
    scanForMaps();
    TD_LOG_INFO("[MapCache] Initialized: {} maps found", m_maps.size());
}

void MapCache::scanForMaps() {
    auto& vfs = io::VFS::instance();

    struct SearchPath {
        container::String vfsPath;
        bool multiplayer = true;
        bool userMap = false;
    };

    container::Vector<SearchPath> searchPaths;
    if (config::TheWritableGlobalData) {
        for (const auto& searchPath : config::TheWritableGlobalData->getMapSearchPaths()) {
            searchPaths.push_back({ recursiveMapPattern(searchPath.vfsPath),
                                    searchPath.multiplayer, false });
        }
        for (const auto& localPath : config::TheWritableGlobalData->getLocalMapPaths()) {
            searchPaths.push_back({ recursiveMapPattern(localPath.vfsRoot),
                                    localPath.multiplayer, true });
        }
    }

    // RefCode enumerates Maps and UserData\Maps independently.  Always retain
    // both source roots, even when a config supplies additional search paths.
    // The separate VFS names mean a user map can never replace an official map
    // merely by using the same relative filename.
    searchPaths.push_back({ "maps/*.map", true, false });
    searchPaths.push_back({ "user/maps/*.map", true, true });

    container::HashSet<container::String> seen;
    container::TreeMap<container::String, MapMetaData> discoveredMaps;
    for (const auto& searchPath : searchPaths) {
        auto files = vfs.getFileList(searchPath.vfsPath);
        for (const auto& file : files) {
            if (!hasMapExtension(file)) continue;

            // Map IDs are complete normalized VFS paths.  Never key by the
            // basename: maps/Foo/Foo.map and maps/Bar/Foo.map are both valid.
            const container::String mapId = normalizePath(file);
            if (mapId.empty() || !seen.insert(mapId).second) continue;

            const container::String mapName = mapDisplayNameFromPath(mapId);
            if (mapName.empty()) continue;

            container::UniquePtr<io::File> mapFile;
            if (!vfs.open(mapId, mapFile) || !mapFile) {
                TD_LOG_WARN("[MapCache] Ignoring unreadable map source '{}'", mapId);
                continue;
            }

            const int64_t fileSize = mapFile->size();
            if (fileSize <= 0) {
                TD_LOG_WARN("[MapCache] Ignoring empty map source '{}'", mapId);
                continue;
            }

            MapMetaData meta;
            meta.mapId = mapId;
            meta.sourcePath = mapId;
            meta.displayName = mapName;
            meta.isMultiplayer = searchPath.multiplayer;
            meta.isUserMap = searchPath.userMap ||
                classifyMapSourcePath(mapId) == MapSourceKind::User;
            meta.isOfficial = !meta.isUserMap;
            meta.filesize = static_cast<uint32_t>(std::min<int64_t>(
                fileSize, static_cast<int64_t>(std::numeric_limits<uint32_t>::max())));
            discoveredMaps.emplace(meta.mapId, std::move(meta));
        }
    }

    m_maps.swap(discoveredMaps);

    TD_LOG_INFO("[MapCache] Scanned {} maps ({} multiplayer)", m_maps.size(), getMultiplayerMaps().size());
}

void MapCache::updateCache() {
    scanForMaps();
    TD_LOG_INFO("[MapCache] Updated: {} maps", m_maps.size());
}

const MapMetaData* MapCache::findMap(container::StringView mapIdOrPath) const {
    const container::String normalized = normalizePath(container::String(mapIdOrPath));
    if (normalized.empty()) return nullptr;

    auto it = m_maps.find(normalized);
    if (it != m_maps.end()) return &it->second;

    // Saved games from the old placeholder UI may contain only a basename.
    // Resolve it only when it identifies exactly one cache entry; choosing a
    // path arbitrarily would reintroduce the duplicate-name bug.
    if (normalized.find('/') == container::String::npos) {
        const container::String requestedStem = mapBasenameWithoutExtension(normalized);
        const MapMetaData* match = nullptr;
        for (const auto& [_, meta] : m_maps) {
            if (mapBasename(meta.sourcePath) != normalized &&
                mapBasenameWithoutExtension(meta.sourcePath) != requestedStem) {
                continue;
            }
            if (match) return nullptr;
            match = &meta;
        }
        return match;
    }

    return nullptr;
}

container::Vector<container::String> MapCache::getMapList() const {
    container::Vector<container::String> result;
    for (const auto& [name, _] : m_maps) {
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

container::Vector<container::String> MapCache::getMultiplayerMaps() const {
    container::Vector<container::String> result;
    for (const auto& [name, meta] : m_maps) {
        if (meta.isMultiplayer) result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace game
