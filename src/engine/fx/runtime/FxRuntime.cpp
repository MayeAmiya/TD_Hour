#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "FxRuntime.h"
#include "FxRuntimeMath.h"
#include "presentation/render/DynamicLightVisualSettings.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <variant>

namespace engine::fx {
namespace {

using runtime_detail::rotateEuler;
using runtime_detail::sampleGroundHeight;

constexpr float kTau = 6.28318530717958647692f;
constexpr uint64_t kSeedStride = 0x9e3779b97f4a7c15ull;
constexpr uint32_t kMaximumFxRecursionDepth = 16;
constexpr uint32_t kMaximumExpandedNuggetsPerInvocation = 4096;
constexpr uint32_t kMaximumEmittersPerNugget = 4096;
constexpr size_t kHardMaximumAttachedEmitters = 65536;
constexpr size_t kHardMaximumPendingPresentationCommands = 65536;
constexpr size_t kRecentInvocationWindow = 65536;

[[nodiscard]] uint64_t mixSeed(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t stableTextHash(container::StringView value) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

[[nodiscard]] float unitRandom(uint64_t seed, uint64_t stream) noexcept {
    constexpr float inverseTwentyFourBits = 1.0f / 16777216.0f;
    return static_cast<float>(mixSeed(seed + stream * kSeedStride) >> 40u) *
        inverseTwentyFourBits;
}

[[nodiscard]] float sampleRange(const ParticleRange& range, uint64_t seed,
                                uint64_t stream) noexcept {
    return range.minimum + (range.maximum - range.minimum) * unitRandom(seed, stream);
}

[[nodiscard]] ParticleVector3 add(ParticleVector3 left, ParticleVector3 right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] ParticleVector3 positionOf(const FxPresentationAnchor& anchor) noexcept {
    return anchor.position;
}

[[nodiscard]] const FxPresentationAnchor* findObject(
    const container::Vector<FxPresentationAnchor>& objects, uint64_t objectKey) noexcept {
    const auto found = std::lower_bound(
        objects.begin(), objects.end(), objectKey,
        [](const FxPresentationAnchor& candidate, uint64_t key) {
            return candidate.objectKey < key;
        });
    return found != objects.end() && found->objectKey == objectKey ? &*found : nullptr;
}

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] uint32_t delayFramesFromMilliseconds(float milliseconds) noexcept {
    if (!(milliseconds > 0.0f)) return 0;
    const double frames = std::ceil(static_cast<double>(milliseconds) * 30.0 / 1000.0);
    return frames >= static_cast<double>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(frames);
}

[[nodiscard]] FxCommandIdentity commandIdentity(
    const FxPresentationInvocation& invocation) noexcept {
    return {
        .eventId = invocation.eventId,
        .confirmedFrame = invocation.confirmedFrame,
        .variationSeed = invocation.variationSeed,
    };
}

[[nodiscard]] FxPresentationAnchor translatedAnchor(
    FxPresentationAnchor anchor, ParticleVector3 localOffset) noexcept {
    anchor.position = add(anchor.position, rotateEuler(
        localOffset, -anchor.rollRadians, anchor.pitchRadians,
        anchor.yawRadians));
    return anchor;
}

[[nodiscard]] FxTypedAnchor translatedTypedAnchor(
    FxTypedAnchor anchor, ParticleVector3 localOffset) {
    std::visit([localOffset](auto& typed) {
        if constexpr (std::is_same_v<std::decay_t<decltype(typed)>,
                                     FxWorldPositionAnchor>) {
            typed.world = translatedAnchor(typed.world, localOffset);
        } else {
            typed.fallback = translatedAnchor(typed.fallback, localOffset);
        }
    }, anchor);
    return anchor;
}

[[nodiscard]] FxWorldPositionAnchor detachedWorldAnchor(
    const FxTypedAnchor& anchor) noexcept {
    return {.world = worldTransform(anchor)};
}

[[nodiscard]] FxTypedAnchor directionalEndpoint(
    const FxTypedAnchor& start, float distance) noexcept {
    FxPresentationAnchor endpoint = worldTransform(start);
    const float finiteDistance = std::isfinite(distance) ? distance : 0.0f;
    endpoint.position.x += std::cos(endpoint.yawRadians) * finiteDistance;
    endpoint.position.y += std::sin(endpoint.yawRadians) * finiteDistance;
    return FxWorldPositionAnchor{.world = endpoint};
}

void appendBonePoseDemand(
    container::Vector<FxBonePoseDemand>& output,
    FxBonePoseDemand demand) {
    if (!demand.valid() ||
        output.size() >= kMaximumFxBonePoseDemands) return;
    output.push_back(std::move(demand));
}

void appendTypedAnchorBoneDemand(
    container::Vector<FxBonePoseDemand>& output,
    const FxTypedAnchor& anchor) {
    const auto* bone = std::get_if<FxBoneAnchor>(&anchor);
    if (!bone || bone->objectKey == 0 || bone->boneName.empty()) return;
    appendBonePoseDemand(output, {
        .objectKey = bone->objectKey,
        .boneName = bone->boneName,
    });
}

} // namespace

const FxPresentationAnchor& worldTransform(
    const FxTypedAnchor& anchor) noexcept {
    return std::visit([](const auto& typed) -> const FxPresentationAnchor& {
        if constexpr (std::is_same_v<std::decay_t<decltype(typed)>,
                                     FxWorldPositionAnchor>) {
            return typed.world;
        } else {
            return typed.fallback;
        }
    }, anchor);
}

FxRuntime::FxRuntime(container::SharedPtr<const ParticleSystemCatalog> particles,
                     container::SharedPtr<const FxListCatalog> fxLists,
                     size_t maximumParticles,
                     size_t initialEmitterCapacity,
                     size_t maximumEmitters,
                     ParticleAdmissionSettings particleAdmission,
                     size_t maximumAttachedEmitters,
                     size_t maximumPresentationCommands)
    : m_particleCatalog(std::move(particles)),
      m_fxListCatalog(std::move(fxLists)),
      m_particles(m_particleCatalog, maximumParticles, initialEmitterCapacity,
                  maximumEmitters, particleAdmission),
      m_maximumAttachedEmitters(std::min(
          maximumAttachedEmitters, kHardMaximumAttachedEmitters)),
      m_maximumPresentationCommands(std::min(
          maximumPresentationCommands,
          kHardMaximumPendingPresentationCommands)) {
    m_attachments.reserve(initialEmitterCapacity);
}

void FxRuntime::updateBonePoses(container::Vector<FxPresentationBonePose> poses) {
    std::sort(poses.begin(), poses.end(), [](const FxPresentationBonePose& left,
                                             const FxPresentationBonePose& right) {
        if (left.objectKey != right.objectKey) return left.objectKey < right.objectKey;
        return left.boneName < right.boneName;
    });
    poses.erase(std::unique(
        poses.begin(), poses.end(),
        [](const FxPresentationBonePose& left,
           const FxPresentationBonePose& right) {
            return left.objectKey == right.objectKey &&
                equalAsciiInsensitive(left.boneName, right.boneName);
        }), poses.end());
    m_bonePoses = std::move(poses);
    updateAttachments(m_attachmentObjects);
    updateBeamEndpointEmitters();
}

void FxRuntime::updateBonePosesRetained(
    container::Span<const FxPresentationBonePose> poses) {
    m_bonePoses.assign(poses.begin(), poses.end());
    std::sort(
        m_bonePoses.begin(), m_bonePoses.end(),
        [](const FxPresentationBonePose& left,
           const FxPresentationBonePose& right) {
            if (left.objectKey != right.objectKey) {
                return left.objectKey < right.objectKey;
            }
            return left.boneName < right.boneName;
        });
    m_bonePoses.erase(std::unique(
        m_bonePoses.begin(), m_bonePoses.end(),
        [](const FxPresentationBonePose& left,
           const FxPresentationBonePose& right) {
            return left.objectKey == right.objectKey &&
                equalAsciiInsensitive(left.boneName, right.boneName);
        }), m_bonePoses.end());
    updateAttachments(m_attachmentObjects);
    updateBeamEndpointEmitters();
}

void FxRuntime::updateModelParticleEmitters(
    const container::Vector<FxModelParticleEmitterPose>& emitters) {
    container::Vector<ModelParticleEmitter>& next =
        m_modelParticleEmitterScratch;
    size_t nextCount = 0;
    next.reserve(emitters.size());
    for (const FxModelParticleEmitterPose& desired : emitters) {
        if (desired.objectKey == 0 || desired.particleSystem.empty() ||
            !m_particleCatalog) {
            continue;
        }
        const uint64_t desiredKey = desired.emitterKey != 0
            ? desired.emitterKey
            : mixSeed(desired.objectKey ^ stableTextHash(desired.boneName) ^
                      stableTextHash(desired.particleSystem));
        const auto existing = std::find_if(
            m_modelParticleEmitters.begin(), m_modelParticleEmitters.end(),
            [desiredKey, &desired](const ModelParticleEmitter& candidate) {
                return candidate.emitterKey == desiredKey &&
                    candidate.particleSystem == desired.particleSystem;
            });
        ParticleEmitterHandle handle;
        if (existing != m_modelParticleEmitters.end()) {
            handle = existing->emitter;
            existing->emitter = {};
        }
        const float rotateX = -desired.anchor.rollRadians;
        const float rotateY = desired.anchor.pitchRadians;
        const float rotateZ = desired.anchor.yawRadians;
        if (!handle || !m_particles.setEmitterTransform(
                handle, desired.anchor.position, rotateX, rotateY, rotateZ)) {
            const ParticleTemplateId templateId =
                m_particleCatalog->findId(desired.particleSystem);
            if (!templateId) continue;
            handle = m_particles.createEmitter({
                .templateId = templateId,
                .position = desired.anchor.position,
                .seed = mixSeed(
                    desiredKey ^ desired.objectKey ^
                    stableTextHash(desired.boneName) ^
                    stableTextHash(desired.particleSystem)),
                .rollRadians = rotateX,
                .pitchRadians = rotateY,
                .yawRadians = rotateZ,
            });
            if (!handle) {
                ++m_stats.rejectedParticleEmitters;
                continue;
            }
            ++m_stats.spawnedParticleEmitters;
        }
        if (nextCount == next.size()) next.emplace_back();
        ModelParticleEmitter& accepted = next[nextCount++];
        accepted.emitter = handle;
        accepted.emitterKey = desiredKey;
        accepted.objectKey = desired.objectKey;
        accepted.boneName = desired.boneName;
        accepted.particleSystem = desired.particleSystem;
    }
    next.resize(nextCount);
    for (const ModelParticleEmitter& stale : m_modelParticleEmitters) {
        if (stale.emitter) static_cast<void>(m_particles.stopEmitter(stale.emitter));
    }
    m_modelParticleEmitters.swap(next);
}

void FxRuntime::updateVehicleParticleEmitters(
    const container::Vector<FxVehicleParticleEmitterPose>& emitters) {
    container::Vector<VehicleParticleEmitter>& next =
        m_vehicleParticleEmitterScratch;
    size_t nextCount = 0;
    next.reserve(emitters.size());
    for (const FxVehicleParticleEmitterPose& desired : emitters) {
        if (desired.emitterKey == 0 || desired.objectKey == 0 ||
            desired.particleSystem.empty() || !m_particleCatalog) {
            continue;
        }
        const auto existing = std::find_if(
            m_vehicleParticleEmitters.begin(),
            m_vehicleParticleEmitters.end(),
            [&desired](const VehicleParticleEmitter& candidate) {
                return candidate.emitterKey == desired.emitterKey &&
                    candidate.particleSystem == desired.particleSystem;
            });
        ParticleEmitterHandle handle;
        uint64_t lastTriggerSequence = 0;
        if (existing != m_vehicleParticleEmitters.end()) {
            handle = existing->emitter;
            lastTriggerSequence = existing->lastTriggerSequence;
            existing->emitter = {};
        }
        const float rotateX = -desired.anchor.rollRadians;
        const float rotateY = desired.anchor.pitchRadians;
        const float rotateZ = desired.anchor.yawRadians;
        const bool updated = handle &&
            m_particles.setEmitterTransform(
                handle, desired.anchor.position, rotateX, rotateY, rotateZ) &&
            m_particles.setEmitterMultipliers(
                handle, desired.velocityMultiplier,
                desired.burstCountMultiplier, desired.sizeMultiplier) &&
            m_particles.setEmitterSpawning(handle, desired.active);
        if (!updated) {
            const ParticleTemplateId templateId =
                m_particleCatalog->findId(desired.particleSystem);
            if (!templateId) {
                ++m_stats.rejectedParticleEmitters;
                continue;
            }
            handle = m_particles.createEmitter({
                .templateId = templateId,
                .position = desired.anchor.position,
                .seed = mixSeed(desired.emitterKey ^ desired.objectKey ^
                    stableTextHash(desired.particleSystem)),
                .rollRadians = rotateX,
                .pitchRadians = rotateY,
                .yawRadians = rotateZ,
                .velocityMultiplier = desired.velocityMultiplier,
                .burstCountMultiplier = desired.burstCountMultiplier,
                .sizeMultiplier = desired.sizeMultiplier,
                .spawning = desired.active,
                .retainedWhenStopped = true,
            });
            if (!handle) {
                ++m_stats.rejectedParticleEmitters;
                continue;
            }
            ++m_stats.spawnedParticleEmitters;
        }
        if (desired.triggerSequence != 0 &&
            desired.triggerSequence != lastTriggerSequence) {
            static_cast<void>(m_particles.triggerEmitter(handle));
            lastTriggerSequence = desired.triggerSequence;
        }
        if (nextCount == next.size()) next.emplace_back();
        VehicleParticleEmitter& accepted = next[nextCount++];
        accepted.emitter = handle;
        accepted.emitterKey = desired.emitterKey;
        accepted.objectKey = desired.objectKey;
        accepted.lastTriggerSequence = lastTriggerSequence;
        accepted.particleSystem = desired.particleSystem;
    }
    next.resize(nextCount);
    for (const VehicleParticleEmitter& stale : m_vehicleParticleEmitters) {
        if (stale.emitter) {
            static_cast<void>(m_particles.stopEmitter(stale.emitter));
        }
    }
    m_vehicleParticleEmitters.swap(next);
}

const FxPresentationBonePose* FxRuntime::findBonePose(
    uint64_t objectKey, container::StringView boneName) const noexcept {
    const auto begin = std::lower_bound(
        m_bonePoses.begin(), m_bonePoses.end(), objectKey,
        [](const FxPresentationBonePose& candidate, uint64_t key) {
            return candidate.objectKey < key;
        });
    for (auto current = begin;
         current != m_bonePoses.end() && current->objectKey == objectKey;
         ++current) {
        if (equalAsciiInsensitive(current->boneName, boneName)) return &*current;
    }
    return nullptr;
}

const FxPresentationBonePose* FxRuntime::findBonePosePrefix(
    uint64_t objectKey, container::StringView bonePrefix,
    uint64_t seed, uint32_t sequenceOrdinal, bool fallbackToBare) const {
    container::Array<const FxPresentationBonePose*,
                     kMaximumNumberedW3dBonePoints> matches{};
    size_t count = 0;
    for (size_t ordinal = 1; ordinal <= matches.size(); ++ordinal) {
        container::String boneName{bonePrefix};
        if (ordinal < 10) boneName.push_back('0');
        boneName += std::to_string(ordinal);
        const FxPresentationBonePose* pose = findBonePose(objectKey, boneName);
        if (!pose) break;
        matches[count++] = pose;
    }
    if (count == 0) {
        return fallbackToBare
            ? findBonePose(objectKey, bonePrefix) : nullptr;
    }
    const size_t selected = sequenceOrdinal != 0
        ? static_cast<size_t>(sequenceOrdinal - 1u) % count
        : std::min<size_t>(
              count - 1u, static_cast<size_t>(unitRandom(seed, 29) *
                                             static_cast<float>(count)));
    return matches[selected];
}

void FxRuntime::stopAttachedParticleGroup(uint64_t objectKey, uint64_t group,
                                          uint64_t stopSequence) {
    if (objectKey == 0 || group == 0) return;
    if (stopSequence != 0) {
        uint64_t& newest = m_attachmentGroupStops[{objectKey, group}];
        newest = std::max(newest, stopSequence);
    }
    std::erase_if(m_attachments, [this, objectKey, group](
        const AttachedEmitter& attachment) {
        if (attachment.objectKey != objectKey || attachment.group != group) {
            return false;
        }
        static_cast<void>(m_particles.stopEmitter(attachment.emitter));
        return true;
    });
}

void FxRuntime::stopAllAttachedParticles(uint64_t objectKey,
                                         uint64_t stopSequence) {
    if (objectKey == 0) return;
    if (stopSequence != 0) {
        uint64_t& newest = m_attachmentObjectStops[objectKey];
        newest = std::max(newest, stopSequence);
    }
    std::erase_if(m_attachments, [this, objectKey](
        const AttachedEmitter& attachment) {
        if (attachment.objectKey != objectKey) return false;
        static_cast<void>(m_particles.stopEmitter(attachment.emitter));
        return true;
    });
}

bool FxRuntime::allowAttachedParticleStart(
    uint64_t objectKey, uint64_t group, uint64_t streamSequence) {
    if (objectKey == 0 || streamSequence == 0) return true;
    const auto objectStopped = m_attachmentObjectStops.find(objectKey);
    if (objectStopped != m_attachmentObjectStops.end()) {
        if (streamSequence <= objectStopped->second) return false;
        m_attachmentObjectStops.erase(objectStopped);
    }
    if (group == 0) return true;
    const auto key = std::pair{objectKey, group};
    const auto groupStopped = m_attachmentGroupStops.find(key);
    if (groupStopped == m_attachmentGroupStops.end()) return true;
    if (streamSequence <= groupStopped->second) return false;
    m_attachmentGroupStops.erase(groupStopped);
    return true;
}

void FxRuntime::submit(const FxPresentationSnapshot& snapshot) {
    container::Vector<FxPresentationInvocation> ordered =
        admitInvocations(snapshot);
    if (snapshot.sessionEpoch == 0 ||
        snapshot.sessionEpoch != m_sessionEpoch) {
        return;
    }
    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const FxPresentationInvocation& left,
           const FxPresentationInvocation& right) {
            if (left.confirmedFrame != right.confirmedFrame) {
                return left.confirmedFrame < right.confirmedFrame;
            }
            return left.streamSequence < right.streamSequence;
        });
    for (const FxPresentationInvocation& invocation : ordered) {
        if (invocation.confirmedFrame != 0) {
            synchronizeSimulationFrame(invocation.confirmedFrame);
        }
        executeInvocation(invocation, true);
    }
    synchronizeSimulationFrame(snapshot.simulationFrame);
    completeDeferredInvocationBarrier();
}

