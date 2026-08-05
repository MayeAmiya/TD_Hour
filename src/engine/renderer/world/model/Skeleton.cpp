#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "engine/renderer/world/model/Skeleton.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>

namespace engine::render {
namespace {

std::atomic<uint64_t> g_nextSkeletonGeneration{1};
std::atomic<uint64_t> g_nextAnimationGeneration{1};

container::String readName(const char* name, size_t capacity) {
    size_t length = 0;
    while (length < capacity && name[length] != '\0') ++length;
    return container::String(name, length);
}

RenderMatrix makeTransform(const data::w3d::Vector3& translation,
                           const data::w3d::Quaternion& rotation) {
    return RenderMatrix::from_trs(
        RenderVector::one(),
        RenderQuaternion{rotation.q[0], rotation.q[1], rotation.q[2], rotation.q[3]}.normalized(),
        RenderVector{translation.x, translation.y, translation.z});
}

RenderMatrix makeEntityTransform(const RenderTransform& transform) {
    return RenderMatrix::from_trs(transform.scale, transform.orientation.normalized(), transform.position);
}

RenderMatrix multiply(const RenderMatrix& lhs, const RenderMatrix& rhs) {
    return lhs * rhs;
}

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] uint64_t lowerAsciiHash(container::StringView value) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const char character : value) {
        unsigned char folded = static_cast<unsigned char>(character);
        if (folded >= static_cast<unsigned char>('A') &&
            folded <= static_cast<unsigned char>('Z')) {
            folded = static_cast<unsigned char>(folded + ('a' - 'A'));
        }
        hash ^= static_cast<uint64_t>(folded);
        hash *= 1099511628211ull;
    }
    return hash;
}

struct NumberedJointName final {
    container::StringView prefix;
    uint32_t ordinal = 0;
};

[[nodiscard]] std::optional<NumberedJointName> parseNumberedJointName(
    container::StringView name) noexcept {
    size_t suffixBegin = name.size();
    while (suffixBegin > 0u && name[suffixBegin - 1u] >= '0' &&
           name[suffixBegin - 1u] <= '9') {
        --suffixBegin;
    }
    const size_t digitCount = name.size() - suffixBegin;
    if (suffixBegin == 0u || digitCount < 2u || digitCount > 3u) {
        return std::nullopt;
    }
    uint32_t ordinal = 0;
    for (size_t index = suffixBegin; index < name.size(); ++index) {
        ordinal = ordinal * 10u +
            static_cast<uint32_t>(name[index] - '0');
    }
    if (ordinal == 0u || ordinal > 999u) return std::nullopt;
    const size_t canonicalDigits = ordinal < 100u ? 2u : 3u;
    if (digitCount != canonicalDigits) return std::nullopt;
    return NumberedJointName{
        .prefix = name.substr(0u, suffixBegin),
        .ordinal = ordinal,
    };
}

[[nodiscard]] std::optional<size_t> resolveBoneControlJoint(
    const Skeleton& skeleton, const RenderBoneControl& control) {
    if (!control.boneNameIsPrefix) {
        return skeleton.findJointIndexInsensitive(control.boneName);
    }
    container::Span<const size_t> numbered =
        skeleton.numberedJointIndicesInsensitive(control.boneName);
    if (numbered.size() > 99u) numbered = numbered.first(99u);
    if (!numbered.empty()) {
        const uint32_t sequence = std::max<uint32_t>(
            1u, control.boneNameSequenceOrdinal);
        return numbered[(sequence - 1u) % numbered.size()];
    }
    return control.boneNamePrefixFallsBackToBare
        ? skeleton.findJointIndexInsensitive(control.boneName)
        : std::optional<size_t>{};
}

