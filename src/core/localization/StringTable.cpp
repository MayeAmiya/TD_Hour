#include "container/container_types.h"
#include "StringTable.h"
#include <atomic>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <memory>
#include <sstream>

namespace engine {

namespace {

container::StringView trimView(container::StringView value) noexcept {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

bool isComment(container::StringView value) noexcept {
    value = trimView(value);
    return value.starts_with("//") || value.starts_with('#');
}

bool equalAsciiInsensitive(container::StringView left,
                           container::StringView right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const unsigned char lc = static_cast<unsigned char>(left[index]);
        const unsigned char rc = static_cast<unsigned char>(right[index]);
        if (std::tolower(lc) != std::tolower(rc)) return false;
    }
    return true;
}

bool closingQuoteAt(container::StringView value, size_t index) noexcept {
    if (index >= value.size() || value[index] != '"') return false;
    size_t slashCount = 0;
    while (index > slashCount && value[index - slashCount - 1] == '\\') {
        ++slashCount;
    }
    return (slashCount & 1u) == 0u;
}

[[nodiscard]] bool isSpeechReferenceCharacter(char character) noexcept {
    const unsigned char value = static_cast<unsigned char>(character);
    return (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || value == '_';
}

[[nodiscard]] container::String parseSpeechReference(
    container::StringView tail) {
    tail = trimView(tail);
    while (!tail.empty() && tail.front() == '=') {
        tail.remove_prefix(1);
        tail = trimView(tail);
    }
    size_t length = 0;
    while (length < tail.size() &&
           isSpeechReferenceCharacter(tail[length])) {
        ++length;
    }
    container::String speech(tail.substr(0, length));
    // ZH GameText::readToEndOfQuote() appends an 'e' to a wave reference
    // whose authored name ends in a digit. Campaign map.str files rely on
    // this legacy normalization (for example XC1XOC02 -> XC1XOC02e).
    if (!speech.empty() && speech.back() >= '0' && speech.back() <= '9') {
        speech.push_back('e');
    }
    return speech;
}

container::String decodeStringLiteral(container::StringView value) {
    container::String decoded;
    decoded.reserve(value.size());
    bool escaped = false;
    for (const char character : value) {
        if (escaped) {
            switch (character) {
            case 'n': decoded.push_back('\n'); break;
            case 't': decoded.push_back('\t'); break;
            case '\\': decoded.push_back('\\'); break;
            case '\'': decoded.push_back('\''); break;
            case '"': decoded.push_back('"'); break;
            case '?': decoded.push_back('?'); break;
            default: decoded.push_back(character); break;
            }
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
        } else if (character == '\r' || character == '\n' ||
                   character == '\t') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(character);
        }
    }
    if (escaped) decoded.push_back('\\');

    // GameText's readToEndOfQuote/stripSpaces pair folds authored physical
    // whitespace while preserving explicit \n and \t escape sequences.
    container::String normalized;
    normalized.reserve(decoded.size());
    bool pendingSpace = false;
    for (const char character : decoded) {
        if (character == ' ') {
            pendingSpace = !normalized.empty() && normalized.back() != '\n' &&
                normalized.back() != '\t';
            continue;
        }
        if (pendingSpace && character != '\n' && character != '\t') {
            normalized.push_back(' ');
        }
        pendingSpace = false;
        normalized.push_back(character);
    }
    return normalized;
}

bool parseStringFileContent(container::StringView content,
                            container::Vector<StringEntry>& output,
                            container::String* error) {
    output.clear();
    if (content.starts_with("\xEF\xBB\xBF")) content.remove_prefix(3);

    std::istringstream stream(container::String{content});
    container::String line;
    size_t lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const container::StringView labelView = trimView(line);
        if (labelView.empty() || isComment(labelView)) continue;
        if (equalAsciiInsensitive(labelView, "END")) {
            if (error) {
                *error = "unexpected END at line " +
                    std::to_string(lineNumber);
            }
            return false;
        }

        StringEntry entry;
        entry.label.assign(labelView);
        bool foundText = false;
        bool foundEnd = false;
        while (std::getline(stream, line)) {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            container::StringView value = trimView(line);
            if (value.empty() || isComment(value)) continue;
            if (equalAsciiInsensitive(value, "END")) {
                foundEnd = true;
                break;
            }
            if (value.front() != '"') continue;
            if (foundText) {
                if (error) {
                    *error = "multiple strings for label '" + entry.label +
                        "' near line " + std::to_string(lineNumber);
                }
                return false;
            }

            container::String literal;
            value.remove_prefix(1);
            bool closed = false;
            while (true) {
                size_t quote = 0;
                while (quote < value.size() &&
                       !closingQuoteAt(value, quote)) {
                    ++quote;
                }
                if (quote < value.size()) {
                    literal.append(value.substr(0, quote));
                    if (entry.speech.empty()) {
                        entry.speech = parseSpeechReference(
                            value.substr(quote + 1u));
                    }
                    closed = true;
                    break;
                }
                literal.append(value);
                literal.push_back(' ');
                if (!std::getline(stream, line)) break;
                ++lineNumber;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                value = line;
            }
            if (!closed) {
                if (error) {
                    *error = "unterminated string for label '" + entry.label +
                        "'";
                }
                return false;
            }
            entry.text = decodeStringLiteral(literal);
            foundText = true;
        }
        if (!foundEnd) {
            if (error) {
                *error = "missing END for label '" + entry.label + "'";
            }
            return false;
        }
        output.push_back(std::move(entry));
    }

