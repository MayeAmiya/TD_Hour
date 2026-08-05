#include "game/session/query/UnifiedCommandRouter.h"

#include "core/container/string_utils.h"
#include "game/player/PlayerRegistry.h"
#include "game/render/LocalPlacementPresentationState.h"
#include "game/selection/LocalSelectionState.h"
#include "game/session/query/LocalSelectionQueryPort.h"
#include "game/session/query/WorldCommandComposer.h"
#include "game/session/core/GameSession.h"
#include "game/session/query/InGameCommandQuerySource.h"
#include "game/session/query/GameSessionObjectQueryPort.h"

#include <algorithm>
#include <utility>

namespace engine::session_query {
namespace {

using game::CommandButtonKind;
using game::CommandButtonOption;

[[nodiscard]] UnifiedCommandRouteResult rejected(
    CommandButtonKind kind, UnifiedCommandRouteRejection reason) {
    return {
        .kind = UnifiedCommandRouteKind::Rejected,
        .rejection = reason,
        .commandKind = kind,
    };
}

[[nodiscard]] UnifiedCommandRouteResult unsupported(
    CommandButtonKind kind, UnifiedCommandBackendGap gap) {
    return {
        .kind = UnifiedCommandRouteKind::UnsupportedBackend,
        .backendGap = gap,
        .commandKind = kind,
    };
}

[[nodiscard]] UnifiedCommandPostAcceptHook postAcceptHook(
    const game::CommandButtonDescriptor& descriptor,
    engine::ObjectId actor) noexcept {
    if (!game::hasCommandButtonOption(
            descriptor.options, CommandButtonOption::SingleUseCommand)) {
        return {};
    }
    return {
        .kind = UnifiedCommandPostAcceptHookKind::MarkSingleUseCommandUsed,
        .actor = actor,
        .commandButtonStableId = descriptor.stableId,
    };
}

[[nodiscard]] engine::GameCommand actorCommand(
    engine::GameTick tick, engine::PlayerId player, engine::ObjectId actor,
    engine::GameCommandType type, container::StringView content = {}) {
    engine::GameCommand command;
    command.tick = tick;
    command.player = player;
    command.source = engine::CommandSource::Local;
    command.type = type;
    command.actors.push_back(actor);
    command.commandName.assign(content);
    return command;
}

[[nodiscard]] UnifiedCommandRouteResult submit(
    CommandButtonKind kind, engine::GameCommand command,
    UnifiedCommandPostAcceptHook hook) {
    UnifiedCommandRouteResult result{
        .kind = UnifiedCommandRouteKind::SubmitGameCommand,
        .commandKind = kind,
        .postAccept = hook,
    };
    result.command = std::move(command);
    return result;
}

[[nodiscard]] UnifiedCommandRouteResult composeOrder(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    engine::PlayerId localPlayer, engine::GameTick tick,
    CommandButtonKind kind, engine::GameCommandType type,
    container::StringView content,
    UnifiedCommandPostAcceptHook hook) {
    engine::selection::WorldCommandRequest request;
    request.tick = tick;
    request.type = type;
    request.commandName.assign(content);
    engine::selection::WorldCommandComposeResult composed =
        engine::selection::WorldCommandComposer::compose(
            session, selection, localPlayer, std::move(request));
    if (!composed) {
        return rejected(
            kind, UnifiedCommandRouteRejection::CommandCompositionRejected);
    }
    UnifiedCommandRouteResult result = submit(
        kind, std::move(*composed.command), hook);
    result.localVoiceEvent = std::move(composed.voiceEventName);
    result.localVoiceObject = composed.voiceObject;
    return result;
}

} // namespace

UnifiedCommandRouteResult UnifiedCommandRouter::route(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    engine::ObjectId actor,
    const game::CommandButtonDescriptor& descriptor,
    const game::CommandButtonTemplate& button,
    const InGameCommandSlotAvailability& availability,
    engine::GameTick tick,
    engine::ObjectId commandTarget) {
    const CommandButtonKind kind = descriptor.kind;
    if (!descriptor.userActivatable() || descriptor != button.descriptor) {
        return rejected(
            kind, UnifiedCommandRouteRejection::InvalidDescriptor);
    }
    if (!availability.visible || !availability.enabled ||
        availability.reason != InGameCommandAvailabilityReason::None) {
        return rejected(kind, UnifiedCommandRouteRejection::Unavailable);
    }
    if (!actor || std::find(selection.selected().begin(),
                            selection.selected().end(), actor) ==
            selection.selected().end()) {
        return rejected(
            kind, UnifiedCommandRouteRejection::InvalidSelection);
    }
    const engine::PlayerState* localPlayer =
        inGameCommandQuerySource(session).localPlayer();
    if (!localPlayer || !localPlayer->isCommandPlayer()) {
        return rejected(
            kind, UnifiedCommandRouteRejection::InvalidLocalPlayer);
    }
    if (inGameCommandQuerySource(session).ownerOf(actor) != localPlayer->id ||
        !session.objectQuery().entity(actor) ||
        session.objectQuery().pendingDestroy(actor)) {
        return rejected(
            kind, UnifiedCommandRouteRejection::UnauthorizedActor);
    }

    const UnifiedCommandPostAcceptHook hook =
        postAcceptHook(descriptor, actor);
    const auto beginPending = [&]() -> UnifiedCommandRouteResult {
        std::optional<engine::selection::PendingWorldCommandMode> pending =
            engine::selection::resolvePendingWorldCommandMode(button, actor);
        if (!pending) {
            return rejected(
                kind, UnifiedCommandRouteRejection::MissingRoutePayload);
        }
        pending->cursor.radiusWorld =
            inGameCommandQuerySource(session).pendingCommandRadius(
                actor, button);
        if (kind == CommandButtonKind::SpecialPower ||
            kind == CommandButtonKind::SpecialPowerFromShortcut) {
            // RefCode CommandXlat reads SpecialPowerModule::InitiateSound,
            // not CommandButton.UnitSpecificSound, for every special-power
            // acknowledgement.  An empty/NoSound value is deliberate and
            // must suppress the normal Move/Attack fallback later.
            pending->voice.unitSpecificSound =
                inGameCommandQuerySource(session).specialPowerInitiateSound(
                    actor, button);
        } else if (kind == CommandButtonKind::CombatDrop ||
                   kind == CommandButtonKind::FireWeapon) {
            // CommandXlat answers this message with the selected object's
            // per-unit acknowledgement (VoiceCombatDrop or the typed weapon
            // DamageType ladder), never CommandButton.UnitSpecificSound.
            pending->voice.unitSpecificSound.clear();
        }
        UnifiedCommandRouteResult result{
            .kind = UnifiedCommandRouteKind::BeginPendingWorldTarget,
            .commandKind = kind,
            .postAccept = hook,
        };
        result.pendingTarget = std::move(*pending);
        return result;
    };

    // This switch is intentionally exhaustive. Adding a CommandButtonKind
    // must choose a real backend, a typed gap, or a typed rejection here.
    switch (kind) {
    case CommandButtonKind::Unknown:
    case CommandButtonKind::None:
        return rejected(
            kind, UnifiedCommandRouteRejection::InvalidDescriptor);

    case CommandButtonKind::DozerConstruct:
        if (button.object.empty()) {
            return rejected(
                kind, UnifiedCommandRouteRejection::MissingRoutePayload);
        }
        return {
            .kind = UnifiedCommandRouteKind::BeginPlacement,
            .commandKind = kind,
            .placementProduct = button.object,
            .postAccept = hook,
        };
    case CommandButtonKind::DozerConstructCancel:
        return submit(
            kind, actorCommand(tick, localPlayer->id, actor,
                               engine::GameCommandType::CancelConstruction),
            hook);

    case CommandButtonKind::UnitBuild:
        if (button.object.empty()) {
            return rejected(
                kind, UnifiedCommandRouteRejection::MissingRoutePayload);
        }
        return submit(
            kind, actorCommand(tick, localPlayer->id, actor,
                               engine::GameCommandType::QueueProduction,
                               button.object),
            hook);
    case CommandButtonKind::CancelUnitBuild: {
        if (availability.queue.headProductionId == 0) {
            return rejected(
                kind, UnifiedCommandRouteRejection::MissingRoutePayload);
        }
        engine::GameCommand command = actorCommand(
            tick, localPlayer->id, actor,
            engine::GameCommandType::CancelProduction);
        command.productionId = availability.queue.headProductionId;
        return submit(kind, std::move(command), hook);
    }

    case CommandButtonKind::PlayerUpgrade:
    case CommandButtonKind::ObjectUpgrade:
        if (button.upgrade.empty()) {
            return rejected(
                kind, UnifiedCommandRouteRejection::MissingRoutePayload);
        }
        return submit(
            kind, actorCommand(tick, localPlayer->id, actor,
                               engine::GameCommandType::QueuePlayerUpgrade,
                               button.upgrade),
            hook);
    case CommandButtonKind::CancelUpgrade:
        if (button.upgrade.empty()) {
            return rejected(
                kind, UnifiedCommandRouteRejection::MissingRoutePayload);
        }
        return submit(
            kind, actorCommand(tick, localPlayer->id, actor,
                               engine::GameCommandType::CancelPlayerUpgrade,
                               button.upgrade),
            hook);

    case CommandButtonKind::AttackMove:
    case CommandButtonKind::SetRallyPoint:
    case CommandButtonKind::CombatDrop:
        return beginPending();

    case CommandButtonKind::SpecialPower:
        if (descriptor.targetKind != game::CommandButtonTargetKind::None) {
            return beginPending();
        }
        {
            UnifiedCommandRouteResult result = composeOrder(
            session, selection, localPlayer->id, tick, kind,
            engine::GameCommandType::SpecialPower, button.name, hook);
            if (result.kind == UnifiedCommandRouteKind::SubmitGameCommand) {
                result.localVoiceEvent =
                    inGameCommandQuerySource(session).
                        specialPowerInitiateSound(actor, button);
                result.localVoiceObject = actor;
            }
            return result;
        }
    case CommandButtonKind::SpecialPowerFromShortcut:
        if (descriptor.targetKind != game::CommandButtonTargetKind::None) {
            UnifiedCommandRouteResult result = beginPending();
            if (result.pendingTarget) {
                result.pendingTarget->sourceMayBeUnselected = true;
            }
            return result;
        }
        {
            UnifiedCommandRouteResult result = submit(
            kind,
            actorCommand(tick, localPlayer->id, actor,
                         engine::GameCommandType::SpecialPower, button.name),
            hook);
            if (result.kind == UnifiedCommandRouteKind::SubmitGameCommand) {
                result.localVoiceEvent =
                    inGameCommandQuerySource(session).
                        specialPowerInitiateSound(actor, button);
                result.localVoiceObject = actor;
            }
            return result;
        }

    case CommandButtonKind::Stop:
        return composeOrder(
            session, selection, localPlayer->id, tick, kind,
            engine::GameCommandType::Stop, {}, hook);
    case CommandButtonKind::HackInternet:
        {
            UnifiedCommandRouteResult result = composeOrder(
            session, selection, localPlayer->id, tick, kind,
            engine::GameCommandType::CommandButton, button.name, hook);
            if (result.kind == UnifiedCommandRouteKind::SubmitGameCommand) {
                result.localVoiceEvent = session.localSelectionQuery().voiceCue(
                    actor, engine::selection::LocalUnitVoiceCue::HackInternet);
                result.localVoiceObject = actor;
            }
            return result;
        }

    case CommandButtonKind::Guard:
    case CommandButtonKind::GuardWithoutPursuit:
    case CommandButtonKind::GuardFlyingUnitsOnly:
        return beginPending();
    case CommandButtonKind::Waypoints:
        return unsupported(kind, UnifiedCommandBackendGap::WaypointMode);
    case CommandButtonKind::ExitContainer:
        if (!commandTarget) {
            return rejected(
                kind, UnifiedCommandRouteRejection::MissingRoutePayload);
        }
        {
            engine::GameCommand command = actorCommand(
                tick, localPlayer->id, actor,
                engine::GameCommandType::ExitContainer);
            command.targetObject = commandTarget;
            return submit(kind, std::move(command), hook);
        }
    case CommandButtonKind::Evacuate:
        return composeOrder(
            session, selection, localPlayer->id, tick, kind,
            engine::GameCommandType::Evacuate, {}, hook);
    case CommandButtonKind::ExecuteRailedTransport:
        return composeOrder(
            session, selection, localPlayer->id, tick, kind,
            engine::GameCommandType::ExecuteRailedTransport, {}, hook);
    case CommandButtonKind::BeaconDelete:
    case CommandButtonKind::PlaceBeacon:
        return unsupported(kind, UnifiedCommandBackendGap::Beacon);
    case CommandButtonKind::Sell:
        return submit(
            kind, actorCommand(tick, localPlayer->id, actor,
                               engine::GameCommandType::Sell),
            hook);
    case CommandButtonKind::FireWeapon:
        if (descriptor.targetKind != game::CommandButtonTargetKind::None) {
            return beginPending();
        }
        return composeOrder(
            session, selection, localPlayer->id, tick, kind,
            engine::GameCommandType::CommandButton, button.name, hook);
    case CommandButtonKind::SwitchWeapon:
        {
            UnifiedCommandRouteResult result = composeOrder(
            session, selection, localPlayer->id, tick, kind,
            engine::GameCommandType::CommandButton, button.name, hook);
            if (result.kind == UnifiedCommandRouteKind::SubmitGameCommand) {
                const engine::selection::LocalUnitVoiceCue cue =
                    descriptor.weaponSlot == 1u
                    ? engine::selection::LocalUnitVoiceCue::WeaponSecondaryMode
                    : descriptor.weaponSlot == 2u
                        ? engine::selection::LocalUnitVoiceCue::WeaponTertiaryMode
                        : engine::selection::LocalUnitVoiceCue::WeaponPrimaryMode;
                result.localVoiceEvent =
                    session.localSelectionQuery().voiceCue(actor, cue);
                result.localVoiceObject = actor;
            }
            return result;
        }
    case CommandButtonKind::SpecialPowerConstruct:
    case CommandButtonKind::SpecialPowerConstructFromShortcut:
        if (button.object.empty() || button.specialPower.empty()) {
            return rejected(
                kind, UnifiedCommandRouteRejection::MissingRoutePayload);
        }
        return {
            .kind = UnifiedCommandRouteKind::BeginPlacement,
            .commandKind = kind,
            .placementProduct = button.object,
            .placementBackend = engine::selection::
                LocalPlacementBackendKind::SpecialPowerConstruct,
            .postAccept = hook,
        };
    case CommandButtonKind::PurchaseScience:
        return unsupported(kind, UnifiedCommandBackendGap::SciencePurchase);
    case CommandButtonKind::ToggleOvercharge:
        return submit(
            kind, actorCommand(tick, localPlayer->id, actor,
                               engine::GameCommandType::ToggleOvercharge),
            hook);
    case CommandButtonKind::HijackVehicle:
    case CommandButtonKind::ConvertToCarBomb:
    case CommandButtonKind::SabotageBuilding:
        return beginPending();
    case CommandButtonKind::SelectAllUnitsOfType:
        if (button.object.empty()) {
            return rejected(
                kind, UnifiedCommandRouteRejection::MissingRoutePayload);
        }
        return {
            .kind = UnifiedCommandRouteKind::ApplySelectionByType,
            .commandKind = kind,
            .selectionObjectType = button.object,
        };
    }

    return rejected(kind, UnifiedCommandRouteRejection::InvalidDescriptor);
}

UnifiedShortcutRouteResult UnifiedCommandRouter::routeShortcut(
    const engine::GameSession& session,
    container::StringView commandButtonName,
    uint64_t buttonStableId,
    engine::GameTick tick) {
    UnifiedShortcutRouteResult result;
    const game::CommandButtonTemplate* button = commandButtonName.empty()
        ? nullptr
        : inGameCommandQuerySource(session).findCommandButton(
              commandButtonName);
    const bool selectionByType = button &&
        button->descriptor.kind == game::CommandButtonKind::SelectAllUnitsOfType;
    const bool valid = button &&
        button->descriptor.stableId == buttonStableId &&
        (button->descriptor.kind ==
             game::CommandButtonKind::SpecialPowerFromShortcut ||
         button->descriptor.kind ==
             game::CommandButtonKind::SpecialPowerConstructFromShortcut ||
         selectionByType) &&
        button->descriptor.userActivatable();
    if (!valid) {
        result.descriptor = {
            .stableId = buttonStableId,
            .kind = game::CommandButtonKind::SpecialPowerFromShortcut,
        };
        result.route = rejected(
            result.descriptor.kind,
            UnifiedCommandRouteRejection::InvalidDescriptor);
        return result;
    }
    result.descriptor = button->descriptor;
    if (selectionByType) {
        result.route = {
            .kind = UnifiedCommandRouteKind::ApplySelectionByType,
            .commandKind = button->descriptor.kind,
            .selectionObjectType = button->object,
        };
        return result;
    }

    const PlayerState* localPlayer =
        inGameCommandQuerySource(session).localPlayer();
    if (!localPlayer || !localPlayer->isCommandPlayer()) {
        result.route = rejected(
            button->descriptor.kind,
            UnifiedCommandRouteRejection::InvalidLocalPlayer);
        return result;
    }
    const InGameCommandAggregateAvailability provider =
        evaluateInGameShortcutAvailability(
            session, localPlayer->id, *button);
    result.actor = provider.actor;
    if (!provider.actor) {
        result.route = rejected(
            button->descriptor.kind,
            UnifiedCommandRouteRejection::Unavailable);
        return result;
    }
    engine::selection::LocalSelectionState selection;
    const container::Array<engine::ObjectId, 1> source{provider.actor};
    static_cast<void>(selection.replace(source));
    result.route = route(
        session, selection, provider.actor, button->descriptor, *button,
        provider.availability, tick);
    if (result.route.kind == UnifiedCommandRouteKind::SubmitGameCommand &&
        button->descriptor.kind != game::CommandButtonKind::SpecialPower &&
        button->descriptor.kind !=
            game::CommandButtonKind::SpecialPowerFromShortcut &&
        button->descriptor.kind != game::CommandButtonKind::HackInternet &&
        button->descriptor.kind != game::CommandButtonKind::SwitchWeapon &&
        button->descriptor.kind != game::CommandButtonKind::FireWeapon &&
        !button->unitSpecificSound.empty()) {
        result.route.localVoiceEvent = button->unitSpecificSound;
        result.route.localVoiceObject = provider.actor;
    }
    return result;
}

UnifiedTokenRouteResult UnifiedCommandRouter::routeActionToken(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    const InGameCommandActionToken& token,
    engine::GameTick tick) {
    UnifiedTokenRouteResult result;
    const InGameCommandQuerySource source =
        inGameCommandQuerySource(session);
    engine::script::ScriptCommandBarPresentationConsumer bar;
    const game::CommandButtonTemplate* button = nullptr;
    const bool constructionCancelContext =
        token.slot == 0u &&
        token.descriptor.kind ==
            game::CommandButtonKind::DozerConstructCancel &&
        source.objectConstruction(token.selectedObject).underConstruction;
    if (constructionCancelContext) {
        // CP_UNDER_CONSTRUCTION is a dedicated synthetic ControlBar context.
        // The unfinished object's authored finished CommandSet intentionally
        // does not contain Command_CancelConstruction, so revalidate the
        // canonical context button rather than the finished layout.
        button = source.findCommandButton("Command_CancelConstruction");
    } else {
        static_cast<void>(synchronizeEffectiveCommandBar(
            bar, session, token.selectedObject));
        if (token.slot >= bar.slots().size()) {
            result.rejection = UnifiedTokenRouteRejection::InvalidSlot;
            return result;
        }
        const engine::script::ScriptCommandBarUiSlot& slot =
            bar.slots()[token.slot];
        if (!slot.visible || slot.commandButtonName.empty()) {
            result.rejection = UnifiedTokenRouteRejection::SlotUnavailable;
            return result;
        }
        button = source.findCommandButton(slot.commandButtonName);
    }
    if (!button || !button->descriptor.userActivatable() ||
        button->descriptor != token.descriptor) {
        result.rejection = UnifiedTokenRouteRejection::DescriptorChanged;
        return result;
    }
    const engine::ObjectId commandTarget = constructionCancelContext
        ? engine::INVALID_OBJECT_ID
        : resolveInGameContainmentPassenger(
              session, token.selectedObject, bar.slots(), token.slot);
    if (commandTarget != token.targetObject) {
        result.rejection = UnifiedTokenRouteRejection::AvailabilityChanged;
        return result;
    }
    InGameCommandSlotAvailability availability;
    if (selection.selected().size() > 1u) {
        const InGameCommandAggregateAvailability aggregate =
            evaluateInGameMultiCommandAvailability(session, selection, *button);
        if (aggregate.actor != token.selectedObject) {
            result.rejection = UnifiedTokenRouteRejection::AvailabilityChanged;
            return result;
        }
        availability = aggregate.availability;
    } else {
        availability = evaluateInGameCommandAvailability(
            session, selection, token.selectedObject, *button, true,
            commandTarget);
    }
    if (!availability.visible || !availability.enabled) {
        result.rejection = UnifiedTokenRouteRejection::AvailabilityChanged;
        return result;
    }
    // A command-bar click identifies the current selected actor and authored
    // slot; it is not a compare-and-swap over every projected availability
    // detail. Queue count/progress, active state and cooldown can legitimately
    // change while multiple clicks are waiting in the mailbox. Re-evaluate
    // authority above and route with that live value instead of rejecting a
    // still-valid second production click because the first one changed the
    // queue snapshot carried by the UI token.
    result.route = route(
        session, selection, token.selectedObject, token.descriptor,
        *button, availability, tick, commandTarget);
    if (result.route.kind == UnifiedCommandRouteKind::SubmitGameCommand &&
        token.descriptor.kind != game::CommandButtonKind::SpecialPower &&
        token.descriptor.kind !=
            game::CommandButtonKind::SpecialPowerFromShortcut &&
        token.descriptor.kind != game::CommandButtonKind::HackInternet &&
        token.descriptor.kind != game::CommandButtonKind::SwitchWeapon &&
        token.descriptor.kind != game::CommandButtonKind::FireWeapon &&
        !token.unitSpecificSound.empty()) {
        result.route.localVoiceEvent = token.unitSpecificSound;
        result.route.localVoiceObject = token.selectedObject;
    }
    return result;
}

} // namespace engine::session_query
