#include "GameLogicIntent.h"
#include "game/session/query/UnifiedCommandRouter.h"
#include "game/session/query/LocalSelectionQueryPort.h"

#include "game/session/core/GameSession.h"
#include "game/render/LocalPlacementPresentationState.h"
#include "game/session/query/LocalSelectionCommandBarPresentationConsumer.h"
#include "game/session/query/LocalSelectionPolicy.h"
#include "game/selection/PendingWorldCommandMode.h"
#include "game/session/query/WorldCommandComposer.h"
#include "game/session/query/SessionPlayerQuery.h"
#include "game/session/query/GameSessionCommandQueryPort.h"
#include "game/session/query/InGameCommandQuerySource.h"
#include "game/session/query/SessionRuntimeQuery.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace app::runtime {
using engine::session_query::InGameCommandActionToken;
using engine::session_query::InGameCommandAvailabilityReason;
using engine::session_query::InGameProductionQueueItemKind;
using engine::session_query::InGameProductionQueueActionToken;
using engine::session_query::InGameCommandSlotAvailability;
using engine::session_query::UnifiedCommandRouteKind;
using engine::session_query::UnifiedCommandRouteRejection;
using engine::session_query::UnifiedCommandRouteResult;
using engine::session_query::UnifiedCommandRouter;
namespace {

template <typename... Functions>
struct Overloaded final : Functions... {
    using Functions::operator()...;
};

template <typename... Functions>
Overloaded(Functions...) -> Overloaded<Functions...>;

// Local unit speech is per-client presentation feedback. It is emitted here,
// after the authoritative command has been submitted, precisely so it can
// never become a simulation input: the GameCommand that reaches lockstep peers
// and the replay stream carries no audio, and this call only appends to the
// session's presentation audio journal.
void playLocalUnitVoice(engine::GameSession* session,
                        container::StringView eventName,
                        engine::ObjectId object) {
    if (!session || eventName.empty() || !object) return;
    static_cast<void>(
        session->presentationPort().emitLocalUnitVoice(eventName, object));
}

// Selection responses are recomputed from the settled selection rather than
// predicted from the gesture, so a gesture that ends up selecting nothing (or
// only foreign units) stays silent.
void playLocalSelectionVoice(engine::GameLogic& gameLogic) {
    engine::GameSession* session = gameLogic.currentSession();
    if (!session) return;
    const engine::selection::LocalUnitVoiceRequest voice =
        engine::selection::LocalSelectionPolicy::selectionVoice(
            *session, gameLogic.localSelection());
    if (!voice) return;
    playLocalUnitVoice(session, voice.eventName, voice.object);
}

enum class IntentDomain : uint8_t {
    Lifecycle,
    Cleanup,
    Camera,
    Gameplay,
};

[[nodiscard]] IntentDomain intentDomain(
    const GameLogicIntent& intent) noexcept {
    return std::visit([]<typename Intent>(const Intent&) noexcept {
        using Value = std::decay_t<Intent>;
        // Lifecycle transitions, presentation acknowledgements and explicit
        // cleanup are valid outside Running. Session revision still rejects
        // a late value from another map before this policy is consulted.
        if constexpr (
            std::is_same_v<Value, StartGameIntent> ||
            std::is_same_v<Value, ClearGameIntent> ||
            std::is_same_v<Value, QueueResultActionIntent> ||
            std::is_same_v<Value, SetScriptPresentationPausedIntent> ||
            std::is_same_v<Value, SetLocalPauseSourceIntent> ||
            std::is_same_v<Value, ReconnectIntent> ||
            std::is_same_v<Value, CancelReconnectIntent> ||
            std::is_same_v<Value, ExitDisconnectedSessionIntent> ||
            std::is_same_v<Value, DismissScriptPopupIntent> ||
            std::is_same_v<Value, AcknowledgeScriptCameraCompletionIntent> ||
            std::is_same_v<Value, AcknowledgeCommandOutcomesIntent> ||
            std::is_same_v<Value, NotifyLoadingScreenPresentedIntent> ||
            std::is_same_v<Value, NotifyLoadingScreenDismissedIntent> ||
            std::is_same_v<Value, NotifyRenderStartupProgressIntent> ||
            std::is_same_v<Value, NotifyRenderStartupFrameSubmittedIntent> ||
            std::is_same_v<Value, NotifyRenderStartupFailureIntent>) {
            return IntentDomain::Lifecycle;
        } else if constexpr (
            std::is_same_v<Value, ResetLocalSelectionIntent> ||
            std::is_same_v<Value, CancelPendingWorldCommandIntent> ||
            std::is_same_v<Value, CancelLocalPlacementIntent> ||
            std::is_same_v<Value,
                CancelSelectedLocalConstructionRouteNodeIntent>) {
            return IntentDomain::Cleanup;
        } else if constexpr (std::is_same_v<Value, QueueCameraInputIntent>) {
            return IntentDomain::Camera;
        } else {
            return IntentDomain::Gameplay;
        }
    }, intent);
}

[[nodiscard]] bool intentAllowedAtDrain(
    const GameLogicIntent& intent,
    engine::GameLogic& gameLogic) noexcept {
    const IntentDomain domain = intentDomain(intent);
    const engine::GameState state = gameLogic.getState();
    if (domain == IntentDomain::Lifecycle ||
        domain == IntentDomain::Cleanup) {
        return true;
    }
    if (domain == IntentDomain::Camera) {
        if (state != engine::GameState::Running &&
            state != engine::GameState::Paused) {
            return false;
        }
    } else if (state != engine::GameState::Running) {
        // Selection, hover, production, placement and gameplay commands
        // sampled before a state transition must not mutate Paused, Result,
        // Transitioning or Loading on a later logic drain.
        return false;
    }

    // The UI projection is necessarily one publication behind the logic
    // owner. Recheck the authoritative script gate here so input sampled just
    // before DISABLE_INPUT cannot cross the cinematic boundary. Camera input
    // remains a distinct domain (and is still allowed while locally paused),
    // but ZH's DISABLE_INPUT suppresses it together with gameplay controls.
    engine::GameSession* session = gameLogic.currentSession();
    return session &&
        session->presentationPort().snapshot().gameplayInputEnabled;
}

[[nodiscard]] engine::CommandOutcomeReason routeReason(
    UnifiedCommandRouteRejection reason) noexcept {
    switch (reason) {
    case UnifiedCommandRouteRejection::None:
        return engine::CommandOutcomeReason::None;
    case UnifiedCommandRouteRejection::InvalidDescriptor:
        return engine::CommandOutcomeReason::RouterInvalidDescriptor;
    case UnifiedCommandRouteRejection::Unavailable:
        return engine::CommandOutcomeReason::RouterUnavailable;
    case UnifiedCommandRouteRejection::InvalidSelection:
        return engine::CommandOutcomeReason::RouterInvalidSelection;
    case UnifiedCommandRouteRejection::InvalidLocalPlayer:
        return engine::CommandOutcomeReason::RouterInvalidLocalPlayer;
    case UnifiedCommandRouteRejection::UnauthorizedActor:
        return engine::CommandOutcomeReason::RouterUnauthorizedActor;
    case UnifiedCommandRouteRejection::MissingRoutePayload:
        return engine::CommandOutcomeReason::RouterMissingRoutePayload;
    case UnifiedCommandRouteRejection::CommandCompositionRejected:
        return engine::CommandOutcomeReason::RouterCompositionRejected;
    }
    return engine::CommandOutcomeReason::RouterInvalidDescriptor;
}

void publishActivationOutcome(
    engine::GameLogic& gameLogic, uint64_t requestSequence,
    const InGameCommandActionToken& token,
    engine::CommandOutcomeState state,
    engine::CommandOutcomeReason reason,
    engine::CommandVoiceDisposition voice,
    engine::CommandCursorDisposition cursor) {
    const engine::GameSession* session = gameLogic.currentSession();
    gameLogic.publishCommandOutcome({
        .requestSequence = requestSequence,
        .buttonStableId = token.descriptor.stableId,
        .commandKind = token.descriptor.kind,
        .state = state,
        .reason = reason,
        .voice = voice,
        .cursor = cursor,
        .confirmedTick = session
            ? static_cast<engine::GameTick>(session->confirmedTick()) : 0,
    });
}

void publishPendingCancellation(
    engine::GameLogic& gameLogic,
    const engine::selection::PendingWorldCommandMode& mode,
    engine::CommandOutcomeReason reason) {
    if (!mode.active()) return;
    const engine::GameSession* session = gameLogic.currentSession();
    gameLogic.publishCommandOutcome({
        .requestSequence = mode.requestSequence,
        .buttonStableId = mode.buttonStableId,
        .commandKind = mode.commandKind,
        .state = engine::CommandOutcomeState::Rejected,
        .reason = reason,
        .voice = engine::CommandVoiceDisposition::None,
        .cursor = engine::CommandCursorDisposition::Unchanged,
        .confirmedTick = session
            ? static_cast<engine::GameTick>(session->confirmedTick()) : 0,
    });
}

void publishPlacementCancellation(
    engine::GameLogic& gameLogic,
    const engine::selection::LocalPlacementPreviewSnapshot& placement,
    engine::CommandOutcomeReason reason) {
    if (!placement.sourceObject ||
        placement.backend != engine::selection::
            LocalPlacementBackendKind::SpecialPowerConstruct ||
        !placement.activation.present()) {
        return;
    }
    const engine::GameSession* session = gameLogic.currentSession();
    gameLogic.publishCommandOutcome({
        .requestSequence = placement.activation.requestSequence,
        .buttonStableId = placement.activation.buttonStableId,
        .commandKind = placement.activation.commandKind,
        .state = engine::CommandOutcomeState::Rejected,
        .reason = reason,
        .voice = engine::CommandVoiceDisposition::None,
        .cursor = engine::CommandCursorDisposition::Unchanged,
        .confirmedTick = session
            ? static_cast<engine::GameTick>(session->confirmedTick()) : 0,
    });
}

void publishProductionQueueOutcome(
    engine::GameLogic& gameLogic, uint64_t requestSequence,
    const InGameProductionQueueActionToken& token,
    engine::CommandOutcomeState state,
    engine::CommandOutcomeReason reason) {
    const engine::GameSession* session = gameLogic.currentSession();
    gameLogic.publishCommandOutcome({
        .requestSequence = requestSequence,
        .commandKind = token.kind == InGameProductionQueueItemKind::Unit
            ? game::CommandButtonKind::CancelUnitBuild
            : game::CommandButtonKind::CancelUpgrade,
        .state = state,
        .reason = reason,
        .voice = state == engine::CommandOutcomeState::Rejected
            ? engine::CommandVoiceDisposition::Rejected
            : engine::CommandVoiceDisposition::Accepted,
        .cursor = state == engine::CommandOutcomeState::Rejected
            ? engine::CommandCursorDisposition::Rejected
            : engine::CommandCursorDisposition::Accepted,
        .confirmedTick = session
            ? static_cast<engine::GameTick>(session->confirmedTick()) : 0,
    });
}

// A discrete UI request can become stale or be rejected by the authoritative
// input gate before its normal visitor runs.  Keep all such request families
// behind one local receipt producer so a newly added action cannot silently
// disappear at the drain boundary.
void publishDrainRejectedReceipt(
    engine::GameLogic& gameLogic, const GameLogicIntent& intent,
    uint64_t requestSequence, engine::CommandOutcomeReason reason) {
    const auto publishDirect = [&gameLogic, requestSequence, reason](
                                   uint64_t buttonStableId,
                                   game::CommandButtonKind commandKind) {
        const engine::GameSession* session = gameLogic.currentSession();
        gameLogic.publishCommandOutcome({
            .requestSequence = requestSequence,
            .buttonStableId = buttonStableId,
            .commandKind = commandKind,
            .state = engine::CommandOutcomeState::Rejected,
            .reason = reason,
            .voice = engine::CommandVoiceDisposition::Rejected,
            .cursor = engine::CommandCursorDisposition::Rejected,
            .confirmedTick = session
                ? static_cast<engine::GameTick>(session->confirmedTick())
                : 0,
        });
    };
    std::visit(
        Overloaded{
            [&gameLogic, requestSequence, reason](
                const ActivateCommandBarSlotIntent& value) {
                publishActivationOutcome(
                    gameLogic, requestSequence, value.token,
                    engine::CommandOutcomeState::Rejected, reason,
                    engine::CommandVoiceDisposition::Rejected,
                    engine::CommandCursorDisposition::Rejected);
            },
            [&gameLogic, requestSequence, reason](
                const CancelProductionQueueItemIntent& value) {
                publishProductionQueueOutcome(
                    gameLogic, requestSequence, value.token,
                    engine::CommandOutcomeState::Rejected, reason);
            },
            [&publishDirect](const PurchaseScienceIntent& value) {
                publishDirect(value.buttonStableId,
                              game::CommandButtonKind::PurchaseScience);
            },
            [&publishDirect](const ActivateSpecialPowerShortcutIntent& value) {
                publishDirect(value.buttonStableId,
                              game::CommandButtonKind::SpecialPowerFromShortcut);
            },
            [&publishDirect](const SubmitLocalConstructionWaypointIntent& value) {
                publishDirect(value.placement.activation.buttonStableId,
                              game::CommandButtonKind::DozerConstruct);
            },
            [](const auto&) {},
        },
        intent);
}

void applyIntent(engine::GameLogic& gameLogic, GameLogicIntent intent,
                 uint64_t requestSequence) {
    std::visit(
        Overloaded{
            [&gameLogic](StartGameIntent value) {
                static_cast<void>(gameLogic.startNewGame(value.info));
            },
            [&gameLogic](ClearGameIntent) { gameLogic.clearGameData(); },
            [&gameLogic](QueueCameraInputIntent value) {
                gameLogic.queueCameraInput(value.input);
            },
            [&gameLogic](SubmitGameCommandIntent value) {
                gameLogic.submitCommand(std::move(value.command));
            },
            [&gameLogic](SubmitRepairTargetIntent value) {
                static_cast<void>(gameLogic.submitRepairTarget(value.target));
            },
            [&gameLogic](SubmitContextualWorldCommandIntent value) {
                engine::GameSession* session = gameLogic.currentSession();
                if (!session || value.viewportWidth == 0 ||
                    value.viewportHeight == 0) {
                    return;
                }
                const auto localPlayer =
                    engine::session_query::localPlayer(*session);
                if (!localPlayer || !localPlayer->commandPlayer) return;

                engine::selection::ContextualWorldCommandRequest request;
                request.tick = gameLogic.currentTick();
                request.targetObject = value.targetObject;
                if (value.targetPosition) {
                    request.targetPosition = *value.targetPosition;
                } else if (!value.targetObject || value.queued || value.forceMove ||
                           value.attackMove || value.guardPosition) {
                    engine::GameCameraState camera = value.presentationCamera
                        .value_or(session->presentationPort().snapshot().camera);
                    camera.tacticalViewportHeightScale = 1.0f;
                    const auto terrainTarget =
                        session->localPlacementPort().screenToTerrain(
                            camera,
                            {value.viewportWidth, value.viewportHeight},
                            value.screenX, value.screenY);
                    if (!terrainTarget) return;
                    request.targetPosition = {
                        .x = math::q32_32{terrainTarget->x()},
                        .y = math::q32_32{terrainTarget->y()},
                        .z = math::q32_32{terrainTarget->z()},
                        .valid = true,
                    };
                }
                request.queued = value.queued;
                request.forceAttack = value.forceAttack;
                request.forceMove = value.forceMove;
                request.attackMove = value.attackMove;
                request.guardPosition = value.guardPosition;
                engine::selection::WorldCommandComposeResult composed =
                    engine::selection::WorldCommandComposer::composeContextual(
                        *session, gameLogic.localSelection(), localPlayer->id,
                        std::move(request));
                if (composed) {
                    const bool replacesLocalRoute =
                        !composed.command->queued;
                    const container::Vector<engine::ObjectId> actors =
                        composed.command->actors;
                    const engine::GameCommandSubmissionResult submitted =
                        gameLogic.submitCommand(std::move(*composed.command));
                    if (submitted.admitted) {
                        if (replacesLocalRoute) {
                            static_cast<void>(
                                gameLogic.cancelLocalConstructionRoutes(
                                    actors));
                        }
                        playLocalUnitVoice(session, composed.voiceEventName,
                                           composed.voiceObject);
                    }
                } else if (value.fallbackSelection) {
                    const engine::selection::LocalSelectionPolicyResult result =
                        engine::selection::LocalSelectionPolicy::applyGesture(
                            *session, gameLogic.localSelection(),
                            std::move(*value.fallbackSelection));
                    if (result.changed) playLocalSelectionVoice(gameLogic);
                }
            },
            [&gameLogic](CancelPendingWorldCommandIntent value) {
                const engine::selection::PendingWorldCommandMode mode =
                    gameLogic.localSelection().pendingWorldCommand();
                if (gameLogic.localSelection().cancelPendingWorldCommand(
                        value.modeRevision)) {
                    publishPendingCancellation(
                        gameLogic, mode,
                        engine::CommandOutcomeReason::CancelledByUser);
                }
            },
            [&gameLogic](SubmitPendingWorldCommandTargetIntent value) {
                engine::GameSession* session = gameLogic.currentSession();
                const engine::selection::PendingWorldCommandMode mode =
                    gameLogic.localSelection().pendingWorldCommand();
                if (!session || !mode.active() ||
                    mode.revision != value.modeRevision ||
                    value.viewportWidth == 0 || value.viewportHeight == 0) {
                    return;
                }
                const auto localPlayer =
                    engine::session_query::localPlayer(*session);
                if (!localPlayer || !localPlayer->commandPlayer) return;

                const engine::session_query::PendingWorldCommandRevalidation
                    revalidation =
                        engine::session_query::revalidatePendingWorldCommand(
                            *session, gameLogic.localSelection(), mode);
                if (!revalidation.descriptorCurrent) {
                    static_cast<void>(gameLogic.localSelection().
                        cancelPendingWorldCommand(mode.revision));
                    gameLogic.publishCommandOutcome({
                        .requestSequence = mode.requestSequence,
                        .buttonStableId = mode.buttonStableId,
                        .commandKind = mode.commandKind,
                        .state = engine::CommandOutcomeState::Rejected,
                        .reason = engine::CommandOutcomeReason::DescriptorChanged,
                        .voice = engine::CommandVoiceDisposition::Rejected,
                        .cursor = engine::CommandCursorDisposition::Rejected,
                        .confirmedTick = static_cast<engine::GameTick>(
                            session->confirmedTick()),
                    });
                    return;
                }
                const InGameCommandSlotAvailability& liveAvailability =
                    revalidation.availability;
                if (!liveAvailability.visible || !liveAvailability.enabled ||
                    liveAvailability.reason !=
                        InGameCommandAvailabilityReason::None) {
                    static_cast<void>(gameLogic.localSelection().
                        cancelPendingWorldCommand(mode.revision));
                    gameLogic.publishCommandOutcome({
                        .requestSequence = mode.requestSequence,
                        .buttonStableId = mode.buttonStableId,
                        .commandKind = mode.commandKind,
                        .state = engine::CommandOutcomeState::Rejected,
                        .reason = engine::CommandOutcomeReason::AvailabilityChanged,
                        .voice = engine::CommandVoiceDisposition::Rejected,
                        .cursor = engine::CommandCursorDisposition::Rejected,
                        .confirmedTick = static_cast<engine::GameTick>(
                            session->confirmedTick()),
                    });
                    return;
                }

                const bool targetIsLive = value.targetObject &&
                    session->runtimeQuery().isLiveObject(value.targetObject);
                const bool objectSelected = targetIsLive &&
                    mode.acceptsObject();
                // A pointer hit on a live but disallowed relation is an
                // invalid object target, not terrain underneath that object.
                // Keep the mode active so the user can choose again.
                if (objectSelected &&
                    !engine::selection::pendingWorldTargetRelationAllowed(
                        mode, *session, localPlayer->id,
                        value.targetObject)) {
                    return;
                }
                if (mode.targetKind == engine::selection::
                        PendingWorldTargetKind::Object &&
                    !objectSelected) {
                    return;
                }

                engine::CommandPosition terrainPosition;
                if (value.targetPosition) {
                    terrainPosition = *value.targetPosition;
                } else if (mode.acceptsPosition() ||
                           mode.kind == engine::selection::
                               PendingWorldCommandKind::CombatDrop) {
                    engine::GameCameraState camera = value.presentationCamera
                        .value_or(session->presentationPort().snapshot().camera);
                    camera.tacticalViewportHeightScale = 1.0f;
                    const auto terrainTarget =
                        session->localPlacementPort().screenToTerrain(
                            camera,
                            {value.viewportWidth, value.viewportHeight},
                            value.screenX, value.screenY);
                    if (!terrainTarget) return;
                    terrainPosition = {
                        .x = math::q32_32{terrainTarget->x()},
                        .y = math::q32_32{terrainTarget->y()},
                        .z = math::q32_32{terrainTarget->z()},
                        .valid = true,
                    };
                }

                engine::selection::WorldCommandRequest request;
                request.tick = gameLogic.currentTick();
                request.queued = value.queued || mode.queued;
                request.commandName = mode.commandButtonName;
                switch (mode.kind) {
                case engine::selection::PendingWorldCommandKind::AttackMove:
                    request.type = engine::GameCommandType::AttackMove;
                    request.targetPosition = terrainPosition;
                    request.commandName.clear();
                    break;
                case engine::selection::PendingWorldCommandKind::Guard:
                    request.type = mode.commandKind ==
                            game::CommandButtonKind::GuardWithoutPursuit
                        ? engine::GameCommandType::GuardWithoutPursuit
                        : mode.commandKind ==
                              game::CommandButtonKind::GuardFlyingUnitsOnly
                            ? engine::GameCommandType::GuardFlyingUnitsOnly
                            : engine::GameCommandType::Guard;
                    request.targetObject = objectSelected
                        ? value.targetObject : engine::INVALID_OBJECT_ID;
                    if (!objectSelected) request.targetPosition = terrainPosition;
                    request.commandName.clear();
                    break;
                case engine::selection::PendingWorldCommandKind::SpecialPower:
                    request.type = engine::GameCommandType::SpecialPower;
                    request.targetObject = objectSelected
                        ? value.targetObject : engine::INVALID_OBJECT_ID;
                    if (!objectSelected) {
                        request.targetPosition = terrainPosition;
                    }
                    break;
                case engine::selection::PendingWorldCommandKind::FireWeapon:
                    request.type = engine::GameCommandType::CommandButton;
                    request.targetObject = objectSelected
                        ? value.targetObject : engine::INVALID_OBJECT_ID;
                    if (!objectSelected) {
                        request.targetPosition = terrainPosition;
                    }
                    break;
                case engine::selection::PendingWorldCommandKind::SetRallyPoint:
                    request.type = engine::GameCommandType::SetFactoryRallyPoint;
                    request.targetPosition = terrainPosition;
                    request.commandName.clear();
                    request.queued = false;
                    break;
                case engine::selection::PendingWorldCommandKind::CombatDrop:
                    request.type = engine::GameCommandType::CombatDrop;
                    request.targetObject = objectSelected
                        ? value.targetObject : engine::INVALID_OBJECT_ID;
                    // CombatDrop's queue consumer requires a concrete landing
                    // position even for the retail object-target form.
                    request.targetPosition = terrainPosition;
                    break;
                case engine::selection::PendingWorldCommandKind::
                        IntentionalContact:
                    if (!objectSelected) return;
                    request.type = engine::GameCommandType::CommandButton;
                    request.targetObject = value.targetObject;
                    request.queued = value.queued;
                    break;
                case engine::selection::PendingWorldCommandKind::None:
                    return;
                }

                engine::selection::LocalSelectionState shortcutSelection;
                const engine::selection::LocalSelectionState* commandSelection =
                    &gameLogic.localSelection();
                if (mode.sourceMayBeUnselected) {
                    const container::Array<engine::ObjectId, 1> source{
                        mode.sourceObject};
                    static_cast<void>(shortcutSelection.replace(source));
                    commandSelection = &shortcutSelection;
                }
                engine::selection::WorldCommandComposeResult composed =
                    engine::selection::WorldCommandComposer::compose(
                        *session, *commandSelection, localPlayer->id,
                        std::move(request));
                if (!composed) return;
                composed.command->activation = {
                    .requestSequence = mode.requestSequence,
                    .buttonStableId = mode.buttonStableId,
                    .commandKind = mode.commandKind,
                    .postAccept = mode.postAccept,
                    .postAcceptActor = mode.postAcceptActor,
                };
                if (!gameLogic.localSelection().cancelPendingWorldCommand(
                        mode.revision)) {
                    return;
                }
                const engine::GameCommandSubmissionResult submitted =
                    [&]() {
                        const bool replacesLocalRoute =
                            !composed.command->queued;
                        const container::Vector<engine::ObjectId> actors =
                            composed.command->actors;
                        engine::GameCommandSubmissionResult result =
                            gameLogic.submitCommand(
                                std::move(*composed.command));
                        if (result.admitted && replacesLocalRoute) {
                            static_cast<void>(
                                gameLogic.cancelLocalConstructionRoutes(
                                    actors));
                        }
                        return result;
                    }();
                if (submitted.admitted) {
                    const bool specialPowerVoice =
                        mode.voice.kind == engine::selection::
                            PendingWorldVoiceKind::SpecialPower;
                    const container::StringView voice =
                        !specialPowerVoice &&
                            mode.voice.unitSpecificSound.empty()
                        ? container::StringView{composed.voiceEventName}
                        : container::StringView{
                              mode.voice.unitSpecificSound};
                    const engine::ObjectId speaker =
                        !specialPowerVoice &&
                            mode.voice.unitSpecificSound.empty()
                        ? composed.voiceObject : mode.sourceObject;
                    playLocalUnitVoice(session, voice, speaker);
                }
            },
            [&gameLogic](SubmitBeaconTextIntent value) {
                static_cast<void>(gameLogic.submitBeaconText(value.text));
            },
            [&gameLogic, requestSequence](PurchaseScienceIntent value) {
                engine::GameSession* session = gameLogic.currentSession();
                std::optional<engine::GameCommand> command = session
                    ? session->commandQuery().composeSciencePurchase(
                          {
                              .tick = gameLogic.currentTick(),
                              .requestSequence = requestSequence,
                              .buttonStableId = value.buttonStableId,
                              .commandButtonName = value.commandButtonName,
                              .science = value.science,
                          })
                    : std::nullopt;
                if (!command) {
                    gameLogic.publishCommandOutcome({
                        .requestSequence = requestSequence,
                        .buttonStableId = value.buttonStableId,
                        .commandKind =
                            game::CommandButtonKind::PurchaseScience,
                        .state = engine::CommandOutcomeState::Rejected,
                        .reason = engine::CommandOutcomeReason::
                            MissingCommandPayload,
                        .voice = engine::CommandVoiceDisposition::Rejected,
                        .cursor = engine::CommandCursorDisposition::Rejected,
                        .confirmedTick = session
                            ? static_cast<engine::GameTick>(
                                  session->confirmedTick())
                            : 0,
                    });
                    return;
                }
                static_cast<void>(
                    gameLogic.submitCommand(std::move(*command)));
            },
            [&gameLogic, requestSequence](
                ActivateSpecialPowerShortcutIntent value) {
                engine::GameSession* session = gameLogic.currentSession();
                const engine::session_query::UnifiedShortcutRouteResult resolved =
                    session
                    ? UnifiedCommandRouter::routeShortcut(
                          *session, value.commandButtonName,
                          value.buttonStableId, gameLogic.currentTick())
                    : engine::session_query::UnifiedShortcutRouteResult{};
                InGameCommandActionToken token;
                token.selectedObject = resolved.actor;
                token.descriptor = resolved.descriptor;
                if (session && resolved.route.kind ==
                        UnifiedCommandRouteKind::ApplySelectionByType) {
                    engine::selection::LocalSelectionGesture gesture;
                    gesture.kind = engine::selection::
                        LocalSelectionGestureKind::ExplicitTypeAcrossMap;
                    gesture.objectType = resolved.route.selectionObjectType;
                    const engine::selection::LocalSelectionPolicyResult result =
                        engine::selection::LocalSelectionPolicy::applyGesture(
                            *session, gameLogic.localSelection(),
                            std::move(gesture));
                    if (result.changed) playLocalSelectionVoice(gameLogic);
                    publishActivationOutcome(
                        gameLogic, requestSequence, token,
                        result.accepted
                            ? engine::CommandOutcomeState::Accepted
                            : engine::CommandOutcomeState::Rejected,
                        result.accepted
                            ? engine::CommandOutcomeReason::None
                            : engine::CommandOutcomeReason::
                                LocalPresentationRejected,
                        result.accepted
                            ? engine::CommandVoiceDisposition::Accepted
                            : engine::CommandVoiceDisposition::Rejected,
                        result.accepted
                            ? engine::CommandCursorDisposition::Accepted
                            : engine::CommandCursorDisposition::Rejected);
                    return;
                }
                if (!session || !resolved.actor ||
                    resolved.route.kind == UnifiedCommandRouteKind::Rejected) {
                    publishActivationOutcome(
                        gameLogic, requestSequence, token,
                        engine::CommandOutcomeState::Rejected,
                        engine::CommandOutcomeReason::RouterUnavailable,
                        engine::CommandVoiceDisposition::Rejected,
                        engine::CommandCursorDisposition::Rejected);
                    return;
                }
                UnifiedCommandRouteResult route = resolved.route;
                if (route.kind ==
                    UnifiedCommandRouteKind::SubmitGameCommand &&
                    route.command) {
                    if (value.queued &&
                        (route.command->type ==
                             engine::GameCommandType::SpecialPower ||
                         route.command->type ==
                             engine::GameCommandType::CommandButton)) {
                        route.command->queued = true;
                    }
                    route.command->activation = {
                        .requestSequence = requestSequence,
                        .buttonStableId = value.buttonStableId,
                        .commandKind = resolved.descriptor.kind,
                    };
                    if (route.postAccept) {
                        route.command->activation.postAccept =
                            engine::CommandPostAcceptAction::
                                MarkSingleUseCommandUsed;
                        route.command->activation.postAcceptActor =
                            route.postAccept.actor;
                    }
                    const engine::GameCommandSubmissionResult submitted =
                        gameLogic.submitCommand(std::move(*route.command));
                    if (submitted.admitted) {
                        playLocalUnitVoice(
                            session, route.localVoiceEvent,
                            route.localVoiceObject);
                    }
                    return;
                }
                if (route.kind ==
                        UnifiedCommandRouteKind::BeginPendingWorldTarget &&
                    route.pendingTarget) {
                    const engine::selection::PendingWorldCommandMode previous =
                        gameLogic.localSelection().pendingWorldCommand();
                    const engine::selection::LocalPlacementPreviewSnapshot
                        previousPlacement =
                            session->localPlacementPort().snapshot();
                    static_cast<void>(
                        session->localPlacementPort().cancel());
                    publishPlacementCancellation(
                        gameLogic, previousPlacement,
                        engine::CommandOutcomeReason::SupersededByLocalMode);
                    route.pendingTarget->requestSequence = requestSequence;
                    route.pendingTarget->buttonStableId =
                        value.buttonStableId;
                    route.pendingTarget->commandKind =
                        resolved.descriptor.kind;
                    route.pendingTarget->queued = value.queued;
                    if (!gameLogic.localSelection().beginPendingWorldCommand(
                            std::move(*route.pendingTarget))) {
                        publishActivationOutcome(
                            gameLogic, requestSequence, token,
                            engine::CommandOutcomeState::Rejected,
                            engine::CommandOutcomeReason::
                                LocalPresentationRejected,
                            engine::CommandVoiceDisposition::Rejected,
                            engine::CommandCursorDisposition::Rejected);
                        return;
                    }
                    if (previous.active()) {
                        publishPendingCancellation(
                            gameLogic, previous,
                            engine::CommandOutcomeReason::
                                SupersededByLocalMode);
                    }
                    publishActivationOutcome(
                        gameLogic, requestSequence, token,
                        engine::CommandOutcomeState::PendingConfirmation,
                        engine::CommandOutcomeReason::None,
                        engine::CommandVoiceDisposition::AwaitConfirmation,
                        engine::CommandCursorDisposition::AwaitConfirmation);
                    return;
                }
                if (route.kind == UnifiedCommandRouteKind::BeginPlacement &&
                    !route.placementProduct.empty()) {
                    engine::CommandActivationContext activation{
                        .requestSequence = requestSequence,
                        .buttonStableId = value.buttonStableId,
                        .commandKind = resolved.descriptor.kind,
                    };
                    if (route.postAccept) {
                        activation.postAccept =
                            engine::CommandPostAcceptAction::
                                MarkSingleUseCommandUsed;
                        activation.postAcceptActor = route.postAccept.actor;
                    }
                    const engine::selection::PendingWorldCommandMode
                        previousWorld =
                            gameLogic.localSelection().pendingWorldCommand();
                    const engine::selection::LocalPlacementPreviewSnapshot
                        previousPlacement =
                            session->localPlacementPort().snapshot();
                    if (!session->localPlacementPort().begin(
                            resolved.actor, route.placementProduct,
                            route.placementBackend, activation)) {
                        publishActivationOutcome(
                            gameLogic, requestSequence, token,
                            engine::CommandOutcomeState::Rejected,
                            engine::CommandOutcomeReason::
                                LocalPresentationRejected,
                            engine::CommandVoiceDisposition::Rejected,
                            engine::CommandCursorDisposition::Rejected);
                        return;
                    }
                    if (gameLogic.localSelection().
                            cancelPendingWorldCommand()) {
                        publishPendingCancellation(
                            gameLogic, previousWorld,
                            engine::CommandOutcomeReason::
                                SupersededByLocalMode);
                    }
                    publishPlacementCancellation(
                        gameLogic, previousPlacement,
                        engine::CommandOutcomeReason::SupersededByLocalMode);
                    publishActivationOutcome(
                        gameLogic, requestSequence, token,
                        engine::CommandOutcomeState::PendingConfirmation,
                        engine::CommandOutcomeReason::None,
                        engine::CommandVoiceDisposition::AwaitConfirmation,
                        engine::CommandCursorDisposition::AwaitConfirmation);
                    return;
                }
                publishActivationOutcome(
                    gameLogic, requestSequence, token,
                    engine::CommandOutcomeState::Rejected,
                    route.kind == UnifiedCommandRouteKind::UnsupportedBackend
                        ? engine::CommandOutcomeReason::UnsupportedBackend
                        : routeReason(route.rejection),
                    engine::CommandVoiceDisposition::Rejected,
                    engine::CommandCursorDisposition::Rejected);
            },
            [&gameLogic](ResetLocalSelectionIntent) {
                gameLogic.clearLocalConstructionRouteNodeSelection();
                const engine::selection::PendingWorldCommandMode mode =
                    gameLogic.localSelection().pendingWorldCommand();
                engine::GameSession* session = gameLogic.currentSession();
                const engine::selection::LocalPlacementPreviewSnapshot
                    placement = session
                        ? session->localPlacementPort().snapshot()
                        : engine::selection::LocalPlacementPreviewSnapshot{};
                static_cast<void>(
                    gameLogic.localSelection().clearTransientInteraction());
                if (session) {
                    static_cast<void>(
                        session->localPlacementPort().cancel());
                }
                if (mode.active() &&
                    !gameLogic.localSelection().pendingWorldCommand().active()) {
                    publishPendingCancellation(
                        gameLogic, mode,
                        engine::CommandOutcomeReason::
                            CancelledBySelectionChange);
                }
                publishPlacementCancellation(
                    gameLogic, placement,
                    engine::CommandOutcomeReason::CancelledBySelectionChange);
            },
            [&gameLogic](ApplyLocalSelectionGestureIntent value) {
                engine::GameSession* session = gameLogic.currentSession();
                if (!session) return;
                gameLogic.clearLocalConstructionRouteNodeSelection();
                const engine::selection::PendingWorldCommandMode mode =
                    gameLogic.localSelection().pendingWorldCommand();
                const engine::selection::LocalSelectionPolicyResult result =
                    engine::selection::LocalSelectionPolicy::
                        applyGesture(*session, gameLogic.localSelection(),
                                     std::move(value.gesture));
                // Only a selection that actually changed answers, so
                // re-clicking a unit the player already has selected does not
                // make it repeat itself every frame.
                if (result.changed) playLocalSelectionVoice(gameLogic);
                if (mode.active() &&
                    !gameLogic.localSelection().pendingWorldCommand().active()) {
                    publishPendingCancellation(
                        gameLogic, mode,
                        engine::CommandOutcomeReason::
                            CancelledBySelectionChange);
                }
            },
            [&gameLogic](SelectLocalOrderWaypointIntent value) {
                engine::GameSession* session = gameLogic.currentSession();
                if (!session || !value.waypoint) return;
                gameLogic.clearLocalConstructionRouteNodeSelection();
                const auto localPlayer =
                    engine::session_query::localPlayer(*session);
                if (!localPlayer || !localPlayer->commandPlayer ||
                    engine::session_query::inGameCommandQuerySource(
                        *session).ownerOf(value.waypoint.actor) !=
                        std::optional<engine::PlayerId>{localPlayer->id}) {
                    return;
                }
                static_cast<void>(gameLogic.localSelection().
                    selectOrderWaypoint(value.waypoint));
            },
            [&gameLogic](SelectLocalConstructionRouteNodeIntent value) {
                if (!value.previewIdentity ||
                    !gameLogic.selectLocalConstructionRouteNode(
                        value.previewIdentity)) {
                    return;
                }
                static_cast<void>(gameLogic.localSelection().clear());
            },
            [&gameLogic](CancelSelectedLocalConstructionRouteNodeIntent) {
                static_cast<void>(
                    gameLogic.cancelSelectedLocalConstructionRouteNode());
            },
            [&gameLogic](ApplyLocalControlGroupIntent value) {
                engine::GameSession* session = gameLogic.currentSession();
                if (!session) return;
                gameLogic.clearLocalConstructionRouteNodeSelection();
                const engine::selection::PendingWorldCommandMode mode =
                    gameLogic.localSelection().pendingWorldCommand();
                const engine::selection::LocalSelectionPolicyResult result =
                    engine::selection::LocalSelectionPolicy::applyControlGroup(
                        *session, gameLogic.localSelection(), value.request);
                // Recalling a control group is the classic group-select cue.
                if (result.changed) playLocalSelectionVoice(gameLogic);
                if (mode.active() &&
                    !gameLogic.localSelection().pendingWorldCommand().active()) {
                    publishPendingCancellation(
                        gameLogic, mode,
                        engine::CommandOutcomeReason::
                            CancelledBySelectionChange);
                }
                if (result.cameraTarget) {
                    engine::GameCameraInput input;
                    input.absoluteTarget = *result.cameraTarget;
                    input.hasAbsoluteTarget = true;
                    input.manualIntent = true;
                    gameLogic.queueCameraInput(input);
                }
            },
            [&gameLogic](ApplyLocalSelectionShortcutIntent value) {
                engine::GameSession* session = gameLogic.currentSession();
                if (!session) return;
                gameLogic.clearLocalConstructionRouteNodeSelection();
                const engine::selection::PendingWorldCommandMode mode =
                    gameLogic.localSelection().pendingWorldCommand();
                const engine::selection::LocalSelectionPolicyResult result =
                    engine::selection::LocalSelectionPolicy::applyShortcut(
                        *session, gameLogic.localSelection(), value.shortcut);
                if (result.changed) playLocalSelectionVoice(gameLogic);
                if (mode.active() &&
                    !gameLogic.localSelection().pendingWorldCommand().active()) {
                    publishPendingCancellation(
                        gameLogic, mode,
                        engine::CommandOutcomeReason::
                            CancelledBySelectionChange);
                }
                if (result.cameraTarget) {
                    engine::GameCameraInput input;
                    input.absoluteTarget = *result.cameraTarget;
                    input.hasAbsoluteTarget = true;
                    input.manualIntent = true;
                    gameLogic.queueCameraInput(input);
                }
            },
            [&gameLogic](SubmitScatterIntent) {
                engine::GameSession* session = gameLogic.currentSession();
                if (!session) return;
                const auto localPlayer =
                    engine::session_query::localPlayer(*session);
                const auto selected = gameLogic.localSelection().selected();
                if (!localPlayer || !localPlayer->commandPlayer ||
                    selected.empty()) {
                    return;
                }
                engine::GameCommand command;
                command.tick = gameLogic.currentTick();
                command.player = localPlayer->id;
                command.source = engine::CommandSource::Local;
                command.type = engine::GameCommandType::Scatter;
                command.actors.assign(selected.begin(), selected.end());
                static_cast<void>(gameLogic.submitCommand(std::move(command)));
            },
            [&gameLogic](SubmitCreateFormationIntent) {
                engine::GameSession* session = gameLogic.currentSession();
                if (!session) return;
                const auto localPlayer =
                    engine::session_query::localPlayer(*session);
                const auto selected = gameLogic.localSelection().selected();
                if (!localPlayer || !localPlayer->commandPlayer ||
                    selected.empty()) {
                    return;
                }
                engine::GameCommand command;
                command.tick = gameLogic.currentTick();
                command.player = localPlayer->id;
                command.source = engine::CommandSource::Local;
                command.type = engine::GameCommandType::CreateFormation;
                command.actors.assign(selected.begin(), selected.end());
                static_cast<void>(gameLogic.submitCommand(
                    std::move(command)));
            },
            [&gameLogic](SetHoveredObjectIntent value) {
                static_cast<void>(
                    gameLogic.localSelection().setHovered(value.object));
            },
            [&gameLogic](CancelLocalPlacementIntent value) {
                if (engine::GameSession* session =
                        gameLogic.currentSession()) {
                    const engine::selection::LocalPlacementPreviewSnapshot
                        placement =
                            session->localPlacementPort().snapshot();
                    if (!session->localPlacementPort().cancel(
                            value.previewIdentity)) {
                        return;
                    }
                    publishPlacementCancellation(
                        gameLogic, placement,
                        engine::CommandOutcomeReason::CancelledByUser);
                }
            },
            [&gameLogic](UpdateLocalPlacementPointerIntent value) {
                engine::GameSession* session = gameLogic.currentSession();
                if (!session || !session->localPlacementPort().active() ||
                    value.viewportWidth == 0 || value.viewportHeight == 0) {
                    return;
                }
                engine::GameCameraState camera = value.presentationCamera
                    .value_or(session->presentationPort().snapshot().camera);
                if (value.fullHeightViewport) {
                    camera.tacticalViewportHeightScale = 1.0f;
                }
                const auto terrainStart =
                    session->localPlacementPort().screenToTerrain(
                        camera,
                        {value.viewportWidth, value.viewportHeight},
                        value.anchorStartX, value.anchorStartY);
                const auto terrainEnd =
                    session->localPlacementPort().screenToTerrain(
                        camera,
                        {value.viewportWidth, value.viewportHeight},
                        value.anchorEndX, value.anchorEndY);
                if (!terrainStart || !terrainEnd) return;

                bool poseUpdated =
                    session->localPlacementPort().updateFromAnchors(
                        *terrainStart, *terrainEnd, value.forceAttackSnap);
                if (value.refreshLegality ||
                    (value.confirm && poseUpdated)) {
                    static_cast<void>(
                        session->localPlacementPort().refreshLegality(
                            value.confirm));
                }
                if (value.confirm && poseUpdated) {
                    // Queued placement updates the shared cursor only. The
                    // paired fixed-value intent below publishes the queued
                    // deterministic Build command immediately.
                    if (value.queueConstruction) return;
                    if (std::optional<engine::GameCommand> command =
                            session->localPlacementPort().takeCommand()) {
                        // `VoiceBuildResponse` is emitted once from the
                        // confirmed builder-task admission.  Do not predict
                        // it here: a direct placement would otherwise speak
                        // at local queue admission and again when its build
                        // task materializes, while a Shift route would only
                        // use the latter path.  Keeping the feedback on the
                        // shared confirmed edge gives ordinary and waypoint
                        // construction identical timing and rejection
                        // semantics.
                        static_cast<void>(
                            gameLogic.submitCommand(std::move(*command)));
                    }
                }
            },
            [&gameLogic, requestSequence](
                SubmitLocalConstructionWaypointIntent value) {
                engine::GameSession* session = gameLogic.currentSession();
                auto reject = [&](engine::CommandOutcomeReason reason) {
                    gameLogic.publishCommandOutcome({
                        .requestSequence = requestSequence,
                        .buttonStableId =
                            value.placement.activation.buttonStableId,
                        .commandKind =
                            game::CommandButtonKind::DozerConstruct,
                        .state = engine::CommandOutcomeState::Rejected,
                        .reason = reason,
                        .voice = engine::CommandVoiceDisposition::Rejected,
                        .cursor = engine::CommandCursorDisposition::Rejected,
                        .confirmedTick = session
                            ? static_cast<engine::GameTick>(
                                  session->confirmedTick())
                            : 0,
                    });
                };
                if (!session) {
                    reject(engine::CommandOutcomeReason::GameNotRunning);
                    return;
                }
                const std::optional<
                    engine::selection::LocalPlacementPreviewSnapshot> node =
                    session->localPlacementPort().
                        makeLocalConstructionRouteNodeFromAnchors(
                            value.placement, value.anchorStart,
                            value.anchorEnd, value.hasDirection,
                            value.forceAttackSnap);
                if (!node ||
                    !gameLogic.appendLocalConstructionRouteNode(*node)) {
                    reject(engine::CommandOutcomeReason::
                               LocalPresentationRejected);
                    return;
                }
                // A Shift path point is a local planning commitment, not a
                // confirmed Build command.  Its own request must finish when
                // the route accepts the node; the future builder task may
                // legally skip it after its second placement/finance check.
                // Do not predict VoiceBuildResponse or alter the active
                // placement cursor here: both belong to their existing
                // confirmed/local owners.
                gameLogic.publishCommandOutcome({
                    .requestSequence = requestSequence,
                    .buttonStableId =
                        value.placement.activation.buttonStableId,
                    .commandKind = game::CommandButtonKind::DozerConstruct,
                    .state = engine::CommandOutcomeState::Accepted,
                    .reason = engine::CommandOutcomeReason::None,
                    .voice = engine::CommandVoiceDisposition::None,
                    .cursor = engine::CommandCursorDisposition::Unchanged,
                    .confirmedTick = static_cast<engine::GameTick>(
                        session->confirmedTick()),
                });
            },
            [&gameLogic, requestSequence](CancelProductionQueueItemIntent value) {
                const auto reject = [&](engine::CommandOutcomeReason reason) {
                    publishProductionQueueOutcome(
                        gameLogic, requestSequence, value.token,
                        engine::CommandOutcomeState::Rejected, reason);
                };
                engine::GameSession* session = gameLogic.currentSession();
                const auto localPlayer = session
                    ? engine::session_query::localPlayer(*session) : std::nullopt;
                if (!session || !localPlayer ||
                    !localPlayer->commandPlayer ||
                    !value.token.isValid()) {
                    reject(engine::CommandOutcomeReason::MissingCommandPayload);
                    return;
                }
                if (value.token.selectionRevision !=
                        gameLogic.localSelection().revision() ||
                    std::find(gameLogic.localSelection().selected().begin(),
                              gameLogic.localSelection().selected().end(),
                              value.token.producer) ==
                        gameLogic.localSelection().selected().end()) {
                    reject(engine::CommandOutcomeReason::StaleSelection);
                    return;
                }
                if (!engine::session_query::isCurrentProductionQueueAction(
                        *session, localPlayer->id, value.token)) {
                    // The selected producer is still valid, but this exact
                    // queue job was consumed or replaced before the local
                    // click reached the logic thread.  Keep it distinct from
                    // a CommandButton availability change so ControlBar can
                    // refresh the player with an actionable explanation.
                    reject(engine::CommandOutcomeReason::QueueChanged);
                    return;
                }

                const bool unit = value.token.kind ==
                    InGameProductionQueueItemKind::Unit;
                const uint8_t requested = unit
                    ? std::clamp<uint8_t>(value.repeatCount, 1u, 5u) : 1u;
                const uint8_t available = unit
                    ? value.token.cancellationProductionIdCount : 1u;
                const uint8_t count = std::min(requested, available);
                for (uint8_t index = 0; index < count; ++index) {
                    engine::GameCommand command;
                    command.tick = gameLogic.currentTick();
                    command.player = localPlayer->id;
                    command.source = engine::CommandSource::Local;
                    command.actors.push_back(value.token.producer);
                    // Queue-row cancellation has already been validated by
                    // InGameProductionQueueActionToken above.  It is not a
                    // CommandButton activation: adding a request sequence
                    // here makes GameLogic route it through the CommandButton
                    // single-use/stability gate with no buttonStableId, which
                    // deterministically rejects a valid queue cancellation.
                    if (unit) {
                        command.type =
                            engine::GameCommandType::CancelProduction;
                        command.productionId =
                            value.token.cancellationProductionIds[
                                available - index - 1u];
                    } else {
                        command.type =
                            engine::GameCommandType::CancelPlayerUpgrade;
                        command.commandName = value.token.upgradeName;
                    }
                    command.activation = {
                        .requestSequence = index == 0 ? requestSequence : 0,
                        .buttonStableId = 0,
                        .commandKind = index == 0
                            ? unit
                                ? game::CommandButtonKind::CancelUnitBuild
                                : game::CommandButtonKind::CancelUpgrade
                            : game::CommandButtonKind::Unknown,
                    };
                    if (!gameLogic.submitCommand(std::move(command)).admitted) {
                        break;
                    }
                }
            },
            [&gameLogic, requestSequence](ActivateCommandBarSlotIntent value) {
                const auto reject = [&](engine::CommandOutcomeReason reason) {
                    publishActivationOutcome(
                        gameLogic, requestSequence, value.token,
                        engine::CommandOutcomeState::Rejected, reason,
                        engine::CommandVoiceDisposition::Rejected,
                        engine::CommandCursorDisposition::Rejected);
                };
                engine::GameSession* session = gameLogic.currentSession();
                if (!session) {
                    reject(engine::CommandOutcomeReason::GameNotRunning);
                    return;
                }
                if (!value.token.isValid()) {
                    reject(engine::CommandOutcomeReason::InvalidActionToken);
                    return;
                }
                if (value.token.selectionRevision !=
                    gameLogic.localSelection().revision()) {
                    reject(engine::CommandOutcomeReason::StaleSelection);
                    return;
                }
                if (value.token.orderWaypoint) {
                    const engine::selection::LocalOrderWaypointSelection
                        selected = gameLogic.localSelection().
                            selectedOrderWaypoint();
                    const auto localPlayer =
                        engine::session_query::localPlayer(*session);
                    const engine::InGameOrderWaypointReadModel live =
                        engine::session_query::inGameCommandQuerySource(
                            *session).orderWaypoint(
                                value.token.selectedObject,
                                value.token.orderWaypointSourceSequence);
                    if (!selected || !localPlayer ||
                        !localPlayer->commandPlayer ||
                        selected.actor != value.token.selectedObject ||
                        selected.sourceSequence !=
                            value.token.orderWaypointSourceSequence ||
                        selected.kind != value.token.orderWaypointKind ||
                        !live.exists || live.kind != selected.kind) {
                        reject(engine::CommandOutcomeReason::SelectionMismatch);
                        return;
                    }
                    engine::GameCommand command;
                    command.player = localPlayer->id;
                    command.source = engine::CommandSource::Local;
                    command.type =
                        engine::GameCommandType::CancelOrderWaypoint;
                    command.actors.push_back(selected.actor);
                    command.productionId = selected.sourceSequence;
                    command.activation = {
                        .requestSequence = requestSequence,
                        .buttonStableId = value.token.descriptor.stableId,
                        .commandKind = value.token.descriptor.kind,
                    };
                    const engine::GameCommandSubmissionResult submitted =
                        gameLogic.submitCommand(std::move(command));
                    if (!submitted.admitted) {
                        // submitCommand() is the sole receipt producer after
                        // a command has been composed.  It has already
                        // published the exact ingress rejection; replacing it
                        // here with RouterUnavailable emitted a second
                        // terminal outcome for one UI request.
                        return;
                    }
                    return;
                }
                const container::Span<const engine::ObjectId> selectedObjects =
                    gameLogic.localSelection().selected();
                if (selectedObjects.empty() ||
                    std::find(selectedObjects.begin(), selectedObjects.end(),
                              value.token.selectedObject) ==
                        selectedObjects.end()) {
                    reject(engine::CommandOutcomeReason::SelectionMismatch);
                    return;
                }
                if (selectedObjects.size() == 1u) {
                    const auto selected = engine::selection::
                        LocalSelectionCommandBarPresentationConsumer::
                            resolveSingleObject(
                                *session, gameLogic.localSelection());
                    if (!selected || selected->object !=
                            value.token.selectedObject) {
                        reject(engine::CommandOutcomeReason::SelectionMismatch);
                        return;
                    }
                }
                engine::session_query::UnifiedTokenRouteResult routed =
                    UnifiedCommandRouter::routeActionToken(
                        *session, gameLogic.localSelection(), value.token,
                        gameLogic.currentTick());
                switch (routed.rejection) {
                case engine::session_query::UnifiedTokenRouteRejection::None:
                    break;
                case engine::session_query::UnifiedTokenRouteRejection::InvalidSlot:
                    reject(engine::CommandOutcomeReason::InvalidSlot);
                    return;
                case engine::session_query::UnifiedTokenRouteRejection::SlotUnavailable:
                    reject(engine::CommandOutcomeReason::SlotUnavailable);
                    return;
                case engine::session_query::UnifiedTokenRouteRejection::DescriptorChanged:
                    reject(engine::CommandOutcomeReason::DescriptorChanged);
                    return;
                case engine::session_query::UnifiedTokenRouteRejection::AvailabilityChanged:
                    reject(engine::CommandOutcomeReason::AvailabilityChanged);
                    return;
                }
                UnifiedCommandRouteResult route = std::move(routed.route);
                switch (route.kind) {
                case UnifiedCommandRouteKind::SubmitGameCommand:
                    if (!route.command) {
                        reject(engine::CommandOutcomeReason::MissingCommandPayload);
                        return;
                    }
                    {
                        if (value.queued &&
                            (route.command->type ==
                                 engine::GameCommandType::SpecialPower ||
                             route.command->type ==
                                 engine::GameCommandType::CommandButton)) {
                            route.command->queued = true;
                        }
                        const container::String localVoiceEvent =
                            route.localVoiceEvent;
                        const engine::ObjectId localVoiceObject =
                            route.localVoiceObject;
                        bool admittedAny = false;
                        uint8_t repeatCount = route.command->type ==
                                engine::GameCommandType::QueueProduction
                            ? std::clamp<uint8_t>(
                                  value.repeatCount, 1u, 5u)
                            : 1u;
                        const auto& queue = value.token.availability.queue;
                        if (queue.capacity != 0) {
                            const uint16_t remaining = queue.capacity >
                                    queue.count
                                ? static_cast<uint16_t>(
                                      queue.capacity - queue.count)
                                : 0u;
                            repeatCount = std::min<uint8_t>(
                                repeatCount,
                                static_cast<uint8_t>(std::min<uint16_t>(
                                    remaining, 5u)));
                        }
                        // A SingleUseCommand changes its authoritative state
                        // after the first accepted command.  Shift must not
                        // turn one such click into four deterministic stale
                        // activations.
                        if (route.postAccept) repeatCount = 1;
                        for (uint8_t index = 0; index < repeatCount; ++index) {
                            if (index != 0) {
                                engine::session_query::UnifiedTokenRouteResult
                                    repeated = UnifiedCommandRouter::
                                        routeActionToken(
                                            *session,
                                            gameLogic.localSelection(),
                                            value.token,
                                            gameLogic.currentTick());
                                if (repeated.rejection != engine::session_query::
                                        UnifiedTokenRouteRejection::None ||
                                    repeated.route.kind !=
                                        UnifiedCommandRouteKind::
                                            SubmitGameCommand ||
                                    !repeated.route.command ||
                                    repeated.route.command->type !=
                                        engine::GameCommandType::
                                            QueueProduction) {
                                    break;
                                }
                                route = std::move(repeated.route);
                            }
                            if (index == 0) {
                                route.command->activation = {
                                    .requestSequence = requestSequence,
                                    .buttonStableId =
                                        value.token.descriptor.stableId,
                                    .commandKind = route.commandKind,
                                    .postAccept = route.postAccept
                                        ? engine::CommandPostAcceptAction::
                                              MarkSingleUseCommandUsed
                                        : engine::CommandPostAcceptAction::None,
                                    .postAcceptActor = route.postAccept
                                        ? route.postAccept.actor
                                        : engine::INVALID_OBJECT_ID,
                                };
                            } else {
                                // One UI click has one terminal receipt. The
                                // remaining Shift-expanded commands are still
                                // independently sequenced deterministic input,
                                // but deliberately have no second UI
                                // activation or post-accept side effect.
                                route.command->activation = {};
                            }
                            if (!gameLogic.submitCommand(
                                    std::move(*route.command)).admitted) {
                                break;
                            }
                            admittedAny = true;
                        }
                        if (admittedAny) {
                            playLocalUnitVoice(
                                session, localVoiceEvent, localVoiceObject);
                        }
                    }
                    return;
                case UnifiedCommandRouteKind::BeginPendingWorldTarget: {
                    if (!route.pendingTarget) {
                        reject(engine::CommandOutcomeReason::MissingCommandPayload);
                        return;
                    }
                    const engine::selection::LocalPlacementPreviewSnapshot
                        previousPlacement =
                            session->localPlacementPort().snapshot();
                    static_cast<void>(
                        session->localPlacementPort().cancel());
                    publishPlacementCancellation(
                        gameLogic, previousPlacement,
                        engine::CommandOutcomeReason::SupersededByLocalMode);
                    route.pendingTarget->requestSequence = requestSequence;
                    route.pendingTarget->buttonStableId =
                        value.token.descriptor.stableId;
                    route.pendingTarget->commandKind = route.commandKind;
                    route.pendingTarget->queued = value.queued;
                    if (route.postAccept) {
                        route.pendingTarget->postAcceptButtonStableId =
                            route.postAccept.commandButtonStableId;
                        route.pendingTarget->postAcceptCommandKind =
                            route.commandKind;
                        route.pendingTarget->postAccept =
                            engine::CommandPostAcceptAction::
                                MarkSingleUseCommandUsed;
                        route.pendingTarget->postAcceptActor =
                            route.postAccept.actor;
                    }
                    const engine::selection::PendingWorldCommandMode previous =
                        gameLogic.localSelection().pendingWorldCommand();
                    if (!gameLogic.localSelection().beginPendingWorldCommand(
                            std::move(*route.pendingTarget))) {
                        reject(engine::CommandOutcomeReason::LocalPresentationRejected);
                        return;
                    }
                    if (previous.active()) {
                        publishPendingCancellation(
                            gameLogic, previous,
                            engine::CommandOutcomeReason::
                                SupersededByLocalMode);
                    }
                    publishActivationOutcome(
                        gameLogic, requestSequence, value.token,
                        engine::CommandOutcomeState::PendingConfirmation,
                        engine::CommandOutcomeReason::None,
                        engine::CommandVoiceDisposition::AwaitConfirmation,
                        engine::CommandCursorDisposition::AwaitConfirmation);
                    return;
                }
                case UnifiedCommandRouteKind::BeginPlacement: {
                    if (route.placementProduct.empty()) {
                        reject(engine::CommandOutcomeReason::MissingCommandPayload);
                        return;
                    }
                    const engine::selection::PendingWorldCommandMode previous =
                        gameLogic.localSelection().pendingWorldCommand();
                    const engine::selection::LocalPlacementPreviewSnapshot
                        previousPlacement =
                            session->localPlacementPort().snapshot();
                    // Ordinary Dozer placement does not keep the original
                    // click request pending, but it must retain the authored
                    // button identity.  A later main-thread Shift waypoint
                    // assigns its own requestSequence and confirmed-command
                    // admission then revalidates this stable descriptor.
                    engine::CommandActivationContext activation{
                        .buttonStableId =
                            value.token.descriptor.stableId,
                        .commandKind = route.commandKind,
                    };
                    const bool specialPowerConstruct =
                        route.placementBackend == engine::selection::
                            LocalPlacementBackendKind::
                                SpecialPowerConstruct;
                    if (specialPowerConstruct) {
                        activation.requestSequence = requestSequence;
                    }
                    if (route.postAccept) {
                        if (activation.buttonStableId == 0) {
                            activation.buttonStableId =
                                route.postAccept.commandButtonStableId;
                        }
                        activation.commandKind = route.commandKind;
                        activation.postAccept = engine::CommandPostAcceptAction::
                            MarkSingleUseCommandUsed;
                        activation.postAcceptActor = route.postAccept.actor;
                    }
                    if (!session->localPlacementPort().begin(
                            value.token.selectedObject,
                            route.placementProduct, route.placementBackend,
                            activation)) {
                        reject(engine::CommandOutcomeReason::LocalPresentationRejected);
                        return;
                    }
                    if (gameLogic.localSelection().
                            cancelPendingWorldCommand()) {
                        publishPendingCancellation(
                            gameLogic, previous,
                            engine::CommandOutcomeReason::
                                SupersededByLocalMode);
                    }
                    publishPlacementCancellation(
                        gameLogic, previousPlacement,
                        engine::CommandOutcomeReason::SupersededByLocalMode);
                    publishActivationOutcome(
                        gameLogic, requestSequence, value.token,
                        specialPowerConstruct
                            ? engine::CommandOutcomeState::PendingConfirmation
                            : engine::CommandOutcomeState::Accepted,
                        engine::CommandOutcomeReason::None,
                        specialPowerConstruct
                            ? engine::CommandVoiceDisposition::AwaitConfirmation
                            : engine::CommandVoiceDisposition::Accepted,
                        specialPowerConstruct
                            ? engine::CommandCursorDisposition::AwaitConfirmation
                            : engine::CommandCursorDisposition::Accepted);
                    return;
                }
                case UnifiedCommandRouteKind::CancelPlacement:
                {
                    const engine::selection::PendingWorldCommandMode previous =
                        gameLogic.localSelection().pendingWorldCommand();
                    if (gameLogic.localSelection().
                            cancelPendingWorldCommand()) {
                        publishPendingCancellation(
                            gameLogic, previous,
                            engine::CommandOutcomeReason::CancelledByUser);
                    }
                    const engine::selection::LocalPlacementPreviewSnapshot
                        placement =
                            session->localPlacementPort().snapshot();
                    static_cast<void>(
                        session->localPlacementPort().cancel());
                    publishPlacementCancellation(
                        gameLogic, placement,
                        engine::CommandOutcomeReason::CancelledByUser);
                    publishActivationOutcome(
                        gameLogic, requestSequence, value.token,
                        engine::CommandOutcomeState::Accepted,
                        engine::CommandOutcomeReason::None,
                        engine::CommandVoiceDisposition::Accepted,
                        engine::CommandCursorDisposition::Accepted);
                    return;
                }
                case UnifiedCommandRouteKind::ApplySelectionByType: {
                    if (route.selectionObjectType.empty()) {
                        reject(engine::CommandOutcomeReason::
                            MissingCommandPayload);
                        return;
                    }
                    engine::selection::LocalSelectionGesture gesture;
                    gesture.kind = engine::selection::
                        LocalSelectionGestureKind::ExplicitTypeAcrossMap;
                    gesture.objectType = std::move(
                        route.selectionObjectType);
                    const engine::selection::LocalSelectionPolicyResult
                        selectionResult = engine::selection::
                            LocalSelectionPolicy::applyGesture(
                                *session, gameLogic.localSelection(),
                                std::move(gesture));
                    if (!selectionResult.accepted) {
                        reject(engine::CommandOutcomeReason::
                            LocalPresentationRejected);
                        return;
                    }
                    if (selectionResult.changed)
                        playLocalSelectionVoice(gameLogic);
                    publishActivationOutcome(
                        gameLogic, requestSequence, value.token,
                        engine::CommandOutcomeState::Accepted,
                        engine::CommandOutcomeReason::None,
                        engine::CommandVoiceDisposition::Accepted,
                        engine::CommandCursorDisposition::Accepted);
                    return;
                }
                case UnifiedCommandRouteKind::Rejected:
                    reject(routeReason(route.rejection));
                    return;
                case UnifiedCommandRouteKind::UnsupportedBackend:
                    publishActivationOutcome(
                        gameLogic, requestSequence, value.token,
                        engine::CommandOutcomeState::Rejected,
                        engine::CommandOutcomeReason::UnsupportedBackend,
                        engine::CommandVoiceDisposition::SuppressedUnsupported,
                        engine::CommandCursorDisposition::Unsupported);
                    return;
                }
            },
            [&gameLogic](QueueResultActionIntent value) {
                static_cast<void>(gameLogic.queueResultAction(value.action));
            },
            [&gameLogic](SetScriptPresentationPausedIntent value) {
                static_cast<void>(
                    gameLogic.setScriptPresentationPaused(value.paused));
            },
            [&gameLogic](SetLocalPauseSourceIntent value) {
                static_cast<void>(gameLogic.setLocalPauseSource(
                    value.source, value.paused));
            },
            [&gameLogic](ReconnectIntent) {
                static_cast<void>(gameLogic.requestDisconnectAction(
                    engine::DisconnectAction::Reconnect));
            },
            [&gameLogic](CancelReconnectIntent) {
                static_cast<void>(gameLogic.requestDisconnectAction(
                    engine::DisconnectAction::Cancel));
            },
            [&gameLogic](ExitDisconnectedSessionIntent) {
                static_cast<void>(gameLogic.requestDisconnectAction(
                    engine::DisconnectAction::Exit));
            },
            [&gameLogic](DismissScriptPopupIntent value) {
                if (engine::GameSession* session =
                        gameLogic.currentSession()) {
                    static_cast<void>(session->presentationPort().dismissPopup(
                        value.presentationEpoch, value.sequence));
                }
            },
            [&gameLogic](AcknowledgeScriptCameraCompletionIntent value) {
                if (engine::GameSession* session =
                        gameLogic.currentSession()) {
                    static_cast<void>(session->presentationPort().
                        acknowledgeScriptCameraCompletion(
                            value.completion));
                }
            },
            [&gameLogic](AcknowledgeCommandOutcomesIntent value) {
                gameLogic.acknowledgeCommandOutcomes(value.revision);
            },
            [&gameLogic](NotifyLoadingScreenPresentedIntent value) {
                gameLogic.notifyLoadingScreenPresented(value.loadingRevision);
            },
            [&gameLogic](NotifyLoadingScreenDismissedIntent value) {
                gameLogic.notifyLoadingScreenDismissed(value.loadingRevision);
            },
            [&gameLogic](NotifyRenderStartupProgressIntent value) {
                gameLogic.notifyRenderStartupProgress(
                    value.loadingRevision, value.sessionRevision,
                    value.progress);
            },
            [&gameLogic](NotifyRenderStartupFrameSubmittedIntent value) {
                gameLogic.notifyRenderStartupFrameSubmitted(
                    value.loadingRevision, value.sessionRevision);
            },
            [&gameLogic](NotifyRenderStartupFailureIntent value) {
                gameLogic.notifyRenderStartupFailure(
                    value.loadingRevision, value.sessionRevision,
                    std::move(value.error));
            },
        },
        std::move(intent));
}

} // namespace

