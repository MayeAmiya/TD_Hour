#pragma once

#include "core/container/container_types.h"

#include "game/script/bridge/ScriptSessionEvents.h"

#include <cstddef>
#include <cstdint>
#include <optional>
namespace engine::script {

// Confirmed script presentation is deliberately kept out of GameSession's
// deterministic state.  This small consumer owns only the UI-facing lifetime
// and replacement rules after GameSession has stamped and released a batch of
// ScriptSessionEvent values.  It has no renderer, ECS, clock, or localization
// dependency, which makes it safe to drive from the presentation thread and
// easy to exercise without a running game session.
struct ScriptPresentationMessage final {
    ScriptSessionEvent event;
    uint64_t fadeBeginTick = 0;
    uint64_t expireTick = 0;
};

struct ScriptPresentationOverlay final {
    ScriptSessionEvent event;
    uint64_t expireTick = 0;
};

class ScriptPresentationState final {
public:
    // RefCode's InGameUI stores six ordinary messages.  Preserve that bounded
    // behavior instead of allowing a fast repeating script to grow a client
    // queue indefinitely while the UI is covered by an overlay.
    static constexpr size_t kMaximumMessages = 6;

    void clear() noexcept;

    // Events must be supplied in the order GameSession released them.  The
    // state intentionally does not sort: source-order effects in one confirmed
    // tick are observable (a later cinematic command replaces an earlier one).
    void consume(container::Vector<ScriptSessionEvent> events,
                 uint64_t currentConfirmedTick,
                 uint32_t ordinaryMessageLifetimeTicks,
                 uint32_t ordinaryMessageFadeTicks);
    void advance(uint64_t currentConfirmedTick) noexcept;

    [[nodiscard]] container::Span<const ScriptPresentationMessage> messages() const noexcept {
        return m_messages;
    }
    [[nodiscard]] const std::optional<ScriptPresentationOverlay>& cinematic() const noexcept {
        return m_cinematic;
    }
    [[nodiscard]] const std::optional<ScriptPresentationOverlay>& subtitle() const noexcept {
        return m_subtitle;
    }

    // The opacity function is deterministic presentation math.  It lets a UI
    // backend choose its own draw primitives while retaining the original
    // fade-after-timeout behavior without a wall-clock timer.
    [[nodiscard]] static uint8_t messageOpacity(const ScriptPresentationMessage& message,
                                                uint64_t currentConfirmedTick) noexcept;

private:
    static uint64_t saturatedAdd(uint64_t value, uint64_t delta) noexcept;

    container::Vector<ScriptPresentationMessage> m_messages;
    std::optional<ScriptPresentationOverlay> m_cinematic;
    std::optional<ScriptPresentationOverlay> m_subtitle;
};

} // namespace engine::script
