#pragma once

#include "core/container/container_types.h"
#include "core/platform/runtime_threads.h"
#include "engine/resource/ResourceSchedulerRuntime.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace engine::render::detail {

template <typename Product, typename Work>
std::future<Product> submitTerrainSceneFuture(
    const char* identity, uint64_t variant, uint64_t estimatedBytes,
    Work work) {
    using Promise = std::promise<Product>;
    const auto promise = std::make_shared<Promise>();
    const auto settled = std::make_shared<std::atomic_bool>(false);
    std::future<Product> future = promise->get_future();

    const auto fail = [promise, settled](std::exception_ptr exception) {
        bool expected = false;
        if (settled->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            promise->set_exception(std::move(exception));
        }
    };

    engine::resource::ResourceSchedulerRuntime* scheduler =
        engine::resource::activeResourceSchedulerRuntime();
    if (!scheduler) {
        fail(std::make_exception_ptr(std::runtime_error(
            "resource scheduler is unavailable")));
        throw std::runtime_error("resource scheduler is unavailable");
    }

    engine::resource::ResourceRequest request;
    request.key.kind = engine::resource::ResourceKind::Scene;
    request.key.canonicalIdentity = identity;
    request.key.variant = variant;
    request.key.generation = 0;
    request.demand = engine::resource::ResourceDemand::Visible;
    request.lane = engine::resource::ResourceLane::Scene;
    request.estimatedBytes = std::max<uint64_t>(estimatedBytes, 1u);

    const engine::resource::ResourceSubmitResult submitted = scheduler->submit(
        std::move(request),
        [promise, settled, fail, work = std::move(work)](
            const engine::resource::ResourceTaskContext& context) mutable {
            platform::runtime::ThreadRoleScope role(
                platform::runtime::ThreadRole::Resource);
            if (context.stopRequested()) {
                fail(std::make_exception_ptr(std::runtime_error(
                    "scene resource task cancelled")));
                return engine::resource::ResourceTaskResult::Failed;
            }
            try {
                if constexpr (std::is_void_v<Product>) {
                    work();
                    bool expected = false;
                    if (settled->compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel)) {
                        promise->set_value();
                    }
                } else {
                    Product product = work();
                    bool expected = false;
                    if (settled->compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel)) {
                        promise->set_value(std::move(product));
                    }
                }
                return engine::resource::ResourceTaskResult::Ready;
            } catch (...) {
                fail(std::current_exception());
                return engine::resource::ResourceTaskResult::Failed;
            }
        },
        [fail](const engine::resource::ResourceCompletion& completion) {
            if (completion.state != engine::resource::ResourceJobState::Ready) {
                fail(std::make_exception_ptr(std::runtime_error(
                    "scene resource task did not complete")));
            }
        });
    if (!submitted.accepted()) {
        fail(std::make_exception_ptr(std::runtime_error(
            "resource scheduler rejected scene task")));
        throw std::runtime_error("resource scheduler rejected scene task");
    }
    return future;
}

template <typename Product, typename Builder>
bool buildIndexedTerrainCpuProducts(
    size_t count,
    const char* taskName,
    Builder builder,
    container::Vector<Product>& output,
    container::String* error) {
    output.clear();
    output.resize(count);
    if (count == 0) return true;
    if (count == 1 || platform::runtime::sceneResourceWorkerCount() == 1u) {
        for (size_t index = 0; index < count; ++index) {
            output[index] = builder(index);
        }
        return true;
    }

    const size_t workerCount = std::max<size_t>(
        1u, platform::runtime::sceneResourceWorkerCount());
    const size_t taskCount = std::min(
        count, std::max<size_t>(1u, workerCount * 2u));
    const size_t grain = (count + taskCount - 1u) / taskCount;
    container::Vector<std::future<void>> tasks;
    tasks.reserve(taskCount);
    try {
        for (size_t begin = 0; begin < count; begin += grain) {
            const size_t end = std::min(begin + grain, count);
            tasks.push_back(submitTerrainSceneFuture<void>(
                taskName, begin, 256ull * 1024ull,
                [builder, &output, begin, end]() mutable {
                    for (size_t index = begin; index < end; ++index) {
                        output[index] = builder(index);
                    }
                }));
        }
    } catch (const std::exception& exception) {
        for (std::future<void>& task : tasks) {
            if (!task.valid()) continue;
            try { static_cast<void>(task.get()); } catch (...) {}
        }
        if (error) {
            *error = container::String("Could not submit ") + taskName +
                " task: " + exception.what();
        }
        return false;
    } catch (...) {
        for (std::future<void>& task : tasks) {
            if (!task.valid()) continue;
            try { static_cast<void>(task.get()); } catch (...) {}
        }
        if (error) {
            *error = container::String("Could not submit ") + taskName +
                " task";
        }
        return false;
    }

    bool failed = false;
    for (std::future<void>& task : tasks) {
        try {
            task.get();
        } catch (const std::exception& exception) {
            if (!failed && error) {
                *error = container::String(taskName) +
                    " task failed: " + exception.what();
            }
            failed = true;
        } catch (...) {
            if (!failed && error) {
                *error = container::String(taskName) + " task failed";
            }
            failed = true;
        }
    }
    return !failed;
}

} // namespace engine::render::detail
