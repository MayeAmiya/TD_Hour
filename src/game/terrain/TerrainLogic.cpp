#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "core/math/fixed/q32_32.h"
#include "TerrainLogic.h"
#include "core/config/GlobalData.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>

namespace game::terrain {
namespace {

using Fixed = math::q32_32;
constexpr int64_t kFixedOneRaw = int64_t{1} << 32u;

[[nodiscard]] int64_t fixedFloorRawToInteger(int64_t raw) noexcept {
    int64_t quotient = raw / kFixedOneRaw;
    if (raw < 0 && raw % kFixedOneRaw != 0) --quotient;
    return quotient;
}

[[nodiscard]] int64_t legacyRoundedRawToInteger(int64_t raw) noexcept {
    constexpr int64_t half = int64_t{1} << 31u;
    return fixedFloorRawToInteger(
        raw > std::numeric_limits<int64_t>::max() - half
            ? std::numeric_limits<int64_t>::max()
            : raw + half);
}

[[nodiscard]] bool pointInsideBoundaryRaw(
    container::Span<const container::Array<int64_t, 3>> boundary,
    int64_t xRaw, int64_t yRaw) noexcept {
    if (boundary.size() < 3) return false;
    const Fixed x = Fixed::from_raw(xRaw);
    const Fixed y = Fixed::from_raw(yRaw);
    bool inside = false;
    for (size_t index = 0, previous = boundary.size() - 1;
         index < boundary.size(); previous = index++) {
        const auto& a = boundary[index];
        const auto& b = boundary[previous];
        const Fixed ax = Fixed::from_raw(a[0]);
        const Fixed ay = Fixed::from_raw(a[1]);
        const Fixed bx = Fixed::from_raw(b[0]);
        const Fixed by = Fixed::from_raw(b[1]);
        const Fixed dy = by - ay;
        const bool crosses = ((ay > y) != (by > y)) && dy != Fixed{} &&
            x < (bx - ax) * (y - ay) / dy + ax;
        if (crosses) inside = !inside;
    }
    return inside;
}

[[nodiscard]] std::optional<int64_t> triangleHeightRawAt(
    const container::Array<int64_t, 3>& a,
    const container::Array<int64_t, 3>& b,
    const container::Array<int64_t, 3>& c,
    int64_t xRaw, int64_t yRaw) noexcept {
    const Fixed ax = Fixed::from_raw(a[0]);
    const Fixed ay = Fixed::from_raw(a[1]);
    const Fixed az = Fixed::from_raw(a[2]);
    const Fixed bx = Fixed::from_raw(b[0]);
    const Fixed by = Fixed::from_raw(b[1]);
    const Fixed bz = Fixed::from_raw(b[2]);
    const Fixed cx = Fixed::from_raw(c[0]);
    const Fixed cy = Fixed::from_raw(c[1]);
    const Fixed cz = Fixed::from_raw(c[2]);
    const Fixed x = Fixed::from_raw(xRaw);
    const Fixed y = Fixed::from_raw(yRaw);
    const Fixed denominator = (by - cy) * (ax - cx) +
        (cx - bx) * (ay - cy);
    if (denominator == Fixed{}) return std::nullopt;
    const Fixed first = ((by - cy) * (x - cx) +
        (cx - bx) * (y - cy)) / denominator;
    const Fixed second = ((cy - ay) * (x - cx) +
        (ax - cx) * (y - cy)) / denominator;
    const Fixed third = Fixed{1} - first - second;
    const Fixed epsilon = Fixed::from_raw(429496);
    if (first < -epsilon || second < -epsilon || third < -epsilon) {
        return std::nullopt;
    }
    return (first * az + second * bz + third * cz).raw();
}

[[nodiscard]] std::optional<int64_t> surfaceHeightRawAt(
    const TerrainElevatedPathfindSurface& surface,
    int64_t xRaw, int64_t yRaw) noexcept {
    if (!pointInsideBoundaryRaw(surface.boundaryRaw, xRaw, yRaw)) {
        return std::nullopt;
    }
    for (size_t index = 1; index + 1 < surface.boundaryRaw.size(); ++index) {
        if (const std::optional<int64_t> height = triangleHeightRawAt(
                surface.boundaryRaw.front(), surface.boundaryRaw[index],
                surface.boundaryRaw[index + 1], xRaw, yRaw)) {
            return height;
        }
    }
    return surface.heightRaw;
}

[[nodiscard]] bool pointInsideTriggerRaw(
    const PolygonTriggerRecord& trigger, int64_t xRaw,
    int64_t yRaw) noexcept {
    if (trigger.points.size() < 3) return false;
    const Fixed x = Fixed::from_raw(xRaw);
    const Fixed y = Fixed::from_raw(yRaw);
    bool inside = false;
    for (size_t index = 0, previous = trigger.points.size() - 1;
         index < trigger.points.size(); previous = index++) {
        const math::int3& a = trigger.points[index];
        const math::int3& b = trigger.points[previous];
        const Fixed ax{a.x};
        const Fixed ay{a.y};
        const Fixed bx{b.x};
        const Fixed by{b.y};
        const Fixed dy = by - ay;
        const bool crosses = ((ay > y) != (by > y)) && dy != Fixed{} &&
            x < (bx - ax) * (y - ay) / dy + ax;
        if (crosses) inside = !inside;
    }
    return inside;
}

[[nodiscard]] container::String canonicalAscii(container::StringView value) {
    container::String result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(container::asciiLower(static_cast<char>(character)));
    }
    return result;
}

constexpr auto asciiEqualsIgnoreCase = container::asciiEqualIgnoreCase;

[[nodiscard]] std::optional<int32_t> playerStartIndex(container::StringView waypointName) {
    constexpr container::StringView prefix = "player_";
    constexpr container::StringView suffix = "_start";
    const container::String normalized = canonicalAscii(waypointName);
    if (!normalized.starts_with(prefix) || !normalized.ends_with(suffix) ||
        normalized.size() <= prefix.size() + suffix.size()) {
        return std::nullopt;
    }

    const char* first = normalized.data() + prefix.size();
    const char* last = normalized.data() + normalized.size() - suffix.size();
    uint64_t oneBased = 0;
    const auto [end, parseError] = std::from_chars(first, last, oneBased);
    if (parseError != std::errc{} || end != last || oneBased == 0 ||
        oneBased > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
    }
    return static_cast<int32_t>(oneBased - 1);
}

[[nodiscard]] bool pointInsideBoundary(
    container::Span<const math::vec3> boundary, float x, float y) noexcept {
    if (boundary.size() < 3 || !std::isfinite(x) || !std::isfinite(y)) return false;
    bool inside = false;
    for (size_t index = 0, previous = boundary.size() - 1;
         index < boundary.size(); previous = index++) {
        const math::vec3& a = boundary[index];
        const math::vec3& b = boundary[previous];
        const float dy = b.y() - a.y();
        const bool crosses = ((a.y() > y) != (b.y() > y)) &&
            std::abs(dy) > std::numeric_limits<float>::epsilon() &&
            x < (b.x() - a.x()) * (y - a.y()) / dy + a.x();
        if (crosses) inside = !inside;
    }
    return inside;
}

[[nodiscard]] std::optional<float> triangleHeightAt(
    const math::vec3& a, const math::vec3& b, const math::vec3& c,
    float x, float y) noexcept {
    const float denominator = (b.y() - c.y()) * (a.x() - c.x()) +
        (c.x() - b.x()) * (a.y() - c.y());
    if (std::abs(denominator) <= std::numeric_limits<float>::epsilon()) {
        return std::nullopt;
    }
    const float first = ((b.y() - c.y()) * (x - c.x()) +
        (c.x() - b.x()) * (y - c.y())) / denominator;
    const float second = ((c.y() - a.y()) * (x - c.x()) +
        (a.x() - c.x()) * (y - c.y())) / denominator;
    const float third = 1.0f - first - second;
    constexpr float kBarycentricEpsilon = 0.0001f;
    if (first < -kBarycentricEpsilon || second < -kBarycentricEpsilon ||
        third < -kBarycentricEpsilon) {
        return std::nullopt;
    }
    return first * a.z() + second * b.z() + third * c.z();
}

[[nodiscard]] std::optional<float> surfaceHeightAt(
    const TerrainElevatedPathfindSurface& surface, float x, float y) noexcept {
    if (!pointInsideBoundary(surface.boundary, x, y)) return std::nullopt;
    for (size_t index = 1; index + 1 < surface.boundary.size(); ++index) {
        if (const std::optional<float> height = triangleHeightAt(
                surface.boundary.front(), surface.boundary[index],
                surface.boundary[index + 1], x, y)) {
            return height;
        }
    }
    // Concave Mod polygons may not be representable by a simple fan. The
    // authored flat height remains the deterministic compatibility fallback;
    // bridge import should emit convex sections for fully sloped sampling.
    return surface.height;
}

} // namespace

