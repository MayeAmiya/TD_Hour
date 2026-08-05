#include "container/container_types.h"
#include "TgaLoader.h"

#include "VFS.h"
#include <cstring>

namespace data::tga {

void TgaLoader::reset()
{
    m_result = TGAData{};
    m_header = TGAHeader{};
    m_error.clear();
}

bool TgaLoader::loadFromFile(const container::String& path)
{
    container::Vector<uint8_t> buffer;
    if (!io::VFS::instance().readToBuffer(path, buffer))
    {
        setError("Failed to read file: " + path);
        return false;
    }
    return loadFromMemory(buffer.data(), buffer.size());
}

bool TgaLoader::loadFromMemory(const uint8_t* data, size_t size)
{
    reset();

    if (!data || size < sizeof(TGAHeader))
    {
        setError("Invalid TGA data: too small");
        return false;
    }

    std::memcpy(&m_header, data, sizeof(TGAHeader));

    if (m_header.width == 0 || m_header.height == 0)
    {
        setError("Invalid TGA dimensions");
        return false;
    }

    // Cap the decoded surface.  Both dimension fields are uint16_t and were
    // accepted unconditionally, so a hostile 65535x65535 32-bpp header passed
    // every check and reached a ~17.2 GB resize() — an unhandled bad_alloc /
    // length_error, i.e. memory-exhaustion DoS on untrusted asset input.  No real
    // game texture comes close to this bound.
    constexpr uint64_t kMaxTgaPixels = 64ull * 1024ull * 1024ull;
    const uint64_t declaredPixels =
        static_cast<uint64_t>(m_header.width) * m_header.height;
    if (declaredPixels > kMaxTgaPixels)
    {
        setError("TGA dimensions exceed the supported surface size");
        return false;
    }

    m_result.width  = m_header.width;
    m_result.height = m_header.height;
    m_result.topDown = (m_header.descriptor & 0x20) != 0;

    // Determine output format
    switch (m_header.bpp)
    {
    case 8:
        m_result.format = TGAFormat::L8;
        break;
    case 16:
        m_result.format = TGAFormat::A8R8G8B8;
        m_result.hasAlpha = true;
        break;
    case 24:
        m_result.format = TGAFormat::R8G8B8;
        break;
    case 32:
        m_result.format = TGAFormat::A8R8G8B8;
        m_result.hasAlpha = true;
        break;
    default:
        setError("Unsupported TGA bit depth: " + std::to_string(m_header.bpp));
        return false;
    }

    // Locate pixel data
    size_t pixelDataOffset = sizeof(TGAHeader) + m_header.idLength;
    if (m_header.colorMapType == 1)
    {
        pixelDataOffset += static_cast<size_t>(m_header.colorMapLength) * (m_header.colorMapDepth / 8);
    }

    if (pixelDataOffset >= size)
    {
        setError("TGA pixel data offset beyond file size");
        return false;
    }

    const uint8_t* pixelData = data + pixelDataOffset;
    size_t pixelDataSize = size - pixelDataOffset;

    switch (static_cast<ImageType>(m_header.imageType))
    {
    case ImageType::TrueColor:
    case ImageType::Grayscale:
        return parseUncompressed(pixelData, pixelDataSize);

    case ImageType::RLE_TrueColor:
    case ImageType::RLE_Grayscale:
        return parseRLE(pixelData, pixelDataSize);

    case ImageType::ColorMapped:
    case ImageType::RLE_ColorMapped:
        setError("Color-mapped TGA not supported");
        return false;

    default:
        setError("Unsupported TGA image type: " + std::to_string(m_header.imageType));
        return false;
    }
}

// ── helpers ──────────────────────────────────────────────────────────────────

static uint8_t expand5to8(uint8_t v)
{
    return static_cast<uint8_t>((v << 3) | (v >> 2));
}

static uint32_t outputBPP(TGAFormat fmt)
{
    switch (fmt)
    {
    case TGAFormat::A8R8G8B8: return 4;
    case TGAFormat::R8G8B8:   return 3;
    case TGAFormat::L8:       return 1;
    default:                  return 0;
    }
}

// Convert one TGA pixel (src, 1/2/3/4 bytes BGR/BGRA) to output pixel (dst)
static void convertPixel(const uint8_t* src, uint8_t* dst, uint32_t srcBPP, TGAFormat dstFmt)
{
    switch (dstFmt)
    {
    case TGAFormat::L8:
        dst[0] = src[0];
        break;
    case TGAFormat::R8G8B8:
        dst[0] = src[2]; // B → R
        dst[1] = src[1]; // G → G
        dst[2] = src[0]; // R → B
        break;
    case TGAFormat::A8R8G8B8:
        if (srcBPP == 2)
        {
            uint16_t p = static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
            dst[0] = expand5to8(static_cast<uint8_t>(p & 0x1F));
            dst[1] = expand5to8(static_cast<uint8_t>((p >> 5) & 0x1F));
            dst[2] = expand5to8(static_cast<uint8_t>((p >> 10) & 0x1F));
            dst[3] = (p & 0x8000) ? 255 : 0;
        }
        else
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = (srcBPP == 4) ? src[3] : 255;
        }
        break;
    default:
        break;
    }
}

