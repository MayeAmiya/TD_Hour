#include "core/container/container_types.h"
#include "InGameDrawCallbacks.h"

#include "ControlBarSchemeRuntime.h"
#include "DrawCallbackRegistry.h"
#include "DrawFunc.h"
#include "Widget.h"
#include "presentation/render/PresentationDefaults.h"
#include "../../../core/constants/Paths.h"
#include "../../../core/constants/Strings.h"
#include "../core/WinInstanceData.h"

#include <algorithm>
#include <cmath>
namespace gui::ingame {
namespace {

bool hasImage(const container::String& image) {
    return !image.empty() && image != NO_IMAGE.data();
}

const WndParser::DrawData* activeDrawData(const Widget& widget) {
    const auto& data = widget.data();
    if (widget.usesOverlayStates()) {
        // Legacy overlay-state buttons always draw the enabled cameo as their
        // base. Disabled, hovered and selected appearance is applied below;
        // selecting DisabledDrawData here would retain stale imagery from the
        // previously selected unit and bypass the automatic state treatment.
        return data.enabledDrawData;
    }
    if ((data.status & WIN_STATUS_ENABLED) == 0) {
        return data.disabledDrawData;
    }
    if (widget.isHovered()) {
        for (const auto& slot : data.hiliteDrawData) {
            if (hasImage(slot.image)) return data.hiliteDrawData;
        }
    }
    return data.enabledDrawData;
}

uint32_t automaticButtonTint(const Widget& widget) noexcept {
    if (!widget.usesOverlayStates() || widget.isEnabled() ||
        (widget.data().status & WIN_STATUS_NOT_READY) != 0) {
        return COLOR_WHITE;
    }
    // The original renderer uses a grayscale draw mode here. The retained UI
    // image shader currently exposes color multiplication only, so use the
    // original ALWAYS_COLOR multiplier and a stronger neutral multiplier for
    // ordinary disabled cameos. This preserves alpha and makes authoritative
    // availability visually unambiguous without requiring duplicate artwork.
    return (widget.data().status & WIN_STATUS_ALWAYS_COLOR) != 0
        ? 0xff909090u : 0xff707070u;
}

bool drawOneImage(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    const auto* drawData = activeDrawData(widget);
    if (!hasImage(drawData[0].image)) return false;

    return draw::drawMappedImage(renderer, texMgr, drawData[0].image,
                                 static_cast<float>(widget.x()),
                                 static_cast<float>(widget.y()),
                                 static_cast<float>(widget.width()),
                                 static_cast<float>(widget.height()),
                                 automaticButtonTint(widget));
}

bool drawThreePartButton(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    const auto* drawData = activeDrawData(widget);
    const container::String& leftName = drawData[0].image;
    const container::String& middleName = drawData[5].image;
    const container::String& rightName = drawData[6].image;

    const int leftW = draw::getImageWidth(texMgr, leftName);
    const int middleW = draw::getImageWidth(texMgr, middleName);
    const int rightW = draw::getImageWidth(texMgr, rightName);
    if (!hasImage(leftName) || !hasImage(middleName) || !hasImage(rightName) ||
        leftW <= 0 || middleW <= 0 || rightW <= 0) {
        return false;
    }

    const float x = static_cast<float>(widget.x());
    const float y = static_cast<float>(widget.y());
    const float w = static_cast<float>(widget.width());
    const float h = static_cast<float>(widget.height());
    const uint32_t tint = automaticButtonTint(widget);

    if (leftW + rightW >= widget.width()) {
        const float halfW = w * 0.5f;
        draw::drawMappedImage(renderer, texMgr, leftName, x, y, halfW, h, tint);
        draw::drawMappedImage(renderer, texMgr, rightName, x + halfW, y, w - halfW, h, tint);
        return true;
    }

    const float leftEndX = x + static_cast<float>(leftW);
    const float rightStartX = x + w - static_cast<float>(rightW);

    float cx = leftEndX;
    while (cx < rightStartX) {
        const float pieceW = std::min(static_cast<float>(middleW), rightStartX - cx);
        draw::drawMappedImage(renderer, texMgr, middleName, cx, y, pieceW, h, tint);
        cx += static_cast<float>(middleW);
    }

    draw::drawMappedImage(renderer, texMgr, leftName, x, y, static_cast<float>(leftW), h, tint);
    draw::drawMappedImage(renderer, texMgr, rightName, rightStartX, y, static_cast<float>(rightW), h, tint);
    return true;
}

bool drawCommandBarBackground(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    (void)widget;
    const int offsetY = ControlBarSchemeRuntime::instance().drawOffsetY();

    container::Vector<ControlBarSchemeImagePart> backgroundParts;
    for (const auto& part : ControlBarSchemeRuntime::instance().imageParts()) {
        if (part.layer >= 4) {
            backgroundParts.push_back(part);
        }
    }

    std::sort(backgroundParts.begin(), backgroundParts.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.layer > rhs.layer;
              });

