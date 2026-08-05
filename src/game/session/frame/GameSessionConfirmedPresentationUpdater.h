#pragma once

namespace engine {

class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Advances simulation-owned presentation components on confirmed time. It
// does not access renderer state or wall-clock presentation time.
class GameSessionConfirmedPresentationUpdater final {
public:
    GameSessionConfirmedPresentationUpdater(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation) noexcept
        : m_content(content), m_world(world), m_presentation(presentation) {}

    void update(float deltaSeconds);

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
};

} // namespace engine
