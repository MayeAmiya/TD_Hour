#pragma once

#include "NavigationLayers.h"
#include "NavigationZones.h"
#include "../search/AStarOracle.h"

#include "core/container/container_types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <type_traits>

namespace engine::navigation
{

enum class NavigationPortalDirection : uint8_t
{
    TwoWay = 0,
    AtoB,
    BtoA,
};

// A portal is a self-contained value. In particular, it does not retain a
// grid, layer, zone, object, or service pointer.
struct NavigationPortal final
{
    NavigationPortalId id = InvalidNavigationPortal;
    NavigationLayerCell sideA;
    NavigationLayerCell sideB;
    NavigationPortalDirection direction = NavigationPortalDirection::TwoWay;
    NavigationProfileId profile = InvalidNavigationProfile;
    uint32_t traversalCost = 0;
    bool active = false;
    constexpr bool operator==(const NavigationPortal&) const noexcept = default;
};

enum class NavigationPortalSetResult : uint8_t
{
    Success = 0,
    InvalidCapacity,
    InvalidPortal,
    InvalidEndpoint,
    DuplicatePortal,
    CapacityExceeded,
};

// Owns portal values in stable-id order. initialize() is the only operation
// that grows storage; successful addPortal() calls within capacity do not
// allocate.
class NavigationPortalSet final
{
public:
    [[nodiscard]] NavigationPortalSetResult initialize(size_t portalCapacity)
    {
        if (portalCapacity > MaxPortalCapacity)
            return NavigationPortalSetResult::InvalidCapacity;
        m_portals.clear();
        m_portals.reserve(portalCapacity);
        m_capacity = portalCapacity;
        return NavigationPortalSetResult::Success;
    }

    [[nodiscard]] NavigationPortalSetResult addPortal(const NavigationLayerSet& layers,
                                                      const NavigationPortal& portal)
    {
        if (!portal.id || !portal.profile || !validDirection(portal.direction) ||
            portal.traversalCost == NavigationSearchScratch::InfiniteCost)
            return NavigationPortalSetResult::InvalidPortal;
        if (!validEndpoint(layers, portal.sideA) || !validEndpoint(layers, portal.sideB) ||
            portal.sideA == portal.sideB)
            return NavigationPortalSetResult::InvalidEndpoint;

        const auto position = std::lower_bound(
            m_portals.begin(), m_portals.end(), portal.id,
            [](const NavigationPortal& value, NavigationPortalId id) { return value.id < id; });
        if (position != m_portals.end() && position->id == portal.id)
            return NavigationPortalSetResult::DuplicatePortal;
        if (m_portals.size() == m_capacity)
            return NavigationPortalSetResult::CapacityExceeded;
        m_portals.insert(position, portal);
        return NavigationPortalSetResult::Success;
    }

    [[nodiscard]] size_t size() const noexcept { return m_portals.size(); }
    [[nodiscard]] size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] container::Span<const NavigationPortal> portals() const noexcept { return m_portals; }

    [[nodiscard]] const NavigationPortal* find(NavigationPortalId id) const noexcept
    {
        const auto position = std::lower_bound(
            m_portals.begin(), m_portals.end(), id,
            [](const NavigationPortal& value, NavigationPortalId sought) { return value.id < sought; });
        return position != m_portals.end() && position->id == id ? &*position : nullptr;
    }

    // Runtime bridge state changes mutate the preallocated canonical portal
    // record in place. The graph owner rebuilds its allocation-free derived
    // edge list after one or more mutations.
    [[nodiscard]] bool setActive(NavigationPortalId id, bool active) noexcept
    {
        const auto position = std::lower_bound(
            m_portals.begin(), m_portals.end(), id,
            [](const NavigationPortal& value, NavigationPortalId sought) { return value.id < sought; });
        if (position == m_portals.end() || position->id != id)
            return false;
        position->active = active;
        return true;
    }

    // Returns the number of portals owned by the layer (including portals
    // already in the requested state), allowing callers to distinguish an
    // empty layer binding without allocating a temporary ID list.
    [[nodiscard]] size_t setActiveByLayer(NavigationLayerId layer, bool active) noexcept
    {
        if (!layer)
            return 0;
        size_t matched = 0;
        for (NavigationPortal& portal : m_portals)
        {
            if (portal.sideA.layer != layer && portal.sideB.layer != layer)
                continue;
            portal.active = active;
            ++matched;
        }
        return matched;
    }

    [[nodiscard]] uint64_t stableHash() const noexcept
    {
        uint64_t hash = 14695981039346656037ULL;
        feed(hash, HashSchemaVersion);
        feed(hash, static_cast<uint64_t>(m_portals.size()));
        for (const NavigationPortal& portal : m_portals)
        {
            feed(hash, portal.id.value);
            feed(hash, portal.sideA.layer.value);
            feed(hash, portal.sideA.cell.value);
            feed(hash, portal.sideB.layer.value);
            feed(hash, portal.sideB.cell.value);
            feed(hash, static_cast<uint8_t>(portal.direction));
            feed(hash, portal.profile.value);
            feed(hash, portal.traversalCost);
            feed(hash, static_cast<uint8_t>(portal.active));
        }
        return hash;
    }

private:
    [[nodiscard]] static bool validEndpoint(const NavigationLayerSet& layers,
                                            NavigationLayerCell endpoint) noexcept
    {
        if (!endpoint)
            return false;
        const NavigationGrid* grid = layers.find(endpoint.layer);
        return grid != nullptr && grid->contains(endpoint.cell);
    }

    [[nodiscard]] static constexpr bool validDirection(NavigationPortalDirection direction) noexcept
    {
        return direction == NavigationPortalDirection::TwoWay || direction == NavigationPortalDirection::AtoB ||
               direction == NavigationPortalDirection::BtoA;
    }

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

    inline static constexpr size_t MaxPortalCapacity =
        (static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - 2U) / 2U;
    inline static constexpr uint32_t HashSchemaVersion = 1;
    container::Vector<NavigationPortal> m_portals;
    size_t m_capacity = 0;
};

struct NavigationCoarsePortalEdge final
{
    uint32_t fromEndpoint = 0;
    uint32_t toEndpoint = 0;
    NavigationPortalId portal = InvalidNavigationPortal;
    NavigationProfileId profile = InvalidNavigationProfile;
    uint32_t traversalCost = 0;
};

enum class NavigationPortalGraphBuildResult : uint8_t
{
    Success = 0,
    InvalidCapacity,
    CapacityExceeded,
};

