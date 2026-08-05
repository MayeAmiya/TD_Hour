#include "core/container/container_types.h"
#include "MapVisibilityAuthority.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace game::terrain {
namespace {

[[nodiscard]] int16_t decrementSaturated(int16_t value) noexcept {
    return value == std::numeric_limits<int16_t>::min()
        ? value : static_cast<int16_t>(value - 1);
}

[[nodiscard]] int16_t incrementSaturated(int16_t value) noexcept {
    return value == std::numeric_limits<int16_t>::max()
        ? value : static_cast<int16_t>(value + 1);
}

struct CircleCellBounds final {
    int32_t firstX = 0;
    int32_t lastX = -1;
    int32_t firstY = 0;
    int32_t lastY = -1;
    math::q32_32 radiusSquared{};
};

[[nodiscard]] int64_t floorFixedRatio(math::q32_32 numerator,
                                      math::q32_32 denominator) noexcept {
    if (denominator <= math::q32_32{}) return 0;
    int64_t quotient = numerator.raw() / denominator.raw();
    if (numerator.raw() < 0 &&
        numerator.raw() % denominator.raw() != 0) {
        --quotient;
    }
    return quotient;
}

[[nodiscard]] int32_t clampCircleCellCoordinate(math::q32_32 worldCoordinate,
                                                 math::q32_32 origin,
                                                 math::q32_32 cellWorldSize,
                                                 int32_t maximumCell) noexcept {
    const int64_t cellCoordinate = floorFixedRatio(
        worldCoordinate - origin, cellWorldSize);
    if (cellCoordinate <= 0) return 0;
    if (cellCoordinate >= maximumCell) return maximumCell;
    return static_cast<int32_t>(cellCoordinate);
}

[[nodiscard]] CircleCellBounds makeCircleCellBounds(math::q32_32 centerX,
                                                     math::q32_32 centerY,
                                                     math::q32_32 radius,
                                                     int32_t width,
                                                     int32_t height,
                                                     math::q32_32 originX,
                                                     math::q32_32 originY,
                                                     math::q32_32 cellWorldSize) noexcept {
    // RefCode PartitionManager::doShroudReveal/undoShroudReveal/doShroudCover/
    // undoShroudCover floor the *cell* radius, never the world radius:
    //   Int cellRadius = worldToCellDist(radius);  // ceil(radius/cellSize)
    //   if (cellRadius < 1) cellRadius = 1;
    // so a sub-cell authored radius still touches the cell holding the center
    // and nothing more. The world-space containment test therefore has to use
    // the authored radius; PartitionData::doCircleFillPrecise does exactly
    // that (doesCircleOverlapCell against the unmodified radius). Inflating
    // the radius used by the distance test made a small authored reveal or
    // shroud cover the whole 40wu ring around the center instead.
    const math::q32_32 authoredRadius = math::q32_32::max(
        radius, math::q32_32{});
    // Cell-granularity floor: scan bounds only, so the cell containing the
    // center is always a candidate even for a zero/sub-cell radius.
    const math::q32_32 scanRadius = math::q32_32::max(
        authoredRadius, cellWorldSize);
    CircleCellBounds result;
    result.firstX = clampCircleCellCoordinate(
        centerX - scanRadius, originX, cellWorldSize, width - 1);
    result.lastX = clampCircleCellCoordinate(
        centerX + scanRadius, originX, cellWorldSize, width - 1);
    result.firstY = clampCircleCellCoordinate(
        centerY - scanRadius, originY, cellWorldSize, height - 1);
    result.lastY = clampCircleCellCoordinate(
        centerY + scanRadius, originY, cellWorldSize, height - 1);
    result.radiusSquared = authoredRadius * authoredRadius;
    return result;
}

} // namespace

void MapVisibilityDirtyRegion::include(int32_t x, int32_t y) noexcept {
    if (x < 0 || y < 0) return;
    if (!isValid()) {
        minX = maxX = x;
        minY = maxY = y;
        return;
    }
    minX = std::min(minX, x);
    minY = std::min(minY, y);
    maxX = std::max(maxX, x);
    maxY = std::max(maxY, y);
}

void MapVisibilityDirtyRegion::includeAll(int32_t width, int32_t height) noexcept {
    if (width <= 0 || height <= 0) {
        *this = {};
        return;
    }
    minX = 0;
    minY = 0;
    maxX = width - 1;
    maxY = height - 1;
}

const MapVisibilityPlayerSnapshot* MapVisibilitySnapshot::player(
    engine::PlayerId id) const noexcept {
    const auto found = std::find_if(players.begin(), players.end(), [id](const auto& value) {
        return value.player == id;
    });
    return found == players.end() ? nullptr : &*found;
}

