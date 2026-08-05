#pragma once

#include <cstdint>

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/object/contracts/ObjectFixedGeometryTypes.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/player/PlayerTypes.h"
#include "game/session/object/GameSessionObjectContracts.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"

namespace engine
{

class GameSessionContentStartState;
class GameSessionWorldState;
class GameSessionScriptPresentationState;

// Confirmed production/upgrade queue admission shared by player commands,
// StrategicAI and script CommandButton routing. Buildability policy still
// consults the Session barrier when prerequisites must observe script overrides.
class GameSessionObjectProductionTransactions final
{
public:
    GameSessionObjectProductionTransactions(GameSessionContentStartState& content,
                                            GameSessionWorldState& world,
                                            GameSessionScriptPresentationState& presentation,
                                            GameSessionProductionPolicyPort policy,
                                            GameSessionLifecycleTransactionPort lifecycle) noexcept;

    [[nodiscard]] GameSessionProductionCommandResult queueProduction(ObjectId producer,
                                                                     PlayerId player,
                                                                     container::StringView productTemplate,
                                                                     uint32_t sourceSequence,
                                                                     uint64_t confirmedTick);
    [[nodiscard]] bool admitsBuildability(PlayerId player,
                                          const game::ObjectArchetype& product,
                                          bool& ignorePrerequisites) const noexcept;
    [[nodiscard]] GameSessionProductionCommandResult queuePlayerUpgrade(
        ObjectId producer,
        PlayerId player,
        container::StringView upgradeName,
        uint32_t sourceSequence,
        uint64_t confirmedTick,
        ObjectUpgradeProductionAdmission admission = ObjectUpgradeProductionAdmission::PlayerCommand);
    [[nodiscard]] GameSessionProductionCommandResult cancelProduction(ObjectId producer,
                                                                      PlayerId player,
                                                                      uint32_t productionId,
                                                                      uint64_t confirmedTick);
    [[nodiscard]] GameSessionProductionCommandResult cancelPlayerUpgrade(ObjectId producer,
                                                                         PlayerId player,
                                                                         container::StringView upgradeName,
                                                                         uint64_t confirmedTick);
    [[nodiscard]] GameSessionProductionCommandResult setFactoryRallyPoint(ObjectId producer,
                                                                          PlayerId player,
                                                                          LogicFixedVec3 position,
                                                                          uint64_t confirmedTick);
    [[nodiscard]] bool cancelConstruction(
        ObjectId object, PlayerId player, uint64_t confirmedTick);

    // Advances every live producer once, stamps detached completion intents
    // with the shared gameplay ordering clock, and publishes them to the
    // canonical transaction drain. Returns whether any completion was staged.
    [[nodiscard]] bool advanceConfirmedProduction();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionProductionPolicyPort m_policy;
    GameSessionLifecycleTransactionPort m_lifecycle;
};

} // namespace engine
