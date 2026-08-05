#pragma once

#include "container/container_types.h"
#include <cstdint>
namespace data::w3d {

enum ChunkType : uint32_t
{
    Chunk_Mesh                       = 0x00000000,
    Chunk_Vertices                   = 0x00000002,
    Chunk_VertexNormals              = 0x00000003,
    Chunk_MeshUserText               = 0x0000000C,
    Chunk_VertexInfluences           = 0x0000000E,
    Chunk_MeshHeader3                = 0x0000001F,
    Chunk_Triangles                  = 0x00000020,
    Chunk_VertexShadeIndices         = 0x00000022,
    Chunk_PrelitUnlit                = 0x00000023,
    Chunk_PrelitVertex               = 0x00000024,
    Chunk_PrelitLightmapMultiPass    = 0x00000025,
    Chunk_PrelitLightmapMultiTexture = 0x00000026,
    Chunk_MaterialInfo               = 0x00000028,
    Chunk_Shaders                    = 0x00000029,
    Chunk_VertexMaterials            = 0x0000002A,
    Chunk_VertexMaterial             = 0x0000002B,
    Chunk_VertexMaterialName         = 0x0000002C,
    Chunk_VertexMaterialInfo         = 0x0000002D,
    Chunk_VertexMapperArgs0          = 0x0000002E,
    Chunk_VertexMapperArgs1          = 0x0000002F,
    Chunk_Textures                   = 0x00000030,
    Chunk_Texture                    = 0x00000031,
    Chunk_TextureName                = 0x00000032,
    Chunk_TextureInfo                = 0x00000033,
    Chunk_MaterialPass               = 0x00000038,
    Chunk_VertexMaterialIds          = 0x00000039,
    Chunk_ShaderIds                  = 0x0000003A,
    Chunk_DCG                        = 0x0000003B,
    Chunk_DIG                        = 0x0000003C,
    Chunk_SCG                        = 0x0000003E,
    Chunk_TextureStage               = 0x00000048,
    Chunk_TextureIds                 = 0x00000049,
    Chunk_StageTexCoords             = 0x0000004A,
    Chunk_PerFaceTexCoordIds         = 0x0000004B,
    Chunk_Deform                     = 0x00000058,
    Chunk_DeformSet                  = 0x00000059,
    Chunk_DeformKeyframe             = 0x0000005A,
    Chunk_DeformData                 = 0x0000005B,
    Chunk_PS2Shaders                 = 0x00000080,
    Chunk_AABTree                    = 0x00000090,
    Chunk_AABTreeHeader              = 0x00000091,
    Chunk_AABTreePolyIndices         = 0x00000092,
    Chunk_AABTreeNodes               = 0x00000093,
    Chunk_Hierarchy                  = 0x00000100,
    Chunk_HierarchyHeader            = 0x00000101,
    Chunk_Pivots                     = 0x00000102,
    Chunk_PivotFixups                = 0x00000103,
    Chunk_Animation                  = 0x00000200,
    Chunk_AnimationHeader            = 0x00000201,
    Chunk_AnimationChannel           = 0x00000202,
    Chunk_BitChannel                 = 0x00000203,
    Chunk_CompressedAnimation        = 0x00000280,
    Chunk_CompressedAnimHeader       = 0x00000281,
    Chunk_CompressedAnimChannel      = 0x00000282,
    Chunk_CompressedBitChannel       = 0x00000283,
    Chunk_MorphAnimation             = 0x000002C0,
    Chunk_MorphAnimHeader            = 0x000002C1,
    Chunk_MorphAnimChannel           = 0x000002C2,
    Chunk_MorphAnimPoseName          = 0x000002C3,
    Chunk_MorphAnimKeyData           = 0x000002C4,
    Chunk_MorphAnimPivotChannelData  = 0x000002C5,
    Chunk_HModel                     = 0x00000300,
    Chunk_HModelHeader               = 0x00000301,
    Chunk_Node                       = 0x00000302,
    Chunk_CollisionNode              = 0x00000303,
    Chunk_SkinNode                   = 0x00000304,
    Chunk_LODModel                   = 0x00000400,
    Chunk_LODModelHeader             = 0x00000401,
    Chunk_LOD                        = 0x00000402,
    Chunk_Collection                 = 0x00000420,
    Chunk_CollectionHeader           = 0x00000421,
    Chunk_CollectionObjName          = 0x00000422,
    Chunk_Placeholder                = 0x00000423,
    Chunk_TransformNode              = 0x00000424,
    Chunk_Points                     = 0x00000440,
    Chunk_Light                      = 0x00000460,
    Chunk_LightInfo                  = 0x00000461,
    Chunk_SpotLightInfo              = 0x00000462,
    Chunk_NearAttenuation            = 0x00000463,
    Chunk_FarAttenuation             = 0x00000464,
    Chunk_Emitter                    = 0x00000500,
    Chunk_EmitterHeader              = 0x00000501,
    Chunk_EmitterUserData            = 0x00000502,
    Chunk_EmitterInfo                = 0x00000503,
    Chunk_EmitterInfoV2              = 0x00000504,
    Chunk_EmitterProps               = 0x00000505,
    Chunk_EmitterLineProperties      = 0x00000508,
    Chunk_EmitterRotationKeyframes   = 0x00000509,
    Chunk_EmitterFrameKeyframes      = 0x0000050A,
    Chunk_EmitterBlurTimeKeyframes   = 0x0000050B,
    Chunk_EmitterExtraInfo           = 0x0000050C,
    Chunk_Aggregate                  = 0x00000600,
    Chunk_AggregateHeader            = 0x00000601,
    Chunk_AggregateInfo              = 0x00000602,
    Chunk_AggregateSubobject         = 0x00000603,
    Chunk_TextureReplacerInfo        = 0x00000604,
    Chunk_AggregateClassInfo         = 0x00000605,
    Chunk_HLod                       = 0x00000700,
    Chunk_HLodHeader                 = 0x00000701,
    Chunk_HLodLodArray               = 0x00000702,
    Chunk_HLodSubObjectArrayHeader   = 0x00000703,
    Chunk_HLodSubObject              = 0x00000704,
    Chunk_HLodAggregateArray         = 0x00000705,
    Chunk_HLodProxyArray             = 0x00000706,
    Chunk_Box                        = 0x00000740,
    Chunk_Sphere                     = 0x00000741,
    Chunk_Ring                       = 0x00000742,
    Chunk_NullObject                 = 0x00000750,
    Chunk_Lightscape                 = 0x00000800,
    Chunk_LightscapeLight            = 0x00000801,
    Chunk_LightTransform             = 0x00000802,
    Chunk_Dazzle                     = 0x00000900,
    Chunk_DazzleName                 = 0x00000901,
    Chunk_DazzleTypeName             = 0x00000902,
    Chunk_SoundRObj                  = 0x00000A00,
    Chunk_SoundRObjHeader            = 0x00000A01,
    Chunk_SoundRObjDefinition        = 0x00000A02,
    Chunk_ShDMesh                    = 0x00000B00,
    Chunk_ShDMeshName                = 0x00000B01,
    Chunk_ShDMeshHeader              = 0x00000B02,
    Chunk_ShDMeshUserText            = 0x00000B03,
    Chunk_ShDSubMesh                 = 0x00000B20,
    Chunk_ShDSubMeshHeader           = 0x00000B21,
    Chunk_ShDSubMeshShader           = 0x00000B40,
    Chunk_ShDSubMeshShaderClassId    = 0x00000B41,
    Chunk_ShDSubMeshShaderDef        = 0x00000B42,
    Chunk_ShDSubMeshVertices         = 0x00000B60,
    Chunk_ShDSubMeshVertexNormals    = 0x00000B61,
    Chunk_ShDSubMeshTriangles        = 0x00000B62,
    Chunk_ShDSubMeshVertShadeIndices = 0x00000B63,
    Chunk_ShDSubMeshUV0              = 0x00000B80,
    Chunk_ShDSubMeshUV1              = 0x00000B81,
    Chunk_ShDSubMeshTangentBasisS    = 0x00000BA0,
    Chunk_ShDSubMeshTangentBasisT    = 0x00000BA1,
    Chunk_ShDSubMeshTangentBasisSxT  = 0x00000BA2,
    Chunk_ShDSubMeshVertexColor      = 0x00000BC0,
    Chunk_ShDSubMeshVertexInfluences = 0x00000BC1,
};

constexpr uint32_t NAME_LEN = 16;

constexpr uint32_t MakeVersion(uint16_t major, uint16_t minor)
{
    return (static_cast<uint32_t>(major) << 16) | minor;
}

constexpr uint16_t GetMajorVersion(uint32_t ver)
{
    return static_cast<uint16_t>(ver >> 16);
}

constexpr uint16_t GetMinorVersion(uint32_t ver)
{
    return static_cast<uint16_t>(ver & 0xFFFF);
}

#pragma pack(push, 1)

struct ChunkHeader
{
    uint32_t type;
    uint32_t size;
};

struct Vector3
{
    float x, y, z;
};

struct Vector4
{
    float x, y, z, w;
};

struct Quaternion
{
    float q[4];
};

struct TexCoord
{
    float u, v;
};

struct ColorRGB
{
    uint8_t r, g, b, pad;
};

struct ColorRGBA
{
    uint8_t r, g, b, a;
};

struct MeshHeader3
{
    uint32_t version;
    uint32_t attributes;
    char     meshName[NAME_LEN];
    char     containerName[NAME_LEN];
    uint32_t numTris;
    uint32_t numVertices;
    uint32_t numMaterials;
    uint32_t numDamageStages;
    int32_t  sortLevel;
    uint32_t prelitVersion;
    uint32_t futureCounts[1];
    uint32_t vertexChannels;
    uint32_t faceChannels;
    Vector3  min;
    Vector3  max;
    Vector3  sphCenter;
    float    sphRadius;
};

static_assert(sizeof(MeshHeader3) == 116);

constexpr uint32_t MESH_FLAG_COLLISION_BOX              = 0x00000001;
constexpr uint32_t MESH_FLAG_SKIN                       = 0x00000002;
constexpr uint32_t MESH_FLAG_SHADOW                     = 0x00000004;
constexpr uint32_t MESH_FLAG_ALIGNED                    = 0x00000008;
constexpr uint32_t MESH_FLAG_COLLISION_TYPE_PHYSICAL    = 0x00000010;
constexpr uint32_t MESH_FLAG_COLLISION_TYPE_PROJECTILE  = 0x00000020;
constexpr uint32_t MESH_FLAG_COLLISION_TYPE_VIS         = 0x00000040;
constexpr uint32_t MESH_FLAG_COLLISION_TYPE_CAMERA      = 0x00000080;
constexpr uint32_t MESH_FLAG_COLLISION_TYPE_VEHICLE     = 0x00000100;
constexpr uint32_t MESH_FLAG_HIDDEN                     = 0x00001000;
constexpr uint32_t MESH_FLAG_TWO_SIDED                  = 0x00002000;
constexpr uint32_t MESH_FLAG_CAST_SHADOW                = 0x00008000;
constexpr uint32_t MESH_FLAG_GEOMETRY_TYPE_NORMAL       = 0x00000000;
constexpr uint32_t MESH_FLAG_GEOMETRY_TYPE_CAMERA_ALIGNED = 0x00010000;
constexpr uint32_t MESH_FLAG_GEOMETRY_TYPE_SKIN         = 0x00020000;
constexpr uint32_t MESH_FLAG_PRELIT_MASK                = 0x0F000000;
constexpr uint32_t MESH_FLAG_PRELIT_UNLIT               = 0x01000000;
constexpr uint32_t MESH_FLAG_PRELIT_VERTEX              = 0x02000000;
constexpr uint32_t MESH_FLAG_PRELIT_LIGHTMAP_MULTI_PASS = 0x04000000;
constexpr uint32_t MESH_FLAG_PRELIT_LIGHTMAP_MULTI_TEXTURE = 0x08000000;
inline constexpr int32_t MESH_SORT_LEVEL_NONE = 0;
inline constexpr int32_t MESH_MAX_SORT_LEVEL = 32;
constexpr uint32_t MESH_FLAG_SHATTERABLE                = 0x10000000;

struct Tri
{
    uint32_t vindex[3];
    uint32_t attributes;
    Vector3  normal;
    float    dist;
};

static_assert(sizeof(Tri) == 32);

struct VertInf
{
    uint16_t boneIdx;
    uint8_t  pad[6];
};

struct MaterialInfo
{
    uint32_t passCount;
    uint32_t vertexMaterialCount;
    uint32_t shaderCount;
    uint32_t textureCount;
};

struct VertexMaterial
{
    uint32_t  attributes;
    ColorRGB  ambient;
    ColorRGB  diffuse;
    ColorRGB  specular;
    ColorRGB  emissive;
    float     shininess;
    float     opacity;
    float     translucency;
};

static_assert(sizeof(VertexMaterial) == 32);

// Detached description of the two classic W3D vertex-mapper stages.  The
// packed material attribute only identifies the mapper implementation; its
// authored parameters live in the sibling null-terminated mapper-args
// chunks.  Keeping the resolved values here prevents the renderer from
// retaining INI text or reconstructing legacy mapper objects at draw time.
enum class VertexMapperType : uint8_t
{
    Uv = 0,
    Environment,
    CheapEnvironment,
    LinearOffset,
    Scale,
    Grid,
    Rotate,
    Unsupported,
};

struct VertexMapperDescriptor
{
    VertexMapperType type = VertexMapperType::Uv;
    // Original W3D mapping ID after removing the stage-specific bit shift.
    // Unsupported authored modes therefore remain diagnosable while using
    // the deterministic UV fallback selected by the renderer.
    uint8_t sourceType = 0;
    bool clampFix = false;
    float uScale = 1.0f;
    float vScale = 1.0f;
    float uPerSecond = 0.0f;
    float vPerSecond = 0.0f;
    float uOffset = 0.0f;
    float vOffset = 0.0f;
    float uCenter = 0.0f;
    float vCenter = 0.0f;
    float gridFramesPerSecond = 1.0f;
    uint32_t gridWidthLog2 = 1;
    uint32_t gridLastFrame = 0;
    uint32_t gridOffset = 0;
    // RotateTextureMapper interprets Speed as turns per second, not radians.
    float turnsPerSecond = 0.1f;
};

struct Shader
{
    uint8_t depthCompare;
    uint8_t depthMask;
    uint8_t colorMask;
    uint8_t destBlend;
    uint8_t fogFunc;
    uint8_t priGradient;
    uint8_t secGradient;
    uint8_t srcBlend;
    uint8_t texturing;
    uint8_t detailColorFunc;
    uint8_t detailAlphaFunc;
    uint8_t shaderPreset;
    uint8_t alphaTest;
    uint8_t postDetailColorFunc;
    uint8_t postDetailAlphaFunc;
    uint8_t pad;
};

static_assert(sizeof(Shader) == 16);

struct TextureInfo
{
    uint16_t attributes;
    uint16_t animType;
    uint32_t frameCount;
    float    frameRate;
};

struct HierarchyHeader
{
    uint32_t version;
    char     name[NAME_LEN];
    uint32_t numPivots;
    Vector3  center;
};

struct Pivot
{
    char      name[NAME_LEN];
    uint32_t  parentIdx;
    Vector3   translation;
    Vector3   eulerAngles;
    Quaternion rotation;
};

struct PivotFixup
{
    float tm[4][3];
};

struct AnimHeader
{
    uint32_t version;
    char     name[NAME_LEN];
    char     hierarchyName[NAME_LEN];
    uint32_t numFrames;
    uint32_t frameRate;
};

struct CompressedAnimHeader
{
    uint32_t version;
    char     name[NAME_LEN];
    char     hierarchyName[NAME_LEN];
    uint32_t numFrames;
    uint16_t frameRate;
    uint16_t flavor;
};

static_assert(sizeof(CompressedAnimHeader) == 44);

struct TimeCodedAnimChannel
{
    uint32_t numTimeCodes;
    uint16_t pivot;
    uint8_t  vectorLen;
    uint8_t  flags;
    uint32_t firstData;
};

static_assert(sizeof(TimeCodedAnimChannel) == 12);

struct TimeCodedBitChannel
{
    uint32_t numTimeCodes;
    uint16_t pivot;
    uint8_t  flags;
    uint8_t  defaultValue;
    uint32_t firstData;
};

static_assert(sizeof(TimeCodedBitChannel) == 12);

struct AdaptiveDeltaAnimChannel
{
    uint32_t numFrames;
    uint16_t pivot;
    uint8_t  vectorLen;
    uint8_t  flags;
    float    scale;
    uint32_t firstData;
};

static_assert(sizeof(AdaptiveDeltaAnimChannel) == 16);

constexpr uint16_t COMPRESSED_ANIM_FLAVOR_TIMECODED = 0;
constexpr uint16_t COMPRESSED_ANIM_FLAVOR_ADAPTIVE_DELTA = 1;
constexpr uint32_t TIMECODED_BINARY_MOVEMENT_FLAG = 0x80000000u;
constexpr uint32_t TIMECODED_BIT_MASK = 0x80000000u;

struct AnimChannel
{
    uint16_t firstFrame;
    uint16_t lastFrame;
    uint16_t vectorLen;
    uint16_t flags;
    uint16_t pivot;
    uint16_t pad;
};

enum AnimChannelType : uint16_t
{
    AnimChannel_X  = 0,
    AnimChannel_Y  = 1,
    AnimChannel_Z  = 2,
    AnimChannel_XR = 3,
    AnimChannel_YR = 4,
    AnimChannel_ZR = 5,
    AnimChannel_Q  = 6,
};

struct HModelHeader
{
    uint32_t version;
    char     name[NAME_LEN];
    char     hierarchyName[NAME_LEN];
    uint16_t numConnections;
};

struct HModelNode
{
    char     renderObjName[NAME_LEN];
    uint16_t pivotIdx;
};

struct LODModelHeader
{
    uint32_t version;
    char     name[NAME_LEN];
    uint16_t numLODs;
};

struct LOD
{
    char    renderObjName[32];
    float   lodMin;
    float   lodMax;
};

struct CollectionHeader
{
    uint32_t version;
    char     name[NAME_LEN];
    uint32_t renderObjectCount;
    uint32_t pad[2];
};

struct Light
{
    uint32_t  attributes;
    uint32_t  unused;
    ColorRGB  ambient;
    ColorRGB  diffuse;
    ColorRGB  specular;
    float     intensity;
};

constexpr uint32_t LIGHT_ATTRIBUTE_POINT       = 0x00000001;
constexpr uint32_t LIGHT_ATTRIBUTE_DIRECTIONAL = 0x00000002;
constexpr uint32_t LIGHT_ATTRIBUTE_SPOT        = 0x00000003;
constexpr uint32_t LIGHT_ATTRIBUTE_CAST_SHADOWS = 0x00000100;

struct SpotLight
{
    Vector3 spotDirection;
    float   spotAngle;
    float   spotExponent;
};

struct LightAttenuation
{
    float start;
    float end;
};

struct EmitterHeader
{
    uint32_t version;
    char     name[NAME_LEN];
};

struct EmitterInfo
{
    char     textureFilename[260];
    float    startSize;
    float    endSize;
    float    lifetime;
    float    emissionRate;
    float    maxEmissions;
    float    velocityRandom;
    float    positionRandom;
    float    fadeTime;
    float    gravity;
    float    elasticity;
    Vector3  velocity;
    Vector3  acceleration;
    ColorRGBA startColor;
    ColorRGBA endColor;
};

struct AggregateHeader
{
    uint32_t version;
    char     name[NAME_LEN];
};

struct AggregateInfo
{
    char    baseModelName[32];
    uint32_t subobjectCount;
};

struct AggregateSubobject
{
    char subobjectName[32];
    char boneName[32];
};

struct HLodHeader
{
    uint32_t version;
    uint32_t lodCount;
    char     name[NAME_LEN];
    char     hierarchyName[NAME_LEN];
};

struct HLodArrayHeader
{
    uint32_t modelCount;
    float    maxScreenSize;
};

struct HLodSubObject
{
    uint32_t boneIndex;
    char     name[32];
};

struct Box
{
    uint32_t version;
    uint32_t attributes;
    char     name[32];
    ColorRGB color;
    Vector3  center;
    Vector3  extent;
};

struct NullObject
{
    uint32_t version;
    uint32_t attributes;
    uint32_t pad[2];
    char     name[32];
};

struct AABTreeHeader
{
    uint32_t nodeCount;
    uint32_t polyCount;
    uint32_t padding[6];
};

struct AABTreeNode
{
    Vector3  boxMin;
    Vector3  boxMax;
    int32_t  frontOrPolyIdx;
    int32_t  backOrChildIdx;
};

#pragma pack(pop)

struct ParsedTextureStage
{
    container::Vector<uint32_t> textureIds;
    container::Vector<TexCoord> texCoords;
    container::Vector<container::Array<uint32_t, 3>> perFaceTexCoordIds;
};

struct ParsedMaterialPass
{
    container::Vector<uint32_t> vertexMaterialIds;
    container::Vector<uint32_t> shaderIds;
    container::Vector<ColorRGBA> vertexColors;
    container::Vector<ColorRGB> vertexIllumination;
    container::Vector<ParsedTextureStage> textureStages;
};

enum class ParsedPrelitMode : uint8_t
{
    None,
    Unlit,
    Vertex,
    LightmapMultiPass,
    LightmapMultiTexture,
};

struct ParsedMesh
{
    char           name[NAME_LEN]{};
    char           containerName[NAME_LEN]{};
    uint32_t       attributes = 0;
    // Legacy W3D static transparent-sort bin (0 means SORT_LEVEL_NONE).
    int32_t        sortLevel = 0;
    Vector3        aabbMin{};
    Vector3        aabbMax{};
    Vector3        sphereCenter{};
    float          sphereRadius = 0;
    // Generals fixes WW3D to PRELIT_MODE_LIGHTMAP_MULTI_PASS at display
    // initialization. Preserve the wrapper actually selected by that policy;
    // the raw attribute mask may advertise several alternate wrappers and is
    // therefore not a valid runtime-lighting discriminator by itself.
    ParsedPrelitMode selectedPrelitMode = ParsedPrelitMode::None;

