#pragma once

#include "../runtime/NavigationRequestQueue.h"
#include "../runtime/PathRepository.h"
#include "../grid/NavigationLayers.h"
#include "../grid/NavigationPortals.h"
#include "../integration/NavigationDestinationAdjustment.h"
#include "../search/AStarOracle.h"

#include "core/container/container_types.h"
#include "game/navigation/contracts/NavigationPathContracts.h"

#include <cstddef>
#include <cstdint>

namespace engine::navigation
{

enum class NavigationAdapterStatus : uint8_t
{
    Pending = 0,
    Ready,
    NoPath,
    Cancelled,
    UnsupportedSafe,
    UnsupportedTraversal,
    StaleNavigationRevision,
    InvalidRequest,
    CapacityExceeded,
};

enum class NavigationAdapterSubmitResult : uint8_t
{
    Accepted = 0,
    Replaced,
    Cancelled,
    InvalidRequest,
    StaleCorrelation,
    NotFound,
    CapacityExceeded,
};

enum class NavigationAdapterProcessResult : uint8_t
{
    Idle = 0,
    Progressed,
    RequestRequeued,
    FeedbackPublished,
    FeedbackCapacityExceeded,
    PublicationBlocked,
};

enum class NavigationSolverKind : uint8_t
{
    None = 0,
    DirectLine,
    WaypointPolyline,
    AStar,
    Portal,
};

enum class NavigationTerminalReason : uint8_t
{
    None = 0,
    GoalReached,
    StartHasNoTraversableCell,
    GoalHasNoAdmissibleCell,
    SearchFrontierExhausted,
    PatchExpansionLimit,
    CrossLayerRouteUnavailable,
    InvalidEndpoint,
    UnsupportedTraversal,
    CapacityDeferred,
    TopologyRevisionChanged,
    Cancelled,
};

struct NavigationQueryDiagnostics final
{
    NavigationSolverKind solver = NavigationSolverKind::None;
    NavigationTerminalReason reason = NavigationTerminalReason::None;
    uint64_t expansions = 0;
    uint32_t totalCost = NavigationSearchScratch::InfiniteCost;
    NavigationCellId closestCell = InvalidNavigationCell;
    uint64_t traceHash = 0;
    uint64_t objectSnapshotTick = 0;
    uint32_t objectCellCount = 0;
    bool usedDestinationAdjustment = false;
    bool usedPatchSuffix = false;
};

struct NavigationAdapterFeedback final
{
    NavigationAdapterStatus status = NavigationAdapterStatus::Pending;
    engine::ai::PathFeedback feedback;
    NavigationRevision navigationRevision = InvalidNavigationRevision;
    NavigationQueryDiagnostics diagnostics;
};

// Deterministic bridge for the production AI value protocol. The service owns
// no grid or repository pointer. Production submitters pass the borrowed
// repository so replacing/cancelling an already-ready feedback can release
// its stable PathHandle; processConfirmedTick() also borrows both owners.
class NavigationPathService final
{
public:
    [[nodiscard]] bool initialize(size_t requestCapacity,
                                  size_t feedbackCapacity,
                                  size_t cellCapacity,
                                  NavigationLayerId layer,
                                  NavigationProfileId profile,
                                  size_t portalCapacity = 0,
                                  size_t pathPointCapacity = 0);

    void setNavigationRevision(NavigationRevision revision) noexcept;
    [[nodiscard]] NavigationRevision navigationRevision() const noexcept;

    [[nodiscard]] NavigationAdapterSubmitResult submit(
        const engine::ai::PathRequest& request,
        uint64_t confirmedTick,
        PathRepository* repository = nullptr,
        NavigationLayerId startLayer = InvalidNavigationLayer,
        NavigationLayerId goalLayer = InvalidNavigationLayer) noexcept;

    [[nodiscard]] NavigationAdapterProcessResult processConfirmedTick(
        uint64_t confirmedTick,
        const NavigationGrid& grid,
        PathRepository& repository,
        uint32_t expansionBudget,
        const NavigationLayerSet* layers = nullptr,
        container::Span<const NavigationZoneField> zones = {},
        const NavigationPortalGraph* portalGraph = nullptr,
        const engine::ai::AIWaypointGraphResolver* waypointGraph = nullptr,
        const NavigationDynamicOverlay* dynamicOverlay = nullptr,
        bool topologyPublished = true,
        NavigationRevisionSet navigationRevisions = {})
        noexcept;

    [[nodiscard]] bool poll(const engine::ai::PathCorrelation& correlation,
                            uint64_t confirmedTick,
                            NavigationRevision expectedRevision,
                            NavigationAdapterFeedback& output,
                            PathRepository* repository = nullptr) noexcept;

