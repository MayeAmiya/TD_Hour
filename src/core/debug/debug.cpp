#include "debug.h"

#include "container/hash_containers.h"

#include <cstdlib>
#include <intrin.h>
#include <limits>
#include <mutex>

#if TD_LOG_ENABLED
    #include <spdlog/spdlog.h>
    #include <spdlog/sinks/basic_file_sink.h>
    #include <spdlog/sinks/stdout_color_sinks.h>
#endif

// 使用同步日志器避免异步线程池 segfault

namespace debug {
namespace {

#if TD_LOG_ENABLED
struct LogState final {
    container::SharedPtr<spdlog::logger> logger;
    bool initialized = false;
};

LogState& logState() noexcept {
    static LogState state;
    return state;
}

struct AggregatedLogState final {
    uint64_t total = 0;
    uint64_t reported = 0;
    uint64_t summaryInterval = 256u;
};

struct AggregatedLogRegistry final {
    std::mutex mutex;
    container::HashMap<container::String, AggregatedLogState> entries;
};

AggregatedLogRegistry& aggregatedLogRegistry() {
    static AggregatedLogRegistry registry;
    return registry;
}
#endif

} // namespace

#if TD_LOG_ENABLED
void log::init(const char* logFile) noexcept {
    LogState& state = logState();
    if (state.initialized) return;
    try {
        container::Vector<spdlog::sink_ptr> sinks;
        sinks.push_back(
            std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        sinks.push_back(
            std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                logFile ? logFile : "generals.log", true));
        state.logger = std::make_shared<spdlog::logger>(
            "game", sinks.begin(), sinks.end());
        spdlog::register_logger(state.logger);
        spdlog::set_default_logger(state.logger);
#if TD_DEBUG_ENABLED
        state.logger->set_level(spdlog::level::debug);
#else
        state.logger->set_level(spdlog::level::info);
#endif
        state.logger->flush_on(spdlog::level::info);
        state.logger->set_pattern(
            "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        state.initialized = true;
    } catch (const spdlog::spdlog_ex&) {
    }
}

void log::shutdown() noexcept {
    flushAggregates();
    spdlog::shutdown();
    LogState& state = logState();
    state.logger.reset();
    state.initialized = false;
}

void log::flush() noexcept {
    if (logState().logger) logState().logger->flush();
}

void log::setTraceEnabled(bool enabled) noexcept {
#if TD_DEBUG_ENABLED
    if (logState().logger) {
        logState().logger->set_level(
            enabled ? spdlog::level::trace : spdlog::level::debug);
    }
#else
    static_cast<void>(enabled);
#endif
}

namespace {
spdlog::level::level_enum toSpdlogLevel(log_level level) noexcept {
    switch (level) {
    case log_level::trace: return spdlog::level::trace;
    case log_level::debug: return spdlog::level::debug;
    case log_level::info: return spdlog::level::info;
    case log_level::warning: return spdlog::level::warn;
    case log_level::error: return spdlog::level::err;
    case log_level::critical: return spdlog::level::critical;
    }
    return spdlog::level::off;
}
} // namespace

bool log::accepts(log_level level) noexcept {
    const auto& logger = logState().logger;
    return logger && logger->should_log(toSpdlogLevel(level));
}

void log::write(log_level level, container::StringView message) noexcept {
    try {
        const auto& logger = logState().logger;
        if (!logger) return;
        logger->log(
            toSpdlogLevel(level),
            spdlog::string_view_t{message.data(), message.size()});
    } catch (...) {
    }
}

aggregated_log_decision log::aggregate(
    container::StringView key, uint64_t summaryInterval) noexcept {
    try {
        AggregatedLogRegistry& registry = aggregatedLogRegistry();
        std::scoped_lock lock(registry.mutex);
        const container::String stableKey = key.empty()
            ? container::String{"<empty-hot-log-key>"}
            : container::String{key};
        auto [entry, inserted] = registry.entries.try_emplace(
            stableKey, AggregatedLogState{
                .summaryInterval = std::max<uint64_t>(2u, summaryInterval),
            });
        AggregatedLogState& state = entry->second;
        if (state.total != std::numeric_limits<uint64_t>::max()) {
            ++state.total;
        }
        if (inserted || state.total == 1u) {
            state.reported = state.total;
            return {
                .action = aggregated_log_action::first_sample,
                .total = state.total,
            };
        }
        if (state.total - state.reported >= state.summaryInterval) {
            const uint64_t additional = state.total - state.reported;
            state.reported = state.total;
            return {
                .action = aggregated_log_action::summary,
                .total = state.total,
                .additional = additional,
            };
        }
        return {.total = state.total};
    } catch (...) {
        // Allocation failure in diagnostics must not affect the game. Fall
        // back to a full sample so the underlying failure is still visible.
        return {.action = aggregated_log_action::first_sample, .total = 1u};
    }
}

void log::flushAggregates() noexcept {
    struct PendingSummary final {
        container::String key;
        uint64_t total = 0;
        uint64_t additional = 0;
    };
    try {
        container::Vector<PendingSummary> pending;
        AggregatedLogRegistry& registry = aggregatedLogRegistry();
        {
            std::scoped_lock lock(registry.mutex);
            pending.reserve(registry.entries.size());
            for (const auto& [key, state] : registry.entries) {
                if (state.total > state.reported) {
                    pending.push_back({
                        .key = key,
                        .total = state.total,
                        .additional = state.total - state.reported,
                    });
                }
            }
            registry.entries.clear();
        }
        if (!logState().logger) return;
        for (const PendingSummary& summary : pending) {
            logState().logger->warn(
                "[HotLog] '{}' repeated {} additional times (total={})",
                summary.key, summary.additional, summary.total);
        }
    } catch (...) {
        // Logging shutdown remains noexcept even under allocation pressure.
    }
}
#endif

namespace detail {

[[noreturn]] void assertion_failed(const char* expression,
                                   const char* message,
                                   const char* file,
                                   int line) noexcept
{
#if TD_LOG_ENABLED
    if (message && *message)
    {
        spdlog::critical("Assertion failed: {} ({}) at {}:{}",
                         expression, message, file, line);
    }
    else
    {
        spdlog::critical("Assertion failed: {} at {}:{}",
                         expression, file, line);
    }
#else
    static_cast<void>(expression);
    static_cast<void>(message);
    static_cast<void>(file);
    static_cast<void>(line);
#endif

    __debugbreak();
    std::abort();
}

} // namespace detail
} // namespace debug
