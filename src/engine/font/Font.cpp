#include "core/container/container_types.h"
#include "Font.h"
#include "engine/renderer/runtime/RendererIdentity.h"
#include "engine/renderer/runtime/UiSrvInvalidation.h"
#include "engine/resource/ResourceSchedulerRuntime.h"
#include "core/platform/runtime_threads.h"
#include "debug/debug.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include "core/constants/Colors.h"

// FreeType library singleton
static FT_Library g_ftLibrary = nullptr;

static FT_Library getFTLibrary() {
    if (!g_ftLibrary) {
        TD_LOG_INFO("[Font] Initializing FreeType...");
        FT_Error err = FT_Init_FreeType(&g_ftLibrary);
        if (err != 0) {
            TD_LOG_ERROR("[Font] FreeType init failed, error={}", (int)err);
            return nullptr;
        }
        TD_LOG_INFO("[Font] FreeType initialized OK, version={}.{}.{}",
            FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH);
    }
    return g_ftLibrary;
}

namespace engine {

namespace {
std::atomic<uint64_t> g_nextGlyphRendererIdentity{1};
constexpr size_t kMaximumPendingGlyphsPerFont = 4096u;

[[nodiscard]] uint64_t allocateGlyphRendererIdentity() noexcept {
    const uint64_t identity =
        render::allocateMonotonicRendererIdentity(g_nextGlyphRendererIdentity);
#if TD_DEBUG_ENABLED
    if (identity == 0) {
        TD_LOG_ERROR(
            "[Font] Renderer identity space exhausted; refusing a cacheable glyph identity");
    }
#endif
    return identity;
}

[[nodiscard]] bool rasterGlyphFromMemory(
    const container::Vector<uint8_t>& fontData, int size,
    uint32_t codepoint, Glyph& glyph) noexcept {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    if (fontData.empty() || FT_Init_FreeType(&library) != 0) return false;
    const auto cleanup = [&]() noexcept {
        if (face) FT_Done_Face(face);
        FT_Done_FreeType(library);
    };
    if (FT_New_Memory_Face(
            library, fontData.data(),
            static_cast<FT_Long>(fontData.size()), 0, &face) != 0) {
        cleanup();
        return false;
    }
    if (FT_Set_Pixel_Sizes(
            face, 0, static_cast<FT_UInt>(std::max(1, size))) != 0) {
        cleanup();
        return false;
    }
    const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
    if (glyphIndex == 0 ||
        FT_Load_Glyph(face, glyphIndex, FT_LOAD_RENDER) != 0) {
        cleanup();
        return false;
    }
    const FT_GlyphSlot slot = face->glyph;
    const int width = static_cast<int>(slot->bitmap.width);
    const int height = static_cast<int>(slot->bitmap.rows);
    glyph.width = width == 0 || height == 0
        ? static_cast<int>(slot->advance.x >> 6) : width;
    glyph.height = height;
    glyph.bearingX = width == 0 || height == 0 ? 0 : slot->bitmap_left;
    glyph.bearingY = width == 0 || height == 0 ? 0 : slot->bitmap_top;
    glyph.advance = static_cast<int>(slot->advance.x);
    if (width > 0 && height > 0) {
        try {
            glyph.pixels.resize(
                static_cast<size_t>(width) * height * 4u);
        } catch (...) {
            cleanup();
            return false;
        }
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const uint8_t alpha = slot->bitmap.buffer[
                    static_cast<ptrdiff_t>(y) * slot->bitmap.pitch + x];
                const size_t destination =
                    (static_cast<size_t>(y) * width + x) * 4u;
                glyph.pixels[destination + 0u] = 0xff;
                glyph.pixels[destination + 1u] = 0xff;
                glyph.pixels[destination + 2u] = 0xff;
                glyph.pixels[destination + 3u] = alpha;
            }
        }
    }
    cleanup();
    return true;
}
}

