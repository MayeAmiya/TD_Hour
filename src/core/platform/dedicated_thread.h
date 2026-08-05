#pragma once

#include "runtime_threads.h"

#include <Windows.h>

#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace platform::runtime {

// 专属运行时线程的统一生命周期包装。工作函数抛出的异常会被保存，主线程
// 可以在事件循环中检查并传播；析构始终先请求停止再 join。
class DedicatedThread final {
public:
    DedicatedThread() = default;

    ~DedicatedThread() {
        requestStop();
        join();
    }

    DedicatedThread(const DedicatedThread&) = delete;
    DedicatedThread& operator=(const DedicatedThread&) = delete;

    template <typename Function>
    void start(ThreadRole role, std::wstring_view name, Function&& function) {
        requestStop();
        join();
        {
            std::lock_guard lock(m_failureMutex);
            m_failure = nullptr;
        }
        m_finished.store(false, std::memory_order_release);
        m_thread = std::jthread(
            [this, role, threadName = std::wstring(name),
             work = std::forward<Function>(function)](
                std::stop_token stopToken) mutable {
                ThreadRoleScope roleScope(role);
                if (!threadName.empty()) {
                    static_cast<void>(::SetThreadDescription(
                        ::GetCurrentThread(), threadName.c_str()));
                }
                try {
                    std::invoke(work, stopToken);
                } catch (...) {
                    std::lock_guard lock(m_failureMutex);
                    m_failure = std::current_exception();
                }
                m_finished.store(true, std::memory_order_release);
            });
    }

    void requestStop() noexcept {
        if (m_thread.joinable()) m_thread.request_stop();
    }

    void join() noexcept {
        if (m_thread.joinable()) m_thread.join();
    }

    [[nodiscard]] bool joinable() const noexcept {
        return m_thread.joinable();
    }

    [[nodiscard]] bool finished() const noexcept {
        return m_finished.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::exception_ptr failure() const noexcept {
        std::lock_guard lock(m_failureMutex);
        return m_failure;
    }

    void rethrowFailure() const {
        if (std::exception_ptr error = failure()) std::rethrow_exception(error);
    }

private:
    std::jthread m_thread;
    mutable std::mutex m_failureMutex;
    std::exception_ptr m_failure;
    std::atomic<bool> m_finished{true};
};

} // namespace platform::runtime
