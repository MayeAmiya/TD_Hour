#include "DrawFunc.h"
#include "engine/renderer/runtime/DX12Renderer.h"
#include "../../../core/constants/Strings.h"
#include "engine/texture/TextureManager.h"

#include <algorithm>

namespace gui::draw {

// ProgressBar: 3-part background (Left/Right/Center) + tiled bar center
// Slots 0-3: background (0=Left, 1=Right, 2=Center)
// Slots 4-7: bar (4=BarLeft, 5=BarRight, 6=BarCenter)
static void drawProgressBarImage(engine::Renderer& renderer, const WinDrawInfo& info,
                                  engine::TextureManager& texMgr, int depth) {
    
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    const auto& dd = info.enabledDrawData;

    // Draw 3-part background
    int bgLeftW = getImageWidth(texMgr, dd[0].image);
    int bgRightW = getImageWidth(texMgr, dd[1].image);
    int bgCenterW = getImageWidth(texMgr, dd[2].image);

    bool hasBgLeft = !dd[0].image.empty() && dd[0].image != NO_IMAGE.data() && bgLeftW > 0;
    bool hasBgRight = !dd[1].image.empty() && dd[1].image != NO_IMAGE.data() && bgRightW > 0;
    bool hasBgCenter = !dd[2].image.empty() && dd[2].image != NO_IMAGE.data() && bgCenterW > 0;

    if (hasBgLeft && hasBgRight && (bgLeftW + bgRightW) <= info.w) {
        float leftEndX = dx + static_cast<float>(bgLeftW);
        float rightStartX = dx + dw - static_cast<float>(bgRightW);
        float centerWidth = rightStartX - leftEndX;

        drawMappedImage(renderer, texMgr, dd[0].image, dx, dy, static_cast<float>(bgLeftW), dh);
        drawMappedImage(renderer, texMgr, dd[1].image, rightStartX, dy, static_cast<float>(bgRightW), dh);

        if (hasBgCenter && centerWidth > 0) {
            float cx = leftEndX;
            while (cx < rightStartX) {
                float pieceW = static_cast<float>(bgCenterW);
                if (cx + pieceW > rightStartX) pieceW = rightStartX - cx;
                drawMappedImage(renderer, texMgr, dd[2].image, cx, dy, pieceW, dh);
                cx += static_cast<float>(bgCenterW);
            }
        }
    }

    // RefCode uses slot 6 for the completed repeating segment and slot 5 for
    // the remaining/de-powered segment. The red slot-0 color in stock WNDs is
    // only a color-draw fallback; painting it when transparent Alpha images
    // are unresolved produces the conspicuous red loading rectangle seen in
    // the modern image path.
    const float progress = std::clamp(info.progress, 0.0f, 1.0f);
    int barCenterW = getImageWidth(texMgr, dd[6].image);
    int barRightW = getImageWidth(texMgr, dd[5].image);
    bool hasBarCenter = !dd[6].image.empty() && dd[6].image != NO_IMAGE.data() && barCenterW > 0;
    bool hasBarRight = !dd[5].image.empty() && dd[5].image != NO_IMAGE.data() && barRightW > 0;

    const float barY = dy + 5.0f;
    const float barH = std::max(0.0f, dh - 10.0f);
    const float barStartX = dx + 10.0f;
    const float barLimitX = dx + std::max(10.0f, dw - 10.0f);
    const float barEndX = barStartX +
        std::max(0.0f, dw - 20.0f) * progress;
    if (hasBarCenter && barH > 0.0f) {
        float cx = barStartX;
        while (cx < barEndX) {
            float pieceW = static_cast<float>(barCenterW);
            if (cx + pieceW > barEndX) pieceW = barEndX - cx;
            drawMappedImage(renderer, texMgr, dd[6].image, cx, barY, pieceW, barH);
            cx += static_cast<float>(barCenterW);
        }
    }
    if (hasBarRight && barH > 0.0f) {
        float cx = barEndX;
        while (cx < barLimitX) {
            float pieceW = static_cast<float>(barRightW);
            if (cx + pieceW > barLimitX) pieceW = barLimitX - cx;
            drawMappedImage(renderer, texMgr, dd[5].image, cx, barY,
                            pieceW, barH);
            cx += static_cast<float>(barRightW);
        }
    }
    if (!hasBarCenter && !hasBarRight && barH > 0.0f) {
        renderer.drawQuad(dx + 2, barY, std::max(0.0f, dw - 4) * progress,
                          barH, 0xFF408040);
    }
}

static void drawProgressBarColor(engine::Renderer& renderer, const WinDrawInfo& info,
                                  engine::TextureManager& texMgr, int depth) {
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);
    uint32_t bgTint = info.enabledDrawData[0].color;
    if (bgTint == 0 || bgTint == COLOR_WHITE) bgTint = COLOR_DEFAULT_TINT;
    renderer.drawQuad(dx, dy, dw, dh, bgTint);
    renderer.drawRect(dx, dy, dw, dh, COLOR_WHITE);
    renderer.drawQuad(dx + 2, dy + 5,
                      std::max(0.0f, dw - 4) * std::clamp(info.progress, 0.0f, 1.0f),
                      dh - 10, 0xFF408040);
}

void initProgressBarDrawFuncs() {
    registerDrawFunc("PROGRESSBAR", drawProgressBarImage, drawProgressBarColor);
}

} // namespace gui::draw
