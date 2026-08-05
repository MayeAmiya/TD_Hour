#pragma once

#include "core/container/container_types.h"

namespace engine::ui {

// Immutable optional map.str source frozen with a candidate GameSession.
// Parsing and activation belong to the committed main-thread presentation
// epoch, so an uncommitted Next/Retry candidate cannot replace the old
// Result session's localized text.
struct MapStringContentLayer final {
    container::String sourcePath;
    container::String content;
};

} // namespace engine::ui
