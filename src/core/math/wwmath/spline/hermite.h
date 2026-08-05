#pragma once

#include "container/container_types.h"

#include "curve.h"

#include <algorithm>

namespace math {

class hermite_curve3 : public curve3
{
public:
    vec3 evaluate(float t) const override
    {
        if (keys_.empty()) { return vec3::zero(); }
        if (keys_.size() == 1) { return keys_[0].point; }

        // Find segment
        size_t i = 0;
        for (size_t j = 1; j < keys_.size(); ++j)
        {
            if (keys_[j].time > t) { break; }
            i = j;
        }
        if (i >= keys_.size() - 1) { return keys_.back().point; }

        const key& k0 = keys_[i];
        const key& k1 = keys_[i + 1];
        // add_key inserts with lower_bound and happily accepts duplicate times,
        // so guard the divisor: two keys at the same time otherwise divide by zero
        // and return an inf/NaN position.  curve_traverser::advance and
        // advance_distance already guard theirs.
        const float span = k1.time - k0.time;
        if (span <= 0.0f) { return k0.point; }
        float seg_t = (t - k0.time) / span;

        auto h00 = [](float s) { return 2*s*s*s - 3*s*s + 1; };
        auto h10 = [](float s) { return s*s*s - 2*s*s + s; };
        auto h01 = [](float s) { return -2*s*s*s + 3*s*s; };
        auto h11 = [](float s) { return s*s*s - s*s; };

        return k0.point * h00(seg_t)
             + tan_in_[i] * h10(seg_t)
             + k1.point * h01(seg_t)
             + tan_in_[i + 1] * h11(seg_t);
    }

    int key_count() const override { return static_cast<int>(keys_.size()); }

    void set_key(int idx, vec3 pt) override
    {
        if (idx >= 0 && idx < static_cast<int>(keys_.size()))
        {
            keys_[idx].point = pt;
        }
    }

    int add_key(vec3 pt, float t) override
    {
        const auto position = std::lower_bound(keys_.begin(), keys_.end(), t,
            [](const key& candidate, float time) { return candidate.time < time; });
        const auto index = static_cast<size_t>(position - keys_.begin());
        keys_.insert(position, {pt, t});
        tan_in_.insert(tan_in_.begin() + static_cast<std::ptrdiff_t>(index), vec3::zero());
        tan_out_.insert(tan_out_.begin() + static_cast<std::ptrdiff_t>(index), vec3::zero());
        return static_cast<int>(index);
    }

    void remove_key(int idx) override
    {
        if (idx >= 0 && idx < static_cast<int>(keys_.size()))
        {
            keys_.erase(keys_.begin() + idx);
            tan_in_.erase(tan_in_.begin() + idx);
            tan_out_.erase(tan_out_.begin() + idx);
        }
    }

    void clear_keys() override
    {
        keys_.clear();
        tan_in_.clear();
        tan_out_.clear();
    }

    void set_tangent_in(int idx, vec3 t) noexcept
    {
        if (idx >= 0 && idx < static_cast<int>(tan_in_.size()))
        {
            tan_in_[idx] = t;
        }
    }
    void set_tangent_out(int idx, vec3 t) noexcept
    {
        if (idx >= 0 && idx < static_cast<int>(tan_out_.size()))
        {
            tan_out_[idx] = t;
        }
    }
    vec3 tangent_in(int idx) const noexcept
    {
        return idx >= 0 && idx < static_cast<int>(tan_in_.size()) ? tan_in_[idx] : vec3::zero();
    }
    vec3 tangent_out(int idx) const noexcept
    {
        return idx >= 0 && idx < static_cast<int>(tan_out_.size()) ? tan_out_[idx] : vec3::zero();
    }

protected:
    container::Vector<vec3> tan_in_;
    container::Vector<vec3> tan_out_;
};

class cardinal_curve3 : public hermite_curve3
{
public:
    void set_tightness(float t) noexcept { tightness_ = t; }

    void auto_compute_tangents()
    {
        if (keys_.size() < 2) { return; }
        for (size_t i = 0; i < keys_.size(); ++i)
        {
            if (i == 0)
            {
                vec3 dir = (keys_[1].point - keys_[0].point) * tightness_;
                tan_in_[i]  = dir;
                tan_out_[i] = dir;
            }
            else if (i == keys_.size() - 1)
            {
                vec3 dir = (keys_[i].point - keys_[i - 1].point) * tightness_;
                tan_in_[i]  = dir;
                tan_out_[i] = dir;
            }
            else
            {
                vec3 dir = (keys_[i + 1].point - keys_[i - 1].point) * tightness_;
                tan_in_[i]  = dir;
                tan_out_[i] = dir;
            }
        }
    }

private:
    float tightness_ = 0.5f;
};

class catmull_rom_curve3 : public cardinal_curve3
{
public:
    catmull_rom_curve3() { set_tightness(0.5f); }
};

class tcb_curve3 : public hermite_curve3
{
public:
    void set_tcb(int idx, float tension, float continuity, float bias)
    {
        if (idx < 0 || idx >= static_cast<int>(keys_.size())) { return; }

        if (keys_.size() < 2) { return; }

        vec3 p0 = (idx > 0) ? keys_[idx - 1].point : keys_[idx].point;
        vec3 p1 = keys_[idx].point;
        vec3 p2 = (idx < static_cast<int>(keys_.size()) - 1) ? keys_[idx + 1].point : keys_[idx].point;

        float tm = (1.0f - tension) * (1.0f - continuity) * (1.0f + bias);
        float tp = (1.0f - tension) * (1.0f + continuity) * (1.0f - bias);

        tan_in_[idx]  = (p1 - p0) * tm * 0.5f + (p2 - p1) * tp * 0.5f;
        tan_out_[idx] = (p2 - p1) * tm * 0.5f + (p1 - p0) * tp * 0.5f;
    }
};

} // namespace math
