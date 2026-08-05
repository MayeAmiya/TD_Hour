#pragma once

#include "core/container/hash_containers.h"

#include "GameCommand.h"
#include "game/base/GameSettings.h"
#include <cstddef>
#include <optional>
namespace engine {

struct ConfirmedCommandFrame {
    GameTick tick = 0;
    container::Array<uint16_t, MAX_SLOTS> commandCounts{};
    bool includesLocalCommands = false;
    container::Vector<uint32_t> acceptedLocalSequences;
    container::Vector<GameCommand> commands;
    // Local-only diagnostic metadata. It is never encoded into a network
    // confirmed frame; remote frames are already exact-tick authoritative.
    size_t expiredCommandCount = 0;
    size_t rejectedCommandCount = 0;
    // Local-only exact values used to terminate ControlBar receipts. They are
    // never encoded in a confirmed network frame.
    container::Vector<GameCommand> expiredLocalCommands;
};

struct LocalCommandFrame {
    GameTick tick = 0;
    container::Vector<GameCommand> commands;
};

enum class LockstepLocalSubmitRejection : uint8_t {
    None,
    InvalidAuthority,
    TickOverflow,
    FrameSealed,
    SequenceExhausted,
    CapacityExceeded,
};

struct LockstepLocalSubmitResult final {
    std::optional<GameCommand> command;
    LockstepLocalSubmitRejection rejection =
        LockstepLocalSubmitRejection::None;
};

class LockstepFrameBuffer {
public:
    // Wire decoding uses the same per-frame command ceiling.  Keep it
    // explicit at the in-memory boundary as well, because focused tools and
    // future transports can construct ConfirmedCommandFrame directly.
    static constexpr size_t MaximumCommandsPerFrame = 4'096;
    // These are ingress budgets, not gameplay limits.  They intentionally
    // allow generous jitter/replay windows while bounding malformed or stale
    // remote input that would otherwise remain retained forever.
    static constexpr size_t MaximumPendingLocalCommands = 16'384;
    static constexpr size_t MaximumPendingConfirmedFrames = 4'096;
    static constexpr size_t MaximumPendingConfirmedEntries = 16'384;

    void configure(PlayerId localPlayer, GameTick frameSendRate);
    void setFrameSendRate(GameTick frameSendRate) { m_frameSendRate = frameSendRate; }
    void reset();

    std::optional<GameCommand> submitLocal(GameCommand command, GameTick currentLogicTick);
    [[nodiscard]] LockstepLocalSubmitResult submitLocalResolved(
        GameCommand command, GameTick currentLogicTick);
    container::Vector<LocalCommandFrame> sealLocalFramesThrough(GameTick tick);
    // Reject malformed/conflicting frames at ingress. In particular a
    // `(player, sequence)` key must be unique within a confirmed frame so
    // ordering never depends on a non-total sort comparator.
    bool receiveConfirmedFrame(ConfirmedCommandFrame frame);
    bool takeReadyFrame(
        GameTick tick, container::Vector<GameCommand>& commands,
        container::Vector<GameCommand>* rejectedLocalCommands = nullptr);

    GameTick frameSendRate() const { return m_frameSendRate; }
    size_t pendingFrameCount() const { return m_confirmedFrames.size(); }

private:
    [[nodiscard]] static bool validateFrameCommandKeys(
        const container::Vector<GameCommand>& commands, GameTick tick);
    [[nodiscard]] static bool validateAcceptedSequences(
        const container::Vector<uint32_t>& sequences);
    [[nodiscard]] bool validateConfirmedFrame(const ConfirmedCommandFrame& frame) const;
    [[nodiscard]] static size_t frameEntryCount(const ConfirmedCommandFrame& frame) noexcept;
    bool assembleFrame(
        const ConfirmedCommandFrame& frame,
        container::Vector<GameCommand>& commands,
        container::Vector<GameCommand>* rejectedLocalCommands) const;

    PlayerId m_localPlayer = INVALID_PLAYER_ID;
    GameTick m_frameSendRate = 0;
    uint32_t m_nextLocalSequence = 1;
    GameTick m_nextFrameToSeal = FirstConfirmedGameTick;
    size_t m_pendingLocalCommandCount = 0;
    size_t m_pendingConfirmedEntryCount = 0;
    std::optional<GameTick> m_lastConsumedConfirmedTick;
    container::HashMap<GameTick, container::Vector<GameCommand>> m_localPending;
    container::HashMap<GameTick, ConfirmedCommandFrame> m_confirmedFrames;
};

} // namespace engine
