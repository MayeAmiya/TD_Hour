#pragma once

#include "container/container_types.h"

#include <cstdint>
namespace io {

enum class FileAccess : uint8_t
{
    Read,
    Write,
    ReadWrite,
};

enum class FileShare : uint8_t
{
    None,
    Read,
    Write,
    ReadWrite,
};

enum class FileSeek : uint8_t
{
    Start,
    Current,
    End,
};

class File
{
public:
    virtual ~File() = default;

    virtual bool open(container::StringView filename, FileAccess access = FileAccess::Read) = 0;
    virtual bool close() = 0;
    virtual bool isOpen() const = 0;

    virtual size_t read(void* buffer, size_t size) = 0;
    virtual size_t readString(char* buffer, size_t maxSize) = 0;
    virtual container::String readAll();

    virtual size_t write(const void* buffer, size_t size) = 0;
    virtual size_t writeString(container::StringView str);

    virtual int64_t seek(int64_t offset, FileSeek origin = FileSeek::Start) = 0;
    virtual int64_t tell() const = 0;

    virtual int64_t size() const = 0;
    virtual container::String getName() const = 0;
    virtual int64_t getPosition() const { return tell(); }

    virtual bool exists(container::StringView filename) const = 0;

    virtual bool remove(container::StringView filename) = 0;
    virtual bool rename(container::StringView oldName, container::StringView newName) = 0;
};

} // namespace io
