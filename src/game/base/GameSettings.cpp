#include "core/container/container_types.h"
#include "game/base/GameSettings.h"
#include "game/base/GameStartInfoBuilder.h"
#include "game/base/CampaignManager.h"
#include "VFS.h"
#include "debug/debug.h"
#include <sstream>
#include <algorithm>
#include <charconv>
#include <optional>
#include "core/constants/Paths.h"
#include "game/base/GameBalanceConstants.h"

namespace engine {

// ── GameSlot ────────────────────────────────────────────────────────────────

void GameSlot::reset() {
    state = SLOT_OPEN;
    color = -1;
    startPos = -1;
    playerTemplate = -1;
    teamNumber = -1;
    name.clear();
}

// ── SkirmishSettings ────────────────────────────────────────────────────────

void SkirmishSettings::reset() {
    mapName.clear();
    mapCRC = 0;
    mapSize = 0;
    seed = 0;
    startingCash = DEFAULT_STARTING_CASH;
    superweaponRestricted = false;
    oldFactionsOnly = false;
    gameSpeedFPS = 30;
    localPlayerSlot = 0;
    for (auto& slot : slots) slot.reset();
}

// ── CampaignSettings ────────────────────────────────────────────────────────

void CampaignSettings::reset() {
    faction.clear();
    difficulty = DIFFICULTY_NORMAL;
    currentMap.clear();
}

// ── GameSettings ────────────────────────────────────────────────────────────

GameSettings& GameSettings::instance() {
    static GameSettings s_instance;
    return s_instance;
}

// ── Helpers ─────────────────────────────────────────────────────────────────

static container::String trimStr(const container::String& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == container::String::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static container::String toLowerStr(const container::String& s) {
    container::String result = s;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

static bool parseIntStrict(const container::String& value, int& output);
static bool parseUint32Strict(const container::String& value, uint32_t& output);
static bool parseBoolStrict(const container::String& value, bool& output);
static std::optional<size_t> slotIndexFromSection(const container::String& section);
static void applySlotField(GameSlot& slot, const container::String& key, const container::String& value);

// ── Skirmish persistence (Skirmish.ini) ─────────────────────────────────────

void GameSettings::loadSkirmish() {
    m_skirmish.reset();

    const container::String content = io::VFS::instance().readAll(SKIRMISH_INI);
    if (content.empty()) {
        TD_LOG_INFO("[GameSettings] Skirmish.ini not found, using defaults");
        return;
    }

    std::istringstream ini(content);
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

        if (curSection == SECTION_SKIRMISH.data()) {
            int parsed = 0;
            uint32_t parsedUnsigned = 0;
            bool parsedBool = false;
            if (key == "map") m_skirmish.mapName = value;
            else if (key == "startingcash" && parseIntStrict(value, parsed)) m_skirmish.startingCash = parsed;
            else if (key == "mapcrc" && parseUint32Strict(value, parsedUnsigned)) {
                m_skirmish.mapCRC = parsedUnsigned;
            } else if (key == "mapsize" && parseUint32Strict(value, parsedUnsigned)) {
                m_skirmish.mapSize = parsedUnsigned;
            } else if (key == "superweaponrestricted" && parseBoolStrict(value, parsedBool)) {
                m_skirmish.superweaponRestricted = parsedBool;
            } else if (key == "oldfactionsonly" && parseBoolStrict(value, parsedBool)) {
                m_skirmish.oldFactionsOnly = parsedBool;
            } else if (key == "fps" && parseIntStrict(value, parsed)) {
                m_skirmish.gameSpeedFPS = parsed;
            } else if (key == "seed" && parseIntStrict(value, parsed)) {
                m_skirmish.seed = parsed;
            } else if (key == "localplayerslot" && parseIntStrict(value, parsed) &&
                       parsed >= 0 && parsed < MAX_SLOTS) {
                m_skirmish.localPlayerSlot = parsed;
            }
        } else if (const auto slotIndex = slotIndexFromSection(curSection)) {
            applySlotField(m_skirmish.slots[*slotIndex], key, value);
        }
    }

    TD_LOG_INFO("[GameSettings] Loaded skirmish: map={} cash={} slots={}",
                m_skirmish.mapName, m_skirmish.startingCash, MAX_SLOTS);
}

static bool parseIntStrict(const container::String& value, int& output) {
    if (value.empty()) return false;
    const char* first = value.data();
    const char* last = first + value.size();
    int parsed = 0;
    const auto [end, error] = std::from_chars(first, last, parsed);
    if (error != std::errc{} || end != last) return false;
    output = parsed;
    return true;
}

static bool parseUint32Strict(const container::String& value, uint32_t& output) {
    if (value.empty() || value.front() == '-') return false;
    const char* first = value.data();
    const char* last = first + value.size();
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(first, last, parsed);
    if (error != std::errc{} || end != last) return false;
    output = parsed;
    return true;
}

static bool parseBoolStrict(const container::String& value, bool& output) {
    const container::String normalized = toLowerStr(value);
    if (normalized == "1" || normalized == "true" || normalized == "yes") {
        output = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no") {
        output = false;
        return true;
    }
    return false;
}

static std::optional<size_t> slotIndexFromSection(const container::String& section) {
    constexpr container::StringView prefix = "slot";
    if (!section.starts_with(prefix) || section.size() == prefix.size()) return std::nullopt;
    unsigned int parsed = 0;
    const char* first = section.data() + prefix.size();
    const char* last = section.data() + section.size();
    const auto [end, error] = std::from_chars(first, last, parsed);
    if (error != std::errc{} || end != last || parsed >= static_cast<unsigned int>(MAX_SLOTS)) {
        return std::nullopt;
    }
    return static_cast<size_t>(parsed);
}

static void applySlotField(GameSlot& slot, const container::String& key, const container::String& value) {
    int parsed = 0;
    if (key == "state") {
        if (parseIntStrict(value, parsed) && parsed >= SLOT_OPEN && parsed <= SLOT_CLOSED) {
            slot.state = static_cast<SlotState>(parsed);
        }
    } else if (key == "color") {
        if (parseIntStrict(value, parsed)) slot.color = parsed;
    } else if (key == "startpos") {
        if (parseIntStrict(value, parsed)) slot.startPos = parsed;
    } else if (key == "playertemplate") {
        if (parseIntStrict(value, parsed)) slot.playerTemplate = parsed;
    } else if (key == "team") {
        if (parseIntStrict(value, parsed)) slot.teamNumber = parsed;
    } else if (key == "name") {
        slot.name = value;
    }
}

void GameSettings::saveSkirmish() {
    std::ostringstream ini;

    ini << "[" << SECTION_SKIRMISH.data() << "]\n";
    ini << KEY_MAP.data() << "=" << m_skirmish.mapName << "\n";
    ini << "MapCRC=" << m_skirmish.mapCRC << "\n";
    ini << "MapSize=" << m_skirmish.mapSize << "\n";
    ini << KEY_STARTING_CASH.data() << "=" << m_skirmish.startingCash << "\n";
    ini << KEY_SUPERWEAPON_RESTRICTED.data() << "=" << (m_skirmish.superweaponRestricted ? 1 : 0) << "\n";
    ini << KEY_OLD_FACTIONS_ONLY.data() << "=" << (m_skirmish.oldFactionsOnly ? 1 : 0) << "\n";
    ini << KEY_FPS.data() << "=" << m_skirmish.gameSpeedFPS << "\n";
    ini << KEY_SEED.data() << "=" << m_skirmish.seed << "\n";
    ini << "LocalPlayerSlot=" << m_skirmish.localPlayerSlot << "\n";
    ini << "\n";

    for (size_t index = 0; index < m_skirmish.slots.size(); ++index) {
        const auto& slot = m_skirmish.slots[index];
        ini << "[Slot" << index << "]\n";
        ini << KEY_STATE.data() << "=" << static_cast<int>(slot.state) << "\n";
        ini << KEY_COLOR.data() << "=" << slot.color << "\n";
        ini << KEY_START_POS.data() << "=" << slot.startPos << "\n";
        ini << KEY_PLAYER_TEMPLATE.data() << "=" << slot.playerTemplate << "\n";
        ini << KEY_TEAM.data() << "=" << slot.teamNumber << "\n";
        ini << KEY_NAME.data() << "=" << slot.name << "\n\n";
    }

    if (!io::VFS::instance().writeAll(SKIRMISH_INI, ini.str())) {
        TD_LOG_ERROR("[GameSettings] Cannot write Skirmish.ini");
        return;
    }

    TD_LOG_INFO("[GameSettings] Saved skirmish to Skirmish.ini");
}

// ── Campaign difficulty persistence (Options.ini) ───────────────────────────

void GameSettings::loadCampaignDifficulty() {
    m_campaign.difficulty = DIFFICULTY_NORMAL;

    const container::String content = io::VFS::instance().readAll(OPTIONS_INI);
    if (content.empty()) return;

    std::istringstream ini(content);
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

        if (curSection == SECTION_GAME.data() && key == "campaigndifficulty") {
            int diff = DIFFICULTY_NORMAL;
            if (parseIntStrict(value, diff) &&
                diff >= DIFFICULTY_EASY && diff <= DIFFICULTY_HARD) {
                m_campaign.difficulty = diff;
            }
        }
    }
}

void GameSettings::saveCampaignDifficulty() {
    // Read existing Options.ini, update or add CampaignDifficulty
    std::istringstream inFile(io::VFS::instance().readAll(OPTIONS_INI));
    container::String content;
    bool found = false;

    container::String line;
    while (std::getline(inFile, line)) {
        container::String trimmed = trimStr(line);
        container::String lower = toLowerStr(trimmed);
        if (lower.find("campaigndifficulty") != container::String::npos) {
            content += "CampaignDifficulty=" + std::to_string(m_campaign.difficulty) + "\n";
            found = true;
        } else {
            content += line + "\n";
        }
    }

    if (!found) {
        content += "\n[";
        content += SECTION_GAME.data();
        content += "]\n";
        content += "CampaignDifficulty=" + std::to_string(m_campaign.difficulty) + "\n";
    }

    io::VFS::instance().writeAll(OPTIONS_INI, content);
}

// ── toGameStartInfo ─────────────────────────────────────────────────────────

GameStartInfo GameSettings::toGameStartInfo(GameMode mode) const {
    GameStartInfo info;
    info.mode = mode;

    switch (mode) {
        case GameMode::Skirmish: {
            info.mapName = m_skirmish.mapName;
            info.mapCRC = m_skirmish.mapCRC;
            info.mapSize = m_skirmish.mapSize;
            info.seed = m_skirmish.seed;
            info.startingMoney = m_skirmish.startingCash;
            info.superweaponRestricted = m_skirmish.superweaponRestricted;
            info.oldFactionsOnly = m_skirmish.oldFactionsOnly;
            info.gameSpeedFPS = m_skirmish.gameSpeedFPS;
            info.difficulty = DIFFICULTY_NORMAL;
            info.localPlayerSlot = m_skirmish.localPlayerSlot;
            info.slots = m_skirmish.slots;
            break;
        }
        case GameMode::SinglePlayer: {
            info.mapName = m_campaign.currentMap;
            info.difficulty = m_campaign.difficulty;
            info.startingMoney = m_skirmish.startingCash;
            info.gameSpeedFPS = m_skirmish.gameSpeedFPS;

            // Campaign maps own their authored SidesList/AI roster. Do not
            // leak whichever multiplayer lobby was used last into a campaign
            // session; this launch boundary contributes only the one local
            // command participant required by the modern match resolver.
            info.localPlayerSlot = 0;
            for (GameSlot& slot : info.slots) slot.reset();
            GameSlot& local = info.playerSlot();
            local.state = SLOT_HUMAN;
            local.name = "CampaignPlayer";

            // MainMenu's campaign keys use the original labels (USA/GLA/
            // China), while PlayerTemplate.ini defines USA as
            // FactionAmerica with Side=America and BaseSide=USA.
            GameStartInfoBuilder::applyCampaignFaction(info, m_campaign.faction);
            if (game::CampaignManager* manager = game::TheCampaignManager) {
                const game::Campaign* campaign = manager->getCurrentCampaign();
                const game::Mission* mission = manager->getCurrentMission();
                if (campaign && mission && mission->mapName == info.mapName) {
                    info.sequence.type = GameSequenceType::Campaign;
                    info.sequence.campaignName = campaign->name;
                    info.sequence.missionName = mission->name;
                }
            }
            break;
        }
        default: {
            break;
        }
    }

    return info;
}

// ── Reset ───────────────────────────────────────────────────────────────────

void GameSettings::resetAll() {
    m_skirmish.reset();
    m_campaign.reset();
}

} // namespace engine
