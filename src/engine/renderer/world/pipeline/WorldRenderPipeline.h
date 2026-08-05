#pragma once

#include "core/container/hash_containers.h"

#include "engine/renderer/runtime/RendererStats.h"
#include "presentation/render/RenderSceneSnapshot.h"
#include "engine/renderer/world/model/Skeleton.h"

#include <taskflow/taskflow.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <utility>
namespace engine::render {

struct CpuStaticModel;

struct PreparedRenderInstance {
    RenderEntityId id = 0;
    RenderEntityId objectId = 0;
    uint32_t channelIndex = 0;
    container::String modelAsset;
    container::SharedPtr<const Skeleton> skeleton;
    uint64_t skeletonGeneration = 0;
    // Exact cross-frame pose-cache descriptor. Generations identify immutable
    // asset payloads; the sampled values below are compared before an arena
    // slice is copied, so cache correctness never depends on a hash collision.
    uint64_t poseAnimationGeneration = 0;
    float poseSampleTimeSeconds = 0.0f;
    float poseSampleRate = 1.0f;
    // Effective end-frame duration after playback-rate scaling. It lets the
    // endpoint matcher identify Loop/LoopBackwards wrap as a discrete jump
    // without retaining an AnimationClip pointer in the published frame.
    float poseSampleDurationSeconds = 0.0f;
    RenderAnimationMode poseSampleMode = RenderAnimationMode::Loop;
    bool poseCacheReusable = false;
    RenderVisualState visual;
    RenderMatrix worldTransform{};
    size_t poseOffset = 0;
    size_t poseCount = 0;
    bool poseReady = false;
    bool visibilityReady = false;
    // One resolved anchor per ParticleSysBone declaration. Ordinary emitters
    // freeze the bone's state-entry pose in model space; explicitly animated
    // emitters use this frame's current pose. A missing value deliberately
    // falls back to object root.
    size_t particleEmitterBoneOffset = 0;
    size_t particleEmitterBoneCount = 0;
    float boundingRadius = 0.0f;
    RenderVector cullingCenterOffset{};
    float directionalLightScale = 1.0f;
    bool interpolationDisabled = false;
    RenderShadowDescriptor shadow;
    container::Array<std::optional<RenderMatrix>, kRenderWeaponSlotCount>
        weaponLaunchBoneWorldTransforms;
};

struct PreparedProjectileRenderSnapshot final {
    ProjectileRenderSnapshot projectile;
};

// Dense renderer-hot columns for one immutable world endpoint. Cold strings,
// visual descriptors and asset owners remain in PreparedRenderInstance; view
// preparation scans only these columns until a visible object needs packets.
struct PreparedWorldHotSoA final {
    container::Vector<RenderEntityId> ids;
    container::Vector<float> positionX;
    container::Vector<float> positionY;
    container::Vector<float> positionZ;
    container::Vector<float> boundingRadii;