// Endpoint indices are canonical: portal 0 contributes A then B, followed by
// portal 1, and so on. Directed coarse edges are derived from direction and
// active state during build().
class NavigationPortalGraph final
{
public:
    [[nodiscard]] NavigationPortalGraphBuildResult initialize(size_t portalCapacity)
    {
        if (portalCapacity > MaxPortalCapacity)
            return NavigationPortalGraphBuildResult::InvalidCapacity;
        m_endpoints.clear();
        m_edges.clear();
        m_endpoints.reserve(portalCapacity * 2U);
        m_edges.reserve(portalCapacity * 2U);
        m_portalCapacity = portalCapacity;
        m_stableHash = 0;
        return NavigationPortalGraphBuildResult::Success;
    }

    [[nodiscard]] NavigationPortalGraphBuildResult build(const NavigationPortalSet& portals)
    {
        m_endpoints.clear();
        m_edges.clear();
        m_stableHash = 0;
        if (portals.size() > m_portalCapacity)
            return NavigationPortalGraphBuildResult::CapacityExceeded;

        uint32_t endpoint = 0;
        for (const NavigationPortal& portal : portals.portals())
        {
            m_endpoints.push_back(portal.sideA);
            m_endpoints.push_back(portal.sideB);
            if (portal.active && (portal.direction == NavigationPortalDirection::TwoWay ||
                                  portal.direction == NavigationPortalDirection::AtoB))
                m_edges.push_back({endpoint, endpoint + 1U, portal.id, portal.profile, portal.traversalCost});
            if (portal.active && (portal.direction == NavigationPortalDirection::TwoWay ||
                                  portal.direction == NavigationPortalDirection::BtoA))
                m_edges.push_back({endpoint + 1U, endpoint, portal.id, portal.profile, portal.traversalCost});
            endpoint += 2U;
        }
        m_stableHash = portals.stableHash();
        return NavigationPortalGraphBuildResult::Success;
    }

    // Runtime bridge events do not add or remove authored portals; they only
    // toggle the directed edges for one layer. Keep the canonical endpoint
    // array and refresh only edges owned by that layer.
    [[nodiscard]] NavigationPortalGraphBuildResult updateActiveForLayer(
        const NavigationPortalSet& portals,
        NavigationLayerId layer) noexcept
    {
        if (!layer || portals.size() * 2U != m_endpoints.size() ||
            portals.size() > m_portalCapacity)
            return NavigationPortalGraphBuildResult::CapacityExceeded;
        for (const NavigationPortal& portal : portals.portals())
        {
            if (portal.sideA.layer != layer && portal.sideB.layer != layer)
                continue;
            m_edges.erase(std::remove_if(
                              m_edges.begin(), m_edges.end(),
                              [&portal](const NavigationCoarsePortalEdge& edge) {
                                  return edge.portal == portal.id;
                              }),
                          m_edges.end());
            const auto portalPosition = std::lower_bound(
                portals.portals().begin(), portals.portals().end(), portal.id,
                [](const NavigationPortal& value, NavigationPortalId id) {
                    return value.id < id;
                });
            if (portalPosition == portals.portals().end() ||
                portalPosition->id != portal.id)
                return NavigationPortalGraphBuildResult::CapacityExceeded;
            const uint32_t endpoint = static_cast<uint32_t>(
                portalPosition - portals.portals().begin()) * 2U;
            if (portal.active &&
                (portal.direction == NavigationPortalDirection::TwoWay ||
                 portal.direction == NavigationPortalDirection::AtoB))
                m_edges.push_back({endpoint, endpoint + 1U, portal.id,
                                   portal.profile, portal.traversalCost});
            if (portal.active &&
                (portal.direction == NavigationPortalDirection::TwoWay ||
                 portal.direction == NavigationPortalDirection::BtoA))
                m_edges.push_back({endpoint + 1U, endpoint, portal.id,
                                   portal.profile, portal.traversalCost});
        }
        std::sort(m_edges.begin(), m_edges.end(),
                  [](const NavigationCoarsePortalEdge& left,
                     const NavigationCoarsePortalEdge& right) {
                      return std::tuple(left.fromEndpoint, left.toEndpoint,
                                        left.portal.value) <
                          std::tuple(right.fromEndpoint, right.toEndpoint,
                                     right.portal.value);
                  });
        m_stableHash = portals.stableHash();
        return NavigationPortalGraphBuildResult::Success;
    }

    [[nodiscard]] size_t portalCapacity() const noexcept { return m_portalCapacity; }
    [[nodiscard]] size_t endpointCount() const noexcept { return m_endpoints.size(); }
    [[nodiscard]] size_t edgeCount() const noexcept { return m_edges.size(); }
    [[nodiscard]] container::Span<const NavigationLayerCell> endpoints() const noexcept { return m_endpoints; }
    [[nodiscard]] container::Span<const NavigationCoarsePortalEdge> edges() const noexcept { return m_edges; }
    [[nodiscard]] uint64_t stableHash() const noexcept { return m_stableHash; }

private:
    inline static constexpr size_t MaxPortalCapacity =
        (static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - 2U) / 2U;
    container::Vector<NavigationLayerCell> m_endpoints;
    container::Vector<NavigationCoarsePortalEdge> m_edges;
    size_t m_portalCapacity = 0;
    uint64_t m_stableHash = 0;
};

enum class NavigationPortalRouteStatus : uint8_t
{
    Success = 0,
    Pending,
    NoPath,
    InvalidRequest,
    CapacityExceeded,
    OutputCapacityExceeded,
};

struct NavigationPortalRouteRequest final
{
    uint64_t requestId = 0;
    NavigationLayerCell start;
    NavigationLayerCell goal;
    NavigationProfileId profile = InvalidNavigationProfile;
    NavigationMovementMask movementMask = 0;
    NavigationClearanceClass clearance = NavigationClearanceClass::Infantry;
    uint64_t ignoredObstacle = 0;
    uint8_t crusherLevel = 0;
    container::Span<const uint64_t> dozerPassableObstacles{};
    container::Span<const engine::ai::AIPathObjectCellSnapshot>
        objectCells{};
};

struct NavigationPortalRouteResult final
{
    NavigationPortalRouteStatus status = NavigationPortalRouteStatus::InvalidRequest;
    size_t requiredPointCount = 0;
    uint32_t totalCost = NavigationSearchScratch::InfiniteCost;
    uint32_t portalCount = 0;
};

