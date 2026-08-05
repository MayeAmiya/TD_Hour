#include "container/container_types.h"
#include "container/string_utils.h"
#include "ArchiveFileSystem.h"
#include "VirtualPath.h"
#include "core/compression/runtime/manager.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace io {

namespace {

constexpr uint32_t kMaximumArchiveEntryCount = 100'000;

container::String normalizeArchivePattern(container::StringView path)
{
    return virtual_path::pattern(path);
}

bool matchesPattern(container::StringView filename, container::StringView pattern)
{
    if (pattern.empty()) return true;

    const container::String name = normalizeArchivePattern(filename);
    const container::String pat = normalizeArchivePattern(pattern);
    if (name.empty() || pat.empty()) return false;

    const bool hasWildcard = pat.find('*') != container::String::npos || pat.find('?') != container::String::npos;
    if (!hasWildcard) {
        return name == pat;
    }

    size_t namePos = 0;
    size_t patPos = 0;
    size_t starPos = container::String::npos;
    size_t matchPos = 0;
    while (namePos < name.size()) {
        if (patPos < pat.size() && (pat[patPos] == '?' || pat[patPos] == name[namePos])) {
            ++namePos;
            ++patPos;
        } else if (patPos < pat.size() && pat[patPos] == '*') {
            starPos = patPos++;
            matchPos = namePos;
        } else if (starPos != container::String::npos) {
            patPos = starPos + 1;
            namePos = ++matchPos;
        } else {
            return false;
        }
    }

    while (patPos < pat.size() && pat[patPos] == '*') {
        ++patPos;
    }
    return patPos == pat.size();
}

} // namespace

ArchiveFileSystem::~ArchiveFileSystem() = default;

ArchiveFileSystem::LoadedArchive::~LoadedArchive()
{
#ifdef _WIN32
    if (mappingHandle || fileHandle) {
        if (data) UnmapViewOfFile(data);
        if (mappingHandle) CloseHandle(static_cast<HANDLE>(mappingHandle));
        if (fileHandle) CloseHandle(static_cast<HANDLE>(fileHandle));
        data = nullptr;
        mappingHandle = nullptr;
        fileHandle = nullptr;
        return;
    }
#endif
    if (owned && data) delete[] data;
    data = nullptr;
}

bool ArchiveFileSystem::loadArchive(container::StringView archivePath)
{
    auto archive = std::make_unique<LoadedArchive>();
    container::String pathStr(archivePath);
    const std::filesystem::path path{pathStr};
    archive->name = path.filename().string();

#ifdef _WIN32
    const HANDLE fileHandle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE) return false;
    archive->fileHandle = fileHandle;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(fileHandle, &fileSize) ||
        fileSize.QuadPart <= 0 ||
        static_cast<uint64_t>(fileSize.QuadPart) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    const HANDLE mappingHandle = CreateFileMappingW(
        fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mappingHandle) return false;
    archive->mappingHandle = mappingHandle;
    archive->data = static_cast<uint8_t*>(MapViewOfFile(
        mappingHandle, FILE_MAP_READ, 0, 0, 0));
    if (!archive->data) return false;
    archive->dataSize = static_cast<size_t>(fileSize.QuadPart);
    archive->owned = false;
#else
#error ArchiveFileSystem disk archives require the Windows/MSVC mapping backend
#endif

    if (!parseArchive(*archive))
    {
        return false;
    }

    for (size_t i = 0; i < archive->entries.size(); ++i)
    {
        auto& entry = archive->entries[i];
        const container::String key = normalizePath(entry.filename);
        if (!key.empty()) m_fileMap[key] = { m_archives.size(), i };
    }

    m_archives.push_back(std::move(archive));
    return true;
}

bool ArchiveFileSystem::loadArchiveFromData(container::StringView name, const uint8_t* data, size_t dataSize)
{
    auto archive = std::make_unique<LoadedArchive>();
    archive->name = name;
    archive->data = const_cast<uint8_t*>(data);
    archive->dataSize = dataSize;
    archive->owned = false;

    if (!parseArchive(*archive))
    {
        return false;
    }

    for (size_t i = 0; i < archive->entries.size(); ++i)
    {
        auto& entry = archive->entries[i];
        const container::String key = normalizePath(entry.filename);
        if (!key.empty()) m_fileMap[key] = { m_archives.size(), i };
    }

    m_archives.push_back(std::move(archive));
    return true;
}

