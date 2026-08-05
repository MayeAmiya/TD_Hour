#include "core/container/container_types.h"
#include "core/container/hash_containers.h"
#include "core/debug/td_assert.h"
#include "GameRenderExtraction.h"
#include "GameRenderExtractionTerrainSource.h"

#include "presentation/render/TrackMarksPerformanceSettings.h"
#include "presentation/render/HeatVisionVisualSettings.h"
#include "game/render/ClientTerrainObjectStore.h"
#include "game/render/LocalPlacementPresentationState.h"
#include "game/render/LocalPlacementPreviewPresentation.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/player/PlayerList.h"
#include "game/script/presentation/ScriptMapPresentationControls.h"
#include "game/data/presentation/ScriptWaterPresentationSettings.h"
#include "game/data/presentation/ScriptTerrainRoadPresentationSettings.h"
#include "presentation/render/ProjectileStreamJoinPresentation.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/movement/ObjectFloat.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/world/ObjectDynamicShroud.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectEmpUpdate.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/combat/ObjectStickyBomb.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/object/simulation/combat/ObjectWeaponBonusUpdate.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/player/FactionTemplate.h"
#include "game/script/runtime/ScriptProgram.h"
#include "game/terrain/MapHeightfieldLoader.h"
#include "game/terrain/TerrainLogic.h"
#include "presentation/render/SupportDrawPresentation.h"
#include "core/config/GlobalData.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include "GameRenderExtractionDetail.h"

