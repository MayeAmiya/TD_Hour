#pragma once

#include "engine/renderer/world/particle/ParticleRenderer.h"

namespace engine::render::particle_render_detail {

struct SmudgeVertex final {
    float position[3]{};
    float uvOffset[2]{};
    float alpha = 0.0f;
};

static_assert(sizeof(SmudgeVertex) == 24);

struct Candidate final {
    ParticleRenderInstance instance;
    fx::ParticleShader shader = fx::ParticleShader::None;
    fx::ParticleKind kind = fx::ParticleKind::None;
    fx::ParticlePriority priority = fx::ParticlePriority::Invalid;
    fx::GpuParticleCompatibilityReason compatibilityReason =
        fx::GpuParticleCompatibilityReason::KindInvalid;
    ParticleRenderRoute route = ParticleRenderRoute::CpuOnly;
    // Valid only while the catalog passed to buildDrawList is alive. The
    // retained scratch is explicitly cleared before the call returns.
    container::StringView textureName;
    float distanceSquared = 0.0f;
    size_t ordinal = 0;
    uint32_t stateSlot = UINT32_MAX;
    uint32_t particleGeneration = 0;
    float smudgeOffsetX = 0.0f;
    float smudgeOffsetY = 0.0f;
};

struct StreakPoint final {
    const fx::ParticleSystemTemplate* definition = nullptr;
    fx::ParticleEmitterHandle emitter;
    fx::ParticleTemplateId templateId;
    fx::ParticleVector3 center;
    float color[4]{};
    float size = 0.0f;
    uint64_t particleOrdinal = 0;
    size_t sourceOrdinal = 0;
};

struct SourcePriority final {
    size_t sourceOrdinal = 0;
    fx::ParticlePriority priority = fx::ParticlePriority::Invalid;
};

} // namespace engine::render::particle_render_detail

namespace engine::render {

struct ParticleRendererScratch final {
    container::Vector<size_t> sourceOrdinals;
    container::Vector<particle_render_detail::SourcePriority> sourcePriorities;
    container::Vector<particle_render_detail::Candidate> candidates;
    container::Vector<particle_render_detail::StreakPoint> streakPoints;
    // Smudge expansion is frame-local CPU staging. Capacity remains owned by
    // the renderer so stable heat-effect workloads do not allocate per frame.
    container::Vector<particle_render_detail::SmudgeVertex> smudgeVertices;
    size_t sourceOrdinalHighWater = 0;
    size_t sourcePriorityHighWater = 0;
    size_t candidateHighWater = 0;
    size_t streakPointHighWater = 0;
};

} // namespace engine::render
