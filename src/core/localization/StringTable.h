#pragma once

#include "container/container_types.h"
#include <atomic>

#include <cstdint>
#include <iosfwd>
namespace engine {

// CSF file format constants
enum : uint32_t {
    CSF_ID            = 0x43534620, // 'CSF '
    CSF_LABEL         = 0x4C424C20, // 'LBL '
    CSF_STRING        = 0x53545220, // 'STR '
    CSF_STRINGWITHWAVE = 0x53545257, // 'STRW'
    CSF_MIN_VERSION   = 2,
    CSF_VERSION       = 3
};

// CSF header structure
#pragma pack(push, 1)
struct CSFHeader {
    uint32_t id;
    uint32_t version;
    uint32_t numLabels;
    uint32_t numStrings;
    uint32_t skip;
    uint32_t langId;
};
#pragma pack(pop)

// String info entry (UTF-8 storage, converted from CSF's UTF-16 at load time)
struct StringEntry {
    container::String label;
    container::String text;      // UTF-8
    container::String speech;    // Optional wave file reference
};

// Language IDs from original game
enum LanguageID : uint32_t {
    LANGUAGE_ID_US = 0,
    LANGUAGE_ID_UK,
    LANGUAGE_ID_GERMAN,
    LANGUAGE_ID_FRENCH,
    LANGUAGE_ID_SPANISH,
    LANGUAGE_ID_ITALIAN,
    LANGUAGE_ID_JAPANESE,
    LANGUAGE_ID_KOREAN,
    LANGUAGE_ID_CHINESE,
    LANGUAGE_ID_COUNT
};

// StringTable singleton
class StringTable {
public:
    StringTable() = default;
    ~StringTable() = default;

    // Load CSF file
    bool load(const container::String& filename);
    bool loadFromMemory(const uint8_t* data, size_t size);

    // Load string file (text format)
    bool loadStringFile(const container::String& filename);

    // Atomically publishes the current map's optional map.str fallback layer.
    // ZH searches the process CSF first and consults map.str only when the
    // label is absent there. Readers may run on UI/render threads while the
    // main presentation owner replaces the immutable layer at a committed
    // session-epoch boundary.
    bool activateMapStringFile(
        uint64_t presentationEpoch,
        container::StringView content,
        container::String* error = nullptr);
    void clearMapStringFile() noexcept;
    [[nodiscard]] uint64_t activeMapStringEpoch() const noexcept {
        return m_mapStringEpoch.load(std::memory_order_acquire);
    }

    // Unload all strings
    void unload();

    // Fetch string by label (returns empty string if not found)
    container::String fetch(const container::String& label) const;

    // Fetch with fallback
    container::String fetchOrFallback(const container::String& label, const container::String& fallback) const;

    // Fetch formatted string (printf-style, label must be C string for va_start)
    container::String fetchFormat(const char* label, ...) const;

    // Check if label exists
    bool exists(const container::String& label) const;

    // Get first N labels (for debugging)
    container::Vector<container::String> fetchLabels(size_t count) const;

    // Get language ID
    LanguageID getLanguage() const { return m_language; }

    // The localized VFS path is authoritative when legacy CSF headers use a
    // product-specific language id not represented by this compact enum.
    void overrideLanguage(LanguageID language) noexcept {
        if (language < LANGUAGE_ID_COUNT) m_language = language;
    }

    // Get string count
    size_t getStringCount() const { return m_strings.size(); }
    size_t getMapStringCount() const noexcept;

    // Singleton
    static StringTable& instance();

private:
    struct MapStringLayer final {
        container::Vector<StringEntry> strings;
        container::Vector<size_t> sortedIndex;
    };

    bool loadCsf(std::istream& stream);

    // XOR decode wide string (CSF uses bitwise NOT encoding)
    void decodeWideString(container::WString& str) const;

    // UTF-16 to UTF-8 conversion
    container::String utf16ToUtf8(const container::WString& utf16) const;

    // Binary search helper
    int compareLabels(const container::String& a, const container::String& b) const;

    // Sort and build lookup index
    void buildIndex();

    container::Vector<StringEntry> m_strings;
    container::Vector<size_t> m_sortedIndex;  // Indices into m_strings, sorted by label
    container::SharedPtr<const MapStringLayer> m_mapStringLayer;
    std::atomic<uint64_t> m_mapStringEpoch{0};
    LanguageID m_language = LANGUAGE_ID_US;
    bool m_loaded = false;
};

// Global accessor
extern StringTable* TheStringTable;

} // namespace engine