MapVisibilityCellState MapVisibilitySnapshot::cellState(engine::PlayerId playerId,
                                                         int32_t x, int32_t y) const noexcept {
    const MapVisibilityPlayerSnapshot* values = player(playerId);
    if (!values || x < 0 || y < 0 || x >= width || y >= height) {
        return MapVisibilityCellState::Shrouded;
    }
    const size_t offset = static_cast<size_t>(y) * static_cast<size_t>(width) +
                          static_cast<size_t>(x);
    return values->cells && offset < values->cells->size()
        ? (*values->cells)[offset]
        : MapVisibilityCellState::Shrouded;
}

bool MapVisibilitySnapshot::footprintHasClearCellRaw(
    engine::PlayerId playerId, int64_t centerXRaw,
    int64_t centerYRaw, int64_t radiusRaw) const noexcept {
    using Fixed = math::q32_32;
    if (radiusRaw < 0 || width <= 0 || height <= 0 ||
        cellWorldSizeRaw <= 0) {
        return false;
    }
    const Fixed cx = Fixed::from_raw(centerXRaw);
    const Fixed cy = Fixed::from_raw(centerYRaw);
    const Fixed radius = Fixed::from_raw(radiusRaw);
    const Fixed originXFixed = Fixed::from_raw(originXRaw);
    const Fixed originYFixed = Fixed::from_raw(originYRaw);
    const Fixed size = Fixed::from_raw(cellWorldSizeRaw);
    const auto floorInteger = [](Fixed value) noexcept {
        constexpr int64_t one = int64_t{1} << 32u;
        int64_t result = value.raw() / one;
        if (value.raw() < 0 && value.raw() % one != 0) --result;
        return result;
    };
    const int64_t minCellX = floorInteger((cx - radius - originXFixed) / size);
    const int64_t maxCellX = floorInteger((cx + radius - originXFixed) / size);
    const int64_t minCellY = floorInteger((cy - radius - originYFixed) / size);
    const int64_t maxCellY = floorInteger((cy + radius - originYFixed) / size);
    if (maxCellX < 0 || maxCellY < 0 || minCellX >= width ||
        minCellY >= height) {
        return false;
    }

    const int32_t firstX = static_cast<int32_t>(
        std::clamp<int64_t>(minCellX, 0, width - 1));
    const int32_t lastX = static_cast<int32_t>(
        std::clamp<int64_t>(maxCellX, 0, width - 1));
    const int32_t firstY = static_cast<int32_t>(
        std::clamp<int64_t>(minCellY, 0, height - 1));
    const int32_t lastY = static_cast<int32_t>(
        std::clamp<int64_t>(maxCellY, 0, height - 1));
    const Fixed radiusSquared = radius * radius;
    for (int32_t y = firstY; y <= lastY; ++y) {
        const Fixed cellMinY = originYFixed + size * Fixed{y};
        const Fixed nearestY = Fixed::clamp(cy, cellMinY, cellMinY + size);
        for (int32_t x = firstX; x <= lastX; ++x) {
            if (cellState(playerId, x, y) != MapVisibilityCellState::Clear) continue;
            const Fixed cellMinX = originXFixed + size * Fixed{x};
            const Fixed nearestX = Fixed::clamp(cx, cellMinX, cellMinX + size);
            const Fixed dx = cx - nearestX;
            const Fixed dy = cy - nearestY;
            if (dx * dx + dy * dy <= radiusSquared) return true;
        }
    }
    return false;
}