bool TerrainLogic::loadMap(container::StringView path, container::String* error) {
    game::MapSourceHandle source;
    if (!game::loadMapSourceBlob(path, source, error)) {
        clear();
        return false;
    }
    const bool loaded = loadMap(source, error);
    // Standalone path callers have no second startup consumer. Session startup
    // uses the handle overload and releases it after script/Scenario import.
    releaseStartupMapSource();
    return loaded;
}

bool TerrainLogic::loadMap(const game::MapSourceHandle& source,
                           container::String* error) {
    float legacyWaterExtentX = 2000.0f;
    float legacyWaterExtentY = 2000.0f;
    if (config::TheWritableGlobalData) {
        legacyWaterExtentX =
            config::TheWritableGlobalData->getWaterExtentX();
        legacyWaterExtentY =
            config::TheWritableGlobalData->getWaterExtentY();
    }
    return loadMap(source, legacyWaterExtentX, legacyWaterExtentY, error);
}

bool TerrainLogic::loadMap(const game::MapSourceHandle& source,
                           float legacyWaterExtentX,
                           float legacyWaterExtentY,
                           container::String* error) {
    clear();
    if (!source || !source->identity().isKnown() || source->bytes().empty()) {
        if (error) *error = "Map source blob is empty or has no content identity";
        return false;
    }

    MapHeightfieldLoader loader;
    loader.setLegacyWaterExtents(
        legacyWaterExtentX, legacyWaterExtentY);
    if (!loader.loadFromMemory(source->bytes()) ||
        !loader.result().isValid() || !m_map.load(loader.takeResult())) {
        if (error) *error = loader.error();
        return false;
    }
    m_contentIdentity = source->identity();
    m_startupMapSource = source;
    for (const MapObjectRecord& object : m_map.heightfield().objects) {
        if (!object.waypointId) continue;
        WaypointRecord waypoint;
        waypoint.id=*object.waypointId; waypoint.name=object.waypointName;
        waypoint.position=object.position;
        waypoint.positionRaw=object.positionRaw;
        waypoint.rawAuthoritative=object.fixedTransformValid;
        waypoint.pathLabels=object.waypointPathLabels; waypoint.biDirectional=object.waypointPathBiDirectional;
        addWaypoint(std::move(waypoint));
    }
    struct StartWaypoint final {
        int32_t index = -1;
        uint32_t waypointId = UINT32_MAX;
        math::vec3 position{};
        container::Array<int64_t, 3> positionRaw{};
    };
    container::Vector<StartWaypoint> startWaypoints;
    startWaypoints.reserve(m_waypoints.size());
    for (const auto& [waypointId, waypoint] : m_waypoints) {
        const std::optional<int32_t> index = playerStartIndex(waypoint.name);
        if (!index) continue;
        startWaypoints.push_back({.index = *index, .waypointId = waypointId,
                                  .position = waypoint.position,
                                  .positionRaw = waypoint.positionRaw});
    }
    std::sort(startWaypoints.begin(), startWaypoints.end(),
              [](const StartWaypoint& lhs, const StartWaypoint& rhs) {
                  return lhs.index == rhs.index ? lhs.waypointId < rhs.waypointId
                                                : lhs.index < rhs.index;
              });
    for (const StartWaypoint& waypoint : startWaypoints) {
        if (!m_multiplayerStartPositions.empty() &&
            m_multiplayerStartPositions.back().index == waypoint.index) {
            clear();
            if (error) {
                *error = "Map declares duplicate Player_N_Start waypoint index " +
                    std::to_string(waypoint.index + 1);
            }
            return false;
        }
        m_multiplayerStartPositions.push_back({.index = waypoint.index,
                                                .position = waypoint.position,
                                                .positionRaw =
                                                    waypoint.positionRaw});
    }
    for (const WaypointLinkRecord& link : m_map.heightfield().waypointLinks) addWaypointLink(link.from, link.to);
    m_triggers = m_map.heightfield().polygonTriggers;
    for (const PolygonTriggerRecord& trigger : m_triggers) {
        if (!trigger.water || trigger.points.empty()) continue;
        TerrainWaterArea area;
        area.triggerId = trigger.id;
        area.name = trigger.name;
        area.polygon = trigger.points;
        area.river = trigger.river;
        area.riverStart = trigger.riverStart;
        area.synthesizedLegacyWater = trigger.synthesizedLegacyWater;
        area.surfaceHeightRaw = Fixed{trigger.points.front().z}.raw();
        area.targetHeightRaw = area.surfaceHeightRaw;
        m_waterAreas.push_back(std::move(area));
    }
    if (!m_waterAreas.empty()) markPathfindWaterMutation();
    if (error) error->clear();
    return true;
}
void TerrainLogic::clear() noexcept {
    m_map.clear();
    m_contentIdentity = {};
    m_startupMapSource.reset();
    m_waypointNames.clear();
    m_waypoints.clear();
    rebuildWaypointGraphRevision();
    m_multiplayerStartPositions.clear();
    m_triggers.clear();
    m_waterAreas.clear();
    m_elevatedPathfindSurfaces.clear();
    m_destroyedBridgeSourceRecords.clear();
    m_waterDamagePulses.clear();
    m_vertexWaterState.clear();
    m_waveWaterTick = UINT64_MAX;
    m_standaloneWaterTick = 0;
    markPathfindWaterMutation();
}

bool TerrainLogic::setHeightSample(int32_t x, int32_t y, uint8_t sample) noexcept {
    return m_map.setHeightSample(x, y, sample);
}

bool TerrainLogic::deformTerrainCircleRaw(
    int64_t worldXRaw, int64_t worldYRaw, int64_t radiusWorldRaw,
    int64_t heightDeltaWorldRaw) noexcept {
    return m_map.deformCircleRaw(
        worldXRaw, worldYRaw, radiusWorldRaw, heightDeltaWorldRaw);
}