// All route-time arrays, including the A* heap/parents and raw segment buffer,
// are allocated by initialize(). route() performs no dynamic allocation.
class NavigationPortalRouteScratch final
{
public:
    [[nodiscard]] bool initialize(size_t portalCapacity, size_t cellCapacity)
    {
        if (portalCapacity > MaxPortalCapacity || cellCapacity == 0 ||
            cellCapacity >= std::numeric_limits<uint32_t>::max())
            return false;
        const size_t nodeCapacity = 2U + portalCapacity * 2U;
        m_nodes.assign(nodeCapacity, {});
        m_cost.assign(nodeCapacity, NavigationSearchScratch::InfiniteCost);
        m_parent.assign(nodeCapacity, InvalidNode);
        m_parentKind.assign(nodeCapacity, ParentNone);
        m_closed.assign(nodeCapacity, 0);
        m_routeNodes.assign(nodeCapacity, InvalidNode);
        m_rawCells.assign(cellCapacity, InvalidNavigationCell);
        if (!m_search.initialize(cellCapacity, cellCapacity))
            return false;
        m_portalCapacity = portalCapacity;
        return true;
    }

    [[nodiscard]] size_t portalCapacity() const noexcept { return m_portalCapacity; }
    [[nodiscard]] size_t cellCapacity() const noexcept { return m_search.cellCapacity(); }
    [[nodiscard]] uint32_t workConsumedLastStep() const noexcept
    {
        return m_workConsumedLastStep;
    }
    void rebindBorrowedSpans(
        container::Span<const uint64_t> dozerPassableObstacles,
        container::Span<const engine::ai::AIPathObjectCellSnapshot>
            objectCells) noexcept
    {
        m_request.dozerPassableObstacles = dozerPassableObstacles;
        m_request.objectCells = objectCells;
        m_segmentOracle.rebindBorrowedSpans(
            dozerPassableObstacles, {}, {}, {});
    }
    [[nodiscard]] uint64_t stableHash() const noexcept
    {
        uint64_t hash = 14695981039346656037ULL;
        const auto feed = [&hash](uint64_t value) noexcept {
            for (size_t byte = 0; byte < sizeof(value); ++byte)
            {
                hash ^= static_cast<uint8_t>(value & 0xffU);
                hash *= 1099511628211ULL;
                value >>= 8U;
            }
        };
        feed(2);
        feed(static_cast<uint64_t>(m_request.objectCells.size()));
        for (const engine::ai::AIPathObjectCellSnapshot& objectCell :
             m_request.objectCells) {
            feed(objectCell.layer);
            feed(objectCell.cell);
            feed(objectCell.object.value);
            feed(static_cast<uint8_t>(objectCell.effect));
        }
        feed(static_cast<uint8_t>(m_routePhase));
        feed(static_cast<uint64_t>(m_nodeCount));
        feed(m_currentNode);
        feed(m_targetNode);
        feed(m_segmentFromNode);
        feed(m_segmentToNode);
        feed(m_edgeIndex);
        feed(static_cast<uint8_t>(m_segmentActive));
        feed(m_routeCount);
        feed(static_cast<uint64_t>(m_routeCursor));
        feed(static_cast<uint64_t>(m_outputCount));
        feed(m_outputPortalCount);
        feed(static_cast<uint64_t>(m_segmentPointIndex));
        feed(m_workConsumedLastStep);
        feed(m_layersHash);
        feed(m_graphHash);
        feed(m_zonesHash);
        feed(m_segmentOracle.totalExpansions());
        feed(m_segmentOracle.traceHash());
        for (size_t index = 0; index < m_nodeCount; ++index)
        {
            feed(m_nodes[index].layer.value);
            feed(m_nodes[index].cell.value);
            feed(m_cost[index]);
            feed(m_parent[index]);
            feed(m_parentKind[index]);
            feed(m_closed[index]);
        }
        for (size_t index = 0; index < m_routeCount; ++index)
            feed(m_routeNodes[index]);
        return hash;
    }

private:
    friend class NavigationPortalRouter;
    enum class RoutePhase : uint8_t
    {
        Idle = 0,
        CoarseSelect,
        CoarseLayerSegments,
        CoarsePortalEdges,
        Reconstruct,
        ReconstructSegment,
        Complete,
        Failed,
    };
    inline static constexpr uint32_t InvalidNode = std::numeric_limits<uint32_t>::max();
    inline static constexpr uint8_t ParentNone = 0;
    inline static constexpr uint8_t ParentLayer = 1;
    inline static constexpr uint8_t ParentPortal = 2;
    inline static constexpr size_t MaxPortalCapacity =
        (static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - 2U) / 2U;

    NavigationSearchScratch m_search;
    container::Vector<NavigationLayerCell> m_nodes;
    container::Vector<uint32_t> m_cost;
    container::Vector<uint32_t> m_parent;
    container::Vector<uint8_t> m_parentKind;
    container::Vector<uint8_t> m_closed;
    container::Vector<uint32_t> m_routeNodes;
    container::Vector<NavigationCellId> m_rawCells;
    size_t m_portalCapacity = 0;
    NavigationPortalRouteRequest m_request;
    RoutePhase m_routePhase = RoutePhase::Idle;
    size_t m_nodeCount = 0;
    uint32_t m_currentNode = NavigationPortalRouteScratch::InvalidNode;
    uint32_t m_targetNode = NavigationPortalRouteScratch::InvalidNode;
    uint32_t m_segmentFromNode = NavigationPortalRouteScratch::InvalidNode;
    uint32_t m_segmentToNode = NavigationPortalRouteScratch::InvalidNode;
    uint32_t m_edgeIndex = 0;
    bool m_segmentActive = false;
    uint32_t m_routeCount = 0;
    size_t m_routeCursor = 0;
    size_t m_outputCount = 0;
    uint32_t m_outputPortalCount = 0;
    size_t m_segmentPointIndex = 0;
    uint32_t m_workConsumedLastStep = 0;
    uint64_t m_layersHash = 0;
    uint64_t m_graphHash = 0;
    uint64_t m_zonesHash = 0;
    AStarOracle m_segmentOracle;
    NavigationPortalRouteResult m_result;
};

