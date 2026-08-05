#pragma once

#include "core/container/hash_containers.h"

#include "presentation/fx/runtime/FxPresentationSnapshot.h"
#include "FxPresentationCommands.h"
#include "ParticleRuntime.h"
#include "presentation/fx/content/FxListCatalog.h"

#include <cstddef>
#include <cstdint>
namespace engine::fx {

struct FxRuntimeStats final {
    uint64_t submittedInvocations = 0;
    uint64_t resolvedFxLists = 0;
    uint64_t missingFxLists = 0;
    uint64_t spawnedParticleEmitters = 0;
    uint64_t spawnedDirectParticleEmitters = 0;
    uint64_t rejectedParticleEmitters = 0;
    uint64_t unsupportedNuggets = 0;
    uint64_t approximatedBoneNuggets = 0;
    uint64_t resolvedBoneNuggets = 0;
    uint64_t missingBonePoses = 0;
    uint64_t groundHeightRequests = 0;
    uint64_t recursionRejections = 0;
    uint64_t expansionBudgetRejections = 0;
    uint64_t duplicateInvocations = 0;
    uint64_t emittedSoundCommands = 0;
    uint64_t emittedRayCommands = 0;
    uint64_t emittedLaserCommands = 0;
    uint64_t emittedRopeCommands = 0;
    uint64_t emittedTracerCommands = 0;
    uint64_t emittedLightPulseCommands = 0;
    uint64_t emittedTerrainScorchCommands = 0;
    uint64_t emittedViewShakeCommands = 0;
    uint64_t rejectedPresentationCommands = 0;
};

// Presentation-owned FXList executor. It consumes the lossless invocation
// stream, resolves immutable catalog definitions and materializes billboard
// ParticleSystem nuggets into the AoSoA ParticleRuntime. Renderer/device
// resources remain outside this class.
class FxRuntime final {
public:
    FxRuntime(container::SharedPtr<const ParticleSystemCatalog> particles,
              container::SharedPtr<const FxListCatalog> fxLists,
              size_t maximumParticles = 8192,
              size_t initialEmitterCapacity = 256,
              size_t maximumEmitters = 4096,
              ParticleAdmissionSettings particleAdmission = {},
              size_t maximumAttachedEmitters = 16384,
              size_t maximumPresentationCommands = 16384);

    void submit(const FxPresentationSnapshot& snapshot);
    // Renderer path: seal exactly-once admission before bone-name discovery,
    // then execute the admitted stream without touching the ledger again.
    [[nodiscard]] container::Vector<FxPresentationInvocation> admitInvocations(
        const FxPresentationSnapshot& snapshot);
    void admitInvocationsInto(
        container::Vector<FxPresentationInvocation>& output,
        const FxPresentationSnapshot& snapshot);
    void submitDeferredInvocations(const FxPresentationSnapshot& snapshot,
                                   bool alreadyAdmitted = false);
    void submitDeferredInvocations(
        const FxPresentationSnapshot& snapshot,
        container::Span<const FxPresentationInvocation> invocations,
        bool alreadyAdmitted);
    // Clears the per-submission stop tombstones after the admitted stream has
    // executed in confirmed-frame/stream order.
    void completeDeferredInvocationBarrier();
    void updateBonePoses(container::Vector<FxPresentationBonePose> poses);
    void updateBonePosesRetained(
        container::Span<const FxPresentationBonePose> poses);
    void updateModelParticleEmitters(
        const container::Vector<FxModelParticleEmitterPose>& emitters);
    void updateVehicleParticleEmitters(
        const container::Vector<FxVehicleParticleEmitterPose>& emitters);
    // Advance authored state from the lossless simulation cursor. Repeated
    // presentation of one confirmed frame is idempotent; forward gaps are
    // replayed without a catch-up cap.
    void synchronizeSimulationFrame(uint64_t simulationFrame);
    void updateSeconds(float deltaSeconds);
    void updateAuthoredFrames(uint32_t frames);
    [[nodiscard]] FxPresentationCommandBatch takeCommands();
    void reset();

