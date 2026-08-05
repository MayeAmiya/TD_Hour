#include "engine/renderer/world/particle/ParticleRenderer.h"
#include "engine/renderer/world/particle/ParticleRendererScratch.h"

#include "debug/debug.h"
#include "presentation/render/RenderViewSnapshot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>

namespace engine::render {
using particle_render_detail::Candidate;
using particle_render_detail::SourcePriority;
using particle_render_detail::StreakPoint;
namespace {

constexpr size_t kVolumeParticleDepth =
    particle_render::performance_limits::kMaximumExpansionPerSource;
constexpr float kVolumeParticleShiftScale = 0.1f;
constexpr float kVisibilityEpsilon = 1.0e-5f;
constexpr float kMaximumSmudgeUvOffset = 0.06f;

[[nodiscard]] constexpr fx::GpuParticleCompatibilityReason
normalizedCompatibilityReason(
    fx::GpuParticleCompatibilityReason reason) noexcept {
    return static_cast<size_t>(reason) < static_cast<size_t>(
        fx::GpuParticleCompatibilityReason::Count)
        ? reason : fx::GpuParticleCompatibilityReason::KindInvalid;
}

[[nodiscard]] constexpr ParticleRenderRoute particleRenderRoute(
    fx::ParticleKind kind,
    fx::GpuParticleCompatibilityReason reason) noexcept {
    // Drawable is deliberately a renderer-local diagnostic proxy.  Loading
    // or submitting its authored W3D model belongs to world presentation, not
    // to the particle backend.  The first GPU route is billboard-only even if
    // malformed catalog data claims another kind is compatible.
    return kind == fx::ParticleKind::Billboard &&
        normalizedCompatibilityReason(reason) ==
            fx::GpuParticleCompatibilityReason::Compatible
        ? ParticleRenderRoute::GpuCompatibleReference
        : ParticleRenderRoute::CpuOnly;
}

[[nodiscard]] bool finitePosition(const fx::ParticleVector3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool sameStreak(const StreakPoint& left, const StreakPoint& right) noexcept {
    return left.emitter == right.emitter &&
        left.templateId == right.templateId;
}

[[nodiscard]] bool visibleColor(fx::ParticleShader shader,
                                const float color[4],
                                const float* endColor = nullptr) noexcept {
    const auto either = [color, endColor](size_t channel, auto predicate) {
        return predicate(color[channel]) || (endColor && predicate(endColor[channel]));
    };
    switch (shader) {
    case fx::ParticleShader::Additive:
        return either(0, [](float value) { return value > kVisibilityEpsilon; }) ||
            either(1, [](float value) { return value > kVisibilityEpsilon; }) ||
            either(2, [](float value) { return value > kVisibilityEpsilon; });
    case fx::ParticleShader::Multiply:
        return either(0, [](float value) { return value < 1.0f - kVisibilityEpsilon; }) ||
            either(1, [](float value) { return value < 1.0f - kVisibilityEpsilon; }) ||
            either(2, [](float value) { return value < 1.0f - kVisibilityEpsilon; });
    case fx::ParticleShader::AlphaTest:
        return true;
    case fx::ParticleShader::None:
    case fx::ParticleShader::Alpha:
        return either(3, [](float value) { return value > kVisibilityEpsilon; });
    case fx::ParticleShader::Count:
        return false;
    }
    return false;
}

void packColor(const fx::ParticleRuntimeParticle& particle, float output[4]) noexcept {
    output[0] = std::clamp(particle.red, 0.0f, 1.0f);
    output[1] = std::clamp(particle.green, 0.0f, 1.0f);
    output[2] = std::clamp(particle.blue, 0.0f, 1.0f);
    output[3] = std::clamp(particle.alpha, 0.0f, 1.0f);
}

[[nodiscard]] float distanceSquared(fx::ParticleVector3 position,
                                    math::vec3 cameraPosition) noexcept {
    const float deltaX = position.x - cameraPosition.x();
    const float deltaY = position.y - cameraPosition.y();
    const float deltaZ = position.z - cameraPosition.z();
    return deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
}

[[nodiscard]] bool distanceSorted(fx::ParticleShader shader) noexcept {
    return shader == fx::ParticleShader::None || shader == fx::ParticleShader::Alpha ||
        shader == fx::ParticleShader::Multiply;
}

[[nodiscard]] uint32_t shaderRank(fx::ParticleShader shader) noexcept {
    switch (shader) {
    case fx::ParticleShader::AlphaTest: return 0;
    case fx::ParticleShader::Multiply: return 1;
    case fx::ParticleShader::None:
    case fx::ParticleShader::Alpha: return 2;
    case fx::ParticleShader::Additive: return 3;
    case fx::ParticleShader::Count: return 4;
    }
    return 4;
}


[[nodiscard]] uint64_t mixSmudgeIdentity(uint64_t value) noexcept {
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] float signedSmudgeOffset(uint64_t value) noexcept {
    const float unit = static_cast<float>(value & 0xffffu) / 65535.0f;
    return std::lerp(-kMaximumSmudgeUvOffset,
                     kMaximumSmudgeUvOffset, unit);
}


} // namespace

ParticleRenderDrawList ParticleRenderer::buildDrawList(
    const fx::ParticleRuntime& runtime,
    const fx::ParticleSystemCatalog& catalog,
    math::vec3 cameraPosition,
    size_t maximumInstances,
    float interpolationAlpha,
    size_t maximumSourceParticles,
    const LocalVisibilityRenderSnapshot& localVisibility) {
    ParticleRenderDrawList result;
    buildDrawListInto(
        result, runtime, catalog, cameraPosition, maximumInstances,
        interpolationAlpha, maximumSourceParticles, localVisibility);
    return result;
}

namespace {

void buildDrawListWithScratch(
    ParticleRenderDrawList& result,
    const fx::ParticleRuntime& runtime,
    const fx::ParticleSystemCatalog& catalog,
    math::vec3 cameraPosition,
    size_t maximumInstances,
    float interpolationAlpha,
    size_t maximumSourceParticles,
    const LocalVisibilityRenderSnapshot& localVisibility,
    ParticleRendererScratch& scratch) {
    const auto buildStarted = std::chrono::steady_clock::now();
    const size_t initialSourceOrdinalCapacity =
        scratch.sourceOrdinals.capacity();
    const size_t initialSourcePriorityCapacity =
        scratch.sourcePriorities.capacity();
    const size_t initialCandidateCapacity = scratch.candidates.capacity();
    const size_t initialStreakPointCapacity = scratch.streakPoints.capacity();
    size_t sourcePriorityScratchPeak = 0;
    size_t sourceOrdinalScratchPeak = 0;
    size_t candidateScratchPeak = 0;
    size_t streakPointScratchPeak = 0;

    result.stats = {};
    result.textureBindingGeneration = 0;
    result.interpolationAlpha = std::clamp(
        interpolationAlpha, 0.0f, 1.0f);
    result.instances.clear();
    result.batches.clear();
    result.smudgeInstances.clear();
    result.gpuVisibilityGenerations.clear();
    result.gpuReferenceSampleCount = 0;
    interpolationAlpha = result.interpolationAlpha;

    result.stats.sourceParticles = runtime.particleCount();
    const size_t effectiveMaximumSourceParticles = std::min(
        maximumSourceParticles,
        particle_render::performance_limits::kHardMaximumSourceParticles);
    const bool selectAllSources =
        result.stats.sourceParticles <= effectiveMaximumSourceParticles;
    auto& sourceOrdinals = scratch.sourceOrdinals;
    auto& selection = scratch.sourcePriorities;
    auto& candidates = scratch.candidates;
    auto& streakPoints = scratch.streakPoints;
    sourceOrdinals.clear();
    selection.clear();
    candidates.clear();
    streakPoints.clear();
    const auto appendCandidate = [&result, &candidates](Candidate&& candidate) {
        if (candidates.size() >=
            particle_render::performance_limits::kHardMaximumCandidates) {
            ++result.stats.scratchHardCapRejected;
            ++result.stats.rejectedBudget;
            return;
        }
        // Never let Vector's geometric growth choose a capacity beyond the
        // renderer hard ceiling. Reserve the next bounded step explicitly
        // before push_back can trigger an allocator-defined growth policy.
        if (candidates.size() == candidates.capacity()) {
            const size_t current = candidates.capacity();
            const size_t growth = std::max<size_t>(current / 2u, 1u);
            const size_t next = std::min(
                particle_render::performance_limits::kHardMaximumCandidates,
                current + growth);
            candidates.reserve(std::max(next, candidates.size() + 1u));
        }
        candidates.push_back(std::move(candidate));
    };
    if (!selectAllSources) {
        sourceOrdinals.reserve(effectiveMaximumSourceParticles);
        selection.reserve(std::min(
            result.stats.sourceParticles,
            particle_render::performance_limits::kHardMaximumSourceParticles));
        runtime.visitParticles(
            [&catalog, &selection](size_t sourceOrdinal,
                                   const fx::ParticleRuntimeParticle& particle) {
                if (selection.size() >= particle_render::performance_limits::
                        kHardMaximumSourceParticles) {
                    return;
                }
                const fx::ParticleSystemTemplate* definition =
                    catalog.find(particle.templateId);
                selection.push_back({
                    .sourceOrdinal = sourceOrdinal,
                    .priority = definition
                        ? definition->priority
                        : fx::ParticlePriority::Invalid,
                });
            });
        sourcePriorityScratchPeak = selection.size();
        std::stable_sort(
            selection.begin(), selection.end(),
            [](const SourcePriority& left, const SourcePriority& right) {
                if (left.priority != right.priority) {
                    return left.priority > right.priority;
                }
                return left.sourceOrdinal < right.sourceOrdinal;
            });
        result.stats.rejectedSourceBudget =
            result.stats.sourceParticles -
            std::min(effectiveMaximumSourceParticles, selection.size());
        if (result.stats.sourceParticles >
            particle_render::performance_limits::kHardMaximumSourceParticles) {
            result.stats.scratchHardCapRejected +=
                result.stats.sourceParticles -
                particle_render::performance_limits::kHardMaximumSourceParticles;
        }
        selection.resize(std::min(effectiveMaximumSourceParticles,
                                  selection.size()));
        for (const SourcePriority& selected : selection) {
            sourceOrdinals.push_back(selected.sourceOrdinal);
        }
        sourceOrdinalScratchPeak = sourceOrdinals.size();
        std::sort(sourceOrdinals.begin(), sourceOrdinals.end());
    }
    const auto selectionFinished = std::chrono::steady_clock::now();
    result.stats.sourceSelectionMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            selectionFinished - buildStarted).count());
    const size_t selectedSourceCount = selectAllSources
        ? result.stats.sourceParticles : sourceOrdinals.size();
    candidates.reserve(std::min(
        selectedSourceCount,
        particle_render::performance_limits::kHardMaximumCandidates));
    streakPoints.reserve(std::min(
        selectedSourceCount,
        particle_render::performance_limits::kHardMaximumSourceParticles));
    size_t candidateOrdinal = 0;
    size_t selectedCursor = 0;
    runtime.visitParticles(
        [&](size_t sourceOrdinal,
            const fx::ParticleRuntimeParticle& particle) {
        if (!selectAllSources) {
            if (selectedCursor >= sourceOrdinals.size() ||
                sourceOrdinals[selectedCursor] != sourceOrdinal) {
                return;
            }
            ++selectedCursor;
        }
        const fx::ParticleSystemTemplate* definition = catalog.find(particle.templateId);
        const fx::GpuParticleCompatibilityReason compatibilityReason =
            normalizedCompatibilityReason(definition
                ? definition->gpuCompatibilityReason
                : fx::GpuParticleCompatibilityReason::KindInvalid);
        ++result.stats.compatibilityReasonSourceParticles[
            static_cast<size_t>(compatibilityReason)];
        if (compatibilityReason ==
            fx::GpuParticleCompatibilityReason::Compatible) {
            ++result.stats.gpuCompatibleEligibleSources;
        }
        // DRAWABLE particles are W3D models. Do not substitute a billboard;
        // their renderer-owned static-mesh path is appended separately after
        // this quad draw list has been built.
        if (definition && definition->kind == fx::ParticleKind::Drawable) {
            ++result.stats.routedDrawableParticles;
            return;
        }
        if (!definition ||
            (definition->kind != fx::ParticleKind::Billboard &&
             definition->kind != fx::ParticleKind::Streak &&
             definition->kind != fx::ParticleKind::Volume &&
             definition->kind != fx::ParticleKind::Smudge) ||
            definition->shader == fx::ParticleShader::Count ||
            !std::isfinite(particle.size) ||
            particle.size <= 0.0f ||
            !std::isfinite(particle.alpha) || !std::isfinite(particle.red) ||
            !std::isfinite(particle.green) || !std::isfinite(particle.blue)) {
            ++result.stats.rejectedInvalid;
            return;
        }

        const fx::ParticleVector3 center = {
            std::lerp(particle.previousPosition.x, particle.position.x, interpolationAlpha),
            std::lerp(particle.previousPosition.y, particle.position.y, interpolationAlpha),
            std::lerp(particle.previousPosition.z, particle.position.z, interpolationAlpha),
        };
        if (!finitePosition(center)) {
            ++result.stats.rejectedInvalid;
            return;
        }
        if (!localVisibility.isInsidePlayableBounds(
                {center.x, center.y, center.z})) {
            ++result.stats.rejectedVisibility;
            return;
        }
        float color[4]{};
        packColor(particle, color);
        if (definition->kind == fx::ParticleKind::Streak) {
            if (streakPoints.size() >= particle_render::performance_limits::
                    kHardMaximumSourceParticles) {
                ++result.stats.scratchHardCapRejected;
                return;
            }
            StreakPoint point;
            point.definition = definition;
            point.emitter = particle.emitter;
            point.templateId = particle.templateId;
            point.center = center;
            std::copy(std::begin(color), std::end(color), std::begin(point.color));
            point.size = particle.size;
            point.particleOrdinal = particle.ordinal;
            point.sourceOrdinal = sourceOrdinal;
            streakPoints.push_back(point);
            return;
        }

        if (!visibleColor(definition->shader, color)) {
            ++result.stats.rejectedVisibility;
            ++result.stats.rejectedColor;
            return;
        }
        if (compatibilityReason ==
            fx::GpuParticleCompatibilityReason::Compatible) {
            if (result.gpuReferenceSampleCount <
                result.gpuReferenceSamples.size()) {
                GpuParticleReferenceSample& sample =
                    result.gpuReferenceSamples[
                        result.gpuReferenceSampleCount++];
                sample = {};
                sample.stateSlot = particle.handle.index;
                sample.particleGeneration = particle.handle.generation;
                sample.position[0] = particle.position.x;
                sample.position[1] = particle.position.y;
                sample.position[2] = particle.position.z;
                sample.previousPosition[0] = particle.previousPosition.x;
                sample.previousPosition[1] = particle.previousPosition.y;
                sample.previousPosition[2] = particle.previousPosition.z;
                sample.size = particle.size;
                sample.angle = particle.angle;
                std::copy(
                    std::begin(color), std::end(color),
                    std::begin(sample.color));
            }
        }

        ParticleRenderInstance instance;
        instance.position[0] = center.x;
        instance.position[1] = center.y;
        instance.position[2] = center.z;
        instance.size = particle.size;
        std::copy(std::begin(instance.position), std::end(instance.position),
                  std::begin(instance.endPosition));
        instance.endSize = instance.size;
        std::copy(std::begin(color), std::end(color), std::begin(instance.color));
        std::copy(std::begin(color), std::end(color), std::begin(instance.endColor));
        instance.angleRadians = std::isfinite(particle.angle) ? particle.angle : 0.0f;
        instance.flags = definition->groundAligned ? kParticleRenderGroundAligned : 0u;
        const size_t layerCount = definition->kind == fx::ParticleKind::Volume
            ? kVolumeParticleDepth : 1u;
        fx::ParticleVector3 cameraDirection{
            cameraPosition.x() - center.x,
            cameraPosition.y() - center.y,
            cameraPosition.z() - center.z,
        };
        const float cameraDistanceSquared = cameraDirection.x * cameraDirection.x +
            cameraDirection.y * cameraDirection.y + cameraDirection.z * cameraDirection.z;
        if (cameraDistanceSquared > kVisibilityEpsilon * kVisibilityEpsilon) {
            const float inverseDistance = 1.0f / std::sqrt(cameraDistanceSquared);
            cameraDirection.x *= inverseDistance;
            cameraDirection.y *= inverseDistance;
            cameraDirection.z *= inverseDistance;
        } else {
            cameraDirection = {};
        }

        for (size_t layer = 0; layer < layerCount; ++layer) {
            Candidate candidate;
            candidate.instance = instance;
            if (definition->kind == fx::ParticleKind::Volume) {
                const float shift = static_cast<float>(layer) * instance.size *
                    (kVolumeParticleShiftScale / static_cast<float>(kVolumeParticleDepth));
                candidate.instance.position[0] += cameraDirection.x * shift;
                candidate.instance.position[1] += cameraDirection.y * shift;
                candidate.instance.position[2] += cameraDirection.z * shift;
                std::copy(std::begin(candidate.instance.position),
                          std::end(candidate.instance.position),
                          std::begin(candidate.instance.endPosition));
                candidate.instance.flags |= kParticleRenderVolumeLayer;
            }
            candidate.shader = definition->shader;
            candidate.kind = definition->kind;
            candidate.priority = definition->priority;
            candidate.compatibilityReason = compatibilityReason;
            candidate.route = particleRenderRoute(
                definition->kind, compatibilityReason);
            candidate.textureName = definition->particleName;
            candidate.distanceSquared = distanceSquared({
                candidate.instance.position[0],
                candidate.instance.position[1],
                candidate.instance.position[2],
            }, cameraPosition);
            if (definition->kind == fx::ParticleKind::Smudge) {
                const uint64_t authoredFrame = static_cast<uint64_t>(
                    std::max(std::floor(particle.ageFrames), 0.0f));
                const uint64_t identity = particle.ordinal ^
                    (static_cast<uint64_t>(particle.emitter.index) << 32u) ^
                    particle.emitter.generation ^
                    (authoredFrame * 0x9e3779b97f4a7c15ull);
                const uint64_t mixed = mixSmudgeIdentity(identity);
                candidate.smudgeOffsetX = signedSmudgeOffset(mixed);
                candidate.smudgeOffsetY = signedSmudgeOffset(mixed >> 16u);
            }
            candidate.ordinal = candidateOrdinal++;
            candidate.stateSlot = particle.handle.index;
            candidate.particleGeneration = particle.handle.generation;
            appendCandidate(std::move(candidate));
        }
    });

