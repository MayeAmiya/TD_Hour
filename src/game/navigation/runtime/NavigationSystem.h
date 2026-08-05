#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "game/navigation/grid/NavigationDynamicOverlay.h"
#include "game/navigation/grid/NavigationPortals.h"
#include "game/navigation/runtime/PathRepository.h"
#include "game/navigation/services/NavigationPathService.h"

namespace game::terrain
{
class TerrainLogic;
}

namespace engine::navigation
{

struct NavigationSystemConfig final
{
    uint32_t width = 0;
    uint32_t height = 0;
    NavigationGridTransform transform;
    NavigationCellValue defaultCell;
    NavigationLayerId primaryLayer;
    NavigationProfileId primaryProfile;
    NavigationMovementMask primaryMovementMask = 0;
    uint32_t dynamicEntityCapacity = 0;
    uint32_t bridgeCapacity = 0;
    uint32_t dynamicEventCapacity = 0;
    // Baseline used to size shared topology/overlay cell arenas. Individual
    // footprints may exceed it while aggregate storage remains available.
    uint32_t maxCellsPerFootprint = 0;
    uint32_t pathCapacity = 0;
    uint32_t maxPointsPerPath = 0;
    uint32_t requestCapacity = 0;
    uint32_t feedbackCapacity = 0;
    // Appended for source compatibility with existing aggregate initializers.
    // Zero derives a conservative startup-only capacity from bridgeCapacity.
    uint32_t layerCapacity = 0;
    uint32_t portalCapacity = 0;
};

struct NavigationSystemBudgets final
{
    uint32_t dynamicEventBudget = 0;
    uint32_t dynamicCellBudget = 0;
    uint32_t pathExpansionBudget = 0;
};

struct NavigationStaticCellUpdate final
{
    NavigationCellId cell = InvalidNavigationCell;
    NavigationCellValue value;
};

struct NavigationWaterAreaRasterState final
{
    uint32_t triggerId = 0;
    int64_t surfaceHeightRaw = 0;
    constexpr bool operator==(const NavigationWaterAreaRasterState&) const noexcept = default;
};

enum class NavigationSystemStatus : uint8_t
{
    Success = 0,
    NotInitialized,
    AlreadyInitialized,
    InvalidConfig,
    InvalidTick,
    GridInitializationFailed,
    DynamicOverlayInitializationFailed,
    ZoneBuildFailed,
    PathRepositoryInitializationFailed,
    PathServiceInitializationFailed,
    InvalidSnapshot,
    StaticChangeFailed,
    TopologyTransactionFailed,
    DynamicCommitFailed,
    GridPublicationFailed,
    PublicationPending,
    RevisionExhausted,
};

enum class NavigationTopologyLedgerStatus : uint8_t
{
    Idle = 0,
    Complete,
    RetryPending,
    InvalidEvent,
    CapacityExceeded,
    RevisionExhausted,
    CommitFailed,
};

enum class NavigationTopologyPublicationState : uint8_t
{
    Published = 0,
    Pending,
    Failed,
};

// GridPublicationFailed used to flatten every failed invariant into one
// status.  Keep the status stable for callers, but retain value-only context
// so a confirmed-tick fault can name the actual topology boundary.
enum class NavigationGridPublicationFailureReason : uint8_t
{
    None = 0,
    DirtyRevisionMissing,
    PrimaryLayerMissing,
    PrimaryCellInvalid,
    PrimaryCellWriteFailed,
    PrimaryClearanceFailed,
    PendingLayerMissing,
    PendingCellInvalid,
    PendingCellWriteFailed,
    PendingClearanceFailed,
    BridgeBindingMissing,
    BridgeLayerIncompatible,
    BridgeCellWriteFailed,
    BridgeClearanceFailed,
    BridgePortalMissing,
    PortalGraphUpdateFailed,
    PortalGraphBuildFailed,
    DirtyRebuildFailed,
};

struct NavigationGridPublicationFailure final
{
    NavigationGridPublicationFailureReason reason =
        NavigationGridPublicationFailureReason::None;
    NavigationLayerId layer = InvalidNavigationLayer;
    NavigationCellId cell = InvalidNavigationCell;
    uint64_t bridgeId = 0;
    NavigationRevision pathRevision = InvalidNavigationRevision;

