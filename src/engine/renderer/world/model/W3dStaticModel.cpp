#include "core/container/hash_containers.h"
#include "engine/renderer/world/model/W3dStaticModel.h"

#include "engine/renderer/world/model/Skeleton.h"
#include "data/w3d/W3dTypes.h"

#include <algorithm>
#include <cctype>
#include <tuple>
namespace engine::render {
namespace {

::container::String boundedString(const char* data, size_t capacity) {
    size_t length = 0;
    while (length < capacity && data[length] != '\0') ++length;
    return ::container::String(data, length);
}

::container::String lowerAscii(::container::String value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

::container::String prototypeKey(::container::String value) {
    const size_t slash = value.find_last_of("/\\");
    if (slash != ::container::String::npos) value.erase(0, slash + 1);
    value = lowerAscii(std::move(value));
    // A dot is part of an exact WW3D mesh prototype (for example
    // AVHUMMER.CHASSIS), not a generic filename extension.
    constexpr ::container::StringView w3dExtension = ".w3d";
    if (value.size() >= w3dExtension.size() &&
        value.compare(value.size() - w3dExtension.size(), w3dExtension.size(), w3dExtension) == 0) {
        value.resize(value.size() - w3dExtension.size());
    }
    return value;
}

::container::String meshFullName(const data::w3d::ParsedMesh& mesh) {
    const ::container::String container = boundedString(mesh.containerName, data::w3d::NAME_LEN);
    const ::container::String name = boundedString(mesh.name, data::w3d::NAME_LEN);
    return container.empty() ? name : container + "." + name;
}

bool isRenderableMesh(const data::w3d::ParsedMesh& mesh,
                      const W3dStaticModelBuildOptions& options) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) return false;
    if (!options.includeHiddenMeshes && (mesh.attributes & data::w3d::MESH_FLAG_HIDDEN) != 0) {
        return false;
    }
    if (!options.includeCollisionMeshes &&
        (mesh.attributes & data::w3d::MESH_FLAG_COLLISION_BOX) != 0) {
        return false;
    }
    const bool hasSkinGeometry = (mesh.attributes & (data::w3d::MESH_FLAG_SKIN |
                                                      data::w3d::MESH_FLAG_GEOMETRY_TYPE_SKIN)) != 0 ||
                                 !mesh.vertexBoneIndices.empty();
    // Classic W3D skins are rigidly attached per vertex. Do not turn a
    // malformed influence stream into a plausible-looking static mesh.
    if (hasSkinGeometry && mesh.vertexBoneIndices.size() != mesh.vertices.size()) {
        return false;
    }
    // Legacy shadow-only geometry belongs to the future shadow pass, not the
    // base opaque model.
    return (mesh.attributes & data::w3d::MESH_FLAG_SHADOW) == 0;
}

uint32_t sourceIdForTriangle(const ::container::Vector<uint32_t>& ids,
                             size_t triangleIndex,
                             size_t triangleCount,
                             size_t vertexCount,
                             uint32_t firstVertex,
                             bool preferPerVertex) {
    if (ids.empty()) return UINT32_MAX;
    if (ids.size() == 1) return ids.front();
    if (preferPerVertex && ids.size() == vertexCount && firstVertex < ids.size()) {
        return ids[firstVertex];
    }
    if (ids.size() >= triangleCount) return ids[triangleIndex];
    if (ids.size() == vertexCount && firstVertex < ids.size()) return ids[firstVertex];
    return triangleIndex < ids.size() ? ids[triangleIndex] : ids.front();
}

math::vec3 sourceVector(const data::w3d::Vector3& value) {
    return {value.x, value.y, value.z};
}

const data::w3d::ParsedHierarchy* findHierarchy(
    const data::w3d::ParsedW3D& parsed, ::container::StringView hierarchyName,
    size_t* matchCount = nullptr) {
    const ::container::String wanted = lowerAscii(::container::String(hierarchyName));
    const data::w3d::ParsedHierarchy* result = nullptr;
    size_t matches = 0;
    for (const auto& hierarchy : parsed.hierarchies) {
        if (lowerAscii(boundedString(hierarchy.name, data::w3d::NAME_LEN)) == wanted) {
            if (!result) result = &hierarchy;
            ++matches;
        }
    }
    if (matchCount) *matchCount = matches;
    return result;
}

void attachHierarchyAnimations(const data::w3d::ParsedW3D& parsed,
                               const data::w3d::ParsedHierarchy& hierarchy,
                               CpuStaticModel& model,
                               SkeletonBuildDiagnostics* skeletonDiagnostics = nullptr) {
    if (!model.skeleton) {
        model.skeleton = Skeleton::fromW3d(hierarchy, skeletonDiagnostics);
    }
    const ::container::String hierarchyName = lowerAscii(
        boundedString(hierarchy.name, data::w3d::NAME_LEN));
    for (const auto& animation : parsed.animations) {
        if (lowerAscii(boundedString(animation.hierarchyName, data::w3d::NAME_LEN)) != hierarchyName) {
            continue;
        }
        CpuStaticModel::Animation output;
        output.name = boundedString(animation.name, data::w3d::NAME_LEN);
        output.clip = AnimationClip::fromW3d(animation);
        const bool duplicate = std::any_of(
            model.animations.begin(), model.animations.end(),
            [&output](const CpuStaticModel::Animation& existing) {
                return lowerAscii(existing.name) == lowerAscii(output.name);
            });
        if (!duplicate && !output.name.empty() && output.clip) {
            model.animations.push_back(std::move(output));
        }
    }
}

const data::w3d::ParsedHLod* selectedHlod(
    const data::w3d::ParsedW3D& parsed,
    const W3dStaticModelBuildOptions& options) {
    const ::container::String requestedPrototype = prototypeKey(options.requestedPrototype);
    for (const data::w3d::ParsedHLod& hlod : parsed.hlods) {
        const ::container::String hlodName = lowerAscii(
            boundedString(hlod.name, data::w3d::NAME_LEN));
        if (!requestedPrototype.empty() && hlodName != requestedPrototype) continue;
        if (hlod.highestDetailLod()) return &hlod;
    }
    return nullptr;
}

const data::w3d::ParsedHLodLod* highestDetailLod(const data::w3d::ParsedHLod& hlod) {
    return hlod.highestDetailLod();
}

struct MaterialKey {
    uint32_t vertexMaterialId = UINT32_MAX;
    uint32_t shaderId = UINT32_MAX;
    uint32_t textureId = UINT32_MAX;
    uint32_t detailTextureId = UINT32_MAX;

