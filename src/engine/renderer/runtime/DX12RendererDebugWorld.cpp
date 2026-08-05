#include "DX12RendererWorldAssetRuntime.h"

#include "core/debug/debug.h"
#include "engine/renderer/world/pipeline/DebugWorldCameraVisualSettings.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"
#include "presentation/camera/GameCameraInput.h"
#include "presentation/render/HeatVisionVisualSettings.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine {

void DX12Renderer::prepareDebugWorld(float elapsedSeconds) {
    if (!m_worldAssets ||
        !m_worldAssets->debugWorld.hasVisualSource(
            static_cast<bool>(m_worldAssets->fx.runtime))) {
        return;
    }

    // renderDebugWorld() consumes the sealed frame once it becomes ready.
    // Do not join the resource executor merely to replace an in-flight debug
    // snapshot; retaining one older visual frame is the correct newest-only
    // presentation behavior.
    if (m_worldAssets->debugWorld.preparationPending()) return;

    render::WorldRenderSnapshot snapshot;
    snapshot.simulationFrame =
        m_worldAssets->debugWorld.nextSimulationFrame();
    if (m_worldRenderer) snapshot.camera = m_worldRenderer->debugCameraSnapshot();
    snapshot.camera.visibilityDistance = std::max(
        2000.0f, snapshot.camera.farClip);
    const auto& debugTerrain = m_worldAssets->debugWorld.terrain();
    snapshot.terrain = debugTerrain;
    if (m_worldAssets->debugWorld.visibilityEnabled() && debugTerrain &&
        debugTerrain->width > 1 && debugTerrain->height > 1) {
        render::LocalVisibilityRenderSnapshot visibility;
        visibility.presentationEpoch = 1;
        visibility.revision = 1;
        visibility.terrainLayoutRevision =
            debugTerrain->layoutRevision;
        visibility.observerPlayer = 0;
        visibility.width = debugTerrain->width - 1;
        visibility.height = debugTerrain->height - 1;
        visibility.borderSize = debugTerrain->borderSize;
        visibility.originX =
            -static_cast<float>(visibility.borderSize) *
            debugTerrain->cellWorldSize;
        visibility.originY = visibility.originX;
        visibility.cellWorldSize = debugTerrain->cellWorldSize;
        visibility.enabled = true;
        visibility.dirtyRegion = {
            .minX = 0,
            .minY = 0,
            .maxX = visibility.width - 1,
            .maxY = visibility.height - 1,
        };
        visibility.cells.resize(
            static_cast<size_t>(visibility.width) *
            static_cast<size_t>(visibility.height));
        for (int32_t y = 0; y < visibility.height; ++y) {
            for (int32_t x = 0; x < visibility.width; ++x) {
                const int32_t band = (x * 3) / std::max(visibility.width, 1);
                visibility.cells[static_cast<size_t>(y) * visibility.width + x] =
                    static_cast<uint8_t>(std::clamp(band, 0, 2));
            }
        }
        snapshot.localVisibility = std::move(visibility);
    }
    const render::W3dModelHandle debugModel =
        m_worldAssets->debugWorld.model();
    if (debugModel) {
        const container::String modelAsset =
            m_worldAssets->residency.assets.prototype(debugModel);
        const math::vec3 debugOrigin = debugTerrain
            ? debugTerrain->worldPosition(
                  debugTerrain->width / 2,
                  debugTerrain->height / 2)
            : math::vec3::zero();
        // The material-effects probe is deliberately a semantic A/B scene,
        // not a shader swatch row. The left slot contains the same stealthed
        // unit before detection (therefore submits no geometry); the right
        // slot shows it after detection through the heat-vision-only path.
        constexpr container::Array<float, 2> showcaseOffsets{-0.8f, 0.8f};
        constexpr container::Array<float, 3> defaultOffsets{-1.0f, 0.0f, 1.0f};
        const container::Span<const float> instanceOffsets =
            m_worldAssets->debugWorld.materialEffectsEnabled()
            ? container::Span<const float>(showcaseOffsets)
            : container::Span<const float>(defaultOffsets);
        snapshot.entities.reserve(instanceOffsets.size());
        for (size_t index = 0; index < instanceOffsets.size(); ++index) {
            render::RenderEntitySnapshot entity;
            entity.id = static_cast<render::RenderEntityId>(index + 1);
            entity.modelAsset = modelAsset;
            entity.transform.position = debugOrigin + math::vec3{
                instanceOffsets[index] *
                    m_worldAssets->debugWorld.instanceSpacing(),
                0.0f, 0.0f};
            entity.transform.orientation = math::quat::from_axis_angle(
                {0.0f, 0.0f, 1.0f},
                elapsedSeconds * (0.20f + static_cast<float>(index) * 0.08f));
            const float showcaseScale =
                m_worldAssets->debugWorld.materialEffectsEnabled()
                ? 1.35f : 1.0f;
            entity.transform.scale = {
                showcaseScale, showcaseScale, showcaseScale};
            entity.boundingRadius =
                m_worldAssets->debugWorld.boundingRadius() * showcaseScale;
            entity.visual.animationState =
                m_worldAssets->debugWorld.animationState();
            entity.visual.animationTimeSeconds = elapsedSeconds;
            if (m_worldAssets->debugWorld.materialEffectsEnabled()) {
                if (index == 0u) {
                    // Undetected enemy stealth: reserve the left comparison
                    // position but let normal hidden-instance culling prove
                    // that the model itself is not visible.
                    entity.visual.hidden = true;
                } else {
                    // Detected enemy stealth: the base material remains
                    // suppressed and only the orange heat response appears.
                    entity.visual.heatVisionIntensity =
                        heat_vision::visual_settings::heatVisionOpacityAfterFrames(
                            snapshot.simulationFrame % 15u);
                    entity.visual.heatVisionOnly = true;
                }
            }
            snapshot.entities.push_back(std::move(entity));
        }
    }
    m_worldAssets->debugWorld.setPreparationPending(
        prepareWorldSnapshot(std::move(snapshot)));
}

