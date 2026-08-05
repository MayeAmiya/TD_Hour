#pragma once

#include "core/container/container_types.h"

#include <cstdint>
namespace game {

enum class MapSourceKind : uint8_t {
    Unknown,
    Official,
    User,
};

// Produces the portable VFS identity used by map selection, launch
// descriptors and session loading.  It also accepts RefCode's
// `UserData/Maps/...` portable spelling and maps it to `user/maps/...`.
// Parent traversal is rejected rather than normalized across a source root.
[[nodiscard]] container::String canonicalMapSourcePath(
    container::StringView path);

[[nodiscard]] MapSourceKind classifyMapSourcePath(
    container::StringView path) noexcept;

// Content identity is intentionally a value type shared by map selection,
// terrain loading, replay validation and future network handshakes.  It names
// the VFS path that actually won lookup rather than pretending a UI map label
// is sufficient proof of byte identity.
struct MapContentIdentity final {
    container::String resolvedPath;
    uint32_t crc = 0;
    uint32_t size = 0;

    [[nodiscard]] bool isKnown() const noexcept { return !resolvedPath.empty(); }
    [[nodiscard]] MapSourceKind sourceKind() const noexcept {
        return classifyMapSourcePath(resolvedPath);
    }
};

[[nodiscard]] uint32_t crc32(container::Span<const uint8_t> bytes) noexcept;

// Computes an identity from bytes already selected by a caller.  This avoids
// a second VFS read in TerrainLogic, which must parse the exact same bytes.
[[nodiscard]] bool fingerprintMapBytes(container::Span<const uint8_t> bytes,
                                       container::StringView resolvedPath,
                                       MapContentIdentity& output);

// Selection/UI convenience for a normal VFS path.  The terrain session still
// uses fingerprintMapBytes after it has resolved legacy base-name fallback.
[[nodiscard]] bool fingerprintMapContent(container::StringView path,
                                         MapContentIdentity& output);

} // namespace game
