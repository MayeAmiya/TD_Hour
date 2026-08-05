#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "InGameGuiSubsystem.h"
#include "app/runtime/GameLogicIntent.h"
#include "app/runtime/GameUiProjection.h"

#include "ControlBarSchemeRuntime.h"
#include "game/base/CampaignManager.h"
#include "game/base/ChallengeGenerals.h"
#include "Font.h"
#include "FontRegistry.h"
#include "Renderer.h"
#include "StringTable.h"
#include "TextureManager.h"
#include "VFS.h"
#include "Widget.h"
#include "system/AudioSubsystem.h"
#include "debug/debug.h"
#include "core/constants/Paths.h"
#include "core/constants/Colors.h"
#include "presentation/render/PresentationDefaults.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <utility>
#include "InGameGuiSubsystemDetail.h"

namespace ingame_gui_detail {

using RuntimePaths = gui::GameWndLayer::RuntimePaths;

RuntimePaths loadingScreenPaths(const engine::GameStartInfo& info) {
    switch (info.mode) {
    case engine::GameMode::SinglePlayer:
        return {SINGLEPLAYER_LOADSCREEN_WND, SINGLEPLAYER_LOADSCREEN_WND_VFS};
    case engine::GameMode::Challenge:
        return {CHALLENGE_LOADSCREEN_WND, CHALLENGE_LOADSCREEN_WND_VFS};
    case engine::GameMode::Replay:
        return {SHELLGAME_LOADSCREEN_WND, SHELLGAME_LOADSCREEN_WND_VFS};
    case engine::GameMode::Skirmish:
        return {MULTIPLAYER_LOADSCREEN_WND, MULTIPLAYER_LOADSCREEN_WND_VFS};
    case engine::GameMode::Invalid:
        break;
    }
    return {SHELLGAME_LOADSCREEN_WND, SHELLGAME_LOADSCREEN_WND_VFS};
}

const game::Mission* loadingMission(const engine::GameStartInfo& info) {
    if (info.sequence.campaignName.empty() || info.sequence.missionName.empty()) return nullptr;
    const game::Campaign* campaign =
        game::CampaignManager::instance().findCampaign(info.sequence.campaignName);
    if (!campaign) return nullptr;
    for (const game::Mission& mission : campaign->missions) {
        if (mission.name == info.sequence.missionName) return &mission;
    }
    return nullptr;
}

container::String loadingText(container::StringView key) {
    if (key.empty()) return {};
    const container::String ownedKey{key};
    container::String localized = engine::StringTable::instance().fetch(ownedKey);
    return localized.empty() ? ownedKey : localized;
}

container::String localizedText(
    std::initializer_list<container::StringView> keys,
    container::StringView fallback) {
    for (const container::StringView key : keys) {
        container::String localized =
            engine::StringTable::instance().fetch(container::String{key});
        if (!localized.empty()) return localized;
    }
    return container::String{fallback};
}

container::String filenameOnly(container::StringView path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == container::StringView::npos) {
        return container::String{path};
    }
    return container::String{path.substr(slash + 1)};
}

void addUnique(container::Vector<container::String>& candidates, container::String candidate) {
    if (candidate.empty()) return;
    for (const auto& existing : candidates) {
        if (existing == candidate) return;
    }
    candidates.push_back(std::move(candidate));
}

void addWndPathCandidate(container::Vector<container::String>& candidates, container::StringView path) {
    if (path.empty()) return;

    addUnique(candidates, container::String{path});
    if (container::startsWithIgnoreCase(path, "Window/")) return;

    addUnique(candidates, "Window/" + container::String{path});
    if (!container::startsWithIgnoreCase(path, "Menus/")) {
        addUnique(candidates, "Window/Menus/" + filenameOnly(path));
    }
}

bool loadRuntime(gui::WndRuntime& runtime, RuntimePaths paths) {
    auto& vfs = io::VFS::instance();
    container::Vector<container::String> candidates;
    addWndPathCandidate(candidates, paths.primary);
    addWndPathCandidate(candidates, paths.fallback);

    for (const auto& path : candidates) {
        container::String content = vfs.readAll(path);
        if (content.empty()) continue;
        if (runtime.loadFromString(path, content)) {
            return true;
        }
    }
    return false;
}

container::String controlBarSideForCurrentGame(const engine::GameStartInfo& info) {
    if (!info.localPlayerBaseSide.empty()) {
        return info.localPlayerBaseSide;
    }
    if (!info.localPlayerSide.empty()) {
        return info.localPlayerSide;
    }
    if (!info.localPlayerTemplateName.empty()) {
        return info.localPlayerTemplateName;
    }

    return "America";
}

