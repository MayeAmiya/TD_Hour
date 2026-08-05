#include "DrawFunc.h"
#include "../../../core/constants/Strings.h"
#include "engine/renderer/runtime/Renderer.h"
#include "engine/texture/TextureManager.h"

namespace gui::draw {

// TextEntry: 3-part (LeftEnd + RepeatingCenter tiled + RightEnd)
// Slots: 0=Left, 1=Right, 2=Center
static void drawTextEntryImage(engine::Renderer& renderer, const WinDrawInfo& info,
                                engine::TextureManager& texMgr, int depth) {
    
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    const auto& dd = info.enabledDrawData;
    int leftW = getImageWidth(texMgr, dd[0].image);
    int rightW = getImageWidth(texMgr, dd[1].image);
    int centerW = getImageWidth(texMgr, dd[2].image);

    bool hasLeft = !dd[0].image.empty() && dd[0].image != NO_IMAGE.data() && leftW > 0;
    bool hasRight = !dd[1].image.empty() && dd[1].image != NO_IMAGE.data() && rightW > 0;
    bool hasCenter = !dd[2].image.empty() && dd[2].image != NO_IMAGE.data() && centerW > 0;

    if (hasLeft && hasRight && (leftW + rightW) <= info.w) {
        float leftEndX = dx + static_cast<float>(leftW);
        float rightStartX = dx + dw - static_cast<float>(rightW);
        float centerWidth = rightStartX - leftEndX;

        drawMappedImage(renderer, texMgr, dd[0].image, dx, dy, static_cast<float>(leftW), dh);
        drawMappedImage(renderer, texMgr, dd[1].image, rightStartX, dy, static_cast<float>(rightW), dh);

        if (hasCenter && centerWidth > 0) {
            float cx = leftEndX;
            while (cx < rightStartX) {
                float pieceW = static_cast<float>(centerW);
                if (cx + pieceW > rightStartX) pieceW = rightStartX - cx;
                drawMappedImage(renderer, texMgr, dd[2].image, cx, dy, pieceW, dh);
                cx += static_cast<float>(centerW);
            }
        }
    } else if (hasLeft) {
        drawMappedImage(renderer, texMgr, dd[0].image, dx, dy, dw, dh);
    } else {
        uint32_t tint = dd[0].color;
        if (tint == 0 || tint == COLOR_WHITE) tint = COLOR_DEFAULT_TINT;
        renderer.drawQuad(dx, dy, dw, dh, tint);
        renderer.drawRect(dx, dy, dw, dh, COLOR_WHITE);
    }
}

static void drawTextEntryColor(engine::Renderer& renderer, const WinDrawInfo& info,
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

void initTextEntryDrawFuncs() {
    registerDrawFunc("ENTRYFIELD", drawTextEntryImage, drawTextEntryColor);
}

} // namespace gui::draw
