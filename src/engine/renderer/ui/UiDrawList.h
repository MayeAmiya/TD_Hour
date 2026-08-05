#pragma once

#include "core/container/container_types.h"

#include "Renderer.h"
#include <optional>
namespace engine {

class Font;

enum class UiDrawCommandType : uint8_t {
    Quad,
    Line,
    Rect,
    Border,
    Texture,
    TextureRegion,
    Text,
};

struct UiDrawCommand {
    UiDrawCommandType type = UiDrawCommandType::Quad;
    const RawTexture* texture = nullptr;
    container::SharedPtr<const RawTexture> textureLease;
    Font* font = nullptr;
    container::String text;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float endX = 0.0f;
    float endY = 0.0f;
    int srcLeft = 0;
    int srcTop = 0;
    int srcRight = 0;
    int srcBottom = 0;
    int srcTextureWidth = 0;
    int srcTextureHeight = 0;
    int thickness = 1;
    uint32_t color = COLOR_WHITE;
    uint32_t endColor = COLOR_WHITE;
};

struct UiOverlayRect final {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct UiLoadingPresentationStamp final {
    uint64_t loadingRevision = 0;
    uint64_t sessionRevision = 0;
};

struct UiAudioPresentationReleaseStamp final {
    uint64_t presentationEpoch = 0;
};

class UiDrawList {
public:
    void clear() {
        m_commands.clear();
        m_tacticalRadarPanel.reset();
        m_loadingPresentation.reset();
        m_audioPresentationRelease.reset();
    }
    const container::Vector<UiDrawCommand>& commands() const { return m_commands; }
    void setTacticalRadarPanel(UiOverlayRect panel) noexcept {
        m_tacticalRadarPanel = panel;
    }
    [[nodiscard]] const std::optional<UiOverlayRect>&
    tacticalRadarPanel() const noexcept {
        return m_tacticalRadarPanel;
    }
    void setLoadingPresentationStamp(
        UiLoadingPresentationStamp stamp) noexcept {
        m_loadingPresentation = stamp;
    }
    [[nodiscard]] const std::optional<UiLoadingPresentationStamp>&
    loadingPresentationStamp() const noexcept {
        return m_loadingPresentation;
    }
    void setAudioPresentationReleaseStamp(
        UiAudioPresentationReleaseStamp stamp) noexcept {
        m_audioPresentationRelease = stamp;
    }
    [[nodiscard]] const std::optional<UiAudioPresentationReleaseStamp>&
    audioPresentationReleaseStamp() const noexcept {
        return m_audioPresentationRelease;
    }

    void add(UiDrawCommand command) { m_commands.push_back(std::move(command)); }

private:
    container::Vector<UiDrawCommand> m_commands;
    std::optional<UiOverlayRect> m_tacticalRadarPanel;
    std::optional<UiLoadingPresentationStamp> m_loadingPresentation;
    std::optional<UiAudioPresentationReleaseStamp>
        m_audioPresentationRelease;
};

class UiDrawListRenderer final : public Renderer {
public:
    UiDrawListRenderer(UiDrawList& drawList, const Renderer& target);

    void drawQuad(float x, float y, float w, float h, uint32_t color) override;
    void drawLine(float startX, float startY, float endX, float endY,
                  float width, uint32_t startColor, uint32_t endColor) override;
    void drawText(const container::String& text, float x, float y, uint32_t color) override;
    void drawText(Font* font, const container::String& text, float x, float y, uint32_t color) override;
    void drawRect(float x, float y, float w, float h, uint32_t color) override;
    void drawBorder(float x, float y, float w, float h, uint32_t color, int thickness) override;
    void drawTexture(const RawTexture* texture, float x, float y, float w, float h, uint32_t tint) override;
    void drawTextureRegion(const RawTexture* texture, int srcLeft, int srcTop, int srcRight, int srcBottom,
                           int srcTextureWidth, int srcTextureHeight,
                           float dstX, float dstY, float dstW, float dstH, uint32_t tint) override;

    float getScaleX() const override { return m_target.getScaleX(); }
    float getScaleY() const override { return m_target.getScaleY(); }
    float getUiCanvasWidth() const override {
        return m_target.getUiCanvasWidth();
    }
    float getUiCanvasHeight() const override {
        return m_target.getUiCanvasHeight();
    }
    float getUiAuthoredWidth() const override {
        return m_target.getUiAuthoredWidth();
    }
    float getUiAuthoredHeight() const override {
        return m_target.getUiAuthoredHeight();
    }
    float getUiViewportWidth() const override {
        return m_target.getUiViewportWidth();
    }
    float getUiViewportHeight() const override {
        return m_target.getUiViewportHeight();
    }
    float getUiCanvasOffsetX() const override {
        return m_target.getUiCanvasOffsetX();
    }
    float getUiCanvasOffsetY() const override {
        return m_target.getUiCanvasOffsetY();
    }
    float getPresentationScaleX() const override {
        return m_target.getPresentationScaleX();
    }
    float getPresentationScaleY() const override {
        return m_target.getPresentationScaleY();
    }
    void getWindowSize(int& w, int& h) const override { m_target.getWindowSize(w, h); }

private:
    UiDrawList& m_drawList;
    const Renderer& m_target;
};

class UiRenderer {
public:
    void submit(Renderer& renderer, const UiDrawList& drawList) const;
};

} // namespace engine