    void rebuild(container::Span<const PreparedRenderInstance> instances);
    [[nodiscard]] bool validFor(size_t count) const noexcept;
};

struct PreparedWorldFrame {
    WorldPreparationStamp stamp;
    RenderViewState sourceView;
    uint64_t simulationFrame = 0;
    uint64_t presentationEpoch = 0;
    uint64_t sessionRevision = 0;
    uint64_t loadingRevision = 0;
    // Preserve the same immutable session-frozen settings handle carried by
    // WorldRenderSnapshot. ParticleScale is a visual descriptor and must not
    // be rediscovered from mutable GameData at the renderer boundary.
    container::SharedPtr<const RenderGameDataSettings> renderGameDataSettings;
    container::SharedPtr<const ResolvedRenderFeatureSnapshot>
        renderFeatureQuality;
    RenderCameraSnapshot camera;
    ScreenFadeRenderState screenFade;
    BlackAndWhiteRenderState blackAndWhite;
    MotionBlurRenderState motionBlur;
    // The request is copied from the sealed game snapshot, while the optional
    // transform is resolved only from this frame's immutable W3D pose. This
    // keeps camera-slave playback independent of ECS, Drawables and GPU
    // assets even though it needs a renderer-side bone lookup.
    CameraSlaveRenderState cameraSlave;
    bool cameraSlaveTargetPresent = false;
    std::optional<RenderMatrix> cameraSlaveBoneWorldTransform;
    SkyboxRenderState skybox;
    TreeSwayRenderState treeSway;
    WeatherRenderState weather;
    ScreenShakeRenderState screenShake;
    ClientOptionsRenderState clientOptions;
    // Object-icon work has no skeletal/GPU preparation dependency. Preserve
    // the sealed values beside the prepared frame so its local overlay is
    // guaranteed to draw the same one-frame-old snapshot as the world pass.
    ObjectIconRenderState objectIcons;
    WorldFeedbackRenderState worldFeedback;
    ObjectUiRenderState objectUi;
    TacticalRadarRenderState tacticalRadar;
    container::Vector<TerrainBibRenderData> terrainBibs;
    ViewCompatibilityRenderState viewCompatibility;
    LocalVisibilityRenderSnapshot localVisibility;
    container::SharedPtr<const TerrainRenderSnapshot> terrain;
    container::Vector<PreparedRenderInstance> visibleInstances;
    // Immutable endpoint-local direct lookup. It is built once when the
    // endpoint is published and reused by interpolation, attachments and
    // sparse FX consumers instead of rebuilding temporary hash tables or
    // linearly scanning every visible channel.
    container::HashMap<RenderEntityId, size_t> visibleInstanceIndicesById;
    PreparedWorldHotSoA hot;
    // Dense B-index -> A-index relation built once when this endpoint is
    // published. UINT32_MAX means the render instance was created in B.
    container::Vector<uint32_t> previousEndpointIndices;
    // A indices not represented by B. They remain drawable while alpha < 1
    // and retire exactly at the authoritative B boundary.
    container::Vector<uint32_t> retiredPreviousEndpointIndices;
    // One byte per B instance. Only compatible same-model/same-clip objects
    // whose root or pose actually changed upload two GPU endpoints.
    container::Vector<uint8_t> interpolationEligible;
    // Model-attached particle emitters have presentation lifetime independent
    // of mesh frustum/distance culling. This list includes every non-hidden
    // live channel that declares ParticleSysBone, including visible ones.
    container::Vector<PreparedRenderInstance> particleEmitterInstances;
    container::Vector<std::optional<RenderMatrix>>
        particleEmitterBoneWorldTransformArena;
    container::Vector<PreparedProjectileRenderSnapshot> projectiles;
    container::Vector<TrackMarkRenderInput> trackMarks;
    container::Vector<RenderMatrix> poseArena;
    container::Vector<uint8_t> visibilityArena;
    // Ordered by source entity and source->transition->active phase. The
    // backend publishes these value-only facts after preparation; it never
    // writes ECS or game clocks from worker threads.
    container::Vector<RenderAnimationCompletionFeedback>
        animationCompletions;
    // Complete live-channel admission set for this endpoint. Unlike
    // visibleInstances it includes hidden, culled and Model=None channels.
    container::Vector<RenderAnimationEndpointAdmission>
        animationEndpointAdmissions;

    [[nodiscard]] container::Span<const RenderMatrix> pose(
        const PreparedRenderInstance& instance) const noexcept;
    [[nodiscard]] std::optional<size_t> visibleInstanceIndexById(
        RenderEntityId id) const noexcept;
    [[nodiscard]] container::Span<const uint8_t> visibility(
        const PreparedRenderInstance& instance) const noexcept;
    [[nodiscard]] container::Span<const std::optional<RenderMatrix>>
    particleEmitterBoneWorldTransforms(
        const PreparedRenderInstance& instance) const noexcept;
};

// All public methods are called by the renderer owner thread. beginPreparation
// seals value-only snapshot data and immutable asset handles before Taskflow
// workers start. Workers write only disjoint prepared spans; they never query
// ECS, mutable cache maps, parsers/decoders or D3D12 objects.
class WorldRenderPipeline {
public:
    WorldRenderPipeline();
    ~WorldRenderPipeline();

    WorldRenderPipeline(const WorldRenderPipeline&) = delete;
    WorldRenderPipeline& operator=(const WorldRenderPipeline&) = delete;

    void registerSkeleton(container::String modelAsset, container::SharedPtr<const Skeleton> skeleton);
    [[nodiscard]] bool registerAnimation(
        container::String modelAsset, container::String animationState,
        container::SharedPtr<const AnimationClip> animation);
    [[nodiscard]] container::String animationRegistrationError(
        container::StringView modelAsset, container::StringView animationState) const;
    void recordModelResolution(
        container::String modelAsset, container::String diagnostic);
    void recordAnimationResolution(
        container::String modelAsset, container::String animationState,
        container::String diagnostic);
    void registerW3dModel(container::String modelAsset, const CpuStaticModel& model);
    void removeModel(container::StringView modelAsset);

