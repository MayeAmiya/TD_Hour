#include "NavigationDynamicOverlay.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace engine::navigation
{

NavigationDynamicOverlayResult NavigationDynamicOverlay::submitBuildingEvent(
    const NavigationBuildingEvent& event,
    container::Span<const NavigationCellId> footprint) noexcept
{
    if (!m_initialized)
        return NavigationDynamicOverlayResult::NotInitialized;
    if (event.confirmedTick == 0 || event.entityId == 0 || !validBuildingReason(event.reason) ||
        !validBuildingState(event.state) ||
        (event.reason == NavigationDynamicEventReason::BuildingDestroyed &&
         event.state != NavigationBuildingState::Absent) ||
        (event.state == NavigationBuildingState::Absent &&
         event.reason != NavigationDynamicEventReason::BuildingDestroyed) ||
        (!event.replaceFootprint && !footprint.empty()) || !event.transaction)
        return NavigationDynamicOverlayResult::InvalidEvent;
    return submitEvent(EventRecord::building(event), footprint);
}

NavigationDynamicOverlayResult NavigationDynamicOverlay::submitBridgeEvent(
    const NavigationBridgeStateEvent& event,
    container::Span<const NavigationCellId> affectedCells) noexcept
{
    if (!m_initialized)
        return NavigationDynamicOverlayResult::NotInitialized;
    if (event.confirmedTick == 0 || event.entityId == 0 || !event.transaction)
        return NavigationDynamicOverlayResult::InvalidEvent;
    return submitEvent(EventRecord::bridge(event), affectedCells);
}

NavigationDynamicOverlayResult NavigationDynamicOverlay::noteStaticNavigationChange(
    NavigationCellBounds dirty) noexcept
{
    if (!m_initialized)
        return NavigationDynamicOverlayResult::NotInitialized;
    if (!validBounds(dirty))
        return NavigationDynamicOverlayResult::InvalidCell;
    if (m_revisions.staticNavigation.value == std::numeric_limits<uint64_t>::max())
        return NavigationDynamicOverlayResult::RevisionExhausted;
    beginRevisionDelta();
    ++m_revisions.staticNavigation.value;
    includeDirty(dirty);
    refreshStableHash();
    return NavigationDynamicOverlayResult::Success;
}

NavigationDynamicCommitResult NavigationDynamicOverlay::commitConfirmedTick(uint64_t confirmedTick,
                                                                              uint32_t eventBudget,
                                                                              uint32_t cellBudget) noexcept
{
    NavigationDynamicCommitResult result;
    result.pendingEvents = static_cast<uint32_t>(m_events.size());
    if (!m_initialized || confirmedTick == 0 || eventBudget == 0)
        return result;

    m_sealedThroughTick = std::max(m_sealedThroughTick, confirmedTick);
    if (!m_activeEvent)
        sortEvents();

    while (result.committedEvents < eventBudget)
    {
        if (!m_activeEvent)
        {
            if (m_events.empty() || m_events.front().confirmedTick > m_sealedThroughTick)
                break;
            const NavigationDynamicCommitStatus started = startFrontEvent();
            if (started != NavigationDynamicCommitStatus::Complete)
            {
                result.status = started;
                result.pendingEvents = static_cast<uint32_t>(m_events.size());
                refreshStableHash();
                return result;
            }
            if (!m_activeEvent)
            {
                result.lastCommittedTransaction = m_events.front().transaction;
                if (m_lastCommittedTransaction < result.lastCommittedTransaction)
                    m_lastCommittedTransaction = result.lastCommittedTransaction;
                popFrontEvent();
                ++result.committedEvents;
                refreshStableHash();
                continue;
            }
        }

        bool completed = false;
        while (!completed)
        {
            if (result.visitedCells == cellBudget && activeNeedsCell())
            {
                result.status = NavigationDynamicCommitStatus::BudgetExhausted;
                result.pendingEvents = static_cast<uint32_t>(m_events.size());
                refreshStableHash();
                return result;
            }
            bool consumedCell = false;
            const NavigationDynamicCommitStatus advanced = advanceActiveEvent(consumedCell, completed);
            if (advanced != NavigationDynamicCommitStatus::Complete)
            {
                result.status = advanced;
                result.pendingEvents = static_cast<uint32_t>(m_events.size());
                refreshStableHash();
                return result;
            }
            if (consumedCell)
                ++result.visitedCells;
            if (!completed && !consumedCell)
                continue;
            if (!completed && result.visitedCells == cellBudget)
            {
                result.status = NavigationDynamicCommitStatus::BudgetExhausted;
                result.pendingEvents = static_cast<uint32_t>(m_events.size());
                refreshStableHash();
                return result;
            }
        }

        result.lastCommittedTransaction = m_events.front().transaction;
        if (m_lastCommittedTransaction < result.lastCommittedTransaction)
            m_lastCommittedTransaction = result.lastCommittedTransaction;
        popFrontEvent();
        ++result.committedEvents;
        refreshStableHash();
    }

    result.pendingEvents = static_cast<uint32_t>(m_events.size());
    const bool eligible = !m_events.empty() && m_events.front().confirmedTick <= m_sealedThroughTick;
    if (m_activeEvent || eligible)
        result.status = NavigationDynamicCommitStatus::BudgetExhausted;
    else if (result.committedEvents != 0)
        result.status = NavigationDynamicCommitStatus::Complete;
    refreshStableHash();
    return result;
}

NavigationDynamicCommitResult NavigationDynamicOverlay::commitStartupEvents(
    uint32_t eventBudget, uint32_t cellBudget) noexcept
{
    NavigationDynamicCommitResult result;
    result.pendingEvents = static_cast<uint32_t>(m_events.size());
    if (!m_initialized || m_sealedThroughTick != 0 || m_activeEvent ||
        eventBudget == 0)
        return result;

    // Bootstrap producers normalize their source boundary to tick one. Use
    // the ordinary deterministic commit implementation, then reopen tick one
    // before returning. The committed transaction high-water mark and the
    // resulting occupancy remain authoritative; only admission sealing is
    // restored so real tick-one transactions are still legal.
    result = commitConfirmedTick(1u, eventBudget, cellBudget);
    m_sealedThroughTick = 0;
    refreshStableHash();
    return result;
}

NavigationDirtyRebuildResult NavigationDynamicOverlay::rebuildDirtyRegion(uint64_t cellBudget) noexcept
{
    if (m_activeEvent || hasSealedPendingEvent())
        return {NavigationDirtyRebuildStatus::CommitPending, 0, dirtyRemaining()};
    if (!m_hasUnpublished)
        return {};

    const uint64_t total = m_unpublishedDirty.cellCount();
    const uint64_t remaining = total - m_dirtyCellsVisited;
    const uint64_t visited = std::min(cellBudget, remaining);
    m_dirtyCellsVisited += visited;
    if (m_dirtyCellsVisited != total)
    {
        refreshStableHash();
        return {NavigationDirtyRebuildStatus::Progressed, visited, total - m_dirtyCellsVisited};
    }

    m_publishedDirty = {m_dirtyBefore, m_revisions, m_unpublishedDirty, true};
    m_hasUnpublished = false;
    m_unpublishedDirty = {};
    m_dirtyCellsVisited = 0;
    refreshStableHash();
    return {NavigationDirtyRebuildStatus::Published, visited, 0};
}

NavigationDynamicOverlayResult NavigationDynamicOverlay::submitEvent(
    EventRecord record,
    container::Span<const NavigationCellId> cells) noexcept
{
    if (record.confirmedTick <= m_sealedThroughTick)
        return NavigationDynamicOverlayResult::TickAlreadySealed;
    if (record.transaction <= m_lastCommittedTransaction)
        return NavigationDynamicOverlayResult::DuplicateEventKey;
    if (cells.size() > std::numeric_limits<uint32_t>::max())
        return NavigationDynamicOverlayResult::FootprintCapacityExceeded;
    for (NavigationCellId cell : cells)
    {
        if (!contains(cell))
            return NavigationDynamicOverlayResult::InvalidCell;
    }
    for (const EventRecord& queued : m_events)
    {
        if (queued.transaction == record.transaction)
            return NavigationDynamicOverlayResult::DuplicateEventKey;
    }
    if (m_events.size() >= m_eventCapacity)
        return NavigationDynamicOverlayResult::EventCapacityExceeded;

    NavigationCellArena::Range allocation;
    if (!m_footprintCells.allocate(static_cast<uint32_t>(cells.size()),
                                   allocation))
        return NavigationDynamicOverlayResult::FootprintCapacityExceeded;
    record.cells = allocation;
    NavigationCellId* destination = eventCells(record);
    std::copy(cells.begin(), cells.end(), destination);
    std::sort(destination, destination + cells.size());
    if (std::adjacent_find(destination, destination + cells.size()) != destination + cells.size())
    {
        m_footprintCells.release(record.cells);
        return NavigationDynamicOverlayResult::DuplicateCell;
    }
    record.cellCount = static_cast<uint32_t>(cells.size());
    m_events.push_back(record);
    refreshStableHash();
    return NavigationDynamicOverlayResult::Success;
}

void NavigationDynamicOverlay::sortEvents() noexcept
{
    std::sort(m_events.begin(), m_events.end(), [](const EventRecord& left, const EventRecord& right) {
        return std::tuple(left.transaction.generation, left.transaction.revision,
                          left.confirmedTick, left.entityId, static_cast<uint8_t>(left.reason)) <
               std::tuple(right.transaction.generation, right.transaction.revision,
                          right.confirmedTick, right.entityId, static_cast<uint8_t>(right.reason));
    });
}

NavigationDynamicCommitStatus NavigationDynamicOverlay::startFrontEvent() noexcept
{
    const EventRecord& event = m_events.front();
    return event.kind == EventKind::Building ? startBuildingEvent(event) : startBridgeEvent(event);
}

NavigationDynamicCommitStatus NavigationDynamicOverlay::startBuildingEvent(const EventRecord& event) noexcept
{
    const size_t order = findEntityOrder(event.entityId);
    const bool exists = order != NoIndex;
    if (!exists && event.buildingState == NavigationBuildingState::Absent)
        return NavigationDynamicCommitStatus::Complete;
    if (!exists && !event.replaceFootprint)
        return NavigationDynamicCommitStatus::InvalidEvent;

    uint32_t slot = 0;
    if (exists)
        slot = m_entityOrder[order];
    else
    {
        if (m_freeEntities.empty())
            return NavigationDynamicCommitStatus::EntityCapacityExceeded;
        slot = m_freeEntities.back();
    }

    const EntityRecord& old = m_entities[slot];
    const uint32_t oldCount = exists ? old.cellCount : 0;
    const uint32_t targetCount = event.replaceFootprint ? event.cellCount : oldCount;
    const NavigationCellId* oldCells = entityCells(slot);
    const NavigationCellId* targetCells = event.replaceFootprint ? eventCells(event) : oldCells;
    const bool targetBlocks = event.buildingState != NavigationBuildingState::Absent && event.value;
    const bool targetAirBlocks = event.buildingState != NavigationBuildingState::Absent &&
        event.blocksAirNavigation;
    const bool targetRubble = event.buildingState !=
            NavigationBuildingState::Absent && event.rubbleSurface;
    const bool targetFence = event.buildingState !=
            NavigationBuildingState::Absent &&
        (event.replaceFootprint ? event.fenceSurface
                                : exists && old.fenceSurface);
    bool sameFootprint = oldCount == targetCount;
    if (sameFootprint)
        sameFootprint = std::equal(oldCells, oldCells + oldCount, targetCells);
    const auto sameOccupiedCells = [oldCount, targetCount, sameFootprint](
            bool oldBlocks, bool nextBlocks) noexcept {
        const bool oldHasCells = oldBlocks && oldCount != 0;
        const bool nextHasCells = nextBlocks && targetCount != 0;
        if (oldHasCells != nextHasCells) return false;
        return !oldHasCells || sameFootprint;
    };
    const bool topologyChanged =
        !sameOccupiedCells(exists && old.blocksNavigation, targetBlocks) ||
        !sameOccupiedCells(exists && old.blocksAirNavigation,
                           targetAirBlocks) ||
        !sameOccupiedCells(exists && old.rubbleSurface,
                           targetRubble) ||
        !sameOccupiedCells(exists && old.fenceSurface,
                           targetFence);
    if (!topologyChanged) {
        // A lifecycle/footprint transaction may alter only the entity record:
        // Placed -> Complete, a non-blocking object, or a zero-cell small
        // object has no grid topology to publish. Preserve the latest record
        // and footprint without manufacturing an empty dirty revision.
        if (event.buildingState == NavigationBuildingState::Absent) {
            EntityRecord& entity = m_entities[slot];
            m_entityOrder.erase(
                m_entityOrder.begin() + static_cast<std::ptrdiff_t>(order));
            m_footprintCells.release(entity.cells);
            entity = {};
            m_freeEntities.push_back(slot);
        } else {
            if (!exists) {
                m_freeEntities.pop_back();
                m_entities[slot] = {
                    event.entityId, event.buildingState, {}, 0,
                    false, false, true, event.transaction};
                const auto position = std::lower_bound(
                    m_entityOrder.begin(), m_entityOrder.end(),
                    event.entityId,
                    [this](uint32_t candidate, uint64_t sought) {
                        return m_entities[candidate].entityId < sought;
                    });
                m_entityOrder.insert(position, slot);
            }
            EntityRecord& entity = m_entities[slot];
            if (event.replaceFootprint) {
                m_footprintCells.release(entity.cells);
                entity.cells = event.cells;
                m_events.front().cells = {};
                m_events.front().cellCount = 0;
            }
            entity.state = event.buildingState;
            entity.cellCount = targetCount;
            entity.blocksNavigation = targetBlocks;
            entity.blocksAirNavigation = targetAirBlocks;
            entity.rubbleSurface = targetRubble;
            entity.fenceSurface = targetFence;
            entity.transaction = event.transaction;
        }
        refreshStableHash();
        return NavigationDynamicCommitStatus::Complete;
    }
    if (m_revisions.dynamicObstacles.value == std::numeric_limits<uint64_t>::max())
        return NavigationDynamicCommitStatus::RevisionExhausted;

    if (targetBlocks)
    {
        for (uint32_t index = 0; index < targetCount; ++index)
        {
            const bool retained = exists && old.blocksNavigation &&
                                  std::binary_search(oldCells, oldCells + oldCount, targetCells[index]);
            if (!retained && m_ownerCount[targetCells[index].value] == std::numeric_limits<uint32_t>::max())
                return NavigationDynamicCommitStatus::RefCountOverflow;
        }
    }
    if (targetAirBlocks)
    {
        for (uint32_t index = 0; index < targetCount; ++index)
        {
            const bool retained = exists && old.blocksAirNavigation &&
                                  std::binary_search(oldCells, oldCells + oldCount, targetCells[index]);
            if (!retained && m_airOwnerCount[targetCells[index].value] == std::numeric_limits<uint32_t>::max())
                return NavigationDynamicCommitStatus::RefCountOverflow;
        }
    }
    if (targetRubble)
    {
        for (uint32_t index = 0; index < targetCount; ++index)
        {
            const bool retained = exists && old.rubbleSurface &&
                std::binary_search(
                    oldCells, oldCells + oldCount,
                    targetCells[index]);
            if (!retained &&
                m_rubbleOwnerCount[targetCells[index].value] ==
                    std::numeric_limits<uint32_t>::max())
                return NavigationDynamicCommitStatus::RefCountOverflow;
        }
    }
    if (targetFence)
    {
        for (uint32_t index = 0; index < targetCount; ++index)
        {
            const bool retained = exists && old.fenceSurface &&
                std::binary_search(
                    oldCells, oldCells + oldCount,
                    targetCells[index]);
            if (!retained &&
                m_fenceOwnerCount[targetCells[index].value] ==
                    std::numeric_limits<uint32_t>::max())
                return NavigationDynamicCommitStatus::RefCountOverflow;
        }
    }

    if (!exists)
    {
        m_freeEntities.pop_back();
        m_entities[slot] = {event.entityId, event.buildingState, {}, 0,
                            false, false, true, event.transaction};
    }
    m_activeEvent = true;
    m_activeKind = EventKind::Building;
    m_activeSlot = slot;
    m_activeWasNew = !exists;
    m_activeOldCount = oldCount;
    m_activeTargetCount = targetCount;
    m_activeOldBlocks = exists && old.blocksNavigation;
    m_activeTargetBlocks = targetBlocks;
    m_activeOldAirBlocks = exists && old.blocksAirNavigation;
    m_activeTargetAirBlocks = targetAirBlocks;
    m_activeOldRubble = exists && old.rubbleSurface;
    m_activeTargetRubble = targetRubble;
    m_activeOldFence = exists && old.fenceSurface;
    m_activeTargetFence = targetFence;
    m_activeTopologyChanged = false;
    m_activeOldIndex = 0;
    m_activeTargetIndex = 0;
    m_activePhase = ActivePhase::Occupancy;
    return NavigationDynamicCommitStatus::Complete;
}

NavigationDynamicCommitStatus NavigationDynamicOverlay::startBridgeEvent(const EventRecord& event) noexcept
{
    const size_t order = findBridgeOrder(event.entityId);
    const bool exists = order != NoIndex;
    if (exists && m_bridges[m_bridgeOrder[order]].active == event.value)
        return NavigationDynamicCommitStatus::Complete;
    if (!exists && m_freeBridges.empty())
        return NavigationDynamicCommitStatus::BridgeCapacityExceeded;
    if (m_revisions.portalTopology.value == std::numeric_limits<uint64_t>::max())
        return NavigationDynamicCommitStatus::RevisionExhausted;

    const uint32_t slot = exists ? m_bridgeOrder[order] : m_freeBridges.back();
    if (!exists)
    {
        m_freeBridges.pop_back();
        m_bridges[slot] = {event.entityId, slot, false, true, event.transaction};
    }
    m_activeEvent = true;
    m_activeKind = EventKind::Bridge;
    m_activeSlot = slot;
    m_activeWasNew = !exists;
    m_activeTargetIndex = 0;
    m_activeTopologyChanged = false;
    m_activePhase = ActivePhase::BridgeDirty;
    return NavigationDynamicCommitStatus::Complete;
}

NavigationDynamicCommitStatus NavigationDynamicOverlay::advanceActiveEvent(bool& consumedCell,
                                                                            bool& completed) noexcept
{
    consumedCell = false;
    completed = false;
    if (m_activeKind == EventKind::Bridge)
        return advanceBridgeEvent(consumedCell, completed);
    return advanceBuildingEvent(consumedCell, completed);
}

bool NavigationDynamicOverlay::activeNeedsCell() const noexcept
{
    const EventRecord& event = m_events.front();
    if (m_activeKind == EventKind::Bridge)
        return m_activePhase == ActivePhase::BridgeDirty && m_activeTargetIndex < event.cellCount;
    if (m_activePhase != ActivePhase::Occupancy)
        return false;
    return m_activeOldIndex < m_activeOldCount ||
           m_activeTargetIndex < m_activeTargetCount;
}

NavigationDynamicCommitStatus NavigationDynamicOverlay::advanceBuildingEvent(bool& consumedCell,
                                                                              bool& completed) noexcept
{
    const EventRecord& event = m_events.front();
    const NavigationCellId* oldCells = entityCells(m_activeSlot);
    const NavigationCellId* targetCells = event.replaceFootprint ? eventCells(event) : oldCells;

    if (m_activePhase == ActivePhase::Occupancy)
    {
        if (m_activeOldIndex < m_activeOldCount ||
            m_activeTargetIndex < m_activeTargetCount)
        {
            consumedCell = true;
            const bool hasOld = m_activeOldIndex < m_activeOldCount;
            const bool hasTarget = m_activeTargetIndex < m_activeTargetCount;
            const NavigationCellId oldCell = hasOld
                ? oldCells[m_activeOldIndex] : InvalidNavigationCell;
            const NavigationCellId targetCell = hasTarget
                ? targetCells[m_activeTargetIndex] : InvalidNavigationCell;
            if (!hasTarget || (hasOld && oldCell < targetCell))
            {
                if (m_activeOldBlocks) removeOwner(oldCell);
                if (m_activeOldAirBlocks) removeAirOwner(oldCell);
                if (m_activeOldRubble) removeRubbleOwner(oldCell);
                if (m_activeOldFence) removeFenceOwner(oldCell);
                ++m_activeOldIndex;
            }
            else if (!hasOld || targetCell < oldCell)
            {
                if (m_activeTargetBlocks) addOwner(targetCell);
                if (m_activeTargetAirBlocks) addAirOwner(targetCell);
                if (m_activeTargetRubble) addRubbleOwner(targetCell);
                if (m_activeTargetFence) addFenceOwner(targetCell);
                ++m_activeTargetIndex;
            }
            else
            {
                if (m_activeOldBlocks != m_activeTargetBlocks)
                {
                    if (m_activeTargetBlocks) addOwner(oldCell);
                    else removeOwner(oldCell);
                }
                if (m_activeOldAirBlocks != m_activeTargetAirBlocks)
                {
                    if (m_activeTargetAirBlocks) addAirOwner(oldCell);
                    else removeAirOwner(oldCell);
                }
                if (m_activeOldRubble != m_activeTargetRubble)
                {
                    if (m_activeTargetRubble) addRubbleOwner(oldCell);
                    else removeRubbleOwner(oldCell);
                }
                if (m_activeOldFence != m_activeTargetFence)
                {
                    if (m_activeTargetFence) addFenceOwner(oldCell);
                    else removeFenceOwner(oldCell);
                }
                ++m_activeOldIndex;
                ++m_activeTargetIndex;
            }
            return NavigationDynamicCommitStatus::Complete;
        }
        m_activePhase = ActivePhase::Finalize;
    }

    if (m_activePhase == ActivePhase::Finalize)
    {
        finalizeBuildingEvent(event);
        completed = true;
    }
    return NavigationDynamicCommitStatus::Complete;
}

NavigationDynamicCommitStatus NavigationDynamicOverlay::advanceBridgeEvent(bool& consumedCell,
                                                                            bool& completed) noexcept
{
    const EventRecord& event = m_events.front();
    if (m_activePhase == ActivePhase::BridgeDirty && m_activeTargetIndex < event.cellCount)
    {
        consumedCell = true;
        includeDirty(eventCells(event)[m_activeTargetIndex++]);
        return NavigationDynamicCommitStatus::Complete;
    }
    beginRevisionDelta();
    ++m_revisions.portalTopology.value;
    BridgeRecord& bridge = m_bridges[m_activeSlot];
    bridge.active = event.value;
    bridge.transaction = event.transaction;
    recordBridgeDirtyChange(event);
    if (m_activeWasNew)
    {
        const auto position = std::lower_bound(
            m_bridgeOrder.begin(), m_bridgeOrder.end(), bridge.entityId,
            [this](uint32_t slot, uint64_t sought) { return m_bridges[slot].entityId < sought; });
        m_bridgeOrder.insert(position, m_activeSlot);
    }
    m_activeEvent = false;
    refreshStableHash();
    completed = true;
    return NavigationDynamicCommitStatus::Complete;
}

void NavigationDynamicOverlay::finalizeBuildingEvent(const EventRecord& event) noexcept
{
    // add/removeOwner opens the revision delta only when the aggregate grid
    // changed. Overlapping owners and zero-cell footprints still update their
    // durable entity metadata, but must not publish an empty grid revision.
    if (m_activeTopologyChanged)
        ++m_revisions.dynamicObstacles.value;
    EntityRecord& entity = m_entities[m_activeSlot];
    if (event.buildingState == NavigationBuildingState::Absent)
    {
        const size_t order = findEntityOrder(event.entityId);
        if (order != NoIndex)
            m_entityOrder.erase(m_entityOrder.begin() + static_cast<std::ptrdiff_t>(order));
        m_footprintCells.release(entity.cells);
        entity = {};
        m_freeEntities.push_back(m_activeSlot);
    }
    else
    {
        if (event.replaceFootprint)
        {
            m_footprintCells.release(entity.cells);
            entity.cells = event.cells;
            m_events.front().cells = {};
            m_events.front().cellCount = 0;
        }
        entity.state = event.buildingState;
        entity.cellCount = m_activeTargetCount;
        entity.blocksNavigation = m_activeTargetBlocks;
        entity.blocksAirNavigation = m_activeTargetAirBlocks;
        entity.rubbleSurface = m_activeTargetRubble;
        entity.fenceSurface = m_activeTargetFence;
        entity.transaction = event.transaction;
        if (m_activeWasNew)
        {
            const auto position = std::lower_bound(
                m_entityOrder.begin(), m_entityOrder.end(), entity.entityId,
                [this](uint32_t slot, uint64_t sought) { return m_entities[slot].entityId < sought; });
            m_entityOrder.insert(position, m_activeSlot);
        }
    }
    m_activeEvent = false;
    m_activeTopologyChanged = false;
    refreshStableHash();
}

void NavigationDynamicOverlay::addOwner(NavigationCellId cell) noexcept
{
    uint32_t& count = m_ownerCount[cell.value];
    const bool wasFenceOnly = count != 0 &&
        count == m_fenceOwnerCount[cell.value];
    if (count == 0 || wasFenceOnly) {
        includeDirty(cell);
        m_activeTopologyChanged = true;
    }
    ++count;
}

void NavigationDynamicOverlay::removeOwner(NavigationCellId cell) noexcept
{
    uint32_t& count = m_ownerCount[cell.value];
    if (count != 0)
    {
        const bool wasFenceOnly =
            count == m_fenceOwnerCount[cell.value];
        --count;
        const bool nowFenceOnly = count != 0 &&
            count == m_fenceOwnerCount[cell.value];
        if (count == 0 || wasFenceOnly != nowFenceOnly) {
            includeDirty(cell);
            m_activeTopologyChanged = true;
        }
    }
}

void NavigationDynamicOverlay::addAirOwner(NavigationCellId cell) noexcept
{
    uint32_t& count = m_airOwnerCount[cell.value];
    if (count == 0) {
        includeDirty(cell);
        m_activeTopologyChanged = true;
    }
    ++count;
}

void NavigationDynamicOverlay::removeAirOwner(NavigationCellId cell) noexcept
{
    uint32_t& count = m_airOwnerCount[cell.value];
    if (count != 0)
    {
        --count;
        if (count == 0) {
            includeDirty(cell);
            m_activeTopologyChanged = true;
        }
    }
}

void NavigationDynamicOverlay::addRubbleOwner(NavigationCellId cell) noexcept
{
    uint32_t& count = m_rubbleOwnerCount[cell.value];
    if (count == 0) {
        includeDirty(cell);
        m_activeTopologyChanged = true;
    }
    ++count;
}

void NavigationDynamicOverlay::removeRubbleOwner(
    NavigationCellId cell) noexcept
{
    uint32_t& count = m_rubbleOwnerCount[cell.value];
    if (count != 0) {
        --count;
        if (count == 0) {
            includeDirty(cell);
            m_activeTopologyChanged = true;
        }
    }
}

void NavigationDynamicOverlay::addFenceOwner(NavigationCellId cell) noexcept
{
    uint32_t& count = m_fenceOwnerCount[cell.value];
    includeDirty(cell);
    m_activeTopologyChanged = true;
    ++count;
}

void NavigationDynamicOverlay::removeFenceOwner(
    NavigationCellId cell) noexcept
{
    uint32_t& count = m_fenceOwnerCount[cell.value];
    if (count != 0) {
        --count;
        includeDirty(cell);
        m_activeTopologyChanged = true;
    }
}

void NavigationDynamicOverlay::beginRevisionDelta() noexcept
{
    if (!m_hasUnpublished)
    {
        m_dirtyBefore = m_revisions;
        m_hasUnpublished = true;
        m_unpublishedDirty = {};
        m_bridgeDirtyChangeCount = 0;
    }
    m_dirtyCellsVisited = 0;
}

void NavigationDynamicOverlay::includeDirty(NavigationCellId cell) noexcept
{
    beginRevisionDelta();
    m_unpublishedDirty.include(cell, m_width);
}

void NavigationDynamicOverlay::includeDirty(NavigationCellBounds bounds) noexcept
{
    beginRevisionDelta();
    m_unpublishedDirty.include(bounds);
}

void NavigationDynamicOverlay::recordBridgeDirtyChange(
    const EventRecord& event) noexcept
{
    NavigationCellBounds bounds;
    const NavigationCellId* cells = eventCells(event);
    for (uint32_t index = 0; index < event.cellCount; ++index)
        bounds.include(cells[index], m_width);
    for (size_t index = 0; index < m_bridgeDirtyChangeCount; ++index)
    {
        NavigationBridgeDirtyChange& change = m_bridgeDirtyChanges[index];
        if (change.bridgeId == event.entityId)
        {
            change.affectedCells.include(bounds);
            return;
        }
    }
    if (m_bridgeDirtyChangeCount >= m_bridgeDirtyChanges.size())
        return;
    m_bridgeDirtyChanges[m_bridgeDirtyChangeCount++] =
        {event.entityId, bounds};
}

void NavigationDynamicOverlay::popFrontEvent() noexcept
{
    m_footprintCells.release(m_events.front().cells);
    m_events.erase(m_events.begin());
}

bool NavigationDynamicOverlay::hasSealedPendingEvent() const noexcept
{
    return !m_events.empty() && m_events.front().confirmedTick <= m_sealedThroughTick;
}

uint64_t NavigationDynamicOverlay::dirtyRemaining() const noexcept
{
    const uint64_t total = m_unpublishedDirty.cellCount();
    return total >= m_dirtyCellsVisited ? total - m_dirtyCellsVisited : 0;
}

} // namespace engine::navigation
