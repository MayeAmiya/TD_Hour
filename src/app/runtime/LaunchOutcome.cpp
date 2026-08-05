#include "LaunchOutcome.h"

#include "core/constants/Paths.h"
#include "core/io/VFS.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace app::runtime {
namespace {

container::String outcomePath(container::StringView ticket) {
    return container::String{USER_SESSION_ROOT} + "/" +
        container::String{ticket} + ".outcome.ini";
}

container::String sanitizeIniValue(container::StringView value) {
    container::String result{value};
    std::replace(result.begin(), result.end(), '\r', ' ');
    std::replace(result.begin(), result.end(), '\n', ' ');
    return result;
}

} // namespace

void LaunchOutcomePublisher::resetState(container::String ticket) {
    m_ticket = std::move(ticket);
    m_lastReason.clear();
    m_lastStage = LaunchOutcomeStage::None;
    m_lastCode = LaunchOutcomeCode::Pending;
    m_revision = 0;
    m_lastRetryable = false;
    m_lastTerminal = false;
    m_terminalPublished = false;
    m_lastExitCode = LaunchExitCode::Success;
}

void LaunchOutcomePublisher::begin(container::String ticket) {
    m_bootstrapOutcomePath.clear();
    resetState(std::move(ticket));
}

void LaunchOutcomePublisher::beginBootstrap(
    container::String ticket, container::String absoluteOutcomePath) {
    const std::filesystem::path output{absoluteOutcomePath};
    m_bootstrapOutcomePath = output.is_absolute()
        ? output.lexically_normal().string()
        : container::String{};
    resetState(std::move(ticket));
}

bool LaunchOutcomePublisher::publish(
    LaunchOutcomeStage stage, LaunchOutcomeCode code,
    container::StringView reason, bool retryable, bool terminal,
    int exitCode) {
    if (!active() || m_terminalPublished) return true;

    const container::String safeReason = sanitizeIniValue(reason);
    if (m_revision != 0 && stage == m_lastStage && code == m_lastCode &&
        safeReason == m_lastReason && retryable == m_lastRetryable &&
        terminal == m_lastTerminal && exitCode == m_lastExitCode) {
        return true;
    }

    const uint64_t nextRevision = m_revision + 1;
    std::ostringstream stream;
    stream << "[LaunchOutcome]\n"
           << "Version=" << LaunchOutcome::kVersion << '\n'
           << "Revision=" << nextRevision << '\n'
           << "Ticket=" << m_ticket << '\n'
           << "Stage=" << static_cast<uint32_t>(stage) << '\n'
           << "Code=" << static_cast<uint32_t>(code) << '\n'
           << "Reason=" << safeReason << '\n'
           << "Retryable=" << (retryable ? "true" : "false") << '\n'
           << "Terminal=" << (terminal ? "true" : "false") << '\n'
           << "ExitCode=" << exitCode << '\n';

    bool written = false;
    if (!m_bootstrapOutcomePath.empty()) {
        std::ofstream output(m_bootstrapOutcomePath,
                             std::ios::binary | std::ios::trunc);
        const container::String payload = stream.str();
        output.write(payload.data(),
                     static_cast<std::streamsize>(payload.size()));
        output.flush();
        written = output.good();
    } else {
        written = io::VFS::instance().writeAll(outcomePath(m_ticket),
                                               stream.str());
    }
    if (!written) {
        return false;
    }

    m_revision = nextRevision;
    m_lastStage = stage;
    m_lastCode = code;
    m_lastReason = safeReason;
    m_lastRetryable = retryable;
    m_lastTerminal = terminal;
    m_lastExitCode = exitCode;
    m_terminalPublished = terminal;
    return true;
}

} // namespace app::runtime
