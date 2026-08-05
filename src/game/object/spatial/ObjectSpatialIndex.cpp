#include "core/container/container_types.h"
#include "ObjectSpatialIndex.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace engine {
namespace {

constexpr int64_t kMaximumIndexedCellsPerObject = 4096;

[[nodiscard]] bool exceedsCellBudget(int64_t countX, int64_t countY) noexcept {
    if (countX <= 0 || countY <= 0 ||
        countX > kMaximumIndexedCellsPerObject || countY > kMaximumIndexedCellsPerObject) {
        return true;
    }
    // Do not multiply untrusted coordinate spans: an INT32_MIN..INT32_MAX
    // query is valid input to this defensive API, but its product overflows a
    // signed 64-bit integer before the old comparison could reject it.
    return countX > kMaximumIndexedCellsPerObject / countY;
}

} // namespace

void ObjectSpatialIndex::clear() noexcept {
    m_cells.clear();
    m_records.clear();
    m_oversized.clear();
    ++m_revision;
    if (m_revision == 0) ++m_revision;
}

bool ObjectSpatialIndex::setCellSize(math::q32_32 value) noexcept {
    if (value <= math::q32_32{}) return false;
    if (value == m_cellSizeFixed) return true;
    m_cellSizeFixed = value;
    clear();
    return true;
}

int32_t ObjectSpatialIndex::cellCoordinate(math::q32_32 value) const noexcept {
    const int64_t divisor = m_cellSizeFixed.raw();
    if (divisor <= 0) return 0;
    int64_t quotient = value.raw() / divisor;
    const int64_t remainder = value.raw() % divisor;
    if (value.raw() < 0 && remainder != 0) --quotient;
    if (quotient <= static_cast<int64_t>(
            std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }
    if (quotient >= static_cast<int64_t>(
            std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(quotient);
}

uint64_t ObjectSpatialIndex::cellKey(int32_t x, int32_t y) noexcept {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32u) |
           static_cast<uint32_t>(y);
}

bool ObjectSpatialIndex::equivalent(
    const IndexedRecord& left, const IndexedRecord& right) noexcept {
    return left.value.object == right.value.object &&
        left.value.position.x == right.value.position.x &&
        left.value.position.y == right.value.position.y &&
        left.value.position.z == right.value.position.z &&
        left.value.radius == right.value.radius &&
        left.value.sphereRadius == right.value.sphereRadius &&
        left.radiusUnbounded == right.radiusUnbounded &&
        left.sphereRadiusUnbounded == right.sphereRadiusUnbounded;
}

std::optional<ObjectSpatialIndex::IndexedRecord>
ObjectSpatialIndex::projectRecord(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity) const {
    const ObjectIdentityComponent* identity =
        ecs::try_get<ObjectIdentityComponent>(registry, entity);
    if (!identity || !identity->id ||
        !lifecycle.entityFromId(identity->id)) {
        return std::nullopt;
    }
    if (const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(registry, entity);
        contained && contained->enclosing) {
        return std::nullopt;
    }
    if (const ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(registry, entity);
        mapStatus && mapStatus->offMap) {
        return std::nullopt;
    }
    if (const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        health && health->terminalDeathIssued) {
        return std::nullopt;
    }
    const ObjectFixedTransformComponent* transform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, entity);
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, entity);
    if (!transform || !transform->authoritative || !geometry) {
        return std::nullopt;
    }
    return IndexedRecord{
        .value = {
            .object = identity->id,
            .position = transform->position,
            .radius = math::q32_32::max(
                math::q32_32{}, geometry->boundingCircleRadiusFixed),
            .sphereRadius = math::q32_32::max(
                math::q32_32{}, geometry->boundingSphereRadiusFixed),
        },
    };
}

void ObjectSpatialIndex::eraseRecord(ObjectId object) {
    const auto found = m_records.find(object);
    if (found == m_records.end()) return;
    const IndexedRecord record = found->second;
    if (record.indexedAsOversized) {
        const auto oversized = std::lower_bound(
            m_oversized.begin(), m_oversized.end(), object);
        if (oversized != m_oversized.end() && *oversized == object) {
            m_oversized.erase(oversized);
        }
    } else {
        const math::q32_32 insertionRadius = math::q32_32::max(
            record.value.radius, record.value.sphereRadius);
        const int32_t minimumX = cellCoordinate(
            record.value.position.x - insertionRadius);
        const int32_t maximumX = cellCoordinate(
            record.value.position.x + insertionRadius);
        const int32_t minimumY = cellCoordinate(
            record.value.position.y - insertionRadius);
        const int32_t maximumY = cellCoordinate(
            record.value.position.y + insertionRadius);
        for (int32_t y = minimumY; y <= maximumY; ++y) {
            for (int32_t x = minimumX; x <= maximumX; ++x) {
                const auto cell = m_cells.find(cellKey(x, y));
                if (cell != m_cells.end()) {
                    const auto member = std::lower_bound(
                        cell->second.begin(), cell->second.end(), object);
                    if (member != cell->second.end() && *member == object) {
                        cell->second.erase(member);
                    }
                    if (cell->second.empty()) m_cells.erase(cell);
                }
                if (x == std::numeric_limits<int32_t>::max()) break;
            }
            if (y == std::numeric_limits<int32_t>::max()) break;
        }
    }
    m_records.erase(found);
}

