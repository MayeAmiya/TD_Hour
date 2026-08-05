#pragma once

#include "core/container/hash_containers.h"

#include "game/base/MapSourceBlob.h"
#include "game/terrain/TerrainVertexWaterState.h"
#include "math/fixed/q32_32.h"
#include "TerrainMap.h"
#include <cstdint>
#include <optional>
namespace game::terrain {

struct WaypointRecord {
    uint32_t id = UINT32_MAX;
    container::String name;
    math::vec3 position{};
    // Canonical simulation position, quantized once after authored Z is
    // projected onto terrain. Script/AI/navigation consumers use these raw
    // values; the vec3 above remains the presentation and legacy query view.
    container::Array<int64_t, 3> positionRaw{};
    bool rawAuthoritative = false;
    container::Array<container::String, 3> pathLabels;
    bool biDirectional = false;
    container::Vector<uint32_t> links;
};

// Player_N_Start waypoint data expressed as a portable, immutable layout.
// `index` is zero based at the game boundary even though the authored map
// names are one based.  A roster resolver consumes this value before any ECS
// object exists, so spawn selection is deterministic and renderer-free.
struct MultiplayerStartPosition final {
    int32_t index = -1;
    math::vec3 position{};
    container::Array<int64_t, 3> positionRaw{};
};

// Runtime water is intentionally separate from immutable PolygonTriggers.
// A flood changes only this value state and is emitted through the same
// renderer snapshot boundary as height deformation; no D3D12 resource or
// game object pointer is stored here.
struct TerrainWaterArea {
    uint32_t triggerId = 0;
    container::String name;
    container::Vector<math::int3> polygon;
    // PolygonTriggers serializes these independently from the generic water
    // bit. Rivers are ordered cross-section strips: riverStart identifies the
    // first inner-bank point and every authored point retains its own Z.
    bool river = false;
    int32_t riverStart = 0;
    bool synthesizedLegacyWater = false;
    // Authoritative Q32.32 state. Float values are created only by explicit
    // legacy-query or render extraction boundaries.
    int64_t surfaceHeightRaw = 0;
    int64_t targetHeightRaw = 0;
    int64_t changePerSecondRaw = 0;
    // This is an authored damage amount per legacy water-damage pulse, not
    // damage-per-second.  RefCode only applies it once per logic second while
    // a table moves (and once on the final transition), so naming it as a DPS
    // rate would make a modern continuous hazard silently too lethal.
    int64_t damageAmountRaw = 0;