bool TerrainLogic::deformTerrainCircle(float worldX, float worldY, float radiusWorld,
                                       float heightDeltaWorld) noexcept {
    return m_map.deformCircle(worldX, worldY, radiusWorld, heightDeltaWorld);
}

TerrainFlattenResult TerrainLogic::flattenFootprintRaw(
    const TerrainFlattenFootprint& footprint) noexcept {
    return m_map.flattenFootprintRaw(footprint);
}

TerrainWaterArea* TerrainLogic::waterAreaByTrigger(uint32_t triggerId) noexcept {
    const auto found = std::find_if(m_waterAreas.begin(), m_waterAreas.end(),
        [triggerId](const TerrainWaterArea& area) { return area.triggerId == triggerId; });
    return found == m_waterAreas.end() ? nullptr : &*found;
}

const TerrainWaterArea* TerrainLogic::waterAreaByTrigger(uint32_t triggerId) const noexcept {
    const auto found = std::find_if(m_waterAreas.begin(), m_waterAreas.end(),
        [triggerId](const TerrainWaterArea& area) { return area.triggerId == triggerId; });
    return found == m_waterAreas.end() ? nullptr : &*found;
}

void TerrainLogic::markWaterMutation() noexcept {
    ++m_waterRevision;
    if (m_waterRevision == 0) ++m_waterRevision;
}

void TerrainLogic::markPathfindWaterMutation() noexcept {
    markWaterMutation();
    ++m_pathfindWaterRevision;
    if (m_pathfindWaterRevision == 0) ++m_pathfindWaterRevision;
}

// Script parameters arrive from the map file with no validation, and isfinite
// does not bound magnitude: Fixed{1e38f} saturates surfaceHeightRaw to
// INT64_MAX, which makes every ground height compare as "underwater" and lets
// the next navigation water raster flip the whole covered area's movement mask
// to Water|Air — one malformed float immobilizing ground units map-wide.
// The bound has to sit on the Fixed overloads too, not just the float ones:
// the authoritative script path converts the authored float to Fixed back in
// the legacy compiler (LegacyScriptCompilerActionsPresentation WATER_CHANGE_-
// HEIGHT) and only ever calls setWaterHeightFixed/beginFloodFixed, so a
// float-only guard never runs for real map content.
//
// One limit covers heights, damage amounts and flood rates. It is ~400x the
// legal map-object vertical span, so no authored height comes near it, and a
// flood rate is |targetDelta| / seconds -- bounded by that same span over a
// minimum one-tick duration, which is three orders of magnitude below the
// limit. Rejecting rather than clamping is deliberate: a clamped water surface
// is indistinguishable from authored content, whereas the caller can report a
// rejection (see GameSessionScriptAuthorityPort::applyWaterAuthority).
static constexpr float kMaximumAdmittedWaterMagnitude = 1.0e6f;
static constexpr int64_t kMaximumAdmittedWaterMagnitudeRaw =
    int64_t{1000000} << 32u;

[[nodiscard]] static bool admissibleWaterValue(float value) noexcept {
    return std::isfinite(value) && value >= -kMaximumAdmittedWaterMagnitude &&
           value <= kMaximumAdmittedWaterMagnitude;
}

bool TerrainLogic::admissibleWaterValueFixed(Fixed value) noexcept {
    return value.raw() >= -kMaximumAdmittedWaterMagnitudeRaw &&
           value.raw() <= kMaximumAdmittedWaterMagnitudeRaw;
}

bool TerrainLogic::setWaterHeight(uint32_t triggerId, float height, float damageAmount) noexcept {
    if (!admissibleWaterValue(height) || !admissibleWaterValue(damageAmount)) {
        return false;
    }
    return setWaterHeightFixed(
        triggerId, Fixed{height}, Fixed{damageAmount});
}

bool TerrainLogic::setWaterHeightFixed(
    uint32_t triggerId, Fixed admittedHeight,
    Fixed admittedDamage) noexcept {
    if (!admissibleWaterValueFixed(admittedHeight) ||
        !admissibleWaterValueFixed(admittedDamage)) {
        return false;
    }
    TerrainWaterArea* area = waterAreaByTrigger(triggerId);
    if (!area) return false;
    if (area->surfaceHeightRaw == admittedHeight.raw()) return false;
    const int64_t previousHeightRaw = area->surfaceHeightRaw;
    area->surfaceHeightRaw = admittedHeight.raw();
    area->targetHeightRaw = admittedHeight.raw();
    area->changePerSecondRaw = 0;
    area->damageAmountRaw = 0;
    markPathfindWaterMutation();
    if (admittedDamage > Fixed{} && area->surfaceHeightRaw > previousHeightRaw) {
        m_waterDamagePulses.push_back({
            .triggerId = triggerId,
            .previousHeightRaw = previousHeightRaw,
            .currentHeightRaw = area->surfaceHeightRaw,
            .damageAmountRaw = admittedDamage.raw(),
            .finalTransition = true,
        });
    }
    return true;
}

bool TerrainLogic::beginFlood(uint32_t triggerId, float targetHeight, float changePerSecond,
                              float damageAmount) noexcept {
    if (!admissibleWaterValue(targetHeight) ||
        !admissibleWaterValue(changePerSecond) ||
        !admissibleWaterValue(damageAmount) || changePerSecond <= 0.0f) {
        return false;
    }
    return beginFloodFixed(
        triggerId, Fixed{targetHeight}, Fixed{changePerSecond},
        Fixed{damageAmount});
}

bool TerrainLogic::beginFloodFixed(
    uint32_t triggerId, Fixed admittedTarget, Fixed admittedRate,
    Fixed admittedDamage) noexcept {
    if (!admissibleWaterValueFixed(admittedTarget) ||
        !admissibleWaterValueFixed(admittedRate) ||
        !admissibleWaterValueFixed(admittedDamage)) {
        return false;
    }
    TerrainWaterArea* area = waterAreaByTrigger(triggerId);
    if (!area || admittedRate <= Fixed{}) return false;
    if (area->targetHeightRaw == admittedTarget.raw() &&
        area->changePerSecondRaw == admittedRate.raw() &&
        area->damageAmountRaw == admittedDamage.raw()) {
        return false;
    }
    area->targetHeightRaw = admittedTarget.raw();
    area->changePerSecondRaw = admittedRate.raw();
    area->damageAmountRaw = admittedDamage.raw();
    markWaterMutation();
    return true;
}

void TerrainLogic::update(float deltaSeconds) noexcept {
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) return;
    const uint64_t tick = m_standaloneWaterTick++;
    updateWaterRaw(Fixed{deltaSeconds}.raw(), tick, 30);
}

void TerrainLogic::update(float deltaSeconds, uint64_t confirmedTick,
                          uint32_t logicFramesPerSecond) noexcept {
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) return;
    updateAtLogicRate(confirmedTick, logicFramesPerSecond);
}

void TerrainLogic::updateAtLogicRate(
    uint64_t confirmedTick, uint32_t logicFramesPerSecond) noexcept {
    const uint32_t framesPerSecond = std::max<uint32_t>(1, logicFramesPerSecond);
    updateWaterRaw(
        Fixed::from_fraction(1, framesPerSecond).raw(),
        confirmedTick, framesPerSecond);
}

