#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

#include "engine/renderer/world/pipeline/WorldRenderPipelineMath.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace engine::render {
namespace {

using world_pipeline_detail::finiteVector;
using world_pipeline_detail::floatSwayOrientation;
using world_pipeline_detail::makeEntityTransform;

[[nodiscard]] uint64_t expandedParticleEmitterIdentity(
    uint64_t base, size_t jointIndex, uint32_t emittedOrdinal) noexcept {
    base ^= (static_cast<uint64_t>(jointIndex) +
             0xD6E8FEB86659FD93ull) +
        (base << 6u) + (base >> 2u);
    base ^= (static_cast<uint64_t>(emittedOrdinal) +
             0xA5A3564E27F8862Full) +
        (base << 6u) + (base >> 2u);
    return base != 0u ? base : 1u;
}

} // namespace

void WorldRenderPipeline::prepareCameraSlave() {
    const CameraSlaveRenderState& request = m_snapshot.cameraSlave;
    if (!request.enabled || request.objectId == 0 || request.boneName.empty()) return;

    const auto entityIt = std::find_if(
        m_snapshot.entities.begin(), m_snapshot.entities.end(),
        [&request](const RenderEntitySnapshot& entity) {
            return entity.id == request.objectId ||
                entity.objectId == request.objectId;
        });
    if (entityIt == m_snapshot.entities.end()) return;

    // This is deliberately set before model/skeleton lookup. A live Object
    // with a temporarily unavailable asset does not behave like a destroyed
    // Object: keep the request armed and fall back to the ordinary camera
    // until the renderer can resolve its real bone.
    m_preparedCameraSlaveTargetPresent = true;
    const auto reportFallback = [this]() noexcept {
        m_cameraPoseFallbackCount.fetch_add(
            1u, std::memory_order_relaxed);
    };
    const size_t entityIndex = static_cast<size_t>(
        std::distance(m_snapshot.entities.begin(), entityIt));
    const ResolvedAnimationPresentation& presentation =
        m_resolvedPresentationsByInput[entityIndex];
    if (presentation.modelAsset.empty()) {
        reportFallback();
        return;
    }

    const auto modelIt = m_models.find(presentation.modelAsset);
    if (modelIt == m_models.end() || !modelIt->second.skeleton ||
        modelIt->second.skeleton->empty()) {
        reportFallback();
        return;
    }

    const std::optional<size_t> boneIndex =
        modelIt->second.skeleton->findJointIndexInsensitive(request.boneName);
    // W3DModelDraw::clientOnly_getRenderObjBoneTransform treats `idx == 0`
    // as failure even when the HTree root happens to carry a name. Preserve
    // that legacy contract explicitly; never substitute entity/root transform
    // for a malformed camera-bone request.
    if (!boneIndex || *boneIndex == 0) {
        reportFallback();
        return;
    }

    if (entityIndex < m_preparedByInput.size()) {
        const PreparedRenderInstance& prepared =
            m_preparedByInput[entityIndex];
        const container::Span<RenderMatrix> pose =
            preparationPose(prepared);
        if (prepared.id == entityIt->id &&
            prepared.modelAsset == presentation.modelAsset &&
            prepared.poseReady && *boneIndex < pose.size()) {
            m_preparedCameraSlaveBoneWorldTransform =
                pose[*boneIndex];
            m_poseReuseCount.fetch_add(1u, std::memory_order_relaxed);
            m_cameraPoseReuseCount.fetch_add(1u, std::memory_order_relaxed);
            return;
        }
    }

    const RenderEntityId objectId = entityIt->objectId != 0
        ? entityIt->objectId : entityIt->id;
    m_cameraVisualScratch = presentation.visual;
    const CompiledPoseSample sample = compilePoseSample(
        m_cameraVisualScratch, modelIt->second, objectId,
        entityIt->channelIndex);
    PreparedRenderInstance& poseSlot = m_preparedByInput[entityIndex];
    const size_t jointCount = modelIt->second.skeleton->joints().size();
    if (poseSlot.poseCount < jointCount) {
        reportFallback();
        return;
    }
    poseSlot.poseCount = jointCount;
    container::Span<RenderMatrix> pose = preparationPose(poseSlot);
    poseSlot.poseReady = evaluatePose(
        RenderPoseInput{
            .consumer = PoseConsumer::Camera,
            .skeleton = modelIt->second.skeleton,
            .animation = sample.animation,
            .animationTimeSeconds = sample.timeSeconds,
            .entityTransform = entityIt->transform,
            .animationMode = sample.mode,
            .animationRate = sample.rate,
            .boneControls = m_cameraVisualScratch.boneControls,
        },
        pose);
    if (!poseSlot.poseReady || *boneIndex >= pose.size()) {
        reportFallback();
        return;
    }
    m_preparedCameraSlaveBoneWorldTransform = pose[*boneIndex];
}

