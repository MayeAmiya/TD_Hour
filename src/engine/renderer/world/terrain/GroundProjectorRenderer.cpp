#include "core/container/hash_containers.h"
#include "engine/renderer/world/terrain/GroundProjectorRenderer.h"
#include "engine/renderer/world/terrain/D3D12TerrainVisual.h"
#include "engine/renderer/world/terrain/TerrainTileMeshBuilder.h"
#include "engine/renderer/d3d12/runtime/D3D12QualitySettings.h"
#include "engine/renderer/d3d12/runtime/D3D12ShaderPackage.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/pipeline/WorldCamera.h"
#include "debug/debug.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>

#ifndef TD_GROUND_PROJECTOR_SHADER_PACKAGE_VERSION
#define TD_GROUND_PROJECTOR_SHADER_PACKAGE_VERSION 2
#endif

#ifndef TD_GROUND_PROJECTOR_SHADER_SOURCE_SHA256
#define TD_GROUND_PROJECTOR_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif

#define TD_GROUND_PROJECTOR_STRINGIFY_INNER(value) #value
#define TD_GROUND_PROJECTOR_STRINGIFY(value) \
    TD_GROUND_PROJECTOR_STRINGIFY_INNER(value)

namespace engine::render {
namespace {

struct alignas(16) ProjectorCameraConstants final {
    float viewProjection[16];
    float visibilityOrigin[2]{};
    float visibilityInverseCellSize = 0.0f;
    float visibilityEnabled = 0.0f;
    float visibilityTextureSize[2]{};
    float playableMinimum[2]{};
    float playableMaximum[2]{};
    float playableBoundsEnabled = 0.0f;
    float padding[3]{};
};
static_assert(sizeof(ProjectorCameraConstants) == 128);

[[nodiscard]] bool finiteVector(const RenderVector& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
           std::isfinite(value.z());
}

[[nodiscard]] int32_t clampedTerrainGridCoordinate(
    float worldCoordinate, float cellWorldSize,
    int32_t minimumGrid, int32_t maximumGrid,
    bool roundUp) noexcept {
    const double gridCoordinate =
        static_cast<double>(worldCoordinate) /
        static_cast<double>(cellWorldSize);
    const double bounded = std::clamp(
        gridCoordinate, static_cast<double>(minimumGrid),
        static_cast<double>(maximumGrid));
    return static_cast<int32_t>(
        roundUp ? std::ceil(bounded) : std::floor(bounded));
}

[[nodiscard]] std::optional<float> terrainHeightAt(
    const TerrainRenderSnapshot* terrain, float worldX, float worldY) noexcept {
    if (!terrain || !terrain->isValid() || !std::isfinite(worldX) ||
        !std::isfinite(worldY)) {
        return std::nullopt;
    }
    const float sampleX = worldX / terrain->cellWorldSize +
        static_cast<float>(terrain->borderSize);
    const float sampleY = worldY / terrain->cellWorldSize +
        static_cast<float>(terrain->borderSize);
    if (sampleX < 0.0f || sampleY < 0.0f ||
        sampleX > static_cast<float>(terrain->width - 1) ||
        sampleY > static_cast<float>(terrain->height - 1)) {
        return std::nullopt;
    }
    const int32_t x0 = static_cast<int32_t>(std::floor(sampleX));
    const int32_t y0 = static_cast<int32_t>(std::floor(sampleY));
    const int32_t x1 = std::min(x0 + 1, terrain->width - 1);
    const int32_t y1 = std::min(y0 + 1, terrain->height - 1);
    const float tx = sampleX - static_cast<float>(x0);
    const float ty = sampleY - static_cast<float>(y0);
    const float h00 = terrain->heightWorld(x0, y0);
    const float h10 = terrain->heightWorld(x1, y0);
    const float h01 = terrain->heightWorld(x0, y1);
    const float h11 = terrain->heightWorld(x1, y1);
    return (h00 + (h10 - h00) * tx) * (1.0f - ty) +
           (h01 + (h11 - h01) * tx) * ty;
}

[[nodiscard]] std::optional<RenderVector> terrainNormalAt(
    const TerrainRenderSnapshot* terrain, float worldX, float worldY) noexcept {
    if (!terrain || !terrain->isValid() || !std::isfinite(worldX) ||
        !std::isfinite(worldY) || !std::isfinite(terrain->cellWorldSize) ||
        terrain->cellWorldSize <= 0.0f) {
        return std::nullopt;
    }
    const float sampleX = worldX / terrain->cellWorldSize +
        static_cast<float>(terrain->borderSize);
    const float sampleY = worldY / terrain->cellWorldSize +
        static_cast<float>(terrain->borderSize);
    if (sampleX < 0.0f || sampleY < 0.0f ||
        sampleX > static_cast<float>(terrain->width - 1) ||
        sampleY > static_cast<float>(terrain->height - 1)) {
        return std::nullopt;
    }
    const int32_t x0 = static_cast<int32_t>(std::floor(sampleX));
    const int32_t y0 = static_cast<int32_t>(std::floor(sampleY));
    const int32_t x1 = std::min(x0 + 1, terrain->width - 1);
    const int32_t y1 = std::min(y0 + 1, terrain->height - 1);
    const float tx = sampleX - static_cast<float>(x0);
    const float ty = sampleY - static_cast<float>(y0);
    const float h00 = terrain->heightWorld(x0, y0);
    const float h10 = terrain->heightWorld(x1, y0);
    const float h01 = terrain->heightWorld(x0, y1);
    const float h11 = terrain->heightWorld(x1, y1);
    const float inverseCell = 1.0f / terrain->cellWorldSize;
    const float slopeX = ((h10 - h00) * (1.0f - ty) +
                          (h11 - h01) * ty) * inverseCell;
    const float slopeY = ((h01 - h00) * (1.0f - tx) +
                          (h11 - h10) * tx) * inverseCell;
    RenderVector normal{-slopeX, -slopeY, 1.0f};
    const float lengthSquared = normal.length_sq();
    if (!std::isfinite(lengthSquared) ||
        lengthSquared <= math::EPSILON * math::EPSILON) {
        return std::nullopt;
    }
    return normal / std::sqrt(lengthSquared);
}

[[nodiscard]] math::vec4 shadowColor(const TerrainRenderSnapshot* terrain) noexcept {
    // W3DShadowManager's default and map payload are both ARGB.
    const uint32_t argb = terrain && terrain->globalLighting &&
                          terrain->globalLighting->shadowColor
        ? *terrain->globalLighting->shadowColor
        : 0x7fa0a0a0u;
    constexpr float inverseByte = 1.0f / 255.0f;
    return {
        static_cast<float>((argb >> 16u) & 0xffu) * inverseByte,
        static_cast<float>((argb >> 8u) & 0xffu) * inverseByte,
        static_cast<float>(argb & 0xffu) * inverseByte,
        static_cast<float>((argb >> 24u) & 0xffu) * inverseByte,
    };
}

void reserveProjectorsForAppend(
    container::Vector<GroundProjectorInstance>& output,
    size_t additionalInstances) {
    const size_t maximumSize = output.max_size();
    additionalInstances = std::min(
        additionalInstances, maximumSize - output.size());
    const size_t required = output.size() + additionalInstances;
    if (required <= output.capacity()) return;

    // Explicit append APIs are commonly called once per decal owner. Grow
    // geometrically so replacing a per-owner temporary vector does not turn
    // into one exact reserve/allocation per owner.
    const size_t growth = std::max(
        output.capacity() / 2u, additionalInstances);
    const size_t grown = growth > maximumSize - output.capacity()
        ? maximumSize : output.capacity() + growth;
    output.reserve(std::max(required, grown));
}

[[nodiscard]] size_t saturatedProjectorCount(
    size_t first, size_t second) noexcept {
    return second > std::numeric_limits<size_t>::max() - first
        ? std::numeric_limits<size_t>::max() : first + second;
}

} // namespace

GroundProjectorRenderer::GroundProjectorRenderer(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures) {
    static_cast<void>(init(device, std::move(textures)));
}

GroundProjectorRenderer::~GroundProjectorRenderer() {
    shutdown();
}

bool GroundProjectorRenderer::init(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures) {
    shutdown();
    m_device = &device;
    m_textures = std::move(textures);
    if (!loadShaderPackage() || !createRootSignature() ||
        !createPipelineStates()) {
        shutdown();
        return false;
    }
    m_initialized = true;
    TD_LOG_INFO("[GroundProjectorRenderer] Initialized");
    return true;
}

void GroundProjectorRenderer::shutdown() {
    releaseTextures();
    m_preparedInstances.clear();
    m_gpuInstances.clear();
    m_stats = {};
    for (auto& pipelineState : m_pipelineStates) pipelineState.Reset();
    m_rootSignature.Reset();
    for (auto& bytecode : m_shaderBytecode) bytecode.clear();
    m_textures.reset();
    m_device = nullptr;
    m_initialized = false;
}

void GroundProjectorRenderer::resetTextureCache() {
    releaseTextures();
}

container::Vector<GroundProjectorInstance>
GroundProjectorRenderer::buildProjectedShadows(
    container::Span<const PreparedRenderInstance> instances,
    container::Span<const PreparedProjectileRenderSnapshot> projectiles,
    const TerrainRenderSnapshot* terrain,
    const WorldLightEnvironment& lights) {
    container::Vector<GroundProjectorInstance> output;
    appendProjectedShadows(output, instances, projectiles, terrain, lights);
    return output;
}

void GroundProjectorRenderer::appendProjectedShadows(
    container::Vector<GroundProjectorInstance>& output,
    container::Span<const PreparedRenderInstance> instances,
    container::Span<const PreparedProjectileRenderSnapshot> projectiles,
    const TerrainRenderSnapshot* terrain,
    const WorldLightEnvironment& lights) {
    if (!terrain || !terrain->isValid()) return;
    reserveProjectorsForAppend(
        output, saturatedProjectorCount(instances.size(), projectiles.size()));

    const math::vec4 baseColor = shadowColor(terrain);
    if (!(baseColor.w() > 0.0f)) return;

    const auto appendShadow = [&](RenderVector position,
                                  const RenderMatrix& worldTransform,
                                  float sourceRadius,
                                  const RenderShadowDescriptor& shadow) {
        if (!shadow.usesGroundProjector() || !finiteVector(position)) return;
        const std::optional<float> centerHeight =
            terrainHeightAt(terrain, position.x(), position.y());
        if (!centerHeight) return;
        const float radius = std::clamp(
            std::isfinite(sourceRadius) && sourceRadius > 0.0f
                ? sourceRadius : 1.0f,
            0.12f, 80.0f);
        // RefCode keeps authored shadow opacity independent of object
        // altitude; its projection follows the light/terrain rather than
        // fading an invented blob as the object rises.
        const float alpha = baseColor.w();
        if (alpha < 1.0f / 255.0f) return;

        math::vec3 localX = shadow.has(
                RenderShadowFlag::DirectionalProjection)
            ? lights.directionalLights[0].directionToLight
            : worldTransform.right();
        localX[2] = 0.0f;
        if (!finiteVector(localX) ||
            localX.length_sq() <= math::EPSILON * math::EPSILON) {
            localX = {1.0f, 0.0f, 0.0f};
        } else {
            localX = localX.normalized();
        }
        // RefCode derives V by rotating object X -90°. The projector shader's
        // corner order uses the opposite (+90°) geometric Y axis so V=0 is
        // the top edge and V=1 is the bottom edge without mirroring texture.
        const math::vec3 localY{-localX.y(), localX.x(), 0.0f};
        const float fallbackSize = radius * 2.0f;
        const float sizeX = std::clamp(
            std::isfinite(shadow.sizeX) && std::abs(shadow.sizeX) > math::EPSILON
                ? std::abs(shadow.sizeX) : fallbackSize,
            0.24f, 160.0f);
        const float sizeY = std::clamp(
            std::isfinite(shadow.sizeY) && std::abs(shadow.sizeY) > math::EPSILON
                ? std::abs(shadow.sizeY) : fallbackSize,
            0.24f, 160.0f);
        const float offsetX = std::isfinite(shadow.offsetX)
            ? shadow.offsetX : 0.0f;
        const float offsetY = std::isfinite(shadow.offsetY)
            ? shadow.offsetY : 0.0f;
        // RefCode's decal bounds shift by -ShadowOffsetX along object X and
        // -ShadowOffsetY along clockwise V. localY is -V, hence +offsetY.
        const RenderVector center = position - localX * offsetX +
            localY * offsetY;
        const RenderVector longAxis = localX * (sizeX * 0.5f);
        const RenderVector shortAxis = localY * (sizeY * 0.5f);
        const container::Array<RenderVector, 4> planarCorners{{
            center - longAxis - shortAxis,
            center - longAxis + shortAxis,
            center + longAxis + shortAxis,
            center + longAxis - shortAxis,
        }};
        const RenderVector terrainNormal = terrainNormalAt(
            terrain, position.x(), position.y()).value_or(
                RenderVector{0.0f, 0.0f, 1.0f});
        const RenderVector terrainSeparation = terrainNormal *
            ground_decals::visual_defaults::
                kProjectedShadowTerrainNormalOffset;

        GroundProjectorInstance projector;
        for (size_t index = 0; index < planarCorners.size(); ++index) {
            const std::optional<float> height = terrainHeightAt(
                terrain, planarCorners[index].x(), planarCorners[index].y());
            if (!height) return;
            projector.corners[index] = {
                planarCorners[index].x() + terrainSeparation.x(),
                planarCorners[index].y() + terrainSeparation.y(),
                *height + terrainSeparation.z()};
        }
        const bool additive = shadow.has(RenderShadowFlag::AdditiveDecal);
        const bool alphaBlend = shadow.has(RenderShadowFlag::AlphaDecal);
        projector.color = additive || alphaBlend
            ? math::vec4{1.0f, 1.0f, 1.0f, alpha}
            : math::vec4{baseColor.x(), baseColor.y(), baseColor.z(), alpha};
        projector.blendMode = additive
            ? GroundProjectorBlendMode::Additive
            : alphaBlend ? GroundProjectorBlendMode::Alpha
                         : GroundProjectorBlendMode::Multiply;
        projector.textureName = shadow.textureName;
        if (projector.textureName.empty() &&
            shadow.has(RenderShadowFlag::Decal)) {
            // RefCode's premade decal fallback is shadow.tga.
            projector.textureName = "shadow.tga";
        }
        projector.radialMask = projector.textureName.empty();
        projector.edgeSoftness = projector.radialMask ? 0.38f : 0.0f;
        output.push_back(projector);
    };

    container::HashSet<RenderEntityId> representedProjectiles;
    representedProjectiles.reserve(projectiles.size());
    for (const PreparedProjectileRenderSnapshot& projectile : projectiles) {
        representedProjectiles.insert(projectile.projectile.objectId);
    }
    container::HashSet<RenderEntityId> shadowedObjects;
    shadowedObjects.reserve(instances.size());
    for (const PreparedRenderInstance& instance : instances) {
        const RenderEntityId objectId = instance.objectId != 0
            ? instance.objectId : instance.id;
        if (!shadowedObjects.insert(objectId).second) continue;
        // Enemy detected stealth is rendered only by RefCode's additive heat
        // pass. Its ordinary object/shadow pass is explicitly skipped, so it
        // must not leave a projected blob after the heat pulse fades.
        if (instance.visual.heatVisionOnly) {
            representedProjectiles.erase(objectId);
            continue;
        }
        appendShadow(instance.worldTransform.translation(), instance.worldTransform,
                     instance.boundingRadius, instance.shadow);
        representedProjectiles.erase(objectId);
    }
    for (const PreparedProjectileRenderSnapshot& prepared : projectiles) {
        if (!representedProjectiles.contains(prepared.projectile.objectId)) continue;
        RenderMatrix projectileWorld = RenderMatrix::identity();
        projectileWorld.set_translation(prepared.projectile.position);
        appendShadow(prepared.projectile.position, projectileWorld,
                     prepared.projectile.boundingRadius,
                     prepared.projectile.shadow);
    }
}

container::Vector<GroundProjectorInstance>
GroundProjectorRenderer::buildTerrainScorches(
    container::Span<const TerrainScorchRenderData> scorches,
    const TerrainRenderSnapshot* terrain) {
    container::Vector<GroundProjectorInstance> output;
    appendTerrainScorches(output, scorches, terrain);
    return output;
}

void GroundProjectorRenderer::appendTerrainScorches(
    container::Vector<GroundProjectorInstance>& output,
    container::Span<const TerrainScorchRenderData> scorches,
    const TerrainRenderSnapshot* terrain,
    const RenderCameraSnapshot* camera,
    float viewportAspectRatio) {
    if (!terrain || !terrain->isValid()) return;
    const TerrainPrimaryCellTopologyResolver topology =
        prepareTerrainPrimaryCellTopologyResolver(*terrain);
    // A scorch is emitted as one quad per covered terrain cell, matching the
    // original height-map mesh instead of fitting a single four-corner plane.
    // The renderer's operational instance budget remains a separate concern.
    for (const TerrainScorchRenderData& scorch : scorches) {
        if (!finiteVector(scorch.position) || !std::isfinite(scorch.radius) ||
            scorch.radius <= 0.0f) continue;
        const float radius = std::max(
            scorch.radius,
            ground_projector_limits::kMinimumTerrainScorchRadius);
        if (camera) {
            RenderVector visibilityCenter = scorch.position;
            if (const std::optional<float> height = terrainHeightAt(
                    terrain, visibilityCenter.x(), visibilityCenter.y())) {
                visibilityCenter[2] = *height;
            }
            // The mark is a square terrain patch.  Use its half-diagonal plus
            // the complete stock height range as a conservative 3D sphere;
            // this rejects off-screen map scorches without clipping visible
            // hills at the camera edge.
            const float heightRange = terrain->heightWorldScale * 255.0f;
            const float visibilityRadius = std::hypot(
                radius * std::sqrt(2.0f), heightRange);
            if (!D3D12TerrainVisual::chunkSphereVisible(
                    *camera, viewportAspectRatio, visibilityCenter,
                    visibilityRadius)) {
                continue;
            }
        }
        const float cellSize = terrain->cellWorldSize;
        const int32_t minimumGridX = -terrain->borderSize;
        const int32_t minimumGridY = -terrain->borderSize;
        const int32_t maximumGridX =
            terrain->width - 1 - terrain->borderSize;
        const int32_t maximumGridY =
            terrain->height - 1 - terrain->borderSize;
        const double scorchMinimumX =
            static_cast<double>(scorch.position.x()) - radius;
        const double scorchMinimumY =
            static_cast<double>(scorch.position.y()) - radius;
        const double scorchMaximumX =
            static_cast<double>(scorch.position.x()) + radius;
        const double scorchMaximumY =
            static_cast<double>(scorch.position.y()) + radius;
        if (!std::isfinite(scorchMinimumX) ||
            !std::isfinite(scorchMinimumY) ||
            !std::isfinite(scorchMaximumX) ||
            !std::isfinite(scorchMaximumY)) {
            continue;
        }
        const int32_t firstGridX = clampedTerrainGridCoordinate(
            static_cast<float>(scorchMinimumX), cellSize,
            minimumGridX, maximumGridX, false);
        const int32_t firstGridY = clampedTerrainGridCoordinate(
            static_cast<float>(scorchMinimumY), cellSize,
            minimumGridY, maximumGridY, false);
        const int32_t lastGridX = clampedTerrainGridCoordinate(
            static_cast<float>(scorchMaximumX), cellSize,
            minimumGridX, maximumGridX, true);
        const int32_t lastGridY = clampedTerrainGridCoordinate(
            static_cast<float>(scorchMaximumY), cellSize,
            minimumGridY, maximumGridY, true);
        if (firstGridX >= lastGridX || firstGridY >= lastGridY) continue;
        const size_t cellCount =
            static_cast<size_t>(lastGridX - firstGridX) *
            static_cast<size_t>(lastGridY - firstGridY);
        const size_t hardMaximum =
            ground_decals::performance_limits::kHardMaximumInstancesPerFrame;
        const size_t remainingBudget = output.size() < hardMaximum
            ? hardMaximum - output.size() : 0u;
        if (remainingBudget == 0u) return;
        reserveProjectorsForAppend(
            output, std::min(cellCount, remainingBudget));

        // EXScorch01 contains a 3x3 set of marks laid out on a 4x4 UV grid:
        // every mark occupies 1/4 and neighbouring marks start 1.5/4 apart,
        // leaving the same filtering gutters as RefCode BaseHeightMap.cpp.
        constexpr float atlasCellScale = 0.25f;
        constexpr float atlasStride = 1.5f / 4.0f;
        constexpr int32_t atlasMarks = 9;
        constexpr int32_t atlasColumns = 3;
        const int32_t type =
            scorch.type >= 0 && scorch.type < atlasMarks ? scorch.type : 0;
        const float atlasU = static_cast<float>(type % atlasColumns) * atlasStride;
        const float atlasV = static_cast<float>(type / atlasColumns) * atlasStride;
        const float inverseDiameter = 1.0f / (radius * 2.0f);

        for (int32_t gridY = firstGridY; gridY < lastGridY; ++gridY) {
            for (int32_t gridX = firstGridX; gridX < lastGridX; ++gridX) {
                if (output.size() >= hardMaximum) return;
                const float x0 = static_cast<float>(gridX) * cellSize;
                const float x1 = static_cast<float>(gridX + 1) * cellSize;
                const float y0 = static_cast<float>(gridY) * cellSize;
                const float y1 = static_cast<float>(gridY + 1) * cellSize;
                const int32_t mapX = gridX + terrain->borderSize;
                const int32_t mapY = gridY + terrain->borderSize;
                const container::Array<RenderVector, 4> terrainCorners{{
                    terrain->worldPosition(mapX, mapY),
                    terrain->worldPosition(mapX, mapY + 1),
                    terrain->worldPosition(mapX + 1, mapY + 1),
                    terrain->worldPosition(mapX + 1, mapY),
                }};
                GroundProjectorInstance projector;
                for (size_t index = 0; index < terrainCorners.size(); ++index) {
                    projector.corners[index] = terrainCorners[index];
                    projector.corners[index][2] += 0.055f;
                }

                const float fractionX0 =
                    (x0 - (scorch.position.x() - radius)) * inverseDiameter;
                const float fractionX1 =
                    (x1 - (scorch.position.x() - radius)) * inverseDiameter;
                const float fractionY0 =
                    (y0 - (scorch.position.y() - radius)) * inverseDiameter;
                const float fractionY1 =
                    (y1 - (scorch.position.y() - radius)) * inverseDiameter;
                projector.uvScale = {
                    atlasCellScale * (fractionX1 - fractionX0),
                    atlasCellScale * (fractionY1 - fractionY0),
                };
                projector.uvOffset = {
                    atlasU + atlasCellScale * fractionX0,
                    atlasV + atlasCellScale * (1.0f - fractionY1),
                };
                projector.textureName = "EXScorch01.tga";
                projector.color = {1.0f, 1.0f, 1.0f, 1.0f};
                projector.radialMask = false;
                projector.edgeSoftness = 0.0f;
                projector.receivesVisibility = true;
                projector.triangleFlip =
                    resolveTerrainPrimaryCellTriangleFlip(
                        topology, mapX, mapY);
                output.push_back(std::move(projector));
            }
        }
    }
}

std::optional<GroundProjectorInstance>
GroundProjectorRenderer::buildTexturedDecal(
    RenderVector position, float radius, float yawRadians,
    container::String textureName, math::vec4 color,
    GroundProjectorBlendMode blendMode,
    const TerrainRenderSnapshot* terrain,
    uint64_t expireSimulationFrame) {
    if (!terrain || !terrain->isValid() || !finiteVector(position) ||
        !std::isfinite(radius) || radius <= 0.0f ||
        !std::isfinite(yawRadians) || textureName.empty() ||
        static_cast<size_t>(blendMode) >=
            static_cast<size_t>(GroundProjectorBlendMode::Count) ||
        !std::isfinite(color.x()) || !std::isfinite(color.y()) ||
        !std::isfinite(color.z()) || !std::isfinite(color.w()) ||
        color.w() <= 0.0f) {
        return std::nullopt;
    }
    const float extent = std::clamp(radius, 0.05f, 1024.0f);
    const float sine = std::sin(yawRadians);
    const float cosine = std::cos(yawRadians);
    const RenderVector axisX{cosine * extent, sine * extent, 0.0f};
    const RenderVector axisY{-sine * extent, cosine * extent, 0.0f};
    const container::Array<RenderVector, 4> planar{{
        position - axisX - axisY,
        position - axisX + axisY,
        position + axisX + axisY,
        position + axisX - axisY,
    }};
    GroundProjectorInstance decal;
    for (size_t index = 0; index < planar.size(); ++index) {
        const std::optional<float> height = terrainHeightAt(
            terrain, planar[index].x(), planar[index].y());
        if (!height) return std::nullopt;
        decal.corners[index] = {
            planar[index].x(), planar[index].y(),
            *height + ground_decals::visual_defaults::kTerrainOffset};
    }
    decal.color = color;
    decal.textureName = std::move(textureName);
    decal.blendMode = blendMode;
    decal.radialMask = false;
    decal.receivesVisibility = true;
    decal.expireSimulationFrame = expireSimulationFrame;
    return decal;
}

container::Vector<GroundProjectorInstance>
GroundProjectorRenderer::buildTexturedDecals(
    RenderVector position, float radius, float yawRadians,
    container::StringView textureName, math::vec4 color,
    GroundProjectorBlendMode blendMode,
    const TerrainRenderSnapshot* terrain,
    uint64_t expireSimulationFrame) {
    container::Vector<GroundProjectorInstance> output;
    appendTexturedDecals(
        output, position, radius, yawRadians, textureName, color, blendMode,
        terrain, expireSimulationFrame);
    return output;
}

void GroundProjectorRenderer::appendTexturedDecals(
    container::Vector<GroundProjectorInstance>& output,
    RenderVector position, float radius, float yawRadians,
    container::StringView textureName, math::vec4 color,
    GroundProjectorBlendMode blendMode,
    const TerrainRenderSnapshot* terrain,
    uint64_t expireSimulationFrame,
    const TerrainPrimaryCellTopologyResolver* topology) {
    if (!std::isfinite(radius) || radius <= 0.0f) return;
    appendTexturedRectDecals(
        output,
        position, radius * 2.0f, radius * 2.0f,
        0.0f, 0.0f, yawRadians, textureName, color, blendMode,
        terrain, expireSimulationFrame, topology);
}

container::Vector<GroundProjectorInstance>
GroundProjectorRenderer::buildTexturedRectDecals(
    RenderVector position, float sizeX, float sizeY,
    float offsetX, float offsetY, float yawRadians,
    container::StringView textureName, math::vec4 color,
    GroundProjectorBlendMode blendMode,
    const TerrainRenderSnapshot* terrain,
    uint64_t expireSimulationFrame) {
    container::Vector<GroundProjectorInstance> output;
    appendTexturedRectDecals(
        output, position, sizeX, sizeY, offsetX, offsetY, yawRadians,
        textureName, color, blendMode, terrain, expireSimulationFrame);
    return output;
}

void GroundProjectorRenderer::appendTexturedRectDecals(
    container::Vector<GroundProjectorInstance>& output,
    RenderVector position, float sizeX, float sizeY,
    float offsetX, float offsetY, float yawRadians,
    container::StringView textureName, math::vec4 color,
    GroundProjectorBlendMode blendMode,
    const TerrainRenderSnapshot* terrain,
    uint64_t expireSimulationFrame,
    const TerrainPrimaryCellTopologyResolver* topology) {
    if (!terrain || !terrain->isValid() || !finiteVector(position) ||
        !std::isfinite(sizeX) || sizeX <= 0.0f ||
        !std::isfinite(sizeY) || sizeY <= 0.0f ||
        !std::isfinite(offsetX) || !std::isfinite(offsetY) ||
        !std::isfinite(yawRadians) || textureName.empty() ||
        static_cast<size_t>(blendMode) >=
            static_cast<size_t>(GroundProjectorBlendMode::Count) ||
        !std::isfinite(color.x()) || !std::isfinite(color.y()) ||
        !std::isfinite(color.z()) || !std::isfinite(color.w()) ||
        color.w() <= 0.0f) {
        return;
    }

    const float cell = terrain->cellWorldSize;
    if (!std::isfinite(cell) || cell <= 0.0f) return;
    const float sine = std::sin(yawRadians);
    const float cosine = std::cos(yawRadians);
    const RenderVector axisX{cosine, sine, 0.0f};
    const RenderVector axisY{-sine, cosine, 0.0f};
    // W3DProjectedShadow stores U offset as +offsetX/size, then builds its
    // footprint around -offsetX on object X. Its V basis points clockwise;
    // axisY is the opposite basis, hence the positive offsetY term below.
    const RenderVector center = position - axisX * offsetX +
        axisY * offsetY;
    const float halfSizeX = sizeX * 0.5f;
    const float halfSizeY = sizeY * 0.5f;
    if (!finiteVector(center) || !std::isfinite(halfSizeX) ||
        !std::isfinite(halfSizeY)) {
        return;
    }

    const container::Array<RenderVector, 4> footprint{{
        center - axisX * halfSizeX - axisY * halfSizeY,
        center - axisX * halfSizeX + axisY * halfSizeY,
        center + axisX * halfSizeX + axisY * halfSizeY,
        center + axisX * halfSizeX - axisY * halfSizeY,
    }};
    float minimumX = footprint[0].x();
    float maximumX = minimumX;
    float minimumY = footprint[0].y();
    float maximumY = minimumY;
    for (const RenderVector& corner : footprint) {
        if (!finiteVector(corner)) return;
        minimumX = std::min(minimumX, corner.x());
        maximumX = std::max(maximumX, corner.x());
        minimumY = std::min(minimumY, corner.y());
        maximumY = std::max(maximumY, corner.y());
    }

    const int32_t minimumGridX = -terrain->borderSize;
    const int32_t minimumGridY = -terrain->borderSize;
    const int32_t maximumGridX =
        terrain->width - 1 - terrain->borderSize;
    const int32_t maximumGridY =
        terrain->height - 1 - terrain->borderSize;
    const int32_t firstGridX = clampedTerrainGridCoordinate(
        minimumX, cell, minimumGridX, maximumGridX, false);
    const int32_t firstGridY = clampedTerrainGridCoordinate(
        minimumY, cell, minimumGridY, maximumGridY, false);
    const int32_t lastGridX = clampedTerrainGridCoordinate(
        maximumX, cell, minimumGridX, maximumGridX, true);
    const int32_t lastGridY = clampedTerrainGridCoordinate(
        maximumY, cell, minimumGridY, maximumGridY, true);
    if (firstGridX >= lastGridX || firstGridY >= lastGridY) return;

    const size_t cellCount =
        static_cast<size_t>(lastGridX - firstGridX) *
        static_cast<size_t>(lastGridY - firstGridY);
    const size_t remainingBudget =
        ground_decals::performance_limits::kHardMaximumInstancesPerFrame >
                output.size()
            ? ground_decals::performance_limits::
                    kHardMaximumInstancesPerFrame - output.size()
            : 0u;
    const size_t estimatedInstances = cellCount;
    reserveProjectorsForAppend(
        output, std::min(remainingBudget, estimatedInstances));
    const float inverseSizeX = 1.0f / sizeX;
    const float inverseSizeY = 1.0f / sizeY;
    const auto projectorUv = [&](const RenderVector& point) {
        const RenderVector relative = point - center;
        return math::vec2{
            relative.dot(axisX) * inverseSizeX + 0.5f,
            0.5f - relative.dot(axisY) * inverseSizeY,
        };
    };
    TerrainPrimaryCellTopologyResolver ownedTopology;
    if (!topology || topology->terrain != terrain) {
        ownedTopology = prepareTerrainPrimaryCellTopologyResolver(*terrain);
        topology = &ownedTopology;
    }

    for (int32_t gridY = firstGridY; gridY < lastGridY; ++gridY) {
        for (int32_t gridX = firstGridX; gridX < lastGridX; ++gridX) {
            if (output.size() >=
                ground_decals::performance_limits::
                    kHardMaximumInstancesPerFrame) {
                return;
            }
            const int32_t mapX = gridX + terrain->borderSize;
            const int32_t mapY = gridY + terrain->borderSize;
            const RenderVector topLeft = terrain->worldPosition(mapX, mapY);
            const RenderVector topRight =
                terrain->worldPosition(mapX + 1, mapY);
            const RenderVector bottomRight =
                terrain->worldPosition(mapX + 1, mapY + 1);
            const RenderVector bottomLeft =
                terrain->worldPosition(mapX, mapY + 1);
            GroundProjectorInstance projector;
            projector.corners = {
                topLeft, topRight, bottomRight, bottomLeft};
            for (RenderVector& corner : projector.corners) {
                corner[2] += ground_decals::visual_defaults::kTerrainOffset;
            }
            projector.cornerUvs = {
                projectorUv(topLeft), projectorUv(topRight),
                projectorUv(bottomRight), projectorUv(bottomLeft)};
            projector.color = color;
            projector.textureName = container::String{textureName};
            projector.blendMode = blendMode;
            projector.radialMask = false;
            projector.edgeSoftness = 0.0f;
            projector.receivesVisibility = true;
            projector.explicitCornerUvs = true;
            projector.triangleFlip = resolveTerrainPrimaryCellTriangleFlip(
                *topology, mapX, mapY);
            projector.expireSimulationFrame = expireSimulationFrame;
            output.push_back(std::move(projector));
        }
    }
}

size_t GroundProjectorRenderer::render(
    container::Span<const GroundProjectorInstance> instances,
    const RenderCameraSnapshot& camera,
    uint64_t simulationFrame,
    const WorldLocalVisibilityGpuBinding& visibility) {
    m_stats = {
        .submittedInstances = static_cast<uint32_t>(std::min<size_t>(
            instances.size(), std::numeric_limits<uint32_t>::max())),
    };
    // Same rule as the other world renderers: configureTextureSampling resets
    // the root signature and pipeline states, can fail to recreate them, and
    // returns false while leaving m_initialized true — its caller only logs.
    // Recording a null root signature / null PSO is invalid D3D12 usage and can
    // turn a recoverable rebuild failure into device removal.
    if (!m_initialized || !m_device || !m_device->commandList() || instances.empty() ||
        !m_rootSignature ||
        std::any_of(m_pipelineStates.begin(), m_pipelineStates.end(),
                    [](const auto& pipeline) { return !pipeline; }) ||
        m_device->width() == 0 || m_device->height() == 0) {
        return 0;
    }

    m_preparedInstances.clear();
    const size_t maximumInstances = std::min<size_t>(
        instances.size(), m_maximumInstancesPerFrame);
    m_preparedInstances.reserve(maximumInstances);
    // A pending texture is intentionally absent from the persistent resident
    // map so a later frame can retry it.  Without a frame-local result cache,
    // however, thousands of cells using the same not-yet-ready decal texture
    // each repeated the identical WorldTextureCache lookup in this frame.
    container::HashMap<container::StringView, uint32_t> frameTextureSrvs;
    frameTextureSrvs.reserve(std::min<size_t>(
        maximumInstances, m_maximumResidentTextures));
    const auto resolveTextureSrvForFrame = [this, &frameTextureSrvs](
                                                container::StringView name) {
        if (const auto found = frameTextureSrvs.find(name);
            found != frameTextureSrvs.end()) {
            return found->second;
        }
        const uint32_t srv = textureSrv(name);
        frameTextureSrvs.emplace(name, srv);
        return srv;
    };
    for (size_t instanceIndex = 0; instanceIndex < maximumInstances;
         ++instanceIndex) {
        const GroundProjectorInstance& instance = instances[instanceIndex];
        if (!instance.visible ||
            (instance.expireSimulationFrame != 0 &&
             simulationFrame >= instance.expireSimulationFrame)) {
            continue;
        }
        const bool valid = std::all_of(
            instance.corners.begin(), instance.corners.end(), finiteVector) &&
            std::isfinite(instance.color.x()) && std::isfinite(instance.color.y()) &&
            std::isfinite(instance.color.z()) && std::isfinite(instance.color.w()) &&
            std::isfinite(instance.uvScale.x()) &&
            std::isfinite(instance.uvScale.y()) &&
            std::isfinite(instance.uvOffset.x()) &&
            std::isfinite(instance.uvOffset.y()) &&
            instance.uvScale.x() > 0.0f && instance.uvScale.y() > 0.0f &&
            (!instance.explicitCornerUvs || std::all_of(
                instance.cornerUvs.begin(), instance.cornerUvs.end(),
                [](const math::vec2& uv) {
                    return std::isfinite(uv.x()) && std::isfinite(uv.y());
                })) &&
            std::isfinite(instance.edgeSoftness) && instance.color.w() > 0.0f &&
            static_cast<size_t>(instance.blendMode) <
                static_cast<size_t>(GroundProjectorBlendMode::Count);
        if (!valid) {
            ++m_stats.rejectedInstances;
            continue;
        }
        PreparedInstance prepared;
        GpuInstance& gpu = prepared.gpu;
        for (size_t corner = 0; corner < instance.corners.size(); ++corner) {
            gpu.corners[corner][0] = instance.corners[corner].x();
            gpu.corners[corner][1] = instance.corners[corner].y();
            gpu.corners[corner][2] = instance.corners[corner].z();
        }
        gpu.color[0] = std::clamp(instance.color.x(), 0.0f, 1.0f);
        gpu.color[1] = std::clamp(instance.color.y(), 0.0f, 1.0f);
        gpu.color[2] = std::clamp(instance.color.z(), 0.0f, 1.0f);
        gpu.color[3] = std::clamp(instance.color.w(), 0.0f, 1.0f);
        gpu.uvScale[0] = instance.uvScale.x();
        gpu.uvScale[1] = instance.uvScale.y();
        gpu.uvOffset[0] = instance.uvOffset.x();
        gpu.uvOffset[1] = instance.uvOffset.y();
        gpu.edgeSoftness = std::clamp(instance.edgeSoftness, 0.01f, 0.99f);
        gpu.radialMask = instance.radialMask ? 1u : 0u;
        gpu.blendMode = static_cast<uint32_t>(instance.blendMode);
        gpu.receivesVisibility = instance.receivesVisibility ? 1u : 0u;
        for (size_t corner = 0; corner < instance.cornerUvs.size(); ++corner) {
            gpu.cornerUvs[corner][0] = instance.cornerUvs[corner].x();
            gpu.cornerUvs[corner][1] = instance.cornerUvs[corner].y();
        }
        gpu.explicitCornerUvs = instance.explicitCornerUvs ? 1u : 0u;
        gpu.triangleFlip = instance.triangleFlip ? 1u : 0u;
        prepared.textureSrv = instance.radialMask || instance.textureName.empty()
            ? 0u
            : resolveTextureSrvForFrame(instance.textureName);
        prepared.blendMode = instance.blendMode;
        math::vec3 center{};
        for (const RenderVector& corner : instance.corners) center += corner;
        center = center / static_cast<float>(instance.corners.size());
        const float distanceSquared =
            (center - camera.position).length_sq();
        prepared.distanceSquared = std::isfinite(distanceSquared)
            ? distanceSquared : -1.0f;
        m_preparedInstances.push_back(std::move(prepared));
    }
    if (instances.size() > maximumInstances) {
        m_stats.budgetRejectedInstances = static_cast<uint32_t>(std::min<size_t>(
            instances.size() - maximumInstances,
            std::numeric_limits<uint32_t>::max()));
    }
    std::stable_sort(
        m_preparedInstances.begin(), m_preparedInstances.end(),
        [](const PreparedInstance& left, const PreparedInstance& right) {
            if (left.blendMode != right.blendMode) {
                return left.blendMode < right.blendMode;
            }
            if (left.distanceSquared != right.distanceSquared) {
                return left.distanceSquared > right.distanceSquared;
            }
            return left.textureSrv < right.textureSrv;
        });
    m_gpuInstances.clear();
    m_gpuInstances.reserve(m_preparedInstances.size());
    for (const PreparedInstance& prepared : m_preparedInstances) {
        m_gpuInstances.push_back(prepared.gpu);
    }
    if (m_gpuInstances.empty() ||
        m_gpuInstances.size() > std::numeric_limits<uint32_t>::max() /
            sizeof(GpuInstance)) {
        return 0;
    }

    const uint32_t instanceBytes = static_cast<uint32_t>(
        m_gpuInstances.size() * sizeof(GpuInstance));
    const d3d12::FrameUploadAllocation instanceAllocation =
        m_device->allocateFrameUpload(m_gpuInstances.data(), instanceBytes,
                                      alignof(GpuInstance));
    const WorldCamera worldCamera = WorldCamera::fromSnapshot(camera);
    const float aspect = static_cast<float>(m_device->width()) /
        static_cast<float>(m_device->height());
    const math::float4x4 viewProjection = worldCamera.viewProjectionMatrix(aspect);
    ProjectorCameraConstants constants{};
    std::memcpy(constants.viewProjection, &viewProjection.m,
                sizeof(constants.viewProjection));
    if (visibility.playableBoundsEnabled &&
        std::isfinite(visibility.playableMinimum.x()) &&
        std::isfinite(visibility.playableMinimum.y()) &&
        std::isfinite(visibility.playableMaximum.x()) &&
        std::isfinite(visibility.playableMaximum.y()) &&
        visibility.playableMinimum.x() <=
            visibility.playableMaximum.x() &&
        visibility.playableMinimum.y() <=
            visibility.playableMaximum.y()) {
        constants.playableMinimum[0] = visibility.playableMinimum.x();
        constants.playableMinimum[1] = visibility.playableMinimum.y();
        constants.playableMaximum[0] = visibility.playableMaximum.x();
        constants.playableMaximum[1] = visibility.playableMaximum.y();
        constants.playableBoundsEnabled = 1.0f;
    }
    if (visibility.enabled && visibility.textureSrv.ptr != 0 &&
        std::isfinite(visibility.origin.x()) &&
        std::isfinite(visibility.origin.y()) &&
        std::isfinite(visibility.inverseCellSize) &&
        visibility.inverseCellSize > 0.0f &&
        std::isfinite(visibility.textureSize.x()) &&
        std::isfinite(visibility.textureSize.y()) &&
        visibility.textureSize.x() > 0.0f &&
        visibility.textureSize.y() > 0.0f) {
        constants.visibilityOrigin[0] = visibility.origin.x();
        constants.visibilityOrigin[1] = visibility.origin.y();
        constants.visibilityInverseCellSize = visibility.inverseCellSize;
        constants.visibilityEnabled = 1.0f;
        constants.visibilityTextureSize[0] = visibility.textureSize.x();
        constants.visibilityTextureSize[1] = visibility.textureSize.y();
    }
    const d3d12::ConstantBufferAllocation cameraAllocation =
        m_device->allocateConstantBuffer(&constants, sizeof(constants));
    if (!instanceAllocation || !cameraAllocation) return 0;

    m_device->flushBatch();
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_device->width());
    const uint32_t tacticalHeight =
        worldCamera.tacticalViewportHeight(m_device->height());
    viewport.Height = static_cast<float>(tacticalHeight);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_device->width()),
                             static_cast<LONG>(tacticalHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    m_device->bindSrvHeap();
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetGraphicsRootConstantBufferView(0, cameraAllocation.gpuAddress);
    commandList->SetGraphicsRootDescriptorTable(
        2, visibility.textureSrv.ptr != 0
            ? visibility.textureSrv
            : m_device->getSrvGpuHandle(0));
    m_device->recordGraphicsDescriptorTableCall();
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    size_t batchStart = 0;
    while (batchStart < m_preparedInstances.size()) {
        const GroundProjectorBlendMode blendMode =
            m_preparedInstances[batchStart].blendMode;
        const uint32_t texture = m_preparedInstances[batchStart].textureSrv;
        size_t batchEnd = batchStart + 1u;
        while (batchEnd < m_preparedInstances.size() &&
               m_preparedInstances[batchEnd].blendMode == blendMode &&
               m_preparedInstances[batchEnd].textureSrv == texture) {
            ++batchEnd;
        }
        const size_t pipelineIndex = static_cast<size_t>(blendMode);
        commandList->SetPipelineState(m_pipelineStates[pipelineIndex].Get());
        m_device->recordPipelineStateCall();
        commandList->SetGraphicsRootDescriptorTable(
            1, m_device->getSrvGpuHandle(texture));
        m_device->recordGraphicsDescriptorTableCall();
        const uint32_t batchBytes = static_cast<uint32_t>(
            (batchEnd - batchStart) * sizeof(GpuInstance));
        const D3D12_VERTEX_BUFFER_VIEW instanceView{
            .BufferLocation = instanceAllocation.gpuAddress +
                batchStart * sizeof(GpuInstance),
            .SizeInBytes = batchBytes,
            .StrideInBytes = sizeof(GpuInstance),
        };
        commandList->IASetVertexBuffers(0, 1, &instanceView);
        m_device->recordVertexBufferCall();
        commandList->DrawInstanced(
            6, static_cast<UINT>(batchEnd - batchStart), 0, 0);
        m_device->recordDrawCall();
        ++m_stats.drawCalls;
        batchStart = batchEnd;
    }
    m_stats.renderedInstances = static_cast<uint32_t>(m_gpuInstances.size());
    m_stats.textureBatches = m_stats.drawCalls;
    m_stats.residentTextures = static_cast<uint32_t>(m_textureSrvs.size());
    return m_gpuInstances.size();
}

