#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

#include "NavigationSystem.h"

namespace engine::navigation
{

NavigationSystemStatus NavigationSystem::stageStaticCell(NavigationCellId cell,
                                                         const NavigationCellValue& value) noexcept
{
    if (!m_initialized)
        return NavigationSystemStatus::NotInitialized;
    if (m_dirtyPublicationActive)
        return NavigationSystemStatus::PublicationPending;
    NavigationGrid* staticGrid = m_staticLayers.findMutable(m_primaryLayer);
    if (staticGrid == nullptr || value.layer != m_primaryLayer ||
        staticGrid->setCell(cell, value) != NavigationGridResult::Success)
        return NavigationSystemStatus::StaticChangeFailed;
    m_stagedStaticDirty.include(cell, staticGrid->width());
    return NavigationSystemStatus::Success;
}

NavigationSystemStatus NavigationSystem::stageStaticCells(
    container::Span<const NavigationStaticCellUpdate> updates) noexcept
{
    if (!m_initialized)
        return NavigationSystemStatus::NotInitialized;
    if (m_dirtyPublicationActive)
        return NavigationSystemStatus::PublicationPending;
    NavigationGrid* staticGrid = m_staticLayers.findMutable(m_primaryLayer);
    if (staticGrid == nullptr)
        return NavigationSystemStatus::StaticChangeFailed;
    NavigationCellId previous = InvalidNavigationCell;
    for (const NavigationStaticCellUpdate& update : updates)
    {
        if (!staticGrid->contains(update.cell) || update.value.layer != m_primaryLayer ||
            (previous && update.cell.value <= previous.value))
            return NavigationSystemStatus::StaticChangeFailed;
        previous = update.cell;
    }
    for (const NavigationStaticCellUpdate& update : updates)
    {
        if (staticGrid->setCell(update.cell, update.value) != NavigationGridResult::Success)
            return NavigationSystemStatus::StaticChangeFailed;
        m_stagedStaticDirty.include(update.cell, staticGrid->width());
    }
    return NavigationSystemStatus::Success;
}

NavigationSystemStatus NavigationSystem::addStartupElevatedLayer(NavigationLayerId layer,
                                                                 const NavigationGrid& staticGrid)
{
    if (!m_initialized)
        return NavigationSystemStatus::NotInitialized;
    if (!startupMutable() || !layer || layer == m_primaryLayer || m_layers.find(layer) != nullptr ||
        m_staticLayers.find(layer) != nullptr || m_layers.size() == m_layers.capacity() ||
        !compatibleLayerGrid(staticGrid, layer))
        return NavigationSystemStatus::StaticChangeFailed;

    NavigationGrid staticLayer = staticGrid;
    NavigationGrid runtimeGrid = staticGrid;
    if (!staticLayer.rebuildClearance() || !runtimeGrid.rebuildClearance())
        return NavigationSystemStatus::ZoneBuildFailed;

    container::Array<NavigationZoneField,
                     NavigationClearanceProfiles.size()> clearanceZones;
    for (size_t index = 0; index < clearanceZones.size(); ++index)
    {
        const NavigationClearanceClass clearance =
            static_cast<NavigationClearanceClass>(index);
        if (clearanceZones[index].initialize(runtimeGrid.cellCount()) !=
                NavigationZoneBuildResult::Success ||
            clearanceZones[index].build(runtimeGrid, m_primaryProfile,
                                        m_primaryMovementMask, layer,
                                        m_zoneBuildFrontier,
                                        clearance) !=
                NavigationZoneBuildResult::Success)
            return NavigationSystemStatus::ZoneBuildFailed;
    }

    if (m_staticLayers.addLayer(layer, std::move(staticLayer)) != NavigationLayerSetResult::Success ||
        m_layers.addLayer(layer, std::move(runtimeGrid)) != NavigationLayerSetResult::Success)
        return NavigationSystemStatus::StaticChangeFailed;
    for (NavigationZoneField& zones : clearanceZones)
    {
        const auto key = std::tuple(layer, zones.clearanceClass());
        const auto position = std::lower_bound(
            m_layerZones.begin(), m_layerZones.end(), key,
            [](const NavigationZoneField& value, const auto& sought)
            {
                return std::tuple(value.layer(), value.clearanceClass()) < sought;
            });
        m_layerZones.insert(position, std::move(zones));
    }
    m_startupTopologyDirty = true;
    return NavigationSystemStatus::Success;
}

NavigationSystemStatus NavigationSystem::addStartupPortal(const NavigationPortal& portal)
{
    if (!m_initialized)
        return NavigationSystemStatus::NotInitialized;
    if (!startupMutable() || m_portals.addPortal(m_layers, portal) != NavigationPortalSetResult::Success)
        return NavigationSystemStatus::StaticChangeFailed;
    m_startupTopologyDirty = true;
    return NavigationSystemStatus::Success;
}

NavigationSystemStatus NavigationSystem::publishStagedStaticTopology() noexcept
{
    if (!m_initialized)
        return NavigationSystemStatus::NotInitialized;
    if (!startupMutable())
        return NavigationSystemStatus::StaticChangeFailed;
    const bool topologyChanged = m_startupTopologyDirty;
    if (!m_stagedStaticDirty.valid())
        return topologyChanged ? publishStartupTopology() : NavigationSystemStatus::Success;
    if (m_dynamicOverlay.noteStaticNavigationChange(m_stagedStaticDirty) != NavigationDynamicOverlayResult::Success)
        return NavigationSystemStatus::StaticChangeFailed;
    m_stagedStaticDirty = {};
    const NavigationDirtyRebuildResult rebuilt =
        m_dynamicOverlay.rebuildDirtyRegion(std::numeric_limits<uint64_t>::max());
    if (rebuilt.status != NavigationDirtyRebuildStatus::Published)
        return NavigationSystemStatus::StaticChangeFailed;
    const NavigationSystemStatus status = publishDirtyTopology();
    if (status != NavigationSystemStatus::Success)
        return status;
    if (m_startupTopologyDirty)
        return publishStartupTopology(false);
    return NavigationSystemStatus::Success;
}

NavigationSystemStatus
NavigationSystem::publishStagedStartupDynamicTopology() noexcept
{
    if (!m_initialized)
        return NavigationSystemStatus::NotInitialized;
    const bool pathServiceBusy = std::any_of(
        m_pathServices.begin(), m_pathServices.end(),
        [](const NavigationPathService& pathService) {
            return pathService.queuedCount() != 0 ||
                pathService.feedbackCount() != 0 ||
                pathService.hasActiveJob();
        });
    if (m_lastConfirmedTick != 0 || pathServiceBusy ||
        m_dirtyPublicationActive || m_dynamicOverlay.commitInProgress())
        return NavigationSystemStatus::PublicationPending;

    // Map-object terrain flattening is authored after the base terrain grid
    // was initialized. Fold those static-height cells into the same startup
    // revision as structure occupancy; publishing them separately would run
    // clearance and zone construction twice over nearly identical bounds.
    if (m_stagedStaticDirty.valid()) {
        if (m_dynamicOverlay.noteStaticNavigationChange(
                m_stagedStaticDirty) !=
            NavigationDynamicOverlayResult::Success) {
            return NavigationSystemStatus::StaticChangeFailed;
        }
        m_stagedStaticDirty = {};
    }

    const NavigationTopologyLedgerResult submitted =
        submitReservedTopologyTransactions(1u);
    if (submitted.status != NavigationTopologyLedgerStatus::Idle &&
        submitted.status != NavigationTopologyLedgerStatus::Complete)
        return NavigationSystemStatus::TopologyTransactionFailed;
    if (!m_topologyLedger.empty())
        return NavigationSystemStatus::TopologyTransactionFailed;

    const NavigationDynamicCommitResult committed =
        m_dynamicOverlay.commitStartupEvents(
            std::numeric_limits<uint32_t>::max(),
            std::numeric_limits<uint32_t>::max());
    if (!validCommitStatus(committed.status) ||
        committed.status == NavigationDynamicCommitStatus::BudgetExhausted ||
        committed.pendingEvents != 0)
        return NavigationSystemStatus::DynamicCommitFailed;

    if (!m_dynamicOverlay.hasUnpublishedDirty())
        return NavigationSystemStatus::Success;
    const NavigationDirtyRebuildResult rebuilt =
        m_dynamicOverlay.rebuildDirtyRegion(
            std::numeric_limits<uint64_t>::max());
    if (rebuilt.status != NavigationDirtyRebuildStatus::Published) {
        noteGridPublicationFailure(
            NavigationGridPublicationFailureReason::DirtyRebuildFailed);
        return NavigationSystemStatus::GridPublicationFailed;
    }
    return publishDirtyTopology();
}

NavigationDynamicOverlayResult NavigationSystem::submitBuildingEvent(
    const NavigationBuildingEvent& event, container::Span<const NavigationCellId> footprint) noexcept
{
    return reserveBuildingTransaction(event, footprint);
}

NavigationDynamicOverlayResult NavigationSystem::submitBridgeEvent(
    const NavigationBridgeStateEvent& event, container::Span<const NavigationCellId> affectedCells) noexcept
{
    return reserveBridgeTransaction(event, affectedCells);
}

NavigationDynamicOverlayResult NavigationSystem::submitBridgeEvent(
    const NavigationBridgeStateEvent& event,
    NavigationLayerId bridgeLayer,
    container::Span<const NavigationCellId> affectedCells) noexcept
{
    if (!m_initialized)
        return NavigationDynamicOverlayResult::NotInitialized;
    const auto position = bridgeBindingPosition(event.entityId);
    const bool exists = position != m_bridgeLayers.end() && position->bridgeId == event.entityId;
    if (!event.entityId || !bridgeLayer || bridgeLayer == m_primaryLayer ||
        m_staticLayers.find(bridgeLayer) == nullptr || m_layers.find(bridgeLayer) == nullptr ||
        !hasPortalForLayer(bridgeLayer) || (exists && position->layer != bridgeLayer))
        return NavigationDynamicOverlayResult::InvalidEvent;
    if (!exists && m_bridgeLayers.size() == m_bridgeLayers.capacity())
        return NavigationDynamicOverlayResult::BridgeCapacityExceeded;
    if (!exists && std::any_of(m_bridgeLayers.begin(),
                               m_bridgeLayers.end(),
                               [bridgeLayer](const NavigationBridgeLayerBinding& binding)
                               { return binding.layer == bridgeLayer; }))
        return NavigationDynamicOverlayResult::InvalidEvent;

    const NavigationDynamicOverlayResult reserved = reserveBridgeTransaction(event, affectedCells);
    if (reserved == NavigationDynamicOverlayResult::Success && !exists)
        // Startup bridge layers are authored active. The first runtime event
        // is therefore compared against true and only a real state change is
        // published as changed-layer work.
        m_bridgeLayers.insert(position, {event.entityId, bridgeLayer, true});
    return reserved;
}

NavigationDynamicOverlayResult NavigationSystem::reserveBuildingTransaction(
    const NavigationBuildingEvent& event,
    container::Span<const NavigationCellId> footprint) noexcept
{
    if (!m_initialized)
        return NavigationDynamicOverlayResult::NotInitialized;
    const bool validReason =
        event.reason == NavigationDynamicEventReason::BuildingPlaced ||
        event.reason == NavigationDynamicEventReason::FootprintChanged ||
        event.reason == NavigationDynamicEventReason::CompletionStateChanged ||
        event.reason == NavigationDynamicEventReason::BuildingDestroyed;
    const bool validState =
        event.state == NavigationBuildingState::Absent ||
        event.state == NavigationBuildingState::Placed ||
        event.state == NavigationBuildingState::Complete;
    if (event.confirmedTick == 0 || event.entityId == 0 || event.transaction ||
        !validReason || !validState ||
        (event.reason == NavigationDynamicEventReason::BuildingDestroyed &&
         event.state != NavigationBuildingState::Absent) ||
        (event.state == NavigationBuildingState::Absent &&
         event.reason != NavigationDynamicEventReason::BuildingDestroyed) ||
        (event.rubbleSurface &&
         (event.state == NavigationBuildingState::Absent ||
          event.blocksNavigation || event.blocksAirNavigation)) ||
        (event.fenceSurface &&
         (event.state == NavigationBuildingState::Absent ||
          !event.blocksNavigation || event.rubbleSurface)) ||
        (!event.replaceFootprint && !footprint.empty()))
        return NavigationDynamicOverlayResult::InvalidEvent;

    return reserveTopologyTransaction(
        {event.confirmedTick,
         event.entityId,
         event.reason,
         event.state,
         {},
         NavigationTopologyTransactionKind::Building,
         event.blocksNavigation,
         event.replaceFootprint,
         event.blocksAirNavigation,
         event.rubbleSurface,
         event.fenceSurface},
        footprint);
}

NavigationDynamicOverlayResult NavigationSystem::reserveBridgeTransaction(
    const NavigationBridgeStateEvent& event,
    container::Span<const NavigationCellId> affectedCells) noexcept
{
    if (!m_initialized)
        return NavigationDynamicOverlayResult::NotInitialized;
    if (event.confirmedTick == 0 || event.entityId == 0 || event.transaction)
        return NavigationDynamicOverlayResult::InvalidEvent;
    return reserveTopologyTransaction(
        {event.confirmedTick,
         event.entityId,
         NavigationDynamicEventReason::BridgeStateChanged,
         NavigationBuildingState::Absent,
         {},
         NavigationTopologyTransactionKind::Bridge,
         event.active,
         true},
        affectedCells);
}

NavigationDynamicOverlayResult NavigationSystem::reserveTopologyTransaction(
    NavigationTopologyTransactionRecord record,
    container::Span<const NavigationCellId> cells) noexcept
{
    const uint64_t ownedTransactions =
        static_cast<uint64_t>(m_topologyLedger.size()) +
        static_cast<uint64_t>(m_dynamicOverlay.pendingEventCount());
    if (ownedTransactions >= m_topologyLedgerCapacity)
        return NavigationDynamicOverlayResult::EventCapacityExceeded;
    if (cells.size() > std::numeric_limits<uint32_t>::max())
        return NavigationDynamicOverlayResult::FootprintCapacityExceeded;
    if (m_topologyGeneration == 0 || m_nextTopologyRevision == 0 ||
        m_nextTopologyRevision == std::numeric_limits<uint64_t>::max())
        return NavigationDynamicOverlayResult::RevisionExhausted;

    const NavigationGrid* primary = m_layers.find(m_primaryLayer);
    if (primary == nullptr)
        return NavigationDynamicOverlayResult::NotInitialized;
    for (NavigationCellId cell : cells)
    {
        if (!primary->contains(cell))
            return NavigationDynamicOverlayResult::InvalidCell;
    }

    NavigationCellArena::Range allocation;
    if (!m_topologyLedgerCells.allocate(
            static_cast<uint32_t>(cells.size()), allocation))
        return NavigationDynamicOverlayResult::FootprintCapacityExceeded;
    record.cells = allocation;
    NavigationCellId* destination = topologyLedgerCells(record);
    std::copy(cells.begin(), cells.end(), destination);
    std::sort(destination, destination + cells.size());
    if (std::adjacent_find(destination, destination + cells.size()) !=
        destination + cells.size())
    {
        m_topologyLedgerCells.release(record.cells);
        return NavigationDynamicOverlayResult::DuplicateCell;
    }

    record.stamp = {m_topologyGeneration, m_nextTopologyRevision++};
    record.cellCount = static_cast<uint32_t>(cells.size());
    const auto transactionLess =
        [](const NavigationTopologyTransactionRecord& left,
           const NavigationTopologyTransactionRecord& right)
        {
            // The stamp is the durable transaction identity and therefore
            // also the canonical commit order.  Sorting by entity before the
            // stamp can commit revision N+1 first; if capacity then delays N,
            // its retry would be indistinguishable from an already committed
            // transaction at the overlay high-water mark.
            return std::tuple(left.stamp.generation,
                              left.stamp.revision,
                              left.sourceConfirmedTick,
                              left.entityId,
                              static_cast<uint8_t>(left.reason)) <
                   std::tuple(right.stamp.generation,
                              right.stamp.revision,
                              right.sourceConfirmedTick,
                              right.entityId,
                              static_cast<uint8_t>(right.reason));
        };
    const auto position = std::lower_bound(
        m_topologyLedger.begin(), m_topologyLedger.end(), record,
        transactionLess);
    m_topologyLedger.insert(position, record);
    return NavigationDynamicOverlayResult::Success;
}

NavigationTopologyLedgerResult NavigationSystem::submitReservedTopologyTransactions(
    uint64_t confirmedTick) noexcept
{
    NavigationTopologyLedgerResult result;
    result.pendingTransactions = static_cast<uint32_t>(m_topologyLedger.size());
    while (!m_topologyLedger.empty())
    {
        NavigationTopologyTransactionRecord& record = m_topologyLedger.front();
        if (record.sourceConfirmedTick > confirmedTick)
        {
            result.status = NavigationTopologyLedgerStatus::RetryPending;
            break;
        }

        const container::Span<const NavigationCellId> cells{
            topologyLedgerCells(record), record.cellCount};
        NavigationDynamicOverlayResult submitted = NavigationDynamicOverlayResult::InvalidEvent;
        if (record.kind == NavigationTopologyTransactionKind::Building)
        {
            submitted = m_dynamicOverlay.submitBuildingEvent(
                {confirmedTick,
                 record.entityId,
                 record.reason,
                 record.buildingState,
                 record.value,
                 record.replaceFootprint,
                 record.stamp,
                 record.blocksAirNavigation,
                 record.rubbleSurface,
                 record.fenceSurface},
                cells);
        }
        else
        {
            submitted = m_dynamicOverlay.submitBridgeEvent(
                {confirmedTick, record.entityId, record.value, record.stamp}, cells);
        }

        result.overlayResult = submitted;
        // DuplicateEventKey means this exact transaction stamp is already
        // owned by the overlay (queued or committed).  Treat it as a durable
        // acknowledgement and retire the ledger record.  Retrying until the
        // queued copy disappears would replay the same topology mutation and
        // could increment building ownership twice.
        if (submitted == NavigationDynamicOverlayResult::Success ||
            submitted == NavigationDynamicOverlayResult::DuplicateEventKey)
        {
            result.lastSubmittedTransaction = record.stamp;
            if (submitted == NavigationDynamicOverlayResult::Success)
                ++result.submittedTransactions;
            releaseFrontTopologyTransaction();
            continue;
        }
        if (retryableTopologySubmission(submitted))
        {
            if (record.retryCount != std::numeric_limits<uint32_t>::max())
                ++record.retryCount;
            ++result.retriedTransactions;
            result.status = NavigationTopologyLedgerStatus::RetryPending;
            break;
        }

        switch (submitted)
        {
        case NavigationDynamicOverlayResult::EventCapacityExceeded:
        case NavigationDynamicOverlayResult::EntityCapacityExceeded:
        case NavigationDynamicOverlayResult::BridgeCapacityExceeded:
        case NavigationDynamicOverlayResult::FootprintCapacityExceeded:
        case NavigationDynamicOverlayResult::RefCountOverflow:
        case NavigationDynamicOverlayResult::InvalidCapacity:
        case NavigationDynamicOverlayResult::AllocationOverflow:
            result.status = NavigationTopologyLedgerStatus::CapacityExceeded;
            break;
        case NavigationDynamicOverlayResult::RevisionExhausted:
            result.status = NavigationTopologyLedgerStatus::RevisionExhausted;
            break;
        case NavigationDynamicOverlayResult::InvalidEvent:
        case NavigationDynamicOverlayResult::InvalidCell:
        case NavigationDynamicOverlayResult::DuplicateCell:
            result.status = NavigationTopologyLedgerStatus::InvalidEvent;
            break;
        default:
            result.status = NavigationTopologyLedgerStatus::CommitFailed;
            break;
        }
        result.pendingTransactions = static_cast<uint32_t>(m_topologyLedger.size());
        return result;
    }

    result.pendingTransactions = static_cast<uint32_t>(m_topologyLedger.size());
    if (result.status == NavigationTopologyLedgerStatus::Idle &&
        result.submittedTransactions != 0)
        result.status = NavigationTopologyLedgerStatus::Complete;
    return result;
}

NavigationCellId* NavigationSystem::topologyLedgerCells(
    const NavigationTopologyTransactionRecord& record) noexcept
{
    return m_topologyLedgerCells.data(record.cells);
}

const NavigationCellId* NavigationSystem::topologyLedgerCells(
    const NavigationTopologyTransactionRecord& record) const noexcept
{
    return m_topologyLedgerCells.data(record.cells);
}

void NavigationSystem::releaseFrontTopologyTransaction() noexcept
{
    m_topologyLedgerCells.release(m_topologyLedger.front().cells);
    m_topologyLedger.erase(m_topologyLedger.begin());
}

NavigationSystemStatus NavigationSystem::publishDirtyTopology() noexcept
{
    const NavigationRevisionDelta publication = m_dynamicOverlay.publishedDirty();
    if (!publication.published) {
        noteGridPublicationFailure(
            NavigationGridPublicationFailureReason::DirtyRevisionMissing);
        return NavigationSystemStatus::GridPublicationFailed;
    }
    const bool primaryDirty = publication.dirtyCells.valid();
    const bool portalChanged = publication.after.portalTopology != m_publishedPortalRevision;
    if (!primaryDirty && !portalChanged) {
        noteGridPublicationFailure(
            NavigationGridPublicationFailureReason::DirtyRevisionMissing);
        return NavigationSystemStatus::GridPublicationFailed;
    }
    if (m_pathRevision.value == std::numeric_limits<uint64_t>::max())
        return NavigationSystemStatus::RevisionExhausted;

    NavigationGrid* primaryGrid = m_layers.findMutable(m_primaryLayer);
    const NavigationGrid* staticPrimary = m_staticLayers.find(m_primaryLayer);
    if (primaryGrid == nullptr || staticPrimary == nullptr) {
        noteGridPublicationFailure(
            NavigationGridPublicationFailureReason::PrimaryLayerMissing,
            m_primaryLayer);
        return NavigationSystemStatus::GridPublicationFailed;
    }
    if (primaryDirty)
    {
        const NavigationCellBounds bounds = publication.dirtyCells;
        for (int32_t y = bounds.minY; y <= bounds.maxY; ++y)
        {
            for (int32_t x = bounds.minX; x <= bounds.maxX; ++x)
            {
                const NavigationCellId cell = primaryGrid->cellId({x, y});
                if (!cell) {
                    noteGridPublicationFailure(
                        NavigationGridPublicationFailureReason::PrimaryCellInvalid,
                        m_primaryLayer, cell);
                    return NavigationSystemStatus::GridPublicationFailed;
                }
                NavigationCellValue value = staticPrimary->cell(cell);
                if (m_dynamicOverlay.rubbleSurface(cell)) {
                    value.passability =
                        NavigationPassability::Traversable;
                    value.movementMask = NavigationMovement::Rubble |
                        NavigationMovement::Air;
                }
                if (m_dynamicOverlay.blocked(cell))
                    value.passability = NavigationPassability::Blocked;
                if (m_dynamicOverlay.blocksAirNavigation(cell))
                    value.movementMask &= ~NavigationMovement::Air;
                if (primaryGrid->setCell(cell, value) != NavigationGridResult::Success) {
                    noteGridPublicationFailure(
                        NavigationGridPublicationFailureReason::PrimaryCellWriteFailed,
                        m_primaryLayer, cell);
                    return NavigationSystemStatus::GridPublicationFailed;
                }
            }
        }
        const int32_t expansion = static_cast<int32_t>(
            NavigationClearance::MaximumRadiusCells);
        const int32_t clearanceMinX = std::max<int32_t>(0, bounds.minX - expansion);
        const int32_t clearanceMinY = std::max<int32_t>(0, bounds.minY - expansion);
        const int32_t clearanceMaxX = std::min<int32_t>(
            static_cast<int32_t>(primaryGrid->width()) - 1,
            bounds.maxX + expansion);
        const int32_t clearanceMaxY = std::min<int32_t>(
            static_cast<int32_t>(primaryGrid->height()) - 1,
            bounds.maxY + expansion);
        if (!primaryGrid->rebuildClearanceRegion(
                clearanceMinX, clearanceMinY,
                clearanceMaxX, clearanceMaxY)) {
            noteGridPublicationFailure(
                NavigationGridPublicationFailureReason::PrimaryClearanceFailed,
                m_primaryLayer);
            return NavigationSystemStatus::GridPublicationFailed;
        }
        const NavigationSystemStatus zoneStatus =
            rebuildLayerZones(m_primaryLayer);
        if (zoneStatus != NavigationSystemStatus::Success)
            return zoneStatus;
    }

    if (portalChanged)
    {
        const NavigationSystemStatus bridgePublication = publishBridgeLayers();
        if (bridgePublication != NavigationSystemStatus::Success)
            return bridgePublication;
        m_publishedPortalRevision = publication.after.portalTopology;
    }
    ++m_pathRevision.value;
    for (NavigationPathService& pathService : m_pathServices)
        pathService.setNavigationRevision(m_pathRevision);
    m_waterRasterPendingDirty = {};
    return NavigationSystemStatus::Success;
}

NavigationSystemStatus NavigationSystem::beginDirtyTopologyPublication() noexcept
{
    if (m_dirtyPublicationActive)
        return NavigationSystemStatus::PublicationPending;
    const NavigationRevisionDelta publication = m_dynamicOverlay.publishedDirty();
    if (!publication.published ||
        (!publication.dirtyCells.valid() &&
         publication.after.portalTopology == m_publishedPortalRevision)) {
        noteGridPublicationFailure(
            NavigationGridPublicationFailureReason::DirtyRevisionMissing);
        return NavigationSystemStatus::GridPublicationFailed;
    }
    if (m_pathRevision.value == std::numeric_limits<uint64_t>::max())
        return NavigationSystemStatus::RevisionExhausted;

    const NavigationGrid* primaryGrid = m_layers.find(m_primaryLayer);
    if (primaryGrid == nullptr) {
        noteGridPublicationFailure(
            NavigationGridPublicationFailureReason::PrimaryLayerMissing,
            m_primaryLayer);
        return NavigationSystemStatus::GridPublicationFailed;
    }
    m_pendingLayers = m_layers;
    m_pendingLayerZones = m_layerZones;
    m_pendingDirtyPublication = publication;
    m_pendingRasterBounds = publication.dirtyCells;
    m_pendingClearanceBounds = {};
    m_pendingRasterCursor = 0;
    m_pendingClearanceCursor = 0;
    m_pendingZoneIndex = 0;
    m_pendingZoneBegin = 0;
    m_pendingZoneEnd = 0;
    m_pendingBridgeChanges.clear();
    const auto publishedBridgeChanges =
        m_dynamicOverlay.publishedBridgeChanges();
    m_pendingBridgeChanges.assign(publishedBridgeChanges.begin(),
                                 publishedBridgeChanges.end());
    m_pendingBridgeChangeIndex = 0;
    m_pendingBridgeRasterBounds = {};
    m_pendingBridgeClearanceBounds = {};
    m_pendingBridgeRasterCursor = 0;
    m_pendingBridgeClearanceCursor = 0;
    m_pendingBridgeZoneBegin = 0;
    m_pendingBridgeZoneIndex = 0;
    m_pendingBridgeZoneEnd = 0;
    m_pendingBridgePhase = 0;
    if (publication.dirtyCells.valid())
    {
        const int32_t expansion = static_cast<int32_t>(
            NavigationClearance::MaximumRadiusCells);
        m_pendingClearanceBounds = {
            std::max<int32_t>(0, publication.dirtyCells.minX - expansion),
            std::max<int32_t>(0, publication.dirtyCells.minY - expansion),
            std::min<int32_t>(static_cast<int32_t>(primaryGrid->width()) - 1,
                              publication.dirtyCells.maxX + expansion),
            std::min<int32_t>(static_cast<int32_t>(primaryGrid->height()) - 1,
                              publication.dirtyCells.maxY + expansion)};
        const auto key = std::tuple(m_primaryLayer,
                                    NavigationClearanceClass::Centered1x1);
        const auto position = std::lower_bound(
            m_pendingLayerZones.begin(), m_pendingLayerZones.end(), key,
            [](const NavigationZoneField& value, const auto& sought) {
                return std::tuple(value.layer(), value.clearanceClass()) < sought;
            });
        if (position == m_pendingLayerZones.end() ||
            position->layer() != m_primaryLayer)
            return NavigationSystemStatus::ZoneBuildFailed;
        m_pendingZoneBegin = static_cast<size_t>(
            position - m_pendingLayerZones.begin());
        m_pendingZoneIndex = m_pendingZoneBegin;
        m_pendingZoneEnd = m_pendingZoneBegin + NavigationClearanceProfiles.size();
    }
    else
    {
        m_pendingZoneBegin = m_pendingLayerZones.size();
        m_pendingZoneIndex = m_pendingZoneBegin;
        m_pendingZoneEnd = m_pendingZoneBegin;
    }
    m_dirtyPublicationActive = true;
    return NavigationSystemStatus::PublicationPending;
}

NavigationSystemStatus NavigationSystem::advanceDirtyTopologyPublication(
    uint64_t workBudget) noexcept
{
    if (!m_dirtyPublicationActive)
    {
        const NavigationSystemStatus started = beginDirtyTopologyPublication();
        if (started != NavigationSystemStatus::PublicationPending)
            return started;
    }
    if (workBudget == 0)
        return NavigationSystemStatus::PublicationPending;

    NavigationGrid* pendingGrid = m_pendingLayers.findMutable(m_primaryLayer);
    const NavigationGrid* staticGrid = m_staticLayers.find(m_primaryLayer);
    if (pendingGrid == nullptr || staticGrid == nullptr) {
        noteGridPublicationFailure(
            NavigationGridPublicationFailureReason::PendingLayerMissing,
            m_primaryLayer);
        return NavigationSystemStatus::GridPublicationFailed;
    }

    uint64_t remaining = workBudget;
    if (m_pendingRasterBounds.valid() &&
        m_pendingRasterCursor < m_pendingRasterBounds.cellCount())
    {
        const uint64_t width = static_cast<uint64_t>(
            m_pendingRasterBounds.maxX - m_pendingRasterBounds.minX + 1);
        while (remaining != 0 &&
               m_pendingRasterCursor < m_pendingRasterBounds.cellCount())
        {
            const uint64_t cursor = m_pendingRasterCursor++;
            const int32_t x = m_pendingRasterBounds.minX +
                static_cast<int32_t>(cursor % width);
            const int32_t y = m_pendingRasterBounds.minY +
                static_cast<int32_t>(cursor / width);
            const NavigationCellId cell = pendingGrid->cellId({x, y});
            if (!cell) {
                noteGridPublicationFailure(
                    NavigationGridPublicationFailureReason::PendingCellInvalid,
                    m_primaryLayer, cell);
                return NavigationSystemStatus::GridPublicationFailed;
            }
            NavigationCellValue value = staticGrid->cell(cell);
            if (m_dynamicOverlay.rubbleSurface(cell)) {
                value.passability = NavigationPassability::Traversable;
                value.movementMask = NavigationMovement::Rubble |
                    NavigationMovement::Air;
            }
            if (m_dynamicOverlay.blocked(cell))
                value.passability = NavigationPassability::Blocked;
            if (m_dynamicOverlay.blocksAirNavigation(cell))
                value.movementMask &= ~NavigationMovement::Air;
            if (pendingGrid->setCell(cell, value) != NavigationGridResult::Success) {
                noteGridPublicationFailure(
                    NavigationGridPublicationFailureReason::PendingCellWriteFailed,
                    m_primaryLayer, cell);
                return NavigationSystemStatus::GridPublicationFailed;
            }
            --remaining;
        }
    }

    if (remaining != 0 && m_pendingClearanceBounds.valid() &&
        m_pendingClearanceCursor < m_pendingClearanceBounds.cellCount())
    {
        const uint64_t width = static_cast<uint64_t>(
            m_pendingClearanceBounds.maxX - m_pendingClearanceBounds.minX + 1);
        while (remaining != 0 &&
               m_pendingClearanceCursor < m_pendingClearanceBounds.cellCount())
        {
            const uint64_t cursor = m_pendingClearanceCursor++;
            const int32_t x = m_pendingClearanceBounds.minX +
                static_cast<int32_t>(cursor % width);
            const int32_t y = m_pendingClearanceBounds.minY +
                static_cast<int32_t>(cursor / width);
            const NavigationCellId cell = pendingGrid->cellId({x, y});
            if (!pendingGrid->rebuildClearanceCell(cell)) {
                noteGridPublicationFailure(
                    NavigationGridPublicationFailureReason::PendingClearanceFailed,
                    m_primaryLayer, cell);
                return NavigationSystemStatus::GridPublicationFailed;
            }
            --remaining;
        }
    }

    if (remaining != 0 && m_pendingZoneIndex < m_pendingZoneEnd)
    {
        NavigationZoneField& zones = m_pendingLayerZones[m_pendingZoneIndex];
        if (!zones.isBuildInProgress())
        {
            const size_t profileOffset = m_pendingZoneIndex - m_pendingZoneBegin;
            const NavigationZoneBuildResult started = zones.beginBuild(
                *pendingGrid, m_primaryProfile, m_primaryMovementMask,
                m_primaryLayer, NavigationClearanceProfiles[profileOffset]);
            if (started != NavigationZoneBuildResult::Success)
                return NavigationSystemStatus::ZoneBuildFailed;
        }
        size_t consumed = 0;
        const NavigationZoneBuildResult stepped = zones.stepBuild(
            *pendingGrid, m_zoneBuildFrontier, static_cast<size_t>(remaining),
            consumed);
        if (stepped != NavigationZoneBuildResult::Success)
            return NavigationSystemStatus::ZoneBuildFailed;
        remaining -= std::min<uint64_t>(remaining, consumed);
        if (!zones.isBuildInProgress())
            ++m_pendingZoneIndex;
    }

    const auto& bridgeChanges = m_pendingBridgeChanges;
    while (remaining != 0 &&
           m_pendingZoneIndex >= m_pendingZoneEnd &&
           m_pendingBridgeChangeIndex < bridgeChanges.size())
    {
        const NavigationBridgeDirtyChange& change =
            bridgeChanges[m_pendingBridgeChangeIndex];
        const auto bindingPosition = std::lower_bound(
            m_bridgeLayers.begin(), m_bridgeLayers.end(), change.bridgeId,
            [](const NavigationBridgeLayerBinding& value, uint64_t id) {
                return value.bridgeId < id;
            });
        if (bindingPosition == m_bridgeLayers.end() ||
            bindingPosition->bridgeId != change.bridgeId) {
            noteGridPublicationFailure(
                NavigationGridPublicationFailureReason::BridgeBindingMissing,
                InvalidNavigationLayer, InvalidNavigationCell, change.bridgeId);
            return NavigationSystemStatus::GridPublicationFailed;
        }
        NavigationGrid* bridgeGrid =
            m_pendingLayers.findMutable(bindingPosition->layer);
        const NavigationGrid* bridgeStatic =
            m_staticLayers.find(bindingPosition->layer);
        if (bridgeGrid == nullptr || bridgeStatic == nullptr ||
            !compatibleGrids(*bridgeGrid, *bridgeStatic)) {
            noteGridPublicationFailure(
                NavigationGridPublicationFailureReason::BridgeLayerIncompatible,
                bindingPosition->layer, InvalidNavigationCell, change.bridgeId);
            return NavigationSystemStatus::GridPublicationFailed;
        }

        if (m_pendingBridgePhase == 0)
        {
            m_pendingBridgeRasterBounds = change.affectedCells;
            if (!m_pendingBridgeRasterBounds.valid())
                m_pendingBridgeRasterBounds = {
                    0, 0,
                    static_cast<int32_t>(bridgeGrid->width()) - 1,
                    static_cast<int32_t>(bridgeGrid->height()) - 1};
            m_pendingBridgeRasterBounds.minX = std::max<int32_t>(
                0, m_pendingBridgeRasterBounds.minX);
            m_pendingBridgeRasterBounds.minY = std::max<int32_t>(
                0, m_pendingBridgeRasterBounds.minY);
            m_pendingBridgeRasterBounds.maxX = std::min<int32_t>(
                static_cast<int32_t>(bridgeGrid->width()) - 1,
                m_pendingBridgeRasterBounds.maxX);
            m_pendingBridgeRasterBounds.maxY = std::min<int32_t>(
                static_cast<int32_t>(bridgeGrid->height()) - 1,
                m_pendingBridgeRasterBounds.maxY);
            const int32_t expansion = static_cast<int32_t>(
                NavigationClearance::MaximumRadiusCells);
            m_pendingBridgeClearanceBounds = {
                std::max<int32_t>(0, m_pendingBridgeRasterBounds.minX - expansion),
                std::max<int32_t>(0, m_pendingBridgeRasterBounds.minY - expansion),
                std::min<int32_t>(static_cast<int32_t>(bridgeGrid->width()) - 1,
                                  m_pendingBridgeRasterBounds.maxX + expansion),
                std::min<int32_t>(static_cast<int32_t>(bridgeGrid->height()) - 1,
                                  m_pendingBridgeRasterBounds.maxY + expansion)};
            m_pendingBridgeRasterCursor = 0;
            m_pendingBridgeClearanceCursor = 0;
            const auto key = std::tuple(
                bindingPosition->layer,
                NavigationClearanceClass::Centered1x1);
            const auto zonePosition = std::lower_bound(
                m_pendingLayerZones.begin(), m_pendingLayerZones.end(), key,
                [](const NavigationZoneField& value, const auto& sought) {
                    return std::tuple(value.layer(), value.clearanceClass()) < sought;
                });
            if (zonePosition == m_pendingLayerZones.end() ||
                zonePosition->layer() != bindingPosition->layer)
                return NavigationSystemStatus::ZoneBuildFailed;
            m_pendingBridgeZoneBegin = static_cast<size_t>(
                zonePosition - m_pendingLayerZones.begin());
            m_pendingBridgeZoneIndex = m_pendingBridgeZoneBegin;
            m_pendingBridgeZoneEnd = m_pendingBridgeZoneBegin +
                NavigationClearanceProfiles.size();
            m_pendingBridgePhase = 1;
        }

        if (m_pendingBridgePhase == 1)
        {
            const uint64_t width = static_cast<uint64_t>(
                m_pendingBridgeRasterBounds.maxX -
                m_pendingBridgeRasterBounds.minX + 1);
            while (remaining != 0 &&
                   m_pendingBridgeRasterCursor <
                       m_pendingBridgeRasterBounds.cellCount())
            {
                const uint64_t cursor = m_pendingBridgeRasterCursor++;
                const int32_t x = m_pendingBridgeRasterBounds.minX +
                    static_cast<int32_t>(cursor % width);
                const int32_t y = m_pendingBridgeRasterBounds.minY +
                    static_cast<int32_t>(cursor / width);
                const NavigationCellId cell = bridgeGrid->cellId({x, y});
                NavigationCellValue value = bridgeStatic->cell(cell);
                if (!m_dynamicOverlay.bridgeActive(change.bridgeId) &&
                    value.passability == NavigationPassability::Traversable)
                    value.passability = NavigationPassability::Blocked;
                if (bridgeGrid->setCell(cell, value) !=
                    NavigationGridResult::Success) {
                    noteGridPublicationFailure(
                        NavigationGridPublicationFailureReason::BridgeCellWriteFailed,
                        bindingPosition->layer, cell, change.bridgeId);
                    return NavigationSystemStatus::GridPublicationFailed;
                }
                --remaining;
            }
            if (m_pendingBridgeRasterCursor <
                m_pendingBridgeRasterBounds.cellCount())
                break;
            m_pendingBridgePhase = 2;
        }

        if (m_pendingBridgePhase == 2)
        {
            const uint64_t width = static_cast<uint64_t>(
                m_pendingBridgeClearanceBounds.maxX -
                m_pendingBridgeClearanceBounds.minX + 1);
            while (remaining != 0 &&
                   m_pendingBridgeClearanceCursor <
                       m_pendingBridgeClearanceBounds.cellCount())
            {
                const uint64_t cursor = m_pendingBridgeClearanceCursor++;
                const int32_t x = m_pendingBridgeClearanceBounds.minX +
                    static_cast<int32_t>(cursor % width);
                const int32_t y = m_pendingBridgeClearanceBounds.minY +
                    static_cast<int32_t>(cursor / width);
                const NavigationCellId cell = bridgeGrid->cellId({x, y});
                if (!bridgeGrid->rebuildClearanceCell(cell)) {
                    noteGridPublicationFailure(
                        NavigationGridPublicationFailureReason::BridgeClearanceFailed,
                        bindingPosition->layer, cell, change.bridgeId);
                    return NavigationSystemStatus::GridPublicationFailed;
                }
                --remaining;
            }
            if (m_pendingBridgeClearanceCursor <
                m_pendingBridgeClearanceBounds.cellCount())
                break;
            m_pendingBridgePhase = 3;
        }

        if (m_pendingBridgePhase == 3)
        {
            if (m_pendingBridgeZoneIndex < m_pendingBridgeZoneEnd)
            {
                NavigationZoneField& zones =
                    m_pendingLayerZones[m_pendingBridgeZoneIndex];
                if (!zones.isBuildInProgress())
                {
                    const size_t profileOffset =
                        m_pendingBridgeZoneIndex - m_pendingBridgeZoneBegin;
                    const NavigationZoneBuildResult started = zones.beginBuild(
                        *bridgeGrid, m_primaryProfile,
                        m_primaryMovementMask, bindingPosition->layer,
                        NavigationClearanceProfiles[profileOffset]);
                    if (started != NavigationZoneBuildResult::Success)
                        return NavigationSystemStatus::ZoneBuildFailed;
                }
                size_t consumed = 0;
                const NavigationZoneBuildResult stepped = zones.stepBuild(
                    *bridgeGrid, m_zoneBuildFrontier,
                    static_cast<size_t>(remaining), consumed);
                if (stepped != NavigationZoneBuildResult::Success)
                    return NavigationSystemStatus::ZoneBuildFailed;
                remaining -= std::min<uint64_t>(remaining, consumed);
                if (!zones.isBuildInProgress())
                    ++m_pendingBridgeZoneIndex;
                if (remaining == 0)
                    break;
                continue;
            }
            ++m_pendingBridgeChangeIndex;
            m_pendingBridgePhase = 0;
            m_pendingBridgeRasterBounds = {};
            m_pendingBridgeClearanceBounds = {};
            continue;
        }
    }

    if (m_pendingRasterCursor < m_pendingRasterBounds.cellCount() ||
        m_pendingClearanceCursor < m_pendingClearanceBounds.cellCount() ||
        m_pendingZoneIndex < m_pendingZoneEnd ||
        m_pendingBridgeChangeIndex < m_pendingBridgeChanges.size())
        return NavigationSystemStatus::PublicationPending;

    std::swap(m_layers, m_pendingLayers);
    std::swap(m_layerZones, m_pendingLayerZones);
    if (m_pendingDirtyPublication.after.portalTopology !=
        m_publishedPortalRevision)
    {
        if (m_pendingBridgeChanges.empty())
        {
            const NavigationSystemStatus bridgeStatus = publishBridgeLayers();
            if (bridgeStatus != NavigationSystemStatus::Success)
                return bridgeStatus;
        }
        else
        {
            for (const NavigationBridgeDirtyChange& change :
                 m_pendingBridgeChanges)
            {
                const auto bindingPosition = std::lower_bound(
                    m_bridgeLayers.begin(), m_bridgeLayers.end(),
                    change.bridgeId,
                    [](const NavigationBridgeLayerBinding& value,
                       uint64_t id) { return value.bridgeId < id; });
                if (bindingPosition == m_bridgeLayers.end() ||
                    bindingPosition->bridgeId != change.bridgeId ||
                    m_portals.setActiveByLayer(
                        bindingPosition->layer,
                        m_dynamicOverlay.bridgeActive(change.bridgeId)) == 0) {
                    noteGridPublicationFailure(
                        NavigationGridPublicationFailureReason::BridgePortalMissing,
                        bindingPosition != m_bridgeLayers.end()
                            ? bindingPosition->layer : InvalidNavigationLayer,
                        InvalidNavigationCell, change.bridgeId);
                    return NavigationSystemStatus::GridPublicationFailed;
                }
                bindingPosition->publishedActive =
                    m_dynamicOverlay.bridgeActive(change.bridgeId);
            }
            for (const NavigationBridgeDirtyChange& change :
                 m_pendingBridgeChanges)
            {
                const auto bindingPosition = std::lower_bound(
                    m_bridgeLayers.begin(), m_bridgeLayers.end(),
                    change.bridgeId,
                    [](const NavigationBridgeLayerBinding& value,
                       uint64_t id) { return value.bridgeId < id; });
                if (bindingPosition == m_bridgeLayers.end() ||
                    m_portalGraph.updateActiveForLayer(
                        m_portals, bindingPosition->layer) !=
                        NavigationPortalGraphBuildResult::Success) {
                    noteGridPublicationFailure(
                        NavigationGridPublicationFailureReason::PortalGraphUpdateFailed,
                        bindingPosition != m_bridgeLayers.end()
                            ? bindingPosition->layer : InvalidNavigationLayer,
                        InvalidNavigationCell, change.bridgeId);
                    return NavigationSystemStatus::GridPublicationFailed;
                }
            }
        }
        m_publishedPortalRevision =
            m_pendingDirtyPublication.after.portalTopology;
    }
    ++m_pathRevision.value;
    for (NavigationPathService& pathService : m_pathServices)
        pathService.setNavigationRevision(m_pathRevision);
    m_waterRasterPendingDirty = {};
    m_dirtyPublicationActive = false;
    m_pendingDirtyPublication = {};
    m_pendingRasterBounds = {};
    m_pendingClearanceBounds = {};
    m_pendingRasterCursor = 0;
    m_pendingClearanceCursor = 0;
    m_pendingZoneBegin = 0;
    m_pendingZoneIndex = 0;
    m_pendingZoneEnd = 0;
    m_pendingBridgeChanges.clear();
    m_pendingBridgeChangeIndex = 0;
    m_pendingBridgeRasterBounds = {};
    m_pendingBridgeClearanceBounds = {};
    m_pendingBridgeRasterCursor = 0;
    m_pendingBridgeClearanceCursor = 0;
    m_pendingBridgeZoneBegin = 0;
    m_pendingBridgeZoneIndex = 0;
    m_pendingBridgeZoneEnd = 0;
    m_pendingBridgePhase = 0;
    return NavigationSystemStatus::Success;
}

NavigationSystemStatus NavigationSystem::publishBridgeLayers(
    container::Span<const NavigationBridgeDirtyChange> changes) noexcept
{
    for (NavigationBridgeLayerBinding& binding : m_bridgeLayers)
    {
        const NavigationBridgeDirtyChange* changed = nullptr;
        if (!changes.empty())
        {
            for (const NavigationBridgeDirtyChange& candidate : changes)
            {
                if (candidate.bridgeId == binding.bridgeId)
                {
                    changed = &candidate;
                    break;
                }
            }
            if (changed == nullptr)
                continue;
        }
        const NavigationGrid* staticGrid = m_staticLayers.find(binding.layer);
        NavigationGrid* runtimeGrid = m_layers.findMutable(binding.layer);
        if (staticGrid == nullptr || runtimeGrid == nullptr ||
            !compatibleGrids(*staticGrid, *runtimeGrid)) {
            noteGridPublicationFailure(
                NavigationGridPublicationFailureReason::BridgeLayerIncompatible,
                binding.layer, InvalidNavigationCell, binding.bridgeId);
            return NavigationSystemStatus::GridPublicationFailed;
        }

        const bool active = m_dynamicOverlay.bridgeActive(binding.bridgeId);
        NavigationCellBounds rasterBounds;
        if (changed != nullptr)
            rasterBounds = changed->affectedCells;
        if (!rasterBounds.valid())
        {
            rasterBounds = {0, 0,
                            static_cast<int32_t>(staticGrid->width()) - 1,
                            static_cast<int32_t>(staticGrid->height()) - 1};
        }
        for (int32_t y = rasterBounds.minY; y <= rasterBounds.maxY; ++y)
        {
            for (int32_t x = rasterBounds.minX; x <= rasterBounds.maxX; ++x)
            {
                const NavigationCellId cell = staticGrid->cellId({x, y});
                NavigationCellValue value = staticGrid->cell(cell);
                if (!active && value.passability == NavigationPassability::Traversable)
                    value.passability = NavigationPassability::Blocked;
                if (runtimeGrid->setCell(cell, value) != NavigationGridResult::Success) {
                    noteGridPublicationFailure(
                        NavigationGridPublicationFailureReason::BridgeCellWriteFailed,
                        binding.layer, cell, binding.bridgeId);
                    return NavigationSystemStatus::GridPublicationFailed;
                }
            }
        }
        const int32_t expansion = static_cast<int32_t>(
            NavigationClearance::MaximumRadiusCells);
        const NavigationCellBounds clearanceBounds = {
            std::max<int32_t>(0, rasterBounds.minX - expansion),
            std::max<int32_t>(0, rasterBounds.minY - expansion),
            std::min<int32_t>(static_cast<int32_t>(runtimeGrid->width()) - 1,
                              rasterBounds.maxX + expansion),
            std::min<int32_t>(static_cast<int32_t>(runtimeGrid->height()) - 1,
                              rasterBounds.maxY + expansion)};
        if (!runtimeGrid->rebuildClearanceRegion(
                clearanceBounds.minX, clearanceBounds.minY,
                clearanceBounds.maxX, clearanceBounds.maxY)) {
            noteGridPublicationFailure(
                NavigationGridPublicationFailureReason::BridgeClearanceFailed,
                binding.layer, InvalidNavigationCell, binding.bridgeId);
            return NavigationSystemStatus::GridPublicationFailed;
        }
        const size_t matchedPortals = m_portals.setActiveByLayer(binding.layer, active);
        if (matchedPortals == 0) {
            noteGridPublicationFailure(
                NavigationGridPublicationFailureReason::BridgePortalMissing,
                binding.layer, InvalidNavigationCell, binding.bridgeId);
            return NavigationSystemStatus::GridPublicationFailed;
        }
        const NavigationSystemStatus zoneStatus =
            rebuildLayerZones(binding.layer);
        if (zoneStatus != NavigationSystemStatus::Success)
            return zoneStatus;
        binding.publishedActive = active;
    }
    if (m_portalGraph.build(m_portals) !=
        NavigationPortalGraphBuildResult::Success) {
        noteGridPublicationFailure(
            NavigationGridPublicationFailureReason::PortalGraphBuildFailed);
        return NavigationSystemStatus::GridPublicationFailed;
    }
    return NavigationSystemStatus::Success;
}

NavigationSystemStatus NavigationSystem::publishStartupTopology(bool advanceRevision) noexcept
{
    if (advanceRevision && m_pathRevision.value == std::numeric_limits<uint64_t>::max())
        return NavigationSystemStatus::RevisionExhausted;
    if (m_portalGraph.build(m_portals) != NavigationPortalGraphBuildResult::Success) {
        noteGridPublicationFailure(
            NavigationGridPublicationFailureReason::PortalGraphBuildFailed);
        return NavigationSystemStatus::GridPublicationFailed;
    }
    m_publishedPortalRevision = m_dynamicOverlay.revisions().portalTopology;
    m_startupTopologyDirty = false;
    if (advanceRevision)
    {
        ++m_pathRevision.value;
        for (NavigationPathService& pathService : m_pathServices)
            pathService.setNavigationRevision(m_pathRevision);
    }
    return NavigationSystemStatus::Success;
}

container::Vector<NavigationBridgeLayerBinding>::iterator NavigationSystem::bridgeBindingPosition(
    uint64_t bridgeId) noexcept
{
    return std::lower_bound(m_bridgeLayers.begin(),
                            m_bridgeLayers.end(),
                            bridgeId,
                            [](const NavigationBridgeLayerBinding& value, uint64_t id) { return value.bridgeId < id; });
}

bool NavigationSystem::hasPortalForLayer(NavigationLayerId layer) const noexcept
{
    return std::any_of(m_portals.portals().begin(),
                       m_portals.portals().end(),
                       [layer](const NavigationPortal& portal)
                       { return portal.sideA.layer == layer || portal.sideB.layer == layer; });
}

} // namespace engine::navigation
