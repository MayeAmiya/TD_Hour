#include "core/container/container_types.h"
#include "DrawFunc.h"
#include "engine/renderer/runtime/DX12Renderer.h"
#include "../../../core/constants/Strings.h"
#include "engine/texture/TextureManager.h"

namespace gui::draw {

// CheckBox: single box image at offset
// Slots: 0=Background, 1=UncheckedBox, 2=CheckedBox
static void drawCheckBoxImage(engine::Renderer& renderer, const WinDrawInfo& info,
                               engine::TextureManager& texMgr, int depth) {
    
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    const auto& dd = info.enabledDrawData;

    if (!dd[0].image.empty() && dd[0].image != NO_IMAGE.data()) {
        drawMappedImage(renderer, texMgr, dd[0].image, dx, dy, dw, dh);
    }

    // Slot 1 = UncheckedBox, Slot 2 = CheckedBox
    const container::String& boxName = info.checked ? dd[2].image : dd[1].image;
    if (!boxName.empty() && boxName != NO_IMAGE.data()) {
        float boxSize = dh - 6.0f;
        if (boxSize > 0)
            drawMappedImage(renderer, texMgr, boxName, dx, dy + 3.0f, boxSize, boxSize);
    } else {
        float boxX = dx;
        float boxY = dy + dh / 3.0f;
        float boxW = dh / 3.0f;
        float boxH = dh / 3.0f;
        renderer.drawRect(boxX, boxY, boxW, boxH, dd[0].color != 0 ? dd[0].color : COLOR_WHITE);
        // Draw check mark if checked
        if (info.checked) {
            float cx = boxX + boxW * 0.2f;
            float cy = boxY + boxH * 0.5f;
            renderer.drawRect(cx, cy - 1.0f, boxW * 0.6f, 2.0f, COLOR_WHITE);
            renderer.drawRect(cx + boxW * 0.1f, cy + boxW * 0.2f, boxW * 0.4f, 2.0f, COLOR_WHITE);
        }
    }
}

static void drawCheckBoxColor(engine::Renderer& renderer, const WinDrawInfo& info,
                               engine::TextureManager& texMgr, int depth) {
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);
    uint32_t tint = info.enabledDrawData[0].color;
    if (tint == 0 || tint == COLOR_WHITE) tint = COLOR_DEFAULT_TINT;
    renderer.drawQuad(dx, dy, dw, dh, tint);
    renderer.drawRect(dx, dy, dw, dh, COLOR_WHITE);
    float boxW = dh / 3.0f;
    renderer.drawRect(dx, dy + dh / 3.0f, boxW, boxW, COLOR_WHITE);
}

void initCheckBoxDrawFuncs() {
    registerDrawFunc("CHECKBOX", drawCheckBoxImage, drawCheckBoxColor);
}

} // namespace gui::draw