    auto asTuple() const noexcept {
        return std::tie(vertexMaterialId, shaderId, textureId, detailTextureId);
    }
    bool operator<(const MaterialKey& other) const noexcept {
        return asTuple() < other.asTuple();
    }
};

[[nodiscard]] StaticTextureMapperDesc makeTextureMapper(
    const data::w3d::VertexMapperDescriptor& source) noexcept {
    StaticTextureMapperDesc output;
    switch (source.type) {
    case data::w3d::VertexMapperType::Uv:
        output.type = StaticTextureMapperType::Uv;
        break;
    case data::w3d::VertexMapperType::Environment:
        output.type = StaticTextureMapperType::Environment;
        break;
    case data::w3d::VertexMapperType::CheapEnvironment:
        output.type = StaticTextureMapperType::CheapEnvironment;
        break;
    case data::w3d::VertexMapperType::LinearOffset:
        output.type = StaticTextureMapperType::LinearOffset;
        break;
    case data::w3d::VertexMapperType::Scale:
        output.type = StaticTextureMapperType::Scale;
        break;
    case data::w3d::VertexMapperType::Grid:
        output.type = StaticTextureMapperType::Grid;
        break;
    case data::w3d::VertexMapperType::Rotate:
        output.type = StaticTextureMapperType::Rotate;
        break;
    case data::w3d::VertexMapperType::Unsupported:
        output.type = StaticTextureMapperType::Unsupported;
        break;
    }
    output.sourceType = source.sourceType;
    output.clampFix = source.clampFix;
    output.uScale = source.uScale;
    output.vScale = source.vScale;
    output.uPerSecond = source.uPerSecond;
    output.vPerSecond = source.vPerSecond;
    output.uOffset = source.uOffset;
    output.vOffset = source.vOffset;
    output.uCenter = source.uCenter;
    output.vCenter = source.vCenter;
    output.gridFramesPerSecond = source.gridFramesPerSecond;
    output.gridWidthLog2 = source.gridWidthLog2;
    output.gridLastFrame = source.gridLastFrame;
    output.gridOffset = source.gridOffset;
    output.turnsPerSecond = source.turnsPerSecond;
    return output;
}

StaticMaterialDesc makeMaterial(const data::w3d::ParsedMesh& mesh,
                                const MaterialKey& key,
                                uint32_t materialPass,
                                uint32_t materialPassCount,
                                uint32_t textureStageCount) {
    StaticMaterialDesc material;
    material.sourceVertexMaterialId = key.vertexMaterialId;
    material.sourceShaderId = key.shaderId;
    material.sourceTextureId = key.textureId;
    material.sourceDetailTextureId = key.detailTextureId;
    material.hasDetailTexture = key.detailTextureId != UINT32_MAX;
    material.materialPass = materialPass;
    if (mesh.selectedPrelitMode ==
            data::w3d::ParsedPrelitMode::LightmapMultiPass) {
        // MeshModelClass/WW3D's lightmap exposure path identifies the final
        // material pass as the baked-light texture pass.
        material.baseTextureIsLightmap = materialPassCount != 0 &&
            materialPass + 1u == materialPassCount;
    } else if (mesh.selectedPrelitMode ==
               data::w3d::ParsedPrelitMode::LightmapMultiTexture) {
        // Generic WW3D multi-texture lightmaps occupy the final texture
        // stage. Generals normally selects the multi-pass wrapper, but keep
        // this format contract complete for explicit Mod/tooling consumers.
        material.detailTextureIsLightmap = textureStageCount > 1u;
        material.baseTextureIsLightmap = textureStageCount == 1u;
    }
    if (key.vertexMaterialId < mesh.vertexMappers.size()) {
        const auto& sourceMappers = mesh.vertexMappers[key.vertexMaterialId];
        material.textureMappers[0] = makeTextureMapper(sourceMappers[0]);
        material.textureMappers[1] = makeTextureMapper(sourceMappers[1]);
    }
    material.twoSided = (mesh.attributes & data::w3d::MESH_FLAG_TWO_SIDED) != 0;
    // MeshModelClass::install_materials() in RefCode calls
    // Post_Load_Process(lighting_enabled, mesh), with real-time lighting
    // disabled only for the PRELIT_VERTEX material wrapper. VertexMaterial's
    // own serialized attributes do not carry a general UseLighting enable.
    material.lightingEnabled =
        mesh.selectedPrelitMode != data::w3d::ParsedPrelitMode::Vertex;

    if (key.vertexMaterialId < mesh.materialNames.size()) {
        material.name = mesh.materialNames[key.vertexMaterialId];
    }
    if (key.textureId < mesh.textureNames.size()) {
        material.textureName = mesh.textureNames[key.textureId];
    }
    if (key.detailTextureId < mesh.textureNames.size()) {
        material.detailTextureName = mesh.textureNames[key.detailTextureId];
    }
    if (key.textureId < mesh.textureInfos.size()) {
        constexpr uint16_t kTextureClampU = 0x0008;
        constexpr uint16_t kTextureClampV = 0x0010;
        const uint16_t attributes = mesh.textureInfos[key.textureId].attributes;
        material.samplerMode = static_cast<uint8_t>(
            ((attributes & kTextureClampU) != 0 ? 1 : 0) |
            ((attributes & kTextureClampV) != 0 ? 2 : 0));
    }
    if (key.detailTextureId < mesh.textureInfos.size()) {
        constexpr uint16_t kTextureClampU = 0x0008;
        constexpr uint16_t kTextureClampV = 0x0010;
        const uint16_t attributes = mesh.textureInfos[key.detailTextureId].attributes;
        material.detailSamplerMode = static_cast<uint8_t>(
            ((attributes & kTextureClampU) != 0 ? 1 : 0) |
            ((attributes & kTextureClampV) != 0 ? 2 : 0));
    }
    if (key.vertexMaterialId < mesh.vertexMaterials.size()) {
        const auto& source = mesh.vertexMaterials[key.vertexMaterialId];
        constexpr float byteToFloat = 1.0f / 255.0f;
        material.ambient = {
            source.ambient.r * byteToFloat,
            source.ambient.g * byteToFloat,
            source.ambient.b * byteToFloat};
        material.diffuse = math::vec4{
            source.diffuse.r * byteToFloat,
            source.diffuse.g * byteToFloat,
            source.diffuse.b * byteToFloat,
            math::clamp(source.opacity, 0.0f, 1.0f)};
        material.specular = {
            source.specular.r * byteToFloat,
            source.specular.g * byteToFloat,
            source.specular.b * byteToFloat};
        material.emissive = {
            source.emissive.r * byteToFloat,
            source.emissive.g * byteToFloat,
            source.emissive.b * byteToFloat};
        material.shininess = math::max(source.shininess, 0.0f);
    }
    if (key.shaderId < mesh.shaders.size()) {
        const auto& shader = mesh.shaders[key.shaderId];
        material.alphaTest = shader.alphaTest != 0;
        // RefCode switches alpha-test direction when the source blend is
        // ONE_MINUS_SRC_ALPHA (W3D enum 3): alpha <= 0x9f instead of the
        // normal alpha >= 0x60. Preserve that semantic explicitly rather
        // than treating every masked material as the common cutout case.
        material.alphaTestInverted = material.alphaTest && shader.srcBlend == 3;
        material.sourceBlend = shader.srcBlend;
        material.destinationBlend = shader.destBlend;
        material.depthCompare = shader.depthCompare;
        material.depthWrite = shader.depthMask != 0;
        material.detailColorFunc = shader.postDetailColorFunc;
        material.detailAlphaFunc = shader.postDetailAlphaFunc;
        material.rawFogFunc = shader.fogFunc;
        // Generals' Convert_Shader uses the default force-fog-disable path.
        // Keep rawFogFunc above for format fidelity, but do not accidentally
        // activate dormant W3D fog bits when a camera later enables fog.
        material.fogFunc = 0;
        material.primaryGradientDisabled = shader.priGradient == 0;
        material.texturingEnabled = shader.texturing != 0;
        material.secondaryGradientEnabled = shader.secGradient != 0;
    }
    return material;
}

std::optional<CpuStaticMesh> buildMesh(const data::w3d::ParsedMesh& source,
                                       uint32_t boneIndex,
                                       const math::transform& localTransform,
                                       CpuStaticModel& model,
                                       math::vec3& modelMin,
                                       math::vec3& modelMax,
                                       bool& hasModelBounds,
                                       const data::w3d::ParsedMaterialPass* materialPass,
                                       uint32_t materialPassIndex) {
    CpuStaticMesh mesh;
    mesh.name = meshFullName(source);
    mesh.sourceBoneIndex = boneIndex;
    mesh.localTransform = localTransform;
    mesh.skinned = !source.vertexBoneIndices.empty();
    mesh.initiallyHidden =
        (source.attributes & data::w3d::MESH_FLAG_HIDDEN) != 0;
    mesh.forceShadow =
        (source.attributes & data::w3d::MESH_FLAG_CAST_SHADOW) != 0;
    mesh.sortLevel = source.sortLevel;
    mesh.vertices.resize(source.vertices.size());

    const data::w3d::ParsedMaterialPass* basePass = materialPass;
    const data::w3d::ParsedTextureStage* baseStage =
        basePass && !basePass->textureStages.empty() ? &basePass->textureStages.front() : nullptr;
    const data::w3d::ParsedTextureStage* detailStage =
        basePass && basePass->textureStages.size() > 1 ? &basePass->textureStages[1] : nullptr;
    const auto& sourceTexCoords = baseStage ? baseStage->texCoords : source.texCoords;
    const auto& sourceVertexColors = basePass ? basePass->vertexColors : source.vertexColors;
    const auto& sourceVertexIllumination =
        basePass ? basePass->vertexIllumination : source.vertexIllumination;
    const auto& sourceVertexMaterialIds =
        basePass ? basePass->vertexMaterialIds : source.vertexMaterialIds;
    const auto& sourceShaderIds = basePass ? basePass->shaderIds : source.shaderIds;
    const auto& sourceTextureIds = baseStage ? baseStage->textureIds : source.textureIds;
    const auto& sourceDetailTextureIds = detailStage ? detailStage->textureIds : source.textureIds;

    ::container::Vector<math::vec3> generatedNormals(source.vertices.size(), math::vec3::zero());
    const bool hasSourceNormals = source.normals.size() == source.vertices.size();
    if (!hasSourceNormals) {
        for (const auto& triangle : source.triangles) {
            if (triangle.vindex[0] >= source.vertices.size() ||
                triangle.vindex[1] >= source.vertices.size() ||
                triangle.vindex[2] >= source.vertices.size()) {
                continue;
            }
            const math::vec3 p0 = sourceVector(source.vertices[triangle.vindex[0]]);
            const math::vec3 p1 = sourceVector(source.vertices[triangle.vindex[1]]);
            const math::vec3 p2 = sourceVector(source.vertices[triangle.vindex[2]]);
            math::vec3 normal = (p1 - p0).cross(p2 - p0);
            if (normal.length_sq() <= math::EPSILON * math::EPSILON) {
                normal = sourceVector(triangle.normal);
            }
            generatedNormals[triangle.vindex[0]] += normal;
            generatedNormals[triangle.vindex[1]] += normal;
            generatedNormals[triangle.vindex[2]] += normal;
        }
    }

    math::vec3 localMin{};
    math::vec3 localMax{};
    bool hasLocalBounds = false;
    for (size_t i = 0; i < source.vertices.size(); ++i) {
        auto& vertex = mesh.vertices[i];
        vertex.position = sourceVector(source.vertices[i]);
        if (hasSourceNormals) {
            vertex.normal = sourceVector(source.normals[i]).normalized();
        } else if (generatedNormals[i].length_sq() > math::EPSILON * math::EPSILON) {
            vertex.normal = generatedNormals[i].normalized();
        }
        if (i < sourceTexCoords.size()) {
            vertex.texcoord = {sourceTexCoords[i].u, 1.0f - sourceTexCoords[i].v};
        }
        if (detailStage && i < detailStage->texCoords.size()) {
            vertex.detailTexcoord = {detailStage->texCoords[i].u, 1.0f - detailStage->texCoords[i].v};
        }
        if (i < sourceVertexColors.size()) {
            constexpr float byteToFloat = 1.0f / 255.0f;
            const auto& color = sourceVertexColors[i];
            vertex.color = {
                color.r * byteToFloat,
                color.g * byteToFloat,
                color.b * byteToFloat,
                color.a * byteToFloat};
        }
        // RefCode folds DIG (per-vertex diffuse illumination) into DCG on
        // load because the legacy vertex path has one diffuse colour stream.
        // Preserve that exact immutable-asset conversion before the modern
        // GPU vertex buffer is built.
        if (i < sourceVertexIllumination.size()) {
            constexpr float byteToFloat = 1.0f / 255.0f;
            const auto& illumination = sourceVertexIllumination[i];
            const math::vec3 dig{
                illumination.r * byteToFloat,
                illumination.g * byteToFloat,
                illumination.b * byteToFloat};
            vertex.color = {
                vertex.color.x() * dig.x(),
                vertex.color.y() * dig.y(),
                vertex.color.z() * dig.z(),
                vertex.color.w()};
        }
        if (mesh.skinned) {
            vertex.boneIndex = source.vertexBoneIndices[i];
        }

        if (!hasLocalBounds) {
            localMin = localMax = vertex.position;
            hasLocalBounds = true;
        } else {
            localMin = {
                std::min(localMin.x(), vertex.position.x()),
                std::min(localMin.y(), vertex.position.y()),
                std::min(localMin.z(), vertex.position.z())};
            localMax = {
                std::max(localMax.x(), vertex.position.x()),
                std::max(localMax.y(), vertex.position.y()),
                std::max(localMax.z(), vertex.position.z())};
        }

        const math::vec3 modelPoint = localTransform.transform_point(vertex.position);
        if (!hasModelBounds) {
            modelMin = modelMax = modelPoint;
            hasModelBounds = true;
        } else {
            modelMin = {
                std::min(modelMin.x(), modelPoint.x()),
                std::min(modelMin.y(), modelPoint.y()),
                std::min(modelMin.z(), modelPoint.z())};
            modelMax = {
                std::max(modelMax.x(), modelPoint.x()),
                std::max(modelMax.y(), modelPoint.y()),
                std::max(modelMax.z(), modelPoint.z())};
        }
    }

    if (!hasLocalBounds) return std::nullopt;
    mesh.localBounds = math::aabb{(localMin + localMax) * 0.5f, (localMax - localMin) * 0.5f};

    // W3D texture stages can index UVs per face corner. Convert that legacy
    // indirection into an ordinary indexed vertex stream once at asset-build
    // time. The renderer then needs no W3D-specific branch, and a source
    // vertex is duplicated only when the stage assigns it a distinct UV.
    const ::container::Vector<StaticMeshVertex> sourceVertices = std::move(mesh.vertices);
    mesh.vertices.clear();
    ::container::TreeMap<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> remappedVertices;

    ::container::TreeMap<MaterialKey, ::container::Vector<uint32_t>> groupedIndices;
    for (size_t triangleIndex = 0; triangleIndex < source.triangles.size(); ++triangleIndex) {
        const auto& triangle = source.triangles[triangleIndex];
        if (triangle.vindex[0] >= source.vertices.size() ||
            triangle.vindex[1] >= source.vertices.size() ||
            triangle.vindex[2] >= source.vertices.size()) {
            continue;
        }

        MaterialKey key;
        key.vertexMaterialId = sourceIdForTriangle(
            sourceVertexMaterialIds, triangleIndex, source.triangles.size(),
            source.vertices.size(), triangle.vindex[0], true);
        key.shaderId = sourceIdForTriangle(
            sourceShaderIds, triangleIndex, source.triangles.size(),
            source.vertices.size(), triangle.vindex[0], false);
        key.textureId = sourceIdForTriangle(
            sourceTextureIds, triangleIndex, source.triangles.size(),
            source.vertices.size(), triangle.vindex[0], false);
        key.detailTextureId = detailStage ? sourceIdForTriangle(
            sourceDetailTextureIds, triangleIndex, source.triangles.size(),
            source.vertices.size(), triangle.vindex[0], false) : UINT32_MAX;

        auto& indices = groupedIndices[key];
        for (size_t corner = 0; corner < 3; ++corner) {
            const uint32_t sourceVertexIndex = triangle.vindex[corner];
            uint32_t texCoordIndex = sourceVertexIndex;
            uint32_t detailTexCoordIndex = sourceVertexIndex;
            if (baseStage && triangleIndex < baseStage->perFaceTexCoordIds.size()) {
                const uint32_t candidate = baseStage->perFaceTexCoordIds[triangleIndex][corner];
                if (candidate < baseStage->texCoords.size()) texCoordIndex = candidate;
            }
            if (detailStage && triangleIndex < detailStage->perFaceTexCoordIds.size()) {
                const uint32_t candidate = detailStage->perFaceTexCoordIds[triangleIndex][corner];
                if (candidate < detailStage->texCoords.size()) detailTexCoordIndex = candidate;
            }
            const auto remapKey = std::make_tuple(sourceVertexIndex, texCoordIndex, detailTexCoordIndex);
            const auto [remapIt, inserted] = remappedVertices.emplace(
                remapKey, static_cast<uint32_t>(mesh.vertices.size()));
            if (inserted) {
                StaticMeshVertex vertex = sourceVertices[sourceVertexIndex];
                if (baseStage && texCoordIndex < baseStage->texCoords.size()) {
                    const auto& texcoord = baseStage->texCoords[texCoordIndex];
                    vertex.texcoord = {texcoord.u, 1.0f - texcoord.v};
                }
                if (detailStage && detailTexCoordIndex < detailStage->texCoords.size()) {
                    const auto& texcoord = detailStage->texCoords[detailTexCoordIndex];
                    vertex.detailTexcoord = {texcoord.u, 1.0f - texcoord.v};
                }
                mesh.vertices.push_back(vertex);
            }
            indices.push_back(remapIt->second);
        }
    }

    for (const auto& [key, sourceIndices] : groupedIndices) {
        StaticPrimitive primitive;
        primitive.firstIndex = static_cast<uint32_t>(mesh.indices.size());
        primitive.indexCount = static_cast<uint32_t>(sourceIndices.size());
        primitive.materialIndex = static_cast<uint32_t>(model.materials.size());
        mesh.indices.insert(mesh.indices.end(), sourceIndices.begin(), sourceIndices.end());
        mesh.primitives.push_back(primitive);
        model.materials.push_back(makeMaterial(
            source, key, materialPassIndex,
            source.materialPasses.empty()
                ? 1u
                : static_cast<uint32_t>(source.materialPasses.size()),
            basePass
                ? static_cast<uint32_t>(basePass->textureStages.size())
                : 0u));
    }

    if (mesh.indices.empty()) return std::nullopt;
    return mesh;
}

} // namespace

