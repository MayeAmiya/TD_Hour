#pragma once

#include "core/container/container_types.h"

#include "game/selection/LocalSelectionState.h"

#include <optional>
namespace engine {
class GameSession;
}

namespace engine::selection {

struct LocalCommandBarSelection final {
    ObjectId object = INVALID_OBJECT_ID;
    container::String objectType;
};

// Converts an exact-one local presentation selection into the immutable
// ObjectType identity consumed by the reconstructed ControlBar.  This is a
// read-only, main-thread presentation bridge: it neither owns the selection
// nor retains an ECS entity/ObjectId after the call.
class LocalSelectionCommandBarPresentationConsumer final {
public:
    // Returns a value copy so the GUI never retains a component string or an
    // ECS object lifetime. Empty/multi selection, a pending/dead/nonvisual
    // object, or a malformed component/archetype relationship resolves to no
    // command bar selection.
    [[nodiscard]] static std::optional<container::String> resolveSingleObjectType(
        const GameSession& session, const LocalSelectionState& selection);
    // Production GUI path retains the stable ObjectId only in local
    // presentation state, allowing two instances of the same ObjectType to
    // expose different CommandSetUpgrade overrides without putting selection
    // into confirmed simulation.
    [[nodiscard]] static std::optional<LocalCommandBarSelection> resolveSingleObject(
        const GameSession& session, const LocalSelectionState& selection);
};

} // namespace engine::selection
