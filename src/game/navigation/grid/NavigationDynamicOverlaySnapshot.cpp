#include "NavigationDynamicOverlay.h"

#include <algorithm>

namespace engine::navigation
{

uint32_t NavigationDynamicOverlay::ownerCount(NavigationCellId cell) const noexcept
{
    return contains(cell) ? m_ownerCount[cell.value] : 0;
}

bool NavigationDynamicOverlay::blocked(NavigationCellId cell) const noexcept
{
    return ownerCount(cell) != 0;
}

uint32_t NavigationDynamicOverlay::airOwnerCount(NavigationCellId cell) const noexcept
{
    return contains(cell) ? m_airOwnerCount[cell.value] : 0;
}

bool NavigationDynamicOverlay::blocksAirNavigation(NavigationCellId cell) const noexcept
{
    return airOwnerCount(cell) != 0;
}

uint32_t NavigationDynamicOverlay::rubbleOwnerCount(
    NavigationCellId cell) const noexcept
{
    return contains(cell) ? m_rubbleOwnerCount[cell.value] : 0;
}

bool NavigationDynamicOverlay::rubbleSurface(
    NavigationCellId cell) const noexcept
{
    return rubbleOwnerCount(cell) != 0;
}

uint32_t NavigationDynamicOverlay::fenceOwnerCount(
    NavigationCellId cell) const noexcept
{
    return contains(cell) ? m_fenceOwnerCount[cell.value] : 0;
}

bool NavigationDynamicOverlay::fenceSurface(
    NavigationCellId cell) const noexcept
{
    return fenceOwnerCount(cell) != 0;
}

NavigationAirObstacleQuery NavigationDynamicOverlay::airObstacleAt(
    NavigationCellId cell, uint64_t ignoredEntityId) const noexcept
{
    NavigationAirObstacleQuery result;
    if (!contains(cell))
        return result;
    for (const uint32_t slot : m_entityOrder) {
        const EntityRecord& entity = m_entities[slot];
        if (!entity.occupied || !entity.blocksAirNavigation ||
            entity.entityId == 0 || entity.entityId == ignoredEntityId)
            continue;
        const NavigationCellId* first = entityCells(slot);
        if (!std::binary_search(
                first, first + entity.cellCount, cell))
            continue;
        result.entityId = entity.entityId;
        for (uint32_t index = 0; index < entity.cellCount; ++index)
            result.bounds.include(first[index], m_width);
        return result;
    }
    return result;
}

bool NavigationDynamicOverlay::bridgeActive(uint64_t entityId) const noexcept
{
    const size_t order = findBridgeOrder(entityId);
    return order != NoIndex && m_bridges[m_bridgeOrder[order]].active;
}

bool NavigationDynamicOverlay::containsEntity(uint64_t entityId) const noexcept
{
    return entityId != 0 && findEntityOrder(entityId) != NoIndex;
}

bool NavigationDynamicOverlay::ownsCell(uint64_t entityId, NavigationCellId cell) const noexcept
{
    const size_t order = findEntityOrder(entityId);
    if (order == NoIndex)
        return false;
    const uint32_t slot = m_entityOrder[order];
    const EntityRecord& entity = m_entities[slot];
    const NavigationCellId* first = entityCells(slot);
    return std::binary_search(first, first + entity.cellCount, cell);
}

bool NavigationDynamicOverlay::entityFenceSurface(
    uint64_t entityId) const noexcept
{
    const size_t order = findEntityOrder(entityId);
    return order != NoIndex &&
        m_entities[m_entityOrder[order]].fenceSurface;
}

bool NavigationDynamicOverlay::requiresAStarFallback(const NavigationPrecomputeMetadata& precompute) const noexcept
{
    return !m_initialized || !m_events.empty() || m_activeEvent || m_hasUnpublished ||
           precompute.revisions != m_revisions;
}

NavigationPathStaleResult NavigationDynamicOverlay::pathStaleness(const NavigationPathMetadata& path) const noexcept
{
    NavigationPathStaleResult result;
    result.requiresAStarFallback = !m_events.empty() || m_activeEvent || m_hasUnpublished;
    if (path.revisions == m_revisions)
        return result;
    if (result.requiresAStarFallback || !m_publishedDirty.published ||
        path.revisions != m_publishedDirty.before || m_revisions != m_publishedDirty.after)
    {
        result.stale = true;
        return result;
    }
    if (path.revisions.portalTopology != m_revisions.portalTopology)
    {
        result.stale = true;
        return result;
    }
    if (path.corridorChunkCount != 0) {
        result.stale = false;
        const size_t count = std::min<size_t>(
            path.corridorChunkCount,
            NavigationPathMetadata::MaximumCorridorChunks);
        for (size_t index = 0; index < count; ++index) {
            if (intersects(
                    path.corridorChunks[index],
                    m_publishedDirty.dirtyCells)) {
                result.stale = true;
                break;
            }
        }
    } else {
        result.stale = intersects(
            path.affectedCells, m_publishedDirty.dirtyCells);
    }
    return result;
}

void NavigationDynamicOverlay::refreshStableHash() noexcept
{
    uint64_t hash = 14695981039346656037ULL;
    feed(hash, HashSchemaVersion);
    feed(hash, m_width);
    feed(hash, m_height);
    feed(hash, m_revisions.staticNavigation.value);
    feed(hash, m_revisions.dynamicObstacles.value);
    feed(hash, m_revisions.portalTopology.value);
    feed(hash, m_lastCommittedTransaction.generation);
    feed(hash, m_lastCommittedTransaction.revision);
    feed(hash, m_sealedThroughTick);
    for (uint32_t count : m_ownerCount)
        feed(hash, count);
    for (uint32_t count : m_airOwnerCount)
        feed(hash, count);
    for (uint32_t count : m_rubbleOwnerCount)
        feed(hash, count);
    for (uint32_t count : m_fenceOwnerCount)
        feed(hash, count);
    feed(hash, static_cast<uint64_t>(m_entityOrder.size()));
    for (uint32_t slot : m_entityOrder)
    {
        const EntityRecord& entity = m_entities[slot];
        feed(hash, entity.entityId);
        feed(hash, static_cast<uint8_t>(entity.state));
        feed(hash, static_cast<uint8_t>(entity.blocksNavigation));
        feed(hash, static_cast<uint8_t>(entity.blocksAirNavigation));
        feed(hash, static_cast<uint8_t>(entity.rubbleSurface));
        feed(hash, static_cast<uint8_t>(entity.fenceSurface));
        feed(hash, entity.cellCount);
        feed(hash, entity.transaction.generation);
        feed(hash, entity.transaction.revision);
        const NavigationCellId* cells = entityCells(slot);
        for (uint32_t index = 0; index < entity.cellCount; ++index)
            feed(hash, cells[index].value);
    }
    feed(hash, static_cast<uint64_t>(m_bridgeOrder.size()));
    for (uint32_t slot : m_bridgeOrder)
    {
        const BridgeRecord& bridge = m_bridges[slot];
        feed(hash, bridge.entityId);
        feed(hash, static_cast<uint8_t>(bridge.active));
        feed(hash, bridge.transaction.generation);
        feed(hash, bridge.transaction.revision);
    }
    feed(hash, static_cast<uint64_t>(m_events.size()));
    for (const EventRecord& event : m_events)
    {
        feed(hash, event.confirmedTick);
        feed(hash, event.entityId);
        feed(hash, static_cast<uint8_t>(event.reason));
        feed(hash, static_cast<uint8_t>(event.kind));
        feed(hash, static_cast<uint8_t>(event.buildingState));
        feed(hash, static_cast<uint8_t>(event.value));
        feed(hash, static_cast<uint8_t>(event.replaceFootprint));
        feed(hash, static_cast<uint8_t>(event.blocksAirNavigation));
        feed(hash, static_cast<uint8_t>(event.rubbleSurface));
        feed(hash, static_cast<uint8_t>(event.fenceSurface));
        feed(hash, event.cellCount);
        feed(hash, event.transaction.generation);
        feed(hash, event.transaction.revision);
        const NavigationCellId* cells = eventCells(event);
        for (uint32_t index = 0; index < event.cellCount; ++index)
            feed(hash, cells[index].value);
    }
    feed(hash, static_cast<uint8_t>(m_activeEvent));
    if (m_activeEvent)
    {
        feed(hash, static_cast<uint8_t>(m_activeKind));
        feed(hash, static_cast<uint8_t>(m_activePhase));
        feed(hash, m_activeSlot);
        feed(hash, static_cast<uint8_t>(m_activeWasNew));
        feed(hash, m_activeOldCount);
        feed(hash, m_activeTargetCount);
        feed(hash, static_cast<uint8_t>(m_activeOldBlocks));
        feed(hash, static_cast<uint8_t>(m_activeTargetBlocks));
        feed(hash, static_cast<uint8_t>(m_activeOldAirBlocks));
        feed(hash, static_cast<uint8_t>(m_activeTargetAirBlocks));
        feed(hash, static_cast<uint8_t>(m_activeOldRubble));
        feed(hash, static_cast<uint8_t>(m_activeTargetRubble));
        feed(hash, static_cast<uint8_t>(m_activeOldFence));
        feed(hash, static_cast<uint8_t>(m_activeTargetFence));
        feed(hash, static_cast<uint8_t>(m_activeTopologyChanged));
        feed(hash, m_activeOldIndex);
        feed(hash, m_activeTargetIndex);
    }
    feed(hash, static_cast<uint8_t>(m_hasUnpublished));
    if (m_hasUnpublished)
    {
        feed(hash, m_dirtyBefore.staticNavigation.value);
        feed(hash, m_dirtyBefore.dynamicObstacles.value);
        feed(hash, m_dirtyBefore.portalTopology.value);
        feed(hash, static_cast<uint32_t>(m_unpublishedDirty.minX));
        feed(hash, static_cast<uint32_t>(m_unpublishedDirty.minY));
        feed(hash, static_cast<uint32_t>(m_unpublishedDirty.maxX));
        feed(hash, static_cast<uint32_t>(m_unpublishedDirty.maxY));
        feed(hash, m_dirtyCellsVisited);
    }
    feed(hash, static_cast<uint8_t>(m_publishedDirty.published));
    if (m_publishedDirty.published)
    {
        feed(hash, m_publishedDirty.before.staticNavigation.value);
        feed(hash, m_publishedDirty.before.dynamicObstacles.value);
        feed(hash, m_publishedDirty.before.portalTopology.value);
        feed(hash, m_publishedDirty.after.staticNavigation.value);
        feed(hash, m_publishedDirty.after.dynamicObstacles.value);
        feed(hash, m_publishedDirty.after.portalTopology.value);
        feed(hash, static_cast<uint32_t>(m_publishedDirty.dirtyCells.minX));
        feed(hash, static_cast<uint32_t>(m_publishedDirty.dirtyCells.minY));
        feed(hash, static_cast<uint32_t>(m_publishedDirty.dirtyCells.maxX));
        feed(hash, static_cast<uint32_t>(m_publishedDirty.dirtyCells.maxY));
    }
    feed(hash, static_cast<uint64_t>(m_bridgeDirtyChangeCount));
    for (size_t index = 0; index < m_bridgeDirtyChangeCount; ++index)
    {
        const NavigationBridgeDirtyChange& change = m_bridgeDirtyChanges[index];
        feed(hash, change.bridgeId);
        feed(hash, static_cast<uint32_t>(change.affectedCells.minX));
        feed(hash, static_cast<uint32_t>(change.affectedCells.minY));
        feed(hash, static_cast<uint32_t>(change.affectedCells.maxX));
        feed(hash, static_cast<uint32_t>(change.affectedCells.maxY));
    }
    m_stableHash = hash;
}

} // namespace engine::navigation