float animationFrame(const AnimationClip* animation, float animationTimeSeconds,
                     RenderAnimationMode animationMode, float animationRate) {
    if (!animation || animation->frameCount() == 0 || animation->frameRate() == 0) return 0.0f;
    const float endFrame = static_cast<float>(animation->frameCount() - 1);
    if (endFrame <= 0.0f) return 0.0f;
    const float frame = std::max(0.0f, animationTimeSeconds) *
                        std::max(0.0f, animationRate) * animation->frameRate();
    switch (animationMode) {
    case RenderAnimationMode::Manual:
    case RenderAnimationMode::Once:
        return std::min(frame, endFrame);
    case RenderAnimationMode::LoopPingPong: {
        const float phase = std::fmod(frame, endFrame * 2.0f);
        return phase <= endFrame ? phase : endFrame * 2.0f - phase;
    }
    case RenderAnimationMode::LoopBackwards:
        return endFrame - std::fmod(frame, endFrame);
    case RenderAnimationMode::OnceBackwards:
        return std::max(endFrame - frame, 0.0f);
    case RenderAnimationMode::Loop:
        return std::fmod(frame, endFrame);
    }
    return 0.0f;
}

} // namespace

container::SharedPtr<const Skeleton> Skeleton::fromW3d(
    const data::w3d::ParsedHierarchy& hierarchy,
    SkeletonBuildDiagnostics* diagnostics) {
    SkeletonBuildDiagnostics localDiagnostics;
    const size_t jointCount = hierarchy.pivots.size();
    container::Vector<int32_t> sanitizedParents(jointCount, -1);
    for (size_t pivotIndex = 0; pivotIndex < jointCount; ++pivotIndex) {
        const uint32_t parent = hierarchy.pivots[pivotIndex].parentIdx;
        if (parent == UINT32_MAX) continue;
        if (parent >= jointCount || parent == pivotIndex) {
            ++localDiagnostics.invalidParentCount;
            continue;
        }
        sanitizedParents[pivotIndex] = static_cast<int32_t>(parent);
    }

    // Parent links form a functional graph. Detect every multi-node cycle and
    // promote all participating pivots to roots, so pose evaluation never
    // depends on traversal order or its unresolved-joint fallback.
    constexpr size_t NoPathPosition = std::numeric_limits<size_t>::max();
    container::Vector<uint8_t> visitState(jointCount, 0);
    container::Vector<size_t> pathPosition(jointCount, NoPathPosition);
    container::Vector<size_t> path;
    path.reserve(jointCount);
    for (size_t start = 0; start < jointCount; ++start) {
        if (visitState[start] != 0) continue;
        path.clear();
        int32_t current = static_cast<int32_t>(start);
        while (current >= 0 && visitState[static_cast<size_t>(current)] == 0) {
            const size_t index = static_cast<size_t>(current);
            visitState[index] = 1;
            pathPosition[index] = path.size();
            path.push_back(index);
            current = sanitizedParents[index];
        }

        if (current >= 0) {
            const size_t cycleEntry = static_cast<size_t>(current);
            if (visitState[cycleEntry] == 1 &&
                pathPosition[cycleEntry] != NoPathPosition) {
                for (size_t pathIndex = pathPosition[cycleEntry];
                     pathIndex < path.size(); ++pathIndex) {
                    sanitizedParents[path[pathIndex]] = -1;
                    ++localDiagnostics.cycleNodeCount;
                }
            }
        }

        for (const size_t index : path) {
            visitState[index] = 2;
            pathPosition[index] = NoPathPosition;
        }
    }

    auto result = std::make_shared<Skeleton>();
    result->m_generation = g_nextSkeletonGeneration.fetch_add(
        1u, std::memory_order_relaxed);
    if (result->m_generation == 0u) {
        result->m_generation = g_nextSkeletonGeneration.fetch_add(
            1u, std::memory_order_relaxed);
    }
    result->m_joints.reserve(jointCount);
    result->m_jointIndicesByLowerNameHash.reserve(jointCount);
    for (size_t pivotIndex = 0; pivotIndex < jointCount; ++pivotIndex) {
        const auto& pivot = hierarchy.pivots[pivotIndex];
        SkeletonJoint joint;
        joint.name = readName(pivot.name, data::w3d::NAME_LEN);
        joint.parentIndex = sanitizedParents[pivotIndex];
        // HTreeClass always replaces pivot zero with the object's root
        // transform. Its authored base transform and animation channels are
        // never evaluated, including in the base-pose path.
        joint.localRestTransform = pivotIndex == 0
            ? RenderMatrix::identity()
            : makeTransform(pivot.translation, pivot.rotation);
        result->m_joints.push_back(std::move(joint));
        const container::String& jointName = result->m_joints.back().name;
        if (!jointName.empty()) {
            auto [bucket, inserted] =
                result->m_jointIndicesByLowerNameHash.emplace(
                    lowerAsciiHash(jointName), container::Vector<size_t>{});
            static_cast<void>(inserted);
            bucket->second.push_back(pivotIndex);
        }
    }

    constexpr size_t kMissingJoint = std::numeric_limits<size_t>::max();
    result->m_numberedJointIndicesByLowerPrefixHash.reserve(jointCount);
    for (size_t jointIndex = 0; jointIndex < jointCount; ++jointIndex) {
        const container::String& jointName = result->m_joints[jointIndex].name;
        const std::optional<NumberedJointName> numbered =
            parseNumberedJointName(jointName);
        if (!numbered) continue;
        auto [bucket, inserted] =
            result->m_numberedJointIndicesByLowerPrefixHash.emplace(
                lowerAsciiHash(numbered->prefix),
                container::Vector<Skeleton::NumberedJointSequence>{});
        static_cast<void>(inserted);
        auto sequence = std::find_if(
            bucket->second.begin(), bucket->second.end(),
            [&numbered](const Skeleton::NumberedJointSequence& candidate) {
                return equalAsciiInsensitive(
                    candidate.prefix, numbered->prefix);
            });
        if (sequence == bucket->second.end()) {
            bucket->second.push_back({
                .prefix = container::String(numbered->prefix),
                .indices = {},
            });
            sequence = std::prev(bucket->second.end());
        }
        if (sequence->indices.size() < numbered->ordinal) {
            sequence->indices.resize(numbered->ordinal, kMissingJoint);
        }
        size_t& slot = sequence->indices[numbered->ordinal - 1u];
        if (slot == kMissingJoint) slot = jointIndex;
    }
    for (auto& [hash, bucket] :
         result->m_numberedJointIndicesByLowerPrefixHash) {
        static_cast<void>(hash);
        for (Skeleton::NumberedJointSequence& sequence : bucket) {
            const auto missing = std::find(
                sequence.indices.begin(), sequence.indices.end(),
                kMissingJoint);
            sequence.indices.erase(missing, sequence.indices.end());
        }
    }

    // Invalid parents and cycles were normalized above. Compile a stable
    // parent-first order once so every pose composes the hierarchy in one
    // linear pass instead of repeatedly scanning unresolved joints.
    container::Vector<container::Vector<size_t>> children(jointCount);
    result->m_evaluationOrder.reserve(jointCount);
    for (size_t jointIndex = 0; jointIndex < jointCount; ++jointIndex) {
        const int32_t parent = sanitizedParents[jointIndex];
        if (parent < 0) {
            result->m_evaluationOrder.push_back(jointIndex);
        } else {
            children[static_cast<size_t>(parent)].push_back(jointIndex);
        }
    }
    for (size_t cursor = 0; cursor < result->m_evaluationOrder.size(); ++cursor) {
        const size_t parent = result->m_evaluationOrder[cursor];
        result->m_evaluationOrder.insert(
            result->m_evaluationOrder.end(),
            children[parent].begin(), children[parent].end());
    }
    result->m_modelRestPose.resize(jointCount);
    for (const size_t jointIndex : result->m_evaluationOrder) {
        const SkeletonJoint& joint = result->m_joints[jointIndex];
        result->m_modelRestPose[jointIndex] = joint.parentIndex < 0
            ? joint.localRestTransform
            : multiply(
                  joint.localRestTransform,
                  result->m_modelRestPose[
                      static_cast<size_t>(joint.parentIndex)]);
    }
    if (diagnostics) *diagnostics = localDiagnostics;
    return result;
}