bool ArchiveFileSystem::open(container::StringView filename, container::UniquePtr<File>& outFile,
                              FileAccess)
{
    auto key = normalizePath(filename);
    auto it = m_fileMap.find(key);
    if (it == m_fileMap.end()) return false;

    auto& ref = it->second;
    auto& archive = m_archives[ref.archiveIdx];
    auto& entry = archive->entries[ref.entryIdx];

    if (entry.compressed)
    {
        return openCompressedEntry(*archive, ref.entryIdx, outFile);
    }
    auto file = std::make_unique<ArchiveFile>();
    if (!file->setArchiveData(archive->data,
            archive->dataSize,
            entry.offset,
            entry.size,
            entry.filename))
    {
        return false;
    }
    file->open(entry.filename);
    outFile = std::move(file);
    return true;
}

bool ArchiveFileSystem::openCompressedEntry(
    LoadedArchive& archive, size_t entryIndex,
    container::UniquePtr<File>& outFile)
{
    if (entryIndex >= archive.entries.size() ||
        entryIndex >= archive.compressedCaches.size() ||
        !archive.compressedCaches[entryIndex]) {
        return false;
    }
    const ArchiveEntry& entry = archive.entries[entryIndex];
    CompressedEntryCache& cache = *archive.compressedCaches[entryIndex];
    container::SharedPtr<const container::Vector<uint8_t>> data;
    bool decode = false;
    {
        std::unique_lock lock(cache.mutex);
        while (cache.state == CompressedCacheState::Loading)
            cache.ready.wait(lock);
        if (cache.state == CompressedCacheState::Ready) {
            data = cache.data;
        } else if (cache.state == CompressedCacheState::Failed) {
            return false;
        } else {
            cache.state = CompressedCacheState::Loading;
            decode = true;
        }
    }

    if (decode) {
        container::SharedPtr<container::Vector<uint8_t>> decoded;
        try {
            if (entry.size <= static_cast<uint32_t>(
                    std::numeric_limits<int32_t>::max())) {
                const uint8_t* source = archive.data + entry.offset;
                const int32_t sourceSize = static_cast<int32_t>(entry.size);
                const int32_t uncompressedSize =
                    compression::manager::uncompressed_size(source, sourceSize);
                if (uncompressedSize > 0) {
                    decoded = std::make_shared<container::Vector<uint8_t>>();
                    decoded->resize(static_cast<size_t>(uncompressedSize));
                    const int32_t result = compression::manager::decompress(
                        decoded->data(), uncompressedSize, source, sourceSize);
                    if (result <= 0 || result > uncompressedSize) {
                        decoded.reset();
                    } else {
                        decoded->resize(static_cast<size_t>(result));
                    }
                }
            }
        } catch (...) {
            decoded.reset();
        }
        {
            std::scoped_lock lock(cache.mutex);
            cache.data = decoded;
            cache.state = decoded ? CompressedCacheState::Ready
                                  : CompressedCacheState::Failed;
            data = cache.data;
        }
        cache.ready.notify_all();
        if (!data) return false;
    }

    auto file = std::make_unique<ArchiveFile>();
    file->setSharedData(std::move(data), entry.filename);
    if (!file->open(entry.filename) || !file->isOpen()) return false;
    outFile = std::move(file);
    return true;
}

bool ArchiveFileSystem::exists(container::StringView filename) const
{
    return m_fileMap.contains(normalizePath(filename));
}

container::Vector<container::String> ArchiveFileSystem::getFileList(container::StringView pattern) const
{
    container::Vector<container::String> files;
    files.reserve(m_fileMap.size());

    if (pattern.empty())
    {
        for (auto& [name, _] : m_fileMap)
        {
            files.push_back(name);
        }
    }
    else
    {
        for (auto& [name, _] : m_fileMap)
        {
            if (matchesPattern(name, pattern))
            {
                files.push_back(name);
            }
        }
    }
    return files;
}

container::String ArchiveFileSystem::getName() const
{
    if (m_archives.empty()) return "ArchiveFileSystem (empty)";
    container::String name = "ArchiveFileSystem [";
    for (size_t i = 0; i < m_archives.size(); ++i)
    {
        if (i > 0) name += ", ";
        name += m_archives[i]->name;
    }
    name += "]";
    return name;
}

size_t ArchiveFileSystem::getFileCount() const
{
    return m_fileMap.size();
}

bool ArchiveFileSystem::entryComesFromArchive(
    container::StringView filename,
    container::StringView archiveName) const
{
    const auto found = m_fileMap.find(normalizePath(filename));
    if (found == m_fileMap.end() ||
        found->second.archiveIdx >= m_archives.size()) {
        return false;
    }
    return container::asciiEqualIgnoreCase(
        m_archives[found->second.archiveIdx]->name, archiveName);
}

