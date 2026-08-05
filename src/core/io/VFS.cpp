#include "container/container_types.h"
#include "VFS.h"
#include "RAMFile.h"
#include "VirtualPath.h"
#include "container/hash_containers.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>

namespace io {
namespace {

container::String canonicalVirtualPath(container::StringView path)
{
    return virtual_path::canonical(path);
}

bool matchesPattern(container::StringView filename, container::StringView pattern)
{
    if (pattern.empty()) return true;

    const container::String name = canonicalVirtualPath(filename);
    const container::String pat = virtual_path::pattern(pattern);
    if (pat.empty()) return false;
    const bool hasWildcard = pat.find('*') != container::String::npos ||
        pat.find('?') != container::String::npos;
    if (!hasWildcard) return name == pat;

    size_t namePos = 0;
    size_t patternPos = 0;
    size_t starPos = container::String::npos;
    size_t matchPos = 0;
    while (namePos < name.size()) {
        if (patternPos < pat.size() &&
            (pat[patternPos] == '?' || pat[patternPos] == name[namePos])) {
            ++namePos;
            ++patternPos;
        } else if (patternPos < pat.size() && pat[patternPos] == '*') {
            starPos = patternPos++;
            matchPos = namePos;
        } else if (starPos != container::String::npos) {
            patternPos = starPos + 1;
            namePos = ++matchPos;
        } else {
            return false;
        }
    }
    while (patternPos < pat.size() && pat[patternPos] == '*') ++patternPos;
    return patternPos == pat.size();
}

[[nodiscard]] bool isDirectW3dTexturePath(
    container::StringView canonicalPath) noexcept {
    constexpr container::StringView root = "art/textures/";
    if (!canonicalPath.starts_with(root)) return false;
    const container::StringView filename = canonicalPath.substr(root.size());
    if (filename.empty() || filename.find('/') != container::StringView::npos) {
        return false;
    }
    return filename.ends_with(".dds") || filename.ends_with(".tga");
}

void applyZeroHourTextureSizeOverride(
    container::StringView canonicalPath, auto& layers) {
    if (!isDirectW3dTexturePath(canonicalPath) || layers.size() < 2u) return;
    auto* zeroHour = dynamic_cast<ArchiveFileSystem*>(layers[0].fs);
    auto* generals = dynamic_cast<ArchiveFileSystem*>(layers[1].fs);
    if (!zeroHour || !generals ||
        !zeroHour->entryComesFromArchive(
            layers[0].path, "TexturesZH.big") ||
        !generals->entryComesFromArchive(
            layers[1].path, "Textures.big")) {
        return;
    }
    const std::optional<uint64_t> zeroHourSize =
        zeroHour->entryLogicalSize(layers[0].path);
    const std::optional<uint64_t> generalsSize =
        generals->entryLogicalSize(layers[1].path);
    if (zeroHourSize && generalsSize && *zeroHourSize < *generalsSize) {
        std::swap(layers[0], layers[1]);
    }
}

} // namespace

VFS& VFS::instance()
{
    static VFS inst;
    return inst;
}

void VFS::mountLocal(container::StringView basePath, int priority)
{
    std::scoped_lock lock(m_indexMutex);
    m_layers.push_back({
        std::make_unique<LocalFileSystem>(basePath),
        priority,
        m_nextMountSequence++
    });
    sortLayers();
    invalidateIndexLocked();
}

void VFS::mountLocal(container::StringView basePath, container::StringView virtualRoot, int priority)
{
    std::scoped_lock lock(m_indexMutex);
    m_layers.push_back({
        std::make_unique<LocalFileSystem>(basePath, virtualRoot),
        priority,
        m_nextMountSequence++
    });
    sortLayers();
    invalidateIndexLocked();
}

bool VFS::mountArchive(container::StringView archivePath, int priority)
{
    auto archiveFs = std::make_unique<ArchiveFileSystem>();
    if (!archiveFs->loadArchive(archivePath))
    {
        return false;
    }
    {
        std::scoped_lock lock(m_indexMutex);
        m_layers.push_back({
            std::move(archiveFs),
            priority,
            m_nextMountSequence++
        });
        sortLayers();
        invalidateIndexLocked();
    }
    return true;
}

bool VFS::mountArchiveFromData(container::StringView name, const uint8_t* data, size_t dataSize, int priority)
{
    auto archiveFs = std::make_unique<ArchiveFileSystem>();
    if (!archiveFs->loadArchiveFromData(name, data, dataSize))
    {
        return false;
    }
    {
        std::scoped_lock lock(m_indexMutex);
        m_layers.push_back({
            std::move(archiveFs),
            priority,
            m_nextMountSequence++
        });
        sortLayers();
        invalidateIndexLocked();
    }
    return true;
}

void VFS::rebuildIndex()
{
    std::scoped_lock lock(m_indexMutex);
    ++m_contentRevision;
    if (m_contentRevision == 0) m_contentRevision = 1;
    m_indexDirty = true;
    ensureIndexLocked();
}

size_t VFS::indexedFileCount() const
{
    std::scoped_lock lock(m_indexMutex);
    ensureIndexLocked();
    return m_winningFiles.size();
}

uint64_t VFS::contentRevision() const
{
    std::scoped_lock lock(m_indexMutex);
    return m_contentRevision;
}

bool VFS::open(container::StringView filename, container::UniquePtr<File>& outFile, FileAccess access)
{
    if (access != FileAccess::Read) {
        std::scoped_lock lock(m_indexMutex);
        // The read winner may be an archive, but writes belong to the strongest
        // local mount whose virtual root covers the requested path. m_layers is
        // already ordered strongest-first, so do not let file existence in a
        // read-only layer participate in write routing.
        for (auto& layer : m_layers) {
            auto* local = dynamic_cast<LocalFileSystem*>(layer.fs.get());
            if (!local || !local->acceptsVirtualPath(filename)) continue;

            const bool opened = local->open(filename, outFile, access);
            if (!opened) continue;
            invalidateIndexLocked();
            return true;
        }
        return false;
    }

    container::Vector<IndexedFileLayer> indexedLayers;
    container::Vector<FileSystem*> fallbackLayers;
    {
        std::scoped_lock lock(m_indexMutex);
        ensureIndexLocked();
        const auto indexed = m_fileLayers.find(canonicalVirtualPath(filename));
        if (indexed != m_fileLayers.end() && !indexed->second.empty()) {
            indexedLayers = indexed->second;
        } else {
            fallbackLayers.reserve(m_layers.size());
            for (const auto& layer : m_layers)
                fallbackLayers.push_back(layer.fs.get());
        }
    }
    // Archive open may allocate/decompress and local open may enter the OS.
    // Both happen outside the global immutable index mutex.
    if (!indexedLayers.empty()) {
        const IndexedFileLayer& winner = indexedLayers.front();
        if (winner.fs && winner.fs->open(winner.path, outFile, access)) {
            return true;
        }

        // ZH checks the active loose file system first and continues into the
        // archive file system only when that loose file can no longer be
        // opened.  Keep that narrow fallback for an index which became stale
        // after a loose file was removed or made inaccessible.  A readable
        // but invalid loose resource remains authoritative, and one broken
        // archive never falls through to another archive.
        if (!dynamic_cast<LocalFileSystem*>(winner.fs)) return false;
        for (size_t index = 1; index < indexedLayers.size(); ++index) {
            const IndexedFileLayer& candidate = indexedLayers[index];
            if (!dynamic_cast<ArchiveFileSystem*>(candidate.fs)) continue;
            return candidate.fs->open(candidate.path, outFile, access);
        }
        return false;
    }

    // Loose save/replay/editor files may appear after startup. Keep that
    // dynamic path as a bounded fallback without penalizing normal content
    // lookups with a scan of every mounted layer.
    for (FileSystem* layer : fallbackLayers)
    {
        if (layer && layer->exists(filename))
        {
            const bool opened = layer->open(filename, outFile, access);
            if (opened) {
                std::scoped_lock lock(m_indexMutex);
                invalidateIndexLocked();
            }
            return opened;
        }
    }
    return false;
}

bool VFS::exists(container::StringView filename) const
{
    container::Vector<FileSystem*> fallbackLayers;
    {
        std::scoped_lock lock(m_indexMutex);
        ensureIndexLocked();
        if (m_fileLayers.contains(canonicalVirtualPath(filename))) return true;
        fallbackLayers.reserve(m_layers.size());
        for (const auto& layer : m_layers)
            fallbackLayers.push_back(layer.fs.get());
    }

    for (FileSystem* layer : fallbackLayers)
    {
        if (layer && layer->exists(filename))
        {
            std::scoped_lock lock(m_indexMutex);
            m_indexDirty = true;
            return true;
        }
    }
    return false;
}

container::Vector<container::String> VFS::getFileList(container::StringView pattern) const
{
    std::scoped_lock lock(m_indexMutex);
    ensureIndexLocked();
    if (pattern.empty()) return m_winningFiles;

    const container::String cacheKey = virtual_path::pattern(pattern);
    if (cacheKey.empty()) return {};
    if (const auto cached = m_fileListCache.find(cacheKey);
        cached != m_fileListCache.end()) {
        return cached->second;
    }

    container::Vector<container::String> files;
    const size_t wildcard = cacheKey.find('*');
    const bool isSingleStarPrefixPattern = wildcard != container::String::npos &&
        wildcard != 0 && cacheKey.find('?', 0) == container::String::npos &&
        cacheKey.find('*', wildcard + 1) == container::String::npos;
    if (isSingleStarPrefixPattern) {
        const container::StringView prefix{cacheKey.data(), wildcard};
        auto current = std::lower_bound(
            m_winningFilesByCanonical.begin(),
            m_winningFilesByCanonical.end(), prefix,
            [](const auto& entry, container::StringView sought) {
                return entry.first < sought;
            });
        container::Vector<size_t> winnerIndices;
        while (current != m_winningFilesByCanonical.end() &&
               current->first.starts_with(prefix)) {
            if (matchesPattern(current->first, cacheKey)) {
                winnerIndices.push_back(current->second);
            }
            ++current;
        }
        std::sort(winnerIndices.begin(), winnerIndices.end());
        files.reserve(winnerIndices.size());
        for (const size_t index : winnerIndices) {
            files.push_back(m_winningFiles[index]);
        }
    } else {
        for (const container::String& file : m_winningFiles) {
            if (matchesPattern(file, pattern)) files.push_back(file);
        }
    }
    m_fileListCache.emplace(cacheKey, files);
    return files;
}

container::String VFS::readAll(container::StringView filename)
{
    container::UniquePtr<File> file;
    if (!open(filename, file)) return {};

    container::String result;
    const auto fileSize = file->size();
    if (fileSize < 0) return {};
    if (fileSize > 0)
    {
        result.resize(static_cast<size_t>(fileSize));
        if (file->read(result.data(), result.size()) != result.size()) {
            return {};
        }
    }
    return result;
}

container::Vector<container::String> VFS::readAllLayers(container::StringView filename)
{
    container::Vector<container::String> results;
    container::Vector<IndexedFileLayer> layers;

    {
        std::scoped_lock lock(m_indexMutex);
        ensureIndexLocked();
        const auto indexed = m_fileLayers.find(canonicalVirtualPath(filename));
        if (indexed == m_fileLayers.end()) return results;
        layers = indexed->second;
    }

    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
    {
        if (!it->fs) continue;

        container::UniquePtr<File> file;
        if (!it->fs->open(it->path, file)) continue;

        container::String content;
        const auto fileSize = file->size();
        if (fileSize < 0) continue;
        if (fileSize > 0)
        {
            content.resize(static_cast<size_t>(fileSize));
            if (file->read(content.data(), content.size()) != content.size()) {
                continue;
            }
        }
        results.push_back(std::move(content));
    }

    return results;
}

bool VFS::readToBuffer(container::StringView filename, container::Vector<uint8_t>& buffer)
{
    buffer.clear();
    container::UniquePtr<File> file;
    if (!open(filename, file)) return false;

    const auto fileSize = file->size();
    if (fileSize < 0) return false;
    if (fileSize > 0)
    {
        buffer.resize(static_cast<size_t>(fileSize));
        if (file->read(buffer.data(), buffer.size()) != buffer.size()) {
            buffer.clear();
            return false;
        }
    }
    return true;
}

bool VFS::writeAll(container::StringView filename, container::StringView content)
{
    container::UniquePtr<File> file;
    if (!open(filename, file, FileAccess::Write)) return false;
    if (!content.empty()) {
        return file->write(content.data(), content.size()) == content.size();
    }
    return true;
}

bool VFS::writeBuffer(container::StringView filename, const container::Vector<uint8_t>& buffer)
{
    container::UniquePtr<File> file;
    if (!open(filename, file, FileAccess::Write)) return false;
    if (!buffer.empty()) {
        return file->write(buffer.data(), buffer.size()) == buffer.size();
    }
    return true;
}

bool VFS::remove(container::StringView filename)
{
    std::scoped_lock lock(m_indexMutex);
    for (auto& layer : m_layers)
    {
        auto* local = dynamic_cast<LocalFileSystem*>(layer.fs.get());
        if (!local || !local->exists(filename)) continue;

        std::error_code ec;
        const bool removed = std::filesystem::remove(local->resolvePath(filename), ec);
        if (removed) invalidateIndexLocked();
        return removed;
    }
    return false;
}

void VFS::sortLayers()
{
    std::sort(m_layers.begin(), m_layers.end(),
        [](const FileSystemLayer& a, const FileSystemLayer& b)
        {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.mountSequence > b.mountSequence;
        });
}

void VFS::invalidateIndexLocked() noexcept
{
    ++m_contentRevision;
    if (m_contentRevision == 0) m_contentRevision = 1;
    m_indexDirty = true;
    m_fileListCache.clear();
}

void VFS::ensureIndexLocked() const
{
    if (!m_indexDirty) return;

    m_fileLayers.clear();
    m_winningFiles.clear();
    m_winningFilesByCanonical.clear();
    m_fileListCache.clear();

    // Preserve the old observable ordering: strongest layer first, with each
    // layer sorted canonically, and only the first occurrence emitted as the
    // winning logical file. Coverage vectors retain the same strong-to-weak
    // order so readAllLayers can replay them in authored base-to-override
    // order.
    for (const FileSystemLayer& layer : m_layers) {
        container::Vector<container::String> files = layer.fs->getFileList();
        std::sort(files.begin(), files.end(), [](const container::String& left,
                                                 const container::String& right) {
            const container::String leftKey = canonicalVirtualPath(left);
            const container::String rightKey = canonicalVirtualPath(right);
            return leftKey == rightKey ? left < right : leftKey < rightKey;
        });
        for (container::String& file : files) {
            const container::String key = canonicalVirtualPath(file);
            if (key.empty()) continue;
            auto [record, inserted] = m_fileLayers.try_emplace(key);
            record->second.push_back({layer.fs.get(), file});
            if (inserted) {
                const size_t winnerIndex = m_winningFiles.size();
                m_winningFiles.push_back(std::move(file));
                m_winningFilesByCanonical.emplace_back(key, winnerIndex);
            }
        }
    }
    for (auto& [canonicalPath, layers] : m_fileLayers) {
        applyZeroHourTextureSizeOverride(canonicalPath, layers);
    }
    std::sort(m_winningFilesByCanonical.begin(),
              m_winningFilesByCanonical.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
    m_indexDirty = false;
}

} // namespace io