struct Font::AsyncState final {
    struct Completion final {
        uint32_t codepoint = 0;
        Glyph glyph;
        bool succeeded = false;
    };

    container::SharedPtr<const container::Vector<uint8_t>> fontData;
    int size = 12;
    std::mutex mutex;
    container::HashSet<uint32_t> pending;
    std::deque<Completion> completions;
    std::atomic<bool> cancelled{false};
};

Font::~Font() {
    unload();
}

Font::Font(Font&& other) noexcept
{
    std::scoped_lock lock(other.m_glyphMutex);
    m_face = other.m_face;
    m_size = other.m_size;
    m_bold = other.m_bold;
    m_name = std::move(other.m_name);
    m_lineHeight = other.m_lineHeight;
    m_ascent = other.m_ascent;
    m_descent = other.m_descent;
    m_glyphs = std::move(other.m_glyphs);
    m_missingGlyphs = std::move(other.m_missingGlyphs);
    m_fontData = std::move(other.m_fontData);
    m_fontDataSize = other.m_fontDataSize;
    m_asyncState = std::move(other.m_asyncState);
    other.m_face = nullptr;
    other.m_glyphs.clear();
}

Font& Font::operator=(Font&& other) noexcept {
    if (this != &other) {
        unload();
        std::scoped_lock lock(m_glyphMutex, other.m_glyphMutex);
        m_face = other.m_face;
        m_size = other.m_size;
        m_bold = other.m_bold;
        m_name = std::move(other.m_name);
        m_lineHeight = other.m_lineHeight;
        m_ascent = other.m_ascent;
        m_descent = other.m_descent;
        m_glyphs = std::move(other.m_glyphs);
        m_missingGlyphs = std::move(other.m_missingGlyphs);
        m_fontData = std::move(other.m_fontData);
        m_fontDataSize = other.m_fontDataSize;
        m_asyncState = std::move(other.m_asyncState);
        other.m_face = nullptr;
        other.m_glyphs.clear();
    }
    return *this;
}

bool Font::load(SDL_Renderer* renderer, const container::String& filePath, int size, bool bold) {
    unload();
    m_size = size;
    m_bold = bold;
    m_name = filePath;
    (void)renderer;  // unused — renderer parameter kept for ABI compat

    // Read file
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) {
        TD_LOG_ERROR("[Font] Cannot open: {}", filePath);
        return false;
    }
    fseek(f, 0, SEEK_END);
    m_fontDataSize = static_cast<size_t>(ftell(f));
    fseek(f, 0, SEEK_SET);
    m_fontData = std::make_shared<container::Vector<uint8_t>>();
    m_fontData->resize(m_fontDataSize);
    fread(m_fontData->data(), 1, m_fontDataSize, f);
    fclose(f);

    // Init FreeType and load face directly (don't call loadFromMemory — it would free m_fontData)
    FT_Library ft = getFTLibrary();
    if (!ft) return false;

    TD_LOG_INFO("[Font] Loading '{}' size={} bold={} ({} bytes)...", filePath, size, bold, m_fontDataSize);
    FT_Error err = FT_New_Memory_Face(ft, m_fontData->data(), static_cast<FT_Long>(m_fontDataSize), 0, &m_face);
    if (err != 0) {
        TD_LOG_ERROR("[Font] FreeType face load failed, error={}", (int)err);
        m_fontData.reset();
        m_fontDataSize = 0;
        return false;
    }

    FT_Set_Pixel_Sizes(m_face, 0, static_cast<FT_UInt>(size));

    m_lineHeight = static_cast<int>(m_face->size->metrics.height >> 6);
    m_ascent = static_cast<int>(m_face->size->metrics.ascender >> 6);
    m_descent = static_cast<int>(m_face->size->metrics.descender >> 6);
    m_asyncState = std::make_shared<AsyncState>();
    m_asyncState->fontData = m_fontData;
    m_asyncState->size = m_size;

    TD_LOG_INFO("[Font] OK '{}' lineHeight={} ascent={}", filePath, m_lineHeight, m_ascent);
    return true;
}

