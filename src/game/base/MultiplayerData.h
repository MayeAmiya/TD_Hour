#pragma once

#include "core/container/container_types.h"

#include "game/player/FactionTemplate.h"

#include <cstdint>
#include "game/base/GameBalanceConstants.h"

namespace game {

struct ColorDef {
    container::String name;
    container::String tooltipName;
    uint32_t rgb = 0;
    uint32_t rgbNight = 0;
};

struct PlayerTemplateDef {
    engine::FactionTemplateId id = engine::INVALID_FACTION_TEMPLATE_ID;
    container::String name;
    container::String side;
    container::String baseSide;
    container::String displayName;
    bool playable = false;
    bool oldFaction = false;
};

class MultiplayerData {
public:
    static MultiplayerData& instance();

    bool loadFromIni(const container::String& multiplayerIniPath,
                     const container::String& playerTemplateIniPath);

    const container::Vector<ColorDef>& colors() const { return m_colors; }
    const container::Vector<int>& startingCash() const { return m_startingCash; }
    int defaultStartingCash() const { return m_defaultStartingCash; }
    const container::Vector<PlayerTemplateDef>& playerTemplates() const { return m_playerTemplates; }
    const engine::MultiplayerRuleset* ruleset() const noexcept { return m_ruleset.get(); }
    container::SharedPtr<const engine::MultiplayerRuleset> rulesetSnapshot() const noexcept {
        return m_ruleset;
    }
    bool hasRuleset() const noexcept { return static_cast<bool>(m_ruleset); }

    const PlayerTemplateDef* findTemplate(const container::String& name) const;
    const PlayerTemplateDef* findPlayableTemplateByIndex(int playableIndex) const;

private:
    MultiplayerData() = default;

    void rebuildLegacyViews();

    // Immutable rules are the authoritative source.  The vectors below are a
    // read-only legacy UI projection retained while menu code migrates from
    // positional indices to typed IDs.
    container::SharedPtr<const engine::MultiplayerRuleset> m_ruleset;
    container::Vector<ColorDef> m_colors;
    container::Vector<int> m_startingCash;
    int m_defaultStartingCash = engine::DEFAULT_STARTING_CASH;
    container::Vector<PlayerTemplateDef> m_playerTemplates;
};

} // namespace game