class NavigationPortalRouter final
{
public:
    [[nodiscard]] NavigationPortalRouteResult beginRoute(
        const NavigationLayerSet& layers,
        container::Span<const NavigationZoneField> zones,
        const NavigationPortalGraph& graph,
        NavigationPortalRouteScratch& scratch,
        const NavigationPortalRouteRequest& request,
        const NavigationDynamicOverlay* dynamicOverlay = nullptr) const noexcept
    {
        scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::Failed;
        scratch.m_result = {};
        const NavigationGrid* startGrid = layers.find(request.start.layer);
        const NavigationGrid* goalGrid = layers.find(request.goal.layer);
        if (request.requestId == 0 || !request.profile || request.movementMask == 0 ||
            !request.start || !request.goal ||
            !validClearanceClass(request.clearance) || startGrid == nullptr ||
            goalGrid == nullptr ||
            !AStarOracle::allowsTraversalCell(
                *startGrid,
                makeSearchRequest(request, request.start, request.goal),
                request.start.cell, dynamicOverlay, true) ||
            !AStarOracle::allowsTraversalCell(
                *goalGrid,
                makeSearchRequest(request, request.goal, request.start),
                request.goal.cell, dynamicOverlay, false))
            return {NavigationPortalRouteStatus::InvalidRequest};
        const size_t nodeCount = 2U + graph.endpointCount();
        if (graph.endpointCount() / 2U > scratch.m_portalCapacity ||
            nodeCount > scratch.m_nodes.size() ||
            startGrid->cellCount() > scratch.m_search.cellCapacity() ||
            goalGrid->cellCount() > scratch.m_search.cellCapacity())
            return capacityResult();
        for (NavigationLayerCell endpoint : graph.endpoints())
        {
            const NavigationGrid* grid = layers.find(endpoint.layer);
            if (grid == nullptr || !grid->contains(endpoint.cell) ||
                grid->cellCount() > scratch.m_search.cellCapacity())
                return invalidResult();
        }
        scratch.m_request = request;
        scratch.m_nodeCount = nodeCount;
        scratch.m_layersHash = layers.stableHash();
        scratch.m_graphHash = graph.stableHash();
        scratch.m_zonesHash = 14695981039346656037ULL;
        for (const NavigationZoneField& zone : zones)
        {
            scratch.m_zonesHash ^= zone.stableHash();
            scratch.m_zonesHash *= 1099511628211ULL;
        }
        scratch.m_nodes[0] = request.start;
        scratch.m_nodes[1] = request.goal;
        for (size_t index = 0; index < graph.endpointCount(); ++index)
            scratch.m_nodes[index + 2U] = graph.endpoints()[index];
        std::fill_n(scratch.m_cost.begin(), nodeCount,
                    NavigationSearchScratch::InfiniteCost);
        std::fill_n(scratch.m_parent.begin(), nodeCount,
                    NavigationPortalRouteScratch::InvalidNode);
        std::fill_n(scratch.m_parentKind.begin(), nodeCount,
                    NavigationPortalRouteScratch::ParentNone);
        std::fill_n(scratch.m_closed.begin(), nodeCount, uint8_t{0});
        scratch.m_cost[0] = 0;
        scratch.m_currentNode = NavigationPortalRouteScratch::InvalidNode;
        scratch.m_targetNode = NavigationPortalRouteScratch::InvalidNode;
        scratch.m_segmentFromNode = NavigationPortalRouteScratch::InvalidNode;
        scratch.m_segmentToNode = NavigationPortalRouteScratch::InvalidNode;
        scratch.m_edgeIndex = 0;
        scratch.m_segmentActive = false;
        scratch.m_routeCount = 0;
        scratch.m_routeCursor = 0;
        scratch.m_outputCount = 0;
        scratch.m_outputPortalCount = 0;
        scratch.m_segmentPointIndex = 0;
        scratch.m_workConsumedLastStep = 0;
        scratch.m_result = {};
        scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::CoarseSelect;
        return {NavigationPortalRouteStatus::Pending};
    }

