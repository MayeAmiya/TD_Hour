#include "container/container_types.h"
#include "W3dLoader.h"

#include "VFS.h"
#include "LocaleResourceLocator.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <utility>
namespace data::w3d {
namespace {

std::optional<uint16_t> classicChannelFlag(uint8_t compressedFlag) {
    // RefCode's compressed reader ultimately installs X/Y/Z/Q channels. Some
    // exporters write the base channel IDs, others write the time-coded IDs.
    switch (compressedFlag) {
    case AnimChannel_X: case 7: case 11: return AnimChannel_X;
    case AnimChannel_Y: case 8: case 12: return AnimChannel_Y;
    case AnimChannel_Z: case 9: case 13: return AnimChannel_Z;
    case AnimChannel_Q: case 10: case 14: return AnimChannel_Q;
    default: return std::nullopt;
    }
}

float adaptiveFilter(uint8_t index) {
    if (index < 16) {
        constexpr float values[] = {1e-8f,1e-7f,1e-6f,1e-5f,1e-4f,1e-3f,1e-2f,1e-1f,
                                    1.0f,10.0f,100.0f,1000.0f,10000.0f,100000.0f,1000000.0f,10000000.0f};
        return values[index];
    }
    const float ratio = static_cast<float>(index - 16) / 240.0f;
    return 1.0f - std::sin(1.57079632679f * ratio);
}

[[nodiscard]] char lowerAscii(char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] container::StringView trimAscii(container::StringView value) noexcept {
    const auto whitespace = [](char character) {
        return character == ' ' || character == '\t' ||
               character == '\r' || character == '\n';
    };
    while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
    while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
    return value;
}

struct VertexMapperArguments final {
    container::Vector<std::pair<container::String, container::String>> values;

    [[nodiscard]] static container::StringView scalarText(
        container::StringView value) noexcept {
        // WW3D mapper arguments are INI text. Shipped assets commonly append
        // an INI ';' comment directly after a scalar (for example FPS=30; ...).
        // Strip that comment before using the stricter from_chars parser.
        const size_t comment = value.find(';');
        if (comment != container::StringView::npos) {
            value = value.substr(0, comment);
        }
        return trimAscii(value);
    }

    [[nodiscard]] container::StringView find(container::StringView requested) const noexcept {
        for (const auto& [key, value] : values) {
            if (key.size() != requested.size()) continue;
            bool equal = true;
            for (size_t index = 0; index < key.size(); ++index) {
                if (lowerAscii(key[index]) != lowerAscii(requested[index])) {
                    equal = false;
                    break;
                }
            }
            if (equal) return value;
        }
        return {};
    }

    [[nodiscard]] float number(container::StringView key, float fallback) const {
        container::StringView value = scalarText(find(key));
        if (value.empty()) return fallback;
        // Several shipped W3D files use legacy INI spellings such as -.05.
        // Normalize only that missing leading zero before using the
        // locale-independent floating parser.
        container::String normalized;
        const size_t decimalIndex = value.front() == '-' || value.front() == '+' ? 1u : 0u;
        if (decimalIndex < value.size() && value[decimalIndex] == '.') {
            normalized.reserve(value.size() + 1);
            normalized.append(value.substr(0, decimalIndex));
            normalized.push_back('0');
            normalized.append(value.substr(decimalIndex));
            value = normalized;
        }
        float parsed = fallback;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        return result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                std::isfinite(parsed)
            ? parsed : fallback;
    }

    [[nodiscard]] int integer(container::StringView key, int fallback) const {
        const container::StringView value = scalarText(find(key));
        if (value.empty()) return fallback;
        int parsed = fallback;
        const auto result = std::from_chars(
            value.data(), value.data() + value.size(), parsed);
        return result.ec == std::errc{} &&
                result.ptr == value.data() + value.size()
            ? parsed : fallback;
    }

