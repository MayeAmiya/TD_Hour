#pragma once

#include "core/container/container_types.h"
#include "game/selection/LocalSelectionState.h"
#include "game/selection/PendingWorldCommandMode.h"
#include "game/command/GameCommand.h"

#include <cstddef>
#include <optional>
namespace engine {
class GameSession;
}
namespace engine::selection {

// Explicit local UI/world intent. It intentionally mirrors only the typed
// public GameCommand shape; it never captures current selection implicitly.
struct WorldCommandRequest final {
    GameTick tick = 0;
    uint32_t sequence = 0;
    GameCommandType type = GameCommandType::None;
    ObjectId targetObject = INVALID_OBJECT_ID;
    CommandPosition targetPosition;
    container::String commandName;
    bool queued = false;
};

// A normal world click is not a command family by itself.  The authoritative
// logic view resolves it to Attack for an eligible hostile object or Move for
// terrain, then emits the same typed GameCommand used by replay/lockstep.
struct ContextualWorldCommandRequest final {
    GameTick tick = 0;
    uint32_t sequence = 0;
    ObjectId targetObject = INVALID_OBJECT_ID;
    CommandPosition targetPosition;
    bool queued = false;
    bool forceAttack = false;
    // A ForceMove click intentionally ignores an object context and composes
    // an ordinary Move to the clicked terrain point.  It is presentation
    // input only, like the local speech response it selects.
    bool forceMove = false;
    bool attackMove = false;
    bool guardPosition = false;
};

enum class WorldCommandComposeRejection : uint8_t {
    None,
    InvalidLocalPlayer,
    EmptySelection,
    NoControllableActors,
    MalformedCommand,
};

struct WorldCommandComposeResult final {
    std::optional<GameCommand> command;
    WorldCommandComposeRejection rejection = WorldCommandComposeRejection::None;
    size_t selectedActorCount = 0;
    size_t controllableActorCount = 0;
    container::String message;
    // Per-client order acknowledgement ("yes sir", "moving out"). It is
    // deliberately NOT part of the GameCommand: the command is the lockstep
    // and replay payload, so a voice riding inside it would make audio a
    // simulation input. The caller plays this locally after submitting.
    // Empty means the acknowledging unit authored no cue for this order.
    container::String voiceEventName;
    ObjectId voiceObject = INVALID_OBJECT_ID;

    [[nodiscard]] explicit operator bool() const noexcept { return command.has_value(); }
};

[[nodiscard]] bool pendingWorldTargetRelationAllowed(
    const PendingWorldCommandMode& mode, const GameSession& session,
    PlayerId localPlayer, ObjectId target) noexcept;

// Compiles a local view selection into a self-contained lockstep/replay-safe
// GameCommand. Hostile/neutral selections remain viewable but are filtered
// here; OrderExecutor validates ownership again at the authoritative frame.
class WorldCommandComposer final {
public:
    [[nodiscard]] static WorldCommandComposeResult compose(
        const GameSession& session, const LocalSelectionState& selection,
        PlayerId localPlayer, WorldCommandRequest request);

    [[nodiscard]] static WorldCommandComposeResult composeContextual(
        const GameSession& session, const LocalSelectionState& selection,
        PlayerId localPlayer,
        ContextualWorldCommandRequest request);
};

} // namespace engine::selection
