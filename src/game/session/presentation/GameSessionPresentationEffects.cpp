#include "game/session/core/GameSession.h"
#include "game/session/integration/GameSessionScriptPresentationPort.h"
#include "game/script/contracts/ScriptPresentationLimits.h"
#include "core/config/GlobalData.h"
#include "core/container/string_utils.h"
#include "game/base/GameTacticalCamera.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/render/VisualAnimationState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "debug/debug.h"
#include "game/session/presentation/GameSessionPresentationDetail.h"

namespace engine {
using namespace game_session_presentation_detail;

bool script::GameSessionScriptPresentationPort::setLetterbox(bool enabled, uint64_t confirmedTick,
                                                  uint32_t sourceScriptId,
                                                  uint32_t ordinal) noexcept {
    if (!m_content.m_active || m_presentation.m_scriptLetterboxPresentation.enabled == enabled) return false;

    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    m_presentation.m_scriptLetterboxPresentation = {
        .enabled = enabled,
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
    };
    return true;
}

void script::GameSessionScriptPresentationPort::emitScreenShake(script::ScriptScreenShakeIntensity intensity,
                                        uint64_t confirmedTick, uint32_t sourceScriptId,
                                        uint32_t ordinal) {
    if (!m_content.m_active || static_cast<uint8_t>(intensity) >=
                         static_cast<uint8_t>(script::ScriptScreenShakeIntensity::Count)) {
        return;
    }

    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    m_presentation.m_scriptScreenShakeJournal.push_back({
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
        .intensity = intensity,
    });
    static_cast<void>(script::trimScriptPresentationJournal(
        m_presentation.m_scriptScreenShakeJournal,
        kMaximumPendingScriptScreenShakeImpulses,
        m_presentation.m_scriptScreenShakeJournalTrimmedThroughSequence));
}

void script::GameSessionScriptPresentationPort::emitLocalizedCameraShake(
    math::vec3 position, float amplitude, float radius, uint32_t durationTicks,
    uint64_t confirmedTick, uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active || !std::isfinite(position.x()) || !std::isfinite(position.y()) ||
        !std::isfinite(position.z()) || !std::isfinite(amplitude) ||
        !std::isfinite(radius) || durationTicks == 0) {
        return;
    }
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    m_presentation.m_scriptLocalizedCameraShakeJournal.push_back({
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
        .position = position,
        .amplitude = amplitude,
        .radius = radius,
        .durationTicks = durationTicks,
    });
    static_cast<void>(script::trimScriptPresentationJournal(
        m_presentation.m_scriptLocalizedCameraShakeJournal,
        kMaximumPendingScriptLocalizedCameraShakeImpulses,
        m_presentation.m_scriptLocalizedCameraShakeJournalTrimmedThroughSequence));
}

void script::GameSessionScriptPresentationPort::emitMoveCameraToSelection(
    uint64_t confirmedTick, uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active) return;
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    // This queue is move-drained synchronously by GameLogic after each script
    // advance.  It never crosses a latest-value mailbox, so a bounded tail
    // would have no recovery path and would silently reorder a cinematic.
    m_presentation.m_scriptMoveCameraToSelectionRequests.push_back({
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
    });
}

void script::GameSessionScriptPresentationPort::emitCameraCommand(
    ScriptCameraPresentationCommand command, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal, bool startsMovement) {
    if (!m_content.m_active) return;
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0)
        ++m_presentation.m_scriptPresentationSequence;
    if (startsMovement) {
        ++m_presentation.m_scriptCameraMovementRevision;
        if (m_presentation.m_scriptCameraMovementRevision == 0)
            ++m_presentation.m_scriptCameraMovementRevision;
    }
    command.stamp = {
        .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
        .sequence = m_presentation.m_scriptPresentationSequence,
        .confirmedTick = confirmedTick,
        .sourceScriptId = sourceScriptId,
        .ordinal = ordinal,
    };
    command.movementRevision =
        m_presentation.m_scriptCameraMovementRevision;
    auto& journal = m_presentation.m_scriptCameraPresentationJournal;
    journal.push_back(std::move(command));
    static_cast<void>(script::trimScriptPresentationJournal(
        journal, script::kMaximumScriptCameraPresentationCommands,
        m_presentation.m_scriptCameraPresentationJournalTrimmedThroughSequence));
}

