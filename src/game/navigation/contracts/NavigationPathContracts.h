#pragma once

#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>

#include "game/object/ai/contracts/AIStateCommands.h"

namespace engine::ai
{

// Navigation owns the value protocol shared with object AI. Every
// asynchronous path and movement value is owned by one state request,
// one repath generation, and one source-order revision. Consumers must match
// all four fields; subject alone is never sufficient correlation.
struct PathCorrelation final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest;
    uint32_t generation = 0;
    uint64_t sourceOrderRevision = 0;
    AIAsyncOrderIdentity orderIdentity;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return subject.isValid() && stateRequest.isValid() && generation != 0 && sourceOrderRevision != 0;
    }
    constexpr bool operator==(const PathCorrelation&) const noexcept = default;
};

struct PathHandle final
{
    uint64_t value = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return value != 0;
    }
    explicit constexpr operator bool() const noexcept
    {
        return isValid();
    }
    constexpr bool operator==(const PathHandle&) const noexcept = default;
};

// Borrowed owner callback for PathHandles that are still resident in the AI
// transient inbox/outbox. It is intentionally excluded from snapshots. Once
// an InstallPath command reaches Movement, the ECS sidecar becomes the owner
// and this callback must no longer release that handle.
struct AIPathHandleReleaser final
{
    void* context = nullptr;
    void (*release)(void*, PathHandle) noexcept = nullptr;

    void operator()(PathHandle path) const noexcept
    {
        if (release && path)
            release(context, path);
    }

    [[nodiscard]] explicit constexpr operator bool() const noexcept
    {
        return release != nullptr;
    }
};

// Immutable point-sequence identity used by FollowPath and waypoint states.
// The repository remains outside AI storage; states retain only this handle,
// a content revision and their stable execution index.
struct AIPathSequenceHandle final
{
    uint64_t value = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return value != 0;
    }
    explicit constexpr operator bool() const noexcept
    {
        return isValid();
    }
    constexpr bool operator==(const AIPathSequenceHandle&) const noexcept = default;
};

enum class AIPathSequenceQueryStatus : uint8_t
{
    Point,
    End,
    Missing,
    StaleRevision,
    Unsupported,
};

struct AIPathSequencePoint final
{
    AIFixedPosition position;
    int64_t distanceToNextRaw = 0;
    bool hasNext = false;
    bool hasFollowing = false;
};

struct AIPathSequenceQuery final
{
    AIPathSequenceQueryStatus status = AIPathSequenceQueryStatus::Unsupported;
    AIPathSequencePoint point;
};

// Borrowed immutable repository adapter. The callback must be deterministic
// for (handle, revision, index) throughout one confirmed tick.
struct AIPathSequenceResolver final
{
    const void* context = nullptr;
    AIPathSequenceQuery (*queryPoint)(const void*, AIPathSequenceHandle, uint64_t, uint32_t) noexcept = nullptr;

    [[nodiscard]] AIPathSequenceQuery query(AIPathSequenceHandle handle,
                                            uint64_t revision,
                                            uint32_t index) const noexcept
    {
        if (!queryPoint || !handle || revision == 0)
            return {};
        return queryPoint(context, handle, revision, index);
    }
};

struct AIWaypointHandle final
{
    uint64_t value = 0;
    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    explicit constexpr operator bool() const noexcept { return isValid(); }
    constexpr bool operator==(const AIWaypointHandle&) const noexcept = default;
};

struct AITeamHandle final
{
    uint64_t value = 0;
    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    explicit constexpr operator bool() const noexcept { return isValid(); }
    constexpr bool operator==(const AITeamHandle&) const noexcept = default;
};

enum class AIWaypointQueryStatus : uint8_t
{
    Node,
    Missing,
    StaleRevision,
    Unsupported,
};

struct AIWaypointNode final
{
    AIFixedPosition position;
    uint32_t linkCount = 0;
    int64_t lookAheadDistanceRaw = 0;
    bool wall = false;
};

struct AIWaypointQuery final
{
    AIWaypointQueryStatus status = AIWaypointQueryStatus::Unsupported;
    AIWaypointNode node;
};

struct AIWaypointLinkQuery final
{
    AIWaypointQueryStatus status = AIWaypointQueryStatus::Unsupported;
    AIWaypointHandle target;
};

