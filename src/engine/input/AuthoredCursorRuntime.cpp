#include "AuthoredCursorRuntime.h"

#include "core/data/dds/DdsLoader.h"
#include "core/data/ini/LegacyIniDirectory.h"
#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "io/LocaleResourceLocator.h"
#include "io/VFS.h"

#include <SDL3/SDL_surface.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

namespace engine::input {
namespace {

struct AniChunk final {
    size_t offset = 0;
    size_t size = 0;
};

struct AniData final {
    container::Vector<AniChunk> icons;
    container::Vector<uint32_t> rates;
    container::Vector<uint32_t> sequence;
    uint32_t defaultRate = 6;
};

struct DecodedCursorFrame final {
    SDL_Surface* surface = nullptr;
    int hotX = 0;
    int hotY = 0;
};

[[nodiscard]] constexpr uint32_t fourCc(
    char a, char b, char c, char d) noexcept {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
        (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8u) |
        (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16u) |
        (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24u);
}

[[nodiscard]] bool readU16(
    container::Span<const uint8_t> bytes, size_t offset,
    uint16_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 2u) return false;
    value = static_cast<uint16_t>(bytes[offset]) |
        static_cast<uint16_t>(bytes[offset + 1u] << 8u);
    return true;
}

[[nodiscard]] bool readU32(
    container::Span<const uint8_t> bytes, size_t offset,
    uint32_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4u) return false;
    value = static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
    return true;
}

[[nodiscard]] bool readI32(
    container::Span<const uint8_t> bytes, size_t offset,
    int32_t& value) noexcept {
    uint32_t raw = 0;
    if (!readU32(bytes, offset, raw)) return false;
    value = static_cast<int32_t>(raw);
    return true;
}

// RIFF nesting costs one stack frame per level but only 12 bytes of file data,
// so a crafted or corrupt .ani of a few hundred KB can drive tens of thousands
// of levels and overflow the stack while loading mod cursor content.  Real
// animated cursors nest RIFF > LIST > LIST at most.
constexpr uint32_t kMaxAniChunkDepth = 16u;

void collectAniChunks(
    container::Span<const uint8_t> bytes, size_t begin, size_t end,
    AniData& result, uint32_t depth = 0) {
    if (depth > kMaxAniChunkDepth) return;
    size_t cursor = begin;
    while (cursor <= end && end - cursor >= 8u) {
        uint32_t id = 0;
        uint32_t size = 0;
        if (!readU32(bytes, cursor, id) ||
            !readU32(bytes, cursor + 4u, size)) {
            return;
        }
        const size_t dataBegin = cursor + 8u;
        if (dataBegin > end || size > end - dataBegin) return;
        const size_t dataEnd = dataBegin + size;
        if ((id == fourCc('L', 'I', 'S', 'T') ||
             id == fourCc('R', 'I', 'F', 'F')) && size >= 4u) {
            collectAniChunks(bytes, dataBegin + 4u, dataEnd, result, depth + 1u);
        } else if (id == fourCc('a', 'n', 'i', 'h') && size >= 36u) {
            static_cast<void>(readU32(bytes, dataBegin + 28u,
                                      result.defaultRate));
        } else if (id == fourCc('r', 'a', 't', 'e')) {
            for (size_t offset = dataBegin; offset + 4u <= dataEnd;
                 offset += 4u) {
                uint32_t rate = 0;
                if (readU32(bytes, offset, rate)) result.rates.push_back(rate);
            }
        } else if (id == fourCc('s', 'e', 'q', ' ')) {
            for (size_t offset = dataBegin; offset + 4u <= dataEnd;
                 offset += 4u) {
                uint32_t frame = 0;
                if (readU32(bytes, offset, frame))
                    result.sequence.push_back(frame);
            }
        } else if (id == fourCc('i', 'c', 'o', 'n') && size != 0u) {
            result.icons.push_back({.offset = dataBegin, .size = size});
        }
        const size_t padded = static_cast<size_t>(size) + (size & 1u);
        if (padded > end - dataBegin) return;
        cursor = dataBegin + padded;
    }
}

