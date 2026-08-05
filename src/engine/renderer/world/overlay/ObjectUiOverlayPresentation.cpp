#include "engine/renderer/world/overlay/ObjectUiOverlayPresentation.h"

#include "engine/renderer/world/overlay/ObjectIconOverlayPresentation.h"
#include "engine/gui/base/GuiDefaults.h"
#include "core/constants/Strings.h"
#include "core/localization/StringTable.h"
#include "engine/font/Font.h"
#include "engine/font/FontRegistry.h"
#include "engine/renderer/runtime/Renderer.h"
#include "engine/texture/TextureManager.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace engine::render {
namespace {

[[nodiscard]] uint8_t byte(float value) noexcept {
    return static_cast<uint8_t>(std::clamp(
        static_cast<int>(value), 0, 255));
}

[[nodiscard]] uint32_t argb(uint8_t red, uint8_t green, uint8_t blue,
                            uint8_t alpha = 255) noexcept {
    return (static_cast<uint32_t>(alpha) << 24u) |
        (static_cast<uint32_t>(red) << 16u) |
        (static_cast<uint32_t>(green) << 8u) |
        static_cast<uint32_t>(blue);
}

[[nodiscard]] size_t drawWaypointOverlay(
    const ObjectUiRenderState& state,
    const RenderCameraSnapshot& camera,
    const RenderViewportMetrics& viewport,
    engine::Renderer& renderer) {
    static_cast<void>(state);
    static_cast<void>(camera);
    static_cast<void>(viewport);
    static_cast<void>(renderer);
    // The dedicated world layer owns EXLaser strips and SCMNode models.
    // Drawing them again here would place them over units and double them.
    return 0;
}

[[nodiscard]] bool drawMappedImage(
    container::StringView name, float x, float y, uint32_t tint,
    engine::Renderer& renderer, engine::TextureManager& textures,
    float* advance = nullptr, float requestedWidth = 0.0f) {
    const MappedImageResult image = textures.findMappedImage(container::String(name));
    if (!image.found || !image.texture) return false;
    const int width = image.right - image.left;
    const int height = image.bottom - image.top;
    const int textureWidth = image.texW > 0 ? image.texW
        : static_cast<int>(image.texture->width);
    const int textureHeight = image.texH > 0 ? image.texH
        : static_cast<int>(image.texture->height);
    if (width <= 0 || height <= 0 || textureWidth <= 0 || textureHeight <= 0)
        return false;
    const float drawWidth = std::isfinite(requestedWidth) &&
            requestedWidth > 0.0f
        ? requestedWidth : static_cast<float>(width);
    const float drawHeight = drawWidth * static_cast<float>(height) /
        static_cast<float>(width);
    renderer.drawTextureRegion(
        image.texture, image.left, image.top, image.right, image.bottom,
        textureWidth, textureHeight, x, y,
        drawWidth, drawHeight, tint);
    if (advance) *advance = drawWidth + 1.0f;
    return true;
}

[[nodiscard]] float projectedRadiusPixels(
    const RenderVector& worldPosition, float worldRadius,
    const RenderCameraSnapshot& camera,
    const RenderViewportMetrics& viewport,
    const math::vec2& center) noexcept {
    float radiusPixels = 12.0f;
    const container::Array<RenderVector, 2> radiusSamples{
        worldPosition + RenderVector{worldRadius, 0.0f, 0.0f},
        worldPosition + RenderVector{0.0f, worldRadius, 0.0f},
    };
    for (const RenderVector& sample : radiusSamples) {
        if (const auto projected =
                ObjectIconOverlayPresentation::projectWorldAnchor(
                    sample, camera, viewport)) {
            const float dx = projected->x() - center.x();
            const float dy = projected->y() - center.y();
            radiusPixels = std::max(radiusPixels, std::hypot(dx, dy));
        }
    }
    return std::clamp(radiusPixels, 8.0f, 120.0f);
}

[[nodiscard]] float projectedWorldWidthPixels(
    const RenderVector& worldAnchor, float worldWidth,
    const RenderCameraSnapshot& camera,
    const RenderViewportMetrics& viewport,
    const math::vec2& center) noexcept {
    if (!std::isfinite(worldWidth) || worldWidth <= 0.0f) return 0.0f;
    const float halfWidth = worldWidth * 0.5f;
    float projectedWidth = 0.0f;
    const container::Array<RenderVector, 2> unitAxes{
        RenderVector{1.0f, 0.0f, 0.0f},
        RenderVector{0.0f, 1.0f, 0.0f},
    };
    constexpr container::Array<float, 4> sampleFractions{
        1.0f, 0.5f, 0.25f, 0.125f};
    for (const RenderVector& unitAxis : unitAxes) {
        for (const float fraction : sampleFractions) {
            const RenderVector axis = unitAxis * (halfWidth * fraction);
            const auto negative = ObjectIconOverlayPresentation::projectWorldAnchor(
                worldAnchor - axis, camera, viewport);
            const auto positive = ObjectIconOverlayPresentation::projectWorldAnchor(
                worldAnchor + axis, camera, viewport);
            if (negative && positive) {
                projectedWidth = std::max(
                    projectedWidth,
                    std::hypot(positive->x() - negative->x(),
                               positive->y() - negative->y()) / fraction);
                break;
            }
            const math::vec2* oneSide = positive
                ? &*positive : negative ? &*negative : nullptr;
            if (oneSide) {
                projectedWidth = std::max(
                    projectedWidth,
                    2.0f * std::hypot(oneSide->x() - center.x(),
                                      oneSide->y() - center.y()) / fraction);
                break;
            }
        }
    }
    return projectedWidth;
}

[[nodiscard]] bool drawAnimatedStatus(
    ObjectIconAnimationLibrary& animations,
    container::StringView animationName,
    const ObjectUiRenderSnapshot& object,
    uint64_t simulationFrame, uint32_t logicFramesPerSecond,
    float x, float y, float requestedWidth,
    engine::Renderer& renderer, engine::TextureManager& textures) {
    ObjectIconRenderSnapshot icon{
        .objectId = object.objectId,
        .animationName = container::String(animationName),
        .startTick = 0,
        .lastVisibleTick = std::numeric_limits<uint64_t>::max(),
        .logicFramesPerSecond = std::max<uint32_t>(1, logicFramesPerSecond),
        .permanent = true,
    };
    const std::optional<container::StringView> frame =
        animations.frameName(icon, simulationFrame);
    return frame && drawMappedImage(
        *frame, x, y, 0xffffffffu, renderer, textures,
        nullptr, requestedWidth);
}

} // namespace

