#pragma once

#include "core/container/container_types.h"

#include "AoSoABlockStorage.h"
#include "GpuParticleCommandNormalization.h"
#include "GpuParticleContract.hlsli"
#include "presentation/fx/runtime/ParticleClock.h"
#include "core/container/generational_slot_map.h"
#include "presentation/fx/content/ParticleSystemCatalog.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
namespace engine::fx {

inline constexpr size_t kParticleAoSoAWidth = 16;
struct FxGroundHeightFieldSnapshot;

struct ParticleEmitterState final {
    ParticleTemplateId templateId;
    ParticleVector3 position;
    // Last position committed by an authored 30 Hz step. Render/extraction
    // may update position more often; burst emission distributes particles
    // from this point toward the newest sealed position.
    ParticleVector3 previousAuthoredPosition;
    float rollRadians = 0.0f;
    float pitchRadians = 0.0f;
    float yawRadians = 0.0f;
    float emissionRadiusOverride = 0.0f;
    ParticleVector3 velocityMultiplier{1.0f, 1.0f, 1.0f};
    float burstCountMultiplier = 1.0f;
    float sizeMultiplier = 1.0f;
    // Instance-local equivalent of ParticleSystemInfo::tintAllColors().
    // The retail function leaves color key 0 untouched and multiplies 1..N.
    ParticleVector3 laterColorKeyTint{1.0f, 1.0f, 1.0f};
    std::optional<uint32_t> systemLifetimeOverrideFrames;
    uint64_t seed = 0;
    uint64_t nextBurstOrdinal = 0;
    uint64_t nextParticleOrdinal = 0;
    uint32_t particleCount = 0;
    float ageFrames = 0.0f;
    float nextBurstFrame = 0.0f;
    float accumulatedSizeBonus = 0.0f;
    float windAngle = 0.0f;
    float windAngleChange = 0.0f;
    float windStartAngle = 0.0f;
    float windEndAngle = 0.0f;
    uint8_t relationDepth = 0;
    bool spawning = true;
    // Renderer-owned Draw emitters survive a temporary stop so start()/stop()
    // does not recreate the system and reset its authored burst phase. A
    // permanent stop clears this latch before normal particle drain/retire.
    bool retainedWhenStopped = false;
    bool emittedBurst = false;
    bool windMovingToEnd = true;
    bool externallyDriven = false;
};

using ParticleEmitterStorage = core::generational_slot_map<ParticleEmitterState>;
using ParticleEmitterHandle = ParticleEmitterStorage::handle;

struct ParticleLocation final {
    AoSoABlockIndex block;
    uint8_t lane = 0;
};

using ParticleLocationStorage = core::generational_slot_map<ParticleLocation>;
using ParticleHandle = ParticleLocationStorage::handle;

struct alignas(64) BillboardParticleBlock final {
    container::Array<float, kParticleAoSoAWidth> positionX{};
    container::Array<float, kParticleAoSoAWidth> positionY{};
    container::Array<float, kParticleAoSoAWidth> positionZ{};
    container::Array<float, kParticleAoSoAWidth> previousX{};
    container::Array<float, kParticleAoSoAWidth> previousY{};
    container::Array<float, kParticleAoSoAWidth> previousZ{};
    container::Array<float, kParticleAoSoAWidth> velocityX{};
    container::Array<float, kParticleAoSoAWidth> velocityY{};
    container::Array<float, kParticleAoSoAWidth> velocityZ{};
    container::Array<float, kParticleAoSoAWidth> driftX{};
    container::Array<float, kParticleAoSoAWidth> driftY{};
    container::Array<float, kParticleAoSoAWidth> driftZ{};
    container::Array<float, kParticleAoSoAWidth> emitterOriginX{};
    container::Array<float, kParticleAoSoAWidth> emitterOriginY{};
    container::Array<float, kParticleAoSoAWidth> emitterOriginZ{};
    container::Array<float, kParticleAoSoAWidth> windRandomness{};
    container::Array<float, kParticleAoSoAWidth> gravity{};
    container::Array<float, kParticleAoSoAWidth> ageFrames{};
    container::Array<float, kParticleAoSoAWidth> lifetimeFrames{};
    container::Array<float, kParticleAoSoAWidth> size{};
    container::Array<float, kParticleAoSoAWidth> sizeRate{};
    container::Array<float, kParticleAoSoAWidth> sizeRateDamping{};
    // `angle`/`angularRate` remain the billboard-facing Z lane used by the
    // existing CPU/GPU billboard paths. X/Y are retained and integrated for
    // authored DRAWABLE particles so their complete orientation is available
    // to the forthcoming drawable-instance renderer instead of being lost at
    // content load.
    container::Array<float, kParticleAoSoAWidth> angleX{};
    container::Array<float, kParticleAoSoAWidth> angleY{};
    container::Array<float, kParticleAoSoAWidth> angle{};
    container::Array<float, kParticleAoSoAWidth> angularRateX{};
    container::Array<float, kParticleAoSoAWidth> angularRateY{};
    container::Array<float, kParticleAoSoAWidth> angularRate{};
    container::Array<float, kParticleAoSoAWidth> angularDamping{};
    container::Array<float, kParticleAoSoAWidth> velocityDamping{};
    container::Array<float, kParticleAoSoAWidth> colorScale{};
    container::Array<float, kParticleAoSoAWidth> alpha{};
    container::Array<float, kParticleAoSoAWidth> red{};
    container::Array<float, kParticleAoSoAWidth> green{};
    container::Array<float, kParticleAoSoAWidth> blue{};
    container::Array<float, kParticleAoSoAWidth> laterColorKeyTintRed{};
    container::Array<float, kParticleAoSoAWidth> laterColorKeyTintGreen{};
    container::Array<float, kParticleAoSoAWidth> laterColorKeyTintBlue{};
    container::Array<uint64_t, kParticleAoSoAWidth> seed{};
    container::Array<uint64_t, kParticleAoSoAWidth> ordinal{};
    container::Array<ParticleTemplateId, kParticleAoSoAWidth> templateId{};
    container::Array<ParticleEmitterHandle, kParticleAoSoAWidth> emitter{};
    container::Array<ParticleHandle, kParticleAoSoAWidth> handle{};
    // Admission metadata stays lane-local so AoSoA compaction can move a
    // particle without invalidating the global per-priority age chains.
    container::Array<ParticleHandle, kParticleAoSoAWidth> priorityPrevious{};
    container::Array<ParticleHandle, kParticleAoSoAWidth> priorityNext{};
    container::Array<uint64_t, kParticleAoSoAWidth> admissionOrdinal{};
    container::Array<ParticlePriority, kParticleAoSoAWidth> priority{};
    container::Array<uint8_t, kParticleAoSoAWidth> fieldParticle{};
    container::Array<ParticleWindMotion, kParticleAoSoAWidth> windMotion{};
    container::Array<uint8_t, kParticleAoSoAWidth> particleUpTowardsEmitter{};
    uint8_t count = 0;
};

struct ParticleAdmissionSettings final {
    // SIZE_MAX means "use the runtime hard ceiling" and preserves the old
    // direct-construction API. Zero is a real authored/LOD disable value.
    size_t ordinaryParticleLimit = std::numeric_limits<size_t>::max();
    size_t fieldParticleLimit = 30;
    ParticlePriority minimumPriority = ParticlePriority::Invalid;
    ParticlePriority minimumSkipPriority = ParticlePriority::Invalid;
    uint32_t skipMask = 0;
};

// Scheduling policy only; it must not change authored particle behaviour.
// Keeping this separate from ParticleAdmissionSettings prevents performance
// tuning from silently changing gameplay particle limits or priority rules.
struct ParticleUpdateSettings final {
    bool parallelIntegration = true;
    size_t parallelParticleThreshold = 256;
    size_t blocksPerTask = 4;
    bool collectPhaseTimings = true;
};

struct ParticleEmitterSpawn final {
    ParticleTemplateId templateId;
    ParticleVector3 position;
    uint64_t seed = 0;
    float rollRadians = 0.0f;
    float pitchRadians = 0.0f;
    float yawRadians = 0.0f;
    std::optional<uint32_t> initialDelayFrames;
    float emissionRadiusOverride = 0.0f;
    ParticleVector3 velocityMultiplier{1.0f, 1.0f, 1.0f};
    float burstCountMultiplier = 1.0f;
    float sizeMultiplier = 1.0f;
    ParticleVector3 laterColorKeyTint{1.0f, 1.0f, 1.0f};
    std::optional<uint32_t> systemLifetimeOverrideFrames;
    bool spawning = true;
    bool retainedWhenStopped = false;
};

struct ParticleRuntimeParticle final {
    ParticleHandle handle;
    ParticleEmitterHandle emitter;
    ParticleTemplateId templateId;
    ParticleVector3 position;
    ParticleVector3 previousPosition;
    ParticleVector3 velocity;
    float ageFrames = 0.0f;
    float lifetimeFrames = 0.0f;
    float size = 0.0f;
    // Full authored Euler state. `angle` below is retained as the projected
    // billboard Z angle for existing callers.
    ParticleVector3 orientation;
    float angle = 0.0f;
    float alpha = 0.0f;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    uint64_t ordinal = 0;
    uint64_t admissionOrdinal = 0;
    ParticlePriority priority = ParticlePriority::Invalid;
    bool fieldParticle = false;
};

struct ParticleRuntimeStats final {
    uint64_t emittedParticles = 0;
    uint64_t expiredParticles = 0;
    uint64_t rejectedParticles = 0;
    uint64_t rejectedByPriority = 0;
    uint64_t rejectedBySkipMask = 0;
    uint64_t rejectedFieldParticles = 0;
    uint64_t rejectedOrdinaryCapacity = 0;
    uint64_t rejectedHardCeiling = 0;
    uint64_t evictedParticles = 0;
    uint64_t evictedLowerPriorityParticles = 0;
    uint64_t evictedSamePriorityParticles = 0;
    uint64_t alwaysRenderAdmissionsBeyondOrdinaryLimit = 0;
    uint64_t alwaysRenderPreemptions = 0;
    uint64_t rejectedEmitters = 0;
    uint64_t rejectedBelowGround = 0;
    uint64_t emittedSlaveParticles = 0;
    uint64_t spawnedAttachedEmitters = 0;
    uint64_t rejectedRelatedSystems = 0;
    uint64_t droppedAuthoredFrames = 0;
    uint64_t integratedParticles = 0;
    uint64_t serialIntegrationFrames = 0;
    uint64_t parallelIntegrationFrames = 0;
    uint64_t parallelIntegrationTasks = 0;
    uint64_t compactedDeadParticles = 0;
    size_t particleHighWater = 0;
    size_t ordinaryParticleHighWater = 0;
    size_t fieldParticleHighWater = 0;
    size_t alwaysRenderParticleHighWater = 0;
    size_t emitterHighWater = 0;
};

// Most recently completed authored particle frame. Timings cover CPU wall
// time including Taskflow scheduling/join; the compact phase includes every
// owner-thread eraseParticle() mutation. This is diagnostics-only and never
// enters gameplay/replay state.
struct ParticleRuntimePhaseProfile final {
    uint64_t sampleOrdinal = 0;
    uint32_t authoredFrames = 0;
    uint64_t emitterUpdateNanoseconds = 0;
    uint64_t integrationNanoseconds = 0;
    uint64_t serialCompactNanoseconds = 0;
    uint64_t integratedParticles = 0;
    uint64_t compactedDeadParticles = 0;
    size_t activeBlocks = 0;
    size_t integrationTasks = 0;
    bool parallelIntegration = false;
};

class ParticleRuntime final {
public:
    explicit ParticleRuntime(container::SharedPtr<const ParticleSystemCatalog> catalog,
                             size_t maximumParticles = 8192,
                             size_t initialEmitterCapacity = 256,
                             size_t maximumEmitters = 4096,
                             ParticleAdmissionSettings admission = {},
                             ParticleUpdateSettings updateSettings = {});