namespace engine {
using namespace render_extraction_detail;
namespace {

[[nodiscard]] std::optional<float> numericMapProperty(
    const game::terrain::MapPropertyDict& properties,
    container::StringView key) noexcept {
    const auto found = properties.find(container::String(key));
    if (found == properties.end()) return std::nullopt;
    if (const int32_t* value = std::get_if<int32_t>(&found->second)) {
        return static_cast<float>(*value);
    }
    if (const float* value = std::get_if<float>(&found->second)) {
        return std::isfinite(*value) ? std::optional<float>{*value} : std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] math::vec3 packedMapRgb(float source) noexcept {
    const uint32_t packed = source > 0.0f
        ? static_cast<uint32_t>(source) : 0u;
    constexpr float inverseByte = 1.0f / 255.0f;
    return {
        static_cast<float>((packed >> 16u) & 0xffu) * inverseByte,
        static_cast<float>((packed >> 8u) & 0xffu) * inverseByte,
        static_cast<float>(packed & 0xffu) * inverseByte,
    };
}

render::TerrainTextureClassRenderData copyTerrainTextureClass(
    const game::terrain::TerrainTextureClass& source) {
    return {
        .firstTile = source.firstTile,
        .tileCount = source.tileCount,
        .tileWidth = source.tileWidth,
        .name = source.name,
    };
}

render::TerrainLightingRenderData copyTerrainLighting(
    const game::terrain::TerrainLighting& source) {
    return {
        .ambient = source.ambient,
        .diffuse = source.diffuse,
        .direction = source.direction,
    };
}

render::TerrainMaterialRenderData copyTerrainMaterials(
    const game::terrain::TerrainBlendTileData& source) {
    render::TerrainMaterialRenderData output;
    output.sourceVersion = source.sourceVersion;
    output.bitmapTileCount = source.bitmapTileCount;
    output.edgeTileCount = source.edgeTileCount;
    output.baseTileIndices = source.baseTileIndices;
    output.blendTileIndices = source.blendTileIndices;
    output.extraBlendTileIndices = source.extraBlendTileIndices;
    output.cliffInfoIndices = source.cliffInfoIndices;
    output.cliffCells = source.cliffCells;
    output.textureClasses.reserve(source.textureClasses.size());
    for (const game::terrain::TerrainTextureClass& textureClass : source.textureClasses) {
        output.textureClasses.push_back(copyTerrainTextureClass(textureClass));
    }
    output.textureClassBySourceTile.assign(
        static_cast<size_t>(std::max(0, output.bitmapTileCount)), -1);
    for (size_t classIndex = 0; classIndex < output.textureClasses.size();
         ++classIndex) {
        const render::TerrainTextureClassRenderData& textureClass =
            output.textureClasses[classIndex];
        const int32_t first = std::max(0, textureClass.firstTile);
        const int32_t end = std::min(
            output.bitmapTileCount,
            textureClass.firstTile + std::max(0, textureClass.tileCount));
        for (int32_t sourceTile = first; sourceTile < end; ++sourceTile) {
            output.textureClassBySourceTile[static_cast<size_t>(sourceTile)] =
                static_cast<int32_t>(classIndex);
        }
    }
    output.edgeTextureClasses.reserve(source.edgeTextureClasses.size());
    for (const game::terrain::TerrainTextureClass& textureClass : source.edgeTextureClasses) {
        output.edgeTextureClasses.push_back(copyTerrainTextureClass(textureClass));
    }
    output.blendDefinitions.reserve(source.blendDefinitions.size());
    for (const game::terrain::TerrainBlendDefinition& definition : source.blendDefinitions) {
        output.blendDefinitions.push_back({
            .blendTileIndex = definition.blendIndex,
            .horizontal = definition.horizontal,
            .vertical = definition.vertical,
            .rightDiagonal = definition.rightDiagonal,
            .leftDiagonal = definition.leftDiagonal,
            .inverted = definition.inverted,
            .longDiagonal = definition.longDiagonal,
            .customEdgeTextureClass = definition.customEdgeTextureClass,
        });
    }
    output.cliffDefinitions.reserve(source.cliffDefinitions.size());
    for (const game::terrain::TerrainCliffDefinition& definition : source.cliffDefinitions) {
        output.cliffDefinitions.push_back({
            .tileIndex = definition.tileIndex,
            .uv = definition.uv,
            .flip = definition.flip,
            .mutant = definition.mutant,
        });
    }
    return output;
}

render::TerrainGlobalLightingRenderData copyTerrainGlobalLighting(
    const game::terrain::TerrainGlobalLighting& source) {
    render::TerrainGlobalLightingRenderData output;
    output.sourceVersion = source.sourceVersion;
    output.timeOfDay = source.timeOfDay;
    output.shadowColor = source.shadowColor;
    for (size_t time = 0; time < output.terrainLights.size(); ++time) {
        for (size_t light = 0; light < output.terrainLights[time].size(); ++light) {
            output.terrainLights[time][light] =
                copyTerrainLighting(source.terrainLights[time][light]);
            output.objectLights[time][light] =
                copyTerrainLighting(source.objectLights[time][light]);
        }
    }
    return output;
}

math::vec4 copyWaterColor(const script::ScriptWaterColor& source) noexcept {
    return {source.red, source.green, source.blue, source.alpha};
}

render::TerrainWaterMaterialRenderData copyTerrainWaterMaterial(
    const script::ScriptWaterPresentationSettings& source,
    size_t timeOfDaySlot) {
    timeOfDaySlot = std::min(timeOfDaySlot, source.timeOfDay.size() - 1u);
    const script::ScriptWaterTimeOfDaySettings& selected =
        source.timeOfDay[timeOfDaySlot];
    render::TerrainWaterMaterialRenderData output;
    output.textureName = selected.waterTexture.empty()
        ? source.standingWaterTexture : selected.waterTexture;
    output.standingWaterTextureName = source.standingWaterTexture;
    output.skyTextureName = selected.skyTexture;
    for (size_t index = 0; index < output.vertexColors.size(); ++index) {
        output.vertexColors[index] = copyWaterColor(selected.vertexColors[index]);
    }
    output.diffuseColor = copyWaterColor(selected.diffuseColor);
    output.transparentDiffuseColor = copyWaterColor(selected.transparentDiffuseColor);
    output.standingWaterColor = copyWaterColor(source.standingWaterColor);
    output.radarWaterColor = copyWaterColor(source.radarWaterColor);
    output.uScrollPerSecond = selected.uScrollPerMillisecond * 1000.0f;
    output.vScrollPerSecond = selected.vScrollPerMillisecond * 1000.0f;
    output.textureRepeatCount = static_cast<float>(
        std::max(1, selected.waterRepeatCount));
    output.skyTexelsPerUnit = selected.skyTexelsPerUnit;
    output.minimumOpacity = source.minimumWaterOpacity;
    output.transparentWaterDepth = source.transparentWaterDepth;
    output.additiveBlending = source.additiveBlending;
    return output;
}

[[nodiscard]] container::StringView pathFileName(
    container::StringView path) noexcept {
    const size_t separator = path.find_last_of("/\\");
    return separator == container::StringView::npos
        ? path : path.substr(separator + 1u);
}

[[nodiscard]] std::optional<render::TerrainVertexWaterRenderData>
selectTerrainVertexWater(
    container::StringView resolvedMapPath,
    const RenderWaterGameData& water,
    const TerrainVertexWaterState& vertexWaterState,
    bool enabled) {
    if (!enabled || resolvedMapPath.empty()) return std::nullopt;
    for (size_t index = 0; index < water.vertexWater.size(); ++index) {
        const RenderVertexWaterGameData& source = water.vertexWater[index];
        if (source.availableMaps.empty() ||
            (!equalsInsensitive(resolvedMapPath, source.availableMaps) &&
             !equalsInsensitive(pathFileName(resolvedMapPath),
                                    pathFileName(source.availableMaps)))) {
            continue;
        }
        render::TerrainVertexWaterRenderData output{
            .sourceSlot = static_cast<uint32_t>(index),
            .position = {
                source.positionX, source.positionY, source.positionZ},
            .angleRadians = source.angleRadians,
            .heightClampLow = source.heightClampLow,
            .heightClampHigh = source.heightClampHigh,
            .gridCellsX = source.gridCellsX,
            .gridCellsY = source.gridCellsY,
            .gridSize = source.gridSize,
            .attenuationA = source.attenuationA,
            .attenuationB = source.attenuationB,
            .attenuationC = source.attenuationC,
            .attenuationRange = source.attenuationRange,
        };
        if (vertexWaterState.configured()) {
            const auto& configured = vertexWaterState.config();
            if (configured.cellsX == source.gridCellsX &&
                configured.cellsY == source.gridCellsY &&
                configured.gridSize == source.gridSize &&
                configured.positionX == source.positionX &&
                configured.positionY == source.positionY &&
                configured.positionZ == source.positionZ &&
                configured.angleRadians == source.angleRadians) {
                output.pointHeights.reserve(vertexWaterState.points().size());
                for (const TerrainVertexWaterPoint& point :
                     vertexWaterState.points()) {
                    output.pointHeights.push_back(point.height);
                }
            }
        }
        if (output.isValid()) return output;
    }
    return std::nullopt;
}

container::SharedPtr<render::TerrainRenderSnapshot> buildTerrainSnapshot(
    const game::terrain::TerrainHeightfieldData& terrain,
    uint64_t terrainRevision,
    uint64_t layoutRevision = 0) {
    if (!terrain.isValid()) return {};

    auto snapshot = std::make_shared<render::TerrainRenderSnapshot>();
    snapshot->revision = terrainRevision;
    snapshot->layoutRevision = layoutRevision != 0 ? layoutRevision : terrainRevision;
    snapshot->width = terrain.width;
    snapshot->height = terrain.height;
    snapshot->borderSize = terrain.borderSize;
    snapshot->cellWorldSize = game::terrain::kMapCellWorldSize;
    snapshot->heightWorldScale = game::terrain::kMapHeightWorldScale;
    // GlobalData is logic-side configuration.  Copy its value while the
    // snapshot is produced so the renderer never observes a mutable global.
    if (config::TheWritableGlobalData) {
        snapshot->adjustCliffTextures =
            config::TheWritableGlobalData->isAdjustCliffTextures();
    }
    snapshot->heights = terrain.heights;
    if (terrain.blendTiles && terrain.blendTiles->isValidFor(terrain.heights.size())) {
        snapshot->materials = copyTerrainMaterials(*terrain.blendTiles);
    }
    if (terrain.globalLighting) {
        snapshot->globalLighting = copyTerrainGlobalLighting(*terrain.globalLighting);
    }
    return snapshot;
}


} // namespace

container::SharedPtr<const render::TerrainRenderSnapshot> GameRenderExtraction::extractTerrain(
    const game::terrain::TerrainHeightfieldData& terrain,
    uint64_t terrainRevision) {
    return buildTerrainSnapshot(terrain, terrainRevision);
}

namespace {

container::SharedPtr<render::TerrainRenderSnapshot> buildTerrainSnapshot(
    const game::terrain::TerrainLogic& terrain) {
    if (!terrain.isLoaded()) return {};
    auto snapshot = buildTerrainSnapshot(
        terrain.map().heightfield(), terrain.map().revision(),
        terrain.map().layoutRevision());
    if (!snapshot) return {};

    const game::terrain::TerrainDirtyRegion& dirty = terrain.map().dirtyRegion();
    snapshot->dirtyRegion = {dirty.minX, dirty.minY, dirty.maxX, dirty.maxY};
    snapshot->dirtyHistory.reserve(terrain.map().dirtyHistory().size());
    for (const game::terrain::TerrainDirtyRevision& entry : terrain.map().dirtyHistory()) {
        snapshot->dirtyHistory.push_back({
            .revision = entry.revision,
            .region = {entry.region.minX, entry.region.minY, entry.region.maxX, entry.region.maxY},
        });
    }
    snapshot->waterRevision = terrain.waterRevision();
    snapshot->waterAreas.reserve(terrain.waterAreas().size());
    for (const game::terrain::TerrainWaterArea& source : terrain.waterAreas()) {
        render::TerrainWaterRenderArea area;
        area.triggerId = source.triggerId;
        area.name = source.name;
        area.river = source.river;
        area.riverStart = source.riverStart;
        area.synthesizedLegacyWater = source.synthesizedLegacyWater;
        area.surfaceHeight = math::q32_32::from_raw(
            source.surfaceHeightRaw).to_float();
        area.targetHeight = math::q32_32::from_raw(
            source.targetHeightRaw).to_float();
        area.damageAmount = math::q32_32::from_raw(
            source.damageAmountRaw).to_float();
        area.transitioning = source.isTransitioning();
        area.polygon.reserve(source.polygon.size());
        for (const math::int3& point : source.polygon) {
            area.polygon.push_back({static_cast<float>(point.x), static_cast<float>(point.y),
                                    static_cast<float>(point.z)});
        }
        snapshot->waterAreas.push_back(std::move(area));
    }
    for (const game::terrain::MapObjectRecord& object :
         terrain.map().heightfield().objects) {
        const std::optional<float> type = numericMapProperty(
            object.properties, "scorchType");
        const std::optional<float> radius = numericMapProperty(
            object.properties, "objectRadius");
        if (!type || !radius || !std::isfinite(*radius) || *radius <= 0.0f ||
            !std::isfinite(object.position.x()) || !std::isfinite(object.position.y())) {
            continue;
        }
        snapshot->scorches.push_back({
            .position = object.position,
            .radius = std::clamp(*radius, 0.15f, 128.0f),
            .type = static_cast<int32_t>(*type),
        });
    }
    for (const game::terrain::MapObjectRecord& object :
         terrain.map().heightfield().objects) {
        const std::optional<float> heightAbove = numericMapProperty(
            object.properties, "lightHeightAboveTerrain");
        const std::optional<float> ambient = numericMapProperty(
            object.properties, "lightAmbientColor");
        const std::optional<float> diffuse = numericMapProperty(
            object.properties, "lightDiffuseColor");
        const std::optional<float> inner = numericMapProperty(
            object.properties, "lightInnerRadius");
        const std::optional<float> outer = numericMapProperty(
            object.properties, "lightOuterRadius");
        if (!heightAbove || !ambient || !diffuse || !inner || !outer ||
            !std::isfinite(*inner) || !std::isfinite(*outer) ||
            *outer <= 0.0f) continue;
        math::vec3 position = object.position;
        if (position.z() < 0.0f) {
            position[2] = terrain.groundHeight(position.x(), position.y()) +
                *heightAbove;
        }
        const float outerRadius = std::clamp(*outer, 0.01f, 4096.0f);
        snapshot->pointLights.push_back({
            .position = position,
            .ambient = packedMapRgb(*ambient),
            .diffuse = packedMapRgb(*diffuse),
            .innerRadius = std::clamp(*inner, 0.0f, outerRadius),
            .outerRadius = outerRadius,
        });
    }
    constexpr int32_t kRoadPoint1 = 0x00000002;
    constexpr int32_t kRoadPoint2 = 0x00000004;
    constexpr int32_t kRoadCornerAngled = 0x00000008;
    constexpr int32_t kRoadCornerTight = 0x00000040;
    constexpr int32_t kRoadJoin = 0x00000080;
    constexpr int32_t kBridgePoint1 = 0x00000010;
    constexpr int32_t kBridgePoint2 = 0x00000020;
    const auto& mapObjects = terrain.map().heightfield().objects;
    for (size_t index = 0; index + 1u < mapObjects.size(); ++index) {
        const game::terrain::MapObjectRecord& first = mapObjects[index];
        if ((first.flags & kRoadPoint1) == 0) continue;
        const game::terrain::MapObjectRecord& second = mapObjects[index + 1u];
        if ((second.flags & kRoadPoint2) == 0) continue;
        render::TerrainRoadRenderSegment road;
        road.styleName = first.name;
        // RefCode assigns unresolved road records uniqueID 1.  A resolved
        // presentation style replaces this with its stable catalog identity.
        road.styleIdentity = 1u;
        road.start = first.position;
        road.end = second.position;
        if (road.start.x() == road.end.x() && road.start.y() == road.end.y()) {
            road.end[0] += 0.25f;
        }
        road.startAngled = (first.flags & kRoadCornerAngled) != 0;
        road.endAngled = (second.flags & kRoadCornerAngled) != 0;
        road.startJoin = (first.flags & kRoadJoin) != 0;
        road.endJoin = (second.flags & kRoadJoin) != 0;
        road.tightCorner = (first.flags & kRoadCornerTight) != 0;
        snapshot->roads.push_back(std::move(road));
        ++index;
    }
    uint32_t bridgeId = 1u;
    for (size_t index = 0; index + 1u < mapObjects.size(); ++index) {
        const game::terrain::MapObjectRecord& first = mapObjects[index];
        if ((first.flags & kBridgePoint1) == 0) continue;
        const game::terrain::MapObjectRecord& second = mapObjects[index + 1u];
        if ((second.flags & kBridgePoint2) == 0) continue;
        if (terrain.isBridgeDestroyed(static_cast<uint64_t>(index))) {
            ++index;
            continue;
        }
        render::TerrainBridgeRenderData bridge;
        bridge.bridgeId = bridgeId++;
        bridge.sourceRecordIndex = static_cast<uint64_t>(index);
        bridge.styleName = first.name;
        bridge.start = first.position;
        bridge.end = second.position;
        bridge.start[2] = terrain.groundHeight(
            bridge.start.x(), bridge.start.y()) + 0.25f;
        bridge.end[2] = terrain.groundHeight(
            bridge.end.x(), bridge.end.y()) + 0.25f;
        if (const std::optional<float> state = numericMapProperty(
                first.properties, "damageState")) {
            const int32_t slot = std::clamp(
                static_cast<int32_t>(*state), 0, 3);
            bridge.damageState = static_cast<render::TerrainBridgeDamageState>(
                slot);
        }
        snapshot->bridges.push_back(std::move(bridge));
        ++index;
    }
    return snapshot;
}

[[nodiscard]] container::Vector<TerrainBridgeAuthoritativeRenderState>
collectTerrainBridgeAuthoritativeStates(const ecs::registry& registry) {
    container::Vector<TerrainBridgeAuthoritativeRenderState> states;
    const auto view = ecs::view<
        const ObjectIdentityComponent,
        const MapObjectProvenanceComponent,
        const ObjectBridgeComponent,
        const ObjectHealthComponent>(registry);
    states.reserve(view.size_hint());
    constexpr int32_t kBridgePoint1 = 0x00000010;
    for (const ecs::entity entity : view) {
        const MapObjectProvenanceComponent& provenance =
            view.template get<MapObjectProvenanceComponent>(entity);
        if ((provenance.mapFlags & kBridgePoint1) == 0) continue;
        const ObjectIdentityComponent& identity =
            view.template get<ObjectIdentityComponent>(entity);
        const ObjectHealthComponent& health =
            view.template get<ObjectHealthComponent>(entity);
        states.push_back({
            .sourceRecordIndex = provenance.sourceRecordIndex,
            .objectId = identity.id.value,
            .damageState = static_cast<render::TerrainBridgeDamageState>(
                health.damageState),
        });
    }
    std::sort(states.begin(), states.end(),
              [](const TerrainBridgeAuthoritativeRenderState& left,
                 const TerrainBridgeAuthoritativeRenderState& right) {
                  if (left.sourceRecordIndex != right.sourceRecordIndex) {
                      return left.sourceRecordIndex < right.sourceRecordIndex;
                  }
                  return left.objectId < right.objectId;
              });
    return states;
}

[[nodiscard]] uint64_t terrainBridgeStateIdentity(
    container::Span<const TerrainBridgeAuthoritativeRenderState> states) noexcept {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value) {
        for (size_t byte = 0; byte < sizeof(value); ++byte) {
            hash ^= static_cast<uint8_t>(value >> (byte * 8u));
            hash *= 1099511628211ull;
        }
    };
    mix(states.size());
    for (const TerrainBridgeAuthoritativeRenderState& state : states) {
        mix(state.sourceRecordIndex);
        mix(state.objectId);
        mix(static_cast<uint64_t>(state.damageState));
    }
    return hash != 0u ? hash : 1u;
}


} // namespace

container::SharedPtr<const render::TerrainRenderSnapshot> GameRenderExtraction::extractTerrain(
    const game::terrain::TerrainLogic& terrain) {
    return buildTerrainSnapshot(terrain);
}

void GameRenderExtraction::applyTerrainPresentation(
    render::TerrainRenderSnapshot& terrain,
    const script::ScriptWaterPresentationSettings& water,
    const script::ScriptTerrainRoadPresentationSettings& roads,
    int32_t waterType) {
    const size_t waterTimeOfDaySlot = terrain.globalLighting
        ? terrain.globalLighting->terrainLightSlot() : 0u;
    terrain.waterMaterial = copyTerrainWaterMaterial(water, waterTimeOfDaySlot);
    terrain.waterMaterial->waterType = waterType;
    const auto reflectionPlane = std::find_if(
        terrain.waterAreas.begin(), terrain.waterAreas.end(),
        [](const render::TerrainWaterRenderArea& area) {
            return !area.river && area.synthesizedLegacyWater &&
                std::isfinite(area.surfaceHeight);
        });
    const auto fallbackPlane = reflectionPlane != terrain.waterAreas.end()
        ? reflectionPlane
        : std::find_if(
              terrain.waterAreas.begin(), terrain.waterAreas.end(),
              [](const render::TerrainWaterRenderArea& area) {
                  return !area.river && std::isfinite(area.surfaceHeight);
              });
    if (waterType == 2 && fallbackPlane != terrain.waterAreas.end()) {
        terrain.waterMaterial->reflectionPlaneZ = fallbackPlane->surfaceHeight;
        terrain.waterMaterial->hasReflectionPlane = true;
    }
    for (render::TerrainRoadRenderSegment& road : terrain.roads) {
        if (const script::ScriptTerrainRoadStyle* style = roads.find(road.styleName)) {
            road.styleIdentity = style->identity;
            road.textureName = style->texture;
            road.width = style->width;
            road.widthInTexture = style->widthInTexture;
        }
    }
    for (render::TerrainBridgeRenderData& bridge : terrain.bridges) {
        const script::ScriptTerrainBridgeStyle* style =
            roads.findBridge(bridge.styleName);
        if (!style) continue;
        bridge.scale = style->scale;
        bridge.bridgeWidth = 34.0f * style->scale;
        bridge.radarColor = style->radarColor;
        bridge.hasRadarColor = true;
        bridge.modelNames = style->modelNames;
        bridge.textureNames = style->textureNames;
        bridge.towerObjectNames = style->towerObjectNames;
        bridge.scaffoldObjectName = style->scaffoldObjectName;
        bridge.scaffoldSupportObjectName =
            style->scaffoldSupportObjectName;
        const math::vec3 delta = bridge.end - bridge.start;
        const float length = delta.length();
        bridge.boundsCenter = (bridge.start + bridge.end) * 0.5f;
        bridge.boundsExtents = {
            length * 0.5f,
            bridge.bridgeWidth * 0.5f,
            std::max(bridge.bridgeWidth * 0.5f,
                     std::abs(delta.z()) * 0.5f + 4.0f),
        };
    }
}

void GameRenderExtraction::applyTerrainBridgeAuthoritativeStates(
    render::TerrainRenderSnapshot& terrain,
    container::Span<const TerrainBridgeAuthoritativeRenderState> states) {
    for (render::TerrainBridgeRenderData& bridge : terrain.bridges) {
        const auto found = std::lower_bound(
            states.begin(), states.end(), bridge.sourceRecordIndex,
            [](const TerrainBridgeAuthoritativeRenderState& state,
               uint64_t sourceRecordIndex) {
                return state.sourceRecordIndex < sourceRecordIndex;
            });
        if (found == states.end() ||
            found->sourceRecordIndex != bridge.sourceRecordIndex) {
            continue;
        }
        bridge.authoritativeObjectId = found->objectId;
        bridge.damageState = found->damageState;
    }
}

void GameRenderExtraction::appendClientTerrainObjects(
    const ClientTerrainObjectStore& source,
    render::WorldRenderSnapshot& destination) {
    const container::Span<const ClientTerrainObject> objects =
        source.objects();
    size_t channelCapacity = 0;
    for (const ClientTerrainObject& object : objects) {
        if (object.treeState == ClientTerrainTreeState::Removed) continue;
        channelCapacity += object.treeState == ClientTerrainTreeState::Stump
            ? 1u : object.visuals.size();
    }
    destination.entities.reserve(
        destination.entities.size() + channelCapacity);

    for (const ClientTerrainObject& object : objects) {
        if (object.id == 0 ||
            object.treeState == ClientTerrainTreeState::Removed) {
            continue;
        }
        const render::LocalVisibilityRenderCellState visibility =
            (destination.localVisibility.enabled ||
             destination.localVisibility.hasPlayableBounds())
            ? destination.localVisibility.worldStateSphere(
                  object.position, object.boundingRadius)
            : render::LocalVisibilityRenderCellState::Visible;
        // W3DPropBuffer draws fogged/explored props through its shroud
        // material pass and rejects only fully shrouded props.  The modern
        // world shroud is applied later, so keep explored client decorations
        // in the sealed scene instead of treating them as ordinary Object
        // memory/ghosts.
        const bool fullyShrouded = visibility ==
            render::LocalVisibilityRenderCellState::Shrouded;
        if (fullyShrouded) {
            ++destination.localVisibility.hiddenEntityCount;
        }

        const math::quat yaw = math::quat::from_axis_angle(
            {0.0f, 0.0f, 1.0f}, object.yawRadians);
        math::quat orientation = yaw;
        if (object.treeState == ClientTerrainTreeState::Falling ||
            object.treeState == ClientTerrainTreeState::Down) {
            const math::vec3 axis{
                -object.toppleDirection.y(), object.toppleDirection.x(), 0.0f};
            orientation = (math::quat::from_axis_angle(
                axis, object.toppleRadians) * yaw).normalized();
        }
        const bool stump =
            object.treeState == ClientTerrainTreeState::Stump;
        const float renderScale = stump ? object.stumpScale : object.scale;
        const float renderRadius = stump
            ? object.stumpBoundingRadius : object.boundingRadius;
        const render::RenderEntityId objectId =
            clientTerrainObjectId(object.id);
        const bool objectReceivesDynamicLights = stump || std::all_of(
            object.visuals.begin(), object.visuals.end(),
            [](const ClientTerrainVisualChannel& channel) {
                return channel.modelAsset.empty() ||
                    channel.receivesDynamicLights;
            });

        const auto appendChannel = [&](container::StringView modelAsset,
                                       uint32_t channelIndex,
                                       const ClientTerrainVisualChannel* visual) {
            if (modelAsset.empty()) return;
            render::RenderEntitySnapshot output;
            output.id = clientTerrainInstanceId(object.id, channelIndex);
            output.objectId = objectId;
            output.channelIndex = channelIndex;
            output.modelAsset = container::String(modelAsset);
            output.transform.position = object.position + math::vec3{
                0.0f, 0.0f, -object.sinkOffset};
            output.transform.orientation = orientation;
            output.transform.scale = {
                renderScale, renderScale, renderScale};
            output.boundingRadius = renderRadius;
            output.visual.modelConditionFlags = object.modelConditions.words;
            output.visual.treeSwayEnabled = object.treeSwayEnabled &&
                object.treeState == ClientTerrainTreeState::Upright;
            if (object.kind == ClientTerrainObjectKind::OptimizedTree &&
                !stump) {
                output.visual.treeTextureAsset = object.treeTextureAsset;
                output.visual.treePushAsideDirection =
                    object.pushAsideDirection;
                output.visual.treePushAsideAmount = object.pushAsideAmount;
                output.visual.treePushAsideDistanceFactor =
                    object.moveOutwardDistanceFactor;
                output.visual.treePushAsideDarkeningFactor =
                    object.darkeningFactor;
            }
            output.visual.hidden = fullyShrouded;
            // Drawable::getReceivesDynamicLights ANDs every enabled Draw
            // module. A client-only prop/tree still represents one Drawable,
            // so all emitted channels must consume the same aggregate rather
            // than their module-local flag.
            output.visual.receivesDynamicLights =
                objectReceivesDynamicLights;
            output.localVisibilityState = visibility;
            output.hiddenByLocalVisibility = fullyShrouded;
            output.visual.receivesLocalVisibility =
                destination.localVisibility.enabled;
            if (visual) {
                output.visual.animationState = visual->animationState;
                output.visual.animationMode =
                    toRenderAnimationMode(visual->animationMode);
                output.visual.subObjectVisibility.reserve(
                    visual->subObjectVisibility.size());
                for (const game::ModelSubObjectVisibility& sourceVisibility :
                     visual->subObjectVisibility) {
                    output.visual.subObjectVisibility.push_back({
                        .name = sourceVisibility.name,
                        .visible = sourceVisibility.visible,
                    });
                }
            }
            const bool treeShadowSuppressed =
                object.kind == ClientTerrainObjectKind::OptimizedTree &&
                object.treeState != ClientTerrainTreeState::Upright;
            if (!treeShadowSuppressed && !stump) {
                output.shadow = {
                    .typeMask = render::filterRenderShadowTypeMask(
                        object.shadow.typeMask,
                        !destination.renderFeatureQuality ||
                            destination.renderFeatureQuality->requested
                                .useShadowVolumes,
                        !destination.renderFeatureQuality ||
                            destination.renderFeatureQuality->requested
                                .useShadowDecals),
                    .textureName = object.shadow.texture,
                    .sizeX = object.shadow.sizeX,
                    .sizeY = object.shadow.sizeY,
                    .offsetX = object.shadow.offsetX,
                    .offsetY = object.shadow.offsetY,
                };
            }
            destination.entities.push_back(std::move(output));
        };

        if (stump) {
            appendChannel(object.stumpModelAsset, 0, nullptr);
            continue;
        }
        for (const ClientTerrainVisualChannel& visual : object.visuals) {
            appendChannel(
                visual.modelAsset, visual.sourceChannelIndex, &visual);
        }
    }
}


void GameRenderExtraction::extractTerrainAdmission(
    const GameRenderTerrainExtractionSource& source,
    render::WorldRenderSnapshot& snapshot) {
    GameRenderTerrainExtractionCache& cache = source.cache;
    const RenderFeatureQualitySettings featureQuality =
        snapshot.renderFeatureQuality
        ? snapshot.renderFeatureQuality->requested
        : RenderFeatureQualitySettings{};
    if (source.terrain.isLoaded()) {
        const container::Vector<TerrainBridgeAuthoritativeRenderState>
            terrainBridgeStates =
                collectTerrainBridgeAuthoritativeStates(source.registry);
        const uint64_t bridgeStateIdentity = terrainBridgeStateIdentity(
            terrainBridgeStates);
        const uint64_t mapRevision = source.terrain.map().revision();
        const uint64_t layoutRevision = source.terrain.map().layoutRevision();
        const uint64_t waterRevision = source.terrain.waterRevision();
        const uint64_t borderRevision =
            source.mapPresentation.borderShroudStamp().sequence;
        const size_t activeBoundary = source.terrain.map().activeBoundary();
        const bool cacheMatches = cache.snapshot &&
            cache.mapRevision == mapRevision &&
            cache.layoutRevision == layoutRevision &&
            cache.waterRevision == waterRevision &&
            cache.borderRevision == borderRevision &&
            cache.presentationEpoch == source.presentationEpoch &&
            cache.activeBoundary == activeBoundary &&
            cache.bridgeStateIdentity == bridgeStateIdentity;
        if (cacheMatches) {
            snapshot.terrain = cache.snapshot;
        } else if (auto terrain = buildTerrainSnapshot(source.terrain)) {
            const bool terrainPresentationChanged =
                cache.presentationRevision == 0 ||
                cache.borderRevision != borderRevision ||
                cache.presentationEpoch != source.presentationEpoch ||
                cache.activeBoundary != activeBoundary;
            if (terrainPresentationChanged) {
                ++cache.presentationRevision;
                if (cache.presentationRevision == 0) {
                    ++cache.presentationRevision;
                }
            }
            const game::terrain::TerrainExtent playable =
                source.terrain.map().playableExtent();
            terrain->playableMinimum = playable.minimum;
            terrain->playableMaximum = playable.maximum;
            terrain->borderShroudEnabled =
                source.mapPresentation.borderShroudEnabled();
            terrain->borderShroudRevision = cache.presentationRevision;
            applyTerrainPresentation(
                *terrain, source.waterPresentation, source.roadPresentation,
                source.renderSettings.visual.water.waterType);
            for (render::TerrainBridgeRenderData& bridge : terrain->bridges) {
                for (size_t tower = 0;
                     tower < bridge.towerObjectNames.size(); ++tower) {
                    if (bridge.towerObjectNames[tower].empty()) continue;
                    const container::SharedPtr<const game::ObjectArchetype>
                        towerArchetype = source.content.findObjectArchetype(
                            bridge.towerObjectNames[tower]);
                    if (towerArchetype) {
                        bridge.towerModelAssets[tower] =
                            towerArchetype->templateData.defaultW3dModel;
                    }
                }
            }
            applyTerrainBridgeAuthoritativeStates(
                *terrain, terrainBridgeStates);
            terrain->bridgeRevision = bridgeStateIdentity;
            if (terrain->waterMaterial) {
                terrain->waterMaterial->showSoftEdge =
                    featureQuality.showSoftWaterEdge;
            }
            terrain->vertexWater = selectTerrainVertexWater(
                source.terrain.contentIdentity().resolvedPath,
                source.renderSettings.visual.water,
                source.terrain.vertexWaterState(),
                source.terrain.waypointByName("WaveGuide1") != nullptr);
            cache.snapshot = std::move(terrain);
            cache.mapRevision = mapRevision;
            cache.layoutRevision = layoutRevision;
            cache.waterRevision = waterRevision;
            cache.borderRevision = borderRevision;
            cache.presentationEpoch = source.presentationEpoch;
            cache.activeBoundary = activeBoundary;
            cache.bridgeStateIdentity = bridgeStateIdentity;
            snapshot.terrain = cache.snapshot;
        }
    } else {
        cache = {};
    }

}

void GameRenderExtraction::finalizeAssembly(
    const GameRenderTerrainExtractionSource& source,
    render::WorldRenderSnapshot& snapshot) {
    const RenderFeatureQualitySettings featureQuality =
        snapshot.renderFeatureQuality
        ? snapshot.renderFeatureQuality->requested
        : RenderFeatureQualitySettings{};
    const container::Vector<selection::LocalPlacementPreviewSnapshot>
        localPlacements = source.localPlacement.snapshots();
    const selection::LocalPlacementPreviewSnapshot localPlacement =
        localPlacements.empty()
        ? selection::LocalPlacementPreviewSnapshot{}
        : localPlacements.front();
    if (localPlacement.previewIdentity && localPlacement.hasPose &&
        !localPlacement.objectType.empty()) {
        const container::SharedPtr<const game::ObjectArchetype> product =
            source.content.findObjectArchetype(
                localPlacement.objectType);
        if (product) {
            render::RenderVector authorPlayerColor{1.0f, 1.0f, 1.0f};
            uint8_t terrainTimeOfDay = 0;
            if (source.terrain.isLoaded()) {
                const auto& heightfield =
                    source.terrain.map().heightfield();
                if (heightfield.globalLighting) {
                    terrainTimeOfDay = static_cast<uint8_t>(
                        heightfield.globalLighting->timeOfDay);
                }
            }
            const std::optional<PlayerId> author =
                source.ownership.ownerOf(localPlacement.sourceObject);
            const PlayerState* player = author
                ? source.players.get(*author) : nullptr;
            if (player) {
                const bool night = terrainTimeOfDay != 0
                    ? terrainTimeOfDay == 4
                    : source.renderSettings.visual.defaultTimeOfDay ==
                          RenderTimeOfDay::Night;
                if (const std::optional<PlayerRgbColor> authoredColor =
                        source.ruleset.presentationColor(
                            *player, night)) {
                    authorPlayerColor = {
                        static_cast<float>(authoredColor->red) / 255.0f,
                        static_cast<float>(authoredColor->green) / 255.0f,
                        static_cast<float>(authoredColor->blue) / 255.0f,
                    };
                }
            }
            for (const selection::LocalPlacementPreviewSnapshot& tile :
                 localPlacements) {
                appendLocalPlacementPreviewEntities(
                    tile, product->templateData, authorPlayerColor,
                    source.renderSettings, featureQuality,
                    snapshot.entities.mutableValues(),
                    terrainTimeOfDay);
            }
        }
    }
    source.localPlacement.appendTerrainBibs(
        snapshot.terrainBibs.mutableValues());

    // Retail keeps the ordinary move hint alive for 40 client frames. Use
    // the same confirmed-frame window for command target/line feedback;
    // holding Shift bypasses the timeout and exposes the complete queue.
    constexpr uint64_t kCommandHintFrames = 40;
    const auto recentFrame = [&](uint64_t frame) noexcept {
        return frame != 0 && frame <= source.confirmedTick &&
            source.confirmedTick - frame <= kCommandHintFrames;
    };
    const auto showConstructionWaypoint =
        [&](const selection::LocalPlacementPreviewSnapshot& placement) {
            return source.showPlayerWaypoints ||
                recentFrame(placement.animationStartTick);
        };

    const auto appendConfirmedConstructionPreview =
        [&](const selection::LocalPlacementPreviewSnapshot& placement) {
            if (placement.routeAnchorOnly) return;
            if (!placement.previewIdentity || !placement.hasPose ||
                placement.objectType.empty()) {
                return;
            }
            const container::SharedPtr<const game::ObjectArchetype> product =
                source.content.findObjectArchetype(placement.objectType);
            if (!product) return;

            render::RenderVector playerColor{1.0f, 1.0f, 1.0f};
            const std::optional<PlayerId> author =
                source.ownership.ownerOf(placement.sourceObject);
            const PlayerState* player = author
                ? source.players.get(*author) : nullptr;
            uint8_t terrainTimeOfDay = 0;
            if (source.terrain.isLoaded()) {
                const auto& heightfield =
                    source.terrain.map().heightfield();
                if (heightfield.globalLighting) {
                    terrainTimeOfDay = static_cast<uint8_t>(
                        heightfield.globalLighting->timeOfDay);
                }
            }
            if (player) {
                const bool night = terrainTimeOfDay != 0
                    ? terrainTimeOfDay == 4
                    : source.renderSettings.visual.defaultTimeOfDay ==
                          RenderTimeOfDay::Night;
                if (const std::optional<PlayerRgbColor> authoredColor =
                        source.ruleset.presentationColor(*player, night)) {
                    playerColor = {
                        static_cast<float>(authoredColor->red) / 255.0f,
                        static_cast<float>(authoredColor->green) / 255.0f,
                        static_cast<float>(authoredColor->blue) / 255.0f,
                    };
                }
            }
            appendLocalPlacementPreviewEntities(
                placement, product->templateData, playerColor,
                source.renderSettings, featureQuality,
                snapshot.entities.mutableValues(), terrainTimeOfDay);

            const render::TerrainBibTint tint =
                placement.legality ==
                        selection::LocalPlacementLegality::Illegal ||
                    placement.feedback ==
                        selection::LocalPlacementPreviewFeedback::Rejected
                ? render::TerrainBibTint::Red
                : render::TerrainBibTint::Yellow;
            const game::ThingTemplate& data = product->templateData;
            if (placement.feedback ==
                    selection::LocalPlacementPreviewFeedback::Queued &&
                showConstructionWaypoint(placement)) {
                snapshot.objectUi.waypoints.mutableValues().push_back({
                    .identity = placement.previewIdentity,
                    .actorObjectId = placement.sourceObject.value,
                    .sourceSequence = placement.sourceSequence,
                    .kind = render::OrderWaypointRenderKind::Build,
                    .color = render::OrderWaypointRenderColor::Blue,
                    .worldPosition = placement.position,
                    .selectionRadius = std::max(
                        4.0f, data.geometry.boundingCircleRadiusFixed.to_float()),
                    .rejected = tint == render::TerrainBibTint::Red,
                });
            }
            const render::TerrainBibFootprintInput footprint{
                .ownerIdentity = placement.previewIdentity,
                .kind = render::TerrainBibKind::PlacementPreview,
                .position = placement.position,
                .yawRadians = placement.yawRadians,
                .geometryMajorRadius =
                    data.geometry.majorRadiusFixed.to_float(),
                .geometryMinorRadius =
                    data.geometry.minorRadiusFixed.to_float(),
                .geometryIsBox = data.geometry.type ==
                    game::ObjectGeometryType::Box,
                .factoryExitWidth =
                    data.factoryExitWidthFixed.to_float(),
                .factoryExtraBibWidth =
                    data.factoryExtraBibWidthFixed.to_float(),
                .highlighted = tint == render::TerrainBibTint::Red,
                .tint = tint,
                .receivesVisibility = true,
            };
            if (std::optional<render::TerrainBibRenderData> bib =
                    render::buildTerrainBibFootprint(footprint)) {
                snapshot.terrainBibs.mutableValues().push_back(
                    std::move(*bib));
            }
        };
    for (const selection::LocalPlacementPreviewSnapshot& placement :
         source.queuedConstructionPlacements) {
        appendConfirmedConstructionPreview(placement);
    }
    for (const selection::TimedLocalPlacementPreview& timed :
         source.rejectedConstructionPlacements) {
        if (timed.expiresAfterTick > source.confirmedTick) {
            appendConfirmedConstructionPreview(timed.placement);
        }
    }

    const auto visibleObjectUi =
        [&](ObjectId object) -> const render::ObjectUiRenderSnapshot* {
            if (!object) return nullptr;
            const auto& objects = snapshot.objectUi.objects;
            const auto found = std::find_if(
                objects.begin(), objects.end(),
                [&](const render::ObjectUiRenderSnapshot& value) {
                    return value.objectId == object.value;
                });
            if (found == objects.end() ||
                found->visibility !=
                    render::LocalVisibilityRenderCellState::Visible ||
                (found->relationship ==
                     render::ObjectUiRelationship::Enemy &&
                 found->stealthed && !found->detected)) {
                return nullptr;
            }
            return &*found;
        };

    const auto appendConstructionRouteSegment =
        [&](const selection::LocalPlacementPreviewSnapshot& start,
            const selection::LocalPlacementPreviewSnapshot& end,
            render::OrderWaypointRenderColor color) {
            if (!source.showPlayerWaypoints &&
                !showConstructionWaypoint(end)) {
                return;
            }
            const float deltaX = end.position.x() - start.position.x();
            const float deltaY = end.position.y() - start.position.y();
            const float length = std::hypot(deltaX, deltaY);
            if (!std::isfinite(length) || length <= 0.001f) return;
            const bool rejected =
                start.legality ==
                    selection::LocalPlacementLegality::Illegal ||
                end.legality ==
                    selection::LocalPlacementLegality::Illegal;
            snapshot.objectUi.waypointSegments.mutableValues().push_back({
                .identity = start.previewIdentity ^
                    (end.previewIdentity * 1099511628211ull),
                .actorObjectId = start.sourceObject.value,
                .sourceSequence = end.sourceSequence,
                .kind = render::OrderWaypointRenderKind::Build,
                .color = color,
                .startWorldPosition = start.position,
                .endWorldPosition = end.position,
                .rejected = rejected,
            });
        };
    const selection::LocalPlacementPreviewSnapshot* previousRoutePoint =
        nullptr;
    for (const selection::LocalPlacementPreviewSnapshot& placement :
         source.queuedConstructionPlacements) {
        if (previousRoutePoint &&
            previousRoutePoint->sourceObject == placement.sourceObject) {
            appendConstructionRouteSegment(
                *previousRoutePoint, placement,
                render::OrderWaypointRenderColor::Orange);
        } else if (const render::ObjectUiRenderSnapshot* builder =
                       visibleObjectUi(placement.sourceObject)) {
            const float deltaX = placement.position.x() -
                builder->worldPosition.x();
            const float deltaY = placement.position.y() -
                builder->worldPosition.y();
            if (std::hypot(deltaX, deltaY) > 0.001f) {
                snapshot.objectUi.waypointSegments.mutableValues().push_back({
                    .identity = placement.previewIdentity ^
                        (static_cast<uint64_t>(
                             placement.sourceObject.value) *
                         1099511628211ull),
                    .actorObjectId = placement.sourceObject.value,
                    .sourceSequence = placement.sourceSequence,
                    .kind = render::OrderWaypointRenderKind::Build,
                    .color = render::OrderWaypointRenderColor::Blue,
                    .startWorldPosition = builder->worldPosition,
                    .endWorldPosition = placement.position,
                    .rejected = placement.legality ==
                        selection::LocalPlacementLegality::Illegal,
                });
            }
        }
        previousRoutePoint = &placement;
    }
    if (!localPlacements.empty()) {
        const auto lastQueued = std::find_if(
            source.queuedConstructionPlacements.rbegin(),
            source.queuedConstructionPlacements.rend(),
            [&](const selection::LocalPlacementPreviewSnapshot& placement) {
                return placement.sourceObject ==
                    localPlacements.front().sourceObject;
            });
        if (lastQueued != source.queuedConstructionPlacements.rend()) {
            appendConstructionRouteSegment(
                *lastQueued, localPlacements.front(),
                render::OrderWaypointRenderColor::Orange);
        } else if (const render::ObjectUiRenderSnapshot* builder =
                       visibleObjectUi(
                           localPlacements.front().sourceObject)) {
            const selection::LocalPlacementPreviewSnapshot anchor{
                .previewIdentity = static_cast<uint64_t>(
                    localPlacements.front().sourceObject.value),
                .sourceObject = localPlacements.front().sourceObject,
                .sourceSequence = localPlacements.front().sourceSequence,
                .position = builder->worldPosition,
                .hasPose = true,
            };
            appendConstructionRouteSegment(
                anchor, localPlacements.front(),
                render::OrderWaypointRenderColor::Blue);
        }
    }

    const auto waypointIdentity = [](ObjectId actor, uint32_t sequence,
                                     render::OrderWaypointRenderKind kind) {
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&](uint64_t value) {
            constexpr uint64_t prime = 1099511628211ull;
            for (uint32_t byte = 0; byte < 8u; ++byte) {
                hash ^= static_cast<uint8_t>(value >> (byte * 8u));
                hash *= prime;
            }
        };
        mix(actor.value);
        mix(sequence);
        mix(static_cast<uint8_t>(kind));
        return hash != 0 ? hash : uint64_t{1};
    };
    const auto appendWaypointLine =
        [&](render::RenderEntityId identity,
            ObjectId actor,
            uint32_t sourceSequence,
            const render::RenderVector& start,
            const render::RenderVector& end,
            render::OrderWaypointRenderKind kind,
            render::OrderWaypointRenderColor color) {
            const float deltaX = end.x() - start.x();
            const float deltaY = end.y() - start.y();
            const float length = std::hypot(deltaX, deltaY);
            if (!std::isfinite(length) || length <= 0.001f) return;
            snapshot.objectUi.waypointSegments.mutableValues().push_back({
                .identity = identity,
                .actorObjectId = actor.value,
                .sourceSequence = sourceSequence,
                .kind = kind,
                .color = color,
                .startWorldPosition = start,
                .endWorldPosition = end,
            });
        };
    const auto waypointKind = [](const ObjectOrderIntent& order)
        -> std::optional<render::OrderWaypointRenderKind> {
        switch (order.kind) {
        case ObjectOrderKind::Move:
            return order.attackMove
                ? render::OrderWaypointRenderKind::AttackMove
                : render::OrderWaypointRenderKind::Move;
        case ObjectOrderKind::Attack:
            return render::OrderWaypointRenderKind::Attack;
        case ObjectOrderKind::Build:
            return render::OrderWaypointRenderKind::Build;
        case ObjectOrderKind::TacticalAttack:
            return order.tacticalAttackSubtype ==
                    ObjectTacticalAttackSubtype::Guard
                ? std::optional<render::OrderWaypointRenderKind>{
                      render::OrderWaypointRenderKind::Guard}
                : std::nullopt;
        case ObjectOrderKind::SpecialPower:
        case ObjectOrderKind::CommandButton:
            return render::OrderWaypointRenderKind::Ability;
        case ObjectOrderKind::Stop:
            return std::nullopt;
        }
        return std::nullopt;
    };
    const auto semanticWaypointColor =
        [](render::OrderWaypointRenderKind kind) noexcept {
            switch (kind) {
            case render::OrderWaypointRenderKind::Move:
            case render::OrderWaypointRenderKind::Build:
                return render::OrderWaypointRenderColor::Blue;
            case render::OrderWaypointRenderKind::Attack:
                return render::OrderWaypointRenderColor::Red;
            case render::OrderWaypointRenderKind::AttackMove:
                return render::OrderWaypointRenderColor::Orange;
            case render::OrderWaypointRenderKind::Guard:
                return render::OrderWaypointRenderColor::Green;
            case render::OrderWaypointRenderKind::Ability:
                return render::OrderWaypointRenderColor::Yellow;
            }
            return render::OrderWaypointRenderColor::Blue;
        };
    const auto waypointRgb =
        [](render::OrderWaypointRenderColor color) noexcept {
            switch (color) {
            case render::OrderWaypointRenderColor::Blue:
                return render::RenderVector{0.15f, 0.48f, 1.0f};
            case render::OrderWaypointRenderColor::Orange:
                return render::RenderVector{1.0f, 0.42f, 0.05f};
            case render::OrderWaypointRenderColor::Red:
                return render::RenderVector{1.0f, 0.08f, 0.04f};
            case render::OrderWaypointRenderColor::Green:
                return render::RenderVector{0.15f, 0.9f, 0.22f};
            case render::OrderWaypointRenderColor::Yellow:
                return render::RenderVector{1.0f, 0.82f, 0.08f};
            }
            return render::RenderVector{0.15f, 0.48f, 1.0f};
        };