bool MapVisibilityAuthority::initialize(const TerrainMap& terrain,
                                        container::Span<const engine::PlayerId> players) {
    clear();
    if (!terrain.isLoaded() || terrain.width() < 2 || terrain.height() < 2) return false;

    const math::q32_32 terrainCellWorldSize{kMapCellWorldSize};
    const math::q32_32 terrainWidth =
        math::q32_32{terrain.width() - 1} * terrainCellWorldSize;
    const math::q32_32 terrainHeight =
        math::q32_32{terrain.height() - 1} * terrainCellWorldSize;
    const auto ceilCellCount = [](math::q32_32 extent,
                                  math::q32_32 cellSize) noexcept {
        if (extent <= math::q32_32{} || cellSize <= math::q32_32{}) {
            return int64_t{0};
        }
        const int64_t quotient = extent.raw() / cellSize.raw();
        return quotient + (extent.raw() % cellSize.raw() != 0 ? 1 : 0);
    };
    const int64_t visibilityWidth = ceilCellCount(
        terrainWidth, kMapVisibilityCellWorldSize);
    const int64_t visibilityHeight = ceilCellCount(
        terrainHeight, kMapVisibilityCellWorldSize);
    if (visibilityWidth <= 0 || visibilityHeight <= 0 ||
        visibilityWidth > std::numeric_limits<int32_t>::max() ||
        visibilityHeight > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    m_width = static_cast<int32_t>(visibilityWidth);
    m_height = static_cast<int32_t>(visibilityHeight);
    m_borderSize = terrain.borderSize();
    m_originX = -math::q32_32{terrain.borderSize()} * terrainCellWorldSize;
    m_originY = m_originX;
    m_cellWorldSize = kMapVisibilityCellWorldSize;
    m_terrainLayoutRevision = terrain.layoutRevision();
    const size_t cellCount = static_cast<size_t>(m_width) * static_cast<size_t>(m_height);
    if (cellCount == 0 || cellCount / static_cast<size_t>(m_width) !=
                            static_cast<size_t>(m_height)) {
        clear();
        return false;
    }

    for (const engine::PlayerId player : players) {
        if (!player || player.value >= engine::PLAYER_REGISTRY_CAPACITY ||
            m_gridByPlayer[player.value].has_value()) {
            continue;
        }
        const size_t slot = m_grids.size();
        PlayerGrid grid;
        grid.player = player;
        grid.cells.resize(cellCount);
        grid.nextDynamicLookerCounts.resize(cellCount);
        grid.nextDynamicShrouderCounts.resize(cellCount);
        m_grids.push_back(std::move(grid));
        m_gridByPlayer[player.value] = slot;
    }
    ++m_revision;
    if (m_revision == 0) ++m_revision;
    markFullDirty();
    rebuildSnapshot();
    return true;
}

void MapVisibilityAuthority::clear() noexcept {
    m_width = 0;
    m_height = 0;
    m_borderSize = 0;
    m_originX = math::q32_32{};
    m_originY = math::q32_32{};
    m_cellWorldSize = kMapVisibilityCellWorldSize;
    m_terrainLayoutRevision = 0;
    m_revision = 0;
    m_renderingActive = false;
    m_shroudEnabled = true;
    m_dynamicLookerGeneration = 0;
    m_lastVisualTransitionTick = UINT64_MAX;
    m_pendingDirty = {};
    m_gridByPlayer.fill(std::nullopt);
    m_grids.clear();
    m_namedPermanentReveals.clear();
    m_activeDynamicLookers.clear();
    m_pendingDynamicUnlooks.clear();
    m_snapshot.reset();
}

bool MapVisibilityAuthority::updateDynamicLookers(
    container::Span<const MapVisibilityDynamicLooker> lookers,
    container::Span<const MapVisibilityDynamicShrouder> shrouders,
    uint64_t confirmedTick,
    uint32_t unlookPersistenceTicks,
    uint32_t fogTransitionTicks) {
    if (!isInitialized()) return false;

    for (PlayerGrid& values : m_grids) {
        if (values.nextDynamicLookerCounts.size() != values.cells.size()) {
            values.nextDynamicLookerCounts.assign(values.cells.size(), uint16_t{0});
        } else {
            std::fill(values.nextDynamicLookerCounts.begin(),
                      values.nextDynamicLookerCounts.end(), uint16_t{0});
        }
        if (values.nextDynamicShrouderCounts.size() !=
                values.cells.size()) {
            values.nextDynamicShrouderCounts.assign(
                values.cells.size(), uint16_t{0});
        } else {
            std::fill(values.nextDynamicShrouderCounts.begin(),
                      values.nextDynamicShrouderCounts.end(), uint16_t{0});
        }
    }

    ++m_dynamicLookerGeneration;
    if (m_dynamicLookerGeneration == 0) ++m_dynamicLookerGeneration;
    for (const MapVisibilityDynamicLooker& looker : lookers) {
        PlayerGrid* values = grid(looker.player);
        if (!values || looker.radius <= math::q32_32{}) {
            continue;
        }
        if (looker.identity != 0) {
            const auto [found, inserted] = m_activeDynamicLookers.try_emplace(
                looker.identity,
                ActiveDynamicLooker{.value = looker,
                                    .seenGeneration = m_dynamicLookerGeneration});
            ActiveDynamicLooker& active = found->second;
            if (!inserted && !sameLookFootprint(active.value, looker)) {
                queueDynamicUnlook(active.value, confirmedTick,
                                   unlookPersistenceTicks);
                active.value = looker;
            }
            active.seenGeneration = m_dynamicLookerGeneration;
        }
        accumulateDynamicLooker(
            *values, looker.x, looker.y, looker.radius);
    }
    for (auto active = m_activeDynamicLookers.begin();
         active != m_activeDynamicLookers.end();) {
        if (active->second.seenGeneration == m_dynamicLookerGeneration) {
            ++active;
            continue;
        }
        queueDynamicUnlook(active->second.value, confirmedTick,
                           unlookPersistenceTicks);
        active = m_activeDynamicLookers.erase(active);
    }
    for (auto pending = m_pendingDynamicUnlooks.begin();
         pending != m_pendingDynamicUnlooks.end();) {
        if (pending->expiresAfterTick < confirmedTick) {
            pending = m_pendingDynamicUnlooks.erase(pending);
            continue;
        }
        if (PlayerGrid* values = grid(pending->value.player)) {
            accumulateDynamicLooker(
                *values, pending->value.x, pending->value.y,
                pending->value.radius);
        }
        ++pending;
    }
    for (const MapVisibilityDynamicShrouder& shrouder : shrouders) {
        PlayerGrid* values = grid(shrouder.player);
        if (!values || shrouder.radius <= math::q32_32{}) {
            continue;
        }
        accumulateDynamicShrouder(
            *values, shrouder.x, shrouder.y, shrouder.radius);
    }

    bool changed = false;
    for (PlayerGrid& values : m_grids) {
        bool playerChanged = false;
        for (size_t cellIndex = 0; cellIndex < values.cells.size(); ++cellIndex) {
            Cell& cell = values.cells[cellIndex];
            const MapVisibilityCellState before = stateFor(cell);
            cell.dynamicLookerCount = values.nextDynamicLookerCounts[cellIndex];
            cell.dynamicShrouderCount =
                values.nextDynamicShrouderCounts[cellIndex];
            if (cell.dynamicLookerCount != 0) cell.explored = true;
            if (before == stateFor(cell)) continue;

            const int32_t y = static_cast<int32_t>(
                cellIndex / static_cast<size_t>(m_width));
            const int32_t x = static_cast<int32_t>(
                cellIndex - static_cast<size_t>(y) * static_cast<size_t>(m_width));
            m_pendingDirty.include(x, y);
            playerChanged = true;
            changed = true;
        }
        if (playerChanged) values.snapshotDirty = true;
    }

    // A normal participant match must fail closed even when its first frame
    // contains no sight-bearing objects. Activating the rendering contract is
    // itself a snapshot change and therefore publishes one full dirty region;
    // subsequent counter-only churn remains revision-free.
    const bool activated = activateRendering();
    const bool visualChanged = advanceVisualLevels(
        confirmedTick, fogTransitionTicks);
    if (changed || activated || visualChanged) markChanged();
    return changed || visualChanged;
}

bool MapVisibilityAuthority::setShroudEnabled(bool enabled) noexcept {
    if (!isInitialized() || m_shroudEnabled == enabled) return false;
    m_shroudEnabled = enabled;
    markFullDirty();
    for (PlayerGrid& values : m_grids) values.snapshotDirty = true;
    markChanged();
    return true;
}

bool MapVisibilityAuthority::refresh() noexcept {
    if (!isInitialized()) return false;
    markFullDirty();
    markChanged();
    return true;
}

bool MapVisibilityAuthority::revealCircle(engine::PlayerId player,
                                           math::q32_32 centerX,
                                           math::q32_32 centerY,
                                           math::q32_32 radius) noexcept {
    PlayerGrid* values = grid(player);
    if (!values) return false;
    const bool activated = activateRendering();
    const bool changed = applyCircle(
        *values, centerX, centerY, radius, true);
    if (changed || activated) markChanged();
    return changed;
}

bool MapVisibilityAuthority::shroudCircle(engine::PlayerId player,
                                           math::q32_32 centerX,
                                           math::q32_32 centerY,
                                           math::q32_32 radius) noexcept {
    PlayerGrid* values = grid(player);
    if (!values) return false;
    const bool activated = activateRendering();
    const bool changed = applyCircle(
        *values, centerX, centerY, radius, false);
    if (changed || activated) markChanged();
    return changed;
}

bool MapVisibilityAuthority::revealAll(engine::PlayerId player) noexcept {
    PlayerGrid* values = grid(player);
    if (!values) return false;
    const bool activated = activateRendering();
    const bool changed = applyAll(*values, true, false);
    if (changed || activated) markChanged();
    return changed;
}

bool MapVisibilityAuthority::shroudAll(engine::PlayerId player) noexcept {
    PlayerGrid* values = grid(player);
    if (!values) return false;
    const bool activated = activateRendering();
    const bool changed = applyAll(*values, false, false);
    if (changed || activated) markChanged();
    return changed;
}

bool MapVisibilityAuthority::revealAllPermanently(engine::PlayerId player) noexcept {
    PlayerGrid* values = grid(player);
    if (!values) return false;
    const bool activated = activateRendering();
    const bool changed = applyAll(*values, true, true);
    if (changed || activated) markChanged();
    return changed;
}

bool MapVisibilityAuthority::undoRevealAllPermanently(engine::PlayerId player) noexcept {
    PlayerGrid* values = grid(player);
    if (!values) return false;
    const bool activated = activateRendering();
    const bool changed = applyAll(*values, false, true);
    if (changed || activated) markChanged();
    return changed;
}

bool MapVisibilityAuthority::createNamedPermanentReveal(container::String name,
                                                         engine::PlayerId player,
                                                         math::q32_32 centerX,
                                                         math::q32_32 centerY,
                                                         math::q32_32 radius) {
    if (name.empty() || !grid(player)) {
        return false;
    }
    const auto existing = std::find_if(m_namedPermanentReveals.begin(),
                                       m_namedPermanentReveals.end(),
                                       [name](const auto& reveal) { return reveal.name == name; });
    if (existing != m_namedPermanentReveals.end()) {
        // ScriptEngine emits a DEBUG_CRASH and refuses to redefine this
        // record. Treat the malformed duplicate as an inert no-op rather
        // than preserving the old follow-up doNamedMapReveal bug, which can
        // leave an extra looker with no removable name.
        return false;
    }
    m_namedPermanentReveals.push_back({
        .name = std::move(name),
        .player = player,
        .centerX = centerX,
        .centerY = centerY,
        .radius = radius,
    });
    PlayerGrid* values = grid(player);
    const bool activated = activateRendering();
    const bool changed = values && applyPermanentCircle(
        *values, centerX, centerY, radius, true);
    if (changed || activated) markChanged();
    return changed;
}

bool MapVisibilityAuthority::undoNamedPermanentReveal(container::StringView name) noexcept {
    const auto found = std::find_if(m_namedPermanentReveals.begin(),
                                    m_namedPermanentReveals.end(),
                                    [name](const auto& reveal) { return reveal.name == name; });
    if (found == m_namedPermanentReveals.end()) return false;
    PlayerGrid* values = grid(found->player);
    const bool activated = activateRendering();
    const bool changed = values && applyPermanentCircle(
        *values, found->centerX, found->centerY, found->radius, false);
    // ScriptEngine removes the named record after undo regardless of whether
    // its player/waypoint was valid by then.
    m_namedPermanentReveals.erase(found);
    if (changed || activated) markChanged();
    return changed;
}

MapVisibilityAuthority::PlayerGrid* MapVisibilityAuthority::grid(engine::PlayerId player) noexcept {
    if (!player || player.value >= m_gridByPlayer.size()) return nullptr;
    const auto slot = m_gridByPlayer[player.value];
    return slot && *slot < m_grids.size() ? &m_grids[*slot] : nullptr;
}

const MapVisibilityAuthority::PlayerGrid* MapVisibilityAuthority::grid(
    engine::PlayerId player) const noexcept {
    if (!player || player.value >= m_gridByPlayer.size()) return nullptr;
    const auto slot = m_gridByPlayer[player.value];
    return slot && *slot < m_grids.size() ? &m_grids[*slot] : nullptr;
}

size_t MapVisibilityAuthority::index(int32_t x, int32_t y) const noexcept {
    return static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x);
}

