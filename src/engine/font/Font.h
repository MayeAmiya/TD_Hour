#pragma once

#include "core/container/hash_containers.h"

#include <cstdint>
#include <memory>
#include <mutex>
// Forward declare FreeType types
struct FT_FaceRec_;
using FT_Face = FT_FaceRec_*;

// Forward declare SDL types (kept for minimal ABI compat)
struct SDL_Renderer;

namespace engine {

// A single rendered glyph stored as raw RGBA pixel data
// Phase 2 will upload these to DX12 textures
struct Glyph {
    container::Vector<uint8_t> pixels;  // RGBA pixel data
    int width = 0;
    int height = 0;
    int bearingX = 0;
    int bearingY = 0;
    int advance = 0;  // in 1/64 pixels
    // Process-unique renderer identity. Font unload/reload creates fresh glyph
    // identities even if allocator addresses are reused.
    uint64_t rendererIdentity = 0;
    bool hasData() const { return !pixels.empty(); }
};

// Font: wraps a FreeType face, caches rendered glyphs as raw pixels
class Font {
public:
    Font() = default;
    ~Font();

    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    Font(Font&& other) noexcept;
    Font& operator=(Font&& other) noexcept;

    // Load from TTF file path (renderer param unused, kept for ABI compat)
    bool load(SDL_Renderer* renderer, const container::String& filePath, int size, bool bold = false);

    // Load from memory buffer (renderer param unused, kept for ABI compat)
    bool loadFromMemory(SDL_Renderer* renderer, const uint8_t* data, size_t dataSize,
                        int size, bool bold = false);

    void unload();

    // Get or create a glyph (stores raw pixels)
    const Glyph* getGlyph(SDL_Renderer* renderer, uint32_t codepoint);

    int getLineHeight() const { return m_lineHeight; }
    int getAscent() const { return m_ascent; }
    int getDescent() const { return m_descent; }
    int getSize() const { return m_size; }
    bool isBold() const { return m_bold; }
    const container::String& getName() const { return m_name; }

    int getTextWidth(const container::String& text) const;

    bool isLoaded() const { return m_face != nullptr; }

private:
    struct AsyncState;
    void publishCompletedGlyphs();

    FT_Face m_face = nullptr;
    int m_size = 12;
    bool m_bold = false;
    container::String m_name;

    int m_lineHeight = 0;
    int m_ascent = 0;
    int m_descent = 0;

    // Glyph pointers returned by getGlyph remain stable when the dense map grows.
    mutable std::mutex m_glyphMutex;
    container::HashMap<uint32_t, container::UniquePtr<Glyph>> m_glyphs;
    container::HashSet<uint32_t> m_missingGlyphs;

    container::SharedPtr<container::Vector<uint8_t>> m_fontData;
    size_t m_fontDataSize = 0;
    container::SharedPtr<AsyncState> m_asyncState;
};

} // namespace engine
