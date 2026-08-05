#pragma once

#include "VFS.h"
#include "core/container/container_types.h"
#include "core/container/hash_containers.h"
#include "core/io/LocaleResourceLocator.h"

#include <algorithm>
#include <cctype>

namespace game::ini {

[[nodiscard]] inline container::String canonicalLegacyIniPath(
    container::StringView value)
{
    container::String result(value);
    std::replace(result.begin(), result.end(), '\\', '/');
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    while (!result.empty() && result.back() == '/') result.pop_back();
    return result;
}

[[nodiscard]] inline bool hasLegacyIniExtension(container::StringView value)
{
    return value.size() >= 4 && value.substr(value.size() - 4) == ".ini";
}

// RefCode INI::loadFileDirectory(root) reads root.ini first, then sorted
// direct root/*.ini fragments, followed by sorted recursive descendants.
// Multiple roots retain their authored order; duplicate logical paths are
// emitted once. Each logical path is later opened through the VFS winner,
// matching INI::loadFileDirectory's file-instance-0 behavior.
[[nodiscard]] inline container::Vector<container::String>
enumerateLegacyIniDirectories(
    container::Span<const container::StringView> loadRoots)
{
    container::Vector<container::String> result;
    container::HashSet<container::String> emitted;
    auto& vfs = io::VFS::instance();
    const auto locator = io::acquireLocaleResourceLocator();

    for (const container::StringView rawRoot : loadRoots) {
        container::String root = canonicalLegacyIniPath(rawRoot);
        if (hasLegacyIniExtension(root)) root.resize(root.size() - 4);
        if (root.empty()) continue;

        const container::String rootFile = root + ".ini";
        const bool rootFileExists = locator
            ? locator->contains(rootFile)
            : vfs.exists(rootFile);
        if (rootFileExists && emitted.insert(rootFile).second) {
            result.push_back(rootFile);
        }

        const container::String prefix = root + '/';
        container::Vector<container::String> fragments;
        const container::Vector<container::String> candidates = locator
            ? locator->enumeratePrefix(prefix, ".ini")
            : vfs.getFileList(prefix + "*.ini");
        for (const container::String& rawPath : candidates) {
            const container::String path = canonicalLegacyIniPath(rawPath);
            if (!path.starts_with(prefix) || !hasLegacyIniExtension(path) ||
                path.size() == prefix.size()) {
                continue;
            }
            fragments.push_back(path);
        }
        std::sort(fragments.begin(), fragments.end(),
            [&prefix](const container::String& left,
                      const container::String& right) {
                const container::StringView leftRelative =
                    container::StringView{left}.substr(prefix.size());
                const container::StringView rightRelative =
                    container::StringView{right}.substr(prefix.size());
                const bool leftDirect =
                    leftRelative.find('/') == container::StringView::npos;
                const bool rightDirect =
                    rightRelative.find('/') == container::StringView::npos;
                if (leftDirect != rightDirect) return leftDirect;
                return left < right;
            });
        fragments.erase(
            std::unique(fragments.begin(), fragments.end()), fragments.end());
        for (container::String& path : fragments) {
            if (emitted.insert(path).second) result.push_back(std::move(path));
        }
    }
    return result;
}

} // namespace game::ini
