#pragma once

#include "core/ecs/ObjectId.h"
#include "game/navigation/grid/NavigationDynamicOverlay.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

#include <optional>

namespace engine {

class GameSessionContentStartState;
class GameSessionFrameCommitState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

class GameSessionNavigationFootprintTransactions final {
public:
    GameSessionNavigationFootprintTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionFrameCommitState& frame) noexcept
        : m_content(content),
          m_world(world),
          m_presentation(presentation),
          m_publication(content, world, presentation, frame) {}

    [[nodiscard]] bool submitBuildingFootprint(
        ObjectId object, ecs::entity entity, uint64_t confirmedTick,
        navigation::NavigationDynamicEventReason reason,
        navigation::NavigationBuildingState state,
        std::optional<bool> blocksNavigationOverride = std::nullopt,
        std::optional<bool> blocksAirNavigationOverride = std::nullopt,
        std::optional<bool> rubbleSurfaceOverride = std::nullopt);
    [[nodiscard]] bool submitBuildingState(
        ObjectId object, uint64_t confirmedTick,
        navigation::NavigationDynamicEventReason reason,
        navigation::NavigationBuildingState state,
        bool blocksNavigation,
        bool blocksAirNavigation = false);

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
