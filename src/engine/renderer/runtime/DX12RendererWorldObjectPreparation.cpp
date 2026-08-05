#include "DX12RendererWorldAssetRuntime.h"

#include "RenderParallelExecutor.h"
#include "core/debug/debug.h"
#include "engine/renderer/world/effects/EnvironmentPresentationRender.h"

#include <vectorclass.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>

namespace engine {

void DX12Renderer::prepareWorldViewVisibility(
    const render::PreparedWorldFrame& frame,
    const render::RenderCameraSnapshot& camera) {
    if (!m_worldAssets) return;
    auto& visibility = m_worldAssets->frame.worldViewVisibility;
    visibility.resize(frame.visibleInstances.size(), 0u);
    m_worldAssets->frame.worldViewVisibleCount = 0;
    m_worldAssets->frame.worldViewDistanceCulledCount = 0;
    m_worldAssets->frame.worldViewFrustumCulledCount = 0;

    const float viewportAspect = m_d3d12.height() != 0u
        ? static_cast<float>(m_d3d12.width()) /
            static_cast<float>(m_d3d12.height())
        : 4.0f / 3.0f;
    const float guardBandX = std::isfinite(frame.viewCompatibility.guardBandX)
        ? frame.viewCompatibility.guardBandX : 0.0f;
    const float guardBandY = std::isfinite(frame.viewCompatibility.guardBandY)
        ? frame.viewCompatibility.guardBandY : 0.0f;
    const float guardBand = std::max({0.0f, guardBandX, guardBandY});
    const bool rangeEnabled = std::isfinite(camera.visibilityDistance) &&
        camera.visibilityDistance > 0.0f;

    const size_t instanceCount = frame.visibleInstances.size();
    const bool currentHotValid = frame.hot.validFor(instanceCount);
    const render::PreparedWorldFrame* previousFrame =
        m_worldAssets->pipeline.previousFrame();
    const bool previousHotValid = previousFrame &&
        previousFrame->hot.validFor(previousFrame->visibleInstances.size());
    const float alpha = std::clamp(
        m_worldAssets->view.current.interpolationAlpha, 0.0f, 1.0f);
    const bool simdCurrentEndpoint = currentHotValid && alpha >= 1.0f &&
        rangeEnabled;

    struct ViewCullingCounts final {
        size_t visible = 0;
        size_t distance = 0;
        size_t frustum = 0;
    };
    const auto prepareRange = [&](size_t begin,
                                  size_t end) -> ViewCullingCounts {
        ViewCullingCounts counts;
        size_t index = begin;
        if (simdCurrentEndpoint) {
            const Vec4f cameraX(camera.position.x());
            const Vec4f cameraY(camera.position.y());
            const Vec4f cameraZ(camera.position.z());
            const Vec4f maximumRange(camera.visibilityDistance + guardBand);
            for (; index + 4u <= end; index += 4u) {
                const Vec4f centerX = Vec4f().load(
                    frame.hot.positionX.data() + index);
                const Vec4f centerY = Vec4f().load(
                    frame.hot.positionY.data() + index);
                const Vec4f centerZ = Vec4f().load(
                    frame.hot.positionZ.data() + index);
                const Vec4f radius = max(
                    Vec4f(0.0f), Vec4f().load(
                        frame.hot.boundingRadii.data() + index));
                const Vec4f deltaX = centerX - cameraX;
                const Vec4f deltaY = centerY - cameraY;
                const Vec4f deltaZ = centerZ - cameraZ;
                const Vec4f distanceSquared =
                    deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
                const Vec4f maximumDistance = maximumRange + radius;
                const uint8_t rangeMask = to_bits(
                    is_finite(distanceSquared) &
                    (distanceSquared <= maximumDistance * maximumDistance));
                for (size_t lane = 0; lane < 4u; ++lane) {
                    const size_t instanceIndex = index + lane;
                    const bool inRange =
                        (rangeMask & (uint8_t{1u} << lane)) != 0u;
                    if (!inRange) {
                        ++counts.distance;
                        visibility[instanceIndex] = 0u;
                        continue;
                    }
                    const bool isVisible =
                        render::D3D12TerrainVisual::chunkSphereVisible(
                            camera, viewportAspect,
                            {frame.hot.positionX[instanceIndex],
                             frame.hot.positionY[instanceIndex],
                             frame.hot.positionZ[instanceIndex]},
                            frame.hot.boundingRadii[instanceIndex] +
                                guardBand);
                    if (!isVisible) ++counts.frustum;
                    visibility[instanceIndex] = isVisible ? 1u : 0u;
                    counts.visible += isVisible ? 1u : 0u;
                }
            }
        }
        for (; index < end; ++index) {
            float centerX;
            float centerY;
            float centerZ;
            float radius;
            if (currentHotValid) {
                centerX = frame.hot.positionX[index];
                centerY = frame.hot.positionY[index];
                centerZ = frame.hot.positionZ[index];
                radius = frame.hot.boundingRadii[index];
            } else {
                const render::PreparedRenderInstance& instance =
                    frame.visibleInstances[index];
                const math::vec3 center =
                    instance.worldTransform.translation() +
                    instance.cullingCenterOffset;
                centerX = center.x();
                centerY = center.y();
                centerZ = center.z();
                radius = std::isfinite(instance.boundingRadius)
                    ? std::max(0.0f, instance.boundingRadius) : 0.0f;
            }

            if (alpha < 1.0f && previousHotValid &&
                index < frame.previousEndpointIndices.size()) {
                const uint32_t previousIndex =
                    frame.previousEndpointIndices[index];
                if (previousIndex == UINT32_MAX ||
                    previousIndex >= previousFrame->hot.ids.size()) {
                    // B-only objects enter at the authoritative B boundary.
                    visibility[index] = 0u;
                    continue;
                }
                centerX = std::lerp(
                    previousFrame->hot.positionX[previousIndex],
                    centerX, alpha);
                centerY = std::lerp(
                    previousFrame->hot.positionY[previousIndex],
                    centerY, alpha);
                centerZ = std::lerp(
                    previousFrame->hot.positionZ[previousIndex],
                    centerZ, alpha);
                radius = std::lerp(
                    previousFrame->hot.boundingRadii[previousIndex],
                    radius, alpha);
            }

            const math::vec3 center{centerX, centerY, centerZ};
            radius = std::max(0.0f, radius) + guardBand;
            bool isVisible = true;
            if (rangeEnabled) {
                const math::vec3 delta = center - camera.position;
                const float distanceSquared = delta.length_sq();
                const float maximumDistance =
                    camera.visibilityDistance + radius;
                isVisible = std::isfinite(distanceSquared) &&
                    distanceSquared <= maximumDistance * maximumDistance;
            }
            if (!isVisible) {
                ++counts.distance;
            } else {
                isVisible = render::D3D12TerrainVisual::chunkSphereVisible(
                    camera, viewportAspect, center, radius);
                if (!isVisible) ++counts.frustum;
            }
            visibility[index] = isVisible ? 1u : 0u;
            counts.visible += isVisible ? 1u : 0u;
        }
        return counts;
    };

    constexpr size_t grain =
        render::performance_limits::kWorldViewEntityGrain;
    if (instanceCount >= grain * 2u &&
        render::parallelWorkerCount() > 1u) {
        const size_t taskCount = (instanceCount + grain - 1u) / grain;
        auto& taskVisibleCounts =
            m_worldAssets->frame.worldViewTaskVisibleCounts;
        auto& taskDistanceCounts =
            m_worldAssets->frame.worldViewTaskDistanceCulledCounts;
        auto& taskFrustumCounts =
            m_worldAssets->frame.worldViewTaskFrustumCulledCounts;
        taskVisibleCounts.assign(taskCount, 0u);
        taskDistanceCounts.assign(taskCount, 0u);
        taskFrustumCounts.assign(taskCount, 0u);
        auto& taskflow = m_worldAssets->frame.worldViewCullingTaskflow;
        taskflow.clear();
        for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex) {
            const size_t begin = taskIndex * grain;
            const size_t end = std::min(begin + grain, instanceCount);
            taskflow.emplace([&, taskIndex, begin, end] {
                const platform::runtime::ThreadRoleScope role(
                    platform::runtime::ThreadRole::RenderWorker);
                const ViewCullingCounts counts = prepareRange(begin, end);
                taskVisibleCounts[taskIndex] = counts.visible;
                taskDistanceCounts[taskIndex] = counts.distance;
                taskFrustumCounts[taskIndex] = counts.frustum;
            });
        }
        render::parallelExecutor().run(taskflow).wait();
        for (size_t taskIndex = 0; taskIndex < taskVisibleCounts.size();
             ++taskIndex) {
            m_worldAssets->frame.worldViewVisibleCount +=
                taskVisibleCounts[taskIndex];
            m_worldAssets->frame.worldViewDistanceCulledCount +=
                taskDistanceCounts[taskIndex];
            m_worldAssets->frame.worldViewFrustumCulledCount +=
                taskFrustumCounts[taskIndex];
        }
    } else {
        const ViewCullingCounts counts = prepareRange(0u, instanceCount);
        m_worldAssets->frame.worldViewVisibleCount = counts.visible;
        m_worldAssets->frame.worldViewDistanceCulledCount = counts.distance;
        m_worldAssets->frame.worldViewFrustumCulledCount = counts.frustum;
    }
}

