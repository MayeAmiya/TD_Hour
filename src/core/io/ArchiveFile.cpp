#include "container/container_types.h"
#include "ArchiveFile.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace io {

bool ArchiveFile::open(container::StringView filename, FileAccess access)
{
    m_filename = filename;
    m_position = 0;
    return true;
}

bool ArchiveFile::close()
{
    m_sharedBuffer.reset();
    m_data = nullptr;
    m_entryOffset = 0;
    m_size = 0;
    m_position = 0;
    m_filename.clear();
    return true;
}

size_t ArchiveFile::read(void* buffer, size_t size)
{
    if (!buffer || size == 0 || !m_data || m_position >= static_cast<int64_t>(m_size))
    {
        return 0;
    }
    size_t bytesToRead = std::min(size, static_cast<size_t>(m_size - m_position));
    uint64_t readOffset = static_cast<uint64_t>(m_entryOffset) + static_cast<uint64_t>(m_position);
    std::memcpy(buffer, m_data + readOffset, bytesToRead);
    m_position += static_cast<int64_t>(bytesToRead);
    return bytesToRead;
}

size_t ArchiveFile::readString(char* buffer, size_t maxSize)
{
    if (!buffer || maxSize == 0 || !m_data || m_position >= static_cast<int64_t>(m_size))
    {
        return 0;
    }
    size_t i = 0;
    while (i < maxSize - 1 && m_position < static_cast<int64_t>(m_size))
    {
        // Widen both operands before adding, exactly like read() does: the old
        // expression promoted to int64_t and skipped read()'s explicit offset
        // computation.
        const uint64_t readOffset =
            static_cast<uint64_t>(m_entryOffset) + static_cast<uint64_t>(m_position);
        char c = static_cast<char>(m_data[readOffset]);
        m_position++;
        buffer[i++] = c;
        if (c == '\n') break;
    }
    buffer[i] = '\0';
    return i;
}

int64_t ArchiveFile::seek(int64_t offset, FileSeek origin)
{
    int64_t newPos = m_position;
    switch (origin)
    {
    case FileSeek::Start:   newPos = offset; break;
    case FileSeek::Current: newPos = m_position + offset; break;
    case FileSeek::End:     newPos = static_cast<int64_t>(m_size) + offset; break;
    }
    m_position = std::clamp(newPos, int64_t(0), static_cast<int64_t>(m_size));
    return m_position;
}

bool ArchiveFile::exists(container::StringView filename) const
{
    return m_filename == filename;
}

bool ArchiveFile::setArchiveData(const uint8_t* data, uint64_t dataSize,
                                  uint32_t entryOffset, uint32_t fileSize,
                                  container::StringView name)
{
    m_sharedBuffer.reset();
    m_data = nullptr;
    m_entryOffset = 0;
    m_size = 0;
    m_position = 0;
    m_filename = name;

    // Validate the entry window against the real mapping.  parseArchive checks
    // this too, but it did its cursor arithmetic in 32 bits, so a wrapped header
    // could get past it — and nothing else stood between a bad offset/size pair
    // and reads outside the mapped view.
    if (!data) return false;
    if (static_cast<uint64_t>(entryOffset) + fileSize > dataSize) return false;

    m_data = data;
    m_entryOffset = entryOffset;
    m_size = fileSize;
    return true;
}

void ArchiveFile::setSharedData(
    container::SharedPtr<const container::Vector<uint8_t>> data,
    container::StringView name)
{
    m_filename = name;
    m_position = 0;
    m_sharedBuffer = std::move(data);
    if (!m_sharedBuffer || m_sharedBuffer->empty() ||
        m_sharedBuffer->size() > std::numeric_limits<uint32_t>::max()) {
        m_sharedBuffer.reset();
        m_data = nullptr;
        m_size = 0;
        return;
    }
    m_data = m_sharedBuffer->data();
    m_entryOffset = 0;
    m_size = static_cast<uint32_t>(m_sharedBuffer->size());
}

} // namespace io
