#include "game/object/contracts/ObjectDisabledTypes.h"

#include "core/container/string_utils.h"

#include <algorithm>
#include <utility>

namespace engine {

std::optional<ObjectDisabledReason>
objectDisabledReasonFromLegacyToken(container::StringView token) noexcept {
    constexpr container::StringView prefix = "DISABLED_";
    constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;
    if (token.size() > prefix.size() &&
        equalInsensitive(token.substr(0, prefix.size()), prefix)) {
        token.remove_prefix(prefix.size());
    }
    constexpr container::Array<
        std::pair<container::StringView, ObjectDisabledReason>,
        static_cast<size_t>(ObjectDisabledReason::Count)> names{{
        {"DEFAULT", ObjectDisabledReason::Default},
        {"HACKED", ObjectDisabledReason::Hacked},
        {"EMP", ObjectDisabledReason::Emp},
        {"HELD", ObjectDisabledReason::Held},
        {"PARALYZED", ObjectDisabledReason::Paralyzed},
        {"UNMANNED", ObjectDisabledReason::Unmanned},
        {"UNDERPOWERED", ObjectDisabledReason::Underpowered},
        {"FREEFALL", ObjectDisabledReason::Freefall},
        {"AWESTRUCK", ObjectDisabledReason::Awestruck},
        {"BRAINWASHED", ObjectDisabledReason::Brainwashed},
        {"SUBDUED", ObjectDisabledReason::Subdued},
        {"SCRIPT_DISABLED", ObjectDisabledReason::ScriptDisabled},
        {"SCRIPT_UNDERPOWERED", ObjectDisabledReason::ScriptUnderpowered},
    }};
    const auto found = std::find_if(
        names.begin(), names.end(),
        [&](const auto& entry) {
            return container::asciiEqualIgnoreCase(token, entry.first);
        });
    return found == names.end()
        ? std::nullopt
        : std::optional<ObjectDisabledReason>{found->second};
}

} // namespace engine
