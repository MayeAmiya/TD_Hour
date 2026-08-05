#pragma once

#include "presentation/render/RenderWorldDescriptorContracts.h"

#include <optional>

namespace engine::render {

// Device-independent state for the legacy W3DTracerDraw presentation.  FX
// INI speed remains world-units/second in the modern catalog; the original
// parser converted it to world-units/logic-frame before advancing at 30 Hz.
struct TypedFxTracerState final {
    RenderVector origin{};
    RenderVector direction{1.0f, 0.0f, 0.0f};
    RenderVector color{1.0f, 1.0f, 1.0f};
    float length = 10.0f;
    float width = 1.0f;
    float speedPerSecond = 0.0f;
    float ageSeconds = 0.0f;
    float lifetimeSeconds = 0.0f;

    [[nodiscard]] RenderVector start() const noexcept;
    [[nodiscard]] RenderVector end() const noexcept;
    [[nodiscard]] float opacity() const noexcept;
    [[nodiscard]] bool expired() const noexcept;
    void advance(float deltaSeconds) noexcept;
};

[[nodiscard]] std::optional<TypedFxTracerState> makeTypedFxTracer(
    RenderVector source, RenderVector target, RenderVector color,
    float speedPerSecond, float decayAt, float length, float width) noexcept;

} // namespace engine::render