struct AIWaypointGraphResolver final
{
    const void* context = nullptr;
    AIWaypointQuery (*queryNode)(const void*, AIWaypointHandle, uint64_t) noexcept = nullptr;
    AIWaypointLinkQuery (*queryLink)(const void*, AIWaypointHandle, uint64_t, uint32_t) noexcept = nullptr;

    [[nodiscard]] AIWaypointQuery node(AIWaypointHandle handle, uint64_t revision) const noexcept
    {
        return queryNode && handle && revision != 0 ? queryNode(context, handle, revision) : AIWaypointQuery{};
    }
    [[nodiscard]] AIWaypointLinkQuery link(AIWaypointHandle handle,
                                           uint64_t revision,
                                           uint32_t index) const noexcept
    {
        return queryLink && handle && revision != 0 ? queryLink(context, handle, revision, index)
                                                    : AIWaypointLinkQuery{};
    }
};

struct AIWaypointTeamProgress final
{
    AITeamHandle team;
    AIWaypointHandle current;
    uint64_t revision = 0;
};

struct AIWaypointTeamProgressRequest final
{
    PathCorrelation correlation;
    AITeamHandle team;
    ObjectId subject = INVALID_OBJECT_ID;
    AIWaypointHandle arrived;
    uint64_t expectedRevision = 0;
};

struct AIWaypointCompletionEvent final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest;
    AIWaypointHandle terminal;
    uint64_t confirmedTick = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return subject.isValid() && stateRequest.isValid() && terminal;
    }

    constexpr bool operator==(
        const AIWaypointCompletionEvent&) const noexcept = default;
};

enum class AIMovementMode : uint8_t { Normal, Wander, Panic };

enum class AIPathTraversalMode : uint8_t
{
    Navmesh,
    WaypointPolyline,
    // RefCode AIUpdateInterface::computeQuickPath(): one immutable world-space
    // segment which deliberately bypasses the ground navigation occupancy
    // grid. It is used by airborne locomotors and by the first production-exit
    // leg while physics ignores the producing structure.
    DirectLine,
};

enum class PathRequestKind : uint8_t
{
    New,
    Patch,
    Cancel,
    Approach,
    Safe,
    MoveAside,
};

// One immutable, request-local projection of a confirmed mobile object onto
// a navigation cell.  Admission builds these values from ECS in stable
// (cell,ObjectId) order; the navigation worker never reads ECS and never
// writes the global dynamic overlay.  Multiple friendly records in one cell
// still produce one legacy ally cost, while hostile/neutral blockers reject
// the cell unless the mover's crusher profile admits them.
enum class AIPathObjectCellEffect : uint8_t
{
    FriendlyCost = 0,
    EnemyBlock,
    NeutralBlock,
};

struct AIPathObjectCellSnapshot final
{
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t layer = UINT32_MAX;
    uint32_t cell = UINT32_MAX;
    AIPathObjectCellEffect effect = AIPathObjectCellEffect::FriendlyCost;

    constexpr bool operator==(
        const AIPathObjectCellSnapshot&) const noexcept = default;
};

// Frozen by the runtime adapter from the subject's confirmed GeometryInfo.
// This is the complete ZH getRadiusAndCenter result, not a caller-authored
// hint: the admitted queue stores radius and the half-cell/grid-line phase by
// value so a later geometry mutation cannot change an in-flight search.
struct AIPathClearanceProfile final
{
    uint8_t radiusCells = 0;
    bool centerInCell = true;
    bool frozen = false;

    [[nodiscard]] constexpr bool validFrozen() const noexcept
    {
        return frozen && radiusCells <= 2U &&
               (radiusCells != 0U || centerInCell);
    }

    constexpr bool operator==(const AIPathClearanceProfile&) const noexcept = default;
};

