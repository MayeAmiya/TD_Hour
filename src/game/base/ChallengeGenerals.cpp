#include "core/container/container_types.h"
#include "game/base/ChallengeGenerals.h"
#include "VFS.h"
#include "core/data/ini/LegacyIniDirectory.h"
#include "debug/debug.h"
#include <charconv>
#include <sstream>

namespace game {

ChallengeGenerals* TheChallengeGenerals = nullptr;

namespace {

constexpr container::StringView kGeneralPersonaPrefix = "GeneralPersona";

} // namespace

ChallengeGenerals& ChallengeGenerals::instance() {
    static ChallengeGenerals s_instance;
    return s_instance;
}

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

static bool parseIniBool(container::StringView value) {
    const container::String normalized = toLowerStr(container::String{value});
    return normalized == "yes" || normalized == "true" ||
        normalized == "1" || normalized == "on";
}

bool ChallengeGenerals::loadFromIni(const container::String& filename) {
    m_generals = {};

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
        TD_LOG_ERROR("[ChallengeGenerals] Cannot read: {}", filename);
        return false;
    }

    std::istringstream stream(content);
    container::String line;
    int currentGeneral = -1;
    bool inChallengeGenerals = false;
    int nesting = 0;
    int lineNumber = 0;

    while (std::getline(stream, line)) {
        ++lineNumber;
        container::String trimmed = trimStr(line);
        if (trimmed.empty() || trimmed[0] == ';') continue;

        if (trimmed == "ChallengeGenerals") {
            inChallengeGenerals = true;
            nesting = 1;
            continue;
        }

        if (!inChallengeGenerals) continue;

        if (trimmed.starts_with(kGeneralPersonaPrefix) &&
            trimmed.size() > kGeneralPersonaPrefix.size()) {
            container::String numStr = trimmed.substr(kGeneralPersonaPrefix.size());
            int num = -1;
            if (parseIntStrict(numStr, num) && num >= 0 && num < NUM_GENERALS) {
                currentGeneral = num;
            } else {
                currentGeneral = -1;
                TD_LOG_WARN(
                    "[ChallengeGenerals] Ignoring invalid GeneralPersona index '{}' at line {}",
                    numStr, lineNumber);
            }
            nesting++;
            continue;
        }

        if (trimmed == "END") {
            nesting--;
            if (nesting == 1) {
                currentGeneral = -1;
            }
            if (nesting <= 0) {
                inChallengeGenerals = false;
            }
            continue;
        }

        if (currentGeneral < 0 || currentGeneral >= NUM_GENERALS) continue;

        container::String key, value;
        size_t eq = trimmed.find('=');
        if (eq != container::String::npos) {
            key = toLowerStr(trimStr(trimmed.substr(0, eq)));
            value = trimStr(trimmed.substr(eq + 1));
        } else {
            size_t space = trimmed.find(' ');
            if (space != container::String::npos) {
                key = toLowerStr(trimmed.substr(0, space));
                value = trimStr(trimmed.substr(space + 1));
            } else {
                continue;
            }
        }

        auto& g = m_generals[currentGeneral];

        if (key == "playertemplate") g.playerTemplateName = value;
        else if (key == "startsenabled") g.startsEnabled = parseIniBool(value);
        else if (key == "binamestring") g.bioName = value;
        else if (key == "biodobstring") g.bioDOB = value;
        else if (key == "biobirthplacestring") g.bioBirthplace = value;
        else if (key == "biostrategystring") g.bioStrategy = value;
        else if (key == "biorankstring") g.bioRank = value;
        else if (key == "biobranchstring") g.bioBranch = value;
        else if (key == "bioclassnumberstring") g.bioClassNumber = value;
        else if (key == "bioportraitsmall") g.imageBioPortraitSmall = value;
        else if (key == "bioportraitlarge") g.imageBioPortraitLarge = value;
        else if (key == "defeatedimage") g.imageDefeated = value;
        else if (key == "victoriousimage") g.imageVictorious = value;
        else if (key == "portraitmovieleftname") g.portraitMovieLeftName = value;
        else if (key == "portraitmovierightname") g.portraitMovieRightName = value;
        else if (key == "selectionsound") g.selectionSound = value;
        else if (key == "tauntsound1") g.tauntSound1 = value;
        else if (key == "tauntsound2") g.tauntSound2 = value;
        else if (key == "tauntsound3") g.tauntSound3 = value;
        else if (key == "winsound") g.winSound = value;
        else if (key == "losssound") g.lossSound = value;
        else if (key == "previewsound") g.previewSound = value;
        else if (key == "namesound") g.nameSound = value;
        else if (key == "defeatedstring") g.defeatedText = value;
        else if (key == "victoriousstring") g.victoriousText = value;
        else if (key == "campaign") g.campaign = value;
    }

    int enabled = getEnabledCount();
    TD_LOG_INFO("[ChallengeGenerals] Loaded {} sources from {}: {} generals, {} enabled",
                sources.size(), filename, NUM_GENERALS, enabled);
    return true;
}

GeneralPersona& ChallengeGenerals::getGeneral(int index) {
    static GeneralPersona s_empty;
    if (index < 0 || index >= NUM_GENERALS) return s_empty;
    return m_generals[index];
}

const GeneralPersona& ChallengeGenerals::getGeneral(int index) const {
    static const GeneralPersona s_empty;
    if (index < 0 || index >= NUM_GENERALS) return s_empty;
    return m_generals[index];
}

int ChallengeGenerals::getPlayerGeneralByCampaignName(const container::String& name) const {
    const container::String normalized = toLowerStr(name);
    for (int i = 0; i < NUM_GENERALS; ++i) {
        if (toLowerStr(m_generals[i].campaign) == normalized) return i;
    }
    return -1;
}

int ChallengeGenerals::getGeneralByTemplateName(const container::String& name) const {
    const container::String normalized = toLowerStr(name);
    for (int i = 0; i < NUM_GENERALS; ++i) {
        if (toLowerStr(m_generals[i].playerTemplateName) == normalized) return i;
    }
    return -1;
}

int ChallengeGenerals::getGeneralByGeneralName(const container::String& name) const {
    const container::String normalized = toLowerStr(name);
    for (int i = 0; i < NUM_GENERALS; ++i) {
        if (toLowerStr(m_generals[i].bioName) == normalized) return i;
    }
    return -1;
}

int ChallengeGenerals::getEnabledCount() const {
    int count = 0;
    for (const auto& g : m_generals) {
        if (g.startsEnabled) ++count;
    }
    return count;
}

} // namespace game
