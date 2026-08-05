#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine {
class GameSession;
}

namespace engine::selection {

class LocalSelectionState;

enum class LocalSelectionGestureKind : uint8_t {
    Point,
    Rectangle,
    MatchingTypeVisible,
    MatchingTypeAcrossMap,
    ExplicitTypeAcrossMap,
};

struct LocalSelectionGesture final {
    LocalSelectionGestureKind kind = LocalSelectionGestureKind::Point;
    container::Vector<ObjectId> candidates;
    ObjectId anchor = INVALID_OBJECT_ID;
    container::String objectType;
    bool additive = false;
};

enum class LocalControlGroupOperation : uint8_t {
    Save,
    Append,
    Recall,
    View,
};

enum class LocalSelectionShortcut : uint8_t {
    NextUnit,
    PreviousUnit,
    NextWorker,
    PreviousWorker,
    NextIdleWorker,
    Hero,
    CommandCenter,
};

struct LocalControlGroupRequest final {
    size_t index = 0;
    LocalControlGroupOperation operation = LocalControlGroupOperation::Recall;
    bool focusCamera = false;
};

struct LocalSelectionPolicyResult final {
    bool accepted = false;
    bool changed = false;
    std::optional<math::vec3> cameraTarget;
};

// One authored selection-response cue plus the object that owns it. This is
// per-client presentation output: computing it reads the confirmed world and
// writes nothing, so it cannot perturb simulation state or the order in which
// SimulationRandom is consumed.
struct LocalUnitVoiceRequest final {
    container::String eventName;
    ObjectId object = INVALID_OBJECT_ID;

    [[nodiscard]] explicit operator bool() const noexcept {
        return !eventName.empty() && object != INVALID_OBJECT_ID;
    }
};

// Logic-thread policy for all local selection mutations. Presentation only
// supplies hit candidates; ownership, lifecycle, status, type and legal group
// composition are revalidated against the current authoritative session.
class LocalSelectionPolicy final {
public:
    // Revalidates an object already held by local presentation state against
    // the confirmed world. Control-group retention is stricter because a
    // group must never retain a dead or foreign object.
    [[nodiscard]] static bool isRetainedSelectionObject(
        const GameSession& session, ObjectId object,
        bool requireLocalLiveObject) noexcept;

    [[nodiscard]] static LocalSelectionPolicyResult applyGesture(
        const GameSession& session, LocalSelectionState& selection,
        LocalSelectionGesture gesture);

    [[nodiscard]] static LocalSelectionPolicyResult applyControlGroup(
        const GameSession& session, LocalSelectionState& selection,
        LocalControlGroupRequest request);
    [[nodiscard]] static LocalSelectionPolicyResult applyShortcut(
        const GameSession& session, LocalSelectionState& selection,
        LocalSelectionShortcut shortcut);

    // Chooses the single unit that answers for the current local selection and
    // returns its authored select cue, following RefCode's CommandXlat rule of
    // picking one speaker rather than letting every selected unit shout. The
    // speaker is the lowest ObjectId among locally controlled selected units,
    // which is stable for a given selection and needs no random draw, so this
    // never touches SimulationRandom.
    [[nodiscard]] static LocalUnitVoiceRequest selectionVoice(
        const GameSession& session, const LocalSelectionState& selection);

};

} // namespace engine::selection
