#include "NavigationSystem.h"

#include <limits>

namespace engine::navigation
{
namespace {

[[nodiscard]] constexpr bool portalAllowsExternalToBridge(
    const NavigationPortal& portal, bool externalIsA) noexcept {
    return portal.direction == NavigationPortalDirection::TwoWay ||
        (externalIsA
            ? portal.direction == NavigationPortalDirection::AtoB
            : portal.direction == NavigationPortalDirection::BtoA);
}

[[nodiscard]] constexpr bool portalAllowsBridgeToExternal(
    const NavigationPortal& portal, bool externalIsA) noexcept {
    return portal.direction == NavigationPortalDirection::TwoWay ||
        (externalIsA
            ? portal.direction == NavigationPortalDirection::BtoA
            : portal.direction == NavigationPortalDirection::AtoB);
}

} // namespace

NavigationAdapterSubmitResult NavigationSystem::submitPathRequest(const engine::ai::PathRequest& request,
                                                                  uint64_t confirmedTick) noexcept
{
    const size_t lane = request.correlation.subject
        ? static_cast<size_t>(request.correlation.subject.value) %
              m_pathServices.size()
        : 0;
    return m_pathServices[lane].submit(
        request, confirmedTick, &m_pathRepository);
}

NavigationAdapterSubmitResult NavigationSystem::submitPathRequest(const engine::ai::PathRequest& request,
                                                                  uint64_t confirmedTick,
                                                                  NavigationLayerId startLayer,
                                                                  NavigationLayerId goalLayer) noexcept
{
    if (m_layers.find(startLayer) == nullptr || m_layers.find(goalLayer) == nullptr)
        return NavigationAdapterSubmitResult::InvalidRequest;
    const size_t lane = request.correlation.subject
        ? static_cast<size_t>(request.correlation.subject.value) %
              m_pathServices.size()
        : 0;
    return m_pathServices[lane].submit(
        request, confirmedTick, &m_pathRepository, startLayer, goalLayer);
}

bool NavigationSystem::pollPathFeedback(const engine::ai::PathCorrelation& correlation,
                                        uint64_t confirmedTick,
                                        NavigationAdapterFeedback& output) noexcept
{
    const size_t lane = correlation.subject
        ? static_cast<size_t>(correlation.subject.value) %
              m_pathServices.size()
        : 0;
    return m_pathServices[lane].poll(
        correlation, confirmedTick, m_pathRevision, output,
        &m_pathRepository);
}

PathRepositoryStatus NavigationSystem::releasePath(engine::ai::PathHandle path,
                                                   NavigationRevision expectedRevision) noexcept
{
    return m_pathRepository.release(path, expectedRevision);
}

bool NavigationSystem::isPathStale(
    engine::ai::PathHandle path,
    NavigationRevision expectedRevision) const noexcept {
    const PathRepositoryMetadataQuery stored =
        m_pathRepository.metadata(path, expectedRevision);
    if (stored.status != PathRepositoryStatus::Success) {
        return true;
    }
    const NavigationPathMetadata& metadata = stored.metadata;
    if (!metadata.revisions.staticNavigation &&
        !metadata.revisions.dynamicObstacles &&
        !metadata.revisions.portalTopology) {
        return m_pathRevision.value != expectedRevision.value;
    }
    // Corridor metadata is layer-agnostic in cell space and portal routes may
    // cross several layers. Dynamic occupancy on a bridge/elevated segment is
    // therefore just as capable of invalidating a stored path as occupancy on
    // the primary layer; the old non-primary shortcut silently ignored it.
    return m_dynamicOverlay.pathStaleness(metadata).stale;
}

ObjectId NavigationSystem::findBrokenBridgeConnecting(
    const engine::ai::PathRequest& request,
    NavigationLayerId startLayer,
    NavigationLayerId goalLayer) const noexcept {
    if (!m_initialized || !topologyQueriesAvailable() ||
        request.traversalMode != engine::ai::AIPathTraversalMode::Navmesh ||
        request.surfaceMask == 0 ||
        (request.surfaceMask & NavigationMovement::Air) != 0 ||
        !request.clearanceProfile.validFrozen()) {
        return INVALID_OBJECT_ID;
    }
    const NavigationClearanceClass clearance = clearanceClassForGeometry(
        request.clearanceProfile.radiusCells,
        request.clearanceProfile.centerInCell);
    const NavigationGrid* startGrid = m_layers.find(startLayer);
    const NavigationGrid* goalGrid = m_layers.find(goalLayer);
    const NavigationZoneField* startZones =
        findLayerZones(startLayer, clearance);
    const NavigationZoneField* goalZones =
        findLayerZones(goalLayer, clearance);
    if (!startGrid || !goalGrid || !startZones || !goalZones ||
        !startZones->isBuilt() || !goalZones->isBuilt()) {
        return INVALID_OBJECT_ID;
    }
    const NavigationCellId startCell = startGrid->cellAt(
        {request.start.xRaw, request.start.yRaw, request.start.zRaw},
        clearance);
    const NavigationCellId goalCell = goalGrid->cellAt(
        {request.originalGoal.xRaw, request.originalGoal.yRaw,
         request.originalGoal.zRaw}, clearance);
    if (!startCell || !goalCell || !startZones->zone(startCell) ||
        !goalZones->zone(goalCell)) {
        return INVALID_OBJECT_ID;
    }
    if (startLayer == goalLayer && startZones == goalZones &&
        startZones->sameZone(startCell, goalCell)) {
        return INVALID_OBJECT_ID;
    }

    const auto endpointConnects = [this, clearance, &request](
        NavigationLayerId endpointLayer, NavigationCellId endpointCell,
        NavigationLayerId actorLayer, NavigationCellId actorCell) noexcept {
        if (endpointLayer != actorLayer) return false;
        const NavigationZoneField* zones =
            findLayerZones(actorLayer, clearance);
        return zones && zones->isBuilt() &&
            (zones->movementMask() & request.surfaceMask) != 0 &&
            zones->sameZone(actorCell, endpointCell);
    };

    for (const NavigationBridgeLayerBinding& bridge : m_bridgeLayers) {
        if (bridge.publishedActive ||
            m_dynamicOverlay.bridgeActive(bridge.bridgeId) ||
            bridge.bridgeId == 0 ||
            bridge.bridgeId > std::numeric_limits<uint32_t>::max()) {
            continue;
        }
        for (const NavigationPortal& entrance : m_portals.portals()) {
            if (entrance.profile != m_primaryProfile) continue;
            const bool entranceAIsBridge =
                entrance.sideA.layer == bridge.layer;
            const bool entranceBIsBridge =
                entrance.sideB.layer == bridge.layer;
            if (entranceAIsBridge == entranceBIsBridge) continue;
            const bool entranceExternalIsA = entranceBIsBridge;
            const NavigationLayerCell entranceExternal = entranceExternalIsA
                ? entrance.sideA : entrance.sideB;
            if (!portalAllowsExternalToBridge(
                    entrance, entranceExternalIsA)) {
                continue;
            }
            for (const NavigationPortal& exit : m_portals.portals()) {
                if (exit.id == entrance.id ||
                    exit.profile != m_primaryProfile) {
                    continue;
                }
                const bool exitAIsBridge =
                    exit.sideA.layer == bridge.layer;
                const bool exitBIsBridge =
                    exit.sideB.layer == bridge.layer;
                if (exitAIsBridge == exitBIsBridge) continue;
                const bool exitExternalIsA = exitBIsBridge;
                const NavigationLayerCell exitExternal = exitExternalIsA
                    ? exit.sideA : exit.sideB;
                if (!portalAllowsBridgeToExternal(exit, exitExternalIsA))
                    continue;
                if (endpointConnects(
                        entranceExternal.layer, entranceExternal.cell,
                        startLayer, startCell) &&
                    endpointConnects(
                        exitExternal.layer, exitExternal.cell,
                        goalLayer, goalCell)) {
                    return ObjectId{
                        static_cast<uint32_t>(bridge.bridgeId)};
                }
            }
        }
    }
    return INVALID_OBJECT_ID;
}

NavigationSystemAdvanceResult NavigationSystem::advanceConfirmedTick(uint64_t confirmedTick,
                                                                     const NavigationSystemBudgets& budgets) noexcept
{
    NavigationSystemAdvanceResult result;
    result.navigationRevisions = m_dynamicOverlay.revisions();
    result.pathRevision = m_pathRevision;
    result.publicationState = topologyPublicationState();
    if (!m_initialized)
        return result;
    if (confirmedTick == 0 || confirmedTick <= m_lastConfirmedTick)
    {
        result.status = NavigationSystemStatus::InvalidTick;
        return result;
    }

    // m_layers is always the last complete, atomically published topology.
    // A dirty publication owns separate work buffers, so pathfinding may
    // safely continue on m_layers while the next revision is prepared. Once
    // the swap completes m_pathRevision advances and consumers repath any
    // route admitted against the previous revision.
    const auto processPublishedPaths = [&]() noexcept -> bool {
        const NavigationGrid* primaryGrid = m_layers.find(m_primaryLayer);
        if (primaryGrid == nullptr) {
            noteGridPublicationFailure(
                NavigationGridPublicationFailureReason::PrimaryLayerMissing,
                m_primaryLayer);
            return false;
        }
        uint32_t remainingPathExpansionBudget =
            budgets.pathExpansionBudget;
        size_t round = 0;
        bool allowZeroBudgetPass = true;
        while (remainingPathExpansionBudget != 0 || allowZeroBudgetPass) {
            allowZeroBudgetPass = false;
            uint32_t consumedThisRound = 0;
            bool publishedThisRound = false;
            const size_t firstLane = static_cast<size_t>(
                (confirmedTick + round) % m_pathServices.size());
            for (size_t offset = 0;
                 offset < m_pathServices.size(); ++offset) {
                const size_t lane =
                    (firstLane + offset) % m_pathServices.size();
                NavigationPathService& pathService =
                    m_pathServices[lane];
                uint32_t laneBudget = std::min(
                    remainingPathExpansionBudget,
                    PathServiceExpansionQuantum);
                bool firstLanePass = true;
                while (laneBudget != 0 || firstLanePass) {
                    firstLanePass = false;
                    result.pathProcess =
                        pathService.processConfirmedTick(
                            confirmedTick, *primaryGrid,
                            m_pathRepository, laneBudget, &m_layers,
                            m_layerZones, &m_portalGraph,
                            &m_waypointGraphResolver,
                            &m_dynamicOverlay, true,
                            m_dynamicOverlay.revisions());
                    const uint32_t consumed = std::min(
                        pathService.expansionsConsumedLastProcess(),
                        laneBudget);
                    laneBudget -= consumed;
                    remainingPathExpansionBudget -= consumed;
                    consumedThisRound += consumed;
                    const bool published = result.pathProcess ==
                            NavigationAdapterProcessResult::
                                FeedbackPublished ||
                        result.pathProcess ==
                            NavigationAdapterProcessResult::RequestRequeued;
                    publishedThisRound =
                        publishedThisRound || published;
                    if (!published ||
                        remainingPathExpansionBudget == 0 ||
                        laneBudget == 0) {
                        break;
                    }
                }
            }
            ++round;
            if (consumedThisRound == 0 && !publishedThisRound)
                break;
        }
        return true;
    };

    // A dirty publication owns a private layer/zone work buffer. Do not admit
    // another dynamic transaction or run pathfinding until the complete
    // snapshot has been swapped into the published side.
    if (m_dirtyPublicationActive)
    {
        const NavigationSystemStatus publication =
            advanceDirtyTopologyPublication(budgets.dynamicCellBudget);
        m_lastConfirmedTick = confirmedTick;
        result.navigationRevisions = m_dynamicOverlay.revisions();
        result.pathRevision = m_pathRevision;
        result.publicationState = topologyPublicationState();
        if (publication == NavigationSystemStatus::PublicationPending)
        {
            if (!processPublishedPaths()) {
                m_topologyPublicationFailed = true;
                result.publicationState =
                    NavigationTopologyPublicationState::Failed;
                result.status =
                    NavigationSystemStatus::GridPublicationFailed;
                return result;
            }
            result.status = NavigationSystemStatus::Success;
            return result;
        }
        if (publication != NavigationSystemStatus::Success)
        {
            m_topologyPublicationFailed = true;
            result.publicationState = NavigationTopologyPublicationState::Failed;
            result.status = publication;
            return result;
        }
        result.topologyPublished = true;
        if (!processPublishedPaths()) {
            m_topologyPublicationFailed = true;
            result.publicationState =
                NavigationTopologyPublicationState::Failed;
            result.status = NavigationSystemStatus::GridPublicationFailed;
            return result;
        }
        result.status = NavigationSystemStatus::Success;
        result.publicationState = topologyPublicationState();
        return result;
    }

    if (m_stagedStaticDirty.valid())
    {
        if (m_dynamicOverlay.noteStaticNavigationChange(m_stagedStaticDirty) != NavigationDynamicOverlayResult::Success)
        {
            m_topologyPublicationFailed = true;
            result.publicationState = NavigationTopologyPublicationState::Failed;
            result.status = NavigationSystemStatus::StaticChangeFailed;
            return result;
        }
        m_stagedStaticDirty = {};
    }

    result.topologyLedger = submitReservedTopologyTransactions(confirmedTick);
    switch (result.topologyLedger.status)
    {
    case NavigationTopologyLedgerStatus::Idle:
    case NavigationTopologyLedgerStatus::Complete:
        break;
    case NavigationTopologyLedgerStatus::RetryPending:
        // Some records from this confirmed topology batch may already be in
        // the overlay queue. Do not commit/publish that prefix while the
        // ledger still owns the remainder: source removal + replacement
        // creation (and any other same-boundary topology changes) must become
        // visible as one completed grid publication.
        m_lastConfirmedTick = confirmedTick;
        result.navigationRevisions = m_dynamicOverlay.revisions();
        result.pathRevision = m_pathRevision;
        result.publicationState = topologyPublicationState();
        if (!processPublishedPaths()) {
            m_topologyPublicationFailed = true;
            result.publicationState =
                NavigationTopologyPublicationState::Failed;
            result.status = NavigationSystemStatus::GridPublicationFailed;
            return result;
        }
        result.status = NavigationSystemStatus::Success;
        return result;
    default:
        m_topologyPublicationFailed = true;
        result.publicationState = NavigationTopologyPublicationState::Failed;
        result.status = NavigationSystemStatus::TopologyTransactionFailed;
        return result;
    }

    result.dynamicCommit =
        m_dynamicOverlay.commitConfirmedTick(confirmedTick, budgets.dynamicEventBudget, budgets.dynamicCellBudget);
    if (!validCommitStatus(result.dynamicCommit.status))
    {
        m_topologyPublicationFailed = true;
        result.publicationState = NavigationTopologyPublicationState::Failed;
        result.status = NavigationSystemStatus::DynamicCommitFailed;
        return result;
    }

    result.dirtyRebuild = m_dynamicOverlay.rebuildDirtyRegion(budgets.dynamicCellBudget);
    if (result.dirtyRebuild.status == NavigationDirtyRebuildStatus::Published)
    {
        const NavigationSystemStatus started =
            beginDirtyTopologyPublication();
        if (started != NavigationSystemStatus::PublicationPending)
        {
            m_topologyPublicationFailed = true;
            result.publicationState = NavigationTopologyPublicationState::Failed;
            result.status = started;
            return result;
        }
        const NavigationSystemStatus publication =
            advanceDirtyTopologyPublication(budgets.dynamicCellBudget);
        if (publication == NavigationSystemStatus::PublicationPending)
        {
            m_lastConfirmedTick = confirmedTick;
            result.navigationRevisions = m_dynamicOverlay.revisions();
            result.pathRevision = m_pathRevision;
            result.publicationState = topologyPublicationState();
            result.status = NavigationSystemStatus::Success;
            return result;
        }
        if (publication != NavigationSystemStatus::Success)
        {
            m_topologyPublicationFailed = true;
            result.publicationState = NavigationTopologyPublicationState::Failed;
            result.status = publication;
            return result;
        }
        result.topologyPublished = true;
    }

    if (!processPublishedPaths())
    {
        m_topologyPublicationFailed = true;
        result.publicationState = NavigationTopologyPublicationState::Failed;
        result.status = NavigationSystemStatus::GridPublicationFailed;
        return result;
    }
    m_lastConfirmedTick = confirmedTick;
    result.navigationRevisions = m_dynamicOverlay.revisions();
    result.pathRevision = m_pathRevision;
    result.publicationState = topologyPublicationState();
    result.status = NavigationSystemStatus::Success;
    return result;
}

} // namespace engine::navigation
