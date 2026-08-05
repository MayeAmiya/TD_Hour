#include "game/session/command/GameSessionConfirmedCommandPort.h"
#include "game/session/command/GameSessionPlayerCommandPolicy.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionPlayerOrderTransactions.h"
#include "game/session/transaction/GameSessionPlayerStateTransactions.h"
#include "game/session/transaction/GameSessionObjectStateTransactions.h"
#include "game/session/transaction/GameSessionPlayerRepairTransactions.h"
#include "game/session/transaction/GameSessionScriptOrderAdmissionTransactions.h"
#include "game/session/query/InGameCommandQuerySource.h"
#include "game/session/query/InGameCommandProjection.h"
#include "game/session/query/GameSessionCommandQueryPort.h"
#include "game/session/query/GameSessionEconomyQueryPort.h"
#include "game/session/query/GameSessionObjectQueryPort.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/session/command/OrderExecutor.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/economy/ObjectRepairRules.h"
#include "game/selection/LocalSelectionState.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace engine {

bool GameSessionConfirmedCommandPort::cancelOrderWaypoint(
    ObjectId actor, PlayerId player, uint32_t sourceSequence,
    uint64_t confirmedTick) {
    if (!isActive() || confirmedTick != this->confirmedTick() || !actor ||
        sourceSequence == 0 || !player.isMapPlayer() ||
        domainState().worldState().m_objects.isPendingDestroy(actor) ||
        domainState().worldState().m_ownership.ownerOf(actor) != player ||
        objectForbidsPlayerCommands(actor)) {
        return false;
    }
    const std::optional<ecs::entity> entity =
        domainState().worldState().m_objects.entityFromId(actor);
    ObjectOrderQueueComponent* queue = entity
        ? ecs::try_get<ObjectOrderQueueComponent>(
              domainState().worldState().m_registry, *entity)
        : nullptr;
    if (!queue) return false;
    const auto selected = std::find_if(
        queue->orders.begin(), queue->orders.end(),
        [&](const ObjectOrderIntent& order) {
            return order.source == ObjectOrderSource::Player &&
                order.sourceSequence == sourceSequence &&
                (order.kind == ObjectOrderKind::Move ||
                 order.kind == ObjectOrderKind::Attack ||
                 order.kind == ObjectOrderKind::Build ||
                 order.kind == ObjectOrderKind::SpecialPower ||
                 order.kind == ObjectOrderKind::CommandButton ||
                 (order.kind == ObjectOrderKind::TacticalAttack &&
                  order.tacticalAttackSubtype ==
                      ObjectTacticalAttackSubtype::Guard));
        });
    if (selected == queue->orders.end()) return false;

    const size_t selectedIndex = static_cast<size_t>(
        selected - queue->orders.begin());
    const bool selectedPlayerPathPoint = [&] {
        const ObjectSystemPathSequenceComponent* path =
            ecs::try_get<ObjectSystemPathSequenceComponent>(
                domainState().worldState().m_registry, *entity);
        return path && path->source == ObjectOrderSource::Player &&
            path->routeSubtype == ObjectMoveRouteSubtype::FollowPath &&
            selectedIndex < path->queuedOrderCount;
    }();

    // Builder approach is a system-owned implementation detail carrying the
    // same correlation. Removing a queued Build node must not leave that Move
    // at the queue head after the player has cancelled its destination.
    std::erase_if(queue->orders, [&](const ObjectOrderIntent& order) {
        return order.sourceSequence == sourceSequence &&
            ((order.source == ObjectOrderSource::Player &&
              (order.kind == ObjectOrderKind::Move ||
               order.kind == ObjectOrderKind::Attack ||
               order.kind == ObjectOrderKind::Build ||
               order.kind == ObjectOrderKind::SpecialPower ||
               order.kind == ObjectOrderKind::CommandButton ||
               (order.kind == ObjectOrderKind::TacticalAttack &&
                order.tacticalAttackSubtype ==
                    ObjectTacticalAttackSubtype::Guard))) ||
             (order.source == ObjectOrderSource::System &&
              (order.systemPurpose == ObjectOrderSystemPurpose::Builder ||
               order.systemPurpose ==
                   ObjectOrderSystemPurpose::SpecialAbility)));
    });
    ++queue->revision;
    ++queue->externalRevision;
    if (queue->externalRevision == 0) ++queue->externalRevision;
    if (selectedPlayerPathPoint) {
        ObjectSystemPathSequenceComponent* path =
            ecs::try_get<ObjectSystemPathSequenceComponent>(
                domainState().worldState().m_registry, *entity);
        container::Vector<LogicFixedVec3> retained;
        size_t retainedOrders = 0;
        for (ObjectOrderIntent& order : queue->orders) {
            if (order.source != ObjectOrderSource::Player ||
                order.kind != ObjectOrderKind::Move ||
                !order.hasTargetPosition ||
                order.systemPurpose != ObjectOrderSystemPurpose::Generic) {
                break;
            }
            order.moveRouteSubtype = retainedOrders == 0
                ? ObjectMoveRouteSubtype::FollowPath
                : ObjectMoveRouteSubtype::Direct;
            retained.push_back({
                order.targetX, order.targetY, order.targetZ});
            ++retainedOrders;
        }
        if (!path || retained.empty()) {
            ecs::remove<ObjectSystemPathSequenceComponent>(
                domainState().worldState().m_registry, *entity);
        } else {
            if (selectedIndex < path->currentPointIndex &&
                path->currentPointIndex != 0) {
                --path->currentPointIndex;
            }
            path->points = std::move(retained);
            path->queuedOrderCount = static_cast<uint32_t>(retainedOrders);
            path->currentPointIndex = std::min<uint32_t>(
                path->currentPointIndex, path->queuedOrderCount - 1u);
            path->issuedTick = queue->orders.front().issuedTick;
            path->firstSourceSequence =
                queue->orders.front().sourceSequence;
        }
    }
    return true;
}