[[nodiscard]] bool awaitingInitialScriptUiPolicy(
    const app::runtime::GameUiProjection& projection) noexcept {
    if (!projection.hasSession || !projection.startInfo) return false;
    const engine::GameStartInfo& info = *projection.startInfo;
    const bool authoredMission =
        info.sequence.type == engine::GameSequenceType::Campaign ||
        info.sequence.type == engine::GameSequenceType::Challenge ||
        info.mode == engine::GameMode::Challenge;
    // Tick zero is the bootstrap projection. Campaign/challenge maps commonly
    // establish their HUD policy on the first confirmed script evaluation;
    // do not flash the default ControlBar for one presentation frame first.
    return authoredMission && projection.scriptUi.confirmedTick == 0;
}

constexpr float kLetterboxMinimumHeightFraction = 0.12f;
constexpr float kLetterboxTargetAspectHeightPerWidth = 9.0f / 16.0f;
constexpr size_t kMaximumMilitaryCaptionLines = 4;

[[nodiscard]] uint32_t millisecondsToTicks(uint32_t milliseconds, int framesPerSecond) noexcept {
    const uint64_t rate = static_cast<uint64_t>(std::max(1, framesPerSecond));
    const uint64_t ticks = (static_cast<uint64_t>(milliseconds) * rate + 999u) / 1000u;
    return static_cast<uint32_t>(std::min<uint64_t>(ticks, std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] uint32_t withAlpha(uint32_t color, uint8_t alpha) noexcept {
    return (static_cast<uint32_t>(alpha) << 24u) | (color & 0x00FFFFFFu);
}

[[nodiscard]] uint8_t alphaFromOpacity(float opacity) noexcept {
    const float clamped = std::clamp(opacity, 0.0f, 1.0f);
    return static_cast<uint8_t>(clamped * 255.0f + 0.5f);
}

[[nodiscard]] float letterboxBarHeight(float width, float height) noexcept {
    if (width <= 0.0f || height <= 0.0f) return 0.0f;

    // The normal RefCode W3D path bars a non-widescreen viewport down to
    // 16:9. Its optional SLIDE_LETTERBOX path uses 12% of display height.
    // Keep the authored 16:9 calculation but retain that 12% cinematic floor
    // on modern 16:9+ displays, where a pure legacy aspect calculation would
    // otherwise draw no visible letterbox at all.
    const float legacyHeight = std::max(0.0f,
        (height - kLetterboxTargetAspectHeightPerWidth * width) * 0.5f);
    return std::min(height * 0.5f,
                    std::max(height * kLetterboxMinimumHeightFraction, legacyHeight));
}

[[nodiscard]] bool asciiEqualIgnoreCase(container::StringView left, container::StringView right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const char a = left[index] >= 'A' && left[index] <= 'Z'
            ? static_cast<char>(left[index] + ('a' - 'A')) : left[index];
        const char b = right[index] >= 'A' && right[index] <= 'Z'
            ? static_cast<char>(right[index] + ('a' - 'A')) : right[index];
        if (a != b) return false;
    }
    return true;
}

[[nodiscard]] container::String trimAscii(container::StringView value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return container::String{value};
}

[[nodiscard]] size_t findAsciiInsensitive(container::StringView source, container::StringView needle) noexcept {
    if (needle.empty() || needle.size() > source.size()) return container::StringView::npos;
    for (size_t begin = 0; begin + needle.size() <= source.size(); ++begin) {
        if (asciiEqualIgnoreCase(source.substr(begin, needle.size()), needle)) return begin;
    }
    return container::StringView::npos;
}

// Old map data stores the cinematic font as e.g. `Arial - Size: 16 [Bold]`.
// RefCode parses this with unchecked pointer walks; keep the same accepted
// form but make malformed mod data fall back to a safe default rather than
// crashing the presentation thread.
[[nodiscard]] ScriptFontSpec parseScriptFont(container::StringView descriptor) {
    ScriptFontSpec output;
    const size_t sizeMarker = findAsciiInsensitive(descriptor, "size:");
    if (sizeMarker == container::StringView::npos) return output;

    container::StringView namePart = descriptor.substr(0, sizeMarker);
    while (!namePart.empty() && (std::isspace(static_cast<unsigned char>(namePart.back())) ||
                                 namePart.back() == '-')) {
        namePart.remove_suffix(1);
    }
    const container::String parsedName = trimAscii(namePart);
    if (!parsedName.empty()) output.name = parsedName;

    container::StringView valuePart = descriptor.substr(sizeMarker + container::StringView{"size:"}.size());
    while (!valuePart.empty() && std::isspace(static_cast<unsigned char>(valuePart.front()))) {
        valuePart.remove_prefix(1);
    }
    uint64_t size = 0;
    bool sawDigit = false;
    while (!valuePart.empty() && std::isdigit(static_cast<unsigned char>(valuePart.front()))) {
        sawDigit = true;
        size = std::min<uint64_t>(size * 10u + static_cast<uint64_t>(valuePart.front() - '0'), 256u);
        valuePart.remove_prefix(1);
    }
    if (sawDigit && size > 0) output.pointSize = static_cast<int>(size);
    output.bold = findAsciiInsensitive(descriptor, "[bold]") != container::StringView::npos ||
                  findAsciiInsensitive(descriptor, " bold") != container::StringView::npos;
    return output;
}

[[nodiscard]] container::String resolveScriptTextKey(container::StringView text, bool localized) {
    if (!localized) return container::String{text};
    engine::StringTable& strings = engine::StringTable::instance();
    const container::String key{text};
    // ScriptActions resolves every localized DISPLAY_TEXT / cinematic label
    // through GameText first. A missing or empty CSF entry is not an authored
    // literal fallback: it draws nothing (and a military caption still clears
    // its predecessor). Diagnostics deliberately set localized=false and are
    // therefore the only script messages allowed to display raw text here.
    if (!strings.exists(key)) return {};
    return strings.fetch(key);
}

[[nodiscard]] container::String resolveScriptText(const engine::script::ScriptSessionEvent& event) {
    return resolveScriptTextKey(event.text, event.localized);
}

[[nodiscard]] container::Vector<container::String> wrapScriptText(
    const engine::Renderer& renderer, const engine::Font* font,
    container::StringView text, float maximumWidth) {
    container::Vector<container::String> lines;
    container::String current;
    const float textScaleX = renderer.getTextLayoutScaleX();
    const auto widthOf = [font, textScaleX](container::StringView value) -> float {
        const float width = font
            ? static_cast<float>(font->getTextWidth(container::String{value}))
            : static_cast<float>(value.size()) * 8.0f;
        return width * textScaleX;
    };
    const auto flush = [&lines, &current]() {
        if (!current.empty()) lines.push_back(std::move(current));
        current.clear();
    };

    size_t cursor = 0;
    while (cursor < text.size()) {
        if (text[cursor] == '\n') {
            flush();
            ++cursor;
            continue;
        }
        while (cursor < text.size() && text[cursor] != '\n' &&
               std::isspace(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < text.size() && text[cursor] != '\n' &&
               !std::isspace(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }
        if (begin == cursor) continue;
        const container::StringView word = text.substr(begin, cursor - begin);
        container::String candidate = current;
        if (!candidate.empty()) candidate.push_back(' ');
        candidate.append(word);
        if (!current.empty() && widthOf(candidate) > maximumWidth) {
            flush();
            current.assign(word);
        } else {
            current = std::move(candidate);
        }
    }
    flush();
    if (lines.empty() && !text.empty()) lines.emplace_back(text);
    return lines;
}

void drawCenteredScriptText(engine::Renderer& renderer, engine::Font* font,
                            const container::Vector<container::String>& lines, float centerX,
                            float topY, uint32_t color) {
    const float textScaleX = renderer.getTextLayoutScaleX();
    const float textScaleY = renderer.getTextLayoutScaleY();
    const float lineHeight = (font
        ? static_cast<float>(font->getLineHeight()) : 16.0f) * textScaleY;
    for (size_t index = 0; index < lines.size(); ++index) {
        const float width = (font
            ? static_cast<float>(font->getTextWidth(lines[index]))
            : static_cast<float>(lines[index].size()) * 8.0f) * textScaleX;
        const float x = centerX - width * 0.5f;
        const float y = topY + static_cast<float>(index) * lineHeight;
        if (font) {
            renderer.drawText(font, lines[index], x + textScaleX,
                              y + textScaleY, kOpaqueBlack);
            renderer.drawText(font, lines[index], x, y, color);
        } else {
            renderer.drawText(lines[index], x + textScaleX,
                              y + textScaleY, kOpaqueBlack);
            renderer.drawText(lines[index], x, y, color);
        }
    }
}

// The original military subtitle accepts authored newlines and has a hard
// four-display-string cap.  Preserve that visual contract, but truncate
// safely instead of relying on RefCode's out-of-range debug assertion.
[[nodiscard]] container::Vector<container::String> splitMilitaryCaptionLines(container::StringView text) {
    container::Vector<container::String> lines;
    lines.reserve(kMaximumMilitaryCaptionLines);
    size_t begin = 0;
    while (lines.size() < kMaximumMilitaryCaptionLines) {
        const size_t newline = text.find('\n', begin);
        if (newline == container::StringView::npos) {
            lines.emplace_back(text.substr(begin));
            break;
        }
        lines.emplace_back(text.substr(begin, newline - begin));
        begin = newline + 1;
        if (begin == text.size()) {
            if (lines.size() < kMaximumMilitaryCaptionLines) lines.emplace_back();
            break;
        }
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

[[nodiscard]] int militaryCaptionPointSize(float width, float height) noexcept {
    // Caption placement in RefCode is based on an 800x600 logical display;
    // scaling both its position and font by the limiting axis keeps the
    // Courier briefing readable on contemporary wide/high-DPI viewports.
    const float scale = std::max(0.1f, std::min(width / 800.0f, height / 600.0f));
    return std::clamp(static_cast<int>(std::lround(12.0f * scale)), 1, 256);
}

} // namespace ingame_gui_detail
