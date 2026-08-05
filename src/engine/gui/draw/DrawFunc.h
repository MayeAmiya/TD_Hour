#pragma once

#include "core/container/container_types.h"
#include "debug/debug.h"

#include <cstdint>
#include <functional>
#include "../core/WndParser.h"
#include "engine/gui/base/GuiDefaults.h"
#include "../../../core/constants/Colors.h"
#include "engine/renderer/runtime/Renderer.h"
#include "engine/texture/TextureManager.h"

namespace gui::draw {

// Standalone draw data for a single visual state slot
struct SlotData {
    container::String image;
    uint32_t color = COLOR_WHITE;
    uint32_t borderColor = COLOR_BLACK;
};

// Minimal window draw info passed to draw functions
struct WinDrawInfo {
    container::String type;
    int x = 0, y = 0, w = 0, h = 0;
    uint64_t status = 0;   // WIN_STATUS_* (64-bit: the low 32 bits are full)

    static constexpr int MAX_SLOTS = 9;
    container::Array<SlotData, MAX_SLOTS> enabledDrawData;
    container::Array<SlotData, MAX_SLOTS> disabledDrawData;
    container::Array<SlotData, MAX_SLOTS> hiliteDrawData;

    container::String text;
    container::String fontName;
    int fontSize = ::gui::defaults::FONT_SIZE;
    bool fontBold = false;
    bool checked = false;  // For CHECKBOX widgets
    float progress = 0.0f; // Normalized runtime value for PROGRESSBAR.
    container::Array<SlotData, MAX_SLOTS> sliderThumbEnabled;
    container::Array<SlotData, MAX_SLOTS> sliderThumbHilite;
    int sliderMinValue = 0;
    int sliderMaxValue = ::gui::defaults::SLIDER_MAX_VALUE;
};

// Draw function signature
using DrawFunc = std::function<void(engine::Renderer&, const WinDrawInfo&, engine::TextureManager&, int)>;

// Get the draw function for a window type + IMAGE status
DrawFunc getDrawFunc(const container::String& windowType, bool hasImageStatus);

// Register a draw function for a window type
void registerDrawFunc(const container::String& windowType, DrawFunc imageFunc, DrawFunc colorFunc);

// Initialize all built-in draw functions
void initDrawFuncs();

// Convert WndParser::WindowDef → WinDrawInfo (called in main.cpp)
WinDrawInfo toDrawInfo(const gui::WndParser::WindowDef& def);

// ── Shared drawing helpers (used by multiple draw files) ──────────────────────

inline bool drawMappedImage(engine::Renderer& renderer, engine::TextureManager& texMgr,
                            const container::String& imageName,
                            float dstX, float dstY, float dstW, float dstH,
                            uint32_t tint = COLOR_WHITE) {
    if (imageName.empty() || imageName == "NoImage") return false;
    auto result = texMgr.findMappedImage(imageName);
    if (!result.found || !result.texture) {
#if TD_DEBUG_ENABLED
        static int s_missCount = 0;
        if (s_missCount < 30) {
            TD_LOG_WARN("[drawMappedImage] MISS '{}' found={} tex={}", imageName, result.found, (void*)result.texture);
            s_missCount++;
        }
#endif
        return false;
    }
    renderer.drawTextureRegion(result.texture,
                           result.left, result.top, result.right, result.bottom,
                           result.texW, result.texH,
                           dstX, dstY, dstW, dstH, tint);
    return true;
}

inline int getImageWidth(engine::TextureManager& texMgr, const container::String& imageName) {
    if (imageName.empty() || imageName == "NoImage") return 0;
    auto result = texMgr.findMappedImage(imageName);
    if (!result.found) return 0;
    return result.right - result.left;
}

inline int getImageHeight(engine::TextureManager& texMgr, const container::String& imageName) {
    if (imageName.empty() || imageName == "NoImage") return 0;
    auto result = texMgr.findMappedImage(imageName);
    if (!result.found) return 0;
    return result.bottom - result.top;
}

} // namespace gui::draw
