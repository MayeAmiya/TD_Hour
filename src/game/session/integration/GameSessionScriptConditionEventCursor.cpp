#include "game/session/integration/GameSessionScriptConditionEventCursor.h"

#include "game/data/base/ScienceCatalog.h"
#include "game/session/state/GameSessionDomainState.h"

#include <utility>

namespace engine::script {

container::Vector<ScriptWorldTeamUnitDestroyedEvent>
GameSessionScriptConditionEventCursor::takeTeamUnitDestroyedHookEvents() {
    container::Vector<ScriptWorldTeamUnitDestroyedEvent> output =
        std::move(m_objectEvents.m_teamUnitDestroyedHookEvents);
    m_objectEvents.m_teamUnitDestroyedHookEvents.clear();
    return output;
}

container::Vector<ScriptWorldObjectHookEvent>
GameSessionScriptConditionEventCursor::takeObjectHookEvents() {
    container::Vector<ScriptWorldObjectHookEvent> output =
        std::move(m_objectEvents.m_objectHookEvents);
    m_objectEvents.m_objectHookEvents.clear();
    return output;
}

bool GameSessionScriptConditionEventCursor::consumePlayerScienceAcquired(
    PlayerId player, container::StringView science) noexcept {
    const ScienceCatalog* catalog =
        m_content.m_contentSnapshot.scienceCatalog();
    if (!player || science.empty() || !catalog || !catalog->isLoaded())
        return false;
    const ScienceDefinition* definition = catalog->find(science);
    return definition && m_content.m_players.consumeScienceAcquired(
        player, definition->name);
}

bool GameSessionScriptConditionEventCursor::consumeSpecialPowerEvent(
    ScriptSpecialPowerEventPhase phase, PlayerId player,
    container::StringView specialPower, ObjectId source) noexcept {
    return m_presentation.m_scriptGameplayEvents.consumeSpecialPower(
        phase, player, specialPower, source).has_value();
}

bool GameSessionScriptConditionEventCursor::consumeUpgradeEvent(
    PlayerId player, container::StringView upgrade,
    ObjectId source) noexcept {
    return m_presentation.m_scriptGameplayEvents.consumeUpgrade(
        player, upgrade, source).has_value();
}

bool GameSessionScriptConditionEventCursor::consumePresentationCompletion(
    ScriptPresentationCompletionKind kind,
    container::StringView mediaName) noexcept {
    if (!m_content.m_active) return false;
    if (kind != ScriptPresentationCompletionKind::Video &&
        !acceptsLocalPresentationCompletion()) {
        return false;
    }
    return m_presentation.m_scriptPresentationCompletions.consumeOne(
        kind, mediaName).has_value();
}

bool GameSessionScriptConditionEventCursor::musicTrackHasCompleted(
    container::StringView trackName,
    int32_t minimumCompletedLoops) const noexcept {
    return acceptsLocalPresentationCompletion() &&
        m_presentation.m_scriptPresentationCompletions.musicTrackHasCompleted(
            trackName, minimumCompletedLoops);
}

bool GameSessionScriptConditionEventCursor::
acceptsLocalPresentationCompletion() const noexcept {
    return m_content.m_active && !m_content.m_startInfo.network.enabled &&
        m_content.m_startInfo.mode != GameMode::Replay;
}

} // namespace engine::script
