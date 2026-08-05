#pragma once

#include "container/container_types.h"

#include "File.h"
#include <cstdint>
namespace io {

struct ArchiveEntry
{
    container::String filename;
    uint32_t offset;
    uint32_t size;
    bool compressed = false;
};

class ArchiveFile : public File
{
public:
    ArchiveFile() = default;

    bool open(container::StringView filename, FileAccess access = FileAccess::Read) override;
    bool close() override;
    bool isOpen() const override { return m_data != nullptr && m_size > 0; }

    size_t read(void* buffer, size_t size) override;
    size_t readString(char* buffer, size_t maxSize) override;

    size_t write(const void* buffer, size_t size) override { return 0; }

    int64_t seek(int64_t offset, FileSeek origin = FileSeek::Start) override;
    int64_t tell() const override { return m_position; }

    int64_t size() const override { return m_size; }
    container::String getName() const override { return m_filename; }

    bool exists(container::StringView filename) const override;
    bool remove(container::StringView filename) override { return false; }
    bool rename(container::StringView oldName, container::StringView newName) override { return false; }

    // `dataSize` is the size of the whole mapping `data` points at.  It is
    // required so the entry window can be validated here: the offset/size pair
    // originates from the archive header and is therefore untrusted.  Returns
    // false and leaves the file empty when the window does not fit.
    bool setArchiveData(const uint8_t* data, uint64_t dataSize, uint32_t entryOffset,
                        uint32_t fileSize, container::StringView name);
    void setSharedData(
        container::SharedPtr<const container::Vector<uint8_t>> data,
        container::StringView name);

private:
    container::SharedPtr<const container::Vector<uint8_t>> m_sharedBuffer;
    const uint8_t* m_data = nullptr;
    uint32_t m_entryOffset = 0;
    uint32_t m_size = 0;
    int64_t m_position = 0;
    container::String m_filename;
};

} // namespace io
