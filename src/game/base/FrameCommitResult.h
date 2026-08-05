#pragma once

#include <cstdint>

namespace engine {

enum class FrameCommitState : uint8_t {
    NoFrame = 0,
    Open,
    Committed,
    Frozen,
    Faulted,
};

enum class SimulationFaultDomain : uint8_t {
    None = 0,
    FrameIngress,
    ScriptRuntime,
    Command,
    ObjectAI,
    Membership,
    Navigation,
    Production,
    Feedback,
    ObjectSimulation,
    Lifecycle,
    PresentationClock,
};

enum class SimulationFaultCode : uint16_t {
    None = 0,
    InvalidConfirmedTick,
    UnfinishedPriorFrame,
    ScriptTickRejected,
    ScriptRecursionLimit,
    MalformedScriptEffect,
    CapacityExceeded,
    InvalidEvent,
    AtomicCommitFailed,
    AcknowledgementLost,
    PresentationFrameRejected,
};

enum class FrameDegradation : uint32_t {
    None = 0,
    MissingObjectRecipe = 1u << 0u,
    MalformedMapObject = 1u << 1u,
    OptionalVisualUnavailable = 1u << 2u,
    ScenarioBindingSkipped = 1u << 3u,
    ObjectCreationSkipped = 1u << 4u,
    UnsupportedAuthoredFeature = 1u << 5u,
};

[[nodiscard]] constexpr uint32_t frameDegradationBit(
    FrameDegradation value) noexcept {
    return static_cast<uint32_t>(value);
}

struct SimulationFault final {
    SimulationFaultDomain domain = SimulationFaultDomain::None;
    SimulationFaultCode code = SimulationFaultCode::None;
    uint64_t confirmedTick = 0;
    uint32_t subject = 0;
    uint32_t sequence = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return domain != SimulationFaultDomain::None &&
            code != SimulationFaultCode::None;
    }
    constexpr bool operator==(const SimulationFault&) const noexcept = default;
};

// Value-only result for exactly one attempted confirmed frame. Normal command
// rejection and tolerated content degradation do not fault the frame. The
// first structural fault is canonical; later reports are counted but cannot
// replace it, keeping Release behavior independent of logging configuration.
struct FrameCommitResult final {
    static constexpr uint32_t SchemaVersion = 1;

    uint32_t schemaVersion = SchemaVersion;
    FrameCommitState state = FrameCommitState::NoFrame;
    uint64_t confirmedTick = 0;
    uint32_t acceptedCommandCount = 0;
    uint32_t rejectedCommandCount = 0;
    uint32_t deferredCommandCount = 0;
    uint32_t degradationMask = 0;
    uint32_t degradationCount = 0;
    uint32_t additionalFaultCount = 0;
    SimulationFault fault;

    [[nodiscard]] constexpr bool terminal() const noexcept {
        return state == FrameCommitState::Committed ||
            state == FrameCommitState::Frozen ||
            state == FrameCommitState::Faulted;
    }
    [[nodiscard]] constexpr bool committed() const noexcept {
        return state == FrameCommitState::Committed ||
            state == FrameCommitState::Frozen;
    }
    [[nodiscard]] constexpr bool faulted() const noexcept {
        return state == FrameCommitState::Faulted ||
            static_cast<bool>(fault);
    }
    [[nodiscard]] constexpr bool degraded() const noexcept {
        return degradationCount != 0 || degradationMask != 0;
    }
    constexpr bool operator==(const FrameCommitResult&) const noexcept = default;
};

} // namespace engine