std::optional<uint64_t> ArchiveFileSystem::entryLogicalSize(
    container::StringView filename) const
{
    const auto found = m_fileMap.find(normalizePath(filename));
    if (found == m_fileMap.end() ||
        found->second.archiveIdx >= m_archives.size()) {
        return std::nullopt;
    }
    const LoadedArchive& archive = *m_archives[found->second.archiveIdx];
    if (found->second.entryIdx >= archive.entries.size()) return std::nullopt;
    const ArchiveEntry& entry = archive.entries[found->second.entryIdx];
    if (!entry.compressed) return static_cast<uint64_t>(entry.size);
    if (entry.size > static_cast<uint32_t>(
            std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
    }
    const int32_t uncompressed = compression::manager::uncompressed_size(
        archive.data + entry.offset, static_cast<int32_t>(entry.size));
    return uncompressed > 0
        ? std::optional<uint64_t>{static_cast<uint64_t>(uncompressed)}
        : std::nullopt;
}

static uint32_t readU32(const uint8_t* data, bool bigEndian)
{
    if (bigEndian)
    {
        return (static_cast<uint32_t>(data[0]) << 24) |
               (static_cast<uint32_t>(data[1]) << 16) |
               (static_cast<uint32_t>(data[2]) << 8) |
               (static_cast<uint32_t>(data[3]));
    }
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool ArchiveFileSystem::parseArchive(LoadedArchive& archive)
{
    if (archive.dataSize < 12) return false;

    const auto* data = archive.data;

    bool isBig4 = (std::memcmp(data, "BIG4", 4) == 0);
    bool isBigF = (std::memcmp(data, "BIGF", 4) == 0);
    if (!isBig4 && !isBigF) return false;

    bool bigEndian = isBigF;

    uint32_t fileCount = readU32(data + 8, bigEndian);
    if (fileCount > kMaximumArchiveEntryCount) return false;

    uint32_t entryStart = isBigF ? 0x10 : 12;

    container::Vector<ArchiveEntry> tempEntries;
    tempEntries.reserve(fileCount);

    // All cursor arithmetic below is done in uint64_t.  The header fields are
    // attacker-controlled: with `offset` near UINT32_MAX the old `offset + 8 >
    // dataSize` test wrapped to a small value and passed, and `nameStart` /
    // `offset = nameEnd + 1` wrapped the same way — letting a crafted BIG build a
    // String from a pointer outside the mapping.  `fileCount` was already capped;
    // the per-entry offsets were not.
    uint64_t offset = entryStart;
    for (uint32_t i = 0; i < fileCount; ++i)
    {
        if (offset + 8u > archive.dataSize) return false;

        uint32_t fileOffset = readU32(data + offset, bigEndian);
        uint32_t fileSizeRaw = readU32(data + offset + 4, bigEndian);
        bool compressed = (fileSizeRaw & 0x80000000) != 0;
        uint32_t fileSize = fileSizeRaw & 0x7FFFFFFF;

        if (static_cast<uint64_t>(fileOffset) + fileSize > archive.dataSize)
            return false;

        uint64_t nameStart = offset + 8u;
        uint64_t nameEnd = nameStart;
        while (nameEnd < archive.dataSize && data[nameEnd] != 0) nameEnd++;
        if (nameEnd >= archive.dataSize) return false;

        container::String filename(reinterpret_cast<const char*>(data + nameStart),
                                   static_cast<size_t>(nameEnd - nameStart));
        if (filename.empty()) return false;

        std::replace(filename.begin(), filename.end(), '\\', '/');

        tempEntries.push_back({ filename, fileOffset, fileSize, compressed });
        offset = nameEnd + 1u;
    }

    // `offset` is bounded by dataSize (checked every iteration), so it fits the
    // 32-bit field; the cast is explicit to keep the narrowing intentional.
    archive.dataStartOffset = static_cast<uint32_t>(offset);
    archive.entries = std::move(tempEntries);
    archive.compressedCaches.clear();
    archive.compressedCaches.resize(archive.entries.size());
    for (size_t index = 0; index < archive.entries.size(); ++index) {
        if (archive.entries[index].compressed) {
            archive.compressedCaches[index] =
                std::make_unique<CompressedEntryCache>();
        }
    }

    return true;
}

container::String ArchiveFileSystem::normalizePath(container::StringView path)
{
    return virtual_path::canonical(path);
}

} // namespace io