    std::stable_sort(streakPoints.begin(), streakPoints.end(),
                     [](const StreakPoint& left, const StreakPoint& right) {
        if (left.emitter.index != right.emitter.index) {
            return left.emitter.index < right.emitter.index;
        }
        if (left.emitter.generation != right.emitter.generation) {
            return left.emitter.generation < right.emitter.generation;
        }
        if (left.templateId.value != right.templateId.value) {
            return left.templateId.value < right.templateId.value;
        }
        if (left.particleOrdinal != right.particleOrdinal) {
            return left.particleOrdinal < right.particleOrdinal;
        }
        return left.sourceOrdinal < right.sourceOrdinal;
    });

    size_t groupStart = 0;
    while (groupStart < streakPoints.size()) {
        size_t groupEnd = groupStart + 1u;
        while (groupEnd < streakPoints.size() &&
               sameStreak(streakPoints[groupStart], streakPoints[groupEnd])) {
            ++groupEnd;
        }
        for (size_t pointIndex = groupStart + 1u; pointIndex < groupEnd; ++pointIndex) {
            const StreakPoint& start = streakPoints[pointIndex - 1u];
            const StreakPoint& end = streakPoints[pointIndex];
            Candidate candidate;
            candidate.instance.position[0] = start.center.x;
            candidate.instance.position[1] = start.center.y;
            candidate.instance.position[2] = start.center.z;
            candidate.instance.size = start.size;
            candidate.instance.endPosition[0] = end.center.x;
            candidate.instance.endPosition[1] = end.center.y;
            candidate.instance.endPosition[2] = end.center.z;
            candidate.instance.endSize = end.size;
            std::copy(std::begin(start.color), std::end(start.color),
                      std::begin(candidate.instance.color));
            std::copy(std::begin(end.color), std::end(end.color),
                      std::begin(candidate.instance.endColor));
            candidate.instance.flags = kParticleRenderStreak |
                (start.definition->groundAligned ? kParticleRenderGroundAligned : 0u);
            if (!visibleColor(start.definition->shader, candidate.instance.color,
                              candidate.instance.endColor)) {
                ++result.stats.rejectedVisibility;
                ++result.stats.rejectedColor;
                continue;
            }
            candidate.shader = start.definition->shader;
            candidate.kind = fx::ParticleKind::Streak;
            candidate.priority = start.definition->priority;
            candidate.compatibilityReason = normalizedCompatibilityReason(
                start.definition->gpuCompatibilityReason);
            candidate.route = particleRenderRoute(
                fx::ParticleKind::Streak,
                candidate.compatibilityReason);
            candidate.textureName = start.definition->particleName;
            const fx::ParticleVector3 midpoint{
                (start.center.x + end.center.x) * 0.5f,
                (start.center.y + end.center.y) * 0.5f,
                (start.center.z + end.center.z) * 0.5f,
            };
            candidate.distanceSquared = distanceSquared(midpoint, cameraPosition);
            candidate.ordinal = candidateOrdinal++;
            appendCandidate(std::move(candidate));
        }
        groupStart = groupEnd;
    }