    bool drewAny = false;
    for (const auto& part : backgroundParts) {
        const bool fullWidthPaving = part.layer == 4 && part.x == 0 &&
            part.w >= engine::presentation_defaults::AUTHORED_WIDTH;
        const float x = fullWidthPaving
            ? 0.0f
            : renderer.layoutUiX(
                  static_cast<float>(part.x), static_cast<float>(part.w));
        const float y = renderer.layoutUiY(
            static_cast<float>(part.y + offsetY),
            static_cast<float>(part.h));
        drewAny |= draw::drawMappedImage(
            renderer, texMgr, part.imageName, x, y,
            fullWidthPaving ? renderer.getUiCanvasWidth()
                            : static_cast<float>(part.w),
            static_cast<float>(part.h));
    }
    return drewAny;
}

bool drawCommandBarForeground(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    (void)widget;
    const int offsetY = ControlBarSchemeRuntime::instance().drawOffsetY();

    container::Vector<ControlBarSchemeImagePart> foregroundParts;
    for (const auto& part : ControlBarSchemeRuntime::instance().imageParts()) {
        if (part.layer < 4) {
            foregroundParts.push_back(part);
        }
    }

    std::sort(foregroundParts.begin(), foregroundParts.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.layer > rhs.layer;
              });

    bool drewAny = false;
    for (const auto& part : foregroundParts) {
        drewAny |= draw::drawMappedImage(
            renderer, texMgr, part.imageName,
            renderer.layoutUiX(
                static_cast<float>(part.x), static_cast<float>(part.w)),
            renderer.layoutUiY(
                static_cast<float>(part.y + offsetY),
                static_cast<float>(part.h)),
            static_cast<float>(part.w), static_cast<float>(part.h));
    }
    return drewAny;
}

bool drawCommandBarGrid(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    if ((widget.data().status & WIN_STATUS_IMAGE) != 0 && drawOneImage(widget, renderer, texMgr)) {
        return true;
    }

    const float x = static_cast<float>(widget.x());
    const float y = static_cast<float>(widget.y());
    const float w = static_cast<float>(widget.width());
    const float h = static_cast<float>(widget.height());
    const uint32_t fillColor = widget.data().enabledDrawData[0].color;
    const uint32_t borderColor = ControlBarSchemeRuntime::instance().commandBarBorderColor();

    if ((fillColor >> 24) != 0) {
        renderer.drawQuad(x, y, w, h, fillColor);
    }
    renderer.drawRect(x, y, w, h, borderColor);
    renderer.drawQuad(x, y + h * 0.333f, w, 1.0f, borderColor);
    renderer.drawQuad(x, y + h * 0.666f, w, 1.0f, borderColor);
    renderer.drawQuad(x + w * 0.333f, y, 1.0f, h, borderColor);
    renderer.drawQuad(x + w * 0.666f, y, 1.0f, h, borderColor);
    return true;
}

bool drawCommandBarTop(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    (void)widget;
    (void)renderer;
    (void)texMgr;
    return true;
}

bool drawRightHud(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    const container::String& image = ControlBarSchemeRuntime::instance().rightHudImage();
    if (image.empty()) return false;

    return draw::drawMappedImage(renderer, texMgr, image,
                                 static_cast<float>(widget.x()),
                                 static_cast<float>(widget.y()),
                                 static_cast<float>(widget.width()),
                                 static_cast<float>(widget.height()));
}

bool drawLeftHud(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    (void)widget;
    (void)renderer;
    (void)texMgr;
    return true;
}

bool drawGadgetPushButtonImage(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    if (!drawThreePartButton(widget, renderer, texMgr)) {
        drawOneImage(widget, renderer, texMgr);
    }
    return true;
}