void TerrainLogic::updateWaterRaw(
    int64_t deltaSecondsRaw, uint64_t confirmedTick,
    uint32_t logicFramesPerSecond) noexcept {
    const uint32_t framesPerSecond =
        std::max<uint32_t>(1, logicFramesPerSecond);
    const Fixed delta = Fixed::from_raw(deltaSecondsRaw);
    if (delta <= Fixed{}) return;
    bool changed = false;
    for (TerrainWaterArea& area : m_waterAreas) {
        if (!area.isTransitioning()) continue;
        const int64_t previousHeightRaw = area.surfaceHeightRaw;
        const Fixed step = Fixed::from_raw(area.changePerSecondRaw) * delta;
        bool finalTransition = false;
        if (area.surfaceHeightRaw < area.targetHeightRaw) {
            const Fixed advanced = Fixed::from_raw(area.surfaceHeightRaw) + step;
            finalTransition = advanced.raw() >= area.targetHeightRaw;
            area.surfaceHeightRaw = finalTransition
                ? area.targetHeightRaw : advanced.raw();
        } else {
            const Fixed advanced = Fixed::from_raw(area.surfaceHeightRaw) - step;
            finalTransition = advanced.raw() <= area.targetHeightRaw;
            area.surfaceHeightRaw = finalTransition
                ? area.targetHeightRaw : advanced.raw();
        }
        const int64_t damageAmountRaw = area.damageAmountRaw;
        if (finalTransition) {
            // RefCode removes this water-update record immediately after its
            // final setWaterHeight call. Keep target as the durable height but
            // retire the transition-only rate/amount for renderer snapshots.
            area.changePerSecondRaw = 0;
            area.damageAmountRaw = 0;
        }
        const bool shouldPulseDamage = finalTransition ||
            (confirmedTick % static_cast<uint64_t>(framesPerSecond) == 0);
        if (shouldPulseDamage && damageAmountRaw > 0 &&
            area.surfaceHeightRaw > previousHeightRaw) {
            m_waterDamagePulses.push_back({
                .triggerId = area.triggerId,
                .previousHeightRaw = previousHeightRaw,
                .currentHeightRaw = area.surfaceHeightRaw,
                .damageAmountRaw = damageAmountRaw,
                .finalTransition = finalTransition,
            });
        }
        changed = true;
    }
    if (changed) markPathfindWaterMutation();
}

container::Vector<TerrainWaterDamagePulse> TerrainLogic::takeWaterDamagePulses() {
    container::Vector<TerrainWaterDamagePulse> output = std::move(m_waterDamagePulses);
    m_waterDamagePulses.clear();
    return output;
}

void TerrainLogic::addWaveWaterMotion(
    float x, float y, float velocity, float preferredHeight,
    uint64_t confirmedTick) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(velocity) || !std::isfinite(preferredHeight)) {
        return;
    }
    if (m_vertexWaterState.addVelocity(
            x, y, velocity, preferredHeight) &&
        m_waveWaterTick != confirmedTick) {
        m_waveWaterTick = confirmedTick;
        markWaterMutation();
    }
}
bool TerrainLogic::addWaypoint(WaypointRecord waypoint) {
    if (!m_map.isLoaded() || waypoint.id == UINT32_MAX) return false;
    if (waypoint.rawAuthoritative) {
        waypoint.positionRaw[2] = m_map.groundHeightRaw(
            waypoint.positionRaw[0], waypoint.positionRaw[1]);
        waypoint.position = {
            Fixed::from_raw(waypoint.positionRaw[0]).to_float(),
            Fixed::from_raw(waypoint.positionRaw[1]).to_float(),
            Fixed::from_raw(waypoint.positionRaw[2]).to_float(),
        };
    } else {
        waypoint.position[2] = m_map.groundHeight(
            waypoint.position.x(), waypoint.position.y());
        waypoint.positionRaw = {
            Fixed{waypoint.position.x()}.raw(),
            Fixed{waypoint.position.y()}.raw(),
            Fixed{waypoint.position.z()}.raw(),
        };
        waypoint.rawAuthoritative = true;
    }
    const uint32_t id = waypoint.id;
    const auto [inserted, created] = m_waypoints.emplace(id, std::move(waypoint));
    if (!created) return false;
    m_waypointNames.push_back({.name = inserted->second.name, .id = id});
    rebuildWaypointGraphRevision();
    return true;
}
bool TerrainLogic::addWaypointLink(uint32_t from, uint32_t to) {
    auto a=m_waypoints.find(from); auto b=m_waypoints.find(to);
    if(a==m_waypoints.end() || b==m_waypoints.end() || from==to) return false;
    const auto add=[&](WaypointRecord& source,uint32_t target) { if(source.links.size()>=8 || std::find(source.links.begin(),source.links.end(),target)!=source.links.end()) return false; source.links.push_back(target); return true; };
    const bool added=add(a->second,to); if(added && a->second.biDirectional) add(b->second,from);
    if (added) rebuildWaypointGraphRevision();
    return added;
}
const WaypointRecord* TerrainLogic::waypointById(uint32_t id) const noexcept { auto it=m_waypoints.find(id); return it==m_waypoints.end()?nullptr:&it->second; }
const WaypointRecord* TerrainLogic::waypointByName(container::StringView name) const noexcept {
    // TerrainLogic appends source records while RefCode's waypoint list is
    // prepend-only.  Reverse traversal therefore preserves the legacy
    // getWaypointByName behavior for duplicate authored names: last added
    // wins.  All script create/audio/camera callers share this one lookup.
    for (auto binding = m_waypointNames.rbegin(); binding != m_waypointNames.rend(); ++binding) {
        if (binding->name != name) continue;
        const auto found = m_waypoints.find(binding->id);
        if (found != m_waypoints.end()) return &found->second;
    }
    return nullptr;
}
const WaypointRecord* TerrainLogic::closestWaypointOnPath(
    float x, float y, container::StringView pathLabel) const noexcept {
    if (pathLabel.empty() || !std::isfinite(x) || !std::isfinite(y)) {
        return nullptr;
    }
    return closestWaypointOnPathRaw(
        Fixed{x}.raw(), Fixed{y}.raw(), pathLabel);
}

const WaypointRecord* TerrainLogic::closestWaypointOnPathRaw(
    int64_t xRaw, int64_t yRaw,
    container::StringView pathLabel) const noexcept {
    if (pathLabel.empty()) return nullptr;
    const WaypointRecord* closest = nullptr;
    Fixed closestDistanceSquared{};
    bool foundClosest = false;
    // Source records are appended locally but prepended by RefCode. Reverse
    // traversal therefore visits the last-authored waypoint first, and the
    // strict comparison below preserves it when distances tie.
    for (auto binding = m_waypointNames.rbegin();
         binding != m_waypointNames.rend(); ++binding) {
        const auto found = m_waypoints.find(binding->id);
        if (found == m_waypoints.end()) continue;
        const WaypointRecord& waypoint = found->second;
        const bool matches = std::any_of(
            waypoint.pathLabels.begin(), waypoint.pathLabels.end(),
            [&](container::StringView candidate) {
                return asciiEqualsIgnoreCase(candidate, pathLabel);
            });
        if (!matches) continue;
        const Fixed dx = Fixed::from_raw(waypoint.positionRaw[0]) -
                         Fixed::from_raw(xRaw);
        const Fixed dy = Fixed::from_raw(waypoint.positionRaw[1]) -
                         Fixed::from_raw(yRaw);
        const Fixed distanceSquared = dx * dx + dy * dy;
        if (!foundClosest || distanceSquared < closestDistanceSquared) {
            closest = &waypoint;
            closestDistanceSquared = distanceSquared;
            foundClosest = true;
        }
    }
    return closest;
}