    [[nodiscard]] constexpr bool failed() const noexcept
    {
        return reason != NavigationGridPublicationFailureReason::None;
    }
};

struct NavigationTopologyLedgerResult final
{
    NavigationTopologyLedgerStatus status = NavigationTopologyLedgerStatus::Idle;
    uint32_t submittedTransactions = 0;
    uint32_t retriedTransactions = 0;
    uint32_t pendingTransactions = 0;
    NavigationTopologyTransactionStamp lastSubmittedTransaction;
    NavigationDynamicOverlayResult overlayResult = NavigationDynamicOverlayResult::Success;
};

struct NavigationSystemAdvanceResult final
{
    NavigationSystemStatus status = NavigationSystemStatus::NotInitialized;
    NavigationDynamicCommitResult dynamicCommit;
    NavigationTopologyLedgerResult topologyLedger;
    NavigationDirtyRebuildResult dirtyRebuild;
    NavigationAdapterProcessResult pathProcess = NavigationAdapterProcessResult::Idle;
    NavigationRevisionSet navigationRevisions;
    NavigationRevision pathRevision = InvalidNavigationRevision;
    NavigationTopologyPublicationState publicationState =
        NavigationTopologyPublicationState::Published;
    bool topologyPublished = false;
};

struct NavigationBridgeLayerBinding final
{
    uint64_t bridgeId = 0;
    NavigationLayerId layer;
    bool publishedActive = true;
    constexpr bool operator==(const NavigationBridgeLayerBinding&) const noexcept = default;
};

enum class NavigationTopologyTransactionKind : uint8_t
{
    Building = 0,
    Bridge,
};

struct NavigationTopologyTransactionRecord final
{
    uint64_t sourceConfirmedTick = 0;
    uint64_t entityId = 0;
    NavigationDynamicEventReason reason = NavigationDynamicEventReason::BuildingPlaced;
    NavigationBuildingState buildingState = NavigationBuildingState::Absent;
    NavigationTopologyTransactionStamp stamp;
    NavigationTopologyTransactionKind kind = NavigationTopologyTransactionKind::Building;
    bool value = false;
    bool replaceFootprint = false;
    bool blocksAirNavigation = false;
    bool rubbleSurface = false;
    bool fenceSurface = false;
    NavigationCellArena::Range cells;
    uint32_t cellCount = 0;
    uint32_t retryCount = 0;
    constexpr bool operator==(const NavigationTopologyTransactionRecord&) const noexcept = default;
};

class NavigationSystemSnapshot final
{
public:
    NavigationSystemSnapshot() = default;
    NavigationSystemSnapshot(const NavigationSystemSnapshot&) = default;
    NavigationSystemSnapshot(NavigationSystemSnapshot&&) noexcept = default;
    NavigationSystemSnapshot& operator=(const NavigationSystemSnapshot&) = default;
    NavigationSystemSnapshot& operator=(NavigationSystemSnapshot&&) noexcept = default;

private:
    friend class NavigationSystem;

