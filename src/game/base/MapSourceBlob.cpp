#include "game/base/MapSourceBlob.h"

#include "VFS.h"

#include <algorithm>

namespace game {
namespace {

[[nodiscard]] container::String mapBaseName(container::StringView path) {
    container::String result = canonicalMapSourcePath(path);
    const size_t slash = result.find_last_of("/\\");
    if (slash != container::String::npos) result.erase(0, slash + 1u);
    const size_t dot = result.find_last_of('.');
    if (dot != container::String::npos) result.erase(dot);
    return result;
}

struct CandidateSource final {
    MapContentIdentity identity;
    container::Vector<uint8_t> bytes;
};

[[nodiscard]] bool readCandidate(container::StringView candidate,
                                 CandidateSource& output,
                                 container::String& loadError,
                                 bool& wasReadable) {
    wasReadable = false;
    output = {};
    if (!io::VFS::instance().readToBuffer(candidate, output.bytes)) {
        loadError = "Failed to read map: " + container::String(candidate);
        return false;
    }
    wasReadable = true;
    if (output.bytes.empty()) {
        loadError = "Map is empty: " + container::String(candidate);
        return false;
    }

    if (!fingerprintMapBytes(output.bytes, candidate, output.identity)) {
        loadError = "Map exceeds the portable 32-bit content identity limit: " +
            container::String(candidate);
        return false;
    }
    return true;
}

} // namespace

bool loadMapSourceBlob(container::StringView requestedPath,
                       MapSourceHandle& output,
                       container::String* error) {
    output.reset();
    container::String loadError;
    CandidateSource candidateSource;

    bool primaryWasReadable = false;
    const container::String canonicalRequest =
        canonicalMapSourcePath(requestedPath);
    if (canonicalRequest.empty()) {
        if (error) *error = "Invalid map source path";
        return false;
    }
    if (readCandidate(canonicalRequest, candidateSource, loadError,
                      primaryWasReadable)) {
        container::SharedPtr<MapSourceBlob> blob(new MapSourceBlob{});
        blob->m_identity = std::move(candidateSource.identity);
        blob->m_bytes = std::move(candidateSource.bytes);
        output = std::move(blob);
        if (error) error->clear();
        return true;
    }

    // A readable source, including an empty/oversized one, is authoritative.
    // Terrain format validation occurs after this factory and must not cause a
    // fallback to a vaguely matching map.
    if (!primaryWasReadable && canonicalRequest.find('/') == container::String::npos) {
        const container::String requestedBase = mapBaseName(canonicalRequest);
        container::Vector<container::String> candidates;
        for (const container::String& candidate :
             io::VFS::instance().getFileList("*.map")) {
            const container::String canonicalCandidate =
                canonicalMapSourcePath(candidate);
            if (classifyMapSourcePath(canonicalCandidate) !=
                    MapSourceKind::Unknown &&
                mapBaseName(canonicalCandidate) == requestedBase) {
                candidates.push_back(canonicalCandidate);
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const container::String& lhs,
                     const container::String& rhs) {
                      return lhs < rhs;
                  });
        candidates.erase(
            std::unique(candidates.begin(), candidates.end()), candidates.end());

        if (candidates.size() > 1) {
            loadError = "Ambiguous legacy map basename '" +
                canonicalRequest + "'; use maps/... or user/maps/...";
        } else for (const container::String& candidate : candidates) {
            bool candidateWasReadable = false;
            if (readCandidate(candidate, candidateSource, loadError,
                              candidateWasReadable)) {
                container::SharedPtr<MapSourceBlob> blob(new MapSourceBlob{});
                blob->m_identity = std::move(candidateSource.identity);
                blob->m_bytes = std::move(candidateSource.bytes);
                output = std::move(blob);
                if (error) error->clear();
                return true;
            }
            if (candidateWasReadable) break;
        }
    }

    if (error) *error = std::move(loadError);
    return false;
}

} // namespace game
