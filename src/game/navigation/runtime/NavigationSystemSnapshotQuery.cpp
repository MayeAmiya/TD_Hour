#include <algorithm>
#include <limits>
#include <tuple>

#include "NavigationSystem.h"

namespace engine::navigation
{

NavigationSystemStatus NavigationSystem::captureSnapshot(NavigationSystemSnapshot& output) const
{
    if (!m_initialized)
        return NavigationSystemStatus::NotInitialized;
    output.m_staticLayers = m_staticLayers;
    output.m_layers = m_layers;
    output.m_pendingLayers = m_pendingLayers;
    output.m_layerZones = m_layerZones;
    output.m_pendingLayerZones = m_pendingLayerZones;
    output.m_zoneBuildFrontier = m_zoneBuildFrontier;
    output.m_portals = m_portals;
    output.m_portalGraph = m_portalGraph;
    output.m_dynamicOverlay = m_dynamicOverlay;
    output.m_pathRepository = m_pathRepository;
    output.m_pathServices = m_pathServices;
    for (NavigationPathService& pathService : output.m_pathServices)
        pathService.rebindSnapshotStorage();
    output.m_primaryLayer = m_primaryLayer;
    output.m_primaryProfile = m_primaryProfile;
    output.m_primaryMovementMask = m_primaryMovementMask;
    output.m_stagedStaticDirty = m_stagedStaticDirty;
    output.m_topologyLedger = m_topologyLedger;
    output.m_topologyLedgerCells = m_topologyLedgerCells;
    output.m_topologyLedgerCapacity = m_topologyLedgerCapacity;
    output.m_topologyGeneration = m_topologyGeneration;
    output.m_nextTopologyRevision = m_nextTopologyRevision;
    output.m_topologyPublicationFailed = m_topologyPublicationFailed;
    output.m_bridgeLayers.clear();
    output.m_bridgeLayers.reserve(m_bridgeBindingCapacity);
    output.m_bridgeLayers.assign(m_bridgeLayers.begin(), m_bridgeLayers.end());
    output.m_bridgeBindingCapacity = m_bridgeBindingCapacity;
    output.m_publishedPortalRevision = m_publishedPortalRevision;
    output.m_pathRevision = m_pathRevision;
    output.m_lastConfirmedTick = m_lastConfirmedTick;
    output.m_startupTopologyDirty = m_startupTopologyDirty;
    output.m_waterRasterAreas = m_waterRasterAreas;
    output.m_waterRasterLandMasks = m_waterRasterLandMasks;
    output.m_waterRasterSourceRevision = m_waterRasterSourceRevision;
    output.m_terrainHeightRevision = m_terrainHeightRevision;
    output.m_waterRasterPendingDirty = m_waterRasterPendingDirty;
    output.m_waterRasterInitialized = m_waterRasterInitialized;
    output.m_pendingDirtyPublication = m_pendingDirtyPublication;
    output.m_pendingRasterBounds = m_pendingRasterBounds;
    output.m_pendingClearanceBounds = m_pendingClearanceBounds;
    output.m_pendingRasterCursor = m_pendingRasterCursor;
    output.m_pendingClearanceCursor = m_pendingClearanceCursor;
    output.m_pendingZoneBegin = m_pendingZoneBegin;
    output.m_pendingZoneIndex = m_pendingZoneIndex;
    output.m_pendingZoneEnd = m_pendingZoneEnd;
    output.m_pendingBridgeChanges = m_pendingBridgeChanges;
    output.m_pendingBridgeChangeIndex = m_pendingBridgeChangeIndex;
    output.m_pendingBridgeRasterBounds = m_pendingBridgeRasterBounds;
    output.m_pendingBridgeClearanceBounds = m_pendingBridgeClearanceBounds;
    output.m_pendingBridgeRasterCursor = m_pendingBridgeRasterCursor;
    output.m_pendingBridgeClearanceCursor = m_pendingBridgeClearanceCursor;
    output.m_pendingBridgeZoneBegin = m_pendingBridgeZoneBegin;
    output.m_pendingBridgeZoneIndex = m_pendingBridgeZoneIndex;
    output.m_pendingBridgeZoneEnd = m_pendingBridgeZoneEnd;
    output.m_pendingBridgePhase = m_pendingBridgePhase;
    output.m_dirtyPublicationActive = m_dirtyPublicationActive;
    output.m_initialized = m_initialized;
    return NavigationSystemStatus::Success;
}

NavigationSystemStatus NavigationSystem::restoreSnapshot(const NavigationSystemSnapshot& snapshot)
{
    if (!validSnapshot(snapshot))
        return NavigationSystemStatus::InvalidSnapshot;
    m_staticLayers = snapshot.m_staticLayers;
    m_layers = snapshot.m_layers;
    m_pendingLayers = snapshot.m_pendingLayers;
    m_layerZones = snapshot.m_layerZones;
    m_pendingLayerZones = snapshot.m_pendingLayerZones;
    m_zoneBuildFrontier = snapshot.m_zoneBuildFrontier;
    m_portals = snapshot.m_portals;
    m_portalGraph = snapshot.m_portalGraph;
    m_dynamicOverlay = snapshot.m_dynamicOverlay;
    m_pathRepository = snapshot.m_pathRepository;
    m_pathServices = snapshot.m_pathServices;
    for (NavigationPathService& pathService : m_pathServices)
        pathService.rebindSnapshotStorage();
    m_primaryLayer = snapshot.m_primaryLayer;
    m_primaryProfile = snapshot.m_primaryProfile;
    m_primaryMovementMask = snapshot.m_primaryMovementMask;
    m_stagedStaticDirty = snapshot.m_stagedStaticDirty;
    m_topologyLedger = snapshot.m_topologyLedger;
    m_topologyLedgerCells = snapshot.m_topologyLedgerCells;
    m_topologyLedgerCapacity = snapshot.m_topologyLedgerCapacity;
    m_topologyGeneration = snapshot.m_topologyGeneration;
    m_nextTopologyRevision = snapshot.m_nextTopologyRevision;
    m_topologyPublicationFailed = snapshot.m_topologyPublicationFailed;
    // Failure diagnostics do not influence topology state or determinism.
    // A restored session will record a fresh precise boundary if it fails.
    m_lastGridPublicationFailure = {};
    m_bridgeLayers.clear();
    m_bridgeLayers.reserve(snapshot.m_bridgeBindingCapacity);
    m_bridgeLayers.assign(snapshot.m_bridgeLayers.begin(), snapshot.m_bridgeLayers.end());
    m_bridgeBindingCapacity = snapshot.m_bridgeBindingCapacity;
    m_publishedPortalRevision = snapshot.m_publishedPortalRevision;
    m_pathRevision = snapshot.m_pathRevision;
    m_lastConfirmedTick = snapshot.m_lastConfirmedTick;
    m_startupTopologyDirty = snapshot.m_startupTopologyDirty;
    m_waterRasterAreas = snapshot.m_waterRasterAreas;
    m_waterRasterLandMasks = snapshot.m_waterRasterLandMasks;
    m_waterRasterSourceRevision = snapshot.m_waterRasterSourceRevision;
    m_terrainHeightRevision = snapshot.m_terrainHeightRevision;
    m_waterRasterPendingDirty = snapshot.m_waterRasterPendingDirty;
    m_waterRasterInitialized = snapshot.m_waterRasterInitialized;
    m_pendingDirtyPublication = snapshot.m_pendingDirtyPublication;
    m_pendingRasterBounds = snapshot.m_pendingRasterBounds;
    m_pendingClearanceBounds = snapshot.m_pendingClearanceBounds;
    m_pendingRasterCursor = snapshot.m_pendingRasterCursor;
    m_pendingClearanceCursor = snapshot.m_pendingClearanceCursor;
    m_pendingZoneBegin = snapshot.m_pendingZoneBegin;
    m_pendingZoneIndex = snapshot.m_pendingZoneIndex;
    m_pendingZoneEnd = snapshot.m_pendingZoneEnd;
    m_pendingBridgeChanges = snapshot.m_pendingBridgeChanges;
    m_pendingBridgeChangeIndex = snapshot.m_pendingBridgeChangeIndex;
    m_pendingBridgeRasterBounds = snapshot.m_pendingBridgeRasterBounds;
    m_pendingBridgeClearanceBounds = snapshot.m_pendingBridgeClearanceBounds;
    m_pendingBridgeRasterCursor = snapshot.m_pendingBridgeRasterCursor;
    m_pendingBridgeClearanceCursor = snapshot.m_pendingBridgeClearanceCursor;
    m_pendingBridgeZoneBegin = snapshot.m_pendingBridgeZoneBegin;
    m_pendingBridgeZoneIndex = snapshot.m_pendingBridgeZoneIndex;
    m_pendingBridgeZoneEnd = snapshot.m_pendingBridgeZoneEnd;
    m_pendingBridgePhase = snapshot.m_pendingBridgePhase;
    m_dirtyPublicationActive = snapshot.m_dirtyPublicationActive;
    m_waterRasterUpdateScratch.clear();
    m_waterRasterUpdateScratch.reserve(m_waterRasterLandMasks.size());
    const NavigationGrid* restoredPrimary = m_layers.find(m_primaryLayer);
    if (restoredPrimary == nullptr)
        return NavigationSystemStatus::InvalidSnapshot;
    if (m_zoneBuildFrontier.size() < restoredPrimary->cellCount())
        m_zoneBuildFrontier.assign(restoredPrimary->cellCount(),
                                   InvalidNavigationCell);
    m_initialized = snapshot.m_initialized;
    return NavigationSystemStatus::Success;
}

const NavigationGrid& NavigationSystem::grid() const noexcept
{
    const NavigationGrid* value = m_layers.find(m_primaryLayer);
    static const NavigationGrid EmptyGrid;
    return value != nullptr ? *value : EmptyGrid;
}

const NavigationGrid& NavigationSystem::staticGrid() const noexcept
{
    const NavigationGrid* value = m_staticLayers.find(m_primaryLayer);
    static const NavigationGrid EmptyGrid;
    return value != nullptr ? *value : EmptyGrid;
}

const NavigationZoneField& NavigationSystem::zones() const noexcept
{
    const NavigationZoneField* value = findLayerZones(m_primaryLayer);
    static const NavigationZoneField EmptyZones;
    return value != nullptr ? *value : EmptyZones;
}

const NavigationLayerSet& NavigationSystem::layers() const noexcept
{
    return m_layers;
}

const NavigationLayerSet& NavigationSystem::staticLayers() const noexcept
{
    return m_staticLayers;
}

container::Span<const NavigationZoneField> NavigationSystem::layerZones() const noexcept
{
    return m_layerZones;
}

const NavigationZoneField* NavigationSystem::layerZones(NavigationLayerId layer) const noexcept
{
    return findLayerZones(layer);
}

const NavigationPortalSet& NavigationSystem::portals() const noexcept
{
    return m_portals;
}

const NavigationPortalGraph& NavigationSystem::portalGraph() const noexcept
{
    return m_portalGraph;
}

const NavigationDynamicOverlay& NavigationSystem::dynamicOverlay() const noexcept
{
    return m_dynamicOverlay;
}

const PathRepository& NavigationSystem::pathRepository() const noexcept
{
    return m_pathRepository;
}

uint64_t NavigationSystem::stableHash() const noexcept
{
    if (!m_initialized)
        return 0;
    uint64_t hash = 14695981039346656037ULL;
    feed(hash, HashSchemaVersion);
    feed(hash, m_staticLayers.stableHash());
    feed(hash, m_layers.stableHash());
    feed(hash, static_cast<uint64_t>(m_layerZones.size()));
    for (const NavigationZoneField& zones : m_layerZones)
        feed(hash, zones.stableHash());
    feed(hash, m_portals.stableHash());
    feed(hash, m_portalGraph.stableHash());
    feed(hash, m_dynamicOverlay.stableHash());
    feed(hash, static_cast<uint8_t>(m_stagedStaticDirty.valid()));
    if (m_stagedStaticDirty.valid())
    {
        feed(hash, static_cast<uint32_t>(m_stagedStaticDirty.minX));
        feed(hash, static_cast<uint32_t>(m_stagedStaticDirty.minY));
        feed(hash, static_cast<uint32_t>(m_stagedStaticDirty.maxX));
        feed(hash, static_cast<uint32_t>(m_stagedStaticDirty.maxY));
    }
    feed(hash, m_topologyGeneration);
    feed(hash, m_nextTopologyRevision);
    feed(hash, static_cast<uint8_t>(m_topologyPublicationFailed));
    feed(hash, static_cast<uint64_t>(m_topologyLedger.size()));
    for (const NavigationTopologyTransactionRecord& record : m_topologyLedger)
    {
        feed(hash, record.sourceConfirmedTick);
        feed(hash, record.entityId);
        feed(hash, static_cast<uint8_t>(record.reason));
        feed(hash, static_cast<uint8_t>(record.buildingState));
        feed(hash, record.stamp.generation);
        feed(hash, record.stamp.revision);
        feed(hash, static_cast<uint8_t>(record.kind));
        feed(hash, static_cast<uint8_t>(record.value));
        feed(hash, static_cast<uint8_t>(record.replaceFootprint));
        feed(hash, static_cast<uint8_t>(record.blocksAirNavigation));
        feed(hash, static_cast<uint8_t>(record.rubbleSurface));
        feed(hash, static_cast<uint8_t>(record.fenceSurface));
        feed(hash, record.cellCount);
        feed(hash, record.retryCount);
        const NavigationCellId* cells = topologyLedgerCells(record);
        for (uint32_t index = 0; index < record.cellCount; ++index)
            feed(hash, cells[index].value);
    }
    feed(hash, m_pathRepository.stableHash());
    feed(hash, static_cast<uint64_t>(m_bridgeLayers.size()));
    for (const NavigationBridgeLayerBinding& binding : m_bridgeLayers)
    {
        feed(hash, binding.bridgeId);
        feed(hash, binding.layer.value);
        feed(hash, static_cast<uint8_t>(binding.publishedActive));
    }
    feed(hash, m_publishedPortalRevision.value);
    feed(hash, m_pathRevision.value);
    feed(hash, m_lastConfirmedTick);
    feed(hash, m_terrainHeightRevision);
    feed(hash, static_cast<uint8_t>(m_startupTopologyDirty));
    feed(hash, static_cast<uint8_t>(m_waterRasterInitialized));
    feed(hash, static_cast<uint64_t>(m_waterRasterAreas.size()));
    for (const NavigationWaterAreaRasterState& area : m_waterRasterAreas)
    {
        feed(hash, area.triggerId);
        feed(hash, static_cast<uint64_t>(area.surfaceHeightRaw));
    }
    feed(hash, static_cast<uint64_t>(m_waterRasterLandMasks.size()));
    for (NavigationMovementMask mask : m_waterRasterLandMasks)
        feed(hash, mask);
    feed(hash, static_cast<uint8_t>(m_waterRasterPendingDirty.valid()));
    if (m_waterRasterPendingDirty.valid())
    {
        feed(hash, static_cast<uint32_t>(m_waterRasterPendingDirty.minX));
        feed(hash, static_cast<uint32_t>(m_waterRasterPendingDirty.minY));
        feed(hash, static_cast<uint32_t>(m_waterRasterPendingDirty.maxX));
        feed(hash, static_cast<uint32_t>(m_waterRasterPendingDirty.maxY));
    }
    feed(hash, static_cast<uint8_t>(m_dirtyPublicationActive));
    if (m_dirtyPublicationActive)
    {
        feed(hash, m_pendingDirtyPublication.before.staticNavigation.value);
        feed(hash, m_pendingDirtyPublication.before.dynamicObstacles.value);
        feed(hash, m_pendingDirtyPublication.before.portalTopology.value);
        feed(hash, m_pendingDirtyPublication.after.staticNavigation.value);
        feed(hash, m_pendingDirtyPublication.after.dynamicObstacles.value);
        feed(hash, m_pendingDirtyPublication.after.portalTopology.value);
        const auto feedBounds = [&hash](NavigationCellBounds bounds) {
            feed(hash, static_cast<uint8_t>(bounds.valid()));
            if (!bounds.valid())
                return;
            feed(hash, static_cast<uint32_t>(bounds.minX));
            feed(hash, static_cast<uint32_t>(bounds.minY));
            feed(hash, static_cast<uint32_t>(bounds.maxX));
            feed(hash, static_cast<uint32_t>(bounds.maxY));
        };
        feedBounds(m_pendingRasterBounds);
        feedBounds(m_pendingClearanceBounds);
        feed(hash, m_pendingRasterCursor);
        feed(hash, m_pendingClearanceCursor);
        feed(hash, static_cast<uint64_t>(m_pendingZoneBegin));
        feed(hash, static_cast<uint64_t>(m_pendingZoneIndex));
        feed(hash, static_cast<uint64_t>(m_pendingZoneEnd));
        feed(hash, static_cast<uint64_t>(m_pendingBridgeChanges.size()));
        for (const NavigationBridgeDirtyChange& change : m_pendingBridgeChanges)
        {
            feed(hash, change.bridgeId);
            feed(hash, static_cast<uint32_t>(change.affectedCells.minX));
            feed(hash, static_cast<uint32_t>(change.affectedCells.minY));
            feed(hash, static_cast<uint32_t>(change.affectedCells.maxX));
            feed(hash, static_cast<uint32_t>(change.affectedCells.maxY));
        }
        feed(hash, static_cast<uint64_t>(m_pendingBridgeChangeIndex));
        feed(hash, static_cast<uint64_t>(m_pendingBridgeRasterCursor));
        feed(hash, static_cast<uint64_t>(m_pendingBridgeClearanceCursor));
        feed(hash, static_cast<uint64_t>(m_pendingBridgeZoneBegin));
        feed(hash, static_cast<uint64_t>(m_pendingBridgeZoneIndex));
        feed(hash, static_cast<uint64_t>(m_pendingBridgeZoneEnd));
        feed(hash, m_pendingBridgePhase);
        feed(hash, static_cast<uint64_t>(m_zoneBuildFrontier.size()));
        for (NavigationCellId cell : m_zoneBuildFrontier)
            feed(hash, cell.value);
        feed(hash, static_cast<uint64_t>(m_pendingLayerZones.size()));
        for (const NavigationZoneField& zones : m_pendingLayerZones)
            feed(hash, zones.buildStableHash());
    }
    feed(hash, static_cast<uint64_t>(m_pathServices.size()));
    for (const NavigationPathService& pathService : m_pathServices)
        feed(hash, pathService.stableHash());
    return hash;
}

bool NavigationSystem::validSnapshot(const NavigationSystemSnapshot& snapshot) noexcept
{
    if (!snapshot.m_initialized || !snapshot.m_primaryLayer || !snapshot.m_primaryProfile ||
        snapshot.m_primaryMovementMask == 0 || !snapshot.m_pathRevision ||
        std::any_of(
            snapshot.m_pathServices.begin(),
            snapshot.m_pathServices.end(),
            [&snapshot](const NavigationPathService& pathService) {
                return pathService.navigationRevision() !=
                    snapshot.m_pathRevision;
            }) ||
        !snapshot.m_dynamicOverlay.isInitialized() || !snapshot.m_pathRepository.isInitialized() ||
        snapshot.m_staticLayers.size() == 0 || snapshot.m_staticLayers.size() != snapshot.m_layers.size() ||
        snapshot.m_layers.size() > std::numeric_limits<size_t>::max() /
            NavigationClearanceProfiles.size() ||
        snapshot.m_layerZones.size() != snapshot.m_layers.size() *
            NavigationClearanceProfiles.size() ||
        snapshot.m_topologyLedgerCapacity == 0 ||
        !snapshot.m_topologyLedgerCells.structurallyValid() ||
        snapshot.m_topologyGeneration == 0 ||
        snapshot.m_nextTopologyRevision == 0 ||
        snapshot.m_topologyLedger.size() > snapshot.m_topologyLedgerCapacity ||
        snapshot.m_bridgeLayers.size() > snapshot.m_bridgeBindingCapacity || !snapshot.m_publishedPortalRevision ||
        snapshot.m_publishedPortalRevision.value > snapshot.m_dynamicOverlay.revisions().portalTopology.value ||
        (!snapshot.m_startupTopologyDirty && snapshot.m_portalGraph.stableHash() != snapshot.m_portals.stableHash()))
        return false;

    const NavigationGrid* primary = snapshot.m_layers.find(snapshot.m_primaryLayer);
    const NavigationGrid* staticPrimary = snapshot.m_staticLayers.find(snapshot.m_primaryLayer);
    if (primary == nullptr || staticPrimary == nullptr || snapshot.m_dynamicOverlay.width() != primary->width() ||
        snapshot.m_dynamicOverlay.height() != primary->height())
        return false;
    if (snapshot.m_dirtyPublicationActive)
    {
        const NavigationGrid* pendingPrimary =
            snapshot.m_pendingLayers.find(snapshot.m_primaryLayer);
        if (!snapshot.m_pendingDirtyPublication.published ||
            pendingPrimary == nullptr ||
            !compatibleGrids(*primary, *pendingPrimary) ||
            snapshot.m_pendingLayers.size() != snapshot.m_layers.size() ||
            snapshot.m_pendingLayerZones.size() != snapshot.m_layerZones.size() ||
            snapshot.m_zoneBuildFrontier.size() < primary->cellCount() ||
            snapshot.m_pendingZoneBegin > snapshot.m_pendingZoneIndex ||
            snapshot.m_pendingZoneIndex > snapshot.m_pendingZoneEnd ||
            snapshot.m_pendingZoneEnd > snapshot.m_pendingLayerZones.size() ||
            snapshot.m_pendingBridgeChangeIndex > snapshot.m_pendingBridgeChanges.size() ||
            snapshot.m_pendingBridgeZoneBegin > snapshot.m_pendingBridgeZoneIndex ||
            snapshot.m_pendingBridgeZoneIndex > snapshot.m_pendingBridgeZoneEnd ||
            snapshot.m_pendingBridgeZoneEnd > snapshot.m_pendingLayerZones.size() ||
            // Phase 3 (bridge zone rebuild) is a legitimately persisted mid-
            // publication state: advanceDirtyTopologyPublication sets it and
            // returns PublicationPending when the tick budget runs out.  A
            // snapshot captured on such a tick used to pass capture and then
            // fail restore with InvalidSnapshot, so save/rollback of an
            // otherwise healthy session failed deterministically.
            snapshot.m_pendingBridgePhase > 3U ||
            snapshot.m_pendingRasterCursor > snapshot.m_pendingRasterBounds.cellCount() ||
            snapshot.m_pendingClearanceCursor > snapshot.m_pendingClearanceBounds.cellCount() ||
            snapshot.m_pendingBridgeRasterCursor > snapshot.m_pendingBridgeRasterBounds.cellCount() ||
            snapshot.m_pendingBridgeClearanceCursor > snapshot.m_pendingBridgeClearanceBounds.cellCount())
            return false;
        const auto validBoundsForPrimary = [&](NavigationCellBounds bounds) {
            return !bounds.valid() ||
                (bounds.minX >= 0 && bounds.minY >= 0 &&
                 bounds.maxX < static_cast<int32_t>(primary->width()) &&
                 bounds.maxY < static_cast<int32_t>(primary->height()));
        };
        if (!validBoundsForPrimary(snapshot.m_pendingRasterBounds) ||
            !validBoundsForPrimary(snapshot.m_pendingClearanceBounds) ||
            !validBoundsForPrimary(snapshot.m_pendingBridgeRasterBounds) ||
            !validBoundsForPrimary(snapshot.m_pendingBridgeClearanceBounds))
            return false;
    }
    if (snapshot.m_stagedStaticDirty.valid() &&
        (snapshot.m_stagedStaticDirty.minX < 0 ||
         snapshot.m_stagedStaticDirty.minY < 0 ||
         snapshot.m_stagedStaticDirty.maxX >=
             static_cast<int32_t>(primary->width()) ||
         snapshot.m_stagedStaticDirty.maxY >=
             static_cast<int32_t>(primary->height())))
        return false;
    if (snapshot.m_waterRasterInitialized &&
        snapshot.m_waterRasterLandMasks.size() != primary->cellCount())
        return false;
    if (!snapshot.m_waterRasterInitialized &&
        (!snapshot.m_waterRasterAreas.empty() ||
         !snapshot.m_waterRasterLandMasks.empty() ||
         snapshot.m_waterRasterSourceRevision != 0 ||
         snapshot.m_waterRasterPendingDirty.valid()))
        return false;
    if (snapshot.m_waterRasterPendingDirty.valid() &&
        (snapshot.m_waterRasterPendingDirty.minX < 0 ||
         snapshot.m_waterRasterPendingDirty.minY < 0 ||
         snapshot.m_waterRasterPendingDirty.maxX >=
             static_cast<int32_t>(primary->width()) ||
         snapshot.m_waterRasterPendingDirty.maxY >=
             static_cast<int32_t>(primary->height())))
        return false;
    if (snapshot.m_waterRasterPendingDirty.valid() &&
        !snapshot.m_stagedStaticDirty.valid() &&
        !snapshot.m_dynamicOverlay.hasUnpublishedDirty())
        return false;
    for (size_t index = 0; index < snapshot.m_waterRasterAreas.size(); ++index)
    {
        for (size_t earlier = 0; earlier < index; ++earlier)
        {
            if (snapshot.m_waterRasterAreas[earlier].triggerId ==
                snapshot.m_waterRasterAreas[index].triggerId)
                return false;
        }
    }

    container::Vector<NavigationCellArena::Range> usedLedgerRanges;
    usedLedgerRanges.reserve(snapshot.m_topologyLedger.size());
    NavigationTopologyTransactionRecord previousTransaction;
    bool hasPreviousTransaction = false;
    uint64_t maximumRevision = 0;
    for (const NavigationTopologyTransactionRecord& record : snapshot.m_topologyLedger)
    {
        if (record.sourceConfirmedTick == 0 || record.entityId == 0 || !record.stamp ||
            record.stamp.generation != snapshot.m_topologyGeneration ||
            record.stamp.revision >= snapshot.m_nextTopologyRevision ||
            record.cellCount != record.cells.count ||
            !snapshot.m_topologyLedgerCells.contains(record.cells))
            return false;
        if (hasPreviousTransaction &&
            record.stamp.revision <= previousTransaction.stamp.revision)
            return false;
        if (hasPreviousTransaction &&
            std::tuple(previousTransaction.stamp.generation,
                       previousTransaction.stamp.revision,
                       previousTransaction.sourceConfirmedTick,
                       previousTransaction.entityId,
                       static_cast<uint8_t>(previousTransaction.reason)) >
                std::tuple(record.stamp.generation,
                           record.stamp.revision,
                           record.sourceConfirmedTick,
                           record.entityId,
                           static_cast<uint8_t>(record.reason)))
            return false;
        const NavigationCellId* cells =
            snapshot.m_topologyLedgerCells.data(record.cells);
        for (uint32_t index = 0; index < record.cellCount; ++index)
        {
            if (!primary->contains(cells[index]) ||
                (index != 0 && !(cells[index - 1] < cells[index])))
                return false;
        }
        for (NavigationCellArena::Range used : usedLedgerRanges)
        {
            const uint64_t usedEnd =
                static_cast<uint64_t>(used.offset) + used.count;
            const uint64_t recordEnd =
                static_cast<uint64_t>(record.cells.offset) +
                record.cells.count;
            if (used.offset < recordEnd && record.cells.offset < usedEnd)
                return false;
        }
        usedLedgerRanges.push_back(record.cells);
        maximumRevision = std::max(maximumRevision, record.stamp.revision);
        previousTransaction = record;
        hasPreviousTransaction = true;
    }
    uint64_t accountedLedgerCells = 0;
    for (NavigationCellArena::Range used : usedLedgerRanges)
    {
        accountedLedgerCells += used.count;
        for (NavigationCellArena::Range free :
             snapshot.m_topologyLedgerCells.freeRanges())
        {
            const uint64_t usedEnd =
                static_cast<uint64_t>(used.offset) + used.count;
            const uint64_t freeEnd =
                static_cast<uint64_t>(free.offset) + free.count;
            if (used.offset < freeEnd && free.offset < usedEnd)
                return false;
        }
    }
    for (NavigationCellArena::Range free :
         snapshot.m_topologyLedgerCells.freeRanges())
        accountedLedgerCells += free.count;
    if (accountedLedgerCells != snapshot.m_topologyLedgerCells.capacity())
        return false;
    if (maximumRevision >= snapshot.m_nextTopologyRevision)
        return false;

    size_t zoneIndex = 0;
    for (const NavigationLayerRecord& layer : snapshot.m_layers.layers())
    {
        const NavigationGrid* staticGrid = snapshot.m_staticLayers.find(layer.id);
        if (staticGrid == nullptr || !compatibleGrids(*staticGrid, layer.grid))
            return false;
        for (NavigationClearanceClass clearance : NavigationClearanceProfiles)
        {
            if (zoneIndex >= snapshot.m_layerZones.size())
                return false;
            const NavigationZoneField& zones =
                snapshot.m_layerZones[zoneIndex++];
            if (!zones.isBuilt() || zones.layer() != layer.id ||
                zones.profile() != snapshot.m_primaryProfile ||
                zones.movementMask() != snapshot.m_primaryMovementMask ||
                zones.clearanceClass() != clearance ||
                zones.width() != layer.grid.width() ||
                zones.height() != layer.grid.height() ||
                zones.cellCount() != layer.grid.cellCount())
                return false;
        }
    }
    uint64_t previousBridge = 0;
    for (const NavigationBridgeLayerBinding& binding : snapshot.m_bridgeLayers)
    {
        if (binding.bridgeId == 0 || binding.bridgeId <= previousBridge || binding.layer == snapshot.m_primaryLayer ||
            snapshot.m_layers.find(binding.layer) == nullptr)
            return false;
        previousBridge = binding.bridgeId;
    }
    return true;
}

const NavigationZoneField* NavigationSystem::findLayerZones(NavigationLayerId layer) const noexcept
{
    return findLayerZones(layer, NavigationClearanceClass::Infantry);
}

const NavigationZoneField* NavigationSystem::findLayerZones(
    NavigationLayerId layer,
    NavigationClearanceClass clearance) const noexcept
{
    const auto key = std::tuple(layer, clearance);
    const auto position =
        std::lower_bound(m_layerZones.begin(),
                         m_layerZones.end(),
                         key,
                         [](const NavigationZoneField& value, const auto& sought)
                         {
                             return std::tuple(value.layer(), value.clearanceClass()) < sought;
                         });
    return position != m_layerZones.end() && position->layer() == layer &&
                   position->clearanceClass() == clearance
        ? &*position
        : nullptr;
}

NavigationZoneField* NavigationSystem::findLayerZonesMutable(
    NavigationLayerId layer,
    NavigationClearanceClass clearance) noexcept
{
    const auto key = std::tuple(layer, clearance);
    const auto position =
        std::lower_bound(m_layerZones.begin(),
                         m_layerZones.end(),
                         key,
                         [](const NavigationZoneField& value, const auto& sought)
                         {
                             return std::tuple(value.layer(), value.clearanceClass()) < sought;
                         });
    return position != m_layerZones.end() && position->layer() == layer &&
                   position->clearanceClass() == clearance
        ? &*position
        : nullptr;
}

NavigationSystemStatus NavigationSystem::rebuildLayerZones(
    NavigationLayerId layer) noexcept
{
    const NavigationGrid* grid = m_layers.find(layer);
    if (grid == nullptr)
        return NavigationSystemStatus::ZoneBuildFailed;
    if (m_zoneBuildFrontier.size() < grid->cellCount())
        return NavigationSystemStatus::ZoneBuildFailed;
    for (NavigationClearanceClass clearance : NavigationClearanceProfiles)
    {
        NavigationZoneField* zones = findLayerZonesMutable(layer, clearance);
        if (zones == nullptr ||
            zones->build(*grid, m_primaryProfile, m_primaryMovementMask,
                         layer, m_zoneBuildFrontier, clearance) !=
                NavigationZoneBuildResult::Success)
            return NavigationSystemStatus::ZoneBuildFailed;
    }
    return NavigationSystemStatus::Success;
}

} // namespace engine::navigation
