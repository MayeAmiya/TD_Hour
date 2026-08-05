#pragma once

#include "container/container_types.h"

#include "../collision/frustum_queries.h"
#include "../geometry/aabb.h"
#include "../geometry/frustum.h"
#include "../geometry/sphere.h"

#include <algorithm>
#include <cstdint>
#include <utility>
namespace math {

// A rebuilt-on-change BVH for broad-phase queries. It deliberately stores no
// ownership information: callers own objects and provide their current bounds.
class aabb_tree
{
public:
    void clear() noexcept
    {
        entries_.clear();
        nodes_.clear();
        indices_.clear();
        dirty_ = false;
    }

    void add(void* object, const aabb& box)
    {
        if (!object) { return; }
        update(object, box);
        if (std::none_of(entries_.begin(), entries_.end(),
                         [object](const entry& value) { return value.object == object; }))
        {
            entries_.push_back({box, object});
        }
        dirty_ = true;
    }

    void remove(void* object)
    {
        const auto it = std::remove_if(entries_.begin(), entries_.end(),
            [object](const entry& value) { return value.object == object; });
        if (it != entries_.end())
        {
            entries_.erase(it, entries_.end());
            dirty_ = true;
        }
    }

    void update(void* object, const aabb& box)
    {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
            [object](const entry& value) { return value.object == object; });
        if (it != entries_.end())
        {
            it->box = box;
            dirty_ = true;
        }
    }

    template <typename F>
    void query(vec3 point, F&& callback) const
    {
        query(aabb{point, vec3::zero()}, std::forward<F>(callback));
    }

    template <typename F>
    void query(const aabb& box, F&& callback) const
    {
        rebuild_if_needed();
        query_nodes([&box](const aabb& candidate) { return candidate.intersects(box); }, callback);
    }

    template <typename F>
    void query(const sphere& volume, F&& callback) const
    {
        rebuild_if_needed();
        query_nodes([&volume](const aabb& candidate) { return candidate.intersects(volume); }, callback);
    }

    template <typename F>
    void query(const frustum& volume, F&& callback) const
    {
        rebuild_if_needed();
        query_nodes([&volume](const aabb& candidate) {
            return overlap::frustum_aabb(volume, candidate) != plane_side::back;
        }, callback);
    }

    [[nodiscard]] int object_count() const noexcept { return static_cast<int>(entries_.size()); }
    [[nodiscard]] int node_count() const { rebuild_if_needed(); return static_cast<int>(nodes_.size()); }
    [[nodiscard]] int depth() const { rebuild_if_needed(); return depth_; }

    [[nodiscard]] aabb bounds() const
    {
        rebuild_if_needed();
        return nodes_.empty() ? aabb{} : nodes_.front().box;
    }

private:
    static constexpr int leaf_capacity = 8;

    struct entry
    {
        aabb box;
        void* object = nullptr;
    };

    struct node
    {
        aabb box;
        int left = -1;
        int right = -1;
        int first = 0;
        int count = 0;

        [[nodiscard]] bool is_leaf() const noexcept { return left < 0; }
    };

    void rebuild_if_needed() const
    {
        if (!dirty_) { return; }
        nodes_.clear();
        indices_.clear();
        depth_ = 0;
        indices_.reserve(entries_.size());
        for (size_t i = 0; i < entries_.size(); ++i) { indices_.push_back(static_cast<int>(i)); }
        if (!indices_.empty()) { build_node(0, static_cast<int>(indices_.size()), 1); }
        dirty_ = false;
    }

    int build_node(int first, int count, int depth) const
    {
        const int node_index = static_cast<int>(nodes_.size());
        nodes_.push_back({});
        depth_ = std::max(depth_, depth);

        aabb box = entries_[indices_[first]].box;
        vec3 centroid_min = box.center();
        vec3 centroid_max = centroid_min;
        for (int i = 1; i < count; ++i)
        {
            const aabb& candidate = entries_[indices_[first + i]].box;
            box.add_box(candidate);
            centroid_min = vec3::min(centroid_min, candidate.center());
            centroid_max = vec3::max(centroid_max, candidate.center());
        }

        nodes_[node_index].box = box;
        if (count <= leaf_capacity)
        {
            nodes_[node_index].first = first;
            nodes_[node_index].count = count;
            return node_index;
        }

        const vec3 extent = centroid_max - centroid_min;
        const int axis = extent.y() > extent.x() ? (extent.z() > extent.y() ? 2 : 1)
                                                : (extent.z() > extent.x() ? 2 : 0);
        const int middle = first + count / 2;
        std::nth_element(indices_.begin() + first, indices_.begin() + middle, indices_.begin() + first + count,
            [this, axis](int lhs, int rhs) {
                const vec3 a = entries_[lhs].box.center();
                const vec3 b = entries_[rhs].box.center();
                return axis == 0 ? a.x() < b.x() : axis == 1 ? a.y() < b.y() : a.z() < b.z();
            });

        const int left = build_node(first, middle - first, depth + 1);
        const int right = build_node(middle, first + count - middle, depth + 1);
        nodes_[node_index].left = left;
        nodes_[node_index].right = right;
        return node_index;
    }

    template <typename Predicate, typename F>
    void query_nodes(Predicate&& overlaps, F&& callback) const
    {
        if (nodes_.empty()) { return; }
        container::Vector<int> pending{0};
        while (!pending.empty())
        {
            const int index = pending.back();
            pending.pop_back();
            const node& current = nodes_[index];
            if (!overlaps(current.box)) { continue; }
            if (current.is_leaf())
            {
                for (int i = 0; i < current.count; ++i)
                {
                    const entry& value = entries_[indices_[current.first + i]];
                    if (overlaps(value.box)) { callback(value.object); }
                }
            }
            else
            {
                pending.push_back(current.left);
                pending.push_back(current.right);
            }
        }
    }

    container::Vector<entry> entries_;
    mutable container::Vector<node> nodes_;
    mutable container::Vector<int> indices_;
    mutable int depth_ = 0;
    mutable bool dirty_ = false;
};

} // namespace math