bool script::GameSessionScriptPresentationPort::setCameraSlave(
    ObjectId object, container::String boneName, bool enabled, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active) return false;

    // The W3D action accepts an arbitrary name, then self-disables on the
    // next client update if the named Object/Drawable is gone. The modern
    // runtime already resolved a stable ObjectId, so represent that same
    // result as an explicit disabled presentation revision rather than
    // retaining a previous target or fabricating a root-transform fallback.
    const bool effectiveEnabled = enabled && object && !boneName.empty();
    if (!effectiveEnabled) {
        object = INVALID_OBJECT_ID;
        boneName.clear();
    }

    // Repeated Enable actions deliberately get a fresh stamp. They re-arm a
    // renderer that had locally auto-disabled after a missing target, just as
    // W3DView::cameraEnableSlaveMode always writes m_isCameraSlaved=true.
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    m_presentation.m_scriptCameraSlavePresentation = {
        .enabled = effectiveEnabled,
        .object = object,
        .boneName = std::move(boneName),
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
    };
    return true;
}

bool script::GameSessionScriptPresentationPort::emitForceObjectSelection(
    ObjectId object, std::optional<math::vec3> position, bool centerInView,
    container::StringView audioEventName, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame || confirmedTick != m_presentation.m_confirmedTick || !object ||
        audioEventName.size() > script::kMaximumScriptPresentationNameLength ||
        audioEventName.find('\0') != container::StringView::npos) {
        return false;
    }
    if (position && (!std::isfinite(position->x()) || !std::isfinite(position->y()) ||
                     !std::isfinite(position->z()))) {
        return false;
    }

    // A non-centering action never needs a copied transform. Drop it at the
    // session boundary so a hand-authored request cannot make consumers infer
    // a hidden camera side effect from an otherwise false legacy BOOLEAN.
    if (!centerInView) position.reset();

    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    // As with MoveCameraToSelection, this is a same-logic-frame drain queue.
    // Preserve every source-ordered local pulse instead of pretending an old
    // request never happened once a map author emits more than an arbitrary
    // fixed count.
    m_presentation.m_scriptForceObjectSelectionRequests.push_back({
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
        .object = object,
        .position = position,
        .centerInView = centerInView,
        .audioEventName = container::String(audioEventName),
    });
    return true;
}

bool script::GameSessionScriptPresentationPort::setScreenFade(
    script::ScriptScreenFadeBlendMode blendMode, float minimumIntensity,
    float maximumIntensity, int32_t increaseFrames, int32_t holdFrames,
    int32_t decreaseFrames, uint64_t confirmedTick, uint32_t sourceScriptId,
    uint32_t ordinal) noexcept {
    if (!m_content.m_active || !std::isfinite(minimumIntensity) || !std::isfinite(maximumIntensity) ||
        static_cast<uint8_t>(blendMode) >=
            static_cast<uint8_t>(script::ScriptScreenFadeBlendMode::Count)) {
        return false;
    }

    // Unlike letterbox, every Fade action replaces its current curve even
    // when the authored values happen to be identical. ScriptEngine::setFade
    // resets m_curFadeFrame unconditionally, so this is a new presentation
    // revision rather than an idempotent desired-state assignment.
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    m_presentation.m_scriptScreenFadePresentation = {
        .active = true,
        .blendMode = blendMode,
        .minimumIntensity = minimumIntensity,
        .maximumIntensity = maximumIntensity,
        .currentIntensity = minimumIntensity,
        .increaseFrames = increaseFrames,
        .holdFrames = holdFrames,
        .decreaseFrames = decreaseFrames,
        .currentFrame = 0,
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
    };

    // ScriptEngine::setFade immediately calls updateFades only for a zero
    // increase segment. This is observable on the action's own logic frame;
    // negative values intentionally follow the normal next-frame path.
    if (increaseFrames == 0) {
        advanceScreenFadeState(
            m_presentation.m_scriptScreenFadePresentation);
    }
    return true;
}