bool MapVisibilityAuthority::applyCircle(PlayerGrid& values,
                                          math::q32_32 centerX,
                                          math::q32_32 centerY,
                                          math::q32_32 radius,
                                          bool reveal) noexcept {
    return mutateCircle(values, centerX, centerY, radius,
                        reveal ? CircleOperation::RevealPulse : CircleOperation::ShroudPulse);
}

bool MapVisibilityAuthority::applyAll(PlayerGrid& values, bool reveal, bool permanent) noexcept {
    bool changed = false;
    for (Cell& cell : values.cells) {
        const MapVisibilityCellState before = stateFor(cell);
        if (permanent) {
            if (reveal) addLooker(cell);
            else removeLooker(cell);
        } else if (reveal) {
            addLooker(cell);
            removeLooker(cell);
            cell.explored = true;
        } else {
            addShrouder(cell);
            removeShrouder(cell);
            cell.explored = false;
        }
        changed = changed || before != stateFor(cell);
    }
    if (changed) {
        values.snapshotDirty = true;
        markFullDirty();
    }
    return changed;
}

bool MapVisibilityAuthority::applyPermanentCircle(PlayerGrid& values,
                                                   math::q32_32 centerX,
                                                   math::q32_32 centerY,
                                                   math::q32_32 radius,
                                                   bool add) noexcept {
    return mutateCircle(values, centerX, centerY, radius,
                        add ? CircleOperation::AddLooker : CircleOperation::RemoveLooker);
}

