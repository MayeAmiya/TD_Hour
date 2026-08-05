#pragma once

#include "container/container_types.h"

#include "../vector/float3.h"
#include "core/math/wwmath/base/wwmath_core.h"
#include <algorithm>
#include <cmath>
namespace math {

class curve3
{
public:
    virtual ~curve3() = default;

    virtual vec3 evaluate(float t) const = 0;
    virtual int key_count() const = 0;
    virtual void set_key(int i, vec3 pt) = 0;
    virtual int add_key(vec3 pt, float t) = 0;
    virtual void remove_key(int i) = 0;
    virtual void clear_keys() = 0;

    float start_time() const
    {
        if (keys_.empty()) { return 0.0f; }
        return keys_.front().time;
    }
    float end_time() const
    {
        if (keys_.empty()) { return 0.0f; }
        return keys_.back().time;
    }
    bool looping() const noexcept { return looping_; }
    void set_looping(bool v) noexcept { looping_ = v; }

protected:
    struct key
    {
        vec3  point;
        float time;
    };
    bool looping_ = false;
    container::Vector<key> keys_;
};

class curve_traverser
{
public:
    void set_curve(const curve3* curve) noexcept { curve_ = curve; }

    void advance(float dt, float speed = 1.0f)
    {
        if (!curve_) { return; }
        t_ += dt * speed;
        float end = curve_->end_time();
        float start = curve_->start_time();
        float range = end - start;
        if (range <= 0.0f) { return; }
        if (curve_->looping())
        {
            while (t_ > end)  { t_ -= range; }
            while (t_ < start) { t_ += range; }
        }
        else
        {
            t_ = clamp(t_, start, end);
        }
    }

    void advance_distance(float dist)
    {
        if (!curve_) { return; }
        // Numerical: sample nearby to find t that gives desired arc length
        // Simplified: use speed * dt approximation
        const float local_length = (curve_->evaluate(t_ + EPSILON) - curve_->evaluate(t_)).length();
        if (local_length > EPSILON) { t_ += dist / local_length; }
    }

    vec3 position()   const { return curve_ ? curve_->evaluate(t_) : vec3::zero(); }
    vec3 direction()  const
    {
        if (!curve_) { return vec3::forward(); }
        vec3 p0 = curve_->evaluate(t_);
        vec3 p1 = curve_->evaluate(t_ + 0.01f);
        return (p1 - p0).normalized();
    }

    float total_length() const
    {
        if (!curve_) { return 0.0f; }
        float len = 0.0f;
        float end = curve_->end_time();
        float start = curve_->start_time();
        constexpr int samples = 100;
        vec3 prev = curve_->evaluate(start);
        for (int i = 1; i <= samples; ++i)
        {
            float t = start + (end - start) * (static_cast<float>(i) / samples);
            vec3 cur = curve_->evaluate(t);
            len += cur.distance(prev);
            prev = cur;
        }
        return len;
    }

    float current_t() const noexcept { return t_; }
    void set_t(float t) noexcept { t_ = t; }

private:
    const curve3* curve_ = nullptr;
    float t_ = 0.0f;
};

} // namespace math
