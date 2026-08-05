#pragma once

#include "core/container/container_types.h"

namespace engine::audio {

// Immutable VFS winner frozen by GameSession at bootstrap. The audio owner
// parses these value layers only when the matching presentation epoch becomes
// active; no VFS lookup or GameSession pointer crosses that boundary.
struct AudioContentLayer final {
    container::String sourcePath;
    container::String content;
};

} // namespace engine::audio
