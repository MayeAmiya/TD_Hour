#pragma once

#include "container/container_types.h"

#include "W3dTypes.h"
#include <cstdint>
namespace data::w3d {

class W3dLoader
{
public:
    W3dLoader() = default;

    bool loadFromFile(const container::String& path);
    bool loadFromMemory(const uint8_t* data, size_t size);

    const ParsedW3D& result() const { return m_result; }
    ParsedW3D takeResult() { return std::move(m_result); }
    const container::String& error() const { return m_error; }

    void reset();

private:
    struct ChunkReader
    {
        const uint8_t* data;
        size_t         size;
        size_t         offset;

        ChunkReader(const uint8_t* d, size_t s);

        bool readChunkHeader(ChunkHeader& header);
        bool readBytes(void* dst, size_t count);
        bool skipBytes(size_t count);
        bool atEnd() const;
        size_t remaining() const;
        ChunkReader subReader(uint32_t chunkSize) const;
    };

    bool parseChunks(ChunkReader reader, uint32_t endOffset);

    bool parseMesh(ChunkReader reader, uint32_t chunkSize);
    bool parseMeshMaterialContainer(ChunkReader reader, uint32_t chunkSize,
                                    ParsedMesh& mesh);
    bool parseMeshHeader3(ChunkReader reader, ParsedMesh& mesh);
    bool parseVertices(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseVertexNormals(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseTriangles(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseVertexInfluences(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseTexCoords(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseShaders(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseVertexMaterials(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseTextures(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseMaterialPass(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseTextureStage(ChunkReader reader, ParsedTextureStage& stage, uint32_t chunkSize);
    bool parseVertexColors(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseVertexIllumination(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize);
    bool parseMaterialInfo(ChunkReader reader, ParsedMesh& mesh);

    bool parseHierarchy(ChunkReader reader, uint32_t chunkSize);
    bool parseHierarchyHeader(ChunkReader reader, ParsedHierarchy& hierarchy);
    bool parsePivots(ChunkReader reader, ParsedHierarchy& hierarchy, uint32_t chunkSize);

    bool parseAnimation(ChunkReader reader, uint32_t chunkSize);
    bool parseCompressedAnimation(ChunkReader reader, uint32_t chunkSize);
    bool parseAnimationHeader(ChunkReader reader, ParsedAnimation& animation);
    bool parseAnimationChannel(ChunkReader reader, ParsedAnimation& animation, uint32_t chunkSize);
    bool parseAnimationVisibilityChannel(ChunkReader reader, ParsedAnimation& animation, uint32_t chunkSize);
    bool parseTimeCodedAnimationChannel(ChunkReader reader, ParsedAnimation& animation,
                                        uint32_t chunkSize);
    bool parseTimeCodedVisibilityChannel(ChunkReader reader, ParsedAnimation& animation,
                                         uint32_t chunkSize);
    bool parseAdaptiveDeltaAnimationChannel(ChunkReader reader, ParsedAnimation& animation,
                                            uint32_t chunkSize);

    bool parseHLod(ChunkReader reader, uint32_t chunkSize);
    bool parseHLodLodArray(ChunkReader reader, uint32_t chunkSize,
                           uint32_t lodIndex, ParsedHLodLod& lod);

    void setError(const container::String& msg);

    ParsedW3D  m_result;
    container::String m_error;
};

} // namespace data::w3d
