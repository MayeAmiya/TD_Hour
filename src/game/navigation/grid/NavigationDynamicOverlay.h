#pragma once

#include "NavigationCellArena.h"
#include "NavigationGrid.h"

#include "core/container/container_types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <tuple>
#include <type_traits>

namespace engine::navigation
{

struct StaticNavigationRevision final
{
    uint64_t value = 0;
    explicit constexpr operator bool() const noexcept { return value != 0; }
    constexpr auto operator<=>(const StaticNavigationRevision&) const noexcept = default;
};

struct DynamicObstacleRevision final
{
    uint64_t value = 0;
    explicit constexpr operator bool() const noexcept { return value != 0; }
    constexpr auto operator<=>(const DynamicObstacleRevision&) const noexcept = default;
};

struct PortalTopologyRevision final
{
    uint64_t value = 0;
    explicit constexpr operator bool() const noexcept { return value != 0; }
    constexpr auto operator<=>(const PortalTopologyRevision&) const noexcept = default;
};

struct NavigationRevisionSet final
{
    StaticNavigationRevision staticNavigation;
    DynamicObstacleRevision dynamicObstacles;
    PortalTopologyRevision portalTopology;
    constexpr bool operator==(const NavigationRevisionSet&) const noexcept = default;
};

struct NavigationTopologyTransactionStamp final
{
    uint64_t generation = 0;
    uint64_t revision = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return generation != 0 && revision != 0;
    }
    constexpr auto operator<=>(const NavigationTopologyTransactionStamp&) const noexcept = default;
};

// Inclusive integer cell bounds. An invalid value is the empty region.
struct NavigationCellBounds final
{
    int32_t minX = 0;
    int32_t minY = 0;
    int32_t maxX = -1;
    int32_t maxY = -1;

    [[nodiscard]] constexpr bool valid() const noexcept { return minX <= maxX && minY <= maxY; }
    constexpr bool operator==(const NavigationCellBounds&) const noexcept = default;

    [[nodiscard]] constexpr uint64_t cellCount() const noexcept
    {
        if (!valid())
            return 0;
        return static_cast<uint64_t>(static_cast<int64_t>(maxX) - minX + 1) *
               static_cast<uint64_t>(static_cast<int64_t>(maxY) - minY + 1);
    }

    void include(NavigationGridCoordinate coordinate) noexcept
    {
        if (!valid())
        {
            minX = maxX = coordinate.x;
            minY = maxY = coordinate.y;
            return;
        }
        minX = std::min(minX, coordinate.x);
        minY = std::min(minY, coordinate.y);
        maxX = std::max(maxX, coordinate.x);
        maxY = std::max(maxY, coordinate.y);
    }

    void include(NavigationCellId cell, uint32_t width) noexcept
    {
        include(coordinateFromCellId(cell, width));
    }

    void include(NavigationCellBounds other) noexcept
    {
        if (!other.valid())
            return;
        include(NavigationGridCoordinate{other.minX, other.minY});
        include(NavigationGridCoordinate{other.maxX, other.maxY});
    }
};

[[nodiscard]] constexpr bool intersects(NavigationCellBounds left, NavigationCellBounds right) noexcept
{
    return left.valid() && right.valid() && left.minX <= right.maxX && right.minX <= left.maxX &&
           left.minY <= right.maxY && right.minY <= left.maxY;
}

enum class NavigationDynamicEventReason : uint8_t
{
    BuildingPlaced = 0,
    FootprintChanged = 1,
    CompletionStateChanged = 2,
    BuildingDestroyed = 3,
    BridgeStateChanged = 4,
};

enum class NavigationBuildingState : uint8_t
{
    Absent = 0,
    Placed,
    Complete,
};

struct NavigationBuildingEvent final
{
    uint64_t confirmedTick = 0;
    uint64_t entityId = 0;
    NavigationDynamicEventReason reason = NavigationDynamicEventReason::BuildingPlaced;
    NavigationBuildingState state = NavigationBuildingState::Absent;
    bool blocksNavigation = false;
    // False retains the entity's existing sorted footprint. Place and geometry
    // changes normally set this to true; completion changes normally do not.
    bool replaceFootprint = false;
    NavigationTopologyTransactionStamp transaction;
    // ZH's AIRCRAFT_PATH_AROUND kind is a separate obstacle semantic. Ground
    // buildings do not set this value and therefore remain transparent to Air.
    bool blocksAirNavigation = false;
    // CELL_RUBBLE is a traversable typed surface, not an absent obstacle.
    // Ground locomotors cannot use it; Rubble and Air locomotors can.
    bool rubbleSurface = false;
    // Authored non-defensive Fence footprint. It remains a normal blocker for
    // most movers; request-local crusher policy may selectively ignore it.
    bool fenceSurface = false;
};