void ObjectSpatialIndex::insertRecord(IndexedRecord record) {
    const math::q32_32 insertionRadius = math::q32_32::max(
        record.value.radius, record.value.sphereRadius);
    const int32_t minimumX = cellCoordinate(
        record.value.position.x - insertionRadius);
    const int32_t maximumX = cellCoordinate(
        record.value.position.x + insertionRadius);
    const int32_t minimumY = cellCoordinate(
        record.value.position.y - insertionRadius);
    const int32_t maximumY = cellCoordinate(
        record.value.position.y + insertionRadius);
    const int64_t cellCountX = static_cast<int64_t>(maximumX) -
        static_cast<int64_t>(minimumX) + 1;
    const int64_t cellCountY = static_cast<int64_t>(maximumY) -
        static_cast<int64_t>(minimumY) + 1;
    record.indexedAsOversized = record.radiusUnbounded ||
        record.sphereRadiusUnbounded ||
        exceedsCellBudget(cellCountX, cellCountY);
    const ObjectId object = record.value.object;
    m_records.emplace(object, record);
    if (record.indexedAsOversized) {
        m_oversized.insert(
            std::lower_bound(m_oversized.begin(), m_oversized.end(), object),
            object);
        return;
    }
    for (int32_t y = minimumY; y <= maximumY; ++y) {
        for (int32_t x = minimumX; x <= maximumX; ++x) {
            container::Vector<ObjectId>& cell = m_cells[cellKey(x, y)];
            cell.insert(std::lower_bound(cell.begin(), cell.end(), object),
                        object);
            if (x == std::numeric_limits<int32_t>::max()) break;
        }
        if (y == std::numeric_limits<int32_t>::max()) break;
    }
}

bool ObjectSpatialIndex::refreshRecord(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity) {
    const ObjectIdentityComponent* identity =
        ecs::try_get<ObjectIdentityComponent>(registry, entity);
    if (!identity || !identity->id) return false;
    const ObjectId object = identity->id;
    const std::optional<IndexedRecord> projected =
        projectRecord(registry, lifecycle, entity);
    const auto current = m_records.find(object);
    if (!projected) {
        if (current == m_records.end()) return false;
        eraseRecord(object);
        return true;
    }
    if (current != m_records.end() &&
        equivalent(current->second, *projected)) {
        return false;
    }
    if (current != m_records.end()) eraseRecord(object);
    insertRecord(*projected);
    return true;
}

