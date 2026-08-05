#pragma once

#include "container/container_types.h"
namespace math {

class ode_solver
{
public:
    using deriv_fn = void (*)(float time, container::Span<const float> state, container::Span<float> derivative);

    [[nodiscard]] static constexpr size_t euler_workspace_size(size_t dimension) noexcept { return dimension; }
    [[nodiscard]] static constexpr size_t midpoint_workspace_size(size_t dimension) noexcept { return dimension * 2; }
    [[nodiscard]] static constexpr size_t rk4_workspace_size(size_t dimension) noexcept { return dimension * 5; }

    static bool euler(deriv_fn derivative, float time, container::Span<float> state, float dt,
                      container::Span<float> workspace) noexcept
    {
        if (!derivative || workspace.size() < euler_workspace_size(state.size())) { return false; }
        auto k1 = workspace.first(state.size());
        derivative(time, state, k1);
        for (size_t i = 0; i < state.size(); ++i) { state[i] += dt * k1[i]; }
        return true;
    }

    static bool midpoint(deriv_fn derivative, float time, container::Span<float> state, float dt,
                         container::Span<float> workspace) noexcept
    {
        const size_t dimension = state.size();
        if (!derivative || workspace.size() < midpoint_workspace_size(dimension)) { return false; }
        auto k1 = workspace.subspan(0, dimension);
        auto temporary = workspace.subspan(dimension, dimension);
        derivative(time, state, k1);
        for (size_t i = 0; i < dimension; ++i) { temporary[i] = state[i] + 0.5f * dt * k1[i]; }
        derivative(time + 0.5f * dt, temporary, k1);
        for (size_t i = 0; i < dimension; ++i) { state[i] += dt * k1[i]; }
        return true;
    }

    static bool rk4(deriv_fn derivative, float time, container::Span<float> state, float dt,
                    container::Span<float> workspace) noexcept
    {
        const size_t dimension = state.size();
        if (!derivative || workspace.size() < rk4_workspace_size(dimension)) { return false; }
        auto k1 = workspace.subspan(0 * dimension, dimension);
        auto k2 = workspace.subspan(1 * dimension, dimension);
        auto k3 = workspace.subspan(2 * dimension, dimension);
        auto k4 = workspace.subspan(3 * dimension, dimension);
        auto temporary = workspace.subspan(4 * dimension, dimension);

        derivative(time, state, k1);
        for (size_t i = 0; i < dimension; ++i) { temporary[i] = state[i] + 0.5f * dt * k1[i]; }
        derivative(time + 0.5f * dt, temporary, k2);
        for (size_t i = 0; i < dimension; ++i) { temporary[i] = state[i] + 0.5f * dt * k2[i]; }
        derivative(time + 0.5f * dt, temporary, k3);
        for (size_t i = 0; i < dimension; ++i) { temporary[i] = state[i] + dt * k3[i]; }
        derivative(time + dt, temporary, k4);
        for (size_t i = 0; i < dimension; ++i)
        {
            state[i] += dt * (k1[i] + 2.0f * k2[i] + 2.0f * k3[i] + k4[i]) / 6.0f;
        }
        return true;
    }

    static bool euler(deriv_fn derivative, float time, container::Span<float> state, float dt)
    {
        container::Vector<float> workspace(euler_workspace_size(state.size()));
        return euler(derivative, time, state, dt, workspace);
    }

    static bool midpoint(deriv_fn derivative, float time, container::Span<float> state, float dt)
    {
        container::Vector<float> workspace(midpoint_workspace_size(state.size()));
        return midpoint(derivative, time, state, dt, workspace);
    }

    static bool rk4(deriv_fn derivative, float time, container::Span<float> state, float dt)
    {
        container::Vector<float> workspace(rk4_workspace_size(state.size()));
        return rk4(derivative, time, state, dt, workspace);
    }
};

} // namespace math