struct NavigationBridgeStateEvent final
{
    uint64_t confirmedTick = 0;
    uint64_t entityId = 0;
    bool active = false;
    NavigationTopologyTransactionStamp transaction;
};

struct NavigationDynamicOverlayConfig final
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t entityCapacity = 0;
    uint32_t bridgeCapacity = 0;
    uint32_t eventCapacity = 0;
    // Aggregate storage is (entityCapacity + eventCapacity) times this
    // baseline. It is a pool budget, not a per-footprint limit.
    uint32_t maxCellsPerFootprint = 0;
};

enum class NavigationDynamicOverlayResult : uint8_t
{
    Success = 0,
    NotInitialized,
    AlreadyInitialized,
    InvalidCapacity,
    AllocationOverflow,
    InvalidEvent,
    InvalidCell,
    DuplicateCell,
    DuplicateEventKey,
    TickAlreadySealed,
    EventCapacityExceeded,
    EntityCapacityExceeded,
    BridgeCapacityExceeded,
    FootprintCapacityExceeded,
    RefCountOverflow,
    RevisionExhausted,
};

enum class NavigationDynamicCommitStatus : uint8_t
{
    Idle = 0,
    Complete,
    BudgetExhausted,
    EntityCapacityExceeded,
    BridgeCapacityExceeded,
    RefCountOverflow,
    RevisionExhausted,
    InvalidEvent,
};

struct NavigationDynamicCommitResult final
{
    NavigationDynamicCommitStatus status = NavigationDynamicCommitStatus::Idle;
    uint32_t committedEvents = 0;
    uint32_t visitedCells = 0;
    uint32_t pendingEvents = 0;
    NavigationTopologyTransactionStamp lastCommittedTransaction;
};

enum class NavigationDirtyRebuildStatus : uint8_t
{
    Clean = 0,
    CommitPending,
    Progressed,
    Published,
};

struct NavigationDirtyRebuildResult final
{
    NavigationDirtyRebuildStatus status = NavigationDirtyRebuildStatus::Clean;
    uint64_t visitedCells = 0;
    uint64_t remainingCells = 0;
};

struct NavigationRevisionDelta final
{
    NavigationRevisionSet before;
    NavigationRevisionSet after;
    NavigationCellBounds dirtyCells;
    bool published = false;
};

struct NavigationBridgeDirtyChange final
{
    uint64_t bridgeId = 0;
    NavigationCellBounds affectedCells;
    constexpr bool operator==(const NavigationBridgeDirtyChange&) const noexcept = default;
};

struct NavigationPathMetadata final
{
    inline static constexpr size_t MaximumCorridorChunks = 32;
    NavigationCellBounds affectedCells;
    std::array<NavigationCellBounds, MaximumCorridorChunks> corridorChunks{};
    uint8_t corridorChunkCount = 0;
    NavigationRevisionSet revisions;
    NavigationLayerId layer = InvalidNavigationLayer;
};

struct NavigationPathStaleResult final
{
    bool stale = false;
    bool requiresAStarFallback = false;
};

struct NavigationPrecomputeMetadata final
{
    NavigationRevisionSet revisions;
};

struct NavigationPathRequestStamp final
{
    uint64_t requestRevision = 0;
    NavigationRevisionSet navigation;
};

struct NavigationPathFeedbackStamp final
{
    uint64_t requestRevision = 0;
    NavigationRevisionSet navigation;
};

[[nodiscard]] constexpr bool canInstallPathFeedback(const NavigationPathRequestStamp& current,
                                                    const NavigationPathFeedbackStamp& feedback) noexcept
{
    return current.requestRevision != 0 && current.requestRevision == feedback.requestRevision &&
           current.navigation == feedback.navigation;
}

struct NavigationAirObstacleQuery final
{
    uint64_t entityId = 0;
    NavigationCellBounds bounds;

    [[nodiscard]] constexpr bool found() const noexcept
    {
        return entityId != 0 && bounds.valid();
    }
};