ObjectUiHealthColors objectUiHealthColors(
    const ObjectUiRenderSnapshot& object) noexcept {
    const float ratio = std::clamp(
        std::isfinite(object.healthRatio) ? object.healthRatio : 0.0f,
        0.0f, 1.0f);
    if (object.underConstruction || object.disabled) {
        return {
            .fill = argb(0, byte(ratio * 255.0f), 255),
            .outline = argb(0, byte(ratio * 128.0f), 128),
        };
    }

    float red = ratio >= 0.5f
        ? 1.0f - (ratio - 0.5f) / 0.5f : 1.0f;
    float green = ratio >= 0.5f
        ? 1.0f : 1.0f - (0.5f - ratio) / 0.5f;
    const float outlineRed = red * 0.5f;
    const float outlineGreen = green * 0.5f;
    // ObjectBodyDamageState: Pristine=0, Damaged=1, ReallyDamaged=2.
    if (object.damageState >= 2u) {
        red = (1.0f + red) * 0.5f;
        green *= 0.5f;
    } else if (object.damageState == 0u) {
        green = (1.0f + green) * 0.5f;
        red *= 0.5f;
    }
    return {
        .fill = argb(byte(red * 255.0f), byte(green * 255.0f), 0),
        .outline = argb(byte(outlineRed * 255.0f),
                        byte(outlineGreen * 255.0f), 0),
    };
}

bool objectUiMayReveal(const ObjectUiRenderSnapshot& object) noexcept {
    return object.visibility == LocalVisibilityRenderCellState::Visible &&
        !object.ignoredInGui;
}

