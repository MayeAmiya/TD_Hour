#pragma once

#include "core/container/container_types.h"

#include "container/ordered_id_set.h"
#include "game/selection/PendingWorldCommandMode.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include <cstddef>
#include <cstdint>
namespace engine::selection {

inline constexpr size_t LOCAL_CONTROL_GROUP_COUNT = 10;

enum class LocalOrderWaypointKind : uint8_t {
    Move,
    Attack,
    AttackMove,
    Build,
    Guard,
    Ability,
};

struct LocalOrderWaypointSelection final {
    ObjectId actor = INVALID_OBJECT_ID;
    uint32_t sourceSequence = 0;
    LocalOrderWaypointKind kind = LocalOrderWaypointKind::Move;

    [[nodiscard]] explicit operator bool() const noexcept {
        return actor && sourceSequence != 0;
    }

    friend bool operator==(const LocalOrderWaypointSelection&,
                           const LocalOrderWaypointSelection&) = default;
};

// Per-client presentation state for object selection and control groups.
// This deliberately belongs neither to PlayerRegistry nor to ECS: a local
// user may inspect an enemy/neutral object, while only WorldCommandComposer
// decides which selected IDs are legal command actors.  Each set is ascending
// ObjectId so input order cannot leak into a later command or overlay.
class LocalSelectionState final {
public:
    [[nodiscard]] bool replace(container::Span<const ObjectId> objects);
    [[nodiscard]] bool add(ObjectId object);
    [[nodiscard]] bool remove(ObjectId object);
    [[nodiscard]] bool toggle(ObjectId object);
    [[nodiscard]] bool contains(ObjectId object) const;
    [[nodiscard]] bool clear();
    // Clears only the currently interactive selection state.  Scripted input
    // suppression and beacon cancellation must preserve the player's 0-9
    // control groups; full reset() is reserved for session retirement.
    [[nodiscard]] bool clearTransientInteraction() noexcept;

    [[nodiscard]] bool selectOrderWaypoint(
        LocalOrderWaypointSelection waypoint);
    [[nodiscard]] const LocalOrderWaypointSelection& selectedOrderWaypoint()
        const noexcept { return m_selectedOrderWaypoint; }

    // One client-local rollover object. This is presentation input only: it
    // never enters control groups, command composition, replay, or lockstep.
    [[nodiscard]] bool setHovered(ObjectId object);
    [[nodiscard]] ObjectId hovered() const noexcept { return m_hovered; }

    [[nodiscard]] container::Span<const ObjectId> selected() const noexcept { return m_selected.values(); }
    [[nodiscard]] uint64_t revision() const noexcept { return m_revision; }

    [[nodiscard]] bool beginPendingWorldCommand(
        PendingWorldCommandMode mode);
    [[nodiscard]] bool cancelPendingWorldCommand(
        uint64_t expectedRevision = 0) noexcept;
    [[nodiscard]] const PendingWorldCommandMode& pendingWorldCommand()
        const noexcept {
        return m_pendingWorldCommand;
    }

    [[nodiscard]] bool saveControlGroup(size_t index);
    [[nodiscard]] bool saveControlGroup(
        size_t index, container::Span<const ObjectId> objects);
    [[nodiscard]] bool appendToControlGroup(size_t index);
    [[nodiscard]] bool appendToControlGroup(
        size_t index, container::Span<const ObjectId> objects);
    [[nodiscard]] bool recallControlGroup(size_t index);
    [[nodiscard]] container::Span<const ObjectId> controlGroup(size_t index) const noexcept;

    // Lifecycle consumption is optional because callers may instead prune
    // against ObjectLifecycle each presentation frame. Both paths remove
    // only unavailable objects; ownership changes do not erase an inspected
    // enemy from local view state.
    [[nodiscard]] bool applyLifecycleEvents(container::Span<const ObjectLifecycleEvent> events);
    [[nodiscard]] bool pruneUnavailable(const ObjectLifecycle& lifecycle);
    [[nodiscard]] bool removeObjects(
        container::Span<const ObjectId> objects);

    void reset() noexcept;

private:
    [[nodiscard]] static bool equal(container::Span<const ObjectId> lhs,
                                    container::Span<const ObjectId> rhs) noexcept;
    [[nodiscard]] bool replaceSet(container::OrderedIdSet<ObjectId>& destination,
                                  container::Span<const ObjectId> objects);
    [[nodiscard]] bool removeFromAll(ObjectId object);
    void bumpRevision() noexcept;

    container::OrderedIdSet<ObjectId> m_selected;
    LocalOrderWaypointSelection m_selectedOrderWaypoint;
    container::Array<container::OrderedIdSet<ObjectId>, LOCAL_CONTROL_GROUP_COUNT> m_controlGroups;
    ObjectId m_hovered = INVALID_OBJECT_ID;
    uint64_t m_revision = 0;
    uint64_t m_nextPendingWorldCommandRevision = 1;
    PendingWorldCommandMode m_pendingWorldCommand;
};

} // namespace engine::selection