bool GameSessionConfirmedCommandPort::toggleOvercharge(
    ObjectId object, PlayerId player, uint64_t confirmedTick) {
    if (!isActive() || !object || !player.isMapPlayer() ||
        confirmedTick != this->confirmedTick() ||
        domainState().worldState().m_objects.isPendingDestroy(object) ||
        domainState().worldState().m_ownership.ownerOf(object) != player ||
        objectForbidsPlayerCommands(object)) {
        return false;
    }
    return domainState().worldState().m_objectSimulation.toggleOvercharge(
        domainState().worldState().m_registry,
        domainState().worldState().m_objects, object, confirmedTick);
}

OrderExecutionResult GameSessionConfirmedCommandPort::toggleFormation(
    const GameCommand& command) {
    return GameSessionPlayerOrderTransactions{
        domainState().contentState(), domainState().presentationState(),
        domainState().worldState(), domainState().aiState().m_objectAI,
        domainState().aiState().m_playerOrderCapabilitySnapshot}
        .toggleFormation(command);
}

OrderExecutionResult GameSessionConfirmedCommandPort::scatter(
    const GameCommand& command) {
    if (!isActive() || command.tick != confirmedTick() ||
        !command.player.isMapPlayer()) {
        return {.accepted = false,
                .rejection = OrderRejectionReason::MalformedOrder,
                .message = "scatter command is outside the confirmed authority frame"};
    }
    container::Vector<ObjectId> admitted = command.actors;
    admitted.erase(std::remove_if(
        admitted.begin(), admitted.end(),
        [this](ObjectId actor) { return objectForbidsPlayerCommands(actor); }),
        admitted.end());
    if (admitted.empty()) return {.accepted = true, .actorCount = 0};
    return OrderExecutor::executeScatter(
        domainState().worldState().m_registry,
        domainState().contentState().m_players,
        domainState().worldState().m_objects, command.player, command.tick,
        command.sequence, container::Span<const ObjectId>{admitted});
}

