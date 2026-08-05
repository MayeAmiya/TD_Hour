#include "LocaleResourceLocator.h"

#include "VFS.h"
#include "VirtualPath.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>

namespace io {
namespace {

std::atomic<container::SharedPtr<const LocaleResourceLocator>> g_locator;

[[nodiscard]] container::String canonicalRelativePath(
    container::StringView value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }

    return virtual_path::canonical(value.substr(first, last - first));
}

[[nodiscard]] container::String canonicalRelativePrefix(
    container::StringView value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    container::String input{value.substr(first, last - first)};
    std::replace(input.begin(), input.end(), '\\', '/');
    const bool trailingSeparator = !input.empty() && input.back() == '/';
    if (trailingSeparator) input.pop_back();
    if (input.empty() || input.find('*') != container::String::npos ||
        input.find('?') != container::String::npos) {
        return {};
    }
    container::String result = canonicalRelativePath(input);
    if (!result.empty() && trailingSeparator) result.push_back('/');
    return result;
}

[[nodiscard]] container::String canonicalSuffix(container::StringView value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    container::String result{value.substr(first, last - first)};
    if (result.find('*') != container::String::npos ||
        result.find('?') != container::String::npos) {
        return {};
    }
    std::replace(result.begin(), result.end(), '\\', '/');
    std::transform(result.begin(), result.end(), result.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return result;
}

[[nodiscard]] container::String normalizeLocale(container::StringView locale) {
    container::String value;
    value.reserve(locale.size());
    for (const unsigned char character : locale) {
        if (std::isspace(character)) continue;
        value.push_back(character == '_'
            ? '-' : static_cast<char>(std::tolower(character)));
    }

    if (value == "zh" || value == "zh-cn" || value == "zh-hans" ||
        value == "chinese") return "chinese";
    if (value == "en" || value == "en-us" || value == "en-gb" ||
        value == "us" || value == "uk" || value == "english") {
        return "english";
    }
    if (value == "de" || value == "de-de" || value == "german") {
        return "german";
    }
    if (value == "fr" || value == "fr-fr" || value == "french") {
        return "french";
    }
    if (value == "es" || value == "es-es" || value == "spanish") {
        return "spanish";
    }
    if (value == "it" || value == "it-it" || value == "italian") {
        return "italian";
    }
    if (value == "ja" || value == "ja-jp" || value == "japanese") {
        return "japanese";
    }
    if (value == "ko" || value == "ko-kr" || value == "korean") {
        return "korean";
    }
    return value.empty() ? container::String{"english"} : value;
}

[[nodiscard]] bool knownLanguageDirectory(container::StringView value) {
    constexpr container::Array<container::StringView, 9> names{{
        "chinese", "english", "german", "french", "spanish", "italian",
        "japanese", "korean", "ukenglish"}};
    return std::find(names.begin(), names.end(), value) != names.end();
}

[[nodiscard]] container::String stripDataLanguagePrefix(
    container::String path) {
    constexpr container::StringView dataPrefix = "data/";
    if (!path.starts_with(dataPrefix)) return path;
    const size_t languageEnd = path.find('/', dataPrefix.size());
    if (languageEnd == container::String::npos) return path;
    const container::StringView language{
        path.data() + dataPrefix.size(), languageEnd - dataPrefix.size()};
    if (!knownLanguageDirectory(language)) return path;
    return path.substr(languageEnd + 1);
}

[[nodiscard]] container::String basename(container::StringView path) {
    const size_t slash = path.find_last_of('/');
    return container::String{slash == container::StringView::npos
        ? path : path.substr(slash + 1)};
}

[[nodiscard]] bool hasExtension(container::StringView path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    return dot != container::StringView::npos &&
        (slash == container::StringView::npos || dot > slash);
}

[[nodiscard]] container::String replaceExtension(
    container::String path, container::StringView extension) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot != container::String::npos &&
        (slash == container::String::npos || dot > slash)) {
        path.resize(dot);
    }
    path.append(extension);
    return path;
}

void appendUnique(container::Vector<container::String>& output,
                  container::String value) {
    if (value.empty() ||
        std::find(output.begin(), output.end(), value) != output.end()) {
        return;
    }
    output.push_back(std::move(value));
}