    container::Vector<size_t> order(output.size());
    for (size_t index = 0; index < order.size(); ++index) order[index] = index;
    const auto compare = [&output](size_t left, size_t right) {
        const container::String& a = output[left].label;
        const container::String& b = output[right].label;
        const size_t common = std::min(a.size(), b.size());
        for (size_t index = 0; index < common; ++index) {
            const unsigned char ac = static_cast<unsigned char>(a[index]);
            const unsigned char bc = static_cast<unsigned char>(b[index]);
            const char al = static_cast<char>(std::tolower(ac));
            const char bl = static_cast<char>(std::tolower(bc));
            if (al != bl) return al < bl;
        }
        return a.size() < b.size();
    };
    std::sort(order.begin(), order.end(), compare);
    for (size_t index = 1; index < order.size(); ++index) {
        const container::String& current = output[order[index]].label;
        const bool previousBeforeCurrent = compare(order[index - 1], order[index]);
        const bool currentBeforePrevious = compare(order[index], order[index - 1]);
        if (!previousBeforeCurrent && !currentBeforePrevious) {
            if (error) {
                *error = "duplicate string label '" + current + "'";
            }
            return false;
        }
    }
    if (error) error->clear();
    return true;
}

} // namespace

StringTable* TheStringTable = nullptr;

StringTable& StringTable::instance() {
    static StringTable s_instance;
    return s_instance;
}

bool StringTable::load(const container::String& filename) {
    std::ifstream file(filename, std::ios::binary);
    return file.is_open() && loadCsf(file);
}

bool StringTable::loadFromMemory(const uint8_t* data, size_t size) {
    if (!data || size == 0) return false;
    container::String bytes(reinterpret_cast<const char*>(data), size);
    std::istringstream stream(std::move(bytes), std::ios::binary);
    return loadCsf(stream);
}

bool StringTable::loadCsf(std::istream& file) {
    unload();

    // Read header
    CSFHeader header{};
    if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
        header.id != CSF_ID || header.version < CSF_MIN_VERSION ||
        header.version > CSF_VERSION) {
        return false;
    }

    m_language = static_cast<LanguageID>(header.langId);
    m_strings.reserve(header.numLabels);

    // Parse entries
    uint32_t entryId;
    while (file.read(reinterpret_cast<char*>(&entryId), sizeof(entryId))) {
        if (entryId != CSF_LABEL) {
            break;  // Unexpected format
        }

        uint32_t numStrings;
        file.read(reinterpret_cast<char*>(&numStrings), sizeof(numStrings));

        // Read label (labelLen includes null terminator in CSF format)
        uint32_t labelLen;
        file.read(reinterpret_cast<char*>(&labelLen), sizeof(labelLen));

        container::String label(labelLen, '\0');
        if (labelLen > 0) {
            file.read(label.data(), labelLen);
        }
        // Strip trailing null terminator(s) from label
        while (!label.empty() && label.back() == '\0') {
            label.pop_back();
        }

        StringEntry entry;
        entry.label = std::move(label);

        // Read string(s)
        for (uint32_t i = 0; i < numStrings; ++i) {
            uint32_t stringId;
            file.read(reinterpret_cast<char*>(&stringId), sizeof(stringId));

            if (stringId != CSF_STRING && stringId != CSF_STRINGWITHWAVE) {
                break;  // Unexpected format
            }

            uint32_t stringLen;
            file.read(reinterpret_cast<char*>(&stringLen), sizeof(stringLen));

            // Read UTF-16 wide string (stringLen includes null terminator)
            container::WString wideStr(stringLen, L'\0');
            if (stringLen > 0) {
                file.read(reinterpret_cast<char*>(wideStr.data()),
                          stringLen * sizeof(wchar_t));
            }
            // Strip trailing null terminator
            while (!wideStr.empty() && wideStr.back() == L'\0') {
                wideStr.pop_back();
            }

            // XOR decode (bitwise NOT)
            decodeWideString(wideStr);

            // Convert to UTF-8 for storage
            if (i == 0) {
                entry.text = utf16ToUtf8(wideStr);
            }

            // Read speech file reference if present
            if (stringId == CSF_STRINGWITHWAVE && i == 0) {
                uint32_t speechLen;
                file.read(reinterpret_cast<char*>(&speechLen), sizeof(speechLen));
                if (speechLen > 0) {
                    entry.speech.resize(speechLen, '\0');
                    file.read(entry.speech.data(), speechLen);
                    // Strip trailing null terminator
                    while (!entry.speech.empty() && entry.speech.back() == '\0') {
                        entry.speech.pop_back();
                    }
                }
            }
        }

        if (!file) return false;
        m_strings.push_back(std::move(entry));
    }

    if (m_strings.size() != header.numLabels) return false;
    buildIndex();
    m_loaded = true;
    return true;
}