[[nodiscard]] std::optional<DecodedCursorFrame> decodeCur(
    container::Span<const uint8_t> bytes) {
    uint16_t reserved = 0;
    uint16_t type = 0;
    uint16_t count = 0;
    if (!readU16(bytes, 0u, reserved) || !readU16(bytes, 2u, type) ||
        !readU16(bytes, 4u, count) || reserved != 0u || type != 2u ||
        count == 0u || bytes.size() < 22u) {
        return std::nullopt;
    }

    const size_t entry = 6u;
    uint16_t hotX = 0;
    uint16_t hotY = 0;
    uint32_t imageBytes = 0;
    uint32_t imageOffset = 0;
    if (!readU16(bytes, entry + 4u, hotX) ||
        !readU16(bytes, entry + 6u, hotY) ||
        !readU32(bytes, entry + 8u, imageBytes) ||
        !readU32(bytes, entry + 12u, imageOffset) ||
        imageOffset > bytes.size() || imageBytes > bytes.size() - imageOffset ||
        imageBytes < 40u) {
        return std::nullopt;
    }

    const size_t imageEnd = static_cast<size_t>(imageOffset) + imageBytes;
    uint32_t headerSize = 0;
    int32_t signedWidth = 0;
    int32_t signedDoubledHeight = 0;
    uint16_t planes = 0;
    uint16_t bitCount = 0;
    uint32_t compression = 0;
    uint32_t colorsUsed = 0;
    if (!readU32(bytes, imageOffset, headerSize) || headerSize < 40u ||
        !readI32(bytes, imageOffset + 4u, signedWidth) ||
        !readI32(bytes, imageOffset + 8u, signedDoubledHeight) ||
        !readU16(bytes, imageOffset + 12u, planes) ||
        !readU16(bytes, imageOffset + 14u, bitCount) ||
        !readU32(bytes, imageOffset + 16u, compression) ||
        !readU32(bytes, imageOffset + 32u, colorsUsed) ||
        planes == 0u || compression != 0u || signedWidth == 0 ||
        signedDoubledHeight == 0 ||
        (bitCount != 1u && bitCount != 4u && bitCount != 8u &&
         bitCount != 24u && bitCount != 32u)) {
        return std::nullopt;
    }
    const int64_t absoluteWidth = signedWidth < 0
        ? -static_cast<int64_t>(signedWidth) : signedWidth;
    const int64_t absoluteDoubledHeight = signedDoubledHeight < 0
        ? -static_cast<int64_t>(signedDoubledHeight)
        : signedDoubledHeight;
    if (absoluteWidth <= 0 || absoluteWidth > 256 ||
        absoluteDoubledHeight < 2 || absoluteDoubledHeight > 512 ||
        (absoluteDoubledHeight & 1) != 0) {
        return std::nullopt;
    }
    const int width = static_cast<int>(absoluteWidth);
    const int height = static_cast<int>(absoluteDoubledHeight / 2);
    const bool topDown = signedDoubledHeight < 0;
    const uint32_t paletteCount = bitCount <= 8u
        ? (colorsUsed != 0u ? colorsUsed : (1u << bitCount)) : 0u;
    const size_t paletteStart = static_cast<size_t>(imageOffset) + headerSize;
    const size_t paletteBytes = static_cast<size_t>(paletteCount) * 4u;
    if (paletteStart > imageEnd || paletteBytes > imageEnd - paletteStart)
        return std::nullopt;
    const size_t xorStart = paletteStart + paletteBytes;
    const size_t xorStride =
        ((static_cast<size_t>(width) * bitCount + 31u) / 32u) * 4u;
    const size_t andStride =
        ((static_cast<size_t>(width) + 31u) / 32u) * 4u;
    const size_t xorBytes = xorStride * static_cast<size_t>(height);
    const size_t andBytes = andStride * static_cast<size_t>(height);
    if (xorStart > imageEnd || xorBytes > imageEnd - xorStart ||
        andBytes > imageEnd - xorStart - xorBytes) {
        return std::nullopt;
    }
    const size_t andStart = xorStart + xorBytes;

    bool hasSourceAlpha = false;
    if (bitCount == 32u) {
        for (int y = 0; y < height && !hasSourceAlpha; ++y) {
            const size_t row = xorStart + static_cast<size_t>(y) * xorStride;
            for (int x = 0; x < width; ++x) {
                if (bytes[row + static_cast<size_t>(x) * 4u + 3u] != 0u) {
                    hasSourceAlpha = true;
                    break;
                }
            }
        }
    }

    SDL_Surface* surface = SDL_CreateSurface(
        width, height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return std::nullopt;
    for (int y = 0; y < height; ++y) {
        const int storedY = topDown ? y : height - 1 - y;
        const size_t xorRow = xorStart +
            static_cast<size_t>(storedY) * xorStride;
        const size_t andRow = andStart +
            static_cast<size_t>(storedY) * andStride;
        for (int x = 0; x < width; ++x) {
            uint8_t red = 0;
            uint8_t green = 0;
            uint8_t blue = 0;
            uint8_t alpha = 255;
            if (bitCount == 32u) {
                const size_t pixel = xorRow + static_cast<size_t>(x) * 4u;
                blue = bytes[pixel];
                green = bytes[pixel + 1u];
                red = bytes[pixel + 2u];
                alpha = hasSourceAlpha ? bytes[pixel + 3u] : 255u;
            } else if (bitCount == 24u) {
                const size_t pixel = xorRow + static_cast<size_t>(x) * 3u;
                blue = bytes[pixel];
                green = bytes[pixel + 1u];
                red = bytes[pixel + 2u];
            } else {
                uint32_t paletteIndex = 0;
                if (bitCount == 8u) {
                    paletteIndex = bytes[xorRow + static_cast<size_t>(x)];
                } else if (bitCount == 4u) {
                    const uint8_t packed = bytes[
                        xorRow + static_cast<size_t>(x) / 2u];
                    paletteIndex = (x & 1) == 0
                        ? static_cast<uint32_t>(packed >> 4u)
                        : static_cast<uint32_t>(packed & 0x0fu);
                } else {
                    const uint8_t packed = bytes[
                        xorRow + static_cast<size_t>(x) / 8u];
                    paletteIndex = (packed >> (7u -
                        static_cast<uint32_t>(x & 7))) & 1u;
                }
                if (paletteIndex >= paletteCount) {
                    SDL_DestroySurface(surface);
                    return std::nullopt;
                }
                const size_t color = paletteStart +
                    static_cast<size_t>(paletteIndex) * 4u;
                blue = bytes[color];
                green = bytes[color + 1u];
                red = bytes[color + 2u];
            }
            const bool transparent = (bytes[
                andRow + static_cast<size_t>(x) / 8u] &
                (0x80u >> static_cast<uint32_t>(x & 7))) != 0u;
            if (transparent) alpha = 0u;
            static_cast<void>(SDL_WriteSurfacePixel(
                surface, x, y, red, green, blue, alpha));
        }
    }
    return DecodedCursorFrame{
        .surface = surface,
        .hotX = std::clamp<int>(hotX, 0, width - 1),
        .hotY = std::clamp<int>(hotY, 0, height - 1),
    };
}

[[nodiscard]] SDL_Cursor* decodeAni(
    container::Span<const uint8_t> bytes) {
    uint32_t riff = 0;
    uint32_t riffSize = 0;
    uint32_t acon = 0;
    if (!readU32(bytes, 0u, riff) || !readU32(bytes, 4u, riffSize) ||
        !readU32(bytes, 8u, acon) || riff != fourCc('R', 'I', 'F', 'F') ||
        acon != fourCc('A', 'C', 'O', 'N') || bytes.size() < 12u) {
        return nullptr;
    }
    const size_t end = std::min(
        bytes.size(), static_cast<size_t>(riffSize) + 8u);
    AniData ani;
    collectAniChunks(bytes, 12u, end, ani);
    if (ani.icons.empty()) return nullptr;

    container::Vector<DecodedCursorFrame> decoded;
    decoded.reserve(ani.icons.size());
    for (const AniChunk& icon : ani.icons) {
        if (icon.offset > bytes.size() || icon.size > bytes.size() - icon.offset)
            continue;
        std::optional<DecodedCursorFrame> frame = decodeCur(
            bytes.subspan(icon.offset, icon.size));
        if (!frame) continue;
        if (!decoded.empty() &&
            (frame->surface->w != decoded.front().surface->w ||
             frame->surface->h != decoded.front().surface->h ||
             frame->hotX != decoded.front().hotX ||
             frame->hotY != decoded.front().hotY)) {
            SDL_DestroySurface(frame->surface);
            continue;
        }
        decoded.push_back(*frame);
    }
    if (decoded.empty()) return nullptr;

    container::Vector<uint32_t> sequence = ani.sequence;
    if (sequence.empty()) {
        sequence.reserve(decoded.size());
        for (uint32_t index = 0; index < decoded.size(); ++index)
            sequence.push_back(index);
    }
    container::Vector<SDL_CursorFrameInfo> frames;
    frames.reserve(sequence.size());
    for (size_t step = 0; step < sequence.size(); ++step) {
        const uint32_t index = sequence[step];
        if (index >= decoded.size()) continue;
        const uint32_t jiffies = step < ani.rates.size()
            ? ani.rates[step] : ani.defaultRate;
        const uint64_t milliseconds =
            (static_cast<uint64_t>(std::max(jiffies, 1u)) * 1000u + 30u) /
            60u;
        frames.push_back({
            .surface = decoded[index].surface,
            .duration = static_cast<uint32_t>(std::clamp<uint64_t>(
                milliseconds, 1u, std::numeric_limits<uint32_t>::max())),
        });
    }
    SDL_Cursor* cursor = nullptr;
    if (frames.size() > 1u) {
        cursor = SDL_CreateAnimatedCursor(
            frames.data(), static_cast<int>(frames.size()),
            decoded.front().hotX, decoded.front().hotY);
    }
    if (!cursor) {
        cursor = SDL_CreateColorCursor(
            decoded.front().surface,
            decoded.front().hotX, decoded.front().hotY);
    }
    for (DecodedCursorFrame& frame : decoded)
        SDL_DestroySurface(frame.surface);
    return cursor;
}

[[nodiscard]] std::optional<DecodedCursorFrame> decodeDds(
    container::Span<const uint8_t> bytes) {
    data::dds::DdsLoader loader;
    if (!loader.loadFromMemory(bytes.data(), bytes.size())) return std::nullopt;
    container::Vector<uint8_t> rgba;
    if (!loader.decodeTopLevelRgba(rgba)) return std::nullopt;
    const auto& dds = loader.result();
    if (dds.width == 0u || dds.height == 0u || dds.width > 256u ||
        dds.height > 256u || rgba.size() != static_cast<size_t>(dds.width) *
            dds.height * 4u) {
        return std::nullopt;
    }
    SDL_Surface* surface = SDL_CreateSurface(
        static_cast<int>(dds.width), static_cast<int>(dds.height),
        SDL_PIXELFORMAT_RGBA32);
    if (!surface) return std::nullopt;
    for (uint32_t y = 0; y < dds.height; ++y) {
        for (uint32_t x = 0; x < dds.width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * dds.width + x) * 4u;
            if (!SDL_WriteSurfacePixel(surface, static_cast<int>(x),
                                       static_cast<int>(y), rgba[offset],
                                       rgba[offset + 1u], rgba[offset + 2u],
                                       rgba[offset + 3u])) {
                SDL_DestroySurface(surface);
                return std::nullopt;
            }
        }
    }
    return DecodedCursorFrame{
        .surface = surface,
        // Mouse::CursorInfo defaults to the centre of a 32x32 image when
        // Mouse.ini does not author HotSpot. Keep that contract for DDS
        // cursors, clamped for custom-sized mod assets.
        .hotX = std::clamp(static_cast<int>(dds.width / 2u), 0,
                           static_cast<int>(dds.width) - 1),
        .hotY = std::clamp(static_cast<int>(dds.height / 2u), 0,
                           static_cast<int>(dds.height) - 1),
    };
}

