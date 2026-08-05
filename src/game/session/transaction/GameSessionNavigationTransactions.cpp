#include "game/session/transaction/GameSessionNavigationTransactions.h"

#include "game/session/frame/GameSessionFrameConstants.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "debug/debug.h"

namespace engine {
namespace {

[[nodiscard]] constexpr const char* navigationStatusName(
    navigation::NavigationSystemStatus status) noexcept {
    using Status = navigation::NavigationSystemStatus;
    switch (status) {
    case Status::Success: return "Success";
    case Status::NotInitialized: return "NotInitialized";
    case Status::AlreadyInitialized: return "AlreadyInitialized";
    case Status::InvalidConfig: return "InvalidConfig";
    case Status::InvalidTick: return "InvalidTick";
    case Status::GridInitializationFailed: return "GridInitializationFailed";
    case Status::DynamicOverlayInitializationFailed:
        return "DynamicOverlayInitializationFailed";
    case Status::ZoneBuildFailed: return "ZoneBuildFailed";
    case Status::PathRepositoryInitializationFailed:
        return "PathRepositoryInitializationFailed";
    case Status::PathServiceInitializationFailed:
        return "PathServiceInitializationFailed";
    case Status::InvalidSnapshot: return "InvalidSnapshot";
    case Status::StaticChangeFailed: return "StaticChangeFailed";
    case Status::TopologyTransactionFailed:
        return "TopologyTransactionFailed";
    case Status::DynamicCommitFailed: return "DynamicCommitFailed";
    case Status::GridPublicationFailed: return "GridPublicationFailed";
    case Status::PublicationPending: return "PublicationPending";
    case Status::RevisionExhausted: return "RevisionExhausted";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* navigationPublicationFailureName(
    navigation::NavigationGridPublicationFailureReason reason) noexcept {
    using Reason = navigation::NavigationGridPublicationFailureReason;
    switch (reason) {
    case Reason::None: return "None";
    case Reason::DirtyRevisionMissing: return "DirtyRevisionMissing";
    case Reason::PrimaryLayerMissing: return "PrimaryLayerMissing";
    case Reason::PrimaryCellInvalid: return "PrimaryCellInvalid";
    case Reason::PrimaryCellWriteFailed: return "PrimaryCellWriteFailed";
    case Reason::PrimaryClearanceFailed: return "PrimaryClearanceFailed";
    case Reason::PendingLayerMissing: return "PendingLayerMissing";
    case Reason::PendingCellInvalid: return "PendingCellInvalid";
    case Reason::PendingCellWriteFailed: return "PendingCellWriteFailed";
    case Reason::PendingClearanceFailed: return "PendingClearanceFailed";
    case Reason::BridgeBindingMissing: return "BridgeBindingMissing";
    case Reason::BridgeLayerIncompatible: return "BridgeLayerIncompatible";
    case Reason::BridgeCellWriteFailed: return "BridgeCellWriteFailed";
    case Reason::BridgeClearanceFailed: return "BridgeClearanceFailed";
    case Reason::BridgePortalMissing: return "BridgePortalMissing";
    case Reason::PortalGraphUpdateFailed: return "PortalGraphUpdateFailed";
    case Reason::PortalGraphBuildFailed: return "PortalGraphBuildFailed";
    case Reason::DirtyRebuildFailed: return "DirtyRebuildFailed";
    }
    return "Unknown";
}

} // namespace

GameSessionNavigationTransactions::GameSessionNavigationTransactions(
    GameSessionContentStartState& content,
    GameSessionScriptPresentationState& presentation) noexcept
    : m_content(content), m_presentation(presentation) {}

bool GameSessionNavigationTransactions::submitBuildingState(
    ObjectId object, uint64_t confirmedTick,
    navigation::NavigationDynamicEventReason reason,
    navigation::NavigationBuildingState state,
    bool blocksNavigation,
    bool blocksAirNavigation) {
    if (!m_content.m_navigation.isInitialized()) return true;
    const uint64_t eventTick = confirmedTick == 0 ? 1 : confirmedTick;
    return m_content.m_navigation.submitBuildingEvent(
               {eventTick, object.value, reason, state,
                blocksNavigation, false, {}, blocksAirNavigation},
               {}) == navigation::NavigationDynamicOverlayResult::Success;
}

navigation::NavigationDynamicOverlayResult
GameSessionNavigationTransactions::submitBridgeState(
    uint64_t bridgeId, navigation::NavigationLayerId bridgeLayer,
    bool active,
    container::Span<const navigation::NavigationCellId> affectedCells,
    uint64_t confirmedTick) noexcept {
    if (!m_content.m_active || !m_content.m_navigation.isInitialized()) {
        return navigation::NavigationDynamicOverlayResult::NotInitialized;
    }
    if (!m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick) {
        return navigation::NavigationDynamicOverlayResult::InvalidEvent;
    }
    if (bridgeLayer) {
        return m_content.m_navigation.submitBridgeEvent(
            {confirmedTick, bridgeId, active}, bridgeLayer, affectedCells);
    }
    return m_content.m_navigation.submitBridgeEvent(
        {confirmedTick, bridgeId, active}, affectedCells);
}

navigation::NavigationSystemStatus
GameSessionNavigationTransactions::synchronizeTerrainAuthority() {
    if (!m_content.m_navigation.isInitialized())
        return navigation::NavigationSystemStatus::Success;
    const navigation::NavigationSystemStatus height =
        m_content.m_navigation.synchronizeTerrainHeight(m_content.m_terrain);
    if (height != navigation::NavigationSystemStatus::Success &&
        height != navigation::NavigationSystemStatus::PublicationPending) {
        return height;
    }
    return m_content.m_navigation.synchronizeWaterRaster(m_content.m_terrain);
}

navigation::NavigationSystemAdvanceResult
GameSessionNavigationTransactions::advanceConfirmedTick(
    uint64_t confirmedTick) noexcept {
    if (!m_content.m_navigation.isInitialized()) {
        return {.status = navigation::NavigationSystemStatus::Success};
    }
    return m_content.m_navigation.advanceConfirmedTick(
        confirmedTick, session_frame_detail::NavigationTickBudgets);
}

bool GameSessionNavigationTransactions::advanceConfirmedTickAndPublishFault(
    uint64_t confirmedTick,
    GameSessionGameplayPublicationPort publication) noexcept {
    const navigation::NavigationSystemAdvanceResult result =
        advanceConfirmedTick(confirmedTick);
    if (result.status == navigation::NavigationSystemStatus::Success)
        return true;

    SimulationFaultCode faultCode =
        result.status == navigation::NavigationSystemStatus::InvalidTick
            ? SimulationFaultCode::InvalidConfirmedTick
            : SimulationFaultCode::AtomicCommitFailed;
    if (result.status ==
            navigation::NavigationSystemStatus::DynamicCommitFailed) {
        switch (result.dynamicCommit.status) {
        case navigation::NavigationDynamicCommitStatus::EntityCapacityExceeded:
        case navigation::NavigationDynamicCommitStatus::BridgeCapacityExceeded:
        case navigation::NavigationDynamicCommitStatus::RefCountOverflow:
            faultCode = SimulationFaultCode::CapacityExceeded;
            break;
        case navigation::NavigationDynamicCommitStatus::InvalidEvent:
            faultCode = SimulationFaultCode::InvalidEvent;
            break;
        default:
            break;
        }
    }
    if (result.status ==
            navigation::NavigationSystemStatus::TopologyTransactionFailed) {
        switch (result.topologyLedger.status) {
        case navigation::NavigationTopologyLedgerStatus::CapacityExceeded:
            faultCode = SimulationFaultCode::CapacityExceeded;
            break;
        case navigation::NavigationTopologyLedgerStatus::InvalidEvent:
            faultCode = SimulationFaultCode::InvalidEvent;
            break;
        default:
            break;
        }
    }
    static_cast<void>(publication.raiseSimulationFault({
        .domain = SimulationFaultDomain::Navigation,
        .code = faultCode,
        .confirmedTick = confirmedTick,
    }));
    const navigation::NavigationGridPublicationFailure& publicationFailure =
        m_content.m_navigation.lastGridPublicationFailure();
    TD_LOG_ERROR(
        "[GameSession] Navigation rejected confirmed tick {}: status={}({}) "
        "dynamic={} committed={} visited={} pending={} "
        "topology={} overlay={} submitted={} retried={} pendingTx={} "
        "dirty={} visited={} remaining={} path={} publication={} published={} "
        "revisions={}/{}/{} pathRevision={} publicationFailure={}({}) "
        "layer={} cell={} bridge={} failurePathRevision={}",
        confirmedTick, static_cast<uint32_t>(result.status),
        navigationStatusName(result.status),
        static_cast<uint32_t>(result.dynamicCommit.status),
        result.dynamicCommit.committedEvents,
        result.dynamicCommit.visitedCells,
        result.dynamicCommit.pendingEvents,
        static_cast<uint32_t>(result.topologyLedger.status),
        static_cast<uint32_t>(result.topologyLedger.overlayResult),
        result.topologyLedger.submittedTransactions,
        result.topologyLedger.retriedTransactions,
        result.topologyLedger.pendingTransactions,
        static_cast<uint32_t>(result.dirtyRebuild.status),
        result.dirtyRebuild.visitedCells,
        result.dirtyRebuild.remainingCells,
        static_cast<uint32_t>(result.pathProcess),
        static_cast<uint32_t>(result.publicationState),
        result.topologyPublished,
        result.navigationRevisions.staticNavigation.value,
        result.navigationRevisions.dynamicObstacles.value,
        result.navigationRevisions.portalTopology.value,
        result.pathRevision.value,
        static_cast<uint32_t>(publicationFailure.reason),
        navigationPublicationFailureName(publicationFailure.reason),
        publicationFailure.layer.value, publicationFailure.cell.value,
        publicationFailure.bridgeId, publicationFailure.pathRevision.value);
    return false;
}

} // namespace engine