void appendTextureVariants(container::Vector<container::String>& output,
                           container::String path) {
    if (!hasExtension(path)) {
        appendUnique(output, path + ".dds");
        appendUnique(output, path + ".tga");
        return;
    }
    if (path.ends_with(".dds") || path.ends_with(".tga")) {
        // WW3D's DDSFileClass rewrites either authored extension to .dds and
        // tries the compressed asset before opening the TGA source. Preserve
        // that choice within each locale/directory layer; outer candidate
        // ordering still keeps an active-language TGA ahead of common DDS.
        appendUnique(output, replaceExtension(path, ".dds"));
        appendUnique(output, replaceExtension(std::move(path), ".tga"));
    } else {
        appendUnique(output, std::move(path));
    }
}

void appendTerrainTextureVariant(
    container::Vector<container::String>& output, container::String path) {
    if (!hasExtension(path)) {
        appendUnique(output, std::move(path) + ".tga");
        return;
    }
    // Terrain.ini names are authored TGA sources. Keep this path separate
    // from WW3D's ordinary DDS-first loader; an explicit .dds remains an
    // explicit request rather than being rewritten.
    appendUnique(output, std::move(path));
}

[[nodiscard]] container::String audioRoot(LocaleResourceKind kind) {
    switch (kind) {
    case LocaleResourceKind::Sound: return "data/audio/sounds/";
    case LocaleResourceKind::Speech: return "data/audio/speech/";
    case LocaleResourceKind::Music: return "data/audio/tracks/";
    default: break;
    }
    return {};
}

} // namespace

LocaleResourceLocator::LocaleResourceLocator(
    container::String localeDirectory,
    container::HashSet<container::String> winningPaths,
    container::Vector<container::String> sortedWinningPaths)
    : m_localeDirectory(std::move(localeDirectory)),
      m_winningPaths(std::move(winningPaths)),
      m_sortedWinningPaths(std::move(sortedWinningPaths)) {}

container::SharedPtr<const LocaleResourceLocator>
LocaleResourceLocator::build(const VFS& vfs, container::StringView locale) {
    container::Vector<container::String> sortedWinners;
    container::HashSet<container::String> winners;
    const container::Vector<container::String> files = vfs.getFileList();
    sortedWinners.reserve(files.size());
    winners.reserve(files.size());
    for (const container::String& file : files) {
        container::String canonical = canonicalRelativePath(file);
        if (!canonical.empty() && winners.insert(canonical).second) {
            sortedWinners.push_back(std::move(canonical));
        }
    }
    std::sort(sortedWinners.begin(), sortedWinners.end());
    return container::SharedPtr<const LocaleResourceLocator>(
        new LocaleResourceLocator(normalizeLocale(locale), std::move(winners),
                                  std::move(sortedWinners)));
}

