#pragma once

#include "container/container_types.h"

#include "FileSystem.h"
#include <filesystem>
namespace io {

class LocalFileSystem : public FileSystem
{
public:
    explicit LocalFileSystem(container::StringView basePath = {});
    LocalFileSystem(container::StringView basePath, container::StringView virtualRoot);

    bool open(container::StringView filename, container::UniquePtr<File>& outFile,
              FileAccess access = FileAccess::Read) override;
    bool exists(container::StringView filename) const override;
    container::Vector<container::String> getFileList(container::StringView pattern = {}) const override;
    container::String getName() const override { return "LocalFileSystem"; }

    container::String resolvePath(container::StringView filename) const;
    [[nodiscard]] bool acceptsVirtualPath(container::StringView filename) const;
    void setBasePath(container::StringView path) { m_basePath = path; }
    container::String getBasePath() const { return m_basePath; }

private:
    container::String toVirtualPath(const std::filesystem::path& path) const;
    container::String stripVirtualRoot(container::StringView filename) const;

    container::String m_basePath;
    container::String m_virtualRoot;
    bool m_virtualRootValid = true;
};

} // namespace io