bool GameSessionConfirmedCommandPort::purchaseScience(
    const GameCommand& command) {
    if (!isActive() || command.tick != confirmedTick() ||
        !command.player.isMapPlayer()) {
        return false;
    }
    const ScienceCatalog* catalog =
        domainState().contentState().m_contentSnapshot.scienceCatalog();
    const ScienceDefinition* science = catalog
        ? catalog->find(command.commandName) : nullptr;
    return science && GameSessionPlayerStateTransactions{
        domainState().contentState().m_players}
        .purchaseScience(command.player, *science);
}
namespace {

[[nodiscard]] const game::CommandButtonTemplate*
findConfirmedActivationButton(
    const session_query::InGameCommandQuerySource& source,
    const GameCommand& command, ObjectId actor,
    bool requireSingleUse = false) {
    const CommandActivationContext& activation = command.activation;
    // CP_UNDER_CONSTRUCTION is a synthetic ControlBar context; the unfinished
    // object's authored finished CommandSet intentionally does not contain
    // Command_CancelConstruction. Revalidate that one context against the
    // live construction state and the canonical authored button instead of
    // searching the finished CommandSet and rejecting every Stop click as a
    // descriptor change.
    if (!requireSingleUse &&
        command.type == GameCommandType::CancelConstruction &&
        activation.commandKind ==
            game::CommandButtonKind::DozerConstructCancel &&
        source.objectConstruction(actor).underConstruction) {
        const game::CommandButtonTemplate* cancel =
            source.findCommandButton("Command_CancelConstruction");
        if (cancel && cancel->descriptor.stableId ==
                activation.buttonStableId &&
            cancel->descriptor.kind == activation.commandKind) {
            return cancel;
        }
    }
    for (size_t slot = 0; slot < game::COMMAND_SET_SLOT_COUNT; ++slot) {
        const container::StringView name =
            source.effectiveObjectCommandBarButton(
                actor, slot);
        if (name.empty()) continue;
        const game::CommandButtonTemplate* button =
            source.findCommandButton(name);
        if (!button) continue;
        const game::CommandButtonDescriptor& descriptor = button->descriptor;
        if (descriptor.stableId == activation.buttonStableId &&
            descriptor.kind == activation.commandKind &&
            (!requireSingleUse || game::hasCommandButtonOption(
                 descriptor.options,
                 game::CommandButtonOption::SingleUseCommand))) {
            return button;
        }
    }
    return nullptr;
}

constexpr auto equalsAsciiIgnoreCase = container::asciiEqualIgnoreCase;

[[nodiscard]] bool hasNoCommandPositionPayload(
    const CommandPosition& position) noexcept {
    return !position.valid && position.x.raw() == 0 &&
        position.y.raw() == 0 && position.z.raw() == 0;
}

[[nodiscard]] const container::String* commandButtonField(
    const game::CommandButtonTemplate& button,
    container::StringView key) noexcept {
    for (auto it = button.fields.rbegin(); it != button.fields.rend(); ++it) {
        if (equalsAsciiIgnoreCase(it->first, key)) return &it->second;
    }
    return nullptr;
}

[[nodiscard]] bool commandButtonHasOption(
    const game::CommandButtonTemplate& button,
    container::StringView option) noexcept {
    size_t cursor = 0;
    while (cursor < button.options.size()) {
        while (cursor < button.options.size() &&
               (std::isspace(static_cast<unsigned char>(button.options[cursor])) ||
                button.options[cursor] == ',' || button.options[cursor] == '|')) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < button.options.size() &&
               !std::isspace(static_cast<unsigned char>(button.options[cursor])) &&
               button.options[cursor] != ',' && button.options[cursor] != '|') {
            ++cursor;
        }
        if (begin != cursor && equalsAsciiIgnoreCase(
                container::StringView{button.options}.substr(begin, cursor - begin),
                option)) return true;
    }
    return false;
}

[[nodiscard]] std::optional<uint32_t> commandButtonUnsignedField(
    const game::CommandButtonTemplate& button,
    container::StringView key) noexcept {
    const container::String* value = commandButtonField(button, key);
    if (!value || value->empty()) return std::nullopt;
    uint32_t parsed = 0;
    const char* begin = value->data();
    const char* end = begin + value->size();
    const auto [next, error] = std::from_chars(begin, end, parsed);
    return error == std::errc{} && next == end
        ? std::optional<uint32_t>{parsed}
        : std::nullopt;
}

// Most CommandButton activations are object-scoped and are revalidated below
// against the current command bar.  PurchaseScience is deliberately
// player-scoped: its wire contract and dispatcher both require an empty actor
// set, so it must be checked against the authored science button instead.
[[nodiscard]] ConfirmedCommandActivationValidation
validConfirmedScienceActivation(
    const GameContentSnapshot& content, const PlayerList& players,
    const GameCommand& command) {
    const CommandActivationContext& activation = command.activation;
    if (!activation.present() || activation.hasPostAcceptAction() ||
        !command.actors.empty() || !command.player.isMapPlayer() ||
        activation.buttonStableId == 0 ||
        activation.commandKind != game::CommandButtonKind::PurchaseScience ||
        command.commandName.empty()) {
        return {.rejection =
                    ConfirmedCommandActivationRejection::MalformedContext};
    }

    const game::CommandButtonTemplate* button =
        content.findCommandButtonByStableId(activation.buttonStableId);
    if (!button ||
        button->descriptor.stableId != activation.buttonStableId ||
        button->descriptor.kind != game::CommandButtonKind::PurchaseScience ||
        !button->descriptor.userActivatable()) {
        return {.rejection =
                    ConfirmedCommandActivationRejection::DescriptorChanged};
    }

    const bool namesAuthoredScience =
        (!button->sciences.empty() &&
         std::find(button->sciences.begin(), button->sciences.end(),
                   command.commandName) != button->sciences.end()) ||
        (button->sciences.empty() && button->science == command.commandName);
    if (!namesAuthoredScience) {
        return {.rejection =
                    ConfirmedCommandActivationRejection::DescriptorChanged};
    }

    const ScienceCatalog* sciences =
        content.scienceCatalog();
    const ScienceDefinition* science = sciences
        ? sciences->find(command.commandName) : nullptr;
    if (!science || !science->grantable) {
        return {.rejection =
                    ConfirmedCommandActivationRejection::ScienceUnavailable};
    }
    return players.canPurchaseScience(command.player, *science)
        ? ConfirmedCommandActivationValidation{}
        : ConfirmedCommandActivationValidation{
              .rejection =
                  ConfirmedCommandActivationRejection::AvailabilityChanged};
}

} // namespace

