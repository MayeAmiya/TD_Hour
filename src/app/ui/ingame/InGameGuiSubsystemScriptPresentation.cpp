#include "core/container/container_types.h"
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

using namespace ingame_gui_detail;

void InGameGuiSubsystem::consumeScriptPresentationEvents() {
    const app::runtime::ScriptUiProjection& scriptUi =
        m_gameProjection.scriptUi;
    const bool validProjection = m_gameProjection.hasSession &&
        scriptUi.sessionRevision == m_gameProjection.sessionRevision &&
        scriptUi.presentationEpoch != 0;
    const bool scopeChanged = !m_hasScriptEventCursor ||
        m_scriptEventCursor.sessionRevision != scriptUi.sessionRevision ||
        m_scriptEventCursor.presentationEpoch != scriptUi.presentationEpoch;
    if (scopeChanged) {
        releaseScriptPopupPause();
        m_scriptPresentation.clear();
        m_scriptLetterbox.clear();
        m_scriptMilitaryCaption.clear();
        m_lastScriptPopupSequence = 0;
        m_lastScriptLocalDefeatSequence = 0;
        m_activeScriptPopupEpoch = 0;
        m_activeScriptPopupSequence = 0;
        m_scriptCameoFlashes.clear();
        m_scriptCameoPresentationEpoch = 0;
        m_layer.setScriptCameoFlashes({}, 0, 0);
        m_scriptEventCursor = scriptUi.eventBatch
            ? scriptUi.eventBatch->beginCursor()
            : app::runtime::ScriptUiProjectionCursor{
                  .sessionRevision = scriptUi.sessionRevision,
                  .presentationEpoch = scriptUi.presentationEpoch,
                  .nextSequence = 1,
              };
        m_hasScriptEventCursor = validProjection;
    }
    if (!validProjection) {
        releaseScriptPopupPause();
        m_scriptMilitaryCaption.clear();
        m_layer.setGameplayHudSuppressed(false);
        m_layer.setGameplayInputEnabled(true);
        m_layer.setSpecialPowerDisplayEnabled(true);
        m_scriptCameoFlashes.clear();
        m_scriptCameoPresentationEpoch = 0;
        m_layer.setScriptCameoFlashes({}, 0, 0);
        return;
    }

    const uint64_t presentationEpoch = scriptUi.presentationEpoch;
    const uint64_t confirmedTick = scriptUi.confirmedTick;
    if (m_scriptCameoPresentationEpoch != presentationEpoch) {
        // A GameSession can be reused for another map.  Pointer identity is
        // therefore insufficient: never let a journal item stamped for the
        // prior presentation epoch become visible when confirmed ticks start
        // over at zero in the new session.
        m_scriptCameoFlashes.clear();
        m_scriptCameoPresentationEpoch = presentationEpoch;
        m_layer.setScriptCameoFlashes(
            {}, confirmedTick, presentationEpoch);
    }

    const auto presentationNow = engine::script::ScriptLetterboxPresentationConsumer::Clock::now();
    static_cast<void>(m_scriptLetterbox.synchronize(
        scriptUi.letterbox,
        presentationNow));
    // Letterbox changes only UI visibility/input routing. Popup pause is
    // handled separately below through GameLogic's explicit local-only
    // presentation authority.
    m_layer.setGameplayHudSuppressed(
        m_scriptLetterbox.suppressesGameplayHud() ||
        awaitingInitialScriptUiPolicy(m_gameProjection));

    m_layer.setGameplayInputEnabled(scriptUi.gameplayInputEnabled);
    m_layer.setSpecialPowerDisplayEnabled(
        scriptUi.specialPowerDisplayEnabled);
    const auto& popup = scriptUi.popup;
    if (popup.active && popup.stamp.presentationEpoch == presentationEpoch) {
        m_lastScriptPopupSequence = std::max(m_lastScriptPopupSequence, popup.stamp.sequence);
        m_activeScriptPopupEpoch = popup.stamp.presentationEpoch;
        m_activeScriptPopupSequence = popup.stamp.sequence;
        if (!popup.pauseRequested) {
            releaseScriptPopupPause();
        } else if (!m_scriptPopupPauseApplied) {
            m_scriptPopupPauseApplied = m_logicIntents.post(
                app::runtime::SetScriptPresentationPausedIntent{.paused = true},
                m_gameProjection.sessionRevision);
        }
    } else {
        releaseScriptPopupPause();
        m_activeScriptPopupEpoch = 0;
        m_activeScriptPopupSequence = 0;
    }
    container::Vector<engine::script::ScriptSessionEvent> events;
    if (scriptUi.eventBatch) {
        if (!scriptUi.eventBatch->canResume(m_scriptEventCursor)) {
            m_scriptPresentation.clear();
            m_scriptCameoFlashes.clear();
            m_scriptEventCursor = scriptUi.eventBatch->beginCursor();
        }
        for (const app::runtime::ScriptUiProjectionEvent& projected :
             scriptUi.eventBatch->eventsFrom(m_scriptEventCursor)) {
            m_scriptEventCursor.nextSequence = projected.sequence + 1;
            if (const auto* event = std::get_if<
                    engine::script::ScriptSessionEvent>(&projected.payload)) {
                events.push_back(*event);
                continue;
            }
            if (const auto* flash = std::get_if<
                    engine::script::ScriptCameoFlashPresentation>(
                        &projected.payload)) {
                if (flash->stamp.presentationEpoch != presentationEpoch) {
                    continue;
                }
                std::erase_if(
                    m_scriptCameoFlashes,
                    [flash](const engine::script::
                                ScriptCameoFlashPresentation& existing) {
                        return asciiEqualIgnoreCase(
                            existing.commandButton, flash->commandButton);
                    });
                if (flash->flashCount != 0) {
                    m_scriptCameoFlashes.push_back(*flash);
                }
                continue;
            }
            if (const auto* localDefeat = std::get_if<
                    engine::script::ScriptLocalDefeatPresentation>(
                        &projected.payload)) {
                if (localDefeat->active &&
                    localDefeat->stamp.presentationEpoch == presentationEpoch &&
                    localDefeat->stamp.sequence >
                        m_lastScriptLocalDefeatSequence) {
                    m_lastScriptLocalDefeatSequence =
                        localDefeat->stamp.sequence;
                    static_cast<void>(
                        openOverlay(gui::GameWndOverlay::LocalDefeat));
                }
                continue;
            }
        }
    }
    const int framesPerSecond = std::max(
        1, m_gameProjection.startInfo
            ? m_gameProjection.startInfo->gameSpeedFPS
            : engine::DEFAULT_GAME_SPEED_FPS);
    for (engine::script::ScriptSessionEvent& event : events) {
        const bool militaryCaption = event.kind ==
            engine::script::ScriptSessionEventKind::MilitaryCaption;
        const bool speechSubtitle = event.kind ==
            engine::script::ScriptSessionEventKind::Subtitle;
        if (!militaryCaption && !speechSubtitle) continue;

        const container::String text = resolveScriptText(event);
        if (speechSubtitle &&
            (text.empty() || text.front() == '*')) {
            // RefCode checks localization before calling militarySubtitle();
            // a missing/suppressed SPEECH_PLAY label does not clear an
            // already visible caption.
            continue;
        }
        uint32_t durationMilliseconds = event.durationMilliseconds;
        if (speechSubtitle) {
            const uint64_t numerator =
                static_cast<uint64_t>(event.durationTicks) * 1000u +
                static_cast<uint64_t>(framesPerSecond - 1);
            durationMilliseconds = static_cast<uint32_t>(
                std::min<uint64_t>(
                    numerator / static_cast<uint64_t>(framesPerSecond),
                    std::numeric_limits<uint32_t>::max()));
        }
        // Both SHOW_MILITARY_CAPTION and SPEECH_PLAY call the same original
        // InGameUI::militarySubtitle owner. A valid later request replaces
        // the prior four-line, lower-left typewriter in source order.
        static_cast<void>(m_scriptMilitaryCaption.present(
            text, durationMilliseconds, event.confirmedTick,
            confirmedTick, static_cast<uint32_t>(framesPerSecond),
            presentationNow));
    }
    std::erase_if(events, [](const engine::script::ScriptSessionEvent& event) {
        return event.kind ==
                engine::script::ScriptSessionEventKind::MilitaryCaption ||
            event.kind == engine::script::ScriptSessionEventKind::Subtitle;
    });

    m_scriptPresentation.consume(std::move(events),
                                 confirmedTick,
                                 millisecondsToTicks(kMessageLifetimeMilliseconds, framesPerSecond),
                                 millisecondsToTicks(kMessageFadeMilliseconds, framesPerSecond));
    m_scriptPresentation.advance(confirmedTick);
    std::erase_if(m_scriptCameoFlashes, [confirmedTick](
                      const engine::script::ScriptCameoFlashPresentation& flash) {
        const uint64_t duration = static_cast<uint64_t>(flash.flashCount) *
                                  static_cast<uint64_t>(flash.framesPerFlash);
        return confirmedTick >= flash.stamp.confirmedTick &&
               confirmedTick - flash.stamp.confirmedTick > duration;
    });
    m_layer.setScriptCameoFlashes(m_scriptCameoFlashes, confirmedTick, presentationEpoch);
}
void InGameGuiSubsystem::renderScriptLetterbox(engine::Renderer& renderer) const {
    const float opacity = m_scriptLetterbox.opacity(
        engine::script::ScriptLetterboxPresentationConsumer::Clock::now());
    const uint8_t alpha = alphaFromOpacity(opacity);
    if (alpha == 0) return;

    const float width = renderer.getUiViewportWidth();
    const float height = renderer.getUiViewportHeight();
    const float barHeight = letterboxBarHeight(width, height);
    if (barHeight <= 0.0f) return;

    const uint32_t color = withAlpha(kOpaqueBlack, alpha);
    renderer.drawQuad(0.0f, 0.0f, width, barHeight, color);
    renderer.drawQuad(
        0.0f, height - barHeight, width, barHeight, color);
}