    [[nodiscard]] bool boolean(container::StringView key, bool fallback) const {
        const container::StringView value = trimAscii(find(key));
        if (value.empty()) return fallback;
        container::String folded;
        folded.reserve(value.size());
        for (char character : value) folded.push_back(lowerAscii(character));
        if (folded == "true" || folded == "yes" || folded == "on" || folded == "1") {
            return true;
        }
        if (folded == "false" || folded == "no" || folded == "off" || folded == "0") {
            return false;
        }
        return fallback;
    }
};

[[nodiscard]] VertexMapperArguments parseVertexMapperArguments(
    container::StringView source) {
    VertexMapperArguments output;
    while (!source.empty()) {
        const size_t lineEnd = source.find_first_of("\r\n");
        container::StringView line = trimAscii(source.substr(0, lineEnd));
        if (lineEnd == container::StringView::npos) {
            source = {};
        } else {
            const char separator = source[lineEnd];
            source.remove_prefix(lineEnd + 1);
            if (separator == '\r' && !source.empty() && source.front() == '\n') {
                source.remove_prefix(1);
            }
        }
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        const size_t separator = line.find('=');
        if (separator == container::StringView::npos) continue;
        const container::StringView key = trimAscii(line.substr(0, separator));
        const container::StringView value = trimAscii(line.substr(separator + 1));
        if (key.empty()) continue;
        output.values.emplace_back(key, value);
    }
    return output;
}

[[nodiscard]] VertexMapperDescriptor makeVertexMapperDescriptor(
    uint32_t attributes, uint32_t stage, container::StringView argumentText) {
    VertexMapperDescriptor descriptor;
    descriptor.sourceType = static_cast<uint8_t>(
        stage == 0 ? ((attributes >> 16u) & 0xffu)
                   : ((attributes >> 8u) & 0xffu));
    const VertexMapperArguments arguments = parseVertexMapperArguments(argumentText);
    descriptor.uScale = arguments.number("UScale", 1.0f);
    descriptor.vScale = arguments.number("VScale", 1.0f);

    switch (descriptor.sourceType) {
    case 0:
        descriptor.type = VertexMapperType::Uv;
        break;
    case 1:
        descriptor.type = VertexMapperType::Environment;
        break;
    case 2:
        descriptor.type = VertexMapperType::CheapEnvironment;
        break;
    case 4:
        descriptor.type = VertexMapperType::LinearOffset;
        descriptor.uPerSecond = arguments.number("UPerSec", 0.0f);
        descriptor.vPerSecond = arguments.number("VPerSec", 0.0f);
        descriptor.uOffset = arguments.number("UOffset", 0.0f);
        descriptor.vOffset = arguments.number("VOffset", 0.0f);
        descriptor.clampFix = arguments.boolean("ClampFix", false);
        break;
    case 6:
        descriptor.type = VertexMapperType::Scale;
        break;
    case 7:
        descriptor.type = VertexMapperType::Grid;
        descriptor.gridFramesPerSecond = arguments.number("FPS", 1.0f);
        descriptor.gridWidthLog2 = static_cast<uint32_t>(std::clamp(
            arguments.integer("Log2Width", 1), 0, 15));
        descriptor.gridLastFrame = static_cast<uint32_t>(std::max(
            0, arguments.integer("Last", 0)));
        descriptor.gridOffset = static_cast<uint32_t>(std::max(
            0, arguments.integer("Offset", 0)));
        break;
    case 8:
        descriptor.type = VertexMapperType::Rotate;
        descriptor.turnsPerSecond = arguments.number("Speed", 0.1f);
        descriptor.uCenter = arguments.number("UCenter", 0.0f);
        descriptor.vCenter = arguments.number("VCenter", 0.0f);
        break;
    default:
        descriptor.type = VertexMapperType::Unsupported;
        break;
    }
    return descriptor;
}

void slerpQuaternion(const float* lhs, const float* rhs, float amount, float* output) {
    float bx = rhs[0], by = rhs[1], bz = rhs[2], bw = rhs[3];
    float dot = lhs[0] * bx + lhs[1] * by + lhs[2] * bz + lhs[3] * bw;
    if (dot < 0.0f) {
        dot = -dot;
        bx = -bx; by = -by; bz = -bz; bw = -bw;
    }
    float lhsWeight = 1.0f - amount;
    float rhsWeight = amount;
    if (dot < 0.9995f) {
        const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
        const float inverseSin = 1.0f / std::sin(theta);
        lhsWeight = std::sin((1.0f - amount) * theta) * inverseSin;
        rhsWeight = std::sin(amount * theta) * inverseSin;
    }
    output[0] = lhs[0] * lhsWeight + bx * rhsWeight;
    output[1] = lhs[1] * lhsWeight + by * rhsWeight;
    output[2] = lhs[2] * lhsWeight + bz * rhsWeight;
    output[3] = lhs[3] * lhsWeight + bw * rhsWeight;
    const float lengthSq = output[0] * output[0] + output[1] * output[1] +
                           output[2] * output[2] + output[3] * output[3];
    if (lengthSq > 0.0f) {
        const float inverseLength = 1.0f / std::sqrt(lengthSq);
        for (size_t index = 0; index < 4; ++index) output[index] *= inverseLength;
    }
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════════
// ChunkReader
// ══════════════════════════════════════════════════════════════════════════════

W3dLoader::ChunkReader::ChunkReader(const uint8_t* d, size_t s)
    : data(d), size(s), offset(0)
{
}

bool W3dLoader::ChunkReader::readChunkHeader(ChunkHeader& header)
{
    if (offset + sizeof(ChunkHeader) > size)
        return false;
    std::memcpy(&header, data + offset, sizeof(ChunkHeader));
    header.size &= 0x7FFFFFFF;
    offset += sizeof(ChunkHeader);
    return true;
}

bool W3dLoader::ChunkReader::readBytes(void* dst, size_t count)
{
    if (offset + count > size)
        return false;
    std::memcpy(dst, data + offset, count);
    offset += count;
    return true;
}

bool W3dLoader::ChunkReader::skipBytes(size_t count)
{
    if (offset + count > size)
        return false;
    offset += count;
    return true;
}

bool W3dLoader::ChunkReader::atEnd() const
{
    return offset >= size;
}

size_t W3dLoader::ChunkReader::remaining() const
{
    return (offset < size) ? (size - offset) : 0;
}

W3dLoader::ChunkReader W3dLoader::ChunkReader::subReader(uint32_t chunkSize) const
{
    size_t actualSize = (offset + chunkSize <= size) ? chunkSize : (size - offset);
    return ChunkReader(data + offset, actualSize);
}

// ══════════════════════════════════════════════════════════════════════════════
// Public interface
// ══════════════════════════════════════════════════════════════════════════════

void W3dLoader::reset()
{
    m_result = ParsedW3D{};
    m_error.clear();
}

bool W3dLoader::loadFromFile(const container::String& path)
{
    container::Vector<uint8_t> buffer;
    container::String resolvedPath = path;
    bool sourceAvailable = true;
    if (const auto locator = io::acquireLocaleResourceLocator()) {
        const std::optional<container::String> resolved = locator->resolve(
            io::LocaleResourceKind::W3d, path);
        sourceAvailable = resolved.has_value();
        if (resolved) resolvedPath = *resolved;
    }
    if (!sourceAvailable ||
        !io::VFS::instance().readToBuffer(resolvedPath, buffer))
    {
        setError("Failed to read file: " + path);
        return false;
    }
    return loadFromMemory(buffer.data(), buffer.size());
}

bool W3dLoader::loadFromMemory(const uint8_t* data, size_t size)
{
    reset();

    if (!data || size < sizeof(ChunkHeader))
    {
        setError("Invalid data: null or too small");
        return false;
    }

    ChunkReader reader(data, size);
    return parseChunks(reader, static_cast<uint32_t>(size));
}

// ══════════════════════════════════════════════════════════════════════════════
// Chunk dispatch
// ══════════════════════════════════════════════════════════════════════════════

bool W3dLoader::parseChunks(ChunkReader reader, uint32_t endOffset)
{
    while (!reader.atEnd() && reader.offset < endOffset)
    {
        ChunkHeader header;
        if (!reader.readChunkHeader(header))
            break;

        size_t chunkDataStart = reader.offset;
        uint32_t chunkSize = header.size;

        switch (header.type)
        {
        case Chunk_Mesh:
            if (!parseMesh(reader.subReader(chunkSize), chunkSize)) return false;
            break;
        case Chunk_Hierarchy:
            if (!parseHierarchy(reader.subReader(chunkSize), chunkSize))
                return false;
            break;
        case Chunk_Animation:
            parseAnimation(reader.subReader(chunkSize), chunkSize);
            break;
        case Chunk_CompressedAnimation:
            if (!parseCompressedAnimation(reader.subReader(chunkSize), chunkSize)) return false;
            break;
        case Chunk_HLod:
            if (!parseHLod(reader.subReader(chunkSize), chunkSize))
                return false;
            break;
        default:
            break;
        }

        reader.offset = chunkDataStart + chunkSize;
    }
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Mesh
// ══════════════════════════════════════════════════════════════════════════════

bool W3dLoader::parseMesh(ChunkReader reader, uint32_t chunkSize)
{
    ParsedMesh mesh;
    uint32_t endOffset = static_cast<uint32_t>(reader.offset) + chunkSize;
    bool parsedSelectedPrelitWrapper = false;

    const auto selectedPrelitChunk = [&mesh]() noexcept -> uint32_t {
        const uint32_t flags = mesh.attributes & MESH_FLAG_PRELIT_MASK;
        // W3DDisplay.cpp fixes Generals to PRELIT_MODE_LIGHTMAP_MULTI_PASS.
        // MeshModelClass then falls through to vertex and finally unlit. The
        // generic WW3D multi-texture mode is deliberately not preferred here.
        if ((flags & MESH_FLAG_PRELIT_LIGHTMAP_MULTI_PASS) != 0)
            return Chunk_PrelitLightmapMultiPass;
        if ((flags & MESH_FLAG_PRELIT_VERTEX) != 0)
            return Chunk_PrelitVertex;
        if ((flags & MESH_FLAG_PRELIT_UNLIT) != 0)
            return Chunk_PrelitUnlit;
        return 0;
    };

    while (!reader.atEnd() && reader.offset < endOffset)
    {
        ChunkHeader header;
        if (!reader.readChunkHeader(header))
            break;

        size_t subDataStart = reader.offset;
        uint32_t subSize = header.size;

        switch (header.type)
        {
        case Chunk_MeshHeader3:
            parseMeshHeader3(reader.subReader(subSize), mesh);
            break;
        case Chunk_Vertices:
            parseVertices(reader.subReader(subSize), mesh, subSize);
            break;
        case Chunk_VertexNormals:
            parseVertexNormals(reader.subReader(subSize), mesh, subSize);
            break;
        case Chunk_Triangles:
            parseTriangles(reader.subReader(subSize), mesh, subSize);
            break;
        case Chunk_VertexShadeIndices:
            break;
        case Chunk_MaterialInfo:
            if (selectedPrelitChunk() == 0 &&
                !parseMaterialInfo(reader.subReader(subSize), mesh))
                return false;
            break;
        case Chunk_Shaders:
            if (selectedPrelitChunk() == 0 &&
                !parseShaders(reader.subReader(subSize), mesh, subSize))
                return false;
            break;
        case Chunk_VertexMaterials:
            if (selectedPrelitChunk() == 0 &&
                !parseVertexMaterials(reader.subReader(subSize), mesh, subSize)) {
                setError("Invalid W3D vertex material or mapper-args chunk");
                return false;
            }
            break;
        case Chunk_Textures:
            if (selectedPrelitChunk() == 0 &&
                !parseTextures(reader.subReader(subSize), mesh, subSize))
                return false;
            break;
        case Chunk_MaterialPass:
            if (selectedPrelitChunk() == 0 &&
                !parseMaterialPass(reader.subReader(subSize), mesh, subSize))
                return false;
            break;
        case Chunk_PrelitUnlit:
        case Chunk_PrelitVertex:
        case Chunk_PrelitLightmapMultiPass:
        case Chunk_PrelitLightmapMultiTexture:
            if (header.type == selectedPrelitChunk()) {
                if (parsedSelectedPrelitWrapper) {
                    setError("Duplicate selected W3D prelit material wrapper");
                    return false;
                }
                if (!parseMeshMaterialContainer(
                        reader.subReader(subSize), subSize, mesh))
                    return false;
                switch (header.type) {
                case Chunk_PrelitLightmapMultiPass:
                    mesh.selectedPrelitMode = ParsedPrelitMode::LightmapMultiPass;
                    break;
                case Chunk_PrelitVertex:
                    mesh.selectedPrelitMode = ParsedPrelitMode::Vertex;
                    break;
                case Chunk_PrelitUnlit:
                    mesh.selectedPrelitMode = ParsedPrelitMode::Unlit;
                    break;
                case Chunk_PrelitLightmapMultiTexture:
                    mesh.selectedPrelitMode = ParsedPrelitMode::LightmapMultiTexture;
                    break;
                default:
                    break;
                }
                parsedSelectedPrelitWrapper = true;
            }
            break;
        case Chunk_AABTree:
            break;
        case Chunk_MeshUserText:
            break;
        case Chunk_VertexInfluences:
            if (!parseVertexInfluences(reader.subReader(subSize), mesh, subSize)) return false;
            break;
        default:
            break;
        }

        reader.offset = subDataStart + subSize;
    }

    if (selectedPrelitChunk() != 0 && !parsedSelectedPrelitWrapper) {
        setError("W3D mesh declares a prelit material wrapper that is missing");
        return false;
    }

    m_result.meshes.push_back(std::move(mesh));
    return true;
}

bool W3dLoader::parseMeshMaterialContainer(
    ChunkReader reader, uint32_t chunkSize, ParsedMesh& mesh)
{
    if (chunkSize > reader.size) return false;
    const size_t endOffset = reader.offset + chunkSize;
    while (!reader.atEnd() && reader.offset < endOffset)
    {
        ChunkHeader header;
        if (!reader.readChunkHeader(header) || header.size > reader.remaining())
            return false;
        const size_t dataStart = reader.offset;
        const uint32_t size = header.size;
        bool valid = true;
        switch (header.type)
        {
        case Chunk_MaterialInfo:
            valid = parseMaterialInfo(reader.subReader(size), mesh);
            break;
        case Chunk_Shaders:
            valid = parseShaders(reader.subReader(size), mesh, size);
            break;
        case Chunk_VertexMaterials:
            valid = parseVertexMaterials(reader.subReader(size), mesh, size);
            break;
        case Chunk_Textures:
            valid = parseTextures(reader.subReader(size), mesh, size);
            break;
        case Chunk_MaterialPass:
            valid = parseMaterialPass(reader.subReader(size), mesh, size);
            break;
        default:
            break;
        }
        if (!valid) {
            setError("Invalid selected W3D prelit material wrapper");
            return false;
        }
        reader.offset = dataStart + size;
    }
    return reader.offset == endOffset;
}

bool W3dLoader::parseMeshHeader3(ChunkReader reader, ParsedMesh& mesh)
{
    MeshHeader3 header;
    if (!reader.readBytes(&header, sizeof(header)))
        return false;

    // DefaultStaticSortListClass accepts only SORT_LEVEL_NONE or bins 1..32.
    // Reject malformed values at the file boundary instead of letting a
    // signed negative masquerade as an ordinary dynamically sorted mesh, or
    // creating a modern-only bin above the legacy maximum.
    if (header.sortLevel < MESH_SORT_LEVEL_NONE ||
        header.sortLevel > MESH_MAX_SORT_LEVEL) {
        setError("W3D mesh sort level is outside the legacy 0..32 range");
        return false;
    }

    std::memcpy(mesh.name, header.meshName, NAME_LEN);
    std::memcpy(mesh.containerName, header.containerName, NAME_LEN);
    mesh.attributes = header.attributes;
    mesh.sortLevel = header.sortLevel;
    mesh.aabbMin = header.min;
    mesh.aabbMax = header.max;
    mesh.sphereCenter = header.sphCenter;
    mesh.sphereRadius = header.sphRadius;

    mesh.vertices.reserve(header.numVertices);
    mesh.normals.reserve(header.numVertices);
    mesh.triangles.reserve(header.numTris);

    return true;
}

bool W3dLoader::parseVertices(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    uint32_t count = chunkSize / sizeof(Vector3);
    mesh.vertices.resize(count);
    return reader.readBytes(mesh.vertices.data(), count * sizeof(Vector3));
}

bool W3dLoader::parseVertexNormals(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    uint32_t count = chunkSize / sizeof(Vector3);
    mesh.normals.resize(count);
    return reader.readBytes(mesh.normals.data(), count * sizeof(Vector3));
}

bool W3dLoader::parseTriangles(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    uint32_t count = chunkSize / sizeof(Tri);
    mesh.triangles.resize(count);
    return reader.readBytes(mesh.triangles.data(), count * sizeof(Tri));
}

bool W3dLoader::parseVertexInfluences(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    if (chunkSize % sizeof(VertInf) != 0) return false;
    const uint32_t count = chunkSize / sizeof(VertInf);
    if (!mesh.vertices.empty() && count != mesh.vertices.size()) return false;

    container::Vector<VertInf> influences(count);
    if (count != 0 && !reader.readBytes(influences.data(), chunkSize)) return false;
    mesh.vertexBoneIndices.resize(count);
    for (uint32_t index = 0; index < count; ++index) {
        mesh.vertexBoneIndices[index] = influences[index].boneIdx;
    }
    return true;
}

bool W3dLoader::parseTexCoords(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    uint32_t count = chunkSize / sizeof(TexCoord);
    mesh.texCoords.resize(count);
    return reader.readBytes(mesh.texCoords.data(), count * sizeof(TexCoord));
}

bool W3dLoader::parseMaterialInfo(ChunkReader reader, ParsedMesh& mesh)
{
    MaterialInfo info;
    if (!reader.readBytes(&info, sizeof(info)))
        return false;
    mesh.materialInfos.push_back(info);
    return true;
}

bool W3dLoader::parseShaders(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    uint32_t count = chunkSize / sizeof(Shader);
    mesh.shaders.resize(count);
    return reader.readBytes(mesh.shaders.data(), count * sizeof(Shader));
}

bool W3dLoader::parseVertexMaterials(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    uint32_t endOffset = static_cast<uint32_t>(reader.offset) + chunkSize;

    while (!reader.atEnd() && reader.offset < endOffset)
    {
        ChunkHeader header;
        if (!reader.readChunkHeader(header))
            break;

        size_t subDataStart = reader.offset;
        uint32_t subSize = header.size;

        if (header.type == Chunk_VertexMaterial)
        {
            if (subSize > reader.remaining()) return false;
            uint32_t vmEnd = static_cast<uint32_t>(reader.offset) + subSize;
            VertexMaterial vertexMaterial{};
            bool hasVertexMaterial = false;
            container::String materialName;
            container::Array<container::String, 2> mapperArguments;
            while (!reader.atEnd() && reader.offset < vmEnd)
            {
                ChunkHeader vmHeader;
                if (!reader.readChunkHeader(vmHeader))
                    return false;
                size_t vmSubStart = reader.offset;
                if (vmHeader.size > vmEnd - vmSubStart) return false;

                if (vmHeader.type == Chunk_VertexMaterialInfo)
                {
                    if (hasVertexMaterial || vmHeader.size < sizeof(vertexMaterial) ||
                        !reader.readBytes(&vertexMaterial, sizeof(vertexMaterial))) {
                        return false;
                    }
                    hasVertexMaterial = true;
                }
                else if (vmHeader.type == Chunk_VertexMaterialName)
                {
                    materialName.resize(vmHeader.size);
                    if (vmHeader.size != 0 &&
                        !reader.readBytes(materialName.data(), vmHeader.size)) return false;
                    const size_t terminator = materialName.find('\0');
                    if (terminator != container::String::npos) materialName.resize(terminator);
                }
                else if (vmHeader.type == Chunk_VertexMapperArgs0 ||
                         vmHeader.type == Chunk_VertexMapperArgs1)
                {
                    const size_t stage = vmHeader.type == Chunk_VertexMapperArgs0 ? 0u : 1u;
                    mapperArguments[stage].resize(vmHeader.size);
                    if (vmHeader.size != 0 &&
                        !reader.readBytes(mapperArguments[stage].data(), vmHeader.size)) {
                        return false;
                    }
                    const size_t terminator = mapperArguments[stage].find('\0');
                    if (terminator != container::String::npos) {
                        mapperArguments[stage].resize(terminator);
                    }
                }

                reader.offset = vmSubStart + vmHeader.size;
            }
            if (!hasVertexMaterial) return false;
            mesh.vertexMaterials.push_back(vertexMaterial);
            mesh.materialNames.push_back(std::move(materialName));
            mesh.vertexMappers.push_back({
                makeVertexMapperDescriptor(vertexMaterial.attributes, 0, mapperArguments[0]),
                makeVertexMapperDescriptor(vertexMaterial.attributes, 1, mapperArguments[1]),
            });
        }

        reader.offset = subDataStart + subSize;
    }
    return true;
}

bool W3dLoader::parseTextures(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    uint32_t endOffset = static_cast<uint32_t>(reader.offset) + chunkSize;

    while (!reader.atEnd() && reader.offset < endOffset)
    {
        ChunkHeader header;
        if (!reader.readChunkHeader(header))
            break;

        size_t subDataStart = reader.offset;
        uint32_t subSize = header.size;

        if (header.type == Chunk_Texture)
        {
            uint32_t texEnd = static_cast<uint32_t>(reader.offset) + subSize;
            container::String textureName;
            TextureInfo textureInfo{};
            bool hasTextureName = false;
            bool hasTextureInfo = false;
            while (!reader.atEnd() && reader.offset < texEnd)
            {
                ChunkHeader texHeader;
                if (!reader.readChunkHeader(texHeader))
                    break;
                size_t texSubStart = reader.offset;

                if (texHeader.type == Chunk_TextureName)
                {
                    char c;
                    while (reader.offset < texSubStart + texHeader.size)
                    {
                        if (!reader.readBytes(&c, 1))
                            break;
                        if (c == '\0')
                            break;
                        textureName += c;
                    }
                    hasTextureName = true;
                }
                else if (texHeader.type == Chunk_TextureInfo)
                {
                    hasTextureInfo = reader.readBytes(&textureInfo, sizeof(textureInfo));
                }

                reader.offset = texSubStart + texHeader.size;
            }
            // TextureInfo is optional, but its index is defined by the parent
            // texture record. Keep it aligned with textureNames even when an
            // earlier record omits the optional chunk.
            if (hasTextureName) {
                mesh.textureNames.push_back(std::move(textureName));
                mesh.textureInfos.push_back(hasTextureInfo ? textureInfo : TextureInfo{});
            }
        }

        reader.offset = subDataStart + subSize;
    }
    return true;
}

bool W3dLoader::parseMaterialPass(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    const uint32_t endOffset = static_cast<uint32_t>(reader.offset) + chunkSize;
    ParsedMaterialPass pass;

    while (!reader.atEnd() && reader.offset < endOffset)
    {
        ChunkHeader header;
        if (!reader.readChunkHeader(header))
            break;

        size_t subDataStart = reader.offset;
        uint32_t subSize = header.size;

        switch (header.type)
        {
        case Chunk_VertexMaterialIds:
        {
            if (subSize % sizeof(uint32_t) != 0) return false;
            const uint32_t count = subSize / sizeof(uint32_t);
            pass.vertexMaterialIds.resize(count);
            if (!reader.readBytes(pass.vertexMaterialIds.data(), subSize)) return false;
            break;
        }
        case Chunk_ShaderIds:
        {
            if (subSize % sizeof(uint32_t) != 0) return false;
            const uint32_t count = subSize / sizeof(uint32_t);
            pass.shaderIds.resize(count);
            if (!reader.readBytes(pass.shaderIds.data(), subSize)) return false;
            break;
        }
        case Chunk_DCG:
        {
            if (subSize % sizeof(ColorRGBA) != 0) return false;
            pass.vertexColors.resize(subSize / sizeof(ColorRGBA));
            if (!reader.readBytes(pass.vertexColors.data(), subSize)) return false;
            break;
        }
        case Chunk_DIG:
        {
            if (subSize % sizeof(ColorRGB) != 0) return false;
            pass.vertexIllumination.resize(subSize / sizeof(ColorRGB));
            if (!reader.readBytes(pass.vertexIllumination.data(), subSize)) return false;
            break;
        }
        case Chunk_SCG:
            break;
        case Chunk_TextureStage:
        {
            ParsedTextureStage stage;
            if (!parseTextureStage(reader.subReader(subSize), stage, subSize)) return false;
            pass.textureStages.push_back(std::move(stage));
            break;
        }
        default:
            break;
        }

        reader.offset = subDataStart + subSize;
    }

    // Preserve a compatibility view for current pass-0/stage-0 consumers
    // while retaining all hierarchy for the modern material pipeline.
    if (mesh.materialPasses.empty()) {
        mesh.vertexMaterialIds = pass.vertexMaterialIds;
        mesh.shaderIds = pass.shaderIds;
        mesh.vertexColors = pass.vertexColors;
        mesh.vertexIllumination = pass.vertexIllumination;
        if (!pass.textureStages.empty()) {
            mesh.textureIds = pass.textureStages.front().textureIds;
            mesh.texCoords = pass.textureStages.front().texCoords;
        }
    }
    mesh.materialPasses.push_back(std::move(pass));
    return true;
}

bool W3dLoader::parseTextureStage(ChunkReader reader, ParsedTextureStage& stage,
                                  uint32_t chunkSize)
{
    const uint32_t endOffset = static_cast<uint32_t>(reader.offset) + chunkSize;
    while (!reader.atEnd() && reader.offset < endOffset)
    {
        ChunkHeader header;
        if (!reader.readChunkHeader(header)) return false;
        const size_t subDataStart = reader.offset;
        const uint32_t subSize = header.size;
        if (subSize > reader.remaining()) return false;

        switch (header.type)
        {
        case Chunk_TextureIds:
            if (subSize % sizeof(uint32_t) != 0) return false;
            stage.textureIds.resize(subSize / sizeof(uint32_t));
            if (!reader.readBytes(stage.textureIds.data(), subSize)) return false;
            break;
        case Chunk_StageTexCoords:
            if (subSize % sizeof(TexCoord) != 0) return false;
            stage.texCoords.resize(subSize / sizeof(TexCoord));
            if (!reader.readBytes(stage.texCoords.data(), subSize)) return false;
            break;
        case Chunk_PerFaceTexCoordIds:
            if (subSize % (sizeof(uint32_t) * 3) != 0) return false;
            stage.perFaceTexCoordIds.resize(subSize / (sizeof(uint32_t) * 3));
            if (!reader.readBytes(stage.perFaceTexCoordIds.data(), subSize)) return false;
            break;
        default:
            break;
        }

        reader.offset = subDataStart + subSize;
    }
    return true;
}

bool W3dLoader::parseVertexColors(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    uint32_t count = chunkSize / sizeof(ColorRGBA);
    size_t oldSize = mesh.vertexColors.size();
    mesh.vertexColors.resize(oldSize + count);
    return reader.readBytes(mesh.vertexColors.data() + oldSize, count * sizeof(ColorRGBA));
}

bool W3dLoader::parseVertexIllumination(ChunkReader reader, ParsedMesh& mesh, uint32_t chunkSize)
{
    uint32_t count = chunkSize / sizeof(ColorRGB);
    size_t oldSize = mesh.vertexIllumination.size();
    mesh.vertexIllumination.resize(oldSize + count);
    return reader.readBytes(mesh.vertexIllumination.data() + oldSize, count * sizeof(ColorRGB));
}

// ══════════════════════════════════════════════════════════════════════════════
// Hierarchy
// ══════════════════════════════════════════════════════════════════════════════

bool W3dLoader::parseHierarchy(ChunkReader reader, uint32_t chunkSize)
{
    ParsedHierarchy hierarchy;
    if (chunkSize > reader.size)
    {
        setError("Truncated hierarchy chunk");
        return false;
    }

    const size_t endOffset = reader.offset + chunkSize;
    bool sawHeader = false;
    bool sawPivots = false;

    while (!reader.atEnd() && reader.offset < endOffset)
    {
        ChunkHeader header;
        if (!reader.readChunkHeader(header))
        {
            setError("Truncated hierarchy subchunk header");
            return false;
        }

        size_t subDataStart = reader.offset;
        uint32_t subSize = header.size;
        if (subSize > reader.remaining())
        {
            setError("Hierarchy subchunk exceeds its parent chunk");
            return false;
        }
        if (!sawHeader && header.type != Chunk_HierarchyHeader)
        {
            setError("Hierarchy does not begin with its header");
            return false;
        }

        switch (header.type)
        {
        case Chunk_HierarchyHeader:
            if (sawHeader)
            {
                setError("Duplicate hierarchy header");
                return false;
            }
            if (sawPivots)
            {
                setError("Hierarchy header appears after pivots");
                return false;
            }
            if (!parseHierarchyHeader(reader.subReader(subSize), hierarchy))
                return false;
            sawHeader = true;
            break;
        case Chunk_Pivots:
            if (!sawHeader)
            {
                setError("Hierarchy pivots appear before the header");
                return false;
            }
            if (sawPivots)
            {
                setError("Duplicate hierarchy pivots chunk");
                return false;
            }
            if (!parsePivots(reader.subReader(subSize), hierarchy, subSize))
                return false;
            sawPivots = true;
            break;
        case Chunk_PivotFixups:
            break;
        default:
            break;
        }

        reader.offset = subDataStart + subSize;
    }

    if (!sawHeader)
    {
        setError("Hierarchy header is missing");
        return false;
    }
    if (!sawPivots)
    {
        setError("Hierarchy pivots chunk is missing");
        return false;
    }
    if (hierarchy.pivots.size() != hierarchy.numPivots)
    {
        setError("Hierarchy pivot count does not match its header");
        return false;
    }

    m_result.hierarchies.push_back(std::move(hierarchy));
    return true;
}

bool W3dLoader::parseHierarchyHeader(ChunkReader reader, ParsedHierarchy& hierarchy)
{
    if (reader.remaining() < sizeof(HierarchyHeader))
    {
        setError("Invalid hierarchy header size");
        return false;
    }

    HierarchyHeader header;
    if (!reader.readBytes(&header, sizeof(header)))
    {
        setError("Truncated hierarchy header");
        return false;
    }

    std::memcpy(hierarchy.name, header.name, NAME_LEN);
    hierarchy.name[NAME_LEN - 1] = '\0';
    hierarchy.numPivots = header.numPivots;
    hierarchy.center = header.center;
    if (hierarchy.numPivots == 0)
    {
        setError("Hierarchy declares no pivots");
        return false;
    }
    return true;
}

bool W3dLoader::parsePivots(ChunkReader reader, ParsedHierarchy& hierarchy, uint32_t chunkSize)
{
    if (chunkSize % sizeof(Pivot) != 0)
    {
        setError("Hierarchy pivots chunk has a partial pivot record");
        return false;
    }

    const uint32_t count = chunkSize / sizeof(Pivot);
    if (count != hierarchy.numPivots)
    {
        setError("Hierarchy pivot count does not match its header");
        return false;
    }

    hierarchy.pivots.resize(count);
    if (count != 0 && !reader.readBytes(hierarchy.pivots.data(), count * sizeof(Pivot)))
    {
        hierarchy.pivots.clear();
        setError("Truncated hierarchy pivots chunk");
        return false;
    }
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Animation (classic, uncompressed HAnim channels)
// ══════════════════════════════════════════════════════════════════════════════

bool W3dLoader::parseAnimation(ChunkReader reader, uint32_t chunkSize)
{
    ParsedAnimation animation;
    const uint32_t endOffset = static_cast<uint32_t>(reader.offset) + chunkSize;

    while (!reader.atEnd() && reader.offset < endOffset) {
        ChunkHeader header;
        if (!reader.readChunkHeader(header)) break;

        const size_t subDataStart = reader.offset;
        switch (header.type) {
        case Chunk_AnimationHeader:
            if (!parseAnimationHeader(reader.subReader(header.size), animation)) return false;
            break;
        case Chunk_AnimationChannel:
            if (!parseAnimationChannel(reader.subReader(header.size), animation, header.size)) return false;
            break;
        case Chunk_BitChannel:
            if (!parseAnimationVisibilityChannel(reader.subReader(header.size), animation, header.size)) return false;
            break;
        default:
            break;
        }
        reader.offset = subDataStart + header.size;
    }

    m_result.animations.push_back(std::move(animation));
    return true;
}

bool W3dLoader::parseAnimationVisibilityChannel(ChunkReader reader, ParsedAnimation& animation,
                                                uint32_t chunkSize)
{
    if (chunkSize < 10) return false;
    struct BitHeader { uint16_t first, last, flags, pivot; uint8_t defaultValue, firstData; } header{};
    static_assert(sizeof(BitHeader) == 10);
    if (!reader.readBytes(&header, sizeof(header)) || header.last < header.first) return false;
    const size_t bitCount = static_cast<size_t>(header.last) - header.first + 1;
    const size_t byteCount = (bitCount + 7) / 8;
    if (byteCount == 0 || chunkSize < 9 + byteCount) return false;
    ParsedAnimationVisibilityChannel channel;
    channel.firstFrame = header.first;
    channel.lastFrame = header.last;
    channel.pivotIndex = header.pivot;
    channel.defaultVisible = header.defaultValue != 0;
    channel.bits.resize(byteCount);
    channel.bits[0] = header.firstData;
    if (byteCount > 1 && !reader.readBytes(channel.bits.data() + 1, byteCount - 1)) return false;
    animation.visibilityChannels.push_back(std::move(channel));
    return true;
}

bool W3dLoader::parseAnimationHeader(ChunkReader reader, ParsedAnimation& animation)
{
    AnimHeader header;
    if (!reader.readBytes(&header, sizeof(header))) return false;

    std::memcpy(animation.name, header.name, NAME_LEN);
    std::memcpy(animation.hierarchyName, header.hierarchyName, NAME_LEN);
    animation.numFrames = header.numFrames;
    animation.frameRate = header.frameRate;
    return true;
}

bool W3dLoader::parseAnimationChannel(ChunkReader reader, ParsedAnimation& animation, uint32_t chunkSize)
{
    if (chunkSize < sizeof(AnimChannel)) {
        setError("Invalid animation channel size");
        return false;
    }

    AnimChannel header;
    if (!reader.readBytes(&header, sizeof(header))) return false;
    if (header.lastFrame < header.firstFrame || header.vectorLen == 0) {
        setError("Invalid animation channel range");
        return false;
    }

    const size_t frameCount = static_cast<size_t>(header.lastFrame) - header.firstFrame + 1;
    const size_t valueCount = frameCount * header.vectorLen;
    const size_t valueSize = valueCount * sizeof(float);
    if (valueSize > reader.remaining()) {
        setError("Truncated animation channel data");
        return false;
    }

    ParsedAnimationChannel channel;
    channel.firstFrame = header.firstFrame;
    channel.lastFrame = header.lastFrame;
    channel.vectorLength = header.vectorLen;
    channel.flags = header.flags;
    channel.pivotIndex = header.pivot;
    channel.values.resize(valueCount);
    if (!reader.readBytes(channel.values.data(), valueSize)) return false;

    animation.channels.push_back(std::move(channel));
    return true;
}

bool W3dLoader::parseCompressedAnimation(ChunkReader reader, uint32_t chunkSize)
{
    ParsedAnimation animation;
    const uint32_t endOffset = static_cast<uint32_t>(reader.offset) + chunkSize;
    uint16_t flavor = UINT16_MAX;
    bool sawHeader = false;

    while (!reader.atEnd() && reader.offset < endOffset) {
        ChunkHeader header;
        if (!reader.readChunkHeader(header) || header.size > reader.remaining()) {
            setError("Truncated compressed animation chunk");
            return false;
        }
        const size_t dataStart = reader.offset;
        switch (header.type) {
        case Chunk_CompressedAnimHeader: {
            if (sawHeader || header.size < sizeof(CompressedAnimHeader)) {
                setError("Invalid compressed animation header");
                return false;
            }
            CompressedAnimHeader compressedHeader{};
            if (!reader.readBytes(&compressedHeader, sizeof(compressedHeader))) return false;
            std::memcpy(animation.name, compressedHeader.name, NAME_LEN);
            std::memcpy(animation.hierarchyName, compressedHeader.hierarchyName, NAME_LEN);
            animation.numFrames = compressedHeader.numFrames;
            animation.frameRate = compressedHeader.frameRate;
            animation.compressed = true;
            flavor = compressedHeader.flavor;
            sawHeader = true;
            break;
        }
        case Chunk_CompressedAnimChannel:
            if (!sawHeader) {
                setError("Compressed animation channel appears before its header");
                return false;
            }
            if (flavor == COMPRESSED_ANIM_FLAVOR_TIMECODED &&
                !parseTimeCodedAnimationChannel(reader.subReader(header.size), animation, header.size)) {
                return false;
            }
            if (flavor == COMPRESSED_ANIM_FLAVOR_ADAPTIVE_DELTA &&
                !parseAdaptiveDeltaAnimationChannel(reader.subReader(header.size), animation, header.size)) {
                return false;
            }
            break;
        case Chunk_CompressedBitChannel:
            if (!sawHeader) {
                setError("Compressed bit channel appears before its header");
                return false;
            }
            if (!parseTimeCodedVisibilityChannel(
                    reader.subReader(header.size), animation, header.size)) {
                return false;
            }
            break;
        default:
            break;
        }
        reader.offset = dataStart + header.size;
    }

    if (!sawHeader) {
        setError("Compressed animation header is missing");
        return false;
    }
    if (flavor == COMPRESSED_ANIM_FLAVOR_TIMECODED || flavor == COMPRESSED_ANIM_FLAVOR_ADAPTIVE_DELTA) {
        m_result.animations.push_back(std::move(animation));
    }
    return true;
}

bool W3dLoader::parseTimeCodedVisibilityChannel(
    ChunkReader reader, ParsedAnimation& animation, uint32_t chunkSize)
{
    if (animation.numFrames == 0 || animation.numFrames > UINT16_MAX + 1ull ||
        chunkSize < sizeof(TimeCodedBitChannel)) {
        setError("Invalid compressed bit channel");
        return false;
    }

    TimeCodedBitChannel header{};
    if (!reader.readBytes(&header, sizeof(header)) ||
        header.numTimeCodes == 0 || header.flags != 0) {
        setError("Unsupported compressed bit channel header");
        return false;
    }
    const uint64_t requiredBytes = sizeof(TimeCodedBitChannel) +
        static_cast<uint64_t>(header.numTimeCodes - 1u) * sizeof(uint32_t);
    if (requiredBytes > chunkSize || requiredBytes > SIZE_MAX) {
        setError("Compressed bit channel data is truncated");
        return false;
    }

    container::Vector<uint32_t> timeCodes(header.numTimeCodes);
    timeCodes[0] = header.firstData;
    if (timeCodes.size() > 1 &&
        !reader.readBytes(timeCodes.data() + 1,
                          (timeCodes.size() - 1u) * sizeof(uint32_t))) {
        setError("Compressed bit channel data is truncated");
        return false;
    }
    uint32_t previousFrame = 0;
    for (size_t index = 0; index < timeCodes.size(); ++index) {
        const uint32_t frame = timeCodes[index] & ~TIMECODED_BIT_MASK;
        if (frame >= animation.numFrames ||
            (index != 0 && frame < previousFrame)) {
            setError("Compressed bit channel time codes are invalid");
            return false;
        }
        previousFrame = frame;
    }

    ParsedAnimationVisibilityChannel channel;
    channel.firstFrame = 0;
    channel.lastFrame = static_cast<uint16_t>(animation.numFrames - 1u);
    channel.pivotIndex = header.pivot;
    channel.defaultVisible = header.defaultValue != 0;
    channel.bits.assign((animation.numFrames + 7u) / 8u, 0);
    size_t timeCodeIndex = 0;
    for (uint32_t frame = 0; frame < animation.numFrames; ++frame) {
        while (timeCodeIndex + 1u < timeCodes.size() &&
               (timeCodes[timeCodeIndex + 1u] & ~TIMECODED_BIT_MASK) <= frame) {
            ++timeCodeIndex;
        }
        if ((timeCodes[timeCodeIndex] & TIMECODED_BIT_MASK) != 0) {
            channel.bits[frame / 8u] |= static_cast<uint8_t>(1u << (frame % 8u));
        }
    }
    animation.visibilityChannels.push_back(std::move(channel));
    return true;
}

bool W3dLoader::parseAdaptiveDeltaAnimationChannel(ChunkReader reader, ParsedAnimation& animation,
                                                    uint32_t chunkSize)
{
    if (chunkSize < sizeof(AdaptiveDeltaAnimChannel)) return false;
    AdaptiveDeltaAnimChannel header{};
    if (!reader.readBytes(&header, sizeof(header)) || header.numFrames == 0 || header.vectorLen == 0 ||
        !classicChannelFlag(header.flags)) return false;
    const uint64_t packetBlocks = (static_cast<uint64_t>(header.numFrames) + 14) / 16;
    const uint64_t dataBytes = static_cast<uint64_t>(header.vectorLen) * sizeof(float) +
        packetBlocks * header.vectorLen * 9;
    if (dataBytes < sizeof(uint32_t) || dataBytes + 12 > chunkSize || dataBytes > SIZE_MAX) return false;
    container::Vector<uint8_t> data(static_cast<size_t>(dataBytes));
    std::memcpy(data.data(), &header.firstData, sizeof(header.firstData));
    if (data.size() > sizeof(header.firstData) && !reader.readBytes(data.data() + sizeof(header.firstData),
        data.size() - sizeof(header.firstData))) return false;

    ParsedAnimationChannel channel;
    channel.firstFrame = 0;
    channel.lastFrame = static_cast<uint16_t>(std::min<uint32_t>(header.numFrames - 1, UINT16_MAX));
    channel.vectorLength = header.vectorLen;
    channel.flags = *classicChannelFlag(header.flags);
    channel.pivotIndex = header.pivot;
    channel.values.resize(static_cast<size_t>(header.numFrames) * header.vectorLen);
    for (uint32_t component = 0; component < header.vectorLen; ++component) {
        float value = std::bit_cast<float>(*reinterpret_cast<const uint32_t*>(data.data() + component * 4));
        channel.values[component] = value;
        for (uint32_t frame = 1; frame < header.numFrames; ++frame) {
            const uint32_t block = (frame - 1) / 16;
            const uint32_t nibble = (frame - 1) % 16;
            const size_t packetOffset = static_cast<size_t>(header.vectorLen) * 4 +
                (static_cast<size_t>(block) * header.vectorLen + component) * 9;
            const float filter = adaptiveFilter(data[packetOffset]) * header.scale;
            const uint8_t packed = data[packetOffset + 1 + nibble / 2];
            int factor = (nibble & 1) ? packed >> 4 : packed & 0x0F;
            if ((factor & 0x8) != 0) factor |= ~0x0F;
            value += static_cast<float>(factor) * filter;
            channel.values[static_cast<size_t>(frame) * header.vectorLen + component] = value;
        }
    }
    animation.channels.push_back(std::move(channel));
    return true;
}

bool W3dLoader::parseTimeCodedAnimationChannel(ChunkReader reader, ParsedAnimation& animation,
                                                uint32_t chunkSize)
{
    // `numFrames` is a raw uint32_t from CompressedAnimHeader and it sizes both
    // the output vector and the loop below, where each iteration allocates two
    // Vector<float> in readVector.  0xFFFFFFFF with vectorLen 4 requests ~68 GB
    // and then runs four billion iterations.  The sibling
    // parseTimeCodedVisibilityChannel already bounds the same field this way.
    if (animation.numFrames == 0 || animation.numFrames > UINT16_MAX + 1ull ||
        chunkSize < sizeof(TimeCodedAnimChannel)) {
        setError("Invalid time-coded animation channel");
        return false;
    }
    TimeCodedAnimChannel header{};
    if (!reader.readBytes(&header, sizeof(header)) || header.numTimeCodes == 0 ||
        header.vectorLen == 0 || !classicChannelFlag(header.flags)) {
        setError("Invalid time-coded animation channel header");
        return false;
    }

    const uint64_t wordCount = static_cast<uint64_t>(header.numTimeCodes) *
                               (static_cast<uint64_t>(header.vectorLen) + 1);
    const uint64_t dataBytes = wordCount * sizeof(uint32_t);
    const uint64_t requiredBytes = sizeof(uint32_t) * 2 + dataBytes;
    if (wordCount == 0 || dataBytes > SIZE_MAX || requiredBytes > chunkSize || dataBytes < sizeof(uint32_t)) {
        setError("Time-coded animation channel data is truncated");
        return false;
    }

    container::Vector<uint32_t> data(static_cast<size_t>(wordCount));
    data[0] = header.firstData;
    if (data.size() > 1 && !reader.readBytes(data.data() + 1,
                                               (data.size() - 1) * sizeof(uint32_t))) {
        return false;
    }

    const uint32_t packetSize = static_cast<uint32_t>(header.vectorLen) + 1;
    const auto readVector = [&data, packetSize](uint32_t packet) {
        container::Vector<float> result(packetSize - 1);
        const size_t offset = static_cast<size_t>(packet) * packetSize + 1;
        for (size_t index = 0; index < result.size(); ++index) {
            result[index] = std::bit_cast<float>(data[offset + index]);
        }
        return result;
    };

    ParsedAnimationChannel channel;
    channel.firstFrame = 0;
    channel.lastFrame = static_cast<uint16_t>(std::min<uint32_t>(
        animation.numFrames - 1, UINT16_MAX));
    channel.vectorLength = header.vectorLen;
    channel.flags = *classicChannelFlag(header.flags);
    channel.pivotIndex = header.pivot;
    channel.values.resize(static_cast<size_t>(animation.numFrames) * header.vectorLen);

    for (uint32_t frame = 0; frame < animation.numFrames; ++frame) {
        uint32_t currentPacket = 0;
        while (currentPacket + 1 < header.numTimeCodes &&
               frame >= (data[static_cast<size_t>(currentPacket + 1) * packetSize] &
                         ~TIMECODED_BINARY_MOVEMENT_FLAG)) {
            ++currentPacket;
        }
        const container::Vector<float> current = readVector(currentPacket);
        container::Vector<float> result = current;
        if (currentPacket + 1 < header.numTimeCodes) {
            const uint32_t currentTime = data[static_cast<size_t>(currentPacket) * packetSize] &
                                         ~TIMECODED_BINARY_MOVEMENT_FLAG;
            const uint32_t nextCode = data[static_cast<size_t>(currentPacket + 1) * packetSize];
            const uint32_t nextTime = nextCode & ~TIMECODED_BINARY_MOVEMENT_FLAG;
            if ((nextCode & TIMECODED_BINARY_MOVEMENT_FLAG) == 0 && nextTime > currentTime) {
                const container::Vector<float> next = readVector(currentPacket + 1);
                const float ratio = std::clamp(
                    (static_cast<float>(frame) - currentTime) /
                    static_cast<float>(nextTime - currentTime), 0.0f, 1.0f);
                if (channel.flags == AnimChannel_Q && header.vectorLen == 4) {
                    slerpQuaternion(current.data(), next.data(), ratio, result.data());
                } else {
                    for (size_t index = 0; index < result.size(); ++index) {
                        result[index] = current[index] + (next[index] - current[index]) * ratio;
                    }
                }
            }
        }
        std::copy(result.begin(), result.end(),
                  channel.values.begin() + static_cast<size_t>(frame) * header.vectorLen);
    }

    animation.channels.push_back(std::move(channel));
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// HLod
// ══════════════════════════════════════════════════════════════════════════════

bool W3dLoader::parseHLod(ChunkReader reader, uint32_t chunkSize)
{
    ParsedHLod hlod;
    if (chunkSize > reader.size)
    {
        setError("Truncated HLOD chunk");
        return false;
    }

    const size_t endOffset = reader.offset + chunkSize;
    bool sawHeader = false;
    bool sawAggregateArray = false;
    bool sawProxyArray = false;
    uint32_t nextLodIndex = 0;

    while (!reader.atEnd() && reader.offset < endOffset)
    {
        ChunkHeader header;
        if (!reader.readChunkHeader(header))
        {
            setError("Truncated HLOD subchunk header");
            return false;
        }

        const size_t subDataStart = reader.offset;
        const uint32_t subSize = header.size;
        if (subSize > reader.remaining())
        {
            setError("HLOD subchunk exceeds its parent chunk");
            return false;
        }

        switch (header.type)
        {
        case Chunk_HLodHeader:
        {
            if (sawHeader || nextLodIndex != 0)
            {
                setError("Duplicate or out-of-order HLOD header");
                return false;
            }
            if (subSize < sizeof(HLodHeader))
            {
                setError("Invalid HLOD header size");
                return false;
            }

            HLodHeader hlodHeader;
            if (!reader.readBytes(&hlodHeader, sizeof(hlodHeader)))
            {
                setError("Truncated HLOD header");
                return false;
            }

            if (hlodHeader.lodCount == 0)
            {
                setError("HLOD contains no LOD arrays");
                return false;
            }

            constexpr size_t MinLodArraySize = sizeof(ChunkHeader) * 2 + sizeof(HLodArrayHeader);
            const size_t bytesAfterHeader = reader.size - (subDataStart + subSize);
            if (hlodHeader.lodCount > bytesAfterHeader / MinLodArraySize)
            {
                setError("HLOD LOD count cannot fit in the chunk");
                return false;
            }

            std::memcpy(hlod.name, hlodHeader.name, NAME_LEN);
            std::memcpy(hlod.hierarchyName, hlodHeader.hierarchyName, NAME_LEN);
            hlod.name[NAME_LEN - 1] = '\0';
            hlod.hierarchyName[NAME_LEN - 1] = '\0';
            hlod.lodCount = hlodHeader.lodCount;
            hlod.lods.reserve(hlod.lodCount);
            sawHeader = true;
            break;
        }
        case Chunk_HLodLodArray:
        {
            if (!sawHeader)
            {
                setError("HLOD LOD array appears before its header");
                return false;
            }
            if (nextLodIndex >= hlod.lodCount)
            {
                setError("HLOD contains more LOD arrays than declared");
                return false;
            }

            ParsedHLodLod lod;
            if (!parseHLodLodArray(reader.subReader(subSize), subSize, nextLodIndex, lod))
                return false;

            hlod.lods.push_back(std::move(lod));
            ++nextLodIndex;
            break;
        }
        case Chunk_HLodAggregateArray:
        case Chunk_HLodProxyArray:
        {
            if (!sawHeader || nextLodIndex != hlod.lodCount)
            {
                setError("HLOD attachment array appears before all LOD arrays");
                return false;
            }
            const bool aggregate = header.type == Chunk_HLodAggregateArray;
            bool& seen = aggregate ? sawAggregateArray : sawProxyArray;
            if (seen)
            {
                setError(aggregate
                    ? "Duplicate HLOD aggregate array"
                    : "Duplicate HLOD proxy array");
                return false;
            }
            ParsedHLodLod attachments;
            if (!parseHLodLodArray(
                    reader.subReader(subSize), subSize, 0, attachments))
                return false;
            if (aggregate)
                hlod.aggregates = std::move(attachments.subObjects);
            else
                hlod.proxies = std::move(attachments.subObjects);
            seen = true;
            break;
        }
        default:
            break;
        }

        reader.offset = subDataStart + subSize;
    }

    if (!sawHeader)
    {
        setError("HLOD header is missing");
        return false;
    }
    if (nextLodIndex != hlod.lodCount)
    {
        setError("HLOD LOD array count does not match its header");
        return false;
    }

    m_result.hlods.push_back(std::move(hlod));
    return true;
}

bool W3dLoader::parseHLodLodArray(ChunkReader reader, uint32_t chunkSize,
                                  uint32_t lodIndex, ParsedHLodLod& lod)
{
    if (chunkSize > reader.size)
    {
        setError("Truncated HLOD LOD array");
        return false;
    }

    ChunkHeader header;
    if (!reader.readChunkHeader(header))
    {
        setError("HLOD LOD array header is missing");
        return false;
    }
    if (header.type != Chunk_HLodSubObjectArrayHeader)
    {
        setError("HLOD LOD array does not begin with an array header");
        return false;
    }
    if (header.size < sizeof(HLodArrayHeader) || header.size > reader.remaining())
    {
        setError("Invalid HLOD LOD array header size");
        return false;
    }

    const size_t headerDataStart = reader.offset;
    HLodArrayHeader arrayHeader;
    if (!reader.readBytes(&arrayHeader, sizeof(arrayHeader)))
    {
        setError("Truncated HLOD LOD array header");
        return false;
    }
    reader.offset = headerDataStart + header.size;

    constexpr size_t MinSubObjectSize = sizeof(ChunkHeader) + sizeof(HLodSubObject);
    if (arrayHeader.modelCount > reader.remaining() / MinSubObjectSize)
    {
        setError("HLOD model count cannot fit in its LOD array");
        return false;
    }

    lod.index = lodIndex;
    lod.maxScreenSize = arrayHeader.maxScreenSize;
    lod.subObjects.reserve(arrayHeader.modelCount);

    for (uint32_t modelIndex = 0; modelIndex < arrayHeader.modelCount; ++modelIndex)
    {
        ChunkHeader subObjectHeader;
        if (!reader.readChunkHeader(subObjectHeader))
        {
            setError("Missing HLOD subobject " + std::to_string(modelIndex));
            return false;
        }
        if (subObjectHeader.type != Chunk_HLodSubObject)
        {
            setError("Unexpected chunk in HLOD subobject array");
            return false;
        }
        if (subObjectHeader.size < sizeof(HLodSubObject) ||
            subObjectHeader.size > reader.remaining())
        {
            setError("Invalid HLOD subobject size");
            return false;
        }

        const size_t subObjectDataStart = reader.offset;
        HLodSubObject subObject{};
        if (!reader.readBytes(&subObject, sizeof(subObject)))
        {
            setError("Truncated HLOD subobject");
            return false;
        }
        subObject.name[sizeof(subObject.name) - 1] = '\0';
        lod.subObjects.push_back(subObject);
        reader.offset = subObjectDataStart + subObjectHeader.size;
    }

    // Future writers may append extension chunks.  Preserve forward
    // compatibility, but still reject duplicate structural records.
    while (!reader.atEnd())
    {
        ChunkHeader extensionHeader;
        if (!reader.readChunkHeader(extensionHeader) || extensionHeader.size > reader.remaining())
        {
            setError("Malformed trailing data in HLOD LOD array");
            return false;
        }
        if (extensionHeader.type == Chunk_HLodSubObjectArrayHeader ||
            extensionHeader.type == Chunk_HLodSubObject)
        {
            setError("HLOD LOD array contains more records than declared");
            return false;
        }
        if (!reader.skipBytes(extensionHeader.size))
        {
            setError("Malformed HLOD LOD array extension");
            return false;
        }
    }

    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Helpers
// ══════════════════════════════════════════════════════════════════════════════

void W3dLoader::setError(const container::String& msg)
{
    m_error = msg;
}

} // namespace data::w3d
