#pragma once

#include "../vector/float3.h"
#include <random>

namespace math {

class vec3_randomizer
{
public:
    virtual ~vec3_randomizer() = default;
    virtual vec3 generate() = 0;
    virtual float max_extent() const = 0;
    virtual void scale(float s) = 0;
};

class box_randomizer : public vec3_randomizer
{
public:
    box_randomizer(vec3 extents, uint32_t seed = 5489u)
        : extents_(extents)
        , rng_(seed)
        , dist_(-1.0f, 1.0f)
    {
    }

    vec3 generate() override
    {
        return vec3{
            extents_.x() * dist_(rng_),
            extents_.y() * dist_(rng_),
            extents_.z() * dist_(rng_),
        };
    }

    float max_extent() const override
    {
        return std::max({extents_.x(), extents_.y(), extents_.z()});
    }

    void scale(float s) override
    {
        extents_ = extents_ * s;
    }

private:
    vec3 extents_;
    std::mt19937 rng_;
    std::uniform_real_distribution<float> dist_;
};

class solid_sphere_randomizer : public vec3_randomizer
{
public:
    solid_sphere_randomizer(float radius, uint32_t seed = 5489u)
        : radius_(radius)
        , rng_(seed)
        , dist_(-1.0f, 1.0f)
    {
    }

    vec3 generate() override
    {
        // Rejection sampling in unit sphere, then scale
        while (true)
        {
            vec3 v{dist_(rng_), dist_(rng_), dist_(rng_)};
            float len_sq = v.length_sq();
            if (len_sq <= 1.0f)
            {
                return v * radius_;
            }
        }
    }

    float max_extent() const override { return radius_; }

    void scale(float s) override { radius_ *= s; }

private:
    float radius_;
    std::mt19937 rng_;
    std::uniform_real_distribution<float> dist_;
};

class hollow_sphere_randomizer : public vec3_randomizer
{
public:
    hollow_sphere_randomizer(float radius, uint32_t seed = 5489u)
        : radius_(radius)
        , rng_(seed)
        , dist_(-1.0f, 1.0f)
    {
    }

    vec3 generate() override
    {
        while (true)
        {
            vec3 v{dist_(rng_), dist_(rng_), dist_(rng_)};
            float len_sq = v.length_sq();
            if (len_sq > EPSILON && len_sq <= 1.0f)
            {
                return v.normalized() * radius_;
            }
        }
    }

    float max_extent() const override { return radius_; }

    void scale(float s) override { radius_ *= s; }

private:
    float radius_;
    std::mt19937 rng_;
    std::uniform_real_distribution<float> dist_;
};

class cylinder_randomizer : public vec3_randomizer
{
public:
    cylinder_randomizer(float extent, float radius, uint32_t seed = 5489u)
        : extent_(extent)
        , radius_(radius)
        , rng_(seed)
        , dist_(-1.0f, 1.0f)
    {
    }

    vec3 generate() override
    {
        // Random point within cylinder along Y axis
        float y = extent_ * dist_(rng_);
        while (true)
        {
            float x = radius_ * dist_(rng_);
            float z = radius_ * dist_(rng_);
            if (x * x + z * z <= radius_ * radius_)
            {
                return {x, y, z};
            }
        }
    }

    float max_extent() const override
    {
        return std::max(extent_, radius_);
    }

    void scale(float s) override
    {
        extent_ *= s;
        radius_ *= s;
    }

private:
    float extent_;
    float radius_;
    std::mt19937 rng_;
    std::uniform_real_distribution<float> dist_;
};

} // namespace math
