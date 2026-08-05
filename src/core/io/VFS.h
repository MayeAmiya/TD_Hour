#pragma once

#include "container/container_types.h"
#include "container/hash_containers.h"

#include "ArchiveFileSystem.h"
#include "File.h"
#include "FileSystem.h"
#include "LocalFileSystem.h"
#include <cstdint>
#include <mutex>
#include <utility>
namespace io {

class VFS : public FileSystem
{
public:
    VFS() = default;
    ~VFS() override = default;

    static VFS& instance();

    void mountLocal(container::StringView basePath, int priority = 0);
    void mountLocal(container::StringView basePath, container::StringView virtualRoot, int priority = 0);
    bool mountArchive(container::StringView archivePath, int priority = 0);
    bool mountArchiveFromData(container::StringView name, const uint8_t* data, size_t dataSize, int priority = 0);

    // Seal the current mount stack into one winner/layer index. Later mounts
    // invalidate it and the next query rebuilds it.
    void rebuildIndex();
    [[nodiscard]] size_t indexedFileCount() const;
    // Monotonic content epoch for caches which retain resolved VFS products
    // such as parsed in-game WND trees. Mount, write and remove operations all
    // invalidate the winner index and advance this value.
    [[nodiscard]] uint64_t contentRevision() const;

    bool open(container::StringView filename, container::UniquePtr<File>& outFile,
              FileAccess access = FileAccess::Read) override;
    bool exists(container::StringView filename) const override;
    container::Vector<container::String> getFileList(container::StringView pattern = {}) const override;
    container::String getName() const override { return "VFS"; }

    container::String readAll(container::StringView filename);
    container::Vector<container::String> readAllLayers(container::StringView filename);
    bool readToBuffer(container::StringView filename, container::Vector<uint8_t>& buffer);
    bool writeAll(container::StringView filename, container::StringView content);
    bool writeBuffer(container::StringView filename, const container::Vector<uint8_t>& buffer);
    bool remove(container::StringView filename);

private:
    struct FileSystemLayer
    {
        container::UniquePtr<FileSystem> fs;
        int priority;
        uint64_t mountSequence;
    };

    struct IndexedFileLayer
    {
        FileSystem* fs = nullptr;
        container::String path;
    };

    container::Vector<FileSystemLayer> m_layers;
    uint64_t m_nextMountSequence = 0;

    mutable std::mutex m_indexMutex;
    mutable container::HashMap<container::String,
                               container::Vector<IndexedFileLayer>> m_fileLayers;
    mutable container::Vector<container::String> m_winningFiles;
    // Canonical path -> index in m_winningFiles. The sorted projection makes
    // directory-prefix enumeration proportional to the matching range while
    // the stored index preserves public winner/layer ordering.
    mutable container::Vector<std::pair<container::String, size_t>>
        m_winningFilesByCanonical;
    mutable container::HashMap<container::String,
                               container::Vector<container::String>> m_fileListCache;
    mutable bool m_indexDirty = true;
    mutable uint64_t m_contentRevision = 1;

    void sortLayers();
    void invalidateIndexLocked() noexcept;
    void ensureIndexLocked() const;
};

} // namespace io