bool Font::loadFromMemory(SDL_Renderer* renderer, const uint8_t* data, size_t dataSize,
                          int size, bool bold) {
    unload();
    if (!data || dataSize == 0u) return false;
    m_size = size;
    m_bold = bold;
    (void)renderer;  // unused

    FT_Library ft = getFTLibrary();
    if (!ft) return false;

    // Always copy data (unload() may have freed the original)
    m_fontData = std::make_shared<container::Vector<uint8_t>>();
    m_fontData->assign(data, data + dataSize);
    m_fontDataSize = dataSize;

    if (FT_New_Memory_Face(ft, m_fontData->data(), static_cast<FT_Long>(m_fontDataSize), 0, &m_face) != 0) {
        TD_LOG_ERROR("[Font] FreeType face load failed");
        m_fontData.reset();
        m_fontDataSize = 0;
        return false;
    }

    FT_Set_Pixel_Sizes(m_face, 0, static_cast<FT_UInt>(size));

    m_lineHeight = static_cast<int>(m_face->size->metrics.height >> 6);
    m_ascent = static_cast<int>(m_face->size->metrics.ascender >> 6);
    m_descent = static_cast<int>(m_face->size->metrics.descender >> 6);
    m_asyncState = std::make_shared<AsyncState>();
    m_asyncState->fontData = m_fontData;
    m_asyncState->size = m_size;

    TD_LOG_INFO("[Font] Loaded '{}' size={} bold={} lineHeight={} ascent={}",
                m_name, m_size, m_bold, m_lineHeight, m_ascent);
    return true;
}

void Font::unload() {
    if (m_asyncState) {
        m_asyncState->cancelled.store(true, std::memory_order_release);
        m_asyncState.reset();
    }
    {
        std::scoped_lock lock(m_glyphMutex);
        for (const auto& [codepoint, glyph] : m_glyphs) {
            static_cast<void>(codepoint);
            if (glyph) {
                render::publishUiSrvInvalidation(
                    render::UiSrvResourceKind::Glyph,
                    glyph->rendererIdentity);
            }
        }
        m_glyphs.clear();
        m_missingGlyphs.clear();
    }

    if (m_face) {
        FT_Done_Face(m_face);
        m_face = nullptr;
    }
    m_fontData.reset();
    m_fontDataSize = 0;
}

void Font::publishCompletedGlyphs() {
    if (!m_asyncState) return;
    std::deque<AsyncState::Completion> completed;
    {
        std::lock_guard lock(m_asyncState->mutex);
        completed.swap(m_asyncState->completions);
    }
    for (AsyncState::Completion& completion : completed) {
        std::scoped_lock glyphLock(m_glyphMutex);
        if (!completion.succeeded) {
            m_missingGlyphs.insert(completion.codepoint);
            continue;
        }
        completion.glyph.rendererIdentity =
            allocateGlyphRendererIdentity();
        if (completion.glyph.rendererIdentity == 0u) continue;
        m_glyphs.emplace(
            completion.codepoint,
            std::make_unique<Glyph>(std::move(completion.glyph)));
    }
}