std::optional<size_t> Skeleton::findJointIndexInsensitive(container::StringView name) const noexcept {
    if (name.empty()) return std::nullopt;
    const auto found = m_jointIndicesByLowerNameHash.find(lowerAsciiHash(name));
    if (found == m_jointIndicesByLowerNameHash.end()) return std::nullopt;
    for (const size_t index : found->second) {
        if (index < m_joints.size() &&
            equalAsciiInsensitive(m_joints[index].name, name)) {
            return index;
        }
    }
    return std::nullopt;
}

container::Span<const size_t> Skeleton::numberedJointIndicesInsensitive(
    container::StringView prefix) const noexcept {
    if (prefix.empty()) return {};
    const auto found = m_numberedJointIndicesByLowerPrefixHash.find(
        lowerAsciiHash(prefix));
    if (found == m_numberedJointIndicesByLowerPrefixHash.end()) return {};
    for (const NumberedJointSequence& sequence : found->second) {
        if (equalAsciiInsensitive(sequence.prefix, prefix)) {
            return sequence.indices;
        }
    }
    return {};
}

container::SharedPtr<const AnimationClip> AnimationClip::fromW3d(const data::w3d::ParsedAnimation& animation) {
    auto result = std::make_shared<AnimationClip>();
    result->m_generation = g_nextAnimationGeneration.fetch_add(
        1u, std::memory_order_relaxed);
    if (result->m_generation == 0u) {
        result->m_generation = g_nextAnimationGeneration.fetch_add(
            1u, std::memory_order_relaxed);
    }
    result->m_hierarchyName = readName(
        animation.hierarchyName, data::w3d::NAME_LEN);
    result->m_frameCount = animation.numFrames;
    result->m_frameRate = animation.frameRate;
    result->m_channels = animation.channels;
    result->m_visibilityChannels = animation.visibilityChannels;
    return result;
}

