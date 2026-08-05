#include "container/container_types.h"
#include "DdsLoader.h"

#include "VFS.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace data::dds {

void DdsLoader::reset()
{
    m_result = DDSData{};
    m_error.clear();
}

bool DdsLoader::loadFromFile(const container::String& path)
{
    container::Vector<uint8_t> buffer;
    if (!io::VFS::instance().readToBuffer(path, buffer))
    {
        setError("Failed to read file: " + path);
        return false;
    }
    return loadFromMemory(buffer.data(), buffer.size());
}

bool DdsLoader::loadFromMemory(const uint8_t* data, size_t size)
{
    reset();

    if (!data || size < 128)
    {
        setError("Invalid DDS data: too small");
        return false;
    }

    uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    if (magic != DDS_MAGIC)
    {
        setError("Invalid DDS magic number");
        return false;
    }

    return parseHeader(data + 4, size - 4);
}

namespace {

inline void color565ToRgb(uint16_t color, uint8_t& red, uint8_t& green,
                          uint8_t& blue) noexcept {
    red = static_cast<uint8_t>(((color >> 11u) & 0x1fu) << 3u);
    green = static_cast<uint8_t>(((color >> 5u) & 0x3fu) << 2u);
    blue = static_cast<uint8_t>((color & 0x1fu) << 3u);
}

void decodeDxt1(const uint8_t* source, uint8_t* destination,
               uint32_t width, uint32_t height) {
    const uint32_t blocksWide = (width + 3u) / 4u;
    const uint32_t blocksHigh = (height + 3u) / 4u;
    for (uint32_t blockY = 0; blockY < blocksHigh; ++blockY) {
        for (uint32_t blockX = 0; blockX < blocksWide; ++blockX) {
            uint16_t color0 = 0;
            uint16_t color1 = 0;
            uint32_t indices = 0;
            std::memcpy(&color0, source, sizeof(color0));
            std::memcpy(&color1, source + 2u, sizeof(color1));
            std::memcpy(&indices, source + 4u, sizeof(indices));
            source += 8u;
            uint8_t colors[4][3]{};
            color565ToRgb(color0, colors[0][0], colors[0][1], colors[0][2]);
            color565ToRgb(color1, colors[1][0], colors[1][1], colors[1][2]);
            if (color0 > color1) {
                for (int channel = 0; channel < 3; ++channel) {
                    colors[2][channel] = static_cast<uint8_t>(
                        (2u * colors[0][channel] + colors[1][channel] + 1u) / 3u);
                    colors[3][channel] = static_cast<uint8_t>(
                        (colors[0][channel] + 2u * colors[1][channel] + 1u) / 3u);
                }
            } else {
                for (int channel = 0; channel < 3; ++channel)
                    colors[2][channel] = static_cast<uint8_t>(
                        (colors[0][channel] + colors[1][channel]) / 2u);
            }
            for (uint32_t y = 0; y < 4u; ++y) {
                for (uint32_t x = 0; x < 4u; ++x) {
                    const uint32_t pixelX = blockX * 4u + x;
                    const uint32_t pixelY = blockY * 4u + y;
                    if (pixelX >= width || pixelY >= height) continue;
                    const uint32_t index = (indices >> (2u * (4u * y + x))) & 3u;
                    uint8_t* pixel = destination +
                        (static_cast<size_t>(pixelY) * width + pixelX) * 4u;
                    pixel[0] = colors[index][0];
                    pixel[1] = colors[index][1];
                    pixel[2] = colors[index][2];
                    pixel[3] = color0 <= color1 && index == 3u ? 0u : 255u;
                }
            }
        }
    }
}

void decodeDxt3(const uint8_t* source, uint8_t* destination,
               uint32_t width, uint32_t height) {
    const uint32_t blocksWide = (width + 3u) / 4u;
    const uint32_t blocksHigh = (height + 3u) / 4u;
    for (uint32_t blockY = 0; blockY < blocksHigh; ++blockY) {
        for (uint32_t blockX = 0; blockX < blocksWide; ++blockX) {
            uint64_t alphaBits = 0;
            uint16_t color0 = 0;
            uint16_t color1 = 0;
            uint32_t indices = 0;
            std::memcpy(&alphaBits, source, sizeof(alphaBits));
            std::memcpy(&color0, source + 8u, sizeof(color0));
            std::memcpy(&color1, source + 10u, sizeof(color1));
            std::memcpy(&indices, source + 12u, sizeof(indices));
            source += 16u;
            uint8_t colors[4][3]{};
            color565ToRgb(color0, colors[0][0], colors[0][1], colors[0][2]);
            color565ToRgb(color1, colors[1][0], colors[1][1], colors[1][2]);
            for (int channel = 0; channel < 3; ++channel) {
                colors[2][channel] = static_cast<uint8_t>(
                    (2u * colors[0][channel] + colors[1][channel] + 1u) / 3u);
                colors[3][channel] = static_cast<uint8_t>(
                    (colors[0][channel] + 2u * colors[1][channel] + 1u) / 3u);
            }
            for (uint32_t y = 0; y < 4u; ++y) {
                for (uint32_t x = 0; x < 4u; ++x) {
                    const uint32_t pixelX = blockX * 4u + x;
                    const uint32_t pixelY = blockY * 4u + y;
                    if (pixelX >= width || pixelY >= height) continue;
                    const uint32_t pixelIndex = 4u * y + x;
                    const uint32_t index = (indices >> (2u * pixelIndex)) & 3u;
                    const uint8_t alpha = static_cast<uint8_t>(
                        ((alphaBits >> (4u * pixelIndex)) & 0xfu) * 17u);
                    uint8_t* pixel = destination +
                        (static_cast<size_t>(pixelY) * width + pixelX) * 4u;
                    pixel[0] = colors[index][0];
                    pixel[1] = colors[index][1];
                    pixel[2] = colors[index][2];
                    pixel[3] = alpha;
                }
            }
        }
    }
}

void decodeDxt5(const uint8_t* source, uint8_t* destination,
               uint32_t width, uint32_t height) {
    const uint32_t blocksWide = (width + 3u) / 4u;
    const uint32_t blocksHigh = (height + 3u) / 4u;
    for (uint32_t blockY = 0; blockY < blocksHigh; ++blockY) {
        for (uint32_t blockX = 0; blockX < blocksWide; ++blockX) {
            const uint8_t alpha0 = source[0];
            const uint8_t alpha1 = source[1];
            uint64_t alphaBits = 0;
            uint16_t color0 = 0;
            uint16_t color1 = 0;
            uint32_t indices = 0;
            std::memcpy(&alphaBits, source + 2u, 6u);
            std::memcpy(&color0, source + 8u, sizeof(color0));
            std::memcpy(&color1, source + 10u, sizeof(color1));
            std::memcpy(&indices, source + 12u, sizeof(indices));
            source += 16u;
            uint8_t alphaTable[8]{alpha0, alpha1};
            if (alpha0 > alpha1) {
                for (uint8_t i = 0; i < 6u; ++i)
                    alphaTable[i + 2u] = static_cast<uint8_t>(
                        ((6u - i) * alpha0 + (i + 1u) * alpha1 + 3u) / 7u);
            } else {
                for (uint8_t i = 0; i < 4u; ++i)
                    alphaTable[i + 2u] = static_cast<uint8_t>(
                        ((4u - i) * alpha0 + (i + 1u) * alpha1 + 2u) / 5u);
                alphaTable[6] = 0;
                alphaTable[7] = 255;
            }
            uint8_t colors[4][3]{};
            color565ToRgb(color0, colors[0][0], colors[0][1], colors[0][2]);
            color565ToRgb(color1, colors[1][0], colors[1][1], colors[1][2]);
            for (int channel = 0; channel < 3; ++channel) {
                colors[2][channel] = static_cast<uint8_t>(
                    (2u * colors[0][channel] + colors[1][channel] + 1u) / 3u);
                colors[3][channel] = static_cast<uint8_t>(
                    (colors[0][channel] + 2u * colors[1][channel] + 1u) / 3u);
            }
            for (uint32_t y = 0; y < 4u; ++y) {
                for (uint32_t x = 0; x < 4u; ++x) {
                    const uint32_t pixelX = blockX * 4u + x;
                    const uint32_t pixelY = blockY * 4u + y;
                    if (pixelX >= width || pixelY >= height) continue;
                    const uint32_t pixelIndex = 4u * y + x;
                    const uint32_t index = (indices >> (2u * pixelIndex)) & 3u;
                    const uint32_t alphaIndex = static_cast<uint32_t>(
                        (alphaBits >> (3u * pixelIndex)) & 7u);
                    uint8_t* pixel = destination +
                        (static_cast<size_t>(pixelY) * width + pixelX) * 4u;
                    pixel[0] = colors[index][0];
                    pixel[1] = colors[index][1];
                    pixel[2] = colors[index][2];
                    pixel[3] = alphaTable[alphaIndex];
                }
            }
        }
    }
}

} // namespace