    [[nodiscard]] NavigationPortalRouteResult stepRoute(
        const NavigationLayerSet& layers,
        container::Span<const NavigationZoneField> zones,
        const NavigationPortalGraph& graph,
        NavigationPortalRouteScratch& scratch,
        uint32_t workBudget,
        container::Span<NavigationLayerPathPoint> output,
        const NavigationDynamicOverlay* dynamicOverlay = nullptr) const noexcept
    {
        if (scratch.m_routePhase == NavigationPortalRouteScratch::RoutePhase::Complete)
        {
            if (scratch.m_result.requiredPointCount > output.size())
                return {NavigationPortalRouteStatus::OutputCapacityExceeded};
            return scratch.m_result;
        }
        if (scratch.m_routePhase == NavigationPortalRouteScratch::RoutePhase::Failed)
            return {NavigationPortalRouteStatus::InvalidRequest};
        if (workBudget == 0)
            return {NavigationPortalRouteStatus::Pending};
        scratch.m_workConsumedLastStep = 0;
        if (layers.stableHash() != scratch.m_layersHash ||
            graph.stableHash() != scratch.m_graphHash)
        {
            scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::Failed;
            return {NavigationPortalRouteStatus::InvalidRequest};
        }
        uint64_t zonesHash = 14695981039346656037ULL;
        for (const NavigationZoneField& zone : zones)
        {
            zonesHash ^= zone.stableHash();
            zonesHash *= 1099511628211ULL;
        }
        if (zonesHash != scratch.m_zonesHash)
        {
            scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::Failed;
            return {NavigationPortalRouteStatus::InvalidRequest};
        }
        uint32_t remaining = workBudget;
        while (remaining != 0)
        {
            if (scratch.m_routePhase == NavigationPortalRouteScratch::RoutePhase::CoarseSelect)
            {
                const uint32_t selected = selectNode(scratch, scratch.m_nodeCount);
                if (selected == NavigationPortalRouteScratch::InvalidNode)
                {
                    scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::Failed;
                    return noPathResult();
                }
                scratch.m_currentNode = selected;
                scratch.m_closed[selected] = 1;
                --remaining;
                ++scratch.m_workConsumedLastStep;
                if (selected == 1U)
                {
                    if (!buildRouteChain(scratch))
                        return failRoute(scratch, NavigationPortalRouteStatus::CapacityExceeded);
                    scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::Reconstruct;
                    continue;
                }
                scratch.m_targetNode = 0;
                scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::CoarseLayerSegments;
                continue;
            }
            if (scratch.m_routePhase == NavigationPortalRouteScratch::RoutePhase::CoarseLayerSegments)
            {
                while ((scratch.m_segmentActive ||
                        scratch.m_targetNode < scratch.m_nodeCount) &&
                       remaining != 0)
                {
                    uint32_t target = scratch.m_segmentToNode;
                    if (!scratch.m_segmentActive)
                    {
                        target = scratch.m_targetNode++;
                        --remaining;
                        ++scratch.m_workConsumedLastStep;
                    }
                    if (target == scratch.m_currentNode ||
                        scratch.m_closed[target] != 0 ||
                        scratch.m_nodes[target].layer !=
                            scratch.m_nodes[scratch.m_currentNode].layer)
                        continue;
                    if (!scratch.m_segmentActive)
                    {
                        uint32_t ignoredCost = 0;
                        const NavigationGrid* targetGrid =
                            layers.find(scratch.m_nodes[target].layer);
                        if (targetGrid == nullptr ||
                            !zoneAllows(zones, *targetGrid,
                                        scratch.m_request,
                                        scratch.m_nodes[scratch.m_currentNode],
                                        scratch.m_nodes[target]))
                            continue;
                        const NavigationGrid* grid =
                            layers.find(scratch.m_nodes[scratch.m_currentNode].layer);
                        if (grid == nullptr)
                            return failRoute(scratch, NavigationPortalRouteStatus::InvalidRequest);
                        const NavigationSearchRequest request =
                            makeSearchRequest(
                                scratch.m_request,
                                scratch.m_nodes[scratch.m_currentNode],
                                scratch.m_nodes[target]);
                        const NavigationSearchStatus beginStatus =
                            scratch.m_segmentOracle.begin(
                                *grid, scratch.m_search, request,
                                scratch.m_request.objectCells,
                                dynamicOverlay);
                        if (beginStatus == NavigationSearchStatus::CapacityExceeded)
                            return failRoute(scratch,
                                             NavigationPortalRouteStatus::CapacityExceeded);
                        if (beginStatus != NavigationSearchStatus::Pending)
                            continue;
                        scratch.m_segmentFromNode = scratch.m_currentNode;
                        scratch.m_segmentToNode = target;
                        scratch.m_segmentActive = true;
                        static_cast<void>(ignoredCost);
                    }
                    if (scratch.m_segmentActive)
                    {
                        const NavigationGrid* grid =
                            layers.find(scratch.m_nodes[scratch.m_currentNode].layer);
                        if (grid == nullptr)
                            return failRoute(scratch, NavigationPortalRouteStatus::InvalidRequest);
                        const NavigationSearchProgress progress =
                            scratch.m_segmentOracle.step(
                                *grid, scratch.m_search, remaining,
                                scratch.m_request.objectCells,
                                dynamicOverlay);
                        const uint32_t consumed = std::min<uint32_t>(
                            remaining, progress.expandedThisStep);
                        remaining -= consumed;
                        scratch.m_workConsumedLastStep += consumed;
                        if (progress.status == NavigationSearchStatus::Pending)
                            return {NavigationPortalRouteStatus::Pending};
                        if (progress.status == NavigationSearchStatus::Success)
                        {
                            if (!relax(scratch, scratch.m_segmentFromNode,
                                       scratch.m_segmentToNode,
                                       scratch.m_search.gCost(
                                           scratch.m_nodes[scratch.m_segmentToNode].cell),
                                       NavigationPortalRouteScratch::ParentLayer))
                                return failRoute(scratch, NavigationPortalRouteStatus::CapacityExceeded);
                        }
                        scratch.m_segmentActive = false;
                        scratch.m_segmentFromNode = NavigationPortalRouteScratch::InvalidNode;
                        scratch.m_segmentToNode = NavigationPortalRouteScratch::InvalidNode;
                        if (remaining == 0)
                            return {NavigationPortalRouteStatus::Pending};
                    }
                }
                if (scratch.m_targetNode < scratch.m_nodeCount)
                    return {NavigationPortalRouteStatus::Pending};
                scratch.m_edgeIndex = 0;
                scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::CoarsePortalEdges;
                continue;
            }
            if (scratch.m_routePhase == NavigationPortalRouteScratch::RoutePhase::CoarsePortalEdges)
            {
                while (scratch.m_edgeIndex < graph.edges().size() && remaining != 0)
                {
                    const NavigationCoarsePortalEdge& edge =
                        graph.edges()[scratch.m_edgeIndex++];
                    --remaining;
                    ++scratch.m_workConsumedLastStep;
                    const uint32_t from = edge.fromEndpoint + 2U;
                    const uint32_t to = edge.toEndpoint + 2U;
                    if (from == scratch.m_currentNode &&
                        edge.profile == scratch.m_request.profile &&
                        scratch.m_closed[to] == 0 &&
                        !relax(scratch, from, to, edge.traversalCost,
                               NavigationPortalRouteScratch::ParentPortal))
                        return failRoute(scratch, NavigationPortalRouteStatus::CapacityExceeded);
                }
                if (scratch.m_edgeIndex < graph.edges().size())
                    return {NavigationPortalRouteStatus::Pending};
                scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::CoarseSelect;
                continue;
            }
            if (scratch.m_routePhase == NavigationPortalRouteScratch::RoutePhase::Reconstruct)
            {
                if (scratch.m_outputCount == 0)
                {
                    if (!appendPoint(layers, scratch.m_nodes[0],
                                     scratch.m_request.clearance, output,
                                     scratch.m_outputCount))
                        return failRoute(scratch, NavigationPortalRouteStatus::InvalidRequest);
                }
                scratch.m_routeCursor = scratch.m_routeCount - 1U;
                scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::ReconstructSegment;
                continue;
            }
            if (scratch.m_routePhase == NavigationPortalRouteScratch::RoutePhase::ReconstructSegment)
            {
                if (scratch.m_routeCursor == 0)
                {
                    scratch.m_result.status = NavigationPortalRouteStatus::Success;
                    scratch.m_result.requiredPointCount = scratch.m_outputCount;
                    scratch.m_result.portalCount = scratch.m_outputPortalCount;
                    scratch.m_result.totalCost = scratch.m_cost[1];
                    scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::Complete;
                    return scratch.m_result;
                }
                const uint32_t from = scratch.m_routeNodes[scratch.m_routeCursor];
                const uint32_t to = scratch.m_routeNodes[scratch.m_routeCursor - 1U];
                if (scratch.m_parentKind[to] == NavigationPortalRouteScratch::ParentPortal)
                {
                    if (!appendPoint(layers, scratch.m_nodes[to],
                                     scratch.m_request.clearance, output,
                                     scratch.m_outputCount))
                        return failRoute(scratch, NavigationPortalRouteStatus::InvalidRequest);
                    ++scratch.m_outputPortalCount;
                    --scratch.m_routeCursor;
                    --remaining;
                    ++scratch.m_workConsumedLastStep;
                    continue;
                }
                if (!scratch.m_segmentActive)
                {
                    const NavigationGrid* grid = layers.find(scratch.m_nodes[from].layer);
                    if (grid == nullptr)
                        return failRoute(scratch, NavigationPortalRouteStatus::InvalidRequest);
                    const NavigationSearchRequest request =
                        makeSearchRequest(
                            scratch.m_request, scratch.m_nodes[from],
                            scratch.m_nodes[to]);
                    const NavigationSearchStatus beginStatus =
                        scratch.m_segmentOracle.begin(
                            *grid, scratch.m_search, request,
                            scratch.m_request.objectCells,
                            dynamicOverlay);
                    if (beginStatus == NavigationSearchStatus::CapacityExceeded)
                        return failRoute(scratch,
                                         NavigationPortalRouteStatus::CapacityExceeded);
                    if (beginStatus != NavigationSearchStatus::Pending)
                        return failRoute(scratch, NavigationPortalRouteStatus::NoPath);
                    scratch.m_segmentActive = true;
                    scratch.m_segmentFromNode = from;
                    scratch.m_segmentToNode = to;
                    scratch.m_segmentPointIndex = 0;
                }
                const NavigationGrid* grid = layers.find(scratch.m_nodes[from].layer);
                if (grid == nullptr)
                    return failRoute(scratch,
                                     NavigationPortalRouteStatus::InvalidRequest);
                if (scratch.m_segmentOracle.status() == NavigationSearchStatus::Pending)
                {
                    const NavigationSearchProgress progress =
                        scratch.m_segmentOracle.step(
                            *grid, scratch.m_search, remaining,
                            scratch.m_request.objectCells,
                            dynamicOverlay);
                    const uint32_t consumed = std::min<uint32_t>(
                        remaining, progress.expandedThisStep);
                    remaining -= consumed;
                    scratch.m_workConsumedLastStep += consumed;
                    if (progress.status == NavigationSearchStatus::Pending)
                        return {NavigationPortalRouteStatus::Pending};
                    if (progress.status != NavigationSearchStatus::Success)
                        return failRoute(scratch, NavigationPortalRouteStatus::NoPath);
                }
                const NavigationPathReadResult path =
                    scratch.m_segmentOracle.readPath(scratch.m_search,
                                                     scratch.m_rawCells);
                if (path.status == NavigationPathReadStatus::OutputCapacityExceeded)
                    return failRoute(scratch,
                                     NavigationPortalRouteStatus::CapacityExceeded);
                if (path.status != NavigationPathReadStatus::Success)
                    return failRoute(scratch, NavigationPortalRouteStatus::NoPath);
                if (scratch.m_segmentPointIndex == 0)
                    scratch.m_segmentPointIndex = 1;
                while (scratch.m_segmentPointIndex < path.requiredCount)
                {
                    if (remaining == 0)
                        return {NavigationPortalRouteStatus::Pending};
                    const size_t index = scratch.m_segmentPointIndex;
                    NavigationWorldPosition position;
                    if (!grid->cellPosition(scratch.m_rawCells[index],
                                             scratch.m_request.clearance,
                                             position))
                        return failRoute(scratch,
                                         NavigationPortalRouteStatus::InvalidRequest);
                    const NavigationLayerPathPoint point = {
                        {scratch.m_nodes[from].layer, scratch.m_rawCells[index]},
                        position};
                    if (scratch.m_outputCount < output.size())
                        output[scratch.m_outputCount] = point;
                    ++scratch.m_outputCount;
                    ++scratch.m_segmentPointIndex;
                    --remaining;
                    ++scratch.m_workConsumedLastStep;
                }
                scratch.m_segmentActive = false;
                scratch.m_segmentFromNode = NavigationPortalRouteScratch::InvalidNode;
                scratch.m_segmentToNode = NavigationPortalRouteScratch::InvalidNode;
                scratch.m_segmentPointIndex = 0;
                --scratch.m_routeCursor;
                continue;
            }
        }
        return {NavigationPortalRouteStatus::Pending};
    }
    // Coarse Dijkstra selects by (totalCost, canonicalNodeIndex). Relaxation
    // keeps the first predecessor on equal cost, making the complete route
    // independent of portal insertion order.
    [[nodiscard]] NavigationPortalRouteResult route(
        const NavigationLayerSet& layers,
        container::Span<const NavigationZoneField> zones,
        const NavigationPortalGraph& graph,
        NavigationPortalRouteScratch& scratch,
        const NavigationPortalRouteRequest& request,
        container::Span<NavigationLayerPathPoint> output,
        const NavigationDynamicOverlay* dynamicOverlay = nullptr) const noexcept
    {
        NavigationPortalRouteResult result;
        const NavigationGrid* startGrid = layers.find(request.start.layer);
        const NavigationGrid* goalGrid = layers.find(request.goal.layer);
        if (request.requestId == 0 || !request.profile || request.movementMask == 0 || !request.start ||
            !request.goal || !validClearanceClass(request.clearance) ||
            startGrid == nullptr || goalGrid == nullptr ||
            !AStarOracle::allowsTraversalCell(
                *startGrid,
                makeSearchRequest(request, request.start, request.goal),
                request.start.cell, dynamicOverlay, true) ||
            !AStarOracle::allowsTraversalCell(
                *goalGrid,
                makeSearchRequest(request, request.goal, request.start),
                request.goal.cell, dynamicOverlay, false))
            return result;

        const size_t nodeCount = 2U + graph.endpointCount();
        if (graph.endpointCount() / 2U > scratch.m_portalCapacity || nodeCount > scratch.m_nodes.size())
            return capacityResult();
        for (NavigationLayerCell endpoint : graph.endpoints())
        {
            const NavigationGrid* grid = layers.find(endpoint.layer);
            if (grid == nullptr || !grid->contains(endpoint.cell))
                return invalidResult();
            if (grid->cellCount() > scratch.m_search.cellCapacity())
                return capacityResult();
        }
        if (startGrid->cellCount() > scratch.m_search.cellCapacity() ||
            goalGrid->cellCount() > scratch.m_search.cellCapacity())
            return capacityResult();

        scratch.m_nodes[0] = request.start;
        scratch.m_nodes[1] = request.goal;
        for (size_t index = 0; index < graph.endpointCount(); ++index)
            scratch.m_nodes[index + 2U] = graph.endpoints()[index];
        std::fill_n(scratch.m_cost.begin(), nodeCount, NavigationSearchScratch::InfiniteCost);
        std::fill_n(scratch.m_parent.begin(), nodeCount, NavigationPortalRouteScratch::InvalidNode);
        std::fill_n(scratch.m_parentKind.begin(), nodeCount, NavigationPortalRouteScratch::ParentNone);
        std::fill_n(scratch.m_closed.begin(), nodeCount, uint8_t{0});
        scratch.m_cost[0] = 0;

        while (true)
        {
            const uint32_t current = selectNode(scratch, nodeCount);
            if (current == NavigationPortalRouteScratch::InvalidNode)
                return noPathResult();
            if (current == 1U)
                break;
            scratch.m_closed[current] = 1;

            for (uint32_t target = 0; target < nodeCount; ++target)
            {
                if (target == current || scratch.m_closed[target] != 0 ||
                    scratch.m_nodes[target].layer != scratch.m_nodes[current].layer)
                    continue;
                uint32_t segmentCost = 0;
                const SegmentStatus segment = exactSegmentCost(
                    layers, zones, scratch, request,
                    scratch.m_nodes[current], scratch.m_nodes[target],
                    dynamicOverlay, segmentCost);
                if (segment == SegmentStatus::CapacityExceeded)
                    return capacityResult();
                if (segment == SegmentStatus::Success &&
                    !relax(scratch, current, target, segmentCost, NavigationPortalRouteScratch::ParentLayer))
                    return capacityResult();
            }

            for (const NavigationCoarsePortalEdge& edge : graph.edges())
            {
                const uint32_t from = edge.fromEndpoint + 2U;
                const uint32_t to = edge.toEndpoint + 2U;
                if (from == current && edge.profile == request.profile && scratch.m_closed[to] == 0)
                {
                    if (!relax(scratch,
                               current,
                               to,
                               edge.traversalCost,
                               NavigationPortalRouteScratch::ParentPortal))
                        return capacityResult();
                }
            }
        }

        size_t routeCount = 0;
        uint32_t cursor = 1;
        while (true)
        {
            if (routeCount == nodeCount)
                return capacityResult();
            scratch.m_routeNodes[routeCount++] = cursor;
            if (cursor == 0)
                break;
            cursor = scratch.m_parent[cursor];
            if (cursor == NavigationPortalRouteScratch::InvalidNode)
                return noPathResult();
        }

        result.status = NavigationPortalRouteStatus::Success;
        result.totalCost = scratch.m_cost[1];
        if (!appendPoint(layers, scratch.m_nodes[0], request.clearance,
                         output, result.requiredPointCount))
            return invalidResult();

        for (size_t reverseIndex = routeCount - 1U; reverseIndex > 0; --reverseIndex)
        {
            const uint32_t from = scratch.m_routeNodes[reverseIndex];
            const uint32_t to = scratch.m_routeNodes[reverseIndex - 1U];
            if (scratch.m_parentKind[to] == NavigationPortalRouteScratch::ParentPortal)
            {
                if (!appendPoint(layers, scratch.m_nodes[to],
                                 request.clearance, output,
                                 result.requiredPointCount))
                    return invalidResult();
                ++result.portalCount;
                continue;
            }

            size_t segmentCount = 0;
            uint32_t ignoredCost = 0;
            const SegmentStatus segment = readExactSegment(layers,
                                                           scratch,
                                                           request,
                                                           scratch.m_nodes[from],
                                                           scratch.m_nodes[to],
                                                           dynamicOverlay,
                                                           segmentCount,
                                                           ignoredCost);
            if (segment == SegmentStatus::CapacityExceeded)
                return capacityResult();
            if (segment != SegmentStatus::Success)
                return noPathResult();
            for (size_t cellIndex = 1; cellIndex < segmentCount; ++cellIndex)
            {
                const NavigationLayerCell point{scratch.m_nodes[from].layer, scratch.m_rawCells[cellIndex]};
                if (!appendPoint(layers, point, request.clearance, output,
                                 result.requiredPointCount))
                    return invalidResult();
            }
        }

        if (result.requiredPointCount > output.size())
            result.status = NavigationPortalRouteStatus::OutputCapacityExceeded;
        return result;
    }

private:
    [[nodiscard]] static NavigationSearchRequest makeSearchRequest(
        const NavigationPortalRouteRequest& request,
        NavigationLayerCell from,
        NavigationLayerCell to) noexcept
    {
        NavigationSearchRequest search;
        search.requestId = request.requestId;
        search.start = from.cell;
        search.goal = to.cell;
        search.profile = request.profile;
        search.movementMask = request.movementMask;
        search.layer = from.layer;
        search.clearance = request.clearance;
        search.ignoredObstacle = request.ignoredObstacle;
        search.crusherLevel = request.crusherLevel;
        search.dozerPassableObstacles =
            request.dozerPassableObstacles;
        return search;
    }

