#include "DrawFunc.h"
#include "engine/renderer/runtime/DX12Renderer.h"
#include "../../../core/constants/Strings.h"
#include "engine/texture/TextureManager.h"

namespace gui::draw {

// ComboBox image draw: background + border using WND draw data
// Slot 0: background image/color + borderColor
static void drawComboBoxImage(engine::Renderer& renderer, const WinDrawInfo& info,
                               engine::TextureManager& texMgr, int depth) {
    
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    const auto& dd = info.enabledDrawData;

    // Draw background (slot 0)
    if (!dd[0].image.empty() && dd[0].image != NO_IMAGE.data()) {
        drawMappedImage(renderer, texMgr, dd[0].image, dx, dy, dw, dh, COLOR_WHITE);
    }

    // Draw border using borderColor from WND data (slot 0)
    uint32_t borderColor = dd[0].borderColor;
    if (borderColor == 0) borderColor = COLOR_DISABLED_TEXT;
    renderer.drawRect(dx, dy, dw, dh, borderColor);
}

static void drawComboBoxColor(engine::Renderer& renderer, const WinDrawInfo& info,
                               engine::TextureManager& texMgr, int depth) {
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    uint32_t tint = info.enabledDrawData[0].color;
    if (tint == 0 || tint == COLOR_WHITE) tint = COLOR_DEFAULT_TINT;
    renderer.drawQuad(dx, dy, dw, dh, tint);
    renderer.drawRect(dx, dy, dw, dh, COLOR_WHITE);
}

void initComboBoxDrawFuncs() {
    registerDrawFunc("COMBOBOX", drawComboBoxImage, drawComboBoxColor);
}

} // namespace gui::draw
