#pragma once

#include "game/session/integration/GamePresentationContentSnapshot.h"
#include "presentation/audio/AudioPresentationContracts.h"
#include "presentation/fx/runtime/FxPresentationSnapshot.h"

#include <cstddef>
#include <cstdint>

namespace engine {

class GameSession;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Lossless presentation media boundary. Content is immutable; FX/audio
// drains and completion admission remain ordered session operations.
class GameSessionMediaPresentationPort final {
public:
    [[nodiscard]] GamePresentationContentSnapshot content() const noexcept;
    [[nodiscard]] fx::FxPresentationSnapshot takeFx(
        uint64_t simulationFrame);
    [[nodiscard]] audio::AudioPresentationSnapshot takeAudio(
        uint64_t simulationFrame);
    [[nodiscard]] size_t admitAudioCompletions(
        container::Vector<audio::AudioNaturalCompletion> completions);

private:
    friend class GameSession;

    GameSessionMediaPresentationPort(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation) noexcept
        : m_content(&content), m_world(&world), m_presentation(&presentation) {}

    GameSessionContentStartState* m_content = nullptr;
    GameSessionWorldState* m_world = nullptr;
    GameSessionScriptPresentationState* m_presentation = nullptr;
};

} // namespace engine
