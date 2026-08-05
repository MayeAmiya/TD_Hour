#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

#include "engine/renderer/runtime/RenderParallelExecutor.h"

#include <algorithm>
#include <exception>
#include <future>
#include <limits>
#include <optional>

namespace engine::render {

// RefCode builds W3DModelDraw with CACHE_ATTACH_BONE enabled.  Consequently
// AttachToBoneInAnotherModule does not inherit the current bone rotation: it
// caches the named bone's pristine model-space translation, rotates/scales that
// offset by this Draw module's object transform, and preserves the Draw
// module's own basis.  Drawable::getPristineBonePositions walks every Draw
// module of the SAME drawable in module order. Shipped content uses this for
// the GLA Technical:
// module 0 is the W3DTruckDraw chassis publishing ExtraPublicBone Dum_Turret,
// module 1 is the skinned gunner with AttachToBoneInAnotherModule = Dum_Turret.
// Without this pass the gunner renders at the vehicle root even though the
// weapon side already fires from the attached pristine offset
// (ObjectCombatPresentation attachOffsetForChannel), so the visible rider and
// its own muzzle disagreed with each other.
void WorldRenderPipeline::resolveModuleAttachments()
{
    const size_t entityCount = std::min(
        m_snapshot.entities.size(), m_preparedByInput.size());
    m_moduleAttachmentChildScratch.clear();
    m_moduleAttachmentObjectScratch.clear();
    for (size_t index = 0; index < entityCount; ++index)
    {
        const RenderEntitySnapshot& source = m_snapshot.entities[index];
        if (source.attachToBoneInAnotherModule.empty() ||
            m_preparedByInput[index].id == 0)
        {
            continue;
        }
        const RenderEntityId objectId =
            source.objectId != 0 ? source.objectId : source.id;
        if (objectId == 0) continue;
        m_moduleAttachmentChildScratch.push_back(index);
        if (!source.attachToBoneInAnotherModuleOffset)
            m_moduleAttachmentObjectScratch.insert(objectId);
    }
    if (m_moduleAttachmentChildScratch.empty()) return;

    // Live game snapshots already carry the condition-state pristine offset.
    // Gather renderer siblings only for lightweight producers that do not own
    // the content catalog, such as placement previews.
    m_moduleAttachmentSiblingScratch.clear();
    if (!m_moduleAttachmentObjectScratch.empty()) {
        for (size_t index = 0; index < entityCount; ++index)
        {
            const RenderEntitySnapshot& source = m_snapshot.entities[index];
            if (m_preparedByInput[index].id == 0) continue;
            const RenderEntityId objectId =
                source.objectId != 0 ? source.objectId : source.id;
            if (objectId != 0 &&
                m_moduleAttachmentObjectScratch.contains(objectId))
                m_moduleAttachmentSiblingScratch.push_back(index);
        }
    }

    m_moduleAttachmentComposedScratch.clear();
    for (const size_t childIndex : m_moduleAttachmentChildScratch)
    {
        const RenderEntitySnapshot& child = m_snapshot.entities[childIndex];
        // RefCode resolves this through Drawable::getPristineBonePositions
        // for the logical condition state, not from whichever client model
        // happens to be prepared during a transition. Live snapshots freeze
        // that exact value. Renderer-owned skeleton lookup remains a fallback
        // for placement previews and other lightweight producers.
        std::optional<RenderVector> pristineOffset =
            child.attachToBoneInAnotherModuleOffset;
        const RenderEntityId objectId =
            child.objectId != 0 ? child.objectId : child.id;
        uint32_t resolvedChannel = std::numeric_limits<uint32_t>::max();
        if (!pristineOffset) {
            for (const size_t siblingIndex :
                 m_moduleAttachmentSiblingScratch) {
                const RenderEntitySnapshot& sibling =
                    m_snapshot.entities[siblingIndex];
                const RenderEntityId siblingObject = sibling.objectId != 0
                    ? sibling.objectId : sibling.id;
                if (siblingObject != objectId ||
                    siblingIndex == childIndex ||
                    sibling.channelIndex >= resolvedChannel) continue;
                const PreparedRenderInstance& parent =
                    m_preparedByInput[siblingIndex];
                if (!parent.skeleton ||
                    parent.skeletonGeneration !=
                        parent.skeleton->generation()) continue;
                const std::optional<size_t> bone =
                    parent.skeleton->findJointIndexInsensitive(
                        child.attachToBoneInAnotherModule);
                if (!bone || *bone == 0) continue;
                const container::Span<const RenderMatrix> restPose =
                    parent.skeleton->modelRestPose();
                if (*bone >= restPose.size()) continue;
                pristineOffset = restPose[*bone].translation();
                resolvedChannel = sibling.channelIndex;
            }
        }
        if (!pristineOffset) continue;
        PreparedRenderInstance& prepared = m_preparedByInput[childIndex];
        const RenderMatrix oldWorld = prepared.worldTransform;
        RenderMatrix attachment = oldWorld;
        attachment.set_translation(
            oldWorld.translation() +
            oldWorld.transform_dir(*pristineOffset));
        // The pose palette is `localBone * root`. Apply exactly the root
        // translation performed by legacy adjustTransformMtx; inheriting the
        // parent's complete animated bone matrix would rotate the rider a
        // second time and separate it from the chassis.
        const RenderMatrix delta =
            oldWorld.inverse() * attachment;
        prepared.worldTransform = attachment;
        for (RenderMatrix& bone : preparationPose(prepared)) bone *= delta;
        for (std::optional<RenderMatrix>& weaponBone :
             prepared.weaponLaunchBoneWorldTransforms)
        {
            if (weaponBone) *weaponBone *= delta;
        }
        m_moduleAttachmentComposedScratch.push_back({
            .id = prepared.id,
            .delta = delta,
        });
    }
    if (m_moduleAttachmentComposedScratch.empty()) return;

    // ParticleSysBone anchors were resolved from this channel's pre-attachment
    // pose during preparation, so they must travel with the same delta instead
    // of emitting from the unattached root.
    for (PreparedRenderInstance& emitter : m_preparedParticleEmitterInstances)
    {
        if (emitter.id == 0) continue;
        const auto composed = std::find_if(
            m_moduleAttachmentComposedScratch.begin(),
            m_moduleAttachmentComposedScratch.end(),
            [&emitter](const ModuleAttachmentComposition& value) noexcept {
                return value.id == emitter.id;
            });
        if (composed == m_moduleAttachmentComposedScratch.end()) continue;
        for (std::optional<RenderMatrix>& anchor :
             preparationParticleEmitterBoneWorldTransforms(emitter))
        {
            if (anchor) *anchor *= composed->delta;
        }
        // prepareParticleEmitterInstances clears this channel's launch-bone
        // cache, so the root is the only remaining transform to re-parent.
        emitter.worldTransform *= composed->delta;
    }
}

void WorldRenderPipeline::resolveContainerAttachments()
{
    const size_t entityCount = m_snapshot.entities.size();
    const bool hasContainerAttachments = std::any_of(
        m_snapshot.entities.begin(), m_snapshot.entities.end(),
        [](const RenderEntitySnapshot& source) noexcept {
            return source.containerObjectId != 0 &&
                !source.attachToBoneInContainer.empty();
        });
    // The emitter reconciliation below only applies to the same dependency
    // channels. Ordinary ParticleSysBone instances need no source graph.
    if (!hasContainerAttachments) return;
    const size_t noParent = entityCount;
    constexpr size_t invalidParent = std::numeric_limits<size_t>::max();
    constexpr size_t invalidDepth = std::numeric_limits<size_t>::max();

    const size_t attachmentCapacityBefore =
        m_attachmentResolutionScratch.capacity();
    m_attachmentResolutionScratch.clear();
    m_attachmentResolutionScratch.resize(entityCount,
                                         AttachmentResolution::Pending);
    if (m_attachmentResolutionScratch.capacity() >
            attachmentCapacityBefore &&
        m_containerCapacityGrowths !=
            std::numeric_limits<uint32_t>::max()) {
        ++m_containerCapacityGrowths;
    }
    auto& states = m_attachmentResolutionScratch;

    // Build both lookup tables before dependency work starts. The object table
    // is immutable while attachment chunks execute; the render-id table also
    // removes the ParticleSysBone post-pass's former linear source search.
    // Dense-map emplace retains the first input index. Malformed duplicate
    // identities therefore use one deterministic canonical parent instead of
    // re-entering the former recursive O(N^2) compatibility scan.
    m_attachmentInputIndexScratch.clear();
    m_attachmentInputIndexScratch.reserve(entityCount);
    m_attachmentRenderIndexScratch.clear();
    m_attachmentRenderIndexScratch.reserve(entityCount);
    for (size_t index = 0; index < entityCount; ++index)
    {
        const RenderEntitySnapshot& source = m_snapshot.entities[index];
        const RenderEntityId objectId =
            source.objectId != 0 ? source.objectId : source.id;
        if (objectId != 0)
        {
            static_cast<void>(
                m_attachmentInputIndexScratch.emplace(objectId, index));
        }
        if (source.id != 0)
            m_attachmentRenderIndexScratch.emplace(source.id, index);
    }

    // W3DDependencyModelDraw asks the container Drawable for a bone, and that
    // query walks every Draw module in authored order. Build the same immutable
    // object -> channel range before attachment workers start; a requested bone
    // must not fall back to the vehicle root merely because channel zero does
    // not own it.
    m_attachmentObjectChannelScratch.clear();
    m_attachmentObjectChannelScratch.reserve(entityCount);
    for (size_t index = 0; index < entityCount; ++index) {
        const RenderEntitySnapshot& source = m_snapshot.entities[index];
        const RenderEntityId objectId =
            source.objectId != 0 ? source.objectId : source.id;
        if (objectId != 0 && m_preparedByInput[index].id != 0)
            m_attachmentObjectChannelScratch.push_back(index);
    }
    std::sort(
        m_attachmentObjectChannelScratch.begin(),
        m_attachmentObjectChannelScratch.end(),
        [this](size_t lhs, size_t rhs) noexcept {
            const RenderEntitySnapshot& left = m_snapshot.entities[lhs];
            const RenderEntitySnapshot& right = m_snapshot.entities[rhs];
            const RenderEntityId leftObject =
                left.objectId != 0 ? left.objectId : left.id;
            const RenderEntityId rightObject =
                right.objectId != 0 ? right.objectId : right.id;
            if (leftObject != rightObject) return leftObject < rightObject;
            if (left.channelIndex != right.channelIndex)
                return left.channelIndex < right.channelIndex;
            return lhs < rhs;
        });
    m_attachmentObjectChannelRangesScratch.clear();
    m_attachmentObjectChannelRangesScratch.reserve(
        m_attachmentInputIndexScratch.size());
    for (size_t begin = 0;
         begin < m_attachmentObjectChannelScratch.size();) {
        const size_t firstIndex = m_attachmentObjectChannelScratch[begin];
        const RenderEntitySnapshot& first = m_snapshot.entities[firstIndex];
        const RenderEntityId objectId =
            first.objectId != 0 ? first.objectId : first.id;
        size_t end = begin + 1u;
        while (end < m_attachmentObjectChannelScratch.size()) {
            const RenderEntitySnapshot& next =
                m_snapshot.entities[m_attachmentObjectChannelScratch[end]];
            const RenderEntityId nextObject =
                next.objectId != 0 ? next.objectId : next.id;
            if (nextObject != objectId) break;
            ++end;
        }
        m_attachmentObjectChannelRangesScratch.emplace(
            objectId, std::pair<size_t, size_t>{begin, end});
        begin = end;
    }

    const auto attachmentTransform = [this](size_t parentIndex,
                                              container::StringView boneName) -> std::optional<RenderMatrix>
    {
        if (parentIndex >= m_preparedByInput.size())
            return std::nullopt;
        const PreparedRenderInstance& parent = m_preparedByInput[parentIndex];
        if (parent.id == 0)
            return std::nullopt;
        const RenderEntitySnapshot& parentSource =
            m_snapshot.entities[parentIndex];
        const RenderEntityId parentObjectId = parentSource.objectId != 0
            ? parentSource.objectId : parentSource.id;
        const auto range =
            m_attachmentObjectChannelRangesScratch.find(parentObjectId);
        if (range != m_attachmentObjectChannelRangesScratch.end()) {
            for (size_t ordinal = range->second.first;
                 ordinal < range->second.second; ++ordinal) {
                const size_t channelIndex =
                    m_attachmentObjectChannelScratch[ordinal];
                const PreparedRenderInstance& channel =
                    m_preparedByInput[channelIndex];
                if (!channel.poseReady || !channel.skeleton ||
                    channel.skeletonGeneration !=
                        channel.skeleton->generation()) {
                    continue;
                }
                const std::optional<size_t> bone =
                    channel.skeleton->findJointIndexInsensitive(boneName);
                const container::Span<RenderMatrix> pose =
                    preparationPose(channel);
                if (bone && *bone != 0u && *bone < pose.size())
                    return pose[*bone];
            }
        }
        // RefCode logs the bad bone and uses the container Drawable's root.
        return parent.worldTransform;
    };

    const auto clearDependent = [this](size_t childIndex)
    {
        m_preparedByInput[childIndex] = {};
        m_animationCompletionsByInput[childIndex].clear();
        m_completionFallbacksByInput[childIndex] = 0;
    };

    const auto composeAttachment =
        [this](size_t childIndex, size_t parentIndex,
               const RenderMatrix& attachment)
    {
        PreparedRenderInstance& child = m_preparedByInput[childIndex];
        const RenderMatrix oldWorld = child.worldTransform;
        const RenderMatrix delta = oldWorld.inverse() * attachment;
        child.worldTransform = attachment;
        for (RenderMatrix& bone : preparationPose(child))
            bone *= delta;
        for (std::optional<RenderMatrix>& weaponBone :
             child.weaponLaunchBoneWorldTransforms)
        {
            if (weaponBone)
                *weaponBone *= delta;
        }

        // Overlord Tank/Truck/Aircraft explicitly forward their current tint;
        // DependencyModel then imitates the container's stealth appearance.
        // Copy only those presentation envelopes, leaving the child model's
        // own animation, subobjects, owner colour, and lighting policy intact.
        const RenderVisualState& parentVisual =
            m_preparedByInput[parentIndex].visual;
        child.visual.scriptFlashTint = parentVisual.scriptFlashTint;
        child.visual.heatVisionIntensity = parentVisual.heatVisionIntensity;
        child.visual.heatVisionOnly = parentVisual.heatVisionOnly;
        child.visual.objectOpacity = parentVisual.objectOpacity;
    };

    {
        auto& parents = m_attachmentParentScratch;
        auto& depths = m_attachmentDepthScratch;
        parents.clear();
        parents.resize(entityCount, noParent);
        depths.clear();
        depths.resize(entityCount, invalidDepth);

        // The unique-id graph is functional (at most one parent per child),
        // so direct lookup plus iterative chain unwinding resolves parent depth
        // in linear time without recursion or mutable graph state in workers.
        for (size_t childIndex = 0; childIndex < entityCount; ++childIndex)
        {
            const PreparedRenderInstance& child =
                m_preparedByInput[childIndex];
            const RenderEntitySnapshot& source =
                m_snapshot.entities[childIndex];
            if (child.id == 0 || source.containerObjectId == 0 ||
                source.attachToBoneInContainer.empty())
            {
                continue;
            }
            const auto parent = m_attachmentInputIndexScratch.find(
                source.containerObjectId);
            if (parent == m_attachmentInputIndexScratch.end() ||
                parent->second == childIndex)
            {
                parents[childIndex] = invalidParent;
            }
            else
            {
                parents[childIndex] = parent->second;
            }
        }

        auto& path = m_attachmentPathScratch;
        path.reserve(entityCount);
        for (size_t start = 0; start < entityCount; ++start)
        {
            if (states[start] == AttachmentResolution::Resolved)
                continue;

            path.clear();
            size_t current = start;
            while (current < entityCount &&
                   states[current] == AttachmentResolution::Pending)
            {
                states[current] = AttachmentResolution::Resolving;
                path.push_back(current);
                const size_t parent = parents[current];
                current = parent;
                if (current >= entityCount)
                    break;
            }

            if (current < entityCount &&
                states[current] == AttachmentResolution::Resolving)
            {
                // Every node on this chain depends on the cycle and therefore
                // fails closed, matching the old recursive DFS behavior.
                for (const size_t index : path)
                {
                    clearDependent(index);
                    depths[index] = invalidDepth;
                    states[index] = AttachmentResolution::Resolved;
                }
                continue;
            }

            while (!path.empty())
            {
                const size_t index = path.back();
                path.pop_back();
                const size_t parent = parents[index];
                if (parent == noParent)
                {
                    depths[index] = 0;
                }
                else if (parent == invalidParent || parent >= entityCount ||
                         depths[parent] == invalidDepth ||
                         m_preparedByInput[parent].id == 0)
                {
                    clearDependent(index);
                    depths[index] = invalidDepth;
                }
                else
                {
                    depths[index] = depths[parent] + 1u;
                }
                states[index] = AttachmentResolution::Resolved;
            }
        }

        size_t maximumDepth = 0;
        size_t attachedCount = 0;
        for (size_t index = 0; index < entityCount; ++index)
        {
            if (parents[index] < entityCount &&
                depths[index] != invalidDepth)
            {
                maximumDepth = std::max(maximumDepth, depths[index]);
                ++attachedCount;
            }
        }

        auto& layerOffsets = m_attachmentLayerOffsetsScratch;
        layerOffsets.clear();
        layerOffsets.resize(maximumDepth + 2u, 0u);
        for (size_t index = 0; index < entityCount; ++index)
        {
            if (parents[index] < entityCount &&
                depths[index] != invalidDepth)
            {
                ++layerOffsets[depths[index] + 1u];
            }
        }
        for (size_t index = 1; index < layerOffsets.size(); ++index)
            layerOffsets[index] += layerOffsets[index - 1u];

        auto& layerCursors = m_attachmentLayerCursorsScratch;
        layerCursors = layerOffsets;
        auto& order = m_attachmentOrderScratch;
        order.clear();
        order.resize(attachedCount);
        for (size_t index = 0; index < entityCount; ++index)
        {
            if (parents[index] < entityCount &&
                depths[index] != invalidDepth)
            {
                order[layerCursors[depths[index]]++] = index;
            }
        }

        const auto composeRange =
            [this, &order, &parents, &attachmentTransform,
             &composeAttachment](size_t beginIndex, size_t endIndex)
        {
            for (size_t position = beginIndex; position < endIndex;
                 ++position)
            {
                const size_t childIndex = order[position];
                const size_t parentIndex = parents[childIndex];
                const std::optional<RenderMatrix> transform =
                    attachmentTransform(
                        parentIndex,
                        m_snapshot.entities[childIndex]
                            .attachToBoneInContainer);
                // Topology construction already rejected an invalid prepared
                // parent; attachmentTransform falls back to its root whenever
                // the requested bone is unavailable.
                if (transform)
                    composeAttachment(childIndex, parentIndex, *transform);
            }
        };

        constexpr size_t serialThreshold =
            performance_limits::kWorldPreparationEntityGrain;
        tf::Executor& executor = platform::runtime::renderWorkerExecutor();
        const size_t workerCount = std::max<size_t>(1u,
                                                    executor.num_workers());
        for (size_t depth = 1; depth <= maximumDepth; ++depth)
        {
            const size_t layerBegin = layerOffsets[depth];
            const size_t layerEnd = layerOffsets[depth + 1u];
            const size_t layerSize = layerEnd - layerBegin;
            if (layerSize <= serialThreshold || workerCount == 1u)
            {
                composeRange(layerBegin, layerEnd);
                continue;
            }

            const size_t taskCount = std::min(
                workerCount,
                (layerSize + serialThreshold - 1u) / serialThreshold);
            const size_t chunkSize =
                (layerSize + taskCount - 1u) / taskCount;
            auto& tasks = m_attachmentTasksScratch;
            tasks.clear();
            tasks.reserve(taskCount);
            size_t submittedEnd = layerBegin;
            bool submissionFailed = false;
            try
            {
                for (size_t chunkBegin = layerBegin; chunkBegin < layerEnd;
                     chunkBegin += chunkSize)
                {
                    const size_t chunkEnd =
                        std::min(layerEnd, chunkBegin + chunkSize);
                    tasks.push_back(executor.async(
                        "world-container-attachments",
                        [&composeRange, chunkBegin, chunkEnd]
                        {
                            platform::runtime::ThreadRoleScope role(
                                platform::runtime::ThreadRole::RenderWorker);
                            composeRange(chunkBegin, chunkEnd);
                        }));
                    submittedEnd = chunkEnd;
                }
            }
            catch (...)
            {
                submissionFailed = true;
            }

            std::exception_ptr taskFailure;
            for (std::future<void>& task : tasks)
            {
                try
                {
                    task.get();
                }
                catch (...)
                {
                    if (!taskFailure)
                        taskFailure = std::current_exception();
                }
            }
            tasks.clear();
            if (taskFailure)
                std::rethrow_exception(taskFailure);
            if (submissionFailed)
                composeRange(submittedEnd, layerEnd);
            // All tasks in this layer are joined before the next layer reads
            // their composed parent transforms and poses.
        }
    }

    // ParticleSysBone preparation intentionally bypasses ordinary frustum
    // culling, but it still belongs to the DependencyModel draw lifecycle.
    // Reuse the resolved channel pose so an add-on cannot leave emitters at
    // its stale logic/root position, and retire the emitter when its host did
    // not clear the dependency this frame.
    for (PreparedRenderInstance& emitter : m_preparedParticleEmitterInstances)
    {
        const auto sourceIndex =
            m_attachmentRenderIndexScratch.find(emitter.id);
        if (sourceIndex == m_attachmentRenderIndexScratch.end())
            continue;
        const RenderEntitySnapshot& source =
            m_snapshot.entities[sourceIndex->second];
        if (source.containerObjectId == 0 ||
            source.attachToBoneInContainer.empty())
        {
            continue;
        }
        const size_t index = sourceIndex->second;
        if (index >= m_preparedByInput.size() || m_preparedByInput[index].id == 0)
        {
            emitter = {};
            continue;
        }
        const PreparedRenderInstance& resolved = m_preparedByInput[index];
        const RenderMatrix emitterDelta =
            emitter.worldTransform.inverse() * resolved.worldTransform;
        for (std::optional<RenderMatrix>& anchor :
             preparationParticleEmitterBoneWorldTransforms(emitter)) {
            if (anchor) *anchor *= emitterDelta;
        }
        emitter.worldTransform = resolved.worldTransform;
        emitter.poseOffset = resolved.poseOffset;
        emitter.poseCount = resolved.poseCount;
        emitter.poseReady = resolved.poseReady;
        emitter.visibilityReady = resolved.visibilityReady;
        emitter.skeleton = resolved.skeleton;
        emitter.skeletonGeneration = resolved.skeletonGeneration;
        emitter.weaponLaunchBoneWorldTransforms = resolved.weaponLaunchBoneWorldTransforms;
        emitter.visual.scriptFlashTint = resolved.visual.scriptFlashTint;
        emitter.visual.heatVisionIntensity = resolved.visual.heatVisionIntensity;
        emitter.visual.heatVisionOnly = resolved.visual.heatVisionOnly;
        emitter.visual.objectOpacity = resolved.visual.objectOpacity;
    }
    std::erase_if(m_preparedParticleEmitterInstances,
                  [](const PreparedRenderInstance& emitter) { return emitter.id == 0; });
}

} // namespace engine::render