void FxRuntime::completeDeferredInvocationBarrier() {
    m_attachmentGroupStops.clear();
    m_attachmentObjectStops.clear();
}

container::Vector<FxPresentationInvocation> FxRuntime::admitInvocations(
    const FxPresentationSnapshot& snapshot) {
    container::Vector<FxPresentationInvocation> admitted;
    admitInvocationsInto(admitted, snapshot);
    return admitted;
}

void FxRuntime::admitInvocationsInto(
    container::Vector<FxPresentationInvocation>& admitted,
    const FxPresentationSnapshot& snapshot) {
    if (snapshot.sessionEpoch == 0) {
        if (m_sessionEpoch != 0) reset();
        admitted.clear();
        return;
    }
    const uint32_t logicFramesPerSecond =
        snapshot.logicFramesPerSecond != 0
        ? snapshot.logicFramesPerSecond
        : kParticleAuthoredFramesPerSecond;
    if (snapshot.sessionEpoch != m_sessionEpoch) {
        reset();
        m_sessionEpoch = snapshot.sessionEpoch;
    } else if (m_submissionFrameInitialized &&
               (snapshot.simulationFrame < m_lastSubmittedSimulationFrame ||
                logicFramesPerSecond != m_logicFramesPerSecond)) {
        reset();
        m_sessionEpoch = snapshot.sessionEpoch;
    }
    m_logicFramesPerSecond = logicFramesPerSecond;
    m_lastSubmittedSimulationFrame = snapshot.simulationFrame;
    m_submissionFrameInitialized = true;
    m_commands.sessionEpoch = m_sessionEpoch;
    m_groundHeights = snapshot.groundHeights;
    m_legacyBeamTemplates = snapshot.legacyBeamTemplates;
    m_particles.setGroundHeightField(m_groundHeights);
    m_attachmentObjects = snapshot.objects;
    updateAttachments(m_attachmentObjects);
    updateVehicleParticleEmitters(snapshot.vehicleEmitters);
    updateBeamEndpointEmitters();

    admitted.resize(snapshot.invocations.size());
    std::copy(snapshot.invocations.begin(), snapshot.invocations.end(),
              admitted.begin());
    std::stable_sort(
        admitted.begin(), admitted.end(),
        [](const FxPresentationInvocation& left,
           const FxPresentationInvocation& right) {
            if (left.confirmedFrame != right.confirmedFrame) {
                return left.confirmedFrame < right.confirmedFrame;
            }
            return left.streamSequence < right.streamSequence;
        });
    size_t admittedCount = 0;
    for (size_t index = 0; index < admitted.size(); ++index) {
        const FxPresentationInvocation& invocation = admitted[index];
        ++m_stats.submittedInvocations;
        if (rememberInvocation(invocation)) {
            if (admittedCount != index) {
                admitted[admittedCount] = invocation;
            }
            ++admittedCount;
        } else {
            ++m_stats.duplicateInvocations;
        }
    }
    admitted.resize(admittedCount);
}

