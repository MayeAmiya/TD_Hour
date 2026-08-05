#include "core/container/container_types.h"
#include "game/session/query/WorldCommandComposer.h"

#include "game/session/core/GameSession.h"
#include "game/session/command/OrderExecutor.h"
#include "game/session/query/InGameCommandQuerySource.h"
#include "game/session/query/GameSessionCommandQueryPort.h"
#include "game/session/query/LocalSelectionQueryPort.h"
#include "game/session/query/WorldCommandQueryPort.h"
#include "game/object/definition/ObjectKindOf.h"

#include <algorithm>
#include <utility>

namespace engine::selection {
namespace {

[[nodiscard]] WorldCommandComposeResult finishCommand(
    GameCommand command, WorldCommandComposeResult result) {
    if (command.actors.empty()) {
        result.rejection = WorldCommandComposeRejection::NoControllableActors;
        result.message = "selection contains no live actors capable of the contextual order";
        return result;
    }
    container::String validationError;
    const std::optional<PlayerOrder> canonical =
        OrderExecutor::fromGameCommand(command, &validationError);
    if (!canonical) {
        result.rejection = WorldCommandComposeRejection::MalformedCommand;
        result.message = std::move(validationError);
        return result;
    }
    command.actors = canonical->actors;
    result.controllableActorCount = command.actors.size();
    result.command = std::move(command);
    return result;
}

// Maps an accepted order onto the authored per-unit cue RefCode's CommandXlat
// would have played for it. Returns None for order families whose
// acknowledgement is not part of the ThingTemplate audio array.
[[nodiscard]] std::optional<LocalUnitVoiceCue> orderVoiceCue(
    GameCommandType type) noexcept {
    switch (type) {
    case GameCommandType::Move:
    case GameCommandType::AttackMove:
        return LocalUnitVoiceCue::Move;
    case GameCommandType::Attack:
        return LocalUnitVoiceCue::Attack;
    case GameCommandType::Repair:
        return LocalUnitVoiceCue::Repair;
    case GameCommandType::CombatDrop:
        return LocalUnitVoiceCue::CombatDrop;
    case GameCommandType::Guard:
    case GameCommandType::GuardWithoutPursuit:
        return LocalUnitVoiceCue::Guard;
    case GameCommandType::EnterContainer:
        return LocalUnitVoiceCue::Enter;
    case GameCommandType::Evacuate:
        return LocalUnitVoiceCue::Unload;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<LocalUnitVoiceCue> localCueForWeaponVoice(
    WorldCommandWeaponVoiceKind kind) noexcept {
    switch (kind) {
    case WorldCommandWeaponVoiceKind::ClearBuilding:
        return LocalUnitVoiceCue::ClearBuilding;
    case WorldCommandWeaponVoiceKind::Subdue:
        return LocalUnitVoiceCue::Subdue;
    case WorldCommandWeaponVoiceKind::Disarm:
        return LocalUnitVoiceCue::Disarm;
    case WorldCommandWeaponVoiceKind::SnipePilot:
        return LocalUnitVoiceCue::SnipePilot;
    case WorldCommandWeaponVoiceKind::Melee:
        return LocalUnitVoiceCue::Melee;
    case WorldCommandWeaponVoiceKind::FlameLocation:
        return LocalUnitVoiceCue::FlameLocation;
    case WorldCommandWeaponVoiceKind::PoisonLocation:
        return LocalUnitVoiceCue::PoisonLocation;
    case WorldCommandWeaponVoiceKind::FireRocketPods:
        return LocalUnitVoiceCue::FireRocketPods;
    case WorldCommandWeaponVoiceKind::None:
        return std::nullopt;
    }
    return std::nullopt;
}

// Chooses the one unit that answers for the whole order. RefCode walks the
// selected drawables and keeps the first cue it finds, "upgrading" it only for
// special cases, so a twelve-tank order produces one voice rather than twelve.
// Walking the already-ordered actor list makes the choice deterministic
// without drawing from any random stream.
void attachOrderVoice(
    const GameSession& session, WorldCommandComposeResult& result,
    ObjectId forceMoveTarget = INVALID_OBJECT_ID) {
    if (!result.command) return;
    std::optional<LocalUnitVoiceCue> cue =
        orderVoiceCue(result.command->type);
    const LocalSelectionQueryPort units = session.localSelectionQuery();
    const WorldCommandQueryPort world = session.worldCommandQuery();
    const game::CommandButtonTemplate* commandButton =
        result.command->commandName.empty() ? nullptr
        : session_query::inGameCommandQuerySource(session).findCommandButton(
              result.command->commandName);
    const bool commandButtonWeapon = commandButton &&
        commandButton->descriptor.kind == game::CommandButtonKind::FireWeapon;
    const std::optional<uint8_t> requestedWeaponSlot = commandButtonWeapon
        ? std::optional<uint8_t>{commandButton->descriptor.weaponSlot}
        : std::nullopt;
    // CommandXlat begins a forced move with VoiceMove, then walks every
    // selected drawable and upgrades the one local acknowledgement to
    // VoiceCrush as soon as one actor can run over the object that was under
    // the cursor.  The resulting Move command carries only the terrain
    // destination; retain the clicked object here only for that local choice.
    if (forceMoveTarget) {
        for (const ObjectId actor : result.command->actors) {
            if (!units.canCrushTarget(actor, forceMoveTarget)) continue;
            result.voiceEventName = units.voiceCue(
                actor, LocalUnitVoiceCue::Crush);
            result.voiceObject = actor;
            return;
        }
    }
    if (result.command->type == GameCommandType::Attack) {
        if (result.command->forceAttack && !result.command->targetObject) {
            cue = LocalUnitVoiceCue::Bombard;
        } else if (result.command->targetObject &&
                   units.hasKind(result.command->targetObject,
                                 game::ObjectKindOf::Aircraft)) {
            cue = LocalUnitVoiceCue::AttackAir;
        }
    }
    const bool weaponOrder = result.command->type == GameCommandType::Attack ||
        commandButtonWeapon;
    if (!cue && !weaponOrder) return;
    for (const ObjectId actor : result.command->actors) {
        std::optional<LocalUnitVoiceCue> actorCue = cue;
        if (weaponOrder) {
            actorCue = localCueForWeaponVoice(world.weaponOrderVoice(
                actor, requestedWeaponSlot, result.command->targetObject,
                commandButtonWeapon)).value_or(
                    actorCue.value_or(LocalUnitVoiceCue::Attack));
        }
        if (!actorCue) continue;
        if (*actorCue == LocalUnitVoiceCue::Move &&
            world.usesWorkerShoesMoveVoice(actor)) {
            // CommandXlat replaces VoiceMove rather than falling back to it
            // once WorkerShoes is complete.  An intentionally unauthored
            // VoiceMoveUpgraded is therefore silent, just like retail.
            actorCue = LocalUnitVoiceCue::MoveUpgraded;
        }
        container::String eventName = units.voiceCue(actor, *actorCue);
        if (eventName.empty()) continue;
        result.voiceEventName = std::move(eventName);
        result.voiceObject = actor;
        return;
    }
}

// CommandXlat selects an Enter acknowledgement from the confirmed target
// classification: HEAL_PAD has the highest priority (VoiceGetHealed), then
// structures select garrison/hostile entry, otherwise the regular Enter cue.
// These specialised cues are UnitSpecificSounds-only names.
void attachContainmentVoice(
    const GameSession& session, PlayerId localPlayer,
    WorldCommandComposeResult& result) {
    if (!result.command) return;
    const LocalSelectionQueryPort units = session.localSelectionQuery();
    const ObjectId target = result.command->targetObject;
    LocalUnitVoiceCue preferred = LocalUnitVoiceCue::Enter;
    if (target) {
        const LocalSelectionObjectSnapshot destination =
            units.inspect(localPlayer, target);
        if (destination.healPad) {
            preferred = LocalUnitVoiceCue::GetHealed;
        } else if (destination.structure) {
            preferred = destination.local
                ? LocalUnitVoiceCue::Garrison
                : LocalUnitVoiceCue::EnterHostile;
        }
    }
    for (const ObjectId actor : result.command->actors) {
        container::String eventName = units.voiceCue(actor, preferred);
        // The original HEAL_PAD branch assigns VoiceGetHealed directly and
        // sets skip; it deliberately does not fall through to VoiceEnter when
        // the unit authored no healing acknowledgement.
        if (eventName.empty() && preferred != LocalUnitVoiceCue::Enter &&
            preferred != LocalUnitVoiceCue::GetHealed) {
            eventName = units.voiceCue(actor, LocalUnitVoiceCue::Enter);
        }
        if (eventName.empty()) continue;
        result.voiceEventName = std::move(eventName);
        result.voiceObject = actor;
        return;
    }
}

} // namespace

bool pendingWorldTargetRelationAllowed(
    const PendingWorldCommandMode& mode, const GameSession& session,
    PlayerId localPlayer, ObjectId target) noexcept {
    return session.worldCommandQuery().relationAllowed(
        mode.allowedRelations, localPlayer, target);
}

WorldCommandComposeResult WorldCommandComposer::compose(
    const GameSession& session, const LocalSelectionState& selection,
    PlayerId localPlayer, WorldCommandRequest request) {
    const WorldCommandQueryPort world = session.worldCommandQuery();
    WorldCommandComposeResult result;
    result.selectedActorCount = selection.selected().size();
    if (!world.isCommandPlayer(localPlayer)) {
        result.rejection = WorldCommandComposeRejection::InvalidLocalPlayer;
        result.message = "local view has no live command player";
        return result;
    }
    if (selection.selected().empty()) {
        result.rejection = WorldCommandComposeRejection::EmptySelection;
        result.message = "no objects are selected";
        return result;
    }

    GameCommand command;
    command.tick = request.tick;
    command.sequence = request.sequence;
    command.player = localPlayer;
    command.source = CommandSource::Local;
    command.type = request.type;
    command.targetObject = request.targetObject;
    command.targetPosition = request.targetPosition;
    command.commandName = std::move(request.commandName);
    command.queued = request.queued;
    command.actors.reserve(selection.selected().size());
    for (const ObjectId object : selection.selected()) {
        if (!world.isControlledLiveObject(localPlayer, object)) {
            continue;
        }
        command.actors.push_back(object);
    }
    result.controllableActorCount = command.actors.size();
    if (command.actors.empty()) {
        result.rejection = WorldCommandComposeRejection::NoControllableActors;
        result.message = "selection contains no live objects controlled by the local player";
        return result;
    }

    if (command.type == GameCommandType::Repair) {
        // Repair is a dedicated confirmed dispatcher transaction, not an
        // ObjectOrderQueue family. Keep its local composition strict while
        // leaving target eligibility to the authoritative session.
        if (!command.targetObject || command.queued ||
            command.targetPosition.valid || !command.commandName.empty()) {
            result.rejection = WorldCommandComposeRejection::MalformedCommand;
            result.message = "repair requires one object target and no unrelated payload";
            return result;
        }
        std::sort(command.actors.begin(), command.actors.end());
        command.actors.erase(
            std::unique(command.actors.begin(), command.actors.end()),
            command.actors.end());
    } else if (command.type == GameCommandType::Evacuate ||
               command.type == GameCommandType::ExecuteRailedTransport) {
        if (command.targetObject || command.queued ||
            command.targetPosition.valid || !command.commandName.empty()) {
            result.rejection = WorldCommandComposeRejection::MalformedCommand;
            result.message =
                "selected-group transaction contains unrelated payload";
            return result;
        }
        std::sort(command.actors.begin(), command.actors.end());
        command.actors.erase(
            std::unique(command.actors.begin(), command.actors.end()),
            command.actors.end());
    } else if (command.type == GameCommandType::SetFactoryRallyPoint) {
        if (command.actors.size() != 1u || !command.targetPosition.valid ||
            command.targetObject || command.queued ||
            !command.commandName.empty()) {
            result.rejection = WorldCommandComposeRejection::MalformedCommand;
            result.message =
                "factory rally requires one selected producer and a terrain position";
            return result;
        }
    } else {
        container::String validationError;
        const std::optional<PlayerOrder> canonical =
            OrderExecutor::fromGameCommand(command, &validationError);
        if (!canonical) {
            result.rejection = WorldCommandComposeRejection::MalformedCommand;
            result.message = std::move(validationError);
            return result;
        }
        command.actors = canonical->actors;
    }
    result.command = std::move(command);
    // Explicit command families (Guard, Evacuate, an ability's world target)
    // acknowledge through the same per-unit voices as a contextual click.
    if (result.command->type == GameCommandType::EnterContainer) {
        attachContainmentVoice(session, localPlayer, result);
    } else {
        attachOrderVoice(session, result);
    }
    return result;
}

WorldCommandComposeResult WorldCommandComposer::composeContextual(
    const GameSession& session, const LocalSelectionState& selection,
    PlayerId localPlayer,
    ContextualWorldCommandRequest request) {
    const WorldCommandQueryPort world = session.worldCommandQuery();
    WorldCommandComposeResult result;
    result.selectedActorCount = selection.selected().size();
    if (!world.isCommandPlayer(localPlayer)) {
        result.rejection = WorldCommandComposeRejection::InvalidLocalPlayer;
        result.message = "local view has no live command player";
        return result;
    }
    if (selection.selected().empty()) {
        result.rejection = WorldCommandComposeRejection::EmptySelection;
        result.message = "no objects are selected";
        return result;
    }

    // Shift/BEGIN_WAYPOINTS controls replacement-versus-append semantics; it
    // does not erase the contextual command family. A queued enemy click is
    // still Attack, a queued attack-move remains AttackMove, and a queued
    // ground click remains Move.
    const bool queuedOrder = request.queued;
    const bool forceMove = request.forceMove && !queuedOrder &&
        !request.attackMove && !request.guardPosition;

    // CommandXlat resolves a dozer's repair/resume context before ordinary
    // ally-object reservation. TD previously classified the same click as
    // Reserved and dropped it, despite already having a confirmed Repair
    // transaction. The typed query also covers repair-dock entry and
    // unfinished-site resumption, while Shift/force modes retain their
    // explicit waypoint/attack semantics.
    if (!queuedOrder && !request.attackMove && !request.guardPosition &&
        !forceMove && !request.forceAttack && request.targetObject &&
        session.commandQuery().repairSelectionTargetAction(
            localPlayer, selection.selected(), request.targetObject) !=
            PlayerRepairTargetAction::None) {
        WorldCommandRequest repair;
        repair.tick = request.tick;
        repair.sequence = request.sequence;
        repair.type = GameCommandType::Repair;
        repair.targetObject = request.targetObject;
        return compose(
            session, selection, localPlayer, std::move(repair));
    }

    // RefCode's forced-attack exception lives inside
    // WeaponSet::getAbleToAttackSpecificObject and is consulted only after the
    // KINDOF_UNATTACKABLE rejection, so Ctrl+click cannot force-fire such an
    // object either. Reject the gesture instead of silently retargeting the
    // terrain under it, matching the Reserved policy below.
    if (request.forceAttack && !forceMove &&
        request.targetObject &&
        !world.isAttackableTarget(request.targetObject)) {
        result.rejection = WorldCommandComposeRejection::MalformedCommand;
        result.message = "clicked object cannot be attacked, even by force-fire";
        return result;
    }
    // ForceMove is intentionally a Move to the terrain under the cursor even
    // when that cursor was over an object.
    const bool contextualEnter = !queuedOrder && !request.attackMove &&
        !request.guardPosition && !forceMove && !request.forceAttack &&
        request.targetObject &&
        std::any_of(selection.selected().begin(), selection.selected().end(),
            [&](ObjectId actor) {
                return world.isControlledLiveObject(localPlayer, actor) &&
                    world.actorCanEnterContainer(actor, request.targetObject);
            });
    // CommandXlat evaluates enter-object before ordinary attack.  This must be
    // actor-aware: only a selection that actually passes containment admission
    // receives an EnterContainer command; all others retain the target's
    // ordinary context action.
    const WorldContextTargetAction targetAction = contextualEnter
        ? WorldContextTargetAction::Enter
        : (request.attackMove || request.guardPosition || forceMove)
        ? WorldContextTargetAction::Ground
        : request.forceAttack
        ? WorldContextTargetAction::Attack
        : (request.targetObject
            ? world.contextualTarget(localPlayer, request.targetObject)
            : WorldContextTargetAction::Ground);
    const bool attack = !forceMove && (request.forceAttack ||
        targetAction == WorldContextTargetAction::Attack);
    const bool enter = !queuedOrder && !forceMove && !request.forceAttack &&
        targetAction == WorldContextTargetAction::Enter;
    // A live object which is not a legal normal-attack target is reserved for
    // later contextual families (repair/enter/dock/etc.).  ZH does not turn a
    // click on such an object into an accidental ground move.
    if (targetAction == WorldContextTargetAction::Reserved) {
        result.rejection = WorldCommandComposeRejection::MalformedCommand;
        result.message = "clicked object has no ordinary Move/Attack context action";
        return result;
    }
    if (!attack && !enter && !request.targetPosition.valid) {
        result.rejection = WorldCommandComposeRejection::MalformedCommand;
        result.message = "ground move requires a valid terrain position";
        return result;
    }

    GameCommand command;
    command.tick = request.tick;
    command.sequence = request.sequence;
    command.player = localPlayer;
    command.source = CommandSource::Local;
    command.type = request.guardPosition ? GameCommandType::Guard
        : request.attackMove ? GameCommandType::AttackMove
        : attack ? GameCommandType::Attack
                          : enter ? GameCommandType::EnterContainer
                                  : GameCommandType::Move;
    command.targetObject = request.guardPosition || forceMove
        ? INVALID_OBJECT_ID : (attack || enter)
        ? request.targetObject : INVALID_OBJECT_ID;
    command.targetPosition = (request.attackMove || request.guardPosition)
        ? request.targetPosition
        : (attack || enter)
        ? (attack && !request.targetObject
            ? request.targetPosition : CommandPosition{})
        : request.targetPosition;
    command.queued = enter ? false : request.queued;
    command.forceAttack = request.forceAttack && !forceMove;
    command.actors.reserve(selection.selected().size());
    for (const ObjectId actor : selection.selected()) {
        if (!world.isControlledLiveObject(localPlayer, actor)) continue;
        const bool admitted = request.guardPosition
            ? world.actorCanAttack(actor) && world.actorCanMove(actor)
            : enter ? world.actorCanEnterContainer(
                  actor, request.targetObject)
            : attack ? (!request.targetObject
                ? world.actorCanAttack(actor)
                : request.forceAttack
                    ? world.actorCanForceAttackTarget(
                          actor, request.targetObject)
                    : world.actorCanAttackTarget(
                          actor, request.targetObject))
                     : world.actorCanMove(actor);
        if (admitted) {
            command.actors.push_back(actor);
        }
    }
    if (enter) {
        if (command.actors.empty()) {
            result.rejection =
                WorldCommandComposeRejection::NoControllableActors;
            result.message =
                "selection contains no movable passenger for containment entry";
            return result;
        }
        std::sort(command.actors.begin(), command.actors.end());
        command.actors.erase(
            std::unique(command.actors.begin(), command.actors.end()),
            command.actors.end());
        result.controllableActorCount = command.actors.size();
        result.command = std::move(command);
        // A containment order prefers the garrison/hostile-entry cue when the
        // destination is a building rather than a transport, matching
        // CommandXlat's VoiceGarrison / VoiceEnterHostile branches. Both live
        // only inside UnitSpecificSounds, so they are reached through
        // perUnitSound() and fall back to VoiceEnter when unauthored.
        attachContainmentVoice(session, localPlayer, result);
        return result;
    }
    result = finishCommand(std::move(command), std::move(result));
    attachOrderVoice(session, result,
                     forceMove ? request.targetObject : INVALID_OBJECT_ID);
    return result;
}

} // namespace engine::selection
