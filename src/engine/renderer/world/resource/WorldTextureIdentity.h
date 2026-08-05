#pragma once

#include "core/container/container_types.h"

#include <algorithm>
#include <cctype>

namespace engine::render::detail {

[[nodiscard]] inline container::String canonicalWorldTextureIdentity(
    container::StringView textureName) {
    container::String identity(textureName);
    std::replace(identity.begin(), identity.end(), '\\', '/');
    std::transform(
        identity.begin(), identity.end(), identity.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return identity;
}

} // namespace engine::render::detail
