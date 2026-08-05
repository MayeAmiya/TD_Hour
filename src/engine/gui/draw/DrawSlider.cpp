#include "DrawFunc.h"
#include "engine/renderer/runtime/DX12Renderer.h"
#include "../../../core/constants/Strings.h"
#include "engine/texture/TextureManager.h"

namespace gui::draw {

// Original engine constants (Gadget.h)
static constexpr int HORIZONTAL_SLIDER_THUMB_WIDTH = 13;
static constexpr int HORIZONTAL_SLIDER_THUMB_HEIGHT = 16;
static constexpr int HORIZONTAL_SLIDER_THUMB_POSITION = HORIZONTAL_SLIDER_THUMB_HEIGHT * 2 / 3; // = 10

// ── Horizontal Slider Track ───────────────────────────────────────────────
// Ported from W3DGadgetHorizontalSliderImageDrawA()
// Track is at the BOTTOM of the control area.
// Two-tone: left of thumb = hilite images, right of thumb = enabled images.
// Slots: 0=Left, 1=Right, 2=Center, 3=SmallCenter
static void drawSliderImage(engine::Renderer& renderer, const WinDrawInfo& info,
                             engine::TextureManager& texMgr, int depth) {
    
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    const auto& dd = info.enabledDrawData;
    const auto& hdd = info.hiliteDrawData;

    // Get image sizes for the ends (using enabled images as reference)
    int leftW = getImageWidth(texMgr, dd[0].image);
    int leftH = getImageHeight(texMgr, dd[0].image);
    int rightW = getImageWidth(texMgr, dd[1].image);
    int centerW = getImageWidth(texMgr, dd[2].image);
    int smallCenterW = getImageWidth(texMgr, dd[3].image);

    // Sanity check - need at least left and right images
    if (leftW <= 0 || rightW <= 0) {
        // No images: draw colored bar filling the FULL control height
        uint32_t tint = dd[0].color;
        if (tint == 0 || tint == COLOR_WHITE) tint = COLOR_DEFAULT_TINT;
        renderer.drawQuad(dx, dy, dw, dh, tint);
        renderer.drawRect(dx, dy, dw, dh, COLOR_WHITE);
        return;
    }

    // Track position (from original code):
    // leftEnd.y = origin.y + size.y  (bottom edge)
    // rightStart.y = origin.y + size.y - leftSize.y  (above bottom by left image height)
    float leftEndX = dx + static_cast<float>(leftW);
    float leftEndY = dy + dh;
    float rightStartX = dx + dw - static_cast<float>(rightW);
    float rightStartY = dy + dh - static_cast<float>(leftH);
    float trackH = leftEndY - rightStartY;

    // Draw center repeating pieces.  A zero-width center image (missing or
    // unresolved mapped image) would advance cx by 0 and spin the render thread
    // forever; the smallCenter loop below already guards its own width.
    float cx = leftEndX;
    if (centerW > 0) {
        while (cx < rightStartX) {
            float pieceW = static_cast<float>(centerW);
            if (cx + pieceW > rightStartX) pieceW = rightStartX - cx;
            drawMappedImage(renderer, texMgr, dd[2].image, cx, rightStartY, pieceW, trackH);
            cx += static_cast<float>(centerW);
        }
    }

    // Draw small center pieces to fill remaining gap
    if (smallCenterW > 0) {
        cx = leftEndX;
        while (cx < rightStartX) {
            float pieceW = static_cast<float>(smallCenterW);
            if (cx + pieceW > rightStartX) pieceW = rightStartX - cx;
            drawMappedImage(renderer, texMgr, dd[3].image, cx, rightStartY, pieceW, trackH);
            cx += static_cast<float>(smallCenterW);
        }
    }

    // Draw left end
    drawMappedImage(renderer, texMgr, dd[0].image, dx, rightStartY, static_cast<float>(leftW), trackH);

    // Draw right end
    drawMappedImage(renderer, texMgr, dd[1].image, rightStartX, rightStartY, static_cast<float>(rightW), trackH);
}

static void drawSliderColor(engine::Renderer& renderer, const WinDrawInfo& info,
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

void initSliderDrawFuncs() {
    registerDrawFunc("HORZSLIDER", drawSliderImage, drawSliderColor);
    registerDrawFunc("VERTSLIDER", drawSliderImage, drawSliderColor);
}

} // namespace gui::draw
