#include "core/container/container_types.h"
#include "WndParser.h"
#include "WinInstanceData.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <charconv>
#include "../../../core/constants/Strings.h"

namespace gui {

namespace {

bool parseIntValue(container::StringView text, int& out) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    if (text.empty()) return false;

    int value = 0;
    auto* begin = text.data();
    auto* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr == begin) return false;
    out = value;
    return true;
}

} // namespace

bool WndParser::parse(const container::String& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parseFromString(buffer.str());
}

bool WndParser::parseFromString(const container::String& content) {
    m_content = content;
    m_pos = 0;
    m_layout = LayoutDef();
    initColorMap();

    if (!parseFileHeader()) return false;
    if (!parseLayoutBlock()) return false;

    // Parse remaining content: optional global settings then windows
    while (hasMore()) {
        container::String token = peekToken();
        container::String lower = toLower(token);
        if (lower == "window") {
            WindowDef def;
            if (parseWindowDef(def)) {
                m_layout.windows.push_back(std::move(def));
            }
        } else if (lower == "child") {
            // top-level CHILD block (rare but possible)
            WindowDef dummy;
            parseChildBlock(dummy);
        } else {
            readToken(); // skip unknown
        }
    }
    return true;
}

// ── Core tokenizer ─────────────────────────────────────────────────────

container::String WndParser::readUntilSemicolon() {
    skipWhitespace();
    size_t start = m_pos;
    while (m_pos < m_content.size() && m_content[m_pos] != ';') {
        m_pos++;
    }
    container::String result = m_content.substr(start, m_pos - start);
    if (m_pos < m_content.size()) m_pos++; // skip ';'
    return trim(result);
}

container::String WndParser::readToken() {
    skipWhitespace();
    if (m_pos >= m_content.size()) return "";

    if (m_content[m_pos] == '"') {
        m_pos++;
        size_t start = m_pos;
        while (m_pos < m_content.size() && m_content[m_pos] != '"') m_pos++;
        container::String token = m_content.substr(start, m_pos - start);
        if (m_pos < m_content.size()) m_pos++;
        return token;
    }

    if (m_content[m_pos] == '/' && m_pos + 1 < m_content.size() && m_content[m_pos + 1] == '/') {
        while (m_pos < m_content.size() && m_content[m_pos] != '\n') m_pos++;
        return readToken();
    }

    size_t start = m_pos;
    if (m_content[m_pos] == '=' || m_content[m_pos] == ':' ||
        m_content[m_pos] == ';' || m_content[m_pos] == ',') {
        m_pos++;
        return "";
    }

    while (m_pos < m_content.size() && !std::isspace(static_cast<unsigned char>(m_content[m_pos])) &&
           m_content[m_pos] != '=' && m_content[m_pos] != ':' &&
           m_content[m_pos] != ';' && m_content[m_pos] != ',') {
        m_pos++;
    }
    return m_content.substr(start, m_pos - start);
}

container::String WndParser::peekToken() {
    size_t saved = m_pos;
    container::String token = readToken();
    m_pos = saved;
    return token;
}

bool WndParser::matchToken(const container::String& expected) {
    container::String token = readToken();
    return toLower(token) == toLower(expected);
}

void WndParser::skipWhitespace() {
    while (m_pos < m_content.size()) {
        char c = m_content[m_pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            m_pos++;
        } else if (c == '/' && m_pos + 1 < m_content.size() && m_content[m_pos + 1] == '/') {
            while (m_pos < m_content.size() && m_content[m_pos] != '\n') m_pos++;
        } else {
            break;
        }
    }
}

void WndParser::eatEquals() {
    skipWhitespace();
    if (m_pos < m_content.size() && m_content[m_pos] == '=') {
        m_pos++;
    }
}

void WndParser::skipComma() {
    skipWhitespace();
    if (m_pos < m_content.size() && m_content[m_pos] == ',') {
        m_pos++;
    }
}

