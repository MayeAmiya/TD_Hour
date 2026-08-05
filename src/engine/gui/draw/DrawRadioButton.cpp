#include "core/container/container_types.h"
#include "DrawFunc.h"
#include "engine/renderer/runtime/DX12Renderer.h"
#include "../../../core/constants/Strings.h"
#include "engine/texture/TextureManager.h"

namespace gui::draw {

// RadioButton: 3-part horizontal (same as PushButton)
// Slots: 0=Left, 1=Middle, 2=Right
static void drawRadioButtonImage(engine::Renderer& renderer, const WinDrawInfo& info,
                                 engine::TextureManager& texMgr, int depth) {
    
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    const auto& dd = info.enabledDrawData;
    const container::String& leftName = dd[0].image;
    const container::String& middleName = dd[1].image;
    const container::String& rightName = dd[2].image;

    int leftW = getImageWidth(texMgr, leftName);
    int rightW = getImageWidth(texMgr, rightName);
    int middleW = getImageWidth(texMgr, middleName);

    bool hasLeft = !leftName.empty() && leftName != NO_IMAGE.data() && leftW > 0;
    bool hasRight = !rightName.empty() && rightName != NO_IMAGE.data() && rightW > 0;
    bool hasMiddle = !middleName.empty() && middleName != NO_IMAGE.data() && middleW > 0;

    if (hasLeft && hasRight && (leftW + rightW) <= info.w) {
        float leftEndX = dx + static_cast<float>(leftW);
        float rightStartX = dx + dw - static_cast<float>(rightW);
        float centerWidth = rightStartX - leftEndX;

        drawMappedImage(renderer, texMgr, leftName, dx, dy, static_cast<float>(leftW), dh);
        drawMappedImage(renderer, texMgr, rightName, rightStartX, dy, static_cast<float>(rightW), dh);

        if (hasMiddle && centerWidth > 0) {
            float cx = leftEndX;
            while (cx < rightStartX) {
                float pieceW = static_cast<float>(middleW);
                if (cx + pieceW > rightStartX) pieceW = rightStartX - cx;
                drawMappedImage(renderer, texMgr, middleName, cx, dy, pieceW, dh);
                cx += static_cast<float>(middleW);
            }
        }
    } else if (hasLeft) {
        drawMappedImage(renderer, texMgr, leftName, dx, dy, dw, dh);
    } else {
        uint32_t tint = dd[0].color;
        if (tint == 0 || tint == COLOR_WHITE) tint = COLOR_DEFAULT_TINT;
        renderer.drawQuad(dx, dy, dw, dh, tint);
        renderer.drawRect(dx, dy, dw, dh, COLOR_WHITE);
    }
}

static void drawRadioButtonColor(engine::Renderer& renderer, const WinDrawInfo& info,
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

void initRadioButtonDrawFuncs() {
    registerDrawFunc("RADIOBUTTON", drawRadioButtonImage, drawRadioButtonColor);
}

} // namespace gui::draw