void FxRuntime::submitDeferredInvocations(
    const FxPresentationSnapshot& snapshot, bool alreadyAdmitted) {
    if (snapshot.sessionEpoch == 0 ||
        snapshot.sessionEpoch != m_sessionEpoch ||
        (snapshot.logicFramesPerSecond != 0
             ? snapshot.logicFramesPerSecond
             : kParticleAuthoredFramesPerSecond) !=
            m_logicFramesPerSecond) {
        return;
    }
    m_groundHeights = snapshot.groundHeights;
    m_legacyBeamTemplates = snapshot.legacyBeamTemplates;
    m_particles.setGroundHeightField(m_groundHeights);
    m_commands.sessionEpoch = m_sessionEpoch;
    container::Vector<FxPresentationInvocation> ordered = snapshot.invocations;
    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const FxPresentationInvocation& left,
           const FxPresentationInvocation& right) {
            if (left.confirmedFrame != right.confirmedFrame) {
                return left.confirmedFrame < right.confirmedFrame;
            }
            return left.streamSequence < right.streamSequence;
        });
    for (const FxPresentationInvocation& invocation : ordered) {
        if (!alreadyAdmitted) {
            ++m_stats.submittedInvocations;
            if (!rememberInvocation(invocation)) {
                ++m_stats.duplicateInvocations;
                continue;
            }
        }
        if (invocation.confirmedFrame != 0) {
            synchronizeSimulationFrame(invocation.confirmedFrame);
        }
        executeInvocation(invocation, true);
    }
    synchronizeSimulationFrame(snapshot.simulationFrame);
}

void FxRuntime::submitDeferredInvocations(
    const FxPresentationSnapshot& snapshot,
    container::Span<const FxPresentationInvocation> invocations,
    bool alreadyAdmitted) {
    if (snapshot.sessionEpoch == 0 ||
        snapshot.sessionEpoch != m_sessionEpoch ||
        (snapshot.logicFramesPerSecond != 0
             ? snapshot.logicFramesPerSecond
             : kParticleAuthoredFramesPerSecond) !=
            m_logicFramesPerSecond) {
        return;
    }
    m_groundHeights = snapshot.groundHeights;
    m_legacyBeamTemplates = snapshot.legacyBeamTemplates;
    m_particles.setGroundHeightField(m_groundHeights);
    m_commands.sessionEpoch = m_sessionEpoch;
    for (const FxPresentationInvocation& invocation : invocations) {
        if (!alreadyAdmitted) {
            ++m_stats.submittedInvocations;
            if (!rememberInvocation(invocation)) {
                ++m_stats.duplicateInvocations;
                continue;
            }
        }
        if (invocation.confirmedFrame != 0) {
            synchronizeSimulationFrame(invocation.confirmedFrame);
        }
        executeInvocation(invocation, true);
    }
    synchronizeSimulationFrame(snapshot.simulationFrame);
}

void FxRuntime::updateSeconds(float deltaSeconds) {
    m_particles.updateSeconds(deltaSeconds);
    removeDeadAttachments();
}

void FxRuntime::synchronizeSimulationFrame(uint64_t simulationFrame) {
    if (m_sessionEpoch == 0) return;
    m_particles.synchronizeAuthoredFrame(
        m_sessionEpoch, simulationFrame, m_logicFramesPerSecond);
    m_lastSubmittedSimulationFrame = std::max(
        m_lastSubmittedSimulationFrame, simulationFrame);
    updateBeamEndpointEmitters();
    removeDeadAttachments();
}

void FxRuntime::updateAuthoredFrames(uint32_t frames) {
    m_particles.updateAuthoredFrames(frames);
    removeDeadAttachments();
}

FxPresentationCommandBatch FxRuntime::takeCommands() {
    FxPresentationCommandBatch output = std::move(m_commands);
    m_commands = {};
    m_commands.sessionEpoch = m_sessionEpoch;
    return output;
}

FxRuntime::ResolvedAnchor FxRuntime::resolveCurrentAnchor(
    const FxTypedAnchor& anchor) const noexcept {
    if (const auto* world = std::get_if<FxWorldPositionAnchor>(&anchor)) {
        return {.anchor = world->world};
    }
    const uint64_t objectKey = std::visit([](const auto& value) -> uint64_t {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, FxWorldPositionAnchor>) return 0;
        else return value.objectKey;
    }, anchor);
    const auto object = std::lower_bound(
        m_attachmentObjects.begin(), m_attachmentObjects.end(), objectKey,
        [](const FxPresentationAnchor& value, uint64_t key) {
            return value.objectKey < key;
        });
    const bool objectAlive = object != m_attachmentObjects.end() &&
        object->objectKey == objectKey;
    if (const auto* bone = std::get_if<FxBoneAnchor>(&anchor)) {
        if (const FxPresentationBonePose* pose = findBonePose(
                bone->objectKey, bone->boneName)) {
            FxPresentationAnchor result = pose->anchor;
            result.objectKey = bone->objectKey;
            result.position.x += bone->worldOffset.x;
            result.position.y += bone->worldOffset.y;
            result.position.z += bone->worldOffset.z;
            return {.anchor = result, .attachmentAlive = true};
        }
        if (objectAlive) {
            FxPresentationAnchor result = *object;
            result.position.x += bone->worldOffset.x;
            result.position.y += bone->worldOffset.y;
            result.position.z += bone->worldOffset.z;
            return {.anchor = result, .attachmentAlive = true};
        }
        // The fallback is already the sealed world endpoint carried by the
        // invocation. Apply worldOffset only to a live resolved bone/object;
        // adding it here would double-offset a detached endpoint.
        return {.anchor = bone->fallback};
    }
    const auto& attached = std::get<FxObjectAnchor>(anchor);
    return objectAlive
        ? ResolvedAnchor{.anchor = *object, .attachmentAlive = true}
        : ResolvedAnchor{.anchor = attached.fallback};
}

void FxRuntime::collectDefinitionBonePoseDemands(
    const FxListDefinition& definition,
    uint64_t objectKey,
    container::Vector<FxBonePoseDemand>& output,
    uint32_t depth,
    container::Vector<FxListId>& recursionStack) const {
    if (objectKey == 0 || depth >= kMaximumFxRecursionDepth ||
        std::find(recursionStack.begin(), recursionStack.end(),
                  definition.id) != recursionStack.end()) {
        return;
    }
    recursionStack.push_back(definition.id);
    for (const FxNugget& nugget : definition.nuggets) {
        if (const auto* tracer = std::get_if<FxTracerNugget>(&nugget);
            tracer && !tracer->boneName.empty()) {
            appendBonePoseDemand(output, {
                .objectKey = objectKey,
                .boneName = tracer->boneName,
            });
            continue;
        }
        const auto* nested = std::get_if<FxListAtBoneNugget>(&nugget);
        if (!nested) continue;
        if (!nested->boneName.empty()) {
            appendBonePoseDemand(output, {
                .objectKey = objectKey,
                .boneName = nested->boneName,
                .numberedPointLimit = kMaximumFxListAtBonePoints,
                .includeBare = true,
            });
        }
        const FxListDefinition* nestedDefinition = m_fxListCatalog
            ? m_fxListCatalog->find(nested->fx) : nullptr;
        if (nestedDefinition) {
            collectDefinitionBonePoseDemands(
                *nestedDefinition, objectKey, output, depth + 1u,
                recursionStack);
        }
    }
    recursionStack.pop_back();
}