bool subObjectVisibilityAffectsMesh(
    const CpuStaticModel& model, size_t controllerMeshIndex,
    size_t targetMeshIndex) noexcept {
    if (controllerMeshIndex >= model.meshes.size() ||
        targetMeshIndex >= model.meshes.size()) {
        return false;
    }
    if (controllerMeshIndex == targetMeshIndex) return true;
    if (!model.skeleton) return false;

    const uint32_t controllerBone =
        model.meshes[controllerMeshIndex].sourceBoneIndex;
    const uint32_t targetBone = model.meshes[targetMeshIndex].sourceBoneIndex;
    const auto& joints = model.skeleton->joints();
    if (controllerBone >= joints.size() || targetBone >= joints.size()) {
        return false;
    }

    int32_t parent = joints[targetBone].parentIndex;
    size_t remaining = joints.size();
    while (parent >= 0 && remaining-- > 0) {
        if (static_cast<uint32_t>(parent) == controllerBone) return true;
        if (static_cast<size_t>(parent) >= joints.size()) return false;
        parent = joints[static_cast<size_t>(parent)].parentIndex;
    }
    return false;
}

std::optional<bool> recursiveSubObjectVisibilityOverride(
    const CpuStaticModel& model, size_t targetMeshIndex,
    ::container::Span<const RenderSubObjectVisibility> overrides) noexcept {
    if (targetMeshIndex >= model.meshes.size()) return std::nullopt;
    std::optional<bool> result;
    for (const RenderSubObjectVisibility& overrideValue : overrides) {
        container::Vector<container::String> selectedNames;
        if (overrideValue.nameIsPrefix) {
            container::Vector<container::String> numbered;
            numbered.reserve(8);
            for (uint32_t ordinal = 1; ordinal <= 99; ++ordinal) {
                container::String candidate = overrideValue.name;
                candidate.push_back(static_cast<char>('0' + ordinal / 10u));
                candidate.push_back(static_cast<char>('0' + ordinal % 10u));
                const bool exists = std::any_of(
                    model.meshes.begin(), model.meshes.end(),
                    [&candidate](const CpuStaticMesh& mesh) {
                        return renderSubObjectNameMatches(
                            mesh.name, candidate);
                    });
                if (!exists) break;
                numbered.push_back(std::move(candidate));
            }
            if (!numbered.empty()) {
                if (overrideValue.nameSequenceOrdinal == 0u &&
                    !overrideValue.visible) {
                    selectedNames = std::move(numbered);
                } else {
                    const uint32_t sequence = std::max<uint32_t>(
                        1u, overrideValue.nameSequenceOrdinal);
                    selectedNames.push_back(
                        std::move(numbered[(sequence - 1u) % numbered.size()]));
                }
            } else if (!overrideValue.namePrefixFallsBackToBare) {
                continue;
            }
        }
        if (selectedNames.empty()) {
            selectedNames.push_back(overrideValue.name);
        }
        for (const container::String& selectedName : selectedNames) {
            for (size_t controllerIndex = 0;
                 controllerIndex < model.meshes.size(); ++controllerIndex) {
                if (!renderSubObjectNameMatches(
                        model.meshes[controllerIndex].name,
                        selectedName) ||
                    !subObjectVisibilityAffectsMesh(
                        model, controllerIndex, targetMeshIndex)) {
                    continue;
                }
                result = overrideValue.visible;
                break;
            }
        }
    }
    return result;
}