bool MapVisibilityAuthority::mutateCircle(PlayerGrid& values,
                                           math::q32_32 centerX,
                                           math::q32_32 centerY,
                                           math::q32_32 radius,
                                           CircleOperation operation) noexcept {
    if (!isInitialized()) return false;
    const CircleCellBounds bounds = makeCircleCellBounds(
        centerX, centerY, radius, m_width, m_height, m_originX, m_originY,
        m_cellWorldSize);
    bool changed = false;
    for (int32_t y = bounds.firstY; y <= bounds.lastY; ++y) {
        const math::q32_32 minimumY =
            m_originY + math::q32_32{y} * m_cellWorldSize;
        const math::q32_32 maximumY = minimumY + m_cellWorldSize;
        const math::q32_32 nearestY = math::q32_32::clamp(
            centerY, minimumY, maximumY);
        for (int32_t x = bounds.firstX; x <= bounds.lastX; ++x) {
            const math::q32_32 minimumX =
                m_originX + math::q32_32{x} * m_cellWorldSize;
            const math::q32_32 maximumX = minimumX + m_cellWorldSize;
            const math::q32_32 nearestX = math::q32_32::clamp(
                centerX, minimumX, maximumX);
            const math::q32_32 dx = centerX - nearestX;
            const math::q32_32 dy = centerY - nearestY;
            if (dx * dx + dy * dy > bounds.radiusSquared) continue;
            Cell& cell = values.cells[index(x, y)];
            const MapVisibilityCellState before = stateFor(cell);
            switch (operation) {
            case CircleOperation::RevealPulse:
                addLooker(cell);
                removeLooker(cell);
                cell.explored = true;
                break;
            case CircleOperation::ShroudPulse:
                addShrouder(cell);
                removeShrouder(cell);
                cell.explored = false;
                break;
            case CircleOperation::AddLooker:
                addLooker(cell);
                break;
            case CircleOperation::RemoveLooker:
                removeLooker(cell);
                break;
            }
            if (before != stateFor(cell)) {
                changed = true;
                m_pendingDirty.include(x, y);
            }
        }
    }
    if (changed) values.snapshotDirty = true;
    return changed;
}

