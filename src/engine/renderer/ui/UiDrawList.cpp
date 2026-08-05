#include "core/container/container_types.h"
#include "UiDrawList.h"
#include "TextureManager.h"

namespace engine {

UiDrawListRenderer::UiDrawListRenderer(UiDrawList& drawList, const Renderer& target)
    : m_drawList(drawList), m_target(target) {
    m_width = target.getWidth();
    m_height = target.getHeight();
}

void UiDrawListRenderer::drawQuad(float x, float y, float w, float h, uint32_t color) {
    m_drawList.add({ .type = UiDrawCommandType::Quad, .x = x, .y = y, .w = w, .h = h, .color = color });
}

void UiDrawListRenderer::drawLine(float startX, float startY,
                                  float endX, float endY, float width,
                                  uint32_t startColor, uint32_t endColor) {
    m_drawList.add({.type = UiDrawCommandType::Line,
                    .x = startX, .y = startY, .w = width,
                    .endX = endX, .endY = endY,
                    .color = startColor, .endColor = endColor});
}

void UiDrawListRenderer::drawText(const container::String& text, float x, float y, uint32_t color) {
    m_drawList.add({ .type = UiDrawCommandType::Text, .text = text, .x = x, .y = y, .color = color });
}

void UiDrawListRenderer::drawText(Font* font, const container::String& text, float x, float y, uint32_t color) {
    m_drawList.add({ .type = UiDrawCommandType::Text, .font = font, .text = text, .x = x, .y = y, .color = color });
}

void UiDrawListRenderer::drawRect(float x, float y, float w, float h, uint32_t color) {
    m_drawList.add({ .type = UiDrawCommandType::Rect, .x = x, .y = y, .w = w, .h = h, .color = color });
}

void UiDrawListRenderer::drawBorder(float x, float y, float w, float h, uint32_t color, int thickness) {
    m_drawList.add({ .type = UiDrawCommandType::Border, .x = x, .y = y, .w = w, .h = h,
                    .thickness = thickness, .color = color });
}

void UiDrawListRenderer::drawTexture(const RawTexture* texture, float x, float y, float w, float h, uint32_t tint) {
    auto lease = texture ? texture->lease() : nullptr;
    m_drawList.add({ .type = UiDrawCommandType::Texture,
                    .texture = lease ? lease.get() : texture,
                    .textureLease = std::move(lease),
                    .x = x, .y = y, .w = w, .h = h, .color = tint });
}

void UiDrawListRenderer::drawTextureRegion(const RawTexture* texture,
                                           int srcLeft, int srcTop, int srcRight, int srcBottom,
                                           int srcTextureWidth, int srcTextureHeight,
                                           float dstX, float dstY, float dstW, float dstH, uint32_t tint) {
    auto lease = texture ? texture->lease() : nullptr;
    m_drawList.add({ .type = UiDrawCommandType::TextureRegion,
                    .texture = lease ? lease.get() : texture,
                    .textureLease = std::move(lease),
                    .x = dstX, .y = dstY, .w = dstW, .h = dstH,
                    .srcLeft = srcLeft, .srcTop = srcTop, .srcRight = srcRight, .srcBottom = srcBottom,
                    .srcTextureWidth = srcTextureWidth, .srcTextureHeight = srcTextureHeight, .color = tint });
}

void UiRenderer::submit(Renderer& renderer, const UiDrawList& drawList) const {
    for (const auto& command : drawList.commands()) {
        switch (command.type) {
        case UiDrawCommandType::Quad:
            renderer.drawQuad(command.x, command.y, command.w, command.h, command.color);
            break;
        case UiDrawCommandType::Line:
            renderer.drawLine(command.x, command.y,
                              command.endX, command.endY, command.w,
                              command.color, command.endColor);
            break;
        case UiDrawCommandType::Rect:
            renderer.drawRect(command.x, command.y, command.w, command.h, command.color);
            break;
        case UiDrawCommandType::Border:
            renderer.drawBorder(command.x, command.y, command.w, command.h, command.color, command.thickness);
            break;
        case UiDrawCommandType::Texture:
            renderer.drawTexture(command.texture, command.x, command.y, command.w, command.h, command.color);
            break;
        case UiDrawCommandType::TextureRegion:
            renderer.drawTextureRegion(command.texture,
                                       command.srcLeft, command.srcTop, command.srcRight, command.srcBottom,
                                       command.srcTextureWidth, command.srcTextureHeight,
                                       command.x, command.y, command.w, command.h, command.color);
            break;
        case UiDrawCommandType::Text:
            if (command.font) {
                renderer.drawText(command.font, command.text, command.x, command.y, command.color);
            } else {
                renderer.drawText(command.text, command.x, command.y, command.color);
            }
            break;
        }
    }
}

} // namespace engine