void DX12Renderer::renderDebugWorld(float elapsedSeconds) {
    if (!m_worldRenderer) return;

    if (m_worldAssets &&
        m_worldAssets->debugWorld.hasVisualSource(
            static_cast<bool>(m_worldAssets->fx.runtime))) {
        if (!m_worldAssets->debugWorld.preparationPending()) {
            prepareDebugWorld(elapsedSeconds);
        }
        if (m_worldAssets->debugWorld.preparationPending()) {
            if (renderPreparedWorld() != 0) return;
        }

        const render::W3dModelHandle debugModel =
            m_worldAssets->debugWorld.model();
        if (debugModel) {
            const auto state = m_worldAssets->residency.assets.state(debugModel);
            if (state == render::W3dAssetState::Failed ||
                state == render::W3dAssetState::GpuUploadFailed) {
                if (!m_worldAssets->debugWorld.failureReported()) {
                    TD_LOG_ERROR("[DX12Renderer] Debug W3D '{}' failed: {}",
                        m_worldAssets->residency.assets.sourcePath(debugModel),
                        m_worldAssets->residency.assets.error(debugModel));
                    m_worldAssets->debugWorld.markFailureReported();
                }
                if (state == render::W3dAssetState::GpuUploadFailed) {
                    m_worldAssets->residency.assets.queueGpuUpload(
                        debugModel,
                        render::W3dGpuUploadPriority::Visible);
                }
            }
        }
    }

    // Preserve the always-available geometry diagnostic while an asset is
    // absent, loading, or failed.
    if (!m_d3d12.beginWorldRenderPass()) return;
    m_worldRenderer->renderDebugScene(elapsedSeconds);
    m_d3d12.resolveWorldRenderPass();
}