    candidateScratchPeak = candidates.size();
    streakPointScratchPeak = streakPoints.size();
    const auto expansionFinished = std::chrono::steady_clock::now();
    result.stats.expansionMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            expansionFinished - selectionFinished).count());

    const size_t effectiveMaximumInstances = std::min(
        maximumInstances,
        particle_render::performance_limits::kHardMaximumCandidates);
    if (candidates.size() > effectiveMaximumInstances) {
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Candidate& left, const Candidate& right) {
            if (left.priority != right.priority) return left.priority > right.priority;
            return left.ordinal < right.ordinal;
        });
        result.stats.rejectedBudget +=
            candidates.size() - effectiveMaximumInstances;
        candidates.resize(effectiveMaximumInstances);
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& left, const Candidate& right) {
        const uint32_t leftRank = shaderRank(left.shader);
        const uint32_t rightRank = shaderRank(right.shader);
        if (leftRank != rightRank) return leftRank < rightRank;
        if (distanceSorted(left.shader) && left.distanceSquared != right.distanceSquared) {
            return left.distanceSquared > right.distanceSquared;
        }
        if (!distanceSorted(left.shader) && left.textureName != right.textureName) {
            return left.textureName < right.textureName;
        }
        if (left.route != right.route) {
            return left.route < right.route;
        }
        return left.ordinal < right.ordinal;
    });
    const auto sortFinished = std::chrono::steady_clock::now();
    result.stats.sortMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            sortFinished - expansionFinished).count());

    result.instances.reserve(candidates.size());
    result.smudgeInstances.reserve(candidates.size());
    result.batches.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        ++result.stats.compatibilityReasonSelectedInstances[
            static_cast<size_t>(candidate.compatibilityReason)];
        if (candidate.route ==
            ParticleRenderRoute::GpuCompatibleReference) {
            ++result.stats.gpuCompatibleSelectedInstances;
            result.gpuVisibilityGenerations.push_back({
                .stateSlot = candidate.stateSlot,
                .particleGeneration = candidate.particleGeneration,
            });
        }
        if (candidate.kind == fx::ParticleKind::Smudge) {
            SmudgeRenderInstance smudge;
            std::copy(std::begin(candidate.instance.position),
                      std::end(candidate.instance.position),
                      std::begin(smudge.position));
            smudge.size = candidate.instance.size;
            smudge.opacity = candidate.instance.color[3];
            smudge.uvOffset[0] = candidate.smudgeOffsetX;
            smudge.uvOffset[1] = candidate.smudgeOffsetY;
            result.smudgeInstances.push_back(smudge);
            ++result.stats.smudgeInstances;
            continue;
        }
        const uint32_t instanceIndex = static_cast<uint32_t>(result.instances.size());
        result.instances.push_back(candidate.instance);
        switch (candidate.kind) {
        case fx::ParticleKind::Billboard: ++result.stats.billboardInstances; break;
        case fx::ParticleKind::Streak: ++result.stats.streakInstances; break;
        case fx::ParticleKind::Volume: ++result.stats.volumeInstances; break;
        case fx::ParticleKind::Drawable: break;
        case fx::ParticleKind::None:
        case fx::ParticleKind::Count:
            break;
        }
        if (result.batches.empty() ||
            result.batches.back().shader != candidate.shader ||
            result.batches.back().route != candidate.route ||
            result.batches.back().textureName != candidate.textureName) {
            result.batches.push_back({
                .shader = candidate.shader,
                .route = candidate.route,
                .textureName = container::String(candidate.textureName),
                .firstInstance = instanceIndex,
                .instanceCount = 1,
            });
        } else {
            ++result.batches.back().instanceCount;
        }
    }
    result.stats.packMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - sortFinished).count());

