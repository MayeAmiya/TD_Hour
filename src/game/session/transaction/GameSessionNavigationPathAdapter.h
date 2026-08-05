#pragma once

#include "game/navigation/services/NavigationPathService.h"

namespace engine {

class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

class GameSessionNavigationPathAdapter final {
public:
    GameSessionNavigationPathAdapter(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation) noexcept
        : m_content(content), m_world(world), m_presentation(presentation) {}

    [[nodiscard]] navigation::NavigationAdapterSubmitResult submit(
        const ai::PathRequest& request, uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool poll(
        const ai::PathCorrelation& correlation, uint64_t confirmedTick,
        ai::PathFeedback& output) noexcept;

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
};

} // namespace engine
