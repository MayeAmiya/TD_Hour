#pragma once

#include "container/container_types.h"

#include "TgaTypes.h"
#include <cstdint>
namespace data::tga {

class TgaLoader
{
public:
    TgaLoader() = default;

    bool loadFromFile(const container::String& path);
    bool loadFromMemory(const uint8_t* data, size_t size);

    const TGAData& result() const { return m_result; }
    TGAData takeResult() { return std::move(m_result); }
    const container::String& error() const { return m_error; }

    void reset();

private:
    bool parseUncompressed(const uint8_t* pixels, size_t pixelSize);
    bool parseRLE(const uint8_t* pixels, size_t pixelSize);

    uint32_t bppToBytes() const;

    void setError(const container::String& msg);

    TGAHeader   m_header{};
    TGAData     m_result;
    container::String m_error;
};

} // namespace data::tga