bool StringTable::loadStringFile(const container::String& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    std::ostringstream bytes;
    bytes << file.rdbuf();
    container::Vector<StringEntry> parsed;
    if (!parseStringFileContent(bytes.str(), parsed, nullptr)) return false;
    m_strings.insert(m_strings.end(),
                     std::make_move_iterator(parsed.begin()),
                     std::make_move_iterator(parsed.end()));

    buildIndex();
    m_loaded = true;
    return true;
}

bool StringTable::activateMapStringFile(
    uint64_t presentationEpoch, container::StringView content,
    container::String* error) {
    if (presentationEpoch == 0) {
        if (error) *error = "map string activation requires a presentation epoch";
        return false;
    }
    if (m_mapStringEpoch.load(std::memory_order_acquire) ==
        presentationEpoch) {
        if (error) error->clear();
        return true;
    }
    auto layer = std::make_shared<MapStringLayer>();
    if (!parseStringFileContent(content, layer->strings, error)) {
        std::atomic_store_explicit(
            &m_mapStringLayer, container::SharedPtr<const MapStringLayer>{},
            std::memory_order_release);
        m_mapStringEpoch.store(presentationEpoch, std::memory_order_release);
        return false;
    }
    layer->sortedIndex.resize(layer->strings.size());
    for (size_t index = 0; index < layer->sortedIndex.size(); ++index) {
        layer->sortedIndex[index] = index;
    }
    std::sort(layer->sortedIndex.begin(), layer->sortedIndex.end(),
              [this, &layer](size_t left, size_t right) {
                  return compareLabels(layer->strings[left].label,
                                       layer->strings[right].label) < 0;
              });
    std::atomic_store_explicit(
        &m_mapStringLayer,
        container::SharedPtr<const MapStringLayer>{std::move(layer)},
        std::memory_order_release);
    m_mapStringEpoch.store(presentationEpoch, std::memory_order_release);
    if (error) error->clear();
    return true;
}

void StringTable::clearMapStringFile() noexcept {
    std::atomic_store_explicit(
        &m_mapStringLayer, container::SharedPtr<const MapStringLayer>{},
        std::memory_order_release);
    m_mapStringEpoch.store(0, std::memory_order_release);
}

void StringTable::unload() {
    clearMapStringFile();
    m_strings.clear();
    m_sortedIndex.clear();
    m_loaded = false;
}

container::String StringTable::fetch(const container::String& label) const {
    if (!m_loaded) {
        return "";
    }

    // Binary search in sorted index
    size_t left = 0;
    size_t right = m_sortedIndex.size();

    while (left < right) {
        size_t mid = left + (right - left) / 2;
        int cmp = compareLabels(m_strings[m_sortedIndex[mid]].label, label);
        if (cmp < 0) {
            left = mid + 1;
        } else if (cmp > 0) {
            right = mid;
        } else {
            return m_strings[m_sortedIndex[mid]].text;
        }
    }

    const container::SharedPtr<const MapStringLayer> mapLayer =
        std::atomic_load_explicit(
            &m_mapStringLayer, std::memory_order_acquire);
    if (!mapLayer) return "";
    left = 0;
    right = mapLayer->sortedIndex.size();
    while (left < right) {
        const size_t mid = left + (right - left) / 2;
        const StringEntry& entry =
            mapLayer->strings[mapLayer->sortedIndex[mid]];
        const int cmp = compareLabels(entry.label, label);
        if (cmp < 0) left = mid + 1;
        else if (cmp > 0) right = mid;
        else return entry.text;
    }
    return "";  // Not found in either the CSF or the current map layer.
}

container::String StringTable::fetchOrFallback(const container::String& label, const container::String& fallback) const {
    container::String result = fetch(label);
    return result.empty() ? fallback : result;
}

container::String StringTable::fetchFormat(const char* label, ...) const {
    container::String base = fetch(label);
    if (base.empty()) {
        return "";
    }

    // Format with variadic args
    va_list args;
    va_start(args, label);

    // Estimate buffer size
    int size = 1024;
    container::Vector<char> buffer(size);

    // Try to format
    int result = _vsnprintf_s(buffer.data(), size, _TRUNCATE, base.c_str(), args);
    va_end(args);

    if (result >= 0 && result < size) {
        return container::String(buffer.data(), result);
    }

    return base;
}