void FxRuntime::collectBonePoseDemands(
    container::Span<const FxPresentationInvocation> invocations,
    container::Vector<FxBonePoseDemand>& output) const {
    container::Vector<FxListId> recursionStack;
    recursionStack.reserve(kMaximumFxRecursionDepth);
    for (const FxPresentationInvocation& invocation : invocations) {
        if (invocation.control != FxPresentationControlKind::Execute) continue;
        if (invocation.primary.objectKey != 0 &&
            !invocation.attachmentBoneName.empty()) {
            appendBonePoseDemand(output, {
                .objectKey = invocation.primary.objectKey,
                .boneName = invocation.attachmentBoneName,
                .numberedPointLimit = invocation.attachmentBoneNameIsPrefix
                    ? static_cast<uint32_t>(kMaximumNumberedW3dBonePoints)
                    : 0u,
                .includeBare =
                    invocation.attachmentBonePrefixFallsBackToBare,
            });
        }
        if (invocation.secondary && invocation.secondary->objectKey != 0 &&
            !invocation.secondaryBoneName.empty()) {
            appendBonePoseDemand(output, {
                .objectKey = invocation.secondary->objectKey,
                .boneName = invocation.secondaryBoneName,
                .numberedPointLimit = invocation.secondaryBoneNameIsPrefix
                    ? static_cast<uint32_t>(kMaximumNumberedW3dBonePoints)
                    : 0u,
                .includeBare =
                    invocation.secondaryBonePrefixFallsBackToBare,
            });
        }
        const FxListDefinition* definition = m_fxListCatalog
            ? m_fxListCatalog->find(invocation.fxListName) : nullptr;
        if (definition && invocation.primary.objectKey != 0) {
            recursionStack.clear();
            collectDefinitionBonePoseDemands(
                *definition, invocation.primary.objectKey, output, 0u,
                recursionStack);
        }
    }
}

void FxRuntime::appendActiveBonePoseDemands(
    container::Vector<FxBonePoseDemand>& output) const {
    for (const AttachedEmitter& attachment : m_attachments) {
        if (attachment.objectKey == 0 || attachment.boneName.empty()) continue;
        appendBonePoseDemand(output, {
            .objectKey = attachment.objectKey,
            .boneName = attachment.boneName,
        });
    }
    for (const BeamEndpointEmitter& endpoint : m_beamEndpointEmitters) {
        appendTypedAnchorBoneDemand(output, endpoint.anchor);
    }
}

void FxRuntime::reset() {
    m_particles.reset();
    m_attachments.clear();
    m_attachmentGroupStops.clear();
    m_attachmentObjectStops.clear();
    m_attachmentObjects.clear();
    m_modelParticleEmitters.clear();
    m_vehicleParticleEmitters.clear();
    m_beamEndpointEmitters.clear();
    m_bonePoses.clear();
    m_groundHeights.reset();
    m_legacyBeamTemplates.reset();
    m_commands = {};
    m_recentEventOrder.clear();
    m_recentEventIds.clear();
    m_recentStreamSequences.clear();
    m_lastConsumedStreamSequence = 0;
    m_sessionEpoch = 0;
    m_lastSubmittedSimulationFrame = 0;
    m_logicFramesPerSecond = kParticleAuthoredFramesPerSecond;
    m_submissionFrameInitialized = false;
    m_stats = {};
}

void FxRuntime::updateAttachments(const container::Vector<FxPresentationAnchor>& objects) {
    size_t index = 0;
    while (index < m_attachments.size()) {
        AttachedEmitter& attachment = m_attachments[index];
        const FxPresentationBonePose* bone = attachment.boneName.empty()
            ? nullptr : findBonePose(attachment.objectKey, attachment.boneName);
        const FxPresentationAnchor* object = bone
            ? &bone->anchor : findObject(objects, attachment.objectKey);
        if (!object) {
#if TD_DEBUG_ENABLED
            TD_LOG_DEBUG(
                "[FxRuntime] attached emitter anchor disappeared: object={} bone='{}' handle={}:{}",
                attachment.objectKey, attachment.boneName,
                attachment.emitter.index, attachment.emitter.generation);
#endif
            (void)m_particles.stopEmitter(attachment.emitter);
            m_attachments[index] = m_attachments.back();
            m_attachments.pop_back();
            continue;
        }

        const ParticleVector3 rotatedOffset = rotateEuler(
            attachment.localOffset, -object->rollRadians,
            object->pitchRadians, object->yawRadians);
        const ParticleVector3 position = add(positionOf(*object), rotatedOffset);
        const bool inheritOrientation =
            attachment.inheritsParentTransform || attachment.orientToObject;
        const float rotateX = attachment.rotateX +
            (inheritOrientation ? -object->rollRadians : 0.0f);
        const float rotateY = attachment.rotateY +
            (inheritOrientation ? object->pitchRadians : 0.0f);
        const float rotateZ = attachment.rotateZ +
            (inheritOrientation ? object->yawRadians : 0.0f);
        if (!m_particles.setEmitterTransform(attachment.emitter, position,
                                             rotateX, rotateY, rotateZ)) {
#if TD_DEBUG_ENABLED
            TD_LOG_DEBUG(
                "[FxRuntime] attached emitter handle update failed: object={} bone='{}' handle={}:{} anchor=({}, {}, {}; {}, {}, {})",
                attachment.objectKey, attachment.boneName,
                attachment.emitter.index, attachment.emitter.generation,
                object->position.x, object->position.y, object->position.z,
                object->rollRadians, object->pitchRadians,
                object->yawRadians);
#endif
            m_attachments[index] = m_attachments.back();
            m_attachments.pop_back();
            continue;
        }
        ++index;
    }
}

void FxRuntime::removeDeadAttachments() {
    std::erase_if(m_attachments, [this](const AttachedEmitter& attachment) {
        return !m_particles.containsEmitter(attachment.emitter);
    });
    std::erase_if(m_beamEndpointEmitters,
                  [this](const BeamEndpointEmitter& endpoint) {
        return !m_particles.containsEmitter(endpoint.emitter);
    });
}

void FxRuntime::updateBeamEndpointEmitters() {
    std::erase_if(m_beamEndpointEmitters,
                  [this](BeamEndpointEmitter& endpoint) {
        const ResolvedAnchor resolved = resolveCurrentAnchor(endpoint.anchor);
        if (endpoint.stopAtFrame != 0 &&
            m_lastSubmittedSimulationFrame >= endpoint.stopAtFrame) {
            static_cast<void>(m_particles.stopEmitter(endpoint.emitter));
            return true;
        }
        const bool attached = !std::holds_alternative<FxWorldPositionAnchor>(
            endpoint.anchor);
        if (attached && !resolved.attachmentAlive) {
            static_cast<void>(m_particles.stopEmitter(endpoint.emitter));
            return true;
        }
        if (!m_particles.setEmitterTransform(
                endpoint.emitter, resolved.anchor.position,
                -resolved.anchor.rollRadians, resolved.anchor.pitchRadians,
                resolved.anchor.yawRadians)) {
            return true;
        }
        return false;
    });
}

void FxRuntime::stopBeamEndpointEmitters(uint64_t beamIdentity) noexcept {
    if (beamIdentity == 0) return;
    std::erase_if(m_beamEndpointEmitters,
                  [this, beamIdentity](const BeamEndpointEmitter& endpoint) {
        if (endpoint.beamIdentity != beamIdentity) return false;
        static_cast<void>(m_particles.stopEmitter(endpoint.emitter));
        return true;
    });
}

void FxRuntime::ensureBeamEndpointEmitter(
    uint64_t beamIdentity, bool targetEndpoint,
    const container::String& particleSystem, const FxTypedAnchor& anchor,
    std::optional<uint32_t> lifetimeFrames, uint64_t stopAtFrame,
    uint64_t seed) {
    if (beamIdentity == 0 || particleSystem.empty() || !m_particleCatalog) {
        return;
    }
    const auto existing = std::find_if(
        m_beamEndpointEmitters.begin(), m_beamEndpointEmitters.end(),
        [beamIdentity, targetEndpoint](const BeamEndpointEmitter& endpoint) {
            return endpoint.beamIdentity == beamIdentity &&
                endpoint.targetEndpoint == targetEndpoint;
        });
    if (existing != m_beamEndpointEmitters.end()) {
        existing->anchor = anchor;
        if (stopAtFrame != 0) existing->stopAtFrame = stopAtFrame;
        updateBeamEndpointEmitters();
        return;
    }
    if (m_beamEndpointEmitters.size() >= m_maximumAttachedEmitters) {
        ++m_stats.rejectedParticleEmitters;
        return;
    }
    const ParticleTemplateId templateId =
        m_particleCatalog->findId(particleSystem);
    if (!templateId) {
        ++m_stats.rejectedParticleEmitters;
        return;
    }
    const ResolvedAnchor resolved = resolveCurrentAnchor(anchor);
    const bool attached =
        !std::holds_alternative<FxWorldPositionAnchor>(anchor);
    if (attached && !resolved.attachmentAlive) return;
    const ParticleEmitterHandle emitter = m_particles.createEmitter({
        .templateId = templateId,
        .position = resolved.anchor.position,
        .seed = mixSeed(seed ^ beamIdentity ^
            (targetEndpoint ? 0xa7u : 0x51u) ^
            stableTextHash(particleSystem)),
        .rollRadians = -resolved.anchor.rollRadians,
        .pitchRadians = resolved.anchor.pitchRadians,
        .yawRadians = resolved.anchor.yawRadians,
        .systemLifetimeOverrideFrames = lifetimeFrames,
    });
    if (!emitter) {
        ++m_stats.rejectedParticleEmitters;
        return;
    }
    ++m_stats.spawnedParticleEmitters;
    ++m_stats.spawnedDirectParticleEmitters;
    m_beamEndpointEmitters.push_back({
        .emitter = emitter,
        .beamIdentity = beamIdentity,
        .stopAtFrame = stopAtFrame,
        .targetEndpoint = targetEndpoint,
        .anchor = anchor,
    });
}

