#pragma once

#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>

namespace engine::track_marks::performance_limits {

// Renderer safety limits. Authored compatibility data can describe smaller
// histories, but runtime presentation always uses the full-detail history and
// can never expand allocations or per-frame upload work beyond these limits.
inline constexpr uint32_t kHardMaximumStreams = 200;
inline constexpr uint32_t kHardMaximumEdgesPerStream = 100;
inline constexpr uint32_t kHardMaximumTotalEdges =
    kHardMaximumStreams * kHardMaximumEdgesPerStream;
inline constexpr uint32_t kMinimumEdgesPerStream = 2;
inline constexpr uint32_t kMinimumFadeDelayMilliseconds = 1;
inline constexpr uint32_t kHardMaximumFadeDelayMilliseconds =
    10u * 60u * 1000u;
inline constexpr uint32_t kReferenceSimulationFramesPerSecond = 30;

// Input clamps also bound geometry generation rate and transient upload size.
inline constexpr float kMinimumTrackWidth = 0.25f;
inline constexpr float kMaximumTrackWidth = 128.0f;
inline constexpr float kMinimumEdgeSpacing = 0.25f;
inline constexpr float kMaximumEdgeSpacing = 128.0f;

[[nodiscard]] constexpr uint32_t fadeFramesFromMilliseconds(
    uint32_t milliseconds,
    uint32_t framesPerSecond = kReferenceSimulationFramesPerSecond) noexcept {
    // Track marks age against the confirmed simulation frame in the renderer.
    // The historical 30 Hz value is only the compatibility default for
    // callers that do not own a session clock; extraction must provide its
    // actual fixed logic rate.
    const uint32_t rate = framesPerSecond == 0 ? 1u : framesPerSecond;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(milliseconds) * rate +
         999u) /
        1000u);
}

} // namespace engine::track_marks::performance_limits

namespace engine::track_marks::performance_defaults {

inline constexpr uint32_t kLowMaximumEdges = 30;
inline constexpr uint32_t kLowOpaqueEdges = 15;
inline constexpr uint32_t kLowFadeDelayMilliseconds = 5000;
inline constexpr uint32_t kMediumMaximumEdges = 100;
inline constexpr uint32_t kMediumOpaqueEdges = 25;
inline constexpr uint32_t kMediumFadeDelayMilliseconds = 30000;
inline constexpr uint32_t kHighMaximumEdges = 100;
inline constexpr uint32_t kHighOpaqueEdges = 25;
inline constexpr uint32_t kHighFadeDelayMilliseconds = 60000;

} // namespace engine::track_marks::performance_defaults

namespace engine {

enum class TrackMarksStaticLod : uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
};

struct TrackMarksHistoryBudget final {
    uint32_t maximumEdges =
        track_marks::performance_defaults::kHighMaximumEdges;
    uint32_t opaqueEdges =
        track_marks::performance_defaults::kHighOpaqueEdges;
    uint32_t fadeDelayMilliseconds =
        track_marks::performance_defaults::kHighFadeDelayMilliseconds;
};

inline constexpr TrackMarksHistoryBudget kLowTrackMarksBudget{
    .maximumEdges = track_marks::performance_defaults::kLowMaximumEdges,
    .opaqueEdges = track_marks::performance_defaults::kLowOpaqueEdges,
    .fadeDelayMilliseconds =
        track_marks::performance_defaults::kLowFadeDelayMilliseconds,
};
inline constexpr TrackMarksHistoryBudget kMediumTrackMarksBudget{
    .maximumEdges = track_marks::performance_defaults::kMediumMaximumEdges,
    .opaqueEdges = track_marks::performance_defaults::kMediumOpaqueEdges,
    .fadeDelayMilliseconds =
        track_marks::performance_defaults::kMediumFadeDelayMilliseconds,
};
inline constexpr TrackMarksHistoryBudget kHighTrackMarksBudget{
    .maximumEdges = track_marks::performance_defaults::kHighMaximumEdges,
    .opaqueEdges = track_marks::performance_defaults::kHighOpaqueEdges,
    .fadeDelayMilliseconds =
        track_marks::performance_defaults::kHighFadeDelayMilliseconds,
};

[[nodiscard]] constexpr bool isValidTrackMarksBudget(
    const TrackMarksHistoryBudget& budget) noexcept {
    return budget.maximumEdges >=
            track_marks::performance_limits::kMinimumEdgesPerStream &&
        budget.maximumEdges <=
            track_marks::performance_limits::kHardMaximumEdgesPerStream &&
        budget.opaqueEdges <= budget.maximumEdges &&
        budget.fadeDelayMilliseconds >=
            track_marks::performance_limits::kMinimumFadeDelayMilliseconds &&
        budget.fadeDelayMilliseconds <=
            track_marks::performance_limits::kHardMaximumFadeDelayMilliseconds;
}

static_assert(isValidTrackMarksBudget(kLowTrackMarksBudget));
static_assert(isValidTrackMarksBudget(kMediumTrackMarksBudget));
static_assert(isValidTrackMarksBudget(kHighTrackMarksBudget));

// Quality and performance policy frozen at session start. This owns only
// bounded history/upload cost; visual attachment and feature enablement live
// in their own contracts.
struct TrackMarksPerformanceSettings final {
    uint32_t maximumTrackedObjects =
        track_marks::performance_limits::kHardMaximumStreams;
    container::Array<TrackMarksHistoryBudget, 3> lodBudgets{{
        kLowTrackMarksBudget,
        kMediumTrackMarksBudget,
        kHighTrackMarksBudget,
    }};

    [[nodiscard]] const TrackMarksHistoryBudget& fullDetailBudget() const noexcept {
        return lodBudgets[static_cast<size_t>(TrackMarksStaticLod::High)];
    }
};

struct TrackMarksRenderBudget final {
    uint32_t maximumEdges = kHighTrackMarksBudget.maximumEdges;
    uint32_t opaqueEdges = kHighTrackMarksBudget.opaqueEdges;
    uint32_t fadeDelayMilliseconds =
        kHighTrackMarksBudget.fadeDelayMilliseconds;
    uint32_t maximumTrackedObjects =
        track_marks::performance_limits::kHardMaximumStreams;
};

} // namespace engine
