#include "NavigationDynamicOverlay.h"

#include <algorithm>
#include <limits>

namespace engine::navigation
{

NavigationDynamicOverlayResult NavigationDynamicOverlay::initialize(const NavigationDynamicOverlayConfig& config)
{
    if (m_initialized)
        return NavigationDynamicOverlayResult::AlreadyInitialized;
    if (config.width == 0 || config.height == 0 || config.entityCapacity == 0 ||
        config.eventCapacity == 0 || config.maxCellsPerFootprint == 0 ||
        config.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        config.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return NavigationDynamicOverlayResult::InvalidCapacity;

    const uint64_t cellCount64 = static_cast<uint64_t>(config.width) * config.height;
    const uint64_t footprintCells64 =
        (static_cast<uint64_t>(config.entityCapacity) +
         config.eventCapacity) * config.maxCellsPerFootprint;
    if (cellCount64 >= std::numeric_limits<uint32_t>::max() ||
        cellCount64 > std::numeric_limits<size_t>::max() ||
        footprintCells64 > std::numeric_limits<uint32_t>::max() ||
        footprintCells64 > std::numeric_limits<size_t>::max())
        return NavigationDynamicOverlayResult::AllocationOverflow;

    m_width = config.width;
    m_height = config.height;
    m_eventCapacity = config.eventCapacity;
    m_ownerCount.assign(static_cast<size_t>(cellCount64), 0);
    m_airOwnerCount.assign(static_cast<size_t>(cellCount64), 0);
    m_rubbleOwnerCount.assign(static_cast<size_t>(cellCount64), 0);
    m_fenceOwnerCount.assign(static_cast<size_t>(cellCount64), 0);
    m_entities.assign(config.entityCapacity, {});
    m_entityOrder.clear();
    m_entityOrder.reserve(config.entityCapacity);
    m_freeEntities.resize(config.entityCapacity);
    for (uint32_t slot = 0; slot < config.entityCapacity; ++slot)
        m_freeEntities[slot] = config.entityCapacity - slot - 1U;

    m_bridges.assign(config.bridgeCapacity, {});
    m_bridgeOrder.clear();
    m_bridgeOrder.reserve(config.bridgeCapacity);
    m_freeBridges.resize(config.bridgeCapacity);
    for (uint32_t slot = 0; slot < config.bridgeCapacity; ++slot)
        m_freeBridges[slot] = config.bridgeCapacity - slot - 1U;

    m_events.clear();
    m_events.reserve(config.eventCapacity);
    const uint64_t allocationCapacity64 =
        static_cast<uint64_t>(config.entityCapacity) + config.eventCapacity;
    if (allocationCapacity64 > std::numeric_limits<uint32_t>::max() ||
        !m_footprintCells.initialize(
            footprintCells64,
            static_cast<uint32_t>(allocationCapacity64)))
        return NavigationDynamicOverlayResult::AllocationOverflow;
    m_bridgeDirtyChanges.assign(config.bridgeCapacity, {});
    m_bridgeDirtyChangeCount = 0;

    m_revisions = {{1}, {1}, {1}};
    m_initialized = true;
    refreshStableHash();
    return NavigationDynamicOverlayResult::Success;
}

NavigationDynamicOverlay::EventRecord NavigationDynamicOverlay::EventRecord::building(
    const NavigationBuildingEvent& event) noexcept
{
    return {event.confirmedTick,
            event.entityId,
            event.reason,
            EventKind::Building,
            event.state,
            event.blocksNavigation,
            event.replaceFootprint,
            {},
            0,
            event.transaction,
            event.blocksAirNavigation,
            event.rubbleSurface,
            event.fenceSurface};
}

NavigationDynamicOverlay::EventRecord NavigationDynamicOverlay::EventRecord::bridge(
    const NavigationBridgeStateEvent& event) noexcept
{
    return {event.confirmedTick,
            event.entityId,
            NavigationDynamicEventReason::BridgeStateChanged,
            EventKind::Bridge,
            NavigationBuildingState::Absent,
            event.active,
            true,
            {},
            0,
            event.transaction,
            false,
            false,
            false};
}

bool NavigationDynamicOverlay::contains(NavigationCellId cell) const noexcept
{
    return cell && static_cast<size_t>(cell.value) < m_ownerCount.size();
}

bool NavigationDynamicOverlay::validBounds(NavigationCellBounds bounds) const noexcept
{
    return bounds.valid() && bounds.minX >= 0 && bounds.minY >= 0 &&
           static_cast<uint32_t>(bounds.maxX) < m_width && static_cast<uint32_t>(bounds.maxY) < m_height;
}

NavigationCellId* NavigationDynamicOverlay::entityCells(uint32_t slot) noexcept
{
    return m_footprintCells.data(m_entities[slot].cells);
}

const NavigationCellId* NavigationDynamicOverlay::entityCells(uint32_t slot) const noexcept
{
    return m_footprintCells.data(m_entities[slot].cells);
}

NavigationCellId* NavigationDynamicOverlay::eventCells(const EventRecord& event) noexcept
{
    return m_footprintCells.data(event.cells);
}

const NavigationCellId* NavigationDynamicOverlay::eventCells(const EventRecord& event) const noexcept
{
    return m_footprintCells.data(event.cells);
}

size_t NavigationDynamicOverlay::findEntityOrder(uint64_t entityId) const noexcept
{
    const auto position = std::lower_bound(
        m_entityOrder.begin(), m_entityOrder.end(), entityId,
        [this](uint32_t slot, uint64_t sought) { return m_entities[slot].entityId < sought; });
    return position != m_entityOrder.end() && m_entities[*position].entityId == entityId
               ? static_cast<size_t>(position - m_entityOrder.begin())
               : NoIndex;
}

size_t NavigationDynamicOverlay::findBridgeOrder(uint64_t entityId) const noexcept
{
    const auto position = std::lower_bound(
        m_bridgeOrder.begin(), m_bridgeOrder.end(), entityId,
        [this](uint32_t slot, uint64_t sought) { return m_bridges[slot].entityId < sought; });
    return position != m_bridgeOrder.end() && m_bridges[*position].entityId == entityId
               ? static_cast<size_t>(position - m_bridgeOrder.begin())
               : NoIndex;
}

} // namespace engine::navigation