void FxRuntime::executeInvocation(const FxPresentationInvocation& invocation,
                                  bool alreadyAdmitted) {
    if (!alreadyAdmitted) {
        ++m_stats.submittedInvocations;
        if (!rememberInvocation(invocation)) {
            ++m_stats.duplicateInvocations;
            return;
        }
    }
    if (invocation.control ==
        FxPresentationControlKind::StopAttachedParticleGroup) {
        stopAttachedParticleGroup(invocation.primary.objectKey,
                                  invocation.attachmentGroup,
                                  invocation.streamSequence);
        return;
    }
    if (invocation.control ==
        FxPresentationControlKind::StopAllAttachedParticles) {
        stopAllAttachedParticles(invocation.primary.objectKey,
                                 invocation.streamSequence);
        return;
    }
    FxPresentationInvocation resolvedInvocation = invocation;
    if (resolvedInvocation.anchorKind ==
            FxPresentationAnchorKind::BonePosition &&
        resolvedInvocation.primary.objectKey != 0 &&
        !resolvedInvocation.attachmentBoneName.empty()) {
        const FxPresentationBonePose* pose =
            resolvedInvocation.attachmentBoneNameIsPrefix
                ? findBonePosePrefix(
                      resolvedInvocation.primary.objectKey,
                      resolvedInvocation.attachmentBoneName,
                      invocation.variationSeed != 0
                          ? invocation.variationSeed
                          : mixSeed(invocation.eventId),
                      resolvedInvocation.attachmentBoneSequenceOrdinal,
                      resolvedInvocation.attachmentBonePrefixFallsBackToBare)
                : findBonePose(resolvedInvocation.primary.objectKey,
                               resolvedInvocation.attachmentBoneName);
        if (pose) {
            const presentation::PlayerAudience audience =
                resolvedInvocation.primary.audience;
            resolvedInvocation.primary = pose->anchor;
            resolvedInvocation.primary.objectKey = invocation.primary.objectKey;
            resolvedInvocation.primary.audience = audience;
            resolvedInvocation.attachmentBoneName = pose->boneName;
            resolvedInvocation.attachmentBoneNameIsPrefix = false;
            ++m_stats.resolvedBoneNuggets;
        } else {
            ++m_stats.missingBonePoses;
            ++m_stats.approximatedBoneNuggets;
        }
    }
    if (resolvedInvocation.secondary &&
        resolvedInvocation.secondary->objectKey != 0 &&
        !resolvedInvocation.secondaryBoneName.empty()) {
        const FxPresentationBonePose* pose =
            resolvedInvocation.secondaryBoneNameIsPrefix
                ? findBonePosePrefix(
                      resolvedInvocation.secondary->objectKey,
                      resolvedInvocation.secondaryBoneName,
                      invocation.variationSeed != 0
                          ? invocation.variationSeed
                          : mixSeed(invocation.eventId),
                      resolvedInvocation.secondaryBoneSequenceOrdinal,
                      resolvedInvocation.secondaryBonePrefixFallsBackToBare)
                : findBonePose(
                      resolvedInvocation.secondary->objectKey,
                      resolvedInvocation.secondaryBoneName);
        if (pose) {
            const uint64_t objectKey =
                resolvedInvocation.secondary->objectKey;
            const presentation::PlayerAudience audience =
                resolvedInvocation.secondary->audience;
            resolvedInvocation.secondary = pose->anchor;
            resolvedInvocation.secondary->objectKey = objectKey;
            resolvedInvocation.secondary->audience = audience;
            resolvedInvocation.secondaryBoneName = pose->boneName;
            resolvedInvocation.secondaryBoneNameIsPrefix = false;
            ++m_stats.resolvedBoneNuggets;
        } else {
            ++m_stats.missingBonePoses;
            ++m_stats.approximatedBoneNuggets;
        }
    }
    if (!resolvedInvocation.inheritResolvedAnchorOrientation) {
        resolvedInvocation.primary.rollRadians = 0.0f;
        resolvedInvocation.primary.pitchRadians = 0.0f;
        resolvedInvocation.primary.yawRadians = 0.0f;
    }
    const uint64_t seed = invocation.variationSeed != 0
        ? invocation.variationSeed
        : mixSeed(invocation.eventId);
    if (resolvedInvocation.directParticle) {
        executeDirectParticle(*resolvedInvocation.directParticle,
                              resolvedInvocation, seed);
        return;
    }
    if (resolvedInvocation.directBeam) {
        executeDirectBeam(*resolvedInvocation.directBeam,
                          resolvedInvocation, seed);
        return;
    }
    if (resolvedInvocation.directScorch) {
        executeDirectScorch(*resolvedInvocation.directScorch,
                            resolvedInvocation);
        return;
    }
    if (resolvedInvocation.directRope) {
        executeDirectRope(*resolvedInvocation.directRope,
                          resolvedInvocation);
        return;
    }
    const FxListDefinition* definition = m_fxListCatalog
        ? m_fxListCatalog->find(invocation.fxListName)
        : nullptr;
    if (!definition) {
        ++m_stats.missingFxLists;
        return;
    }

    container::Vector<FxListId>& recursionStack =
        m_fxListRecursionScratch;
    recursionStack.clear();
    recursionStack.reserve(kMaximumFxRecursionDepth);
    uint32_t expansionBudget = kMaximumExpandedNuggetsPerInvocation;
    executeDefinition(*definition, resolvedInvocation, seed, 0, expansionBudget,
                      recursionStack);
}

void FxRuntime::executeDirectParticle(
    const FxPresentationDirectParticle& direct,
    const FxPresentationInvocation& invocation, uint64_t seed) {
    const bool attachmentRequested = direct.attachToObject;
    const FxPresentationAnchor* attachmentAnchor = nullptr;
    if (attachmentRequested && invocation.primary.objectKey != 0 &&
        invocation.anchorKind != FxPresentationAnchorKind::WorldPosition) {
        if (!invocation.attachmentBoneName.empty()) {
            if (const FxPresentationBonePose* bone = findBonePose(
                    invocation.primary.objectKey,
                    invocation.attachmentBoneName)) {
                attachmentAnchor = &bone->anchor;
            }
        }
        if (!attachmentAnchor) {
            attachmentAnchor = findObject(
                m_attachmentObjects, invocation.primary.objectKey);
        }
    }
    const bool attached = attachmentAnchor != nullptr;
    if (attachmentRequested && !attached) {
#if TD_DEBUG_ENABLED
        TD_LOG_DEBUG(
            "[FxRuntime] direct particle attachment unavailable; using frozen world fallback: template='{}' object={} bone='{}' anchorKind={} event={}",
            direct.particleSystemName, invocation.primary.objectKey,
            invocation.attachmentBoneName,
            static_cast<uint32_t>(invocation.anchorKind),
            invocation.eventId);
#endif
    }
    if (attached &&
        !allowAttachedParticleStart(
            invocation.primary.objectKey, invocation.attachmentGroup,
            invocation.streamSequence)) {
        m_stats.rejectedParticleEmitters += direct.emitterCount;
#if TD_DEBUG_ENABLED
        TD_LOG_DEBUG(
            "[FxRuntime] direct particle attachment start rejected: template='{}' object={} group={} sequence={} event={}",
            direct.particleSystemName, invocation.primary.objectKey,
            invocation.attachmentGroup, invocation.streamSequence,
            invocation.eventId);
#endif
        return;
    }
    ParticleTemplateId templateId = m_particleCatalog
        ? m_particleCatalog->findId(direct.particleSystemName)
        : ParticleTemplateId{};
    ParticleVector3 laterColorKeyTint{1.0f, 1.0f, 1.0f};
    if (!templateId && m_particleCatalog &&
        !direct.fallbackParticleSystemName.empty()) {
        templateId = m_particleCatalog->findId(
            direct.fallbackParticleSystemName);
        if (templateId && direct.fallbackColorKeyTint) {
            laterColorKeyTint = *direct.fallbackColorKeyTint;
        }
    }
    if (!templateId) {
        ++m_stats.rejectedParticleEmitters;
#if TD_DEBUG_ENABLED
        TD_LOG_DEBUG(
            "[FxRuntime] direct particle template missing: template='{}' fallback='{}' object={} event={}",
            direct.particleSystemName, direct.fallbackParticleSystemName,
            invocation.primary.objectKey, invocation.eventId);
#endif
        return;
    }

    const uint32_t count = std::min(
        direct.emitterCount, kMaximumEmittersPerNugget);
    m_stats.rejectedParticleEmitters += direct.emitterCount - count;
    const float major = std::max(0.0f, direct.footprintMajorRadius);
    const float minor = std::max(0.0f,
        direct.footprintMinorRadius > 0.0f
            ? direct.footprintMinorRadius : major);
    const float height = std::max(0.0f, direct.maximumHeight);
    for (uint32_t index = 0; index < count; ++index) {
        const uint64_t emitterSeed = mixSeed(
            seed ^ (static_cast<uint64_t>(index + 1) * kSeedStride));
        ParticleVector3 offset{};
        if (major > 0.0f || minor > 0.0f) {
            if (direct.boxFootprint) {
                offset.x = (unitRandom(emitterSeed, 1) * 2.0f - 1.0f) * major;
                offset.y = (unitRandom(emitterSeed, 2) * 2.0f - 1.0f) * minor;
            } else {
                const float angle = unitRandom(emitterSeed, 1) * kTau;
                const float radial = std::sqrt(unitRandom(emitterSeed, 2));
                offset.x = std::cos(angle) * major * radial;
                offset.y = std::sin(angle) * minor * radial;
            }
        }
        if (height > 0.0f) {
            const float minimumHeight = std::min(3.0f, height);
            offset.z = minimumHeight +
                (height - minimumHeight) * unitRandom(emitterSeed, 3);
            const float length = std::sqrt(offset.x * offset.x +
                                           offset.y * offset.y +
                                           offset.z * offset.z);
            if (length > height && length > 0.0f) {
                offset.z = offset.z / length * height;
            }
        }

        std::optional<uint32_t> initialDelay;
        if (direct.initialDelayMaximumFrames > 0 ||
            direct.initialDelayMinimumFrames > 0) {
            const uint64_t minimum = direct.initialDelayMinimumFrames;
            const uint64_t maximum = std::max(
                direct.initialDelayMinimumFrames,
                direct.initialDelayMaximumFrames);
            const uint64_t span = maximum - minimum + 1u;
            const uint64_t sampled = minimum + std::min<uint64_t>(
                span - 1u, static_cast<uint64_t>(
                    unitRandom(emitterSeed, 4) * static_cast<float>(span)));
            initialDelay = static_cast<uint32_t>(sampled);
        }

        // Direct attachment always resolves the live parent transform here;
        // inheritResolvedAnchorOrientation is an invocation/FXList policy and
        // must not erase createAttachedParticleSystemID's parent matrix.
        const FxPresentationAnchor& anchor = attachmentAnchor
            ? *attachmentAnchor : invocation.primary;
        const float anchorX = -anchor.rollRadians;
        const float anchorY = anchor.pitchRadians;
        const float anchorZ = anchor.yawRadians;
        const ParticleVector3 localOffset = add(
            invocation.attachmentLocalOffset, offset);
        const ParticleVector3 rotatedOffset = rotateEuler(
            localOffset, anchorX, anchorY, anchorZ);
        const ParticleEmitterHandle emitter = m_particles.createEmitter({
            .templateId = templateId,
            .position = add(anchor.position, rotatedOffset),
            .seed = emitterSeed,
            // createAttachedParticleSystemID starts from a local identity and
            // immediately inherits the complete object/Drawable transform.
            // TD anchor Euler convention is X=-roll, Y=+pitch, Z=+yaw.
            .rollRadians = attachmentRequested ? anchorX : 0.0f,
            .pitchRadians = attachmentRequested ? anchorY : 0.0f,
            .yawRadians = attachmentRequested ? anchorZ : 0.0f,
            .initialDelayFrames = initialDelay,
            .laterColorKeyTint = laterColorKeyTint,
            .systemLifetimeOverrideFrames = direct.systemLifetimeFrames,
        });
        if (!emitter) {
            ++m_stats.rejectedParticleEmitters;
#if TD_DEBUG_ENABLED
            TD_LOG_DEBUG(
                "[FxRuntime] direct particle emitter handle creation failed: template='{}' object={} anchor=({}, {}, {}; {}, {}, {}) event={} ordinal={}",
                direct.particleSystemName, invocation.primary.objectKey,
                anchor.position.x, anchor.position.y, anchor.position.z,
                anchor.rollRadians, anchor.pitchRadians,
                anchor.yawRadians, invocation.eventId, index);
#endif
            continue;
        }
        ++m_stats.spawnedParticleEmitters;
        ++m_stats.spawnedDirectParticleEmitters;
#if TD_DEBUG_ENABLED
        if (index == 0) {
            TD_LOG_DEBUG(
                "[FxRuntime] direct particle emitter started: template='{}' object={} attached={} position=({}, {}, {}) handle={}:{} event={} count={}",
                direct.particleSystemName, invocation.primary.objectKey,
                attached, anchor.position.x, anchor.position.y,
                anchor.position.z, emitter.index, emitter.generation,
                invocation.eventId, count);
        }
#endif
        if (attached) {
            if (m_attachments.size() >= m_maximumAttachedEmitters) {
                static_cast<void>(m_particles.stopEmitter(emitter));
                ++m_stats.rejectedParticleEmitters;
#if TD_DEBUG_ENABLED
                TD_LOG_DEBUG(
                    "[FxRuntime] direct particle attachment capacity rejected: template='{}' object={} handle={}:{} capacity={} event={}",
                    direct.particleSystemName, invocation.primary.objectKey,
                    emitter.index, emitter.generation,
                    m_maximumAttachedEmitters, invocation.eventId);
#endif
                continue;
            }
            m_attachments.push_back({
                .emitter = emitter,
                .objectKey = invocation.primary.objectKey,
                .boneName = invocation.attachmentBoneName,
                .localOffset = localOffset,
                .inheritsParentTransform = true,
                .group = invocation.attachmentGroup,
            });
        }
    }
}

