#pragma once

#include "hermite.h"
#include <algorithm>
#include <numbers>

namespace math {

enum class curve_direction : uint32_t
{
    forward,
    reverse,
};

enum class height_restriction : uint32_t
{
    none,
    must_stay_on_ground,
    must_stay_above_ground_and_below_height,
};

class vehicle_curve : public curve3
{
public:
    vec3 evaluate(float t) const override
    {
        // Find the current segment
        if (keys_.empty()) { return vec3::zero(); }
        if (keys_.size() == 1) { return keys_[0].point; }
        if (t <= keys_.front().time) { return keys_.front().point; }
        if (t >= keys_.back().time)  { return keys_.back().point; }

        size_t seg = 0;
        for (size_t i = 0; i < keys_.size() - 1; ++i)
        {
            if (t >= keys_[i].time && t < keys_[i + 1].time)
            {
                seg = i;
                break;
            }
        }

        const key& k0 = keys_[seg];
        const key& k1 = keys_[seg + 1];
        // Same duplicate-time hazard as hermite_curve3: add_key permits two keys
        // at the same time, which made this divide by zero.
        const float span = k1.time - k0.time;
        if (span <= 0.0f) { return k0.point; }
        float seg_t = (t - k0.time) / span;

        vec3 pt = lerp(k0.point, k1.point, seg_t);
        pt += vec3{0.0f, sin_offset_ * std::sin(seg_t * 2.0f * std::numbers::pi_v<float> * sin_freq_), 0.0f};
        return pt;
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
        const int index = static_cast<int>(position - keys_.begin());
        keys_.insert(position, {pt, t});
        return index;
    }

    void remove_key(int idx) override
    {
        if (idx >= 0 && idx < static_cast<int>(keys_.size()))
        {
            keys_.erase(keys_.begin() + idx);
        }
    }

    void clear_keys() override { keys_.clear(); }

    void set_sin_offset(float offset) noexcept { sin_offset_ = offset; }
    void set_sin_freq(float freq)    noexcept { sin_freq_ = freq; }
    void set_direction(curve_direction dir) noexcept { direction_ = dir; }
    void set_height_restriction(height_restriction r) noexcept { height_limit_ = r; }

    curve_direction direction() const noexcept { return direction_; }
    height_restriction height_restriction() const noexcept { return height_limit_; }

private:
    float sin_offset_ = 0.0f;
    float sin_freq_   = 1.0f;
    curve_direction direction_ = curve_direction::forward;
    math::height_restriction height_limit_ = math::height_restriction::none;
};

} // namespace math
