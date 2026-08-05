#pragma once

#include <cstdint>

namespace engine {

// Shared authored/runtime bound. The Plan compiler warns beyond this value;
// the simulation applies the hard bound at the consumption boundary.
inline constexpr int64_t kMaximumExperienceScalarInteger = 32768;

} // namespace engine