    NavigationLayerSet m_staticLayers;
    NavigationLayerSet m_layers;
    NavigationLayerSet m_pendingLayers;
    container::Vector<NavigationZoneField> m_layerZones;
    container::Vector<NavigationZoneField> m_pendingLayerZones;
    container::Vector<NavigationCellId> m_zoneBuildFrontier;
    NavigationPortalSet m_portals;
    NavigationPortalGraph m_portalGraph;
    NavigationDynamicOverlay m_dynamicOverlay;
    PathRepository m_pathRepository;
    std::array<NavigationPathService, 2> m_pathServices;
    NavigationLayerId m_primaryLayer;
    NavigationProfileId m_primaryProfile;
    NavigationMovementMask m_primaryMovementMask = 0;
    NavigationCellBounds m_stagedStaticDirty;
    container::Vector<NavigationTopologyTransactionRecord> m_topologyLedger;
    NavigationCellArena m_topologyLedgerCells;
    uint32_t m_topologyLedgerCapacity = 0;
    uint64_t m_topologyGeneration = 0;
    uint64_t m_nextTopologyRevision = 0;
    bool m_topologyPublicationFailed = false;
    container::Vector<NavigationBridgeLayerBinding> m_bridgeLayers;
    size_t m_bridgeBindingCapacity = 0;
    PortalTopologyRevision m_publishedPortalRevision;
    NavigationRevision m_pathRevision = InvalidNavigationRevision;
    uint64_t m_lastConfirmedTick = 0;
    bool m_startupTopologyDirty = false;
    container::Vector<NavigationWaterAreaRasterState> m_waterRasterAreas;
    container::Vector<NavigationMovementMask> m_waterRasterLandMasks;
    uint64_t m_waterRasterSourceRevision = 0;
    uint64_t m_terrainHeightRevision = 0;
    NavigationCellBounds m_waterRasterPendingDirty;
    bool m_waterRasterInitialized = false;
    NavigationRevisionDelta m_pendingDirtyPublication;
    NavigationCellBounds m_pendingRasterBounds;
    NavigationCellBounds m_pendingClearanceBounds;
    uint64_t m_pendingRasterCursor = 0;
    uint64_t m_pendingClearanceCursor = 0;
    size_t m_pendingZoneBegin = 0;
    size_t m_pendingZoneIndex = 0;
    size_t m_pendingZoneEnd = 0;
    container::Vector<NavigationBridgeDirtyChange> m_pendingBridgeChanges;
    size_t m_pendingBridgeChangeIndex = 0;
    NavigationCellBounds m_pendingBridgeRasterBounds;
    NavigationCellBounds m_pendingBridgeClearanceBounds;
    uint64_t m_pendingBridgeRasterCursor = 0;
    uint64_t m_pendingBridgeClearanceCursor = 0;
    size_t m_pendingBridgeZoneBegin = 0;
    size_t m_pendingBridgeZoneIndex = 0;
    size_t m_pendingBridgeZoneEnd = 0;
    uint8_t m_pendingBridgePhase = 0;
    bool m_dirtyPublicationActive = false;
    bool m_initialized = false;
};

// Owns canonical static/runtime grids and zone fields for every navigation
// layer plus the cross-layer portal graph. The legacy path service continues to
// consume the primary grid; multi-layer consumers borrow the complete values.
// Dynamic events are committed in confirmed-tick order and published atomically
// before either view observes a new topology revision.
class NavigationSystem final
{
public:
    void reset() noexcept;

    [[nodiscard]] NavigationSystemStatus initialize(const NavigationSystemConfig& config);
    [[nodiscard]] bool isInitialized() const noexcept;
    [[nodiscard]] NavigationRevision pathRevision() const noexcept;
    [[nodiscard]] NavigationRevisionSet navigationRevisions() const noexcept;
    [[nodiscard]] NavigationTopologyPublicationState
    topologyPublicationState() const noexcept;
    [[nodiscard]] const NavigationGridPublicationFailure&
    lastGridPublicationFailure() const noexcept
    {
        return m_lastGridPublicationFailure;
    }
    [[nodiscard]] bool topologyQueriesAvailable() const noexcept
    {
        return topologyPublicationState() ==
               NavigationTopologyPublicationState::Published;
    }

    // Static map edits are staged in the immutable-base grid and become visible
    // with dynamic edits at the next completed dirty publication.
    [[nodiscard]] NavigationSystemStatus stageStaticCell(NavigationCellId cell,
                                                         const NavigationCellValue& value) noexcept;

    // Map ingestion supplies one canonical, strictly cell-id-sorted batch.
    // Validation completes before mutation so malformed content cannot leave a
    // partially staged topology.
    [[nodiscard]] NavigationSystemStatus stageStaticCells(
        container::Span<const NavigationStaticCellUpdate> updates) noexcept;

    // Applies only the navigation cells touched by TerrainMap height
    // mutations. The static grid remains the authoritative base; the update
    // is staged and becomes visible together with clearance/zones at the next
    // completed topology publication.
    [[nodiscard]] NavigationSystemStatus synchronizeTerrainHeight(
        const game::terrain::TerrainLogic& terrain) noexcept;

    // Startup terrain ingestion has already sampled every height into the
    // authoritative static grid.  Record that source revision so the first
    // confirmed tick does not rebuild the complete map a second time.
    void markTerrainHeightSynchronized(uint64_t revision) noexcept
    {
        if (m_initialized) m_terrainHeightRevision = revision;
    }

    // Mirrors ZH's terrain-owned water classification into the authoritative
    // navigation grid. Startup classifies the complete map; later calls touch
    // only cells covered by water areas whose fixed surface height changed.
    [[nodiscard]] NavigationSystemStatus synchronizeWaterRaster(
        const game::terrain::TerrainLogic& terrain) noexcept;