    [[nodiscard]] static NavigationPortalRouteResult failRoute(
        NavigationPortalRouteScratch& scratch,
        NavigationPortalRouteStatus status) noexcept
    {
        scratch.m_routePhase = NavigationPortalRouteScratch::RoutePhase::Failed;
        scratch.m_result = {status};
        return scratch.m_result;
    }

    [[nodiscard]] static bool buildRouteChain(
        NavigationPortalRouteScratch& scratch) noexcept
    {
        uint32_t cursor = 1U;
        scratch.m_routeCount = 0;
        while (cursor != NavigationPortalRouteScratch::InvalidNode &&
               scratch.m_routeCount < scratch.m_nodeCount)
        {
            scratch.m_routeNodes[scratch.m_routeCount++] = cursor;
            if (cursor == 0U)
                return true;
            cursor = scratch.m_parent[cursor];
        }
        return false;
    }

    enum class SegmentStatus : uint8_t
    {
        Success = 0,
        NoPath,
        CapacityExceeded,
    };

    [[nodiscard]] static NavigationPortalRouteResult invalidResult() noexcept { return {}; }

    [[nodiscard]] static NavigationPortalRouteResult noPathResult() noexcept
    {
        NavigationPortalRouteResult result;
        result.status = NavigationPortalRouteStatus::NoPath;
        return result;
    }