    [[nodiscard]] size_t queuedCount() const noexcept;
    [[nodiscard]] size_t feedbackCount() const noexcept;
    [[nodiscard]] bool hasActiveJob() const noexcept;
    [[nodiscard]] bool consumedExpansionBudgetLastProcess() const noexcept {
        return m_expansionsConsumedLastProcess != 0;
    }
    // Exact bounded work charged by the last process call. Portal routing
    // includes both coarse graph steps and nested segment expansions.
    [[nodiscard]] uint32_t expansionsConsumedLastProcess() const noexcept {
        return m_expansionsConsumedLastProcess;
    }

    [[nodiscard]] uint64_t stableHash() const noexcept;
    void rebindSnapshotStorage() noexcept;

private:
    // Capacity pressure is transient, but retrying every confirmed tick can
    // monopolize the bounded path queues. Keep the original correlation and
    // expose one deterministic retry opportunity at this interval.
    inline static constexpr uint64_t CapacityRetryDelayTicks = 4;

    struct StoredFeedback final
    {
        NavigationAdapterFeedback value;
        uint64_t visibleTick = 0;
    };

    inline static constexpr size_t NoIndex = static_cast<size_t>(-1);

    [[nodiscard]] static NavigationWorldPosition toWorld(
        const engine::ai::AIFixedPosition& value) noexcept;

    [[nodiscard]] static engine::ai::AIFixedPosition toAI(
        const NavigationWorldPosition& value) noexcept;

    [[nodiscard]] static bool replaces(const engine::ai::PathCorrelation& current,
                                       const engine::ai::PathCorrelation& incoming) noexcept;

    [[nodiscard]] static NavigationAdapterSubmitResult mapQueueResult(
        NavigationRequestQueueResult result) noexcept;

    [[nodiscard]] NavigationPathMetadata makePathMetadata(
        container::Span<const NavigationLayerPathPoint> points,
        NavigationLayerId layer,
        uint32_t clearanceRadiusCells) const noexcept;

    [[nodiscard]] size_t ownedSubject(engine::ObjectId subject) const noexcept;

    [[nodiscard]] const engine::ai::PathCorrelation& correlationAt(
        size_t index) const noexcept;

    void discardOwned(size_t index, PathRepository* repository) noexcept;

    void releaseFeedbackPath(size_t index,
                             PathRepository* repository) noexcept;

    [[nodiscard]] bool canPublish(engine::ObjectId subject) const noexcept;

    void publish(const StoredFeedback& feedback) noexcept;

    [[nodiscard]] static StoredFeedback makeTerminal(const engine::ai::PathCorrelation& correlation,
                                                       uint64_t confirmedTick,
                                                       NavigationAdapterStatus sidecar,
                                                       engine::ai::PathFeedbackStatus publicStatus,
                                                       NavigationRevision revision) noexcept;

    [[nodiscard]] static StoredFeedback makeDelayed(
        const engine::ai::PathCorrelation& correlation,
        uint64_t confirmedTick,
        NavigationRevision revision,
        NavigationTerminalReason reason =
            NavigationTerminalReason::CapacityDeferred) noexcept;

    void annotateActiveFeedback(
        StoredFeedback& feedback,
        NavigationSolverKind solver,
        NavigationTerminalReason reason,
        const NavigationSearchProgress* progress = nullptr) const noexcept;

    static void annotateQueuedFeedback(
        StoredFeedback& feedback,
        const QueuedNavigationRequest& queued,
        NavigationSolverKind solver,
        NavigationTerminalReason reason,
        bool usedDestinationAdjustment = false) noexcept;

    [[nodiscard]] NavigationAdapterSubmitResult submitCancel(
        const engine::ai::PathCorrelation& correlation,
        uint64_t confirmedTick,
        PathRepository* repository) noexcept;

    [[nodiscard]] NavigationAdapterProcessResult publishPendingCancel(
        uint64_t confirmedTick) noexcept;

    [[nodiscard]] NavigationAdapterProcessResult finishPortalRoute(
        uint64_t confirmedTick,
        const NavigationLayerSet* layers,
        container::Span<const NavigationZoneField> zones,
        const NavigationPortalGraph* graph,
        const NavigationDynamicOverlay* dynamicOverlay,
        uint32_t expansionBudget,
        PathRepository& repository) noexcept;