bool drawPower(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    const ControlBarPowerMeterState power =
        ControlBarSchemeRuntime::instance().powerMeterState();
    const container::String pointImage =
        power.consumption > power.production
            ? "PowerPointR"
            : power.consumption > power.production - power.yellowRange
                ? "PowerPointY"
                : "PowerPointG";
    constexpr container::StringView kSliderImage = "PowerBarSlider";
    const int pointWidth = draw::getImageWidth(texMgr, pointImage);
    const int sliderWidth = draw::getImageWidth(
        texMgr, container::String{kSliderImage});
    const int sliderHeight = draw::getImageHeight(
        texMgr, container::String{kSliderImage});
    if (pointWidth <= 0 || sliderWidth <= 0 || sliderHeight <= 0 ||
        widget.width() <= 0 || widget.height() <= 0) {
        return true;
    }

    const auto logarithmicX = [&](float value) {
        if (!std::isfinite(value) || value <= 1.0f) return 0.0f;
        const float intervals = std::log(value) /
            std::log(static_cast<float>(std::max(2, power.logarithmicBase)));
        return std::clamp(
            intervals * static_cast<float>(widget.width()) /
                std::max(0.01f, power.intervals),
            0.0f, static_cast<float>(widget.width()));
    };
    const float x = static_cast<float>(widget.x());
    const float y = static_cast<float>(widget.y());
    const float h = static_cast<float>(widget.height());
    const float productionWidth = logarithmicX(
        static_cast<float>(power.production));
    for (float offset = 0.0f; offset < productionWidth;
         offset += static_cast<float>(pointWidth)) {
        const float pieceWidth = std::min(
            static_cast<float>(pointWidth), productionWidth - offset);
        static_cast<void>(draw::drawMappedImage(
            renderer, texMgr, pointImage, x + offset, y, pieceWidth, h));
    }

    const float needleValue = power.consumption == 1
        ? 1.5f : static_cast<float>(power.consumption);
    const float needleCenter = logarithmicX(needleValue);
    if (productionWidth <= 0.0f && needleCenter <= 0.0f) return true;
    const float needleMaximumX = std::max(
        x, x + static_cast<float>(widget.width() - sliderWidth));
    const float needleX = std::clamp(
        x + needleCenter - static_cast<float>(sliderWidth) * 0.5f,
        x, needleMaximumX);
    static_cast<void>(draw::drawMappedImage(
        renderer, texMgr, container::String{kSliderImage}, needleX,
        y + h - static_cast<float>(sliderHeight),
        static_cast<float>(sliderWidth), static_cast<float>(sliderHeight)));
    return true;
}

bool drawCommandBarGenExp(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    (void)widget;
    (void)renderer;
    (void)texMgr;
    return true;
}

bool drawGameWinDefault(Widget& widget, engine::Renderer& renderer, engine::TextureManager& texMgr) {
    const auto& data = widget.data();
    const auto* drawData = activeDrawData(widget);

    if ((data.status & WIN_STATUS_IMAGE) != 0) {
        drawOneImage(widget, renderer, texMgr);
        return true;
    }
    if ((data.status & WIN_STATUS_SEE_THRU) != 0) {
        return true;
    }

    const float x = static_cast<float>(widget.x());
    const float y = static_cast<float>(widget.y());
    const float w = static_cast<float>(widget.width());
    const float h = static_cast<float>(widget.height());
    const uint32_t color = drawData[0].color;
    const uint32_t borderColor = drawData[0].borderColor;

    if ((data.status & WIN_STATUS_BORDER) != 0 && ((borderColor >> 24) != 0)) {
        renderer.drawRect(x, y, w, h, borderColor);
    }
    if ((color >> 24) != 0) {
        const float borderInset = ((data.status & WIN_STATUS_BORDER) != 0) ? 1.0f : 0.0f;
        renderer.drawQuad(x + borderInset,
                          y + borderInset,
                          std::max(0.0f, w - borderInset * 2.0f),
                          std::max(0.0f, h - borderInset * 2.0f),
                          color);
    }
    return true;
}

} // namespace

void registerInGameDrawCallbacks() {
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_GAME_WIN_DEFAULT),
        drawGameWinDefault);
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_W3D_GAME_WIN_DEFAULT),
        drawGameWinDefault);
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_W3D_COMMAND_BAR_BACKGROUND),
        drawCommandBarBackground);
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_W3D_COMMAND_BAR_FOREGROUND),
        drawCommandBarForeground);
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_W3D_COMMAND_BAR_GRID),
        drawCommandBarGrid);
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_W3D_COMMAND_BAR_TOP),
        drawCommandBarTop);
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_W3D_RIGHT_HUD),
        drawRightHud);
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_W3D_LEFT_HUD),
        drawLeftHud);
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_W3D_GADGET_PUSH_BUTTON_IMAGE),
        drawGadgetPushButtonImage);
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_W3D_POWER),
        drawPower);
    DrawCallbackRegistry::instance().registerCallback(
        container::String(DRAW_CB_W3D_COMMAND_BAR_GEN_EXP),
        drawCommandBarGenExp);
}

} // namespace gui::ingame
