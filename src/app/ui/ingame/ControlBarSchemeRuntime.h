#pragma once

#include "core/container/hash_containers.h"

#include <cstdint>
#include <optional>
#include <utility>
#include "../../../core/constants/Colors.h"

namespace gui::ingame {

struct ControlBarSchemeImagePart {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int layer = 0;
    container::String imageName;
};

struct ControlBarPowerMeterState final {
    int32_t production = 0;
    int32_t consumption = 0;
    int32_t logarithmicBase = 7;
    float intervals = 3.0f;
    int32_t yellowRange = 5;
    bool sufficient = true;
};

class ControlBarSchemeRuntime {
public:
    static ControlBarSchemeRuntime& instance();

    const container::Vector<ControlBarSchemeImagePart>& imageParts();
    const container::String& rightHudImage();
    const container::String& queueButtonImage();
    const container::String& commandMarkerImage();
    const container::String& expBarForegroundImage();
    const container::String& powerPurchaseImage();
    uint32_t commandBarBorderColor();
    uint32_t buttonBorderBuildColor();
    uint32_t buttonBorderActionColor();
    uint32_t buttonBorderUpgradeColor();
    uint32_t buttonBorderSystemColor();
    container::String namedImage(container::StringView key);
    std::optional<std::pair<int, int>> namedPoint(container::StringView key);
    int drawOffsetY() const { return m_drawOffsetY; }
    void setDrawOffsetY(int offset) { m_drawOffsetY = offset; }
    void setPlayerSide(container::StringView side);
    void setPowerMeterState(ControlBarPowerMeterState state) noexcept {
        m_powerMeter = state;
    }
    [[nodiscard]] ControlBarPowerMeterState powerMeterState() const noexcept {
        return m_powerMeter;
    }
    void reset();

private:
    ControlBarSchemeRuntime() = default;

    void ensureLoaded();
    bool loadFromVfs();
    bool parse(const container::String& content);

    bool m_loaded = false;
    container::String m_playerSide;
    container::String m_rightHudImage;
    container::String m_queueButtonImage;
    container::String m_commandMarkerImage;
    container::String m_expBarForegroundImage;
    container::String m_powerPurchaseImage;
    uint32_t m_commandBarBorderColor = COLOR_WHITE;
    // RefCode uses GAME_COLOR_UNDEFINED (transparent white) until the selected
    // scheme supplies these authored values.
    uint32_t m_buttonBorderBuildColor = 0x00ffffffu;
    uint32_t m_buttonBorderActionColor = 0x00ffffffu;
    uint32_t m_buttonBorderUpgradeColor = 0x00ffffffu;
    uint32_t m_buttonBorderSystemColor = 0x00ffffffu;
    int m_drawOffsetY = 0;
    ControlBarPowerMeterState m_powerMeter;
    container::HashMap<container::String, container::String> m_namedImages;
    container::HashMap<container::String, std::pair<int, int>> m_namedPoints;
    container::Vector<ControlBarSchemeImagePart> m_imageParts;
};

} // namespace gui::ingame
