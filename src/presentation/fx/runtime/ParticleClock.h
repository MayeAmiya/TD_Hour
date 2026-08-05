#pragma once

#include <cstdint>

namespace engine::fx {

// ParticleSystem authoring is expressed in the original fixed 30 Hz domain.
// A session may run a different confirmed tick rate, but that is an input to
// an exact rational mapping and never changes this authored visual contract.
inline constexpr uint32_t kParticleAuthoredFramesPerSecond = 30;
static_assert(kParticleAuthoredFramesPerSecond != 0);

} // namespace engine::fx