uint64_t AnimationClip::estimatedByteSize() const noexcept {
    uint64_t bytes = sizeof(AnimationClip) + m_hierarchyName.size();
    bytes += static_cast<uint64_t>(m_channels.size()) *
        sizeof(data::w3d::ParsedAnimationChannel);
    for (const auto& channel : m_channels) {
        bytes += static_cast<uint64_t>(channel.values.size()) * sizeof(float);
    }
    bytes += static_cast<uint64_t>(m_visibilityChannels.size()) *
        sizeof(data::w3d::ParsedAnimationVisibilityChannel);
    for (const auto& channel : m_visibilityChannels) {
        bytes += channel.bits.size();
    }
    return bytes;
}

bool AnimationClip::isComplete(
    float animationTimeSeconds,
    RenderAnimationMode animationMode,
    float animationRate) const noexcept {
    if (animationMode != RenderAnimationMode::Once &&
        animationMode != RenderAnimationMode::OnceBackwards) {
        return false;
    }
    if (m_frameCount == 0 || m_frameRate == 0) return true;
    const float endFrame = static_cast<float>(m_frameCount - 1u);
    const float sampled = std::max(0.0f, animationTimeSeconds) *
        std::max(0.0f, animationRate) * static_cast<float>(m_frameRate);
    return sampled >= endFrame;
}

float AnimationClip::completionTimeSeconds(float animationRate) const noexcept {
    if (m_frameCount <= 1 || m_frameRate == 0) return 0.0f;
    const float samplesPerSecond = std::max(0.0f, animationRate) *
        static_cast<float>(m_frameRate);
    if (samplesPerSecond <= 0.0f) return std::numeric_limits<float>::infinity();
    return static_cast<float>(m_frameCount - 1u) / samplesPerSecond;
}

container::Vector<RenderMatrix> evaluateSkeletonRestPose(const Skeleton& skeleton,
                                                   const RenderTransform& entityTransform) {
    container::Vector<RenderMatrix> result(
        skeleton.modelRestPose().begin(), skeleton.modelRestPose().end());
    const RenderMatrix entity = makeEntityTransform(entityTransform);
    for (RenderMatrix& bone : result) bone = multiply(bone, entity);
    return result;
}

void evaluateSkeletonPoseInto(
    const Skeleton& skeleton, const AnimationClip* animation,
    float animationTimeSeconds, const RenderTransform& entityTransform,
    RenderAnimationMode animationMode, float animationRate,
    container::Span<const RenderBoneControl> boneControls,
    container::Vector<RenderMatrix>& poseOutput,
    container::Vector<uint8_t>* visibilityOutput,
    SkeletonEvaluationScratch& scratch) {
    poseOutput.resize(skeleton.joints().size());
    if (visibilityOutput) {
        visibilityOutput->resize(skeleton.joints().size());
    }
    static_cast<void>(evaluateSkeletonPoseIntoSpan(
        skeleton, animation, animationTimeSeconds, entityTransform,
        animationMode, animationRate, boneControls, poseOutput,
        visibilityOutput ? container::Span<uint8_t>(*visibilityOutput)
                         : container::Span<uint8_t>{},
        scratch));
}