bool GameLogicIntentMailbox::post(
    GameLogicIntent intent, uint64_t expectedSessionRevision) {
    return postTracked(
        std::move(intent), expectedSessionRevision).has_value();
}

std::optional<uint64_t> GameLogicIntentMailbox::postTracked(
    GameLogicIntent intent, uint64_t expectedSessionRevision) {
    uint64_t sequence = m_nextSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence == 0) {
        sequence = m_nextSequence.fetch_add(1, std::memory_order_relaxed);
    }
    GameLogicIntentEnvelope envelope{
        .sequence = sequence,
        .expectedSessionRevision = expectedSessionRevision,
        .intent = std::move(intent),
    };
    bool posted = false;
    if (std::holds_alternative<QueueCameraInputIntent>(envelope.intent)) {
        std::lock_guard lock(m_discreteMutex);
        if (!m_closed) {
            const auto& incoming =
                std::get<QueueCameraInputIntent>(envelope.intent);
            if (!incoming.replacePending && m_cameraInput &&
                m_cameraInput->expectedSessionRevision ==
                    envelope.expectedSessionRevision) {
                auto& accumulated =
                    std::get<QueueCameraInputIntent>(m_cameraInput->intent)
                        .input;
                accumulated.accumulate(
                    std::get<QueueCameraInputIntent>(envelope.intent).input);
                m_cameraInput->sequence = envelope.sequence;
            } else {
                m_cameraInput = std::move(envelope);
            }
            posted = true;
        }
    } else if (std::holds_alternative<SetHoveredObjectIntent>(
                   envelope.intent)) {
        posted = m_hoverInput.publish(std::move(envelope));
    } else if (const auto* placement =
                   std::get_if<UpdateLocalPlacementPointerIntent>(
                       &envelope.intent);
               placement && !placement->confirm) {
        posted = m_placementPointerInput.publish(std::move(envelope));
    } else if (std::holds_alternative<NotifyRenderStartupProgressIntent>(
                   envelope.intent)) {
        posted = m_startupProgressInput.publish(std::move(envelope));
    } else {
        std::lock_guard lock(m_discreteMutex);
        if (!m_closed) {
            m_discrete.push_back(std::move(envelope));
            posted = true;
        }
    }
    if (posted) {
        m_posted.fetch_add(1, std::memory_order_relaxed);
    }
    return posted ? std::optional<uint64_t>{sequence} : std::nullopt;
}

