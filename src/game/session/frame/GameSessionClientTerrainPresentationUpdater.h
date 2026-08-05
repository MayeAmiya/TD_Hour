#pragma once

namespace engine {

class GameSessionContentStartState;
class GameSessionGameplayPublicationPort;
class GameSessionWorldState;

// Advances confirmed client-terrain lifecycles and publishes detached FX.
// Renderer resources remain outside the session boundary.
class GameSessionClientTerrainPresentationUpdater final {
public:
    GameSessionClientTerrainPresentationUpdater(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionGameplayPublicationPort& publication) noexcept
        : m_content(content), m_world(world), m_publication(publication) {}

    void update(float deltaSeconds);

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionGameplayPublicationPort& m_publication;
};

} // namespace engine
