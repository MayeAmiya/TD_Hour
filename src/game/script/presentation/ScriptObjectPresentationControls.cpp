#include "core/container/container_types.h"
#include "ScriptObjectPresentationControls.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::script {
namespace {

[[nodiscard]] bool finiteColor(const math::vec3& color) noexcept {
    return std::isfinite(color.x()) && std::isfinite(color.y()) && std::isfinite(color.z());
}

[[nodiscard]] bool sameColor(const math::vec3& left, const math::vec3& right) noexcept {
    return left.x() == right.x() && left.y() == right.y() && left.z() == right.z();
}

[[nodiscard]] uint32_t roundedScaledTicks(uint32_t framesPerSecond,
                                          uint32_t referenceTicks) noexcept {
    const uint64_t normalizedFramesPerSecond = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t numerator = normalizedFramesPerSecond * referenceTicks +
        ScriptObjectPresentationState::kReferenceLogicFramesPerSecond / 2u;
    const uint64_t rounded = numerator /
        ScriptObjectPresentationState::kReferenceLogicFramesPerSecond;
    return static_cast<uint32_t>(std::max<uint64_t>(1, std::min<uint64_t>(
        rounded, std::numeric_limits<uint32_t>::max())));
}

} // namespace

uint32_t ScriptObjectPresentationState::flashIntervalTicks(
    uint32_t logicFramesPerSecond) noexcept {
    return roundedScaledTicks(logicFramesPerSecond, kLegacyFlashIntervalTicks);
}

uint32_t ScriptObjectPresentationState::flashDecayTicks(
    uint32_t logicFramesPerSecond) noexcept {
    return roundedScaledTicks(logicFramesPerSecond, kLegacyFlashDecayTicks);
}

void ScriptObjectPresentationState::reset(uint64_t presentationEpoch) noexcept {
    static_cast<void>(presentationEpoch);
    m_records.clear();
    m_emoticons.clear();
}

void ScriptObjectPresentationState::rebindPresentationEpoch(
    uint64_t presentationEpoch) noexcept {
    for (ScriptObjectPresentationRecord& record : m_records) {
        if (record.flash) {
            record.flash->stamp.presentationEpoch = presentationEpoch;
        }
    }
    for (ScriptObjectEmoticonPresentation& emoticon : m_emoticons) {
        emoticon.stamp.presentationEpoch = presentationEpoch;
    }
}

void ScriptObjectPresentationState::advance(uint64_t confirmedTick) noexcept {
    for (ScriptObjectPresentationRecord& value : m_records) {
        if (value.flash && confirmedTick >= value.flash->endTick) value.flash.reset();
    }
    const auto expired = std::remove_if(m_records.begin(), m_records.end(),
                                        [](const ScriptObjectPresentationRecord& value) {
        return !value.customIndicatorColor && !value.flash &&
            !value.specialPowerDisplayHidden;
    });
    m_records.erase(expired, m_records.end());

    // Drawable draws an icon while `keepTillFrame >= now`, so an exact
    // end-tick remains visible.  This is intentionally `>` rather than the
    // flash path's `>= endTick`, whose end value is an exclusive pulse bound.
    const auto expiredEmoticons = std::remove_if(
        m_emoticons.begin(), m_emoticons.end(), [confirmedTick](
            const ScriptObjectEmoticonPresentation& value) {
            return value.lastVisibleTick && confirmedTick > *value.lastVisibleTick;
        });
    m_emoticons.erase(expiredEmoticons, m_emoticons.end());
}

bool ScriptObjectPresentationState::setCustomIndicatorColor(
    ObjectId object, math::vec3 color, ScriptPresentationControlStamp stamp) {
    if (!object || !finiteColor(color)) return false;
    ScriptObjectPresentationRecord* value = mutableRecord(object);
    if (!value) return false;
    if (value->customIndicatorColor && sameColor(*value->customIndicatorColor, color)) return false;
    value->customIndicatorColor = color;
    static_cast<void>(stamp);
    return true;
}