bool script::GameSessionScriptPresentationPort::setBlackAndWhite(
    bool enabled, int32_t transitionFrames, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active) return false;

    // Every legacy action is a new filter command. In particular, repeated
    // Begin commands reset the renderer-local fade, while repeated End
    // commands must remain observable because this session cannot inspect the
    // renderer's active filter slot. Do not make End idempotent based on the
    // last command or pretend that it synchronously disables a local effect.
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    m_presentation.m_scriptBlackAndWhitePresentation = {
        .enabled = enabled,
        .transitionFrames = transitionFrames,
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
    };
    m_presentation.m_scriptBlackAndWhiteJournal.push_back(m_presentation.m_scriptBlackAndWhitePresentation);
    static_cast<void>(script::trimScriptPresentationJournal(
        m_presentation.m_scriptBlackAndWhiteJournal,
        kMaximumPendingScriptBlackAndWhiteCommands,
        m_presentation.m_scriptBlackAndWhiteJournalTrimmedThroughSequence));
    return true;
}

bool script::GameSessionScriptPresentationPort::emitMotionBlur(
    script::ScriptMotionBlurMode mode, bool saturate,
    std::optional<math::vec3> jumpTarget, int32_t followAmount,
    uint64_t confirmedTick, uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active || static_cast<uint8_t>(mode) >=
            static_cast<uint8_t>(script::ScriptMotionBlurMode::Count)) {
        return false;
    }
    if (jumpTarget && (!std::isfinite(jumpTarget->x()) ||
                       !std::isfinite(jumpTarget->y()) ||
                       !std::isfinite(jumpTarget->z()))) {
        return false;
    }
    // RefCode's JUMP returns before installing the filter if its waypoint is
    // absent. The bridge normally enforces this, while the session keeps the
    // value contract explicit for direct probe/service callers.
    if (mode == script::ScriptMotionBlurMode::ZoomJump && !jumpTarget) return false;
    if (mode != script::ScriptMotionBlurMode::ZoomJump) jumpTarget.reset();

    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    m_presentation.m_scriptMotionBlurPresentation = {
        .mode = mode,
        .saturate = saturate,
        .jumpTarget = std::move(jumpTarget),
        .followAmount = followAmount,
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
    };
    m_presentation.m_scriptMotionBlurJournal.push_back(m_presentation.m_scriptMotionBlurPresentation);
    static_cast<void>(script::trimScriptPresentationJournal(
        m_presentation.m_scriptMotionBlurJournal,
        kMaximumPendingScriptMotionBlurCommands,
        m_presentation.m_scriptMotionBlurJournalTrimmedThroughSequence));
    return true;
}

bool script::GameSessionScriptPresentationPort::setSkybox(
    bool enabled, uint64_t confirmedTick, uint32_t sourceScriptId,
    uint32_t ordinal) noexcept {
    if (!m_content.m_active || m_presentation.m_scriptSkyboxPresentation.enabled == enabled) return false;

    // W3DWater's Begin/End actions simply write m_drawSkyBox. There is no
    // transition or restart behavior to preserve, so duplicate final values
    // intentionally retain their existing revision.
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    m_presentation.m_scriptSkyboxPresentation = {
        .enabled = enabled,
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
    };
    return true;
}