uint32_t GroundProjectorRenderer::textureSrv(container::StringView textureName) {
    if (textureName.empty() || !m_textures) return 0;
    const container::String key(textureName);
    if (const auto found = m_textureSrvs.find(key);
        found != m_textureSrvs.end()) {
        return found->second;
    }
    if (m_textureSrvs.size() >= m_maximumResidentTextures) {
        return 0;
    }
    const std::optional<uint32_t> acquired = m_textures->acquire(textureName);
    if (!acquired) return 0;
    try {
        m_textureSrvs.emplace(key, *acquired);
    } catch (...) {
        m_textures->release(textureName);
        return 0;
    }
    return *acquired;
}

void GroundProjectorRenderer::configureOperationalBudget(
    uint32_t maximumInstancesPerFrame,
    uint32_t maximumResidentTextures) {
    const ground_decals::performance_limits::OperationalBudget budget =
        ground_decals::performance_limits::operationalBudget(
            maximumInstancesPerFrame, maximumResidentTextures);
    const uint32_t instances = budget.maximumInstancesPerFrame;
    const uint32_t textures = budget.maximumResidentTextures;
    if (textures < m_textureSrvs.size()) releaseTextures();
    m_maximumInstancesPerFrame = instances;
    m_maximumResidentTextures = textures;
}

