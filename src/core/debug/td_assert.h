#pragma once

#include "debug_config.h"

namespace debug::detail {

[[noreturn]] void assertion_failed(const char* expression,
                                   const char* message,
                                   const char* file,
                                   int line) noexcept;

} // namespace debug::detail

#if TD_DEBUG_ENABLED

    #define TD_ASSERT(expr) \
        do { \
            if (!(expr)) { \
                ::debug::detail::assertion_failed(#expr, nullptr, __FILE__, __LINE__); \
            } \
        } while (false)

    #define TD_ASSERT_MSG(expr, message) \
        do { \
            if (!(expr)) { \
                ::debug::detail::assertion_failed(#expr, message, __FILE__, __LINE__); \
            } \
        } while (false)

#else

    #define TD_ASSERT(expr) ((void)0)
    #define TD_ASSERT_MSG(expr, message) ((void)0)

#endif
