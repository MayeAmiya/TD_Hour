#pragma once

#include "container/hash_containers.h"

#include "ArchiveFile.h"
#include "FileSystem.h"
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
namespace io {

class ArchiveFileSystem : public FileSystem
{
public:
    ArchiveFileSystem() = default;
    ~ArchiveFileSystem() override;

    bool loadArchive(container::StringView archivePath);
    bool loadArchiveFromData(container::StringView name, const uint8_t* data, size_t dataSize);

    bool open(container::StringView filename, container::UniquePtr<File>& outFile,
              FileAccess access = FileAccess::Read) override;
    bool exists(container::StringView filename) const override;
    container::Vector<container::String> getFileList(container::StringView pattern = {}) const override;
    container::String getName() const override;

    size_t getArchiveCount() const { return m_archives.size(); }
    size_t getFileCount() const;
    [[nodiscard]] bool entryComesFromArchive(
        container::StringView filename,
        container::StringView archiveName) const;
    [[nodiscard]] std::optional<uint64_t> entryLogicalSize(
        container::StringView filename) const;

private:
    enum class CompressedCacheState : uint8_t
    {
        Empty = 0,
        Loading,
        Ready,
        Failed,
    };

    struct CompressedEntryCache final
    {
        std::mutex mutex;
        std::condition_variable ready;
        container::SharedPtr<const container::Vector<uint8_t>> data;
        CompressedCacheState state = CompressedCacheState::Empty;
    };

    struct LoadedArchive
    {
        container::String name;
        uint8_t* data = nullptr;
        size_t dataSize = 0;
        container::Vector<ArchiveEntry> entries;
        container::Vector<container::UniquePtr<CompressedEntryCache>>
            compressedCaches;
        uint32_t dataStartOffset = 0;
        bool owned = true;
        void* fileHandle = nullptr;
        void* mappingHandle = nullptr;

        ~LoadedArchive();
    };

    container::Vector<container::UniquePtr<LoadedArchive>> m_archives;

    struct EntryRef { size_t archiveIdx; size_t entryIdx; };
    container::HashMap<container::String, EntryRef> m_fileMap;

    bool parseArchive(LoadedArchive& archive);
    bool openCompressedEntry(LoadedArchive& archive, size_t entryIndex,
                             container::UniquePtr<File>& outFile);

    static container::String normalizePath(container::StringView path);
};

} // namespace io