#if TD_DEBUG_ENABLED
    // Keep the evidence at the instance/GPU boundary: successful emitter
    // creation alone cannot prove that finite, camera-near billboards were
    // packed. Sample transitions and a bounded periodic interval only.
    static thread_local uint64_t debugParticleFrame = 0;
    static thread_local bool debugHadParticles = false;
    ++debugParticleFrame;
    const bool hasParticles = !result.instances.empty();
    const bool sampleParticles = hasParticles &&
        (!debugHadParticles || debugParticleFrame % 30u == 0u);
    if (sampleParticles) {
        float minimum[3]{
            result.instances.front().position[0],
            result.instances.front().position[1],
            result.instances.front().position[2]};
        float maximum[3]{minimum[0], minimum[1], minimum[2]};
        float minimumSize = result.instances.front().size;
        float maximumSize = minimumSize;
        float minimumDistanceSquared = distanceSquared(
            {minimum[0], minimum[1], minimum[2]}, cameraPosition);
        float maximumDistanceSquared = minimumDistanceSquared;
        for (const ParticleRenderInstance& instance : result.instances) {
            for (size_t axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], instance.position[axis]);
                maximum[axis] = std::max(maximum[axis], instance.position[axis]);
            }
            minimumSize = std::min(minimumSize, instance.size);
            maximumSize = std::max(maximumSize, instance.size);
            const float instanceDistanceSquared = distanceSquared(
                {instance.position[0], instance.position[1],
                 instance.position[2]}, cameraPosition);
            minimumDistanceSquared = std::min(
                minimumDistanceSquared, instanceDistanceSquared);
            maximumDistanceSquared = std::max(
                maximumDistanceSquared, instanceDistanceSquared);
        }
        container::String batchTextures;
        const size_t textureSampleCount = std::min<size_t>(
            result.batches.size(), 8u);
        for (size_t index = 0; index < textureSampleCount; ++index) {
            if (!batchTextures.empty()) batchTextures += ',';
            batchTextures += result.batches[index].textureName;
        }
        TD_LOG_DEBUG(
            "[ParticleRenderer] packed instances={} batches={} camera=({}, {}, {}) bounds=({}, {}, {})..({}, {}, {}) size={}..{} distance={}..{} textures=[{}]",
            result.instances.size(), result.batches.size(),
            cameraPosition.x(), cameraPosition.y(), cameraPosition.z(),
            minimum[0], minimum[1], minimum[2], maximum[0], maximum[1],
            maximum[2], minimumSize, maximumSize,
            std::sqrt(std::max(0.0f, minimumDistanceSquared)),
            std::sqrt(std::max(0.0f, maximumDistanceSquared)),
            batchTextures);
    } else if (!hasParticles && debugHadParticles) {
        TD_LOG_DEBUG("[ParticleRenderer] packed particle set became empty");
    }
    debugHadParticles = hasParticles;