void MapVisibilityAuthority::accumulateDynamicLooker(PlayerGrid& values,
                                                       math::q32_32 centerX,
                                                       math::q32_32 centerY,
                                                       math::q32_32 radius) noexcept {
    const CircleCellBounds bounds = makeCircleCellBounds(
        centerX, centerY, radius, m_width, m_height, m_originX, m_originY,
        m_cellWorldSize);
    for (int32_t y = bounds.firstY; y <= bounds.lastY; ++y) {
        const math::q32_32 minimumY =
            m_originY + math::q32_32{y} * m_cellWorldSize;
        const math::q32_32 maximumY = minimumY + m_cellWorldSize;
        const math::q32_32 nearestY = math::q32_32::clamp(
            centerY, minimumY, maximumY);
        for (int32_t x = bounds.firstX; x <= bounds.lastX; ++x) {
            const math::q32_32 minimumX =
                m_originX + math::q32_32{x} * m_cellWorldSize;
            const math::q32_32 maximumX = minimumX + m_cellWorldSize;
            const math::q32_32 nearestX = math::q32_32::clamp(
                centerX, minimumX, maximumX);
            const math::q32_32 dx = centerX - nearestX;
            const math::q32_32 dy = centerY - nearestY;
            if (dx * dx + dy * dy > bounds.radiusSquared) continue;

            uint16_t& count = values.nextDynamicLookerCounts[index(x, y)];
            if (count != std::numeric_limits<uint16_t>::max()) ++count;
        }
    }
}