container::Vector<container::StringView> objectUiStatusAnimations(
    const ObjectUiRenderSnapshot& object,
    uint64_t simulationFrame) {
    container::Vector<container::StringView> result;
    if (object.effectivelyDead || object.ignoredInGui) return result;
    result.reserve(4);
    const bool recentlyHealing = object.recentlyHealing &&
        (simulationFrame == 0 || object.recentlyHealingUntilTick == 0 ||
         simulationFrame <= object.recentlyHealingUntilTick);
    if (recentlyHealing && !object.noHealIcon && !object.sold) {
        result.push_back(object.structure ? "StructureHeal"
            : object.vehicle ? "VehicleHeal" : "DefaultHeal");
    }
    if (object.carBomb && object.relationship == ObjectUiRelationship::Owned)
        result.push_back("CarBomb");
    if (object.enthusiastic)
        result.push_back(object.subliminal ? "Subliminal" : "Enthusiastic");
    if (object.disabledIcon) result.push_back("Disabled");
    return result;
}

std::optional<size_t> objectUiStickyBombTimedFrameIndex(
    const ObjectUiRenderSnapshot& object, uint64_t simulationFrame,
    uint32_t logicFramesPerSecond, size_t frameCount) noexcept {
    if (!object.stickyBombAttached || !object.stickyBombTimed ||
        frameCount == 0) {
        return std::nullopt;
    }
    const uint64_t rate = std::max<uint32_t>(1u, logicFramesPerSecond);
    const uint64_t remainingTicks = object.stickyBombDieTick > simulationFrame
        ? object.stickyBombDieTick - simulationFrame : 0u;
    const uint64_t seconds = remainingTicks / rate +
        static_cast<uint64_t>(remainingTicks % rate != 0u);
    const uint64_t maximumSeconds = frameCount - 1u;
    const uint64_t clampedSeconds = std::min(seconds, maximumSeconds);
    return static_cast<size_t>(maximumSeconds - clampedSeconds);
}

std::optional<ObjectUiHoverHit> objectUiHoverHitTest(
    const ObjectUiRenderState& state,
    const RenderCameraSnapshot& camera,
    const RenderViewportMetrics& viewport,
    math::vec2 pointer) noexcept {
    if (!viewport.valid() ||
        !std::isfinite(pointer.x()) || !std::isfinite(pointer.y())) {
        return std::nullopt;
    }
    std::optional<ObjectUiHoverHit> hit;
    for (const ObjectUiRenderSnapshot& object : state.objects) {
        if (!objectUiMayReveal(object)) continue;
        const std::optional<math::vec2> center =
            ObjectIconOverlayPresentation::projectWorldAnchor(
                object.worldPosition, camera, viewport);
        if (!center) continue;
        const float radius = projectedRadiusPixels(
            object.worldPosition, object.worldRadius,
            camera, viewport, *center);
        const float dx = pointer.x() - center->x();
        const float dy = pointer.y() - center->y();
        const float distanceSquared = dx * dx + dy * dy;
        if (distanceSquared > radius * radius) continue;
        if (!hit || distanceSquared < hit->distanceSquared ||
            (distanceSquared == hit->distanceSquared &&
             object.objectId < hit->objectId)) {
            hit = ObjectUiHoverHit{
                .objectId = object.objectId,
                .distanceSquared = distanceSquared,
            };
        }
    }
    return hit;
}

void ObjectUiOverlayPresentation::reset() noexcept {
    m_accepted = {};
    m_previousAnchors.clear();
    m_acceptedSimulationFrame = 0;
    m_animations.clear();
    m_initialized = false;
    m_animationLoadAttempted = false;
}

const ObjectUiRenderState& ObjectUiOverlayPresentation::consume(
    const ObjectUiRenderState& incoming, uint64_t simulationFrame) {
    // Logic may publish a frame belonging to a finished session after a newer
    // session frame was already accepted.  Treating that regression as an epoch
    // change would adopt the stale state and clear m_previousAnchors, drawing
    // one frame of the previous map's health bars/captions and destroying
    // interpolation for the frame after it.  Every sibling consumer of this
    // delivery pipeline (SelectionFlashPresentation, ObjectIconOverlay-
    // Presentation, ClientOptionsPresentationConsumer) rejects older epochs.
    if (m_initialized && incoming.presentationEpoch != 0 &&
        m_accepted.presentationEpoch != 0 &&
        incoming.presentationEpoch < m_accepted.presentationEpoch) {
        return m_accepted;
    }
    if (!m_initialized ||
        incoming.presentationEpoch != m_accepted.presentationEpoch) {
        m_previousAnchors.clear();
        m_accepted = incoming;
        m_acceptedSimulationFrame = simulationFrame;
        m_initialized = true;
    } else if (simulationFrame > m_acceptedSimulationFrame) {
        m_previousAnchors.clear();
        m_previousAnchors.reserve(m_accepted.objects.size());
        for (const ObjectUiRenderSnapshot& object : m_accepted.objects) {
            m_previousAnchors.emplace(object.objectId, EndpointAnchors{
                .worldPosition = object.worldPosition,
                .captionAnchor = object.captionAnchor,
                .healthAnchor = object.healthAnchor,
                .worldRadius = object.worldRadius,
                .healthBoxWorldWidth = object.healthBoxWorldWidth,
            });
        }
        m_accepted = incoming;
        m_acceptedSimulationFrame = simulationFrame;
    } else if (simulationFrame == m_acceptedSimulationFrame &&
               incoming.presentationSequence >
                   m_accepted.presentationSequence) {
        m_accepted = incoming;
    }
    return m_accepted;
}

