#include "core/container/container_types.h"
#include "game/base/MapContentIdentity.h"

#include "VFS.h"
#include "core/io/VirtualPath.h"

#include <algorithm>
#include <cctype>
#include <limits>
namespace game {

container::String canonicalMapSourcePath(container::StringView path) {
    container::String canonical = io::virtual_path::canonical(path);

    constexpr container::StringView legacyUserRoot = "userdata/maps";
    if (canonical == legacyUserRoot) return "user/maps";
    if (canonical.starts_with(container::String{legacyUserRoot} + "/")) {
        canonical.replace(0, legacyUserRoot.size(), "user/maps");
    }
    return canonical;
}

MapSourceKind classifyMapSourcePath(container::StringView path) noexcept {
    // Identities produced by this module are already canonical.  Keep this
    // noexcept classifier allocation-free for save/replay comparison paths.
    const auto startsAtRoot = [path](container::StringView root) {
        if (path.size() < root.size()) return false;
        for (size_t index = 0; index < root.size(); ++index) {
            const unsigned char authored =
                static_cast<unsigned char>(path[index]);
            const char value = authored == '\\' ? '/' :
                static_cast<char>(std::tolower(authored));
            if (value != root[index]) return false;
        }
        return path.size() == root.size() || path[root.size()] == '/' ||
            path[root.size()] == '\\';
    };
    if (startsAtRoot("user/maps") || startsAtRoot("userdata/maps")) {
        return MapSourceKind::User;
    }
    if (startsAtRoot("maps")) return MapSourceKind::Official;
    return MapSourceKind::Unknown;
}

uint32_t crc32(container::Span<const uint8_t> bytes) noexcept {
    uint32_t value = 0xFFFFFFFFu;
    for (const uint8_t byte : bytes) {
        value ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1u) ^ ((value & 1u) != 0u ? 0xEDB88320u : 0u);
        }
    }
    return ~value;
}

bool fingerprintMapBytes(container::Span<const uint8_t> bytes, container::StringView resolvedPath,
                         MapContentIdentity& output) {
    output = {};
    container::String canonicalPath = canonicalMapSourcePath(resolvedPath);
    if (canonicalPath.empty() ||
        bytes.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    output = {
        .resolvedPath = std::move(canonicalPath),
        .crc = crc32(bytes),
        .size = static_cast<uint32_t>(bytes.size()),
    };
    return true;
}

bool fingerprintMapContent(container::StringView path, MapContentIdentity& output) {
    const container::String canonicalPath = canonicalMapSourcePath(path);
    if (canonicalPath.empty()) {
        output = {};
        return false;
    }
    container::Vector<uint8_t> bytes;
    if (!io::VFS::instance().readToBuffer(canonicalPath, bytes)) {
        output = {};
        return false;
    }
    return fingerprintMapBytes(bytes, canonicalPath, output);
}

} // namespace game
