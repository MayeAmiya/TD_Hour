#pragma once

#include "container/container_types.h"

#include <cstdint>
namespace data::dds {

constexpr uint32_t DDS_MAGIC = 0x20534444;

constexpr uint32_t DDSD_CAPS        = 0x00000001;
constexpr uint32_t DDSD_HEIGHT      = 0x00000002;
constexpr uint32_t DDSD_WIDTH       = 0x00000004;
constexpr uint32_t DDSD_PITCH       = 0x00000008;
constexpr uint32_t DDSD_PIXELFORMAT = 0x00001000;
constexpr uint32_t DDSD_MIPMAPCOUNT = 0x00020000;
constexpr uint32_t DDSD_LINEARSIZE  = 0x00080000;
constexpr uint32_t DDSD_DEPTH       = 0x00800000;

constexpr uint32_t DDPF_ALPHAPIXELS = 0x00000001;
constexpr uint32_t DDPF_ALPHA       = 0x00000002;
constexpr uint32_t DDPF_FOURCC      = 0x00000004;
constexpr uint32_t DDPF_RGB         = 0x00000040;
constexpr uint32_t DDPF_LUMINANCE   = 0x00020000;

constexpr uint32_t FOURCC_DXT1 = 0x31545844;
constexpr uint32_t FOURCC_DXT2 = 0x32545844;
constexpr uint32_t FOURCC_DXT3 = 0x33545844;
constexpr uint32_t FOURCC_DXT4 = 0x34545844;
constexpr uint32_t FOURCC_DXT5 = 0x35545844;

constexpr uint32_t DDSCAPS_COMPLEX = 0x00000008;
constexpr uint32_t DDSCAPS_TEXTURE = 0x00001000;
constexpr uint32_t DDSCAPS_MIPMAP  = 0x00400000;

enum class DDSFormat : uint32_t
{
    Unknown = 0,
    R8G8B8,
    A8R8G8B8,
    X8R8G8B8,
    R5G6B5,
    X1R5G5B5,
    A1R5G5B5,
    A4R4G4B4,
    DXT1,
    DXT2,
    DXT3,
    DXT4,
    DXT5,
    L8,
    A8L8,
};

struct DDSData
{
    uint32_t    width = 0;
    uint32_t    height = 0;
    uint32_t    depth = 1;
    uint32_t    mipLevels = 1;
    DDSFormat   format = DDSFormat::Unknown;
    uint32_t    pitch = 0;
    uint32_t    linearSize = 0;
    bool        hasAlpha = false;
    container::Vector<uint8_t> pixels;
};

} // namespace data::dds
