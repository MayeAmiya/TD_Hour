#pragma once

#include "core/container/container_types.h"
#include "game/base/FrameCommitResult.h"
#include "game/command/CommandOutcome.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "game/script/runtime/ScriptRuntime.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

#include <cstdint>
#include <cstddef>

namespace engine {

class GameSessionScriptFrameTransactions;
class GameSessionContentStartState;
class GameSessionFrameCommitState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Narrow authority used by the app lockstep coordinator. It represents one
// confirmed-frame transaction and its ordered presentation/backend journals;
// no world containers or unrelated script capabilities are exposed.
class GameSessionFramePort final {
public:
    explicit GameSessionFramePort(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents,
        GameSessionFrameCommitState& frame) noexcept
        : m_content(content),
          m_world(world),
          m_presentation(presentation),
          m_objectEvents(objectEvents),
          m_frame(frame),
          m_publication(content, world, presentation, frame) {}
    GameSessionFramePort(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents,
        GameSessionFrameCommitState& frame,
        GameSessionScriptFrameTransactions& scriptRuntime) noexcept
        : GameSessionFramePort(
              content, world, presentation, objectEvents, frame) {
        m_scriptRuntime = &scriptRuntime;
    }

    [[nodiscard]] bool simulationTimeFrozen() const noexcept;
    [[nodiscard]] bool begin(
        uint64_t confirmedFrame,
        bool worldFrozen = false) noexcept;
    [[nodiscard]] const FrameCommitResult& result() const noexcept;
    void noteCommandOutcome(
        bool accepted,
        uint64_t count = 1) noexcept;
    void noteDeferredCommands(uint64_t count) noexcept;
    void noteDegradation(
        FrameDegradation degradation,
        uint64_t count = 1) noexcept;

    [[nodiscard]] script::ScriptRuntimeStepResult advanceScripts(
        uint64_t confirmedInputTick,
        container::Span<const ObjectId> localSelection,
        uint64_t worldConfirmedTick);
    [[nodiscard]] container::Vector<
        script::ScriptForceObjectSelectionPresentation>
    takeForceSelectionPresentations();
    [[nodiscard]] container::Vector<
        script::ScriptMoveCameraToSelectionPresentation>
    takeSelectionCameraPresentations();
    [[nodiscard]] container::Vector<CommandBackendOutcome>
    takeBackendOutcomes();
    [[nodiscard]] container::Vector<ObjectLifecycleEvent>
    lifecyclePresentationEvents(size_t offset = 0) const;

    [[nodiscard]] FrameCommitResult complete(
        bool worldFrozen = false) noexcept;

private:
    void drainVisualAnimationCompletions();

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionObjectEventState& m_objectEvents;
    GameSessionFrameCommitState& m_frame;
    GameSessionGameplayPublicationPort m_publication;
    // Temporary until ScriptRuntime effect application is split from the
    // legacy aggregate authority. No other FramePort operation uses it.
    GameSessionScriptFrameTransactions* m_scriptRuntime = nullptr;
};

} // namespace engine