    // Startup-only ingestion. Validation and zone construction complete before
    // either authoritative layer set is mutated; both sets share identical
    // capacities, so the two no-allocation insertions form one atomic publish.
    [[nodiscard]] NavigationSystemStatus addStartupElevatedLayer(NavigationLayerId layer,
                                                                 const NavigationGrid& staticGrid);
    [[nodiscard]] NavigationSystemStatus addStartupPortal(const NavigationPortal& portal);

    // Startup-only fast publication for a fully staged map. It is intentionally
    // unavailable after confirmed-tick execution or request submission begins.
    [[nodiscard]] NavigationSystemStatus publishStagedStaticTopology() noexcept;

    // Finalizes map-imported structure/bridge occupancy before the first
    // simulation tick. Runtime mutations continue through the bounded
    // confirmed-tick publisher below.
    [[nodiscard]] NavigationSystemStatus
    publishStagedStartupDynamicTopology() noexcept;

    [[nodiscard]] NavigationDynamicOverlayResult submitBuildingEvent(
        const NavigationBuildingEvent& event, container::Span<const NavigationCellId> footprint) noexcept;
    [[nodiscard]] NavigationDynamicOverlayResult submitBridgeEvent(
        const NavigationBridgeStateEvent& event, container::Span<const NavigationCellId> affectedCells) noexcept;
    [[nodiscard]] NavigationDynamicOverlayResult submitBridgeEvent(
        const NavigationBridgeStateEvent& event,
        NavigationLayerId bridgeLayer,
        container::Span<const NavigationCellId> affectedCells) noexcept;

    [[nodiscard]] NavigationAdapterSubmitResult submitPathRequest(const engine::ai::PathRequest& request,
                                                                  uint64_t confirmedTick) noexcept;
    [[nodiscard]] NavigationAdapterSubmitResult submitPathRequest(const engine::ai::PathRequest& request,
                                                                  uint64_t confirmedTick,
                                                                  NavigationLayerId startLayer,
                                                                  NavigationLayerId goalLayer) noexcept;
    void setWaypointGraphResolver(
        engine::ai::AIWaypointGraphResolver resolver) noexcept {
        m_waypointGraphResolver = resolver;
    }
    [[nodiscard]] bool pollPathFeedback(const engine::ai::PathCorrelation& correlation,
                                        uint64_t confirmedTick,
                                        NavigationAdapterFeedback& output) noexcept;
    [[nodiscard]] PathRepositoryStatus releasePath(engine::ai::PathHandle path,
                                                    NavigationRevision expectedRevision) noexcept;
    [[nodiscard]] bool isPathStale(engine::ai::PathHandle path,
                                   NavigationRevision expectedRevision) const noexcept;
    [[nodiscard]] ObjectId findBrokenBridgeConnecting(
        const engine::ai::PathRequest& request,
        NavigationLayerId startLayer,
        NavigationLayerId goalLayer) const noexcept;

    [[nodiscard]] NavigationSystemStatus captureSnapshot(NavigationSystemSnapshot& output) const;
    [[nodiscard]] NavigationSystemStatus restoreSnapshot(const NavigationSystemSnapshot& snapshot);

    [[nodiscard]] NavigationSystemAdvanceResult advanceConfirmedTick(uint64_t confirmedTick,
                                                                     const NavigationSystemBudgets& budgets) noexcept;

    [[nodiscard]] const NavigationGrid& grid() const noexcept;
    [[nodiscard]] const NavigationGrid& staticGrid() const noexcept;
    [[nodiscard]] const NavigationZoneField& zones() const noexcept;
    [[nodiscard]] const NavigationLayerSet& layers() const noexcept;
    [[nodiscard]] const NavigationLayerSet& staticLayers() const noexcept;
    [[nodiscard]] container::Span<const NavigationZoneField> layerZones() const noexcept;
    [[nodiscard]] const NavigationZoneField* layerZones(NavigationLayerId layer) const noexcept;
    [[nodiscard]] const NavigationPortalSet& portals() const noexcept;
    [[nodiscard]] const NavigationPortalGraph& portalGraph() const noexcept;
    [[nodiscard]] const NavigationDynamicOverlay& dynamicOverlay() const noexcept;
    [[nodiscard]] size_t pendingTopologyTransactionCount() const noexcept
    {
        return m_topologyLedger.size();
    }
    [[nodiscard]] const PathRepository& pathRepository() const noexcept;

