#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::d3d12::performance_limits {

// Baseline target is at least 6 GiB system memory / 3 GiB VRAM. An 8192-entry
// shader-visible heap is still far below D3D12 tier limits and avoids turning
// descriptor admission into an artificial bottleneck before byte residency.
inline constexpr uint32_t kSrvDescriptorCapacity = 8192;
// The shader-visible heap is shared by world texture residency, UI texture
// and glyph caches, and a small set of fixed/dynamic renderer descriptors
// (post-process, shadows, particle state, etc.). Keep an explicit reserve so
// cache budgets cannot add up to the physical heap size and leave no room for
// those non-cache descriptors or fence-retiring slots.
inline constexpr uint32_t kSrvFixedDescriptorReserve = 512;
inline constexpr uint32_t kSrvPressureWarningPercent = 85;
inline constexpr uint32_t kSrvPressureCriticalPercent = 95;
inline constexpr uint32_t kSrvWarningHighWaterStep = 128;
// UI textures and glyphs publish explicit destruction invalidations. Keep a
// bounded idle fallback as well, so a notification allocation failure or an
// abandoned producer cannot accumulate descriptors until device shutdown.
// Pruning itself is amortized rather than run every frame.
inline constexpr uint64_t kUiSrvIdleLifetimeFrames = 1800;
inline constexpr uint64_t kUiSrvPruneIntervalFrames = 60;
// UI shares the shader-visible heap with world rendering. These
// operational budgets bound cache residency while leaving most descriptors
// for world textures; they are not content/visual semantics.
inline constexpr size_t kUiTextureSrvCacheBudget = 512;
inline constexpr size_t kUiGlyphSrvCacheBudget = 1024;
static_assert(
    kSrvDescriptorCapacity >= kUiTextureSrvCacheBudget +
        kUiGlyphSrvCacheBudget + kSrvFixedDescriptorReserve);
inline constexpr size_t kWorldTextureSrvCacheBudget =
    kSrvDescriptorCapacity - kUiTextureSrvCacheBudget -
    kUiGlyphSrvCacheBudget - kSrvFixedDescriptorReserve;
// A pressure eviction cannot be reused until the two-frame device fence has
// completed. Suppress cache-miss uploads for one additional frame so churn
// cannot consume fresh descriptors faster than retirement catches up.
inline constexpr uint64_t kUiSrvPressureRetirementFrames = 3;

// Shared per-frame dynamic GPU upload arena. These are renderer performance
// budgets rather than authored/gameplay limits: the primary slice is always
// present, while fence-scoped spill pages are retained per backbuffer and may
// grow only up to the explicit cap.
inline constexpr uint32_t kFrameUploadPrimaryBytes = 4u * 1024u * 1024u;
inline constexpr uint32_t kFrameUploadSpillPageBytes = 4u * 1024u * 1024u;
inline constexpr uint32_t kMaximumFrameUploadSpillBytes = 64u * 1024u * 1024u;

// Experimental GPU particle presentation qualifications. This is a renderer
// validation window, not an authored particle or gameplay limit. Two seconds
// at 60 Hz catches both backbuffer slots and normal birth/retire churn without
// permitting a single matching frame to approve the count contract.
inline constexpr uint32_t kGpuParticleCountParityRequiredSamples = 120;
inline constexpr uint32_t kGpuParticleStateParityRequiredSamples = 120;
inline constexpr uint32_t kGpuParticleAbStateSampleCapacity = 32;

static_assert(kFrameUploadSpillPageBytes <= kMaximumFrameUploadSpillBytes);

[[nodiscard]] constexpr uint32_t percentageOf(
    uint32_t value, uint32_t capacity) noexcept {
    return capacity == 0
        ? 0u
        : static_cast<uint32_t>(
              static_cast<uint64_t>(value) * 100u / capacity);
}

static_assert(percentageOf(0, kSrvDescriptorCapacity) == 0);
static_assert(percentageOf(6964, kSrvDescriptorCapacity) == 85);
static_assert(percentageOf(7783, kSrvDescriptorCapacity) == 95);
static_assert(
    kWorldTextureSrvCacheBudget + kUiTextureSrvCacheBudget +
        kUiGlyphSrvCacheBudget + kSrvFixedDescriptorReserve <=
    kSrvDescriptorCapacity);

} // namespace engine::d3d12::performance_limits
