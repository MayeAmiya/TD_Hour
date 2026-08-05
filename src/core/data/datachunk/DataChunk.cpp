#include "container/container_types.h"
#include "DataChunk.h"

#include <cstring>
#include <stdexcept>

namespace data {

DataChunkInput::DataChunkInput(const uint8_t* data, size_t size)
    : m_data(data)
    , m_size(size)
{
}

bool DataChunkInput::openChunk()
{
    if (m_chunkStack.empty())
    {
        if (m_bytesRead + 12 > m_size)
            return false;

        m_chunkLabel = *reinterpret_cast<const uint32_t*>(m_data + m_bytesRead);
        m_chunkVersion = *reinterpret_cast<const uint32_t*>(m_data + m_bytesRead + 4);
        m_chunkSize = *reinterpret_cast<const uint32_t*>(m_data + m_bytesRead + 8);
        m_chunkOffset = m_bytesRead + 12;
        m_chunkBytesRead = 0;

        ChunkStack frame;
        frame.endOffset = m_chunkOffset + m_chunkSize;
        frame.nextChunkOffset = m_chunkOffset;
        frame.bytesRead = 0;
        frame.parentSize = m_chunkSize;
        m_chunkStack.push_back(frame);

        m_bytesRead += 12;
        return true;
    }

    auto& parent = m_chunkStack.back();
    if (parent.bytesRead >= parent.parentSize)
        return false;

    m_bytesRead = parent.nextChunkOffset;

    if (m_bytesRead + 12 > m_size)
        return false;

    m_chunkLabel = *reinterpret_cast<const uint32_t*>(m_data + m_bytesRead);
    m_chunkVersion = *reinterpret_cast<const uint32_t*>(m_data + m_bytesRead + 4);
    m_chunkSize = *reinterpret_cast<const uint32_t*>(m_data + m_bytesRead + 8);
    m_chunkOffset = m_bytesRead + 12;
    m_chunkBytesRead = 0;

    ChunkStack frame;
    frame.endOffset = m_chunkOffset + m_chunkSize;
    frame.nextChunkOffset = m_chunkOffset + m_chunkSize;
    frame.bytesRead = 0;
    frame.parentSize = m_chunkSize;
    m_chunkStack.push_back(frame);

    m_bytesRead += 12;
    return true;
}

bool DataChunkInput::openChunk(ChunkLabel expectedLabel)
{
    while (openChunk())
    {
        if (getCurrentLabel() == expectedLabel)
            return true;
        closeChunk();
    }
    return false;
}

void DataChunkInput::closeChunk()
{
    if (m_chunkStack.empty())
        return;

    auto& current = m_chunkStack.back();
    m_bytesRead = current.endOffset;
    current.bytesRead = m_chunkSize;
    m_chunkStack.pop_back();

    if (!m_chunkStack.empty())
    {
        auto& parent = m_chunkStack.back();
        parent.bytesRead += m_chunkSize + 12;
        parent.nextChunkOffset = m_bytesRead;
        m_chunkSize = parent.parentSize;
    }
}

uint8_t DataChunkInput::readByte()
{
    if (m_chunkBytesRead >= m_chunkSize)
        return 0;
    uint8_t value = m_data[m_bytesRead];
    m_bytesRead++;
    m_chunkBytesRead++;
    if (!m_chunkStack.empty())
        m_chunkStack.back().bytesRead++;
    return value;
}

uint16_t DataChunkInput::readShort()
{
    uint16_t value;
    readBytes(&value, sizeof(value));
    return value;
}

uint32_t DataChunkInput::readInt()
{
    uint32_t value;
    readBytes(&value, sizeof(value));
    return value;
}

float DataChunkInput::readFloat()
{
    float value;
    readBytes(&value, sizeof(value));
    return value;
}

double DataChunkInput::readDouble()
{
    double value;
    readBytes(&value, sizeof(value));
    return value;
}

container::String DataChunkInput::readString()
{
    uint32_t len = readInt();
    if (len == 0 || len > m_chunkSize - m_chunkBytesRead)
        return {};
    container::String value(reinterpret_cast<const char*>(m_data + m_bytesRead), len);
    m_bytesRead += len;
    m_chunkBytesRead += len;
    if (!m_chunkStack.empty())
        m_chunkStack.back().bytesRead += len;
    return value;
}

void DataChunkInput::readBytes(void* buffer, size_t size)
{
    if (!buffer || size == 0)
        return;
    size_t remaining = m_chunkSize - m_chunkBytesRead;
    size_t toRead = size < remaining ? size : remaining;
    std::memcpy(buffer, m_data + m_bytesRead, toRead);
    m_bytesRead += toRead;
    m_chunkBytesRead += toRead;
    if (!m_chunkStack.empty())
        m_chunkStack.back().bytesRead += toRead;
}

DataChunkOutput::DataChunkOutput()
{
    m_data.reserve(4096);
}

void DataChunkOutput::beginChunk(ChunkLabel label, uint32_t version)
{
    writeInt(label);
    writeInt(version);
    size_t sizePos = m_data.size();
    writeInt(0);

    ChunkFrame frame;
    frame.sizePos = sizePos;
    m_chunkStack.push_back(frame);
}

void DataChunkOutput::endChunk()
{
    if (m_chunkStack.empty())
        return;
    auto frame = m_chunkStack.back();
    m_chunkStack.pop_back();

    uint32_t dataSize = static_cast<uint32_t>(m_data.size() - frame.sizePos - 4);
    std::memcpy(m_data.data() + frame.sizePos, &dataSize, sizeof(dataSize));
}

void DataChunkOutput::writeByte(uint8_t value)
{
    m_data.push_back(value);
}

void DataChunkOutput::writeShort(uint16_t value)
{
    m_data.resize(m_data.size() + sizeof(value));
    std::memcpy(m_data.data() + m_data.size() - sizeof(value), &value, sizeof(value));
}

void DataChunkOutput::writeInt(uint32_t value)
{
    m_data.resize(m_data.size() + sizeof(value));
    std::memcpy(m_data.data() + m_data.size() - sizeof(value), &value, sizeof(value));
}

void DataChunkOutput::writeFloat(float value)
{
    m_data.resize(m_data.size() + sizeof(value));
    std::memcpy(m_data.data() + m_data.size() - sizeof(value), &value, sizeof(value));
}

void DataChunkOutput::writeDouble(double value)
{
    m_data.resize(m_data.size() + sizeof(value));
    std::memcpy(m_data.data() + m_data.size() - sizeof(value), &value, sizeof(value));
}

void DataChunkOutput::writeString(const container::String& value)
{
    writeInt(static_cast<uint32_t>(value.size()));
    m_data.insert(m_data.end(), value.begin(), value.end());
}

void DataChunkOutput::writeBytes(const void* buffer, size_t size)
{
    if (!buffer || size == 0)
        return;
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    m_data.insert(m_data.end(), bytes, bytes + size);
}

} // namespace data