    [[nodiscard]] ParticleEmitterHandle createEmitter(const ParticleEmitterSpawn& spawn);
    [[nodiscard]] bool setEmitterPosition(ParticleEmitterHandle emitter,
                                          ParticleVector3 position) noexcept;
    [[nodiscard]] bool setEmitterTransform(ParticleEmitterHandle emitter,
                                           ParticleVector3 position,
                                           float rollRadians,
                                           float pitchRadians,
                                           float yawRadians) noexcept;
    [[nodiscard]] bool setEmitterMultipliers(
        ParticleEmitterHandle emitter, ParticleVector3 velocityMultiplier,
        float burstCountMultiplier, float sizeMultiplier) noexcept;
    [[nodiscard]] bool setEmitterSpawning(
        ParticleEmitterHandle emitter, bool spawning) noexcept;
    [[nodiscard]] bool triggerEmitter(ParticleEmitterHandle emitter);
    [[nodiscard]] bool stopEmitter(ParticleEmitterHandle emitter) noexcept;
    void setGroundHeightField(
        container::SharedPtr<const FxGroundHeightFieldSnapshot> groundHeights) noexcept;

    void updateSeconds(float deltaSeconds);
    void updateAuthoredFrames(uint32_t frames);
    void synchronizeAuthoredFrame(uint64_t epoch, uint64_t simulationFrame,
                                  uint32_t logicFramesPerSecond =
                                      kParticleAuthoredFramesPerSecond);
    void setParticleScale(float scale) noexcept;
    void setDynamicAdmissionPolicy(ParticlePriority minimumPriority,
                                   ParticlePriority minimumSkipPriority,
                                   uint32_t skipMask) noexcept;
    [[nodiscard]] float interpolationAlpha() const noexcept {
        return m_renderInterpolationAlpha;
    }
    void reset();
    void setGpuCommandCaptureEnabled(bool enabled);
    [[nodiscard]] ParticleGpuCommandBatch takeGpuCommands();

