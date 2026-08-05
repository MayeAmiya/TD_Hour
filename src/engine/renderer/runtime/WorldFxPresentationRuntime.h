#pragma once

#include "core/container/hash_containers.h"
#include "engine/fx/runtime/FxRuntime.h"
#include "engine/renderer/world/effects/TypedFxPresentationOwner.h"
#include "engine/renderer/world/resource/FxSkeletonBindings.h"

#include <cstddef>
#include <cstdint>

namespace engine::d3d12 { class D3D12Device; }
namespace engine::render {
class Skeleton;
class WorldTextureCache;
}

namespace engine::render {

struct WorldFxPresentationRuntime final {
    WorldFxPresentationRuntime(
        d3d12::D3D12Device& device,
        container::SharedPtr<WorldTextureCache> textures);

    void invalidateSkeletonBindings() { skeletonBindings.clear(); }
    [[nodiscard]] FxSkeletonBindings* bindingsFor(
        const container::SharedPtr<const Skeleton>& skeleton);
    [[nodiscard]] size_t namedJoint(
        const container::SharedPtr<const Skeleton>& skeleton,
        container::StringView boneName);
    [[nodiscard]] container::Span<const size_t> numberedJoints(
        const container::SharedPtr<const Skeleton>& skeleton,
        container::StringView bonePrefix);

    TypedFxPresentationOwner typed;
    container::SharedPtr<const fx::ParticleSystemCatalog> particleCatalog;
    container::SharedPtr<const fx::FxListCatalog> listCatalog;
    container::UniquePtr<fx::FxRuntime> runtime;
    // Confirmed FX snapshots may arrive before their world endpoint has
    // finished CPU preparation. Keep the ordered stream renderer-owned and
    // release it only when that simulation frame is actually displayed.
    container::Deque<fx::FxPresentationSnapshot> pendingSnapshots;
    size_t maximumParticles = 8192;
    size_t maximumFieldParticles = 30;
    fx::ParticlePriority minimumParticlePriority =
        fx::ParticlePriority::Invalid;
    fx::ParticlePriority minimumParticleSkipPriority =
        fx::ParticlePriority::Invalid;
    uint32_t particleSkipMask = 0;
    float particleScale = 1.0f;
    size_t initialEmitterCapacity = 256;
    size_t maximumEmitters = 4096;
    size_t maximumAttachedEmitters = 16384;
    size_t maximumPresentationCommands = 16384;
    size_t particleDrawExpansionFactor = 6;
    container::HashMap<uint64_t, FxSkeletonBindings> skeletonBindings;
};

} // namespace engine::render
