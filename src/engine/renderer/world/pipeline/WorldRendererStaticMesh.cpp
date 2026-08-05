#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include "debug/debug.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/model/W3dStaticModel.h"
#include "engine/renderer/world/pipeline/WorldRendererGpuLayout.h"
#include "engine/renderer/world/pipeline/WorldRendererMaterialPacking.h"
#include "engine/renderer/world/pipeline/WorldRendererPipelineStateContract.h"
#include "engine/renderer/world/pipeline/WorldRendererShadowSettings.h"
#include "engine/renderer/world/pipeline/WorldRendererUploadOwner.h"
#include "presentation/render/LocalVisibilityVisualSettings.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <tuple>
#include <type_traits>

namespace engine::render {
namespace {

using world_renderer_detail::StaticMeshGpuInstance;
using world_renderer_detail::StaticMeshRecordedDraw;
using world_renderer_detail::WorldRendererUploadOwner;

struct alignas(16) CameraConstants {
    float viewProjection[16];
    float cameraPosition[3];
    float padding0 = 0.0f;
    float cameraRight[3]{1.0f, 0.0f, 0.0f};
    float worldTimeSeconds = 0.0f;
    float cameraUp[3]{0.0f, 0.0f, 1.0f};
    float cameraBasisPadding = 0.0f;
    float fogColor[3];
    float fogEnabled = 0.0f;
    float fogStartDistance = 0.0f;
    float fogEndDistance = 0.0f;
    // Reuse the former fog padding for the active WorldBuilder boundary. The
    // matching HLSL layout remains unchanged.
    float playableMinimum[2]{};
    float sceneAmbient[3]{0.25f, 0.25f, 0.25f};
    float ambientPadding = 0.0f;
    // xyz is surface-to-light and w remains explicit padding. Fixed-size
    // arrays keep the C++/HLSL CBV layout stable and match GlobalLighting's
    // three object-light slots.
    float directionalToLight[kTerrainRenderGlobalLightCount][4]{};
    float directionalDiffuse[kTerrainRenderGlobalLightCount][4]{};
    // xyz + inner radius, then RGB already multiplied by intensity + outer
    // radius.  Fixed arrays avoid descriptor/resource lifetime in the first
    // renderer-only point-light path.
    float dynamicPointPositionInner[kMaximumWorldDynamicPointLights][4]{};
    float dynamicPointColorOuter[kMaximumWorldDynamicPointLights][4]{};
    float dynamicPointAmbient[kMaximumWorldDynamicPointLights][4]{};
    uint32_t dynamicPointLightCount = 0;
    uint32_t dynamicPointLightPadding[3]{};
    float shadowViewProjection[16]{};
    float shadowTexelSize[2]{};
    float shadowDepthBias = 0.0012f;
    float shadowStrength = 0.0f;
    float visibilityOrigin[2]{};
    float visibilityInvCellSize = 0.0f;
    float visibilityEnabled = 0.0f;
    float visibilityTextureSize[2]{};
    // These occupy the former explored/shrouded scalar slots. The R8 texture
    // already carries those luminance values directly.
    float playableMaximum[2]{};
    float waterReflectionViewProjection[16]{};
    uint32_t waterReflectionEnabled = 0;
    uint32_t playableBoundsEnabled = 0;
    float playableBorderFadeWidth = 0.0f;
    uint32_t borderShroudEnabled = 0;
};
static_assert(kMaximumWorldDynamicPointLights == 20);
static_assert(std::is_standard_layout_v<CameraConstants>);
static_assert(offsetof(CameraConstants, directionalToLight) == 160);
static_assert(offsetof(CameraConstants, directionalDiffuse) == 208);
static_assert(offsetof(CameraConstants, dynamicPointPositionInner) == 256);
static_assert(offsetof(CameraConstants, dynamicPointColorOuter) == 576);
static_assert(offsetof(CameraConstants, dynamicPointAmbient) == 896);
static_assert(offsetof(CameraConstants, dynamicPointLightCount) == 1216);
static_assert(offsetof(CameraConstants, shadowViewProjection) == 1232);
static_assert(offsetof(CameraConstants, visibilityOrigin) == 1312);
static_assert(offsetof(CameraConstants, visibilityTextureSize) == 1328);
static_assert(offsetof(CameraConstants, waterReflectionViewProjection) == 1344);
static_assert(offsetof(CameraConstants, waterReflectionEnabled) == 1408);
static_assert(offsetof(CameraConstants, playableBoundsEnabled) == 1412);
static_assert(offsetof(CameraConstants, playableBorderFadeWidth) == 1416);
static_assert(offsetof(CameraConstants, borderShroudEnabled) == 1420);
static_assert(sizeof(CameraConstants) == 1424);

struct alignas(16) WorldConstants {
    float world[16];
    float diffuse[4];
    float ambient[3];
    float shininess = 0.0f;
    float specular[3];
    float alphaCutoff = -1.0f;
    float emissive[3];
    uint32_t skinBoneCount = 0;
    uint32_t samplerMode = 0;
    uint32_t alphaTestMode = 0;
    uint32_t detailSamplerMode = 0;
    uint32_t hasDetailTexture = 0;
    uint32_t detailColorFunc = 0;
    uint32_t detailAlphaFunc = 0;
    uint32_t fogFunc = 0;
    // Diagnostic wireframe needs to remain visible even if a terrain's
    // prelit vertex colours are all black.  This occupies the existing
    // padding slot and therefore does not change the GPU constant layout.
    uint32_t ignoreVertexColor = 0;
    uint32_t lightingEnabled = 0;
    uint32_t waterSurface = 0;
    uint32_t terrainEdgePhase = 0;
    // Script infantry lighting scales the directional environment only.
    // This reuses the old padding lane, preserving the 16-byte CBV layout.
    float directionalLightScale = 1.0f;
    // Drawable::colorFlash contributes this envelope to the local light
    // environment.  It is added in the lit-material branch so prelit W3D
    // meshes retain their original no-lighting behaviour.
    float scriptFlashTint[3]{};
    float scriptFlashTintPadding = 0.0f;
    // A script custom indicator/house colour is carried per draw packet. The
    // two flags select RefCode's HOUSECOLOR vertex-material and ZHC texture
    // conventions without ever editing an immutable W3D asset.
    float scriptIndicatorColor[3]{};
    uint32_t houseColorFlags = 0;
    // 0 disables receiving, 1 shadows the primary dynamic light, and 2
    // attenuates the estimated primary-light share of prelit terrain/W3D.
    uint32_t receivesShadow = 0;
    uint32_t receivesVisibility = 0;
    // Point-light receiving follows the packet's existing receiver policy;
    // it is intentionally independent of directional-shadow availability.
    uint32_t receivesDynamicPointLights = 0;
    uint32_t texturingEnabled = 1;
    float mapperScaleOffset[2][4]{
        {1.0f, 1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f, 0.0f},
    };
    // Linear uses xy as U/V per second. Rotate uses x as turns per second;
    // both use zw as the authored rotation center where applicable.
    float mapperMotionCenter[2][4]{};
    uint32_t mapperTypes[2]{};
    uint32_t mapperClampFix[2]{};
    float mapperTimeSeconds = 0.0f;
    float mapperPadding[3]{};
    float objectDynamicAmbient[3]{};
    uint32_t objectDynamicDiffuseCount = 0;
    float objectDynamicPositionInner[
        dynamic_lights::performance_limits::kObjectDiffuseMaximumLights][4]{};
    float objectDynamicColorOuter[
        dynamic_lights::performance_limits::kObjectDiffuseMaximumLights][4]{};
    uint32_t terrainMacroFlags = 0;
    float terrainMacroScale = 1.0f / 315.0f;
    float terrainCloudSpeed[2]{-0.02f, -0.03f};
};
static_assert(sizeof(WorldConstants) == 480);
static_assert(std::is_standard_layout_v<WorldConstants>);
static_assert(offsetof(WorldConstants, receivesShadow) == 208);
static_assert(offsetof(WorldConstants, receivesVisibility) == 212);
static_assert(offsetof(WorldConstants, receivesDynamicPointLights) == 216);
static_assert(offsetof(WorldConstants, texturingEnabled) == 220);
static_assert(offsetof(WorldConstants, mapperScaleOffset) == 224);
static_assert(offsetof(WorldConstants, mapperTimeSeconds) == 304);
static_assert(offsetof(WorldConstants, objectDynamicAmbient) == 320);
static_assert(offsetof(WorldConstants, objectDynamicDiffuseCount) == 332);
static_assert(offsetof(WorldConstants, objectDynamicPositionInner) == 336);
static_assert(offsetof(WorldConstants, objectDynamicColorOuter) == 400);
static_assert(offsetof(WorldConstants, terrainMacroFlags) == 464);
static_assert(offsetof(WorldConstants, terrainMacroScale) == 468);
static_assert(offsetof(WorldConstants, terrainCloudSpeed) == 472);

[[nodiscard]] bool sameVec3(const math::vec3& lhs, const math::vec3& rhs) noexcept {
    return lhs.x() == rhs.x() && lhs.y() == rhs.y() && lhs.z() == rhs.z();
}

[[nodiscard]] bool sameVec4(const math::vec4& lhs, const math::vec4& rhs) noexcept {
    return lhs.x() == rhs.x() && lhs.y() == rhs.y() &&
           lhs.z() == rhs.z() && lhs.w() == rhs.w();
}

[[nodiscard]] bool canInstanceTogether(const StaticMeshDrawPacket& lhs,
                                       const StaticMeshDrawPacket& rhs) noexcept {
    // Transparent instances need strict back-to-front primitive ordering;
    // skinned instances need independent palette addressing. Both remain
    // one-packet batches until their dedicated GPU representations exist.
    if (lhs.blendMode != StaticMeshBlendMode::Opaque ||
        rhs.blendMode != StaticMeshBlendMode::Opaque ||
        lhs.skinBoneCount != 0 || rhs.skinBoneCount != 0) {
        return false;
    }
    return lhs.vertexBuffer.BufferLocation == rhs.vertexBuffer.BufferLocation &&
        lhs.vertexBuffer.SizeInBytes == rhs.vertexBuffer.SizeInBytes &&
        lhs.vertexBuffer.StrideInBytes == rhs.vertexBuffer.StrideInBytes &&
        lhs.indexBuffer.BufferLocation == rhs.indexBuffer.BufferLocation &&
        lhs.indexBuffer.SizeInBytes == rhs.indexBuffer.SizeInBytes &&
        lhs.indexBuffer.Format == rhs.indexBuffer.Format &&
        lhs.textureSrv.ptr == rhs.textureSrv.ptr &&
        lhs.detailTextureSrv.ptr == rhs.detailTextureSrv.ptr &&
        lhs.terrainCloudTextureSrv.ptr == rhs.terrainCloudTextureSrv.ptr &&
        lhs.terrainMacroTextureSrv.ptr == rhs.terrainMacroTextureSrv.ptr &&
        lhs.terrainMacroFlags == rhs.terrainMacroFlags &&
        lhs.textureMappers == rhs.textureMappers &&
        lhs.visualTimeSeconds == rhs.visualTimeSeconds &&
        sameVec4(lhs.diffuse, rhs.diffuse) &&
        sameVec3(lhs.ambient, rhs.ambient) &&
        sameVec3(lhs.specular, rhs.specular) &&
        sameVec3(lhs.emissive, rhs.emissive) &&
        lhs.shininess == rhs.shininess &&
        lhs.lightingEnabled == rhs.lightingEnabled &&
        lhs.primaryGradientDisabled == rhs.primaryGradientDisabled &&
        lhs.texturingEnabled == rhs.texturingEnabled &&
        lhs.lightmapPass == rhs.lightmapPass &&
        lhs.twoSided == rhs.twoSided &&
        lhs.alphaTestMode == rhs.alphaTestMode &&
        lhs.terrainEdgePhase == rhs.terrainEdgePhase &&
        lhs.samplerMode == rhs.samplerMode &&
        lhs.detailSamplerMode == rhs.detailSamplerMode &&
        lhs.detailColorFunc == rhs.detailColorFunc &&
        lhs.detailAlphaFunc == rhs.detailAlphaFunc &&
        lhs.fogFunc == rhs.fogFunc &&
        lhs.hasDetailTexture == rhs.hasDetailTexture &&
        lhs.depthWrite == rhs.depthWrite &&
        lhs.depthCompare == rhs.depthCompare &&
        lhs.blendMode == rhs.blendMode &&
        lhs.waterSurface == rhs.waterSurface &&
        lhs.receivesShadow == rhs.receivesShadow &&
        lhs.receivesVisibility == rhs.receivesVisibility &&
        lhs.receivesMapBorder == rhs.receivesMapBorder &&
        lhs.fadesMapBorder == rhs.fadesMapBorder &&
        lhs.receivesDynamicLights == rhs.receivesDynamicLights &&
        lhs.receivesScenePointLights == rhs.receivesScenePointLights &&
        lhs.dynamicLightReceiver == rhs.dynamicLightReceiver &&
        lhs.worldLayer == rhs.worldLayer &&
        lhs.materialPass == rhs.materialPass &&
        lhs.firstIndex == rhs.firstIndex &&
        lhs.indexCount == rhs.indexCount &&
        lhs.baseVertex == rhs.baseVertex;
}

} // namespace

bool staticMeshDrawOrderLess(const StaticMeshDrawPacket& lhs,
                             const StaticMeshDrawPacket& rhs,
                             math::vec3 cameraPosition) noexcept {
    if (lhs.worldLayer != rhs.worldLayer) {
        return lhs.worldLayer < rhs.worldLayer;
    }
    const bool lhsOpaque = lhs.blendMode == StaticMeshBlendMode::Opaque;
    const bool rhsOpaque = rhs.blendMode == StaticMeshBlendMode::Opaque;
    if (lhsOpaque != rhsOpaque) return lhsOpaque;
    // W3DRoadBuffer promotes a ROAD_JOIN owner above the cross-material road
    // it fades into.  Camera-distance sorting must not reverse that authored
    // stacking order inside the fixed Roads layer.
    if (lhs.worldLayer == StaticMeshWorldLayer::Roads &&
        lhs.materialPass != rhs.materialPass) {
        return lhs.materialPass < rhs.materialPass;
    }
    // PRELIT_LIGHTMAP_MULTI_PASS is rendered in authored pass order across
    // the category in RefCode. The final baked-light overlay must not move in
    // front of an earlier transparent base pass merely because its packet
    // center is farther from the camera.
    //
    // This rule must stay expressible as a per-packet key.  The earlier form,
    // `(lhs.lightmapPass || rhs.lightmapPass) && materialPass differs`, was
    // pair-dependent and therefore not a strict weak ordering: a lightmap
    // packet could compare by materialPass against two non-lightmap packets
    // that compare against each other by distance, forming the cycle
    // A(lm,pass=1) < C(pass=2) < B(pass=0) < A.  Feeding that to std::stable_sort
    // is undefined behavior.  Ordering the lightmap group as a whole after the
    // non-lightmap group, then by authored pass inside it, keeps the overlay
    // behind nothing it should precede while remaining transitive.
    if (lhs.lightmapPass != rhs.lightmapPass) return !lhs.lightmapPass;
    if (lhs.lightmapPass && lhs.materialPass != rhs.materialPass) {
        return lhs.materialPass < rhs.materialPass;
    }
    if (lhsOpaque) {
        const auto stateKey = [](const StaticMeshDrawPacket& draw) {
            return std::tuple{
                draw.materialPass,
                static_cast<uint8_t>(draw.blendMode),
                static_cast<uint8_t>(draw.depthCompare),
                draw.depthWrite,
                draw.twoSided,
                draw.vertexBuffer.BufferLocation,
                draw.indexBuffer.BufferLocation,
                draw.firstIndex,
                draw.indexCount,
                draw.baseVertex,
                draw.textureSrv.ptr,
                draw.detailTextureSrv.ptr,
            };
        };
        return stateKey(lhs) < stateKey(rhs);
    }
    // SORT_LEVEL_NONE participates in ordinary camera/triangle sorting and
    // is submitted before the deferred static lists. Positive static levels
    // are then rendered high-to-low; lower levels have higher foreground
    // priority and therefore render later. Preserve source packet order
    // inside one explicit level, as DefaultStaticSortListClass does.
    const bool lhsStaticSort = lhs.sortLevel > 0;
    const bool rhsStaticSort = rhs.sortLevel > 0;
    if (lhsStaticSort != rhsStaticSort) return !lhsStaticSort;
    if (lhsStaticSort) {
        if (lhs.sortLevel != rhs.sortLevel) {
            return lhs.sortLevel > rhs.sortLevel;
        }
        return false;
    }
    const float rawLhsDistance =
        (lhs.resolvedSortCenter() - cameraPosition).length_sq();
    const float rawRhsDistance =
        (rhs.resolvedSortCenter() - cameraPosition).length_sq();
    const float lhsDistance = std::isfinite(rawLhsDistance)
        ? rawLhsDistance : -1.0f;
    const float rhsDistance = std::isfinite(rawRhsDistance)
        ? rawRhsDistance : -1.0f;
    if (lhsDistance != rhsDistance) return lhsDistance > rhsDistance;
    return lhs.materialPass < rhs.materialPass;
}

void WorldRenderer::renderStaticMeshes(container::Span<const StaticMeshDrawPacket> drawPackets,
                                        const RenderCameraSnapshot& cameraSnapshot,
                                        const WorldLightEnvironment& lightEnvironment,
                                        const LocalVisibilityRenderSnapshot& localVisibility,
                                        float worldTimeSeconds,
                                        container::Span<const DynamicPointLightRenderData> dynamicPointLights,
                                        container::Span<const TerrainPointLightRenderData> scenePointLights,
                                        StaticMeshPassExecution execution) {
    const bool splitBeforeProjectors =
        execution == StaticMeshPassExecution::FullWorldBeforeProjectors;
    const bool splitAfterProjectors =
        execution == StaticMeshPassExecution::FullWorldAfterProjectors;
    const bool fullWorldPass =
        execution == StaticMeshPassExecution::FullWorld ||
        splitBeforeProjectors;
    const bool mainWorldPass = fullWorldPass || splitAfterProjectors;
    const bool finalMainWorldPass =
        execution == StaticMeshPassExecution::FullWorld ||
        splitAfterProjectors;
    const bool reflectionPass = execution == StaticMeshPassExecution::Reflection;
    const bool waterReflectionAvailable = mainWorldPass &&
        m_waterReflectionValid && m_waterReflectionSrv != UINT32_MAX;
    if ((mainWorldPass || reflectionPass) && m_uploadOwner) {
        m_uploadOwner->resetPaletteEntries();
    }
    // A split pre phase must preserve the freshly captured reflection until
    // the post phase reaches the Water layer. Single-pass FullWorld and the
    // split post phase consume it exactly once.
    if (finalMainWorldPass) m_waterReflectionValid = false;
    if (fullWorldPass) {
        m_lastStaticMeshStats = {};
        m_lastStaticMeshStats.shadowAvailable = m_directionalShadowAvailable;
        if (m_uploadOwner) {
            m_uploadOwner->projectCapacities(m_lastStaticMeshStats);
        }
    }
    if (!splitAfterProjectors) {
        m_lastStaticMeshStats.submittedPackets += static_cast<uint32_t>(
            std::min<size_t>(
                drawPackets.size(),
                std::numeric_limits<uint32_t>::max() -
                    m_lastStaticMeshStats.submittedPackets));
    }
    if (!m_initialized || !m_device ||
        m_device->width() == 0 || m_device->height() == 0) {
        return;
    }

    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    if (!commandList) return;

    // The device owns the current RTV/DSV and has already cleared/bound both
    // in beginFrame(). Flushing protects this raw pass if a UI batch preceded it.
    m_device->flushBatch();
    const bool visibilityAuthorityActive = localVisibility.isValid();
    if ((fullWorldPass || reflectionPass) &&
        !updateLocalVisibilityTexture(localVisibility)) {
        TD_LOG_WARN(
            "[WorldRenderer] Local visibility texture update failed; using fail-closed shroud fallback for this frame");
    }
    if (fullWorldPass) {
        m_lastStaticMeshStats.visibilityRevision =
            localVisibility.isValid() ? localVisibility.revision : 0;
        m_lastStaticMeshStats.visibilityTextureWidth = m_localVisibilityWidth;
        m_lastStaticMeshStats.visibilityTextureHeight = m_localVisibilityHeight;
        m_lastStaticMeshStats.visibilityEnabled = visibilityAuthorityActive;
        m_lastStaticMeshStats.visibilityFallbackBound =
            !m_localVisibilityEnabled || m_localVisibilitySrv == UINT32_MAX;
    }
    if (drawPackets.empty()) return;
    // Directional depth must be complete before any terrain/object receiver
    // samples t2. The shadow pass restores the main RTV/DSV and shader-read
    // state before returning, including partial upload-failure paths.
    if (fullWorldPass) {
        renderDirectionalShadowMap(drawPackets, cameraSnapshot,
                                   lightEnvironment);
    }

    // A WorldRenderer has no mutable game camera.  The prepared frame owns
    // this complete value snapshot, so target/up/FOV/clip planes cannot race
    // with logic while command recording is in progress.
    const WorldCamera frameCamera = WorldCamera::fromSnapshot(cameraSnapshot);
    const float aspectRatio = reflectionPass ? 1.0f
        : static_cast<float>(m_device->width()) /
              static_cast<float>(m_device->height());
    const math::float4x4 viewProjection = frameCamera.viewProjectionMatrix(aspectRatio);

    const bool skeletonMode = m_skeletonMode;
    // Geometry-only skeleton output wins over texture isolation so F1's
    // contract remains unchanged even if F3 was enabled beforehand.
    const bool textureOnlyMode = !skeletonMode && m_textureOnlyMode;
    const auto pipelineFor = [this, reflectionPass](const StaticMeshDrawPacket& draw) {
        const size_t index = std::min(static_cast<size_t>(draw.blendMode),
                                      m_pipelineStates.size() - 1);
        const size_t depthWrite = draw.depthWrite ? 1 : 0;
        const size_t depthCompare = std::min(static_cast<size_t>(draw.depthCompare),
                                             world_renderer_detail::
                                                 kDepthCompareFunctions.size() - 1);
        const auto& pipelines = m_pipelineStates[index][depthWrite][depthCompare];
        if (draw.twoSided) return pipelines.twoSided.Get();
        return reflectionPass ? pipelines.frontFaceCulled.Get()
                              : pipelines.backFaceCulled.Get();
    };

    auto& orderedDraws = m_orderedDrawScratch;
    orderedDraws.clear();
    orderedDraws.reserve(drawPackets.size());
    const auto validPacket = [this, &drawPackets](const StaticMeshDrawPacket& draw) {
        const size_t packetIndex = static_cast<size_t>(&draw - drawPackets.data());
        if (draw.indexCount == 0) return false;
        if (draw.vertexBuffer.BufferLocation == 0 || draw.vertexBuffer.SizeInBytes == 0 ||
            draw.vertexBuffer.StrideInBytes != sizeof(StaticMeshVertex)) {
            TD_LOG_WARN("[WorldRenderer] Skipping static draw {} with an invalid vertex view",
                        packetIndex);
            return false;
        }
        if (draw.indexBuffer.BufferLocation == 0 || draw.indexBuffer.SizeInBytes == 0 ||
            draw.indexBuffer.Format != DXGI_FORMAT_R32_UINT) {
            TD_LOG_WARN("[WorldRenderer] Skipping static draw {}: R32 index view required",
                        packetIndex);
            return false;
        }
        const uint64_t indexEnd = static_cast<uint64_t>(draw.firstIndex) + draw.indexCount;
        if (indexEnd * sizeof(uint32_t) > draw.indexBuffer.SizeInBytes) {
            TD_LOG_WARN("[WorldRenderer] Skipping static draw {} outside its index buffer",
                        packetIndex);
            return false;
        }
        if (draw.skinBoneCount != 0 &&
            (!draw.skinPalette || draw.skinBoneCount >
                 world_renderer_detail::kMaximumSkinBones)) {
            TD_LOG_WARN("[WorldRenderer] Skipping static draw {} with invalid skin palette",
                        packetIndex);
            return false;
        }
        return true;
    };
    const auto belongsToCurrentSegment = [splitBeforeProjectors,
                                          splitAfterProjectors](
                                             const StaticMeshDrawPacket& draw) {
        if (splitBeforeProjectors) {
            return draw.worldLayer <= StaticMeshWorldLayer::Roads;
        }
        if (splitAfterProjectors) {
            return draw.worldLayer >= StaticMeshWorldLayer::Bridges;
        }
        return true;
    };
    for (const StaticMeshDrawPacket& draw : drawPackets) {
        if (!belongsToCurrentSegment(draw)) continue;
        if (!validPacket(draw)) {
            ++m_lastStaticMeshStats.skippedPackets;
            continue;
        }
        ++m_lastStaticMeshStats.validPackets;
        orderedDraws.push_back(&draw);
    }
    if (orderedDraws.empty()) {
        if (m_uploadOwner) {
            m_uploadOwner->resetPaletteEntries();
        }
        return;
    }
    const math::vec3 cameraPosition = frameCamera.position();
    std::stable_sort(
        orderedDraws.begin(), orderedDraws.end(),
        [&cameraPosition](const StaticMeshDrawPacket* lhs,
                          const StaticMeshDrawPacket* rhs) {
            return staticMeshDrawOrderLess(*lhs, *rhs, cameraPosition);
        });

    CameraConstants cameraConstants{};
    std::memcpy(cameraConstants.viewProjection, &viewProjection.m,
                sizeof(cameraConstants.viewProjection));
    cameraConstants.cameraPosition[0] = frameCamera.position().x();
    cameraConstants.cameraPosition[1] = frameCamera.position().y();
    cameraConstants.cameraPosition[2] = frameCamera.position().z();
    math::vec3 cameraForward = frameCamera.target() - frameCamera.position();
    if (cameraForward.length_sq() <= math::EPSILON * math::EPSILON) {
        cameraForward = {0.0f, 1.0f, 0.0f};
    } else {
        cameraForward = cameraForward.normalized();
    }
    math::vec3 cameraUp = frameCamera.up();
    if (cameraUp.length_sq() <= math::EPSILON * math::EPSILON) {
        cameraUp = WorldCamera::worldUp();
    } else {
        cameraUp = cameraUp.normalized();
    }
    math::vec3 cameraRight = cameraForward.cross(cameraUp);
    if (cameraRight.length_sq() <= math::EPSILON * math::EPSILON) {
        cameraUp = std::abs(cameraForward.z()) < 0.999f
            ? WorldCamera::worldUp() : math::vec3{0.0f, 1.0f, 0.0f};
        cameraRight = cameraForward.cross(cameraUp);
    }
    cameraRight = cameraRight.normalized();
    cameraUp = cameraRight.cross(cameraForward).normalized();
    cameraConstants.cameraRight[0] = cameraRight.x();
    cameraConstants.cameraRight[1] = cameraRight.y();
    cameraConstants.cameraRight[2] = cameraRight.z();
    cameraConstants.cameraUp[0] = cameraUp.x();
    cameraConstants.cameraUp[1] = cameraUp.y();
    cameraConstants.cameraUp[2] = cameraUp.z();
    cameraConstants.worldTimeSeconds = std::isfinite(worldTimeSeconds)
        ? worldTimeSeconds : 0.0f;
    cameraConstants.fogColor[0] = cameraSnapshot.fogColor.x();
    cameraConstants.fogColor[1] = cameraSnapshot.fogColor.y();
    cameraConstants.fogColor[2] = cameraSnapshot.fogColor.z();
    cameraConstants.fogEnabled = cameraSnapshot.fogEnabled ? 1.0f : 0.0f;
    cameraConstants.fogStartDistance = cameraSnapshot.fogStartDistance;
    cameraConstants.fogEndDistance = std::max(cameraSnapshot.fogEndDistance,
                                               cameraSnapshot.fogStartDistance + 0.0001f);
    if (localVisibility.playableBoundsEnabled) {
        cameraConstants.playableMinimum[0] =
            localVisibility.playableMinimum.x();
        cameraConstants.playableMinimum[1] =
            localVisibility.playableMinimum.y();
        cameraConstants.playableMaximum[0] =
            localVisibility.playableMaximum.x();
        cameraConstants.playableMaximum[1] =
            localVisibility.playableMaximum.y();
        cameraConstants.playableBoundsEnabled = 1u;
        cameraConstants.playableBorderFadeWidth =
            std::isfinite(localVisibility.cellWorldSize)
            ? std::max(localVisibility.cellWorldSize, 0.0f)
            : 0.0f;
        cameraConstants.borderShroudEnabled =
            localVisibility.borderShroudEnabled ? 1u : 0u;
    }
    cameraConstants.sceneAmbient[0] = lightEnvironment.ambient.x();
    cameraConstants.sceneAmbient[1] = lightEnvironment.ambient.y();
    cameraConstants.sceneAmbient[2] = lightEnvironment.ambient.z();
    for (size_t lightIndex = 0; lightIndex < lightEnvironment.directionalLights.size(); ++lightIndex) {
        const WorldDirectionalLight& light = lightEnvironment.directionalLights[lightIndex];
        cameraConstants.directionalToLight[lightIndex][0] = light.directionToLight.x();
        cameraConstants.directionalToLight[lightIndex][1] = light.directionToLight.y();
        cameraConstants.directionalToLight[lightIndex][2] = light.directionToLight.z();
        cameraConstants.directionalDiffuse[lightIndex][0] = light.diffuse.x();
        cameraConstants.directionalDiffuse[lightIndex][1] = light.diffuse.y();
        cameraConstants.directionalDiffuse[lightIndex][2] = light.diffuse.z();
    }
    for (const DynamicPointLightRenderData& light : dynamicPointLights) {
        if (cameraConstants.dynamicPointLightCount >= kMaximumWorldDynamicPointLights) break;
        if (!std::isfinite(light.position.x()) || !std::isfinite(light.position.y()) ||
            !std::isfinite(light.position.z()) || !std::isfinite(light.color.x()) ||
            !std::isfinite(light.color.y()) || !std::isfinite(light.color.z()) ||
            !std::isfinite(light.outerRadius) || light.outerRadius <= 0.0f) {
            continue;
        }
        const float innerRadius = std::clamp(
            std::isfinite(light.innerRadius) ? light.innerRadius : 0.0f,
            0.0f, light.outerRadius);
        const float red = std::max(light.color.x(), 0.0f);
        const float green = std::max(light.color.y(), 0.0f);
        const float blue = std::max(light.color.z(), 0.0f);
        const math::vec3 ambient = light.hasSeparateAmbientColor
            ? light.ambientColor : light.color;
        const float ambientRed = std::max(ambient.x(), 0.0f);
        const float ambientGreen = std::max(ambient.y(), 0.0f);
        const float ambientBlue = std::max(ambient.z(), 0.0f);
        if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue) ||
            !std::isfinite(ambientRed) || !std::isfinite(ambientGreen) ||
            !std::isfinite(ambientBlue) ||
            (red <= 0.0f && green <= 0.0f && blue <= 0.0f)) {
            continue;
        }
        const uint32_t pointIndex = cameraConstants.dynamicPointLightCount++;
        cameraConstants.dynamicPointPositionInner[pointIndex][0] = light.position.x();
        cameraConstants.dynamicPointPositionInner[pointIndex][1] = light.position.y();
        cameraConstants.dynamicPointPositionInner[pointIndex][2] = light.position.z();
        cameraConstants.dynamicPointPositionInner[pointIndex][3] = innerRadius;
        cameraConstants.dynamicPointColorOuter[pointIndex][0] = red;
        cameraConstants.dynamicPointColorOuter[pointIndex][1] = green;
        cameraConstants.dynamicPointColorOuter[pointIndex][2] = blue;
        cameraConstants.dynamicPointColorOuter[pointIndex][3] = light.outerRadius;
        cameraConstants.dynamicPointAmbient[pointIndex][0] = ambientRed;
        cameraConstants.dynamicPointAmbient[pointIndex][1] = ambientGreen;
        cameraConstants.dynamicPointAmbient[pointIndex][2] = ambientBlue;
    }
    if (fullWorldPass) {
        m_lastStaticMeshStats.dynamicPointLights =
            cameraConstants.dynamicPointLightCount;
        m_lastStaticMeshStats.dynamicLightReceivingPackets =
            static_cast<uint32_t>(std::min<size_t>(
                std::count_if(
                    drawPackets.begin(), drawPackets.end(),
                    [](const StaticMeshDrawPacket& packet) {
                        return packet.receivesDynamicLights &&
                            !packet.waterSurface;
                    }),
                std::numeric_limits<uint32_t>::max()));
        m_lastStaticMeshStats.scenePointLights = static_cast<uint32_t>(
            std::min<size_t>(scenePointLights.size(),
                             std::numeric_limits<uint32_t>::max()));
        m_lastStaticMeshStats.sceneLightReceivingPackets =
            static_cast<uint32_t>(std::min<size_t>(
                std::count_if(
                    drawPackets.begin(), drawPackets.end(),
                    [](const StaticMeshDrawPacket& packet) {
                        return packet.receivesScenePointLights &&
                            !packet.waterSurface;
                    }),
                std::numeric_limits<uint32_t>::max()));
    }
    cameraConstants.directionalToLight[0][3] = std::isfinite(worldTimeSeconds)
        ? worldTimeSeconds : 0.0f;
    std::memcpy(cameraConstants.shadowViewProjection,
                &m_shadowViewProjection.m,
                sizeof(cameraConstants.shadowViewProjection));
    cameraConstants.shadowTexelSize[0] =
        1.0f / static_cast<float>(world_renderer_shadow::kMapSize);
    cameraConstants.shadowTexelSize[1] =
        1.0f / static_cast<float>(world_renderer_shadow::kMapSize);
    cameraConstants.shadowDepthBias = world_renderer_shadow::kDepthBias;
    cameraConstants.shadowStrength =
        !skeletonMode && m_directionalShadowValid
        ? world_renderer_shadow::kStrength : 0.0f;
    std::memcpy(cameraConstants.waterReflectionViewProjection,
                &m_waterReflectionViewProjection.m,
                sizeof(cameraConstants.waterReflectionViewProjection));
    cameraConstants.waterReflectionEnabled =
        !reflectionPass && !skeletonMode && waterReflectionAvailable
            ? 1u : 0u;
    if (!skeletonMode && visibilityAuthorityActive) {
        cameraConstants.visibilityOrigin[0] = localVisibility.originX;
        cameraConstants.visibilityOrigin[1] = localVisibility.originY;
        cameraConstants.visibilityInvCellSize = 1.0f / localVisibility.cellWorldSize;
        cameraConstants.visibilityEnabled = 1.0f;
        cameraConstants.visibilityTextureSize[0] =
            static_cast<float>(localVisibility.width);
        cameraConstants.visibilityTextureSize[1] =
            static_cast<float>(localVisibility.height);
    }

    const auto cameraAllocation = m_device->allocateConstantBuffer(
        &cameraConstants, sizeof(cameraConstants));
    if (!cameraAllocation) {
        TD_LOG_WARN("[WorldRenderer] Frame upload unavailable; skipping static draws");
        m_lastStaticMeshStats.skippedPackets += static_cast<uint32_t>(
            std::min<size_t>(
                orderedDraws.size(),
                std::numeric_limits<uint32_t>::max() -
                    m_lastStaticMeshStats.skippedPackets));
        m_orderedDrawScratch.clear();
        if (m_uploadOwner) m_uploadOwner->resetPaletteEntries();
        return;
    }

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(reflectionPass
        ? kWaterReflectionSize : m_device->width());
    const uint32_t tacticalHeight = reflectionPass
        ? kWaterReflectionSize
        : frameCamera.tacticalViewportHeight(m_device->height());
    viewport.Height = static_cast<float>(tacticalHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(reflectionPass
                                 ? kWaterReflectionSize : m_device->width()),
                             static_cast<LONG>(tacticalHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    m_device->bindSrvHeap();
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetGraphicsRootConstantBufferView(0, cameraAllocation.gpuAddress);
    const uint32_t shadowSrv =
        m_directionalShadowValid && m_shadowMapSrv != UINT32_MAX
            ? m_shadowMapSrv
            : (m_shadowFallbackSrv != UINT32_MAX ? m_shadowFallbackSrv : 0u);
    m_lastStaticMeshStats.shadowValid = m_directionalShadowValid;
    m_lastStaticMeshStats.shadowFallbackBound =
        !m_directionalShadowValid || shadowSrv != m_shadowMapSrv;
    const D3D12_GPU_DESCRIPTOR_HANDLE shadowGpuHandle =
        m_device->getSrvGpuHandle(shadowSrv);
    commandList->SetGraphicsRootDescriptorTable(
        5, shadowGpuHandle);
    m_device->recordGraphicsDescriptorTableCall();
    const uint32_t visibilitySrv =
        m_localVisibilityEnabled && m_localVisibilitySrv != UINT32_MAX
            ? m_localVisibilitySrv
            : (m_localVisibilityFallbackSrv != UINT32_MAX
                   ? m_localVisibilityFallbackSrv : 0u);
    const D3D12_GPU_DESCRIPTOR_HANDLE visibilityGpuHandle =
        m_device->getSrvGpuHandle(visibilitySrv);
    commandList->SetGraphicsRootDescriptorTable(
        6, visibilityGpuHandle);
    m_device->recordGraphicsDescriptorTableCall();
    const uint32_t waterReflectionSrv =
        cameraConstants.waterReflectionEnabled != 0u
            ? m_waterReflectionSrv : 0u;
    const D3D12_GPU_DESCRIPTOR_HANDLE waterReflectionGpuHandle =
        m_device->getSrvGpuHandle(waterReflectionSrv);
    commandList->SetGraphicsRootDescriptorTable(
        7, waterReflectionGpuHandle);
    m_device->recordGraphicsDescriptorTableCall();
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (!m_uploadOwner) {
        m_uploadOwner = std::make_unique<WorldRendererUploadOwner>();
    }
    auto skinPaletteUploads = m_uploadOwner->beginPaletteUploads(
        *m_device, orderedDraws.size(), m_lastStaticMeshStats);
    container::Vector<StaticMeshGpuInstance>& gpuInstances =
        m_uploadOwner->instances();
    container::Vector<StaticMeshRecordedDraw>& recordedDraws =
        m_uploadOwner->recordedDraws();
    recordedDraws.clear();
    recordedDraws.reserve(orderedDraws.size());
    constexpr size_t kMaximumInstancesPerUpload =
        std::numeric_limits<uint32_t>::max() / sizeof(StaticMeshGpuInstance);

    size_t packetIndex = 0;
    while (packetIndex < orderedDraws.size()) {
        const StaticMeshDrawPacket& draw = *orderedDraws[packetIndex];
        size_t batchEnd = packetIndex + 1;
        const StaticMeshDynamicLightReceiver dynamicReceiver =
            draw.dynamicLightReceiver != StaticMeshDynamicLightReceiver::None
                ? draw.dynamicLightReceiver
                : draw.receivesDynamicLights
                    ? StaticMeshDynamicLightReceiver::Object
                    : StaticMeshDynamicLightReceiver::None;
        const bool requiresPerObjectLightEnvironment =
            dynamicReceiver == StaticMeshDynamicLightReceiver::Object &&
            ((!dynamicPointLights.empty() && draw.receivesDynamicLights) ||
             (!scenePointLights.empty() && draw.receivesScenePointLights));
        while (batchEnd < orderedDraws.size() &&
               batchEnd - packetIndex < kMaximumInstancesPerUpload &&
               !requiresPerObjectLightEnvironment &&
               canInstanceTogether(draw, *orderedDraws[batchEnd])) {
            ++batchEnd;
        }

        WorldConstants worldConstants{};
        std::memcpy(worldConstants.world, &draw.worldTransform.m,
                    sizeof(worldConstants.world));
        const math::vec4 diffuse = skeletonMode
            ? math::vec4{0.20f, 1.00f, 0.36f, 1.00f}
            : draw.diffuse;
        const math::vec3 ambient = skeletonMode
            ? math::vec3{1.0f, 1.0f, 1.0f}
            : draw.ambient;
        worldConstants.diffuse[0] = diffuse.x();
        worldConstants.diffuse[1] = diffuse.y();
        worldConstants.diffuse[2] = diffuse.z();
        worldConstants.diffuse[3] = diffuse.w();
        worldConstants.ambient[0] = ambient.x();
        worldConstants.ambient[1] = ambient.y();
        worldConstants.ambient[2] = ambient.z();
        worldConstants.shininess = draw.shininess;
        worldConstants.specular[0] = draw.specular.x();
        worldConstants.specular[1] = draw.specular.y();
        worldConstants.specular[2] = draw.specular.z();
        worldConstants.alphaCutoff = draw.alphaTestMode == StaticMeshAlphaTestMode::LessEqual
            ? (0xff - 0x60) / 255.0f
            : world_renderer_detail::kW3dAlphaTestCutoff;
        worldConstants.emissive[0] = draw.emissive.x();
        worldConstants.emissive[1] = draw.emissive.y();
        worldConstants.emissive[2] = draw.emissive.z();
        worldConstants.lightingEnabled = !skeletonMode && draw.lightingEnabled ? 1u : 0u;
        worldConstants.waterSurface = !skeletonMode && draw.waterSurface ? 1u : 0u;
        worldConstants.terrainEdgePhase = skeletonMode ? 0u : std::min<uint32_t>(
            static_cast<uint32_t>(draw.terrainEdgePhase),
            static_cast<uint32_t>(StaticMeshTerrainEdgePhase::EdgeRgb));
        worldConstants.directionalLightScale =
            std::isfinite(draw.directionalLightScale) && draw.directionalLightScale > 0.0f
                ? draw.directionalLightScale : 1.0f;
        worldConstants.scriptFlashTint[0] = std::isfinite(draw.scriptFlashTint.x())
            ? draw.scriptFlashTint.x() : 0.0f;
        worldConstants.scriptFlashTint[1] = std::isfinite(draw.scriptFlashTint.y())
            ? draw.scriptFlashTint.y() : 0.0f;
        worldConstants.scriptFlashTint[2] = std::isfinite(draw.scriptFlashTint.z())
            ? draw.scriptFlashTint.z() : 0.0f;
        worldConstants.scriptIndicatorColor[0] = std::isfinite(draw.scriptIndicatorColor.x())
            ? std::clamp(draw.scriptIndicatorColor.x(), 0.0f, 1.0f) : 0.0f;
        worldConstants.scriptIndicatorColor[1] = std::isfinite(draw.scriptIndicatorColor.y())
            ? std::clamp(draw.scriptIndicatorColor.y(), 0.0f, 1.0f) : 0.0f;
        worldConstants.scriptIndicatorColor[2] = std::isfinite(draw.scriptIndicatorColor.z())
            ? std::clamp(draw.scriptIndicatorColor.z(), 0.0f, 1.0f) : 0.0f;
        if (!skeletonMode && draw.hasScriptIndicatorColor) {
            worldConstants.houseColorFlags =
                (draw.houseColorVertexMaterial ? 1u : 0u) |
                (draw.houseColorTexture ? 2u : 0u) |
                (draw.houseColorInverseAlphaMask ? 4u : 0u);
        }
        worldConstants.skinBoneCount = draw.skinBoneCount;
        worldConstants.samplerMode = std::min<uint32_t>(draw.samplerMode, 3);
        worldConstants.alphaTestMode = skeletonMode
            ? static_cast<uint32_t>(StaticMeshAlphaTestMode::Disabled)
            : static_cast<uint32_t>(draw.alphaTestMode);
        worldConstants.detailSamplerMode = std::min<uint32_t>(draw.detailSamplerMode, 3);
        worldConstants.hasDetailTexture = !skeletonMode && draw.hasDetailTexture ? 1 : 0;
        worldConstants.detailColorFunc = std::min<uint32_t>(draw.detailColorFunc, 12);
        worldConstants.detailAlphaFunc = std::min<uint32_t>(draw.detailAlphaFunc, 3);
        worldConstants.fogFunc = skeletonMode ? 0 : std::min<uint32_t>(draw.fogFunc, 3);
        worldConstants.texturingEnabled =
            !skeletonMode && draw.texturingEnabled ? 1u : 0u;
        worldConstants.ignoreVertexColor = skeletonMode ? 1u
            : textureOnlyMode ? 2u
            : draw.primaryGradientDisabled ? 3u : 0u;
        worldConstants.receivesShadow =
            !skeletonMode && m_directionalShadowValid && draw.receivesShadow
                ? (draw.lightingEnabled ? 1u : 2u)
                : 0u;
        // 1/2 retain strict object/effect clipping (with/without fog). 3/4
        // retain terrain-like exterior geometry and fade it into the border
        // shroud (with/without fog). 0 is an unmasked overlay/background.
        const bool receivesFog = !skeletonMode &&
            visibilityAuthorityActive && draw.receivesVisibility;
        const bool receivesBorder = !skeletonMode &&
            localVisibility.playableBoundsEnabled && draw.receivesMapBorder;
        worldConstants.receivesVisibility = receivesFog
            ? (receivesBorder && draw.fadesMapBorder ? 3u : 1u)
            : receivesBorder
                ? (draw.fadesMapBorder ? 4u : 2u)
                : 0u;
        const bool receivesObjectPointLights = !skeletonMode &&
            !draw.waterSurface &&
            dynamicReceiver == StaticMeshDynamicLightReceiver::Object &&
            ((draw.receivesDynamicLights && !dynamicPointLights.empty()) ||
             (draw.receivesScenePointLights && !scenePointLights.empty()));
        worldConstants.receivesDynamicPointLights = receivesObjectPointLights
            ? 1u
            : !skeletonMode && !draw.waterSurface &&
                    draw.receivesDynamicLights &&
                    dynamicReceiver == StaticMeshDynamicLightReceiver::Terrain
                ? 2u : 0u;
        if (worldConstants.receivesDynamicPointLights == 1u) {
            const ObjectDynamicLightEnvironment objectLights =
                selectObjectPointLights(
                    draw.resolvedSortCenter(), dynamicPointLights,
                    scenePointLights, draw.receivesDynamicLights,
                    draw.receivesScenePointLights);
            worldConstants.objectDynamicAmbient[0] =
                objectLights.ambient.x();
            worldConstants.objectDynamicAmbient[1] =
                objectLights.ambient.y();
            worldConstants.objectDynamicAmbient[2] =
                objectLights.ambient.z();
            worldConstants.objectDynamicDiffuseCount =
                objectLights.diffuseLightCount;
            for (uint32_t lightIndex = 0;
                 lightIndex < objectLights.diffuseLightCount;
                 ++lightIndex) {
                const DynamicPointLightRenderData& light =
                    objectLights.diffuseLights[lightIndex];
                worldConstants.objectDynamicPositionInner[lightIndex][0] =
                    light.position.x();
                worldConstants.objectDynamicPositionInner[lightIndex][1] =
                    light.position.y();
                worldConstants.objectDynamicPositionInner[lightIndex][2] =
                    light.position.z();
                worldConstants.objectDynamicPositionInner[lightIndex][3] =
                    std::clamp(light.innerRadius, 0.0f, light.outerRadius);
                worldConstants.objectDynamicColorOuter[lightIndex][0] =
                    light.color.x();
                worldConstants.objectDynamicColorOuter[lightIndex][1] =
                    light.color.y();
                worldConstants.objectDynamicColorOuter[lightIndex][2] =
                    light.color.z();
                worldConstants.objectDynamicColorOuter[lightIndex][3] =
                    light.outerRadius;
            }
        }
        for (size_t stage = 0; stage < draw.textureMappers.size(); ++stage) {
            world_renderer_detail::packTextureMapper(
                              draw.textureMappers[stage],
                              worldConstants.mapperScaleOffset[stage],
                              worldConstants.mapperMotionCenter[stage],
                              worldConstants.mapperTypes[stage],
                              worldConstants.mapperClampFix[stage]);
        }
        worldConstants.mapperTimeSeconds =
            std::isfinite(draw.visualTimeSeconds) && draw.visualTimeSeconds >= 0.0f
                ? draw.visualTimeSeconds : 0.0f;
        worldConstants.terrainMacroFlags =
            !skeletonMode && !textureOnlyMode && !reflectionPass
                ? static_cast<uint32_t>(draw.terrainMacroFlags & 0x3u)
                : 0u;

        const auto worldAllocation = m_device->allocateConstantBuffer(
            &worldConstants, sizeof(worldConstants));
        if (!worldAllocation) {
            TD_LOG_WARN("[WorldRenderer] Frame upload unavailable after {} static packets",
                        packetIndex);
            m_lastStaticMeshStats.skippedPackets += static_cast<uint32_t>(std::min<size_t>(
                orderedDraws.size() - packetIndex, std::numeric_limits<uint32_t>::max()));
            break;
        }

        const d3d12::ConstantBufferAllocation skinAllocation =
            skinPaletteUploads.resolve(draw);
        if (!skinAllocation) {
            TD_LOG_WARN("[WorldRenderer] Frame upload unavailable for skin palette after {} static packets",
                        packetIndex);
            m_lastStaticMeshStats.skippedPackets += static_cast<uint32_t>(std::min<size_t>(
                orderedDraws.size() - packetIndex, std::numeric_limits<uint32_t>::max()));
            break;
        }

        D3D12_INDEX_BUFFER_VIEW drawIndexView = draw.indexBuffer;
        uint32_t drawFirstIndex = draw.firstIndex;
        const uint64_t sortingEnd = static_cast<uint64_t>(draw.firstIndex) +
            draw.indexCount;
        if (!skeletonMode && draw.requiresTriangleSorting &&
            draw.sortingVertices && draw.sortingIndices &&
            draw.sortingVertexCount != 0 &&
            sortingEnd <= draw.sortingIndexCount &&
            draw.indexCount % 3u == 0u) {
            auto& triangles = m_transparentTriangleSortScratch;
            auto& sortedIndices = m_transparentIndexScratch;
            triangles.clear();
            sortedIndices.clear();
            triangles.reserve(draw.indexCount / 3u);
            sortedIndices.reserve(draw.indexCount);
            const auto transformedPosition = [&draw](uint32_t vertexIndex) {
                const StaticMeshVertex& vertex =
                    draw.sortingVertices[vertexIndex];
                if (draw.skinPalette &&
                    vertex.boneIndex < draw.skinBoneCount) {
                    return draw.skinPalette[vertex.boneIndex].transform_point(
                        vertex.position);
                }
                return draw.worldTransform.transform_point(vertex.position);
            };
            bool validSortingGeometry = true;
            for (uint32_t localIndex = 0; localIndex < draw.indexCount;
                 localIndex += 3u) {
                math::vec3 centroid{};
                for (uint32_t corner = 0; corner < 3u; ++corner) {
                    const uint32_t sourceIndex = draw.sortingIndices[
                        draw.firstIndex + localIndex + corner];
                    const int64_t vertexIndex =
                        static_cast<int64_t>(sourceIndex) + draw.baseVertex;
                    if (vertexIndex < 0 ||
                        vertexIndex >= draw.sortingVertexCount) {
                        validSortingGeometry = false;
                        break;
                    }
                    centroid += transformedPosition(
                        static_cast<uint32_t>(vertexIndex));
                }
                if (!validSortingGeometry) break;
                centroid = centroid * (1.0f / 3.0f);
                triangles.push_back({
                    .sourceIndex = localIndex,
                    .depth = (centroid - cameraPosition).dot(cameraForward),
                });
            }
            if (validSortingGeometry) {
                std::stable_sort(
                    triangles.begin(), triangles.end(),
                    [](const TransparentTriangleSortEntry& lhs,
                       const TransparentTriangleSortEntry& rhs) {
                        return lhs.depth > rhs.depth;
                    });
                for (const TransparentTriangleSortEntry& triangle :
                     triangles) {
                    const uint32_t source =
                        draw.firstIndex + triangle.sourceIndex;
                    sortedIndices.push_back(draw.sortingIndices[source]);
                    sortedIndices.push_back(draw.sortingIndices[source + 1u]);
                    sortedIndices.push_back(draw.sortingIndices[source + 2u]);
                }
                const uint32_t sortedBytes = static_cast<uint32_t>(
                    sortedIndices.size() * sizeof(uint32_t));
                const d3d12::FrameUploadAllocation sortedUpload =
                    m_device->allocateFrameUpload(
                        sortedIndices.data(), sortedBytes,
                        alignof(uint32_t));
                if (sortedUpload) {
                    drawIndexView = {
                        .BufferLocation = sortedUpload.gpuAddress,
                        .SizeInBytes = sortedBytes,
                        .Format = DXGI_FORMAT_R32_UINT,
                    };
                    drawFirstIndex = 0;
                }
            }
        }

        gpuInstances.clear();
        const size_t instanceCapacityBefore = gpuInstances.capacity();
        gpuInstances.reserve(batchEnd - packetIndex);
        m_uploadOwner->noteInstanceReserve(
            instanceCapacityBefore, m_lastStaticMeshStats);
        for (size_t instanceIndex = packetIndex; instanceIndex < batchEnd; ++instanceIndex) {
            const StaticMeshDrawPacket& instanceDraw = *orderedDraws[instanceIndex];
            StaticMeshGpuInstance gpuInstance{};
            std::memcpy(gpuInstance.world, &instanceDraw.worldTransform.m,
                        sizeof(gpuInstance.world));
            std::memcpy(
                gpuInstance.previousWorld,
                &instanceDraw.previousWorldTransform.m,
                sizeof(gpuInstance.previousWorld));
            gpuInstance.interpolationAlpha = std::clamp(
                instanceDraw.interpolationAlpha, 0.0f, 1.0f);
            gpuInstance.directionalLightScale =
                std::isfinite(instanceDraw.directionalLightScale) &&
                instanceDraw.directionalLightScale > 0.0f
                    ? instanceDraw.directionalLightScale : 1.0f;
            gpuInstance.scriptFlashTint[0] = std::isfinite(instanceDraw.scriptFlashTint.x())
                ? instanceDraw.scriptFlashTint.x() : 0.0f;
            gpuInstance.scriptFlashTint[1] = std::isfinite(instanceDraw.scriptFlashTint.y())
                ? instanceDraw.scriptFlashTint.y() : 0.0f;
            gpuInstance.scriptFlashTint[2] = std::isfinite(instanceDraw.scriptFlashTint.z())
                ? instanceDraw.scriptFlashTint.z() : 0.0f;
            gpuInstance.scriptIndicatorColor[0] = std::isfinite(instanceDraw.scriptIndicatorColor.x())
                ? std::clamp(instanceDraw.scriptIndicatorColor.x(), 0.0f, 1.0f) : 0.0f;
            gpuInstance.scriptIndicatorColor[1] = std::isfinite(instanceDraw.scriptIndicatorColor.y())
                ? std::clamp(instanceDraw.scriptIndicatorColor.y(), 0.0f, 1.0f) : 0.0f;
            gpuInstance.scriptIndicatorColor[2] = std::isfinite(instanceDraw.scriptIndicatorColor.z())
                ? std::clamp(instanceDraw.scriptIndicatorColor.z(), 0.0f, 1.0f) : 0.0f;
            if (!skeletonMode && instanceDraw.hasScriptIndicatorColor) {
                gpuInstance.houseColorFlags =
                    (instanceDraw.houseColorVertexMaterial ? 1u : 0u) |
                    (instanceDraw.houseColorTexture ? 2u : 0u) |
                    (instanceDraw.houseColorInverseAlphaMask ? 4u : 0u);
            }
            if (!skeletonMode && std::isfinite(instanceDraw.heatVisionIntensity)) {
                gpuInstance.heatVisionIntensity = std::clamp(
                    instanceDraw.heatVisionIntensity, 0.0f, 1.0f);
                gpuInstance.heatVisionMode = gpuInstance.heatVisionIntensity > 0.0f
                    ? (instanceDraw.heatVisionOnly ? 2u : 1u)
                    : 0u;
            }
            gpuInstance.objectOpacity = std::isfinite(instanceDraw.objectOpacity)
                ? std::clamp(instanceDraw.objectOpacity, 0.0f, 1.0f)
                : 1.0f;
            gpuInstance.treePushAsideDirection[0] =
                std::isfinite(instanceDraw.treePushAsideDirection.x())
                ? instanceDraw.treePushAsideDirection.x() : 0.0f;
            gpuInstance.treePushAsideDirection[1] =
                std::isfinite(instanceDraw.treePushAsideDirection.y())
                ? instanceDraw.treePushAsideDirection.y() : 0.0f;
            gpuInstance.treePushAsideAmount =
                std::isfinite(instanceDraw.treePushAsideAmount)
                ? std::clamp(instanceDraw.treePushAsideAmount, 0.0f, 1.0f)
                : 0.0f;
            gpuInstance.treePushAsideDistanceFactor =
                std::isfinite(instanceDraw.treePushAsideDistanceFactor)
                ? std::max(0.0f, instanceDraw.treePushAsideDistanceFactor)
                : 0.0f;
            gpuInstance.treePushAsideDarkeningFactor =
                std::isfinite(instanceDraw.treePushAsideDarkeningFactor)
                ? instanceDraw.treePushAsideDarkeningFactor : 0.0f;
            gpuInstances.push_back(gpuInstance);
        }
        const uint32_t instanceBytes = static_cast<uint32_t>(
            gpuInstances.size() * sizeof(StaticMeshGpuInstance));
        const d3d12::FrameUploadAllocation instanceAllocation =
            m_device->allocateFrameUpload(gpuInstances.data(), instanceBytes,
                                          alignof(StaticMeshGpuInstance));
        if (!instanceAllocation) {
            TD_LOG_WARN("[WorldRenderer] Frame upload unavailable for {} W3D instances",
                        gpuInstances.size());
            m_lastStaticMeshStats.skippedPackets += static_cast<uint32_t>(std::min<size_t>(
                orderedDraws.size() - packetIndex, std::numeric_limits<uint32_t>::max()));
            break;
        }

        const D3D12_GPU_DESCRIPTOR_HANDLE texture = !skeletonMode && draw.textureSrv.ptr != 0
            ? draw.textureSrv
            : m_device->getSrvGpuHandle(0);
        const D3D12_GPU_DESCRIPTOR_HANDLE detailTexture = !skeletonMode && draw.detailTextureSrv.ptr != 0
            ? draw.detailTextureSrv
            : m_device->getSrvGpuHandle(0);
        const D3D12_GPU_DESCRIPTOR_HANDLE terrainCloudTexture =
            !skeletonMode && draw.terrainCloudTextureSrv.ptr != 0
                ? draw.terrainCloudTextureSrv
                : m_device->getSrvGpuHandle(0);
        const D3D12_GPU_DESCRIPTOR_HANDLE terrainMacroTexture =
            !skeletonMode && draw.terrainMacroTextureSrv.ptr != 0
                ? draw.terrainMacroTextureSrv
                : m_device->getSrvGpuHandle(0);
        ID3D12PipelineState* desiredPipelineState = skeletonMode
            ? m_skeletonPipelineState.Get()
            : pipelineFor(draw);
        const D3D12_VERTEX_BUFFER_VIEW instanceView{
            .BufferLocation = instanceAllocation.gpuAddress,
            .SizeInBytes = instanceBytes,
            .StrideInBytes = sizeof(StaticMeshGpuInstance),
        };
        const uint32_t instanceCount = static_cast<uint32_t>(gpuInstances.size());
        recordedDraws.push_back({
            .pipeline = desiredPipelineState,
            .worldConstants = worldAllocation.gpuAddress,
            .skinConstants = skinAllocation.gpuAddress,
            .texture = texture,
            .detailTexture = detailTexture,
            .terrainCloudTexture = terrainCloudTexture,
            .terrainMacroTexture = terrainMacroTexture,
            .vertexView = draw.vertexBuffer,
            .instanceView = instanceView,
            .indexView = drawIndexView,
            .indexCount = draw.indexCount,
            .instanceCount = instanceCount,
            .firstIndex = drawFirstIndex,
            .baseVertex = draw.baseVertex,
        });
        ++m_lastStaticMeshStats.drawCalls;
        if (instanceCount > 1) ++m_lastStaticMeshStats.instancedDrawCalls;
        m_lastStaticMeshStats.renderedInstances += instanceCount;
        m_lastStaticMeshStats.renderedTriangles +=
            static_cast<uint64_t>(draw.indexCount / 3u) * instanceCount;
        packetIndex = batchEnd;
    }

    struct RecordedCommandStats final {
        uint32_t pipelineStates = 0;
        uint32_t descriptorTables = 0;
        uint32_t vertexBuffers = 0;
        uint32_t indexBuffers = 0;
        uint32_t draws = 0;
    };
    const auto recordRange =
        [&](ID3D12GraphicsCommandList* list, size_t begin, size_t end,
            RecordedCommandStats& stats) {
            if (!list || begin >= end || end > recordedDraws.size()) return;
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetGraphicsRootSignature(m_rootSignature.Get());
            list->SetGraphicsRootConstantBufferView(
                0, cameraAllocation.gpuAddress);
            list->SetGraphicsRootDescriptorTable(
                5, shadowGpuHandle);
            list->SetGraphicsRootDescriptorTable(
                6, visibilityGpuHandle);
            list->SetGraphicsRootDescriptorTable(
                7, waterReflectionGpuHandle);
            list->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            stats.descriptorTables += 3u;

            ID3D12PipelineState* activePipeline = nullptr;
            D3D12_GPU_DESCRIPTOR_HANDLE activeTexture{};
            D3D12_GPU_DESCRIPTOR_HANDLE activeDetail{};
            D3D12_GPU_DESCRIPTOR_HANDLE activeCloud{};
            D3D12_GPU_DESCRIPTOR_HANDLE activeMacro{};
            D3D12_GPU_VIRTUAL_ADDRESS activeVertex = 0;
            D3D12_GPU_VIRTUAL_ADDRESS activeIndex = 0;
            for (size_t index = begin; index < end; ++index) {
                const StaticMeshRecordedDraw& draw = recordedDraws[index];
                if (draw.pipeline != activePipeline) {
                    list->SetPipelineState(draw.pipeline);
                    activePipeline = draw.pipeline;
                    ++stats.pipelineStates;
                }
                list->SetGraphicsRootConstantBufferView(
                    1, draw.worldConstants);
                list->SetGraphicsRootConstantBufferView(
                    2, draw.skinConstants);
                if (draw.texture.ptr != activeTexture.ptr) {
                    list->SetGraphicsRootDescriptorTable(3, draw.texture);
                    activeTexture = draw.texture;
                    ++stats.descriptorTables;
                }
                if (draw.detailTexture.ptr != activeDetail.ptr) {
                    list->SetGraphicsRootDescriptorTable(
                        4, draw.detailTexture);
                    activeDetail = draw.detailTexture;
                    ++stats.descriptorTables;
                }
                if (draw.terrainCloudTexture.ptr != activeCloud.ptr) {
                    list->SetGraphicsRootDescriptorTable(
                        8, draw.terrainCloudTexture);
                    activeCloud = draw.terrainCloudTexture;
                    ++stats.descriptorTables;
                }
                if (draw.terrainMacroTexture.ptr != activeMacro.ptr) {
                    list->SetGraphicsRootDescriptorTable(
                        9, draw.terrainMacroTexture);
                    activeMacro = draw.terrainMacroTexture;
                    ++stats.descriptorTables;
                }
                if (draw.vertexView.BufferLocation != activeVertex) {
                    list->IASetVertexBuffers(0, 1, &draw.vertexView);
                    activeVertex = draw.vertexView.BufferLocation;
                    ++stats.vertexBuffers;
                }
                list->IASetVertexBuffers(1, 1, &draw.instanceView);
                ++stats.vertexBuffers;
                if (draw.indexView.BufferLocation != activeIndex) {
                    list->IASetIndexBuffer(&draw.indexView);
                    activeIndex = draw.indexView.BufferLocation;
                    ++stats.indexBuffers;
                }
                list->DrawIndexedInstanced(
                    draw.indexCount, draw.instanceCount, draw.firstIndex,
                    draw.baseVertex, 0);
                ++stats.draws;
            }
        };

    container::Array<RecordedCommandStats, 8> commandStats{};
    uint32_t recordingListCount = 1u;
    bool recorded = false;
    constexpr size_t kMinimumParallelDrawsPerList = 64u;
    const uint32_t availableWorkers =
        m_device->parallelGraphicsWorkerCount();
    // D3D12Device intentionally permits one primary-list split per frame.
    // Use it for the leading FullWorld/pre-projector segment; the post
    // segment records sequentially on the continuation after projectors.
    if (fullWorldPass && availableWorkers > 1u &&
        recordedDraws.size() >= kMinimumParallelDrawsPerList * 2u) {
        recordingListCount = std::min<uint32_t>(
            availableWorkers,
            static_cast<uint32_t>((recordedDraws.size() +
                kMinimumParallelDrawsPerList - 1u) /
                kMinimumParallelDrawsPerList));
        const size_t drawsPerList =
            (recordedDraws.size() + recordingListCount - 1u) /
            recordingListCount;
        container::Vector<d3d12::ParallelGraphicsRecorder> recorders;
        recorders.reserve(recordingListCount);
        for (uint32_t worker = 0; worker < recordingListCount; ++worker) {
            const size_t begin = static_cast<size_t>(worker) * drawsPerList;
            const size_t end = std::min(
                recordedDraws.size(), begin + drawsPerList);
            recorders.push_back(
                [&, worker, begin, end](ID3D12GraphicsCommandList* list) {
                    recordRange(list, begin, end, commandStats[worker]);
                });
        }
        recorded = m_device->recordParallelGraphics(recorders);
    }
    if (!recorded) {
        recordingListCount = 1u;
        commandStats = {};
        recordRange(
            m_device->commandList(), 0, recordedDraws.size(),
            commandStats[0]);
    }
    for (uint32_t index = 0; index < recordingListCount; ++index) {
        const RecordedCommandStats& stats = commandStats[index];
        m_device->recordPipelineStateCall(stats.pipelineStates);
        m_device->recordGraphicsRootSignatureCall();
        m_device->recordGraphicsDescriptorTableCall(
            stats.descriptorTables);
        m_device->recordVertexBufferCall(stats.vertexBuffers);
        m_device->recordIndexBufferCall(stats.indexBuffers);
        m_device->recordDrawCall(stats.draws);
        m_lastStaticMeshStats.pipelineStateChanges +=
            stats.pipelineStates;
    }
    m_orderedDrawScratch.clear();
    m_uploadOwner->clearFramePointers();
}

} // namespace engine::render
