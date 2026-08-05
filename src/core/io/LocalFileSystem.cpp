#include "container/container_types.h"
#include "LocalFileSystem.h"
#include "LocalFile.h"
#include "VirtualPath.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace io {

namespace {

container::String normalizePath(container::String value)
{
    return virtual_path::canonical(value);
}

bool matchesPattern(container::StringView filename, container::StringView pattern)
{
    if (pattern.empty()) return true;

    const container::String name = normalizePath(container::String(filename));
    const container::String pat = virtual_path::pattern(pattern);
    if (pat.empty()) return false;

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

LocalFileSystem::LocalFileSystem(container::StringView basePath)
    : m_basePath(basePath)
{
}

LocalFileSystem::LocalFileSystem(container::StringView basePath, container::StringView virtualRoot)
    : m_basePath(basePath)
    , m_virtualRoot(normalizePath(container::String(virtualRoot)))
    , m_virtualRootValid(virtualRoot.empty() || !m_virtualRoot.empty())
{
    while (!m_virtualRoot.empty() && m_virtualRoot.back() == '/') {
        m_virtualRoot.pop_back();
    }
}

bool LocalFileSystem::open(container::StringView filename, container::UniquePtr<File>& outFile,
                            FileAccess access)
{
    if (!acceptsVirtualPath(filename)) return false;

    auto fullPath = resolvePath(filename);
    if (access != FileAccess::Read) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(fullPath).parent_path(), ec);
    }
    auto file = std::make_unique<LocalFile>();
    if (file->open(fullPath, access))
    {
        outFile = std::move(file);
        return true;
    }
    return false;
}

bool LocalFileSystem::exists(container::StringView filename) const
{
    if (!acceptsVirtualPath(filename)) return false;

    auto fullPath = resolvePath(filename);
    struct _stat64 statBuf;
    return _stat64(fullPath.c_str(), &statBuf) == 0;
}

container::Vector<container::String> LocalFileSystem::getFileList(container::StringView pattern) const
{
    container::Vector<container::String> files;
    if (!m_virtualRootValid) return files;

    std::filesystem::path root = m_basePath.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(m_basePath);

    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return files;

    const container::String normalizedPattern = pattern.empty()
        ? container::String{} : virtual_path::pattern(pattern);
    if (!pattern.empty() && normalizedPattern.empty()) return files;
    for (auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        const container::String virtualPath = toVirtualPath(entry.path());
        if (matchesPattern(virtualPath, normalizedPattern)) {
            files.push_back(virtualPath);
        }
    }
    return files;
}

container::String LocalFileSystem::resolvePath(container::StringView filename) const
{
    const container::String localName = stripVirtualRoot(filename);
    if (m_basePath.empty()) return localName;
    return (std::filesystem::path(m_basePath) / localName).string();
}

container::String LocalFileSystem::toVirtualPath(const std::filesystem::path& path) const
{
    std::filesystem::path root = m_basePath.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(m_basePath);

    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    container::String value = virtual_path::canonical(
        ec ? path.filename().string() : relative.string());
    if (value.empty()) return {};

    if (m_virtualRoot.empty()) return value;
    return m_virtualRoot + "/" + value;
}

bool LocalFileSystem::acceptsVirtualPath(container::StringView filename) const
{
    if (!m_virtualRootValid) return false;
    if (filename.empty()) return false;
    const container::String value = virtual_path::canonical(filename);
    if (value.empty()) return false;
    if (m_virtualRoot.empty()) return true;
    if (value == m_virtualRoot) return true;
    const container::String prefix = m_virtualRoot + "/";
    return value.rfind(prefix, 0) == 0;
}

container::String LocalFileSystem::stripVirtualRoot(container::StringView filename) const
{
    container::String value = virtual_path::canonical(filename);
    if (value.empty()) return {};
    if (m_virtualRoot.empty()) return value;

    if (value == m_virtualRoot) return {};
    const container::String prefix = m_virtualRoot + "/";
    if (value.rfind(prefix, 0) == 0) {
        return value.substr(prefix.size());
    }
    return value;
}

} // namespace io