const Glyph* Font::getGlyph(SDL_Renderer* renderer, uint32_t codepoint) {
    static_cast<void>(renderer);
    if (!m_face || !m_asyncState) return nullptr;
    publishCompletedGlyphs();
    {
        std::scoped_lock lock(m_glyphMutex);
        if (const auto found = m_glyphs.find(codepoint);
            found != m_glyphs.end()) {
            return found->second.get();
        }
        if (m_missingGlyphs.contains(codepoint)) return nullptr;
    }

    const container::SharedPtr<AsyncState> state = m_asyncState;
    {
        std::lock_guard lock(state->mutex);
        if (state->pending.size() >= kMaximumPendingGlyphsPerFont) {
            return nullptr;
        }
        if (!state->pending.emplace(codepoint).second) return nullptr;
    }
    engine::resource::ResourceSchedulerRuntime* scheduler =
        engine::resource::activeResourceSchedulerRuntime();
    if (!scheduler) {
        std::lock_guard lock(state->mutex);
        state->pending.erase(codepoint);
        return nullptr;
    }
    const uint64_t estimatedBytes = static_cast<uint64_t>(
        std::max(1, state->size)) * static_cast<uint64_t>(
        std::max(1, state->size)) * 4u;
    engine::resource::ResourceRequest resourceRequest;
    resourceRequest.key.kind = engine::resource::ResourceKind::Glyph;
    resourceRequest.key.canonicalIdentity = m_name.empty()
        ? "memory-font" : m_name;
    resourceRequest.key.variant =
        (static_cast<uint64_t>(static_cast<uint32_t>(state->size)) << 32u) |
        codepoint;
    resourceRequest.demand = engine::resource::ResourceDemand::Visible;
    resourceRequest.estimatedBytes = estimatedBytes;
    const engine::resource::ResourceSubmitResult submitted = scheduler->submit(
        std::move(resourceRequest),
        [state, codepoint](
            const engine::resource::ResourceTaskContext& context) noexcept {
                platform::runtime::ThreadRoleScope role(
                    platform::runtime::ThreadRole::Resource);
                Glyph glyph;
                const bool succeeded = !context.stopRequested() &&
                    !state->cancelled.load(
                        std::memory_order_acquire) &&
                    state->fontData && rasterGlyphFromMemory(
                        *state->fontData, state->size, codepoint, glyph);
                std::lock_guard lock(state->mutex);
                state->pending.erase(codepoint);
                if (!state->cancelled.load(std::memory_order_acquire)) {
                    state->completions.push_back({
                        .codepoint = codepoint,
                        .glyph = std::move(glyph),
                        .succeeded = succeeded,
                    });
                }
                return succeeded
                    ? engine::resource::ResourceTaskResult::Ready
                    : engine::resource::ResourceTaskResult::Failed;
            });
    if (!submitted.accepted()) {
        std::lock_guard lock(state->mutex);
        state->pending.erase(codepoint);
    }
    return nullptr;
}

int Font::getTextWidth(const container::String& text) const {
    if (!m_face || text.empty()) return 0;
    std::scoped_lock lock(m_glyphMutex);

    int totalWidth = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(text.c_str());
    const uint8_t* const textEnd = p + text.size();
    while (*p) {
        uint32_t codepoint = 0;
        // Bound every continuation read against the real end.  A string ending
        // in a truncated multibyte lead (e.g. a lone 0xC3) used to consume the
        // NUL terminator and then keep reading past the buffer until it
        // happened upon a zero byte.
        const size_t available = static_cast<size_t>(textEnd - p);
        if (*p < 0x80) {
            codepoint = *p++;
        } else if ((*p & 0xE0) == 0xC0 && available >= 2) {
            codepoint = (*p++ & 0x1F) << 6;
            codepoint |= (*p++ & 0x3F);
        } else if ((*p & 0xF0) == 0xE0 && available >= 3) {
            codepoint = (*p++ & 0x0F) << 12;
            codepoint |= (*p++ & 0x3F) << 6;
            codepoint |= (*p++ & 0x3F);
        } else {
            codepoint = *p++;
        }

        // Use cached advance if available, otherwise estimate
        auto it = m_glyphs.find(codepoint);
        if (it != m_glyphs.end()) {
            totalWidth += it->second->advance >> 6;  // 1/64 pixels → pixels
        } else {
            // Estimate from font metrics
            totalWidth += m_size * 2 / 3;
        }
    }
    return totalWidth;
}

} // namespace engine