struct PathRequest final
{
    PathCorrelation correlation;
    AIFixedPosition start;
    AIFixedPosition originalGoal;
    bool adjustDestinations = false;
    ObjectId ignoredObstacle = INVALID_OBJECT_ID;
    uint32_t surfaceMask = 0;
    AIPathClearanceProfile clearanceProfile;
    int64_t arrivalRadiusRaw = 0;
    // Attack Approach uses an annulus, not only a maximum radius. Producers
    // freeze weapon boundary distance; confirmed admission adds both object
    // footprints before the navigation worker observes this value.
    int64_t minimumArrivalRadiusRaw = 0;
    PathRequestKind kind = PathRequestKind::New;
    PathHandle currentPath;
    ObjectId safePathRepulsor = INVALID_OBJECT_ID;
    // Frozen at session admission for Safe requests. The navigation worker
    // must not read a mutable ECS transform while solving the path.
    AIFixedPosition safePathRepulsorPosition;
    int64_t safePathRadiusRaw = 0;
    ObjectId safePathRepulsor2 = INVALID_OBJECT_ID;
    AIFixedPosition safePathRepulsor2Position;
    AIPathTraversalMode traversalMode = AIPathTraversalMode::Navmesh;
    AIWaypointHandle waypointStart;
    uint64_t waypointGraphRevision = 0;
    uint32_t waypointHopLimit = 0;
    AIFixedPosition polylineOffset;
    // Nonzero identifies a Command/Transaction-owned shared group
    // centerline. Ordinal zero performs the sole A* search; Navigation clones
    // that immutable centerline with each member's fixed column offset.
    uint64_t groupPathId = 0;
    uint32_t groupPathMemberOrdinal = 0;
    uint32_t groupPathMemberCount = 0;
    AIFixedPosition groupPathOffset;
    int64_t extraDistanceRaw = 0;
    bool pathThroughUnits = false;
    bool preciseFinalZ = false;
    // Frozen locomotor appearance fact used only by aircraft QuickPath.
    // Wings must clip their orbit-sized destination away from tall blockers.
    bool airWings = false;
    // Frozen ThingTemplate::CrusherLevel. A nonzero crusher may cross an
    // authored non-defensive Fence footprint, but no other static obstacle.
    uint8_t crusherLevel = 0;
    // Sorted stable ObjectIds of dynamic obstacle owners which a Dozer may
    // cross because their relationship is not Enemies. This is request-local;
    // the navigation worker never queries PlayerRegistry or ECS.
    container::Vector<uint64_t> dozerPassableObstacles;
    // Attack goal identity and contact semantics are producer values. Every
    // field below lineOfSightEnabled is overwritten by confirmed admission.
    ObjectId attackTarget = INVALID_OBJECT_ID;
    bool attackContactWeapon = false;
    bool attackLineOfSightEnabled = false;
    ObjectId attackSubjectContainer = INVALID_OBJECT_ID;
    ObjectId attackTargetContainer = INVALID_OBJECT_ID;
    ObjectId attackSubjectSlaver = INVALID_OBJECT_ID;
    ObjectId attackTargetSlaver = INVALID_OBJECT_ID;
    container::Vector<uint64_t> attackSeeThroughObstacles;
    // Filled only by the confirmed session admission boundary.  Producers
    // cannot author occupancy, and queued/active searches retain this value
    // snapshot even if objects move while the worker is still searching.
    uint64_t objectSnapshotTick = 0;
    container::Vector<AIPathObjectCellSnapshot> objectCells;
    // Filled only by confirmed Navigation admission. This is not a nearest-
    // bridge hint: it identifies an inactive authored bridge layer whose two
    // portals connect the request's otherwise-disconnected endpoint zones.
    ObjectId blockingBridgeCandidate = INVALID_OBJECT_ID;

    constexpr bool operator==(const PathRequest&) const noexcept = default;
};

enum class PathFeedbackStatus : uint8_t
{
    Pending,
    Delayed,
    Ready,
    NoPath,
    Cancelled,
    Unsupported,
};

struct PathFeedback final
{
    PathCorrelation correlation;
    PathFeedbackStatus status = PathFeedbackStatus::Pending;
    uint64_t confirmedTick = 0;

    // Ready payload. A Ready value is valid only when path is nonzero.
    PathHandle path;
    AIFixedPosition adjustedGoal;
    uint32_t adjustedLayer = 0;
    bool retryPath = false;

    // Delayed payload. This is an absolute confirmed-tick boundary.
    uint64_t nextEligibleTick = 0;
    // Present only on NoPath after the admitted topology candidate survived
    // the actual path solve. AI may request repair for this exact bridge.
    ObjectId blockingBridge = INVALID_OBJECT_ID;
};

