#pragma once

#include "core/container/container_types.h"
#include "game/command/CommandButtonTypes.h"
#include "game/command/GameCommand.h"

#include <cstddef>
#include <cstdint>

namespace engine {

enum class CommandOutcomeState : uint8_t {
    PendingConfirmation,
    Accepted,
    Rejected,
};

enum class CommandOutcomeReason : uint8_t {
    None,
    StaleSession,
    InvalidActionToken,
    StaleSelection,
    SelectionMismatch,
    InvalidSlot,
    SlotUnavailable,
    DescriptorChanged,
    AvailabilityChanged,
    SingleUseConsumed,
    ScienceUnavailable,
    QueueChanged,
    RouterInvalidDescriptor,
    RouterUnavailable,
    RouterInvalidSelection,
    RouterInvalidLocalPlayer,
    RouterUnauthorizedActor,
    RouterMissingRoutePayload,
    RouterCompositionRejected,
    UnsupportedBackend,
    MissingCommandPayload,
    LocalPresentationRejected,
    CancelledByUser,
    CancelledBySelectionChange,
    SupersededByLocalMode,
    SourceBecameUnavailable,
    GameNotRunning,
    NetworkNotReady,
    MissingLocalCommandPlayer,
    QueueCapacityExceeded,
    DuplicateSequence,
    FrameSealed,
    ExpiredBeforeConfirmation,
    AuthorityRejected,
    DispatcherMalformedPayload,
    DispatcherUnsupported,
    DispatcherRejected,
    BackendRejected,
    BackendTimedOut,
};

enum class CommandBackendKind : uint8_t {
    SpecialPower,
    CombatDrop,
};

// Confirmed simulation-to-local-UI handoff for command families whose
// Dispatcher acceptance only queues asynchronous work.
struct CommandBackendOutcome final {
    PlayerId player = INVALID_PLAYER_ID;
    ObjectId source = INVALID_OBJECT_ID;
    uint32_t sourceSequence = 0;
    CommandBackendKind kind = CommandBackendKind::SpecialPower;
    bool accepted = false;
    GameTick confirmedTick = 0;
};

enum class CommandVoiceDisposition : uint8_t {
    None,
    AwaitConfirmation,
    Accepted,
    Rejected,
    SuppressedUnsupported,
};

enum class CommandCursorDisposition : uint8_t {
    Unchanged,
    AwaitConfirmation,
    Accepted,
    Rejected,
    Unsupported,
};

// Pointer-free, Release-visible receipt. A request may first publish Pending
// and later publish one terminal value with the same requestSequence.
struct CommandOutcome final {
    uint64_t requestSequence = 0;
    uint32_t commandSequence = 0;
    uint64_t buttonStableId = 0;
    game::CommandButtonKind commandKind = game::CommandButtonKind::Unknown;
    CommandOutcomeState state = CommandOutcomeState::Rejected;
    CommandOutcomeReason reason = CommandOutcomeReason::None;
    CommandVoiceDisposition voice = CommandVoiceDisposition::None;
    CommandCursorDisposition cursor = CommandCursorDisposition::Unchanged;
    GameTick confirmedTick = 0;
    uint64_t revision = 0;

    [[nodiscard]] bool terminal() const noexcept {
        return state != CommandOutcomeState::PendingConfirmation;
    }
};

struct CommandOutcomeProjection final {
    uint64_t revision = 0;
    // UI advances this watermark only after consuming the matching ordered
    // prefix. Discrete receipts must not be overwritten by a latest-value
    // projection while the main thread is stalled.
    uint64_t acknowledgedRevision = 0;
    CommandOutcome latest;
    container::Vector<CommandOutcome> records;
};

} // namespace engine
