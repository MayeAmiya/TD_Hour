#pragma once

#include "app/input/CommandMapRuntime.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace app::input {

class CommandModeInputState final {
public:
    [[nodiscard]] bool applyHeldAction(
        CommandMapAction action, bool pressed) noexcept {
        switch (action) {
        case CommandMapAction::BeginForceAttack:
        case CommandMapAction::EndForceAttack:
            m_forceAttack = pressed;
            return true;
        case CommandMapAction::BeginForceMove:
        case CommandMapAction::EndForceMove:
            m_forceMove = pressed;
            return true;
        case CommandMapAction::BeginWaypoints:
        case CommandMapAction::EndWaypoints:
            m_waypoints = pressed;
            return true;
        case CommandMapAction::BeginPreferSelection:
        case CommandMapAction::EndPreferSelection:
            m_preferSelection = pressed;
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool recallFocus(
        size_t group, uint64_t timestampMilliseconds,
        uint64_t sessionRevision,
        uint64_t intervalMilliseconds = 500) noexcept {
        const bool focus = m_lastRecallSessionRevision == sessionRevision &&
            m_lastRecalledGroup == group &&
            timestampMilliseconds >= m_lastRecallMilliseconds &&
            timestampMilliseconds - m_lastRecallMilliseconds <=
                intervalMilliseconds;
        m_lastRecalledGroup = group;
        m_lastRecallMilliseconds = timestampMilliseconds;
        m_lastRecallSessionRevision = sessionRevision;
        return focus;
    }

    void reset() noexcept {
        m_forceAttack = false;
        m_forceMove = false;
        m_waypoints = false;
        m_preferSelection = false;
        m_lastRecalledGroup = std::numeric_limits<size_t>::max();
        m_lastRecallMilliseconds = 0;
        m_lastRecallSessionRevision = 0;
    }

    [[nodiscard]] bool forceAttack() const noexcept { return m_forceAttack; }
    [[nodiscard]] bool forceMove() const noexcept { return m_forceMove; }
    [[nodiscard]] bool waypoints() const noexcept { return m_waypoints; }
    // World-order queuing accepts both the authored waypoint modifier and
    // Shift's PreferSelection modifier. Shift remains additive selection over
    // objects and repeated input over WND, while a world target appends to the
    // deterministic order queue.
    [[nodiscard]] bool queueOrders() const noexcept {
        return m_waypoints || m_preferSelection;
    }
    [[nodiscard]] bool preferSelection() const noexcept {
        return m_preferSelection;
    }

private:
    bool m_forceAttack = false;
    bool m_forceMove = false;
    bool m_waypoints = false;
    bool m_preferSelection = false;
    size_t m_lastRecalledGroup = std::numeric_limits<size_t>::max();
    uint64_t m_lastRecallMilliseconds = 0;
    uint64_t m_lastRecallSessionRevision = 0;
};

} // namespace app::input
