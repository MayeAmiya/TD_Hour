#pragma once

#include "game/player/FactionTemplate.h"

#include <optional>

namespace engine {

struct PlayerState;

// Narrow read-only projection over the frozen session ruleset. Consumers get
// presentation values, never the ruleset owner or pointers into its storage.
class GameSessionRulesetQueryPort final {
public:
    explicit GameSessionRulesetQueryPort(
        const MultiplayerRuleset* ruleset) noexcept
        : m_ruleset(ruleset) {}

    [[nodiscard]] std::optional<PlayerRgbColor> presentationColor(
        const PlayerState& player, bool night = false) const noexcept;

    [[nodiscard]] std::optional<PlayerTemplatePresentationData>
    factionPresentation(container::StringView factionName) const;
    [[nodiscard]] std::optional<PlayerTemplatePresentationData>
    factionPresentation(FactionTemplateId faction) const noexcept;

private:
    const MultiplayerRuleset* m_ruleset = nullptr;
};

} // namespace engine
