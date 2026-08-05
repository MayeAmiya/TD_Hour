#include "core/container/hash_containers.h"
#include "MappedImageCollection.h"
#include "io/VFS.h"
#include "io/LocaleResourceLocator.h"
#include "debug/debug.h"
#include "core/constants/Paths.h"
#include <charconv>
#include <sstream>
#include <algorithm>
#include <cctype>
namespace engine {

container::String MappedImageCollection::toLowerStr(const container::String& s) {
    container::String result = s;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

static container::String trimStr(const container::String& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == container::String::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static container::String normalizePath(container::String path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path;
}

static bool parseIntStrict(container::StringView text, int32_t& value) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == container::StringView::npos) return false;
    const size_t last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1u);
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

void MappedImageCollection::load() {
    clearSession();
    m_images.clear();
    auto& vfs = io::VFS::instance();
    container::HashSet<container::String> loadedFiles;
    size_t fileCount = 0;

    auto loadDirectory = [&](container::StringView directory) {
        container::String prefix(directory);
        std::replace(prefix.begin(), prefix.end(), '\\', '/');
        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        prefix.push_back('/');
        const auto locator = io::acquireLocaleResourceLocator();
        auto files = locator
            ? locator->enumeratePrefix(prefix, ".ini")
            : vfs.getFileList(prefix + "*.ini");
        std::sort(files.begin(), files.end(), [](const container::String& a, const container::String& b) {
            return normalizePath(a) < normalizePath(b);
        });

        for (const auto& f : files) {
            container::String normalized = normalizePath(f);
            if (!loadedFiles.insert(normalized).second) {
                continue;
            }

            container::String content = vfs.readAll(f);
            if (!content.empty()) {
                size_t before = m_images.size();
                parseINIData(content, m_images, &m_images);
                size_t parsed = m_images.size() - before;
                fileCount++;
                if (parsed > 0) {
                    TD_LOG_INFO("[MappedImageCollection]   {} -> {} images", f, parsed);
                }
            }
        }
    };

    // RefCode loads user-authored mapped images before the two installed
    // collections. userRoot is mounted at the VFS root, so its physical
    // UserData/INI/MappedImages directory is addressed as ini/mappedimages.
    loadDirectory("ini/mappedimages");
    loadDirectory("data/ini/mappedimages/texturesize_512");
    loadDirectory("data/ini/mappedimages/handcreated");

    TD_LOG_INFO("[MappedImageCollection] Loaded {} images from {} INI files", m_images.size(), fileCount);
}

void MappedImageCollection::parseINIData(
    const container::String& data, ImageMap& destination,
    const ImageMap* fallback) {
    std::istringstream stream(data);
    container::String line;

    while (std::getline(stream, line)) {
        container::String trimmed = trimStr(line);

        // Skip comments and empty lines
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '/' || trimmed[0] == '#')
            continue;

        // Check for "MappedImage <name>" or "Image <name>"
        container::String lower = toLowerStr(trimmed);
        if (lower.substr(0, 12) == "mappedimage " || lower.substr(0, 6) == "image ") {
            size_t spacePos = trimmed.find(' ');
            if (spacePos == container::String::npos) continue;

            const container::String name = trimStr(trimmed.substr(spacePos + 1));
            const container::String nameKey = toLowerStr(name);
            auto existing = destination.find(nameKey);
            const MappedImage* inherited =
                existing != destination.end() && existing->second
                ? existing->second.get() : nullptr;
            if (!inherited && fallback) {
                const auto base = fallback->find(nameKey);
                if (base != fallback->end() && base->second) {
                    inherited = base->second.get();
                }
            }
            MappedImage img = inherited ? *inherited : MappedImage{};
            img.name = name;

            // Parse fields until "End"
            while (std::getline(stream, line)) {
                container::String fieldLine = trimStr(line);
                container::String fieldLower = toLowerStr(fieldLine);

                if (fieldLower == "end") break;
                if (fieldLower.empty() || fieldLower[0] == ';') continue;

                // Parse "Key = Value" or "Key Value"
                container::String key, value;
                size_t eqPos = fieldLine.find('=');
                if (eqPos != container::String::npos) {
                    key = trimStr(fieldLine.substr(0, eqPos));
                    value = trimStr(fieldLine.substr(eqPos + 1));
                } else {
                    std::istringstream iss(fieldLine);
                    iss >> key;
                    std::getline(iss, value);
                    value = trimStr(value);
                }

                container::String keyLower = toLowerStr(key);

                if (keyLower == "texture") {
                    img.textureFile = value;
                } else if (keyLower == "texturewidth") {
                    int32_t parsed = 0;
                    if (parseIntStrict(value, parsed)) img.textureWidth = parsed;
                } else if (keyLower == "textureheight") {
                    int32_t parsed = 0;
                    if (parseIntStrict(value, parsed)) img.textureHeight = parsed;
                } else if (keyLower == "coords") {
                    // Format: Left:N Top:N Right:N Bottom:N
                    container::String coordsLower = toLowerStr(value);
                    auto findCoord = [&](const container::String& name,
                                         int32_t fallback) -> int32_t {
                        size_t pos = coordsLower.find(name);
                        if (pos == container::String::npos) return fallback;
                        size_t colonPos = coordsLower.find(':', pos);
                        if (colonPos == container::String::npos) return fallback;
                        size_t begin = coordsLower.find_first_not_of(
                            " \t", colonPos + 1u);
                        if (begin == container::String::npos) return fallback;
                        size_t end = begin;
                        if (coordsLower[end] == '+' || coordsLower[end] == '-') ++end;
                        while (end < coordsLower.size() &&
                               std::isdigit(static_cast<unsigned char>(coordsLower[end]))) {
                            ++end;
                        }
                        int32_t parsed = fallback;
                        return parseIntStrict(
                            container::StringView{coordsLower}.substr(begin, end - begin),
                            parsed) ? parsed : fallback;
                    };
                    img.left = findCoord("left", img.left);
                    img.top = findCoord("top", img.top);
                    img.right = findCoord("right", img.right);
                    img.bottom = findCoord("bottom", img.bottom);
                } else if (keyLower == "status") {
                    container::String valLower = toLowerStr(value);
                    img.status = valLower.find("rotated_90_clockwise") !=
                        container::String::npos ? 1 : 0;
                }
            }

            if (!img.name.empty()) {
                if (existing != destination.end() && existing->second) {
                    // RefCode reuses the existing Image and applies only fields
                    // present in the later block. Preserve pointer stability
                    // for widgets and animation descriptors already resolved.
                    *existing->second = std::move(img);
                } else {
                    destination[nameKey] =
                        std::make_unique<MappedImage>(std::move(img));
                }
            }
        }
    }
}

const MappedImage* MappedImageCollection::findByName(const container::String& name) const {
    container::String key = toLowerStr(name);
    const auto session = m_sessionImages.find(key);
    if (session != m_sessionImages.end()) return session->second.get();
    auto it = m_images.find(key);
    return it != m_images.end() ? it->second.get() : nullptr;
}

void MappedImageCollection::activateSession(
    uint64_t presentationEpoch,
    container::Span<const ui::MappedImageContentLayer> layers) {
    if (presentationEpoch == 0) {
        clearSession();
        return;
    }
    if (presentationEpoch == m_sessionEpoch) return;

    ImageMap candidate;
    for (const ui::MappedImageContentLayer& layer : layers) {
        if (layer.content.empty()) continue;
        parseINIData(layer.content, candidate, &m_images);
    }
    m_sessionImages = std::move(candidate);
    m_sessionEpoch = presentationEpoch;
    TD_LOG_INFO(
        "[MappedImageCollection] Activated session epoch {} with {} map-local images",
        m_sessionEpoch, m_sessionImages.size());
}

void MappedImageCollection::clearSession() noexcept {
    m_sessionImages.clear();
    m_sessionEpoch = 0;
}

} // namespace engine
