#pragma once

#include "game/command/GameCommand.h"
#include "game/session/object/GameSessionObjectContracts.h"
#include "game/object/contracts/ObjectFixedGeometryTypes.h"
#include "game/session/command/OrderContracts.h"
#include "game/session/transaction/GameSessionBuildPlacementContracts.h"
#include "game/session/transaction/GameSessionObjectProductionTransactions.h"
#include "game/session/transaction/GameSessionObjectSaleTransactions.h"
#include "game/session/transaction/GameSessionScriptOrderAdmissionTransactions.h"

#include <cstdint>
#include <utility>

namespace game {
struct ObjectArchetype;
}

namespace engine {

class GameSessionObjectQueryPort;
class GameSessionStateRoot;

enum class ConfirmedCommandActivationRejection : uint8_t {
    None = 0,
    MalformedContext,
    DescriptorChanged,
    ActorUnavailable,
    AvailabilityChanged,
    SingleUseConsumed,
    ScienceUnavailable,
};

struct ConfirmedCommandActivationValidation final {
    ConfirmedCommandActivationRejection rejection =
        ConfirmedCommandActivationRejection::None;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return rejection == ConfirmedCommandActivationRejection::None;
    }
};

// Confirmed player-command mutations that do not belong to PlayerOrder.
// CommandDispatcher receives this capability instead of the full script API.
class GameSessionConfirmedCommandPort final {
public:
    explicit GameSessionConfirmedCommandPort(
        GameSessionStateRoot& state,
        GameSessionObjectProductionTransactions production,
        GameSessionScriptOrderAdmissionTransactions orderAdmission,
        GameSessionObjectSaleTransactions sale) noexcept
        : m_state(&state), m_production(std::move(production)),
          m_orderAdmission(std::move(orderAdmission)),
          m_sale(std::move(sale)) {}

    [[nodiscard]] bool objectForbidsPlayerCommands(
        ObjectId object) const noexcept;
    [[nodiscard]] ConfirmedCommandActivationValidation validateActivation(
        const GameCommand& command) const;
    [[nodiscard]] bool applyPostAccept(const GameCommand& command);
    [[nodiscard]] bool toggleOvercharge(
        ObjectId object, PlayerId player, uint64_t confirmedTick);
    [[nodiscard]] OrderExecutionResult toggleFormation(
        const GameCommand& command);
    [[nodiscard]] OrderExecutionResult scatter(const GameCommand& command);
    [[nodiscard]] bool purchaseScience(const GameCommand& command);
    [[nodiscard]] bool enterContainer(
        ObjectId object, ObjectId container, PlayerId player,
        uint64_t confirmedTick, uint32_t sourceSequence = 0);
    [[nodiscard]] bool exitContainer(
        ObjectId container, ObjectId passenger, PlayerId player,
        uint64_t confirmedTick);
    [[nodiscard]] bool evacuate(
        ObjectId container, PlayerId player, uint64_t confirmedTick,
        uint32_t sourceSequence = 0);
    [[nodiscard]] bool executeRailedTransport(
        ObjectId transport, PlayerId player, uint64_t confirmedTick);
    [[nodiscard]] bool sellObject(
        ObjectId object, PlayerId player, uint64_t confirmedTick);
    [[nodiscard]] bool cancelConstruction(
        ObjectId object, PlayerId player, uint64_t confirmedTick);
    [[nodiscard]] bool cancelOrderWaypoint(
        ObjectId actor, PlayerId player, uint32_t sourceSequence,
        uint64_t confirmedTick);
    [[nodiscard]] OrderExecutionResult repair(
        PlayerId player, container::Span<const ObjectId> actors,
        ObjectId structure, uint32_t sourceSequence,
        uint64_t confirmedTick);
    [[nodiscard]] OrderExecutionResult executeCommandButton(
        const GameCommand& command);
    [[nodiscard]] OrderExecutionResult executeSpecialPowerConstruct(
        const GameCommand& command);
    [[nodiscard]] OrderExecutionResult executeOrder(const PlayerOrder& order);

    [[nodiscard]] GameSessionCaptionCommandResult setBeaconText(
        PlayerId player,
        container::Span<const ObjectId> actors,
        container::StringView text,
        uint64_t confirmedTick);
    [[nodiscard]] GameSessionProductionCommandResult queueProduction(
        ObjectId producer, PlayerId player, container::StringView product,
        uint32_t sourceSequence, uint64_t confirmedTick);
    [[nodiscard]] GameSessionProductionCommandResult cancelProduction(
        ObjectId producer, PlayerId player, uint32_t productionId,
        uint64_t confirmedTick);
    [[nodiscard]] GameSessionProductionCommandResult queuePlayerUpgrade(
        ObjectId producer, PlayerId player, container::StringView upgrade,
        uint32_t sourceSequence, uint64_t confirmedTick);
    [[nodiscard]] GameSessionProductionCommandResult cancelPlayerUpgrade(
        ObjectId producer, PlayerId player, container::StringView upgrade,
        uint64_t confirmedTick);
    [[nodiscard]] GameSessionProductionCommandResult setFactoryRallyPoint(
        ObjectId producer, PlayerId player, LogicFixedVec3 position,
        uint64_t confirmedTick);

private:
    [[nodiscard]] GameSessionStateRoot& domainState() noexcept;
    [[nodiscard]] const GameSessionStateRoot& domainState() const noexcept;
    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] uint64_t confirmedTick() const noexcept;
    [[nodiscard]] GameSessionObjectQueryPort objectQuery() const noexcept;
    [[nodiscard]] GameSessionBuildPlacementLegalityEvaluation
    evaluateBuildPlacementFixed(
        ObjectId source, LogicFixedVec3 position,
        math::q32_32 yawRadians, PlayerId player,
        const game::ObjectArchetype& product,
        bool consumeBuildability);
    [[nodiscard]] bool setObjectDrawableCaption(
        ObjectId object, container::StringView text,
        uint64_t confirmedTick);
    [[nodiscard]] GameSessionObjectProductionTransactions
    productionTransactions() noexcept;

    GameSessionStateRoot* m_state = nullptr;
    GameSessionObjectProductionTransactions m_production;
    GameSessionScriptOrderAdmissionTransactions m_orderAdmission;
    GameSessionObjectSaleTransactions m_sale;
};

} // namespace engine