void appendUnique(container::Vector<container::String>& paths,
                  container::String path) {
    if (path.empty()) return;
    for (const auto& existing : paths) {
        if (container::asciiEqualIgnoreCase(existing, path)) return;
    }
    paths.push_back(std::move(path));
}

[[nodiscard]] container::Vector<container::String> ddsCandidates(
    container::StringView texture) {
    container::String authored{texture};
    if (!container::endsWithIgnoreCase(authored, ".dds")) authored += ".dds";
    container::Vector<container::String> paths;
    const auto locator = io::acquireLocaleResourceLocator();
    if (locator) {
        for (const auto& candidate : locator->candidates(
                 io::LocaleResourceKind::Texture, authored)) {
            appendUnique(paths, candidate);
        }
    }
    // These direct candidates also make cursor loading work during the short
    // pre-locator startup window and with local/mod files that are not part of
    // the frozen locale winner set. The first form is the virtual path used
    // by the original Art/Textures resource family.
    appendUnique(paths, "art/textures/" + authored);
    appendUnique(paths, "textures/" + authored);
    appendUnique(paths, "data/" + authored);
    return paths;
}

[[nodiscard]] std::optional<DecodedCursorFrame> readDdsFrame(
    container::StringView texture) {
    for (const auto& path : ddsCandidates(texture)) {
        container::Vector<uint8_t> bytes;
        if (!io::VFS::instance().readToBuffer(path, bytes) || bytes.empty())
            continue;
        std::optional<DecodedCursorFrame> frame = decodeDds(bytes);
        if (frame) return frame;
        TD_LOG_WARN("[AuthoredCursorRuntime] DDS cursor decode failed: {}", path);
    }
    return std::nullopt;
}

