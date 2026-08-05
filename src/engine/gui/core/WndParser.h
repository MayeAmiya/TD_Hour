#pragma once

#include "core/container/hash_containers.h"

#include <cstdint>
#include "engine/gui/base/GuiDefaults.h"
#include "presentation/render/PresentationDefaults.h"
#include "../../../core/constants/Colors.h"

namespace gui {

class WndParser {
public:
    WndParser() = default;
    ~WndParser() = default;

    bool parse(const container::String& filename);
    bool parseFromString(const container::String& content);

    struct DrawData {
        container::String image;
        uint32_t color = COLOR_WHITE;
        uint32_t borderColor = COLOR_BLACK;
    };

    struct WindowDef {
        container::String type;
        container::String name;
        int x = 0, y = 0, w = 0, h = 0;
        int creationResW = engine::presentation_defaults::VIRTUAL_WIDTH, creationResH = engine::presentation_defaults::VIRTUAL_HEIGHT;
        uint64_t status = 0;   // WIN_STATUS_* (64-bit: the low 32 bits are full)
        uint32_t style = 0;
        container::String systemCallback;
        container::String inputCallback;
        container::String tooltipCallback;
        container::String drawCallback;
        container::String text;
        container::String tooltip;
        int tooltipDelay = 0;
        DrawData enabledDrawData[9];
        DrawData disabledDrawData[9];
        DrawData hiliteDrawData[9];
        uint32_t enabledColor = COLOR_WHITE;
        uint32_t disabledColor = COLOR_DISABLED_TEXT;
        uint32_t hiliteColor = COLOR_HILITE_TEXT;
        uint32_t textColor = COLOR_WHITE;
        uint32_t hiliteTextColor = COLOR_WHITE;
        uint32_t bgColor = COLOR_WIDGET_BACKGROUND;
        container::String fontName;
        int fontSize = ::gui::defaults::FONT_SIZE;
        bool fontBold = false;
        bool textCentered = false;
        DrawData sliderThumbEnabled[9];
        DrawData sliderThumbDisabled[9];
        DrawData sliderThumbHilite[9];
        int sliderMinValue = 0;
        int sliderMaxValue = ::gui::defaults::SLIDER_MAX_VALUE;
        container::Vector<WindowDef> children;
    };

    struct LayoutDef {
        int version = 0;
        container::String initCallback;
        container::String updateCallback;
        container::String shutdownCallback;
        uint32_t enabledColor = COLOR_WHITE;
        uint32_t disabledColor = COLOR_DISABLED_TEXT;
        uint32_t hiliteColor = COLOR_HILITE_TEXT;
        uint32_t selectedColor = COLOR_SELECTED;
        uint32_t textColor = COLOR_WHITE;
        uint32_t bgColor = COLOR_WIDGET_BACKGROUND;
        container::String fontName;
        int fontSize = ::gui::defaults::FONT_SIZE;
        bool fontBold = false;
        container::Vector<WindowDef> windows;
    };

    const LayoutDef& getLayout() const { return m_layout; }
    size_t getPos() const { return m_pos; }

private:
    LayoutDef m_layout;
    container::String m_content;
    size_t m_pos = 0;

    container::String readUntilSemicolon();
    container::String readToken();
    container::String peekToken();
    bool matchToken(const container::String& expected);
    bool hasMore() const;
    void skipWhitespace();
    void eatEquals();
    void skipComma();

    bool parseFileHeader();
    bool parseLayoutBlock();
    bool parseWindowDef(WindowDef& def);
    bool parseChildBlock(WindowDef& parent);
    void parseScreenRect(const container::String& value, WindowDef& def);
    void parseDrawData(DrawData data[9], const container::String& value);
    void parseFont(const container::String& value, container::String& name, int& size, bool& bold);

    container::String trim(const container::String& s);
    container::String toLower(const container::String& s);
    container::String unquote(const container::String& s);
    uint32_t parseColor(const container::String& str);
    uint64_t parseStatus(const container::String& str);
    uint32_t parseStyle(const container::String& str);
    void initColorMap();
    container::HashMap<container::String, container::String> m_colorMap;
};

} // namespace gui
