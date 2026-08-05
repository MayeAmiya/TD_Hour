#include "core/container/container_types.h"
#include "game/base/SkirmishGameInfo.h"
#include "game/base/MapContentIdentity.h"
#include "game/base/MapCache.h"
#include "debug/debug.h"
#include "game/base/GameBalanceConstants.h"
#include <cstdlib>
#include <ctime>

namespace game {

SkirmishGameInfo* TheSkirmishGameInfo = nullptr;
SkirmishGameInfo* TheChallengeGameInfo = nullptr;

SkirmishGameInfo& SkirmishGameInfo::instance() {
    static SkirmishGameInfo s_instance;
    return s_instance;
}

void SkirmishGameInfo::init() {
    reset();
}

void SkirmishGameInfo::reset() {
    m_mapName.clear();
    m_mapId.clear();
    m_mapCRC = 0;
    m_mapSize = 0;
    m_seed = static_cast<int>(std::time(nullptr));
    m_startingCash = engine::DEFAULT_STARTING_CASH;
    m_superweaponRestricted = false;
    m_inProgress = false;
    for (auto& slot : m_slots) {
        slot.reset();
    }
}

void SkirmishGameInfo::clearSlotList() {
    for (auto& slot : m_slots) {
        slot.reset();
    }
}

void SkirmishGameInfo::setMap(const container::String& mapNameOrPath) {
    if (mapNameOrPath.empty()) {
        m_mapName.clear();
        m_mapId.clear();
        m_mapCRC = 0;
        m_mapSize = 0;
        return;
    }

    if (TheMapCache) {
        if (const MapMetaData* map = TheMapCache->findMap(mapNameOrPath)) {
            setMap(*map);
            return;
        }
    }

    m_mapName = canonicalMapSourcePath(mapNameOrPath);
    m_mapId = m_mapName;
    m_mapCRC = 0;
    m_mapSize = 0;
    MapContentIdentity identity;
    if (!fingerprintMapContent(m_mapName, identity) || identity.size == 0) {
        m_mapCRC = 0;
        m_mapSize = 0;
    } else {
        m_mapCRC = identity.crc;
        m_mapSize = identity.size;
    }
    TD_LOG_WARN("[SkirmishGameInfo] Map '{}' is not present in MapCache", mapNameOrPath);
}

void SkirmishGameInfo::setMap(const MapMetaData& map) {
    m_mapId = map.mapId.empty() ? map.sourcePath : map.mapId;
    m_mapName = map.sourcePath.empty() ? m_mapId : map.sourcePath;
    m_mapCRC = map.CRC;
    m_mapSize = map.filesize;

    // MapCache verifies that the VFS entry can be opened.  Compute the actual
    // content fingerprint only for the selected map, avoiding a full read of
    // every map each time the selection screen refreshes.
    MapContentIdentity identity;
    if (fingerprintMapContent(m_mapName, identity) && identity.size != 0) {
        m_mapCRC = identity.crc;
        m_mapSize = identity.size;
    } else {
        TD_LOG_WARN("[SkirmishGameInfo] Could not fingerprint selected map '{}'", m_mapName);
    }

    TD_LOG_INFO("[SkirmishGameInfo] Map set: id='{}' path='{}' crc={} size={}",
                m_mapId, m_mapName, m_mapCRC, m_mapSize);
}

engine::GameSlot& SkirmishGameInfo::getSlot(int index) {
    if (index < 0 || index >= engine::MAX_SLOTS) {
        static engine::GameSlot s_empty;
        return s_empty;
    }
    return m_slots[index];
}

const engine::GameSlot& SkirmishGameInfo::getSlot(int index) const {
    if (index < 0 || index >= engine::MAX_SLOTS) {
        static const engine::GameSlot s_empty;
        return s_empty;
    }
    return m_slots[index];
}

void SkirmishGameInfo::startGame(int gameID) {
    m_inProgress = true;
    m_seed = static_cast<int>(std::time(nullptr));
    TD_LOG_INFO("[SkirmishGameInfo] Game started: map={} seed={} cash={}",
                m_mapName, m_seed, m_startingCash);
}

} // namespace game