bool script::GameSessionScriptPresentationPort::applyUiPresentation(const script::ScriptUiEffect& effect,
                                            uint64_t confirmedTick,
                                            uint32_t sourceScriptId,
                                            uint32_t ordinal) {
    if (!m_content.m_active) return false;

    const auto nextStamp = [&]() noexcept {
        ++m_presentation.m_scriptPresentationSequence;
        if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
        return script::ScriptPresentationControlStamp{
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        };
    };

    switch (effect.command) {
    case script::ScriptUiCommand::SetControl: {
        bool current = true;
        switch (effect.control) {
        case script::ScriptUiControlKind::GameplayInput:
            current = m_presentation.m_scriptUiPresentation.gameplayInputEnabled();
            break;
        case script::ScriptUiControlKind::SpecialPowerDisplay:
            current = m_presentation.m_scriptUiPresentation.specialPowerDisplayEnabled();
            break;
        case script::ScriptUiControlKind::NamedTimerDisplay:
            current = m_presentation.m_scriptUiPresentation.namedTimerDisplayEnabled();
            break;
        }
        if (current == effect.enabled) return false;
        return m_presentation.m_scriptUiPresentation.setControl(effect.control, effect.enabled, nextStamp());
    }
    case script::ScriptUiCommand::ShowNamedIndicator:
        if (effect.name.empty() || effect.text.empty()) return false;
        return m_presentation.m_scriptUiPresentation.showNamedIndicator(
            effect.name, effect.text, effect.indicatorKind, nextStamp());
    case script::ScriptUiCommand::HideNamedIndicator:
        if (effect.name.empty()) return false;
        return m_presentation.m_scriptUiPresentation.hideNamedIndicator(effect.name, nextStamp());
    case script::ScriptUiCommand::ShowPopup:
        if (effect.text.empty()) return false;
        m_presentation.m_scriptUiPresentation.requestPopup({
            .text = effect.text,
            .localized = true,
            .xPercent = effect.xPercent,
            .yPercent = effect.yPercent,
            .width = effect.width,
            .pauseRequested = effect.pauseRequested,
            .stamp = nextStamp(),
        });
        return true;
    case script::ScriptUiCommand::ShowLocalDefeat:
        m_presentation.m_scriptUiPresentation.requestLocalDefeat(nextStamp());
        return true;
    case script::ScriptUiCommand::FlashCameo:
        if (effect.name.empty() || effect.framesPerFlash == 0) return false;
        m_presentation.m_scriptUiPresentation.enqueueCameoFlash({
            .commandButton = effect.name,
            .flashCount = effect.flashCount,
            .framesPerFlash = effect.framesPerFlash,
            .stamp = nextStamp(),
        });
        return true;
    }
    return false;
}

bool script::GameSessionScriptPresentationPort::applyClientOptions(
    const script::ScriptClientOptionsEffect& effect, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active) return false;
    const auto nextStamp = [&]() noexcept {
        ++m_presentation.m_scriptPresentationSequence;
        if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
        return script::ScriptPresentationControlStamp{
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        };
    };

    switch (effect.command) {
    case script::ScriptClientOptionCommand::SetFrameRateLimit: {
        // RefCode maps exactly zero to GlobalData's default before it writes
        // FramePacer. Do that once at the session boundary; main consumes the
        // detached effective value and never mutates a global configuration.
        int32_t effective = effect.frameRateLimit;
        if (effective == 0) {
            effective = config::TheWritableGlobalData
                ? config::TheWritableGlobalData->framesPerSecondLimit() : 120;
        }
        static_cast<void>(m_presentation.m_scriptClientOptions.setFrameRateLimit(
            effect.frameRateLimit, effective, nextStamp()));
        return true;
    }
    case script::ScriptClientOptionCommand::SetOcclusionMode:
        static_cast<void>(m_presentation.m_scriptClientOptions.setOcclusionEnabled(effect.enabled, nextStamp()));
        return true;
    case script::ScriptClientOptionCommand::SetDrawIconUiMode:
        static_cast<void>(m_presentation.m_scriptClientOptions.setDrawIconUiEnabled(effect.enabled, nextStamp()));
        return true;
    case script::ScriptClientOptionCommand::SetDynamicParticleLodMode:
        static_cast<void>(m_presentation.m_scriptClientOptions.setDynamicParticleLodEnabled(
            effect.enabled, nextStamp()));
        return true;
    }
    return false;
}

