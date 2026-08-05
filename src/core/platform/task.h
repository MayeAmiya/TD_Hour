#pragma once

#include "runtime_threads.h"

#include <taskflow/taskflow.hpp>
#include <taskflow/algorithm/for_each.hpp>
#include <cstddef>
#include <functional>

namespace platform {

using taskflow = tf::Taskflow;
using executor = tf::Executor;

inline executor& get_executor() noexcept
{
    return runtime::resourceExecutor();
}

template <typename IndexT, typename Func>
inline void parallel_for(IndexT first, IndexT last, Func&& func)
{
    get_executor().for_each(
        tf::IndexRange<IndexT>(first, last),
        std::forward<Func>(func)
    ).wait();
}

template <typename IterT, typename Func>
inline void parallel_for_each(IterT first, IterT last, Func&& func)
{
    get_executor().for_each(
        tf::ref(first), tf::ref(last),
        std::forward<Func>(func)
    ).wait();
}

template <typename... Tasks>
inline void run(Tasks&&... tasks)
{
    get_executor().run(std::forward<Tasks>(tasks)...);
}

inline void wait_all()
{
    get_executor().wait_for_all();
}

} // namespace platform
