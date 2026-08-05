#pragma once

#include "core/container/container_types.h"
#include "core/debug/debug_config.h"

// STL — commonly used + expensive to parse
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
// spdlog — core headers only (sinks may pull in <windows.h>)
#if TD_DEBUG_ENABLED
    #include <spdlog/spdlog.h>
    #include <spdlog/common.h>
    #include <spdlog/logger.h>
    #include <spdlog/details/synchronous_factory.h>
#endif

// fmt
#include <fmt/core.h>
#include <fmt/format.h>