    [[nodiscard]] NavigationAdapterProcessResult finishSearch(uint64_t confirmedTick,
                                                              const NavigationGrid& grid,
                                                              PathRepository& repository,
                                                              NavigationSearchStatus status,
                                                              const NavigationDynamicOverlay* dynamicOverlay) noexcept;
    [[nodiscard]] NavigationAdapterProcessResult finishGroupFollower(
        uint64_t confirmedTick, const NavigationGrid& grid,
        const QueuedNavigationRequest& queued,
        const NavigationDynamicOverlay* dynamicOverlay,
        PathRepository& repository) noexcept;

    [[nodiscard]] NavigationAdapterProcessResult finishDirectLine(
        uint64_t confirmedTick, const NavigationGrid& grid,
        const QueuedNavigationRequest& queued,
        const NavigationDynamicOverlay* dynamicOverlay,
        PathRepository& repository) noexcept;

    [[nodiscard]] NavigationAdapterProcessResult finishImmediateRequest(
        uint64_t confirmedTick,
        const NavigationGrid& primaryGrid,
        const QueuedNavigationRequest& queued,
        const NavigationLayerSet* layers,
        const engine::ai::AIWaypointGraphResolver* waypointGraph,
        const NavigationDynamicOverlay* dynamicOverlay,
        PathRepository& repository) noexcept;

    [[nodiscard]] NavigationAdapterProcessResult finishWaypointPolyline(
        uint64_t confirmedTick,
        const QueuedNavigationRequest& queued,
        const NavigationLayerSet* layers,
        const engine::ai::AIWaypointGraphResolver* waypointGraph,
        PathRepository& repository) noexcept;

    void clearActive(bool clearCancel = true) noexcept;

    void removeFeedback(size_t index) noexcept;

    bool m_initialized = false;
    NavigationLayerId m_layer = InvalidNavigationLayer;
    NavigationProfileId m_profile = InvalidNavigationProfile;
    NavigationRevision m_navigationRevision = InvalidNavigationRevision;
    NavigationRevisionSet m_navigationRevisions;
    uint32_t m_metadataGridWidth = 0;
    uint32_t m_metadataGridHeight = 0;
    NavigationRequestQueue m_requests;
    NavigationSearchScratch m_scratch;
    AStarOracle m_oracle;
    NavigationPortalRouteScratch m_portalScratch;
    NavigationPortalRouter m_portalRouter;
    container::Vector<NavigationCellId> m_rawCells;
    container::Vector<NavigationLayerPathPoint> m_pathPoints;
    container::Vector<PathRepositoryPoint> m_groupPathScratch;
    container::Vector<PathRepositoryPoint> m_patchPathScratch;
    container::Vector<NavigationCellId> m_patchGoalCells;
    container::Vector<NavigationSearchRequest::AdjustmentGoal>
        m_adjustedGoals;
    struct GroupPathCache final {
        uint64_t id = 0;
        engine::ai::PathHandle centerPath;
        engine::ai::AIFixedPosition adjustedGoal;
        NavigationRevision revision = InvalidNavigationRevision;
        NavigationLayerId layer = InvalidNavigationLayer;
        uint32_t remainingFollowers = 0;
        uint64_t createdTick = 0;
        engine::ai::PathFeedbackStatus status =
            engine::ai::PathFeedbackStatus::NoPath;
    };
    container::Vector<GroupPathCache> m_groupPaths;
    container::Vector<StoredFeedback> m_feedback;
    size_t m_feedbackCount = 0;
    bool m_active = false;
    // Per-call accounting only. It lets NavigationSystem drain zero-cost
    // terminal requests without multiplying the confirmed-tick search budget.
    uint32_t m_expansionsConsumedLastProcess = 0;
    engine::ai::PathRequest m_activeRequest;
    NavigationRevision m_activeRevision = InvalidNavigationRevision;
    NavigationLayerId m_activeStartLayer = InvalidNavigationLayer;
    NavigationLayerId m_activeGoalLayer = InvalidNavigationLayer;
    NavigationCellId m_activeStartCell = InvalidNavigationCell;
    NavigationCellId m_activeGoalCell = InvalidNavigationCell;
    uint32_t m_activePatchPointCount = 0;
    uint32_t m_activePatchSuffixStart = 0;
    uint32_t m_activePatchGoalCount = 0;
    bool m_activePatchReuse = false;
    uint32_t m_activeAdjustedGoalCount = 0;
    uint32_t m_activeAdjustmentMaximumAnchorOffsetCost = 0;
    bool m_activeUsesPortalRouter = false;
    bool m_activePortalRouteStarted = false;
    bool m_cancelPending = false;
    engine::ai::PathCorrelation m_cancelCorrelation;
    uint64_t m_cancelVisibleTick = 0;
    NavigationRevision m_cancelRevision = InvalidNavigationRevision;
};

} // namespace engine::navigation