void MapVisibilityAuthority::accumulateDynamicShrouder(
    PlayerGrid& values, math::q32_32 centerX, math::q32_32 centerY,
    math::q32_32 radius) noexcept {
    const CircleCellBounds bounds = makeCircleCellBounds(
        centerX, centerY, radius, m_width, m_height, m_originX, m_originY,
        m_cellWorldSize);
    for (int32_t y = bounds.firstY; y <= bounds.lastY; ++y) {
        const math::q32_32 minimumY =
            m_originY + math::q32_32{y} * m_cellWorldSize;
        const math::q32_32 maximumY = minimumY + m_cellWorldSize;
        const math::q32_32 nearestY = math::q32_32::clamp(
            centerY, minimumY, maximumY);
        for (int32_t x = bounds.firstX; x <= bounds.lastX; ++x) {
            const math::q32_32 minimumX =
                m_originX + math::q32_32{x} * m_cellWorldSize;
            const math::q32_32 maximumX = minimumX + m_cellWorldSize;
            const math::q32_32 nearestX = math::q32_32::clamp(
                centerX, minimumX, maximumX);
            const math::q32_32 dx = centerX - nearestX;
            const math::q32_32 dy = centerY - nearestY;
            if (dx * dx + dy * dy > bounds.radiusSquared) continue;

            uint16_t& count =
                values.nextDynamicShrouderCounts[index(x, y)];
            if (count != std::numeric_limits<uint16_t>::max()) ++count;
        }
    }
}

bool MapVisibilityAuthority::sameLookFootprint(
    const MapVisibilityDynamicLooker& left,
    const MapVisibilityDynamicLooker& right) const noexcept {
    if (left.player != right.player ||
        m_cellWorldSize <= math::q32_32{}) {
        return false;
    }
    const auto cellCoordinate = [this](math::q32_32 value,
                                        math::q32_32 origin) noexcept {
        return floorFixedRatio(value - origin, m_cellWorldSize);
    };
    const auto cellRadius = [this](math::q32_32 value) noexcept {
        return std::max<int64_t>(
            1, floorFixedRatio(value, m_cellWorldSize));
    };
    return cellCoordinate(left.x, m_originX) ==
               cellCoordinate(right.x, m_originX) &&
           cellCoordinate(left.y, m_originY) ==
               cellCoordinate(right.y, m_originY) &&
           cellRadius(left.radius) == cellRadius(right.radius);
}

void MapVisibilityAuthority::queueDynamicUnlook(
    const MapVisibilityDynamicLooker& looker,
    uint64_t confirmedTick,
    uint32_t persistenceTicks) {
    if (persistenceTicks == 0) return;
    const uint64_t maximumTick = std::numeric_limits<uint64_t>::max();
    const uint64_t expiresAfterTick = confirmedTick > maximumTick - persistenceTicks
        ? maximumTick
        : confirmedTick + persistenceTicks;
    m_pendingDynamicUnlooks.push_back({
        .value = looker,
        .expiresAfterTick = expiresAfterTick,
    });
}

bool MapVisibilityAuthority::advanceVisualLevels(
    uint64_t confirmedTick, uint32_t transitionTicks) noexcept {
    if (m_lastVisualTransitionTick == confirmedTick) return false;
    uint64_t elapsedTicks = 1;
    if (m_lastVisualTransitionTick != UINT64_MAX &&
        confirmedTick > m_lastVisualTransitionTick) {
        elapsedTicks = confirmedTick - m_lastVisualTransitionTick;
    }
    m_lastVisualTransitionTick = confirmedTick;
    const uint64_t perTickStep = transitionTicks == 0
        ? 255u : std::max<uint64_t>(1u, (255u + transitionTicks - 1u) /
                                           transitionTicks);
    const uint8_t maximumStep = static_cast<uint8_t>(std::min<uint64_t>(
        255u, perTickStep * std::min<uint64_t>(elapsedTicks, 255u)));
    bool changed = false;
    for (PlayerGrid& values : m_grids) {
        bool playerChanged = false;
        for (size_t cellIndex = 0; cellIndex < values.cells.size(); ++cellIndex) {
            Cell& cell = values.cells[cellIndex];
            const MapVisibilityCellState state = m_shroudEnabled
                ? stateFor(cell) : MapVisibilityCellState::Clear;
            const uint8_t target = state == MapVisibilityCellState::Clear
                ? uint8_t{255}
                : (state == MapVisibilityCellState::Fogged
                    ? uint8_t{127} : uint8_t{0});
            if (cell.visualLevel == target) continue;
            if (cell.visualLevel < target) {
                cell.visualLevel = static_cast<uint8_t>(std::min<uint32_t>(
                    target, static_cast<uint32_t>(cell.visualLevel) +
                                maximumStep));
            } else {
                cell.visualLevel = static_cast<uint8_t>(std::max<int32_t>(
                    target, static_cast<int32_t>(cell.visualLevel) -
                                maximumStep));
            }
            const int32_t y = static_cast<int32_t>(
                cellIndex / static_cast<size_t>(m_width));
            const int32_t x = static_cast<int32_t>(
                cellIndex - static_cast<size_t>(y) *
                                    static_cast<size_t>(m_width));
            m_pendingDirty.include(x, y);
            playerChanged = true;
            changed = true;
        }
        if (playerChanged) values.snapshotDirty = true;
    }
    return changed;
}

