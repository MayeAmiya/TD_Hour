#include "LanguageFilter.h"

#include "core/io/VFS.h"

#include <algorithm>
#include <cstdint>

namespace engine::text {
namespace {

constexpr char32_t kLanguageXorKey = 0x5555u;
constexpr size_t kMaximumWordListBytes = 16u * 1024u * 1024u;
constexpr size_t kMaximumWords = 65536u;
constexpr size_t kMaximumWordCodeUnits = 127u;

struct Utf8CodePoint final {
    char32_t value = 0;
    size_t byteOffset = 0;
    size_t byteLength = 1;
};

[[nodiscard]] char32_t foldAscii(char32_t value) noexcept {
    return value >= U'A' && value <= U'Z' ? value + (U'a' - U'A') : value;
}

[[nodiscard]] bool ignoredInComparison(char32_t value) noexcept {
    return value == U'-' || value == U'_' || value == U'*' ||
        value == U'\'' || value == U'"';
}

[[nodiscard]] bool tokenDelimiter(char32_t value) noexcept {
    switch (value) {
    case U' ': case U';': case U',': case U'.': case U'!': case U'?':
    case U':': case U'=': case U'\\': case U'/': case U'>': case U'<':
    case U'`': case U'~': case U'(': case U')': case U'&': case U'^':
    case U'%': case U'#': case U'\n': case U'\t':
        return true;
    default:
        return false;
    }
}

[[nodiscard]] char32_t unhaxor(char32_t value) noexcept {
    switch (foldAscii(value)) {
    case U'1': return U'l';
    case U'3': return U'e';
    case U'4': case U'@': return U'a';
    case U'5': case U'$': return U's';
    case U'6': return U'b';
    case U'7': case U'+': return U't';
    case U'0': return U'o';
    default: return foldAscii(value);
    }
}

[[nodiscard]] container::U32String normalizeToken(
    const container::Vector<Utf8CodePoint>& codePoints,
    size_t begin, size_t end) {
    container::U32String normalized;
    normalized.reserve(end - begin);
    for (size_t index = begin; index < end; ++index) {
        char32_t value = codePoints[index].value;
        if (ignoredInComparison(value)) continue;
        value = foldAscii(value);
        if (value == U'p' && index + 1 < end &&
            foldAscii(codePoints[index + 1].value) == U'h') {
            normalized.push_back(U'f');
            ++index;
            continue;
        }
        normalized.push_back(unhaxor(value));
    }
    return normalized;
}

[[nodiscard]] container::U32String normalizeWord(
    container::U32StringView word) {
    container::Vector<Utf8CodePoint> values;
    values.reserve(word.size());
    for (char32_t value : word)
        values.push_back({.value = value});
    return normalizeToken(values, 0, values.size());
}

[[nodiscard]] container::Vector<Utf8CodePoint> decodeUtf8(
    container::StringView text) {
    container::Vector<Utf8CodePoint> result;
    result.reserve(text.size());
    size_t offset = 0;
    while (offset < text.size()) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        size_t length = 1;
        char32_t value = lead;
        if ((lead & 0xe0u) == 0xc0u) { length = 2; value = lead & 0x1fu; }
        else if ((lead & 0xf0u) == 0xe0u) { length = 3; value = lead & 0x0fu; }
        else if ((lead & 0xf8u) == 0xf0u) { length = 4; value = lead & 0x07u; }
        bool valid = length != 1 || lead < 0x80u;
        if (offset + length > text.size()) valid = false;
        if (valid && length > 1) {
            for (size_t index = 1; index < length; ++index) {
                const auto continuation =
                    static_cast<unsigned char>(text[offset + index]);
                if ((continuation & 0xc0u) != 0x80u) {
                    valid = false;
                    break;
                }
                value = (value << 6u) | (continuation & 0x3fu);
            }
            const char32_t minimum = length == 2 ? 0x80u :
                length == 3 ? 0x800u : 0x10000u;
            if (value < minimum || value > 0x10ffffu ||
                (value >= 0xd800u && value <= 0xdfffu)) valid = false;
        }
        if (!valid) {
            length = 1;
            value = lead;
        }
        result.push_back({.value = value, .byteOffset = offset,
                          .byteLength = length});
        offset += length;
    }
    return result;
}

} // namespace

LanguageFilter& LanguageFilter::instance() {
    static LanguageFilter filter;
    return filter;
}

bool LanguageFilter::reloadFromVfs(container::StringView path) {
    m_loadAttempted = true;
    container::Vector<uint8_t> bytes;
    if (!io::VFS::instance().readToBuffer(path, bytes) || bytes.empty() ||
        bytes.size() > kMaximumWordListBytes || (bytes.size() & 1u) != 0u) {
        m_words.clear();
        return false;
    }

    container::TreeSet<container::U32String> words;
    container::U32String word;
    word.reserve(kMaximumWordCodeUnits);
    const auto commit = [&]() -> bool {
        if (word.empty()) return true;
        container::U32String normalized = normalizeWord(word);
        word.clear();
        if (normalized.empty()) return true;
        if (words.size() >= kMaximumWords) return false;
        words.insert(std::move(normalized));
        return true;
    };

    bool valid = true;
    for (size_t offset = 0; offset < bytes.size(); offset += 2u) {
        char32_t value = static_cast<char32_t>(
            static_cast<uint16_t>(bytes[offset]) |
            (static_cast<uint16_t>(bytes[offset + 1u]) << 8u));
        if (value == U' ') {
            if (!commit()) { valid = false; break; }
            continue;
        }
        if (word.size() >= kMaximumWordCodeUnits) { valid = false; break; }
        word.push_back(value ^ kLanguageXorKey);
    }
    if (valid) valid = commit();
    if (!valid) {
        m_words.clear();
        return false;
    }
    m_words = std::move(words);
    return true;
}

bool LanguageFilter::ensureLoaded(container::StringView path) {
    if (m_loadAttempted) return !m_words.empty();
    return reloadFromVfs(path);
}

void LanguageFilter::clear() noexcept {
    m_words.clear();
    m_loadAttempted = false;
}

container::String LanguageFilter::filterLine(container::StringView utf8) const {
    if (utf8.empty() || m_words.empty()) return container::String{utf8};
    const container::Vector<Utf8CodePoint> codePoints = decodeUtf8(utf8);
    container::Vector<bool> filtered(codePoints.size(), false);
    size_t begin = 0;
    while (begin < codePoints.size()) {
        while (begin < codePoints.size() &&
               tokenDelimiter(codePoints[begin].value)) ++begin;
        size_t end = begin;
        while (end < codePoints.size() &&
               !tokenDelimiter(codePoints[end].value)) ++end;
        if (begin < end && m_words.contains(
                normalizeToken(codePoints, begin, end))) {
            std::fill(filtered.begin() + static_cast<ptrdiff_t>(begin),
                      filtered.begin() + static_cast<ptrdiff_t>(end), true);
        }
        begin = end;
    }

    container::String result;
    result.reserve(utf8.size());
    for (size_t index = 0; index < codePoints.size(); ++index) {
        if (filtered[index]) {
            result.push_back('*');
            continue;
        }
        result.append(utf8.substr(codePoints[index].byteOffset,
                                  codePoints[index].byteLength));
    }
    return result;
}

} // namespace engine::text
