#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::render {

enum class GpuParticlePresentationRejection : uint32_t {
    None = 0,
    NotRequested = 1u << 0u,
    InfrastructureUnavailable = 1u << 1u,
    GraphicsUnavailable = 1u << 2u,
    VisibilityContractIncomplete = 1u << 3u,
    OutputParityUnverified = 1u << 4u,
    ProfileUnapproved = 1u << 5u,
    DynamicBudgetReduced = 1u << 6u,
    BelowProfileThreshold = 1u << 7u,
    MaterialMappingIncomplete = 1u << 8u,
    CountParityUnverified = 1u << 9u,
    StateParityUnverified = 1u << 10u,
    VisibilityParityUnverified = 1u << 11u,
};

[[nodiscard]] constexpr GpuParticlePresentationRejection operator|(
    GpuParticlePresentationRejection left,
    GpuParticlePresentationRejection right) noexcept {
    return static_cast<GpuParticlePresentationRejection>(
        static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

constexpr GpuParticlePresentationRejection& operator|=(
    GpuParticlePresentationRejection& left,
    GpuParticlePresentationRejection right) noexcept {
    left = left | right;
    return left;
}

struct GpuParticlePresentationGateInput final {
    bool requested = false;
    bool infrastructureReady = false;
    bool graphicsReady = false;
    bool visibilityContractReady = false;
    bool outputParityVerified = false;
    bool countParityVerified = false;
    bool stateParityVerified = false;
    bool visibilityParityVerified = false;
    bool profileApproved = false;
    bool materialMappingComplete = false;
    size_t particleCount = 0;
    size_t particleBudget = 0;
    size_t profileMinimumParticleCount = 0;
};

struct GpuParticlePresentationGateDecision final {
    GpuParticlePresentationRejection rejection =
        GpuParticlePresentationRejection::None;
    bool effectiveGpuPresentation = false;
};

[[nodiscard]] constexpr GpuParticlePresentationGateDecision
evaluateGpuParticlePresentationGate(
    const GpuParticlePresentationGateInput& input) noexcept {
    GpuParticlePresentationRejection rejection =
        GpuParticlePresentationRejection::None;
    if (!input.requested) {
        rejection |= GpuParticlePresentationRejection::NotRequested;
    }
    if (!input.infrastructureReady) {
        rejection |=
            GpuParticlePresentationRejection::InfrastructureUnavailable;
    }
    if (!input.graphicsReady) {
        rejection |= GpuParticlePresentationRejection::GraphicsUnavailable;
    }
    if (!input.visibilityContractReady) {
        rejection |=
            GpuParticlePresentationRejection::VisibilityContractIncomplete;
    }
    if (!input.outputParityVerified) {
        rejection |=
            GpuParticlePresentationRejection::OutputParityUnverified;
    }
    if (!input.countParityVerified) {
        rejection |=
            GpuParticlePresentationRejection::CountParityUnverified;
    }
    if (!input.stateParityVerified) {
        rejection |=
            GpuParticlePresentationRejection::StateParityUnverified;
    }
    if (!input.visibilityParityVerified) {
        rejection |=
            GpuParticlePresentationRejection::VisibilityParityUnverified;
    }
    if (!input.profileApproved) {
        rejection |= GpuParticlePresentationRejection::ProfileUnapproved;
    }
    if (!input.materialMappingComplete) {
        rejection |=
            GpuParticlePresentationRejection::MaterialMappingIncomplete;
    }
    if (input.particleBudget < input.particleCount) {
        rejection |=
            GpuParticlePresentationRejection::DynamicBudgetReduced;
    }
    if (input.particleCount < input.profileMinimumParticleCount) {
        rejection |=
            GpuParticlePresentationRejection::BelowProfileThreshold;
    }
    return {
        .rejection = rejection,
        .effectiveGpuPresentation =
            rejection == GpuParticlePresentationRejection::None,
    };
}

} // namespace engine::render