    void beginPreparation(WorldRenderSnapshot snapshot,
                          uint32_t viewportWidth = 4,
                          uint32_t viewportHeight = 3);
    bool isPreparing() const;
    const PreparedWorldFrame& finishPreparation();
    [[nodiscard]] bool hasCompletedFrame() const noexcept {
        return m_hasCompletedFrame;
    }
    [[nodiscard]] const PreparedWorldFrame* completedFrame() const noexcept {
        return m_hasCompletedFrame ? &m_completedFrame : nullptr;
    }
    [[nodiscard]] const PreparedWorldFrame* previousFrame() const noexcept {
        return m_hasPreviousFrame ? &m_previousFrame : nullptr;
    }
    [[nodiscard]] const WorldPreparationStats& lastPreparationStats() const noexcept {
        return m_lastPreparationStats;
    }
    void recordViewCullingStats(
        uint32_t distanceCulled, uint32_t frustumCulled) noexcept {
        m_lastPreparationStats.distanceCulledInstances = distanceCulled;
        m_lastPreparationStats.frustumCulledInstances = frustumCulled;
    }
    // Owner-thread map/session boundary. Any outstanding worker preparation
    // is joined before immutable asset handles and prepared-frame values are
    // released; worker tasks never execute concurrently with this reset.
    void resetPresentationEpoch(uint64_t presentationEpoch);

private:
    struct RegisteredModel {
        container::String hierarchyName;
        container::String resolutionError;
        container::SharedPtr<const Skeleton> skeleton;
        container::HashMap<container::String, container::SharedPtr<const AnimationClip>> animations;
        container::HashMap<container::String, container::String> animationErrors;
    };

    enum class PrepareOutcome : uint8_t {
        PreparedStatic,
        PreparedAnimated,
        Invalid,
        Hidden,
    };

    enum class PoseConsumer : uint8_t {
        Ordinary,
        Camera,
        ParticleEmitter,
        TrackMark,
    };

    enum class AttachmentResolution : uint8_t {
        Pending,
        Resolving,
        Resolved,
    };

    // One resolved AttachToBoneInAnotherModule composition. The delta is the
    // pristine-offset correction already applied to the channel's root and
    // pose palette; bone-derived anchors published before the pass must travel
    // with it so every consumer of this frame agrees on one attached position.
    struct ModuleAttachmentComposition final {
        RenderEntityId id = 0;
        RenderMatrix delta{};
    };

    struct FrozenParticleEmitterAnchor final {
        RenderMatrix localTransform{};
        container::String modelAsset;
        container::String boneName;
        uint64_t skeletonGeneration = 0;
    };

    struct RenderPoseInput final {
        PoseConsumer consumer = PoseConsumer::Ordinary;
        container::SharedPtr<const Skeleton> skeleton;
        container::SharedPtr<const AnimationClip> animation;
        float animationTimeSeconds = 0.0f;
        RenderTransform entityTransform;
        RenderAnimationMode animationMode = RenderAnimationMode::Loop;
        float animationRate = 1.0f;
        container::Span<const RenderBoneControl> boneControls;
    };

    struct CompiledPoseSample final {
        container::SharedPtr<const AnimationClip> animation;
        float timeSeconds = 0.0f;
        RenderAnimationMode mode = RenderAnimationMode::Loop;
        float rate = 1.0f;
    };

    struct ResolvedAnimationPresentation final {
        container::String modelAsset;
        RenderVisualState visual;
        container::Array<container::String, kRenderWeaponSlotCount> weaponLaunchBones;
        container::Array<uint32_t, kRenderWeaponSlotCount>
            weaponLaunchBoneSequenceOrdinals{};
        container::Vector<RenderAnimationCompletionFeedback> completions;
        uint32_t terminalFallbackCompletions = 0;
    };

    struct LocalVisibilityMemoryRecord final {
        SharedSnapshotVector<RenderEntitySnapshot>::ElementHandle snapshot;
        uint64_t lastClearFrame = 0;
        uint32_t persistenceTicks = 0;
        RenderLocalVisibilityMemoryPolicy policy =
            RenderLocalVisibilityMemoryPolicy::None;
    };