size_t GameLogicIntentMailbox::drainAndApply(engine::GameLogic& gameLogic) {
    container::Vector<GameLogicIntentEnvelope> pending;
    {
        std::lock_guard lock(m_discreteMutex);
        pending.reserve(m_discrete.size() + 4u);
        while (!m_discrete.empty()) {
            pending.push_back(std::move(m_discrete.front()));
            m_discrete.pop_front();
        }
        if (m_cameraInput) {
            pending.push_back(std::move(*m_cameraInput));
            m_cameraInput.reset();
        }
    }
    const auto takeLatest = [&pending](auto& mailbox) {
        GameLogicIntentEnvelope value;
        if (mailbox.tryTake(value)) pending.push_back(std::move(value));
    };
    takeLatest(m_hoverInput);
    takeLatest(m_placementPointerInput);
    takeLatest(m_startupProgressInput);
    std::sort(
        pending.begin(), pending.end(),
        [](const GameLogicIntentEnvelope& left,
           const GameLogicIntentEnvelope& right) {
            return left.sequence < right.sequence;
        });
    for (GameLogicIntentEnvelope& queued : pending) {
        auto apply = [this, &gameLogic](GameLogicIntentEnvelope envelope) {
            if (envelope.expectedSessionRevision != 0 &&
                envelope.expectedSessionRevision !=
                    gameLogic.sessionRevision()) {
                m_rejectedStaleSession.fetch_add(
                    1, std::memory_order_relaxed);
                publishDrainRejectedReceipt(
                    gameLogic, envelope.intent, envelope.sequence,
                    engine::CommandOutcomeReason::StaleSession);
                return;
            }
            if (!intentAllowedAtDrain(envelope.intent, gameLogic)) {
                m_rejectedLifecycle.fetch_add(
                    1, std::memory_order_relaxed);
                // A UI request sampled just before a cinematic/teardown gate
                // must receive the same terminal receipt as a normal routing
                // rejection.  Silently dropping it left ControlBar state
                // pending forever even though its command can never run.
                const engine::CommandOutcomeReason reason =
                    gameLogic.getState() == engine::GameState::Running
                        ? engine::CommandOutcomeReason::SourceBecameUnavailable
                        : engine::CommandOutcomeReason::GameNotRunning;
                publishDrainRejectedReceipt(
                    gameLogic, envelope.intent, envelope.sequence, reason);
                return;
            }
            applyIntent(
                gameLogic, std::move(envelope.intent), envelope.sequence);
            m_applied.fetch_add(1, std::memory_order_relaxed);
        };
        apply(std::move(queued));
    }
    return pending.size();
}