void MapVisibilityAuthority::addLooker(Cell& cell) noexcept {
    // RefCode: current = min(current - 1, -1). Saturation avoids signed
    // overflow in malformed maps while retaining every practical legacy path.
    cell.currentShroud = std::min(decrementSaturated(cell.currentShroud), int16_t{-1});
}

void MapVisibilityAuthority::removeLooker(Cell& cell) noexcept {
    // RefCode asserts if this path is called while not looked-at, then still
    // increments. Preserve that useful malformed-content behavior safely.
    if (cell.currentShroud == -1) {
        cell.currentShroud = std::min(cell.activeShroud, int16_t{1});
    } else {
        cell.currentShroud = incrementSaturated(cell.currentShroud);
    }
}

void MapVisibilityAuthority::addShrouder(Cell& cell) noexcept {
    cell.activeShroud = incrementSaturated(cell.activeShroud);
    if (cell.currentShroud == 0) cell.currentShroud = 1;
}

void MapVisibilityAuthority::removeShrouder(Cell& cell) noexcept {
    if (cell.activeShroud > std::numeric_limits<int16_t>::min()) --cell.activeShroud;
}

MapVisibilityCellState MapVisibilityAuthority::stateFor(Cell cell) noexcept {
    if (cell.dynamicLookerCount != 0 || cell.currentShroud < 0) {
        return MapVisibilityCellState::Clear;
    }
    if (cell.dynamicShrouderCount != 0) {
        return MapVisibilityCellState::Shrouded;
    }
    return cell.explored || cell.currentShroud == 0
        ? MapVisibilityCellState::Fogged
        : MapVisibilityCellState::Shrouded;
}

bool MapVisibilityAuthority::activateRendering() noexcept {
    if (m_renderingActive) return false;
    m_renderingActive = true;
    markFullDirty();
    return true;
}

void MapVisibilityAuthority::markFullDirty() noexcept {
    m_pendingDirty.includeAll(m_width, m_height);
}

void MapVisibilityAuthority::markChanged() noexcept {
    if (!m_pendingDirty.isValid()) markFullDirty();
    ++m_revision;
    if (m_revision == 0) ++m_revision;
    rebuildSnapshot();
}

void MapVisibilityAuthority::rebuildSnapshot() {
    auto snapshot = std::make_shared<MapVisibilitySnapshot>();
    snapshot->revision = m_revision;
    snapshot->terrainLayoutRevision = m_terrainLayoutRevision;
    snapshot->width = m_width;
    snapshot->height = m_height;
    snapshot->borderSize = m_borderSize;
    snapshot->originX = m_originX.to_float();
    snapshot->originY = m_originY.to_float();
    snapshot->cellWorldSize = m_cellWorldSize.to_float();
    snapshot->originXRaw = m_originX.raw();
    snapshot->originYRaw = m_originY.raw();
    snapshot->cellWorldSizeRaw = m_cellWorldSize.raw();
    snapshot->renderingActive = m_shroudEnabled && m_renderingActive;
    snapshot->dirtyRegion = m_pendingDirty;
    snapshot->players.reserve(m_grids.size());
    for (PlayerGrid& grid : m_grids) {
        if (grid.snapshotDirty || !grid.snapshotCells) {
            auto cells = std::make_shared<container::Vector<MapVisibilityCellState>>();
            auto visualLevels = std::make_shared<container::Vector<uint8_t>>();
            cells->reserve(grid.cells.size());
            visualLevels->reserve(grid.cells.size());
            for (const Cell cell : grid.cells) {
                cells->push_back(m_shroudEnabled
                    ? stateFor(cell) : MapVisibilityCellState::Clear);
                visualLevels->push_back(cell.visualLevel);
            }
            grid.snapshotCells = std::move(cells);
            grid.snapshotVisualLevels = std::move(visualLevels);
            grid.snapshotRevision = m_revision;
            grid.snapshotDirty = false;
        }
        MapVisibilityPlayerSnapshot values;
        values.player = grid.player;
        values.revision = grid.snapshotRevision;
        values.cells = grid.snapshotCells;
        values.visualLevels = grid.snapshotVisualLevels;
        snapshot->players.push_back(std::move(values));
    }
    m_snapshot = std::move(snapshot);
    m_pendingDirty = {};
}

} // namespace game::terrain
