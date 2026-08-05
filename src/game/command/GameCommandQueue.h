#pragma once

#include "core/container/hash_containers.h"

#include "GameCommand.h"

#include <cstdint>
#include <optional>
namespace engine {

enum class GameCommandQueueRejection : uint8_t {
    None,
    CapacityExceeded,
    DuplicateSequence,
    SequenceExhausted,
};

struct GameCommandQueueSubmitResult final {
    std::optional<GameCommand> command;
    GameCommandQueueRejection rejection = GameCommandQueueRejection::None;

    [[nodiscard]] explicit operator bool() const noexcept {
        return command.has_value();
    }
};

class GameCommandQueue {
public:
    // This is an in-process ingress guard, not a gameplay selection limit.
    // It is intentionally far above a normal frame's command volume while
    // still preventing a malformed replay/UI producer from retaining an
    // unbounded amount of value payload before confirmation catches up.
    static constexpr size_t MaximumPendingCommands = 16'384;

    void clear();
    // A confirmed frame has one total order per `(player, sequence)` pair.
    // Duplicate explicit keys are rejected rather than leaving an equivalent
    // comparator pair for std::sort to reorder arbitrarily. A zero sequence
    // is local ingress shorthand and is assigned a collision-free value.
    bool submit(GameCommand command);
    [[nodiscard]] GameCommandQueueSubmitResult submitResolved(
        GameCommand command);
    container::Vector<GameCommand> drainForTick(GameTick tick);
    // Commands for an already-confirmed tick are never executed late. The
    // caller can surface this count as an ingress/recording diagnostic rather
    // than silently shifting simulation input into a later frame.
    [[nodiscard]] size_t takeExpiredCount() noexcept;
    [[nodiscard]] container::Vector<GameCommand> takeExpiredCommands();
    // Duplicate/sequencing rejection is a separate ingress diagnostic from
    // an expired command: rejected commands never become pending input.
    [[nodiscard]] size_t takeRejectedCount() noexcept;
    bool empty() const { return m_pending.empty(); }
    size_t size() const { return m_pending.size(); }

private:
    // The vector is the owned, sortable payload. This side index is only an
    // ingress membership check; no observable ordering is ever taken from
    // the hash table.
    struct PendingCommandKey final {
        GameTick tick = 0;
        uint8_t player = 0xff;
        uint32_t sequence = 0;

        [[nodiscard]] constexpr bool operator==(const PendingCommandKey&) const noexcept = default;
    };

    struct PendingCommandKeyHash final {
        [[nodiscard]] size_t operator()(const PendingCommandKey& key) const noexcept;
    };

    [[nodiscard]] static PendingCommandKey keyFor(const GameCommand& command) noexcept;
    [[nodiscard]] bool hasPendingSequence(GameTick tick, PlayerId player,
                                          uint32_t sequence) const noexcept;
    [[nodiscard]] uint32_t allocateSequence(GameTick tick, PlayerId player) noexcept;

    container::Vector<GameCommand> m_pending;
    container::HashSet<PendingCommandKey, PendingCommandKeyHash> m_pendingKeys;
    uint32_t m_nextSequence = 1;
    size_t m_expiredSinceLastDrain = 0;
    container::Vector<GameCommand> m_expiredCommands;
    size_t m_rejectedSinceLastDrain = 0;
};

} // namespace engine
