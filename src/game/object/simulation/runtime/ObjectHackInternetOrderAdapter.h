#pragma once

#include "game/command/CommandButtonStore.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"

namespace engine
{

[[nodiscard]] inline bool isHackInternetCommandButton(
    ObjectOrderKind kind, container::StringView contentName,
    const game::CommandButtonTemplate* button) noexcept
{
    if (kind != ObjectOrderKind::CommandButton || contentName.empty() ||
        !button)
        return false;
    // Command kind is load-time parsed into descriptor.kind; do not re-parse
    // the raw Command string on the hot path.
    return button->descriptor.kind == game::CommandButtonKind::HackInternet;
}

} // namespace engine
