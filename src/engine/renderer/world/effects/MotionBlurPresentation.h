#pragma once

#include <cstdint>

namespace engine::render {

// ScreenMotionBlurFilter builds its result from one opaque source sample and
// up to thirty progressively transformed samples of the same captured view.
// Keeping this arithmetic outside the D3D12 command path makes the authored
// DX8 constants and integer envelope independently testable.
enum class LegacyMotionBlurGeometry : uint8_t {
    Radial,
    Pan,
    EndPan,
};

struct LegacyMotionBlurSamplePlan final {
    float centerX = 0.5f;
    float centerY = 0.5f;
    float baseScale = 1.0f;
    float stepScaleX = 1.0f;
    float stepScaleY = 1.0f;
    float sampleAlpha = 0.0f;
    uint32_t tapCount = 0;
    bool additive = false;
};

inline constexpr int32_t kLegacyMotionBlurMaximumCount = 60;
inline constexpr int32_t kLegacyMotionBlurMaximumTaps = 30;
inline constexpr int32_t kLegacyMotionBlurCountStep = 5;
inline constexpr int32_t kLegacyMotionBlurDefaultPanFactor = 30;

// RefCode substitutes 30 only for values below one. Positive ScriptAction
// values are not capped.
[[nodiscard]] int32_t legacyMotionBlurPanFactor(int32_t authoredAmount) noexcept;

// RefCode derives a pan count from the projected look-at delta, then bounds it
// to [factor/2, factor]. This safe form preserves the entire positive int32
// authoring range without undefined float-to-int overflow.
[[nodiscard]] int32_t legacyMotionBlurPanCount(float deltaLength,
                                                int32_t panFactor) noexcept;

[[nodiscard]] LegacyMotionBlurSamplePlan legacyMotionBlurSamplePlan(
    int32_t maxCount, bool additive, LegacyMotionBlurGeometry geometry,
    float priorDeltaX = 0.0f, float priorDeltaY = 0.0f) noexcept;

} // namespace engine::render
