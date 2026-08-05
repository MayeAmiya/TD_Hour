#pragma once

#include "core/container/hash_containers.h"

#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>
namespace engine {
class ObjectLifecycle;
}

namespace engine {

// Deterministic simulation-side broad phase for ObjectId queries. This is
// intentionally not math::grid: that reusable render/culling utility stores
// void* and uses unordered traversal, whereas gameplay selection/AI/path
// consumers need stable ObjectId results. Hash maps are lookup-only here;
// every externally visible result is ObjectId-sorted.
struct ObjectSpatialRecord final {
    ObjectId object = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    // The 2D radius is the footprint/broad-phase radius used by selection and
    // path-facing queries.  Damage in the original game uses the distinct
    // 3D bounding sphere, so retain both rather than silently treating a
    // tall building or aircraft as a flat disc.
    math::q32_32 radius{};
    math::q32_32 sphereRadius{};
};

class ObjectSpatialIndex final {
public:
    ObjectSpatialIndex() = default;

    void clear() noexcept;
    [[nodiscard]] bool setCellSize(math::q32_32 value) noexcept;
    [[nodiscard]] math::q32_32 cellSize() const noexcept {
        return m_cellSizeFixed;
    }
    [[nodiscard]] uint64_t revision() const noexcept { return m_revision; }
    [[nodiscard]] size_t objectCount() const noexcept { return m_records.size(); }

    // Rebuilds after the confirmed object-update/structural boundary. The
    // implementation sorts input by ObjectId before cell insertion so an
    // EnTT storage reordering cannot change a later query's tie-break.
    void rebuild(const ecs::registry& registry, const ObjectLifecycle& lifecycle);
    // Applies only entities whose shared dirty marker includes Spatial.
    // Producers may call this at several deterministic barriers; a barrier
    // with no changed object is O(1) with respect to world population.
    void refreshDirty(ecs::registry& registry,
                      const ObjectLifecycle& lifecycle);
    // Physical lifecycle reclamation removes the marker together with the
    // entity. Call only when a destroy flush actually reclaimed objects.
    void pruneMissing(const ObjectLifecycle& lifecycle);

    [[nodiscard]] container::Vector<ObjectId> queryRadiusFixed(
        const LogicFixedVec3& center, math::q32_32 radius) const;
    // Caller-owned result form for hot inner loops. The output is cleared on
    // entry but retains its capacity; externally visible ordering remains
    // ObjectId-sorted and duplicate-free exactly like the value-return form.
    void queryRadiusFixed(
        const LogicFixedVec3& center, math::q32_32 radius,
        container::Vector<ObjectId>& out) const;
    // A 2D grid broad phase for a later exact 3D bounding-sphere test.  The
    // returned IDs are still only candidates: callers must account for Z and
    // the exact geometry center before applying gameplay damage.
    [[nodiscard]] container::Vector<ObjectId> querySphereRadiusFixed(
        const LogicFixedVec3& center, math::q32_32 radius) const;
    void querySphereRadiusFixed(
        const LogicFixedVec3& center, math::q32_32 radius,
        container::Vector<ObjectId>& out) const;
    [[nodiscard]] std::optional<ObjectId> nearestAtFixed(
        const LogicFixedVec3& point, math::q32_32 maximumDistance) const;
    [[nodiscard]] container::Vector<ObjectSpatialRecord> records() const;

private:
    struct IndexedRecord final {
        ObjectSpatialRecord value;
        bool radiusUnbounded = false;
        bool sphereRadiusUnbounded = false;
        bool indexedAsOversized = false;
    };

    [[nodiscard]] int32_t cellCoordinate(math::q32_32 value) const noexcept;
    [[nodiscard]] static uint64_t cellKey(int32_t x, int32_t y) noexcept;
    void queryRadiusImplFixed(
        const LogicFixedVec3& center, math::q32_32 radius,
        bool useBoundingSphere, container::Vector<ObjectId>& out) const;
    [[nodiscard]] std::optional<IndexedRecord> projectRecord(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ecs::entity entity) const;
    [[nodiscard]] static bool equivalent(
        const IndexedRecord& left, const IndexedRecord& right) noexcept;
    void eraseRecord(ObjectId object);
    void insertRecord(IndexedRecord record);
    [[nodiscard]] bool refreshRecord(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ecs::entity entity);

    math::q32_32 m_cellSizeFixed{int32_t{64}};
    uint64_t m_revision = 0;
    container::HashMap<uint64_t, container::Vector<ObjectId>> m_cells;
    container::HashMap<ObjectId, IndexedRecord> m_records;
    // Extremely large/malformed geometry is retained in a small sorted
    // overflow list instead of expanding one record across billions of cells.
    container::Vector<ObjectId> m_oversized;
    // Reused canonical projection for barrier refreshes. Repeated same-frame
    // refreshes neither allocate a temporary vector nor rebuild cell storage
    // when the value projection is unchanged.
    container::Vector<IndexedRecord> m_rebuildScratch;
};

} // namespace engine