container::Vector<container::String> LocaleResourceLocator::candidates(
    LocaleResourceKind kind, container::StringView authoredPath) const {
    container::Vector<container::String> result;
    container::String authored = canonicalRelativePath(authoredPath);
    if (authored.empty()) return result;

    const container::String activeDataPrefix =
        "data/" + m_localeDirectory + "/";
    const auto stripActiveLanguage = [&](container::String path) {
        if (path.starts_with(activeDataPrefix)) {
            return path.substr(activeDataPrefix.size());
        }
        return stripDataLanguagePrefix(std::move(path));
    };

    if (kind == LocaleResourceKind::Csf) {
        const container::String common = stripActiveLanguage(authored);
        const container::String file = basename(common);
        appendUnique(result, "data/" + m_localeDirectory + "/" + file);
        return result;
    }

    if (kind == LocaleResourceKind::Sound ||
        kind == LocaleResourceKind::Speech ||
        kind == LocaleResourceKind::Music) {
        const container::String root = audioRoot(kind);
        container::String common = authored;
        if (common.starts_with(root)) {
            container::String remainder = common.substr(root.size());
            const size_t slash = remainder.find('/');
            if (slash != container::String::npos &&
                (knownLanguageDirectory(
                     container::StringView{remainder}.substr(0, slash)) ||
                 container::StringView{remainder}.substr(0, slash) ==
                     m_localeDirectory)) {
                remainder.erase(0, slash + 1);
            }
            common = root + remainder;
        }
        const container::String file = basename(common);
        appendUnique(result, root + m_localeDirectory + "/" + file);
        appendUnique(result, common);
        if (!common.starts_with(root)) {
            appendUnique(result, root + file);
        }
        return result;
    }

    container::String common = stripActiveLanguage(authored);
    if (kind == LocaleResourceKind::W3d) {
        if (common.find('/') == container::String::npos) {
            common = "art/w3d/" + common;
        }
        if (!common.ends_with(".w3d")) common += ".w3d";
        appendUnique(result, "data/" + m_localeDirectory + "/" + common);
        appendUnique(result, common);
        appendUnique(result, "user/w3d/" + basename(common));
        // WW3D's second on-demand attempt prepends "..\\" to a filename.
        // After GameFileClass adds Art/W3D this is the constrained Art parent,
        // not arbitrary path traversal. Publish the normalized equivalents
        // explicitly so canonical identities can continue rejecting '..'.
        constexpr container::StringView w3dRoot = "art/w3d/";
        if (common.starts_with(w3dRoot)) {
            const container::String parent =
                "art/" + common.substr(w3dRoot.size());
            appendUnique(
                result, "data/" + m_localeDirectory + "/" + parent);
            appendUnique(result, parent);
        }
        return result;
    }

    if (kind == LocaleResourceKind::Texture) {
        container::Vector<container::String> bases;
        if (common.find('/') == container::String::npos) {
            bases.push_back("art/textures/" + common);
            bases.push_back(common);
        } else {
            bases.push_back(common);
        }
        const bool terrainTexture = common.starts_with("art/terrain/");
        // All active-language candidates precede every common candidate,
        // including extension alternatives. Terrain keeps its authored TGA
        // rule; ordinary WW3D textures retain the same-layer DDS → TGA rule.
        for (const container::String& base : bases) {
            if (terrainTexture) {
                appendTerrainTextureVariant(
                    result, "data/" + m_localeDirectory + "/" + base);
            } else {
                appendTextureVariants(
                    result, "data/" + m_localeDirectory + "/" + base);
            }
        }
        for (const container::String& base : bases) {
            if (terrainTexture) {
                appendTerrainTextureVariant(result, base);
            } else {
                appendTextureVariants(result, base);
            }
        }
        const container::String file = basename(common);
        if (terrainTexture) {
            appendTerrainTextureVariant(result, "user/textures/" + file);
        } else {
            appendTextureVariants(result, "user/textures/" + file);
        }
        // RefCode's MapPreviews fallback is intentionally TGA-only. It is
        // last, after both installed and general UserData texture candidates.
        appendUnique(result, "user/mappreviews/" +
            replaceExtension(file, ".tga"));
    }
    return result;
}

std::optional<container::String> LocaleResourceLocator::resolve(
    LocaleResourceKind kind, container::StringView authoredPath) const {
    for (container::String& candidate : candidates(kind, authoredPath)) {
        if (m_winningPaths.contains(candidate)) return std::move(candidate);
    }
    return std::nullopt;
}

bool LocaleResourceLocator::contains(container::StringView path) const {
    const container::String canonical = canonicalRelativePath(path);
    return !canonical.empty() && m_winningPaths.contains(canonical);
}

container::Vector<container::String> LocaleResourceLocator::enumeratePrefix(
    container::StringView prefix, container::StringView suffix) const {
    container::Vector<container::String> result;
    const container::String canonicalPrefix = canonicalRelativePrefix(prefix);
    if (canonicalPrefix.empty()) return result;
    const container::String canonicalRequiredSuffix = canonicalSuffix(suffix);
    if (!suffix.empty() && canonicalRequiredSuffix.empty()) return result;

    auto current = std::lower_bound(
        m_sortedWinningPaths.begin(), m_sortedWinningPaths.end(),
        container::StringView{canonicalPrefix},
        [](const container::String& path, container::StringView sought) {
            return path < sought;
        });
    while (current != m_sortedWinningPaths.end() &&
           current->starts_with(canonicalPrefix)) {
        if (canonicalRequiredSuffix.empty() ||
            current->ends_with(canonicalRequiredSuffix)) {
            result.push_back(*current);
        }
        ++current;
    }
    return result;
}

void publishLocaleResourceLocator(
    container::SharedPtr<const LocaleResourceLocator> locator) noexcept {
    g_locator.store(std::move(locator), std::memory_order_release);
}

void clearLocaleResourceLocator() noexcept {
    g_locator.store(container::SharedPtr<const LocaleResourceLocator>{},
                    std::memory_order_release);
}

container::SharedPtr<const LocaleResourceLocator>
acquireLocaleResourceLocator() noexcept {
    return g_locator.load(std::memory_order_acquire);
}

} // namespace io
