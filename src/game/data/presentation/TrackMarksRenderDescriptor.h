#pragma once

#include "core/container/container_types.h"

#include "presentation/render/TrackMarksPerformanceSettings.h"
#include "presentation/render/TrackMarksVisualSettings.h"
namespace engine {

// Session-frozen composition root. Feature behavior and performance/quality
// policy remain strongly separated even though loaders expose one value.
struct TrackMarksPresentationSettings final {
    TrackMarksFeatureSettings feature;
    TrackMarksPerformanceSettings performance;
};

// Final value at the game/render boundary. Callers cannot accidentally treat
// an authored texture/bone as a budget, or a history cap as visual behavior.
struct TrackMarksRenderDescriptor final {
    TrackMarksRenderVisualDescriptor visual;
    TrackMarksRenderBudget performance;
};

[[nodiscard]] TrackMarksRenderDescriptor compileTrackMarksRenderDescriptor(
    const game::TrackMarksVisualDescriptor& visual,
    const TrackMarksPresentationSettings& settings) noexcept;

// Sparse layer application helpers. Diagnostics are stable presentation
// warnings: malformed fields keep the previous layer's value; unsafe numeric
// values are clamped before they can size or divide renderer history.
[[nodiscard]] bool applyTrackMarksGameDataIni(
    container::StringView content,
    TrackMarksPresentationSettings& settings,
    container::Vector<container::String>* diagnostics = nullptr,
    container::String* error = nullptr);
[[nodiscard]] bool applyTrackMarksGameLodIni(
    container::StringView content,
    TrackMarksPresentationSettings& settings,
    container::Vector<container::String>* diagnostics = nullptr,
    container::String* error = nullptr);

} // namespace engine