    [[nodiscard]] std::optional<ParticleRuntimeParticle> particle(ParticleHandle handle) const;
    [[nodiscard]] container::Vector<ParticleRuntimeParticle> snapshotParticles() const;
    // Frame-scoped read-only traversal over the packed AoSoA store. The value
    // reference is valid only for the callback invocation; consumers may copy
    // the compact fields they need but cannot retain runtime block pointers.
    template <typename Visitor>
    void visitParticles(Visitor&& visitor) const {
        size_t denseOrdinal = 0;
        for (const AoSoABlockIndex index : m_activeBlocks) {
            const BillboardParticleBlock& block = m_blocks.get(index);
            for (size_t lane = 0; lane < block.count; ++lane) {
                const ParticleRuntimeParticle value =
                    snapshotLane(block, lane);
                visitor(denseOrdinal++, value);
            }
        }
    }

    [[nodiscard]] size_t particleCount() const noexcept { return m_particleCount; }
    [[nodiscard]] size_t emitterCount() const noexcept { return m_emitters.size(); }
    [[nodiscard]] bool containsEmitter(ParticleEmitterHandle emitter) const noexcept {
        return m_emitters.contains(emitter);
    }
    [[nodiscard]] size_t maximumParticleCount() const noexcept { return m_maximumParticles; }
    [[nodiscard]] size_t ordinaryParticleLimit() const noexcept {
        return m_admission.ordinaryParticleLimit;
    }
    [[nodiscard]] size_t fieldParticleLimit() const noexcept {
        return m_admission.fieldParticleLimit;
    }
    [[nodiscard]] size_t ordinaryParticleCount() const noexcept {
        return m_ordinaryParticleCount;
    }
    [[nodiscard]] size_t fieldParticleCount() const noexcept {
        return m_fieldParticleCount;
    }
    // Frame-boundary quality changes update future admission without
    // destroying live effects or replaying already-consumed FX events. The
    // storage hard ceiling remains fixed for the runtime lifetime.
    void setAdmissionSettings(ParticleAdmissionSettings admission) noexcept;
    [[nodiscard]] size_t alwaysRenderParticleCount() const noexcept {
        return m_alwaysRenderParticleCount;
    }
    [[nodiscard]] size_t maximumEmitterCount() const noexcept { return m_maximumEmitters; }
    [[nodiscard]] size_t activeBlockCount() const noexcept { return m_activeBlocks.size(); }
    [[nodiscard]] const ParticleRuntimeStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] const ParticleRuntimePhaseProfile& lastPhaseProfile()
        const noexcept { return m_lastPhaseProfile; }

private:
    static constexpr size_t kMaximumRelatedSystemDepth = 8;
    struct SlaveEmitterLink final {
        ParticleEmitterHandle master;
        ParticleEmitterHandle slave;
        ParticleVector3 offset;
    };

