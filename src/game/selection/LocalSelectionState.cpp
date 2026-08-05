#include "core/container/container_types.h"
#include "game/selection/LocalSelectionState.h"

#include <algorithm>
#include <limits>
namespace engine::selection {

bool LocalSelectionState::replace(container::Span<const ObjectId> objects) {
    bool changed = replaceSet(m_selected, objects);
    if (m_selectedOrderWaypoint) {
        m_selectedOrderWaypoint = {};
        changed = true;
        bumpRevision();
    }
    if (changed) static_cast<void>(cancelPendingWorldCommand());
    return changed;
}

bool LocalSelectionState::add(ObjectId object) {
    if (!object || !m_selected.insert(object)) return false;
    m_selectedOrderWaypoint = {};
    bumpRevision();
    static_cast<void>(cancelPendingWorldCommand());
    return true;
}

bool LocalSelectionState::remove(ObjectId object) {
    if (!m_selected.erase(object)) return false;
    bumpRevision();
    static_cast<void>(cancelPendingWorldCommand());
    return true;
}

bool LocalSelectionState::toggle(ObjectId object) {
    if (!object) return false;
    if (m_selected.erase(object)) {
        bumpRevision();
        static_cast<void>(cancelPendingWorldCommand());
        return true;
    }
    if (!m_selected.insert(object)) return false;
    m_selectedOrderWaypoint = {};
    bumpRevision();
    static_cast<void>(cancelPendingWorldCommand());
    return true;
}

bool LocalSelectionState::contains(ObjectId object) const {
    return m_selected.contains(object);
}

bool LocalSelectionState::clear() {
    if (m_selected.empty() && !m_selectedOrderWaypoint) return false;
    m_selected.clear();
    m_selectedOrderWaypoint = {};
    bumpRevision();
    static_cast<void>(cancelPendingWorldCommand());
    return true;
}

bool LocalSelectionState::clearTransientInteraction() noexcept {
    const bool changed = !m_selected.empty() ||
        static_cast<bool>(m_selectedOrderWaypoint) ||
        m_hovered != INVALID_OBJECT_ID || m_pendingWorldCommand.active();
    m_selected.clear();
    m_selectedOrderWaypoint = {};
    m_hovered = INVALID_OBJECT_ID;
    static_cast<void>(cancelPendingWorldCommand());
    if (changed) bumpRevision();
    return changed;
}

bool LocalSelectionState::selectOrderWaypoint(
    LocalOrderWaypointSelection waypoint) {
    if (!waypoint) return false;
    const bool changed = m_selectedOrderWaypoint != waypoint ||
        !m_selected.empty();
    if (!changed) return false;
    m_selected.clear();
    m_selectedOrderWaypoint = waypoint;
    static_cast<void>(cancelPendingWorldCommand());
    bumpRevision();
    return true;
}

bool LocalSelectionState::beginPendingWorldCommand(
    PendingWorldCommandMode mode) {
    if (!mode.sourceObject ||
        (!mode.sourceMayBeUnselected && !contains(mode.sourceObject)) ||
        mode.kind == PendingWorldCommandKind::None ||
        mode.targetKind == PendingWorldTargetKind::None ||
        mode.commandButtonName.empty()) {
        return false;
    }
    uint64_t revision = m_nextPendingWorldCommandRevision++;
    if (revision == 0) {
        revision = m_nextPendingWorldCommandRevision++;
    }
    mode.revision = revision;
    m_pendingWorldCommand = std::move(mode);
    return true;
}

bool LocalSelectionState::cancelPendingWorldCommand(
    uint64_t expectedRevision) noexcept {
    if (!m_pendingWorldCommand.active() ||
        (expectedRevision != 0 &&
         m_pendingWorldCommand.revision != expectedRevision)) {
        return false;
    }
    m_pendingWorldCommand = {};
    return true;
}

bool LocalSelectionState::setHovered(ObjectId object) {
    if (m_hovered == object) return false;
    m_hovered = object;
    return true;
}

bool LocalSelectionState::saveControlGroup(size_t index) {
    return saveControlGroup(index, m_selected.values());
}

bool LocalSelectionState::saveControlGroup(
    size_t index, container::Span<const ObjectId> objects) {
    if (index >= m_controlGroups.size()) return false;
    return replaceSet(m_controlGroups[index], objects);
}

bool LocalSelectionState::appendToControlGroup(size_t index) {
    return appendToControlGroup(index, m_selected.values());
}

bool LocalSelectionState::appendToControlGroup(
    size_t index, container::Span<const ObjectId> objects) {
    if (index >= m_controlGroups.size()) return false;
    bool changed = false;
    for (const ObjectId object : objects) {
        changed = m_controlGroups[index].insert(object) || changed;
    }
    if (changed) bumpRevision();
    return changed;
}

bool LocalSelectionState::recallControlGroup(size_t index) {
    if (index >= m_controlGroups.size()) return false;
    const bool changed = replaceSet(m_selected, m_controlGroups[index].values());
    if (changed) static_cast<void>(cancelPendingWorldCommand());
    return changed;
}

container::Span<const ObjectId> LocalSelectionState::controlGroup(size_t index) const noexcept {
    return index < m_controlGroups.size() ? m_controlGroups[index].values() : container::Span<const ObjectId>{};
}

bool LocalSelectionState::applyLifecycleEvents(container::Span<const ObjectLifecycleEvent> events) {
    bool selectionChanged = false;
    bool hoverChanged = false;
    for (const ObjectLifecycleEvent& event : events) {
        if (event.kind == ObjectLifecycleEventKind::DestroyRequested ||
            event.kind == ObjectLifecycleEventKind::Destroyed) {
            if (m_selectedOrderWaypoint.actor == event.object) {
                m_selectedOrderWaypoint = {};
                selectionChanged = true;
            }
            selectionChanged = removeFromAll(event.object) || selectionChanged;
            if (m_hovered == event.object) {
                m_hovered = INVALID_OBJECT_ID;
                hoverChanged = true;
            }
        }
    }
    if (selectionChanged) {
        bumpRevision();
        static_cast<void>(cancelPendingWorldCommand());
    } else if (m_pendingWorldCommand.active()) {
        for (const ObjectLifecycleEvent& event : events) {
            if (event.object == m_pendingWorldCommand.sourceObject &&
                (event.kind == ObjectLifecycleEventKind::DestroyRequested ||
                 event.kind == ObjectLifecycleEventKind::Destroyed)) {
                static_cast<void>(cancelPendingWorldCommand());
                break;
            }
        }
    }
    return selectionChanged || hoverChanged;
}

bool LocalSelectionState::pruneUnavailable(const ObjectLifecycle& lifecycle) {
    bool selectionChanged = false;
    bool hoverChanged = false;
    const auto prune = [&lifecycle, &selectionChanged](container::OrderedIdSet<ObjectId>& values) {
        container::Vector<ObjectId> live;
        live.reserve(values.size());
        for (const ObjectId object : values.values()) {
            if (lifecycle.entityFromId(object)) live.push_back(object);
        }
        if (equal(values.values(), live)) return;
        values.assign(std::move(live));
        selectionChanged = true;
    };
    prune(m_selected);
    if (m_selectedOrderWaypoint &&
        !lifecycle.entityFromId(m_selectedOrderWaypoint.actor)) {
        m_selectedOrderWaypoint = {};
        selectionChanged = true;
    }
    for (auto& group : m_controlGroups) prune(group);
    if (m_hovered && !lifecycle.entityFromId(m_hovered)) {
        m_hovered = INVALID_OBJECT_ID;
        hoverChanged = true;
    }
    if (selectionChanged) {
        bumpRevision();
        static_cast<void>(cancelPendingWorldCommand());
    } else if (m_pendingWorldCommand.active() &&
               !lifecycle.entityFromId(
                   m_pendingWorldCommand.sourceObject)) {
        static_cast<void>(cancelPendingWorldCommand());
    }
    return selectionChanged || hoverChanged;
}

bool LocalSelectionState::removeObjects(
    container::Span<const ObjectId> objects) {
    bool changed = false;
    for (const ObjectId object : objects) {
        changed = removeFromAll(object) || changed;
        if (m_selectedOrderWaypoint.actor == object) {
            m_selectedOrderWaypoint = {};
            changed = true;
        }
        if (m_hovered == object) m_hovered = INVALID_OBJECT_ID;
    }
    if (changed) {
        bumpRevision();
        static_cast<void>(cancelPendingWorldCommand());
    }
    return changed;
}

void LocalSelectionState::reset() noexcept {
    bool changed = !m_selected.empty() ||
        static_cast<bool>(m_selectedOrderWaypoint);
    m_selected.clear();
    m_selectedOrderWaypoint = {};
    m_hovered = INVALID_OBJECT_ID;
    for (auto& group : m_controlGroups) {
        changed = changed || !group.empty();
        group.clear();
    }
    static_cast<void>(cancelPendingWorldCommand());
    if (changed) bumpRevision();
}

bool LocalSelectionState::equal(container::Span<const ObjectId> lhs,
                                container::Span<const ObjectId> rhs) noexcept {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

bool LocalSelectionState::replaceSet(container::OrderedIdSet<ObjectId>& destination,
                                     container::Span<const ObjectId> objects) {
    container::Vector<ObjectId> canonical;
    canonical.reserve(objects.size());
    for (const ObjectId object : objects) {
        if (object) canonical.push_back(object);
    }
    std::sort(canonical.begin(), canonical.end());
    canonical.erase(std::unique(canonical.begin(), canonical.end()), canonical.end());
    if (equal(destination.values(), canonical)) return false;
    destination.assign(std::move(canonical));
    bumpRevision();
    return true;
}

bool LocalSelectionState::removeFromAll(ObjectId object) {
    if (!object) return false;
    bool changed = m_selected.erase(object);
    for (auto& group : m_controlGroups) {
        changed = group.erase(object) || changed;
    }
    return changed;
}

void LocalSelectionState::bumpRevision() noexcept {
    if (m_revision != std::numeric_limits<uint64_t>::max()) ++m_revision;
}

} // namespace engine::selection
