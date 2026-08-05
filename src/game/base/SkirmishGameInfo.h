#pragma once

#include "core/container/container_types.h"

#include "game/base/GameSettings.h"
#include <cstdint>
#include "game/base/GameBalanceConstants.h"

namespace game {

struct MapMetaData {
    // Stable identity used by the cache and network-facing selection code.  This
    // is the normalized VFS path, rather than a basename, so two maps named
    // "MyMap.map" in different directories remain distinct entries.
    container::String mapId;

    // VFS path used to actually open the map.  UI code must pass this value to
    // GameStartInfo::mapName; displayName is deliberately not a load target.
    container::String sourcePath;
    container::String displayName;
    int numPlayers = 0;
    bool isMultiplayer = false;
    bool isOfficial = false;
    bool isUserMap = false;
    uint32_t filesize = 0;
    uint32_t CRC = 0;
};

class SkirmishGameInfo {
public:
    static SkirmishGameInfo& instance();

    void init();
    void reset();
    void clearSlotList();

    // Resolves a cache ID/path when possible and stores the VFS-loadable path.
    // The string overload intentionally remains for persisted legacy settings.
    void setMap(const container::String& mapNameOrPath);
    void setMap(const MapMetaData& map);
    const container::String& getMap() const { return m_mapName; }
    const container::String& getMapPath() const { return m_mapName; }
    const container::String& getMapId() const { return m_mapId; }

    void setMapCRC(uint32_t crc) { m_mapCRC = crc; }
    uint32_t getMapCRC() const { return m_mapCRC; }

    void setMapSize(uint32_t size) { m_mapSize = size; }
    uint32_t getMapSize() const { return m_mapSize; }

    void setSeed(int seed) { m_seed = seed; }
    int getSeed() const { return m_seed; }

    void setStartingCash(int cash) { m_startingCash = cash; }
    int getStartingCash() const { return m_startingCash; }

    void setSuperweaponRestricted(bool restricted) { m_superweaponRestricted = restricted; }
    bool isSuperweaponRestricted() const { return m_superweaponRestricted; }

    engine::GameSlot& getSlot(int index);
    const engine::GameSlot& getSlot(int index) const;

    void startGame(int gameID);
    bool isInProgress() const { return m_inProgress; }

private:
    SkirmishGameInfo() = default;

    // m_mapName is retained as the legacy API name, but now always contains a
    // VFS-loadable path after a successful MapCache lookup.
    container::String m_mapName;
    container::String m_mapId;
    uint32_t m_mapCRC = 0;
    uint32_t m_mapSize = 0;
    int m_seed = 0;
    int m_startingCash = engine::DEFAULT_STARTING_CASH;
    bool m_superweaponRestricted = false;
    bool m_inProgress = false;

    container::Array<engine::GameSlot, engine::MAX_SLOTS> m_slots;
};

extern SkirmishGameInfo* TheSkirmishGameInfo;
extern SkirmishGameInfo* TheChallengeGameInfo;

} // namespace game