#endif

    scratch.sourcePriorityHighWater = std::max(
        scratch.sourcePriorityHighWater, sourcePriorityScratchPeak);
    scratch.sourceOrdinalHighWater = std::max(
        scratch.sourceOrdinalHighWater, sourceOrdinalScratchPeak);
    scratch.candidateHighWater = std::max(
        scratch.candidateHighWater, candidateScratchPeak);
    scratch.streakPointHighWater = std::max(
        scratch.streakPointHighWater, streakPointScratchPeak);
    result.stats.sourcePriorityScratchCapacity = selection.capacity();
    result.stats.sourceOrdinalScratchCapacity = sourceOrdinals.capacity();
    result.stats.candidateScratchCapacity = candidates.capacity();
    result.stats.streakPointScratchCapacity = streakPoints.capacity();
    result.stats.sourcePriorityScratchHighWater =
        scratch.sourcePriorityHighWater;
    result.stats.sourceOrdinalScratchHighWater =
        scratch.sourceOrdinalHighWater;
    result.stats.candidateScratchHighWater = scratch.candidateHighWater;
    result.stats.streakPointScratchHighWater = scratch.streakPointHighWater;

    const auto recordContainerReuse = [&result](size_t initialCapacity,
                                                size_t finalCapacity,
                                                bool used) {
        if (finalCapacity > initialCapacity) {
            ++result.stats.scratchCapacityGrowths;
        } else if (used && initialCapacity != 0) {
            ++result.stats.scratchContainersReused;
        }
    };
    recordContainerReuse(initialSourcePriorityCapacity, selection.capacity(),
                         sourcePriorityScratchPeak != 0);
    recordContainerReuse(initialSourceOrdinalCapacity, sourceOrdinals.capacity(),
                         sourceOrdinalScratchPeak != 0);
    recordContainerReuse(initialCandidateCapacity, candidates.capacity(),
                         candidateScratchPeak != 0);
    recordContainerReuse(initialStreakPointCapacity, streakPoints.capacity(),
                         streakPointScratchPeak != 0);

    // Candidate texture views and StreakPoint definition pointers are valid
    // only for this call. Clear every retained table before returning while
    // preserving their bounded capacities for the next frame.
    candidates.clear();
    streakPoints.clear();
    selection.clear();
    sourceOrdinals.clear();
}

} // namespace