    PrepareOutcome prepareInstance(size_t index);
    void prepareRange(size_t beginIndex, size_t endIndex);
    void resolveContainerAttachments();
    void resolveModuleAttachments();
    void prepareCameraSlave();
    void prepareTrackMarks();
    void prepareParticleEmitterInstances();
    void prepareProjectiles();
    bool evaluatePose(
        const RenderPoseInput& input,
        container::Span<RenderMatrix> poseOutput,
        container::Span<uint8_t> visibilityOutput = {});
    [[nodiscard]] bool tryReusePreviousPose(
        PreparedRenderInstance& prepared,
        const CompiledPoseSample& sample,
        container::Span<RenderMatrix> poseOutput,
        container::Span<uint8_t> visibilityOutput);
    [[nodiscard]] static CompiledPoseSample compilePoseSample(
        RenderVisualState& visual, const RegisteredModel& model,
        RenderEntityId objectId, uint32_t channelIndex);
    void resolveAnimationPresentationInto(
        ResolvedAnimationPresentation& output,
        const RenderEntitySnapshot& instance) const;
    void applyLocalVisibilityObjectMemory(WorldRenderSnapshot& snapshot);
    void recordPreparationTaskStart() noexcept;
    void recordPreparationTaskCompletion() noexcept;
    [[nodiscard]] container::Span<RenderMatrix> preparationPose(
        const PreparedRenderInstance& instance) noexcept;
    [[nodiscard]] container::Span<uint8_t> preparationVisibility(
        const PreparedRenderInstance& instance) noexcept;
    [[nodiscard]] container::Span<std::optional<RenderMatrix>>
    preparationParticleEmitterBoneWorldTransforms(
        const PreparedRenderInstance& instance) noexcept;

