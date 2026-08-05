#include "core/container/hash_containers.h"
#include "core/data/ini/LegacyIniDirectory.h"
#include "engine/renderer/world/overlay/ObjectIconOverlayPresentation.h"

#include "core/constants/Strings.h"
#include "core/localization/StringTable.h"
#include "engine/font/Font.h"
#include "engine/font/FontRegistry.h"

#include "Renderer.h"
#include "TextureManager.h"
#include "VFS.h"
#include "engine/renderer/world/pipeline/WorldCamera.h"
#include "debug/debug.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
namespace engine::render {
namespace {

constexpr uint64_t kFnv1aOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnv1aPrime = 1099511628211ull;
constexpr uint32_t kMaximumThreeDigitFrameIndex = 999;

[[nodiscard]] container::StringView trim(container::StringView value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] container::StringView withoutComment(container::StringView value) noexcept {
    const size_t semicolon = value.find(';');
    const size_t hash = value.find('#');
    const size_t slash = value.find("//");
    const size_t cut = std::min({semicolon, hash, slash, value.size()});
    return trim(value.substr(0, cut));
}

[[nodiscard]] container::String lowerAscii(container::StringView value) {
    container::String result;
    result.reserve(value.size());
    for (const char character : value) {
        result.push_back(character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A'))
            : character);
    }
    return result;
}

[[nodiscard]] std::pair<container::StringView, container::StringView> splitField(
    container::StringView value) noexcept {
    const size_t equals = value.find('=');
    if (equals != container::StringView::npos) {
        return {trim(value.substr(0, equals)), trim(value.substr(equals + 1))};
    }
    const size_t whitespace = value.find_first_of(" \t");
    if (whitespace == container::StringView::npos) return {value, {}};
    return {value.substr(0, whitespace), trim(value.substr(whitespace + 1))};
}

[[nodiscard]] bool parseUnsigned(container::StringView value, uint32_t& output) noexcept {
    value = trim(value);
    if (value.empty()) return false;
    uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end == value.data() ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    output = static_cast<uint32_t>(parsed);
    return true;
}

[[nodiscard]] uint64_t stableIconSeed(const ObjectIconRenderSnapshot& icon) noexcept {
    // This replaces GameClientRandomValue for RandomizeStartFrame. It is
    // intentionally presentation-local yet stable across a dropped render
    // frame, and it does not consume SimulationRandom or replay state.
    uint64_t value = kFnv1aOffsetBasis;
    const auto mix = [&value](uint64_t part) noexcept {
        value ^= part;
        value *= kFnv1aPrime;
    };
    mix(icon.objectId);
    mix(icon.presentationEpoch);
    mix(icon.presentationSequence);
    mix(icon.startTick);
    return value;
}

[[nodiscard]] uint64_t delayTicks(uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    // INI::parseDurationUnsignedShort uses ceil(msec -> logic frames). A zero
    // delay progresses on each logic tick in the modern sampler, which is the
    // meaningful bounded equivalent of old Anim2D's every-draw update.
    if (milliseconds == 0) return 1;
    const uint64_t numerator = static_cast<uint64_t>(milliseconds) * rate + 999ull;
    return std::max<uint64_t>(1, numerator / 1000ull);
}

[[nodiscard]] ObjectIconAnimationMode parseMode(container::StringView name) {
    const container::String key = lowerAscii(name);
    if (key == "once") return ObjectIconAnimationMode::Once;
    if (key == "once_backwards") return ObjectIconAnimationMode::OnceBackwards;
    if (key == "loop_backwards") return ObjectIconAnimationMode::LoopBackwards;
    if (key == "ping_pong") return ObjectIconAnimationMode::PingPong;
    if (key == "ping_pong_backwards") return ObjectIconAnimationMode::PingPongBackwards;
    return ObjectIconAnimationMode::Loop;
}

[[nodiscard]] bool parseBool(container::StringView name) {
    const container::String key = lowerAscii(name);
    return key == "yes" || key == "true" || key == "1";
}

[[nodiscard]] bool isBackwards(ObjectIconAnimationMode mode) noexcept {
    return mode == ObjectIconAnimationMode::OnceBackwards ||
           mode == ObjectIconAnimationMode::LoopBackwards ||
           mode == ObjectIconAnimationMode::PingPongBackwards;
}

} // namespace

void ObjectIconAnimationLibrary::clear() noexcept {
    m_templates.clear();
}

bool ObjectIconAnimationLibrary::loadFromVfs(container::StringView directory) {
    auto& vfs = io::VFS::instance();
    const container::Array<container::StringView, 1> roots{{directory}};
    const container::Vector<container::String> candidates =
        game::ini::enumerateLegacyIniDirectories(roots);
    bool parsedAny = false;
    for (const container::String& path : candidates) {
        const container::String text = vfs.readAll(path);
        if (!text.empty()) parsedAny = parseDefinitionText(text) || parsedAny;
    }
    return parsedAny;
}

bool ObjectIconAnimationLibrary::parseDefinitionText(container::StringView text) {
    std::optional<ObjectIconAnimationTemplate> current;
    uint32_t expectedFrameCount = 0;
    bool parsedAny = false;

    const auto finish = [&]() {
        if (!current) return;
        if (expectedFrameCount != 0 &&
            current->mappedImageFrames.size() > expectedFrameCount) {
            current->mappedImageFrames.resize(expectedFrameCount);
        }
        if (!current->name.empty() && !current->mappedImageFrames.empty()) {
            const container::String canonical = canonicalName(current->name);
            if (m_templates.contains(canonical)) {
                // RefCode INI::parseAnim2DDefinition does not support
                // overrides: a duplicate is an authored-data error and the
                // first template remains authoritative.
                TD_LOG_ERROR("[Animation2D] Animation template '{}' already exists; "
                             "ignoring the later definition",
                             current->name);
            } else {
                m_templates.emplace(canonical, std::move(*current));
                parsedAny = true;
            }
        }
        current.reset();
        expectedFrameCount = 0;
    };

    size_t cursor = 0;
    while (cursor <= text.size()) {
        const size_t end = text.find('\n', cursor);
        container::StringView line = end == container::StringView::npos
            ? text.substr(cursor)
            : text.substr(cursor, end - cursor);
        line = withoutComment(line);
        if (!line.empty()) {
            const auto [key, value] = splitField(line);
            const container::String keyLower = lowerAscii(key);
            if (keyLower == "animation") {
                finish();
                const container::StringView name = trim(value);
                if (!name.empty()) {
                    current.emplace();
                    current->name.assign(name);
                }
            } else if (keyLower == "end") {
                finish();
            } else if (current) {
                if (keyLower == "numberimages") {
                    uint32_t parsedCount = 0;
                    if (parseUnsigned(value, parsedCount)) expectedFrameCount = parsedCount;
                } else if (keyLower == "image") {
                    const container::StringView image = trim(value);
                    if (!image.empty() && (expectedFrameCount == 0 ||
                                           current->mappedImageFrames.size() < expectedFrameCount)) {
                        current->mappedImageFrames.emplace_back(image);
                    }
                } else if (keyLower == "imagesequence") {
                    const container::StringView base = trim(value);
                    if (!base.empty() && expectedFrameCount != 0) {
                        for (uint32_t index = static_cast<uint32_t>(current->mappedImageFrames.size());
                             index < expectedFrameCount; ++index) {
                            if (index > kMaximumThreeDigitFrameIndex) break;
                            char suffix[4]{
                                static_cast<char>('0' + (index / 100) % 10),
                                static_cast<char>('0' + (index / 10) % 10),
                                static_cast<char>('0' + index % 10),
                                '\0',
                            };
                            current->mappedImageFrames.emplace_back(
                                container::String(base) + container::String(suffix, suffix + 3));
                        }
                    }
                } else if (keyLower == "animationmode") {
                    current->mode = parseMode(value);
                } else if (keyLower == "animationdelay") {
                    uint32_t delay = 0;
                    if (parseUnsigned(value, delay)) current->delayMilliseconds = delay;
                } else if (keyLower == "randomizestartframe") {
                    current->randomizedStartFrame = parseBool(value);
                }
            }
        }
        if (end == container::StringView::npos) break;
        cursor = end + 1;
    }
    finish();
    return parsedAny;
}

const ObjectIconAnimationTemplate* ObjectIconAnimationLibrary::find(
    container::StringView name) const {
    const auto found = m_templates.find(canonicalName(name));
    return found == m_templates.end() ? nullptr : &found->second;
}

std::optional<container::StringView> ObjectIconAnimationLibrary::frameName(
    const ObjectIconRenderSnapshot& icon, uint64_t simulationFrame) const {
    const ObjectIconAnimationTemplate* animation = find(icon.animationName);
    if (!animation || animation->mappedImageFrames.empty() ||
        simulationFrame < icon.startTick ||
        (!icon.permanent && simulationFrame > icon.lastVisibleTick)) {
        return std::nullopt;
    }

    const uint64_t frameCount = animation->mappedImageFrames.size();
    uint64_t initialFrame = isBackwards(animation->mode) ? frameCount - 1 : 0;
    if (animation->randomizedStartFrame) initialFrame = stableIconSeed(icon) % frameCount;

    const uint64_t elapsed = simulationFrame - icon.startTick;
    const uint64_t steps = elapsed / delayTicks(animation->delayMilliseconds,
                                                icon.logicFramesPerSecond);
    uint64_t frame = initialFrame;
    switch (animation->mode) {
    case ObjectIconAnimationMode::Once:
        frame = std::min<uint64_t>(frameCount - 1, initialFrame + steps);
        break;
    case ObjectIconAnimationMode::OnceBackwards:
        frame = steps >= initialFrame ? 0 : initialFrame - steps;
        break;
    case ObjectIconAnimationMode::Loop:
        frame = (initialFrame + steps % frameCount) % frameCount;
        break;
    case ObjectIconAnimationMode::LoopBackwards:
        frame = (initialFrame + frameCount - steps % frameCount) % frameCount;
        break;
    case ObjectIconAnimationMode::PingPong:
    case ObjectIconAnimationMode::PingPongBackwards: {
        if (frameCount == 1) break;
        const uint64_t period = (frameCount - 1) * 2;
        const uint64_t stepInPeriod = steps % period;
        const uint64_t phase = animation->mode == ObjectIconAnimationMode::PingPong
            ? (initialFrame + stepInPeriod) % period
            : (initialFrame + period - stepInPeriod) % period;
        frame = phase < frameCount ? phase : period - phase;
        break;
    }
    }
    return animation->mappedImageFrames[static_cast<size_t>(frame)];
}

container::String ObjectIconAnimationLibrary::canonicalName(container::StringView value) {
    return lowerAscii(trim(value));
}

void ObjectIconOverlayPresentation::reset() noexcept {
    m_animations.clear();
    m_accepted = {};
    m_unscoped = {};
    m_acceptedWorldFeedback = {};
    m_unscopedWorldFeedback = {};
    m_acceptedSimulationFrame = 0;
    m_acceptedWorldFeedbackFrame = 0;
    m_initialized = false;
    m_worldFeedbackInitialized = false;
    m_animationLoadAttempted = false;
}

const WorldFeedbackRenderState&
ObjectIconOverlayPresentation::consumeWorldFeedback(
    const WorldFeedbackRenderState& incoming, uint64_t simulationFrame) {
    if (incoming.presentationEpoch == 0) {
        m_unscopedWorldFeedback = incoming;
        return m_unscopedWorldFeedback;
    }
    if (!m_worldFeedbackInitialized ||
        incoming.presentationEpoch >
            m_acceptedWorldFeedback.presentationEpoch) {
        m_acceptedWorldFeedback = incoming;
        m_acceptedWorldFeedbackFrame = simulationFrame;
        m_worldFeedbackInitialized = true;
    } else if (incoming.presentationEpoch ==
                   m_acceptedWorldFeedback.presentationEpoch &&
               (incoming.presentationSequence >
                    m_acceptedWorldFeedback.presentationSequence ||
                (incoming.presentationSequence ==
                     m_acceptedWorldFeedback.presentationSequence &&
                 simulationFrame >= m_acceptedWorldFeedbackFrame))) {
        m_acceptedWorldFeedback = incoming;
        m_acceptedWorldFeedbackFrame = simulationFrame;
    }
    return m_acceptedWorldFeedback;
}

const ObjectIconRenderState& ObjectIconOverlayPresentation::consume(
    const ObjectIconRenderState& incoming, uint64_t simulationFrame) {
    if (incoming.presentationEpoch == 0) {
        m_unscoped = incoming;
        return m_unscoped;
    }
    if (!m_initialized || incoming.presentationEpoch > m_accepted.presentationEpoch) {
        m_accepted = incoming;
        m_acceptedSimulationFrame = simulationFrame;
        m_initialized = true;
    } else if (incoming.presentationEpoch == m_accepted.presentationEpoch &&
               (incoming.presentationSequence > m_accepted.presentationSequence ||
                (incoming.presentationSequence == m_accepted.presentationSequence &&
                 simulationFrame >= m_acceptedSimulationFrame))) {
        m_accepted = incoming;
        m_acceptedSimulationFrame = simulationFrame;
    }
    return m_accepted;
}

std::optional<math::vec2> ObjectIconOverlayPresentation::projectWorldAnchor(
    const math::vec3& anchor, const RenderCameraSnapshot& camera,
    const RenderViewportMetrics& viewport) noexcept {
    const float virtualWidth = viewport.virtualWidth;
    const float virtualHeight = viewport.virtualHeight;
    if (!std::isfinite(anchor.x()) || !std::isfinite(anchor.y()) ||
        !std::isfinite(anchor.z()) || !std::isfinite(virtualWidth) ||
        !std::isfinite(virtualHeight) || !viewport.valid()) {
        return std::nullopt;
    }
    const WorldCamera worldCamera = WorldCamera::fromSnapshot(camera);
    const math::float4x4 projection = worldCamera.viewProjectionMatrix(
        viewport.fullAspectRatio());
    const math::vec4 clip = projection.transform_vec4(
        {anchor.x(), anchor.y(), anchor.z(), 1.0f});
    if (!std::isfinite(clip.x()) || !std::isfinite(clip.y()) ||
        !std::isfinite(clip.z()) || !std::isfinite(clip.w()) ||
        clip.w() <= math::EPSILON) {
        return std::nullopt;
    }
    const float inverseW = 1.0f / clip.w();
    const float ndcX = clip.x() * inverseW;
    const float ndcY = clip.y() * inverseW;
    const float ndcZ = clip.z() * inverseW;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY) || !std::isfinite(ndcZ) ||
        ndcX < -1.0f || ndcX > 1.0f || ndcY < -1.0f || ndcY > 1.0f ||
        ndcZ < 0.0f || ndcZ > 1.0f) {
        return std::nullopt;
    }
    const float tacticalHeight = virtualHeight * std::clamp(
        camera.tacticalViewportHeightScale, 0.1f, 1.0f);
    return math::vec2{
        (ndcX * 0.5f + 0.5f) * virtualWidth,
        (-ndcY * 0.5f + 0.5f) * tacticalHeight,
    };
}