void FxRuntime::executeDirectBeam(
    const FxPresentationDirectBeam& direct,
    const FxPresentationInvocation& invocation, uint64_t seed) {
    const uint64_t beamIdentity = direct.beamIdentity != 0
        ? direct.beamIdentity : invocation.eventId;
    if (beamIdentity == 0) {
        ++m_stats.rejectedPresentationCommands;
        return;
    }
    if (direct.control == FxPresentationDirectBeam::Control::End) {
        stopBeamEndpointEmitters(beamIdentity);
        if (!canAppendCommand()) {
            ++m_stats.rejectedPresentationCommands;
            return;
        }
        m_commands.lasers.push_back({
            .identity = commandIdentity(invocation),
            .control = direct.control,
            .beamIdentity = beamIdentity,
            .sizeDeltaFrames = direct.sizeDeltaFrames,
            .decayFrames = direct.decayFrames,
            .primary = invocationAnchor(invocation),
            .secondary = secondaryAnchor(invocation),
        });
        ++m_stats.emittedLaserCommands;
        return;
    }
    const LegacyBeamTemplate* descriptor = nullptr;
    if (m_legacyBeamTemplates) {
        const auto found = m_legacyBeamTemplates->find(direct.objectTemplate);
        if (found != m_legacyBeamTemplates->end()) descriptor = &found->second;
    }
    if (!descriptor || descriptor->kind != LegacyBeamTemplateKind::Laser) {
        ++m_stats.rejectedPresentationCommands;
        return;
    }
    if (!canAppendCommand()) {
        ++m_stats.rejectedPresentationCommands;
        return;
    }
    m_commands.lasers.push_back({
        .identity = commandIdentity(invocation),
        .control = direct.control,
        .beamIdentity = beamIdentity,
        .sizeDeltaFrames = direct.sizeDeltaFrames,
        .decayFrames = direct.decayFrames,
        .descriptor = *descriptor,
        .primary = invocationAnchor(invocation),
        .secondary = secondaryAnchor(invocation),
    });
    ++m_stats.emittedLaserCommands;
    const LegacyLaserTemplate& laser = descriptor->laser;
    std::optional<uint32_t> endpointLifetimeFrames;
    const uint64_t envelopeFrames =
        static_cast<uint64_t>(laser.maximumIntensityFrames) +
        static_cast<uint64_t>(laser.fadeFrames);
    if (envelopeFrames != 0) {
        endpointLifetimeFrames = static_cast<uint32_t>(std::min<uint64_t>(
            envelopeFrames, std::numeric_limits<uint32_t>::max()));
    } else if (direct.beamIdentity == 0) {
        const float minimum = std::max(
            1.0f / 30.0f, laser.minimumLifetimeSeconds);
        const float maximum = std::max(
            minimum, laser.maximumLifetimeSeconds);
        const uint64_t bits = invocation.variationSeed != 0
            ? invocation.variationSeed : invocation.eventId;
        const float unit = static_cast<float>((bits >> 40u) & 0xffffffu) /
            static_cast<float>(0xffffffu);
        endpointLifetimeFrames = std::max(1u, static_cast<uint32_t>(
            std::ceil((minimum + (maximum - minimum) * unit) * 30.0f)));
    }
    if (direct.control == FxPresentationDirectBeam::Control::Begin) {
        stopBeamEndpointEmitters(beamIdentity);
    }
    const auto finishFrame = [](uint64_t start, uint64_t frames) noexcept {
        return start > std::numeric_limits<uint64_t>::max() - frames
            ? std::numeric_limits<uint64_t>::max() : start + frames;
    };
    uint64_t endpointStopFrame = endpointLifetimeFrames
        ? finishFrame(invocation.confirmedFrame, *endpointLifetimeFrames)
        : 0;
    if (direct.decayFrames != 0) {
        endpointStopFrame = finishFrame(
            invocation.confirmedFrame, direct.decayFrames);
    } else if (direct.sizeDeltaFrames < 0) {
        endpointStopFrame = finishFrame(
            invocation.confirmedFrame, static_cast<uint64_t>(
                -static_cast<int64_t>(direct.sizeDeltaFrames)));
    }
    const FxTypedAnchor primary = invocationAnchor(invocation);
    const FxTypedAnchor secondary = secondaryAnchor(invocation);
    ensureBeamEndpointEmitter(
        beamIdentity, false, laser.muzzleParticleSystem, primary,
        endpointLifetimeFrames, endpointStopFrame, seed);
    ensureBeamEndpointEmitter(
        beamIdentity, true, laser.targetParticleSystem, secondary,
        endpointLifetimeFrames, endpointStopFrame, seed);
}

void FxRuntime::executeDirectScorch(
    const FxPresentationDirectScorch& direct,
    const FxPresentationInvocation& invocation) {
    if (!canAppendCommand() || !std::isfinite(direct.radius) ||
        direct.radius <= 0.0f) {
        ++m_stats.rejectedPresentationCommands;
        return;
    }
    FxWorldPositionAnchor anchor = detachedWorldAnchor(
        invocationAnchor(invocation));
    if (const std::optional<float> ground = sampleGroundHeight(
            m_groundHeights.get(), anchor.world.position.x,
            anchor.world.position.y)) {
        anchor.world.position.z = *ground;
    } else if (invocation.groundHeight &&
               std::isfinite(*invocation.groundHeight)) {
        anchor.world.position.z = *invocation.groundHeight;
    }
    m_commands.terrainScorches.push_back({
        .identity = commandIdentity(invocation),
        .anchor = anchor,
        .type = direct.type,
        .radius = direct.radius,
    });
    ++m_stats.emittedTerrainScorchCommands;
}

void FxRuntime::executeDirectRope(
    const FxPresentationDirectRope& direct,
    const FxPresentationInvocation& invocation) {
    const bool finiteDescriptor =
        std::isfinite(direct.maximumLength) &&
        std::isfinite(direct.currentLength) &&
        std::isfinite(direct.width) &&
        std::isfinite(direct.color.x) &&
        std::isfinite(direct.color.y) &&
        std::isfinite(direct.color.z) &&
        std::isfinite(direct.wobbleLength) &&
        std::isfinite(direct.wobbleAmplitude) &&
        std::isfinite(direct.wobbleRatePerFrame) &&
        std::isfinite(direct.wobblePhase) &&
        std::isfinite(direct.verticalOffset) &&
        std::isfinite(direct.currentSpeedPerFrame) &&
        std::isfinite(direct.maximumSpeedPerFrame) &&
        std::isfinite(direct.accelerationPerFrame);
    const bool drawableDescriptor =
        direct.control == FxPresentationRopeControl::End ||
        (direct.maximumLength >= 1.0f && direct.width > 0.0f &&
         direct.wobbleLength > 0.0f);
    if (direct.ropeIdentity == 0 || !finiteDescriptor ||
        !drawableDescriptor || !canAppendCommand()) {
        ++m_stats.rejectedPresentationCommands;
        return;
    }
    m_commands.ropes.push_back({
        .identity = commandIdentity(invocation),
        .rope = direct,
        .anchor = invocationAnchor(invocation),
    });
    ++m_stats.emittedRopeCommands;
}

