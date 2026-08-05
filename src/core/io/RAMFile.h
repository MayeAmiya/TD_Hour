#pragma once

#include "container/container_types.h"

#include "File.h"
namespace io {

class RAMFile : public File
{
public:
    RAMFile() = default;
    explicit RAMFile(container::StringView filename, const void* data = nullptr, size_t dataSize = 0);

    bool open(container::StringView filename, FileAccess access = FileAccess::Read) override;
    bool close() override;
    bool isOpen() const override { return !m_data.empty(); }

    size_t read(void* buffer, size_t size) override;
    size_t readString(char* buffer, size_t maxSize) override;

    size_t write(const void* buffer, size_t size) override;

    int64_t seek(int64_t offset, FileSeek origin = FileSeek::Start) override;
    int64_t tell() const override { return m_position; }

    int64_t size() const override { return static_cast<int64_t>(m_data.size()); }
    container::String getName() const override { return m_filename; }

    bool exists(container::StringView filename) const override;
    bool remove(container::StringView filename) override;
    bool rename(container::StringView oldName, container::StringView newName) override;

    void setData(container::StringView filename, const void* data, size_t dataSize);
    const uint8_t* data() const { return m_data.data(); }

private:
    container::Vector<uint8_t> m_data;
    container::String m_filename;
    int64_t m_position = 0;
};

} // namespace io
