#pragma once

#include "core/container/container_types.h"

#include "game/base/SkirmishGameInfo.h"
namespace game {

class MapCache {
public:
    static MapCache& instance();

    void init();
    void updateCache();

    // Accepts a normalized map ID, a VFS source path, or an unambiguous legacy
    // basename.  Ambiguous basenames intentionally do not resolve.
    const MapMetaData* findMap(container::StringView mapIdOrPath) const;

    const container::TreeMap<container::String, MapMetaData>& getMaps() const { return m_maps; }

    container::Vector<container::String> getMapList() const;
    container::Vector<container::String> getMultiplayerMaps() const;

private:
    MapCache() = default;

    void scanForMaps();

    container::TreeMap<container::String, MapMetaData> m_maps;
};

extern MapCache* TheMapCache;

} // namespace game