bool GameSessionConfirmedCommandPort::objectForbidsPlayerCommands(
    ObjectId object) const noexcept {
    return session_command_policy::objectForbidsPlayerCommands(
        domainState().worldState().m_registry,
        domainState().worldState().m_objects, object);
}

ConfirmedCommandActivationValidation
GameSessionConfirmedCommandPort::validateActivation(
    const GameCommand& command) const {
    const CommandActivationContext& activation = command.activation;
    // Queue-row and selected-waypoint cancellation are direct UI
    // transactions.  They carry request correlation for the final confirmed
    // receipt but do not name an authored CommandButton to revalidate.
    const bool directReceiptOnly = activation.present() &&
        activation.postAccept == CommandPostAcceptAction::None &&
        !activation.postAcceptActor &&
        ((command.type == GameCommandType::CancelProduction &&
          activation.buttonStableId == 0 &&
          activation.commandKind == game::CommandButtonKind::CancelUnitBuild) ||
         (command.type == GameCommandType::CancelPlayerUpgrade &&
          activation.buttonStableId == 0 &&
          activation.commandKind == game::CommandButtonKind::CancelUpgrade) ||
         (command.type == GameCommandType::CancelOrderWaypoint &&
          activation.buttonStableId != 0 &&
          activation.commandKind != game::CommandButtonKind::Unknown));
    if (directReceiptOnly) return {};
    const GameSessionStateRoot& state = domainState();
    if (command.type == GameCommandType::PurchaseScience) {
        return validConfirmedScienceActivation(
            state.contentState().m_contentSnapshot,
            state.contentState().m_players, command);
    }
    if (!activation.present()) {
        return activation.hasPostAcceptAction()
            ? ConfirmedCommandActivationValidation{
                  .rejection =
                      ConfirmedCommandActivationRejection::MalformedContext}
            : ConfirmedCommandActivationValidation{};
    }
    if (activation.buttonStableId == 0 ||
        activation.commandKind == game::CommandButtonKind::Unknown ||
        !command.player.isMapPlayer() || command.actors.empty()) {
        return {.rejection =
                    ConfirmedCommandActivationRejection::MalformedContext};
    }
    const session_query::InGameCommandQuerySource source{
        state.worldState().m_registry,
        state.contentState().m_players,
        state.worldState().m_ownership,
        state.worldState().m_objects,
        state.contentState().m_contentSnapshot,
        state.presentationState().m_scriptCommandBarOverrides,
        state.presentationState().m_scriptObjectBuildabilityOverrides};
    const GameSessionCommandQueryPort commands{
        state.contentState(), state.worldState(), state.aiState()};
    const GameSessionEconomyQueryPort economy{
        state.worldState().m_registry, state.worldState().m_objects};
    selection::LocalSelectionState selection;
    static_cast<void>(selection.replace(command.actors));

    const game::CommandButtonTemplate* admittedButton = nullptr;
    ObjectId admittedActor = INVALID_OBJECT_ID;
    bool sawLiveOwnedActor = false;
    bool sawActivationButton = false;
    ConfirmedCommandActivationRejection rejection =
        ConfirmedCommandActivationRejection::AvailabilityChanged;
    for (const ObjectId actor : command.actors) {
        if (!actor || objectQuery().pendingDestroy(actor) ||
            state.worldState().m_ownership.ownerOf(actor) !=
                std::optional<PlayerId>{command.player}) {
            continue;
        }
        sawLiveOwnedActor = true;
        const game::CommandButtonTemplate* button =
            findConfirmedActivationButton(source, command, actor);
        if (!button) continue;
        sawActivationButton = true;
        const session_query::InGameCommandSlotAvailability availability =
            session_query::evaluateInGameCommandAvailability(
                {
                    .source = source,
                    .commands = commands,
                    .economy = economy,
                    .confirmedTick = confirmedTick(),
                    .logicFramesPerSecond = static_cast<uint32_t>(
                        std::max(1, state.contentState()
                                        .m_startInfo.gameSpeedFPS)),
                    .player = command.player,
                },
                selection, actor, *button, true, command.targetObject);
        const bool deferredQueuedBuildGate =
            command.type == GameCommandType::Build && command.queued &&
            (availability.reason ==
                 session_query::InGameCommandAvailabilityReason::QueueBusy ||
             availability.reason ==
                 session_query::InGameCommandAvailabilityReason::
                     InsufficientFunds);
        if (!availability.visible ||
            (!availability.enabled && !deferredQueuedBuildGate) ||
            (availability.reason !=
                 session_query::InGameCommandAvailabilityReason::None &&
              !deferredQueuedBuildGate)) {
            if (availability.reason ==
                session_query::InGameCommandAvailabilityReason::
                    SingleUseConsumed) {
                rejection =
                    ConfirmedCommandActivationRejection::SingleUseConsumed;
            }
            continue;
        }
        admittedButton = button;
        admittedActor = actor;
        break;
    }
    if (!admittedButton) {
        if (!sawLiveOwnedActor) {
            return {.rejection =
                        ConfirmedCommandActivationRejection::ActorUnavailable};
        }
        if (!sawActivationButton) {
            return {.rejection =
                        ConfirmedCommandActivationRejection::DescriptorChanged};
        }
        return {.rejection = rejection};
    }

    if (!activation.hasPostAcceptAction()) return {};
    if (activation.postAccept !=
            CommandPostAcceptAction::MarkSingleUseCommandUsed ||
        !activation.postAcceptActor ||
        !std::binary_search(command.actors.begin(), command.actors.end(),
                            activation.postAcceptActor) ||
        admittedActor != activation.postAcceptActor ||
        !game::hasCommandButtonOption(
            admittedButton->descriptor.options,
            game::CommandButtonOption::SingleUseCommand)) {
        return {.rejection =
                    ConfirmedCommandActivationRejection::DescriptorChanged};
    }
    const std::optional<ecs::entity> entity =
        objectQuery().entity(activation.postAcceptActor);
    if (!entity) {
        return {.rejection =
                    ConfirmedCommandActivationRejection::ActorUnavailable};
    }
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(
            domainState().worldState().m_registry, *entity);
    if (!status) {
        return {.rejection =
                    ConfirmedCommandActivationRejection::ActorUnavailable};
    }
    return status->singleUseCommandUsed
        ? ConfirmedCommandActivationValidation{
              .rejection =
                  ConfirmedCommandActivationRejection::SingleUseConsumed}
        : ConfirmedCommandActivationValidation{};
}