bool evaluateSkeletonPoseIntoSpan(
    const Skeleton& skeleton, const AnimationClip* animation,
    float animationTimeSeconds, const RenderTransform& entityTransform,
    RenderAnimationMode animationMode, float animationRate,
    container::Span<const RenderBoneControl> boneControls,
    container::Span<RenderMatrix> poseOutput,
    container::Span<uint8_t> visibilityOutput,
    SkeletonEvaluationScratch& scratch) {
    const auto& joints = skeleton.joints();
    if (poseOutput.size() < joints.size() ||
        (!visibilityOutput.empty() &&
         visibilityOutput.size() < joints.size())) {
        return false;
    }
    poseOutput = poseOutput.first(joints.size());
    if (!visibilityOutput.empty()) {
        visibilityOutput = visibilityOutput.first(joints.size());
    }
    scratch.translations.assign(joints.size(), RenderVector{});
    scratch.rotations.assign(
        joints.size(), RenderQuaternion::identity());
    auto& translations = scratch.translations;
    auto& rotations = scratch.rotations;
    const float frame = animationFrame(
        animation, animationTimeSeconds, animationMode, animationRate);

    if (animation && animation->frameCount() > 0 && animation->frameRate() > 0) {
        for (const auto& channel : animation->channels()) {
            if (channel.pivotIndex >= joints.size() || channel.values.empty()) continue;
            // Legacy HTree animation starts at pivot one. Pivot zero is the
            // identity/object-transform root, not an animatable joint.
            if (channel.pivotIndex == 0) continue;
            const size_t first = channel.firstFrame;
            const size_t last = channel.lastFrame;
            const size_t vectorLength = channel.vectorLength;
            if (last < first || vectorLength == 0) continue;
            const size_t sampleCount = last - first + 1u;
            if (sampleCount > channel.values.size() / vectorLength) continue;
            const size_t frame0 = static_cast<size_t>(std::floor(frame));
            const size_t frame1 = std::min<size_t>(
                frame0 + 1u, animation->frameCount() - 1u);
            const float fraction = frame - static_cast<float>(frame0);
            const auto sample = [&](size_t sampledFrame) -> const float* {
                if (sampledFrame < first || sampledFrame > last) return nullptr;
                return channel.values.data() +
                    (sampledFrame - first) * vectorLength;
            };
            const float* value0 = sample(frame0);
            const float* value1 = sample(frame1);
            // Raw WW3D motion channels are sparse deltas. Each interpolation
            // endpoint is sampled independently: a missing translation sample
            // is zero and a missing quaternion sample is identity. In
            // particular, do not clamp frame1 to lastFrame; that would hold a
            // channel's final key for one extra fractional frame.
            const auto scalar = [&] {
                const float scalar0 = value0 ? value0[0] : 0.0f;
                const float scalar1 = value1 ? value1[0] : 0.0f;
                return scalar0 + (scalar1 - scalar0) * fraction;
            };
            auto& translation = translations[channel.pivotIndex];
            switch (channel.flags) {
            case data::w3d::AnimChannel_X:
                translation = {scalar(), translation.y(), translation.z()};
                break;
            case data::w3d::AnimChannel_Y:
                translation = {translation.x(), scalar(), translation.z()};
                break;
            case data::w3d::AnimChannel_Z:
                translation = {translation.x(), translation.y(), scalar()};
                break;
            case data::w3d::AnimChannel_Q:
                if (vectorLength == 4) {
                    const RenderQuaternion rotation0 = value0
                        ? RenderQuaternion{value0[0], value0[1], value0[2], value0[3]}
                        : RenderQuaternion::identity();
                    const RenderQuaternion rotation1 = value1
                        ? RenderQuaternion{value1[0], value1[1], value1[2], value1[3]}
                        : RenderQuaternion::identity();
                    rotations[channel.pivotIndex] = RenderQuaternion::slerp(
                        rotation0, rotation1, fraction);
                }
                break;
            default: break;
            }
        }
    }

    auto& controlJoints = scratch.controlJoints;
    controlJoints.clear();
    controlJoints.reserve(boneControls.size());
    for (const RenderBoneControl& control : boneControls) {
        controlJoints.push_back(resolveBoneControlJoint(skeleton, control));
    }

    auto& localPose = scratch.localPose;
    localPose.resize(joints.size());
    for (size_t i = 0; i < joints.size(); ++i) {
        const auto delta = RenderMatrix::from_trs(RenderVector::one(), rotations[i].normalized(), translations[i]);
        RenderMatrix controlDelta = RenderMatrix::identity();
        for (size_t controlIndex = 0;
             controlIndex < boneControls.size(); ++controlIndex) {
            if (!controlJoints[controlIndex] ||
                *controlJoints[controlIndex] != i) continue;
            const RenderBoneControl& control = boneControls[controlIndex];
            const RenderMatrix authored = RenderMatrix::from_trs(
                RenderVector::one(), control.rotation.normalized(),
                control.translation);
            controlDelta = multiply(controlDelta, authored);
        }
        // WW3D source matrices compose for column vectors. wwmath/HLSL use
        // row vectors, so the equivalent local transform reverses the source
        // multiplication order: animation delta -> gameplay control -> rest.
        localPose[i] = multiply(
            multiply(delta, controlDelta), joints[i].localRestTransform);
    }

    const RenderMatrix entityMatrix = makeEntityTransform(entityTransform);
    for (const size_t jointIndex : skeleton.evaluationOrder()) {
        const int32_t parent = joints[jointIndex].parentIndex;
        poseOutput[jointIndex] = parent < 0
            ? multiply(localPose[jointIndex], entityMatrix)
            : multiply(localPose[jointIndex],
                       poseOutput[static_cast<size_t>(parent)]);
    }

    if (!visibilityOutput.empty()) {
        std::fill(visibilityOutput.begin(), visibilityOutput.end(), 1u);
        if (animation) {
            const uint32_t visibilityFrame = static_cast<uint32_t>(
                std::floor(frame));
            for (const auto& channel : animation->visibilityChannels()) {
                if (channel.pivotIndex >= visibilityOutput.size()) continue;
                if (channel.pivotIndex == 0) continue;
                bool value = channel.defaultVisible;
                if (visibilityFrame >= channel.firstFrame &&
                    visibilityFrame <= channel.lastFrame) {
                    const size_t bit = visibilityFrame - channel.firstFrame;
                    const size_t byte = bit / 8u;
                    if (byte < channel.bits.size()) {
                        value = (channel.bits[byte] &
                                 (1u << (bit % 8u))) != 0;
                    }
                }
                visibilityOutput[channel.pivotIndex] = value ? 1u : 0u;
            }
        }
    }
    return true;
}