bool GroundProjectorRenderer::configureTextureSampling(
    uint32_t textureFilter, uint32_t anisotropyLevel,
    uint32_t sampleCount) {
    const d3d12::TextureSamplingQuality current =
        d3d12::textureSamplingQuality(
            m_textureFilter, m_anisotropyLevel);
    const d3d12::TextureSamplingQuality requested =
        d3d12::textureSamplingQuality(textureFilter, anisotropyLevel);
    m_textureFilter = textureFilter;
    m_anisotropyLevel = anisotropyLevel;
    const uint32_t previousSampleCount = m_sampleCount;
    m_sampleCount = sampleCount;
    if (!m_initialized ||
        (current.filter == requested.filter &&
         current.maximumAnisotropy == requested.maximumAnisotropy &&
         current.maximumLod == requested.maximumLod &&
         previousSampleCount == sampleCount)) {
        return true;
    }
    for (auto& pipeline : m_pipelineStates) pipeline.Reset();
    m_rootSignature.Reset();
    return createRootSignature() && createPipelineStates();
}

void GroundProjectorRenderer::releaseTextures() {
    if (m_textures) {
        for (const auto& [textureName, textureSrvIndex] : m_textureSrvs) {
            static_cast<void>(textureSrvIndex);
            m_textures->release(textureName);
        }
    }
    m_textureSrvs.clear();
}

