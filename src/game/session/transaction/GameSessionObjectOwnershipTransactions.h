#pragma once

#include "core/ecs/ObjectId.h"
#include "game/object/contracts/ObjectTeamRegistry.h"
#include "game/player/PlayerTypes.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"

#include <cstdint>

namespace engine {

class GameSessionContentStartState;
class GameSessionWorldState;
class GameSessionAIState;
class GameSessionGameplayPublicationPort;
class GameSessionScriptPresentationState;
struct ObjectDefectionRequest;
struct ObjectPilotVehicleTakeoverRequest;

// Confirmed ownership / primary-team transfer for every setTeam-style ingress.
// ECS owner+team mutation, capture dependents, production refund, AI idle and
// difficulty reapplication live here. Lifecycle-event publication still runs
// through the Session barrier sink so Created/Destroy cascades stay singular.
class GameSessionObjectOwnershipTransactions final {
public:
    GameSessionObjectOwnershipTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionLifecycleTransactionPort lifecyclePublisher,
        GameSessionGameplayPublicationPort* publication = nullptr) noexcept;

    [[nodiscard]] bool transferObjectToTeam(
        ObjectId id, ObjectTeamId team, uint64_t confirmedTick);
    [[nodiscard]] bool transferTeamOwnership(
        ObjectTeamId team, PlayerId owner, uint64_t confirmedTick);
    void projectTeamRelationshipPolicy(ObjectTeamId team);
    [[nodiscard]] bool applyDifficultyBonusPolicy(
        ObjectId object, bool receiving, uint64_t confirmedTick);
    [[nodiscard]] bool applyDefection(
        const ObjectDefectionRequest& request);
    [[nodiscard]] bool applyPilotVehicleTakeover(
        const ObjectPilotVehicleTakeoverRequest& request);

private:
    [[nodiscard]] bool applyDefectionRecursive(
        const ObjectDefectionRequest& request,
        container::Vector<ObjectId>& visited);
    void refreshDerivedAggregates(uint64_t confirmedTick);
    void publishLifecycleAfterOwnerChange();

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionLifecycleTransactionPort m_lifecyclePublisher;
    GameSessionGameplayPublicationPort* m_publication = nullptr;
};

} // namespace engine
