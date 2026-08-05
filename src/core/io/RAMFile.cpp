#include "container/container_types.h"
#include "RAMFile.h"
#include <algorithm>
#include <cstring>

namespace io {

RAMFile::RAMFile(container::StringView filename, const void* data, size_t dataSize)
    : m_filename(filename)
{
    if (data && dataSize > 0)
    {
        m_data.assign(static_cast<const uint8_t*>(data),
                      static_cast<const uint8_t*>(data) + dataSize);
    }
}

bool RAMFile::open(container::StringView filename, FileAccess access)
{
    m_filename = filename;
    m_position = 0;
    return true;
}

bool RAMFile::close()
{
    m_data.clear();
    m_filename.clear();
    m_position = 0;
    return true;
}

size_t RAMFile::read(void* buffer, size_t size)
{
    if (!buffer || size == 0 || m_position >= static_cast<int64_t>(m_data.size()))
    {
        return 0;
    }
    size_t bytesToRead = std::min(size, static_cast<size_t>(m_data.size() - m_position));
    std::memcpy(buffer, m_data.data() + m_position, bytesToRead);
    m_position += static_cast<int64_t>(bytesToRead);
    return bytesToRead;
}

size_t RAMFile::readString(char* buffer, size_t maxSize)
{
    if (!buffer || maxSize == 0 || m_position >= static_cast<int64_t>(m_data.size()))
    {
        return 0;
    }
    size_t i = 0;
    while (i < maxSize - 1 && m_position < static_cast<int64_t>(m_data.size()))
    {
        char c = static_cast<char>(m_data[static_cast<size_t>(m_position)]);
        m_position++;
        buffer[i++] = c;
        if (c == '\n') break;
    }
    buffer[i] = '\0';
    return i;
}

size_t RAMFile::write(const void* buffer, size_t size)
{
    if (!buffer || size == 0) return 0;

    auto newPos = m_position + static_cast<int64_t>(size);
    if (newPos > static_cast<int64_t>(m_data.size()))
    {
        m_data.resize(static_cast<size_t>(newPos));
    }

    std::memcpy(m_data.data() + m_position, buffer, size);
    m_position = newPos;
    return size;
}

int64_t RAMFile::seek(int64_t offset, FileSeek origin)
{
    int64_t newPos = m_position;
    switch (origin)
    {
    case FileSeek::Start:   newPos = offset; break;
    case FileSeek::Current: newPos = m_position + offset; break;
    case FileSeek::End:     newPos = static_cast<int64_t>(m_data.size()) + offset; break;
    }
    m_position = std::clamp(newPos, int64_t(0), static_cast<int64_t>(m_data.size()));
    return m_position;
}

bool RAMFile::exists(container::StringView filename) const
{
    return m_filename == filename;
}

bool RAMFile::remove(container::StringView filename)
{
    return false;
}

bool RAMFile::rename(container::StringView oldName, container::StringView newName)
{
    return false;
}

void RAMFile::setData(container::StringView filename, const void* data, size_t dataSize)
{
    m_filename = filename;
    m_position = 0;
    if (data && dataSize > 0)
    {
        m_data.assign(static_cast<const uint8_t*>(data),
                      static_cast<const uint8_t*>(data) + dataSize);
    }
    else
    {
        m_data.clear();
    }
}

} // namespace io
