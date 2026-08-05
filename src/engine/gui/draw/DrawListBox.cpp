#include "DrawFunc.h"
#include "engine/renderer/runtime/DX12Renderer.h"
#include "../../../core/constants/Strings.h"
#include "engine/texture/TextureManager.h"

namespace gui::draw {

// ListBox image draw: background image + 3-part selected item highlight
// Slots: 0=Background, 1=SelectedLeft, 2=SelectedRight, 3=SelectedCenter, 4=SelectedSmallCenter
// The ListBox also contains up/down buttons and a slider as child windows
static void drawListBoxImage(engine::Renderer& renderer, const WinDrawInfo& info,
                              engine::TextureManager& texMgr, int depth) {
    
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    const auto& dd = info.enabledDrawData;

    // Draw background image (slot 0)
    if (!dd[0].image.empty() && dd[0].image != NO_IMAGE.data()) {
        drawMappedImage(renderer, texMgr, dd[0].image, dx, dy, dw, dh, dd[0].color);
    } else {
        // Fallback: dark background
        renderer.drawQuad(dx, dy, dw, dh, COLOR_SELECTION_HIGHLIGHT);
        renderer.drawRect(dx, dy, dw, dh, COLOR_THUMB_DEFAULT);
    }
}

static void drawListBoxColor(engine::Renderer& renderer, const WinDrawInfo& info,
                              engine::TextureManager& texMgr, int depth) {
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    renderer.drawQuad(dx, dy, dw, dh, COLOR_SELECTION_HIGHLIGHT);
    renderer.drawRect(dx, dy, dw, dh, COLOR_THUMB_DEFAULT);
}

void initListBoxDrawFuncs() {
    registerDrawFunc("SCROLLLISTBOX", drawListBoxImage, drawListBoxColor);
}

} // namespace gui::draw