void TerrainLogic::rebuildWaypointGraphRevision() noexcept {
    constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    uint64_t hash = kFnvOffsetBasis;
    const auto hashByte = [&](uint8_t value) {
        hash ^= value;
        hash *= kFnvPrime;
    };
    const auto hashUint32 = [&](uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hashByte(static_cast<uint8_t>(value >> shift));
        }
    };
    const auto hashUint64 = [&](uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            hashByte(static_cast<uint8_t>(value >> shift));
        }
    };
    const auto hashString = [&](container::StringView value) {
        hashUint64(static_cast<uint64_t>(value.size()));
        for (const unsigned char character : value) hashByte(character);
    };

    container::Vector<uint32_t> waypointIds;
    waypointIds.reserve(m_waypoints.size());
    for (const auto& [id, waypoint] : m_waypoints) waypointIds.push_back(id);
    std::sort(waypointIds.begin(), waypointIds.end());
    for (const uint32_t id : waypointIds) {
        const WaypointRecord& waypoint = m_waypoints.find(id)->second;
        hashUint32(id);
        hashString(waypoint.name);
        hashUint32(std::bit_cast<uint32_t>(waypoint.position.x()));
        hashUint32(std::bit_cast<uint32_t>(waypoint.position.y()));
        hashUint32(std::bit_cast<uint32_t>(waypoint.position.z()));
        for (const container::String& pathLabel : waypoint.pathLabels) {
            hashString(pathLabel);
        }
        hashByte(waypoint.biDirectional ? 1u : 0u);
        hashUint64(static_cast<uint64_t>(waypoint.links.size()));
        for (const uint32_t link : waypoint.links) hashUint32(link);
    }
    m_waypointGraphRevision = hash == 0 ? 1 : hash;
}
const WaypointRecord* TerrainLogic::nearestWaypoint(float x, float y,
                                                     float z) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return nullptr;
    return nearestWaypointRaw(
        Fixed{x}.raw(), Fixed{y}.raw(), Fixed{z}.raw());
}

