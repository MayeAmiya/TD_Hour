#pragma once

#include "core/container/container_types.h"

#include "core/ecs/ObjectId.h"
#include "game/player/PlayerTypes.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "game/script/contracts/ScriptPresentationValueTypes.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cstdint>
#include <optional>
namespace engine::script {

// Object-addressed presentation actions share a selector that is deliberately
// independent from an ECS entity.  A legacy UNIT name and a legacy TEAM name
// are different namespaces, so preserve that distinction instead of passing
// both through an untyped string field.  Future named/team flash, object
// audio, and panel actions can reuse this selector at the same boundary.

// One confirmed script flash. The renderer samples this strictly from the
// sealed simulation frame, so a delayed/collapsed render queue cannot drop a
// pulse or make it run at a different rate on a fast GPU.
struct ScriptObjectFlashPresentation final {
    math::vec3 color{};
    ScriptPresentationControlStamp stamp{};
    uint64_t firstPulseTick = 0;
    uint64_t endTick = 0;
    uint32_t pulseIntervalTicks = 15;
    uint32_t decayTicks = 4;
};

// A modern value-only replacement for Drawable::m_iconInfo[ICON_EMOTICON].
// `lastVisibleTick` is inclusive for positive lifetimes. A missing value
// represents the legacy FOREVER sentinel; a zero converted duration clears
// the slot at the session boundary before a render snapshot is produced.
// This is deliberately not simulation state and is never replayed.
struct ScriptObjectEmoticonPresentation final {
    ObjectId object = INVALID_OBJECT_ID;
    container::String animationName;
    ScriptPresentationControlStamp stamp{};
    uint64_t startTick = 0;
    std::optional<uint64_t> lastVisibleTick;
};

// Session-owned per-object presentation record. A custom indicator colour is
// durable and is used by later ordinary flashes as well as the renderer's
// per-instance house-colour path. It never mutates a shared W3D asset.
struct ScriptObjectPresentationRecord final {
    ObjectId object = INVALID_OBJECT_ID;
    std::optional<math::vec3> customIndicatorColor;
    std::optional<ScriptObjectFlashPresentation> flash;
    bool specialPowerDisplayHidden = false;
};

class ScriptObjectPresentationState final {
public:
    static constexpr uint32_t kReferenceLogicFramesPerSecond = 30;
    static constexpr uint32_t kLegacyFlashIntervalTicks = 15;
    static constexpr uint32_t kLegacyFlashDecayTicks = 4;

    // The source game is 30 Hz, where the Drawable pulse interval is 15
    // frames and its TintEnvelope decays for four. Modern sessions may use a
    // different fixed rate, so scale these real-time durations once at the
    // session boundary rather than speeding up presentation on a 45/60 Hz
    // simulation.
    [[nodiscard]] static uint32_t flashIntervalTicks(uint32_t logicFramesPerSecond) noexcept;
    [[nodiscard]] static uint32_t flashDecayTicks(uint32_t logicFramesPerSecond) noexcept;

    void reset(uint64_t presentationEpoch = 0) noexcept;
    void rebindPresentationEpoch(uint64_t presentationEpoch) noexcept;
    // Removes expired transient flashes, retaining a durable custom colour.
    void advance(uint64_t confirmedTick) noexcept;

    [[nodiscard]] bool setCustomIndicatorColor(ObjectId object, math::vec3 color,
                                                ScriptPresentationControlStamp stamp);
    // RefCode uses packed Color value zero as the absence sentinel for an
    // object's custom indicator. Keep an explicit erase operation so a
    // scripted zero restores the owner colour rather than becoming a durable
    // opaque black override.
    [[nodiscard]] bool clearCustomIndicatorColor(ObjectId object,
                                                  ScriptPresentationControlStamp stamp);
    // ScriptEngine::transferObjectName carries only the old Object's durable
    // custom indicator colour to its replacement; a transient Drawable flash
    // belongs to the old drawable and must not follow it.
    [[nodiscard]] bool transferCustomIndicatorColor(ObjectId from, ObjectId to,
                                                     ScriptPresentationControlStamp stamp);
    // ObjectId values are never recycled. Prune presentation data at the
    // authoritative lifecycle boundary rather than retaining dead records.
    void forgetObject(ObjectId object) noexcept;
    [[nodiscard]] bool startFlash(ObjectId object, math::vec3 color,
                                  uint64_t firstPulseTick, uint64_t endTick,
                                  uint32_t pulseIntervalTicks, uint32_t decayTicks,
                                  ScriptPresentationControlStamp stamp);
    // Replaces the prior icon unconditionally, even when the template name
    // matches: RefCode creates a fresh Anim2D instance and restarts it.
    [[nodiscard]] bool setEmoticon(ObjectId object, container::String animationName,
                                   uint64_t startTick,
                                   std::optional<uint64_t> lastVisibleTick,
                                   ScriptPresentationControlStamp stamp);
    [[nodiscard]] bool clearEmoticon(ObjectId object) noexcept;
    [[nodiscard]] bool setSpecialPowerDisplayVisible(
        ObjectId object, bool visible, ScriptPresentationControlStamp stamp);
    [[nodiscard]] bool specialPowerDisplayVisible(ObjectId object) const noexcept;
    [[nodiscard]] std::optional<math::vec3> customIndicatorColor(ObjectId object) const noexcept;
    // Returns the additive colour for this logic frame. The strict tick-based
    // envelope mirrors Drawable::setFlashCount/colorFlash: a pulse begins
    // every half second and linearly decays across the scaled four-frame
    // reference interval.
    [[nodiscard]] std::optional<math::vec3> flashTint(ObjectId object,
                                                       uint64_t confirmedTick) const noexcept;
    [[nodiscard]] const ScriptObjectFlashPresentation* flash(
        ObjectId object) const noexcept;
    [[nodiscard]] const ScriptObjectEmoticonPresentation* emoticon(ObjectId object) const noexcept;
    [[nodiscard]] container::Span<const ScriptObjectEmoticonPresentation> emoticons() const noexcept {
        return m_emoticons;
    }

private:
    [[nodiscard]] ScriptObjectPresentationRecord* mutableRecord(ObjectId object);
    [[nodiscard]] const ScriptObjectPresentationRecord* record(ObjectId object) const noexcept;
    [[nodiscard]] container::Vector<ScriptObjectPresentationRecord>::iterator findRecord(
        ObjectId object) noexcept;
    [[nodiscard]] container::Vector<ScriptObjectEmoticonPresentation>::iterator findEmoticon(
        ObjectId object) noexcept;
    [[nodiscard]] container::Vector<ScriptObjectEmoticonPresentation>::const_iterator findEmoticon(
        ObjectId object) const noexcept;

    container::Vector<ScriptObjectPresentationRecord> m_records;
    // ObjectId-sorted for deterministic extraction and cheap point lookup.
    container::Vector<ScriptObjectEmoticonPresentation> m_emoticons;
};

} // namespace engine::script
