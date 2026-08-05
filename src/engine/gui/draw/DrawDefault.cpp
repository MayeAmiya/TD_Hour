#include "DrawFunc.h"
#include "engine/renderer/runtime/DX12Renderer.h"
#include "engine/texture/TextureManager.h"
#include "../../../core/constants/Strings.h"
#include "../../../core/constants/Colors.h"

namespace gui::draw {

static void drawDefaultImage(engine::Renderer& renderer, const WinDrawInfo& info,
                             engine::TextureManager& texMgr, int depth) {
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    if (!info.enabledDrawData[0].image.empty() && info.enabledDrawData[0].image != NO_IMAGE.data()) {
        
        auto result = texMgr.findMappedImage(info.enabledDrawData[0].image);
        if (result.found && result.texture) {
            renderer.drawTextureRegion(result.texture,
                                   result.left, result.top, result.right, result.bottom,
                                   result.texW, result.texH,
                                   dx, dy, dw, dh, COLOR_WHITE);
            return;
        }
    }

    // Fallback: draw background color if present (skip fully transparent)
    uint32_t tint = info.enabledDrawData[0].color;
    if (tint != 0 && tint != COLOR_WHITE) {
        renderer.drawQuad(dx, dy, dw, dh, tint);
    }
}

static void drawDefaultColor(engine::Renderer& renderer, const WinDrawInfo& info,
                             engine::TextureManager& texMgr, int depth) {
    float dx = static_cast<float>(info.x);
    float dy = static_cast<float>(info.y);
    float dw = static_cast<float>(info.w);
    float dh = static_cast<float>(info.h);

    uint32_t tint = info.enabledDrawData[0].color;
    if (tint != 0 && tint != COLOR_WHITE) {
        renderer.drawQuad(dx, dy, dw, dh, tint);
        renderer.drawRect(dx, dy, dw, dh, COLOR_WHITE);
    }
}

void initDefaultDrawFuncs() {
    registerDrawFunc("TABCONTROL", drawDefaultImage, drawDefaultColor);
    registerDrawFunc("USER", drawDefaultImage, drawDefaultColor);

    // STATICTEXT: no background — text is rendered separately by Widget
    auto noopFunc = [](engine::Renderer&, const WinDrawInfo&, engine::TextureManager&, int) {};
    registerDrawFunc("STATICTEXT", noopFunc, noopFunc);

    // ROOT: virtual container, never drawn
    registerDrawFunc("ROOT", noopFunc, noopFunc);
}

} // namespace gui::draw
