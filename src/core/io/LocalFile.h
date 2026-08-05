#pragma once

#include "container/container_types.h"

#include "File.h"
#include <cstdio>
namespace io {

class LocalFile : public File
{
public:
    LocalFile() = default;
    explicit LocalFile(container::StringView filename, FileAccess access = FileAccess::Read);
    ~LocalFile() override { close(); }

    bool open(container::StringView filename, FileAccess access = FileAccess::Read) override;
    bool close() override;
    bool isOpen() const override { return m_file != nullptr; }

    size_t read(void* buffer, size_t size) override;
    size_t readString(char* buffer, size_t maxSize) override;

    size_t write(const void* buffer, size_t size) override;

    int64_t seek(int64_t offset, FileSeek origin = FileSeek::Start) override;
    int64_t tell() const override;

    int64_t size() const override;
    container::String getName() const override { return m_filename; }

    bool exists(container::StringView filename) const override;
    bool remove(container::StringView filename) override;
    bool rename(container::StringView oldName, container::StringView newName) override;

private:
    FILE* m_file = nullptr;
    container::String m_filename;
    FileAccess m_access = FileAccess::Read;

    const char* accessMode(FileAccess access) const;
};

} // namespace io