void FxRuntime::executeDefinition(const FxListDefinition& definition,
                                  const FxPresentationInvocation& invocation,
                                  uint64_t seed, uint32_t depth,
                                  uint32_t& expansionBudget,
                                  container::Vector<FxListId>& recursionStack) {
    if (depth >= kMaximumFxRecursionDepth ||
        std::find(recursionStack.begin(), recursionStack.end(), definition.id) !=
            recursionStack.end()) {
        ++m_stats.recursionRejections;
        return;
    }

    ++m_stats.resolvedFxLists;
    recursionStack.push_back(definition.id);
    for (size_t nuggetIndex = 0; nuggetIndex < definition.nuggets.size(); ++nuggetIndex) {
        if (expansionBudget == 0) {
            ++m_stats.expansionBudgetRejections;
            break;
        }
        --expansionBudget;
        const FxNugget& nugget = definition.nuggets[nuggetIndex];
        const uint64_t nuggetSeed = mixSeed(seed ^
            (static_cast<uint64_t>(nuggetIndex + 1) * kSeedStride));
        if (const auto* particle = std::get_if<FxParticleSystemNugget>(&nugget)) {
            executeParticleNugget(*particle, invocation, nuggetSeed);
        } else if (const auto* sound = std::get_if<FxSoundNugget>(&nugget)) {
            if (canAppendCommand()) {
                m_commands.sounds.push_back({
                    .identity = commandIdentity(invocation),
                    .eventName = sound->name,
                    .anchor = invocationAnchor(invocation),
                });
                ++m_stats.emittedSoundCommands;
            } else {
                ++m_stats.rejectedPresentationCommands;
            }
        } else if (const auto* ray = std::get_if<FxRayEffectNugget>(&nugget)) {
            if (canAppendCommand()) {
                FxTypedAnchor primary = translatedTypedAnchor(
                    invocationAnchor(invocation), ray->primaryOffset);
                FxTypedAnchor secondary = translatedTypedAnchor(
                    secondaryAnchor(invocation), ray->secondaryOffset);
                const LegacyBeamTemplate* descriptor = nullptr;
                if (m_legacyBeamTemplates) {
                    const auto found = m_legacyBeamTemplates->find(
                        ray->objectTemplate);
                    if (found != m_legacyBeamTemplates->end()) {
                        descriptor = &found->second;
                    }
                }
                if (descriptor &&
                    descriptor->kind == LegacyBeamTemplateKind::Laser) {
                    m_commands.lasers.push_back({
                        .identity = commandIdentity(invocation),
                        .descriptor = *descriptor,
                        .primary = std::move(primary),
                        .secondary = std::move(secondary),
                    });
                    ++m_stats.emittedLaserCommands;
                } else {
                    m_commands.rays.push_back({
                        .identity = commandIdentity(invocation),
                        .objectTemplate = ray->objectTemplate,
                        .descriptor = descriptor ? *descriptor
                                                 : LegacyBeamTemplate{},
                        .templateResolved = descriptor != nullptr,
                        .primary = std::move(primary),
                        .secondary = std::move(secondary),
                    });
                    ++m_stats.emittedRayCommands;
                }
            } else {
                ++m_stats.rejectedPresentationCommands;
            }
        } else if (const auto* tracer = std::get_if<FxTracerNugget>(&nugget)) {
            if (unitRandom(nuggetSeed, 17) > std::clamp(tracer->probability, 0.0f, 1.0f)) {
                continue;
            }
            if (canAppendCommand()) {
                if (m_legacyBeamTemplates && !tracer->tracerName.empty()) {
                    const auto descriptor = m_legacyBeamTemplates->find(
                        tracer->tracerName);
                    if (descriptor == m_legacyBeamTemplates->end() ||
                        descriptor->second.kind !=
                            LegacyBeamTemplateKind::Tracer) {
                        ++m_stats.rejectedPresentationCommands;
                        continue;
                    }
                }
                FxTypedAnchor primary = invocationAnchor(invocation);
                if (!tracer->boneName.empty() && invocation.primary.objectKey != 0) {
                    if (const FxPresentationBonePose* pose = findBonePose(
                            invocation.primary.objectKey, tracer->boneName)) {
                        primary = FxBoneAnchor{
                            .objectKey = invocation.primary.objectKey,
                            .boneName = tracer->boneName,
                            .fallback = pose->anchor,
                        };
                        ++m_stats.resolvedBoneNuggets;
                    } else {
                        ++m_stats.missingBonePoses;
                        ++m_stats.approximatedBoneNuggets;
                    }
                }
                FxTypedAnchor secondary = invocation.secondary
                    ? secondaryAnchor(invocation)
                    : directionalEndpoint(primary, tracer->length);
                m_commands.tracers.push_back({
                    .identity = commandIdentity(invocation),
                    .tracerName = tracer->tracerName,
                    .primary = std::move(primary),
                    .secondary = std::move(secondary),
                    .speed = tracer->speed > 0.0f ? tracer->speed : invocation.primarySpeed,
                    .decayAt = tracer->decayAt,
                    .length = tracer->length,
                    .width = tracer->width,
                    .color = tracer->color,
                });
                ++m_stats.emittedTracerCommands;
            } else {
                ++m_stats.rejectedPresentationCommands;
            }
        } else if (const auto* light = std::get_if<FxLightPulseNugget>(&nugget)) {
            if (canAppendCommand()) {
                float effectiveRadius = std::max(0.0f, light->radius);
                if (invocation.anchorKind ==
                        FxPresentationAnchorKind::ObjectAttachment &&
                    light->radiusAsPercentOfObjectSize > 0.0f &&
                    invocation.primary.objectBoundingCircleRadius > 0.0f) {
                    effectiveRadius =
                        invocation.primary.objectBoundingCircleRadius *
                        light->radiusAsPercentOfObjectSize;
                }
                if (dynamic_lights::visual_defaults::kInnerRadius +
                        effectiveRadius <
                    dynamic_lights::visual_defaults::kMinimumOriginalOuterRadius) {
                    ++m_stats.rejectedPresentationCommands;
                    continue;
                }
                m_commands.lightPulses.push_back({
                    .identity = commandIdentity(invocation),
                    .anchor = invocationAnchor(invocation),
                    .color = light->color,
                    .radius = effectiveRadius,
                    .increaseTimeMilliseconds = light->increaseTimeMilliseconds,
                    .decreaseTimeMilliseconds = light->decreaseTimeMilliseconds,
                });
                ++m_stats.emittedLightPulseCommands;
            } else {
                ++m_stats.rejectedPresentationCommands;
            }
        } else if (const auto* shake = std::get_if<FxViewShakeNugget>(&nugget)) {
            if (canAppendCommand()) {
                m_commands.viewShakes.push_back({
                    .identity = commandIdentity(invocation),
                    .type = shake->type,
                    .anchor = detachedWorldAnchor(invocationAnchor(invocation)),
                });
                ++m_stats.emittedViewShakeCommands;
            } else {
                ++m_stats.rejectedPresentationCommands;
            }
        } else if (const auto* scorch = std::get_if<FxTerrainScorchNugget>(&nugget)) {
            if (canAppendCommand()) {
                FxWorldPositionAnchor anchor = detachedWorldAnchor(
                    invocationAnchor(invocation));
                if (const std::optional<float> ground = sampleGroundHeight(
                        m_groundHeights.get(), anchor.world.position.x,
                        anchor.world.position.y)) {
                    anchor.world.position.z = *ground;
                } else if (invocation.groundHeight &&
                           std::isfinite(*invocation.groundHeight)) {
                    anchor.world.position.z = *invocation.groundHeight;
                }
                m_commands.terrainScorches.push_back({
                    .identity = commandIdentity(invocation),
                    .anchor = anchor,
                    .type = scorch->type,
                    .radius = std::max(0.0f, scorch->radius),
                });
                ++m_stats.emittedTerrainScorchCommands;
            } else {
                ++m_stats.rejectedPresentationCommands;
            }
        } else if (const auto* nested = std::get_if<FxListAtBoneNugget>(&nugget)) {
            const FxListDefinition* nestedDefinition = m_fxListCatalog
                ? m_fxListCatalog->find(nested->fx)
                : nullptr;
            if (!nestedDefinition) {
                ++m_stats.missingFxLists;
                continue;
            }
            // FXListAtBonePos exists only in the object form in RefCode.
            // A missing object, Drawable, or bone yields zero nested
            // invocations; it must not degrade to the object root because
            // that turns absent muzzle/impact bones into visible white
            // flashes and dynamic lights at the model origin.
            if (invocation.primary.objectKey == 0 || nested->boneName.empty()) {
                ++m_stats.missingBonePoses;
                continue;
            }

            bool resolvedAnyPose = false;
            const auto executeAtPose = [&](const FxPresentationBonePose& pose,
                                           uint64_t poseSeed) {
                FxPresentationInvocation nestedInvocation = invocation;
                // The original invokes the nested list through doFXPos: use
                // the current bone transform as a detached one-shot position,
                // never as an ObjectAttachment lifetime.
                nestedInvocation.attachmentBoneName = pose.boneName;
                nestedInvocation.attachmentBoneNameIsPrefix = false;
                nestedInvocation.anchorKind =
                    FxPresentationAnchorKind::BonePosition;
                nestedInvocation.primary = pose.anchor;
                nestedInvocation.primary.objectKey = invocation.primary.objectKey;
                nestedInvocation.primary.audience = invocation.primary.audience;
                if (!nested->orientToBone) {
                    nestedInvocation.primary.rollRadians =
                        invocation.primary.rollRadians;
                    nestedInvocation.primary.pitchRadians =
                        invocation.primary.pitchRadians;
                    nestedInvocation.primary.yawRadians =
                        invocation.primary.yawRadians;
                }
                ++m_stats.resolvedBoneNuggets;
                executeDefinition(*nestedDefinition, nestedInvocation, poseSeed,
                                  depth + 1, expansionBudget, recursionStack);
                resolvedAnyPose = true;
            };

            if (const FxPresentationBonePose* pose = findBonePose(
                    invocation.primary.objectKey, nested->boneName)) {
                executeAtPose(*pose, nuggetSeed);
            }
            for (size_t ordinal = 1;
                 ordinal <= kMaximumFxListAtBonePoints && expansionBudget != 0;
                 ++ordinal) {
                container::String numberedName = nested->boneName;
                if (ordinal < 10) numberedName.push_back('0');
                numberedName += std::to_string(ordinal);
                const FxPresentationBonePose* pose = findBonePose(
                    invocation.primary.objectKey, numberedName);
                if (!pose) break;
                executeAtPose(*pose, mixSeed(
                    nuggetSeed ^ (static_cast<uint64_t>(ordinal) * kSeedStride)));
            }
            if (!resolvedAnyPose) {
                ++m_stats.missingBonePoses;
            }
        } else {
            ++m_stats.unsupportedNuggets;
        }
    }
    recursionStack.pop_back();
}

