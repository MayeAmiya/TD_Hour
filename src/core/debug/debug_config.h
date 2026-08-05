#pragma once

#if defined(TD_DEBUG_DISABLE)
    #define TD_DEBUG_ENABLED 0
#elif defined(TD_DEBUG_ENABLE)
    #define TD_DEBUG_ENABLED 1
#elif defined(TD_DEBUG)
    #define TD_DEBUG_ENABLED 1
#else
    #define TD_DEBUG_ENABLED 0
#endif

// Operational logging is independent from Debug diagnostics. Shipping builds
// keep Info/Warning/Error output for the console, generals.log and crash
// triage, while Trace/Debug, assertions and Tracy remain Debug-only.
#if defined(TD_LOG_DISABLE)
    #define TD_LOG_ENABLED 0
#elif defined(TD_LOG_ENABLE) || defined(TD_LOG) || TD_DEBUG_ENABLED
    #define TD_LOG_ENABLED 1
#else
    #define TD_LOG_ENABLED 0
#endif