bool script::GameSessionScriptPresentationPort::applyCommandBarOverride(
    const script::ScriptCommandBarOverrideEffect& effect, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active) return false;
    const auto nextStamp = [&]() noexcept {
        ++m_presentation.m_scriptPresentationSequence;
        if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
        return game::CommandBarOverrideMutationStamp{
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        };
    };

    const container::SharedPtr<const game::ObjectArchetype> objectTemplate =
        m_content.m_contentSnapshot.findObjectArchetype(effect.objectTypeName);
    // ScriptActions returns without side effects for an absent Object type or
    // a template with no CommandSet. Treat that legacy-compatible no-op as
    // accepted so the bridge does not manufacture a false diagnostic.
    if (!objectTemplate || objectTemplate->templateData.commandSet.empty()) return true;
    const game::CommandSetTemplate* commandSet =
        m_content.m_contentSnapshot.findCommandSet(objectTemplate->templateData.commandSet);
    if (!commandSet) return true;

    switch (effect.command) {
    case script::ScriptCommandBarOverrideCommand::RemoveButtonFromObjectType: {
        if (effect.commandButtonName.empty()) return true;
        size_t slot = game::COMMAND_SET_SLOT_COUNT;
        for (size_t index = 0; index < game::COMMAND_SET_SLOT_COUNT; ++index) {
            const container::StringView effective =
                m_presentation.m_scriptCommandBarOverrides.effectiveButtonName(
                    commandSet->name, index, commandSet->commands[index]);
            if (effective == effect.commandButtonName) {
                slot = index;
                break;
            }
        }
        if (slot == game::COMMAND_SET_SLOT_COUNT) return true;
        static_cast<void>(m_presentation.m_scriptCommandBarOverrides.setSlotOverride(
            commandSet->name, slot, {}, nextStamp()));
        return true;
    }
    case script::ScriptCommandBarOverrideCommand::AddButtonToObjectTypeSlot:
        // RefCode validates the button and converts the author-facing
        // one-based slot before mutation. Invalid values are silent no-ops.
        if (!m_content.m_contentSnapshot.findCommandButton(effect.commandButtonName) ||
            effect.oneBasedSlot <= 0 ||
            effect.oneBasedSlot > static_cast<int32_t>(game::COMMAND_SET_SLOT_COUNT)) {
            return true;
        }
        static_cast<void>(m_presentation.m_scriptCommandBarOverrides.setSlotOverride(
            commandSet->name, static_cast<size_t>(effect.oneBasedSlot - 1),
            effect.commandButtonName, nextStamp()));
        return true;
    }
    return false;
}

bool script::GameSessionScriptPresentationPort::applyViewCompatibility(
    const script::ScriptViewCompatibilityEffect& effect, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal) noexcept {
    if (!m_content.m_active) return false;
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
    const script::ScriptPresentationControlStamp stamp{
        .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
        .sequence = m_presentation.m_scriptPresentationSequence,
        .confirmedTick = confirmedTick,
        .sourceScriptId = sourceScriptId,
        .ordinal = ordinal,
    };

    switch (effect.command) {
    case script::ScriptViewCompatibilityCommand::SetTerrainOversizeTiles:
        // D3D12TerrainVisual owns a complete immutable map chunk set rather
        // than W3D's moving terrain window. Retain the authored amount for
        // compatibility/diagnostics, but never allocate/rebuild GPU terrain
        // from a map-script integer.
        return m_presentation.m_scriptViewCompatibility.setTerrainOversizeTiles(
            effect.terrainOversizeTiles, stamp);
    case script::ScriptViewCompatibilityCommand::SetGuardBandBias:
        return m_presentation.m_scriptViewCompatibility.setGuardBandBias(
            effect.guardBandX, effect.guardBandY, stamp);
    }
    return false;
}

