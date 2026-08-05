#pragma once

#include "core/container/container_types.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include "presentation/render/PresentationDefaults.h"
#include "core/constants/Colors.h"

// Forward declare
struct SDL_Texture;
struct SDL_Renderer;
namespace engine { struct RawTexture; }

namespace engine {

class Font;

// Rendering backend interface
class Renderer {
public:
    virtual ~Renderer() = default;

    virtual bool init(uint32_t width, uint32_t height, bool fullscreen) { 
        m_width = width;
        m_height = height;
        m_fullscreen = fullscreen;
        return true; 
    }

    virtual void shutdown() {}
    virtual void beginFrame() {}
    virtual void endFrame() {}

    virtual void resize(uint32_t width, uint32_t height) {
        m_width = width;
        m_height = height;
    }

    uint32_t getWidth() const { return m_width.load(std::memory_order_acquire); }
    uint32_t getHeight() const { return m_height.load(std::memory_order_acquire); }
    bool isFullscreen() const { return m_fullscreen.load(std::memory_order_acquire); }

    virtual void setVirtualResolution(int vw, int vh) {}
    virtual float getScaleX() const { return 1.0f; }
    virtual float getScaleY() const { return 1.0f; }
    virtual float getUiCanvasWidth() const {
        return static_cast<float>(presentation_defaults::AUTHORED_WIDTH);
    }
    virtual float getUiCanvasHeight() const {
        return static_cast<float>(presentation_defaults::AUTHORED_HEIGHT);
    }
    virtual float getUiAuthoredWidth() const {
        return static_cast<float>(presentation_defaults::AUTHORED_WIDTH);
    }
    virtual float getUiAuthoredHeight() const {
        return static_cast<float>(presentation_defaults::AUTHORED_HEIGHT);
    }
    virtual float getUiViewportWidth() const { return getUiCanvasWidth(); }
    virtual float getUiViewportHeight() const { return getUiCanvasHeight(); }
    virtual float getUiCanvasOffsetX() const { return 0.0f; }
    virtual float getUiCanvasOffsetY() const { return 0.0f; }
    float layoutUiX(float authoredX, float elementWidth) const {
        const float extra = std::max(
            0.0f, getUiCanvasWidth() - getUiAuthoredWidth());
        const float center = authoredX + elementWidth * 0.5f;
        const float authored = std::max(getUiAuthoredWidth(), 1.0f);
        if (center < authored / 3.0f) return authoredX;
        if (center > authored * (2.0f / 3.0f)) return authoredX + extra;
        return authoredX + extra * 0.5f;
    }
    float layoutUiY(float authoredY, float elementHeight) const {
        const float extra = std::max(
            0.0f, getUiCanvasHeight() - getUiAuthoredHeight());
        const float center = authoredY + elementHeight * 0.5f;
        const float authored = std::max(getUiAuthoredHeight(), 1.0f);
        if (center < authored / 3.0f) return authoredY;
        if (center > authored * (2.0f / 3.0f)) return authoredY + extra;
        return authoredY + extra * 0.5f;
    }
    float windowToUiX(float physicalX) const {
        const float scale = std::max(getScaleX(), 0.0001f);
        return physicalX / scale;
    }
    float windowToUiY(float physicalY) const {
        const float scale = std::max(getScaleY(), 0.0001f);
        return physicalY / scale;
    }
    // UI geometry may use independent X/Y output scales. Glyph shapes do
    // not: compensate their local metrics so the final physical glyph uses
    // one uniform scale while its anchor still follows the stretched layout.
    virtual float getPresentationScaleX() const {
        const float width = static_cast<float>(getWidth());
        const float canvas = getUiCanvasWidth();
        return width > 0.0f && canvas > 0.0f ? width / canvas : 1.0f;
    }
    virtual float getPresentationScaleY() const {
        const float height = static_cast<float>(getHeight());
        const float canvas = getUiCanvasHeight();
        return height > 0.0f && canvas > 0.0f ? height / canvas : 1.0f;
    }
    float getTextLayoutScaleX() const {
        const float x = std::max(getPresentationScaleX(), 0.0001f);
        const float y = std::max(getPresentationScaleY(), 0.0001f);
        return std::min(x, y) / x;
    }
    float getTextLayoutScaleY() const {
        const float x = std::max(getPresentationScaleX(), 0.0001f);
        const float y = std::max(getPresentationScaleY(), 0.0001f);
        return std::min(x, y) / y;
    }

    virtual void winRepaint() {}
    virtual void getWindowSize(int& w, int& h) const {
        w = static_cast<int>(m_width.load(std::memory_order_acquire));
        h = static_cast<int>(m_height.load(std::memory_order_acquire));
    }

    virtual void* getSDLRenderer() const { return nullptr; }

    virtual uint32_t createTexture(const container::String& filename) { return 0; }
    virtual void destroyTexture(uint32_t handle) {}

    // Draw primitives — RawTexture* replaces SDL_Texture*
    virtual void drawQuad(float x, float y, float w, float h, uint32_t color) {}
    virtual void drawLine(float startX, float startY, float endX, float endY,
                          float width, uint32_t startColor, uint32_t endColor) {}
    virtual void drawText(const container::String& text, float x, float y, uint32_t color) {}
    virtual void drawText(Font* font, const container::String& text, float x, float y, uint32_t color) {}
    virtual void drawRect(float x, float y, float w, float h, uint32_t color) {}
    virtual void drawBorder(float x, float y, float w, float h, uint32_t color, int thickness = 1) {}

    virtual void drawTexture(const RawTexture* tex, float x, float y, float w, float h, uint32_t tint = COLOR_WHITE) {}
    virtual void drawTextureRegion(const RawTexture* tex, int srcLeft, int srcTop, int srcRight, int srcBottom,
                                   int srcTexW, int srcTexH,
                                   float dstX, float dstY, float dstW, float dstH,
                                   uint32_t tint) {}

    static Renderer& instance();
    static void setInstance(container::UniquePtr<Renderer> renderer);

protected:
    std::atomic<uint32_t> m_width{engine::presentation_defaults::VIRTUAL_WIDTH};
    std::atomic<uint32_t> m_height{engine::presentation_defaults::VIRTUAL_HEIGHT};
    std::atomic<bool> m_fullscreen{false};

    static container::UniquePtr<Renderer> s_instance;
};

class StubRenderer : public Renderer {
public:
    ~StubRenderer() override = default;
};

} // namespace engine