bool GameSessionConfirmedCommandPort::applyPostAccept(
    const GameCommand& command) {
    const CommandActivationContext& activation = command.activation;
    if (!activation.hasPostAcceptAction()) return true;
    if (activation.postAccept !=
        CommandPostAcceptAction::MarkSingleUseCommandUsed) {
        return false;
    }
    if (objectForbidsPlayerCommands(activation.postAcceptActor)) {
        return true;
    }
    return GameSessionObjectStateTransactions{
        domainState().worldState().m_registry,
        domainState().worldState().m_objects}
        .markSingleUseCommandUsed(
            activation.postAcceptActor, confirmedTick());
}

OrderExecutionResult GameSessionConfirmedCommandPort::executeOrder(
    const PlayerOrder& order) {
    return GameSessionPlayerOrderTransactions{
        domainState().contentState(),
        domainState().presentationState(),
        domainState().worldState(), domainState().aiState().m_objectAI,
        domainState().aiState().m_playerOrderCapabilitySnapshot}
        .execute(order);
}

OrderExecutionResult
GameSessionConfirmedCommandPort::executeSpecialPowerConstruct(
    const GameCommand& command) {
    const auto reject = [](OrderRejectionReason reason,
                           container::String message) {
        return OrderExecutionResult{
            .accepted = false,
            .rejection = reason,
            .message = std::move(message),
        };
    };
    const game::CommandButtonKind kind = command.activation.commandKind;
    if (!domainState().contentState().m_active ||
        !domainState().presentationState().m_hasConfirmedFrame ||
        command.tick != domainState().presentationState().m_confirmedTick ||
        command.type != GameCommandType::SpecialPower ||
        !command.player.isMapPlayer() || command.actors.size() != 1u ||
        !command.actors.front() || !command.activation.present() ||
        command.activation.buttonStableId == 0 ||
        (kind != game::CommandButtonKind::SpecialPowerConstruct &&
         kind != game::CommandButtonKind::
             SpecialPowerConstructFromShortcut) ||
        command.commandName.empty() || !command.targetPosition.valid ||
        command.targetObject || command.productionId != 0 || command.queued ||
        !hasNoCommandPositionPayload(command.placementEndPosition)) {
        return reject(OrderRejectionReason::MalformedOrder,
                      "malformed SpecialPower construct command");
    }

    const game::CommandButtonTemplate* button =
        domainState().contentState().m_contentSnapshot.findCommandButton(
            command.commandName);
    if (!button || !button->descriptor.userActivatable() ||
        button->descriptor.stableId != command.activation.buttonStableId ||
        button->descriptor.kind != kind || button->object.empty() ||
        button->specialPower.empty()) {
        return reject(OrderRejectionReason::UnsupportedCommand,
                      "SpecialPower construct descriptor is unavailable");
    }

    const ObjectId source = command.actors.front();
    if (objectForbidsPlayerCommands(source)) {
        return {.accepted = true, .actorCount = 0};
    }
    const std::optional<ecs::entity> sourceEntity =
        domainState().worldState().m_objects.entityFromId(source);
    if (!sourceEntity ||
        domainState().worldState().m_objects.isPendingDestroy(source) ||
        domainState().worldState().m_ownership.ownerOf(source) !=
            std::optional<PlayerId>{command.player}) {
        return reject(OrderRejectionReason::OwnershipMismatch,
                      "SpecialPower construct source is unauthorized");
    }
    const ObjectProductionComponent* production =
        ecs::try_get<ObjectProductionComponent>(
            domainState().worldState().m_registry, *sourceEntity);
    const ObjectSpecialPowerComponent* specialPowers =
        ecs::try_get<ObjectSpecialPowerComponent>(
            domainState().worldState().m_registry, *sourceEntity);
    if (!production || !production->plan || !specialPowers ||
        !specialPowers->plan) {
        return reject(OrderRejectionReason::UnsupportedCommand,
                      "SpecialPower construct source has no production/special-power owner");
    }

    const container::SharedPtr<const game::ObjectArchetype> product =
        domainState().contentState().m_contentSnapshot.findObjectArchetype(
            button->object);
    const PlayerState* player =
        domainState().contentState().m_players.get(command.player);
    if (!product || !player || !player->isCommandPlayer()) {
        return reject(OrderRejectionReason::InvalidPlayer,
                      "SpecialPower construct product or player is unavailable");
    }
    const bool structure = game::objectHasKind(
        product->kindOfMask, game::ObjectKindOf::Structure);
    bool ignorePrerequisites = false;
    if ((structure && !player->constructionPolicy.baseConstructionEnabled) ||
        (!structure &&
         !player->constructionPolicy.unitConstructionEnabled) ||
        !productionTransactions().admitsBuildability(
            command.player, *product, ignorePrerequisites) ||
        (!ignorePrerequisites &&
         (!playerSatisfiesObjectProductionPrerequisites(
              domainState().worldState().m_registry,
              domainState().contentState().m_players,
              domainState().contentState().m_contentSnapshot,
              command.player, *product) ||
          !playerCanBuildMoreOfObjectType(
              domainState().worldState().m_registry,
              command.player, *product)))) {
        return reject(OrderRejectionReason::UnsupportedCommand,
                      "SpecialPower construct product is no longer buildable");
    }

    const GameSessionBuildPlacementLegalityEvaluation placement =
        evaluateBuildPlacementFixed(
            source,
            {command.targetPosition.x, command.targetPosition.y,
             command.targetPosition.z},
            command.placementYawRadians, command.player, *product, true);
    if (!placement.evaluated || placement.legality !=
            selection::LocalPlacementLegality::Legal) {
        return reject(OrderRejectionReason::InvalidTarget,
                      "SpecialPower construct placement is illegal");
    }

    container::String conversionError;
    const std::optional<PlayerOrder> order =
        OrderExecutor::fromGameCommand(command, &conversionError);
    if (!order) {
        return reject(OrderRejectionReason::MalformedOrder,
                      std::move(conversionError));
    }
    return executeOrder(*order);
}

