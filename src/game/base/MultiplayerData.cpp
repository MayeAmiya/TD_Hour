#include "core/container/container_types.h"
#include "game/base/MultiplayerData.h"

#include "VFS.h"
#include "debug/debug.h"
#include "game/data/base/ContentDiagnostics.h"

#include <algorithm>
#include <utility>
namespace game {
namespace {

void appendUnique(container::Vector<container::String>& values, container::StringView value) {
    if (value.empty()) return;
    const container::String candidate(value);
    if (std::find(values.begin(), values.end(), candidate) == values.end()) {
        values.push_back(candidate);
    }
}

[[nodiscard]] uint32_t toArgb(engine::PlayerRgbColor color) noexcept {
    return 0xff000000u |
        (static_cast<uint32_t>(color.red) << 16u) |
        (static_cast<uint32_t>(color.green) << 8u) |
        static_cast<uint32_t>(color.blue);
}

} // namespace

MultiplayerData& MultiplayerData::instance() {
    static MultiplayerData s_instance;
    return s_instance;
}

bool MultiplayerData::loadFromIni(const container::String& multiplayerIniPath,
                                  const container::String& playerTemplateIniPath) {
    // The original archives have appeared under both logical names across
    // distributions.  Prefer the configured VFS names, then try the known
    // system-file spellings without falling back to host filesystem paths.
    container::Vector<container::String> multiplayerCandidates;
    container::Vector<container::String> templateCandidates;
    appendUnique(multiplayerCandidates, multiplayerIniPath);
    appendUnique(multiplayerCandidates, "data/ini/Main_Multiplayer.ini");
    appendUnique(multiplayerCandidates, "Main_Multiplayer.ini");
    appendUnique(templateCandidates, playerTemplateIniPath);
    appendUnique(templateCandidates, "data/ini/Main_PlayerTemplate.ini");
    appendUnique(templateCandidates, "Main_PlayerTemplate.ini");

    container::String finalError;
    container::SharedPtr<engine::MultiplayerRuleset> loaded;
    for (const container::String& templatePath : templateCandidates) {
        if (!io::VFS::instance().exists(templatePath)) continue;
        for (const container::String& multiplayerPath : multiplayerCandidates) {
            if (!io::VFS::instance().exists(multiplayerPath)) continue;
            auto candidate = std::make_shared<engine::MultiplayerRuleset>();
            container::String error;
            if (candidate->loadFromVfs(multiplayerPath, templatePath, &error)) {
                loaded = std::move(candidate);
                break;
            }
            finalError = std::move(error);
        }
        if (loaded) break;
    }

    if (!loaded) {
        loaded = std::make_shared<engine::MultiplayerRuleset>();
        loaded->sealEmpty();
        processContentDiagnostics().warn({
            .source = "data/ini/Multiplayer+PlayerTemplate",
            .block = "MultiplayerRuleset",
            .module = "MultiplayerData",
            .rawValue = finalError.empty()
                ? "no compatible VFS INI pair found" : finalError,
            .adoptedValue = "sealed empty ruleset",
            .reason = "multiplayer/player-template content is unavailable; no factions or colors were fabricated",
        });
    }

    m_ruleset = std::move(loaded);
    rebuildLegacyViews();
    TD_LOG_INFO("[MultiplayerData] Ready: {} colors, {} cash options, {} templates, simulation={:016X}, content={:016X}",
                m_colors.size(), m_startingCash.size(), m_playerTemplates.size(),
                m_ruleset->simulationFingerprint(), m_ruleset->contentFingerprint());
    return true;
}

void MultiplayerData::rebuildLegacyViews() {
    m_colors.clear();
    m_startingCash.clear();
    m_playerTemplates.clear();
    m_defaultStartingCash = engine::DEFAULT_STARTING_CASH;
    if (!m_ruleset) return;

    const engine::MultiplayerRules& rules = m_ruleset->multiplayer();
    m_startingCash.reserve(rules.startingMoneyChoices.size());
    for (const int32_t amount : rules.startingMoneyChoices) {
        m_startingCash.push_back(amount);
    }
    m_defaultStartingCash = rules.defaultStartingMoney;

    m_colors.reserve(m_ruleset->colorIdsByAuthoredOrder().size());
    for (const engine::MultiplayerColorId id : m_ruleset->colorIdsByAuthoredOrder()) {
        const engine::MultiplayerColorDefinition* color = m_ruleset->findColor(id);
        if (!color) continue;
        m_colors.push_back({
            .name = color->name,
            .tooltipName = color->tooltipName,
            .rgb = toArgb(color->day),
            .rgbNight = toArgb(color->night),
        });
    }

    m_playerTemplates.reserve(m_ruleset->templateIdsByAuthoredOrder().size());
    for (const engine::FactionTemplateId id : m_ruleset->templateIdsByAuthoredOrder()) {
        const engine::FactionTemplate* templateValue = m_ruleset->findFaction(id);
        if (!templateValue) continue;
        m_playerTemplates.push_back({
            .id = id,
            .name = templateValue->name,
            .side = templateValue->side,
            .baseSide = templateValue->baseSide,
            .displayName = templateValue->presentation.displayName,
            .playable = templateValue->playable,
            .oldFaction = templateValue->oldFaction,
        });
    }
}

const PlayerTemplateDef* MultiplayerData::findTemplate(const container::String& name) const {
    if (!m_ruleset) return nullptr;
    const engine::FactionTemplate* source = m_ruleset->findFaction(name);
    if (!source) return nullptr;
    const auto found = std::find_if(m_playerTemplates.begin(), m_playerTemplates.end(),
        [&](const PlayerTemplateDef& templateValue) { return templateValue.id == source->id; });
    return found == m_playerTemplates.end() ? nullptr : &*found;
}

const PlayerTemplateDef* MultiplayerData::findPlayableTemplateByIndex(int playableIndex) const {
    if (playableIndex < 0 || !m_ruleset) return nullptr;
    const auto id = m_ruleset->playableTemplateIdAt(static_cast<size_t>(playableIndex));
    if (!id) return nullptr;
    const auto found = std::find_if(m_playerTemplates.begin(), m_playerTemplates.end(),
        [&](const PlayerTemplateDef& templateValue) { return templateValue.id == *id; });
    return found == m_playerTemplates.end() ? nullptr : &*found;
}

} // namespace game
