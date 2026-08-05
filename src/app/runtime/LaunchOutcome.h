#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace app::runtime {

enum class LaunchOutcomeStage : uint32_t {
    None = 0,
    Descriptor = 1,
    Queued = 2,
    Loading = 3,
    Running = 4,
    Result = 5,
    Runtime = 6,
    Shutdown = 7,
};

enum class LaunchOutcomeCode : uint32_t {
    Pending = 0,
    Running = 1,
    Completed = 2,
    Cancelled = 3,

    DescriptorRejected = 100,
    QueueRejected = 101,
    OutcomePublicationFailed = 102,
    LoadingFailed = 110,
    RenderThreadFailed = 120,
    LogicThreadFailed = 121,
    InputQueueOverflow = 122,
    MainPresentationFailed = 123,
};

namespace LaunchExitCode {
inline constexpr int Success = 0;
inline constexpr int DescriptorRejected = 10;
inline constexpr int QueueRejected = 11;
inline constexpr int OutcomePublicationFailed = 12;
inline constexpr int LoadingFailed = 20;
inline constexpr int RenderThreadFailed = 30;
inline constexpr int LogicThreadFailed = 31;
inline constexpr int InputQueueOverflow = 32;
inline constexpr int MainPresentationFailed = 33;
} // namespace LaunchExitCode

struct LaunchOutcome final {
    static constexpr uint32_t kVersion = 1;

    container::String ticket;
    LaunchOutcomeStage stage = LaunchOutcomeStage::None;
    LaunchOutcomeCode code = LaunchOutcomeCode::Pending;
    container::String reason;
    bool retryable = false;
    bool terminal = false;
    int exitCode = LaunchExitCode::Success;
};

// Main-thread owner for the launcher-visible outcome file. The launcher input
// is a restricted ticket rather than an arbitrary output path, so publication
// cannot escape the mounted user sessions root.
class LaunchOutcomePublisher final {
public:
    void begin(container::String ticket);
    // Formal bootstrap runs before VFS construction.  Bind the outcome to an
    // absolute launcher-owned path beside the descriptor so descriptor
    // rejection remains observable without consulting the process cwd.
    void beginBootstrap(container::String ticket,
                        container::String absoluteOutcomePath);
    [[nodiscard]] bool active() const noexcept { return !m_ticket.empty(); }
    [[nodiscard]] bool terminalPublished() const noexcept {
        return m_terminalPublished;
    }
    [[nodiscard]] LaunchOutcomeStage lastStage() const noexcept {
        return m_lastStage;
    }

    [[nodiscard]] bool publish(
        LaunchOutcomeStage stage, LaunchOutcomeCode code,
        container::StringView reason, bool retryable, bool terminal,
        int exitCode);

private:
    void resetState(container::String ticket);

    container::String m_ticket;
    container::String m_bootstrapOutcomePath;
    container::String m_lastReason;
    LaunchOutcomeStage m_lastStage = LaunchOutcomeStage::None;
    LaunchOutcomeCode m_lastCode = LaunchOutcomeCode::Pending;
    uint64_t m_revision = 0;
    bool m_lastRetryable = false;
    bool m_lastTerminal = false;
    bool m_terminalPublished = false;
    int m_lastExitCode = LaunchExitCode::Success;
};

} // namespace app::runtime