const WaypointRecord* TerrainLogic::nearestWaypointRaw(
    int64_t xRaw, int64_t yRaw, int64_t zRaw) const noexcept {
    const WaypointRecord* nearest = nullptr;
    Fixed nearestDistanceSquared{};
    for (const auto& [id, waypoint] : m_waypoints) {
        // Subtract as saturating Fixed values, not as raw int64.  A saturated
        // coordinate (an authored value large enough to pin q32_32 at
        // +/-INT64_MAX) made the raw form signed-overflow UB on any ordinary
        // query; closestWaypointOnPathRaw already used the saturating form.
        const Fixed dx = Fixed::from_raw(waypoint.positionRaw[0]) - Fixed::from_raw(xRaw);
        const Fixed dy = Fixed::from_raw(waypoint.positionRaw[1]) - Fixed::from_raw(yRaw);
        const Fixed dz = Fixed::from_raw(waypoint.positionRaw[2]) - Fixed::from_raw(zRaw);
        const Fixed distanceSquared = dx * dx + dy * dy + dz * dz;
        if (!nearest || distanceSquared < nearestDistanceSquared ||
            (distanceSquared == nearestDistanceSquared && id < nearest->id)) {
            nearest = &waypoint;
            nearestDistanceSquared = distanceSquared;
        }
    }
    return nearest;
}
const PolygonTriggerRecord* TerrainLogic::triggerById(uint32_t id) const noexcept { for (const auto& trigger : m_triggers) if (trigger.id == id) return &trigger; return nullptr; }
const PolygonTriggerRecord* TerrainLogic::triggerByName(container::StringView name) const noexcept { for (const auto& trigger : m_triggers) if (trigger.name == name) return &trigger; return nullptr; }
std::optional<PolygonTriggerLegacyBounds> TerrainLogic::legacyTriggerBounds(
    const PolygonTriggerRecord& trigger) noexcept {
    if (trigger.points.empty()) return std::nullopt;
    int32_t minimumX = trigger.points.front().x;
    int32_t maximumX = minimumX;
    int32_t minimumY = trigger.points.front().y;
    int32_t maximumY = minimumY;
    for (const math::int3& point : trigger.points) {
        minimumX = std::min(minimumX, point.x);
        maximumX = std::max(maximumX, point.x);
        minimumY = std::min(minimumY, point.y);
        maximumY = std::max(maximumY, point.y);
    }
    const Fixed halfWidth =
        (Fixed{maximumX} - Fixed{minimumX}) /
        Fixed{int32_t{2}};
    // RefCode PolygonTrigger::updateBounds shipped with `hi.y + lo.y`
    // here. It makes radius depend on the area's world-space Y coordinate,
    // but authored GuardArea behavior and old maps rely on that exact value.
    const Fixed legacyHalfHeight =
        (Fixed{maximumY} + Fixed{minimumY}) /
        Fixed{int32_t{2}};
    return PolygonTriggerLegacyBounds{
        .centerX = (Fixed{minimumX} + Fixed{maximumX}) /
                   Fixed{int32_t{2}},
        .centerY = (Fixed{minimumY} + Fixed{maximumY}) /
                   Fixed{int32_t{2}},
        .radius = Fixed::sqrt(halfWidth * halfWidth +
                              legacyHalfHeight * legacyHalfHeight),
    };
}
uint64_t TerrainLogic::triggerRevision(
    const PolygonTriggerRecord& trigger) noexcept {
    uint64_t hash = 1469598103934665603ull;
    const auto byte = [&hash](uint8_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    const auto integer = [&byte](uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            byte(static_cast<uint8_t>(value >> shift));
    };
    const auto text = [&integer, &byte](container::StringView value) noexcept {
        integer(value.size());
        for (const char character : value)
            byte(static_cast<uint8_t>(character));
    };
    integer(trigger.id);
    text(trigger.name);
    text(trigger.layerName);
    byte(trigger.water ? uint8_t{1} : uint8_t{0});
    byte(trigger.river ? uint8_t{1} : uint8_t{0});
    integer(static_cast<uint32_t>(trigger.riverStart));
    byte(trigger.synthesizedLegacyWater ? uint8_t{1} : uint8_t{0});
    integer(trigger.points.size());
    for (const math::int3& point : trigger.points) {
        integer(static_cast<uint32_t>(point.x));
        integer(static_cast<uint32_t>(point.y));
        integer(static_cast<uint32_t>(point.z));
    }
    return hash == 0 ? 1 : hash;
}
bool TerrainLogic::isInsideTrigger(const PolygonTriggerRecord& trigger, float x, float y) const noexcept {
    if (trigger.points.size() < 3) return false;
    bool inside = false;
    for (size_t i=0, j=trigger.points.size()-1; i<trigger.points.size(); j=i++) {
        const auto& a=trigger.points[i]; const auto& b=trigger.points[j];
        const bool crosses=((a.y>y)!=(b.y>y)) && (x < (static_cast<float>(b.x-a.x)*(y-a.y)/static_cast<float>(b.y-a.y)+a.x));
        if (crosses) inside=!inside;
    }
    return inside;
}
bool TerrainLogic::isInsideTriggerRaw(
    const PolygonTriggerRecord& trigger, int64_t xRaw,
    int64_t yRaw) const noexcept {
    return pointInsideTriggerRaw(trigger, xRaw, yRaw);
}
bool TerrainLogic::isInsideTriggerLegacy(
    const PolygonTriggerRecord& trigger, float worldX, float worldY) const noexcept {
    if (!std::isfinite(worldX) || !std::isfinite(worldY)) return false;
    return isInsideTriggerLegacyRaw(
        trigger, Fixed{worldX}.raw(), Fixed{worldY}.raw());
}

bool TerrainLogic::isInsideTriggerLegacyRaw(
    const PolygonTriggerRecord& trigger, int64_t worldXRaw,
    int64_t worldYRaw) const noexcept {
    if (trigger.points.empty()) return false;
    constexpr int64_t oneRaw = int64_t{1} << 32u;
    const int64_t truncatedX = worldXRaw / oneRaw;
    const int64_t truncatedY = worldYRaw / oneRaw;
    if (truncatedX < std::numeric_limits<int32_t>::min() ||
        truncatedX > std::numeric_limits<int32_t>::max() ||
        truncatedY < std::numeric_limits<int32_t>::min() ||
        truncatedY > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    const int32_t pointX = static_cast<int32_t>(truncatedX);
    const int32_t pointY = static_cast<int32_t>(truncatedY);
    int32_t minimumX = trigger.points.front().x;
    int32_t maximumX = minimumX;
    int32_t minimumY = trigger.points.front().y;
    int32_t maximumY = minimumY;
    for (const math::int3& point : trigger.points) {
        minimumX = std::min(minimumX, point.x);
        maximumX = std::max(maximumX, point.x);
        minimumY = std::min(minimumY, point.y);
        maximumY = std::max(maximumY, point.y);
    }
    if (pointX < minimumX || pointX > maximumX ||
        pointY < minimumY || pointY > maximumY) return false;
    bool inside = false;
    for (size_t index = 0; index < trigger.points.size(); ++index) {
        const math::int3& first = trigger.points[index];
        const math::int3& second =
            trigger.points[(index + 1) % trigger.points.size()];
        if (first.y == second.y || (first.y < pointY && second.y < pointY) ||
            (first.y >= pointY && second.y >= pointY) ||
            (first.x < pointX && second.x < pointX)) continue;
        const Fixed intersectionX = Fixed{first.x} +
            (Fixed{second.x} - Fixed{first.x}) *
                (Fixed{pointY} - Fixed{first.y}) /
                (Fixed{second.y} - Fixed{first.y});
        if (intersectionX >= Fixed{pointX}) inside = !inside;
    }
    return inside;
}
bool TerrainLogic::isInsideWaterArea(float x, float y) const noexcept { for(const auto& trigger:m_triggers) if(trigger.water && isInsideTrigger(trigger,x,y)) return true; return false; }
std::optional<float> TerrainLogic::waterHeightAt(float x, float y) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    std::optional<int64_t> highest;
    for (const TerrainWaterArea& area : m_waterAreas) {
        const PolygonTriggerRecord* trigger = triggerById(area.triggerId);
        if (!trigger || !isInsideTrigger(*trigger, x, y)) continue;
        if (!highest || area.surfaceHeightRaw > *highest)
            highest = area.surfaceHeightRaw;
    }
    return highest
        ? std::optional<float>{Fixed::from_raw(*highest).to_float()}
        : std::nullopt;
}

std::optional<int64_t> TerrainLogic::waterHeightRawAt(
    int64_t xRaw, int64_t yRaw) const noexcept {
    std::optional<int64_t> highest;
    for (const TerrainWaterArea& area : m_waterAreas) {
        const PolygonTriggerRecord* trigger = triggerById(area.triggerId);
        if (!trigger || !pointInsideTriggerRaw(*trigger, xRaw, yRaw)) continue;
        if (!highest || area.surfaceHeightRaw > *highest) {
            highest = area.surfaceHeightRaw;
        }
    }
    return highest;
}

std::optional<float> TerrainLogic::waterSurfaceHeightLegacyAt(float x, float y) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    // TerrainLogic::getWaterHandle() in RefCode rounds only the point used to
    // test polygon membership. The caller retains the original coordinate for
    // any later terrain sample, which matters on a sloped shoreline.
    const float roundedX = std::floor(x + 0.5f);
    const float roundedY = std::floor(y + 0.5f);
    std::optional<int64_t> highest;
    for (const TerrainWaterArea& area : m_waterAreas) {
        const PolygonTriggerRecord* trigger = triggerById(area.triggerId);
        if (!trigger || !isInsideTrigger(*trigger, roundedX, roundedY)) continue;
        if (!highest || area.surfaceHeightRaw >= *highest)
            highest = area.surfaceHeightRaw;
    }
    return highest
        ? std::optional<float>{Fixed::from_raw(*highest).to_float()}
        : std::nullopt;
}

std::optional<int64_t> TerrainLogic::waterSurfaceHeightLegacyRawAt(
    int64_t xRaw, int64_t yRaw) const noexcept {
    const int64_t roundedX = legacyRoundedRawToInteger(xRaw);
    const int64_t roundedY = legacyRoundedRawToInteger(yRaw);
    if (roundedX < std::numeric_limits<int32_t>::min() ||
        roundedX > std::numeric_limits<int32_t>::max() ||
        roundedY < std::numeric_limits<int32_t>::min() ||
        roundedY > std::numeric_limits<int32_t>::max()) {
        return std::nullopt;
    }
    const int64_t roundedXRaw = Fixed{static_cast<int32_t>(roundedX)}.raw();
    const int64_t roundedYRaw = Fixed{static_cast<int32_t>(roundedY)}.raw();
    std::optional<int64_t> highest;
    for (const TerrainWaterArea& area : m_waterAreas) {
        const PolygonTriggerRecord* trigger = triggerById(area.triggerId);
        if (!trigger || !pointInsideTriggerRaw(
                *trigger, roundedXRaw, roundedYRaw)) {
            continue;
        }
        if (!highest || area.surfaceHeightRaw >= *highest) {
            highest = area.surfaceHeightRaw;
        }
    }
    return highest;
}

bool TerrainLogic::isUnderwaterLegacy(float x, float y) const noexcept {
    const std::optional<float> highest = waterSurfaceHeightLegacyAt(x, y);
    return highest && groundHeight(x, y) < *highest;
}

bool TerrainLogic::isUnderwaterLegacyRaw(
    int64_t xRaw, int64_t yRaw) const noexcept {
    const std::optional<int64_t> highest =
        waterSurfaceHeightLegacyRawAt(xRaw, yRaw);
    return highest && groundHeightRaw(xRaw, yRaw) < *highest;
}

