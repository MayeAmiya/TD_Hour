#pragma once

#include "core/container/container_types.h"
namespace game::track_marks::visual_defaults {

inline constexpr container::StringView kLeftWidthBone = "TREADFX01";
inline constexpr container::StringView kRightWidthBone = "TREADFX02";
inline constexpr float kFallbackWidth = 14.0f;
inline constexpr float kAdditionalTreadWidth = 4.0f;
inline constexpr float kSegmentLength = 10.0f;

static_assert(kFallbackWidth > 0.0f);
static_assert(kAdditionalTreadWidth >= 0.0f);
static_assert(kSegmentLength > 0.0f);

} // namespace game::track_marks::visual_defaults

namespace game {

// Functional/visual contract authored by W3DModelDraw. These values describe
// what a track looks like and how it is attached; they do not grant memory,
// history length or upload budget to the renderer.
struct TrackMarksVisualDescriptor final {
    container::String sourceModuleClass;
    container::String sourceModuleTag;
    container::String textureName;
    container::String leftWidthBone{track_marks::visual_defaults::kLeftWidthBone};
    container::String rightWidthBone{track_marks::visual_defaults::kRightWidthBone};
    float fallbackWidth = track_marks::visual_defaults::kFallbackWidth;
    float additionalTreadWidth =
        track_marks::visual_defaults::kAdditionalTreadWidth;
    float segmentLength = track_marks::visual_defaults::kSegmentLength;

    [[nodiscard]] bool usable() const noexcept { return !textureName.empty(); }
};

} // namespace game

namespace engine {

// Feature policy is kept separate from quality/performance budgets. Disabling
// the feature changes presentation behavior; it is not a resource-limit knob.
struct TrackMarksFeatureSettings final {
    bool enabled = true;
};

struct TrackMarksRenderVisualDescriptor final {
    bool enabled = false;
    container::String textureName;
    container::String leftWidthBone;
    container::String rightWidthBone;
    float fallbackWidth = game::track_marks::visual_defaults::kFallbackWidth;
    float additionalTreadWidth =
        game::track_marks::visual_defaults::kAdditionalTreadWidth;
    float segmentLength = game::track_marks::visual_defaults::kSegmentLength;
};

} // namespace engine
