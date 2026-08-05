#include "core/container/container_types.h"
#include "CommandDispatcher.h"

#include "game/command/CommandCodec.h"
#include "game/command/CommandButtonTypes.h"
#include "game/session/command/OrderExecutor.h"
#include "debug/debug.h"
#include "game/session/core/GameSession.h"

#include <utility>
#include <algorithm>

namespace engine {
namespace {

const char* commandTypeName(GameCommandType type) {
    switch (type) {
    case GameCommandType::None: return "None";
    case GameCommandType::UIAction: return "UIAction";
    case GameCommandType::CommandButton: return "CommandButton";
    case GameCommandType::Move: return "Move";
    case GameCommandType::Attack: return "Attack";
    case GameCommandType::Build: return "Build";
    case GameCommandType::SpecialPower: return "SpecialPower";
    case GameCommandType::Pause: return "Pause";
    case GameCommandType::Surrender: return "Surrender";
    case GameCommandType::Stop: return "Stop";
    case GameCommandType::QueueProduction: return "QueueProduction";
    case GameCommandType::CancelProduction: return "CancelProduction";
    case GameCommandType::QueuePlayerUpgrade: return "QueuePlayerUpgrade";
    case GameCommandType::CancelPlayerUpgrade: return "CancelPlayerUpgrade";
    case GameCommandType::SetFactoryRallyPoint: return "SetFactoryRallyPoint";
    case GameCommandType::AttackMove: return "AttackMove";
    case GameCommandType::SetBeaconText: return "SetBeaconText";
    case GameCommandType::Repair: return "Repair";
    case GameCommandType::Sell: return "Sell";
    case GameCommandType::CancelConstruction: return "CancelConstruction";
    case GameCommandType::ExitContainer: return "ExitContainer";
    case GameCommandType::Evacuate: return "Evacuate";
    case GameCommandType::ExecuteRailedTransport: return "ExecuteRailedTransport";
    case GameCommandType::EnterContainer: return "EnterContainer";
    case GameCommandType::CombatDrop: return "CombatDrop";
    case GameCommandType::PurchaseScience: return "PurchaseScience";
    case GameCommandType::Scatter: return "Scatter";
    case GameCommandType::CreateFormation: return "CreateFormation";
    case GameCommandType::Guard: return "Guard";
    case GameCommandType::GuardWithoutPursuit: return "GuardWithoutPursuit";
    case GameCommandType::GuardFlyingUnitsOnly: return "GuardFlyingUnitsOnly";
    case GameCommandType::ToggleOvercharge: return "ToggleOvercharge";
    case GameCommandType::CancelOrderWaypoint: return "CancelOrderWaypoint";
    }
    return "Unknown";
}

const char* commandSourceName(CommandSource source) {
    switch (source) {
    case CommandSource::Local: return "Local";
    case CommandSource::Network: return "Network";
    case CommandSource::Replay: return "Replay";
    }
    return "Unknown";
}

[[nodiscard]] bool hasNoPositionPayload(const CommandPosition& position) noexcept {
    return !position.valid && position.x.raw() == 0 &&
        position.y.raw() == 0 && position.z.raw() == 0;
}

[[nodiscard]] const char* factoryProductionPayloadError(const GameCommand& command) noexcept {
    if (!command.player.isMapPlayer()) return "factory-production command has an invalid player";
    if (command.actors.size() != 1 || !command.actors.front()) {
        return "factory-production command requires exactly one nonzero factory actor";
    }
    switch (command.type) {
    case GameCommandType::QueueProduction:
        if (command.commandName.empty()) return "queue-production command requires a product template";
        if (command.productionId != 0 || command.targetObject || command.queued ||
            !hasNoPositionPayload(command.targetPosition)) {
            return "queue-production command contains unrelated payload fields";
        }
        return nullptr;
    case GameCommandType::CancelProduction:
        if (command.productionId == 0) return "cancel-production command requires a nonzero production ID";
        if (!command.commandName.empty() || command.targetObject || command.queued ||
            !hasNoPositionPayload(command.targetPosition)) {
            return "cancel-production command contains unrelated payload fields";
        }
        return nullptr;
    case GameCommandType::QueuePlayerUpgrade:
        if (command.commandName.empty()) return "queue-player-upgrade command requires an upgrade name";
        if (command.productionId != 0 || command.targetObject || command.queued ||
            !hasNoPositionPayload(command.targetPosition)) {
            return "queue-player-upgrade command contains unrelated payload fields";
        }
        return nullptr;
    case GameCommandType::CancelPlayerUpgrade:
        if (command.commandName.empty()) return "cancel-player-upgrade command requires an upgrade name";
        if (command.productionId != 0 || command.targetObject || command.queued ||
            !hasNoPositionPayload(command.targetPosition)) {
            return "cancel-player-upgrade command contains unrelated payload fields";
        }
        return nullptr;
    case GameCommandType::SetFactoryRallyPoint:
        if (!command.targetPosition.valid) return "factory-rally command requires a target position";
        if (!command.commandName.empty() || command.productionId != 0 || command.targetObject ||
            command.queued) {
            return "factory-rally command contains unrelated payload fields";
        }
        return nullptr;
    case GameCommandType::None:
    case GameCommandType::UIAction:
    case GameCommandType::CommandButton:
    case GameCommandType::Move:
    case GameCommandType::AttackMove:
    case GameCommandType::Attack:
    case GameCommandType::Build:
    case GameCommandType::SpecialPower:
    case GameCommandType::Pause:
    case GameCommandType::Surrender:
    case GameCommandType::Stop:
    case GameCommandType::SetBeaconText:
        return "command is not a factory-production transaction";
    case GameCommandType::Repair:
    case GameCommandType::Sell:
    case GameCommandType::CancelConstruction:
    case GameCommandType::ExitContainer:
    case GameCommandType::Evacuate:
    case GameCommandType::ExecuteRailedTransport:
    case GameCommandType::EnterContainer:
    case GameCommandType::CombatDrop:
    case GameCommandType::PurchaseScience:
    case GameCommandType::Scatter:
    case GameCommandType::CreateFormation:
    case GameCommandType::Guard:
    case GameCommandType::GuardWithoutPursuit:
    case GameCommandType::GuardFlyingUnitsOnly:
    case GameCommandType::ToggleOvercharge:
    case GameCommandType::CancelOrderWaypoint:
        return "command is not a factory-production transaction";
    }
    return "unknown factory-production command";
}

[[nodiscard]] const char* beaconTextPayloadError(
    const GameCommand& command) noexcept {
    if (!command.player.isMapPlayer())
        return "beacon-text command has an invalid player";
    if (command.actors.size() != 1u)
        return "beacon-text command requires exactly one actor";
    if (command.commandName.size() > CommandCodec::MaximumCommandNameBytes)
        return "beacon-text command exceeds its UTF-8 byte budget";
    if (command.productionId != 0 || command.targetObject || command.queued ||
        !hasNoPositionPayload(command.targetPosition) ||
        command.placementYawRadians.raw() != 0 ||
        !hasNoPositionPayload(command.placementEndPosition)) {
        return "beacon-text command contains unrelated payload fields";
    }
    return nullptr;
}

[[nodiscard]] const char* repairPayloadError(
    const GameCommand& command) noexcept {
    if (!command.player.isMapPlayer())
        return "repair command has an invalid player";
    if (command.actors.empty() || command.actors.size() > CommandCodec::MaximumActors)
        return "repair command requires a non-empty bounded actor group";
    if (!command.targetObject)
        return "repair command requires a target object";
    if (!command.commandName.empty() || command.productionId != 0 ||
        command.queued || !hasNoPositionPayload(command.targetPosition) ||
        command.placementYawRadians.raw() != 0 ||
        !hasNoPositionPayload(command.placementEndPosition)) {
        return "repair command contains unrelated payload fields";
    }
    return nullptr;
}

void logFactoryProductionResult(const GameCommand& command, bool accepted,
                                const container::String& message) {
    if (accepted) {
        TD_LOG_TRACE("[CommandDispatcher] Applied {} tick={} seq={} player={} factory={} source={}",
                     commandTypeName(command.type), command.tick, command.sequence,
                     command.player.value, command.actors.front().value,
                     commandSourceName(command.source));
        return;
    }
    TD_LOG_DEBUG("[CommandDispatcher] Rejected {} tick={} seq={} player={} factory={} source={}: {}",
                commandTypeName(command.type), command.tick, command.sequence,
                command.player.value, command.actors.front().value,
                commandSourceName(command.source), message);
}

} // namespace

CommandDispatchResult CommandDispatcher::dispatch(GameSession& session, const GameCommand& command) {
    GameSessionConfirmedCommandPort confirmedCommands =
        session.confirmedCommandPort();
    if (command.forceAttack && command.type != GameCommandType::Attack) {
        return {
            .accepted = false,
            .producedOrder = false,
            .rejection = CommandDispatchRejection::MalformedPayload,
            .message = "force-attack is valid only for attack commands",
        };
    }
    switch (command.type) {
    case GameCommandType::ToggleOvercharge: {
        if (!command.player.isMapPlayer() || command.actors.empty() ||
            command.actors.size() > CommandCodec::MaximumActors ||
            !command.commandName.empty() || command.productionId != 0 ||
            command.targetObject || command.queued || command.forceAttack ||
            !hasNoPositionPayload(command.targetPosition) ||
            command.placementYawRadians.raw() != 0 ||
            !hasNoPositionPayload(command.placementEndPosition)) {
            return {.accepted = false,
                    .rejection = CommandDispatchRejection::MalformedPayload,
                    .message = "malformed overcharge command"};
        }
        size_t applied = 0;
        for (const ObjectId actor : command.actors) {
            applied += confirmedCommands.toggleOvercharge(
                actor, command.player, command.tick) ? 1u : 0u;
        }
        return {
            .accepted = applied != 0,
            .actorCount = applied,
            .rejection = applied != 0
                ? CommandDispatchRejection::None
                : CommandDispatchRejection::Rejected,
            .message = applied != 0
                ? container::String{}
                : container::String{"no selected power plant accepted overcharge"},
        };
    }
    case GameCommandType::CreateFormation: {
        if (!command.player.isMapPlayer() || command.actors.empty() ||
            command.actors.size() > CommandCodec::MaximumActors ||
            !command.commandName.empty() || command.productionId != 0 ||
            command.targetObject || command.queued || command.forceAttack ||
            !hasNoPositionPayload(command.targetPosition) ||
            command.placementYawRadians.raw() != 0 ||
            !hasNoPositionPayload(command.placementEndPosition)) {
            return {
                .accepted = false,
                .rejection = CommandDispatchRejection::MalformedPayload,
                .message = "malformed create-formation command",
            };
        }
        const OrderExecutionResult result =
            confirmedCommands.toggleFormation(command);
        return {
            .accepted = result.accepted,
            .actorCount = result.actorCount,
            .rejection = result.accepted
                ? CommandDispatchRejection::None
                : CommandDispatchRejection::Rejected,
            .message = result.message,
        };
    }
    case GameCommandType::Scatter: {
        if (!command.player.isMapPlayer() || command.actors.empty() ||
            command.actors.size() > CommandCodec::MaximumActors ||
            !command.commandName.empty() || command.productionId != 0 ||
            command.targetObject || command.queued ||
            !hasNoPositionPayload(command.targetPosition) ||
            command.placementYawRadians.raw() != 0 ||
            !hasNoPositionPayload(command.placementEndPosition)) {
            return {
                .accepted = false,
                .producedOrder = false,
                .rejection = CommandDispatchRejection::MalformedPayload,
                .message = "malformed scatter command",
            };
        }
        const OrderExecutionResult result = confirmedCommands.scatter(command);
        return {
            .accepted = result.accepted,
            .producedOrder = result.actorCount != 0u,
            .actorCount = result.actorCount,
            .rejection = result.accepted
                ? CommandDispatchRejection::None
                : result.rejection == OrderRejectionReason::MalformedOrder
                    ? CommandDispatchRejection::MalformedPayload
                    : result.rejection == OrderRejectionReason::UnsupportedCommand
                        ? CommandDispatchRejection::Unsupported
                        : CommandDispatchRejection::Rejected,
            .message = result.message,
        };
    }
    case GameCommandType::PurchaseScience: {
        if (!command.player.isMapPlayer() || !command.actors.empty() ||
            command.commandName.empty() || command.productionId != 0 ||
            command.targetObject || command.queued ||
            !hasNoPositionPayload(command.targetPosition) ||
            command.placementYawRadians.raw() != 0 ||
            !hasNoPositionPayload(command.placementEndPosition)) {
            return {
                .accepted = false,
                .producedOrder = false,
                .rejection = CommandDispatchRejection::MalformedPayload,
                .message = "malformed science-purchase command",
            };
        }
        const bool accepted = confirmedCommands.purchaseScience(command);
        return {
            .accepted = accepted,
            .producedOrder = false,
            .rejection = accepted
                ? CommandDispatchRejection::None
                : CommandDispatchRejection::Rejected,
            .message = accepted
                ? container::String{}
                : container::String{
                      "science is missing, unavailable, already owned, or not purchasable"},
        };
    }
    case GameCommandType::EnterContainer: {
        if (!command.player.isMapPlayer() || command.actors.empty() ||
            command.actors.size() > CommandCodec::MaximumActors ||
            !command.targetObject || !command.commandName.empty() ||
            command.productionId != 0 || command.queued ||
            !hasNoPositionPayload(command.targetPosition) ||
            command.placementYawRadians.raw() != 0 ||
            !hasNoPositionPayload(command.placementEndPosition)) {
            return {.accepted = false, .producedOrder = false,
                    .rejection = CommandDispatchRejection::MalformedPayload,
                    .message = "malformed containment-entry command"};
        }
        size_t applied = 0;
        for (const ObjectId actor : command.actors) {
            applied += confirmedCommands.enterContainer(
                actor, command.targetObject, command.player, command.tick,
                command.sequence) ? 1u : 0u;
        }
        return {
            .accepted = applied != 0,
            .producedOrder = applied != 0,
            .rejection = applied != 0
                ? CommandDispatchRejection::None
                : CommandDispatchRejection::Rejected,
            .message = applied != 0
                ? container::String{}
                : container::String{"no selected passenger could enter the target"},
        };
    }
    case GameCommandType::ExitContainer: {
        if (!command.player.isMapPlayer() || command.actors.size() != 1u ||
            !command.actors.front() || !command.targetObject ||
            command.targetObject == command.actors.front() ||
            !command.commandName.empty() || command.productionId != 0 ||
            command.queued || !hasNoPositionPayload(command.targetPosition) ||
            command.placementYawRadians.raw() != 0 ||
            !hasNoPositionPayload(command.placementEndPosition)) {
            return {.accepted = false, .producedOrder = false,
                    .rejection = CommandDispatchRejection::MalformedPayload,
                    .message = "malformed containment command"};
        }
        const bool accepted = confirmedCommands.exitContainer(
            command.actors.front(), command.targetObject,
            command.player, command.tick);
        return {
            .accepted = accepted,
            .producedOrder = false,
            .rejection = accepted
                ? CommandDispatchRejection::None
                : CommandDispatchRejection::Rejected,
            .message = accepted
                ? container::String{}
                : container::String{"containment command had no eligible owned actor"},
        };
    }
    case GameCommandType::Evacuate:
    case GameCommandType::ExecuteRailedTransport: {
        if (!command.player.isMapPlayer() || command.actors.empty() ||
            command.actors.size() > CommandCodec::MaximumActors ||
            !command.commandName.empty() || command.productionId != 0 ||
            command.targetObject || command.queued ||
            !hasNoPositionPayload(command.targetPosition) ||
            command.placementYawRadians.raw() != 0 ||
            !hasNoPositionPayload(command.placementEndPosition)) {
            return {.accepted = false, .producedOrder = false,
                    .rejection = CommandDispatchRejection::MalformedPayload,
                    .message = "malformed selected-group transaction"};
        }
        size_t applied = 0;
        for (const ObjectId actor : command.actors) {
            const bool accepted = command.type == GameCommandType::Evacuate
                ? confirmedCommands.evacuate(
                      actor, command.player, command.tick,
                      command.sequence)
                : confirmedCommands.executeRailedTransport(
                      actor, command.player, command.tick);
            applied += accepted ? 1u : 0u;
        }
        return {
            .accepted = applied != 0,
            .producedOrder = false,
            .rejection = applied != 0
                ? CommandDispatchRejection::None
                : CommandDispatchRejection::Rejected,
            .message = applied != 0
                ? container::String{}
                : container::String{"no selected actor accepted the transaction"},
        };
    }
    case GameCommandType::Sell:
    case GameCommandType::CancelConstruction: {
        const bool exactOne =
            command.type == GameCommandType::CancelConstruction;
        if (!command.player.isMapPlayer() || command.actors.empty() ||
            (exactOne && command.actors.size() != 1u) ||
            !command.commandName.empty() || command.productionId != 0 ||
            command.targetObject || command.queued ||
            !hasNoPositionPayload(command.targetPosition) ||
            command.placementYawRadians.raw() != 0 ||
            !hasNoPositionPayload(command.placementEndPosition)) {
            return {.accepted = false, .producedOrder = false,
                    .rejection = CommandDispatchRejection::MalformedPayload,
                    .message = "malformed object-retirement command"};
        }
        size_t applied = 0;
        for (const ObjectId actor : command.actors) {
            const bool accepted = command.type == GameCommandType::Sell
                ? confirmedCommands.sellObject(
                      actor, command.player, command.tick)
                : confirmedCommands.cancelConstruction(
                      actor, command.player, command.tick);
            applied += accepted ? 1u : 0u;
        }
        return {
            .accepted = applied != 0,
            .producedOrder = false,
            .rejection = applied != 0
                ? CommandDispatchRejection::None
                : CommandDispatchRejection::Rejected,
            .message = applied != 0
                ? container::String{}
                : container::String{"no eligible owned object accepted the command"},
        };
    }
    case GameCommandType::CancelOrderWaypoint: {
        if (!command.player.isMapPlayer() || command.actors.size() != 1u ||
            !command.actors.front() || !command.commandName.empty() ||
            command.productionId == 0 || command.targetObject ||
            command.queued ||
            !hasNoPositionPayload(command.targetPosition) ||
            command.placementYawRadians.raw() != 0 ||
            !hasNoPositionPayload(command.placementEndPosition)) {
            return {.accepted = false, .producedOrder = false,
                    .rejection = CommandDispatchRejection::MalformedPayload,
                    .message = "malformed waypoint-cancellation command"};
        }
        const bool accepted = confirmedCommands.cancelOrderWaypoint(
            command.actors.front(), command.player,
            command.productionId, command.tick);
        return {
            .accepted = accepted,
            .producedOrder = false,
            .rejection = accepted
                ? CommandDispatchRejection::None
                : CommandDispatchRejection::Rejected,
            .message = accepted ? container::String{}
                                : container::String{
                                      "waypoint no longer exists"},
        };
    }
    case GameCommandType::Repair: {
        if (const char* error = repairPayloadError(command)) {
            TD_LOG_DEBUG("[CommandDispatcher] Rejected Repair tick={} seq={} player={}: {}",
                        command.tick, command.sequence, command.player.value, error);
            return {.accepted = false, .producedOrder = false,
                    .rejection = CommandDispatchRejection::MalformedPayload,
                    .message = error};
        }
        const OrderExecutionResult result = confirmedCommands.repair(
            command.player,
            container::Span<const ObjectId>{command.actors.data(), command.actors.size()},
            command.targetObject, command.sequence, command.tick);
        if (!result.accepted) {
            TD_LOG_DEBUG("[CommandDispatcher] Rejected Repair tick={} seq={} player={} actors={} target={}: {}",
                        command.tick, command.sequence, command.player.value,
                        command.actors.size(), command.targetObject.value,
                        result.message);
        } else {
            TD_LOG_TRACE("[CommandDispatcher] Applied Repair tick={} seq={} player={} assigned={} target={} source={}",
                         command.tick, command.sequence, command.player.value,
                         result.actorCount, command.targetObject.value,
                         commandSourceName(command.source));
        }
        return {.accepted = result.accepted, .producedOrder = true,
                .rejection = result.accepted
                    ? CommandDispatchRejection::None
                    : result.rejection == OrderRejectionReason::UnsupportedCommand
                        ? CommandDispatchRejection::Unsupported
                        : result.rejection == OrderRejectionReason::MalformedOrder
                            ? CommandDispatchRejection::MalformedPayload
                            : CommandDispatchRejection::Rejected,
                .message = result.message};
    }
    case GameCommandType::CommandButton: {
        const OrderExecutionResult result =
            confirmedCommands.executeCommandButton(command);
        return {
            .accepted = result.accepted,
            .producedOrder = result.actorCount != 0,
            .actorCount = result.actorCount,
            .rejection = result.accepted
                ? CommandDispatchRejection::None
                : result.rejection == OrderRejectionReason::UnsupportedCommand
                    ? CommandDispatchRejection::Unsupported
                    : result.rejection == OrderRejectionReason::MalformedOrder
                        ? CommandDispatchRejection::MalformedPayload
                        : CommandDispatchRejection::Rejected,
            .message = result.message,
        };
    }
    case GameCommandType::SpecialPower:
        if (command.activation.commandKind ==
                ::game::CommandButtonKind::SpecialPowerConstruct ||
            command.activation.commandKind == ::game::CommandButtonKind::
                SpecialPowerConstructFromShortcut) {
            const OrderExecutionResult result =
                confirmedCommands.executeSpecialPowerConstruct(command);
            return {
                .accepted = result.accepted,
                .producedOrder = result.actorCount != 0,
                .actorCount = result.actorCount,
                .rejection = result.accepted
                    ? CommandDispatchRejection::None
                    : result.rejection ==
                            OrderRejectionReason::UnsupportedCommand
                        ? CommandDispatchRejection::Unsupported
                        : result.rejection ==
                                OrderRejectionReason::MalformedOrder
                            ? CommandDispatchRejection::MalformedPayload
                            : CommandDispatchRejection::Rejected,
                .message = result.message,
            };
        }
        break;
    case GameCommandType::SetBeaconText: {
        if (const char* error = beaconTextPayloadError(command)) {
            TD_LOG_DEBUG("[CommandDispatcher] Rejected SetBeaconText tick={} seq={} player={}: {}",
                        command.tick, command.sequence, command.player.value, error);
            return {.accepted = false, .producedOrder = false,
                    .rejection = CommandDispatchRejection::MalformedPayload,
                    .message = error};
        }
        GameSessionCaptionCommandResult result = session.confirmedCommandPort().setBeaconText(
            command.player,
            container::Span<const ObjectId>{command.actors.data(), command.actors.size()},
            command.commandName, command.tick);
        if (!result.accepted) {
            TD_LOG_DEBUG("[CommandDispatcher] Rejected SetBeaconText tick={} seq={} player={} actors={}: {}",
                        command.tick, command.sequence, command.player.value,
                        command.actors.size(), result.message);
        } else {
            TD_LOG_TRACE("[CommandDispatcher] Applied SetBeaconText tick={} seq={} player={} actors={} changed={} source={}",
                         command.tick, command.sequence, command.player.value,
                         result.actorCount, result.changedCount,
                         commandSourceName(command.source));
        }
        return {.accepted = result.accepted, .producedOrder = false,
                .rejection = result.accepted
                    ? CommandDispatchRejection::None
                    : CommandDispatchRejection::Rejected,
                .message = std::move(result.message)};
    }
    case GameCommandType::QueueProduction:
    case GameCommandType::CancelProduction:
    case GameCommandType::QueuePlayerUpgrade:
    case GameCommandType::CancelPlayerUpgrade:
    case GameCommandType::SetFactoryRallyPoint: {
        if (const char* error = factoryProductionPayloadError(command)) {
            TD_LOG_DEBUG("[CommandDispatcher] Rejected tick={} seq={} player={} source={} type={}: {}",
                        command.tick, command.sequence, command.player.value,
                        commandSourceName(command.source), commandTypeName(command.type), error);
            return {.accepted = false, .producedOrder = false,
                    .rejection = CommandDispatchRejection::MalformedPayload,
                    .message = error};
        }
        if (confirmedCommands.objectForbidsPlayerCommands(
                command.actors.front())) {
            return {
                .accepted = true,
                .producedOrder = false,
                .actorCount = 0,
            };
        }

        if (command.type == GameCommandType::QueueProduction) {
            auto result = session.confirmedCommandPort().queueProduction(command.actors.front(), command.player,
                                                  command.commandName, command.sequence, command.tick);
            logFactoryProductionResult(command, result.accepted, result.message);
            return {.accepted = result.accepted, .producedOrder = false,
                    .rejection = result.accepted
                        ? CommandDispatchRejection::None
                        : CommandDispatchRejection::Rejected,
                    .message = std::move(result.message)};
        }
        if (command.type == GameCommandType::CancelProduction) {
            auto result = session.confirmedCommandPort().cancelProduction(command.actors.front(), command.player,
                                                   command.productionId, command.tick);
            logFactoryProductionResult(command, result.accepted, result.message);
            return {.accepted = result.accepted, .producedOrder = false,
                    .rejection = result.accepted
                        ? CommandDispatchRejection::None
                        : CommandDispatchRejection::Rejected,
                    .message = std::move(result.message)};
        }
        if (command.type == GameCommandType::QueuePlayerUpgrade) {
            auto result = session.confirmedCommandPort().queuePlayerUpgrade(command.actors.front(), command.player,
                                                     command.commandName, command.sequence, command.tick);
            logFactoryProductionResult(command, result.accepted, result.message);
            return {.accepted = result.accepted, .producedOrder = false,
                    .rejection = result.accepted
                        ? CommandDispatchRejection::None
                        : CommandDispatchRejection::Rejected,
                    .message = std::move(result.message)};
        }
        if (command.type == GameCommandType::CancelPlayerUpgrade) {
            auto result = session.confirmedCommandPort().cancelPlayerUpgrade(command.actors.front(), command.player,
                                                      command.commandName, command.tick);
            logFactoryProductionResult(command, result.accepted, result.message);
            return {.accepted = result.accepted, .producedOrder = false,
                    .rejection = result.accepted
                        ? CommandDispatchRejection::None
                        : CommandDispatchRejection::Rejected,
                    .message = std::move(result.message)};
        }

        auto result = session.confirmedCommandPort().setFactoryRallyPoint(command.actors.front(), command.player,
            {command.targetPosition.x, command.targetPosition.y,
             command.targetPosition.z}, command.tick);
        logFactoryProductionResult(command, result.accepted, result.message);
        return {.accepted = result.accepted, .producedOrder = false,
                .rejection = result.accepted
                    ? CommandDispatchRejection::None
                    : CommandDispatchRejection::Rejected,
                .message = std::move(result.message)};
    }
    case GameCommandType::None:
    case GameCommandType::UIAction:
    case GameCommandType::Move:
    case GameCommandType::AttackMove:
    case GameCommandType::Attack:
    case GameCommandType::Build:
    case GameCommandType::Pause:
    case GameCommandType::Surrender:
    case GameCommandType::Stop:
    case GameCommandType::CombatDrop:
    case GameCommandType::Guard:
    case GameCommandType::GuardWithoutPursuit:
    case GameCommandType::GuardFlyingUnitsOnly:
        break;
    }

    container::String conversionError;
    const std::optional<PlayerOrder> order = OrderExecutor::fromGameCommand(command, &conversionError);
    if (!order) {
        // UIAction/Pause/Surrender intentionally stay out of the object-order
        // path until their dedicated session systems exist. They are reported
        // as unsupported simulation input rather than logged as if execution
        // had happened, which was the former empty implementation's problem.
        TD_LOG_DEBUG("[CommandDispatcher] Rejected tick={} seq={} player={} source={} type={}: {}",
                    command.tick, command.sequence, command.player.value,
                    commandSourceName(command.source), commandTypeName(command.type), conversionError);
        const bool unsupported = command.type == GameCommandType::None ||
            command.type == GameCommandType::UIAction ||
            command.type == GameCommandType::CommandButton ||
            command.type == GameCommandType::Pause ||
            command.type == GameCommandType::Surrender;
        return {.accepted = false, .producedOrder = false,
                .rejection = unsupported
                    ? CommandDispatchRejection::Unsupported
                    : CommandDispatchRejection::MalformedPayload,
                .message = std::move(conversionError)};
    }
    const OrderExecutionResult execution = confirmedCommands.executeOrder(*order);
    if (!execution.accepted) {
        TD_LOG_DEBUG("[CommandDispatcher] Rejected {} order tick={} seq={} player={} actors={}: {}",
                    commandTypeName(command.type), command.tick, command.sequence,
                    command.player.value, order->actors.size(), execution.message);
        return {.accepted = false, .producedOrder = true,
                .rejection = execution.rejection ==
                        OrderRejectionReason::UnsupportedCommand
                    ? CommandDispatchRejection::Unsupported
                    : execution.rejection == OrderRejectionReason::MalformedOrder
                        ? CommandDispatchRejection::MalformedPayload
                        : CommandDispatchRejection::Rejected,
                .message = execution.message};
    }
    TD_LOG_TRACE("[CommandDispatcher] Applied {} order tick={} seq={} player={} actors={} source={}",
                 commandTypeName(command.type), command.tick, command.sequence,
                 command.player.value, execution.actorCount, commandSourceName(command.source));
    return {
        .accepted = true,
        .producedOrder = true,
        .actorCount = execution.actorCount,
    };
}

} // namespace engine
