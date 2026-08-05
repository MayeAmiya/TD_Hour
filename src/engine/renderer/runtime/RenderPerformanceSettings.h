#pragma once

#include <cstddef>
#include <cstdint>
#include "engine/renderer/d3d12/runtime/D3D12PerformanceSettings.h"

namespace engine::render::performance_limits {

// Renderer-internal scheduling policy.  These values affect CPU task
// overhead and worker utilization only; they do not change authored content,
// gameplay limits, render-feature quality, or display/device settings.
inline constexpr uint32_t kMinimumParallelWorkerCount = 1;
inline constexpr size_t kWorldPreparationEntityGrain = 64;
inline constexpr size_t kWorldViewEntityGrain = 256;
inline constexpr float kWorldInterpolationMinimumSnapDistance = 64.0f;
inline constexpr float kWorldInterpolationRadiusSnapFactor = 8.0f;
// Hard frame-arena bound. One joint consumes one 64-byte world matrix and
// one visibility byte. Overflow rejects whole instance palettes in stable
// input order and falls back to the rigid/root path; partial palettes are
// never published.
inline constexpr size_t kMaximumFramePoseJoints = 1u << 20u;
// Pre-reserve one quarter of the hard arena bound in each rotating physical
// slot. At 64 bytes/matrix plus one visibility byte this is about 16.25 MiB
// per slot (roughly 49 MiB across three slots), appropriate for the supported
// 6 GiB system-memory baseline and substantially reduces battle-time growth.
inline constexpr size_t kInitialFramePoseJoints = 1u << 18u;
static_assert(kInitialFramePoseJoints <= kMaximumFramePoseJoints);

inline constexpr size_t kAnimationReadyPublishesPerFrame = 32u;
inline constexpr uint64_t kAnimationReadyBytesPerFrame =
    8ull * 1024ull * 1024ull;
inline constexpr uint64_t kAnimationReadyMicrosecondsPerFrame = 1000u;
inline constexpr size_t kAnimationReadyPublishesPerLoadingFrame = 256u;
inline constexpr uint64_t kAnimationReadyBytesPerLoadingFrame =
    64ull * 1024ull * 1024ull;
inline constexpr uint64_t kAnimationReadyMicrosecondsPerLoadingFrame = 12000u;
inline constexpr size_t kModelReadyPublishesPerFrame = 32u;
inline constexpr uint64_t kModelReadyBytesPerFrame =
    64ull * 1024ull * 1024ull;
inline constexpr uint64_t kModelReadyMicrosecondsPerFrame = 2000u;
inline constexpr size_t kModelReadyPublishesPerLoadingFrame = 256u;
inline constexpr uint64_t kModelReadyBytesPerLoadingFrame =
    256ull * 1024ull * 1024ull;
inline constexpr uint64_t kModelReadyMicrosecondsPerLoadingFrame = 12000u;
inline constexpr size_t kTextureUploadsPerFrame = 8u;
inline constexpr size_t kTextureUploadsPerLoadingFrame = 64u;
inline constexpr uint64_t kTextureUploadBytesPerFrame =
    16ull * 1024ull * 1024ull;
inline constexpr uint64_t kTextureUploadBytesPerLoadingFrame =
    128ull * 1024ull * 1024ull;
inline constexpr uint64_t kTextureUploadMicrosecondsPerFrame = 2000u;
inline constexpr uint64_t kTextureUploadMicrosecondsPerLoadingFrame = 12000u;
// Fence-safe world residency policy. These are operational cache bounds, not
// authored content or gameplay limits. Owners/pins always win over budgets.
// WorldTextureCache owns one SRV per resident variant. Derive its residency
// limit from the shared shader-visible heap budget instead of maintaining a
// second independent entry limit that can overcommit the heap once UI and
// fixed renderer descriptors are present.
inline constexpr size_t kWorldTextureResidentLimit =
    d3d12::performance_limits::kWorldTextureSrvCacheBudget;
inline constexpr uint64_t kWorldTextureResidentBytes =
    1024ull * 1024ull * 1024ull;
inline constexpr size_t kW3dResidentModelLimit = 2048u;
inline constexpr uint64_t kW3dResidentModelBytes =
    1024ull * 1024ull * 1024ull;
inline constexpr uint64_t kRenderAssetResidencyGraceFrames = 300u;
inline constexpr size_t kW3dResidencyEvictionsPerFrame = 16u;
inline constexpr size_t kWorldTextureResidencyEvictionsPerFrame = 32u;
inline constexpr uint64_t kWorldTextureDecodedCpuBytes =
    256ull * 1024ull * 1024ull;
inline constexpr size_t kWorldTextureCpuGcItemsPerFrame = 16u;

} // namespace engine::render::performance_limits