[[nodiscard]] SDL_Cursor* decodeAuthoredDds(
    container::StringView texture) {
    container::Vector<DecodedCursorFrame> decoded;
    // ZH ships both a static SCCAttack.dds and a 21-frame
    // SCCAttack0000.dds...SCCAttack0020.dds sequence. The original
    // W3DMouse chooses the animated resource when a frame sequence exists;
    // trying the unnumbered image first silently downgraded AttackObj to a
    // static cursor in TD. Always probe the numbered sequence first.
    constexpr size_t maxFrames = 21u;
    for (size_t index = 0; index < maxFrames; ++index) {
        container::String frameName{texture};
        if (index < 10u) frameName += "000";
        else if (index < 100u) frameName += "00";
        else frameName += "0";
        frameName += std::to_string(index);
        std::optional<DecodedCursorFrame> frame = readDdsFrame(frameName);
        if (!frame) {
            if (index == 0u) break;
            break;
        }
        if (!decoded.empty() &&
            (frame->surface->w != decoded.front().surface->w ||
             frame->surface->h != decoded.front().surface->h ||
             frame->hotX != decoded.front().hotX ||
             frame->hotY != decoded.front().hotY)) {
            SDL_DestroySurface(frame->surface);
            continue;
        }
        decoded.push_back(*frame);
    }
    if (decoded.empty()) {
        if (std::optional<DecodedCursorFrame> frame = readDdsFrame(texture)) {
            decoded.push_back(*frame);
        }
    }
    // Mouse.ini retains SCCAttMov for the attack-move command, but the stock
    // ZH texture archive does not contain that legacy alias. Preserve a
    // visible/dynamic contextual cursor by using the authored attack sequence
    // only for this missing stock alias; Mod-provided SCCAttMov resources
    // still win because they were attempted above.
    if (decoded.empty() &&
        container::asciiEqualIgnoreCase(texture, "SCCAttMov")) {
        return decodeAuthoredDds("SCCAttack");
    }
    if (decoded.empty()) return nullptr;

    container::Vector<SDL_CursorFrameInfo> frames;
    frames.reserve(decoded.size());
    for (const auto& frame : decoded) {
        frames.push_back({
            .surface = frame.surface,
            .duration = decoded.size() > 1u ? 50u : 1u,
        });
    }
    SDL_Cursor* cursor = decoded.size() > 1u
        ? SDL_CreateAnimatedCursor(frames.data(), static_cast<int>(frames.size()),
                                   decoded.front().hotX, decoded.front().hotY)
        : SDL_CreateColorCursor(decoded.front().surface,
                                decoded.front().hotX, decoded.front().hotY);
    if (!cursor && decoded.size() > 1u) {
        cursor = SDL_CreateColorCursor(decoded.front().surface,
                                       decoded.front().hotX, decoded.front().hotY);
    }
    for (auto& frame : decoded) SDL_DestroySurface(frame.surface);
    return cursor;
}