    [[nodiscard]] uint64_t stableHash() const noexcept;

private:
    inline static constexpr uint32_t HashSchemaVersion = 12;

    [[nodiscard]] static bool validConfig(const NavigationSystemConfig& config) noexcept;
    [[nodiscard]] static size_t effectiveLayerCapacity(const NavigationSystemConfig& config) noexcept;
    [[nodiscard]] static size_t effectivePortalCapacity(const NavigationSystemConfig& config) noexcept;
    [[nodiscard]] static bool validCommitStatus(NavigationDynamicCommitStatus status) noexcept;
    [[nodiscard]] static bool retryableTopologySubmission(
        NavigationDynamicOverlayResult result) noexcept;
    [[nodiscard]] static bool validSnapshot(const NavigationSystemSnapshot& snapshot) noexcept;

    void noteGridPublicationFailure(
        NavigationGridPublicationFailureReason reason,
        NavigationLayerId layer = InvalidNavigationLayer,
        NavigationCellId cell = InvalidNavigationCell,
        uint64_t bridgeId = 0) noexcept
    {
        m_lastGridPublicationFailure = {
            .reason = reason,
            .layer = layer,
            .cell = cell,
            .bridgeId = bridgeId,
            .pathRevision = m_pathRevision,
        };
    }

    [[nodiscard]] NavigationSystemStatus publishDirtyTopology() noexcept;
    [[nodiscard]] NavigationSystemStatus beginDirtyTopologyPublication() noexcept;
    [[nodiscard]] NavigationSystemStatus advanceDirtyTopologyPublication(
        uint64_t workBudget) noexcept;
    [[nodiscard]] NavigationDynamicOverlayResult reserveBuildingTransaction(
        const NavigationBuildingEvent& event,
        container::Span<const NavigationCellId> footprint) noexcept;
    [[nodiscard]] NavigationDynamicOverlayResult reserveBridgeTransaction(
        const NavigationBridgeStateEvent& event,
        container::Span<const NavigationCellId> affectedCells) noexcept;
    [[nodiscard]] NavigationDynamicOverlayResult reserveTopologyTransaction(
        NavigationTopologyTransactionRecord record,
        container::Span<const NavigationCellId> cells) noexcept;
    [[nodiscard]] NavigationTopologyLedgerResult submitReservedTopologyTransactions(
        uint64_t confirmedTick) noexcept;
    [[nodiscard]] NavigationCellId* topologyLedgerCells(
        const NavigationTopologyTransactionRecord& record) noexcept;
    [[nodiscard]] const NavigationCellId* topologyLedgerCells(
        const NavigationTopologyTransactionRecord& record) const noexcept;
    void releaseFrontTopologyTransaction() noexcept;
    [[nodiscard]] NavigationSystemStatus publishBridgeLayers(
        container::Span<const NavigationBridgeDirtyChange> changes = {}) noexcept;
    [[nodiscard]] NavigationSystemStatus publishStartupTopology(bool advanceRevision = true) noexcept;
    [[nodiscard]] bool startupMutable() const noexcept;
    [[nodiscard]] bool compatibleLayerGrid(const NavigationGrid& grid, NavigationLayerId layer) const noexcept;
    [[nodiscard]] static bool compatibleGrids(const NavigationGrid& first, const NavigationGrid& second) noexcept;
    [[nodiscard]] const NavigationZoneField* findLayerZones(NavigationLayerId layer) const noexcept;
    [[nodiscard]] const NavigationZoneField* findLayerZones(
        NavigationLayerId layer,
        NavigationClearanceClass clearance) const noexcept;
    [[nodiscard]] NavigationZoneField* findLayerZonesMutable(
        NavigationLayerId layer,
        NavigationClearanceClass clearance = NavigationClearanceClass::Infantry) noexcept;
    [[nodiscard]] NavigationSystemStatus rebuildLayerZones(
        NavigationLayerId layer) noexcept;
    [[nodiscard]] container::Vector<NavigationBridgeLayerBinding>::iterator bridgeBindingPosition(
        uint64_t bridgeId) noexcept;
    [[nodiscard]] bool hasPortalForLayer(NavigationLayerId layer) const noexcept;
    [[nodiscard]] bool topologyPublicationPending() const noexcept;

