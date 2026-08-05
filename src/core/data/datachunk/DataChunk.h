#pragma once

#include "container/container_types.h"

#include <cstddef>
#include <cstdint>
namespace data {

constexpr uint32_t DATACHUNK_LABEL_SIZE = 4;

using ChunkLabel = uint32_t;

constexpr ChunkLabel makeLabel(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a) |
           (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) |
           (static_cast<uint32_t>(d) << 24);
}

class DataChunkInput
{
public:
    explicit DataChunkInput(const uint8_t* data, size_t size);

    bool openChunk();
    bool openChunk(ChunkLabel expectedLabel);
    void closeChunk();

    uint8_t    readByte();
    uint16_t   readShort();
    uint32_t   readInt();
    float      readFloat();
    double     readDouble();
    container::String readString();
    void       readBytes(void* buffer, size_t size);

    ChunkLabel getCurrentLabel() const { return m_chunkLabel; }
    uint32_t   getCurrentVersion() const { return m_chunkVersion; }
    size_t     getCurrentSize() const { return m_chunkSize; }
    size_t     getCurrentOffset() const { return m_chunkOffset; }
    size_t     getBytesRead() const { return m_chunkBytesRead; }
    bool       isComplete() const { return m_bytesRead >= m_size; }

private:
    const uint8_t* m_data;
    size_t         m_size;
    size_t         m_bytesRead = 0;

    struct ChunkStack
    {
        size_t endOffset;
        size_t nextChunkOffset;
        size_t bytesRead = 0;
        size_t parentSize = 0;
    };

    container::Vector<ChunkStack> m_chunkStack;

    ChunkLabel m_chunkLabel = 0;
    uint32_t   m_chunkVersion = 0;
    size_t     m_chunkSize = 0;
    size_t     m_chunkOffset = 0;
    size_t     m_chunkBytesRead = 0;
};

class DataChunkOutput
{
public:
    DataChunkOutput();

    void beginChunk(ChunkLabel label, uint32_t version = 0);
    void endChunk();

    void writeByte(uint8_t value);
    void writeShort(uint16_t value);
    void writeInt(uint32_t value);
    void writeFloat(float value);
    void writeDouble(double value);
    void writeString(const container::String& value);
    void writeBytes(const void* buffer, size_t size);

    const container::Vector<uint8_t>& getData() const { return m_data; }
    container::Vector<uint8_t> takeData() { return std::move(m_data); }

private:
    container::Vector<uint8_t> m_data;

    struct ChunkFrame
    {
        size_t sizePos;
    };

    container::Vector<ChunkFrame> m_chunkStack;
};

} // namespace data
