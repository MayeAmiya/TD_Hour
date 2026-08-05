#pragma once

#include "core/container/container_types.h"
#include "GameWndLayer.h"

#include <cstdint>
#include <initializer_list>

namespace app::runtime {
struct GameUiProjection;
}

namespace engine {
class Font;
class Renderer;
struct GameStartInfo;
namespace script {
struct ScriptSessionEvent;
}
}

namespace game {
struct Mission;
}

namespace gui {
class WndRuntime;
}

namespace ingame_gui_detail {

using RuntimePaths = gui::GameWndLayer::RuntimePaths;

inline constexpr uint32_t kOpaqueWhite = 0xFFFFFFFFu;
inline constexpr uint32_t kOpaqueBlack = 0xFF000000u;
inline constexpr uint32_t kSubtitleColor = 0xFFF1E2B8u;
inline constexpr uint32_t kMilitaryCaptionColor = 0xFFC8C81Eu;
inline constexpr uint32_t kMessageLifetimeMilliseconds = 5000;
inline constexpr uint32_t kMessageFadeMilliseconds = 1000;

struct ScriptFontSpec final {
    container::String name = "Arial";
    int pointSize = 16;
    bool bold = false;
};

[[nodiscard]] RuntimePaths loadingScreenPaths(
    const engine::GameStartInfo& info);
[[nodiscard]] const game::Mission* loadingMission(
    const engine::GameStartInfo& info);
[[nodiscard]] container::String loadingText(container::StringView key);
[[nodiscard]] container::String localizedText(
    std::initializer_list<container::StringView> keys,
    container::StringView fallback);
[[nodiscard]] bool loadRuntime(gui::WndRuntime& runtime, RuntimePaths paths);
[[nodiscard]] container::String controlBarSideForCurrentGame(
    const engine::GameStartInfo& info);
[[nodiscard]] bool awaitingInitialScriptUiPolicy(
    const app::runtime::GameUiProjection& projection) noexcept;
[[nodiscard]] uint32_t millisecondsToTicks(
    uint32_t milliseconds, int framesPerSecond) noexcept;
[[nodiscard]] uint32_t withAlpha(uint32_t color, uint8_t alpha) noexcept;
[[nodiscard]] uint8_t alphaFromOpacity(float opacity) noexcept;
[[nodiscard]] float letterboxBarHeight(float width, float height) noexcept;
[[nodiscard]] bool asciiEqualIgnoreCase(
    container::StringView left, container::StringView right) noexcept;
[[nodiscard]] ScriptFontSpec parseScriptFont(
    container::StringView descriptor);
[[nodiscard]] container::String resolveScriptTextKey(
    container::StringView text, bool localized);
[[nodiscard]] container::String resolveScriptText(
    const engine::script::ScriptSessionEvent& event);
[[nodiscard]] container::Vector<container::String> wrapScriptText(
    const engine::Renderer& renderer, const engine::Font* font,
    container::StringView text,
    float maximumWidth);
void drawCenteredScriptText(
    engine::Renderer& renderer, engine::Font* font,
    const container::Vector<container::String>& lines,
    float centerX, float topY, uint32_t color);
[[nodiscard]] container::Vector<container::String>
splitMilitaryCaptionLines(container::StringView text);
[[nodiscard]] int militaryCaptionPointSize(
    float width, float height) noexcept;

} // namespace ingame_gui_detail