bool TerrainLogic::cellTouchesUnderwaterLegacy(
    float left, float top, float right, float bottom) const noexcept {
    const container::Array<float, 4> xs{left, left, right, right};
    const container::Array<float, 4> ys{top, bottom, bottom, top};
    container::Array<float, 4> roundedXs{};
    container::Array<float, 4> roundedYs{};
    container::Array<float, 4> groundHeights{};
    for (size_t corner = 0; corner < xs.size(); ++corner) {
        if (!std::isfinite(xs[corner]) || !std::isfinite(ys[corner])) {
            return false;
        }
        roundedXs[corner] = std::floor(xs[corner] + 0.5f);
        roundedYs[corner] = std::floor(ys[corner] + 0.5f);
        groundHeights[corner] = groundHeight(xs[corner], ys[corner]);
    }
    for (const TerrainWaterArea& area : m_waterAreas) {
        const PolygonTriggerRecord* trigger = triggerById(area.triggerId);
        if (!trigger) continue;
        for (size_t corner = 0; corner < xs.size(); ++corner) {
            if (groundHeights[corner] <
                    Fixed::from_raw(area.surfaceHeightRaw).to_float() &&
                isInsideTrigger(*trigger, roundedXs[corner],
                                roundedYs[corner])) {
                return true;
            }
        }
    }
    return false;
}

bool TerrainLogic::cellTouchesUnderwaterLegacyRaw(
    int64_t leftRaw, int64_t topRaw,
    int64_t rightRaw, int64_t bottomRaw) const noexcept {
    const container::Array<int64_t, 4> xs{
        leftRaw, leftRaw, rightRaw, rightRaw};
    const container::Array<int64_t, 4> ys{
        topRaw, bottomRaw, bottomRaw, topRaw};
    container::Array<int64_t, 4> roundedXs{};
    container::Array<int64_t, 4> roundedYs{};
    container::Array<int64_t, 4> groundHeights{};
    for (size_t corner = 0; corner < xs.size(); ++corner) {
        const int64_t roundedX = legacyRoundedRawToInteger(xs[corner]);
        const int64_t roundedY = legacyRoundedRawToInteger(ys[corner]);
        if (roundedX < std::numeric_limits<int32_t>::min() ||
            roundedX > std::numeric_limits<int32_t>::max() ||
            roundedY < std::numeric_limits<int32_t>::min() ||
            roundedY > std::numeric_limits<int32_t>::max()) {
            return false;
        }
        roundedXs[corner] = Fixed{static_cast<int32_t>(roundedX)}.raw();
        roundedYs[corner] = Fixed{static_cast<int32_t>(roundedY)}.raw();
        groundHeights[corner] = groundHeightRaw(xs[corner], ys[corner]);
    }
    for (const TerrainWaterArea& area : m_waterAreas) {
        const PolygonTriggerRecord* trigger = triggerById(area.triggerId);
        if (!trigger) continue;
        for (size_t corner = 0; corner < xs.size(); ++corner) {
            if (groundHeights[corner] < area.surfaceHeightRaw &&
                pointInsideTriggerRaw(
                    *trigger, roundedXs[corner], roundedYs[corner])) {
                return true;
            }
        }
    }
    return false;
}

bool TerrainLogic::setElevatedPathfindSurfaces(
    container::Vector<TerrainElevatedPathfindSurface> surfaces) noexcept {
    for (TerrainElevatedPathfindSurface& surface : surfaces) {
        if (surface.rawAuthoritative) {
            if (surface.layer == kGroundPathfindLayer ||
                surface.boundaryRaw.size() < 3 ||
                surface.transitionEffectsHeightRaw < 0) {
                return false;
            }
            surface.boundary.clear();
            surface.boundary.reserve(surface.boundaryRaw.size());
            for (const container::Array<int64_t, 3>& point :
                 surface.boundaryRaw) {
                surface.boundary.push_back({
                    Fixed::from_raw(point[0]).to_float(),
                    Fixed::from_raw(point[1]).to_float(),
                    Fixed::from_raw(point[2]).to_float(),
                });
            }
            surface.from = {
                Fixed::from_raw(surface.fromRaw[0]).to_float(),
                Fixed::from_raw(surface.fromRaw[1]).to_float(),
                Fixed::from_raw(surface.fromRaw[2]).to_float(),
            };
            surface.to = {
                Fixed::from_raw(surface.toRaw[0]).to_float(),
                Fixed::from_raw(surface.toRaw[1]).to_float(),
                Fixed::from_raw(surface.toRaw[2]).to_float(),
            };
            surface.height = Fixed::from_raw(surface.heightRaw).to_float();
            surface.transitionEffectsHeight = Fixed::from_raw(
                surface.transitionEffectsHeightRaw).to_float();
            continue;
        }
        if (surface.layer == kGroundPathfindLayer || surface.boundary.size() < 3 ||
            !std::isfinite(surface.height) ||
            !std::isfinite(surface.transitionEffectsHeight) ||
            surface.transitionEffectsHeight < 0.0f) {
            return false;
        }
        for (const math::vec3& point : surface.boundary) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
                !std::isfinite(point.z())) {
                return false;
            }
        }
        if (!std::isfinite(surface.from.x()) ||
            !std::isfinite(surface.from.y()) ||
            !std::isfinite(surface.from.z()) ||
            !std::isfinite(surface.to.x()) ||
            !std::isfinite(surface.to.y()) ||
            !std::isfinite(surface.to.z())) {
            return false;
        }
        surface.boundaryRaw.clear();
        surface.boundaryRaw.reserve(surface.boundary.size());
        for (const math::vec3& point : surface.boundary) {
            surface.boundaryRaw.push_back({
                Fixed{point.x()}.raw(), Fixed{point.y()}.raw(),
                Fixed{point.z()}.raw()});
        }
        surface.fromRaw = {Fixed{surface.from.x()}.raw(),
                           Fixed{surface.from.y()}.raw(),
                           Fixed{surface.from.z()}.raw()};
        surface.toRaw = {Fixed{surface.to.x()}.raw(),
                         Fixed{surface.to.y()}.raw(),
                         Fixed{surface.to.z()}.raw()};
        surface.heightRaw = Fixed{surface.height}.raw();
        surface.transitionEffectsHeightRaw =
            Fixed{surface.transitionEffectsHeight}.raw();
    }
    std::sort(surfaces.begin(), surfaces.end(),
        [](const TerrainElevatedPathfindSurface& left,
           const TerrainElevatedPathfindSurface& right) {
            return left.layer == right.layer
                ? left.heightRaw < right.heightRaw
                                             : left.layer < right.layer;
        });
    m_elevatedPathfindSurfaces = std::move(surfaces);
    return true;
}

bool TerrainLogic::destroyBridgeBySourceRecordIndex(
    uint64_t sourceRecordIndex) noexcept {
    if (sourceRecordIndex == UINT64_MAX ||
        std::binary_search(m_destroyedBridgeSourceRecords.begin(),
                           m_destroyedBridgeSourceRecords.end(),
                           sourceRecordIndex)) {
        return false;
    }
    const auto position = std::lower_bound(
        m_destroyedBridgeSourceRecords.begin(),
        m_destroyedBridgeSourceRecords.end(), sourceRecordIndex);
    m_destroyedBridgeSourceRecords.insert(position, sourceRecordIndex);
    std::erase_if(m_elevatedPathfindSurfaces,
        [sourceRecordIndex](const TerrainElevatedPathfindSurface& surface) {
            return surface.sourceRecordIndex == sourceRecordIndex;
        });
    markWaterMutation();
    return true;
}