void WorldRenderPipeline::prepareTrackMarks() {
    for (TrackMarkRenderInput& input : m_preparedTrackMarks) {
        if (input.objectId == 0 || input.leftWidthBone.empty() ||
            input.rightWidthBone.empty() ||
            !std::isfinite(input.additionalTreadWidth)) {
            continue;
        }

        const auto entityIt = std::find_if(
            m_snapshot.entities.begin(), m_snapshot.entities.end(),
            [&input](const RenderEntitySnapshot& entity) {
                return entity.id == input.objectId ||
                    entity.objectId == input.objectId;
            });
        if (entityIt == m_snapshot.entities.end()) continue;

        const size_t entityIndex = static_cast<size_t>(
            std::distance(m_snapshot.entities.begin(), entityIt));
        const ResolvedAnimationPresentation& presentation =
            m_resolvedPresentationsByInput[entityIndex];
        const auto modelIt = m_models.find(presentation.modelAsset);
        if (modelIt == m_models.end() || !modelIt->second.skeleton ||
            modelIt->second.skeleton->empty()) {
            continue;
        }

        const Skeleton& skeleton = *modelIt->second.skeleton;
        const std::optional<size_t> leftIndex =
            skeleton.findJointIndexInsensitive(input.leftWidthBone);
        const std::optional<size_t> rightIndex =
            skeleton.findJointIndexInsensitive(input.rightWidthBone);
        // RefCode treats Get_Bone_Index()==0 as lookup failure. Preserve that
        // root-index sentinel along with the missing-bone fallback contract.
        if (!leftIndex || !rightIndex || *leftIndex == 0 || *rightIndex == 0) {
            continue;
        }

        // Track spacing is a model property. Reuse the skeleton's immutable
        // bind/rest palette and apply only this entity's sealed transform;
        // no animation sample or transient full palette is required.
        const container::Span<const RenderMatrix> restPose =
            skeleton.modelRestPose();
        if (*leftIndex >= restPose.size() || *rightIndex >= restPose.size()) {
            continue;
        }
        const RenderMatrix entityWorld = makeEntityTransform(
            entityIt->transform);
        const RenderVector delta =
            (restPose[*rightIndex] * entityWorld).translation() -
            (restPose[*leftIndex] * entityWorld).translation();
        const float width = delta.length() + input.additionalTreadWidth;
        if (std::isfinite(width) && width > 0.0f) input.trackWidth = width;
    }
}

