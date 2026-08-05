#pragma once

#include "container/container_types.h"
#include "container/hash_containers.h"

#include <cstdint>
#include <optional>

namespace io {

class VFS;

enum class LocaleResourceKind : uint8_t {
    Csf,
    Sound,
    Speech,
    Music,
    W3d,
    Texture,
};

// Frozen view of the mounted VFS winner set.  Candidate generation follows
// the active launch locale, while resolution is an exact hash lookup: no
// request scans the VFS and no basename can accidentally bind another asset.
class LocaleResourceLocator final {
public:
    [[nodiscard]] static container::SharedPtr<const LocaleResourceLocator>
    build(const VFS& vfs, container::StringView locale);

    [[nodiscard]] const container::String& localeDirectory() const noexcept {
        return m_localeDirectory;
    }
    [[nodiscard]] size_t indexedWinnerCount() const noexcept {
        return m_winningPaths.size();
    }

    [[nodiscard]] container::Vector<container::String> candidates(
        LocaleResourceKind kind, container::StringView authoredPath) const;
    [[nodiscard]] std::optional<container::String> resolve(
        LocaleResourceKind kind, container::StringView authoredPath) const;
    [[nodiscard]] bool contains(container::StringView path) const;
    // Enumerate exact canonical winners in lexical order. Prefix lookup is a
    // lower_bound range over the frozen winner projection; an optional suffix
    // filters only that range and never falls back to wildcard/basename scans.
    [[nodiscard]] container::Vector<container::String> enumeratePrefix(
        container::StringView prefix,
        container::StringView suffix = {}) const;

private:
    LocaleResourceLocator(container::String localeDirectory,
                          container::HashSet<container::String> winningPaths,
                          container::Vector<container::String> sortedWinningPaths);

    container::String m_localeDirectory;
    container::HashSet<container::String> m_winningPaths;
    container::Vector<container::String> m_sortedWinningPaths;
};

// FileSystemSubsystem publishes one immutable locator after the mount stack
// has been indexed.  Atomic shared ownership lets audio/resource workers keep
// a stable generation through shutdown without locks on the request path.
void publishLocaleResourceLocator(
    container::SharedPtr<const LocaleResourceLocator> locator) noexcept;
void clearLocaleResourceLocator() noexcept;
[[nodiscard]] container::SharedPtr<const LocaleResourceLocator>
acquireLocaleResourceLocator() noexcept;

} // namespace io