bool DX12Renderer::setDebugWorldAsset(const container::String& source) {
    if (!m_worldAssets || source.empty()) return false;

    render::W3dAssetRequest request;
    request.source = source;
    request.queueGpuUpload = true;
    request.gpuUploadPriority = render::W3dGpuUploadPriority::Visible;
    const render::W3dModelHandle handle = m_worldAssets->residency.assets.request(request);
    if (!handle) {
        TD_LOG_ERROR("[DX12Renderer] Invalid debug W3D request: '{}'", source);
        return false;
    }

    const auto state = m_worldAssets->residency.assets.state(handle);
    if (state == render::W3dAssetState::Failed) {
        TD_LOG_ERROR("[DX12Renderer] Debug W3D CPU load failed '{}': {}",
            source, m_worldAssets->residency.assets.error(handle));
        if (handle != m_worldAssets->debugWorld.model()) {
            m_worldAssets->residency.assets.evict(handle);
        }
        return false;
    }

    const render::W3dModelHandle previous =
        m_worldAssets->debugWorld.model();
    if (m_worldAssets->debugWorld.preparationPending()) {
        m_worldAssets->pipeline.finishPreparation();
        m_worldAssets->debugWorld.cancelPreparation();
    }
    m_worldAssets->submittedPreparationPending = false;
    m_worldAssets->debugWorld.setModel(handle);
    m_worldAssets->residency.assets.setGpuResidencyPinned(
        handle, render::RenderAssetPinScope::Debug, true);
    if (previous && previous != handle) {
        m_worldAssets->residency.assets.setGpuResidencyPinned(
            previous, render::RenderAssetPinScope::Debug, false);
        m_worldAssets->residency.assets.evict(previous);
    }

    if (const auto cpuModel = m_worldAssets->residency.assets.cpuModel(handle)) {
        const math::vec3 center = cpuModel->bounds.center();
        const math::vec3 extents = cpuModel->bounds.extents();
        const float radius = math::max(extents.length(), 0.25f);
        m_worldAssets->debugWorld.configureModelBounds(radius);

        const float sceneHalfWidth =
            m_worldAssets->debugWorld.instanceSpacing() + radius;
        const float cameraDistance = math::max(sceneHalfWidth * 1.35f, radius * 4.0f);
        const math::vec3 viewDirection = math::vec3{1.0f, -1.0f, 0.65f}.normalized();
        m_worldRenderer->debugCamera().lookAt(center + viewDirection * cameraDistance, center);
        m_worldRenderer->debugCamera().setPerspective(
            math::deg_to_rad(60.0f),
            debug_world_camera::visual_defaults::nearClipForDistance(cameraDistance),
            math::max(2000.0f, cameraDistance * 10.0f));

        if (!cpuModel->animations.empty()) {
            container::String animationNames;
            constexpr size_t maxReportedAnimations = 8;
            const size_t count = std::min(cpuModel->animations.size(), maxReportedAnimations);
            for (size_t index = 0; index < count; ++index) {
                if (!animationNames.empty()) animationNames += ", ";
                animationNames += cpuModel->animations[index].name;
            }
            if (cpuModel->animations.size() > count) animationNames += ", ...";
            TD_LOG_INFO("[DX12Renderer] Debug W3D animations ({}): {}",
                        cpuModel->animations.size(), animationNames);
        }
    }

    TD_LOG_INFO("[DX12Renderer] Debug W3D queued: '{}' prototype='{}'",
        m_worldAssets->residency.assets.sourcePath(handle),
        m_worldAssets->residency.assets.prototype(handle));
    return true;
}