void WorldRenderPipeline::prepareParticleEmitterInstances() {
    const size_t anchorCapacityBefore =
        m_preparedParticleEmitterBoneWorldTransformArena.capacity();
    m_preparedParticleEmitterBoneWorldTransformArena.clear();
    const size_t capacityBefore =
        m_preparedParticleEmitterInstances.capacity();
    m_preparedParticleEmitterInstances.reserve(m_snapshot.entities.size());
    m_bodyParticleSelectionCounts.clear();
    m_liveFrozenParticleEmitterAnchors.clear();
    m_staleFrozenParticleEmitterAnchors.clear();
    if (m_preparedParticleEmitterInstances.capacity() > capacityBefore &&
        m_containerCapacityGrowths !=
            std::numeric_limits<uint32_t>::max()) {
        // This task is the sole writer after beginPreparation() and the owner
        // thread reads the value only after the Taskflow join.
        ++m_containerCapacityGrowths;
    }
    size_t preparedEmitterCount = 0;
    for (size_t inputIndex = 0; inputIndex < m_snapshot.entities.size();
         ++inputIndex) {
        const RenderEntitySnapshot& instance = m_snapshot.entities[inputIndex];
        if (instance.id == 0 || instance.visual.hidden ||
            !finiteVector(instance.transform.position)) {
            continue;
        }
        const ResolvedAnimationPresentation& presentation =
            m_resolvedPresentationsByInput[inputIndex];
        if (presentation.visual.particleSystemBones.empty()) continue;

        if (preparedEmitterCount ==
            m_preparedParticleEmitterInstances.size()) {
            m_preparedParticleEmitterInstances.emplace_back();
        }
        PreparedRenderInstance& prepared =
            m_preparedParticleEmitterInstances[preparedEmitterCount++];
        prepared.id = instance.id;
        prepared.objectId = instance.objectId != 0
            ? instance.objectId : instance.id;
        prepared.channelIndex = instance.channelIndex;
        prepared.modelAsset = presentation.modelAsset;
        prepared.visual = presentation.visual;
        PreparedRenderInstance& poseSlot = m_preparedByInput[inputIndex];
        prepared.worldTransform = poseSlot.id != 0u
            ? poseSlot.worldTransform
            : makeEntityTransform(instance.transform);
        prepared.boundingRadius = instance.boundingRadius;
        prepared.cullingCenterOffset = instance.cullingCenterOffset;
        prepared.directionalLightScale = instance.directionalLightScale;
        prepared.shadow = instance.shadow;
        prepared.skeleton.reset();
        prepared.skeletonGeneration = 0;
        prepared.poseReady = false;
        prepared.visibilityReady = false;
        prepared.weaponLaunchBoneWorldTransforms = {};
        prepared.poseOffset = poseSlot.poseOffset;
        prepared.poseCount = poseSlot.poseCount;

        const auto modelIt = m_models.find(prepared.modelAsset);
        const container::SharedPtr<const Skeleton> skeleton =
            modelIt != m_models.end() ? modelIt->second.skeleton : nullptr;
        auto& emitters = prepared.visual.particleSystemBones;
        const size_t compactEmitterCount = emitters.size();
        bool hasNumberedPrefixes = false;
        size_t expansionUpperBound = 0;
        for (size_t emitterIndex = 0;
             emitterIndex < compactEmitterCount; ++emitterIndex) {
            const RenderParticleSystemBone& emitter = emitters[emitterIndex];
            if (!emitter.numberedBonePrefix) continue;
            hasNumberedPrefixes = true;
            expansionUpperBound += std::min<uint32_t>(
                emitter.maximumEmitters, 16u);
        }
        if (hasNumberedPrefixes) {
            emitters.reserve(compactEmitterCount + expansionUpperBound);
            for (size_t emitterIndex = 0;
                 emitterIndex < compactEmitterCount; ++emitterIndex) {
                const RenderParticleSystemBone descriptor =
                    emitters[emitterIndex];
                if (!descriptor.numberedBonePrefix || !skeleton ||
                    descriptor.boneName.empty() ||
                    descriptor.particleSystem.empty() ||
                    descriptor.maximumEmitters == 0u) {
                    continue;
                }
                container::Span<const size_t> joints =
                    skeleton->numberedJointIndicesInsensitive(
                        descriptor.boneName);
                if (joints.size() > 16u) joints = joints.first(16u);
                if (joints.empty()) continue;

                uint32_t alreadySelected = 0u;
                if (descriptor.selectionGroup != 0u) {
                    alreadySelected =
                        m_bodyParticleSelectionCounts[
                            descriptor.selectionGroup];
                }
                const uint32_t maximum = std::min<uint32_t>(
                    descriptor.maximumEmitters, 16u);
                const uint32_t remaining = alreadySelected < maximum
                    ? maximum - alreadySelected : 0u;
                const uint32_t selected = std::min<uint32_t>(
                    remaining, static_cast<uint32_t>(joints.size()));
                for (uint32_t ordinal = 0; ordinal < selected; ++ordinal) {
                    const size_t jointIndex = joints[ordinal];
                    if (jointIndex >= skeleton->joints().size()) continue;
                    RenderParticleSystemBone expanded = descriptor;
                    expanded.identity = expandedParticleEmitterIdentity(
                        descriptor.identity, jointIndex, ordinal);
                    expanded.boneName = skeleton->joints()[jointIndex].name;
                    expanded.numberedBonePrefix = false;
                    expanded.maximumEmitters = 1u;
                    expanded.selectionGroup = 0u;
                    emitters.push_back(std::move(expanded));
                }
                if (descriptor.selectionGroup != 0u) {
                    m_bodyParticleSelectionCounts[
                        descriptor.selectionGroup] = alreadySelected + selected;
                }
            }
            emitters.erase(std::remove_if(
                emitters.begin(), emitters.end(),
                [](const RenderParticleSystemBone& emitter) {
                    return emitter.numberedBonePrefix;
                }), emitters.end());
        }
        bool needsFrozenEntryPose = false;
        const uint64_t skeletonGeneration = skeleton
            ? skeleton->generation() : 0u;
        for (const RenderParticleSystemBone& emitter : emitters) {
            if (emitter.followsAnimatedBone || emitter.identity == 0u ||
                emitter.boneName.empty()) {
                continue;
            }
            m_liveFrozenParticleEmitterAnchors.insert(emitter.identity);
            const auto frozen =
                m_frozenParticleEmitterAnchors.find(emitter.identity);
            if (frozen == m_frozenParticleEmitterAnchors.end() ||
                frozen->second.skeletonGeneration != skeletonGeneration ||
                frozen->second.modelAsset != prepared.modelAsset ||
                frozen->second.boneName != emitter.boneName) {
                needsFrozenEntryPose = true;
            }
        }
        prepared.particleEmitterBoneOffset =
            m_preparedParticleEmitterBoneWorldTransformArena.size();
        prepared.particleEmitterBoneCount =
            prepared.visual.particleSystemBones.size();
        m_preparedParticleEmitterBoneWorldTransformArena.resize(
            prepared.particleEmitterBoneOffset +
                prepared.particleEmitterBoneCount);
        container::Span<std::optional<RenderMatrix>> emitterTransforms =
            preparationParticleEmitterBoneWorldTransforms(prepared);

        const bool needsAnimatedPose = std::any_of(
            prepared.visual.particleSystemBones.begin(),
            prepared.visual.particleSystemBones.end(),
            [](const RenderParticleSystemBone& emitter) {
                return emitter.followsAnimatedBone &&
                    !emitter.boneName.empty();
            });
        const bool needsRestPose = std::any_of(
            prepared.visual.particleSystemBones.begin(),
            prepared.visual.particleSystemBones.end(),
            [](const RenderParticleSystemBone& emitter) {
                return !emitter.followsAnimatedBone &&
                    !emitter.boneName.empty();
            });
        const bool needsSampledPose =
            needsAnimatedPose || needsFrozenEntryPose;
        if ((needsSampledPose || needsRestPose) &&
            modelIt != m_models.end() &&
            modelIt->second.skeleton && !modelIt->second.skeleton->empty()) {
            prepared.skeleton = modelIt->second.skeleton;
            prepared.skeletonGeneration = prepared.skeleton->generation();
            if (needsSampledPose) {
                const CompiledPoseSample sample = compilePoseSample(
                    prepared.visual, modelIt->second, prepared.objectId,
                    prepared.channelIndex);
                RenderTransform presentedTransform = instance.transform;
                if (presentation.visual.floatSwayEnabled) {
                    presentedTransform.orientation = floatSwayOrientation(
                        presentation.visual.floatSwayBaseYawRadians,
                        presentation.visual.floatSwaySampleTick);
                }
                const size_t jointCount =
                    modelIt->second.skeleton->joints().size();
                if (poseSlot.poseCount >= jointCount) {
                    poseSlot.poseCount = jointCount;
                    prepared.poseCount = jointCount;
                }
                container::Span<RenderMatrix> currentPose =
                    preparationPose(poseSlot);
                if (poseSlot.poseReady) {
                    m_poseReuseCount.fetch_add(
                        1u, std::memory_order_relaxed);
                    m_emitterPoseReuseCount.fetch_add(
                        1u, std::memory_order_relaxed);
                } else if (currentPose.size() >= jointCount) {
                    poseSlot.poseReady = evaluatePose(
                        RenderPoseInput{
                            .consumer = PoseConsumer::ParticleEmitter,
                            .skeleton = modelIt->second.skeleton,
                            .animation = sample.animation,
                            .animationTimeSeconds = sample.timeSeconds,
                            // Use the same presentation root as the ordinary
                            // model pose. Frozen state-entry anchors are
                            // converted back to local space with
                            // prepared.worldTransform below; mixing the raw
                            // simulation root with a swayed presentation root
                            // corrupts every later ParticleSysBone position.
                            .entityTransform = presentedTransform,
                            .animationMode = sample.mode,
                            .animationRate = sample.rate,
                            .boneControls = prepared.visual.boneControls,
                        },
                        currentPose);
                }
                prepared.poseReady = poseSlot.poseReady;
            }

            const container::Span<const RenderMatrix> restPose =
                needsRestPose
                ? modelIt->second.skeleton->modelRestPose()
                : container::Span<const RenderMatrix>{};
            const RenderMatrix entityWorld = prepared.worldTransform;
            const container::Span<RenderMatrix> currentPose =
                preparationPose(poseSlot);
            for (size_t emitterIndex = 0;
                 emitterIndex < prepared.visual.particleSystemBones.size();
                 ++emitterIndex) {
                const RenderParticleSystemBone& emitter =
                    prepared.visual.particleSystemBones[emitterIndex];
                if (emitter.boneName.empty()) continue;
                const std::optional<size_t> bone =
                    modelIt->second.skeleton->findJointIndexInsensitive(
                        emitter.boneName);
                if (!bone || *bone == 0u) continue;
                if (emitter.followsAnimatedBone &&
                    *bone < currentPose.size()) {
                    emitterTransforms[emitterIndex] =
                        currentPose[*bone];
                } else if (!emitter.followsAnimatedBone) {
                    auto frozen = m_frozenParticleEmitterAnchors.find(
                        emitter.identity);
                    const bool frozenCompatible =
                        frozen != m_frozenParticleEmitterAnchors.end() &&
                        frozen->second.skeletonGeneration ==
                            prepared.skeletonGeneration &&
                        frozen->second.modelAsset == prepared.modelAsset &&
                        frozen->second.boneName == emitter.boneName;
                    if (frozenCompatible) {
                        emitterTransforms[emitterIndex] =
                            frozen->second.localTransform * entityWorld;
                    } else if (poseSlot.poseReady &&
                               *bone < currentPose.size()) {
                        FrozenParticleEmitterAnchor& accepted =
                            m_frozenParticleEmitterAnchors[emitter.identity];
                        accepted.localTransform =
                            currentPose[*bone] * entityWorld.inverse();
                        accepted.modelAsset = prepared.modelAsset;
                        accepted.boneName = emitter.boneName;
                        accepted.skeletonGeneration =
                            prepared.skeletonGeneration;
                        emitterTransforms[emitterIndex] = currentPose[*bone];
                    } else if (*bone < restPose.size()) {
                        // Resource/arena pressure may make the state-entry pose
                        // temporarily unavailable. Render at the rest anchor
                        // for this frame but do not cache it; a later prepared
                        // frame can still capture the authored entry pose.
                        emitterTransforms[emitterIndex] =
                            restPose[*bone] * entityWorld;
                    }
                }
            }
        }
        uint32_t rootFallbacks = 0;
        for (size_t emitterIndex = 0;
             emitterIndex < prepared.visual.particleSystemBones.size();
             ++emitterIndex) {
            if (!prepared.visual.particleSystemBones[emitterIndex]
                     .boneName.empty() &&
                !emitterTransforms[emitterIndex]) {
                ++rootFallbacks;
            }
        }
        m_emitterRootFallbackCount.fetch_add(
            rootFallbacks, std::memory_order_relaxed);
    }
    for (const auto& [identity, anchor] :
         m_frozenParticleEmitterAnchors) {
        static_cast<void>(anchor);
        if (!m_liveFrozenParticleEmitterAnchors.contains(identity)) {
            m_staleFrozenParticleEmitterAnchors.push_back(identity);
        }
    }
    for (const uint64_t identity : m_staleFrozenParticleEmitterAnchors) {
        m_frozenParticleEmitterAnchors.erase(identity);
    }
    m_preparedParticleEmitterInstances.resize(preparedEmitterCount);
    if (m_preparedParticleEmitterBoneWorldTransformArena.capacity() >
            anchorCapacityBefore &&
        m_containerCapacityGrowths !=
            std::numeric_limits<uint32_t>::max()) {
        ++m_containerCapacityGrowths;
    }
}

void WorldRenderPipeline::prepareProjectiles() {
    m_completedFrame.projectiles.reserve(m_snapshot.projectiles.size());
    size_t preparedProjectileCount = 0;
    for (const ProjectileRenderSnapshot& source : m_snapshot.projectiles) {
        if (source.objectId == 0) continue;
        if (preparedProjectileCount == m_completedFrame.projectiles.size()) {
            m_completedFrame.projectiles.emplace_back();
        }
        PreparedProjectileRenderSnapshot& prepared =
            m_completedFrame.projectiles[preparedProjectileCount++];
        prepared.projectile = source;
        // ProjectileStreamUpdate connects live projectile Objects, never the
        // weapon muzzle. Preserve that contract: the prepared model and every
        // ribbon point use the same authoritative fixed-point path projection.
        // WeaponFireFXBone remains the sole owner of muzzle-only visuals.
    }
    m_completedFrame.projectiles.resize(preparedProjectileCount);
}

} // namespace engine::render