OrderExecutionResult GameSessionConfirmedCommandPort::executeCommandButton(
    const GameCommand& command) {
    const auto reject = [](OrderRejectionReason reason,
                           container::String message) {
        return OrderExecutionResult{
            .accepted = false,
            .rejection = reason,
            .message = std::move(message),
        };
    };
    if (!domainState().contentState().m_active ||
        !domainState().presentationState().m_hasConfirmedFrame ||
        command.tick != domainState().presentationState().m_confirmedTick ||
        command.type != GameCommandType::CommandButton ||
        !command.player.isMapPlayer() || command.actors.empty() ||
        command.commandName.empty() || command.productionId != 0 ||
        (command.targetObject && command.targetPosition.valid) ||
        (!command.targetPosition.valid &&
         !hasNoCommandPositionPayload(command.targetPosition)) ||
        command.placementYawRadians.raw() != 0 ||
        !hasNoCommandPositionPayload(command.placementEndPosition)) {
        return reject(OrderRejectionReason::MalformedOrder,
                      "malformed player CommandButton");
    }
    container::Vector<ObjectId> admittedActors{
        command.actors.begin(), command.actors.end()};
    admittedActors.erase(std::remove_if(
        admittedActors.begin(), admittedActors.end(),
        [this](ObjectId actor) {
            return objectForbidsPlayerCommands(actor);
        }), admittedActors.end());
    if (admittedActors.empty()) {
        return {.accepted = true, .actorCount = 0};
    }
    const game::CommandButtonTemplate* button =
        domainState().contentState().m_contentSnapshot.findCommandButton(
            command.commandName);
    const game::CommandButtonKind kind = button
        ? button->descriptor.kind : game::CommandButtonKind::Unknown;
    const bool weapon = kind == game::CommandButtonKind::FireWeapon ||
        kind == game::CommandButtonKind::SwitchWeapon;
    const bool intentionalContact =
        kind == game::CommandButtonKind::HijackVehicle ||
        kind == game::CommandButtonKind::ConvertToCarBomb ||
        kind == game::CommandButtonKind::SabotageBuilding;
    // HACK_INTERNET already has a completed owner behind
    // executeScriptCommandButton: it filters actors on the authored
    // ObjectEconomy hackInternet plan and hands the CommandButton order to
    // the AIStateId::HackInternet machine. Unlike COMBATDROP it carries no
    // dedicated GameCommandType, so this admission is its only route.
    const bool hackInternet = kind == game::CommandButtonKind::HackInternet;
    if (!button || (!weapon && !intentionalContact && !hackInternet) ||
        !button->descriptor.userActivatable() ||
        command.activation.buttonStableId != button->descriptor.stableId ||
        command.activation.commandKind != kind) {
        return reject(OrderRejectionReason::UnsupportedCommand,
                      "player CommandButton has no completed owner");
    }
    for (const ObjectId actor : admittedActors) {
        if (!actor || domainState().worldState().m_objects.isPendingDestroy(actor) ||
            domainState().worldState().m_ownership.ownerOf(actor) !=
                std::optional<PlayerId>{command.player}) {
            return reject(OrderRejectionReason::OwnershipMismatch,
                          "player CommandButton contains an unauthorized actor");
        }
    }

    if (intentionalContact) {
        const ObjectIntentionalContactKind contactKind =
            kind == game::CommandButtonKind::HijackVehicle
                ? ObjectIntentionalContactKind::HijackVehicle
                : kind == game::CommandButtonKind::ConvertToCarBomb
                    ? ObjectIntentionalContactKind::ConvertToCarBomb
                    : ObjectIntentionalContactKind::SabotageBuilding;
        return GameSessionPlayerOrderTransactions{
            domainState().contentState(),
            domainState().presentationState(),
            domainState().worldState(), domainState().aiState().m_objectAI,
            domainState().aiState().m_playerOrderCapabilitySnapshot}
            .executeIntentionalContact(
                command, admittedActors, contactKind);
    }

    ScriptOrderIntent intent;
    intent.contextPlayer = command.player;
    intent.confirmedTick = command.tick;
    intent.sourceEffectOrdinal = command.sequence;
    intent.kind = ObjectOrderKind::CommandButton;
    intent.actors = std::move(admittedActors);
    intent.targetObject = command.targetObject;
    intent.targetPosition = command.targetPosition;
    intent.contentName = command.commandName;
    intent.queued = command.queued;
    return m_orderAdmission
        .executeScriptCommandButton(intent, true);
}

