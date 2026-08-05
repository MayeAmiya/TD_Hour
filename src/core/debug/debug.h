#pragma once

#include "container/container_types.h"

#include "td_assert.h"

#include <cstdint>
#if TD_LOG_ENABLED
    #include <fmt/format.h>
#endif

// ── Tracy ───────────────────────────────────────────────────────────────
#if TD_DEBUG_ENABLED
    #include <tracy/Tracy.hpp>
#else
    #define ZoneScoped
    #define ZoneScopedN(...)
    #define ZoneText(...)
    #define ZoneValue(...)
    #define ZoneColor(...)
    #define FrameMark
    #define TracyPlot(...)
    #define TracyPlotConfig(...)
    #define TracyAlloc(...)
    #define TracyFree(...)
    #define TracyMessage(...)
    #define TracyMessageL(...)
#endif

namespace debug {

enum class log_level : uint8_t {
    trace,
    debug,
    info,
    warning,
    error,
    critical,
};

enum class aggregated_log_action : uint8_t {
    suppress,
    first_sample,
    summary,
};

struct aggregated_log_decision final {
    aggregated_log_action action = aggregated_log_action::suppress;
    uint64_t total = 0;
    uint64_t additional = 0;
};

class log
{
public:
#if TD_LOG_ENABLED
    static void init(const char* log_file = "generals.log") noexcept;
    static void shutdown() noexcept;
    static void flush() noexcept;
    static void setTraceEnabled(bool enabled) noexcept;
    [[nodiscard]] static bool accepts(log_level level) noexcept;
    static void write(log_level level, container::StringView message) noexcept;

    // Aggregates a stable error key across all engine systems. The first
    // occurrence remains a full caller-provided sample; repeated occurrences
    // are suppressed and periodically summarized. shutdown() publishes the
    // final partial interval before the logger is released.
    [[nodiscard]] static aggregated_log_decision aggregate(
        container::StringView key,
        uint64_t summaryInterval = 256u) noexcept;

private:
    static void flushAggregates() noexcept;
#else
    static void init(const char* = "generals.log") noexcept {}
    static void shutdown() noexcept {}
    static void flush() noexcept {}
    static void setTraceEnabled(bool) noexcept {}
    [[nodiscard]] static bool accepts(log_level) noexcept { return false; }
    static void write(log_level, container::StringView) noexcept {}
    [[nodiscard]] static aggregated_log_decision aggregate(
        container::StringView, uint64_t = 256u) noexcept {
        return {};
    }
#endif
};

class log_scope final
{
public:
    explicit log_scope(const char* log_file = "generals.log") noexcept
    {
        log::init(log_file);
    }

    ~log_scope() noexcept { log::shutdown(); }

    log_scope(const log_scope&) = delete;
    log_scope& operator=(const log_scope&) = delete;
};

} // namespace debug

#if TD_LOG_ENABLED
namespace debug::detail {

// Keep spdlog and its sinks out of every gameplay translation unit. Only this
// small formatting adapter is instantiated at call sites; the actual logger,
// sinks and level dispatch live in debug.cpp.
template <typename... Args>
void write_formatted(log_level level, container::StringView pattern,
                     Args&&... args) noexcept {
    if (!log::accepts(level)) return;
    if constexpr (sizeof...(Args) == 0) {
        log::write(level, pattern);
    } else {
        try {
            const container::String rendered = fmt::vformat(
                fmt::string_view{pattern.data(), pattern.size()},
                fmt::make_format_args(args...));
            log::write(level, rendered);
        } catch (...) {
            log::write(level, "<log formatting failed>");
        }
    }
}

} // namespace debug::detail
#endif

// ── 日志宏 ──────────────────────────────────────────────────────────────
#if TD_LOG_ENABLED

#if TD_DEBUG_ENABLED

    #define TD_LOG_TRACE(...)                                            \
        ::debug::detail::write_formatted(                                \
            ::debug::log_level::trace, __VA_ARGS__)
    #define TD_LOG_DEBUG(...)                                            \
        ::debug::detail::write_formatted(                                \
            ::debug::log_level::debug, __VA_ARGS__)

#else

    #define TD_LOG_TRACE(...)    ((void)0)
    #define TD_LOG_DEBUG(...)    ((void)0)

#endif

    #define TD_LOG_INFO(...)                                             \
        ::debug::detail::write_formatted(                                \
            ::debug::log_level::info, __VA_ARGS__)
    #define TD_LOG_WARN(...)                                             \
        ::debug::detail::write_formatted(                                \
            ::debug::log_level::warning, __VA_ARGS__)
    #define TD_LOG_ERROR(...)                                            \
        ::debug::detail::write_formatted(                                \
            ::debug::log_level::error, __VA_ARGS__)

    #define TD_LOG_WARN_AGGREGATED(key, ...)                              \
        do {                                                              \
            const container::StringView td_aggregate_key{key};            \
            const auto td_aggregate_decision =                            \
                ::debug::log::aggregate(td_aggregate_key);                \
            if (td_aggregate_decision.action ==                            \
                ::debug::aggregated_log_action::first_sample) {           \
                TD_LOG_WARN(__VA_ARGS__);                                  \
            } else if (td_aggregate_decision.action ==                     \
                       ::debug::aggregated_log_action::summary) {          \
                TD_LOG_WARN(                                               \
                    "[HotLog] '{}' repeated {} additional times (total={})", \
                    td_aggregate_key, td_aggregate_decision.additional,    \
                    td_aggregate_decision.total);                          \
            }                                                             \
        } while (false)

    #define TD_LOG_ERROR_AGGREGATED(key, ...)                             \
        do {                                                              \
            const container::StringView td_aggregate_key{key};            \
            const auto td_aggregate_decision =                            \
                ::debug::log::aggregate(td_aggregate_key);                \
            if (td_aggregate_decision.action ==                            \
                ::debug::aggregated_log_action::first_sample) {           \
                TD_LOG_ERROR(__VA_ARGS__);                                 \
            } else if (td_aggregate_decision.action ==                     \
                       ::debug::aggregated_log_action::summary) {          \
                TD_LOG_ERROR(                                              \
                    "[HotLog] '{}' repeated {} additional times (total={})", \
                    td_aggregate_key, td_aggregate_decision.additional,    \
                    td_aggregate_decision.total);                          \
            }                                                             \
        } while (false)

#else

    #define TD_LOG_TRACE(...)    ((void)0)
    #define TD_LOG_DEBUG(...)    ((void)0)
    #define TD_LOG_INFO(...)     ((void)0)
    #define TD_LOG_WARN(...)     ((void)0)
    #define TD_LOG_ERROR(...)    ((void)0)
    #define TD_LOG_WARN_AGGREGATED(...)  ((void)0)
    #define TD_LOG_ERROR_AGGREGATED(...) ((void)0)
#endif
