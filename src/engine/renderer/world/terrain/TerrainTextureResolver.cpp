#include "engine/renderer/world/terrain/TerrainTextureResolver.h"

#include "VFS.h"
#include "LocaleResourceLocator.h"
#include "debug/debug.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace engine::render {
namespace {

container::String trimAscii(container::StringView value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return container::String(value.substr(begin, end - begin));
}

container::String lowercaseAscii(container::StringView value) {
    container::String result(value);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return result;
}

container::String basenameLowercase(container::StringView path) {
    const size_t slash = path.find_last_of("/\\");
    return lowercaseAscii(slash == container::StringView::npos
        ? path : path.substr(slash + 1));
}

} // namespace

TerrainTextureResolver::TerrainTextureResolver() {
    container::HashSet<container::String> seenVirtualPaths;
    size_t parsedLayerCount = 0;
    const auto locator = io::acquireLocaleResourceLocator();
    const container::Vector<container::String> terrainIniFiles = locator
        ? locator->enumeratePrefix("data/ini/", "terrain.ini")
        : io::VFS::instance().getFileList("data/ini/*terrain.ini");
    for (const container::String& path : terrainIniFiles) {
        if (basenameLowercase(path) != "terrain.ini") continue;
        container::String canonicalPath = lowercaseAscii(path);
        std::replace(canonicalPath.begin(), canonicalPath.end(), '\\', '/');
        if (!seenVirtualPaths.emplace(std::move(canonicalPath)).second) {
            continue;
        }
        const container::String contents = io::VFS::instance().readAll(path);
        if (contents.empty()) continue;
        m_hasTerrainIni = true;
        ++parsedLayerCount;
        parse(contents);
    }
    if (m_hasTerrainIni) {
        TD_LOG_INFO(
            "[TerrainTextureResolver] loaded {} type mappings from {} layered INI sources",
            m_textureByTerrainType.size(), parsedLayerCount);
    }
}

TerrainTextureResolution TerrainTextureResolver::resolve(
    container::StringView terrainClass) const {
    const container::String key = lowercaseAscii(trimAscii(terrainClass));
    if (!key.empty()) {
        if (const auto found = m_textureByTerrainType.find(key);
            found != m_textureByTerrainType.end() &&
            !found->second.empty()) {
            return {terrainPath(found->second), true, m_hasTerrainIni};
        }
    }
    return {container::String(terrainClass), false, m_hasTerrainIni};
}

container::String TerrainTextureResolver::terrainPath(
    container::String texture) {
    const container::String lower = lowercaseAscii(texture);
    if (lower.rfind("art/", 0) == 0 || lower.rfind("data/", 0) == 0 ||
        texture.find('/') != container::String::npos ||
        texture.find('\\') != container::String::npos) {
        return texture;
    }
    return "Art/Terrain/" + texture;
}

void TerrainTextureResolver::parse(container::StringView contents) {
    container::String activeTerrain;
    size_t offset = 0;
    while (offset <= contents.size()) {
        const size_t lineEnd = contents.find_first_of("\r\n", offset);
        container::String line = trimAscii(contents.substr(
            offset, lineEnd - offset));
        offset = lineEnd == container::StringView::npos
            ? contents.size() + 1 : lineEnd + 1;
        if (line.empty()) continue;
        const size_t semicolon = line.find(';');
        const size_t slashComment = line.find("//");
        const size_t comment = std::min(semicolon, slashComment);
        if (comment != container::String::npos) {
            line = trimAscii(container::StringView(line).substr(0, comment));
        }
        if (line.empty()) continue;

        const size_t firstWhitespace = line.find_first_of(" \t");
        const container::String command = lowercaseAscii(
            firstWhitespace == container::String::npos
                ? container::StringView(line)
                : container::StringView(line).substr(0, firstWhitespace));
        if (command == "terrain") {
            activeTerrain = lowercaseAscii(trimAscii(
                firstWhitespace == container::String::npos
                    ? container::StringView{}
                    : container::StringView(line).substr(
                          firstWhitespace + 1)));
            if (!activeTerrain.empty()) {
                m_textureByTerrainType.try_emplace(activeTerrain);
            }
            continue;
        }
        if (command == "end") {
            activeTerrain.clear();
            continue;
        }
        if (activeTerrain.empty()) continue;

        const size_t equals = line.find('=');
        if (equals == container::String::npos) continue;
        const container::String key = lowercaseAscii(trimAscii(
            container::StringView(line).substr(0, equals)));
        if (key != "texture") continue;
        m_textureByTerrainType[activeTerrain] = trimAscii(
            container::StringView(line).substr(equals + 1));
    }

    const auto defaultTerrain =
        m_textureByTerrainType.find("defaultterrain");
    if (defaultTerrain == m_textureByTerrainType.end() ||
        defaultTerrain->second.empty()) return;
    for (auto& [name, texture] : m_textureByTerrainType) {
        static_cast<void>(name);
        if (texture.empty()) texture = defaultTerrain->second;
    }
}

int32_t terrainSourceGridWidth(
    const TerrainTextureClassRenderData& textureClass) noexcept {
    constexpr int32_t maximumTilesPerAxis = 10;
    if (textureClass.tileCount <= 0) return 0;
    const int32_t storedWidth = textureClass.tileWidth;
    if (storedWidth > 0 && storedWidth <= maximumTilesPerAxis &&
        static_cast<int64_t>(storedWidth) * storedWidth <=
            textureClass.tileCount) {
        return storedWidth;
    }
    const int32_t inferred = static_cast<int32_t>(std::floor(std::sqrt(
        static_cast<float>(textureClass.tileCount))));
    return std::clamp(inferred, 1, maximumTilesPerAxis);
}

} // namespace engine::render
