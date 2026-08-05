#include <algorithm>
#include <limits>
#include <utility>

#include "NavigationSystem.h"

namespace engine::navigation
{

void NavigationSystem::reset() noexcept
{
    *this = NavigationSystem{};
}

NavigationSystemStatus NavigationSystem::initialize(const NavigationSystemConfig& config)
{
    if (m_initialized)
        return NavigationSystemStatus::AlreadyInitialized;
    if (!validConfig(config))
        return NavigationSystemStatus::InvalidConfig;

    const size_t layerCapacity = effectiveLayerCapacity(config);
    const size_t portalCapacity = effectivePortalCapacity(config);
    if (layerCapacity == 0 || m_staticLayers.initialize(layerCapacity) != NavigationLayerSetResult::Success ||
        m_layers.initialize(layerCapacity) != NavigationLayerSetResult::Success ||
        m_portals.initialize(portalCapacity) != NavigationPortalSetResult::Success ||
        m_portalGraph.initialize(portalCapacity) != NavigationPortalGraphBuildResult::Success)
        return NavigationSystemStatus::InvalidConfig;

    NavigationGrid staticGrid;
    NavigationGrid runtimeGrid;
    const NavigationGridResult staticGridResult =
        staticGrid.initialize(config.width, config.height, config.transform, config.defaultCell);
    const NavigationGridResult gridResult =
        runtimeGrid.initialize(config.width, config.height, config.transform, config.defaultCell);
    if (staticGridResult != NavigationGridResult::Success || gridResult != NavigationGridResult::Success)
        return NavigationSystemStatus::GridInitializationFailed;
    if (!staticGrid.rebuildClearance() || !runtimeGrid.rebuildClearance())
        return NavigationSystemStatus::GridInitializationFailed;
    if (m_staticLayers.addLayer(config.primaryLayer, std::move(staticGrid)) != NavigationLayerSetResult::Success ||
        m_layers.addLayer(config.primaryLayer, std::move(runtimeGrid)) != NavigationLayerSetResult::Success)
        return NavigationSystemStatus::GridInitializationFailed;

    const NavigationDynamicOverlayConfig overlayConfig{config.width,
                                                       config.height,
                                                       config.dynamicEntityCapacity,
                                                       config.bridgeCapacity,
                                                       config.dynamicEventCapacity,
                                                       config.maxCellsPerFootprint};
    if (m_dynamicOverlay.initialize(overlayConfig) != NavigationDynamicOverlayResult::Success)
        return NavigationSystemStatus::DynamicOverlayInitializationFailed;
    const uint64_t ledgerCellCapacity =
        static_cast<uint64_t>(config.dynamicEventCapacity) *
        config.maxCellsPerFootprint;
    if (ledgerCellCapacity > std::numeric_limits<size_t>::max())
        return NavigationSystemStatus::InvalidConfig;
    m_topologyLedger.clear();
    m_topologyLedger.reserve(config.dynamicEventCapacity);
    if (ledgerCellCapacity > std::numeric_limits<uint32_t>::max() ||
        !m_topologyLedgerCells.initialize(
            ledgerCellCapacity, config.dynamicEventCapacity))
        return NavigationSystemStatus::InvalidConfig;
    m_topologyLedgerCapacity = config.dynamicEventCapacity;
    m_topologyGeneration = 1;
    m_nextTopologyRevision = 1;
    m_layerZones.clear();
    if (layerCapacity > std::numeric_limits<size_t>::max() /
                            NavigationClearanceProfiles.size())
        return NavigationSystemStatus::InvalidConfig;
    m_layerZones.reserve(layerCapacity * NavigationClearanceProfiles.size());
    const NavigationGrid* primaryGrid = m_layers.find(config.primaryLayer);
    if (primaryGrid == nullptr)
        return NavigationSystemStatus::ZoneBuildFailed;
    m_zoneBuildFrontier.assign(primaryGrid->cellCount(),
                               InvalidNavigationCell);
    for (NavigationClearanceClass clearance : NavigationClearanceProfiles)
    {
        NavigationZoneField zones;
        if (zones.initialize(primaryGrid->cellCount()) != NavigationZoneBuildResult::Success ||
            zones.build(*primaryGrid, config.primaryProfile,
                        config.primaryMovementMask, config.primaryLayer,
                        m_zoneBuildFrontier,
                        clearance) != NavigationZoneBuildResult::Success)
            return NavigationSystemStatus::ZoneBuildFailed;
        m_layerZones.push_back(std::move(zones));
    }
    if (m_portalGraph.build(m_portals) != NavigationPortalGraphBuildResult::Success)
        return NavigationSystemStatus::GridInitializationFailed;
    if (m_pathRepository.initialize(config.pathCapacity, config.maxPointsPerPath) != PathRepositoryStatus::Success)
        return NavigationSystemStatus::PathRepositoryInitializationFailed;
    for (NavigationPathService& pathService : m_pathServices) {
        if (!pathService.initialize(config.requestCapacity,
                                    config.feedbackCapacity,
                                    primaryGrid->cellCount(),
                                    config.primaryLayer,
                                    config.primaryProfile,
                                    portalCapacity,
                                    config.maxPointsPerPath)) {
            return NavigationSystemStatus::PathServiceInitializationFailed;
        }
    }

    m_primaryLayer = config.primaryLayer;
    m_primaryProfile = config.primaryProfile;
    m_primaryMovementMask = config.primaryMovementMask;
    m_bridgeLayers.clear();
    m_bridgeLayers.reserve(config.bridgeCapacity);
    m_pendingBridgeChanges.clear();
    m_pendingBridgeChanges.reserve(config.bridgeCapacity);
    m_bridgeBindingCapacity = config.bridgeCapacity;
    m_publishedPortalRevision = m_dynamicOverlay.revisions().portalTopology;
    m_pathRevision = {1};
    for (NavigationPathService& pathService : m_pathServices)
        pathService.setNavigationRevision(m_pathRevision);
    m_initialized = true;
    return NavigationSystemStatus::Success;
}

bool NavigationSystem::isInitialized() const noexcept
{
    return m_initialized;
}

NavigationRevision NavigationSystem::pathRevision() const noexcept
{
    return m_pathRevision;
}

NavigationRevisionSet NavigationSystem::navigationRevisions() const noexcept
{
    return m_dynamicOverlay.revisions();
}

NavigationTopologyPublicationState
NavigationSystem::topologyPublicationState() const noexcept
{
    if (!m_initialized || m_topologyPublicationFailed)
        return NavigationTopologyPublicationState::Failed;
    return topologyPublicationPending()
        ? NavigationTopologyPublicationState::Pending
        : NavigationTopologyPublicationState::Published;
}

bool NavigationSystem::topologyPublicationPending() const noexcept
{
    return m_startupTopologyDirty || m_stagedStaticDirty.valid() ||
           m_waterRasterPendingDirty.valid() ||
           m_dirtyPublicationActive ||
           !m_topologyLedger.empty() ||
           m_dynamicOverlay.pendingEventCount() != 0 ||
           m_dynamicOverlay.commitInProgress() ||
           m_dynamicOverlay.hasUnpublishedDirty();
}

bool NavigationSystem::validConfig(const NavigationSystemConfig& config) noexcept
{
    return config.width != 0 && config.height != 0 && config.transform.cellSizeRaw > 0 &&
           config.defaultCell.layer == config.primaryLayer && config.primaryLayer && config.primaryProfile &&
           config.primaryMovementMask != 0 && config.dynamicEntityCapacity != 0 && config.dynamicEventCapacity != 0 &&
           config.maxCellsPerFootprint != 0 && config.pathCapacity != 0 && config.maxPointsPerPath != 0 &&
           config.requestCapacity != 0 && config.feedbackCapacity != 0;
}

size_t NavigationSystem::effectiveLayerCapacity(const NavigationSystemConfig& config) noexcept
{
    if (config.layerCapacity != 0)
        return config.layerCapacity;
    return static_cast<size_t>(config.bridgeCapacity) + 1U;
}

size_t NavigationSystem::effectivePortalCapacity(const NavigationSystemConfig& config) noexcept
{
    if (config.portalCapacity != 0)
        return config.portalCapacity;
    return static_cast<size_t>(config.bridgeCapacity) * 2U;
}

bool NavigationSystem::validCommitStatus(NavigationDynamicCommitStatus status) noexcept
{
    return status == NavigationDynamicCommitStatus::Idle || status == NavigationDynamicCommitStatus::Complete ||
           status == NavigationDynamicCommitStatus::BudgetExhausted;
}

bool NavigationSystem::retryableTopologySubmission(
    NavigationDynamicOverlayResult result) noexcept
{
    return result == NavigationDynamicOverlayResult::EventCapacityExceeded ||
           result == NavigationDynamicOverlayResult::TickAlreadySealed;
}

bool NavigationSystem::startupMutable() const noexcept
{
    const bool pathServicesIdle = std::all_of(
        m_pathServices.begin(), m_pathServices.end(),
        [](const NavigationPathService& pathService) {
            return pathService.queuedCount() == 0 &&
                pathService.feedbackCount() == 0 &&
                !pathService.hasActiveJob();
        });
    return m_lastConfirmedTick == 0 && pathServicesIdle &&
           m_topologyLedger.empty() &&
           m_dynamicOverlay.pendingEventCount() == 0 &&
           !m_dynamicOverlay.commitInProgress();
}

bool NavigationSystem::compatibleLayerGrid(const NavigationGrid& grid, NavigationLayerId layer) const noexcept
{
    if (!grid.isInitialized())
        return false;
    return std::all_of(
        grid.layer().begin(), grid.layer().end(), [layer](NavigationLayerId cellLayer) { return cellLayer == layer; });
}

bool NavigationSystem::compatibleGrids(const NavigationGrid& first, const NavigationGrid& second) noexcept
{
    return first.isInitialized() && second.isInitialized() && first.width() == second.width() &&
           first.height() == second.height() && first.cellCount() == second.cellCount() &&
           first.transform() == second.transform();
}

} // namespace engine::navigation