size_t countSubObjectVisibleDrawPackets(
    const CpuStaticModel& model,
    ::container::Span<const RenderSubObjectVisibility> overrides) noexcept {
    size_t result = 0;
    for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
        const CpuStaticMesh& mesh = model.meshes[meshIndex];
        const std::optional<bool> explicitVisibility =
            recursiveSubObjectVisibilityOverride(model, meshIndex, overrides);
        if (!explicitVisibility.value_or(!mesh.initiallyHidden)) continue;
        result += static_cast<size_t>(std::count_if(
            mesh.primitives.begin(), mesh.primitives.end(),
            [&model](const StaticPrimitive& primitive) {
                return primitive.materialIndex < model.materials.size();
            }));
    }
    return result;
}

container::Vector<W3dAdditionalModelInstance>
resolveW3dAdditionalModelInstances(
    const CpuStaticModel& model, const math::transform& entityWorld,
    container::Span<const math::transform> boneWorldTransforms,
    container::Span<const uint8_t> boneVisibility) {
    container::Vector<W3dAdditionalModelInstance> result;
    result.reserve(model.additionalModels.size());
    for (const CpuStaticModel::HierarchyModelReference& reference :
         model.additionalModels) {
        if (reference.modelName.empty() ||
            (reference.boneIndex < boneVisibility.size() &&
             boneVisibility[reference.boneIndex] == 0)) {
            continue;
        }
        result.push_back({
            .modelName = reference.modelName,
            .worldTransform = reference.boneIndex < boneWorldTransforms.size()
                ? boneWorldTransforms[reference.boneIndex]
                : entityWorld,
            .boneIndex = reference.boneIndex,
        });
    }
    return result;
}