enum class MovementCommandKind : uint8_t
{
    InstallPath,
    // Transfers the currently installed immutable path to a temporary AI
    // state without acquiring, releasing, or recalculating it. RefCode's
    // AIMoveOutOfTheWayState continues AIUpdateInterface::getPath() and only
    // changes its collision policy when that path becomes stuck.
    RebindExistingPath,
    EndMovement,
};

struct MovementCommand final
{
    PathCorrelation correlation;
    MovementCommandKind kind = MovementCommandKind::InstallPath;
    PathHandle path;
    // Frozen from the state/path request that produced this installation.
    // Physics consumes this value from the installed ECS movement sidecar;
    // it must not reach back into mutable AI runtime state during pair tests.
    ObjectId ignoredObstacle = INVALID_OBJECT_ID;
    // Positive values cap locomotor speed for this installed path. Zero keeps
    // the object's own maximum (legacy FAST_AS_POSSIBLE).
    int64_t speedLimitRaw = 0;
    // Extends only the locomotor's distance-to-goal used for final braking.
    // It does not enlarge the completion radius: waypoint look-ahead keeps
    // speed through a node, while contact attacks must still reach it.
    int64_t extraDistanceRaw = 0;
    // Movement mode is part of the authoritative path installation.  The
    // previous split AIMovementModeCommand was emitted when Wander/Panic
    // entered, then discarded before the asynchronous path became ready, so
    // ObjectSimulation always installed Normal movement.  Carry the frozen
    // mode with the correlated command which actually transfers the path.
    AIMovementMode mode = AIMovementMode::Normal;
    bool panicking = false;
    bool clearGoal = false;
    bool preserveUltraAccurateFinalPosition = false;
    bool allowPathThroughUnits = false;
    uint64_t confirmedTick = 0;

    constexpr bool operator==(const MovementCommand&) const noexcept = default;
};

enum class MovementFeedbackStatus : uint8_t
{
    Started,
    Progress,
    // Compatibility spelling retained for staged kernels while the wire now
    // distinguishes the first accepted command from later progress.
    Moving = Progress,
    Completed,
    Blocked,
    Stuck,
    Cancelled,
    Unsupported,
};

[[nodiscard]] constexpr bool isMovementActiveFeedback(
    MovementFeedbackStatus status) noexcept
{
    return status == MovementFeedbackStatus::Started ||
           status == MovementFeedbackStatus::Progress;
}

struct MovementFeedback final
{
    PathCorrelation correlation;
    MovementFeedbackStatus status = MovementFeedbackStatus::Moving;
    uint64_t confirmedTick = 0;
    uint32_t blockedTicks = 0;
    int64_t alongPathDistanceRaw = 0;
    int64_t finalNodeXYDistanceRaw = 0;

    constexpr bool operator==(const MovementFeedback&) const noexcept = default;
};

template <typename Value, size_t CapacityValue = 4>
struct AIPathValueBuffer final
{
    static_assert(CapacityValue > 0);

    container::Array<Value, CapacityValue> values{};
    size_t count = 0;
    bool overflowed = false;

    [[nodiscard]] constexpr bool hasCapacity(
        size_t additional = 1) const noexcept
    {
        return count <= values.size() &&
               additional <= values.size() - count;
    }

    [[nodiscard]] bool push(const Value& value) noexcept
    {
        if (!hasCapacity())
        {
            overflowed = true;
            return false;
        }
        values[count++] = value;
        return true;
    }

    void clear() noexcept
    {
        count = 0;
        overflowed = false;
    }
};

using PathRequestBuffer = AIPathValueBuffer<PathRequest>;
using MovementCommandBuffer = AIPathValueBuffer<MovementCommand>;
using AIWaypointTeamProgressBuffer = AIPathValueBuffer<AIWaypointTeamProgressRequest>;
using AIWaypointCompletionBuffer = AIPathValueBuffer<AIWaypointCompletionEvent>;

// These ports are borrowed for one state executor call. Producers copy values
// into bounded sinks; feedback remains read-only and request-correlated.
struct AIPathServicePorts final
{
    PathRequestBuffer* pathRequests = nullptr;
    MovementCommandBuffer* movementCommands = nullptr;
    const PathFeedback* pathFeedback = nullptr;
    const MovementFeedback* movementFeedback = nullptr;
};

} // namespace engine::ai
