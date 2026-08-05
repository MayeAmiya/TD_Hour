#pragma once

#include "container/container_types.h"

#include <cstdint>
namespace data::tga {

#pragma pack(push, 1)
struct TGAHeader
{
    uint8_t  idLength;
    uint8_t  colorMapType;
    uint8_t  imageType;
    uint16_t colorMapOrigin;
    uint16_t colorMapLength;
    uint8_t  colorMapDepth;
    uint16_t xOrigin;
    uint16_t yOrigin;
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint8_t  descriptor;
};
#pragma pack(pop)

enum class ImageType : uint8_t
{
    None              = 0,
    ColorMapped       = 1,
    TrueColor         = 2,
    Grayscale         = 3,
    RLE_ColorMapped   = 9,
    RLE_TrueColor     = 10,
    RLE_Grayscale     = 11,
};

enum class TGAFormat : uint32_t
{
    Unknown = 0,
    R8G8B8,
    A8R8G8B8,
    L8,
};

struct TGAData
{
    uint32_t  width = 0;
    uint32_t  height = 0;
    TGAFormat format = TGAFormat::Unknown;
    bool      hasAlpha = false;
    bool      topDown = false;
    container::Vector<uint8_t> pixels;
};

} // namespace data::tga