bool DdsLoader::decodeTopLevelRgba(container::Vector<uint8_t>& rgba) const {
    rgba.clear();
    const uint64_t pixelCount = static_cast<uint64_t>(m_result.width) *
        m_result.height;
    if (m_result.width == 0 || m_result.height == 0 ||
        pixelCount > std::numeric_limits<size_t>::max() / 4u ||
        m_result.pixels.empty()) return false;
    const size_t byteCount = static_cast<size_t>(pixelCount) * 4u;
    rgba.resize(byteCount);
    const size_t sourceBytes = [&]() -> size_t {
        switch (m_result.format) {
        case DDSFormat::A8R8G8B8:
        case DDSFormat::X8R8G8B8: return static_cast<size_t>(pixelCount) * 4u;
        case DDSFormat::R8G8B8: return static_cast<size_t>(pixelCount) * 3u;
        case DDSFormat::R5G6B5: return static_cast<size_t>(pixelCount) * 2u;
        case DDSFormat::DXT1: return static_cast<size_t>((m_result.width + 3u) / 4u) *
            ((m_result.height + 3u) / 4u) * 8u;
        case DDSFormat::DXT2:
        case DDSFormat::DXT3:
        case DDSFormat::DXT4:
        case DDSFormat::DXT5: return static_cast<size_t>((m_result.width + 3u) / 4u) *
            ((m_result.height + 3u) / 4u) * 16u;
        default: return 0u;
        }
    }();
    if (sourceBytes == 0 || sourceBytes > m_result.pixels.size()) {
        rgba.clear();
        return false;
    }
    const uint8_t* source = m_result.pixels.data();
    switch (m_result.format) {
    case DDSFormat::A8R8G8B8:
    case DDSFormat::X8R8G8B8:
        for (size_t i = 0; i < static_cast<size_t>(pixelCount); ++i) {
            rgba[i * 4u + 0u] = source[i * 4u + 2u];
            rgba[i * 4u + 1u] = source[i * 4u + 1u];
            rgba[i * 4u + 2u] = source[i * 4u + 0u];
            rgba[i * 4u + 3u] = m_result.format == DDSFormat::A8R8G8B8
                ? source[i * 4u + 3u] : 255u;
        }
        return true;
    case DDSFormat::R8G8B8:
        for (size_t i = 0; i < static_cast<size_t>(pixelCount); ++i) {
            rgba[i * 4u + 0u] = source[i * 3u + 0u];
            rgba[i * 4u + 1u] = source[i * 3u + 1u];
            rgba[i * 4u + 2u] = source[i * 3u + 2u];
            rgba[i * 4u + 3u] = 255u;
        }
        return true;
    case DDSFormat::R5G6B5:
        for (size_t i = 0; i < static_cast<size_t>(pixelCount); ++i) {
            uint16_t pixel = 0;
            std::memcpy(&pixel, source + i * 2u, sizeof(pixel));
            rgba[i * 4u + 0u] = static_cast<uint8_t>(((pixel >> 11u) & 0x1fu) << 3u);
            rgba[i * 4u + 1u] = static_cast<uint8_t>(((pixel >> 5u) & 0x3fu) << 2u);
            rgba[i * 4u + 2u] = static_cast<uint8_t>((pixel & 0x1fu) << 3u);
            rgba[i * 4u + 3u] = 255u;
        }
        return true;
    case DDSFormat::DXT1: decodeDxt1(source, rgba.data(), m_result.width, m_result.height); return true;
    case DDSFormat::DXT2:
    case DDSFormat::DXT3: decodeDxt3(source, rgba.data(), m_result.width, m_result.height); return true;
    case DDSFormat::DXT4:
    case DDSFormat::DXT5: decodeDxt5(source, rgba.data(), m_result.width, m_result.height); return true;
    default: rgba.clear(); return false;
    }
}

