#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/navigation/grid/NavigationDynamicOverlay.h"
#include "game/navigation/grid/NavigationTypes.h"
#include "game/navigation/runtime/NavigationSystem.h"

#include <cstdint>

namespace engine {

class GameSessionContentStartState;
class GameSessionGameplayPublicationPort;
class GameSessionScriptPresentationState;

// Confirmed dynamic-navigation mutation boundary. Gameplay systems submit
// value events here and never reach through Session/AIDomain to Navigation.
class GameSessionNavigationTransactions final {
public:
    GameSessionNavigationTransactions(
        GameSessionContentStartState& content,
        GameSessionScriptPresentationState& presentation) noexcept;

    [[nodiscard]] bool submitBuildingState(
        ObjectId object, uint64_t confirmedTick,
        navigation::NavigationDynamicEventReason reason,
        navigation::NavigationBuildingState state,
        bool blocksNavigation,
        bool blocksAirNavigation = false);
    [[nodiscard]] navigation::NavigationDynamicOverlayResult
    submitBridgeState(
        uint64_t bridgeId, navigation::NavigationLayerId bridgeLayer,
        bool active,
        container::Span<const navigation::NavigationCellId> affectedCells,
        uint64_t confirmedTick) noexcept;

    [[nodiscard]] navigation::NavigationSystemStatus
    synchronizeTerrainAuthority();
    [[nodiscard]] navigation::NavigationSystemAdvanceResult
    advanceConfirmedTick(uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool advanceConfirmedTickAndPublishFault(
        uint64_t confirmedTick,
        GameSessionGameplayPublicationPort publication) noexcept;

private:
    GameSessionContentStartState& m_content;
    GameSessionScriptPresentationState& m_presentation;
};

} // namespace engine