    // Shift and the authored waypoint modifier expose the complete confirmed
    // queue of every object owned by the local player, not merely the current
    // selection. Without Shift, selected actors retain their complete current
    // route regardless of when its commands were issued. Dynamic targets are
    // joined only through observer-filtered ObjectUi; hidden or undetected
    // objects never leak their current transform through a route node.
    const PlayerState* localPlayer = source.players.localPlayer();
    const auto orderView =
        ecs::view<const ObjectIdentityComponent,
                  const ObjectOrderQueueComponent>(source.registry);
    for (const ecs::entity actorEntity : orderView) {
            const ObjectId actor = orderView.template get<
                const ObjectIdentityComponent>(actorEntity).id;
            const bool included = source.showPlayerWaypoints
                ? localPlayer && source.ownership.ownerOf(actor) ==
                      std::optional<PlayerId>{localPlayer->id}
                : std::find(source.localSelection.begin(),
                            source.localSelection.end(), actor) !=
                      source.localSelection.end();
            if (!included) continue;
            const render::ObjectUiRenderSnapshot* actorUi =
                visibleObjectUi(actor);
            if (!actorUi) continue;
            const ObjectOrderQueueComponent& queue =
                orderView.template get<const ObjectOrderQueueComponent>(
                    actorEntity);
            if (queue.orders.empty()) continue;

            size_t firstOrder = 0;
            if (const ObjectSystemPathSequenceComponent* path =
                    ecs::try_get<ObjectSystemPathSequenceComponent>(
                        source.registry, actorEntity);
                path && path->source == ObjectOrderSource::Player &&
                path->routeSubtype == ObjectMoveRouteSubtype::FollowPath &&
                path->systemPurpose == ObjectOrderSystemPurpose::Generic &&
                path->queuedOrderCount != 0 &&
                path->queuedOrderCount <= queue.orders.size()) {
                firstOrder = std::min<size_t>(
                    path->currentPointIndex, path->queuedOrderCount);
            }

            render::RenderVector previous = actorUi->worldPosition;
            size_t emitted = 0;
            for (size_t index = firstOrder;
                 index < queue.orders.size() && emitted < 512u; ++index) {
                const ObjectOrderIntent& order = queue.orders[index];
                if (order.source != ObjectOrderSource::Player ||
                    order.sourceSequence == 0) {
                    continue;
                }
                const std::optional<render::OrderWaypointRenderKind> kind =
                    waypointKind(order);
                if (!kind) continue;

                render::RenderVector target{};
                bool dynamicTarget = false;
                if (order.targetObject) {
                    const render::ObjectUiRenderSnapshot* targetUi =
                        visibleObjectUi(order.targetObject);
                    if (!targetUi) break;
                    target = targetUi->worldPosition;
                    dynamicTarget = true;
                } else if (order.hasTargetPosition) {
                    target = {
                        order.targetX.to_float(), order.targetY.to_float(),
                        order.targetZ.to_float()};
                } else if (*kind ==
                           render::OrderWaypointRenderKind::Ability) {
                    // A no-target queued ability fires where the actor finishes
                    // the preceding route. It adds a gold action node at that
                    // endpoint, but no movement segment and no invented target.
                    target = previous;
                } else {
                    continue;
                }

                const render::OrderWaypointRenderColor segmentColor =
                    semanticWaypointColor(*kind);
                render::OrderWaypointRenderColor nodeColor = segmentColor;
                if ((*kind == render::OrderWaypointRenderKind::Move ||
                     *kind == render::OrderWaypointRenderKind::Build) &&
                    std::any_of(
                        queue.orders.begin() +
                            static_cast<std::ptrdiff_t>(index + 1u),
                        queue.orders.end(),
                        [&](const ObjectOrderIntent& later) {
                            return later.source == ObjectOrderSource::Player &&
                                later.sourceSequence != 0 &&
                                waypointKind(later).has_value();
                        })) {
                    nodeColor = render::OrderWaypointRenderColor::Orange;
                }

                const bool constructionAlreadyPresented =
                    *kind == render::OrderWaypointRenderKind::Build &&
                    std::any_of(
                        source.queuedConstructionPlacements.begin(),
                        source.queuedConstructionPlacements.end(),
                        [&](const selection::LocalPlacementPreviewSnapshot&
                                placement) {
                            return placement.sourceObject == actor &&
                                placement.sourceSequence ==
                                    order.sourceSequence;
                        });
                if (constructionAlreadyPresented) {
                    previous = target;
                    continue;
                }

                const render::RenderEntityId identity = waypointIdentity(
                    actor, order.sourceSequence, *kind);
                appendWaypointLine(
                    identity, actor, order.sourceSequence, previous, target,
                    *kind, segmentColor);
                snapshot.objectUi.waypoints.mutableValues().push_back({
                    .identity = identity,
                    .actorObjectId = actor.value,
                    .sourceSequence = order.sourceSequence,
                    .targetObjectId = order.targetObject.value,
                    .kind = *kind,
                    .color = nodeColor,
                    .worldPosition = target,
                    .selectionRadius = 6.0f,
                    .dynamicTarget = dynamicTarget,
                });
                snapshot.entities.mutableValues().push_back({
                    .id = identity,
                    .modelAsset = "SCMRally",
                    .transform = {.position = target},
                    .visual = {
                        .animationState = "SCMRally.SCMRally",
                        .animationMode = render::RenderAnimationMode::Loop,
                        .hasScriptIndicatorColor = true,
                        .scriptIndicatorColor = waypointRgb(nodeColor),
                    },
                    .boundingRadius = 8.0f,
                    .animationCompletionFeedbackEnabled = false,
                    .interpolationDisabled = true,
                });
                previous = target;
                ++emitted;
            }
        }
    appendClientTerrainObjects(source.clientTerrainObjects, snapshot);
}

} // namespace engine
