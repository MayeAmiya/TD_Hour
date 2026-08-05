#pragma once

#include "container/container_types.h"

#include "File.h"
namespace io {

class FileSystem
{
public:
    virtual ~FileSystem() = default;

    virtual bool open(container::StringView filename, container::UniquePtr<File>& outFile,
                      FileAccess access = FileAccess::Read) = 0;
    virtual bool exists(container::StringView filename) const = 0;
    virtual container::Vector<container::String> getFileList(container::StringView pattern = {}) const = 0;
    virtual container::String getName() const = 0;
};

} // namespace io