    struct ControlledEmitterLink final {
        ParticleHandle particle;
        ParticleEmitterHandle emitter;
    };

    struct ParticleDeathRecord final {
        size_t denseIndex = 0;
        ParticleHandle handle;
    };

    struct ParticleIntegrationBatchResult final {
        container::Vector<ParticleDeathRecord> deaths;
        uint64_t integratedParticles = 0;
        uint64_t expiredParticles = 0;
    };

    [[nodiscard]] ParticleEmitterHandle createEmitterInternal(
        const ParticleEmitterSpawn& spawn, uint8_t relationDepth,
        bool externallyDriven, container::Vector<ParticleTemplateId>& recursionStack);
    void primeEmitter(ParticleEmitterHandle emitter);
    void stepOneAuthoredFrame();
    void updateControlledEmitters();
    void updateEmitters();
    void updateParticles();
    void integrateParticleBlocks(
        size_t firstBlock, size_t lastBlock,
        const ParticleSystemCatalog* catalogSnapshot,
        const ParticleEmitterStorage& emitterSnapshot,
        ParticleIntegrationBatchResult& result);
    void commitEmitterPositions() noexcept;
    void updateEmitterWind(ParticleEmitterState& state,
                           const ParticleSystemTemplate& definition);
    void cleanupEmitterRelations();
    void emitBurst(ParticleEmitterHandle emitter, ParticleEmitterState& state,
                   const ParticleSystemTemplate& definition);
    [[nodiscard]] std::optional<ParticleHandle> createParticle(
        ParticleEmitterHandle emitter, ParticleEmitterState& emitterState,
        const ParticleSystemTemplate& definition, uint64_t seed,
        ParticlePriority priorityOverride = ParticlePriority::Count,
        bool enforceAboveGround = true,
        bool captureGpuBirth = true);
    [[nodiscard]] std::optional<ParticleHandle> createSlaveParticle(
        ParticleEmitterHandle emitter, ParticleEmitterState& emitterState,
        const ParticleSystemTemplate& definition, ParticleHandle masterParticle,
        ParticleVector3 offset, uint64_t seed);
    void createPerParticleAttachedEmitter(
        ParticleHandle particle, const ParticleSystemTemplate& definition,
        uint8_t relationDepth, uint64_t seed);
    void eraseParticle(ParticleHandle handle);
    [[nodiscard]] bool admitParticle(const ParticleSystemTemplate& definition,
                                     ParticlePriority priority);
    [[nodiscard]] bool evictOldestAtOrBelow(ParticlePriority maximumPriority,
                                             bool allowSamePriority,
                                             bool alwaysRenderPreemption);
    void linkPriorityTail(ParticleHandle handle, BillboardParticleBlock& block,
                          size_t lane, ParticlePriority priority);
    void unlinkPriority(ParticleHandle handle, BillboardParticleBlock& block,
                        size_t lane);
    void copyLane(BillboardParticleBlock& destination, size_t destinationLane,
                  const BillboardParticleBlock& source, size_t sourceLane);
    void appendGpuBirthCommand(
        const BillboardParticleBlock& block, size_t lane);
    void appendGpuRetireCommand(
        const BillboardParticleBlock& block, size_t lane);
    [[nodiscard]] ParticleRuntimeParticle snapshotLane(const BillboardParticleBlock& block,
                                                       size_t lane) const;

