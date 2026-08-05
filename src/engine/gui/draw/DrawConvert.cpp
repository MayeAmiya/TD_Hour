#include "DrawFunc.h"
#include "../../../core/constants/Strings.h"
#include "../core/WndParser.h"

namespace gui::draw {

WinDrawInfo toDrawInfo(const gui::WndParser::WindowDef& def) {
    WinDrawInfo info;
    info.type = def.type;
    info.x = def.x;
    info.y = def.y;
    info.w = def.w;
    info.h = def.h;
    info.status = def.status;
    info.text = def.text;
    info.fontName = def.fontName;
    info.fontSize = def.fontSize;
    info.fontBold = def.fontBold;

    for (int i = 0; i < WinDrawInfo::MAX_SLOTS; i++) {
        info.enabledDrawData[i] = { def.enabledDrawData[i].image, def.enabledDrawData[i].color, def.enabledDrawData[i].borderColor };
        info.disabledDrawData[i] = { def.disabledDrawData[i].image, def.disabledDrawData[i].color, def.disabledDrawData[i].borderColor };
        info.hiliteDrawData[i] = { def.hiliteDrawData[i].image, def.hiliteDrawData[i].color, def.hiliteDrawData[i].borderColor };
        info.sliderThumbEnabled[i] = { def.sliderThumbEnabled[i].image, def.sliderThumbEnabled[i].color, def.sliderThumbEnabled[i].borderColor };
        info.sliderThumbHilite[i] = { def.sliderThumbHilite[i].image, def.sliderThumbHilite[i].color, def.sliderThumbHilite[i].borderColor };
    }
    info.sliderMinValue = def.sliderMinValue;
    info.sliderMaxValue = def.sliderMaxValue;

    return info;
}

} // namespace gui::draw