bool DX12Renderer::setDebugWorldTerrain(
    container::SharedPtr<const render::TerrainRenderSnapshot> terrain) {
    if (!m_worldAssets || !terrain || !terrain->isValid()) return false;

#if TD_DEBUG_ENABLED
    const auto pointInsidePolygon = [](const container::Vector<render::RenderVector>& polygon,
                                       float x, float y) noexcept {
        bool inside = false;
        if (polygon.size() < 3) return inside;
        for (size_t current = 0, previous = polygon.size() - 1;
             current < polygon.size(); previous = current++) {
            const render::RenderVector& a = polygon[current];
            const render::RenderVector& b = polygon[previous];
            const bool crosses = (a.y() > y) != (b.y() > y);
            if (!crosses) continue;
            const float edgeX = (b.x() - a.x()) * (y - a.y()) /
                    (b.y() - a.y()) +
                a.x();
            if (x < edgeX) inside = !inside;
        }
        return inside;
    };
    for (const render::TerrainWaterRenderArea& water : terrain->waterAreas) {
        float minimumPolygonZ = std::numeric_limits<float>::max();
        float maximumPolygonZ = -std::numeric_limits<float>::max();
        float minimumPolygonX = std::numeric_limits<float>::max();
        float minimumPolygonY = std::numeric_limits<float>::max();
        float maximumPolygonX = -std::numeric_limits<float>::max();
        float maximumPolygonY = -std::numeric_limits<float>::max();
        float minimumTerrain = std::numeric_limits<float>::max();
        float maximumTerrain = -std::numeric_limits<float>::max();
        uint64_t insideSamples = 0;
        uint64_t belowSamples = 0;
        uint64_t equalSamples = 0;
        uint64_t aboveSamples = 0;
        for (const render::RenderVector& point : water.polygon) {
            minimumPolygonX = std::min(minimumPolygonX, point.x());
            minimumPolygonY = std::min(minimumPolygonY, point.y());
            maximumPolygonX = std::max(maximumPolygonX, point.x());
            maximumPolygonY = std::max(maximumPolygonY, point.y());
            minimumPolygonZ = std::min(minimumPolygonZ, point.z());
            maximumPolygonZ = std::max(maximumPolygonZ, point.z());
        }
        for (int32_t y = 0; y < terrain->height; ++y) {
            for (int32_t x = 0; x < terrain->width; ++x) {
                const render::RenderVector position = terrain->worldPosition(x, y);
                if (!pointInsidePolygon(water.polygon, position.x(), position.y())) continue;
                ++insideSamples;
                minimumTerrain = std::min(minimumTerrain, position.z());
                maximumTerrain = std::max(maximumTerrain, position.z());
                const float delta = water.surfaceHeight - position.z();
                if (delta > 0.001f) ++belowSamples;
                else if (delta < -0.001f) ++aboveSamples;
                else ++equalSamples;
            }
        }
        TD_LOG_INFO(
            "[DX12Renderer] Debug water '{}' id={} legacy={} surface={:.3f} "
            "polygonXY=[{:.1f},{:.1f}]-[{:.1f},{:.1f}] polygonZ=[{:.3f},{:.3f}] "
            "terrainSamples={} below={} equal={} above={} terrainZ=[{:.3f},{:.3f}]",
            water.name, water.triggerId, water.synthesizedLegacyWater,
            water.surfaceHeight, minimumPolygonX, minimumPolygonY,
            maximumPolygonX, maximumPolygonY,
            minimumPolygonZ, maximumPolygonZ, insideSamples,
            belowSamples, equalSamples, aboveSamples,
            insideSamples != 0 ? minimumTerrain : 0.0f,
            insideSamples != 0 ? maximumTerrain : 0.0f);
        if (water.polygon.size() <= 8u) {
            for (size_t pointIndex = 0; pointIndex < water.polygon.size(); ++pointIndex) {
                const render::RenderVector& point = water.polygon[pointIndex];
                TD_LOG_INFO(
                    "[DX12Renderer] Debug water point {}=({:.1f},{:.1f},{:.1f})",
                    pointIndex, point.x(), point.y(), point.z());
            }
        }
    }
#endif

    const math::vec3 center = terrain->worldPosition(terrain->width / 2, terrain->height / 2);
    const float halfWidth = static_cast<float>(terrain->width - terrain->borderSize * 2) *
                            terrain->cellWorldSize * 0.5f;
    const float halfHeight = static_cast<float>(terrain->height - terrain->borderSize * 2) *
                             terrain->cellWorldSize * 0.5f;
    const float radius = math::max(math::vec3{halfWidth, halfHeight, 0.0f}.length(), 50.0f);
    const math::vec3 viewDirection = math::vec3{1.0f, -1.0f, 1.1f}.normalized();
    const float cameraDistance = radius * 2.1f;
    m_worldRenderer->debugCamera().lookAt(
        center + viewDirection * cameraDistance, center);
    m_worldRenderer->debugCamera().setPerspective(
        math::deg_to_rad(60.0f),
        debug_world_camera::visual_defaults::nearClipForDistance(cameraDistance),
        radius * 8.0f);
    m_worldAssets->debugWorld.setTerrain(std::move(terrain));
    const auto& acceptedTerrain = m_worldAssets->debugWorld.terrain();
    TD_LOG_INFO("[DX12Renderer] Debug terrain accepted: revision={} samples={}x{} border={}",
        acceptedTerrain->revision, acceptedTerrain->width,
        acceptedTerrain->height, acceptedTerrain->borderSize);
    return true;
}

void DX12Renderer::setDebugWorldVisibility(bool enabled) noexcept {
    if (!m_worldAssets) return;
    m_worldAssets->debugWorld.setVisibilityEnabled(enabled);
}

