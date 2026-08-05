#pragma once

#include "core/container/container_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::render {

// Cross-cache observation vocabulary only. The type-specific caches retain
// ownership, state machines and thread rules; this layer never becomes a
// mutable central cache and never carries parser, ECS or D3D12 pointers.
enum class RenderAssetKind : uint8_t {
    Model,
    Hierarchy,
    Animation,
    TextureSource,
    TextureVariant,
    UiTexture,
    Glyph,
    Count,
};

enum class RenderAssetLifecycleState : uint8_t {
    Requested,
    IoQueued,
    IoInFlight,
    CpuReady,
    GpuQueued,
    GpuInFlight,
    GpuResident,
    Fallback,
    Failed,
    Count,
};

enum class RenderAssetErrorKind : uint8_t {
    None,
    Resolve,
    Io,
    Decode,
    Parse,
    Build,
    Upload,
    Dependency,
    Cancelled,
    Stale,
};

enum class RenderAssetDependencyPolicy : uint8_t {
    Required,
    Optional,
    FallbackAllowed,
};

enum class RenderAssetDependencyResolution : uint8_t {
    Referenced,
    Ready,
    Missing,
    Fallback,
    CycleRejected,
    DepthRejected,
};

struct RenderAssetDependencyEdge final {
    uint32_t sourceNode = 0;
    uint32_t targetNode = 0;
    RenderAssetDependencyPolicy policy =
        RenderAssetDependencyPolicy::Required;
    RenderAssetDependencyResolution resolution =
        RenderAssetDependencyResolution::Referenced;
};

// On-demand diagnostic value. These strings are never embedded in the
// per-frame aggregate below; callers ask the owning cache for one record only
// when a handle/source needs explanation.
struct RenderAssetIdentity final {
    RenderAssetKind kind = RenderAssetKind::Model;
    container::String logicalName;
    container::String canonicalSource;
    container::String variant;
    uint64_t generation = 0;
    uint64_t revision = 0;

    // generation identifies a cache/content-mount incarnation; revision
    // identifies content replacement within it. GPU residency incarnations
    // and descriptor indices are intentionally not part of this value.
    friend bool operator==(
        const RenderAssetIdentity&, const RenderAssetIdentity&) = default;
};

struct RenderAssetLifecycleRecord final {
    RenderAssetIdentity identity;
    RenderAssetLifecycleState state =
        RenderAssetLifecycleState::Requested;
    RenderAssetErrorKind errorKind = RenderAssetErrorKind::None;
    container::String diagnostic;
    uint32_t ownerReferences = 0;
};

inline constexpr size_t kRenderAssetKindCount =
    static_cast<size_t>(RenderAssetKind::Count);
inline constexpr size_t kRenderAssetLifecycleStateCount =
    static_cast<size_t>(RenderAssetLifecycleState::Count);

struct RenderAssetKindLifecycleStats final {
    uint32_t tracked = 0;
    uint32_t ownerReferences = 0;
    std::array<uint32_t, kRenderAssetLifecycleStateCount> currentStates{};
    uint64_t requests = 0;
    uint64_t cacheHits = 0;
    uint64_t cacheMisses = 0;
    uint64_t published = 0;
    uint64_t failures = 0;
    uint64_t cancelledQueued = 0;
    uint64_t cancelledPending = 0;
    uint64_t staleRejected = 0;
    uint64_t evictions = 0;
    uint64_t resets = 0;
    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
};

struct RenderAssetDependencyLifecycleStats final {
    uint32_t nodes = 0;
    uint32_t edges = 0;
    uint32_t requiredEdges = 0;
    uint32_t optionalEdges = 0;
    uint32_t fallbackAllowedEdges = 0;
    uint32_t missingRequired = 0;
    uint32_t fallbackResolved = 0;
    uint32_t cycleRejected = 0;
    uint32_t depthRejected = 0;
    uint32_t sharedTargets = 0;
    uint64_t incomingReferences = 0;
};

// Fixed-size per-frame snapshot. Identity/error strings remain behind
// explicit cache diagnostic queries and are never copied into every frame.
struct RenderAssetLifecycleStats final {
    std::array<RenderAssetKindLifecycleStats, kRenderAssetKindCount> kinds{};
    RenderAssetDependencyLifecycleStats dependencies;

    [[nodiscard]] const RenderAssetKindLifecycleStats& operator[](
        RenderAssetKind kind) const noexcept {
        return kinds[static_cast<size_t>(kind)];
    }
    [[nodiscard]] RenderAssetKindLifecycleStats& operator[](
        RenderAssetKind kind) noexcept {
        return kinds[static_cast<size_t>(kind)];
    }
};

} // namespace engine::render