void GameLogicIntentMailbox::close() noexcept {
    {
        std::lock_guard lock(m_discreteMutex);
        m_closed = true;
    }
    m_hoverInput.close();
    m_placementPointerInput.close();
    m_startupProgressInput.close();
}

void GameLogicIntentMailbox::reset() {
    {
        std::lock_guard lock(m_discreteMutex);
        m_discrete.clear();
        m_cameraInput.reset();
        m_closed = false;
    }
    m_hoverInput.reset();
    m_placementPointerInput.reset();
    m_startupProgressInput.reset();
    m_nextSequence.store(1, std::memory_order_relaxed);
    m_posted.store(0, std::memory_order_relaxed);
    m_applied.store(0, std::memory_order_relaxed);
    m_rejectedStaleSession.store(0, std::memory_order_relaxed);
    m_rejectedLifecycle.store(0, std::memory_order_relaxed);
    m_rejectedOverflow.store(0, std::memory_order_relaxed);
}

GameLogicIntentStats GameLogicIntentMailbox::stats() const noexcept {
    return {
        .posted = m_posted.load(std::memory_order_relaxed),
        .applied = m_applied.load(std::memory_order_relaxed),
        .rejectedStaleSession =
            m_rejectedStaleSession.load(std::memory_order_relaxed),
        .rejectedLifecycle =
            m_rejectedLifecycle.load(std::memory_order_relaxed),
        .rejectedOverflow =
            m_rejectedOverflow.load(std::memory_order_relaxed),
    };
}

} // namespace app::runtime