::container::String W3dStaticModelBuilder::requiredHierarchyName(
    const data::w3d::ParsedW3D& parsed,
    const W3dStaticModelBuildOptions& options) {
    const data::w3d::ParsedHLod* hlod = selectedHlod(parsed, options);
    return hlod
        ? boundedString(hlod->hierarchyName, data::w3d::NAME_LEN)
        : ::container::String{};
}

std::optional<CpuStaticModel> W3dStaticModelBuilder::build(
    const data::w3d::ParsedW3D& parsed,
    const W3dStaticModelBuildOptions& options,
    ::container::String* error) {
    if (error) error->clear();
    if (parsed.meshes.empty()) {
        if (error) *error = "W3D contains no mesh chunks";
        return std::nullopt;
    }

    CpuStaticModel model;
    const ::container::String requestedPrototype = prototypeKey(options.requestedPrototype);
    math::vec3 modelMin{};
    math::vec3 modelMax{};
    bool hasModelBounds = false;

    container::HashMap<::container::String, const data::w3d::ParsedMesh*> meshLookup;
    for (const auto& mesh : parsed.meshes) {
        const ::container::String fullName = lowerAscii(meshFullName(mesh));
        if (!fullName.empty()) meshLookup.emplace(fullName, &mesh);
    }

    const auto appendMesh = [&](const data::w3d::ParsedMesh& source,
                                uint32_t boneIndex,
                                const math::transform& localTransform) {
        if (!isRenderableMesh(source, options)) return;
        const auto appendMaterialPass = [&](const data::w3d::ParsedMaterialPass* materialPass,
                                            uint32_t materialPassIndex) {
            auto mesh = buildMesh(source, boneIndex, localTransform, model,
                                  modelMin, modelMax, hasModelBounds,
                                  materialPass, materialPassIndex);
            if (mesh) model.meshes.push_back(std::move(*mesh));
        };
        if (source.materialPasses.empty()) {
            appendMaterialPass(nullptr, 0);
        } else {
            for (uint32_t index = 0; index < source.materialPasses.size(); ++index) {
                appendMaterialPass(&source.materialPasses[index], index);
            }
        }
    };

    bool expandedHlod = false;
    for (const auto& hlod : parsed.hlods) {
        const ::container::String hlodName = lowerAscii(boundedString(hlod.name, data::w3d::NAME_LEN));
        if (!requestedPrototype.empty() && hlodName != requestedPrototype) continue;

        const auto* lod = highestDetailLod(hlod);
        if (!lod || lod->subObjects.empty()) continue;

        model.name = boundedString(hlod.name, data::w3d::NAME_LEN);
        model.hierarchyName = boundedString(
            hlod.hierarchyName, data::w3d::NAME_LEN);
        ::container::Vector<RenderMatrix> restTransforms;
        size_t hierarchyMatches = 0;
        const data::w3d::ParsedHierarchy* hierarchy =
            findHierarchy(parsed, model.hierarchyName, &hierarchyMatches);
        const data::w3d::ParsedW3D* hierarchySource = &parsed;
        if (hierarchyMatches > 1) {
            hierarchy = nullptr;
            model.hierarchyConflictCount = static_cast<uint32_t>(hierarchyMatches);
            model.diagnostics.push_back(
                std::to_string(hierarchyMatches) + " hierarchies named '" +
                model.hierarchyName + "' conflicted in one W3D source");
        } else if (!hierarchy && options.externalHierarchySource) {
            size_t externalMatches = 0;
            hierarchy = findHierarchy(
                *options.externalHierarchySource, model.hierarchyName,
                &externalMatches);
            if (externalMatches > 1) {
                hierarchy = nullptr;
                model.hierarchyConflictCount =
                    static_cast<uint32_t>(externalMatches);
                model.diagnostics.push_back(
                    std::to_string(externalMatches) + " hierarchies named '" +
                    model.hierarchyName + "' conflicted in one W3D source");
            }
            hierarchySource = options.externalHierarchySource;
            model.externalHierarchyResolved = hierarchy != nullptr;
        }
        if (hierarchy) {
            if (hierarchy->numPivots != hierarchy->pivots.size()) {
                model.diagnostics.push_back(
                    "hierarchy '" + model.hierarchyName + "' declared " +
                    std::to_string(hierarchy->numPivots) + " pivots but parsed " +
                    std::to_string(hierarchy->pivots.size()));
            }
            SkeletonBuildDiagnostics skeletonDiagnostics;
            attachHierarchyAnimations(
                *hierarchySource, *hierarchy, model, &skeletonDiagnostics);
            model.invalidHierarchyParentCount =
                skeletonDiagnostics.invalidParentCount;
            model.hierarchyCycleNodeCount = skeletonDiagnostics.cycleNodeCount;
            if (model.invalidHierarchyParentCount != 0) {
                model.diagnostics.push_back(
                    std::to_string(model.invalidHierarchyParentCount) +
                    " hierarchy pivots referenced invalid parents and were remapped to roots");
            }
            if (model.hierarchyCycleNodeCount != 0) {
                model.diagnostics.push_back(
                    std::to_string(model.hierarchyCycleNodeCount) +
                    " hierarchy pivots participated in parent cycles and were remapped to roots");
            }
            if (hierarchySource != &parsed) {
                attachHierarchyAnimations(parsed, *hierarchy, model);
            }
            if (model.skeleton && !model.skeleton->empty()) {
                restTransforms = evaluateSkeletonRestPose(*model.skeleton, RenderTransform{});
            }
        } else if (!model.hierarchyName.empty()) {
            model.diagnostics.push_back(
                "missing hierarchy '" + model.hierarchyName + "'");
        }

        model.additionalModels.reserve(hlod.aggregates.size());
        for (const data::w3d::HLodSubObject& aggregate : hlod.aggregates) {
            const container::String name = boundedString(
                aggregate.name, sizeof(aggregate.name));
            if (name.empty()) continue;
            model.additionalModels.push_back({
                .modelName = name,
                .boneIndex = aggregate.boneIndex,
            });
        }
        model.proxies.reserve(hlod.proxies.size());
        for (const data::w3d::HLodSubObject& proxy : hlod.proxies) {
            const container::String name = boundedString(
                proxy.name, sizeof(proxy.name));
            if (name.empty()) continue;
            model.proxies.push_back({
                .proxyName = name,
                .boneIndex = proxy.boneIndex,
            });
        }

        for (const auto& subObject : lod->subObjects) {
            ::container::String sourceName = lowerAscii(boundedString(subObject.name, sizeof(subObject.name)));
            auto meshIt = meshLookup.find(sourceName);
            if (meshIt == meshLookup.end()) continue;

            uint32_t resolvedBoneIndex = subObject.boneIndex;
            if (!restTransforms.empty() && resolvedBoneIndex >= restTransforms.size()) {
                resolvedBoneIndex = 0;
                ++model.invalidSubObjectBoneCount;
            }
            math::transform localTransform = math::transform::identity();
            if (resolvedBoneIndex < restTransforms.size()) {
                localTransform = restTransforms[resolvedBoneIndex];
            }
            appendMesh(*meshIt->second, resolvedBoneIndex, localTransform);
        }
        if (model.invalidSubObjectBoneCount != 0) {
            model.diagnostics.push_back(
                std::to_string(model.invalidSubObjectBoneCount) +
                " HLOD subobjects referenced missing hierarchy pivots and were remapped to bone 0");
        }
        expandedHlod = !model.meshes.empty();
        if (expandedHlod) break;
    }

    if (!expandedHlod) {
        model.name = options.requestedPrototype.empty()
            ? meshFullName(parsed.meshes.front())
            : options.requestedPrototype;
        for (const auto& mesh : parsed.meshes) {
            if (!requestedPrototype.empty()) {
                const ::container::String container = lowerAscii(
                    boundedString(mesh.containerName, data::w3d::NAME_LEN));
                const ::container::String name = lowerAscii(
                    boundedString(mesh.name, data::w3d::NAME_LEN));
                const ::container::String fullName = lowerAscii(meshFullName(mesh));
                if (container != requestedPrototype && name != requestedPrototype &&
                    fullName != requestedPrototype) {
                    continue;
                }
            }
            appendMesh(mesh, UINT32_MAX, math::transform::identity());
        }
    }

    if (model.meshes.empty() || !hasModelBounds) {
        if (error) *error = "W3D contains no renderable static mesh";
        return std::nullopt;
    }

    if (model.skeleton && !model.skeleton->empty()) {
        const size_t jointCount = model.skeleton->joints().size();
        for (CpuStaticMesh& mesh : model.meshes) {
            if (!mesh.skinned) continue;
            for (StaticMeshVertex& vertex : mesh.vertices) {
                if (vertex.boneIndex == UINT32_MAX || vertex.boneIndex < jointCount) continue;
                vertex.boneIndex = 0;
                ++model.invalidInfluenceCount;
            }
        }
        if (model.invalidInfluenceCount != 0) {
            model.diagnostics.push_back(
                std::to_string(model.invalidInfluenceCount) +
                " skin influences referenced missing hierarchy pivots and were remapped to bone 0");
        }
    } else {
        bool hasSkin = false;
        for (CpuStaticMesh& mesh : model.meshes) {
            if (!mesh.skinned) continue;
            hasSkin = true;
            mesh.skinned = false;
            for (StaticMeshVertex& vertex : mesh.vertices) {
                vertex.boneIndex = UINT32_MAX;
            }
        }
        if (hasSkin) {
            model.skeletonFallback = true;
            model.diagnostics.push_back(
                "skin rendered in rigid bind-pose fallback because its hierarchy is unavailable");
        }
    }

    ::container::TreeSet<uint8_t> unsupportedMapperTypes;
    for (const StaticMaterialDesc& material : model.materials) {
        for (const StaticTextureMapperDesc& mapper : material.textureMappers) {
            if (mapper.type == StaticTextureMapperType::Unsupported) {
                unsupportedMapperTypes.insert(mapper.sourceType);
            }
        }
    }
    for (uint8_t mapperType : unsupportedMapperTypes) {
        model.diagnostics.push_back(
            "unsupported W3D vertex mapper type " +
            std::to_string(static_cast<uint32_t>(mapperType)) +
            " uses authored-UV fallback");
    }

    model.bounds = math::aabb{(modelMin + modelMax) * 0.5f, (modelMax - modelMin) * 0.5f};
    return model;
}

} // namespace engine::render