bool TerrainLogic::setBridgeActiveBySourceRecordIndex(
    uint64_t sourceRecordIndex, bool active) noexcept {
    if (sourceRecordIndex == UINT64_MAX ||
        isBridgeDestroyed(sourceRecordIndex)) {
        return false;
    }
    bool found = false;
    bool changed = false;
    for (TerrainElevatedPathfindSurface& surface :
         m_elevatedPathfindSurfaces) {
        if (surface.sourceRecordIndex != sourceRecordIndex) continue;
        found = true;
        if (surface.active == active) continue;
        surface.active = active;
        changed = true;
    }
    if (changed) markWaterMutation();
    return found;
}

bool TerrainLogic::isBridgeDestroyed(
    uint64_t sourceRecordIndex) const noexcept {
    return std::binary_search(m_destroyedBridgeSourceRecords.begin(),
                              m_destroyedBridgeSourceRecords.end(),
                              sourceRecordIndex);
}

TerrainPathfindLayerId TerrainLogic::highestPathfindLayerAt(
    float x, float y, float z) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return kGroundPathfindLayer;
    }
    TerrainPathfindLayerId result = kGroundPathfindLayer;
    float resultHeight = groundHeight(x, y);
    constexpr float kLayerSelectionEpsilon = 0.01f;
    for (const TerrainElevatedPathfindSurface& surface : m_elevatedPathfindSurfaces) {
        if (!surface.active) continue;
        const std::optional<float> sampledHeight = surfaceHeightAt(surface, x, y);
        if (!sampledHeight || *sampledHeight > z + kLayerSelectionEpsilon ||
            *sampledHeight < resultHeight) {
            continue;
        }
        result = surface.layer;
        resultHeight = *sampledHeight;
    }
    return result;
}

TerrainPathfindLayerId TerrainLogic::highestPathfindLayerAtRaw(
    int64_t xRaw, int64_t yRaw, int64_t zRaw) const noexcept {
    TerrainPathfindLayerId result = kGroundPathfindLayer;
    int64_t resultHeightRaw = groundHeightRaw(xRaw, yRaw);
    const int64_t maximumLayerHeightRaw =
        (Fixed::from_raw(zRaw) +
         Fixed::from_fraction(1, 100)).raw();
    for (const TerrainElevatedPathfindSurface& surface :
         m_elevatedPathfindSurfaces) {
        if (!surface.active) continue;
        const std::optional<int64_t> sampledHeight =
            surfaceHeightRawAt(surface, xRaw, yRaw);
        if (!sampledHeight || *sampledHeight > maximumLayerHeightRaw ||
            *sampledHeight < resultHeightRaw) {
            continue;
        }
        result = surface.layer;
        resultHeightRaw = *sampledHeight;
    }
    return result;
}

TerrainPathfindLayerId TerrainLogic::pathfindLayerForDestination(
    float x, float y, float z) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return kGroundPathfindLayer;
    }
    TerrainPathfindLayerId result = kGroundPathfindLayer;
    float bestDistance = std::abs(z - groundHeight(x, y));
    for (const TerrainElevatedPathfindSurface& surface :
         m_elevatedPathfindSurfaces) {
        if (!surface.active) continue;
        const std::optional<float> sampledHeight =
            surfaceHeightAt(surface, x, y);
        if (!sampledHeight) continue;
        const float distance = std::abs(z - *sampledHeight);
        // RefCode uses a strict comparison, so an exact tie keeps the
        // ground/earlier surface selected above.
        if (distance < bestDistance) {
            result = surface.layer;
            bestDistance = distance;
        }
    }
    return result;
}

TerrainPathfindLayerId TerrainLogic::pathfindLayerForDestinationRaw(
    int64_t xRaw, int64_t yRaw, int64_t zRaw) const noexcept {
    TerrainPathfindLayerId result = kGroundPathfindLayer;
    Fixed bestDistance = Fixed::abs(
        Fixed::from_raw(zRaw) - Fixed::from_raw(groundHeightRaw(xRaw, yRaw)));
    for (const TerrainElevatedPathfindSurface& surface :
         m_elevatedPathfindSurfaces) {
        if (!surface.active) continue;
        const std::optional<int64_t> sampledHeight =
            surfaceHeightRawAt(surface, xRaw, yRaw);
        if (!sampledHeight) continue;
        const Fixed distance = Fixed::abs(
            Fixed::from_raw(zRaw) - Fixed::from_raw(*sampledHeight));
        if (distance < bestDistance) {
            result = surface.layer;
            bestDistance = distance;
        }
    }
    return result;
}

TerrainPathfindLayerId TerrainLogic::highestPathfindLayerAtXY(
    float x, float y) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) return kGroundPathfindLayer;
    TerrainPathfindLayerId result = kGroundPathfindLayer;
    float resultHeight = groundHeight(x, y);
    for (const TerrainElevatedPathfindSurface& surface : m_elevatedPathfindSurfaces) {
        if (!surface.active) continue;
        const std::optional<float> sampledHeight = surfaceHeightAt(surface, x, y);
        if (!sampledHeight || *sampledHeight <= resultHeight) continue;
        result = surface.layer;
        resultHeight = *sampledHeight;
    }
    return result;
}

TerrainPathfindLayerId TerrainLogic::highestPathfindLayerAtXYRaw(
    int64_t xRaw, int64_t yRaw) const noexcept {
    TerrainPathfindLayerId result = kGroundPathfindLayer;
    int64_t resultHeightRaw = groundHeightRaw(xRaw, yRaw);
    for (const TerrainElevatedPathfindSurface& surface :
         m_elevatedPathfindSurfaces) {
        if (!surface.active) continue;
        const std::optional<int64_t> sampledHeight =
            surfaceHeightRawAt(surface, xRaw, yRaw);
        if (!sampledHeight || *sampledHeight <= resultHeightRaw) continue;
        result = surface.layer;
        resultHeightRaw = *sampledHeight;
    }
    return result;
}

std::optional<float> TerrainLogic::pathfindLayerHeightAt(
    TerrainPathfindLayerId layer, float x, float y) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    if (layer == kGroundPathfindLayer) return groundHeight(x, y);
    std::optional<float> result;
    for (const TerrainElevatedPathfindSurface& surface : m_elevatedPathfindSurfaces) {
        if (!surface.active || surface.layer != layer) continue;
        const std::optional<float> sampledHeight = surfaceHeightAt(surface, x, y);
        if (sampledHeight && (!result || *sampledHeight > *result)) result = sampledHeight;
    }
    return result;
}


std::optional<int64_t> TerrainLogic::pathfindLayerHeightRawAt(
    TerrainPathfindLayerId layer, int64_t xRaw, int64_t yRaw) const noexcept {
    if (layer == kGroundPathfindLayer) return groundHeightRaw(xRaw, yRaw);
    std::optional<int64_t> result;
    for (const TerrainElevatedPathfindSurface& surface :
         m_elevatedPathfindSurfaces) {
        if (!surface.active || surface.layer != layer) continue;
        const std::optional<int64_t> sampledHeight =
            surfaceHeightRawAt(surface, xRaw, yRaw);
        if (sampledHeight && (!result || *sampledHeight > *result)) {
            result = sampledHeight;
        }
    }
    return result;
}
} // namespace game::terrain
