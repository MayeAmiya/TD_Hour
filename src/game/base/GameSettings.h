#pragma once

#include "core/container/container_types.h"

#include <cstdint>
#include "game/base/GameBalanceConstants.h"

namespace engine {

// ── Enums matching original game ────────────────────────────────────────────

enum GameDifficulty : int {
    DIFFICULTY_EASY = 0,
    DIFFICULTY_NORMAL = 1,
    DIFFICULTY_HARD = 2,
};

enum SlotState : int {
    SLOT_OPEN = 0,
    SLOT_HUMAN = 1,
    SLOT_AI = 2,
    SLOT_EASY_AI = 3,
    SLOT_NORMAL_AI = 4,
    SLOT_HARD_AI = 5,
    SLOT_CLOSED = 6,
};

enum class GameMode : uint8_t {
    SinglePlayer,
    Skirmish,
    Challenge,
    Replay,
    Invalid
};

enum class GameSequenceType : uint8_t {
    None,
    Campaign,
    Challenge,
};

struct GameSequenceIdentity {
    GameSequenceType type = GameSequenceType::None;
    container::String campaignName;
    container::String missionName;
    container::String challengeGeneral;

    void reset() {
        type = GameSequenceType::None;
        campaignName.clear();
        missionName.clear();
        challengeGeneral.clear();
    }
};

struct NetworkSessionInfo {
    bool enabled = false;
    container::String serverHost;
    uint16_t serverPort = 0;
    container::String sessionId;
    container::String joinToken;
    uint16_t protocolVersion = 1;
    uint32_t frameSendRate = 3;

    void reset() {
        enabled = false;
        serverHost.clear();
        serverPort = 0;
        sessionId.clear();
        joinToken.clear();
        protocolVersion = 1;
        frameSendRate = 3;
    }
};

// ── GameSlot ────────────────────────────────────────────────────────────────
// Per-player slot settings (matches original GameSlot)

struct GameSlot {
    SlotState state = SLOT_OPEN;
    int color = -1;            // -1 = random
    int startPos = -1;         // -1 = random
    int playerTemplate = -1;   // -1 = random (faction index)
    int teamNumber = -1;       // -1 = none
    container::String name;          // human player name

    void reset();
};

inline constexpr int MAX_SLOTS = DEFAULT_MAX_SLOTS;
inline constexpr int DEFAULT_GAME_SPEED_FPS = 30;

// ── GameStartInfo ─────────────────────────────────────────────────────────
// Unified runtime data passed to GameLogic for all game modes.
// Replaces scattered global singletons with one clean data flow.

struct GameStartInfo {
    GameMode mode = GameMode::Invalid;
    GameSequenceIdentity sequence;

    container::String mapName;
    uint32_t mapCRC = 0;
    uint32_t mapSize = 0;
    uint32_t rulesCRC = 0;
    int difficulty = DIFFICULTY_NORMAL;
    int rankPoints = 0;
    int gameSpeedFPS = DEFAULT_GAME_SPEED_FPS;
    int seed = 0;
    bool superweaponRestricted = false;
    bool oldFactionsOnly = false;
    int startingMoney = 0;
    int localPlayerSlot = 0;
    container::String localPlayerTemplateName;
    container::String localPlayerSide;
    container::String localPlayerBaseSide;
    container::String saveFileName;
    container::String replayFileName;
    NetworkSessionInfo network;

    container::Array<GameSlot, MAX_SLOTS> slots;

    // Legacy callers used playerSlot() as an accidental alias for slots[0].
    // A match may have its local client in any slot, so preserve a safe
    // reference API while making it follow localPlayerSlot.
    GameSlot& playerSlot() {
        const int index = localPlayerSlot >= 0 && localPlayerSlot < MAX_SLOTS ? localPlayerSlot : 0;
        return slots[static_cast<size_t>(index)];
    }
    const GameSlot& playerSlot() const {
        const int index = localPlayerSlot >= 0 && localPlayerSlot < MAX_SLOTS ? localPlayerSlot : 0;
        return slots[static_cast<size_t>(index)];
    }

    void reset() {
        mode = GameMode::Invalid;
        sequence.reset();
        mapName.clear();
        mapCRC = 0;
        mapSize = 0;
        rulesCRC = 0;
        difficulty = DIFFICULTY_NORMAL;
        rankPoints = 0;
        gameSpeedFPS = DEFAULT_GAME_SPEED_FPS;
        seed = 0;
        superweaponRestricted = false;
        oldFactionsOnly = false;
        startingMoney = 0;
        localPlayerSlot = 0;
        localPlayerTemplateName.clear();
        localPlayerSide.clear();
        localPlayerBaseSide.clear();
        saveFileName.clear();
        replayFileName.clear();
        network.reset();
        slots = {};
    }
};

// ── SkirmishSettings ────────────────────────────────────────────────────────
// All settings for a skirmish game (persisted to Skirmish.ini)

struct SkirmishSettings {
    container::String mapName;
    uint32_t mapCRC = 0;
    uint32_t mapSize = 0;

    int seed = 0;
    int startingCash = DEFAULT_STARTING_CASH;
    bool superweaponRestricted = false;
    bool oldFactionsOnly = false;
    int gameSpeedFPS = DEFAULT_GAME_SPEED_FPS;
    int localPlayerSlot = 0;

    container::Array<GameSlot, MAX_SLOTS> slots;

    GameSlot& localSlot() {
        const int index = localPlayerSlot >= 0 && localPlayerSlot < MAX_SLOTS ? localPlayerSlot : 0;
        return slots[static_cast<size_t>(index)];
    }
    const GameSlot& localSlot() const {
        const int index = localPlayerSlot >= 0 && localPlayerSlot < MAX_SLOTS ? localPlayerSlot : 0;
        return slots[static_cast<size_t>(index)];
    }

    void reset();
};

// ── CampaignSettings ────────────────────────────────────────────────────────
// Campaign state (persisted to Options.ini)

struct CampaignSettings {
    container::String faction;       // "USA", "GLA", "China"
    int difficulty = DIFFICULTY_NORMAL;
    container::String currentMap;

    void reset();
};

// ── GameSettings (singleton) ────────────────────────────────────────────────
// Global game configuration, shared between UI and GameLogic.
// Persisted to INI files (Skirmish.ini, Options.ini).
// At runtime, call toGameStartInfo() to get unified data for GameLogic.

class GameSettings {
public:
    static GameSettings& instance();

    SkirmishSettings& skirmish() { return m_skirmish; }
    const SkirmishSettings& skirmish() const { return m_skirmish; }

    CampaignSettings& campaign() { return m_campaign; }
    const CampaignSettings& campaign() const { return m_campaign; }

    // Convert to unified GameStartInfo for GameLogic
    GameStartInfo toGameStartInfo(GameMode mode) const;

    // Persistence
    void loadSkirmish();
    void saveSkirmish();
    void loadCampaignDifficulty();
    void saveCampaignDifficulty();

    // Reset everything
    void resetAll();

private:
    GameSettings() = default;
    SkirmishSettings m_skirmish;
    CampaignSettings m_campaign;
};

} // namespace engine
