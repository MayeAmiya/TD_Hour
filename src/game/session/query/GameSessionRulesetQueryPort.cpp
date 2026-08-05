#include "game/session/query/GameSessionRulesetQueryPort.h"

#include "game/player/PlayerRegistry.h"

namespace engine {

std::optional<PlayerRgbColor>
GameSessionRulesetQueryPort::presentationColor(
    const PlayerState& player, bool night) const noexcept {
    if (!m_ruleset) return std::nullopt;
    return resolvePlayerPresentationColor(player, *m_ruleset, night);
}

std::optional<PlayerTemplatePresentationData>
GameSessionRulesetQueryPort::factionPresentation(
    container::StringView factionName) const {
    if (!m_ruleset) return std::nullopt;
    const FactionTemplate* faction = m_ruleset->findFaction(factionName);
    return faction
        ? std::optional<PlayerTemplatePresentationData>{faction->presentation}
        : std::nullopt;
}

std::optional<PlayerTemplatePresentationData>
GameSessionRulesetQueryPort::factionPresentation(
    FactionTemplateId factionId) const noexcept {
    if (!m_ruleset) return std::nullopt;
    const FactionTemplate* faction = m_ruleset->findFaction(factionId);
    return faction
        ? std::optional<PlayerTemplatePresentationData>{faction->presentation}
        : std::nullopt;
}

} // namespace engine
