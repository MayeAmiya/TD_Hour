#include "core/container/container_types.h"
#include "DrawFunc.h"
#include "../../../core/constants/Strings.h"
#include "../../../core/constants/Colors.h"

namespace gui::draw {

// PushButton: 3-part horizontal (Left cap fixed + Middle tiled + Right cap fixed)
// Slots: 0=Left, 5=Middle, 6=Right
static void drawPushButtonImage(engine::Renderer& renderer, const WinDrawInfo& info,
                                engine::TextureManager& texMgr, int depth) {
    
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    const auto& dd = info.enabledDrawData;
    const container::String& leftName = dd[0].image;
    const container::String& middleName = dd[5].image;
    const container::String& rightName = dd[6].image;

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

        drawMappedImage(renderer, texMgr, leftName,
                       dx, dy, static_cast<float>(leftW), dh);

        drawMappedImage(renderer, texMgr, rightName,
                       rightStartX, dy, static_cast<float>(rightW), dh);

        if (hasMiddle && centerWidth > 0) {
            float cx = leftEndX;
            while (cx < rightStartX) {
                float pieceW = static_cast<float>(middleW);
                if (cx + pieceW > rightStartX)
                    pieceW = rightStartX - cx;
                drawMappedImage(renderer, texMgr, middleName,
                               cx, dy, pieceW, dh);
                cx += static_cast<float>(middleW);
            }
        }
    } else if (hasLeft) {
        drawMappedImage(renderer, texMgr, leftName, dx, dy, dw, dh);
    } else {
        renderer.drawQuad(dx, dy, dw, dh, COLOR_DEFAULT_TINT);
        renderer.drawRect(dx, dy, dw, dh, COLOR_WHITE);
    }
}

static void drawPushButtonColor(engine::Renderer& renderer, const WinDrawInfo& info,
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

void initPushButtonDrawFuncs() {
    registerDrawFunc("PUSHBUTTON", drawPushButtonImage, drawPushButtonColor);
}

} // namespace gui::draw