void DX12Renderer::prepareWorldObjectPackets(
    const render::PreparedWorldFrame& frame,
    const render::TreeSwayRenderState& treeSway,
    float visualTimeSeconds,
    container::Span<const uint32_t> instanceSubset,
    bool currentEndpointOnly) {
    if (!m_worldAssets) return;
    const render::PreparedWorldFrame* previousFrame =
        currentEndpointOnly ? nullptr
                            : m_worldAssets->pipeline.previousFrame();
    const float interpolationAlpha = currentEndpointOnly
        ? 1.0f : m_worldAssets->view.current.interpolationAlpha;
    const auto retiredEndpointVisible = [&](size_t instanceIndex) {
        const render::PreparedRenderInstance& instance =
            frame.visibleInstances[instanceIndex];
        const math::vec3 center = frame.hot.validFor(
            frame.visibleInstances.size())
            ? math::vec3{
                  frame.hot.positionX[instanceIndex],
                  frame.hot.positionY[instanceIndex],
                  frame.hot.positionZ[instanceIndex]}
            : instance.worldTransform.translation() +
                  instance.cullingCenterOffset;
        const float guardBand = std::max({
            0.0f,
            std::isfinite(frame.viewCompatibility.guardBandX)
                ? frame.viewCompatibility.guardBandX : 0.0f,
            std::isfinite(frame.viewCompatibility.guardBandY)
                ? frame.viewCompatibility.guardBandY : 0.0f});
        const float radius = std::max(
            0.0f, std::isfinite(instance.boundingRadius)
                ? instance.boundingRadius : 0.0f) + guardBand;
        const render::RenderCameraSnapshot& camera =
            m_worldAssets->view.current.camera;
        if (std::isfinite(camera.visibilityDistance) &&
            camera.visibilityDistance > 0.0f) {
            const math::vec3 delta = center - camera.position;
            const float maximumDistance =
                camera.visibilityDistance + radius;
            if (!std::isfinite(delta.length_sq()) ||
                delta.length_sq() > maximumDistance * maximumDistance) {
                ++m_worldAssets->frame.worldViewDistanceCulledCount;
                return false;
            }
        }
        const float viewportAspect = m_d3d12.height() != 0u
            ? static_cast<float>(m_d3d12.width()) /
                static_cast<float>(m_d3d12.height())
            : 4.0f / 3.0f;
        const bool visible = render::D3D12TerrainVisual::chunkSphereVisible(
            camera, viewportAspect, center, radius);
        if (!visible) ++m_worldAssets->frame.worldViewFrustumCulledCount;
        return visible;
    };
    const size_t iterationCount = instanceSubset.empty()
        ? frame.visibleInstances.size() : instanceSubset.size();
    for (size_t iteration = 0; iteration < iterationCount; ++iteration) {
        const size_t instanceIndex = instanceSubset.empty()
            ? iteration : static_cast<size_t>(instanceSubset[iteration]);
        if (instanceIndex >= frame.visibleInstances.size()) continue;
        if (currentEndpointOnly) {
            if (!retiredEndpointVisible(instanceIndex)) continue;
            ++m_worldAssets->frame.worldViewVisibleCount;
        } else if (
            instanceIndex >= m_worldAssets->frame.worldViewVisibility.size() ||
            m_worldAssets->frame.worldViewVisibility[instanceIndex] == 0u) {
            continue;
        }
        const render::PreparedRenderInstance& instance =
            frame.visibleInstances[instanceIndex];
        if (instance.modelAsset.empty()) continue;

        const render::W3dModelHandle handle =
            m_worldAssets->residency.assets.requestAsync(
                instance.modelAsset, false,
                render::RenderAssetPriority::Visible);
        if (!handle) continue;

        const auto state = m_worldAssets->residency.assets.state(handle);
        if (state == render::W3dAssetState::Failed ||
            state == render::W3dAssetState::GpuUploadFailed) {
            if (m_worldAssets->residency.reportedAssetFailures.insert(instance.modelAsset).second) {
                TD_LOG_ERROR("[DX12Renderer] World asset '{}' failed: {}",
                    instance.modelAsset, m_worldAssets->residency.assets.error(handle));
            }
            if (state == render::W3dAssetState::GpuUploadFailed) {
                m_worldAssets->residency.assets.queueGpuUpload(
                    handle, render::W3dGpuUploadPriority::Visible);
            }
            continue;
        }
        if (state == render::W3dAssetState::CpuReady) {
            m_worldAssets->residency.assets.queueGpuUpload(
                handle, render::W3dGpuUploadPriority::Visible);
            continue;
        }
        if (state != render::W3dAssetState::GpuReady) continue;

        const auto model = std::dynamic_pointer_cast<const render::D3D12W3dModel>(
            m_worldAssets->residency.assets.gpuModel(handle));
        if (model && !model->retired()) {
            container::Span<const render::RenderMatrix> pose =
                frame.pose(instance);
            container::Span<const uint8_t> visibility =
                frame.visibility(instance);
            if (!pose.empty()) {
                const auto cpu = m_worldAssets->residency.assets.cpuModel(handle);
                const bool generationMatches = cpu && cpu->skeleton &&
                    instance.skeleton &&
                    instance.skeletonGeneration ==
                        instance.skeleton->generation() &&
                    instance.skeletonGeneration ==
                        cpu->skeleton->generation();
                if (!generationMatches) {
                    pose = {};
                    visibility = {};
                    if (m_worldAssets->frame.poseBindingGenerationRejects !=
                        std::numeric_limits<uint32_t>::max()) {
                        ++m_worldAssets->frame.poseBindingGenerationRejects;
                    }
                }
            }
            const render::RenderVector modelFlashTint =
                instance.visual.scriptFlashTint +
                m_worldAssets->selectionFlash.tintFor(
                    instance.objectId != 0 ? instance.objectId : instance.id);
            const math::transform instanceWorld =
                instance.visual.treeSwayEnabled
                    ? render::applyTreeSwayPresentation(
                          instance.worldTransform, treeSway, instance.id, frame.simulationFrame)
                    : instance.worldTransform;
            const render::PreparedRenderInstance* previousInstance = nullptr;
            container::Span<const render::RenderMatrix> previousPose;
            math::transform previousInstanceWorld;
            if (previousFrame && interpolationAlpha < 1.0f &&
                instanceIndex < frame.interpolationEligible.size() &&
                frame.interpolationEligible[instanceIndex] != 0u &&
                instanceIndex < frame.previousEndpointIndices.size()) {
                const uint32_t previousIndex =
                    frame.previousEndpointIndices[instanceIndex];
                if (previousIndex != UINT32_MAX &&
                    previousIndex < previousFrame->visibleInstances.size()) {
                    const render::PreparedRenderInstance& candidate =
                        previousFrame->visibleInstances[previousIndex];
                    const bool sameRenderable =
                        candidate.id == instance.id &&
                        candidate.modelAsset == instance.modelAsset &&
                        candidate.skeletonGeneration ==
                            instance.skeletonGeneration;
                    if (sameRenderable) {
                        previousInstance = &candidate;
                        previousPose = previousFrame->pose(candidate);
                        previousInstanceWorld =
                            candidate.visual.treeSwayEnabled
                            ? render::applyTreeSwayPresentation(
                                  candidate.worldTransform,
                                  previousFrame->treeSway, candidate.id,
                                  previousFrame->simulationFrame)
                            : candidate.worldTransform;
                    }
                }
            }
            const size_t instancePacketStart =
                m_worldAssets->frame.drawPackets.size();
            auto& treeTextureOverrides =
                m_worldAssets->frame.materialTextureOverrideScratch;
            treeTextureOverrides.clear();
            if (!instance.visual.treeTextureAsset.empty()) {
                const std::optional<uint32_t> treeTextureSrv =
                    m_worldAssets->residency.treeTextureOverrides.acquire(
                        *m_worldAssets->residency.textures,
                        instance.visual.treeTextureAsset,
                        frame.presentationEpoch);
                if (treeTextureSrv) {
                    const container::Span<const render::W3dMaterialTextureBinding>
                        bindings = model->materialTextureBindings();
                    treeTextureOverrides.reserve(bindings.size());
                    for (const render::W3dMaterialTextureBinding& binding :
                         bindings) {
                        treeTextureOverrides.push_back({
                            .materialIndex = binding.materialIndex,
                            .textureSrvIndex = *treeTextureSrv,
                            .samplerMode = 3,
                            .overridesDetailTexture = true,
                            .detailTextureSrvIndex = 0,
                        });
                    }
                } else if (m_worldAssets->residency.reportedAssetFailures.insert(
                               instance.visual.treeTextureAsset +
                               ":W3DTreeDraw.TextureName").second) {
                    TD_LOG_ERROR(
                        "[DX12Renderer] W3DTreeDraw texture '{}' upload failed",
                        instance.visual.treeTextureAsset);
                }
            }
            static_cast<void>(render::appendW3dModelGraphDrawPackets(
                m_worldAssets->residency.assets, handle, instanceWorld,
                pose, visibility,
                m_worldAssets->frame.drawPackets,
                {.visualTimeSeconds = visualTimeSeconds,
                 .directionalLightScale = instance.directionalLightScale,
                 .materialTextureOverrides = treeTextureOverrides,
                 .scriptFlashTint = modelFlashTint,
                 .heatVisionIntensity =
                     instance.visual.heatVisionIntensity,
                 .heatVisionOnly = instance.visual.heatVisionOnly,
                 .objectOpacity = instance.visual.objectOpacity,
                 .scriptIndicatorColor =
                     instance.visual.scriptIndicatorColor,
                 .hasScriptIndicatorColor =
                     instance.visual.hasScriptIndicatorColor,
                 .receivesDynamicLights =
                     instance.visual.receivesDynamicLights,
                 .subObjectVisibility =
                     instance.visual.subObjectVisibility,
                 .treePushAsideDirection =
                     {instance.visual.treePushAsideDirection.x(),
                      instance.visual.treePushAsideDirection.y()},
                 .treePushAsideAmount =
                     instance.visual.treePushAsideAmount,
                 .treePushAsideDistanceFactor =
                     instance.visual.treePushAsideDistanceFactor,
                 .treePushAsideDarkeningFactor =
                     instance.visual.treePushAsideDarkeningFactor,
                 .vehicleTreads = instance.visual.vehicleTreads,
                 .restPalettes = &m_worldAssets->residency.restPalettes,
                 .previousEntityWorld = previousInstance
                     ? &previousInstanceWorld : nullptr,
                 .previousSkinPalette = previousPose,
                 .interpolationAlpha = previousInstance
                     ? interpolationAlpha : 1.0f},
                &m_worldAssets->frame.modelGraphTraversalStats));
            for (size_t packetIndex = instancePacketStart;
                 packetIndex < m_worldAssets->frame.drawPackets.size();
                 ++packetIndex) {
                render::StaticMeshDrawPacket& packet =
                    m_worldAssets->frame.drawPackets[packetIndex];
                packet.receivesMapBorder = true;
                packet.receivesVisibility =
                    instance.visual.receivesLocalVisibility;
                if (!instance.shadow.castsDirectionalShadow()) {
                    packet.castsShadow = false;
                }
            }
        }
    }
    if (!currentEndpointOnly && interpolationAlpha < 1.0f &&
        previousFrame &&
        !frame.retiredPreviousEndpointIndices.empty()) {
        const uint32_t previousTickRate = std::max(
            1u, previousFrame->objectUi.logicFramesPerSecond);
        const float previousVisualTimeSeconds =
            static_cast<float>(previousFrame->simulationFrame) /
            static_cast<float>(previousTickRate);
        prepareWorldObjectPackets(
            *previousFrame, previousFrame->treeSway,
            previousVisualTimeSeconds,
            frame.retiredPreviousEndpointIndices, true);
    }
    if (!currentEndpointOnly) {
        m_worldAssets->pipeline.recordViewCullingStats(
            static_cast<uint32_t>(std::min<size_t>(
                m_worldAssets->frame.worldViewDistanceCulledCount,
                std::numeric_limits<uint32_t>::max())),
            static_cast<uint32_t>(std::min<size_t>(
                m_worldAssets->frame.worldViewFrustumCulledCount,
                std::numeric_limits<uint32_t>::max())));
    }
}

} // namespace engine