    tf::Taskflow m_taskflow;
    tf::Future<void> m_pendingWork;
    bool m_hasCompletedFrame = false;
    container::Vector<tf::Task> m_entityPreparationTasks;
    container::HashSet<RenderEntityId> m_currentVisibilityEntities;
    container::HashMap<RenderEntityId, size_t> m_attachmentInputIndexScratch;
    container::HashMap<RenderEntityId, size_t> m_attachmentRenderIndexScratch;
    // All prepared Draw channels grouped by object and authored channel order.
    // Container attachment bones are Drawable-wide in RefCode, so resolving
    // only the first channel would incorrectly fall back to the vehicle root
    // whenever the requested turret/fire-point bone belongs to a later Draw.
    container::Vector<size_t> m_attachmentObjectChannelScratch;
    container::HashMap<RenderEntityId, std::pair<size_t, size_t>>
        m_attachmentObjectChannelRangesScratch;
    // Built by the owner thread before workers launch and then read-only for
    // the complete preparation taskflow.
    container::HashMap<RenderEntityId, size_t> m_previousPoseById;
    container::Vector<AttachmentResolution> m_attachmentResolutionScratch;
    container::Vector<size_t> m_attachmentParentScratch;
    container::Vector<size_t> m_attachmentDepthScratch;
    container::Vector<size_t> m_attachmentPathScratch;
    container::Vector<size_t> m_attachmentOrderScratch;
    container::Vector<size_t> m_attachmentLayerOffsetsScratch;
    container::Vector<size_t> m_attachmentLayerCursorsScratch;
    container::Vector<std::future<void>> m_attachmentTasksScratch;
    container::Vector<size_t> m_moduleAttachmentChildScratch;
    container::Vector<size_t> m_moduleAttachmentSiblingScratch;
    container::HashSet<RenderEntityId> m_moduleAttachmentObjectScratch;
    container::Vector<ModuleAttachmentComposition>
        m_moduleAttachmentComposedScratch;
    container::HashMap<container::String, RegisteredModel> m_models;
    container::HashMap<RenderEntityId, LocalVisibilityMemoryRecord>
        m_localVisibilityMemory;
    uint64_t m_localVisibilityMemoryEpoch = UINT64_MAX;
    uint64_t m_localVisibilityMemoryFrame = 0;
    WorldRenderSnapshot m_snapshot;
    container::Vector<PreparedRenderInstance> m_preparedByInput;
    container::Vector<ResolvedAnimationPresentation>
        m_resolvedPresentationsByInput;
    container::Vector<RenderMatrix> m_preparationPoseArena;
    container::Vector<uint8_t> m_preparationVisibilityArena;
    container::Vector<container::Vector<RenderAnimationCompletionFeedback>>
        m_animationCompletionsByInput;
    container::Vector<uint32_t> m_completionFallbacksByInput;
    container::Vector<PreparedRenderInstance> m_preparedParticleEmitterInstances;
    container::Vector<std::optional<RenderMatrix>>
        m_preparedParticleEmitterBoneWorldTransformArena;
    // Reused once per preparation. ActiveBody auto-particle categories use
    // one group across all Draw channels of an Object, matching the original
    // object-wide pristine-bone query without a per-frame allocation.
    container::HashMap<uint64_t, uint32_t> m_bodyParticleSelectionCounts;
    // RefCode creates a state's ParticleSysBone systems after installing that
    // state's animation, stores their model-space bone transforms, and only
    // resamples them when PARTICLES_ATTACHED_TO_ANIMATED_BONES is authored.
    // Identity includes object/channel/phase/declaration, so this cache has the
    // same lifetime as one active authored emitter set.
    container::HashMap<uint64_t, FrozenParticleEmitterAnchor>
        m_frozenParticleEmitterAnchors;
    container::HashSet<uint64_t> m_liveFrozenParticleEmitterAnchors;
    container::Vector<uint64_t> m_staleFrozenParticleEmitterAnchors;
    container::Vector<TrackMarkRenderInput> m_preparedTrackMarks;
    CameraSlaveRenderState m_preparedCameraSlave;
    RenderVisualState m_cameraVisualScratch;
    bool m_preparedCameraSlaveTargetPresent = false;
    std::optional<RenderMatrix> m_preparedCameraSlaveBoneWorldTransform;
    PreparedWorldFrame m_completedFrame;
    PreparedWorldFrame m_previousFrame;
    bool m_hasPreviousFrame = false;
    WorldPreparationStats m_lastPreparationStats;
    std::chrono::steady_clock::time_point m_preparationStarted{};
    uint32_t m_preparationTaskCount = 0;
    uint32_t m_scheduledPreparationTaskCount = 0;
    std::atomic<uint32_t> m_completedPreparationTaskCount{0};
    std::atomic<uint64_t> m_preparationWorkerMask{0};
    std::atomic<uint32_t> m_animatedPreparedCount{0};
    std::atomic<uint32_t> m_hiddenCount{0};
    std::atomic<uint32_t> m_invalidCount{0};
    std::atomic<uint32_t> m_poseEvaluationCount{0};
    std::atomic<uint32_t> m_poseReuseCount{0};
    std::atomic<uint32_t> m_ordinaryPoseEvaluationCount{0};
    std::atomic<uint32_t> m_cameraPoseEvaluationCount{0};
    std::atomic<uint32_t> m_emitterPoseEvaluationCount{0};
    std::atomic<uint32_t> m_trackMarkPoseEvaluationCount{0};
    std::atomic<uint32_t> m_cameraPoseReuseCount{0};
    std::atomic<uint32_t> m_emitterPoseReuseCount{0};
    uint32_t m_completionFallbackCount = 0;
    std::atomic<uint32_t> m_cameraPoseFallbackCount{0};
    std::atomic<uint32_t> m_emitterRootFallbackCount{0};
    std::atomic<uint64_t> m_poseJointCount{0};
    std::atomic<uint64_t> m_poseAnimationChannelCount{0};
    std::atomic<uint64_t> m_poseControlCount{0};
    std::atomic<uint64_t> m_ordinaryPoseNanoseconds{0};
    std::atomic<uint64_t> m_cameraPoseNanoseconds{0};
    std::atomic<uint64_t> m_emitterPoseNanoseconds{0};
    std::atomic<uint64_t> m_trackMarkPoseNanoseconds{0};
    uint32_t m_poseArenaGrowths = 0;
    uint32_t m_visibilityArenaGrowths = 0;
    uint64_t m_poseArenaCapacityHighWaterBytes = 0;
    uint64_t m_visibilityArenaCapacityHighWaterBytes = 0;
    uint64_t m_poseArenaRequestedJoints = 0;
    uint32_t m_poseArenaRejectedInstances = 0;
    uint32_t m_containerCapacityGrowths = 0;
    uint64_t m_retainedContainerCapacityHighWaterBytes = 0;
    uint64_t m_retainedNestedCapacityBytes = 0;
    uint64_t m_retainedNestedCapacityHighWaterBytes = 0;
    uint32_t m_nestedCapacityGrowthFrames = 0;
    uint64_t m_nestedCapacityLastSampleFrame = 0;
    bool m_nestedCapacitySampleValid = false;
    bool m_preparing = false;
};

} // namespace engine::render