    container::SharedPtr<const ParticleSystemCatalog> m_catalog;
    AoSoABlockStorage<BillboardParticleBlock> m_blocks;
    container::Vector<AoSoABlockIndex> m_activeBlocks;
    ParticleEmitterStorage m_emitters;
    container::Vector<ParticleEmitterHandle> m_frameEmitterScratch;
    ParticleLocationStorage m_particleLocations;
    container::Vector<SlaveEmitterLink> m_slaveEmitters;
    container::Vector<ControlledEmitterLink> m_controlledEmitters;
    container::Array<
        container::Vector<std::pair<ParticleHandle, uint64_t>>,
        kMaximumRelatedSystemDepth + 1u> m_attachedParentScratch;
    container::Array<container::Vector<ParticleTemplateId>,
        kMaximumRelatedSystemDepth + 1u> m_relatedSystemRecursionScratch;
    container::SharedPtr<const FxGroundHeightFieldSnapshot> m_groundHeights;
    size_t m_maximumParticles = 0;
    size_t m_maximumEmitters = 0;
    size_t m_particleCount = 0;
    ParticleAdmissionSettings m_admission;
    ParticleUpdateSettings m_updateSettings;
    container::Vector<ParticleIntegrationBatchResult> m_integrationBatches;
    static constexpr size_t kPriorityCount =
        static_cast<size_t>(ParticlePriority::Count);
    container::Array<ParticleHandle, kPriorityCount> m_priorityHeads{};
    container::Array<ParticleHandle, kPriorityCount> m_priorityTails{};
    container::Array<size_t, kPriorityCount> m_priorityCounts{};
    size_t m_ordinaryParticleCount = 0;
    size_t m_fieldParticleCount = 0;
    size_t m_alwaysRenderParticleCount = 0;
    uint64_t m_nextAdmissionOrdinal = 1;
    uint64_t m_particleGenerationCount = 0;
    uint64_t m_authoredEpoch = 0;
    uint64_t m_authoredSimulationFrame = 0;
    uint64_t m_authoredFrame = 0;
    uint32_t m_authoredLogicFramesPerSecond =
        kParticleAuthoredFramesPerSecond;
    bool m_authoredCursorInitialized = false;
    float m_renderInterpolationAlpha = 0.0f;
    float m_particleScale = 1.0f;
    ParticleRuntimeStats m_stats;
    ParticleRuntimePhaseProfile m_lastPhaseProfile;
    uint64_t m_phaseProfileOrdinal = 0;
    container::Vector<gpu_particle::GpuParticleBirthCommand>
        m_gpuBirthCommands;
    container::Vector<gpu_particle::GpuParticleRetireCommand>
        m_gpuRetireCommands;
    uint32_t m_gpuAuthorityEpoch = 1;
    uint32_t m_gpuCommandSequence = 0;
    bool m_gpuCommandCaptureEnabled = false;
};

} // namespace engine::fx