container::Vector<RenderMatrix> evaluateSkeletonPose(
    const Skeleton& skeleton, const AnimationClip* animation,
    float animationTimeSeconds, const RenderTransform& entityTransform,
    RenderAnimationMode animationMode, float animationRate,
    container::Span<const RenderBoneControl> boneControls) {
    container::Vector<RenderMatrix> pose;
    SkeletonEvaluationScratch scratch;
    evaluateSkeletonPoseInto(
        skeleton, animation, animationTimeSeconds, entityTransform,
        animationMode, animationRate, boneControls, pose, nullptr, scratch);
    return pose;
}

container::Vector<uint8_t> evaluateSkeletonVisibility(const Skeleton& skeleton, const AnimationClip* animation,
                                                float animationTimeSeconds,
                                                RenderAnimationMode animationMode,
                                                float animationRate) {
    container::Vector<uint8_t> visible(skeleton.joints().size(), 1);
    if (!animation || animation->visibilityChannels().empty()) return visible;

    const uint32_t frame = static_cast<uint32_t>(std::floor(
        animationFrame(animation, animationTimeSeconds, animationMode, animationRate)));
    for (const auto& channel : animation->visibilityChannels()) {
        if (channel.pivotIndex >= visible.size()) continue;
        if (channel.pivotIndex == 0) continue;
        bool value = channel.defaultVisible;
        if (frame >= channel.firstFrame && frame <= channel.lastFrame) {
            const size_t bit = frame - channel.firstFrame;
            const size_t byte = bit / 8;
            if (byte < channel.bits.size()) value = (channel.bits[byte] & (1u << (bit % 8))) != 0;
        }
        visible[channel.pivotIndex] = value ? 1 : 0;
    }
    return visible;
}

} // namespace engine::render