size_t ObjectIconOverlayPresentation::render(
    const ObjectIconRenderState& incoming, const RenderCameraSnapshot& camera,
    const RenderViewportMetrics& viewport, uint64_t simulationFrame,
    const ClientOptionsRenderState& acceptedOptions,
    engine::Renderer& renderer, engine::TextureManager& textures) {
    const ObjectIconRenderState& icons = consume(incoming, simulationFrame);
    // A current option epoch may be newer than an old queued frame. Never
    // render the old layer under a newly accepted session/options policy.
    if (acceptedOptions.presentationEpoch != 0 && icons.presentationEpoch != 0 &&
        icons.presentationEpoch != acceptedOptions.presentationEpoch) {
        return 0;
    }
    if (!acceptedOptions.drawIconUiEnabled || icons.icons.empty()) return 0;

    if (!m_animationLoadAttempted) {
        m_animationLoadAttempted = true;
        static_cast<void>(m_animations.loadFromVfs());
    }
    if (m_animations.size() == 0) return 0;

    if (!viewport.valid()) return 0;

    size_t drawCount = 0;
    for (const ObjectIconRenderSnapshot& icon : icons.icons) {
        const std::optional<container::StringView> frame = m_animations.frameName(icon, simulationFrame);
        if (!frame) continue;
        const std::optional<math::vec2> projected = projectWorldAnchor(
            icon.worldAnchor, camera, viewport);
        if (!projected) continue;

        // TextureManager resolves through MappedImageCollection and retains
        // texture lifetime. The overlay merely consumes the returned region;
        // it never caches an SDL/D3D resource or adds another global atlas.
        const MappedImageResult image = textures.findMappedImage(container::String(*frame));
        if (!image.found || !image.texture) continue;
        const int width = image.right - image.left;
        const int height = image.bottom - image.top;
        const int textureWidth = image.texW > 0 ? image.texW :
            static_cast<int>(image.texture->width);
        const int textureHeight = image.texH > 0 ? image.texH :
            static_cast<int>(image.texture->height);
        if (width <= 0 || height <= 0 || textureWidth <= 0 || textureHeight <= 0) continue;

        renderer.drawTextureRegion(
            image.texture, image.left, image.top, image.right, image.bottom,
            textureWidth, textureHeight,
            projected->x() - static_cast<float>(width) * 0.5f,
            projected->y() - static_cast<float>(height),
            static_cast<float>(width), static_cast<float>(height), 0xffffffffu);
        ++drawCount;
    }
    return drawCount;
}

