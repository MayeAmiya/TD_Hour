#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

#include "game/player/PlayerList.h"
#include "game/command/CommandButtonTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
namespace engine {

using GameTick = uint32_t;
// Tick zero is the unassigned/default sentinel throughout command ingress and
// several gameplay services. The first executable confirmed frame is one.
inline constexpr GameTick FirstConfirmedGameTick = 1;

enum class CommandSource : uint8_t {
    Local,
    Network,
    Replay,
};

enum class GameCommandType : uint8_t {
    None,
    UIAction,
    CommandButton,
    Move,
    Attack,
    Build,
    SpecialPower,
    Pause,
    Surrender,
    // Stop is an actor command rather than a UI-only command button.  It
    // clears the selected actors' deterministic order queues immediately.
    Stop,
    // Factory transactions are deliberately separate from ObjectOrderQueue:
    // each targets exactly one producer and changes its authoritative
    // production state rather than directing a mobile actor.
    QueueProduction,
    CancelProduction,
    // PLAYER upgrades share a factory FIFO with units but resolve through a
    // frozen UpgradeCatalog and are cancelled by their exact authored name,
    // matching ProductionUpdate's upgrade-specific legacy operation.
    QueuePlayerUpgrade,
    CancelPlayerUpgrade,
    SetFactoryRallyPoint,
    // Explicit attack-move intent.  It shares Move's destination payload but
    // remains distinct on the lockstep wire and in ObjectOrderIntent so
    // autonomous combat cannot infer it from an ordinary ground position.
    AttackMove,
    // Retail MSG_SET_BEACON_TEXT is player-authored lockstep input.  Actors
    // are the explicit selected beacon group and commandName carries the
    // already language-filtered UTF-8 caption (empty clears it).
    SetBeaconText,
    // Retail MSG_DO_REPAIR: the actors are the explicit selected group and
    // targetObject is the damaged structure.  This is a dedicated command
    // family because it starts Dozer/Worker gameplay rather than a generic
    // attack/move order or an authored CommandButton transaction.
    Repair,
    // Confirmed ControlBar transactions. Sell starts the authored delayed
    // sale lifecycle; CancelConstruction removes exactly one owned
    // under-construction structure and refunds its current full build cost
    // unless it is a reconstructing hole replacement.
    Sell,
    CancelConstruction,
    // Confirmed containment transactions. ExitContainer carries the one
    // selected container in actors and the clicked inventory passenger in
    // targetObject; Evacuate carries the canonical selected container group.
    ExitContainer,
    Evacuate,
    ExecuteRailedTransport,
    // Contextual right-click entry. actors is the canonical controlled
    // passenger group and targetObject is the transport/garrison/container.
    EnterContainer,
    // Typed player-facing Chinook insertion command. commandName retains the
    // authored button identity for presentation/content provenance only;
    // simulation routing never infers behavior from its text.
    CombatDrop,
    // Retail MSG_PURCHASE_SCIENCE is a player transaction with no object
    // actor. commandName carries the exact frozen Science identity; the
    // confirmed dispatcher revalidates points, prerequisites and script
    // availability before spending anything.
    PurchaseScience,
    // Retail MSG_DO_SCATTER. The selected actor group is the entire payload;
    // every peer derives the same fixed radial destinations from confirmed
    // transforms and geometry.
    Scatter,
    CreateFormation,
    // Player Guard is a first-class lockstep family. Distinct wire values
    // preserve the two ZH mode flags without string routing or an extra
    // loosely-typed integer payload.
    Guard,
    GuardWithoutPursuit,
    GuardFlyingUnitsOnly,
    // Immediate selected-group power-plant transaction.
    ToggleOvercharge,
    // Removes exactly one player-authored order node from one actor's
    // deterministic queue. productionId carries the nonzero sourceSequence;
    // the command never trusts a renderer waypoint identity or target pose.
    CancelOrderWaypoint,
};

struct CommandPosition {
    // Canonical lockstep/save/replay representation. Camera/WND hit testing
    // quantizes at command composition; no authoritative consumer may rebuild
    // these values from a float projection.
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    bool valid = false;
};

enum class CommandPostAcceptAction : uint8_t {
    None,
    MarkSingleUseCommandUsed,
};

// Deterministic, pointer-free origin metadata for a ControlBar command. It is
// part of the wire/replay envelope because a post-accept gameplay transition
// must run on every simulation peer, while UI outcomes are projected only for
// the locally controlled player.
struct CommandActivationContext final {
    uint64_t requestSequence = 0;
    uint64_t buttonStableId = 0;
    game::CommandButtonKind commandKind = game::CommandButtonKind::Unknown;
    CommandPostAcceptAction postAccept = CommandPostAcceptAction::None;
    ObjectId postAcceptActor = INVALID_OBJECT_ID;

    [[nodiscard]] bool present() const noexcept {
        return requestSequence != 0;
    }

    [[nodiscard]] bool hasPostAcceptAction() const noexcept {
        return postAccept != CommandPostAcceptAction::None;
    }

};

// External lockstep envelope. It contains player-authored intent only; script
// actions use the separate internal ScriptOrderIntent path and are never
// encoded into a network/replay command stream. The old single `subject` and
// opaque payload design has been removed: one order carries a canonical actor
// set and explicit fields understood by its executor.
struct GameCommand {
    GameTick tick = 0;
    uint32_t sequence = 0;
    PlayerId player = INVALID_PLAYER_ID;
    CommandSource source = CommandSource::Local;
    GameCommandType type = GameCommandType::None;
    container::Vector<ObjectId> actors;
    ObjectId targetObject = INVALID_OBJECT_ID;
    CommandPosition targetPosition;
    // Fixed world-space command orientation in radians. Build consumes it as the
    // placement yaw; SpecialPower preserves the command angle passed to OCL
    // creation. A valid end position is still Build-only and marks the
    // anchored LINEBUILD payload. These values are explicit lockstep input so
    // the authoritative session never derives orientation from a local
    // camera or trusts a client-generated tile list.
    math::q32_32 placementYawRadians{};
    CommandPosition placementEndPosition;
    bool queued = false;
    // Explicit player-authored force-attack intent.  This is part of the
    // lockstep/save/replay envelope; consumers must not reconstruct it from a
    // presentation-thread Ctrl state.  Only Attack accepts this flag.
    bool forceAttack = false;
    // For Build and QueueProduction this is the ThingTemplate name; for
    // QueuePlayerUpgrade/CancelPlayerUpgrade it is the exact-case Upgrade
    // catalog name; for CommandButton and SpecialPower it is the immutable
    // authored content name; PurchaseScience uses the exact-case Science
    // identity. SetBeaconText uses it as a bounded UTF-8 caption.
    // A later content snapshot replaces authored names with a frozen typed
    // handle at the authoritative session boundary.
    container::String commandName;
    // CancelProduction identifies a producer-local production job. Zero is
    // reserved/invalid; all other command families leave this at zero.
    uint32_t productionId = 0;
    CommandActivationContext activation;
};

} // namespace engine