// ── uncompressed ────────────────────────────────────────────────────────────

bool TgaLoader::parseUncompressed(const uint8_t* data, size_t dataSize)
{
    uint32_t srcBPP = m_header.bpp / 8;
    uint32_t dstBPP = outputBPP(m_result.format);
    uint32_t pixelsPerRow = m_result.width;

    size_t expectedSize = static_cast<size_t>(m_result.width) * m_result.height * srcBPP;
    if (dataSize < expectedSize)
    {
        setError("TGA uncompressed data too small");
        return false;
    }

    m_result.pixels.resize(static_cast<size_t>(m_result.width) * m_result.height * dstBPP);

    for (uint32_t y = 0; y < m_result.height; ++y)
    {
        // TGA stores rows bottom-to-top by default (topDown=false)
        uint32_t srcY = m_result.topDown ? y : (m_result.height - 1 - y);
        const uint8_t* srcRow = data + static_cast<size_t>(srcY) * pixelsPerRow * srcBPP;
        uint8_t* dstRow = m_result.pixels.data() + static_cast<size_t>(y) * pixelsPerRow * dstBPP;

        for (uint32_t x = 0; x < m_result.width; ++x)
        {
            convertPixel(srcRow, dstRow, srcBPP, m_result.format);
            srcRow += srcBPP;
            dstRow += dstBPP;
        }
    }

    return true;
}

// ── RLE ─────────────────────────────────────────────────────────────────────

bool TgaLoader::parseRLE(const uint8_t* data, size_t dataSize)
{
    uint32_t srcBPP = m_header.bpp / 8;
    uint32_t dstBPP = outputBPP(m_result.format);
    uint32_t pixelCount = m_result.width * m_result.height;

    // Decode into row-major buffer (TGA storage order)
    container::Vector<uint8_t> raw;
    raw.resize(static_cast<size_t>(pixelCount) * srcBPP);

    const uint8_t* src = data;
    const uint8_t* end = data + dataSize;
    uint8_t* rawPtr = raw.data();
    uint32_t decoded = 0;

    while (decoded < pixelCount && src < end)
    {
        if (src + 1 > end) break;
        uint8_t packetHeader = *src++;
        uint8_t count = (packetHeader & 0x7F) + 1;

        if (packetHeader & 0x80)
        {
            // RLE: replicate next pixel
            if (src + srcBPP > end) break;
            for (uint8_t i = 0; i < count && decoded < pixelCount; ++i, ++decoded)
            {
                std::memcpy(rawPtr, src, srcBPP);
                rawPtr += srcBPP;
            }
            src += srcBPP;
        }
        else
        {
            // Raw: copy count pixels
            for (uint8_t i = 0; i < count && decoded < pixelCount; ++i, ++decoded)
            {
                if (src + srcBPP > end) break;
                std::memcpy(rawPtr, src, srcBPP);
                src += srcBPP;
                rawPtr += srcBPP;
            }
        }
    }

    if (decoded != pixelCount)
    {
        setError("TGA RLE decode: unexpected end of data");
        return false;
    }

    // Convert raw (in TGA storage order) to output (top-down, RGB)
    m_result.pixels.resize(static_cast<size_t>(pixelCount) * dstBPP);

    for (uint32_t y = 0; y < m_result.height; ++y)
    {
        uint32_t rawY = m_result.topDown ? y : (m_result.height - 1 - y);
        const uint8_t* srcRow = raw.data() + static_cast<size_t>(rawY) * m_result.width * srcBPP;
        uint8_t* dstRow = m_result.pixels.data() + static_cast<size_t>(y) * m_result.width * dstBPP;

        for (uint32_t x = 0; x < m_result.width; ++x)
        {
            convertPixel(srcRow, dstRow, srcBPP, m_result.format);
            srcRow += srcBPP;
            dstRow += dstBPP;
        }
    }

    return true;
}

void TgaLoader::setError(const container::String& msg)
{
    m_error = msg;
}

} // namespace data::tga