bool WndParser::hasMore() const {
    size_t pos = m_pos;
    while (pos < m_content.size()) {
        char c = m_content[pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { pos++; continue; }
        if (c == '/' && pos + 1 < m_content.size() && m_content[pos + 1] == '/') {
            while (pos < m_content.size() && m_content[pos] != '\n') pos++;
            continue;
        }
        return true;
    }
    return false;
}

// ── Parsing ────────────────────────────────────────────────────────────

bool WndParser::parseFileHeader() {
    if (!matchToken("FILE_VERSION")) return false;
    eatEquals();
    container::String val = readUntilSemicolon();
    if (!parseIntValue(val, m_layout.version)) return false;
    return true;
}

bool WndParser::parseLayoutBlock() {
    if (!matchToken("STARTLAYOUTBLOCK")) return false;

    while (hasMore()) {
        container::String token = peekToken();
        container::String lower = toLower(token);
        if (lower == "endlayoutblock") { readToken(); return true; }

        readToken(); // consume key
        eatEquals();
        container::String value = readUntilSemicolon();

        if (lower == "layoutinit") m_layout.initCallback = value;
        else if (lower == "layoutupdate") m_layout.updateCallback = value;
        else if (lower == "layoutshutdown") m_layout.shutdownCallback = value;
    }
    return false;
}

bool WndParser::parseWindowDef(WindowDef& def) {
    readToken(); // consume "WINDOW" keyword

    while (hasMore()) {
        container::String token = readToken();
        if (token.empty()) continue; // skip stray delimiters
        container::String lower = toLower(token);

        if (lower == "end") return true;

        if (lower == "child") {
            parseChildBlock(def);
            continue;
        }

        eatEquals();
        container::String value = readUntilSemicolon();

        if (lower == "windowtype") def.type = value;
        else if (lower == "name") def.name = value;
        else if (lower == "screenrect") parseScreenRect(value, def);
        else if (lower == "status") def.status = parseStatus(value);
        else if (lower == "style") def.style = parseStyle(value);
        else if (lower == "systemcallback") def.systemCallback = unquote(value);
        else if (lower == "inputcallback") def.inputCallback = unquote(value);
        else if (lower == "tooltipcallback") def.tooltipCallback = unquote(value);
        else if (lower == "drawcallback") def.drawCallback = unquote(value);
        else if (lower == "text") def.text = unquote(value);
        else if (lower == "tooltiptext") def.tooltip = unquote(value);
        else if (lower == "tooltipdelay") {
            int parsed = 0;
            if (parseIntValue(value, parsed)) def.tooltipDelay = parsed;
        }
        else if (lower == "enabledcolor") def.enabledColor = parseColor(value);
        else if (lower == "disabledcolor") def.disabledColor = parseColor(value);
        else if (lower == "hilitecolor") def.hiliteColor = parseColor(value);
        else if (lower == "textcolor") {
            // TEXTCOLOR = ENABLED: R G B A, DISABLED: R G B A, HILITE: R G B A;
            container::String valLower = toLower(value);
            // Parse ENABLED sub-key
            size_t enabledPos = valLower.find("enabled:");
            if (enabledPos != container::String::npos) {
                container::String afterEnabled = trim(value.substr(enabledPos + 8));
                std::istringstream iss(afterEnabled);
                int r = 255, g = 255, b = 255, a = 255;
                iss >> r >> g >> b >> a;
                def.textColor = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                                (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
            // Parse HILITE sub-key
            size_t hilitePos = valLower.find("hilite:");
            if (hilitePos != container::String::npos) {
                container::String afterHilite = trim(value.substr(hilitePos + 7));
                std::istringstream iss(afterHilite);
                int r = 255, g = 255, b = 255, a = 255;
                iss >> r >> g >> b >> a;
                def.hiliteTextColor = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                                      (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
            // Fallback: if no structured sub-keys, parse as flat color
            if (enabledPos == container::String::npos && hilitePos == container::String::npos) {
                def.textColor = parseColor(value);
                def.hiliteTextColor = def.textColor;
            }
        }
        else if (lower == "backgroundcolor" || lower == "bgcolor") def.bgColor = parseColor(value);
        else if (lower == "font") parseFont(value, def.fontName, def.fontSize, def.fontBold);
        else if (lower == "enableddrawdata") parseDrawData(def.enabledDrawData, value);
        else if (lower == "disableddrawdata") parseDrawData(def.disabledDrawData, value);
        else if (lower == "hilitedrawdata") parseDrawData(def.hiliteDrawData, value);
        else if (lower == "statictextdata") {
            container::String valLower = toLower(value);
            size_t centeredPos = valLower.find("centered:");
            if (centeredPos != container::String::npos) {
                container::String afterCentered = trim(value.substr(centeredPos + 9));
                def.textCentered = (afterCentered == "1" || afterCentered == "true");
            }
        }
        else if (lower == "sliderdata") {
            container::String valLower = toLower(value);
            size_t minPos = valLower.find("minvalue:");
            if (minPos != container::String::npos) {
                container::String afterMin = trim(value.substr(minPos + 9));
                size_t end = afterMin.find_first_of(",; \t");
                int parsed = 0;
                if (parseIntValue(end != container::String::npos ? container::StringView(afterMin).substr(0, end) : container::StringView(afterMin), parsed))
                    def.sliderMinValue = parsed;
            }
            size_t maxPos = valLower.find("maxvalue:");
            if (maxPos != container::String::npos) {
                container::String afterMax = trim(value.substr(maxPos + 9));
                size_t end = afterMax.find_first_of(",; \t");
                int parsed = 0;
                if (parseIntValue(end != container::String::npos ? container::StringView(afterMax).substr(0, end) : container::StringView(afterMax), parsed))
                    def.sliderMaxValue = parsed;
            }
        }
        else if (lower == "sliderthumbenableddrawdata") parseDrawData(def.sliderThumbEnabled, value);
        else if (lower == "sliderthumbdisableddrawdata") parseDrawData(def.sliderThumbDisabled, value);
        else if (lower == "sliderthumbhilitedrawdata") parseDrawData(def.sliderThumbHilite, value);
        // DRAWCOMPLEX and other unknown fields: value already consumed, discard
    }
    return true;
}

void WndParser::parseScreenRect(const container::String& value, WindowDef& def) {
    // Format: UPPERLEFT: x y, BOTTOMRIGHT: x y, CREATIONRESOLUTION: w h
    container::String lower = toLower(value);

    // Find each sub-field by keyword
    size_t ulPos = lower.find("upperleft");
    size_t brPos = lower.find("bottomright");
    size_t crPos = lower.find("creationresolution");

    if (ulPos != container::String::npos && brPos != container::String::npos) {
        // Parse UPPERLEFT x y
        std::istringstream ulStream(value.substr(ulPos));
        container::String keyword;
        ulStream >> keyword; // "UPPERLEFT:"
        // Skip the colon - the keyword may be "UPPERLEFT:" or just "UPPERLEFT"
        def.x = 0; def.y = 0;
        ulStream >> def.x >> def.y;

        // Parse BOTTOMRIGHT x y
        std::istringstream brStream(value.substr(brPos));
        brStream >> keyword;
        int x2 = 0, y2 = 0;
        brStream >> x2 >> y2;
        def.w = x2 - def.x;
        def.h = y2 - def.y;
    }

    if (crPos != container::String::npos) {
        std::istringstream crStream(value.substr(crPos));
        container::String keyword;
        crStream >> keyword;
        def.creationResW = 0; def.creationResH = 0;
        crStream >> def.creationResW >> def.creationResH;
    }
}

bool WndParser::parseChildBlock(WindowDef& parent) {
    while (hasMore()) {
        container::String token = peekToken();
        container::String lower = toLower(token);

        if (lower == "endallchildren" || lower == "end") {
            readToken();
            // Consume the trailing semicolon if present
            skipWhitespace();
            if (m_pos < m_content.size() && m_content[m_pos] == ';') m_pos++;
            return true;
        }

        if (lower == "window") {
            WindowDef child;
            if (parseWindowDef(child)) {
                parent.children.push_back(std::move(child));
            }
        } else {
            readToken(); // skip unknown token inside CHILD block
        }
    }
    return false;
}

void WndParser::parseDrawData(DrawData data[9], const container::String& value) {
    // Format per slot: IMAGE: name, COLOR: R G B A, BORDERCOLOR: R G B A
    // Slots are separated by newlines/whitespace, NOT by commas.
    // Commas separate IMAGE/COLOR/BORDERCOLOR within a single slot.

    container::String lower = toLower(value);

    // Find each "image:" occurrence — each marks the start of a slot
    int slot = 0;
    size_t searchPos = 0;
    while (slot < 9) {
        size_t imgPos = lower.find("image", searchPos);
        if (imgPos == container::String::npos) break;

        // Check it's "IMAGE:" not part of another word
        if (imgPos > 0 && std::isalnum(static_cast<unsigned char>(lower[imgPos - 1]))) {
            searchPos = imgPos + 5;
            continue;
        }

        // Extract slot region: from this IMAGE: to the next IMAGE: or end
        size_t nextImg = lower.find("image", imgPos + 5);
        // Also check for next "image" that is a standalone word
        while (nextImg != container::String::npos) {
            if (nextImg > 0 && std::isalnum(static_cast<unsigned char>(lower[nextImg - 1]))) {
                nextImg = lower.find("image", nextImg + 5);
                continue;
            }
            break;
        }
        container::String slotRegion = (nextImg != container::String::npos)
            ? value.substr(imgPos, nextImg - imgPos)
            : value.substr(imgPos);

        container::String lowerSlot = toLower(slotRegion);

        // Parse IMAGE: name
        {
            std::istringstream imgStream(slotRegion);
            container::String keyword;
            imgStream >> keyword; // "IMAGE:" or "IMAGE"
            imgStream >> data[slot].image;
            // Strip trailing comma/whitespace from image name
            while (!data[slot].image.empty() &&
                   (data[slot].image.back() == ',' || data[slot].image.back() == ' ')) {
                data[slot].image.pop_back();
            }
        }

        // Parse COLOR: R G B A (skip if it's actually BORDERCOLOR)
        {
            size_t colorPos = lowerSlot.find("color");
            while (colorPos != container::String::npos) {
                // Check if this is "bordercolor"
                if (colorPos >= 6 && lowerSlot.substr(colorPos - 6, 11) == "bordercolor") {
                    colorPos = lowerSlot.find("color", colorPos + 11);
                    continue;
                }
                // Check it's a standalone "color" (not part of bordercolor)
                if (colorPos > 0 && lowerSlot[colorPos - 1] == 'r') {
                    // part of "bordercolor" where ...rcolor
                    colorPos = lowerSlot.find("color", colorPos + 5);
                    continue;
                }
                break;
            }
            if (colorPos != container::String::npos) {
                std::istringstream colorStream(slotRegion.substr(colorPos));
                container::String keyword;
                colorStream >> keyword;
                uint32_t r = 255, g = 255, b = 255, a = 255;
                colorStream >> r >> g >> b >> a;
                data[slot].color = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }

        // Parse BORDERCOLOR: R G B A
        {
            size_t borderPos = lowerSlot.find("bordercolor");
            if (borderPos != container::String::npos) {
                std::istringstream borderStream(slotRegion.substr(borderPos));
                container::String keyword;
                borderStream >> keyword;
                uint32_t r = 0, g = 0, b = 0, a = 0;
                borderStream >> r >> g >> b >> a;
                data[slot].borderColor = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }

        slot++;
        searchPos = (nextImg != container::String::npos) ? nextImg : value.size();
    }
}

void WndParser::parseFont(const container::String& value, container::String& name, int& size, bool& bold) {
    // Original format: NAME, "fontName", SIZE, 15, BOLD, 1
    // Extract font name between quotes
    size_t q1 = value.find('"');
    if (q1 != container::String::npos) {
        size_t q2 = value.find('"', q1 + 1);
        if (q2 != container::String::npos) {
            name = value.substr(q1 + 1, q2 - q1 - 1);
        }
    }

    // Extract SIZE value
    container::String lower = toLower(value);
    size_t sizePos = lower.find("size");
    if (sizePos != container::String::npos) {
        size_t numStart = sizePos + 4;
        while (numStart < value.size() && (value[numStart] == ' ' || value[numStart] == ',' || value[numStart] == ':' || value[numStart] == '\t')) numStart++;
        container::String numStr;
        while (numStart < value.size() && value[numStart] >= '0' && value[numStart] <= '9') {
            numStr += value[numStart++];
        }
        int parsed = 0;
        if (!numStr.empty() && parseIntValue(numStr, parsed)) size = parsed;
    }

    // Extract BOLD value
    size_t boldPos = lower.find("bold");
    if (boldPos != container::String::npos) {
        size_t numStart = boldPos + 4;
        while (numStart < value.size() && (value[numStart] == ' ' || value[numStart] == ',' || value[numStart] == ':' || value[numStart] == '\t')) numStart++;
        container::String numStr;
        while (numStart < value.size() && value[numStart] >= '0' && value[numStart] <= '9') {
            numStr += value[numStart++];
        }
        int parsed = 0;
        if (!numStr.empty() && parseIntValue(numStr, parsed)) bold = (parsed != 0);
    }
}

// ── Helpers ────────────────────────────────────────────────────────────

container::String WndParser::trim(const container::String& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == container::String::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

container::String WndParser::toLower(const container::String& s) {
    container::String result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

container::String WndParser::unquote(const container::String& s) {
    container::String t = trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

void WndParser::initColorMap() {
    m_colorMap = {
        {COLOR_NAME_BLACK.data(), "0 0 0 255"}, {COLOR_NAME_WHITE.data(), "255 255 255 255"},
        {COLOR_NAME_RED.data(), "255 0 0 255"}, {COLOR_NAME_GREEN.data(), "0 255 0 255"},
        {COLOR_NAME_BLUE.data(), "0 0 255 255"}, {COLOR_NAME_YELLOW.data(), "255 255 0 255"},
        {COLOR_NAME_CYAN.data(), "0 255 255 255"}, {COLOR_NAME_MAGENTA.data(), "255 0 255 255"},
        {COLOR_NAME_ORANGE.data(), "255 165 0 255"}, {COLOR_NAME_PURPLE.data(), "128 0 128 255"},
        {COLOR_NAME_GRAY.data(), "128 128 128 255"}, {COLOR_NAME_GREY.data(), "128 128 128 255"},
        {COLOR_NAME_SILVER.data(), "192 192 192 255"}, {COLOR_NAME_MAROON.data(), "128 0 0 255"},
        {COLOR_NAME_OLIVE.data(), "128 128 0 255"}, {COLOR_NAME_LIME.data(), "0 255 0 255"},
        {COLOR_NAME_TEAL.data(), "0 128 128 255"}, {COLOR_NAME_NAVY.data(), "0 0 128 255"},
    };
}

uint32_t WndParser::parseColor(const container::String& str) {
    container::String lower = toLower(trim(str));
    auto it = m_colorMap.find(lower);
    if (it != m_colorMap.end()) return parseColor(it->second);

    // Handle TEXTCOLOR structured format: "ENABLED: R G B A, DISABLED: ..."
    // If it contains "enabled:", extract the first color after "enabled:"
    size_t enabledPos = lower.find("enabled:");
    if (enabledPos != container::String::npos) {
        // Extract substring starting from "enabled:" and parse just the R G B A after it
        container::String afterEnabled = trim(str.substr(enabledPos + 8)); // skip "enabled:"
        std::istringstream iss(afterEnabled);
        int r = 255, g = 255, b = 255, a = 255;
        iss >> r >> g >> b >> a;
        return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }

    // Strip "COLOR:" prefix if present
    size_t colonPos = lower.find("color:");
    container::String clean = (colonPos != container::String::npos) ? trim(str.substr(colonPos + 6)) : trim(str);

    std::istringstream iss(clean);
    int r = 255, g = 255, b = 255, a = 255;
    iss >> r >> g >> b >> a;
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

uint64_t WndParser::parseStatus(const container::String& str) {
    uint64_t status = 0;
    container::String s = str;
    // Strip "STATUS:" prefix if present
    container::String lower = toLower(s);
    size_t pos = lower.find("status:");
    if (pos != container::String::npos) s = trim(s.substr(pos + 7));

    std::istringstream iss(s);
    container::String flag;
    while (std::getline(iss, flag, '+')) {
        container::String f = toLower(trim(flag));
        if (f == "active") status |= WIN_STATUS_ACTIVE;
        else if (f == "toggle") status |= WIN_STATUS_TOGGLE;
        else if (f == "dragable") status |= WIN_STATUS_DRAGABLE;
        else if (f == "enabled") status |= WIN_STATUS_ENABLED;
        else if (f == "hidden") status |= WIN_STATUS_HIDDEN;
        else if (f == "above") status |= WIN_STATUS_ABOVE;
        else if (f == "below") status |= WIN_STATUS_BELOW;
        else if (f == "image") status |= WIN_STATUS_IMAGE;
        else if (f == "noinput") status |= WIN_STATUS_NO_INPUT;
        else if (f == "nofocus") status |= WIN_STATUS_NO_FOCUS;
        else if (f == "border") status |= WIN_STATUS_BORDER;
        else if (f == "oneline") status |= WIN_STATUS_ONE_LINE;
        else if (f == "seethru" || f == "see_thru") status |= WIN_STATUS_SEE_THRU;
        else if (f == "right_click") status |= WIN_STATUS_RIGHT_CLICK;
        else if (f == "check_like") status |= WIN_STATUS_CHECK_LIKE;
        else if (f == "on_mouse_down") status |= WIN_STATUS_ON_MOUSE_DOWN;
        else if (f == "on_char") status |= WIN_STATUS_ON_CHAR;
        else if (f == "keyable") status |= WIN_STATUS_KEYABLE;
        else if (f == "flash") status |= WIN_STATUS_FLASH;
        else if (f == "one_modal") status |= WIN_STATUS_ONE_MODAL;
        else if (f == "always_game_active") status |= WIN_STATUS_ALWAYS_GAME_ACTIVE;
        else if (f == "left_click") status |= WIN_STATUS_LEFT_CLICK;
        else if (f == "mouse_escapable") status |= WIN_STATUS_MOUSE_ESCAPABLE;
        else if (f == "on_mouse_up") status |= WIN_STATUS_ON_MOUSE_UP;
        else if (f == "on_mouse_drag") status |= WIN_STATUS_ON_MOUSE_DRAG;
        else if (f == "on_mouse_enter") status |= WIN_STATUS_ON_MOUSE_ENTER;
        else if (f == "on_mouse_leave") status |= WIN_STATUS_ON_MOUSE_LEAVE;
        else if (f == "on_key_down") status |= WIN_STATUS_ON_KEY_DOWN;
        else if (f == "on_key_up") status |= WIN_STATUS_ON_KEY_UP;
        else if (f == "on_char_im") status |= WIN_STATUS_ON_CHAR_IM;
        else if (f == "on_key_down_im") status |= WIN_STATUS_ON_KEY_DOWN_IM;
        else if (f == "on_key_up_im") status |= WIN_STATUS_ON_KEY_UP_IM;
        else if (f == "use_overlay_states") status |= WIN_STATUS_USE_OVERLAY_STATES;
        else if (f == "always_color") status |= WIN_STATUS_ALWAYS_COLOR;
    }
    return status;
}

uint32_t WndParser::parseStyle(const container::String& str) {
    uint32_t style = 0;
    container::String s = str;
    container::String lower = toLower(s);
    size_t pos = lower.find("style:");
    // "style:" is 6 characters.  The 7 here was copied from parseStatus's
    // "status:" prefix, so with no space after the colon the first flag lost
    // its leading letter and the whole style parsed to 0.
    if (pos != container::String::npos) s = trim(s.substr(pos + 6));

    std::istringstream iss(s);
    container::String flag;
    while (std::getline(iss, flag, '+')) {
        container::String f = toLower(trim(flag));
        if (f == "pushbutton") style |= GWS_PUSH_BUTTON;
        else if (f == "radiobutton") style |= GWS_RADIO_BUTTON;
        else if (f == "checkbox") style |= GWS_CHECK_BOX;
        else if (f == "vertslider") style |= GWS_VERT_SLIDER;
        else if (f == "horzslider") style |= GWS_HORZ_SLIDER;
        else if (f == "scrolllistbox") style |= GWS_SCROLL_LISTBOX;
        else if (f == "entryfield") style |= GWS_ENTRY_FIELD;
        else if (f == "statictext") style |= GWS_STATIC_TEXT;
        else if (f == "progressbar") style |= GWS_PROGRESS_BAR;
        else if (f == "user") style |= GWS_USER_WINDOW;
        else if (f == "mousetrack") style |= GWS_MOUSE_TRACK;
        else if (f == "animated") style |= GWS_ANIMATED;
        else if (f == "tabcontrol") style |= GWS_TAB_CONTROL;
        else if (f == "tabpane") style |= GWS_TAB_PANE;
        else if (f == "combobox") style |= GWS_COMBO_BOX;
        else if (f == "image") style |= GWS_STYLE_IMAGE;
        else if (f == "border") style |= GWS_STYLE_BORDER;
        else if (f == "multilinetext") style |= GWS_MULTILINETEXT;
        else if (f == "form") style |= GWS_FORM;
    }
    return style;
}

} // namespace gui