[[nodiscard]] container::String canonical(container::StringView value) {
    value = container::trimAsciiView(value);
    container::String result;
    result.reserve(value.size());
    for (const char c : value) result.push_back(container::asciiLower(c));
    return result;
}

} // namespace

AuthoredCursorRuntime::~AuthoredCursorRuntime() {
    clear();
}

void AuthoredCursorRuntime::clear() noexcept {
    SDL_Cursor* active = SDL_GetCursor();
    for (const auto& [name, cursor] : m_cursorByTexture) {
        static_cast<void>(name);
        if (!cursor) continue;
        if (active == cursor) SDL_SetCursor(SDL_GetDefaultCursor());
        SDL_DestroyCursor(cursor);
    }
    m_cursorByTexture.clear();
    m_textureByLogicalName.clear();
    m_definitionsLoaded = false;
}

void AuthoredCursorRuntime::synchronizeContent() {
    const uint64_t revision = io::VFS::instance().contentRevision();
    if (m_contentRevision == revision) return;
    clear();
    m_contentRevision = revision;
}

bool AuthoredCursorRuntime::loadDefinitions() {
    if (m_definitionsLoaded) return !m_textureByLogicalName.empty();
    m_definitionsLoaded = true;
    constexpr container::Array<container::StringView, 2> roots{{
        "data/ini/default/Mouse",
        "data/ini/Mouse",
    }};
    const container::Vector<container::String> sources =
        game::ini::enumerateLegacyIniDirectories(roots);
    for (const container::String& source : sources) {
        const container::String text = io::VFS::instance().readAll(source);
        container::String current;
        size_t cursor = 0;
        while (cursor <= text.size()) {
            const size_t end = text.find('\n', cursor);
            container::StringView line{text.data() + cursor,
                (end == container::String::npos ? text.size() : end) - cursor};
            const size_t comment = line.find(';');
            if (comment != container::StringView::npos)
                line = line.substr(0, comment);
            line = container::trimAsciiView(line);
            if (container::startsWithIgnoreCase(line, "MouseCursor ")) {
                current = canonical(line.substr(12u));
            } else if (container::asciiEqualIgnoreCase(line, "End")) {
                current.clear();
            } else if (!current.empty()) {
                const size_t equals = line.find('=');
                if (equals != container::StringView::npos &&
                    container::asciiEqualIgnoreCase(
                        container::trimAsciiView(line.substr(0, equals)),
                        "Texture")) {
                    const container::String texture = container::trimAsciiCopy(
                        line.substr(equals + 1u));
                    if (!texture.empty()) {
                        // INI_LOAD_OVERWRITE: later normal-root fragments
                        // replace Default and earlier same-name definitions.
                        m_textureByLogicalName[current] = texture;
                    }
                }
            }
            if (end == container::String::npos) break;
            cursor = end + 1u;
        }
    }
    return !m_textureByLogicalName.empty();
}

