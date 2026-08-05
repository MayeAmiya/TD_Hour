#include "DX12Renderer.h"

#include "engine/font/Font.h"
#include "engine/font/FontRegistry.h"
#include "engine/gui/base/GuiDefaults.h"
#include "engine/texture/TextureManager.h"

#include <cmath>

namespace engine {
namespace {

void argbToFloats(
    uint32_t color, float& red, float& green, float& blue,
    float& alpha) noexcept {
    alpha = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
    red = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
    green = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
    blue = static_cast<float>(color & 0xFF) / 255.0f;
}

} // namespace

void DX12Renderer::setVirtualResolution(int width, int height) {
    if (width <= 0 || height <= 0) return;
    m_virtualW = width;
    m_virtualH = height;
    updateUiViewport();
}

void DX12Renderer::updateUiViewport() noexcept {
    // Geometry follows the swap-chain pixel aspect so it remains physically
    // square under Windows DPI virtualization. Input conversion separately
    // uses SDL logical extents through getScaleX/Y.
    const float outputW = static_cast<float>(std::max(
        m_width.load(std::memory_order_acquire), 1u));
    const float outputH = static_cast<float>(std::max(
        m_height.load(std::memory_order_acquire), 1u));
    const float authoredW = static_cast<float>(std::max(m_virtualW, 1));
    const float authoredH = static_cast<float>(std::max(m_virtualH, 1));
    const float outputAspect = outputW / outputH;
    const float authoredAspect = authoredW / authoredH;
    if (outputAspect >= authoredAspect) {
        m_uiCanvasH = authoredH;
        m_uiCanvasW = authoredH * outputAspect;
        m_uiOffsetX = 0.0f;
        m_uiOffsetY = 0.0f;
    } else {
        m_uiCanvasW = authoredW;
        m_uiCanvasH = authoredW / outputAspect;
        m_uiOffsetX = 0.0f;
        m_uiOffsetY = 0.0f;
    }
    m_d3d12.setVirtualResolution(m_uiCanvasW, m_uiCanvasH);
}

void DX12Renderer::drawQuad(
    float x, float y, float width, float height, uint32_t color) {
    float red, green, blue, alpha;
    argbToFloats(color, red, green, blue, alpha);
    m_d3d12.drawSolidQuad(
        x + m_uiOffsetX, y + m_uiOffsetY,
        width, height, red, green, blue, alpha);
}

void DX12Renderer::drawLine(
    float startX, float startY, float endX, float endY, float width,
    uint32_t startColor, uint32_t endColor) {
    float startRed, startGreen, startBlue, startAlpha;
    float endRed, endGreen, endBlue, endAlpha;
    argbToFloats(
        startColor, startRed, startGreen, startBlue, startAlpha);
    argbToFloats(endColor, endRed, endGreen, endBlue, endAlpha);
    m_d3d12.drawSolidGradientLine(
        startX + m_uiOffsetX, startY + m_uiOffsetY,
        endX + m_uiOffsetX, endY + m_uiOffsetY, width,
        startRed, startGreen, startBlue, startAlpha,
        endRed, endGreen, endBlue, endAlpha);
}

void DX12Renderer::drawRect(
    float x, float y, float width, float height, uint32_t color) {
    float red, green, blue, alpha;
    argbToFloats(color, red, green, blue, alpha);
    x += m_uiOffsetX;
    y += m_uiOffsetY;
    m_d3d12.drawSolidQuad(
        x, y, width, 1.0f, red, green, blue, alpha);
    m_d3d12.drawSolidQuad(
        x, y + height - 1.0f, width, 1.0f,
        red, green, blue, alpha);
    m_d3d12.drawSolidQuad(
        x, y, 1.0f, height, red, green, blue, alpha);
    m_d3d12.drawSolidQuad(
        x + width - 1.0f, y, 1.0f, height,
        red, green, blue, alpha);
}

void DX12Renderer::drawBorder(
    float x, float y, float width, float height, uint32_t color,
    int thickness) {
    for (int index = 0; index < thickness; ++index) {
        drawRect(
            x + index, y + index,
            width - 2 * index, height - 2 * index, color);
    }
}

void DX12Renderer::drawTexture(
    const RawTexture* texture, float x, float y, float width,
    float height, uint32_t tint) {
    if (!texture || texture->pixels.empty() ||
        texture->width == 0 || texture->height == 0) {
        drawQuad(x, y, width, height, tint);
        return;
    }
    const uint32_t srvIndex = getOrCreateTextureSrv(texture);
    if (srvIndex == UINT32_MAX) {
        drawQuad(x, y, width, height, tint);
        return;
    }
    float red, green, blue, alpha;
    argbToFloats(tint, red, green, blue, alpha);
    m_d3d12.drawTexturedQuad(
        x + m_uiOffsetX, y + m_uiOffsetY, width, height,
        0.0f, 0.0f, 1.0f, 1.0f,
        red, green, blue, alpha,
        m_d3d12.getSrvGpuHandle(srvIndex));
}

void DX12Renderer::drawTextureRegion(
    const RawTexture* texture, int sourceLeft, int sourceTop,
    int sourceRight, int sourceBottom, int sourceTextureWidth,
    int sourceTextureHeight, float destinationX, float destinationY,
    float destinationWidth, float destinationHeight, uint32_t tint) {
    if (!texture || texture->pixels.empty() ||
        texture->width == 0 || texture->height == 0) {
#if TD_DEBUG_ENABLED
        static int emptyTextureReports = 0;
        if (emptyTextureReports++ < 5) {
            TD_LOG_WARN(
                "[DX12Renderer] drawTextureRegion: null/empty texture pixels={} w={} h={}",
                texture ? texture->pixels.size() : 0,
                texture ? texture->width : 0,
                texture ? texture->height : 0);
        }
#endif
        drawQuad(
            destinationX, destinationY,
            destinationWidth, destinationHeight, tint);
        return;
    }
    const uint32_t srvIndex = getOrCreateTextureSrv(texture);
    if (srvIndex == UINT32_MAX) {
#if TD_DEBUG_ENABLED
        static int uploadFailureReports = 0;
        if (uploadFailureReports++ < 5) {
            TD_LOG_WARN(
                "[DX12Renderer] drawTextureRegion: uploadTexture failed for {}x{}",
                texture->width, texture->height);
        }
#endif
        drawQuad(
            destinationX, destinationY,
            destinationWidth, destinationHeight, tint);
        return;
    }
    const float u0 = static_cast<float>(sourceLeft) /
        static_cast<float>(sourceTextureWidth);
    const float v0 = static_cast<float>(sourceTop) /
        static_cast<float>(sourceTextureHeight);
    const float u1 = static_cast<float>(sourceRight) /
        static_cast<float>(sourceTextureWidth);
    const float v1 = static_cast<float>(sourceBottom) /
        static_cast<float>(sourceTextureHeight);
    float red, green, blue, alpha;
    argbToFloats(tint, red, green, blue, alpha);
    m_d3d12.drawTexturedQuad(
        destinationX + m_uiOffsetX, destinationY + m_uiOffsetY,
        destinationWidth, destinationHeight,
        u0, v0, u1, v1, red, green, blue, alpha,
        m_d3d12.getSrvGpuHandle(srvIndex));
}

void DX12Renderer::drawText(
    const container::String& text, float x, float y, uint32_t color) {
    Font* font = FontRegistry::instance().getFont(
        FONT_ARIAL.data(), ::gui::defaults::FONT_SIZE);
    if (!font) {
        const float textScaleX = getTextLayoutScaleX();
        const float textScaleY = getTextLayoutScaleY();
        drawQuad(
            x, y,
            static_cast<float>(text.size()) * 8.0f * textScaleX,
            16.0f * textScaleY,
            color);
        return;
    }
    drawText(font, text, x, y, color);
}

void DX12Renderer::drawText(
    Font* font, const container::String& text, float x, float y,
    uint32_t color) {
    const float presentationScaleX = std::max(
        getPresentationScaleX(), 0.0001f);
    const float presentationScaleY = std::max(
        getPresentationScaleY(), 0.0001f);
    const float textScaleX = getTextLayoutScaleX();
    const float textScaleY = getTextLayoutScaleY();
    if (!font || !font->isLoaded()) {
        drawQuad(
            x, y, static_cast<float>(text.size()) * 8.0f * textScaleX,
            16.0f * textScaleY,
            color);
        return;
    }

    // WND positions and control bounds live in the virtual UI canvas, but a
    // glyph bitmap must be generated at its final physical pixel density.
    // Scaling a 12 px FreeType bitmap to (for example) 18 output pixels loses
    // detail even with point sampling.  Keep the authored Font for layout and
    // use a cached, output-density Font for rasterization instead.
    Font* rasterFont = font;
    float glyphToUiX = textScaleX;
    float glyphToUiY = textScaleY;
    const float outputFontScale = std::min(
        presentationScaleX, presentationScaleY);
    const int outputFontSize = std::max(
        1, static_cast<int>(std::lround(
            static_cast<float>(font->getSize()) * outputFontScale)));
    if (outputFontSize != font->getSize() && !font->getName().empty()) {
        Font* outputFont = FontRegistry::instance().getFont(
            font->getName(), outputFontSize, font->isBold());
        if (outputFont && outputFont->isLoaded()) {
            rasterFont = outputFont;
            // One raster pixel occupies exactly one physical output pixel.
            // Independent divisors also prevent a non-square presentation
            // transform from stretching the glyph shape.
            glyphToUiX = 1.0f / presentationScaleX;
            glyphToUiY = 1.0f / presentationScaleY;
        }
    }

    m_d3d12.setSamplerMode(1);
    float cursorX = x + m_uiOffsetX;
    y += m_uiOffsetY;
    float red, green, blue, alpha;
    argbToFloats(color, red, green, blue, alpha);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.c_str());
    const size_t length = text.size();
    size_t byteIndex = 0;
    while (byteIndex < length) {
        uint32_t codepoint = 0;
        const unsigned char first = bytes[byteIndex];
        if (first < 0x80) {
            codepoint = first;
            ++byteIndex;
        } else if ((first & 0xE0) == 0xC0 && byteIndex + 1 < length &&
                   (bytes[byteIndex + 1] & 0xC0) == 0x80) {
            codepoint = (first & 0x1F) << 6;
            codepoint |= bytes[byteIndex + 1] & 0x3F;
            byteIndex += 2;
        } else if ((first & 0xF0) == 0xE0 && byteIndex + 2 < length &&
                   (bytes[byteIndex + 1] & 0xC0) == 0x80 &&
                   (bytes[byteIndex + 2] & 0xC0) == 0x80) {
            codepoint = (first & 0x0F) << 12;
            codepoint |= (bytes[byteIndex + 1] & 0x3F) << 6;
            codepoint |= bytes[byteIndex + 2] & 0x3F;
            byteIndex += 3;
        } else if ((first & 0xF8) == 0xF0 && byteIndex + 3 < length &&
                   (bytes[byteIndex + 1] & 0xC0) == 0x80 &&
                   (bytes[byteIndex + 2] & 0xC0) == 0x80 &&
                   (bytes[byteIndex + 3] & 0xC0) == 0x80) {
            codepoint = (first & 0x07) << 18;
            codepoint |= (bytes[byteIndex + 1] & 0x3F) << 12;
            codepoint |= (bytes[byteIndex + 2] & 0x3F) << 6;
            codepoint |= bytes[byteIndex + 3] & 0x3F;
            byteIndex += 4;
        } else {
            ++byteIndex;
            continue;
        }

        const Glyph* glyph = rasterFont->getGlyph(nullptr, codepoint);
        if (!glyph || !glyph->hasData()) {
            Font* fallback = FontRegistry::instance().getCjkFallbackFont(
                rasterFont->getSize());
            if (fallback) {
                glyph = fallback->getGlyph(nullptr, codepoint);
            }
            if (!glyph || !glyph->hasData()) {
                cursorX += static_cast<float>(rasterFont->getSize()) *
                    0.5f * glyphToUiX;
                continue;
            }
        }

        const float unsnappedGlyphX = cursorX +
            static_cast<float>(glyph->bearingX) * glyphToUiX;
        const float unsnappedGlyphY = y +
            static_cast<float>(rasterFont->getAscent() - glyph->bearingY) *
                glyphToUiY;
        // Align the textured quad to physical pixel boundaries.  This keeps a
        // 1:1 glyph from shimmering or appearing soft at fractional UI
        // coordinates while preserving the caller's virtual-canvas anchor.
        const float glyphX = std::round(
            unsnappedGlyphX * presentationScaleX) / presentationScaleX;
        const float glyphY = std::round(
            unsnappedGlyphY * presentationScaleY) / presentationScaleY;
        const float glyphWidth =
            static_cast<float>(glyph->width) * glyphToUiX;
        const float glyphHeight =
            static_cast<float>(glyph->height) * glyphToUiY;
        if (glyphWidth > 0.0f && glyphHeight > 0.0f) {
            const uint32_t srvIndex = getOrCreateGlyphSrv(
                glyph->rendererIdentity, glyph->pixels.data(),
                static_cast<uint32_t>(glyph->width),
                static_cast<uint32_t>(glyph->height));
            if (srvIndex != UINT32_MAX) {
                m_d3d12.drawTexturedQuad(
                    glyphX, glyphY, glyphWidth, glyphHeight,
                    0.0f, 0.0f, 1.0f, 1.0f,
                    red, green, blue, alpha,
                    m_d3d12.getSrvGpuHandle(srvIndex));
            } else {
                m_d3d12.drawSolidQuad(
                    glyphX, glyphY, glyphWidth, glyphHeight,
                    red, green, blue, alpha);
            }
        }
        cursorX += glyph->advance / 64.0f * glyphToUiX;
    }
    m_d3d12.setSamplerMode(0);
}

void DX12Renderer::winRepaint() {
    if (m_window) SDL_UpdateWindowSurface(m_window);
}

bool DX12Renderer::captureScreenshot(const container::String& filename) {
    if (filename.empty()) {
        TD_LOG_ERROR(
            "[DX12Renderer] Screenshot request has an empty filename");
        return false;
    }
    if (!m_pendingScreenshotFilename.empty()) {
        TD_LOG_WARN(
            "[DX12Renderer] Screenshot request rejected while '{}' is pending",
            m_pendingScreenshotFilename);
        return false;
    }
    if (!m_d3d12.requestPresentCapture()) {
        TD_LOG_ERROR(
            "[DX12Renderer] Screenshot capture could not be armed for '{}'",
            filename);
        return false;
    }
    m_pendingScreenshotFilename = filename;
    TD_LOG_INFO(
        "[DX12Renderer] Screenshot queued for next frame: '{}'", filename);
    return true;
}

void DX12Renderer::showCursor(bool show) {
    if (show) SDL_ShowCursor();
    else SDL_HideCursor();
}

void DX12Renderer::setCursorVisible(bool visible) {
    if (visible) SDL_ShowCursor();
    else SDL_HideCursor();
}

} // namespace engine
