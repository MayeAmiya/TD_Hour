#pragma once

#include "container/container_types.h"

#include <cstdint>

namespace engine {

enum class LaunchProduct : uint8_t {
    ZeroHour,
};

// Process-start content authority supplied by the external launcher.  All
// paths are normalized absolute Windows paths before this value is published;
// consumers must never complete them from the process working directory.
struct LaunchContentContract {
    // GeneralsTD is an in-game Zero Hour engine, mirroring RefCode's
    // compile-time RTS_ZEROHOUR product boundary. The launcher must state the
    // product explicitly; consumers never infer it from a map name or cwd.
    LaunchProduct product = LaunchProduct::ZeroHour;
    container::String contentRoot;
    container::String baseContentRoot;
    // Optional external Mod path. It may name a directory containing BIG
    // files or one explicit BIG file, matching RefCode -mod. Mod archives are
    // mounted above product archives but below active ZH loose files.
    container::String modRoot;
    container::String userRoot;
    container::String locale;

    [[nodiscard]] bool complete() const noexcept {
        return !contentRoot.empty() && !baseContentRoot.empty() &&
            !userRoot.empty() && !locale.empty();
    }
};

} // namespace engine