SDL_Cursor* AuthoredCursorRuntime::cursor(
    container::StringView logicalName) {
    synchronizeContent();
    if (logicalName.empty()) return nullptr;
    static_cast<void>(loadDefinitions());
    const container::String key = canonical(logicalName);
    container::String texture = container::String{logicalName};
    const auto definition = m_textureByLogicalName.find(key);
    if (definition != m_textureByLogicalName.end()) texture = definition->second;
    const container::String textureKey = canonical(texture);
    const auto cached = m_cursorByTexture.find(textureKey);
    if (cached != m_cursorByTexture.end()) return cached->second;

    container::String path = "data/cursors/";
    path += texture;
    if (!container::endsWithIgnoreCase(path, ".ani")) path += ".ani";
    container::Vector<uint8_t> bytes;
    SDL_Cursor* result = nullptr;
    if (io::VFS::instance().readToBuffer(path, bytes) && !bytes.empty())
        result = decodeAni(bytes);
    if (!result) {
        result = decodeAuthoredDds(texture);
        if (result) {
            TD_LOG_INFO("[AuthoredCursorRuntime] Loaded authored DDS cursor '{}'",
                        texture);
        }
    }
    if (!result) {
        TD_LOG_WARN("[AuthoredCursorRuntime] Cursor '{}' has no ANI or DDS resource",
                     texture);
    }
    m_cursorByTexture.emplace(textureKey, result);
    return result;
}

} // namespace engine::input
