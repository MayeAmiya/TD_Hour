#pragma once

#include "core/container/hash_containers.h"

#include "Font.h"
#include <mutex>
namespace engine {

// FontRegistry: maps (name, size, bold) → Font
class FontRegistry {
public:
    static FontRegistry& instance();

    // Get or create a font. Returns nullptr on failure.
    Font* getFont(const container::String& name, int size, bool bold = false);

    // Register a custom font file path for a name
    void registerFontPath(const container::String& name, const container::String& path);

    // Preload a font (renderer param kept for ABI compat, unused)
    Font* loadFont(const container::String& name, int size, bool bold, const container::String& path);

    // Load font mappings from INI file [Fonts] section
    void loadFromIni(const container::String& iniPath);

    void clear();

    // CJK fallback font (msyh.ttc) — used when primary font has no glyph
    Font* getCjkFallbackFont(int size);

private:
    FontRegistry() = default;

    struct FontKey {
        container::String name;
        int size = 0;
        bool bold = false;
        bool operator==(const FontKey& o) const {
            return name == o.name && size == o.size && bold == o.bold;
        }
    };

    struct FontKeyHash {
        size_t operator()(const FontKey& k) const {
            size_t h = std::hash<container::String>{}(k.name);
            h ^= std::hash<int>{}(k.size) << 1;
            h ^= std::hash<bool>{}(k.bold) << 2;
            return h;
        }
    };

    container::String resolvePath(const container::String& name);

    container::HashMap<FontKey, container::UniquePtr<Font>, FontKeyHash> m_fonts;
    container::HashMap<container::String, container::String> m_fontPaths;
    mutable std::mutex m_mutex;
};

} // namespace engine
