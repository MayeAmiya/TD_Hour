#pragma once

#include "core/ecs/registry.h"
#include "game/session/integration/GameSessionScriptQueryPort.h"
#include "game/script/bridge/ScriptSessionEvents.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "game/script/runtime/ScriptRuntime.h"

namespace engine {
class GameCameraDirector;
struct GameStartInfo;
}

namespace game::terrain {
class TerrainLogic;
}

namespace game {
struct GameAudioControlEvent;
struct GameAudioEvent;
}

namespace engine::script {

class GameSessionScriptPresentationPort;

namespace detail {
[[nodiscard]] bool applyPresentationEffect(
    GameSessionScriptPresentationPort& port, const ScriptEffect& effect);
} // namespace detail

// Applies local audiovisual/UI/camera effects. It cannot commit gameplay
// authority and may only update the query port's local-selection projection.
class GameSessionScriptPresentationPort final {
public:
    GameSessionScriptPresentationPort(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionScriptQueryPort& queries,
        GameSessionScriptLocalPresentationState& localPresentation,
        uint64_t confirmedTick) noexcept;

    [[nodiscard]] bool apply(const ScriptEffect& effect);

private:
    friend bool detail::applyPresentationEffect(
        GameSessionScriptPresentationPort& port, const ScriptEffect& effect);

    [[nodiscard]] std::optional<ObjectTeamId> resolveEffectTeam(
        container::StringView name,
        const ScriptEffectHeader& header) const noexcept;
    void emitDiagnostic(
        const ScriptEffectHeader& header, container::String text);
    void emitSessionEvent(ScriptSessionEvent event);
    [[nodiscard]] bool emitMoviePresentation(
        ScriptMovieTarget target, container::String movieName,
        uint64_t confirmedTick, uint32_t sourceScriptId, uint32_t ordinal);
    [[nodiscard]] bool beginMusicCompletionTracking(
        container::String trackName, uint64_t confirmedTick,
        uint32_t sourceScriptId, uint32_t ordinal);
    [[nodiscard]] bool emitAudioEvent(game::GameAudioEvent event);
    [[nodiscard]] bool emitAudioControlEvent(
        game::GameAudioControlEvent event);
    [[nodiscard]] bool applyUiPresentation(
        const ScriptUiEffect& effect, uint64_t confirmedTick,
        uint32_t sourceScriptId, uint32_t ordinal);
    [[nodiscard]] bool applyCommandBarOverride(
        const ScriptCommandBarOverrideEffect& effect,
        uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal);
    [[nodiscard]] bool applyClientOptions(
        const ScriptClientOptionsEffect& effect, uint64_t confirmedTick,
        uint32_t sourceScriptId, uint32_t ordinal);
    [[nodiscard]] bool applyViewCompatibility(
        const ScriptViewCompatibilityEffect& effect,
        uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal) noexcept;
    [[nodiscard]] bool applyObjectPresentation(
        const ScriptObjectPresentationEffect& effect,
        uint64_t confirmedTick, uint32_t sourceScriptId, uint32_t ordinal,
        PlayerId currentPlayer, container::StringView currentPlayerAlias);
    [[nodiscard]] bool applyMapPresentation(
        const ScriptMapPresentationEffect& effect,
        uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal);
    [[nodiscard]] bool setObjectAmbientAudioEnabled(
        ObjectId object, bool enabled);
    [[nodiscard]] math::vec3 scriptIndicatorColor(
        ObjectId object) const noexcept;
    [[nodiscard]] bool emitForceObjectSelection(
        ObjectId object, std::optional<math::vec3> position,
        bool centerInView, container::StringView audioEventName,
        uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal);
    [[nodiscard]] bool setLetterbox(
        bool enabled, uint64_t confirmedTick,
        uint32_t sourceScriptId, uint32_t ordinal) noexcept;
    void emitMoveCameraToSelection(
        uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal);
    void emitCameraCommand(
        ScriptCameraPresentationCommand command,
        uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal, bool startsMovement);
    [[nodiscard]] bool setCameraSlave(
        ObjectId object, container::String boneName, bool enabled,
        uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal);
    void emitScreenShake(
        ScriptScreenShakeIntensity intensity, uint64_t confirmedTick,
        uint32_t sourceScriptId, uint32_t ordinal);
    void emitLocalizedCameraShake(
        math::vec3 position, float amplitude, float radius,
        uint32_t durationTicks, uint64_t confirmedTick,
        uint32_t sourceScriptId, uint32_t ordinal);
    [[nodiscard]] bool setScreenFade(
        ScriptScreenFadeBlendMode blendMode, float minimumIntensity,
        float maximumIntensity, int32_t increaseFrames,
        int32_t holdFrames, int32_t decreaseFrames,
        uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal) noexcept;
    [[nodiscard]] bool setBlackAndWhite(
        bool enabled, int32_t transitionFrames,
        uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal);
    [[nodiscard]] bool emitMotionBlur(
        ScriptMotionBlurMode mode, bool saturate,
        std::optional<math::vec3> jumpTarget, int32_t followAmount,
        uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal);
    [[nodiscard]] bool setSkybox(
        bool enabled, uint64_t confirmedTick,
        uint32_t sourceScriptId, uint32_t ordinal) noexcept;

    struct ForceObjectSelectionTarget final {
        ObjectId object = INVALID_OBJECT_ID;
        std::optional<math::vec3> position;
    };
    [[nodiscard]] std::optional<ForceObjectSelectionTarget>
        resolveForceObjectSelectionTarget(
            ObjectTeamId team, container::StringView objectTypeName,
            bool capturePosition) const noexcept;
    [[nodiscard]] std::optional<ecs::entity> entityFromId(
        ObjectId object) const noexcept;

    [[nodiscard]] const GameStartInfo& startInfo() const noexcept;
    game::terrain::TerrainLogic& terrain() noexcept;
    GameCameraDirector& cameraDirector() noexcept;
    void armCameraTimeFreeze() noexcept;
    void setCameraFollow(ObjectId object, bool snap) noexcept;
    void stopCameraFollow() noexcept;
    void setCameraTether(ObjectId object, bool snap, float play) noexcept;
    void stopCameraTether() noexcept;
    [[nodiscard]] bool setTreeSwayPresentation(
        float directionRadians, float intensityRadians, float leanRadians,
        int32_t periodFrames, float randomness, uint64_t confirmedTick,
        uint32_t sourceScriptId, uint32_t ordinal) noexcept;
    [[nodiscard]] bool setWeatherPresentation(
        bool visible, uint64_t confirmedTick, uint32_t sourceScriptId,
        uint32_t ordinal) noexcept;
    [[nodiscard]] bool setInfantryLightingPresentation(
        std::optional<float> overrideScale, uint64_t confirmedTick,
        uint32_t sourceScriptId, uint32_t ordinal) noexcept;

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionScriptQueryPort& m_queries;
    GameSessionScriptLocalPresentationState& m_localPresentation;
    uint64_t m_confirmedTick = 0;
};

} // namespace engine::script