void ObjectSpatialIndex::rebuild(const ecs::registry& registry, const ObjectLifecycle& lifecycle) {
    container::Vector<IndexedRecord>& ordered = m_rebuildScratch;
    ordered.clear();
    auto view = ecs::view<ObjectIdentityComponent,
                          ObjectFixedTransformComponent,
                          ObjectGeometryComponent>(registry);
    if (ordered.capacity() < view.size_hint()) ordered.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity = ecs::get<ObjectIdentityComponent>(registry, entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id)) continue;
        if (const ObjectContainedByComponent* contained =
                ecs::try_get<ObjectContainedByComponent>(registry, entity);
            contained && contained->enclosing) {
            continue;
        }
        if (const ObjectMapStatusComponent* mapStatus =
                ecs::try_get<ObjectMapStatusComponent>(registry, entity);
            mapStatus && mapStatus->offMap) {
            continue;
        }
        // Object::onDie performs partition maintenance even when
        // KeepObjectDie/SlowDeath leaves the Object allocation alive. A
        // retained rubble entity remains renderable and script-addressable,
        // but must not stay in the combat/selection broad phase as a live
        // target. InactiveBody scenery starts `effectivelyDead` by design,
        // yet never entered a death transaction and may still be legitimate
        // static geometry, so use terminalDeathIssued rather than HP alone.
        if (const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(registry, entity);
            health && health->terminalDeathIssued) {
            continue;
        }
        const ObjectFixedTransformComponent& transform =
            ecs::get<ObjectFixedTransformComponent>(registry, entity);
        const ObjectGeometryComponent& geometry = ecs::get<ObjectGeometryComponent>(registry, entity);
        if (!transform.authoritative) continue;
        const LogicFixedVec3 position = transform.position;
        const math::q32_32 radius = math::q32_32::max(
            math::q32_32{}, geometry.boundingCircleRadiusFixed);
        const math::q32_32 sphereRadius = math::q32_32::max(
            math::q32_32{}, geometry.boundingSphereRadiusFixed);
        ordered.push_back({
            .value = {
                .object = identity.id,
                .position = position,
                .radius = radius,
                .sphereRadius = sphereRadius,
            },
            .radiusUnbounded = false,
            .sphereRadiusUnbounded = false,
        });
    }
    std::sort(ordered.begin(), ordered.end(), [](const IndexedRecord& lhs,
                                                  const IndexedRecord& rhs) {
        return lhs.value.object < rhs.value.object;
    });

    // Several confirmed-frame barriers request a fresh broad phase so their
    // following consumers see same-tick creates, deaths and movement. Most
    // barriers do not actually change spatial state. Preserve that semantic
    // contract while avoiding a second cell-map teardown/repopulation when
    // the canonical source projection is byte-for-value identical.
    bool unchanged = ordered.size() == m_records.size();
    if (unchanged) {
        for (const IndexedRecord& candidate : ordered) {
            const auto current = m_records.find(candidate.value.object);
            if (current == m_records.end() ||
                !equivalent(candidate, current->second)) {
                unchanged = false;
                break;
            }
        }
    }
    if (unchanged) return;

    m_cells.clear();
    m_records.clear();
    m_oversized.clear();

    for (IndexedRecord record : ordered) insertRecord(std::move(record));
    ++m_revision;
    if (m_revision == 0) ++m_revision;
}

void ObjectSpatialIndex::refreshDirty(
    ecs::registry& registry, const ObjectLifecycle& lifecycle) {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    container::Vector<ecs::entity> staleDirty;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectDirtyComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectDirtyComponent& dirty =
            view.template get<ObjectDirtyComponent>(entity);
        if ((dirty.domains & objectDirtyBit(ObjectDirtyDomain::Spatial)) == 0)
            continue;
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id) {
            staleDirty.push_back(entity);
            continue;
        }
        candidates.push_back({identity.id, entity});
    }
    for (const ecs::entity entity : staleDirty) {
        clearObjectDirty(registry, entity, ObjectDirtyDomain::Spatial);
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });
    bool changed = false;
    for (const Candidate& candidate : candidates) {
        changed = refreshRecord(registry, lifecycle, candidate.entity) ||
            changed;
        clearObjectDirty(
            registry, candidate.entity, ObjectDirtyDomain::Spatial);
    }
    if (changed) {
        ++m_revision;
        if (m_revision == 0) ++m_revision;
    }
}

void ObjectSpatialIndex::pruneMissing(const ObjectLifecycle& lifecycle) {
    container::Vector<ObjectId> missing;
    missing.reserve(m_records.size());
    for (const auto& [object, record] : m_records) {
        static_cast<void>(record);
        if (!lifecycle.entityFromIdIncludingPending(object)) {
            missing.push_back(object);
        }
    }
    if (missing.empty()) return;
    std::sort(missing.begin(), missing.end());
    for (const ObjectId object : missing) eraseRecord(object);
    ++m_revision;
    if (m_revision == 0) ++m_revision;
}

container::Vector<ObjectId> ObjectSpatialIndex::queryRadiusFixed(
    const LogicFixedVec3& center, math::q32_32 radius) const {
    container::Vector<ObjectId> result;
    queryRadiusFixed(center, radius, result);
    return result;
}

void ObjectSpatialIndex::queryRadiusFixed(
    const LogicFixedVec3& center, math::q32_32 radius,
    container::Vector<ObjectId>& out) const {
    queryRadiusImplFixed(center, radius, false, out);
}

container::Vector<ObjectId> ObjectSpatialIndex::querySphereRadiusFixed(
    const LogicFixedVec3& center, math::q32_32 radius) const {
    container::Vector<ObjectId> result;
    querySphereRadiusFixed(center, radius, result);
    return result;
}

void ObjectSpatialIndex::querySphereRadiusFixed(
    const LogicFixedVec3& center, math::q32_32 radius,
    container::Vector<ObjectId>& out) const {
    queryRadiusImplFixed(center, radius, true, out);
}