size_t ObjectIconOverlayPresentation::renderWorldFeedback(
    const WorldFeedbackRenderState& incoming,
    const RenderCameraSnapshot& camera,
    const RenderViewportMetrics& viewport,
    uint64_t simulationFrame,
    const ClientOptionsRenderState& acceptedOptions,
    engine::Renderer& renderer,
    engine::TextureManager& textures) {
    const WorldFeedbackRenderState& feedback =
        consumeWorldFeedback(incoming, simulationFrame);
    if (acceptedOptions.presentationEpoch != 0 &&
        feedback.presentationEpoch != 0 &&
        feedback.presentationEpoch != acceptedOptions.presentationEpoch) {
        return 0;
    }
    if (!acceptedOptions.drawIconUiEnabled || !viewport.valid()) return 0;

    if (!feedback.animations.empty() && !m_animationLoadAttempted) {
        m_animationLoadAttempted = true;
        static_cast<void>(m_animations.loadFromVfs());
    }

    size_t drawCount = 0;
    for (const ObjectIconRenderSnapshot& icon : feedback.animations) {
        const std::optional<container::StringView> frame =
            m_animations.frameName(icon, simulationFrame);
        if (!frame) continue;
        RenderVector anchor = icon.worldAnchor;
        const uint32_t framesPerSecond = std::max(1u,
            icon.logicFramesPerSecond);
        const uint64_t elapsedFrames = simulationFrame - icon.startTick;
        anchor[2] += icon.zRisePerSecond *
            static_cast<float>(elapsedFrames) /
            static_cast<float>(framesPerSecond);
        const std::optional<math::vec2> projected = projectWorldAnchor(
            anchor, camera, viewport);
        if (!projected) continue;
        const MappedImageResult image = textures.findMappedImage(
            container::String(*frame));
        if (!image.found || !image.texture) continue;
        const int width = image.right - image.left;
        const int height = image.bottom - image.top;
        const int textureWidth = image.texW > 0 ? image.texW :
            static_cast<int>(image.texture->width);
        const int textureHeight = image.texH > 0 ? image.texH :
            static_cast<int>(image.texture->height);
        if (width <= 0 || height <= 0 || textureWidth <= 0 ||
            textureHeight <= 0) continue;
        uint8_t alpha = 255u;
        if (icon.fadeOnExpire && !icon.permanent) {
            const uint64_t fadeFrames = framesPerSecond;
            const uint64_t framesRemaining = icon.lastVisibleTick >=
                    simulationFrame
                ? icon.lastVisibleTick - simulationFrame + 1u : 0u;
            if (framesRemaining < fadeFrames) {
                alpha = static_cast<uint8_t>(std::min<uint64_t>(
                    255u, framesRemaining * 255u /
                        std::max<uint64_t>(1u, fadeFrames)));
            }
        }
        const uint32_t tint =
            (static_cast<uint32_t>(alpha) << 24u) | 0x00ffffffu;
        renderer.drawTextureRegion(
            image.texture, image.left, image.top, image.right, image.bottom,
            textureWidth, textureHeight,
            projected->x() - static_cast<float>(width) * 0.5f,
            projected->y() - static_cast<float>(height) * 0.5f,
            static_cast<float>(width), static_cast<float>(height), tint);
        ++drawCount;
    }

    engine::Font* font = nullptr;
    for (const WorldFloatingTextRenderSnapshot& floating :
         feedback.floatingTexts) {
        if (simulationFrame < floating.startTick ||
            simulationFrame >= floating.expireTick) continue;
        const std::optional<math::vec2> projected = projectWorldAnchor(
            floating.worldAnchor, camera, viewport);
        if (!projected) continue;
        const uint32_t framesPerSecond = std::max(
            1u, floating.logicFramesPerSecond);
        const uint64_t elapsedFrames = simulationFrame - floating.startTick;
        uint32_t alpha = (floating.color >> 24u) & 0xffu;
        if (simulationFrame > floating.timeoutTick) {
            const uint64_t fadingFrames = std::min<uint64_t>(
                simulationFrame - floating.timeoutTick, 4096u);
            const float perFrame = floating.vanishPerSecond /
                static_cast<float>(framesPerSecond);
            for (uint64_t frame = 1; frame <= fadingFrames && alpha != 0;
                 ++frame) {
                const uint32_t amount = static_cast<uint32_t>(std::max(
                    0.0f, std::round(static_cast<float>(frame) * perFrame)));
                alpha = amount >= alpha ? 0u : alpha - amount;
            }
        }
        if (alpha == 0) continue;
        const bool cashLoss = floating.amount < 0;
        const uint64_t magnitude = cashLoss
            ? static_cast<uint64_t>(-(floating.amount + 1)) + 1u
            : static_cast<uint64_t>(floating.amount);
        const int formattedAmount = static_cast<int>(std::min<uint64_t>(
            magnitude,
            static_cast<uint64_t>(std::numeric_limits<int>::max())));
        container::String text = engine::StringTable::instance().fetchFormat(
            cashLoss ? "GUI:LoseCash" : "GUI:AddCash", formattedAmount);
        if (text.empty()) {
            text = container::String{cashLoss ? "-" : "+"} +
                std::to_string(magnitude);
        }
        if (!font) {
            font = engine::FontRegistry::instance().getFont(
                container::String(FONT_ARIAL), 10, false);
        }
        const float width = font && font->isLoaded()
            ? static_cast<float>(font->getTextWidth(text))
            : static_cast<float>(text.size()) * 8.0f;
        const float rise = static_cast<float>(elapsedFrames) *
            floating.moveUpPerSecond / static_cast<float>(framesPerSecond);
        const float x = projected->x() - width * 0.5f;
        const float y = projected->y() - rise;
        const uint32_t color = (floating.color & 0x00ffffffu) |
            (alpha << 24u);
        const uint32_t shadow = alpha << 24u;
        renderer.drawText(font, text, x + 1.0f, y + 1.0f, shadow);
        renderer.drawText(font, text, x, y, color);
        drawCount += 2;
    }
    return drawCount;
}

} // namespace engine::render