bool script::GameSessionScriptPresentationPort::applyObjectPresentation(
    const script::ScriptObjectPresentationEffect& effect, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal, PlayerId currentPlayer,
    container::StringView currentPlayerAlias) {
    static_cast<void>(currentPlayer);
    static_cast<void>(currentPlayerAlias);
    if (!m_content.m_active) return false;

    const auto liveObject = [this](ObjectId object) noexcept {
        const std::optional<ecs::entity> entity = entityFromId(object);
        return entity.has_value();
    };
    const auto liveRenderable = [this, &liveObject](ObjectId object) noexcept {
        if (!liveObject(object)) return false;
        const std::optional<ecs::entity> entity = entityFromId(object);
        if (!entity) return false;
        const RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(m_world.m_registry, *entity);
        return visual && !visual->modelAsset.empty();
    };
    const auto nextStamp = [&]() noexcept {
        ++m_presentation.m_scriptPresentationSequence;
        if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
        return script::ScriptPresentationControlStamp{
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        };
    };

    switch (effect.command) {
    case script::ScriptObjectPresentationCommand::CreateRadarEvent:
        // The bridge maps this command directly to ScriptMapPresentation so
        // the radar event state remains the single authoritative owner.
        return false;
    case script::ScriptObjectPresentationCommand::SetCustomIndicatorColor: {
        // RefCode stores this state on Object rather than Drawable. Retain it
        // for an alive Object even if a visual module has not attached yet;
        // a later name transfer or renderer bind must still inherit it.
        if (!effect.object || !liveObject(*effect.object)) return true;
        const uint32_t color = effect.packedColor;
        const script::ScriptPresentationControlStamp stamp = nextStamp();
        // Object::m_indicatorColor uses exact packed zero as its "use Team
        // colour" sentinel. A black Color with a non-zero alpha byte remains
        // a valid explicit override, so test the packed value before RGB
        // decoding.
        if (color == 0) {
            static_cast<void>(m_presentation.m_scriptObjectPresentation.clearCustomIndicatorColor(
                *effect.object, stamp));
            return true;
        }
        const math::vec3 decoded{
            static_cast<float>((color >> 16u) & 0xffu) / 255.0f,
            static_cast<float>((color >> 8u) & 0xffu) / 255.0f,
            static_cast<float>(color & 0xffu) / 255.0f,
        };
        static_cast<void>(m_presentation.m_scriptObjectPresentation.setCustomIndicatorColor(
            *effect.object, decoded, stamp));
        return true;
    }
    case script::ScriptObjectPresentationCommand::SetEmoticon: {
        // ScriptActions does its `(Int)(duration * LOGICFRAMES_PER_SECOND)`
        // conversion at apply time, after the target has been resolved. Keep
        // the authored REAL out of ScriptRuntime and use this session's
        // frozen rate once. Negative values map to Drawable's FOREVER.
        if (!std::isfinite(effect.emoticonDurationSeconds)) return true;
        const uint32_t logicFramesPerSecond = static_cast<uint32_t>(
            std::max(1, m_content.m_startInfo.gameSpeedFPS));
        std::optional<uint64_t> lastVisibleTick;
        bool clearEmoticon = false;
        if (effect.emoticonDurationSeconds >= 0.0f) {
            const double scaledDuration =
                static_cast<double>(effect.emoticonDurationSeconds) *
                static_cast<double>(logicFramesPerSecond);
            // Positive finite values cannot produce a negative result. Use
            // truncation explicitly: the original C-style Int conversion
            // truncates toward zero, rather than rounding animation life.
            const uint64_t durationFrames = scaledDuration >=
                    static_cast<double>(std::numeric_limits<uint64_t>::max())
                ? std::numeric_limits<uint64_t>::max()
                : static_cast<uint64_t>(std::trunc(scaledDuration));
            // Script opcode documentation and campaign content use zero as
            // the explicit clear request. Do not leave a one-render-frame
            // icon behind when the truncation of a fractional value reaches
            // zero; this is the modernized, stable interpretation of the
            // old `0 get rid of` contract.
            clearEmoticon = durationFrames == 0;
            if (!clearEmoticon) {
                lastVisibleTick = confirmedTick >
                        std::numeric_limits<uint64_t>::max() - durationFrames
                    ? std::numeric_limits<uint64_t>::max()
                    : confirmedTick + durationFrames;
            }
        }

        container::Vector<ObjectId> targets;
        if (effect.target.kind == script::ScriptObjectPresentationTargetKind::NamedObject) {
            if (effect.object && liveRenderable(*effect.object)) targets.push_back(*effect.object);
        } else if (effect.target.kind == script::ScriptObjectPresentationTargetKind::Team) {
            const std::optional<ObjectTeamId> team = effect.team
                ? std::optional<ObjectTeamId>{effect.team}
                : m_queries.resolveScenarioTeamAlias(effect.target.name);
            if (team) {
                for (const ObjectId object : m_world.m_objectTeams.members(*team)) {
                    if (liveRenderable(object)) targets.push_back(object);
                }
            }
        } else {
            return false;
        }
        if (targets.empty()) return true;

        const script::ScriptPresentationControlStamp stamp = nextStamp();
        for (const ObjectId object : targets) {
            if (clearEmoticon) {
                static_cast<void>(m_presentation.m_scriptObjectPresentation.clearEmoticon(object));
                continue;
            }
            // A new request replaces the old ObjectId slot even if it names
            // the same Anim2D template, restarting its frame progression.
            static_cast<void>(m_presentation.m_scriptObjectPresentation.setEmoticon(
                object, effect.emoticonName, confirmedTick, lastVisibleTick, stamp));
        }
        return true;
    }
    case script::ScriptObjectPresentationCommand::SetAmbientSoundEnabled: {
        // ScriptActions silently ignores a missing named Object/Drawable or
        // an object with no ambient event.  The modern path makes the same
        // decision while it can still see the session-frozen archetype; the
        // later audio subsystem receives only an ObjectId and event name.
        if (!effect.object || !liveObject(*effect.object)) return true;
        return setObjectAmbientAudioEnabled(
            *effect.object, effect.ambientSoundEnabled);
    }
    case script::ScriptObjectPresentationCommand::SetSpecialPowerDisplayVisible: {
        // This is distinct from ENABLE/DISABLE_SPECIAL_POWER_DISPLAY: the
        // original stores a per-Object exclusion in InGameUI while leaving
        // the global superweapon panel enabled.
        if (!effect.object || !liveObject(*effect.object)) return true;
        static_cast<void>(m_presentation.m_scriptObjectPresentation.setSpecialPowerDisplayVisible(
            *effect.object, effect.specialPowerDisplayVisible, nextStamp()));
        return true;
    }
    case script::ScriptObjectPresentationCommand::Flash: {
        if (effect.durationSeconds <= 0) return true;
        const uint32_t logicFramesPerSecond = static_cast<uint32_t>(
            std::max(1, m_content.m_startInfo.gameSpeedFPS));
        const uint64_t seconds = static_cast<uint64_t>(effect.durationSeconds);
        const uint64_t interval = script::ScriptObjectPresentationState::flashIntervalTicks(
            logicFramesPerSecond);
        const uint32_t decayTicks = script::ScriptObjectPresentationState::flashDecayTicks(
            logicFramesPerSecond);
        // The action's duration is authored in whole seconds. At the
        // original 30 Hz, Drawable flashes every 15 frames, exactly twice
        // per authored second. Derive the count in that reference domain,
        // then schedule the individual pulses in this session's scaled tick
        // domain; computing `frames / roundedInterval` would incorrectly
        // drop one pulse at rates such as 45 Hz.
        constexpr uint64_t kReferencePulsesPerSecond =
            script::ScriptObjectPresentationState::kReferenceLogicFramesPerSecond /
            script::ScriptObjectPresentationState::kLegacyFlashIntervalTicks;
        static_assert(kReferencePulsesPerSecond != 0);
        const uint64_t pulseCount = seconds > std::numeric_limits<uint64_t>::max() /
                kReferencePulsesPerSecond
            ? std::numeric_limits<uint64_t>::max()
            : seconds * kReferencePulsesPerSecond;
        if (pulseCount == 0) return true;
        const uint64_t activeTicks = pulseCount > std::numeric_limits<uint64_t>::max() / interval
            ? std::numeric_limits<uint64_t>::max()
            : pulseCount * interval;
        // Drawable::update() does not pulse relative to the action's issue
        // tick. It checks the shared GameClient frame modulo
        // DRAWABLE_FRAMES_PER_FLASH, so an action issued between boundaries
        // waits for the next global 15-frame boundary. Preserve that phase
        // rather than starting every object at an arbitrary local tick.
        const uint64_t remainder = confirmedTick % interval;
        const uint64_t ticksUntilFirstPulse = remainder == 0 ? 0 : interval - remainder;
        const uint64_t firstPulseTick = confirmedTick >
                std::numeric_limits<uint64_t>::max() - ticksUntilFirstPulse
            ? std::numeric_limits<uint64_t>::max()
            : confirmedTick + ticksUntilFirstPulse;
        const uint64_t endTick = firstPulseTick > std::numeric_limits<uint64_t>::max() - activeTicks
            ? std::numeric_limits<uint64_t>::max() : firstPulseTick + activeTicks;
        if (endTick <= firstPulseTick) return true;

        container::Vector<ObjectId> targets;
        if (effect.target.kind == script::ScriptObjectPresentationTargetKind::NamedObject) {
            if (effect.object && liveRenderable(*effect.object)) targets.push_back(*effect.object);
        } else if (effect.target.kind == script::ScriptObjectPresentationTargetKind::Team) {
            const std::optional<ObjectTeamId> team = effect.team
                ? std::optional<ObjectTeamId>{effect.team}
                : m_queries.resolveScenarioTeamAlias(effect.target.name);
            if (team) {
                for (const ObjectId object : m_world.m_objectTeams.legacyMembers(*team)) {
                    // RefCode breaks, rather than skips, at the first Team
                    // member without a Drawable. The prefix is observable
                    // because Team membership is newest-first.
                    if (!liveRenderable(object)) break;
                    targets.push_back(object);
                }
            }
        } else {
            return false;
        }
        if (targets.empty()) return true;

        const script::ScriptPresentationControlStamp stamp = nextStamp();
        for (const ObjectId object : targets) {
            const math::vec3 color = effect.flashColor ==
                    script::ScriptObjectPresentationFlashColor::White
                ? math::vec3{1.0f, 1.0f, 1.0f}
                : scriptIndicatorColor(object);
            static_cast<void>(m_presentation.m_scriptObjectPresentation.startFlash(
                object, color, firstPulseTick, endTick,
                static_cast<uint32_t>(interval), decayTicks, stamp));
        }
        return true;
    }
    }
    return false;
}

math::vec3 script::GameSessionScriptPresentationPort::scriptIndicatorColor(
    ObjectId object) const noexcept {
    return resolveScriptIndicatorColor(
        m_presentation.m_scriptObjectPresentation, m_world.m_registry,
        m_content.m_players, m_content.m_ruleset.get(),
        entityFromId(object), object);
}

} // namespace engine