bool ScriptObjectPresentationState::clearCustomIndicatorColor(
    ObjectId object, ScriptPresentationControlStamp stamp) {
    if (!object) return false;
    const auto position = std::lower_bound(
        m_records.begin(), m_records.end(), object,
        [](const ScriptObjectPresentationRecord& value, ObjectId needle) {
            return value.object.value < needle.value;
        });
    if (position == m_records.end() || position->object != object ||
        !position->customIndicatorColor) {
        return false;
    }
    position->customIndicatorColor.reset();
    static_cast<void>(stamp);
    if (!position->flash && !position->specialPowerDisplayHidden)
        m_records.erase(position);
    return true;
}

bool ScriptObjectPresentationState::transferCustomIndicatorColor(
    ObjectId from, ObjectId to, ScriptPresentationControlStamp stamp) {
    if (!from || !to || from == to) return false;
    const auto source = findRecord(from);
    const std::optional<math::vec3> color = source != m_records.end()
        ? source->customIndicatorColor : std::nullopt;
    bool changed = false;
    if (source != m_records.end()) {
        source->customIndicatorColor.reset();
        if (!source->flash && !source->specialPowerDisplayHidden)
            m_records.erase(source);
        changed = true;
    }
    // `setCustomIndicatorColor()` may insert and reallocate `m_records`, so
    // always finish with the source iterator before calling it.
    if (color) return setCustomIndicatorColor(to, *color, stamp) || changed;
    return clearCustomIndicatorColor(to, stamp) || changed;
}

void ScriptObjectPresentationState::forgetObject(ObjectId object) noexcept {
    if (!object) return;
    const auto position = findRecord(object);
    if (position != m_records.end()) m_records.erase(position);
    const auto emoticon = findEmoticon(object);
    if (emoticon != m_emoticons.end()) m_emoticons.erase(emoticon);
}

bool ScriptObjectPresentationState::startFlash(ObjectId object, math::vec3 color,
                                                uint64_t firstPulseTick, uint64_t endTick,
                                                uint32_t pulseIntervalTicks, uint32_t decayTicks,
                                                ScriptPresentationControlStamp stamp) {
    if (!object || !finiteColor(color) || endTick <= firstPulseTick ||
        pulseIntervalTicks == 0 || decayTicks == 0) {
        return false;
    }
    ScriptObjectPresentationRecord* value = mutableRecord(object);
    if (!value) return false;
    value->flash = {
        .color = color,
        .stamp = stamp,
        .firstPulseTick = firstPulseTick,
        .endTick = endTick,
        .pulseIntervalTicks = pulseIntervalTicks,
        .decayTicks = decayTicks,
    };
    return true;
}

bool ScriptObjectPresentationState::setEmoticon(
    ObjectId object, container::String animationName, uint64_t startTick,
    std::optional<uint64_t> lastVisibleTick, ScriptPresentationControlStamp stamp) {
    if (!object) return false;
    // Drawable::setEmoticon clears first, then a missing template leaves the
    // slot empty.  An empty authored name is the value-only equivalent of
    // that lookup failure and therefore explicitly clears an existing icon.
    if (animationName.empty()) return clearEmoticon(object);
    if (lastVisibleTick && *lastVisibleTick < startTick) return false;

    auto position = findEmoticon(object);
    ScriptObjectEmoticonPresentation replacement{
        .object = object,
        .animationName = std::move(animationName),
        .stamp = stamp,
        .startTick = startTick,
        .lastVisibleTick = lastVisibleTick,
    };
    if (position != m_emoticons.end() && position->object == object) {
        // Do not coalesce identical names: the original allocates a new
        // Anim2D and starts it from its initial frame every time.
        *position = std::move(replacement);
    } else {
        m_emoticons.insert(position, std::move(replacement));
    }
    return true;
}

bool ScriptObjectPresentationState::clearEmoticon(ObjectId object) noexcept {
    if (!object) return false;
    const auto position = findEmoticon(object);
    if (position == m_emoticons.end() || position->object != object) return false;
    m_emoticons.erase(position);
    return true;
}