void ObjectSpatialIndex::queryRadiusImplFixed(
    const LogicFixedVec3& center, math::q32_32 radius,
    bool useBoundingSphere, container::Vector<ObjectId>& out) const {
    out.clear();
    if (radius < math::q32_32{}) return;
    const int32_t minimumX = cellCoordinate(center.x - radius);
    const int32_t maximumX = cellCoordinate(center.x + radius);
    const int32_t minimumY = cellCoordinate(center.y - radius);
    const int32_t maximumY = cellCoordinate(center.y + radius);
    const int64_t cellCountX = static_cast<int64_t>(maximumX) - static_cast<int64_t>(minimumX) + 1;
    const int64_t cellCountY = static_cast<int64_t>(maximumY) - static_cast<int64_t>(minimumY) + 1;
    const auto matches = [&center, radius, useBoundingSphere](
                             const IndexedRecord& record) {
        if (useBoundingSphere ? record.sphereRadiusUnbounded
                              : record.radiusUnbounded) {
            return true;
        }
        const math::q32_32 combinedRadius = radius + (useBoundingSphere
            ? record.value.sphereRadius
            : record.value.radius);
        const math::q32_32 dx = center.x - record.value.position.x;
        const math::q32_32 dy = center.y - record.value.position.y;
        return dx * dx + dy * dy <= combinedRadius * combinedRadius;
    };
    if (exceedsCellBudget(cellCountX, cellCountY)) {
        // An enormous area query is rare (debug/strategic scans) and scanning
        // the ObjectId records is safer than allocating/iterating billions of
        // grid cells. Traversal still ends in deterministic ObjectId order.
        for (const auto& [object, record] : m_records) {
            if (matches(record)) out.push_back(object);
        }
    } else {
        for (int32_t y = minimumY; y <= maximumY; ++y) {
            for (int32_t x = minimumX; x <= maximumX; ++x) {
                const auto cell = m_cells.find(cellKey(x, y));
                if (cell == m_cells.end()) continue;
                for (const ObjectId object : cell->second) {
                    const auto record = m_records.find(object);
                    if (record != m_records.end() && matches(record->second)) out.push_back(object);
                }
                if (x == std::numeric_limits<int32_t>::max()) break;
            }
            if (y == std::numeric_limits<int32_t>::max()) break;
        }
    }
    // m_oversized follows ObjectId-sorted rebuild order, so appending it does
    // not expose an unordered container traversal. The final sort below also
    // handles a large object's overlap with ordinary cells.
    for (const ObjectId object : m_oversized) {
        const auto record = m_records.find(object);
        if (record == m_records.end()) continue;
        if (matches(record->second)) {
            out.push_back(object);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

std::optional<ObjectId> ObjectSpatialIndex::nearestAtFixed(
    const LogicFixedVec3& point, math::q32_32 maximumDistance) const {
    if (maximumDistance < math::q32_32{}) return std::nullopt;
    const container::Vector<ObjectId> candidates =
        queryRadiusFixed(point, maximumDistance);
    // queryRadiusFixed pads its test by each record's own bounding radius, so a
    // wide-footprint (or unbounded-radius) candidate can overlap the query while
    // its centre sits outside maximumDistance.  Selection below is by centre
    // distance, so the caller's limit has to be re-applied here or such a record
    // would be reported as the nearest object within maximumDistance.
    const math::q32_32 maximumDistanceSquared = maximumDistance * maximumDistance;
    std::optional<ObjectId> result;
    math::q32_32 nearestDistanceSquared = math::q32_32::from_raw(
        std::numeric_limits<int64_t>::max());
    for (const ObjectId object : candidates) {
        const auto record = m_records.find(object);
        if (record == m_records.end()) continue;
        const math::q32_32 dx = point.x - record->second.value.position.x;
        const math::q32_32 dy = point.y - record->second.value.position.y;
        const math::q32_32 distanceSquared = dx * dx + dy * dy;
        if (distanceSquared > maximumDistanceSquared) continue;
        if (!result || distanceSquared < nearestDistanceSquared ||
            (distanceSquared == nearestDistanceSquared && object < *result)) {
            result = object;
            nearestDistanceSquared = distanceSquared;
        }
    }
    return result;
}

container::Vector<ObjectSpatialRecord> ObjectSpatialIndex::records() const {
    container::Vector<ObjectSpatialRecord> result;
    result.reserve(m_records.size());
    for (const auto& [id, record] : m_records) {
        static_cast<void>(id);
        result.push_back(record.value);
    }
    std::sort(result.begin(), result.end(), [](const ObjectSpatialRecord& lhs,
                                               const ObjectSpatialRecord& rhs) {
        return lhs.object < rhs.object;
    });
    return result;
}

} // namespace engine
