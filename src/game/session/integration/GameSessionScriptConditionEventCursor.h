#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/player/PlayerTypes.h"
#include "game/script/runtime/ScriptWorldQuery.h"

namespace engine {
class GameSessionContentStartState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
}

namespace engine::script {

// Destructive, ordered condition-event cursor for one confirmed script step.
// Pure ScriptWorldQuery implementations delegate here instead of owning
// mutable event ledgers themselves.
class GameSessionScriptConditionEventCursor final {
public:
    GameSessionScriptConditionEventCursor(
        GameSessionContentStartState& content,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents) noexcept
        : m_content(content),
          m_presentation(presentation),
          m_objectEvents(objectEvents) {}

    [[nodiscard]] container::Vector<ScriptWorldTeamUnitDestroyedEvent>
    takeTeamUnitDestroyedHookEvents();
    [[nodiscard]] container::Vector<ScriptWorldObjectHookEvent>
    takeObjectHookEvents();
    [[nodiscard]] bool consumePlayerScienceAcquired(
        PlayerId player, container::StringView science) noexcept;
    [[nodiscard]] bool consumeSpecialPowerEvent(
        ScriptSpecialPowerEventPhase phase, PlayerId player,
        container::StringView specialPower, ObjectId source) noexcept;
    [[nodiscard]] bool consumeUpgradeEvent(
        PlayerId player, container::StringView upgrade,
        ObjectId source) noexcept;
    [[nodiscard]] bool consumePresentationCompletion(
        ScriptPresentationCompletionKind kind,
        container::StringView mediaName) noexcept;
    [[nodiscard]] bool musicTrackHasCompleted(
        container::StringView trackName,
        int32_t minimumCompletedLoops) const noexcept;

private:
    [[nodiscard]] bool acceptsLocalPresentationCompletion() const noexcept;

    GameSessionContentStartState& m_content;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionObjectEventState& m_objectEvents;
};

} // namespace engine::script