bool DdsLoader::parseHeader(const uint8_t* hdr, size_t hdrSize)
{
    if (hdrSize < 124)
    {
        setError("DDS header too small");
        return false;
    }

    auto readU32 = [&](size_t offset) -> uint32_t
    {
        uint32_t v = 0;
        std::memcpy(&v, hdr + offset, 4);
        return v;
    };

    uint32_t dwSize = readU32(0);
    if (dwSize != 124)
    {
        setError("Invalid DDS header size");
        return false;
    }

    uint32_t dwFlags         = readU32(4);
    uint32_t dwHeight        = readU32(8);
    uint32_t dwWidth         = readU32(12);
    uint32_t dwPitchOrLinear = readU32(16);
    uint32_t dwDepth         = readU32(20);
    uint32_t dwMipMapCount   = readU32(24);
    uint32_t pfFlags         = readU32(76);
    uint32_t pfFourCC        = readU32(80);
    uint32_t pfRGBBits       = readU32(84);
    uint32_t caps            = readU32(104);
    uint32_t caps2           = readU32(108);

    m_result.width  = dwWidth;
    m_result.height = dwHeight;
    m_result.mipLevels = (dwFlags & DDSD_MIPMAPCOUNT) ? dwMipMapCount : 1;
    if (m_result.mipLevels == 0)
        m_result.mipLevels = 1;

    if (pfFlags & DDPF_FOURCC)
    {
        switch (pfFourCC)
        {
        case FOURCC_DXT1:
            m_result.format = DDSFormat::DXT1;
            break;
        case FOURCC_DXT2:
            m_result.format = DDSFormat::DXT2;
            break;
        case FOURCC_DXT3:
            m_result.format = DDSFormat::DXT3;
            break;
        case FOURCC_DXT4:
            m_result.format = DDSFormat::DXT4;
            break;
        case FOURCC_DXT5:
            m_result.format = DDSFormat::DXT5;
            break;
        default:
            setError("Unsupported DXT format");
            return false;
        }
    }
    else if (pfFlags & DDPF_RGB)
    {
        m_result.hasAlpha = (pfFlags & DDPF_ALPHAPIXELS) != 0;
        switch (pfRGBBits)
        {
        case 32:
            m_result.format = m_result.hasAlpha ? DDSFormat::A8R8G8B8 : DDSFormat::X8R8G8B8;
            break;
        case 24:
            m_result.format = DDSFormat::R8G8B8;
            break;
        case 16:
            m_result.format = DDSFormat::R5G6B5;
            break;
        default:
            setError("Unsupported RGB bit count");
            return false;
        }
    }
    else if (pfFlags & DDPF_LUMINANCE)
    {
        m_result.format = m_result.hasAlpha ? DDSFormat::A8L8 : DDSFormat::L8;
    }
    else
    {
        setError("Unsupported pixel format");
        return false;
    }

    if (dwFlags & DDSD_LINEARSIZE)
    {
        m_result.linearSize = dwPitchOrLinear;
    }
    else if (dwFlags & DDSD_PITCH)
    {
        m_result.pitch = dwPitchOrLinear;
    }
    else
    {
        m_result.pitch = dwWidth * (pfRGBBits / 8);
    }

    if (caps2 & 0x20000000) // DDSCAPS2_VOLUME
    {
        m_result.depth = (dwFlags & DDSD_DEPTH) ? dwDepth : 1;
    }

    // Accumulate in 64-bit.  calculateDataSize itself multiplies width*height*4
    // in uint32_t, so a 65536x65536 A8R8G8B8 header truncated the total to 0: the
    // truncation check below then passed, resize(0) succeeded, and width/height
    // were stored unclamped — leaving the parser advertising a huge surface backed
    // by an empty pixel vector.  `dwMipMapCount` is also attacker-controlled and
    // was used unbounded as the loop count.
    constexpr uint32_t kMaxDdsMipLevels = 32;  // enough for a 2^31 surface
    if (m_result.mipLevels > kMaxDdsMipLevels)
    {
        setError("DDS mip level count exceeds the supported maximum");
        return false;
    }

    uint64_t totalSize = 0;
    uint32_t w = m_result.width;
    uint32_t h = m_result.height;
    uint32_t d = m_result.depth;
    for (uint32_t level = 0; level < m_result.mipLevels; ++level)
    {
        totalSize += static_cast<uint64_t>(calculateDataSize(w, h, m_result.format)) * d;
        w = (w > 1) ? (w / 2) : 1;
        h = (h > 1) ? (h / 2) : 1;
        d = (d > 1) ? (d / 2) : 1;
    }

    size_t pixelOffset = 124;
    if (totalSize > hdrSize || pixelOffset + totalSize > hdrSize)
    {
        setError("DDS pixel data truncated");
        return false;
    }

    m_result.pixels.resize(static_cast<size_t>(totalSize));
    std::memcpy(m_result.pixels.data(), hdr + pixelOffset,
                static_cast<size_t>(totalSize));

    return true;
}