container::SharedPtr<const render::TerrainRenderSnapshot>
DX12Renderer::debugWorldTerrainSnapshot() const noexcept {
    return m_worldAssets ? m_worldAssets->debugWorld.terrain() : nullptr;
}

void DX12Renderer::focusDebugWorldCamera(math::vec3 target, float distance) noexcept {
    if (!m_worldRenderer || !std::isfinite(distance)) return;
    distance = std::max(distance, 1.0f);
    const math::vec3 viewDirection = math::vec3{1.0f, -1.0f, 0.85f}.normalized();
    m_worldRenderer->debugCamera().lookAt(target + viewDirection * distance, target);
    m_worldRenderer->debugCamera().setPerspective(
        math::deg_to_rad(55.0f),
        debug_world_camera::visual_defaults::nearClipForDistance(distance),
        std::max(2000.0f, distance * 20.0f));
}

bool DX12Renderer::applyDebugWorldCameraInput(
    const GameCameraInput& input, float deltaSeconds) noexcept {
    if (!m_worldRenderer || !input.hasManualInput()) return false;

    render::WorldCamera& debugCamera = m_worldRenderer->debugCamera();
    GameCameraState camera{
        .position = debugCamera.position(),
        .target = debugCamera.target(),
        .up = debugCamera.up(),
        .verticalFovRadians = debugCamera.verticalFovRadians(),
        // Camera input limits should remain independent from the adaptive
        // render near plane; otherwise zooming out once would permanently
        // prevent a later close inspection.
        .nearClip = debug_world_camera::visual_defaults::kMinimumNearClip,
        .farClip = debugCamera.farClip(),
        .visibilityDistance = debugCamera.farClip(),
    };
    // Match GameCameraController's standalone update semantics: a paused
    // debugger or dragged window cannot turn held arrow input into a camera
    // teleport when presentation resumes.
    const float safeDelta = std::isfinite(deltaSeconds) && deltaSeconds > 0.0f
        ? std::min(deltaSeconds, 0.100f)
        : 0.0f;
    GameCameraManipulator::apply(camera, input, safeDelta);
    debugCamera.lookAt(camera.position, camera.target, camera.up);
    const float cameraDistance = (camera.position - camera.target).length();
    debugCamera.setPerspective(
        camera.verticalFovRadians,
        debug_world_camera::visual_defaults::nearClipForDistance(cameraDistance),
        camera.farClip);
    return true;
}

bool DX12Renderer::zoomDebugWorldCamera(float wheelUnits) noexcept {
    if (!m_worldRenderer || !std::isfinite(wheelUnits) ||
        std::abs(wheelUnits) <= math::EPSILON) {
        return false;
    }
    render::WorldCamera& camera = m_worldRenderer->debugCamera();
    math::vec3 radial = camera.position() - camera.target();
    if (radial.length_sq() <= math::EPSILON * math::EPSILON) {
        radial = {1.0f, -1.0f, 0.85f};
    }
    const float distance = std::clamp(
        radial.length() * std::pow(0.85f, wheelUnits), 2.0f, 10000.0f);
    camera.lookAt(camera.target() + radial.normalized() * distance, camera.target());
    camera.setPerspective(
        camera.verticalFovRadians(),
        debug_world_camera::visual_defaults::nearClipForDistance(distance),
        camera.farClip());
    return true;
}

void DX12Renderer::setDebugWorldAnimation(container::String animationState) {
    if (!m_worldAssets) return;
    m_worldAssets->debugWorld.setAnimationState(
        std::move(animationState));
}

void DX12Renderer::setDebugMaterialEffects(bool enabled) noexcept {
    if (!m_worldAssets) return;
    m_worldAssets->debugWorld.setMaterialEffectsEnabled(enabled);
    if (enabled) {
        TD_LOG_INFO(
            "[DX12Renderer] Debug stealth showcase enabled: "
            "left=undetected/invisible, right=detected/heat-vision-only");
    }
}

void DX12Renderer::setWorldSkeletonMode(bool enabled) {
    if (m_worldRenderer) m_worldRenderer->setSkeletonMode(enabled);
}

void DX12Renderer::setWorldTextureOnlyMode(bool enabled) {
    if (m_worldRenderer) m_worldRenderer->setTextureOnlyMode(enabled);
}

} // namespace engine