    [[nodiscard]] uint64_t sessionEpoch() const noexcept { return m_sessionEpoch; }
    // Highest simulation frame admitted by the lossless FX stream. Prepared
    // world frames can lag that stream; renderer-side clock advancement must
    // never use such a stale frame to rewind ParticleRuntime.
    [[nodiscard]] uint64_t lastSubmittedSimulationFrame() const noexcept {
        return m_lastSubmittedSimulationFrame;
    }
    [[nodiscard]] size_t attachedEmitterCount() const noexcept { return m_attachments.size(); }
    [[nodiscard]] size_t vehicleEmitterCount() const noexcept {
        return m_vehicleParticleEmitters.size();
    }
    [[nodiscard]] const FxRuntimeStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] ParticleRuntime& particles() noexcept { return m_particles; }
    [[nodiscard]] const ParticleRuntime& particles() const noexcept { return m_particles; }
    struct ResolvedAnchor final {
        FxPresentationAnchor anchor;
        bool attachmentAlive = false;
    };
    [[nodiscard]] ResolvedAnchor resolveCurrentAnchor(
        const FxTypedAnchor& anchor) const noexcept;
    // Collects only bones needed by the admitted invocation set or currently
    // live attached FX. Callers retain and de-duplicate the output; no ECS or
    // renderer object is accessed here.
    void collectBonePoseDemands(
        container::Span<const FxPresentationInvocation> invocations,
        container::Vector<FxBonePoseDemand>& output) const;
    void appendActiveBonePoseDemands(
        container::Vector<FxBonePoseDemand>& output) const;

private:
    struct AttachedEmitter final {
        ParticleEmitterHandle emitter;
        uint64_t objectKey = 0;
        container::String boneName;
        ParticleVector3 localOffset;
        float rotateX = 0.0f;
        float rotateY = 0.0f;
        float rotateZ = 0.0f;
        // ParticleSystemManager::createAttachedParticleSystemID creates a
        // local-identity system and ParticleSystem::update concatenates the
        // complete parent transform every frame. This is independent from an
        // FXList nugget's authored OrientToObject switch.
        bool inheritsParentTransform = false;
        bool orientToObject = false;
        uint64_t group = 0;
    };

    struct ModelParticleEmitter final {
        ParticleEmitterHandle emitter;
        uint64_t emitterKey = 0;
        uint64_t objectKey = 0;
        container::String boneName;
        container::String particleSystem;
    };

    struct VehicleParticleEmitter final {
        ParticleEmitterHandle emitter;
        uint64_t emitterKey = 0;
        uint64_t objectKey = 0;
        uint64_t lastTriggerSequence = 0;
        container::String particleSystem;
    };

    struct BeamEndpointEmitter final {
        ParticleEmitterHandle emitter;
        uint64_t beamIdentity = 0;
        uint64_t stopAtFrame = 0;
        bool targetEndpoint = false;
        FxTypedAnchor anchor;
    };