DDSFormat DdsLoader::determineFormat(const uint8_t* pfData)
{
    uint32_t pfFlags = 0, pfFourCC = 0;
    std::memcpy(&pfFlags, pfData + 4, 4);
    std::memcpy(&pfFourCC, pfData + 8, 4);

    if (pfFlags & DDPF_FOURCC)
    {
        switch (pfFourCC)
        {
        case FOURCC_DXT1: return DDSFormat::DXT1;
        case FOURCC_DXT2: return DDSFormat::DXT2;
        case FOURCC_DXT3: return DDSFormat::DXT3;
        case FOURCC_DXT4: return DDSFormat::DXT4;
        case FOURCC_DXT5: return DDSFormat::DXT5;
        default: return DDSFormat::Unknown;
        }
    }
    if (pfFlags & DDPF_RGB)
    {
        uint32_t rMask = 0;
        std::memcpy(&rMask, pfData + 16, 4);
        switch (rMask)
        {
        case 0x00FF0000: return DDSFormat::A8R8G8B8;
        case 0xF800:     return DDSFormat::R5G6B5;
        default: return DDSFormat::Unknown;
        }
    }
    return DDSFormat::Unknown;
}

uint32_t DdsLoader::calculateDataSize(uint32_t width, uint32_t height, DDSFormat format)
{
    switch (format)
    {
    case DDSFormat::DXT1:
        return std::max(1u, ((width + 3) / 4)) * std::max(1u, ((height + 3) / 4)) * 8;
    case DDSFormat::DXT2:
    case DDSFormat::DXT3:
    case DDSFormat::DXT4:
    case DDSFormat::DXT5:
        return std::max(1u, ((width + 3) / 4)) * std::max(1u, ((height + 3) / 4)) * 16;
    case DDSFormat::A8R8G8B8:
    case DDSFormat::X8R8G8B8:
        return width * height * 4;
    case DDSFormat::R8G8B8:
        return width * height * 3;
    case DDSFormat::R5G6B5:
    case DDSFormat::X1R5G5B5:
    case DDSFormat::A1R5G5B5:
        return width * height * 2;
    case DDSFormat::L8:
        return width * height;
    case DDSFormat::A8L8:
        return width * height * 2;
    default:
        return 0;
    }
}

void DdsLoader::setError(const container::String& msg)
{
    m_error = msg;
}

} // namespace data::dds
