#pragma once

#include "core/container/hash_containers.h"

#include "container/ordered_id_set.h"
#include "game/player/PlayerTypes.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include <cstddef>
#include <optional>
namespace engine {

// Match-owned, rebuildable projection of ObjectLifecycle ownership.  The
// ECS OwnerComponent remains the structural truth; this index exists so UI,
// selection, production, AI and defeat checks do not scan every entity just
// to answer "which live objects belong to player X?".  Each public iteration
// view is ascending ObjectId and therefore safe for deterministic simulation
// work.  The unordered map is lookup-only and must never define traversal.
class ObjectOwnershipIndex final {
public:
    void reset() noexcept;

    // Apply events in their lifecycle emission order. DestroyRequested
    // removes the ID immediately because ObjectLifecycle has already hidden
    // the pending entity from normal live-object lookup, and an OwnershipChanged
    // arriving in that pending window must not put the ID back.
    void apply(const ObjectLifecycleEvent& event);
    void apply(container::Span<const ObjectLifecycleEvent> events);

    [[nodiscard]] container::Span<const ObjectId> objects(PlayerId owner) const noexcept;
    [[nodiscard]] std::optional<PlayerId> ownerOf(ObjectId object) const noexcept;
    [[nodiscard]] bool contains(PlayerId owner, ObjectId object) const noexcept;
    [[nodiscard]] size_t objectCount() const noexcept { return m_ownerByObject.size(); }

private:
    [[nodiscard]] static bool validOwner(PlayerId owner) noexcept;
    void eraseObject(ObjectId object) noexcept;
    void assignObject(ObjectId object, PlayerId owner);

    container::Array<container::OrderedIdSet<ObjectId>, PLAYER_REGISTRY_CAPACITY> m_objectsByOwner;
    container::HashMap<ObjectId, PlayerId> m_ownerByObject;
    // IDs whose destroy was requested but whose Destroyed event has not landed
    // yet.  ObjectLifecycle::changeOwner(..., allowPendingDestroy) publishes
    // OwnershipChanged inside that deferred window, possibly a tick or more
    // before flushRequestedDestroys().  Lookup-only, like m_ownerByObject.
    container::HashSet<ObjectId> m_destroyRequested;
};

} // namespace engine