bool ScriptObjectPresentationState::setSpecialPowerDisplayVisible(
    ObjectId object, bool visible, ScriptPresentationControlStamp stamp) {
    if (!object) return false;
    ScriptObjectPresentationRecord* value = mutableRecord(object);
    if (!value) return false;
    const bool hidden = !visible;
    if (value->specialPowerDisplayHidden == hidden) {
        if (!value->customIndicatorColor && !value->flash && !hidden) {
            const auto position = findRecord(object);
            if (position != m_records.end()) m_records.erase(position);
        }
        return false;
    }
    value->specialPowerDisplayHidden = hidden;
    static_cast<void>(stamp);
    if (!hidden && !value->customIndicatorColor && !value->flash) {
        const auto position = findRecord(object);
        if (position != m_records.end()) m_records.erase(position);
    }
    return true;
}

bool ScriptObjectPresentationState::specialPowerDisplayVisible(
    ObjectId object) const noexcept {
    const ScriptObjectPresentationRecord* value = record(object);
    return !value || !value->specialPowerDisplayHidden;
}

std::optional<math::vec3> ScriptObjectPresentationState::customIndicatorColor(
    ObjectId object) const noexcept {
    const ScriptObjectPresentationRecord* value = record(object);
    return value ? value->customIndicatorColor : std::nullopt;
}

std::optional<math::vec3> ScriptObjectPresentationState::flashTint(
    ObjectId object, uint64_t confirmedTick) const noexcept {
    const ScriptObjectPresentationRecord* value = record(object);
    if (!value || !value->flash) return std::nullopt;
    const ScriptObjectFlashPresentation& flash = *value->flash;
    if (confirmedTick < flash.firstPulseTick || confirmedTick >= flash.endTick ||
        flash.pulseIntervalTicks == 0 || flash.decayTicks == 0) {
        return std::nullopt;
    }
    const uint64_t phase = (confirmedTick - flash.firstPulseTick) % flash.pulseIntervalTicks;
    if (phase >= flash.decayTicks) return std::nullopt;
    const float intensity = static_cast<float>(flash.decayTicks - phase) /
                            static_cast<float>(flash.decayTicks);
    return flash.color * intensity;
}

const ScriptObjectFlashPresentation* ScriptObjectPresentationState::flash(
    ObjectId object) const noexcept {
    const ScriptObjectPresentationRecord* value = record(object);
    return value && value->flash ? &*value->flash : nullptr;
}

const ScriptObjectEmoticonPresentation* ScriptObjectPresentationState::emoticon(
    ObjectId object) const noexcept {
    if (!object) return nullptr;
    const auto position = findEmoticon(object);
    return position != m_emoticons.end() && position->object == object ? &*position : nullptr;
}

ScriptObjectPresentationRecord* ScriptObjectPresentationState::mutableRecord(ObjectId object) {
    if (!object) return nullptr;
    const auto position = findRecord(object);
    if (position != m_records.end() && position->object == object) return &*position;
    return &*m_records.insert(position, {.object = object});
}

const ScriptObjectPresentationRecord* ScriptObjectPresentationState::record(
    ObjectId object) const noexcept {
    if (!object) return nullptr;
    const auto position = std::lower_bound(
        m_records.begin(), m_records.end(), object,
        [](const ScriptObjectPresentationRecord& value, ObjectId needle) {
            return value.object.value < needle.value;
        });
    return position != m_records.end() && position->object == object ? &*position : nullptr;
}

container::Vector<ScriptObjectPresentationRecord>::iterator
ScriptObjectPresentationState::findRecord(ObjectId object) noexcept {
    return std::lower_bound(
        m_records.begin(), m_records.end(), object,
        [](const ScriptObjectPresentationRecord& value, ObjectId needle) {
            return value.object.value < needle.value;
        });
}

container::Vector<ScriptObjectEmoticonPresentation>::iterator
ScriptObjectPresentationState::findEmoticon(ObjectId object) noexcept {
    return std::lower_bound(
        m_emoticons.begin(), m_emoticons.end(), object,
        [](const ScriptObjectEmoticonPresentation& value, ObjectId needle) {
            return value.object.value < needle.value;
        });
}

container::Vector<ScriptObjectEmoticonPresentation>::const_iterator
ScriptObjectPresentationState::findEmoticon(ObjectId object) const noexcept {
    return std::lower_bound(
        m_emoticons.begin(), m_emoticons.end(), object,
        [](const ScriptObjectEmoticonPresentation& value, ObjectId needle) {
            return value.object.value < needle.value;
        });
}

} // namespace engine::script