bool GameSessionCommandQueryPort::canRepairSelectionTarget(
    PlayerId player, container::Span<const ObjectId> actors,
    ObjectId structure) const {
    return repairSelectionTargetAction(player, actors, structure) !=
        PlayerRepairTargetAction::None;
}

PlayerRepairTargetAction
GameSessionCommandQueryPort::repairSelectionTargetAction(
    PlayerId player, container::Span<const ObjectId> actors,
    ObjectId structure) const {
    const PlayerState* issuingPlayer = m_content->m_players.get(player);
    if (!m_content->m_active || !issuingPlayer ||
        !issuingPlayer->isCommandPlayer() || actors.empty() || !structure ||
        m_world->m_objects.isPendingDestroy(structure)) {
        return PlayerRepairTargetAction::None;
    }
    const std::optional<ecs::entity> targetEntity =
        m_world->m_objects.entityFromId(structure);
    if (!targetEntity) return PlayerRepairTargetAction::None;
    if (const auto visibility = m_world->m_mapVisibility.snapshot();
        visibility && visibility->renderingActive) {
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(m_world->m_registry, *targetEntity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(m_world->m_registry, *targetEntity);
        const LogicFixedVec3 position = transform
            ? readAuthoritativeObjectPosition(
                  m_world->m_registry, *targetEntity,
                  *transform)
            : LogicFixedVec3{};
        const math::q32_32 radius = geometry
            ? math::q32_32::max(
                  math::q32_32{}, geometry->boundingCircleRadiusFixed)
            : math::q32_32{};
        bool visible = transform && visibility->footprintHasClearCellRaw(
            player, position.x.raw(), position.y.raw(), radius.raw());
        for (const PlayerId ally : m_content->m_players.activePlayerIds()) {
            if (visible) break;
            if (ally != player &&
                m_content->m_players.relationship(player, ally) ==
                    PlayerRelationship::Allies &&
                transform && visibility->footprintHasClearCellRaw(
                    ally, position.x.raw(), position.y.raw(), radius.raw())) {
                visible = true;
            }
        }
        if (!visible) {
            return PlayerRepairTargetAction::None;
        }
    }
    const OwnerComponent* targetOwner =
        ecs::try_get<OwnerComponent>(
            m_world->m_registry, *targetEntity);
    const bool repairDock = object_repair_rules::isRepairDockTarget(
        m_world->m_registry, *targetEntity) &&
        targetOwner && targetOwner->player &&
        m_content->m_players.relationship(
            player, targetOwner->player) == PlayerRelationship::Allies;
    const bool aircraftAirfield =
        object_repair_rules::isAircraftRepairAirfieldTarget(
            m_world->m_registry, *targetEntity) &&
        targetOwner && targetOwner->player &&
        m_content->m_players.relationship(
            player, targetOwner->player) == PlayerRelationship::Allies;
    for (const ObjectId actor : actors) {
        if (objectForbidsPlayerCommands(actor)) continue;
        if (m_world->m_ownership.ownerOf(actor) != player)
            continue;
        if (m_world->m_objectSimulation.canObjectResumeConstruction(
                m_world->m_registry, m_world->m_objects,
                m_content->m_players, actor, structure)) {
            return PlayerRepairTargetAction::ResumeConstruction;
        }
        if (m_world->m_objectSimulation.canObjectRepair(
                m_world->m_registry, m_world->m_objects,
                m_content->m_players, actor, structure)) {
            return PlayerRepairTargetAction::DoRepair;
        }
        const std::optional<ecs::entity> actorEntity =
            m_world->m_objects.entityFromId(actor);
        if (repairDock && actorEntity &&
            object_repair_rules::isDamagedRepairDockActor(
                m_world->m_registry, *actorEntity) &&
            m_ai->m_objectAI.hasOrderCapability(
                actor, ai::ObjectAIOrderCapability::MoveStop))
            return PlayerRepairTargetAction::GetRepaired;
        if (aircraftAirfield && actorEntity &&
            object_repair_rules::isDamagedAircraftRepairActor(
                m_world->m_registry, *actorEntity)) {
            return PlayerRepairTargetAction::GetRepaired;
        }
    }
    return PlayerRepairTargetAction::None;
}

OrderExecutionResult GameSessionConfirmedCommandPort::repair(
    PlayerId player, container::Span<const ObjectId> actors,
    ObjectId structure, uint32_t sourceSequence, uint64_t confirmedTick) {
    return GameSessionPlayerRepairTransactions{
        domainState().contentState(), domainState().worldState(),
        domainState().aiState(), domainState().presentationState()}
        .execute(
            player, actors, structure, sourceSequence, confirmedTick);
}

} // namespace engine