    void updateAttachments(const container::Vector<FxPresentationAnchor>& objects);
    void updateBeamEndpointEmitters();
    void removeDeadAttachments();
    void stopBeamEndpointEmitters(uint64_t beamIdentity) noexcept;
    void ensureBeamEndpointEmitter(
        uint64_t beamIdentity, bool targetEndpoint,
        const container::String& particleSystem, const FxTypedAnchor& anchor,
        std::optional<uint32_t> lifetimeFrames, uint64_t stopAtFrame,
        uint64_t seed);
    void executeInvocation(const FxPresentationInvocation& invocation,
                           bool alreadyAdmitted = false);
    void executeDefinition(const FxListDefinition& definition,
                           const FxPresentationInvocation& invocation,
                           uint64_t seed, uint32_t depth,
                           uint32_t& expansionBudget,
                           container::Vector<FxListId>& recursionStack);
    void executeParticleNugget(const FxParticleSystemNugget& nugget,
                               const FxPresentationInvocation& invocation,
                               uint64_t seed);
    void executeDirectParticle(
        const FxPresentationDirectParticle& direct,
        const FxPresentationInvocation& invocation, uint64_t seed);
    void executeDirectBeam(const FxPresentationDirectBeam& direct,
                           const FxPresentationInvocation& invocation,
                           uint64_t seed);
    void executeDirectScorch(const FxPresentationDirectScorch& direct,
                             const FxPresentationInvocation& invocation);
    void executeDirectRope(const FxPresentationDirectRope& direct,
                           const FxPresentationInvocation& invocation);
    void collectDefinitionBonePoseDemands(
        const FxListDefinition& definition,
        uint64_t objectKey,
        container::Vector<FxBonePoseDemand>& output,
        uint32_t depth,
        container::Vector<FxListId>& recursionStack) const;
    [[nodiscard]] const FxPresentationBonePose* findBonePose(
        uint64_t objectKey, container::StringView boneName) const noexcept;
    [[nodiscard]] const FxPresentationBonePose* findBonePosePrefix(
        uint64_t objectKey, container::StringView bonePrefix,
        uint64_t seed, uint32_t sequenceOrdinal,
        bool fallbackToBare) const;
    void stopAttachedParticleGroup(uint64_t objectKey, uint64_t group,
                                   uint64_t stopSequence);
    void stopAllAttachedParticles(uint64_t objectKey, uint64_t stopSequence);
    [[nodiscard]] bool allowAttachedParticleStart(
        uint64_t objectKey, uint64_t group, uint64_t streamSequence);
    [[nodiscard]] FxTypedAnchor invocationAnchor(
        const FxPresentationInvocation& invocation) const;
    [[nodiscard]] FxTypedAnchor secondaryAnchor(
        const FxPresentationInvocation& invocation) const;
    [[nodiscard]] bool rememberInvocation(
        const FxPresentationInvocation& invocation);
    [[nodiscard]] bool canAppendCommand() const noexcept;

    container::SharedPtr<const ParticleSystemCatalog> m_particleCatalog;
    container::SharedPtr<const FxListCatalog> m_fxListCatalog;
    ParticleRuntime m_particles;
    container::Vector<AttachedEmitter> m_attachments;
    // Stops and starts are evaluated in admitted stream order. This tombstone
    // prevents an older start from resurrecting an emitter after its state
    // group was stopped within the same presentation barrier.
    container::TreeMap<std::pair<uint64_t, uint64_t>, uint64_t>
        m_attachmentGroupStops;
    container::TreeMap<uint64_t, uint64_t> m_attachmentObjectStops;
    // Last immutable object anchors admitted for this presentation frame.
    // Prepared bone poses arrive later in the renderer pass; retaining these
    // anchors lets updateBonePoses correct root-fallback emitters immediately.
    container::Vector<FxPresentationAnchor> m_attachmentObjects;
    container::Vector<ModelParticleEmitter> m_modelParticleEmitters;
    container::Vector<ModelParticleEmitter> m_modelParticleEmitterScratch;
    container::Vector<VehicleParticleEmitter> m_vehicleParticleEmitters;
    container::Vector<VehicleParticleEmitter> m_vehicleParticleEmitterScratch;
    container::Vector<BeamEndpointEmitter> m_beamEndpointEmitters;
    container::Vector<FxPresentationBonePose> m_bonePoses;
    container::Vector<FxListId> m_fxListRecursionScratch;
    container::SharedPtr<const FxGroundHeightFieldSnapshot> m_groundHeights;
    container::SharedPtr<const LegacyBeamTemplateCatalog> m_legacyBeamTemplates;
    FxPresentationCommandBatch m_commands;
    container::Deque<uint64_t> m_recentEventOrder;
    container::HashSet<uint64_t> m_recentEventIds;
    container::TreeSet<uint64_t> m_recentStreamSequences;
    uint64_t m_lastConsumedStreamSequence = 0;
    uint64_t m_sessionEpoch = 0;
    uint64_t m_lastSubmittedSimulationFrame = 0;
    uint32_t m_logicFramesPerSecond =
        kParticleAuthoredFramesPerSecond;
    size_t m_maximumAttachedEmitters = 16384;
    size_t m_maximumPresentationCommands = 16384;
    bool m_submissionFrameInitialized = false;
    FxRuntimeStats m_stats;
};

} // namespace engine::fx