bool GroundProjectorRenderer::createRootSignature() {
    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.BaseShaderRegister = 0;
    textureRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE visibilityRange{};
    visibilityRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    visibilityRange.NumDescriptors = 1;
    visibilityRange.BaseShaderRegister = 1;
    visibilityRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    container::Array<D3D12_ROOT_PARAMETER, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &textureRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &visibilityRange;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    const d3d12::TextureSamplingQuality sampling =
        d3d12::textureSamplingQuality(
            m_textureFilter, m_anisotropyLevel);
    sampler.Filter = sampling.filter;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = sampling.maximumLod;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxAnisotropy = sampling.maximumAnisotropy;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serializeResult)) {
        TD_LOG_ERROR("[GroundProjectorRenderer] root signature serialization failed: 0x{:08X}",
                     serializeResult);
        return false;
    }
    const HRESULT createResult = m_device->getDevice()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(createResult)) {
        TD_LOG_ERROR("[GroundProjectorRenderer] root signature creation failed: 0x{:08X}",
                     createResult);
        return false;
    }
    return true;
}

bool GroundProjectorRenderer::loadShaderPackage() {
    const container::Array<d3d12::ShaderPackageEntrySpec, 2> entries{{
        {"vertex_file", "ground_projector_vs.cso",
         "vertex_profile", "vs_5_0"},
        {"pixel_file", "ground_projector_ps.cso",
         "pixel_profile", "ps_5_0"},
    }};
    container::Vector<container::Vector<uint8_t>> loaded;
    if (!d3d12::loadShaderPackage(
            "ground_projector",
            TD_GROUND_PROJECTOR_STRINGIFY(
                TD_GROUND_PROJECTOR_SHADER_PACKAGE_VERSION),
            TD_GROUND_PROJECTOR_SHADER_SOURCE_SHA256,
            {entries.data(), entries.size()}, loaded) ||
        loaded.size() != m_shaderBytecode.size()) {
        TD_LOG_ERROR(
            "[GroundProjectorRenderer] precompiled shader package unavailable; ground projectors disabled");
        return false;
    }
    for (size_t index = 0; index < loaded.size(); ++index) {
        m_shaderBytecode[index] = std::move(loaded[index]);
    }
    return true;
}

