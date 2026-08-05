#pragma once

#include "game/navigation/runtime/NavigationSystem.h"

namespace engine::session_frame_detail {

inline constexpr navigation::NavigationSystemBudgets NavigationTickBudgets{
    128,
    4096,
    // A 385x530 campaign grid contains ~204k cells. Five thousand expansions
    // made an otherwise ordinary long click wait tens of 30 Hz logic frames.
    // Keep the search bounded, but finish typical cross-screen routes within
    // one or two confirmed ticks in Release.
    50000,
};

} // namespace engine::session_frame_detail