    [[nodiscard]] bool isTransitioning() const noexcept {
        return changePerSecondRaw > 0 && surfaceHeightRaw != targetHeightRaw;
    }
};

// Terrain owns no ECS/Object pointers.  A rising water table emits this
// value-only fact; GameSession expands it to ObjectDamageRequest values at
// the authoritative simulation boundary.  `finalTransition` distinguishes
// the forced final pulse from the once-per-second intermediate pulses for
// diagnostics/probes without exposing a client-side water implementation.
struct TerrainWaterDamagePulse final {
    uint32_t triggerId = 0;
    int64_t previousHeightRaw = 0;
    int64_t currentHeightRaw = 0;
    int64_t damageAmountRaw = 0;
    bool finalTransition = false;
};

// Compatibility projection of PolygonTrigger::updateBounds/getCenterPoint.
// Keep the historical radius calculation in one terrain-owned adapter so
// script conditions and AI orders cannot silently diverge from shipped maps.
struct PolygonTriggerLegacyBounds final {
    math::q32_32 centerX{};
    math::q32_32 centerY{};
    math::q32_32 radius{};
};

using TerrainPathfindLayerId = uint32_t;
inline constexpr TerrainPathfindLayerId kGroundPathfindLayer = 0;

// Logic-owned elevated walkable surface. Terrain Bridge construction supplies
// these polygons once it has paired the two authored bridge endpoints. Keeping
// this value contract in TerrainLogic lets projectile/locomotor gameplay use
// the same layer without retaining bridge renderer or map-object pointers.
struct TerrainElevatedPathfindSurface final {
    TerrainPathfindLayerId layer = kGroundPathfindLayer;
    uint64_t sourceRecordIndex = UINT64_MAX;
    container::Vector<math::vec3> boundary;
    // Stable bridge centerline used by WaveGuide and future portal builders;
    // unlike a raw map index+1 this is valid for both point-pair and Landmark
    // BOX bridges.
    math::vec3 from{};
    math::vec3 to{};
    float height = 0.0f;
    // TerrainRoads.ini TransitionEffectsHeight is gameplay input for the
    // BridgeDieFX/OCL sampling prism, not a renderer-only preference.
    float transitionEffectsHeight = 0.0f;
    // Quantized once by setElevatedPathfindSurfaces(). Simulation-side bridge
    // queries consume only these Q32.32 mirrors.
    container::Vector<container::Array<int64_t, 3>> boundaryRaw;
    container::Array<int64_t, 3> fromRaw{};
    container::Array<int64_t, 3> toRaw{};
    int64_t heightRaw = 0;
    int64_t transitionEffectsHeightRaw = 0;
    // Bootstrap/loaders that already quantized map/config ingress set this so
    // TerrainLogic preserves the raw geometry instead of rebuilding it from
    // the float presentation projection.
    bool rawAuthoritative = false;
    // Ordinary BridgeBehavior rubble is reversible.  Keep the authored
    // surface record for repair/render provenance while excluding it from
    // gameplay layer queries. WaveGuide permanent deletion still erases it.
    bool active = true;
};

class TerrainLogic final {
public:
    bool loadMap(container::StringView path, container::String* error = nullptr);
    bool loadMap(const game::MapSourceHandle& source,
                 container::String* error = nullptr);
    // Session startup has already applied Map.ini/solo.ini GameData layers.
    // Pass their frozen water extents explicitly so legacy PolygonTriggers v1
    // synthesis never reaches back into the process-global base GameData.
    bool loadMap(const game::MapSourceHandle& source,
                 float legacyWaterExtentX, float legacyWaterExtentY,
                 container::String* error = nullptr);
    [[nodiscard]] const game::MapSourceHandle& startupMapSource() const noexcept {
        return m_startupMapSource;
    }
    void releaseStartupMapSource() noexcept { m_startupMapSource.reset(); }
    void clear() noexcept;
    [[nodiscard]] bool isLoaded() const noexcept { return m_map.isLoaded(); }
    [[nodiscard]] const TerrainMap& map() const noexcept { return m_map; }
    [[nodiscard]] const MapContentIdentity& contentIdentity() const noexcept {
        return m_contentIdentity;
    }
    [[nodiscard]] container::Span<const MultiplayerStartPosition> multiplayerStartPositions() const noexcept {
        return m_multiplayerStartPositions;
    }
    [[nodiscard]] float groundHeight(float x, float y) const noexcept { return m_map.groundHeight(x, y); }
    [[nodiscard]] int64_t groundHeightRaw(int64_t xRaw, int64_t yRaw) const noexcept {
        return m_map.groundHeightRaw(xRaw, yRaw);
    }
    [[nodiscard]] bool isCliffCell(float x, float y) const noexcept { return m_map.isCliffCell(x, y); }
    [[nodiscard]] bool isCliffCellRaw(int64_t xRaw, int64_t yRaw) const noexcept {
        return m_map.isCliffCellRaw(xRaw, yRaw);
    }
    // MAP_SWITCH_BORDER selects one authored logical terrain boundary. This
    // is logic-owned map state, not a renderer clip rectangle.
    bool setActiveBoundary(size_t index) noexcept { return m_map.setActiveBoundary(index); }
    [[nodiscard]] uint64_t waterRevision() const noexcept { return m_waterRevision; }
    // Navigation listens only to authoritative standing-water surface
    // changes. Vertex-water waves and other presentation revisions must not
    // wake the pathfind raster synchronizer.
    [[nodiscard]] uint64_t pathfindWaterRevision() const noexcept {
        return m_pathfindWaterRevision;
    }
    [[nodiscard]] const container::Vector<TerrainWaterArea>& waterAreas() const noexcept { return m_waterAreas; }