void InGameGuiSubsystem::renderScriptPresentation(engine::Renderer& renderer) const {
    const app::runtime::ScriptUiProjection& scriptUi =
        m_gameProjection.scriptUi;
    const uint64_t tick = scriptUi.confirmedTick;
    const float width = renderer.getUiCanvasWidth();
    const float height = renderer.getUiCanvasHeight();
    const float textScaleX = renderer.getTextLayoutScaleX();
    const float textScaleY = renderer.getTextLayoutScaleY();

    // The legacy message stack has newest at zero but draws the oldest first,
    // yielding a stable top-to-bottom history.  These values intentionally
    // use the live authored UI canvas. The renderer maps this layout to the
    // output extent; glyph-local metrics retain a uniform physical scale.
    engine::Font* messageFont = engine::FontRegistry::instance().getFont("Arial", 14, false);
    const float messageLineHeight = (messageFont
        ? static_cast<float>(messageFont->getLineHeight()) : 16.0f) *
        textScaleY;
    float messageY = 22.0f;
    const auto messages = m_scriptPresentation.messages();
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        const uint8_t alpha = engine::script::ScriptPresentationState::messageOpacity(*it, tick);
        if (alpha == 0) continue;
        const container::String text = resolveScriptText(it->event);
        if (text.empty()) continue;
        const uint32_t color = withAlpha(kOpaqueWhite, alpha);
        const uint32_t shadow = withAlpha(kOpaqueBlack, alpha);
        if (messageFont) {
            renderer.drawText(messageFont, text, 21.0f, messageY + 1.0f, shadow);
            renderer.drawText(messageFont, text, 20.0f, messageY, color);
        } else {
            renderer.drawText(text, 21.0f, messageY + 1.0f, shadow);
            renderer.drawText(text, 20.0f, messageY, color);
        }
        messageY += messageLineHeight;
    }

    if (!m_commandOutcomeFeedback.empty()) {
        constexpr auto kFeedbackLifetime = std::chrono::milliseconds{2200};
        const auto elapsed = std::chrono::steady_clock::now() -
            m_commandOutcomeFeedbackStartedAt;
        if (elapsed >= std::chrono::steady_clock::duration::zero() &&
            elapsed < kFeedbackLifetime) {
            const float remaining = 1.0f - std::chrono::duration<float>(
                elapsed).count() /
                std::chrono::duration<float>(kFeedbackLifetime).count();
            const uint8_t alpha = alphaFromOpacity(
                std::clamp(remaining * 2.0f, 0.0f, 1.0f));
            const uint32_t color = withAlpha(0xffffd070u, alpha);
            const uint32_t shadow = withAlpha(kOpaqueBlack, alpha);
            if (messageFont) {
                renderer.drawText(messageFont, m_commandOutcomeFeedback,
                                  21.0f, messageY + 1.0f, shadow);
                renderer.drawText(messageFont, m_commandOutcomeFeedback,
                                  20.0f, messageY, color);
            } else {
                renderer.drawText(m_commandOutcomeFeedback,
                                  21.0f, messageY + 1.0f, shadow);
                renderer.drawText(m_commandOutcomeFeedback,
                                  20.0f, messageY, color);
            }
        }
    }

    const auto captionNow = engine::script::ScriptMilitaryCaptionPresentationConsumer::Clock::now();
    if (m_scriptMilitaryCaption.active(captionNow)) {
        const uint8_t alpha = alphaFromOpacity(m_scriptMilitaryCaption.opacity(captionNow));
        if (alpha != 0) {
            const int pointSize = militaryCaptionPointSize(width, height);
            engine::Font* titleFont = engine::FontRegistry::instance().getFont("Courier", pointSize, true);
            engine::Font* bodyFont = engine::FontRegistry::instance().getFont("Courier", pointSize, false);
            const uint32_t color = withAlpha(kMilitaryCaptionColor, alpha);
            const uint32_t shadow = withAlpha(kOpaqueBlack, alpha);
            const container::Vector<container::String> lines =
                splitMilitaryCaptionLines(m_scriptMilitaryCaption.visibleText(captionNow));
            const bool drawCursor =
                m_scriptMilitaryCaption.cursorVisible(captionNow);
            const float originX = width * (10.0f / 800.0f);
            float lineY = height * (380.0f / 600.0f);
            for (size_t index = 0; index < lines.size(); ++index) {
                engine::Font* font = index == 0 ? titleFont : bodyFont;
                const float lineHeight = std::max(1.0f, font
                    ? static_cast<float>(font->getLineHeight()) * textScaleY
                    : static_cast<float>(pointSize) * textScaleY);
                container::String visibleLine = lines[index];
                if (drawCursor && index + 1 == lines.size()) {
                    // Keep the cursor in the same glyph-layout pass as the
                    // visible text.  Measuring Courier and then drawing CJK
                    // through the fallback font gives different advances and
                    // makes a separately positioned quad drift into the line.
                    // U+2588 preserves the original filled-block cursor while
                    // sharing the exact fallback, scaling and advance path.
                    visibleLine += "\xE2\x96\x88";
                }
                if (!visibleLine.empty()) {
                    if (font) {
                        if (!lines[index].empty()) {
                            renderer.drawText(font, lines[index], originX + 1.0f, lineY + 1.0f, shadow);
                        }
                        renderer.drawText(font, visibleLine, originX, lineY, color);
                    } else {
                        if (!lines[index].empty()) {
                            renderer.drawText(lines[index], originX + 1.0f, lineY + 1.0f, shadow);
                        }
                        renderer.drawText(visibleLine, originX, lineY, color);
                    }
                }
                lineY += lineHeight;
            }
        }
    }

    if (const auto& cinematic = m_scriptPresentation.cinematic()) {
        const container::String text = resolveScriptText(cinematic->event);
        if (!text.empty()) {
            const ScriptFontSpec spec = parseScriptFont(cinematic->event.fontDescriptor);
            engine::Font* font = engine::FontRegistry::instance().getFont(spec.name, spec.pointSize, spec.bold);
            const container::Vector<container::String> lines = wrapScriptText(
                renderer, font, text, std::max(40.0f, width - 40.0f));
            const float lineHeight = (font
                ? static_cast<float>(font->getLineHeight()) : 16.0f) *
                textScaleY;
            const float top = height * 0.90f - lineHeight * static_cast<float>(lines.size());
            drawCenteredScriptText(renderer, font, lines, width * 0.5f, top, kOpaqueWhite);
        }
    }

    if (!m_gameProjection.hasSession) return;
    if (scriptUi.namedTimerDisplayEnabled) {
        float indicatorY = height * 0.18f;
        for (const auto& indicator : scriptUi.namedIndicators) {
            const container::String label = resolveScriptTextKey(
                indicator.presentation.label, true);
            if (label.empty()) continue;
            const int32_t value = indicator.valueResolved
                ? indicator.value : 0;
            const container::String text =
                indicator.presentation.kind ==
                    engine::script::ScriptNamedIndicatorKind::Countdown
                ? label + ": " + std::to_string(std::max(0, value))
                : label + ": " + std::to_string(value);
            const float textWidth = messageFont
                ? static_cast<float>(messageFont->getTextWidth(text)) *
                      textScaleX
                : static_cast<float>(text.size()) * 8.0f * textScaleX;
            const float left = std::max(12.0f, width - textWidth - 32.0f);
            renderer.drawQuad(left - 8.0f, indicatorY - 4.0f, textWidth + 16.0f,
                              messageLineHeight + 8.0f, 0xA0000000u);
            if (messageFont) {
                renderer.drawText(messageFont, text, left + 1.0f, indicatorY + 1.0f, kOpaqueBlack);
                renderer.drawText(messageFont, text, left, indicatorY, kOpaqueWhite);
            } else {
                renderer.drawText(text, left + 1.0f, indicatorY + 1.0f, kOpaqueBlack);
                renderer.drawText(text, left, indicatorY, kOpaqueWhite);
            }
            indicatorY += messageLineHeight + 10.0f;
        }
    }

    const auto& popup = scriptUi.popup;
    if (popup.active && popup.stamp.presentationEpoch ==
            scriptUi.presentationEpoch) {
        const container::String text = resolveScriptTextKey(popup.text, popup.localized);
        if (!text.empty()) {
            const float popupWidth = std::clamp(width * static_cast<float>(popup.width) / 100.0f,
                                                180.0f, width * 0.8f);
            const float left = std::clamp(width * static_cast<float>(popup.xPercent) / 100.0f,
                                          8.0f, std::max(8.0f, width - popupWidth - 8.0f));
            const float top = std::clamp(height * static_cast<float>(popup.yPercent) / 100.0f,
                                         8.0f, std::max(8.0f, height - 96.0f));
            const container::Vector<container::String> lines = wrapScriptText(
                renderer, messageFont, text, popupWidth - 24.0f);
            // Reserve a separate final line for the local dismissal hint;
            // otherwise a one-line popup would overlap its own text.
            const float popupHeight = std::max(64.0f,
                messageLineHeight * static_cast<float>(std::max<size_t>(1, lines.size()) + 1u) +
                    30.0f);
            renderer.drawQuad(left, top, popupWidth, popupHeight, 0xE0101010u);
            float y = top + 12.0f;
            for (const container::String& line : lines) {
                if (messageFont) renderer.drawText(messageFont, line, left + 13.0f, y, kOpaqueWhite);
                else renderer.drawText(line, left + 13.0f, y, kOpaqueWhite);
                y += messageLineHeight;
            }
            const container::String continueHint = "Click or press Enter to continue";
            const float hintWidth = messageFont
                ? static_cast<float>(messageFont->getTextWidth(continueHint)) *
                      textScaleX
                : static_cast<float>(continueHint.size()) * 8.0f *
                      textScaleX;
            const float hintY = top + popupHeight - messageLineHeight - 5.0f;
            if (messageFont) {
                renderer.drawText(messageFont, continueHint, left + popupWidth - hintWidth - 12.0f,
                                  hintY, 0xFFC8C8C8u);
            } else {
                renderer.drawText(continueHint, left + popupWidth - hintWidth - 12.0f,
                                  hintY, 0xFFC8C8C8u);
            }
        }
    }

    // Tactical radar and object icon UI are rendered from the same sealed
    // world snapshot as the 3D frame.  This GUI layer intentionally has no
    // second live-GameSession radar path which could disagree with R2 shroud
    // filtering or resurrect a stale event after an epoch change.
}
