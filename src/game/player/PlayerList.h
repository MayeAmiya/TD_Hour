#pragma once

#include "PlayerRegistry.h"

namespace engine {

// Source-compatible transition name.  New code should use PlayerRegistry;
// the implementation no longer has RefCode's process-global/list semantics.
using PlayerList = PlayerRegistry;

} // namespace engine