    container::Vector<Vector3>  vertices;
    container::Vector<Vector3>  normals;
    container::Vector<Tri>      triangles;
    container::Vector<TexCoord> texCoords;
    // W3D's classic skin format stores one hierarchy-bone link per vertex.
    // It is intentionally represented as plain indices rather than exposing
    // the packed VertInf file struct beyond the loader.
    container::Vector<uint16_t> vertexBoneIndices;

    container::Vector<VertexMaterial> vertexMaterials;
    container::Vector<container::String>    materialNames;
    // One immutable stage-0/stage-1 pair for every vertexMaterials entry.
    container::Vector<container::Array<VertexMapperDescriptor, 2>> vertexMappers;
    container::Vector<Shader>         shaders;
    container::Vector<container::String>    textureNames;
    container::Vector<TextureInfo>    textureInfos;
    container::Vector<MaterialInfo>   materialInfos;

    // Hierarchical representation matching W3D material pass -> texture stage.
    // The flat arrays below mirror pass 0/stage 0 for transitional callers.
    container::Vector<ParsedMaterialPass> materialPasses;

    container::Vector<uint32_t> vertexMaterialIds;
    container::Vector<uint32_t> shaderIds;
    container::Vector<uint32_t> textureIds;

    container::Vector<ColorRGBA> vertexColors;
    container::Vector<ColorRGB>  vertexIllumination;
};

struct ParsedHierarchy
{
    char              name[NAME_LEN]{};
    uint32_t          numPivots = 0;
    Vector3           center{};
    container::Vector<Pivot> pivots;
};

struct ParsedAnimationChannel
{
    uint16_t firstFrame = 0;
    uint16_t lastFrame = 0;
    uint16_t vectorLength = 0;
    uint16_t flags = 0;
    uint16_t pivotIndex = 0;
    container::Vector<float> values;
};

struct ParsedAnimationVisibilityChannel
{
    uint16_t firstFrame = 0;
    uint16_t lastFrame = 0;
    uint16_t pivotIndex = 0;
    bool defaultVisible = true;
    container::Vector<uint8_t> bits;
};

struct ParsedAnimation
{
    char name[NAME_LEN]{};
    char hierarchyName[NAME_LEN]{};
    uint32_t numFrames = 0;
    uint32_t frameRate = 0;
    container::Vector<ParsedAnimationChannel> channels;
    container::Vector<ParsedAnimationVisibilityChannel> visibilityChannels;
    bool compressed = false;
};

struct ParsedHLodLod
{
    // W3D stores LOD arrays from lowest detail to highest detail.  Keeping the
    // file index explicit makes the fixed highest-detail selection auditable.
    uint32_t               index = 0;
    float                  maxScreenSize = 0;
    container::Vector<HLodSubObject> subObjects;
};

struct ParsedHLod
{
    char                       name[NAME_LEN]{};
    char                       hierarchyName[NAME_LEN]{};
    uint32_t                   lodCount = 0;
    container::Vector<ParsedHLodLod> lods;
    // Aggregate entries are additional render models attached to hierarchy
    // bones; proxy entries are application-defined named bone anchors. Both
    // use the same on-disk subobject-array framing as a LOD array.
    container::Vector<HLodSubObject> aggregates;
    container::Vector<HLodSubObject> proxies;

    [[nodiscard]] const ParsedHLodLod* highestDetailLod() const noexcept
    {
        return lods.empty() ? nullptr : &lods.back();
    }

    [[nodiscard]] ParsedHLodLod* highestDetailLod() noexcept
    {
        return lods.empty() ? nullptr : &lods.back();
    }
};

struct ParsedW3D
{
    container::Vector<ParsedMesh>      meshes;
    container::Vector<ParsedHierarchy> hierarchies;
    container::Vector<ParsedAnimation> animations;
    container::Vector<ParsedHLod>      hlods;
};

} // namespace data::w3d
