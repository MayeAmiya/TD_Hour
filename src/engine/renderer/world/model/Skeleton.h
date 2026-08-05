#pragma once

#include "core/container/hash_containers.h"
#include "core/container/container_types.h"

#include "presentation/render/RenderWorldDescriptorContracts.h"

#include "data/w3d/W3dTypes.h"
#include <optional>
namespace engine::render {

struct SkeletonJoint {
    container::String name;
    int32_t parentIndex = -1;
    RenderMatrix localRestTransform{};
};

struct SkeletonBuildDiagnostics {
    uint32_t invalidParentCount = 0;
    uint32_t cycleNodeCount = 0;
};

class Skeleton {
public:
    static container::SharedPtr<const Skeleton> fromW3d(
        const data::w3d::ParsedHierarchy& hierarchy,
        SkeletonBuildDiagnostics* diagnostics = nullptr);

    const container::Vector<SkeletonJoint>& joints() const { return m_joints; }
    const container::Vector<size_t>& evaluationOrder() const noexcept {
        return m_evaluationOrder;
    }
    [[nodiscard]] container::Span<const RenderMatrix> modelRestPose() const
        noexcept {
        return m_modelRestPose;
    }
    bool empty() const { return m_joints.empty(); }
    [[nodiscard]] uint64_t generation() const noexcept {
        return m_generation;
    }
    // W3D HTreeClass::Get_Bone_Index uses stricmp. Keep the same ASCII
    // spelling rule at the renderer boundary so camera-slave bone authoring
    // is not accidentally made case-sensitive by modern containers.
    [[nodiscard]] std::optional<size_t> findJointIndexInsensitive(
        container::StringView name) const noexcept;
    // Returns the contiguous W3D sequence prefix01, prefix02 ... compiled at
    // skeleton-build time. Three-digit ordinals continue naturally at 100;
    // a missing ordinal terminates the sequence exactly like legacy lookup.
    [[nodiscard]] container::Span<const size_t>
    numberedJointIndicesInsensitive(
        container::StringView prefix) const noexcept;

private:
    struct NumberedJointSequence final {
        container::String prefix;
        container::Vector<size_t> indices;
    };

    container::Vector<SkeletonJoint> m_joints;
    uint64_t m_generation = 0;
    container::Vector<size_t> m_evaluationOrder;
    container::Vector<RenderMatrix> m_modelRestPose;
    container::HashMap<uint64_t, container::Vector<size_t>>
        m_jointIndicesByLowerNameHash;
    container::HashMap<uint64_t, container::Vector<NumberedJointSequence>>
        m_numberedJointIndicesByLowerPrefixHash;
};

class AnimationClip {
public:
    static container::SharedPtr<const AnimationClip> fromW3d(const data::w3d::ParsedAnimation& animation);

    [[nodiscard]] uint64_t generation() const noexcept { return m_generation; }
    uint32_t frameCount() const { return m_frameCount; }
    uint32_t frameRate() const { return m_frameRate; }
    const container::String& hierarchyName() const noexcept { return m_hierarchyName; }
    [[nodiscard]] uint64_t estimatedByteSize() const noexcept;
    [[nodiscard]] bool isComplete(
        float animationTimeSeconds,
        RenderAnimationMode animationMode,
        float animationRate = 1.0f) const noexcept;
    [[nodiscard]] float completionTimeSeconds(
        float animationRate = 1.0f) const noexcept;
    const container::Vector<data::w3d::ParsedAnimationChannel>& channels() const { return m_channels; }
    const container::Vector<data::w3d::ParsedAnimationVisibilityChannel>& visibilityChannels() const {
        return m_visibilityChannels;
    }

private:
    uint64_t m_generation = 0;
    container::String m_hierarchyName;
    uint32_t m_frameCount = 0;
    uint32_t m_frameRate = 0;
    container::Vector<data::w3d::ParsedAnimationChannel> m_channels;
    container::Vector<data::w3d::ParsedAnimationVisibilityChannel> m_visibilityChannels;
};

// Reusable worker-local storage for pose evaluation. WorldRenderPipeline
// keeps one instance per executor thread so animated entities do not allocate
// four temporary arrays for every pose. Output palettes/visibility remain
// frame-owned by PreparedRenderInstance and never alias this scratch.
struct SkeletonEvaluationScratch final {
    container::Vector<RenderVector> translations;
    container::Vector<RenderQuaternion> rotations;
    container::Vector<std::optional<size_t>> controlJoints;
    container::Vector<RenderMatrix> localPose;
};

// Evaluates pose and, when requested, hierarchy visibility from the same
// animation frame. The output containers are replaced in-place and may keep
// their capacity across frames. `visibilityOutput == nullptr` skips visibility
// work for camera/FX-only consumers.
void evaluateSkeletonPoseInto(
    const Skeleton& skeleton, const AnimationClip* animation,
    float animationTimeSeconds, const RenderTransform& entityTransform,
    RenderAnimationMode animationMode, float animationRate,
    container::Span<const RenderBoneControl> boneControls,
    container::Vector<RenderMatrix>& poseOutput,
    container::Vector<uint8_t>* visibilityOutput,
    SkeletonEvaluationScratch& scratch);

// Arena-targeted form used when the caller has already reserved one sealed
// frame slice. The spans must be empty (visibility only) or large enough for
// every skeleton joint; no allocation or ownership escapes through them.
[[nodiscard]] bool evaluateSkeletonPoseIntoSpan(
    const Skeleton& skeleton, const AnimationClip* animation,
    float animationTimeSeconds, const RenderTransform& entityTransform,
    RenderAnimationMode animationMode, float animationRate,
    container::Span<const RenderBoneControl> boneControls,
    container::Span<RenderMatrix> poseOutput,
    container::Span<uint8_t> visibilityOutput,
    SkeletonEvaluationScratch& scratch);

container::Vector<RenderMatrix> evaluateSkeletonRestPose(const Skeleton& skeleton,
                                                   const RenderTransform& entityTransform);
container::Vector<RenderMatrix> evaluateSkeletonPose(const Skeleton& skeleton, const AnimationClip* animation,
                                               float animationTimeSeconds,
                                               const RenderTransform& entityTransform,
                                               RenderAnimationMode animationMode = RenderAnimationMode::Loop,
                                               float animationRate = 1.0f,
                                               container::Span<const RenderBoneControl>
                                                   boneControls = {});
// WW3D bit channels animate visibility for hierarchy-attached mesh parts.
// Skinned vertices remain shader-visible; their per-vertex visibility is a
// separate capability and is deliberately not implied by this API.
container::Vector<uint8_t> evaluateSkeletonVisibility(const Skeleton& skeleton, const AnimationClip* animation,
                                                float animationTimeSeconds,
                                                RenderAnimationMode animationMode = RenderAnimationMode::Loop,
                                                float animationRate = 1.0f);

} // namespace engine::render