    bool setHeightSample(int32_t x, int32_t y, uint8_t sample) noexcept;
    // Deterministic deformation for simulation callers: signed Q32.32 raw
    // centre, radius and height delta. The float overload below only converts
    // and forwards, and exists for tools and diagnostics.
    bool deformTerrainCircleRaw(int64_t worldXRaw, int64_t worldYRaw,
                                int64_t radiusWorldRaw,
                                int64_t heightDeltaWorldRaw) noexcept;
    bool deformTerrainCircle(float worldX, float worldY, float radiusWorld,
                             float heightDeltaWorld) noexcept;
    [[nodiscard]] TerrainFlattenResult flattenFootprintRaw(
        const TerrainFlattenFootprint& footprint) noexcept;
    void beginHeightMutationBatch() noexcept {
        m_map.beginHeightMutationBatch();
    }
    void endHeightMutationBatch() noexcept {
        m_map.endHeightMutationBatch();
    }
    // Script/scenario water floats reach these setters unvalidated, and
    // isfinite bounds nothing: a saturated Fixed height makes every ground
    // sample compare as underwater and the next navigation water raster flips
    // the covered area's movement mask. Every overload below rejects an
    // out-of-range value rather than clamping it. Callers that own a
    // diagnostic channel should test admissibility first so a malformed map
    // reports instead of being silently dropped -- the bool return also means
    // "no change", so it is not a usable error signal on its own.
    [[nodiscard]] static bool admissibleWaterValueFixed(
        math::q32_32 value) noexcept;
    // A direct script water change supplies the legacy near-lethal amount;
    // non-script callers may omit it when they only need visual/topology
    // mutation. Damage is emitted only when the water table rises.
    bool setWaterHeight(uint32_t triggerId, float height,
                        float damageAmount = 0.0f) noexcept;
    bool setWaterHeightFixed(
        uint32_t triggerId, math::q32_32 height,
        math::q32_32 damageAmount = {}) noexcept;
    bool beginFlood(uint32_t triggerId, float targetHeight, float changePerSecond,
                    float damageAmount = 0.0f) noexcept;
    bool beginFloodFixed(
        uint32_t triggerId, math::q32_32 targetHeight,
        math::q32_32 changePerSecond,
        math::q32_32 damageAmount = {}) noexcept;
    // Standalone tools may use the simple overload. Confirmed GameSession
    // updates pass their tick/rate so intermediate water damage follows the
    // original once-per-logic-second cadence.
    void update(float deltaSeconds) noexcept;
    void update(float deltaSeconds, uint64_t confirmedTick,
                uint32_t logicFramesPerSecond) noexcept;
    // Authoritative session path: the fixed step is derived directly from
    // the frozen logic rate, so a render-frame float delta never enters water
    // simulation.
    void updateAtLogicRate(uint64_t confirmedTick,
                           uint32_t logicFramesPerSecond) noexcept;
    [[nodiscard]] container::Vector<TerrainWaterDamagePulse> takeWaterDamagePulses();
    void addWaveWaterMotion(float x, float y, float velocity,
                            float preferredHeight,
                            uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool configureVertexWater(
        const engine::TerrainVertexWaterGridConfig& config) {
        const bool configured = m_vertexWaterState.configure(config);
        if (configured) markWaterMutation();
        return configured;
    }
    [[nodiscard]] bool advanceVertexWater(float gravityPerUpdate) noexcept {
        if (!m_vertexWaterState.configured()) return false;
        const bool wasMoving = m_vertexWaterState.inMotion();
        const bool advanced = m_vertexWaterState.advance(gravityPerUpdate);
        if (advanced && (wasMoving || m_vertexWaterState.inMotion())) {
            markWaterMutation();
        }
        return advanced;
    }
    [[nodiscard]] const engine::TerrainVertexWaterState& vertexWaterState()
        const noexcept { return m_vertexWaterState; }
    [[nodiscard]] bool restoreVertexWaterState(
        const engine::TerrainVertexWaterGridConfig& config,
        container::Vector<engine::TerrainVertexWaterPoint> points) {
        if (!m_vertexWaterState.restore(config, std::move(points))) {
            return false;
        }
        markWaterMutation();
        return true;
    }
    [[nodiscard]] std::optional<float> waterHeightAt(float x, float y) const noexcept;
    [[nodiscard]] std::optional<int64_t> waterHeightRawAt(
        int64_t xRaw, int64_t yRaw) const noexcept;
    // RefCode's water-handle lookup rounds only the polygon membership
    // coordinate (`floor(x + .5)`). Consumers such as FloatUpdate need the
    // resolved surface even when terrain is already above it, so this stays
    // distinct from the boolean underwater predicate below.
    [[nodiscard]] std::optional<float> waterSurfaceHeightLegacyAt(float x, float y) const noexcept;
    [[nodiscard]] std::optional<int64_t> waterSurfaceHeightLegacyRawAt(
        int64_t xRaw, int64_t yRaw) const noexcept;
    // RefCode rounds only the water-trigger lookup coordinate (`floor(x+.5)`)
    // while it samples terrain height at the original Object position.  Water
    // damage uses this compatibility predicate; generic terrain callers keep
    // the continuous `waterHeightAt` query above.
    [[nodiscard]] bool isUnderwaterLegacy(float x, float y) const noexcept;
    [[nodiscard]] bool isUnderwaterLegacyRaw(
        int64_t xRaw, int64_t yRaw) const noexcept;
    // Pathfinder classifies a grid cell from four corners. Resolve each
    // Water area/trigger once for that batch instead of performing four full
    // area scans; membership still uses ZH's rounded point while terrain is
    // sampled at the original coordinate.
    [[nodiscard]] bool cellTouchesUnderwaterLegacy(
        float left, float top, float right, float bottom) const noexcept;
    [[nodiscard]] bool cellTouchesUnderwaterLegacyRaw(
        int64_t leftRaw, int64_t topRaw,
        int64_t rightRaw, int64_t bottomRaw) const noexcept;
    bool addWaypoint(WaypointRecord waypoint);
    bool addWaypointLink(uint32_t from, uint32_t to);
    [[nodiscard]] const WaypointRecord* waypointById(uint32_t id) const noexcept;
    [[nodiscard]] const WaypointRecord* waypointByName(container::StringView name) const noexcept;
    [[nodiscard]] const WaypointRecord* closestWaypointOnPath(
        float x, float y, container::StringView pathLabel) const noexcept;
    [[nodiscard]] const WaypointRecord* closestWaypointOnPathRaw(
        int64_t xRaw, int64_t yRaw,
        container::StringView pathLabel) const noexcept;
    [[nodiscard]] uint64_t waypointGraphRevision() const noexcept {
        return m_waypointGraphRevision;
    }
    [[nodiscard]] const WaypointRecord* nearestWaypoint(
        float x, float y, float z) const noexcept;
    [[nodiscard]] const WaypointRecord* nearestWaypointRaw(
        int64_t xRaw, int64_t yRaw, int64_t zRaw) const noexcept;
    [[nodiscard]] const PolygonTriggerRecord* triggerById(uint32_t id) const noexcept;
    [[nodiscard]] const PolygonTriggerRecord* triggerByName(container::StringView name) const noexcept;
    [[nodiscard]] static std::optional<PolygonTriggerLegacyBounds>
    legacyTriggerBounds(const PolygonTriggerRecord& trigger) noexcept;
    [[nodiscard]] static uint64_t triggerRevision(
        const PolygonTriggerRecord& trigger) noexcept;
    [[nodiscard]] bool isInsideTrigger(const PolygonTriggerRecord& trigger, float x, float y) const noexcept;
    [[nodiscard]] bool isInsideTriggerRaw(
        const PolygonTriggerRecord& trigger, int64_t xRaw,
        int64_t yRaw) const noexcept;
    [[nodiscard]] bool isInsideTriggerLegacy(
        const PolygonTriggerRecord& trigger, float x, float y) const noexcept;
    [[nodiscard]] bool isInsideTriggerLegacyRaw(
        const PolygonTriggerRecord& trigger, int64_t xRaw,
        int64_t yRaw) const noexcept;
    [[nodiscard]] container::Span<const PolygonTriggerRecord> triggers() const noexcept {
        return m_triggers;
    }
    [[nodiscard]] bool isInsideWaterArea(float x, float y) const noexcept;
    // Replaces the complete elevated-layer set transactionally. Layer zero is
    // reserved for the heightfield. Several sections may share one layer ID;
    // their vertex Z values provide the sloped per-point bridge height.
    bool setElevatedPathfindSurfaces(
        container::Vector<TerrainElevatedPathfindSurface> surfaces) noexcept;
    [[nodiscard]] container::Span<const TerrainElevatedPathfindSurface>
    elevatedPathfindSurfaces() const noexcept {
        return m_elevatedPathfindSurfaces;
    }
    [[nodiscard]] TerrainPathfindLayerId highestPathfindLayerAt(
        float x, float y, float z) const noexcept;
    [[nodiscard]] TerrainPathfindLayerId highestPathfindLayerAtRaw(
        int64_t xRaw, int64_t yRaw, int64_t zRaw) const noexcept;
    // Object::setPosition followed by getLayerForDestination selects the
    // walkable surface closest to the authored Z, not necessarily the
    // highest surface below it.  Map/script/production spawn uses this
    // query to materialize the object's durable pathfind-layer value.
    [[nodiscard]] TerrainPathfindLayerId pathfindLayerForDestination(
        float x, float y, float z) const noexcept;
    [[nodiscard]] TerrainPathfindLayerId pathfindLayerForDestinationRaw(
        int64_t xRaw, int64_t yRaw, int64_t zRaw) const noexcept;
    [[nodiscard]] TerrainPathfindLayerId highestPathfindLayerAtXY(
        float x, float y) const noexcept;
    [[nodiscard]] TerrainPathfindLayerId highestPathfindLayerAtXYRaw(
        int64_t xRaw, int64_t yRaw) const noexcept;
    [[nodiscard]] std::optional<float> pathfindLayerHeightAt(
        TerrainPathfindLayerId layer, float x, float y) const noexcept;
    [[nodiscard]] std::optional<int64_t> pathfindLayerHeightRawAt(
        TerrainPathfindLayerId layer, int64_t xRaw,
        int64_t yRaw) const noexcept;
    bool setBridgeActiveBySourceRecordIndex(
        uint64_t sourceRecordIndex, bool active) noexcept;
    bool destroyBridgeBySourceRecordIndex(uint64_t sourceRecordIndex) noexcept;
    [[nodiscard]] bool isBridgeDestroyed(
        uint64_t sourceRecordIndex) const noexcept;
private:
    struct WaypointNameBinding final {
        container::String name;
        uint32_t id = UINT32_MAX;
    };

    TerrainWaterArea* waterAreaByTrigger(uint32_t triggerId) noexcept;
    const TerrainWaterArea* waterAreaByTrigger(uint32_t triggerId) const noexcept;
    void markWaterMutation() noexcept;
    void markPathfindWaterMutation() noexcept;
    void updateWaterRaw(int64_t deltaSecondsRaw, uint64_t confirmedTick,
                        uint32_t logicFramesPerSecond) noexcept;
    void rebuildWaypointGraphRevision() noexcept;

    TerrainMap m_map;
    MapContentIdentity m_contentIdentity;
    // Retained only across the synchronous session-start consumers. Bootstrap
    // releases this handle immediately after legacy script/Scenario import.
    game::MapSourceHandle m_startupMapSource;
    // Preserve MapObject/source insertion order for duplicate authored names.
    // RefCode prepends new waypoints to its linked list, so getWaypointByName
    // observes the last-authored matching name first.  The hash map remains
    // ID-only; name resolution deliberately scans this record in reverse.
    container::Vector<WaypointNameBinding> m_waypointNames;
    container::HashMap<uint32_t, WaypointRecord> m_waypoints;
    // FNV-1a's offset basis is also the deterministic nonzero empty-graph
    // revision produced by rebuildWaypointGraphRevision().
    uint64_t m_waypointGraphRevision = 14695981039346656037ull;
    container::Vector<MultiplayerStartPosition> m_multiplayerStartPositions;
    container::Vector<PolygonTriggerRecord> m_triggers;
    container::Vector<TerrainWaterArea> m_waterAreas;
    container::Vector<TerrainElevatedPathfindSurface> m_elevatedPathfindSurfaces;
    container::Vector<uint64_t> m_destroyedBridgeSourceRecords;
    container::Vector<TerrainWaterDamagePulse> m_waterDamagePulses;
    engine::TerrainVertexWaterState m_vertexWaterState;
    uint64_t m_waveWaterTick = UINT64_MAX;
    uint64_t m_standaloneWaterTick = 0;
    uint64_t m_waterRevision = 0;
    uint64_t m_pathfindWaterRevision = 0;
};

} // namespace game::terrain