// Deterministic value sidecar for dynamic navigation. initialize() performs all
// allocations. Queueing, sorting, add/update/remove, bounded commit, dirty
// publication, hashing, and queries allocate no storage.
class NavigationDynamicOverlay final
{
public:
    [[nodiscard]] NavigationDynamicOverlayResult initialize(const NavigationDynamicOverlayConfig& config);

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] size_t cellCount() const noexcept { return m_ownerCount.size(); }
    [[nodiscard]] NavigationRevisionSet revisions() const noexcept { return m_revisions; }
    [[nodiscard]] uint64_t stableHash() const noexcept { return m_initialized ? m_stableHash : 0; }
    [[nodiscard]] size_t pendingEventCount() const noexcept { return m_events.size(); }
    [[nodiscard]] bool commitInProgress() const noexcept { return m_activeEvent; }
    [[nodiscard]] bool hasUnpublishedDirty() const noexcept { return m_hasUnpublished; }
    [[nodiscard]] NavigationCellBounds unpublishedDirtyCells() const noexcept { return m_unpublishedDirty; }
    [[nodiscard]] NavigationRevisionDelta publishedDirty() const noexcept { return m_publishedDirty; }
    [[nodiscard]] container::Span<const NavigationBridgeDirtyChange>
    publishedBridgeChanges() const noexcept
    {
        return {m_bridgeDirtyChanges.data(), m_bridgeDirtyChangeCount};
    }
    [[nodiscard]] NavigationTopologyTransactionStamp lastCommittedTransaction() const noexcept
    {
        return m_lastCommittedTransaction;
    }

    [[nodiscard]] uint32_t ownerCount(NavigationCellId cell) const noexcept;
    [[nodiscard]] bool blocked(NavigationCellId cell) const noexcept;
    [[nodiscard]] uint32_t airOwnerCount(NavigationCellId cell) const noexcept;
    [[nodiscard]] bool blocksAirNavigation(NavigationCellId cell) const noexcept;
    [[nodiscard]] uint32_t rubbleOwnerCount(NavigationCellId cell) const noexcept;
    [[nodiscard]] bool rubbleSurface(NavigationCellId cell) const noexcept;
    [[nodiscard]] uint32_t fenceOwnerCount(NavigationCellId cell) const noexcept;
    [[nodiscard]] bool fenceSurface(NavigationCellId cell) const noexcept;
    [[nodiscard]] NavigationAirObstacleQuery airObstacleAt(
        NavigationCellId cell,
        uint64_t ignoredEntityId = 0) const noexcept;

    // Bridge state is retained by stable authored/runtime bridge id. Missing
    // bridges are inactive; callers which own a binding validate existence at
    // submission time and query only after the portal revision is published.
    [[nodiscard]] bool bridgeActive(uint64_t entityId) const noexcept;
    [[nodiscard]] bool containsEntity(uint64_t entityId) const noexcept;
    [[nodiscard]] bool ownsCell(uint64_t entityId, NavigationCellId cell) const noexcept;
    [[nodiscard]] bool entityFenceSurface(uint64_t entityId) const noexcept;

    [[nodiscard]] NavigationDynamicOverlayResult submitBuildingEvent(
        const NavigationBuildingEvent& event,
        container::Span<const NavigationCellId> footprint) noexcept;

    [[nodiscard]] NavigationDynamicOverlayResult submitBridgeEvent(
        const NavigationBridgeStateEvent& event,
        container::Span<const NavigationCellId> affectedCells) noexcept;

    [[nodiscard]] NavigationDynamicOverlayResult noteStaticNavigationChange(NavigationCellBounds dirty) noexcept;

    [[nodiscard]] NavigationDynamicCommitResult commitConfirmedTick(uint64_t confirmedTick,
                                                                    uint32_t eventBudget,
                                                                    uint32_t cellBudget) noexcept;

    // Map import authors tick-one footprints before simulation starts. Commit
    // that closed bootstrap batch without sealing tick one: scripts and
    // gameplay may still author additional tick-one changes later. This is
    // deliberately unavailable once any confirmed tick has been sealed.
    [[nodiscard]] NavigationDynamicCommitResult commitStartupEvents(
        uint32_t eventBudget, uint32_t cellBudget) noexcept;

    [[nodiscard]] NavigationDirtyRebuildResult rebuildDirtyRegion(uint64_t cellBudget) noexcept;

    [[nodiscard]] bool requiresAStarFallback(const NavigationPrecomputeMetadata& precompute) const noexcept;

    [[nodiscard]] NavigationPathStaleResult pathStaleness(const NavigationPathMetadata& path) const noexcept;

