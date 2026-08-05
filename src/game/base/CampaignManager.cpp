#include "core/container/container_types.h"
#include "game/base/CampaignManager.h"
#include "VFS.h"
#include "core/data/ini/LegacyIniDirectory.h"
#include "debug/debug.h"
#include <algorithm>
#include <charconv>
#include <sstream>

namespace game {

CampaignManager* TheCampaignManager = nullptr;

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

static bool parseIntStrict(container::StringView text, int& value) {
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

static int indexedField(container::StringView key,
                        container::StringView prefix, int count) {
    if (!key.starts_with(prefix) || key.size() != prefix.size() + 1u) {
        return -1;
    }
    const char digit = key.back();
    if (digit < '0' || digit > '9') return -1;
    const int index = digit - '0';
    return index < count ? index : -1;
}

static bool parseIniBool(container::StringView value) {
    const container::String normalized = toLowerStr(container::String{value});
    return normalized == "yes" || normalized == "true" ||
        normalized == "1" || normalized == "on";
}

static container::StringView stockCampaignPlayerFaction(
    container::StringView campaignName) {
    const container::String normalized =
        toLowerStr(container::String{campaignName});
    if (normalized == "usa") return "FactionAmerica";
    if (normalized == "gla") return "FactionGLA";
    if (normalized == "china") return "FactionChina";
    return {};
}

static container::String canonicalMapPath(container::StringView path) {
    container::String result{path};
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) {
            if (c == '/') return '\\';
            return static_cast<char>(std::tolower(c));
        });
    while (!result.empty() &&
           (result.front() == ' ' || result.front() == '\t')) {
        result.erase(result.begin());
    }
    while (!result.empty() &&
           (result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }
    return result;
}

const Mission* Campaign::getNextMission(const Mission* current) const {
    if (current == nullptr) {
        for (const auto& m : missions) {
            if (toLowerStr(m.name) == toLowerStr(firstMission)) return &m;
        }
        return nullptr;
    }

    if (current->nextMission.empty()) return nullptr;

    for (const auto& m : missions) {
        if (toLowerStr(m.name) == toLowerStr(current->nextMission)) return &m;
    }
    return nullptr;
}

CampaignManager& CampaignManager::instance() {
    static CampaignManager s_instance;
    return s_instance;
}

bool CampaignManager::loadFromIni(const container::String& filename) {
    m_campaigns.clear();
    m_currentCampaign = nullptr;
    m_currentMission = nullptr;

    container::String root = filename;
    if (toLowerStr(root).ends_with(".ini")) root.resize(root.size() - 4u);
    const container::Array<container::StringView, 1> roots{{root}};
    const container::Vector<container::String> sources =
        game::ini::enumerateLegacyIniDirectories(roots);
    container::String content;
    for (const container::String& source : sources) {
        container::String layer = io::VFS::instance().readAll(source);
        if (layer.empty()) continue;
        content.append(layer);
        content.push_back('\n');
    }
    if (content.empty()) {
        TD_LOG_ERROR("[CampaignManager] Cannot read: {}", filename);
        return false;
    }

    std::istringstream stream(content);
    container::String line;
    Campaign* curCampaign = nullptr;
    Mission* curMission = nullptr;
    int lineNum = 0;
    bool inCampaign = false;

    while (std::getline(stream, line)) {
        lineNum++;
        container::String trimmed = trimStr(line);
        if (trimmed.empty() || trimmed[0] == ';') continue;

        if (trimmed.find("Campaign ") == 0) {
            container::String name = trimStr(trimmed.substr(8));
            // RefCode CampaignManager::newCampaign removes an earlier
            // same-name definition before appending the new complete block.
            // This is INI_LOAD_OVERWRITE, not mission-list accumulation.
            std::erase_if(m_campaigns, [&name](const Campaign& campaign) {
                return toLowerStr(campaign.name) == toLowerStr(name);
            });
            m_campaigns.emplace_back();
            curCampaign = &m_campaigns.back();
            curCampaign->name = name;
            curMission = nullptr;
            inCampaign = true;
            continue;
        }

        if (trimmed.find("Mission ") == 0 && curCampaign) {
            container::String name = trimStr(trimmed.substr(7));
            curCampaign->missions.emplace_back();
            curMission = &curCampaign->missions.back();
            curMission->name = name;
            continue;
        }

        if (trimmed == "END") {
            if (curMission) {
                curMission = nullptr;
            } else if (inCampaign) {
                inCampaign = false;
                curCampaign = nullptr;
            }
            continue;
        }

        if (!curCampaign) continue;

        container::String key, value;
        size_t eq = trimmed.find('=');
        if (eq != container::String::npos) {
            key = toLowerStr(trimStr(trimmed.substr(0, eq)));
            value = trimStr(trimmed.substr(eq + 1));
        } else {
            size_t space = trimmed.find(' ');
            if (space == container::String::npos) continue;
            key = toLowerStr(trimmed.substr(0, space));
            value = trimStr(trimmed.substr(space + 1));
        }

        if (curMission) {
            if (key == "map") curMission->mapName = value;
            else if (key == "nextmission") curMission->nextMission = value;
            else if (key == "intromovie") curMission->movieLabel = value;
            else if (key == "generalname") curMission->generalName = value;
            else if (key == "locationnamelabel") curMission->locationName = value;
            else if (key == "briefingvoice") curMission->briefingVoice = value;
            else if (key == "voicelength") {
                int parsed = 0;
                if (parseIntStrict(value, parsed)) {
                    curMission->voiceLength = parsed;
                } else {
                    TD_LOG_WARN(
                        "[CampaignManager] Ignoring invalid VoiceLength '{}' at line {}",
                        value, lineNum);
                }
            }
            else if (const int index = indexedField(key, "objectiveline", 5);
                     index >= 0) {
                curMission->objectives[index] = value;
            }
            else if (const int index = indexedField(key, "unitnames", 3);
                     index >= 0) {
                curMission->unitNames[index] = value;
            }
        } else {
            if (key == "campaignnamelabel") curCampaign->campaignNameLabel = value;
            else if (key == "firstmission") curCampaign->firstMission = value;
            else if (key == "finalvictorymovie") curCampaign->finalMovieName = value;
            else if (key == "ischallengecampaign") curCampaign->isChallengeCampaign = parseIniBool(value);
            else if (key == "playerfaction") curCampaign->playerFactionName = value;
        }
    }

    // Shipped Generals and Zero Hour Campaign.ini omit PlayerFaction for the
    // three ordinary campaigns.  The shell supplied that identity out of
    // band; a launcher-authored session does not pass through the shell, so
    // normalize the same stock contract at the content-loading boundary.
    // Explicit authored values (including mod campaigns) always win.
    for (Campaign& campaign : m_campaigns) {
        if (campaign.isChallengeCampaign ||
            !campaign.playerFactionName.empty()) {
            continue;
        }
        campaign.playerFactionName =
            stockCampaignPlayerFaction(campaign.name);
    }

    TD_LOG_INFO("[CampaignManager] Loaded {} sources from {}: {} campaigns",
                sources.size(), filename, m_campaigns.size());
    return true;
}

Campaign* CampaignManager::findCampaign(const container::String& name) {
    for (auto& c : m_campaigns) {
        if (toLowerStr(c.name) == toLowerStr(name)) return &c;
    }
    return nullptr;
}

const Mission* CampaignManager::findMissionByMap(
    container::StringView mapName, const Campaign** matchedCampaign) const {
    if (matchedCampaign) *matchedCampaign = nullptr;
    const container::String sought = canonicalMapPath(mapName);
    if (sought.empty()) return nullptr;
    const Campaign* bestCampaign = nullptr;
    const Mission* bestMission = nullptr;
    for (const Campaign& campaign : m_campaigns) {
        for (const Mission& mission : campaign.missions) {
            if (canonicalMapPath(mission.mapName) != sought) continue;
            // Shipped Campaign.ini contains demo aliases for some maps before
            // their real continuable campaign (for example MD_USA01). Those
            // aliases intentionally omit PlayerFaction. Prefer the authored
            // faction-bearing campaign so direct start and Next resolve the
            // real sequence; retain first-match behavior when neither match
            // is continuable.
            if (!bestMission ||
                (bestCampaign->playerFactionName.empty() &&
                 !campaign.playerFactionName.empty())) {
                bestCampaign = &campaign;
                bestMission = &mission;
            }
        }
    }
    if (matchedCampaign) *matchedCampaign = bestCampaign;
    return bestMission;
}

const Campaign* CampaignManager::findCampaignByMap(
    container::StringView mapName) const {
    const Campaign* campaign = nullptr;
    static_cast<void>(findMissionByMap(mapName, &campaign));
    return campaign;
}

void CampaignManager::setCampaign(const container::String& name) {
    m_currentCampaign = findCampaign(name);
    if (m_currentCampaign) {
        m_currentMission = m_currentCampaign->getNextMission(nullptr);
        TD_LOG_INFO("[CampaignManager] Campaign set: {} firstMission={}",
                    name, m_currentMission ? m_currentMission->mapName : "none");
    } else {
        TD_LOG_WARN("[CampaignManager] Campaign '{}' not found", name);
        m_currentMission = nullptr;
    }
}

void CampaignManager::gotoNextMission() {
    if (!m_currentCampaign || !m_currentMission) return;
    m_currentMission = m_currentCampaign->getNextMission(m_currentMission);
    if (m_currentMission) {
        TD_LOG_INFO("[CampaignManager] Next mission: {} map={}",
                    m_currentMission->name, m_currentMission->mapName);
    } else {
        TD_LOG_INFO("[CampaignManager] Campaign complete!");
    }
}

container::String CampaignManager::getCurrentMap() const {
    if (m_currentMission) return m_currentMission->mapName;
    return {};
}

} // namespace game