    [[nodiscard]] static NavigationPortalRouteResult capacityResult() noexcept
    {
        NavigationPortalRouteResult result;
        result.status = NavigationPortalRouteStatus::CapacityExceeded;
        return result;
    }

    [[nodiscard]] static uint32_t selectNode(const NavigationPortalRouteScratch& scratch,
                                             size_t nodeCount) noexcept
    {
        uint32_t selected = NavigationPortalRouteScratch::InvalidNode;
        uint32_t selectedCost = NavigationSearchScratch::InfiniteCost;
        for (uint32_t node = 0; node < nodeCount; ++node)
        {
            if (scratch.m_closed[node] == 0 &&
                (scratch.m_cost[node] < selectedCost ||
                 (scratch.m_cost[node] == selectedCost && scratch.m_cost[node] != NavigationSearchScratch::InfiniteCost &&
                  node < selected)))
            {
                selected = node;
                selectedCost = scratch.m_cost[node];
            }
        }
        return selected;
    }

    [[nodiscard]] static bool relax(NavigationPortalRouteScratch& scratch,
                                    uint32_t from,
                                    uint32_t to,
                                    uint32_t edgeCost,
                                    uint8_t parentKind) noexcept
    {
        if (edgeCost > std::numeric_limits<uint32_t>::max() - scratch.m_cost[from])
            return false;
        const uint32_t candidate = scratch.m_cost[from] + edgeCost;
        if (candidate == NavigationSearchScratch::InfiniteCost)
            return false;
        if (candidate >= scratch.m_cost[to])
            return true;
        scratch.m_cost[to] = candidate;
        scratch.m_parent[to] = from;
        scratch.m_parentKind[to] = parentKind;
        return true;
    }