    template <typename Unsigned>
    static void feed(uint64_t& hash, Unsigned value) noexcept
    {
        static_assert(std::is_unsigned_v<Unsigned>);
        uint64_t remaining = static_cast<uint64_t>(value);
        for (size_t byte = 0; byte < sizeof(Unsigned); ++byte)
        {
            hash ^= static_cast<uint8_t>(remaining & 0xffU);
            hash *= 1099511628211ULL;
            remaining >>= 8U;
        }
    }

    NavigationLayerSet m_staticLayers;
    NavigationLayerSet m_layers;
    // Work buffers retain the last published layers while a dirty publication
    // is being built. Path queries never observe these buffers until the final
    // swap, so a bounded publication cannot expose half-rasterized topology.
    NavigationLayerSet m_pendingLayers;
    container::Vector<NavigationZoneField> m_layerZones;
    container::Vector<NavigationZoneField> m_pendingLayerZones;
    // One reusable BFS queue for all layer/profile zone builds.  Completed
    // zone fields retain only zone ids, not five full-map frontier arrays.
    container::Vector<NavigationCellId> m_zoneBuildFrontier;
    NavigationPortalSet m_portals;
    NavigationPortalGraph m_portalGraph;
    NavigationDynamicOverlay m_dynamicOverlay;
    PathRepository m_pathRepository;
    inline static constexpr size_t PathServiceLaneCount = 2;
    inline static constexpr uint32_t PathServiceExpansionQuantum = 512;
    std::array<NavigationPathService, PathServiceLaneCount> m_pathServices;
    engine::ai::AIWaypointGraphResolver m_waypointGraphResolver;
    NavigationLayerId m_primaryLayer;
    NavigationProfileId m_primaryProfile;
    NavigationMovementMask m_primaryMovementMask = 0;
    NavigationCellBounds m_stagedStaticDirty;
    container::Vector<NavigationTopologyTransactionRecord> m_topologyLedger;
    NavigationCellArena m_topologyLedgerCells;
    uint32_t m_topologyLedgerCapacity = 0;
    uint64_t m_topologyGeneration = 0;
    uint64_t m_nextTopologyRevision = 0;
    bool m_topologyPublicationFailed = false;
    NavigationGridPublicationFailure m_lastGridPublicationFailure;
    container::Vector<NavigationBridgeLayerBinding> m_bridgeLayers;
    size_t m_bridgeBindingCapacity = 0;
    PortalTopologyRevision m_publishedPortalRevision;
    NavigationRevision m_pathRevision = InvalidNavigationRevision;
    uint64_t m_lastConfirmedTick = 0;
    bool m_startupTopologyDirty = false;
    container::Vector<NavigationWaterAreaRasterState> m_waterRasterAreas;
    container::Vector<NavigationMovementMask> m_waterRasterLandMasks;
    container::Vector<NavigationStaticCellUpdate> m_waterRasterUpdateScratch;
    uint64_t m_waterRasterSourceRevision = 0;
    uint64_t m_terrainHeightRevision = 0;
    container::Vector<NavigationStaticCellUpdate> m_terrainHeightUpdateScratch;
    NavigationCellBounds m_waterRasterPendingDirty;
    bool m_waterRasterInitialized = false;
    NavigationRevisionDelta m_pendingDirtyPublication;
    NavigationCellBounds m_pendingRasterBounds;
    NavigationCellBounds m_pendingClearanceBounds;
    uint64_t m_pendingRasterCursor = 0;
    uint64_t m_pendingClearanceCursor = 0;
    size_t m_pendingZoneBegin = 0;
    size_t m_pendingZoneIndex = 0;
    size_t m_pendingZoneEnd = 0;
    container::Vector<NavigationBridgeDirtyChange> m_pendingBridgeChanges;
    size_t m_pendingBridgeChangeIndex = 0;
    NavigationCellBounds m_pendingBridgeRasterBounds;
    NavigationCellBounds m_pendingBridgeClearanceBounds;
    uint64_t m_pendingBridgeRasterCursor = 0;
    uint64_t m_pendingBridgeClearanceCursor = 0;
    size_t m_pendingBridgeZoneBegin = 0;
    size_t m_pendingBridgeZoneIndex = 0;
    size_t m_pendingBridgeZoneEnd = 0;
    uint8_t m_pendingBridgePhase = 0;
    bool m_dirtyPublicationActive = false;
    bool m_initialized = false;
};

} // namespace engine::navigation
