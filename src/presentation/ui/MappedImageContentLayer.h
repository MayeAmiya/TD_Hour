#pragma once

#include "core/container/container_types.h"

namespace engine::ui {

// Immutable map.ini/solo.ini winner frozen with the GameSession. Parsing and
// activation remain main-thread presentation work; no Image, RawTexture or
// renderer handle crosses the logic/UI boundary.
struct MappedImageContentLayer final {
    container::String sourcePath;
    container::String content;
};

} // namespace engine::ui
