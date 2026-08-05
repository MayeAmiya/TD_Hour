#pragma once

#include "container/hash_containers.h"

#include "../collision/frustum_queries.h"
#include "../geometry/aabb.h"
#include "../geometry/frustum.h"
#include "../geometry/sphere.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
namespace math {

// Uniform-grid broad phase. Queries return each registered object at most once
// and perform a bounding-volume test before invoking the callback.
class grid
{
public:
    void set_world(const aabb& world, float cell_size)
    {
        if (cell_size <= 0.0f) { throw std::invalid_argument("grid cell size must be positive"); }
        world_ = world;
        cell_size_ = cell_size;
        inv_cell_size_ = 1.0f / cell_size;
        cells_.clear();
        entries_.clear();
    }

    void clear() noexcept
    {
        cells_.clear();
        entries_.clear();
    }

    void add(void* object, const aabb& box)
    {
        if (!object) { return; }
        remove(object);
        entries_.emplace(object, box);
        for_each_cell(box, [this, object](int32_t x, int32_t y, int32_t z) {
            cells_[cell_key(x, y, z)].push_back(object);
        });
    }

    void remove(void* object)
    {
        if (!entries_.erase(object)) { return; }
        for (auto it = cells_.begin(); it != cells_.end(); )
        {
            auto& bucket = it->second;
            bucket.erase(std::remove(bucket.begin(), bucket.end(), object), bucket.end());
            it = bucket.empty() ? cells_.erase(it) : std::next(it);
        }
    }

    void update(void* object, const aabb&, const aabb& new_box) { add(object, new_box); }
    void update(void* object, const aabb& new_box) { add(object, new_box); }

    template <typename F>
    void query(vec3 point, F&& callback) const
    {
        query(aabb{point, vec3::zero()}, std::forward<F>(callback));
    }

    template <typename F>
    void query(const aabb& box, F&& callback) const
    {
        query_cells(box, [&box](const aabb& candidate) { return candidate.intersects(box); }, callback);
    }

    template <typename F>
    void query(const sphere& volume, F&& callback) const
    {
        const aabb broad_phase{volume.center(), vec3{volume.radius(), volume.radius(), volume.radius()}};
        query_cells(broad_phase, [&volume](const aabb& candidate) { return candidate.intersects(volume); }, callback);
    }

    template <typename F>
    void query(const frustum& volume, F&& callback) const
    {
        container::HashSet<void*> seen;
        for (const auto& [object, box] : entries_)
        {
            if (overlap::frustum_aabb(volume, box) != plane_side::back && seen.insert(object).second)
            {
                callback(object);
            }
        }
    }

    [[nodiscard]] int object_count() const noexcept { return static_cast<int>(entries_.size()); }

private:
    [[nodiscard]] int3 world_to_cell(vec3 point) const noexcept
    {
        const vec3 offset = point - world_.min();
        return {
            static_cast<int32_t>(std::floor(offset.x() * inv_cell_size_)),
            static_cast<int32_t>(std::floor(offset.y() * inv_cell_size_)),
            static_cast<int32_t>(std::floor(offset.z() * inv_cell_size_)),
        };
    }

    template <typename F>
    void for_each_cell(const aabb& box, F&& callback) const
    {
        const int3 min_cell = world_to_cell(box.min());
        const int3 max_cell = world_to_cell(box.max());
        for (int32_t z = min_cell.z; z <= max_cell.z; ++z)
            for (int32_t y = min_cell.y; y <= max_cell.y; ++y)
                for (int32_t x = min_cell.x; x <= max_cell.x; ++x)
                    callback(x, y, z);
    }

    template <typename Predicate, typename F>
    void query_cells(const aabb& broad_phase, Predicate&& matches, F&& callback) const
    {
        container::HashSet<void*> seen;
        for_each_cell(broad_phase, [this, &seen, &matches, &callback](int32_t x, int32_t y, int32_t z) {
            const auto cell = cells_.find(cell_key(x, y, z));
            if (cell == cells_.end()) { return; }
            for (void* object : cell->second)
            {
                const auto entry = entries_.find(object);
                if (entry != entries_.end() && seen.insert(object).second && matches(entry->second))
                {
                    callback(object);
                }
            }
        });
    }

    [[nodiscard]] static int64_t cell_key(int32_t x, int32_t y, int32_t z) noexcept
    {
        return (static_cast<int64_t>(static_cast<uint32_t>(x) & 0x1fffff) << 42)
             | (static_cast<int64_t>(static_cast<uint32_t>(y) & 0x1fffff) << 21)
             | static_cast<int64_t>(static_cast<uint32_t>(z) & 0x1fffff);
    }

    aabb world_{};
    float cell_size_ = 1.0f;
    float inv_cell_size_ = 1.0f;
    container::HashMap<int64_t, container::Vector<void*>> cells_;
    container::HashMap<void*, aabb> entries_;
};

} // namespace math