private:
    enum class EventKind : uint8_t
    {
        Building,
        Bridge,
    };

    struct EventRecord final
    {
        uint64_t confirmedTick = 0;
        uint64_t entityId = 0;
        NavigationDynamicEventReason reason = NavigationDynamicEventReason::BuildingPlaced;
        EventKind kind = EventKind::Building;
        NavigationBuildingState buildingState = NavigationBuildingState::Absent;
        bool value = false;
        bool replaceFootprint = false;
        NavigationCellArena::Range cells;
        uint32_t cellCount = 0;
        NavigationTopologyTransactionStamp transaction;
        bool blocksAirNavigation = false;
        bool rubbleSurface = false;
        bool fenceSurface = false;

        [[nodiscard]] static EventRecord building(const NavigationBuildingEvent& event) noexcept;
        [[nodiscard]] static EventRecord bridge(const NavigationBridgeStateEvent& event) noexcept;
    };

    struct EntityRecord final
    {
        uint64_t entityId = 0;
        NavigationBuildingState state = NavigationBuildingState::Absent;
        NavigationCellArena::Range cells;
        uint32_t cellCount = 0;
        bool blocksNavigation = false;
        bool blocksAirNavigation = false;
        bool occupied = false;
        NavigationTopologyTransactionStamp transaction;
        bool rubbleSurface = false;
        bool fenceSurface = false;
    };

    struct BridgeRecord final
    {
        uint64_t entityId = 0;
        uint32_t slot = 0;
        bool active = false;
        bool occupied = false;
        NavigationTopologyTransactionStamp transaction;
    };

    enum class ActivePhase : uint8_t
    {
        Occupancy,
        BridgeDirty,
        Finalize,
    };

    inline static constexpr size_t NoIndex = static_cast<size_t>(-1);

    [[nodiscard]] bool contains(NavigationCellId cell) const noexcept;
    [[nodiscard]] bool validBounds(NavigationCellBounds bounds) const noexcept;

    [[nodiscard]] static constexpr bool validBuildingState(NavigationBuildingState state) noexcept
    {
        return state == NavigationBuildingState::Absent || state == NavigationBuildingState::Placed ||
               state == NavigationBuildingState::Complete;
    }

    [[nodiscard]] static constexpr bool validBuildingReason(NavigationDynamicEventReason reason) noexcept
    {
        return reason == NavigationDynamicEventReason::BuildingPlaced ||
               reason == NavigationDynamicEventReason::FootprintChanged ||
               reason == NavigationDynamicEventReason::CompletionStateChanged ||
               reason == NavigationDynamicEventReason::BuildingDestroyed;
    }

    [[nodiscard]] NavigationCellId* entityCells(uint32_t slot) noexcept;
    [[nodiscard]] const NavigationCellId* entityCells(uint32_t slot) const noexcept;
    [[nodiscard]] NavigationCellId* eventCells(const EventRecord& event) noexcept;
    [[nodiscard]] const NavigationCellId* eventCells(const EventRecord& event) const noexcept;

    [[nodiscard]] NavigationDynamicOverlayResult submitEvent(EventRecord record,
                                                               container::Span<const NavigationCellId> cells) noexcept;
    void sortEvents() noexcept;
    [[nodiscard]] size_t findEntityOrder(uint64_t entityId) const noexcept;
    [[nodiscard]] size_t findBridgeOrder(uint64_t entityId) const noexcept;
    [[nodiscard]] NavigationDynamicCommitStatus startFrontEvent() noexcept;
    [[nodiscard]] NavigationDynamicCommitStatus startBuildingEvent(const EventRecord& event) noexcept;
    [[nodiscard]] NavigationDynamicCommitStatus startBridgeEvent(const EventRecord& event) noexcept;
    [[nodiscard]] NavigationDynamicCommitStatus advanceActiveEvent(bool& consumedCell, bool& completed) noexcept;
    [[nodiscard]] bool activeNeedsCell() const noexcept;
    [[nodiscard]] NavigationDynamicCommitStatus advanceBuildingEvent(bool& consumedCell, bool& completed) noexcept;
    [[nodiscard]] NavigationDynamicCommitStatus advanceBridgeEvent(bool& consumedCell, bool& completed) noexcept;
    void finalizeBuildingEvent(const EventRecord& event) noexcept;
    void addOwner(NavigationCellId cell) noexcept;
    void removeOwner(NavigationCellId cell) noexcept;
    void addAirOwner(NavigationCellId cell) noexcept;
    void removeAirOwner(NavigationCellId cell) noexcept;
    void addRubbleOwner(NavigationCellId cell) noexcept;
    void removeRubbleOwner(NavigationCellId cell) noexcept;
    void addFenceOwner(NavigationCellId cell) noexcept;
    void removeFenceOwner(NavigationCellId cell) noexcept;
    void beginRevisionDelta() noexcept;
    void includeDirty(NavigationCellId cell) noexcept;
    void includeDirty(NavigationCellBounds bounds) noexcept;
    void recordBridgeDirtyChange(const EventRecord& event) noexcept;
    void popFrontEvent() noexcept;
    [[nodiscard]] bool hasSealedPendingEvent() const noexcept;
    [[nodiscard]] uint64_t dirtyRemaining() const noexcept;

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

    void refreshStableHash() noexcept;

    inline static constexpr uint32_t HashSchemaVersion = 8;
    bool m_initialized = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_eventCapacity = 0;
    NavigationRevisionSet m_revisions;
    container::Vector<uint32_t> m_ownerCount;
    container::Vector<uint32_t> m_airOwnerCount;
    container::Vector<uint32_t> m_rubbleOwnerCount;
    container::Vector<uint32_t> m_fenceOwnerCount;
    container::Vector<EntityRecord> m_entities;
    container::Vector<uint32_t> m_entityOrder;
    container::Vector<uint32_t> m_freeEntities;
    container::Vector<BridgeRecord> m_bridges;
    container::Vector<uint32_t> m_bridgeOrder;
    container::Vector<uint32_t> m_freeBridges;
    container::Vector<EventRecord> m_events;
    NavigationCellArena m_footprintCells;
    container::Vector<NavigationBridgeDirtyChange> m_bridgeDirtyChanges;
    size_t m_bridgeDirtyChangeCount = 0;
    uint64_t m_sealedThroughTick = 0;
    bool m_activeEvent = false;
    EventKind m_activeKind = EventKind::Building;
    ActivePhase m_activePhase = ActivePhase::Occupancy;
    uint32_t m_activeSlot = 0;
    bool m_activeWasNew = false;
    uint32_t m_activeOldCount = 0;
    uint32_t m_activeTargetCount = 0;
    bool m_activeOldBlocks = false;
    bool m_activeTargetBlocks = false;
    bool m_activeOldAirBlocks = false;
    bool m_activeTargetAirBlocks = false;
    bool m_activeOldRubble = false;
    bool m_activeTargetRubble = false;
    bool m_activeOldFence = false;
    bool m_activeTargetFence = false;
    // True only when this in-progress building event changed the aggregate
    // published grid (owner count 0 <-> 1). Entity metadata and overlapping
    // ownership may change without requiring a topology revision.
    bool m_activeTopologyChanged = false;
    uint32_t m_activeOldIndex = 0;
    uint32_t m_activeTargetIndex = 0;
    bool m_hasUnpublished = false;
    NavigationRevisionSet m_dirtyBefore;
    NavigationCellBounds m_unpublishedDirty;
    uint64_t m_dirtyCellsVisited = 0;
    NavigationRevisionDelta m_publishedDirty;
    NavigationTopologyTransactionStamp m_lastCommittedTransaction;
    uint64_t m_stableHash = 0;
};