bool GroundProjectorRenderer::createPipelineStates() {
    if (m_shaderBytecode[0].empty() || m_shaderBytecode[1].empty()) {
        TD_LOG_ERROR(
            "[GroundProjectorRenderer] shader bytecode unavailable");
        return false;
    }

    static_assert(std::is_standard_layout_v<GpuInstance>);
    static_assert(offsetof(GpuInstance, corners) == 0);
    static_assert(offsetof(GpuInstance, color) == 48);
    static_assert(offsetof(GpuInstance, uvScale) == 64);
    static_assert(offsetof(GpuInstance, uvOffset) == 72);
    static_assert(offsetof(GpuInstance, edgeSoftness) == 80);
    static_assert(offsetof(GpuInstance, radialMask) == 84);
    static_assert(offsetof(GpuInstance, blendMode) == 88);
    static_assert(offsetof(GpuInstance, receivesVisibility) == 92);
    static_assert(offsetof(GpuInstance, cornerUvs) == 96);
    static_assert(offsetof(GpuInstance, explicitCornerUvs) == 128);
    static_assert(offsetof(GpuInstance, triangleFlip) == 132);
    static_assert(sizeof(GpuInstance) == 136);
    const D3D12_INPUT_ELEMENT_DESC input[] = {
        {"PROJECTOR_CORNER", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_CORNER", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_CORNER", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_CORNER", 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_UV_SCALE", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 64,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_UV_OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 72,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_SOFTNESS", 0, DXGI_FORMAT_R32_FLOAT, 0, 80,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_RADIAL", 0, DXGI_FORMAT_R32_UINT, 0, 84,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_BLEND", 0, DXGI_FORMAT_R32_UINT, 0, 88,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_VISIBILITY", 0, DXGI_FORMAT_R32_UINT, 0, 92,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_EXPLICIT_UV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 96,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_EXPLICIT_UV", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 104,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_EXPLICIT_UV", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 112,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_EXPLICIT_UV", 3, DXGI_FORMAT_R32G32_FLOAT, 0, 120,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_EXPLICIT_UVS", 0, DXGI_FORMAT_R32_UINT, 0, 128,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PROJECTOR_TRIANGLE_FLIP", 0, DXGI_FORMAT_R32_UINT, 0, 132,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_rootSignature.Get();
    description.VS = {
        m_shaderBytecode[0].data(), m_shaderBytecode[0].size()};
    description.PS = {
        m_shaderBytecode[1].data(), m_shaderBytecode[1].size()};
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.FrontCounterClockwise = TRUE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    description.InputLayout = {input, static_cast<UINT>(std::size(input))};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = d3d12::D3D12Device::SWAP_FORMAT;
    description.DSVFormat = d3d12::D3D12Device::DEPTH_FORMAT;
    description.SampleDesc.Count = m_sampleCount;

    for (size_t index = 0; index < m_pipelineStates.size(); ++index) {
        auto& blend = description.BlendState.RenderTarget[0];
        blend = {};
        blend.BlendEnable = TRUE;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        const auto mode = static_cast<GroundProjectorBlendMode>(index);
        if (mode == GroundProjectorBlendMode::Additive) {
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_ONE;
        } else if (mode == GroundProjectorBlendMode::Multiply) {
            blend.SrcBlend = D3D12_BLEND_DEST_COLOR;
            blend.DestBlend = D3D12_BLEND_ZERO;
        } else {
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        }
        const HRESULT result = m_device->getDevice()->CreateGraphicsPipelineState(
            &description, IID_PPV_ARGS(&m_pipelineStates[index]));
        if (FAILED(result)) {
            TD_LOG_ERROR(
                "[GroundProjectorRenderer] pipeline {} creation failed: 0x{:08X}",
                index, result);
            return false;
        }
    }
    return true;
}

} // namespace engine::render