    [[nodiscard]] static bool zoneAllows(container::Span<const NavigationZoneField> zones,
                                         const NavigationGrid& grid,
                                         const NavigationPortalRouteRequest& request,
                                         NavigationLayerCell from,
                                         NavigationLayerCell to) noexcept
    {
        for (const NavigationZoneField& zone : zones)
        {
                if (zone.isBuilt() && zone.profile() == request.profile &&
                    zone.movementMask() == request.movementMask &&
                    zone.clearanceClass() == request.clearance &&
                zone.layer() == from.layer && zone.width() == grid.width() && zone.height() == grid.height() &&
                zone.cellCount() == grid.cellCount())
                return zone.sameZone(from.cell, to.cell);
        }
        return false;
    }

    [[nodiscard]] static SegmentStatus runAStar(const NavigationGrid& grid,
                                                NavigationPortalRouteScratch& scratch,
                                                const NavigationPortalRouteRequest& request,
                                                NavigationLayerCell from,
                                                NavigationLayerCell to,
                                                const NavigationDynamicOverlay* dynamicOverlay,
                                                bool readPath,
                                                size_t& pointCount,
                                                uint32_t& totalCost) noexcept
    {
        AStarOracle oracle;
        const NavigationSearchRequest searchRequest =
            makeSearchRequest(request, from, to);
        NavigationSearchStatus status = oracle.begin(
            grid, scratch.m_search, searchRequest, request.objectCells,
            dynamicOverlay);
        while (status == NavigationSearchStatus::Pending)
            status = oracle.step(
                grid, scratch.m_search,
                static_cast<uint32_t>(grid.cellCount()),
                request.objectCells, dynamicOverlay).status;
        if (status == NavigationSearchStatus::CapacityExceeded)
            return SegmentStatus::CapacityExceeded;
        if (status != NavigationSearchStatus::Success)
            return SegmentStatus::NoPath;
        totalCost = scratch.m_search.gCost(to.cell);
        pointCount = 0;
        if (!readPath)
            return SegmentStatus::Success;
        const NavigationPathReadResult path = oracle.readPath(scratch.m_search, scratch.m_rawCells);
        if (path.status == NavigationPathReadStatus::OutputCapacityExceeded)
            return SegmentStatus::CapacityExceeded;
        if (path.status != NavigationPathReadStatus::Success)
            return SegmentStatus::NoPath;
        pointCount = path.requiredCount;
        totalCost = path.totalCost;
        return SegmentStatus::Success;
    }

    [[nodiscard]] static SegmentStatus exactSegmentCost(const NavigationLayerSet& layers,
                                                        container::Span<const NavigationZoneField> zones,
                                                        NavigationPortalRouteScratch& scratch,
                                                        const NavigationPortalRouteRequest& request,
                                                        NavigationLayerCell from,
                                                        NavigationLayerCell to,
                                                        const NavigationDynamicOverlay* dynamicOverlay,
                                                        uint32_t& totalCost) noexcept
    {
        const NavigationGrid* grid = layers.find(from.layer);
        const NavigationSearchRequest searchRequest =
            makeSearchRequest(request, from, to);
        if (grid == nullptr || from.layer != to.layer ||
            !AStarOracle::allowsTraversalCell(
                *grid, searchRequest, from.cell, dynamicOverlay, true) ||
            !AStarOracle::allowsTraversalCell(
                *grid, searchRequest, to.cell, dynamicOverlay, false) ||
            !zoneAllows(zones, *grid, request, from, to))
            return SegmentStatus::NoPath;
        size_t ignoredCount = 0;
        return runAStar(
            *grid, scratch, request, from, to, dynamicOverlay, false,
            ignoredCount, totalCost);
    }

    [[nodiscard]] static SegmentStatus readExactSegment(const NavigationLayerSet& layers,
                                                        NavigationPortalRouteScratch& scratch,
                                                        const NavigationPortalRouteRequest& request,
                                                        NavigationLayerCell from,
                                                        NavigationLayerCell to,
                                                        const NavigationDynamicOverlay* dynamicOverlay,
                                                        size_t& pointCount,
                                                        uint32_t& totalCost) noexcept
    {
        const NavigationGrid* grid = layers.find(from.layer);
        if (grid == nullptr || from.layer != to.layer)
            return SegmentStatus::NoPath;
        return runAStar(
            *grid, scratch, request, from, to, dynamicOverlay, true,
            pointCount, totalCost);
    }

    [[nodiscard]] static bool appendPoint(const NavigationLayerSet& layers,
                                          NavigationLayerCell location,
                                          NavigationClearanceClass clearance,
                                          container::Span<NavigationLayerPathPoint> output,
                                          size_t& count) noexcept
    {
        const NavigationGrid* grid = layers.find(location.layer);
        NavigationWorldPosition position;
        if (grid == nullptr ||
            !grid->cellPosition(location.cell, clearance, position))
            return false;
        if (count < output.size())
            output[count] = {location, position};
        ++count;
        return true;
    }
};

} // namespace engine::navigation
