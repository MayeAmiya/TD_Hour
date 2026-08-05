#pragma once

#include "RenderPerformanceSettings.h"
#include "core/platform/runtime_threads.h"

#include <taskflow/taskflow.hpp>

#include <algorithm>
#include <cstdint>
#include <thread>

namespace engine::render {

// Renderer extraction/pose work, CPU particle integration and command-list
// recording share one bounded renderer-domain executor. Background decoders
// and foreground terrain construction own different queues within the same
// total hardware budget, so neither can starve a render frame.
[[nodiscard]] inline uint32_t parallelWorkerCount() noexcept {
    return std::max(performance_limits::kMinimumParallelWorkerCount,
                    platform::runtime::renderWorkerCount());
}

[[nodiscard]] inline tf::Executor& parallelExecutor() {
    return platform::runtime::renderWorkerExecutor();
}

} // namespace engine::render