void FxRuntime::executeParticleNugget(const FxParticleSystemNugget& nugget,
                                      const FxPresentationInvocation& invocation,
                                      uint64_t seed) {
    if (!nugget.particleSystem || !m_particleCatalog ||
        !m_particleCatalog->find(nugget.particleSystem)) {
        ++m_stats.rejectedParticleEmitters;
        return;
    }

    const uint32_t requestedCount = nugget.count > 0
        ? static_cast<uint32_t>(nugget.count) : 0;
    const uint32_t count = std::min(requestedCount, kMaximumEmittersPerNugget);
    m_stats.rejectedParticleEmitters += requestedCount - count;
    const bool attached = nugget.attachToObject &&
        invocation.primary.objectKey != 0 &&
        invocation.anchorKind ==
            FxPresentationAnchorKind::ObjectAttachment;
    if (attached && !allowAttachedParticleStart(
            invocation.primary.objectKey, invocation.attachmentGroup,
            invocation.streamSequence)) {
        m_stats.rejectedParticleEmitters += count;
        return;
    }
    for (uint32_t emitterIndex = 0; emitterIndex < count; ++emitterIndex) {
        const uint64_t emitterSeed = mixSeed(seed ^
            (static_cast<uint64_t>(emitterIndex + 1) * kSeedStride));
        const float radius = sampleRange(nugget.radius, emitterSeed, 1);
        const float radiusAngle = unitRandom(emitterSeed, 2) * kTau;
        const float height = sampleRange(nugget.height, emitterSeed, 3);
        const ParticleVector3 randomizedOffset = {
            nugget.offset.x + radius * std::cos(radiusAngle),
            nugget.offset.y + radius * std::sin(radiusAngle),
            nugget.offset.z + height,
        };

        float anchorRotateX = -invocation.primary.rollRadians;
        float anchorRotateY = invocation.primary.pitchRadians;
        float anchorRotateZ = invocation.primary.yawRadians;
        if (nugget.ricochet && invocation.secondary) {
            const float deltaX = invocation.primary.position.x - invocation.secondary->position.x;
            const float deltaY = invocation.primary.position.y - invocation.secondary->position.y;
            anchorRotateX = 0.0f;
            anchorRotateY = 0.0f;
            anchorRotateZ = std::atan2(deltaY, deltaX);
        }

        const ParticleVector3 worldOffset = rotateEuler(
            randomizedOffset, anchorRotateX, anchorRotateY, anchorRotateZ);
        const ParticleVector3 position = add(invocation.primary.position, worldOffset);
        ParticleVector3 sealedPosition = position;
        if (nugget.createAtGroundHeight) {
            if (const std::optional<float> ground = sampleGroundHeight(
                    m_groundHeights.get(), sealedPosition.x,
                    sealedPosition.y)) {
                sealedPosition.z = *ground;
            } else if (invocation.groundHeight &&
                       std::isfinite(*invocation.groundHeight)) {
                sealedPosition.z = *invocation.groundHeight;
            }
        }
        const float emitterRotateX = nugget.rotateX +
            (nugget.orientToObject ? anchorRotateX : 0.0f);
        const float emitterRotateY = nugget.rotateY +
            (nugget.orientToObject ? anchorRotateY : 0.0f);
        const float emitterRotateZ = nugget.rotateZ +
            (nugget.orientToObject ? anchorRotateZ : 0.0f);

        std::optional<uint32_t> initialDelay;
        const float delayMilliseconds = sampleRange(nugget.initialDelay, emitterSeed, 4);
        if (delayMilliseconds >= 0.0f) {
            initialDelay = delayFramesFromMilliseconds(delayMilliseconds);
        }
        if (nugget.createAtGroundHeight) ++m_stats.groundHeightRequests;

        const ParticleEmitterHandle emitter = m_particles.createEmitter({
            .templateId = nugget.particleSystem,
            .position = sealedPosition,
            .seed = emitterSeed,
            .rollRadians = emitterRotateX,
            .pitchRadians = emitterRotateY,
            .yawRadians = emitterRotateZ,
            .initialDelayFrames = initialDelay,
            .emissionRadiusOverride = nugget.useCallersRadius
                ? std::max(0.0f, invocation.overrideRadius) : 0.0f,
        });
        if (!emitter) {
            ++m_stats.rejectedParticleEmitters;
            continue;
        }
        ++m_stats.spawnedParticleEmitters;
#if TD_DEBUG_ENABLED
        if (emitterIndex == 0) {
            const ParticleSystemTemplate* definition =
                m_particleCatalog->find(nugget.particleSystem);
            TD_LOG_DEBUG(
                "[FxRuntime] FXList particle emitter started: fx='{}' template='{}' object={} attached={} position=({}, {}, {}) handle={}:{} event={} count={}",
                invocation.fxListName,
                definition ? definition->name : container::String{},
                invocation.primary.objectKey, attached,
                sealedPosition.x, sealedPosition.y, sealedPosition.z,
                emitter.index, emitter.generation, invocation.eventId,
                count);
        }
#endif

        if (attached) {
            if (m_attachments.size() >= m_maximumAttachedEmitters) {
                static_cast<void>(m_particles.stopEmitter(emitter));
                ++m_stats.rejectedParticleEmitters;
                continue;
            }
            m_attachments.push_back({
                .emitter = emitter,
                .objectKey = invocation.primary.objectKey,
                .boneName = invocation.attachmentBoneName,
                .localOffset = randomizedOffset,
                .rotateX = nugget.rotateX,
                .rotateY = nugget.rotateY,
                .rotateZ = nugget.rotateZ,
                .orientToObject = nugget.orientToObject,
            });
        }
    }
}

FxTypedAnchor FxRuntime::invocationAnchor(
    const FxPresentationInvocation& invocation) const {
    if (invocation.anchorKind == FxPresentationAnchorKind::BonePosition &&
        invocation.primary.objectKey != 0 &&
        !invocation.attachmentBoneName.empty()) {
        return FxBoneAnchor{
            .objectKey = invocation.primary.objectKey,
            .boneName = invocation.attachmentBoneName,
            .fallback = invocation.primary,
        };
    }
    if (invocation.anchorKind == FxPresentationAnchorKind::ObjectAttachment &&
        invocation.primary.objectKey != 0) {
        return FxObjectAnchor{
            .objectKey = invocation.primary.objectKey,
            .fallback = invocation.primary,
        };
    }
    return FxWorldPositionAnchor{.world = invocation.primary};
}

FxTypedAnchor FxRuntime::secondaryAnchor(
    const FxPresentationInvocation& invocation) const {
    if (!invocation.secondary) return invocationAnchor(invocation);
    if (invocation.secondary->objectKey != 0 &&
        !invocation.secondaryBoneName.empty()) {
        return FxBoneAnchor{
            .objectKey = invocation.secondary->objectKey,
            .boneName = invocation.secondaryBoneName,
            .fallback = *invocation.secondary,
            .worldOffset = invocation.secondaryWorldOffset,
        };
    }
    if (invocation.secondary->objectKey != 0) {
        return FxObjectAnchor{
            .objectKey = invocation.secondary->objectKey,
            .fallback = *invocation.secondary,
        };
    }
    return FxWorldPositionAnchor{.world = *invocation.secondary};
}

bool FxRuntime::rememberInvocation(
    const FxPresentationInvocation& invocation) {
    if (invocation.streamSequence != 0) {
        const uint64_t sequence = invocation.streamSequence;
        if (m_recentStreamSequences.contains(sequence)) {
            return false;
        }
        const uint64_t staleFloor = m_lastConsumedStreamSequence >
                kRecentInvocationWindow
            ? m_lastConsumedStreamSequence - kRecentInvocationWindow
            : 0;
        if (sequence <= staleFloor) return false;

        m_lastConsumedStreamSequence = std::max(
            m_lastConsumedStreamSequence, sequence);
        m_recentStreamSequences.insert(sequence);
        const uint64_t retainedFloor = m_lastConsumedStreamSequence >
                kRecentInvocationWindow
            ? m_lastConsumedStreamSequence - kRecentInvocationWindow
            : 0;
        while (!m_recentStreamSequences.empty() &&
               *m_recentStreamSequences.begin() <= retainedFloor) {
            m_recentStreamSequences.erase(m_recentStreamSequences.begin());
        }
        return true;
    }
    const uint64_t eventId = invocation.eventId;
    if (eventId == 0) return true;
    if (m_recentEventIds.contains(eventId)) return false;
    m_recentEventIds.insert(eventId);
    m_recentEventOrder.push_back(eventId);
    while (m_recentEventOrder.size() > kRecentInvocationWindow) {
        m_recentEventIds.erase(m_recentEventOrder.front());
        m_recentEventOrder.pop_front();
    }
    return true;
}

bool FxRuntime::canAppendCommand() const noexcept {
    return m_commands.size() < m_maximumPresentationCommands;
}

} // namespace engine::fx