bool StringTable::exists(const container::String& label) const {
    if (!m_loaded) return false;

    size_t left = 0;
    size_t right = m_sortedIndex.size();

    while (left < right) {
        size_t mid = left + (right - left) / 2;
        int cmp = compareLabels(m_strings[m_sortedIndex[mid]].label, label);
        if (cmp < 0) {
            left = mid + 1;
        } else if (cmp > 0) {
            right = mid;
        } else {
            return true;
        }
    }

    const container::SharedPtr<const MapStringLayer> mapLayer =
        std::atomic_load_explicit(
            &m_mapStringLayer, std::memory_order_acquire);
    if (!mapLayer) return false;
    left = 0;
    right = mapLayer->sortedIndex.size();
    while (left < right) {
        const size_t mid = left + (right - left) / 2;
        const StringEntry& entry =
            mapLayer->strings[mapLayer->sortedIndex[mid]];
        const int cmp = compareLabels(entry.label, label);
        if (cmp < 0) left = mid + 1;
        else if (cmp > 0) right = mid;
        else return true;
    }
    return false;
}

container::Vector<container::String> StringTable::fetchLabels(size_t count) const {
    container::Vector<container::String> result;
    if (!m_loaded) return result;

    size_t n = std::min(count, m_strings.size());
    result.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        result.push_back(m_strings[m_sortedIndex[i]].label);
    }
    const container::SharedPtr<const MapStringLayer> mapLayer =
        std::atomic_load_explicit(
            &m_mapStringLayer, std::memory_order_acquire);
    if (mapLayer && result.size() < count) {
        const size_t mapCount = std::min(
            count - result.size(), mapLayer->sortedIndex.size());
        for (size_t index = 0; index < mapCount; ++index) {
            result.push_back(
                mapLayer->strings[mapLayer->sortedIndex[index]].label);
        }
    }
    return result;
}

size_t StringTable::getMapStringCount() const noexcept {
    const container::SharedPtr<const MapStringLayer> mapLayer =
        std::atomic_load_explicit(
            &m_mapStringLayer, std::memory_order_acquire);
    return mapLayer ? mapLayer->strings.size() : 0u;
}

void StringTable::decodeWideString(container::WString& str) const {
    // CSF uses bitwise NOT encoding
    for (auto& ch : str) {
        ch = ~ch;
    }
}

container::String StringTable::utf16ToUtf8(const container::WString& utf16) const {
    container::String utf8;
    utf8.reserve(utf16.size() * 2);  // Reserve space for worst case

    for (wchar_t wc : utf16) {
        if (wc < 0x80) {
            // ASCII
            utf8 += static_cast<char>(wc);
        } else if (wc < 0x800) {
            // 2 bytes
            utf8 += static_cast<char>(0xC0 | (static_cast<uint32_t>(wc) >> 6));
            utf8 += static_cast<char>(0x80 | (static_cast<uint32_t>(wc) & 0x3F));
        } else if (wc < 0x10000) {
            // 3 bytes
            utf8 += static_cast<char>(0xE0 | (static_cast<uint32_t>(wc) >> 12));
            utf8 += static_cast<char>(0x80 | ((static_cast<uint32_t>(wc) >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (static_cast<uint32_t>(wc) & 0x3F));
        } else {
            // 4 bytes
            utf8 += static_cast<char>(0xF0 | (static_cast<uint32_t>(wc) >> 18));
            utf8 += static_cast<char>(0x80 | ((static_cast<uint32_t>(wc) >> 12) & 0x3F));
            utf8 += static_cast<char>(0x80 | ((static_cast<uint32_t>(wc) >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (static_cast<uint32_t>(wc) & 0x3F));
        }
    }

    return utf8;
}

int StringTable::compareLabels(const container::String& a, const container::String& b) const {
    // Case-insensitive comparison
    size_t minLen = std::min(a.size(), b.size());
    for (size_t i = 0; i < minLen; ++i) {
        char ca = std::tolower(static_cast<unsigned char>(a[i]));
        char cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca < cb) return -1;
        if (ca > cb) return 1;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

void StringTable::buildIndex() {
    m_sortedIndex.resize(m_strings.size());
    for (size_t i = 0; i < m_strings.size(); ++i) {
        m_sortedIndex[i] = i;
    }

    // Sort by label (case-insensitive)
    std::sort(m_sortedIndex.begin(), m_sortedIndex.end(),
              [this](size_t a, size_t b) {
                  return compareLabels(m_strings[a].label, m_strings[b].label) < 0;
              });
}

} // namespace engine
