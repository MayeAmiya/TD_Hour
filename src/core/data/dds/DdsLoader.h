#pragma once

#include "container/container_types.h"

#include "DdsTypes.h"
#include <cstdint>
namespace data::dds {

class DdsLoader
{
public:
    DdsLoader() = default;

    bool loadFromFile(const container::String& path);
    bool loadFromMemory(const uint8_t* data, size_t size);

    // Decode only the top mip level into the canonical RGBA byte order used
    // by SDL surfaces and the CPU-side texture cache.  This is intentionally
    // kept next to the DDS parser so non-render clients (for example the
    // authored mouse cursor runtime) do not depend on TextureManager.
    bool decodeTopLevelRgba(container::Vector<uint8_t>& rgba) const;

    const DDSData& result() const { return m_result; }
    DDSData takeResult() { return std::move(m_result); }
    const container::String& error() const { return m_error; }

    void reset();

    static DDSFormat determineFormat(const uint8_t* pfData);
    static uint32_t calculateDataSize(uint32_t width, uint32_t height, DDSFormat format);

private:
    bool parseHeader(const uint8_t* hdr, size_t hdrSize);
    void setError(const container::String& msg);

    DDSData    m_result;
    container::String m_error;
};

} // namespace data::dds