void ParticleRenderer::buildDrawListInto(
    ParticleRenderDrawList& result,
    const fx::ParticleRuntime& runtime,
    const fx::ParticleSystemCatalog& catalog,
    math::vec3 cameraPosition,
    size_t maximumInstances,
    float interpolationAlpha,
    size_t maximumSourceParticles,
    const LocalVisibilityRenderSnapshot& localVisibility) {
    ParticleRendererScratch scratch;
    buildDrawListWithScratch(
        result, runtime, catalog, cameraPosition, maximumInstances,
        interpolationAlpha, maximumSourceParticles, localVisibility, scratch);
}

void ParticleRenderer::buildDrawListIntoRetained(
    ParticleRenderDrawList& result,
    const fx::ParticleRuntime& runtime,
    const fx::ParticleSystemCatalog& catalog,
    math::vec3 cameraPosition,
    size_t maximumInstances,
    float interpolationAlpha,
    size_t maximumSourceParticles,
    const LocalVisibilityRenderSnapshot& localVisibility) {
    if (!m_buildScratch) {
        m_buildScratch = std::make_unique<ParticleRendererScratch>();
    }
    buildDrawListWithScratch(
        result, runtime, catalog, cameraPosition, maximumInstances,
        interpolationAlpha, maximumSourceParticles, localVisibility,
        *m_buildScratch);
}


} // namespace engine::render
