#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace engine::script {

enum class ScriptCommandBarOverrideCommand : uint8_t {
    RemoveButtonFromObjectType,
    AddButtonToObjectTypeSlot,
};

struct ScriptCommandBarOverrideAction final {
    ScriptCommandBarOverrideCommand command =
        ScriptCommandBarOverrideCommand::RemoveButtonFromObjectType;
    container::String commandButtonName;
    container::String objectTypeName;
    // Legacy authoring is one-based.  Invalid signed values remain harmless
    // no-ops at the confirmed session boundary.
    int32_t oneBasedSlot = 0;
};

struct ScriptCommandBarOverrideEffect final {
    ScriptCommandBarOverrideCommand command =
        ScriptCommandBarOverrideCommand::RemoveButtonFromObjectType;
    container::String commandButtonName;
    container::String objectTypeName;
    int32_t oneBasedSlot = 0;
};

} // namespace engine::script