// Detached value input for the attack line-of-sight walk. Entity ids are the
// same stable ObjectId values that submitBuildingEvent() published, so the
// query keeps no ECS handle and no registry pointer.
struct NavigationAttackLineOfSightRequest final
{
    NavigationWorldPosition attacker{};
    NavigationWorldPosition victim{};
    // RefCode attackBlockedByObstacleCallback never lets the attacker's own
    // footprint, the victim's own footprint, either side's container, or
    // either side's slaver block the shot.
    uint64_t attackerEntityId = 0;
    uint64_t victimEntityId = 0;
    uint64_t attackerContainerEntityId = 0;
    uint64_t victimContainerEntityId = 0;
    uint64_t attackerSlaverEntityId = 0;
    uint64_t victimSlaverEntityId = 0;
    // Sorted, deduplicated ObjectIds of live obstacles authored
    // KINDOF_CAN_SEE_THROUGH_STRUCTURE. RefCode caches this per pathfind cell
    // as PathfindCell::m_obstacleIsTransparent; here the same decision is
    // reconstructed by comparing a cell's total owner count against how many
    // of these transparent owners cover it, which needs no new overlay state
    // and therefore leaves the committed topology hash untouched.
    container::Span<const uint64_t> seeThroughEntityIds{};
};

// Pathfinder::isAttackViewBlockedByObstacle, restricted to the dynamic
// obstacle field. Callers own the KINDOF_ATTACK_NEEDS_LINE_OF_SIGHT and
// AIData.AttackUsesLineOfSight gates; this function answers only "does a
// non-exempt, non-transparent obstacle cell lie on the segment".
[[nodiscard]] inline bool attackViewBlockedByObstacle(
    const NavigationGrid& grid,
    const NavigationDynamicOverlay& overlay,
    const NavigationAttackLineOfSightRequest& request) noexcept
{
    if (!grid.isInitialized() || !overlay.isInitialized())
        return false;
    const NavigationCellId from = grid.cellAt(request.attacker);
    const NavigationCellId to = grid.cellAt(request.victim);
    if (!from || !to || from == to)
        return false;

    const auto exempt = [&overlay, &request](NavigationCellId cell) noexcept {
        for (const uint64_t entityId : {request.attackerEntityId,
                                        request.victimEntityId,
                                        request.attackerContainerEntityId,
                                        request.victimContainerEntityId,
                                        request.attackerSlaverEntityId,
                                        request.victimSlaverEntityId}) {
            if (entityId != 0 && overlay.ownsCell(entityId, cell))
                return true;
        }
        // Every obstacle covering the cell is see-through. Owner counts are
        // exact, so an opaque co-owner keeps the cell blocking.
        const uint32_t owners = overlay.ownerCount(cell);
        if (owners == 0)
            return true;
        uint32_t transparent = 0;
        for (const uint64_t entityId : request.seeThroughEntityIds) {
            if (entityId != 0 && overlay.ownsCell(entityId, cell))
                ++transparent;
        }
        return transparent >= owners;
    };

    const NavigationGridCoordinate start = grid.coordinate(from);
    const NavigationGridCoordinate goal = grid.coordinate(to);
    const int64_t deltaX = static_cast<int64_t>(goal.x) - start.x;
    const int64_t deltaY = static_cast<int64_t>(goal.y) - start.y;
    const int32_t stepX = deltaX < 0 ? -1 : 1;
    const int32_t stepY = deltaY < 0 ? -1 : 1;
    const uint64_t stepsX =
        static_cast<uint64_t>(deltaX < 0 ? -deltaX : deltaX);
    const uint64_t stepsY =
        static_cast<uint64_t>(deltaY < 0 ? -deltaY : deltaY);

    int32_t x = start.x;
    int32_t y = start.y;
    uint64_t advancedX = 0;
    uint64_t advancedY = 0;
    // The attacker's own cell is never tested, and the victim's cell is always
    // exempt: RefCode seeds ViewAttackBlockedStruct::victimCell from the
    // victim position and then skips any obstacle also present in that cell,
    // which the terminal cell trivially satisfies.
    while (advancedX != stepsX || advancedY != stepsY)
    {
        // Integer comparison of the next vertical and horizontal boundary
        // crossing. Equality is a corner crossing; both cardinal neighbours
        // are inspected before the diagonal step, matching the strict corner
        // policy used by the navigation grid itself.
        const uint64_t horizontal = (advancedX * 2U + 1U) * stepsY;
        const uint64_t vertical = (advancedY * 2U + 1U) * stepsX;
        if (horizontal == vertical)
        {
            const NavigationCellId sideX = grid.cellId({x + stepX, y});
            const NavigationCellId sideY = grid.cellId({x, y + stepY});
            const bool sideXBlocks = overlay.blocked(sideX) && !exempt(sideX);
            const bool sideYBlocks = overlay.blocked(sideY) && !exempt(sideY);
            if (sideXBlocks && sideYBlocks)
                return true;
            x += stepX;
            y += stepY;
            ++advancedX;
            ++advancedY;
        }
        else if (horizontal < vertical)
        {
            x += stepX;
            ++advancedX;
        }
        else
        {
            y += stepY;
            ++advancedY;
        }
        const NavigationCellId next = grid.cellId({x, y});
        if (next == to)
            break;
        if (overlay.blocked(next) && !exempt(next))
            return true;
    }
    return false;
}

} // namespace engine::navigation
