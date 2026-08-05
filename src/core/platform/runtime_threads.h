#pragma once

#include <taskflow/taskflow.hpp>

#include <algorithm>
#include <cstdint>
#include <thread>

namespace platform::runtime {

enum class ThreadRole : uint8_t {
    Unknown,
    Main,
    Logic,
    Render,
    // CPU-side presentation work and isolated D3D12 command-list recording
    // may share the bounded worker executor with resource jobs, but they are
    // still part of the render ownership domain. Keeping a distinct role
    // prevents worker command recording from masquerading as file/decoder
    // work and lets ownership assertions distinguish the two contracts.
    RenderWorker,
    SimulationWorker,
    Resource,
    ResourceScheduler,
};

// Main/event, logic, D3D12 submission, and the lightweight resource-admission
// owner each reserve one thread outside the worker executors.
inline constexpr uint32_t kDedicatedRuntimeThreadCount = 4u;

[[nodiscard]] inline uint32_t hardwareThreadCount() noexcept {
    return std::max(1u, std::thread::hardware_concurrency());
}

// The remaining hardware budget is split between one shared foreground pool
// (simulation, scene construction and renderer preparation) and background
// asset decoding. Simulation and rendering peak at different parts of a
// frame; statically partitioning the foreground share strands cores and can
// make a 30 Hz renderer fall behind while the process is otherwise idle.
// Sharing one executor keeps the process-wide worker count bounded and lets
// Taskflow schedule whichever confirmed/presentation work is ready.
[[nodiscard]] inline uint32_t availableWorkerCount() noexcept {
    const uint32_t hardware = hardwareThreadCount();
    return hardware > kDedicatedRuntimeThreadCount
        ? hardware - kDedicatedRuntimeThreadCount
        : 1u;
}

[[nodiscard]] inline uint32_t resourceWorkerCount() noexcept {
    const uint32_t available = availableWorkerCount();
    return available >= 6u ? std::max(2u, available / 4u) : 1u;
}

[[nodiscard]] inline uint32_t renderWorkerCount() noexcept {
    const uint32_t available = availableWorkerCount();
    const uint32_t background = resourceWorkerCount();
    return available > background ? available - background : 1u;
}

[[nodiscard]] inline uint32_t sceneResourceWorkerCount() noexcept {
    return renderWorkerCount();
}

struct RuntimeExecutorBudget final {
    uint32_t hardware = 1;
    uint32_t dedicated = kDedicatedRuntimeThreadCount;
    uint32_t availableWorkers = 1;
    uint32_t renderWorkers = 1;
    uint32_t sceneWorkers = 1;
    uint32_t resourceWorkers = 1;
    bool sharesConstrainedExecutor = false;
};

[[nodiscard]] inline RuntimeExecutorBudget executorBudget() noexcept {
    const uint32_t hardware = hardwareThreadCount();
    const uint32_t available = availableWorkerCount();
    return {
        .hardware = hardware,
        .dedicated = kDedicatedRuntimeThreadCount,
        .availableWorkers = available,
        .renderWorkers = renderWorkerCount(),
        .sceneWorkers = sceneResourceWorkerCount(),
        .resourceWorkers = resourceWorkerCount(),
        .sharesConstrainedExecutor = available < 3u,
    };
}

// Machines with fewer than three worker slots cannot host independent
// foreground/background executors without oversubscription. Share one bounded
// pool there; queue ownership remains separate at the caller boundary.
[[nodiscard]] inline tf::Executor& constrainedWorkerExecutor() {
    static tf::Executor executor(availableWorkerCount());
    return executor;
}

[[nodiscard]] inline tf::Executor& foregroundWorkerExecutor() {
    if (availableWorkerCount() < 3u) return constrainedWorkerExecutor();
    static tf::Executor executor(renderWorkerCount());
    return executor;
}

[[nodiscard]] inline tf::Executor& renderWorkerExecutor() {
    return foregroundWorkerExecutor();
}

[[nodiscard]] inline tf::Executor& sceneResourceExecutor() {
    return foregroundWorkerExecutor();
}

// Confirmed simulation jobs use the foreground/scene share of the global
// worker budget. This is an alias, not another executor: AI bursts therefore
// cannot oversubscribe the machine beside render/resource workers.
[[nodiscard]] inline tf::Executor& simulationWorkerExecutor() {
    return sceneResourceExecutor();
}

[[nodiscard]] inline tf::Executor& resourceExecutor() {
    if (availableWorkerCount() < 3u) return constrainedWorkerExecutor();
    static tf::Executor executor(resourceWorkerCount());
    return executor;
}

inline thread_local ThreadRole g_currentThreadRole = ThreadRole::Unknown;

inline void setCurrentThreadRole(ThreadRole role) noexcept {
    g_currentThreadRole = role;
}

[[nodiscard]] inline ThreadRole currentThreadRole() noexcept {
    return g_currentThreadRole;
}

[[nodiscard]] inline bool isCurrentThread(ThreadRole role) noexcept {
    return currentThreadRole() == role;
}

class ThreadRoleScope final {
public:
    explicit ThreadRoleScope(ThreadRole role) noexcept
        : m_previous(currentThreadRole()) {
        setCurrentThreadRole(role);
    }

    ~ThreadRoleScope() {
        setCurrentThreadRole(m_previous);
    }

    ThreadRoleScope(const ThreadRoleScope&) = delete;
    ThreadRoleScope& operator=(const ThreadRoleScope&) = delete;

private:
    ThreadRole m_previous = ThreadRole::Unknown;
};

} // namespace platform::runtime
