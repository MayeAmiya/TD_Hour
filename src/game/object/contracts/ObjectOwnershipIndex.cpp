#include "core/container/container_types.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"

namespace engine {

void ObjectOwnershipIndex::reset() noexcept {
    for (auto& objects : m_objectsByOwner) objects.clear();
    m_ownerByObject.clear();
    m_destroyRequested.clear();
}

void ObjectOwnershipIndex::apply(const ObjectLifecycleEvent& event) {
    if (!event.object) return;
    switch (event.kind) {
    case ObjectLifecycleEventKind::Created:
        // Creation is the only point that can legitimately revive an ID, so it
        // also clears any leftover pending-destroy mark for it.
        m_destroyRequested.erase(event.object);
        assignObject(event.object, event.owner);
        break;
    case ObjectLifecycleEventKind::OwnershipChanged:
        // Use the existing reverse lookup as the removal source.  It makes a
        // duplicated/stale notification harmless without trusting an old
        // owner field to erase another player's current membership.
        eraseObject(event.object);
        // A pending destroy outranks a later ownership transfer: re-inserting
        // here would republish a logically destroyed object under its new owner
        // until Destroyed arrives, and consumers enumerating a player's live
        // objects (scoring, scripts, capture) would observe it.
        if (m_destroyRequested.contains(event.object)) break;
        assignObject(event.object, event.owner);
        break;
    case ObjectLifecycleEventKind::DestroyRequested:
        m_destroyRequested.insert(event.object);
        eraseObject(event.object);
        break;
    case ObjectLifecycleEventKind::Destroyed:
        m_destroyRequested.erase(event.object);
        eraseObject(event.object);
        break;
    }
}

void ObjectOwnershipIndex::apply(container::Span<const ObjectLifecycleEvent> events) {
    for (const ObjectLifecycleEvent& event : events) apply(event);
}

container::Span<const ObjectId> ObjectOwnershipIndex::objects(PlayerId owner) const noexcept {
    if (!validOwner(owner)) return {};
    return m_objectsByOwner[owner.value].values();
}

std::optional<PlayerId> ObjectOwnershipIndex::ownerOf(ObjectId object) const noexcept {
    const auto found = m_ownerByObject.find(object);
    return found == m_ownerByObject.end() ? std::nullopt : std::optional<PlayerId>{found->second};
}

bool ObjectOwnershipIndex::contains(PlayerId owner, ObjectId object) const noexcept {
    return validOwner(owner) && m_objectsByOwner[owner.value].contains(object);
}

bool ObjectOwnershipIndex::validOwner(PlayerId owner) noexcept {
    return owner.isValid() && owner.value < PLAYER_REGISTRY_CAPACITY;
}

void ObjectOwnershipIndex::eraseObject(ObjectId object) noexcept {
    const auto found = m_ownerByObject.find(object);
    if (found == m_ownerByObject.end()) return;
    if (validOwner(found->second)) {
        m_objectsByOwner[found->second.value].erase(object);
    }
    m_ownerByObject.erase(found);
}

void ObjectOwnershipIndex::assignObject(ObjectId object, PlayerId owner) {
    if (!object || !validOwner(owner)) return;
    eraseObject(object);
    m_objectsByOwner[owner.value].insert(object);
    m_ownerByObject.emplace(object, owner);
}

} // namespace engine
