#include "container/container_types.h"
#include "LocalFile.h"
#include <cstring>
#include <sys/stat.h>

namespace io {

LocalFile::LocalFile(container::StringView filename, FileAccess access)
{
    open(filename, access);
}

bool LocalFile::open(container::StringView filename, FileAccess access)
{
    close();
    m_filename = filename;
    m_access = access;
    m_file = nullptr;
    if (fopen_s(&m_file, m_filename.c_str(), accessMode(access)) != 0)
    {
        m_file = nullptr;
        return false;
    }
    return true;
}

bool LocalFile::close()
{
    if (m_file)
    {
        fclose(m_file);
        m_file = nullptr;
        return true;
    }
    return false;
}

size_t LocalFile::read(void* buffer, size_t size)
{
    if (!m_file) return 0;
    return fread(buffer, 1, size, m_file);
}

size_t LocalFile::readString(char* buffer, size_t maxSize)
{
    if (!m_file || !buffer || maxSize == 0) return 0;
    if (!fgets(buffer, static_cast<int>(maxSize), m_file)) return 0;
    return strlen(buffer);
}

size_t LocalFile::write(const void* buffer, size_t size)
{
    if (!m_file) return 0;
    return fwrite(buffer, 1, size, m_file);
}

int64_t LocalFile::seek(int64_t offset, FileSeek origin)
{
    if (!m_file) return -1;
    int fseekOrigin = SEEK_SET;
    switch (origin)
    {
    case FileSeek::Start:   fseekOrigin = SEEK_SET; break;
    case FileSeek::Current: fseekOrigin = SEEK_CUR; break;
    case FileSeek::End:     fseekOrigin = SEEK_END; break;
    }
    if (_fseeki64(m_file, offset, fseekOrigin) != 0) return -1;
    return tell();
}

int64_t LocalFile::tell() const
{
    if (!m_file) return -1;
    return _ftelli64(m_file);
}

int64_t LocalFile::size() const
{
    if (m_file)
    {
        auto pos = tell();
        const_cast<LocalFile*>(this)->seek(0, FileSeek::End);
        int64_t fileSize = tell();
        const_cast<LocalFile*>(this)->seek(pos, FileSeek::Start);
        return fileSize;
    }
    struct _stat64 statBuf;
    if (_stat64(m_filename.c_str(), &statBuf) == 0)
    {
        return statBuf.st_size;
    }
    return -1;
}

bool LocalFile::exists(container::StringView filename) const
{
    struct _stat64 statBuf;
    return _stat64(container::String(filename).c_str(), &statBuf) == 0;
}

bool LocalFile::remove(container::StringView filename)
{
    return std::remove(filename.data()) == 0;
}

bool LocalFile::rename(container::StringView oldName, container::StringView newName)
{
    return std::rename(oldName.data(), newName.data()) == 0;
}

const char* LocalFile::accessMode(FileAccess access) const
{
    switch (access)
    {
    case FileAccess::Read:      return "rb";
    case FileAccess::Write:     return "wb";
    case FileAccess::ReadWrite: return "r+b";
    }
    return "rb";
}

} // namespace io