size_t ObjectUiOverlayPresentation::render(
    const ObjectUiRenderState& incoming, const RenderCameraSnapshot& camera,
    const RenderViewportMetrics& viewport, uint64_t simulationFrame,
    float interpolationAlpha,
    const ClientOptionsRenderState& acceptedOptions,
    engine::Renderer& renderer, engine::TextureManager& textures) {
    const ObjectUiRenderState& state = consume(incoming, simulationFrame);
    interpolationAlpha = std::clamp(interpolationAlpha, 0.0f, 1.0f);
    if (!acceptedOptions.drawIconUiEnabled ||
        (acceptedOptions.presentationEpoch && state.presentationEpoch &&
         acceptedOptions.presentationEpoch != state.presentationEpoch)) {
        return 0;
    }
    if (!viewport.valid()) return 0;
    if (!m_animationLoadAttempted) {
        m_animationLoadAttempted = true;
        static_cast<void>(m_animations.loadFromVfs());
    }

    size_t draws = 0;
    draws += drawWaypointOverlay(state, camera, viewport, renderer);
    for (const ObjectUiRenderSnapshot& object : state.objects) {
        RenderVector worldPosition = object.worldPosition;
        RenderVector captionAnchor = object.captionAnchor;
        RenderVector healthAnchor = object.healthAnchor;
        float worldRadius = object.worldRadius;
        float healthBoxWorldWidth = object.healthBoxWorldWidth;
        if (interpolationAlpha < 1.0f) {
            const auto previous = m_previousAnchors.find(object.objectId);
            if (previous != m_previousAnchors.end()) {
                const auto interpolateVector = [interpolationAlpha](
                    const RenderVector& from,
                    const RenderVector& to) noexcept {
                    return from + (to - from) * interpolationAlpha;
                };
                worldPosition = interpolateVector(
                    previous->second.worldPosition, object.worldPosition);
                captionAnchor = interpolateVector(
                    previous->second.captionAnchor, object.captionAnchor);
                healthAnchor = interpolateVector(
                    previous->second.healthAnchor, object.healthAnchor);
                worldRadius = std::lerp(
                    previous->second.worldRadius, object.worldRadius,
                    interpolationAlpha);
                healthBoxWorldWidth = std::lerp(
                    previous->second.healthBoxWorldWidth,
                    object.healthBoxWorldWidth, interpolationAlpha);
            }
        }
        const bool revealObjectUi = objectUiMayReveal(object);
        // Drawable::drawCaption precedes the IGNORED_IN_GUI/death early-out.
        // Visibility remains an observer-local extraction/consumer gate so a
        // shrouded enemy caption can never disclose object presence or text.
        const bool revealCaption = !object.caption.empty() &&
            object.visibility == LocalVisibilityRenderCellState::Visible;
        if (!revealObjectUi && !revealCaption) continue;
        const std::optional<math::vec2> center = revealObjectUi
            ? ObjectIconOverlayPresentation::projectWorldAnchor(
                  worldPosition, camera, viewport)
            : std::nullopt;
        const std::optional<math::vec2> captionCenter = revealCaption
            ? ObjectIconOverlayPresentation::projectWorldAnchor(
                  captionAnchor, camera, viewport)
            : std::nullopt;
        if (!center && !captionCenter) continue;

        if (captionCenter) {
            const ObjectUiCaptionStyle& style = state.captionStyle;
            const int pointSize = std::clamp(style.pointSize, 1, 256);
            engine::Font* font = engine::FontRegistry::instance().getFont(
                style.fontName.empty() ? container::String(FONT_ARIAL)
                                       : style.fontName,
                pointSize, style.bold);
            if (!font && container::StringView(style.fontName) != FONT_ARIAL) {
                font = engine::FontRegistry::instance().getFont(
                    container::String(FONT_ARIAL), pointSize, style.bold);
            }
            const float textWidth = font && font->isLoaded()
                ? static_cast<float>(font->getTextWidth(object.caption))
                : static_cast<float>(object.caption.size()) *
                    static_cast<float>(pointSize) * 0.5f;
            const float textHeight = font && font->isLoaded()
                ? static_cast<float>(font->getLineHeight())
                : static_cast<float>(pointSize + 2);
            const float textX = captionCenter->x() - textWidth * 0.5f;
            const float textY = captionCenter->y();
            renderer.drawQuad(textX - 1.0f, textY - 1.0f,
                              textWidth + 2.0f, textHeight + 2.0f,
                              0x7d000000u);
            renderer.drawBorder(textX - 1.0f, textY - 1.0f,
                                textWidth + 2.0f, textHeight + 2.0f,
                                0xff141414u, 1);
            renderer.drawText(font, object.caption,
                              textX + 1.0f, textY + 1.0f, 0xff000000u);
            renderer.drawText(font, object.caption,
                              textX, textY, style.color);
            draws += 4;
        }
        if (!revealObjectUi) continue;
        if (!center) continue;

        const std::optional<math::vec2> health =
            ObjectIconOverlayPresentation::projectWorldAnchor(
                healthAnchor, camera, viewport);
        if (!health) continue;

        if (object.selected) {
            const uint32_t color = object.indicatorColor;
            constexpr size_t kSelectionSamples = 48;
            for (size_t index = 0; index < kSelectionSamples; ++index) {
                const float angle = math::TWO_PI *
                    static_cast<float>(index) /
                    static_cast<float>(kSelectionSamples);
                const RenderVector point = worldPosition + RenderVector{
                    std::cos(angle) * worldRadius,
                    std::sin(angle) * worldRadius, 0.08f};
                if (const auto projected =
                        ObjectIconOverlayPresentation::projectWorldAnchor(
                            point, camera, viewport)) {
                    renderer.drawQuad(
                        projected->x() - 1.0f, projected->y() - 1.0f,
                        2.0f, 2.0f, color);
                    ++draws;
                }
            }
        }

        const float barWidth = projectedWorldWidthPixels(
            healthAnchor, healthBoxWorldWidth, camera, viewport, *health);
        if (!(barWidth > 0.0f) || !std::isfinite(barWidth)) continue;
        const float barLeft = health->x() - barWidth * 0.45f;
        const float barTop = health->y() - 1.5f;

        if (object.underConstruction && !object.sold) {
            container::String text = engine::StringTable::instance().fetchFormat(
                "CONTROLBAR:UnderConstructionDesc",
                static_cast<double>(object.constructionPercent));
            if (text.empty()) {
                text = std::to_string(static_cast<int>(std::lround(
                    std::clamp(object.constructionPercent, 0.0f, 100.0f)))) +
                    "%";
            }
            engine::Font* font = engine::FontRegistry::instance().getFont(
                container::String(FONT_ARIAL), ::gui::defaults::FONT_SIZE);
            const float textWidth = font && font->isLoaded()
                ? static_cast<float>(font->getTextWidth(text))
                : static_cast<float>(text.size()) * 8.0f;
            const float textHeight = font && font->isLoaded()
                ? static_cast<float>(font->getLineHeight()) : 16.0f;
            const float textX = center->x() - textWidth * 0.5f;
            const float textY = center->y();
            renderer.drawQuad(textX - 1.0f, textY - 1.0f,
                              textWidth + 2.0f, textHeight + 2.0f,
                              0x7d000000u);
            renderer.drawBorder(textX - 1.0f, textY - 1.0f,
                                textWidth + 2.0f, textHeight + 2.0f,
                                0xff141414u, 1);
            renderer.drawText(font, text, textX, textY, 0xffffffffu);
            draws += 3;
        }

        float statusX = barLeft;
        const float statusWidth = std::clamp(barWidth * 0.3f, 10.0f, 32.0f);
        if (!object.effectivelyDead && object.stickyBombAttached &&
            !object.ignoredInGui) {
            // Drawable::drawBombed uses 65% of the health-region width and
            // centers the icon just below the three-pixel health bar, with
            // BOMB_ICON_EXTRA_OFFSET=5.  It is not part of the top-row status
            // icon packing and therefore must not shift healing/disabled UI.
            const float bombWidth = barWidth * 0.65f;
            const float bombX = barLeft + barWidth * 0.5f - bombWidth * 0.5f;
            const float bombY = barTop + 6.5f;
            if (drawAnimatedStatus(
                    m_animations, "BombRemote", object, simulationFrame,
                    state.logicFramesPerSecond, bombX, bombY, bombWidth,
                    renderer, textures)) {
                ++draws;
            }
            if (const ObjectIconAnimationTemplate* timed =
                    m_animations.find("BombTimed")) {
                const std::optional<size_t> timedFrame =
                    objectUiStickyBombTimedFrameIndex(
                        object, simulationFrame, state.logicFramesPerSecond,
                        timed->mappedImageFrames.size());
                if (timedFrame && *timedFrame <
                        timed->mappedImageFrames.size() &&
                    drawMappedImage(
                        timed->mappedImageFrames[*timedFrame], bombX, bombY,
                        0xffffffffu, renderer, textures, nullptr,
                        bombWidth)) {
                    ++draws;
                }
            }
        }
        for (const container::StringView animation :
             objectUiStatusAnimations(object, simulationFrame)) {
            if (drawAnimatedStatus(
                    m_animations, animation, object, simulationFrame,
                    state.logicFramesPerSecond, statusX,
                    barTop - statusWidth - 2.0f, statusWidth,
                    renderer, textures)) {
                statusX += statusWidth + 1.0f;
                ++draws;
            }
        }

        if (!object.effectivelyDead && object.veterancyLevel >= 1u &&
            object.veterancyLevel <= 3u) {
            const container::String image =
                "SCVeter" + std::to_string(object.veterancyLevel);
            if (drawMappedImage(image, barLeft + barWidth + 1.0f,
                                barTop - 1.0f, 0xffffffffu,
                                renderer, textures)) ++draws;
        }

        if (!state.showObjectHealth ||
            (!object.selected && !object.hovered) ||
            object.healthRatio <= 0.0f) continue;

        const ObjectUiHealthColors colors = objectUiHealthColors(object);
        renderer.drawRect(barLeft, barTop, barWidth, 3.0f, colors.outline);
        renderer.drawQuad(barLeft + 1.0f, barTop + 1.0f,
            std::max(0.0f, (barWidth - 2.0f) * object.healthRatio),
            1.0f, colors.fill);
        draws += 2;

        if (object.experienceRatio > 0.0f && object.experienceRatio < 1.0f) {
            renderer.drawQuad(barLeft, barTop + 5.0f, barWidth, 2.0f,
                              0xff182030u);
            renderer.drawQuad(barLeft, barTop + 5.0f,
                barWidth * object.experienceRatio, 2.0f, 0xff50a0ffu);
            draws += 2;
        }

        if (object.relationship != ObjectUiRelationship::Owned) continue;

        float pipX = barLeft;
        float pipAdvance = 0.0f;
        const float ammoY = barTop + 9.0f;
        for (uint16_t index = 0; index < object.ammoTotal; ++index) {
            const container::StringView name = index < object.ammoFull
                ? "SCPAmmoFull" : "SCPAmmoEmpty";
            if (drawMappedImage(name, pipX, ammoY, 0xffffffffu,
                                renderer, textures, &pipAdvance)) {
                pipX += pipAdvance;
                ++draws;
            }
        }
        pipX = barLeft;
        const float containedY = ammoY + 8.0f;
        for (uint16_t index = 0; index < object.containerTotal; ++index) {
            const bool full = index < object.containerFull;
            const uint32_t tint = !full ? 0xffffffffu
                : index < object.containerInfantry ? 0xff00ff00u
                : 0xff0000ffu;
            if (drawMappedImage(full ? "SCPPipFull" : "SCPPipEmpty",
                                pipX, containedY, tint,
                                renderer, textures, &pipAdvance)) {
                pipX += pipAdvance;
                ++draws;
            }
        }
    }

    return draws;
}

} // namespace engine::render
